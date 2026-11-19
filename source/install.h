#ifndef NXLOAD_INSTALL_H
#define NXLOAD_INSTALL_H

#include "fichier.h"

bool nsp_name(const char *path);
bool nsp_signature(const char *path);
const char *install_nsp(FichierJob *job, const char *path);

#endif
