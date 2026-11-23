#ifndef NXLOAD_RANGE_H
#define NXLOAD_RANGE_H

#include "fichier.h"

typedef struct {
    const char *url;
    const char *referer;
    const char *ca_path;
} RangeSource;

typedef struct {
    uint64_t length;
    char etag[160];
    char disposition[256];
    long status;
} RangeInfo;

typedef enum {
    RANGE_OK,
    RANGE_RETRY_SINGLE,
    RANGE_INTERRUPTED,
    RANGE_RATE_LIMIT,
    RANGE_SD_ERROR
} RangeResult;

bool range_probe(const RangeSource *source, FichierJob *job, RangeInfo *info);
RangeResult range_download(const RangeSource *source, const RangeInfo *info, FichierJob *job, int fd);

#endif
