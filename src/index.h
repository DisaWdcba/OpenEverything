#ifndef INDEX_H
#define INDEX_H

#include "common.h"

void index_init(APP_STATE *app);
void index_name_pool_init(INDEX_NAME_POOL *pool);
void index_name_pool_free(INDEX_NAME_POOL *pool);
int index_name_pool_append_wide(INDEX_NAME_POOL *pool, const wchar_t *name,
                                size_t name_length, unsigned int *offset,
                                unsigned short *utf8_length);
int index_name_pool_append_utf8(INDEX_NAME_POOL *pool, const char *name,
                                size_t name_length, unsigned int *offset,
                                unsigned short *utf8_length);
const char *index_name_pool_at(const INDEX_NAME_POOL *pool,
                               unsigned int offset, unsigned short length);
void index_build_init(INDEX_BUILD *build);
void index_build_free(INDEX_BUILD *build);
const char *index_entry_name_utf8_locked(const APP_STATE *app,
                                          const INDEX_ENTRY *entry);
int index_copy_entry_name_locked(const APP_STATE *app, const INDEX_ENTRY *entry,
                                 wchar_t *buffer, size_t capacity);
wchar_t *index_duplicate_entry_name_locked(const APP_STATE *app,
                                            const INDEX_ENTRY *entry);
int index_copy_entry_extension_locked(const APP_STATE *app,
                                      const INDEX_ENTRY *entry,
                                      wchar_t *buffer, size_t capacity);
int index_compare_entry_names_locked(const APP_STATE *app,
                                     const INDEX_ENTRY *left,
                                     const INDEX_ENTRY *right);
int index_set_entry_name_locked(APP_STATE *app, INDEX_ENTRY *entry,
                                const wchar_t *name, size_t name_length);
void index_prepare_entry(APP_STATE *app, INDEX_ENTRY *entry);
int index_compact_entry_names(APP_STATE *app);
void index_clear(APP_STATE *app);
int index_add_entry(APP_STATE *app, INDEX_ENTRY *entry, const wchar_t *name);
int index_add_entries(APP_STATE *app, INDEX_BUILD *build);
int index_apply_usn_changes(APP_STATE *app, USN_CHANGE *changes, int count);
void index_build_paths(APP_STATE *app);
int index_build_name_char_index(APP_STATE *app);
void index_clear_name_char_index(APP_STATE *app);
int index_build_filter_index(APP_STATE *app);
void index_clear_filter_index(APP_STATE *app);
int index_build_ref_index(APP_STATE *app);
void index_clear_ref_index(APP_STATE *app);
/* The caller must hold index_lock. Repairs parent_index using the ref index. */
int index_resolve_parent_locked(APP_STATE *app, INDEX_ENTRY *entry);
void index_sort_entries_by_name(APP_STATE *app);
void index_free_entry(INDEX_ENTRY *entry);
/* The caller must hold app->index_lock while using these helpers. */
wchar_t *index_duplicate_entry_path_locked(APP_STATE *app, int entry_index);
size_t index_entry_path_length_locked(APP_STATE *app, int entry_index);

#endif /* INDEX_H */
