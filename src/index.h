#ifndef INDEX_H
#define INDEX_H

#include "common.h"

void index_init(APP_STATE *app);
void index_prepare_entry(INDEX_ENTRY *entry);
void index_clear(APP_STATE *app);
int index_add_entry(APP_STATE *app, INDEX_ENTRY *entry);
int index_add_entries(APP_STATE *app, INDEX_ENTRY *entries, int count);
int index_apply_usn_changes(APP_STATE *app, USN_CHANGE *changes, int count);
void index_build_paths(APP_STATE *app);
int index_build_name_char_index(APP_STATE *app);
void index_clear_name_char_index(APP_STATE *app);
int index_build_ref_index(APP_STATE *app);
void index_clear_ref_index(APP_STATE *app);
void index_sort_entries_by_name(APP_STATE *app);
int index_sort_by_name(const void *a, const void *b);
int index_sort_by_path(const void *a, const void *b);
int index_sort_by_size(const void *a, const void *b);
int index_sort_by_date_modified(const void *a, const void *b);
int index_sort_by_date_created(const void *a, const void *b);
int index_sort_by_attributes(const void *a, const void *b);
void index_free_entry(INDEX_ENTRY *entry);

#endif /* INDEX_H */
