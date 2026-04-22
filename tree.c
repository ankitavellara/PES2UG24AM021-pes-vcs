// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int index_load(Index *index);
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── Recursive helper ────────────────────────────────────────────────────────
// Builds a tree for all index entries under a given prefix at depth 0.
// For subdirectories, recurses with the subdirectory path as a new prefix.

static int write_tree_level(IndexEntry *entries, int count, int depth, const char *prefix, ObjectID *id_out) {
    Tree tree = {0};

    for (int i = 0; i < count; i++) {
        IndexEntry *entry = &entries[i];
        const char *path = entry->path;

        if (prefix && strncmp(path, prefix, strlen(prefix)) != 0)
            continue;

        if (prefix)
            path = path + strlen(prefix);

        int slashes = 0;
        for (const char *p = path; *p; p++)
            if (*p == '/') slashes++;

        if (slashes < depth) continue;

        int slash_count = 0;
        const char *current = path;
        while (slash_count < depth && *current) {
            if (*current == '/') slash_count++;
            current++;
        }

        const char *next_slash = strchr(current, '/');
        char component_name[256] = {0};

        if (next_slash) {
            int len = next_slash - current;
            snprintf(component_name, sizeof(component_name), "%.*s", len, current);
        } else {
            snprintf(component_name, sizeof(component_name), "%s", current);
        }

        int found = 0;
        for (int j = 0; j < tree.count; j++) {
            if (strcmp(tree.entries[j].name, component_name) == 0) {
                found = 1; break;
            }
        }

        if (!found && tree.count < MAX_TREE_ENTRIES) {
            TreeEntry *tentry = &tree.entries[tree.count];
            snprintf(tentry->name, sizeof(tentry->name), "%s", component_name);
            if (next_slash) {
                tentry->mode = MODE_DIR;
            } else {
                tentry->mode  = entry->mode;
                tentry->hash  = entry->hash;
            }
            tree.count++;
        }
    }

    // Recurse into subdirectories
    for (int i = 0; i < tree.count; i++) {
        if (tree.entries[i].mode == MODE_DIR) {
            char new_prefix[512] = {0};
            if (prefix)
                snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, tree.entries[i].name);
            else
                snprintf(new_prefix, sizeof(new_prefix), "%s/", tree.entries[i].name);

            if (write_tree_level(entries, count, 0, new_prefix, &tree.entries[i].hash) != 0)
                return -1;
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    if (object_write(OBJ_TREE, data, len, id_out) != 0) { free(data); return -1; }
    free(data);
    return 0;
}

// ─── tree_from_index stub (to be wired next) ─────────────────────────────────

int tree_from_index(ObjectID *id_out) {
    (void)id_out;
    return -1; // stub
}