// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
// added helper functions 
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "pes.h"

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
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

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
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
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
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
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
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

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
// 1. Declare object_write to prevent the implicit declaration warning
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// 2. Comparator function to sort IndexEntries (MUST be defined before index_save)
static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *entry_a = (const IndexEntry *)a;
    const IndexEntry *entry_b = (const IndexEntry *)b;
    return strcmp(entry_a->path, entry_b->path);
}

// 3. Load the index from .pes/index.
int index_load(Index *index) {
    index->count = 0;
    
    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0; // Not an error if it doesn't exist yet

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *entry = &index->entries[index->count];
        char hex_hash[65];
        
        if (sscanf(line, "%o %64s %lu %u %[^\n]", 
                   &entry->mode, hex_hash, &entry->mtime_sec, &entry->size, entry->path) == 5) {
            
            if (hex_to_hash(hex_hash, &entry->hash) == 0) {
                index->count++;
            }
        }
    }
    fclose(f);
    return 0;
}

// 4. Save the index to .pes/index atomically.
int index_save(const Index *index) {
    // PREVENT STACK OVERFLOW: Allocate only the needed memory on the Heap!
    IndexEntry *sorted_entries = NULL;
    
    if (index->count > 0) {
        sorted_entries = malloc(index->count * sizeof(IndexEntry));
        if (!sorted_entries) return -1;
        
        // Copy just the active entries and sort them
        memcpy(sorted_entries, index->entries, index->count * sizeof(IndexEntry));
        qsort(sorted_entries, index->count, sizeof(IndexEntry), compare_index_entries);
    }

    char temp_path[] = ".pes/index_XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd == -1) {
        free(sorted_entries);
        return -1;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        free(sorted_entries);
        return -1;
    }

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *entry = &sorted_entries[i];
        char hex_hash[65];
        hash_to_hex(&entry->hash, hex_hash);
        
        fprintf(f, "%06o %s %lu %u %s\n", 
                entry->mode, hex_hash, entry->mtime_sec, entry->size, entry->path);
    }

    // Free the heap memory when we are done!
    if (sorted_entries) {
        free(sorted_entries);
    }

    fflush(f);
    fsync(fd);
    fclose(f); 

    if (rename(temp_path, ".pes/index") != 0) {
        return -1;
    }

    return 0;
}

// 5. Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    struct stat st;
    
    // 1. Check if file exists and get metadata
    if (stat(path, &st) != 0) return -1; 
    if (!S_ISREG(st.st_mode)) return -1; 

    // 2. Read the full file contents
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    void *buffer = malloc(st.st_size);
    if (st.st_size > 0 && fread(buffer, 1, st.st_size, f) != (size_t)st.st_size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 3. Write as Blob
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, buffer, st.st_size, &blob_id) != 0) {
        free(buffer);
        return -1;
    }
    free(buffer);

    // 4. Update or Create Index Entry
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1; 
        entry = &index->entries[index->count++];
        snprintf(entry->path, sizeof(entry->path), "%s", path);
    }

    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = blob_id;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;

    // IMPORTANT: Save changes to disk
    return index_save(index);
}
