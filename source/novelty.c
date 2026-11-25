#define _GNU_SOURCE
#include "novelty.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NOVELTY_PATH NXLOAD_APP_DIR "/catalog-added.bin"
#define NOVELTY_BACKUP NOVELTY_PATH ".prev"
#define NOVELTY_TEMP NOVELTY_PATH ".tmp"
#define NOVELTY_MAGIC "NXLNEW01"
#define NOVELTY_BASE UINT64_C(4294967296)
#define NOVELTY_MAX_RECORDS 131072

typedef struct {
    char magic[8];
    uint32_t count, initialized_sources;
    uint64_t next_order, checksum;
} NoveltyHeader;

static uint64_t hash_url(unsigned source, const char *url) {
    uint64_t result = UINT64_C(14695981039346656037);
    result ^= (unsigned char)(source + 1);
    result *= UINT64_C(1099511628211);
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        result ^= *p;
        result *= UINT64_C(1099511628211);
    }
    return result ? result : 1;
}

static uint64_t checksum_records(const NoveltyRecord *items, size_t count) {
    const unsigned char *p = (const unsigned char *)items;
    uint64_t result = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count * sizeof(*items); ++i) {
        result ^= p[i];
        result *= UINT64_C(1099511628211);
    }
    return result;
}

static int compare_records(const void *aa, const void *bb) {
    const NoveltyRecord *a = aa, *b = bb;
    return (a->key > b->key) - (a->key < b->key);
}

static const NoveltyRecord *lookup(const NoveltyTracker *tracker, uint64_t key) {
    NoveltyRecord wanted = {.key = key};
    return tracker->count ?
        bsearch(&wanted, tracker->records, tracker->count,
                sizeof(*tracker->records), compare_records) : NULL;
}

static bool read_records(const char *path, NoveltyTracker *tracker) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    NoveltyHeader header = {0};
    bool ok = fread(&header, sizeof(header), 1, file) == 1 &&
        !memcmp(header.magic, NOVELTY_MAGIC, sizeof(header.magic)) &&
        header.count <= NOVELTY_MAX_RECORDS &&
        header.initialized_sources < (1u << CATALOG_SOURCE_COUNT) &&
        header.next_order >= NOVELTY_BASE;
    NoveltyRecord *records = NULL;
    if (ok && header.count) {
        records = calloc(header.count, sizeof(*records));
        ok = records &&
            fread(records, sizeof(*records), header.count, file) == header.count;
    }
    if (ok) {
        ok = fgetc(file) == EOF && !ferror(file) &&
            checksum_records(records, header.count) == header.checksum;
        for (uint32_t i = 0; ok && i < header.count; ++i) {
            if (!records[i].key || !records[i].order ||
                (i && records[i - 1].key >= records[i].key))
                ok = false;
        }
    }
    fclose(file);
    if (!ok) {
        free(records);
        return false;
    }
    tracker->records = records;
    tracker->count = tracker->capacity = header.count;
    tracker->next_order = header.next_order;
    tracker->initialized_sources = header.initialized_sources;
    return true;
}

static bool reserve(NoveltyTracker *tracker, size_t count) {
    if (count > NOVELTY_MAX_RECORDS) return false;
    if (count <= tracker->capacity) return true;
    size_t size = tracker->capacity ? tracker->capacity : 4096;
    while (size < count) size *= 2;
    if (size > NOVELTY_MAX_RECORDS) size = NOVELTY_MAX_RECORDS;
    NoveltyRecord *records = realloc(tracker->records, size * sizeof(*records));
    if (!records) return false;
    tracker->records = records;
    tracker->capacity = size;
    return true;
}

