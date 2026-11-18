#ifndef NXLOAD_UPDATER_H
#define NXLOAD_UPDATER_H

#include "fichier.h"

const char *stage_update(FichierJob *job, const char *target);
bool apply_update(const char *target);

#endif
