// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
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

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
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

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
typedef struct {
    TreeEntry *entries;
    int count;
    int capacity;
} EntryList;

static void add_entry(EntryList *list, TreeEntry *entry) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity * 2 + 10;
        list->entries = realloc(list->entries, list->capacity * sizeof(TreeEntry));
    }
    list->entries[list->count++] = *entry;
}
static int write_tree_level(TreeEntry *all_entries, int total_count, int depth, ObjectID *id_out) {
    if (total_count == 0) return -1;
    
    Tree tree;
    tree.count = 0;
    
    EntryList current_entries = {0};
    current_entries.capacity = 10;
    current_entries.entries = malloc(current_entries.capacity * sizeof(TreeEntry));
    
    typedef struct {
        char name[256];
        TreeEntry *children;
        int child_count;
        int child_capacity;
    } Subdir;
    
    Subdir subdirs[100];
    int subdir_count = 0;
    
    for (int i = 0; i < total_count; i++) {
        TreeEntry *entry = &all_entries[i];
        char *slash = strchr(entry->name + depth, '/');
        
        if (slash == NULL) {
            add_entry(&current_entries, entry);
        } else {
            int name_len = slash - (entry->name + depth);
            char dir_name[256];
            memcpy(dir_name, entry->name + depth, name_len);
            dir_name[name_len] = '\0';
            
            int found = -1;
            for (int s = 0; s < subdir_count; s++) {
                if (strcmp(subdirs[s].name, dir_name) == 0) {
                    found = s;
                    break;
                }
            }
            if (found == -1) {
                found = subdir_count++;
                strcpy(subdirs[found].name, dir_name);
                subdirs[found].children = NULL;
                subdirs[found].child_count = 0;
                subdirs[found].child_capacity = 0;
            }
            
            Subdir *sd = &subdirs[found];
            if (sd->child_count >= sd->child_capacity) {
                sd->child_capacity = sd->child_capacity * 2 + 5;
                sd->children = realloc(sd->children, sd->child_capacity * sizeof(TreeEntry));
            }
            sd->children[sd->child_count++] = *entry;
        }
    }
    
    for (int s = 0; s < subdir_count; s++) {
        Subdir *sd = &subdirs[s];
        ObjectID subtree_hash;
        
        if (write_tree_level(sd->children, sd->child_count, depth + strlen(sd->name) + 1, &subtree_hash) == 0) {
            TreeEntry subtree_entry;
            subtree_entry.mode = MODE_DIR;
            subtree_entry.hash = subtree_hash;
            strcpy(subtree_entry.name, sd->name);
            add_entry(&current_entries, &subtree_entry);
        }
        free(sd->children);
    }
    
    tree.count = current_entries.count;
    for (int i = 0; i < current_entries.count; i++) {
        tree.entries[i] = current_entries.entries[i];
    }
    
    void *serialized;
    size_t serialized_len;
    if (tree_serialize(&tree, &serialized, &serialized_len) != 0) {
        free(current_entries.entries);
        return -1;
    }
    
    int result = object_write(OBJ_TREE, serialized, serialized_len, id_out);
    free(serialized);
    free(current_entries.entries);
    return result;
}
int tree_from_index(ObjectID *id_out) {
    // TODO: Implement recursive tree building
    // (See Lab Appendix for logical steps)
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
    (void)id_out;
    return -1;
}
