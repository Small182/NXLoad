#include "cover_cache.h"
#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COVER_CACHE_DIR NXLOAD_DOWNLOAD_DIR "/covers"
#define COVER_CACHE_SLOTS 2048u
#define COVER_CACHE_ITEM_LIMIT (96u * 1024)
#define COVER_CACHE_PATH 96

static void cache_path(char path[COVER_CACHE_PATH], const char *url, const char *extension) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    snprintf(path, COVER_CACHE_PATH, COVER_CACHE_DIR "/%04x.%s",
             (unsigned)(hash % COVER_CACHE_SLOTS), extension);
}

static bool read_key(FILE *file, const char *url) {
    char key[512 + 2];
    if (!fgets(key, sizeof(key), file)) return false;
    size_t size = strlen(key);
    if (!size || key[size - 1] != '\n') return false;
    key[size - 1] = 0;
    return !strcmp(key, url);
}

unsigned char *cover_cache_read(const char *url, size_t *bytes) {
    *bytes = 0;
    char path[COVER_CACHE_PATH];
    cache_path(path, url, "img");
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    unsigned char *data = NULL;
    if (read_key(file, url)) {
        long start = ftell(file);
        long size = start >= 0 && !fseek(file, 0, SEEK_END) ? ftell(file) - start : 0;
        if (size > 0 && size <= COVER_CACHE_ITEM_LIMIT && !fseek(file, start, SEEK_SET)) {
            data = malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, file) == (size_t)size) *bytes = (size_t)size;
        }
    }
    fclose(file);
    if (*bytes) return data;
    free(data);
    return NULL;
}

void cover_cache_write(const char *url, const unsigned char *data, size_t bytes) {
    if (!bytes || bytes > COVER_CACHE_ITEM_LIMIT || strlen(url) >= 512 || strpbrk(url, "\r\n") ||
        !nxload_ensure_storage() || !nxload_ensure_directory(COVER_CACHE_DIR)) return;
    char path[COVER_CACHE_PATH], temporary[COVER_CACHE_PATH];
    cache_path(path, url, "img");
    cache_path(temporary, url, "tmp");
    FILE *file = fopen(temporary, "wb");
    if (!file) return;
    bool ok = fprintf(file, "%s\n", url) > 0 && fwrite(data, 1, bytes, file) == bytes;
    if (fclose(file)) ok = false;
    if (ok && rename(temporary, path) == 0) return;
    if (ok && errno == EEXIST && unlink(path) == 0 && rename(temporary, path) == 0) return;
    unlink(temporary);
}

void cover_cache_discard(const char *url) {
    char path[COVER_CACHE_PATH];
    cache_path(path, url, "img");
    FILE *file = fopen(path, "rb");
    if (!file) return;
    bool matches = read_key(file, url);
    fclose(file);
    if (matches) unlink(path);
}
