#ifndef CATALOG_H
#define CATALOG_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <curl/curl.h>
#include "fichier.h"

#define CATALOG_URL 512
#define CATALOG_TEXT 192
#define CATALOG_PAGE_GAMES 64
#define CATALOG_CAPACITY 8192
#define MERGED_CAPACITY (2 * CATALOG_CAPACITY)
#define CATALOG_MAX_PAGES 512
#define CATALOG_CACHE_TTL (24 * 60 * 60)
#define CATALOG_FILE_COUNT 24
#define CATALOG_SCREENSHOTS 32

typedef enum { CATALOG_NSROMS, CATALOG_ROMSHQ } CatalogSource;
typedef enum { FILE_DLARCHIVE, FILE_ROMSHQ_SIGNED, FILE_DATANODES, FILE_1FICHIER } GameFileSource;

typedef struct {
    char title[CATALOG_TEXT];
    char page[CATALOG_URL];
    char cover[CATALOG_URL];
} Game;

typedef struct {
    const char *title, *eshop_icon, *cover, *nintendo_life;
    unsigned released, genres, rating;
} CatalogMetadata;

typedef struct {
    const Game *games[2];
    const CatalogMetadata *metadata;
} CatalogEntry;

typedef struct {
    char title[CATALOG_TEXT];
    char id[DLARCHIVE_ID_CAPACITY];
    char url[CATALOG_URL];
    GameFileSource source;
    char size[32];
} GameFile;

typedef struct {
    char title[CATALOG_TEXT];
    GameFile files[CATALOG_FILE_COUNT];
    size_t count;
    char screenshots[CATALOG_SCREENSHOTS][CATALOG_URL];
    size_t screenshot_count;
    char cover_url[CATALOG_URL];
} GameDetails;

typedef struct {
    Game game;
    CatalogSource source;
    atomic_bool cancel;
    long http;
    char error[CATALOG_TEXT];
    GameDetails details;
} CatalogJob;

typedef struct {
    long http;
    long verify;
    int curl;
} CatalogFetchStatus;

typedef struct {
    size_t count;
    time_t saved_at;
    Game games[CATALOG_CAPACITY];
} CatalogIndex;

typedef struct {
    CatalogIndex index;
    CatalogSource source;
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
bool catalog_read_cache(CatalogIndex *index, CatalogSource source);
unsigned char *catalog_fetch_cover(const char *url, atomic_bool *cancel, size_t *bytes);
unsigned char *catalog_fetch_thumbnail(CURL *connection, const char *url, atomic_bool *cancel, size_t *bytes);
unsigned char *catalog_fetch_document(const char *url, atomic_bool *cancel, size_t *bytes,
                                      CatalogFetchStatus *status);

#endif
