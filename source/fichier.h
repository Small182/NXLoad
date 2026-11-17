#ifndef FICHIER_H
#define FICHIER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define FICHIER_INPUT_CAPACITY 256
#define DLARCHIVE_ID_CAPACITY 64
#define DOWNLOAD_DIR "sdmc:/switch/SwitchDownloadProbe"
#define MEGAUP_FILE_HOST "e13.megaupdownup.org"
#define BUNNY_NSWGF_URL "https://nswgf.com/a-pretty-odd-bunny-nintendo-switch-nsp-xci-nsz-download-free/"

typedef struct {
    atomic_bool cancelled;
    atomic_bool unpacking;
    atomic_uint_fast64_t received;
    atomic_uint_fast64_t total;
    atomic_uint streams;
    atomic_uint waiting;
    const char *result;
    long http;
    long api_http;
    uint64_t avg_speed;
    char tls_detail[128];
    char path[256];
    const char *url;
    bool megaup;
    bool dlarchive;
    bool datanodes;
    bool romshq;
    bool local_unpack;
    const char *dlarchive_id;
    const char *romshq_id;
} FichierJob;

void fichier_download(FichierJob *job);

#endif
