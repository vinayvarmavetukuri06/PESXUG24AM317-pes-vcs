#include "commit.h"
#include "pes.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Declarations for object store
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

int commit_parse(const void *data, size_t len, Commit *commit_out) {
    const char *p = (const char *)data;
    char hex[65];
    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    hex_to_hash(hex, &commit_out->tree);
    p = strchr(p, '\n') + 1;
    if (strncmp(p, "parent ", 7) == 0) {
        sscanf(p, "parent %64s\n", hex);
        hex_to_hash(hex, &commit_out->parent);
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else { commit_out->has_parent = 0; }
    char author_buf[256];
    sscanf(p, "author %255[^\n]\n", author_buf);
    char *last_space = strrchr(author_buf, ' ');
    commit_out->timestamp = strtoull(last_space + 1, NULL, 10);
    *last_space = '\0';
    strncpy(commit_out->author, author_buf, sizeof(commit_out->author));
    p = strchr(p, '\n') + 1; p = strchr(p, '\n') + 1; p = strchr(p, '\n') + 1;
    strncpy(commit_out->message, p, sizeof(commit_out->message));
    return 0;
}

int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[65], parent_hex[65];
    hash_to_hex(&commit->tree, tree_hex);
    char buf[8192];
    int n = sprintf(buf, "tree %s\n", tree_hex);
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        n += sprintf(buf + n, "parent %s\n", parent_hex);
    }
    n += sprintf(buf + n, "author %s %lu\ncommitter %s %lu\n\n%s", 
                 commit->author, commit->timestamp, commit->author, commit->timestamp, commit->message);
    *data_out = strdup(buf);
    *len_out = strlen(buf);
    return 0;
}

int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;
    while (1) {
        ObjectType type; void *raw; size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;
        Commit c; commit_parse(raw, raw_len, &c);
        callback(&id, &c, ctx);
        if (!c.has_parent) { free(raw); break; }
        id = c.parent; free(raw);
    }
    return 0;
}

int head_read(ObjectID *id_out) {
    FILE *f = fopen(".pes/HEAD", "r");
    if (!f) return -1;
    char line[512]; fgets(line, sizeof(line), f); fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (strncmp(line, "ref: ", 5) == 0) {
        char path[512]; sprintf(path, ".pes/%s", line + 5);
        f = fopen(path, "r");
        if (!f) return -1;
        fgets(line, sizeof(line), f); fclose(f);
        line[strcspn(line, "\r\n")] = 0;
    }
    return hex_to_hash(line, id_out);
}

int head_update(const ObjectID *new_commit) {
    char hex[65]; hash_to_hex(new_commit, hex);
    FILE *f = fopen(".pes/refs/heads/main", "w");
    if (!f) return -1;
    fprintf(f, "%s\n", hex); fclose(f);
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int commit_create(const char *message, ObjectID *commit_id_out) {
    Commit commit; memset(&commit, 0, sizeof(Commit));
    if (tree_from_index(&commit.tree) != 0) return -1;
    if (head_read(&commit.parent) == 0) commit.has_parent = 1;
    snprintf(commit.author, sizeof(commit.author), "%s", pes_author());
    commit.timestamp = (uint64_t)time(NULL);
    strncpy(commit.message, message, sizeof(commit.message));
    void *data; size_t len;
    commit_serialize(&commit, &data, &len);
    object_write(OBJ_COMMIT, data, len, commit_id_out);
    free(data);
    return head_update(commit_id_out);
}
