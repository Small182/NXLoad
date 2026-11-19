#ifndef NXLOAD_UNPACK_H
#define NXLOAD_UNPACK_H

#include <stddef.h>

#include "fichier.h"

bool package_name(const char *name);
bool archive_name(const char *name);
bool archive_signature(const char *path);
#define ARCHIVE_PACKAGE_LIMIT 24
typedef struct {
    char paths[ARCHIVE_PACKAGE_LIMIT][320];
    size_t count;
} UnpackedPackages;

const char *unpack_archive(FichierJob *job, const char *path, const char *directory,
                       UnpackedPackages *packages);

#endif
