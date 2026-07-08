#ifndef SEARCH_H
#define SEARCH_H

#include "common.h"

void search_prepare_query(SEARCH_QUERY *query);
void search_execute(APP_STATE *app);
int search_execute_to_buffer(APP_STATE *app, const SEARCH_QUERY *query, int *out_indices, int max_results);
int search_execute_subset_to_buffer(APP_STATE *app, const SEARCH_QUERY *query,
                                    int *out_indices, int max_results,
                                    const int *base_indices, int base_count,
                                    int base_identity, LONG generation);
int search_match_entry(INDEX_ENTRY *entry, const SEARCH_QUERY *query);
void search_sort_results(APP_STATE *app);
void search_sort_indices(APP_STATE *app, const SEARCH_QUERY *query, int *indices, int count);

#endif /* SEARCH_H */
