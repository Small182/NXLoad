#ifndef NXLOAD_STEAM_MEDIA_H
#define NXLOAD_STEAM_MEDIA_H

#include "catalog.h"
#include "trailers.h"

typedef struct {
    char screenshots[CATALOG_SCREENSHOTS][CATALOG_URL];
    size_t screenshot_count;
    char trailers[TRAILER_LIMIT][CATALOG_URL];
    size_t trailer_count;
    bool matched;
    int curl_error;
    long http_error;
} SteamMedia;

/* Search a verified matching Steam game. Returns screenshots and direct MP4
 * trailers from Steam's non-Nintendo CDN; never accepts an unrelated title. */
bool steam_media_lookup(const char *title, atomic_bool *cancel, SteamMedia *media);

#endif
