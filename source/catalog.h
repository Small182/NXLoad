#ifndef CATALOG_H
#define CATALOG_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "fichier.h"

#define CATALOG_URL 512
#define CATALOG_TEXT 192
#define CATALOG_PAGE_GAMES 64
#define CATALOG_SEARCH_LIMIT 4096
#define CATALOG_DEFAULT_PAGE 1
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

typedef enum { CATALOG_PAGE, CATALOG_SEARCH, CATALOG_DETAILS } CatalogRequest;

typedef struct {
    CatalogRequest request;
    unsigned page;
    char query[96];
    Game game;
    atomic_bool cancel;
    atomic_uint searching_page;
    long http;
    char error[CATALOG_TEXT];
    Game games[CATALOG_SEARCH_LIMIT];
    size_t count;
    bool more;
    GameDetails details;
} CatalogJob;

void catalog_request(CatalogJob *job);

#endif
