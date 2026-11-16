#ifndef FICHIER_H
#define FICHIER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define FICHIER_INPUT_CAPACITY 256
#define MEGAUP_FILE_HOST "e13.megaupdownup.org"
#define BUNNY_NSWGF_URL "https://nswgf.com/a-pretty-odd-bunny-nintendo-switch-nsp-xci-nsz-download-free/"

typedef struct {
    atomic_bool cancelled;
    atomic_uint_fast64_t received;
    atomic_uint_fast64_t total;
    atomic_uint waiting;
    const char *result;
    long http;
    long api_http;
    char path[256];
    const char *url;
    bool megaup;
    bool dlarchive;
} FichierJob;

void fichier_download(FichierJob *job);

#endif
