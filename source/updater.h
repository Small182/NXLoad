#ifndef NXLOAD_UPDATER_H
#define NXLOAD_UPDATER_H

#include "fichier.h"

typedef struct {
    char id[24], digest[72], sha[41], version[24];
    uint64_t run;
} UpdateBuild;

typedef struct {
    atomic_bool cancelled;
    UpdateBuild build;
    const char *error;
} UpdateCheck;

void check_update(UpdateCheck *check);
const char *stage_update(FichierJob *job, const char *target, const UpdateBuild *build);
bool apply_update(const char *target, const char *sha);

#endif
