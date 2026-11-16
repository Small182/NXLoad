#ifndef FICHIER_H
#define FICHIER_H

#include <stdatomic.h>
#include <stdint.h>

#define FICHIER_SHARE_URL "https://1fichier.com/?u5n5c73gd1sgod2gegpx"

typedef struct {
    atomic_bool cancelled;
    atomic_uint_fast64_t received;
    atomic_uint_fast64_t total;
    atomic_uint waiting;
    const char *result;
    long http;
    char path[256];
    const char *url;
} FichierJob;

void fichier_download(FichierJob *job);

#endif
