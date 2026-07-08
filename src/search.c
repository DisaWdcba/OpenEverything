#include "search.h"
#include "index.h"

#define SEARCH_LOCK_CHUNK 512

typedef struct {
    APP_STATE *app;
    const SEARCH_QUERY *query;
} SORT_CONTEXT;

static int compare_int64(long long a, long long b)
{
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static wchar_t search_lower_char(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z')
        return ch + (L'a' - L'A');
    if (ch < 128)
        return ch;
    return (wchar_t)towlower(ch);
}

static int search_mask_slot(wchar_t ch)
{
    ch = search_lower_char(ch);
    
    if (ch >= L'a' && ch <= L'z')
        return (int)(ch - L'a');
    if (ch >= L'0' && ch <= L'9')
        return 26 + (int)(ch - L'0');
    if (ch == L'_')
        return 36;
    if (ch == L'-')
        return 37;
    if (ch == L'.')
        return 38;
    if (ch == L' ')
        return 39;
    
    return -1;
}

static int search_contains_ignore_case(const wchar_t *text, const wchar_t *needle)
{
    wchar_t first;
    size_t needle_len;
    
    if (!text || !needle)
        return 0;
    if (!needle[0])
        return 1;
    
    first = search_lower_char(needle[0]);
    needle_len = wcslen(needle);
    
    for (const wchar_t *p = text; *p; p++) {
        if (search_lower_char(*p) != first)
            continue;
        
        size_t i = 1;
        while (i < needle_len && p[i] &&
               search_lower_char(p[i]) == search_lower_char(needle[i])) {
            i++;
        }
        if (i == needle_len)
            return 1;
    }
    
    return 0;
}

static int search_contains_folded(const wchar_t *text, const wchar_t *folded_needle, int needle_len)
{
    wchar_t first;
    
    if (!text || !folded_needle)
        return 0;
    if (needle_len <= 0)
        return 1;
    
    first = folded_needle[0];
    
    for (const wchar_t *p = text; *p; p++) {
        if (search_lower_char(*p) != first)
            continue;
        
        int i = 1;
        while (i < needle_len && p[i] &&
               search_lower_char(p[i]) == folded_needle[i]) {
            i++;
        }
        if (i == needle_len)
            return 1;
    }
    
    return 0;
}

static int search_contains_pre_folded(const wchar_t *folded_text, const wchar_t *folded_needle, int needle_len)
{
    wchar_t first;
    
    if (!folded_text || !folded_needle)
        return 0;
    if (needle_len <= 0)
        return 1;
    
    first = folded_needle[0];
    for (const wchar_t *p = folded_text; *p; p++) {
        if (*p != first)
            continue;
        if (wcsncmp(p, folded_needle, needle_len) == 0)
            return 1;
    }
    
    return 0;
}

void search_prepare_query(SEARCH_QUERY *query)
{
    int i;
    
    if (!query)
        return;
    
    query->text_len = (int)wcslen(query->text);
    if (query->text_len > 511)
        query->text_len = 511;
    
    for (i = 0; i < query->text_len; i++)
        query->folded_text[i] = search_lower_char(query->text[i]);
    query->folded_text[i] = L'\0';
    query->folded_ready = 1;
    
    query->char_mask = 0;
    for (i = 0; i < query->text_len; i++) {
        int slot = search_mask_slot(query->text[i]);
        if (slot >= 0)
            query->char_mask |= 1ULL << slot;
    }
}

static int compare_entries_for_query(const INDEX_ENTRY *a, const INDEX_ENTRY *b, int column)
{
    int result = 0;
    
    switch (column) {
        case COL_PATH:
            result = _wcsicmp(a->path ? a->path : L"", b->path ? b->path : L"");
            break;
        case COL_SIZE:
            result = compare_int64(a->size, b->size);
            break;
        case COL_DATE_MODIFIED:
            result = compare_int64(a->modification_time, b->modification_time);
            break;
        case COL_DATE_CREATED:
            result = compare_int64(a->creation_time, b->creation_time);
            break;
        case COL_ATTRIBUTES:
            if (a->attributes < b->attributes) result = -1;
            else if (a->attributes > b->attributes) result = 1;
            break;
        case COL_NAME:
        default:
            result = _wcsicmp(a->name ? a->name : L"", b->name ? b->name : L"");
            break;
    }
    
    if (result == 0)
        result = _wcsicmp(a->path ? a->path : L"", b->path ? b->path : L"");
    if (result == 0)
        result = compare_int64(a->file_ref, b->file_ref);
    
    return result;
}

static int compare_filtered_indices_ctx(void *ctx, const void *lhs, const void *rhs)
{
    SORT_CONTEXT *sort = (SORT_CONTEXT *)ctx;
    int ia = *(const int *)lhs;
    int ib = *(const int *)rhs;
    INDEX_ENTRY *a = &sort->app->entries[ia];
    INDEX_ENTRY *b = &sort->app->entries[ib];
    int result = compare_entries_for_query(a, b, sort->query->sort_column);
    
    return sort->query->sort_ascending ? result : -result;
}

static int match_wildcard(const wchar_t *text, const wchar_t *pattern, int case_sensitive)
{
    if (!text || !pattern) return 0;
    
    int ti = 0, pi = 0;
    int star_ti = -1, star_pi = -1;
    
    while (text[ti]) {
        if (pattern[pi] == L'*') {
            star_ti = ti;
            star_pi = pi;
            pi++;
        } else if (pattern[pi] == L'?' || 
                   (case_sensitive ? pattern[pi] == text[ti] 
                                   : towlower(pattern[pi]) == towlower(text[ti]))) {
            ti++;
            pi++;
        } else if (star_pi != -1) {
            ti = ++star_ti;
            pi = star_pi + 1;
        } else {
            return 0;
        }
    }
    
    while (pattern[pi] == L'*') pi++;
    return pattern[pi] == L'\0';
}

static int match_whole_word(const wchar_t *text, const wchar_t *word, int case_sensitive)
{
    const wchar_t *p = text;
    size_t word_len = wcslen(word);
    
    while (*p) {
        while (*p && !iswalnum(*p)) p++;
        if (!*p) break;
        
        const wchar_t *wstart = p;
        while (*p && iswalnum(*p)) p++;
        size_t len = p - wstart;
        
        if (len == word_len) {
            int match = case_sensitive 
                ? (wcsncmp(wstart, word, len) == 0)
                : (_wcsnicmp(wstart, word, len) == 0);
            if (match) return 1;
        }
    }
    return 0;
}

int search_match_entry(INDEX_ENTRY *entry, const SEARCH_QUERY *query)
{
    if (!query->text[0]) return 1;
    
    const wchar_t *target = query->match_path ? entry->path : entry->name;
    const wchar_t *folded_target = query->match_path ? NULL : entry->folded_name;
    if (!target) return 0;
    
    /* Handle special type filters */
    if (wcsstr(query->text, L"ext:")) {
        /* Extension filter */
        const wchar_t *exts = wcsstr(query->text, L"ext:") + 4;
        wchar_t exts_copy[512];
        wcscpy_s(exts_copy, 512, exts);
        
        wchar_t *ctx = NULL;
        wchar_t *ext = wcstok_s(exts_copy, L";", &ctx);
        while (ext) {
            if (_wcsicmp(entry->extension ? entry->extension : L"", ext) == 0) return 1;
            ext = wcstok_s(NULL, L";", &ctx);
        }
        return 0;
    }
    
    if (wcsstr(query->text, L"folder:")) {
        return entry->is_directory ? 1 : 0;
    }
    
    if (query->char_mask) {
        unsigned long long entry_mask = query->match_path
            ? entry->path_char_mask
            : entry->name_char_mask;
        if ((entry_mask & query->char_mask) != query->char_mask)
            return 0;
    }
    
    /* Regular matching */
    if (query->match_whole_word) {
        return match_whole_word(target, query->text, query->match_case);
    }
    
    if (!wcschr(query->text, L'*') && !wcschr(query->text, L'?')) {
        return query->match_case
            ? (wcsstr(target, query->text) != NULL)
            : (query->folded_ready
                ? (folded_target
                    ? search_contains_pre_folded(folded_target, query->folded_text, query->text_len)
                    : search_contains_folded(target, query->folded_text, query->text_len))
                : search_contains_ignore_case(target, query->text));
    }
    
    /* Wildcard match - wrap with * if no wildcards */
    wchar_t pattern[512];
    wcscpy_s(pattern, 512, query->text);
    
    return match_wildcard(target, pattern, query->match_case);
}

void search_sort_results(APP_STATE *app)
{
    search_sort_indices(app, &app->query, app->filtered_indices, app->filtered_count);
}

void search_sort_indices(APP_STATE *app, const SEARCH_QUERY *query, int *indices, int count)
{
    SORT_CONTEXT ctx;
    
    if (count <= 1)
        return;
    if (query->sort_column == COL_NAME && query->sort_ascending)
        return;
    
    ctx.app = app;
    ctx.query = query;
    qsort_s(indices, count, sizeof(int), compare_filtered_indices_ctx, &ctx);
}

void search_execute(APP_STATE *app)
{
    search_prepare_query(&app->query);
    
    EnterCriticalSection(&app->index_lock);
    
    app->filtered_count = 0;
    
    for (int i = 0; i < app->entry_count && app->filtered_count < SEARCH_MAX_RESULTS; i++) {
        if (search_match_entry(&app->entries[i], &app->query)) {
            app->filtered_indices[app->filtered_count++] = i;
        }
    }
    
    search_sort_results(app);
    
    LeaveCriticalSection(&app->index_lock);
}

int search_execute_subset_to_buffer(APP_STATE *app, const SEARCH_QUERY *query,
                                    int *out_indices, int max_results,
                                    const int *base_indices, int base_count,
                                    int base_identity, LONG generation)
{
    int count = 0;
    int i = 0;
    
    if (!out_indices || max_results <= 0)
        return 0;
    
    if (!query->text[0]) {
        int entry_count;
        int limit;
        
        EnterCriticalSection(&app->index_lock);
        entry_count = app->entry_count;
        LeaveCriticalSection(&app->index_lock);
        
        limit = entry_count < max_results ? entry_count : max_results;
        for (int n = 0; n < limit; n++)
            out_indices[n] = n;
        
        count = limit;
        if (generation != app->search_generation)
            return count;
        if (!(query->sort_column == COL_NAME && query->sort_ascending)) {
            EnterCriticalSection(&app->index_lock);
            search_sort_indices(app, query, out_indices, count);
            LeaveCriticalSection(&app->index_lock);
        }
        return count;
    }
    
    if ((base_indices || base_identity) && base_count > 0) {
        for (;;) {
            int end;
            int entry_count;
            
            EnterCriticalSection(&app->index_lock);
            
            entry_count = app->entry_count;
            if (i >= base_count || count >= max_results) {
                LeaveCriticalSection(&app->index_lock);
                break;
            }
            
            end = i + SEARCH_LOCK_CHUNK;
            if (end > base_count)
                end = base_count;
            
            for (; i < end && count < max_results; i++) {
                int idx = base_identity ? i : base_indices[i];
                if (idx >= 0 && idx < entry_count &&
                    search_match_entry(&app->entries[idx], query))
                    out_indices[count++] = idx;
            }
            
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
            Sleep(0);
        }
    } else {
        for (;;) {
            int end;
            int entry_count;
            
            EnterCriticalSection(&app->index_lock);
            
            entry_count = app->entry_count;
            if (i >= entry_count || count >= max_results) {
                LeaveCriticalSection(&app->index_lock);
                break;
            }
            
            end = i + SEARCH_LOCK_CHUNK;
            if (end > entry_count)
                end = entry_count;
            
            for (; i < end && count < max_results; i++) {
                if (search_match_entry(&app->entries[i], query))
                    out_indices[count++] = i;
            }
            
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
            Sleep(0);
        }
    }
    
    if (generation != app->search_generation)
        return count;
    
    if (!(query->sort_column == COL_NAME && query->sort_ascending)) {
        int valid_count = 0;
        int entry_count;
        
        EnterCriticalSection(&app->index_lock);
        entry_count = app->entry_count;
        for (int n = 0; n < count; n++) {
            if (out_indices[n] >= 0 && out_indices[n] < entry_count)
                out_indices[valid_count++] = out_indices[n];
        }
        count = valid_count;
        search_sort_indices(app, query, out_indices, count);
        LeaveCriticalSection(&app->index_lock);
    }
    return count;
}

int search_execute_to_buffer(APP_STATE *app, const SEARCH_QUERY *query, int *out_indices, int max_results)
{
    return search_execute_subset_to_buffer(app, query, out_indices, max_results,
                                           NULL, 0, 0, app->search_generation);
}
