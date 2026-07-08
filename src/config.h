#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

void config_load(APP_STATE *app);
void config_save(APP_STATE *app);
void config_get_path(wchar_t *buf, size_t size);

#endif
