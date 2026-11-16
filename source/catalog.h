#ifndef CATALOG_H
#define CATALOG_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "fichier.h"

#define CATALOG_URL 512
#define CATALOG_TEXT 192
#define CATALOG_PAGE_GAMES 64
#define CATALOG_CAPACITY 4096
#define CATALOG_MAX_PAGES 120
#define CATALOG_CACHE_TTL (24 * 60 * 60)
#define CATALOG_FILE_COUNT 24

typedef struct {
    char title[CATALOG_TEXT];
    char page[CATALOG_URL];
    char cover[CATALOG_URL];
} Game;

typedef struct {
    char title[CATALOG_TEXT];
    char id[DLARCHIVE_ID_CAPACITY];
} GameFile;

typedef struct {
    char title[CATALOG_TEXT];
    char info[CATALOG_TEXT];
    GameFile files[CATALOG_FILE_COUNT];
    size_t count;
    unsigned char *cover;
    size_t cover_bytes;
} GameDetails;

typedef struct {
    Game game;
    atomic_bool cancel;
    long http;
    char error[CATALOG_TEXT];
    GameDetails details;
} CatalogJob;

typedef struct {
    size_t count;
    time_t saved_at;
    Game games[CATALOG_CAPACITY];
} CatalogIndex;

typedef struct {
    CatalogIndex index;
    atomic_bool cancel;
    atomic_bool paused;
    atomic_size_t published;
    atomic_uint page;
    atomic_uint pages;
    bool complete;
    char error[CATALOG_TEXT];
} CatalogIndexJob;

void catalog_request(CatalogJob *job);
void catalog_build_index(CatalogIndexJob *job);
bool catalog_read_cache(CatalogIndex *index);
unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes);

#endif
