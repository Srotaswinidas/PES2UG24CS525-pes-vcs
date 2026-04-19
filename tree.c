#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

uint32_t get_file_mode(const char *path) {
    return 0100644;  // Simple - always return file mode
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = data;
    const uint8_t *end = ptr + len;
    
    while (ptr < end && tree_out->count < 1024) {
        TreeEntry *e = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        
        char mode_str[16] = {0};
        memcpy(mode_str, ptr, space - ptr);
        e->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        
        memcpy(e->name, ptr, null_byte - ptr);
        e->name[null_byte - ptr] = '\0';
        ptr = null_byte + 1;
        
        if (ptr + 32 > end) return -1;
        memcpy(e->hash.hash, ptr, 32);
        ptr += 32;
        
        tree_out->count++;
    }
    return 0;
}

static int cmp(const void *a, const void *b) {
    return strcmp(((const TreeEntry*)a)->name, ((const TreeEntry*)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), cmp);
    
    size_t total = 0;
    for (int i = 0; i < sorted.count; i++) {
        total += snprintf(NULL, 0, "%o %s", sorted.entries[i].mode, sorted.entries[i].name) + 1 + 32;
    }
    
    char *buf = malloc(total);
    if (!buf) return -1;
    
    size_t off = 0;
    for (int i = 0; i < sorted.count; i++) {
        off += sprintf(buf + off, "%o %s", sorted.entries[i].mode, sorted.entries[i].name);
        buf[off++] = '\0';
        memcpy(buf + off, sorted.entries[i].hash.hash, 32);
        off += 32;
    }
    
    *data_out = buf;
    *len_out = total;
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0 || index.count == 0) return -1;
    
    Tree tree;
    tree.count = 0;
    
    for (int i = 0; i < index.count && tree.count < 1024; i++) {
        tree.entries[tree.count].mode = index.entries[i].mode;
        tree.entries[tree.count].hash = index.entries[i].hash;
        strcpy(tree.entries[tree.count].name, index.entries[i].path);
        tree.count++;
    }
    
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    
    int result = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return result;
}
