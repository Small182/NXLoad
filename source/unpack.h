#ifndef NXLOAD_UNPACK_H
#define NXLOAD_UNPACK_H

#include <stddef.h>

#include "fichier.h"

bool package_name(const char *name);
bool rar_name(const char *name);
bool rar_signature(const char *path);
#define RAR_PACKAGE_LIMIT 24
typedef struct {
    char paths[RAR_PACKAGE_LIMIT][320];
    size_t count;
} UnpackedPackages;

const char *unpack_rar(FichierJob *job, const char *path, const char *directory,
                       UnpackedPackages *packages);

#endif
