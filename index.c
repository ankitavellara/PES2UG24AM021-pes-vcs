// index.c — Staging area implementation

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size  != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            unstaged_count++;
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0)  continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry*)a)->path, ((IndexEntry*)b)->path);
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        if (fscanf(f, "%u %64s %llu %u %511s",
                   &index->entries[index->count].mode,
                   hex,
                   &index->entries[index->count].mtime_sec,
                   &index->entries[index->count].size,
                   index->entries[index->count].path) != 5) {
            break;
        }
        if (hex_to_hash(hex, &index->entries[index->count].hash) != 0) {
            fclose(f);
            return -1;
        }
        index->count++;
    }
    fclose(f);
    return 0;
}

// ─── index_save ──────────────────────────────────────────────────────────────
// Writes index to a temp file, fsyncs, then atomically renames over the real index.

int index_save(const Index *index) {
    IndexEntry *sorted = malloc(index->count * sizeof(IndexEntry));
    if (!sorted) return -1;
    memcpy(sorted, index->entries, index->count * sizeof(IndexEntry));
    qsort(sorted, index->count, sizeof(IndexEntry), compare_entries);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        hash_to_hex(&sorted[i].hash, hex);
        if (fprintf(f, "%u %s %llu %u %s\n",
                    sorted[i].mode, hex,
                    sorted[i].mtime_sec, sorted[i].size,
                    sorted[i].path) < 0) {
            fclose(f); unlink(tmp_path); free(sorted); return -1;
        }
    }

    if (fflush(f) != 0)          { fclose(f); unlink(tmp_path); free(sorted); return -1; }
    if (fsync(fileno(f)) != 0)   { fclose(f); unlink(tmp_path); free(sorted); return -1; }
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) { free(sorted); return -1; }
    free(sorted);
    return 0;
}

// ─── stub ────────────────────────────────────────────────────────────────────

int index_add(Index *index, const char *path) {
    (void)index; (void)path;
    return -1; // stub
}