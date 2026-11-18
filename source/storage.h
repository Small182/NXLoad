#ifndef STORAGE_H
#define STORAGE_H

#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#define NXLOAD_STORAGE_PREFIX "sdmc:/"
#else
#define NXLOAD_STORAGE_PREFIX "./"
#endif

#define NXLOAD_APP_RELATIVE "switch/nxload"
#define NXLOAD_DOWNLOAD_RELATIVE NXLOAD_APP_RELATIVE "/downloads"
#define NXLOAD_SWITCH_DIR NXLOAD_STORAGE_PREFIX "switch"
#define NXLOAD_APP_DIR NXLOAD_STORAGE_PREFIX NXLOAD_APP_RELATIVE
#define NXLOAD_DOWNLOAD_DIR NXLOAD_STORAGE_PREFIX NXLOAD_DOWNLOAD_RELATIVE

static inline bool nxload_ensure_directory(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    if (errno != EEXIST) return false;

    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static inline bool nxload_ensure_storage(void) {
    return nxload_ensure_directory(NXLOAD_SWITCH_DIR) &&
           nxload_ensure_directory(NXLOAD_APP_DIR) &&
           nxload_ensure_directory(NXLOAD_DOWNLOAD_DIR);
}

#endif