static bool save_records(const NoveltyTracker *tracker) {
    if (!nxload_ensure_storage()) return false;
    NoveltyHeader header = {0};
    memcpy(header.magic, NOVELTY_MAGIC, sizeof(header.magic));
    header.count = (uint32_t)tracker->count;
    header.initialized_sources = tracker->initialized_sources;
    header.next_order = tracker->next_order;
    header.checksum = checksum_records(tracker->records, tracker->count);
    FILE *file = fopen(NOVELTY_TEMP, "wb");
    bool ok = file && fwrite(&header, sizeof(header), 1, file) == 1 &&
        fwrite(tracker->records, sizeof(*tracker->records), tracker->count, file) == tracker->count &&
        !fflush(file) && !fsync(fileno(file));
    if (file && fclose(file)) ok = false;
    if (!ok) {
        unlink(NOVELTY_TEMP);
        return false;
    }
    bool backed_up = false;
    if (!access(NOVELTY_PATH, F_OK)) {
        unlink(NOVELTY_BACKUP);
        if (rename(NOVELTY_PATH, NOVELTY_BACKUP)) {
            unlink(NOVELTY_TEMP);
            return false;
        }
        backed_up = true;
    }
    if (rename(NOVELTY_TEMP, NOVELTY_PATH)) {
        if (backed_up) rename(NOVELTY_BACKUP, NOVELTY_PATH);
        unlink(NOVELTY_TEMP);
        return false;
    }
    unlink(NOVELTY_BACKUP);
    return true;
}

void novelty_load(NoveltyTracker *tracker) {
    *tracker = (NoveltyTracker){.next_order = NOVELTY_BASE};
    if (!read_records(NOVELTY_PATH, tracker))
        read_records(NOVELTY_BACKUP, tracker);
}

void novelty_sync(NoveltyTracker *tracker,
                  const CatalogIndex indexes[CATALOG_SOURCE_COUNT],
                  unsigned source_mask) {
    bool changed = false;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        const unsigned bit = 1u << source;
        const CatalogIndex *index = &indexes[source];
        if (!(source_mask & bit) || !index->count) continue;
        if (!reserve(tracker, tracker->count + index->count)) continue;
        bool baseline = !(tracker->initialized_sources & bit);
        // Source catalog pages normally show new additions first; scanning
        // backward gives the first item the largest order in each refresh.
        size_t previous_count = tracker->count;
        for (size_t i = index->count; i > 0; --i) {
            const uint64_t key = hash_url(source, index->games[i - 1].page);
            // Appended records are unsorted until the end of this source pass.
            NoveltyRecord wanted = {.key = key};
            if (previous_count &&
                bsearch(&wanted, tracker->records, previous_count,
                        sizeof(*tracker->records), compare_records))
                continue;
            const uint64_t order = baseline ?
                (uint64_t)(CATALOG_CAPACITY - (i - 1)) : ++tracker->next_order;
            tracker->records[tracker->count++] = (NoveltyRecord){.key = key, .order = order};
        }
        if (tracker->count > previous_count)
            qsort(tracker->records, tracker->count,
                  sizeof(*tracker->records), compare_records);
        if (baseline) {
            tracker->initialized_sources |= bit;
            changed = true;
        }
        if (tracker->count > previous_count) changed = true;
    }
    if (changed) save_records(tracker);
}

uint64_t novelty_rank(const NoveltyTracker *tracker,
                      const CatalogIndex indexes[CATALOG_SOURCE_COUNT],
                      const CatalogEntry *entry) {
    uint64_t best = 0;
    for (unsigned source = 0; source < CATALOG_SOURCE_COUNT; ++source) {
        const Game *game = entry->games[source];
        if (!game) continue;
        const NoveltyRecord *record = lookup(tracker, hash_url(source, game->page));
        uint64_t value = record ? record->order : 0;
        if (!value) {
            // Untracked entries from a partial download still have a useful
            // ordering based on their position in that source's catalog.
            const Game *start = indexes[source].games;
            if (game >= start && game < start + indexes[source].count)
                value = CATALOG_CAPACITY - (uint64_t)(game - start);
        }
        if (value > best) best = value;
    }
    return best;
}

void novelty_free(NoveltyTracker *tracker) {
    free(tracker->records);
    *tracker = (NoveltyTracker){0};
}
