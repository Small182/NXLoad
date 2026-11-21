#ifndef NXLOAD_SETTINGS_H
#define NXLOAD_SETTINGS_H

#include <stdbool.h>
#include <time.h>
#define UPDATE_BRANCH_MAX 80

#include "catalog.h"

typedef struct {
    bool install_default;
    bool delete_rar, delete_nsp;
    bool auto_catalog, source_enabled[CATALOG_SOURCE_COUNT], light_theme, english, test_updates;
    char update_branch[UPDATE_BRANCH_MAX];
    time_t catalog_checked[CATALOG_SOURCE_COUNT];
} Settings;

Settings load_settings(void);
bool save_settings(const Settings *s);
bool daily_check_due(time_t now, time_t checked);
bool valid_update_branch(const char *branch);

#endif
