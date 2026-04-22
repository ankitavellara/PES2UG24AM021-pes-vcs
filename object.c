// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { memset(id_out->hash, 0, HASH_SIZE); return; }
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── object_write ────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    size_t full_len = header_len + 1 + len;
    void *full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, header_len);
    ((char*)full_obj)[header_len] = '\0';
    memcpy((char*)full_obj + header_len + 1, data, len);

    ObjectID id;
    compute_hash(full_obj, full_len, &id);

    if (object_exists(&id)) {
        free(full_obj);
        *id_out = id;
        return 0;
    }

    char shard_dir[512];
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/.tmp", shard_dir);

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full_obj); return -1; }

    if (write(fd, full_obj, full_len) != (ssize_t)full_len) {
        close(fd); unlink(temp_path); free(full_obj); return -1;
    }

    if (fsync(fd) != 0) {
        close(fd); unlink(temp_path); free(full_obj); return -1;
    }
    close(fd);

    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));

    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path); free(full_obj); return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    free(full_obj);
    *id_out = id;
    return 0;
}

// ─── object_read ─────────────────────────────────────────────────────────────
// Reads an object from the store, verifies its integrity via SHA-256,
// and returns the data portion after the header null byte.

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) { fclose(f); return -1; }

    void *file_data = malloc(fsize);
    if (!file_data) { fclose(f); return -1; }

    if (fread(file_data, 1, fsize, f) != (size_t)fsize) {
        free(file_data); fclose(f); return -1;
    }
    fclose(f);

    // Find null byte separating header from data
    void *null_pos = memchr(file_data, '\0', fsize);
    if (!null_pos) { free(file_data); return -1; }

    size_t header_len = (char*)null_pos - (char*)file_data;

    char header[256];
    if (header_len >= sizeof(header)) { free(file_data); return -1; }
    memcpy(header, file_data, header_len);
    header[header_len] = '\0';

    char type_str[32];
    size_t stored_size;
    if (sscanf(header, "%31s %zu", type_str, &stored_size) != 2) {
        free(file_data); return -1;
    }

    size_t actual_data_len = fsize - header_len - 1;
    if (actual_data_len != stored_size) { free(file_data); return -1; }

    // Integrity check: recompute hash and compare to filename
    ObjectID computed_id;
    compute_hash(file_data, fsize, &computed_id);
    if (memcmp(computed_id.hash, id->hash, HASH_SIZE) != 0) {
        free(file_data); return -1;
    }

    if (strncmp(type_str, "blob", 4) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp(type_str, "tree", 4) == 0)   *type_out = OBJ_TREE;
    else if (strncmp(type_str, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else { free(file_data); return -1; }

    void *data = malloc(actual_data_len);
    if (!data) { free(file_data); return -1; }

    memcpy(data, (char*)file_data + header_len + 1, actual_data_len);
    free(file_data);

    *data_out = data;
    *len_out  = actual_data_len;
    return 0;
}

printf("Phase 1 completed");