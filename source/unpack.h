#ifndef NXLOAD_UNPACK_H
#define NXLOAD_UNPACK_H

#include "fichier.h"

bool package_name(const char *name);
bool rar_name(const char *name);
bool rar_signature(const char *path);
const char *unpack_rar(FichierJob *job, const char *path, const char *directory);

#endif
