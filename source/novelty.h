#ifndef NXLOAD_NOVELTY_H
#define NXLOAD_NOVELTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "catalog.h"

typedef struct {
    uint64_t key;
    uint64_t order;
} NoveltyRecord;

typedef struct {
    NoveltyRecord *records;
    size_t count, capacity;
    uint64_t next_order;
    unsigned initialized_sources;
} NoveltyTracker;

/* Existing cached titles are a baseline. Later discoveries get a greater
 * persistent order, regardless of the game's original release year. */
void novelty_load(NoveltyTracker *tracker);
void novelty_sync(NoveltyTracker *tracker,
                  const CatalogIndex indexes[CATALOG_SOURCE_COUNT],
                  unsigned source_mask);
uint64_t novelty_rank(const NoveltyTracker *tracker,
                      const CatalogIndex indexes[CATALOG_SOURCE_COUNT],
                      const CatalogEntry *entry);
void novelty_free(NoveltyTracker *tracker);

#endif
