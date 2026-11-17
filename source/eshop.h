#ifndef NXLOAD_ESHOP_H
#define NXLOAD_ESHOP_H

#include "catalog.h"

#define ESHOP_SCREENSHOTS 12

typedef struct {
    char title[CATALOG_TEXT];
    char publisher[CATALOG_TEXT];
    char description[1536];
    char released[16];
    char cover[CATALOG_URL];
    char page[CATALOG_URL];
    char issue[CATALOG_TEXT];
    char screenshots[ESHOP_SCREENSHOTS][CATALOG_URL];
    size_t screenshot_count;
} EshopGame;

typedef struct {
    Game game;
    char heading[CATALOG_TEXT];
    EshopGame match;
    atomic_bool cancel;
} EshopJob;

void eshop_lookup(EshopJob *job);

#endif
