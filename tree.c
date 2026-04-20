#include "tree.h"
#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
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
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

static int write_tree_level(IndexEntry *entries, int count, int depth, ObjectID *out_id) {
    Tree current_tree;
    current_tree.count = 0;
    
    int i = 0;
    while (i < count && current_tree.count < MAX_TREE_ENTRIES) {
        char *slash = strchr(entries[i].path + depth, '/');
        TreeEntry *t_entry = &current_tree.entries[current_tree.count];
        
        if (slash == NULL) {
            strcpy(t_entry->name, entries[i].path + depth);
            t_entry->mode = entries[i].mode;
            memcpy(t_entry->hash.hash, entries[i].hash.hash, HASH_SIZE);
            current_tree.count++;
            i++; 
        } else {
            int dir_name_len = slash - (entries[i].path + depth);
            strncpy(t_entry->name, entries[i].path + depth, dir_name_len);
            t_entry->name[dir_name_len] = '\0';
            t_entry->mode = MODE_DIR;
            
            int sub_count = 0;
            while (i + sub_count < count) {
                if (strncmp(entries[i].path + depth, entries[i + sub_count].path + depth, dir_name_len) == 0 &&
                    (entries[i + sub_count].path[depth + dir_name_len] == '/')) {
                    sub_count++;
                } else {
                    break;
                }
            }
            
            ObjectID sub_tree_id;
            if (write_tree_level(&entries[i], sub_count, depth + dir_name_len + 1, &sub_tree_id) != 0) {
                return -1;
            }
            
            memcpy(t_entry->hash.hash, sub_tree_id.hash, HASH_SIZE);
            current_tree.count++;
            i += sub_count;
        }
    }
    
    void *tree_data = NULL;
    size_t tree_len = 0;
    
    if (tree_serialize(&current_tree, &tree_data, &tree_len) != 0) {
        return -1;
    }
    
    int result = object_write(OBJ_TREE, tree_data, tree_len, out_id);
    free(tree_data);
    
    return result;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;
    
    if (index_load(&idx) != 0) {
        return -1; 
    }
    
    // Notice I changed idx.entry_count to idx.count here based on your compiler error!
    if (idx.count == 0) {
        return -1;
    }
    
    return write_tree_level(idx.entries, idx.count, 0, id_out);
}
