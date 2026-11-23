#ifndef NXLOAD_COVER_CACHE_H
#define NXLOAD_COVER_CACHE_H

#include <stddef.h>

unsigned char *cover_cache_read(const char *url, size_t *bytes);
void cover_cache_write(const char *url, const unsigned char *data, size_t bytes);
void cover_cache_discard(const char *url);

#endif
