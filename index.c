#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

// Manual declaration for object store
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    if (!index || !path) return NULL;
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
    return -1;
}

int index_status(const Index *index) {
    if (!index) return -1;
    printf("Staged changes:\n");
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
    }
    printf("\nUntracked files:\n");
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o")) continue;

            if (!index_find((Index*)index, ent->d_name)) {
                printf("  untracked:  %s\n", ent->d_name);
            }
        }
        closedir(dir);
    }
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        if (fscanf(f, "%o %64s %ld %u ", &e->mode, hex, (long *)&e->mtime_sec, &e->size) != 4) break;
        if (fgets(e->path, sizeof(e->path), f) == NULL) break;
        e->path[strcspn(e->path, "\r\n")] = 0;
        hex_to_hash(hex, &e->hash);
        index->count++;
    }
    fclose(f);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    if (!index) return -1;

    // Use a simpler path for now to avoid rename issues
    FILE *f = fopen(".pes/index", "w");
    if (!f) {
        perror("Error opening index for writing");
        return -1;
    }

    for (int i = 0; i < index->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&index->entries[i].hash, hex);
        
        // Ensure path exists before printing
        const char *out_path = index->entries[i].path;
        if (!out_path || out_path[0] == '\0') continue;

        fprintf(f, "%o %s %ld %u %s\n", 
                index->entries[i].mode, 
                hex, 
                (long)index->entries[i].mtime_sec, 
                index->entries[i].size, 
                out_path);
    }

    fclose(f);
    return 0;
}

int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    // 1. Read file safely
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *data = malloc(st.st_size > 0 ? st.st_size : 1);
    if (!data) { fclose(f); return -1; }
    
    // Only read if there is data to read
    if (st.st_size > 0) {
        fread(data, 1, st.st_size, f);
    }
    fclose(f);

    // 2. Write object and get ID
    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    // 3. Find or Create Entry (Very Safe Scan)
    IndexEntry *e = NULL;
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            e = &index->entries[i];
            break;
        }
    }

    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        memset(e->path, 0, sizeof(e->path));
        strncpy(e->path, path, sizeof(e->path) - 1);
    }

    // 4. Update metadata
    e->hash = id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;
    e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    return index_save(index);
}
