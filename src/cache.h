#ifndef CACHE_H
#define CACHE_H

#include "common.h"

#define CACHE_LOAD_FAILED 0
#define CACHE_LOAD_CURRENT 1
#define CACHE_LOAD_LEGACY 2

int cache_load_index(APP_STATE *app);
int cache_load_index_from_path(APP_STATE *app, const wchar_t *path);
int cache_save_index(APP_STATE *app);
int cache_save_index_to_path(APP_STATE *app, const wchar_t *path);
void cache_get_index_path(wchar_t *buf, size_t size);

#endif /* CACHE_H */
