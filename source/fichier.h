#ifndef FICHIER_H
#define FICHIER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define FICHIER_INPUT_CAPACITY 256
#define DLARCHIVE_ID_CAPACITY 64
#define MEGAUP_FILE_HOST "e13.megaupdownup.org"
#define BUNNY_NSWGF_URL "https://nswgf.com/a-pretty-odd-bunny-nintendo-switch-nsp-xci-nsz-download-free/"

typedef struct {
    atomic_bool cancelled;
    atomic_bool unpacking;
    atomic_bool copying_port_files;
    atomic_bool installing;
    atomic_bool yield_for_game_requests;
    atomic_bool update_received;
    atomic_uint_fast64_t received;
    atomic_uint_fast64_t total;
    atomic_uint streams;
    atomic_uint waiting;
    const char *result;
    long http;
    long api_http;
    uint64_t avg_speed;
    char tls_detail[128];
    char install_error[192];
    char path[256];
    const char *url;
    bool megaup;
    bool dlarchive;
    bool datanodes;
    bool romshq;
    bool local_unpack;
    bool local_install;
    bool install_after_download;
    bool homebrew_port;
    bool app_update, update_staged;
    bool delete_rar, delete_nsp;
    const char *dlarchive_id;
    const char *romshq_id;
} FichierJob;

void fichier_download(FichierJob *job);

#endif
