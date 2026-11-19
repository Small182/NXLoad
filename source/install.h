#ifndef NXLOAD_INSTALL_H
#define NXLOAD_INSTALL_H

#include "fichier.h"

typedef enum { INSTALL_UNKNOWN, INSTALL_NSP, INSTALL_XCI } InstallFormat;

InstallFormat install_format(const char *path);
const char *install_package(FichierJob *job, const char *path, InstallFormat format);

#endif
