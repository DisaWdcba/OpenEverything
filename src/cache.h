#ifndef CACHE_H
#define CACHE_H

#include "common.h"

int cache_load_index(APP_STATE *app);
int cache_save_index(APP_STATE *app);
void cache_get_index_path(wchar_t *buf, size_t size);

#endif /* CACHE_H */
