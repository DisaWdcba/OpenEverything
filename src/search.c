#include "search.h"
#include "index.h"

#define SEARCH_LOCK_CHUNK 512
#define SEARCH_SCOPE_PART_MAX 512

typedef struct {
    APP_STATE *app;
    const SEARCH_QUERY *query;
} SORT_CONTEXT;

typedef struct {
    const wchar_t *start;
    size_t length;
} SEARCH_SCOPE_PART;

static int search_find_scope_volume_locked(const APP_STATE *app,
                                           const SEARCH_QUERY *query);
static int search_entry_is_volume_root_locked(const APP_STATE *app,
                                              const INDEX_ENTRY *entry);

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

static int search_matches_filter(const INDEX_ENTRY *entry, int filter_id)
{
    if (filter_id <= FILTER_EVERYTHING || filter_id >= FILTER_COUNT)
        return 1;
    return entry->filter_type == filter_id;
}

static int search_scope_cache_matches_locked(const APP_STATE *app,
                                             const SEARCH_QUERY *query)
{
    return app && query && app->scope_index_ready &&
           app->scope_index_revision == app->index_revision &&
           app->scope_index_include_subfolders == query->include_subfolders &&
           _wcsicmp(app->scope_index_path, query->folder_scope) == 0;
}

static int search_parse_scope(const SEARCH_QUERY *query,
                              SEARCH_SCOPE_PART *parts, int capacity)
{
    const wchar_t *cursor;
    size_t length;
    int count = 0;

    if (!query || !parts || capacity <= 0 || !query->folder_scope[0])
        return 0;
    length = wcslen(query->folder_scope);
    if (length < 3 || query->folder_scope[1] != L':' ||
        (query->folder_scope[2] != L'\\' && query->folder_scope[2] != L'/'))
        return -1;

    cursor = query->folder_scope + 3;
    while (*cursor) {
        const wchar_t *start;
        while (*cursor == L'\\' || *cursor == L'/')
            cursor++;
        if (!*cursor)
            break;
        start = cursor;
        while (*cursor && *cursor != L'\\' && *cursor != L'/')
            cursor++;
        if (count >= capacity)
            return -1;
        parts[count].start = start;
        parts[count].length = (size_t)(cursor - start);
        count++;
    }
    return count;
}

static int search_entry_name_matches_part_locked(const APP_STATE *app,
                                                 const INDEX_ENTRY *entry,
                                                 SEARCH_SCOPE_PART part)
{
    wchar_t name[SEARCH_FOLDER_SCOPE_MAX];
    size_t name_length;

    if (!index_copy_entry_name_locked(app, entry, name,
                                      sizeof(name) / sizeof(name[0])))
        return 0;
    name_length = wcslen(name);
    return name_length == part.length &&
           _wcsnicmp(name, part.start, part.length) == 0;
}

static int search_entry_parent_is_root_locked(const APP_STATE *app,
                                              const INDEX_ENTRY *entry)
{
    return entry->parent_ref == 0 || entry->parent_ref == NTFS_ROOT_FRN ||
           search_entry_is_volume_root_locked(app, entry);
}

static int search_scope_path_matches_entry_locked(APP_STATE *app,
                                                  int entry_index,
                                                  SEARCH_SCOPE_PART *parts,
                                                  int part_count)
{
    int current = entry_index;

    for (int part = part_count - 1; part >= 0; part--) {
        int parent;

        if (current < 0 || current >= app->entry_count ||
            !search_entry_name_matches_part_locked(
                app, &app->entries[current], parts[part]))
            return 0;
        if (part == 0)
            return search_entry_parent_is_root_locked(
                app, &app->entries[current]);
        parent = index_resolve_parent_locked(app, &app->entries[current]);
        if (parent < 0 || parent == current)
            return 0;
        current = parent;
    }
    return 0;
}

static int search_find_scope_folder_locked(APP_STATE *app,
                                            const SEARCH_QUERY *query,
                                            SEARCH_SCOPE_PART *parts,
                                            int part_count,
                                            int volume)
{
    if (part_count <= 0 || volume < 0)
        return -1;
    for (int i = 0; i < app->entry_count; i++) {
        INDEX_ENTRY *entry = &app->entries[i];
        if (!entry->is_directory || entry->volume_index != volume)
            continue;
        if (search_scope_path_matches_entry_locked(
                app, i, parts, part_count))
            return i;
    }
    return -1;
}

static int search_scope_entry_is_descendant_locked(APP_STATE *app,
                                                   int entry_index,
                                                   int target_index,
                                                   int include_subfolders)
{
    int current;

    if (entry_index == target_index)
        return 0;
    current = entry_index;
    for (int depth = 0; depth < SEARCH_SCOPE_PART_MAX; depth++) {
        int parent = index_resolve_parent_locked(
            app, &app->entries[current]);
        if (parent == target_index)
            return 1;
        if (!include_subfolders || parent < 0 || parent == current)
            return 0;
        current = parent;
        if (search_entry_parent_is_root_locked(app, &app->entries[current]))
            return 0;
    }
    return 0;
}

static int search_scope_append_candidate_locked(APP_STATE *app, int index)
{
    int new_capacity;
    int *resized;

    if (!app || app->scope_index_count >= app->entry_count)
        return 0;
    if (app->scope_index_count >= app->scope_index_capacity) {
        if (app->scope_index_capacity <= 0)
            new_capacity = app->entry_count < 256 ? app->entry_count : 256;
        else if (app->scope_index_capacity > app->entry_count / 2)
            new_capacity = app->entry_count;
        else
            new_capacity = app->scope_index_capacity * 2;
        resized = (int *)realloc(
            app->scope_index_pool,
            (size_t)new_capacity * sizeof(*app->scope_index_pool));
        if (!resized)
            return 0;
        app->scope_index_pool = resized;
        app->scope_index_capacity = new_capacity;
    }
    app->scope_index_pool[app->scope_index_count++] = index;
    return 1;
}

static int search_build_scope_index_locked(APP_STATE *app,
                                           const SEARCH_QUERY *query)
{
    SEARCH_SCOPE_PART parts[SEARCH_SCOPE_PART_MAX];
    int part_count;
    int volume;
    int target;
    int *volume_indices;
    int volume_count;

    if (!app || !query || !query->folder_scope[0] ||
        query->scope_is_volume_root)
        return 0;
    if (search_scope_cache_matches_locked(app, query))
        return 1;

    part_count = search_parse_scope(query, parts,
                                    SEARCH_SCOPE_PART_MAX);
    volume = search_find_scope_volume_locked(app, query);
    target = part_count > 0
        ? search_find_scope_folder_locked(app, query, parts, part_count, volume)
        : -1;
    free(app->scope_index_pool);
    app->scope_index_pool = NULL;
    app->scope_index_count = 0;
    app->scope_index_capacity = 0;
    app->scope_index_target = target;
    app->scope_index_volume = volume;
    app->scope_index_include_subfolders = query->include_subfolders;
    app->scope_index_revision = app->index_revision;
    wcsncpy_s(app->scope_index_path, SEARCH_FOLDER_SCOPE_MAX,
              query->folder_scope, _TRUNCATE);
    app->scope_index_ready = 1;
    if (target < 0 || volume < 0)
        return 1;

    volume_indices = app->volume_indices[volume];
    volume_count = app->volume_counts[volume];
    for (int phase = 0; phase < 2; phase++) {
        int source_count = volume_indices ? volume_count : app->entry_count;
        for (int source = 0; source < source_count; source++) {
            int index = volume_indices ? volume_indices[source] : source;
            INDEX_ENTRY *entry;

            if (index < 0 || index >= app->entry_count)
                continue;
            entry = &app->entries[index];
            if ((phase == 0) != (entry->is_directory != 0) ||
                (!volume_indices && entry->volume_index != volume))
                continue;
            if (search_scope_entry_is_descendant_locked(
                    app, index, target, query->include_subfolders) &&
                !search_scope_append_candidate_locked(app, index)) {
                free(app->scope_index_pool);
                app->scope_index_pool = NULL;
                app->scope_index_count = 0;
                app->scope_index_capacity = 0;
                app->scope_index_ready = 0;
                return 0;
            }
        }
    }
    return 1;
}

static int search_find_scope_volume_locked(const APP_STATE *app,
                                           const SEARCH_QUERY *query)
{
    wchar_t drive;

    if (!app || !query || !query->scope_drive_letter)
        return -1;
    drive = towupper(query->scope_drive_letter);
    for (int volume = 0; volume < app->volume_count && volume < 26; volume++) {
        if (towupper(app->volumes[volume].drive_letter[0]) == drive)
            return volume;
    }
    return -1;
}

static int search_entry_is_volume_root_locked(const APP_STATE *app,
                                               const INDEX_ENTRY *entry)
{
    const char *name;

    if (!app || !entry || entry->file_ref != NTFS_ROOT_FRN)
        return 0;
    name = index_entry_name_utf8_locked(app, entry);
    return entry->parent_ref == NTFS_ROOT_FRN || entry->name_length == 0 ||
           (entry->name_length == 1 && name[0] == '.');
}

static int search_matches_volume_root_locked(const APP_STATE *app,
                                             const INDEX_ENTRY *entry,
                                             const SEARCH_QUERY *query)
{
    int volume;

    if (!query->scope_is_volume_root || !query->include_subfolders)
        return 0;
    volume = entry->volume_index;
    if (volume < 0 || volume >= app->volume_count || volume >= 26 ||
        towupper(app->volumes[volume].drive_letter[0]) !=
            towupper(query->scope_drive_letter)) {
        return 0;
    }
    return !search_entry_is_volume_root_locked(app, entry);
}

static int search_matches_folder(const wchar_t *path, const SEARCH_QUERY *query)
{
    const wchar_t *last;
    size_t scope_len;
    size_t path_len;

    if (!query->folder_scope[0])
        return 1;
    if (!path)
        path = L"";
    scope_len = wcslen(query->folder_scope);
    path_len = wcslen(path);

    if (query->include_subfolders) {
        if (path_len <= scope_len ||
            _wcsnicmp(path, query->folder_scope, scope_len) != 0)
            return 0;
        if (query->folder_scope[scope_len - 1] == L'\\')
            return 1;
        return path[scope_len] == L'\\';
    }

    last = wcsrchr(path, L'\\');
    if (!last)
        return 0;
    if (scope_len == 3 && query->folder_scope[1] == L':' &&
        query->folder_scope[2] == L'\\') {
        return last == path + 2 && _wcsnicmp(path, query->folder_scope, 2) == 0;
    }
    return (size_t)(last - path) == scope_len &&
           _wcsnicmp(path, query->folder_scope, scope_len) == 0;
}

static int search_query_has_plain_name_fast_path(const SEARCH_QUERY *query)
{
    if (!query || !query->text[0])
        return 0;
    if (query->match_path || query->match_case || query->match_whole_word ||
        query->use_regex)
        return 0;
    if (wcschr(query->text, L'*') || wcschr(query->text, L'?'))
        return 0;
    if (StrStrIW(query->text, L"ext:") || StrStrIW(query->text, L"folder:"))
        return 0;
    return query->char_mask != 0;
}

static int search_best_name_char_slot(APP_STATE *app, const SEARCH_QUERY *query)
{
    int best_slot = -1;
    int best_count = INT_MAX;
    
    if (!app || !query || !app->name_char_index_ready)
        return -1;
    
    for (int slot = 0; slot < SEARCH_CHAR_SLOT_COUNT; slot++) {
        if (query->char_mask & (1ULL << slot)) {
            int count = app->name_char_counts[slot];
            if (count <= 0)
                return slot;
            if (count < best_count) {
                best_count = count;
                best_slot = slot;
            }
        }
    }
    
    return best_slot;
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
    const wchar_t *ext;
    size_t scope_len;
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
        int slot = index_char_mask_slot(query->text[i]);
        if (slot >= 0)
            query->char_mask |= 1ULL << slot;
    }

    /* Hoisted out of search_match_entry, which ran these scans once per
       indexed file. Case-sensitive wcsstr preserves the previous matching
       behaviour for the "ext:"/"folder:" prefixes. */
    ext = wcsstr(query->text, L"ext:");
    query->ext_filter_offset = ext ? (int)(ext - query->text) + 4 : -1;
    query->has_folder_filter = wcsstr(query->text, L"folder:") != NULL;
    query->has_wildcard = wcschr(query->text, L'*') != NULL ||
                          wcschr(query->text, L'?') != NULL;
    scope_len = wcslen(query->folder_scope);
    query->scope_is_volume_root = scope_len == 3 &&
                                  query->folder_scope[1] == L':' &&
                                  (query->folder_scope[2] == L'\\' ||
                                   query->folder_scope[2] == L'/');
    query->scope_drive_letter = scope_len >= 2 &&
                                query->folder_scope[1] == L':'
        ? towupper(query->folder_scope[0]) : L'\0';
}

static int compare_entries_for_query(APP_STATE *app, int ia, int ib, int column)
{
    INDEX_ENTRY *a = &app->entries[ia];
    INDEX_ENTRY *b = &app->entries[ib];
    int result = 0;
    
    switch (column) {
        case COL_PATH:
        {
            wchar_t *apath = index_duplicate_entry_path_locked(app, ia);
            wchar_t *bpath = index_duplicate_entry_path_locked(app, ib);
            result = _wcsicmp(apath ? apath : L"", bpath ? bpath : L"");
            free(apath);
            free(bpath);
            break;
        }
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
            result = index_compare_entry_names_locked(app, a, b);
            break;
    }
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
    int result;

    /* Keep directories together at the top for every sort direction. */
    if (a->is_directory != b->is_directory)
        return a->is_directory ? -1 : 1;

    result = compare_entries_for_query(sort->app, ia, ib, sort->query->sort_column);
    
    return sort->query->sort_ascending ? result : -result;
}

static int search_sort_nearly_ordered(int *indices, int count, SORT_CONTEXT *ctx)
{
    int repairs = 0;

    for (int i = 1; i < count; i++) {
        int value;
        int lo;
        int hi;

        if (compare_filtered_indices_ctx(ctx, &indices[i - 1], &indices[i]) <= 0)
            continue;
        if (++repairs > 64)
            return 0;

        value = indices[i];
        lo = 0;
        hi = i;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (compare_filtered_indices_ctx(ctx, &indices[mid], &value) <= 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        memmove(indices + lo + 1, indices + lo,
                (size_t)(i - lo) * sizeof(*indices));
        indices[lo] = value;
    }
    return 1;
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

int search_match_entry(APP_STATE *app, int entry_index, const SEARCH_QUERY *query)
{
    INDEX_ENTRY *entry;
    wchar_t *path = NULL;
    wchar_t *owned_name = NULL;
    wchar_t name_buffer[512];
    const wchar_t *target;
    int matched = 0;
    int volume_root_scope;
    int scope_checked = 0;

    if (!app || entry_index < 0 || entry_index >= app->entry_count || !query)
        return 0;
    entry = &app->entries[entry_index];
    if (!search_matches_filter(entry, query->filter_id))
        return 0;
    volume_root_scope = query->scope_is_volume_root && query->include_subfolders;
    if (volume_root_scope &&
        !search_matches_volume_root_locked(app, entry, query)) {
        return 0;
    }
    if (query->folder_scope[0] && !volume_root_scope &&
        search_scope_cache_matches_locked(app, query)) {
        scope_checked = 1;
        if (app->scope_index_target < 0 ||
            !search_scope_entry_is_descendant_locked(
                app, entry_index, app->scope_index_target,
                query->include_subfolders))
            return 0;
    }
    if (query->match_path ||
        (query->folder_scope[0] && !volume_root_scope && !scope_checked)) {
        path = index_duplicate_entry_path_locked(app, entry_index);
        if (!path)
            return 0;
    }
    if (!volume_root_scope && !scope_checked &&
        !search_matches_folder(path, query)) {
        free(path);
        return 0;
    }
    if (query->match_path) {
        target = path;
    } else if (index_copy_entry_name_locked(app, entry, name_buffer,
                                            sizeof(name_buffer) /
                                                sizeof(name_buffer[0]))) {
        target = name_buffer;
    } else {
        owned_name = index_duplicate_entry_name_locked(app, entry);
        target = owned_name;
    }
    if (!query->text[0]) {
        matched = 1;
        goto done;
    }
    if (!target) goto done;
    
    /* Handle special type filters */
    if (query->ext_filter_offset >= 0) {
        const wchar_t *exts = query->text + query->ext_filter_offset;
        wchar_t entry_ext[512];
        wchar_t exts_copy[512];

        if (!index_copy_entry_extension_locked(
                app, entry, entry_ext,
                sizeof(entry_ext) / sizeof(entry_ext[0]))) {
            goto done;
        }

        wcsncpy_s(exts_copy, 512, exts, _TRUNCATE);

        wchar_t *ctx = NULL;
        wchar_t *ext = wcstok_s(exts_copy, L";", &ctx);
        while (ext) {
            if (_wcsicmp(entry_ext, ext) == 0) {
                matched = 1;
                goto done;
            }
            ext = wcstok_s(NULL, L";", &ctx);
        }
        goto done;
    }

    if (query->has_folder_filter) {
        matched = entry->is_directory ? 1 : 0;
        goto done;
    }
    
    /* Regular matching */
    if (query->match_whole_word) {
        matched = match_whole_word(target, query->text, query->match_case);
        goto done;
    }
    
    if (!query->has_wildcard) {
        matched = query->match_case
            ? (wcsstr(target, query->text) != NULL)
            : (query->folded_ready
                ? search_contains_folded(target, query->folded_text, query->text_len)
                : search_contains_ignore_case(target, query->text));
        goto done;
    }

    /* match_wildcard does not modify the pattern, so pass it straight through;
       the old code copied 1 KB onto the stack for every candidate entry. */
    matched = match_wildcard(target, query->text, query->match_case);

done:
    free(owned_name);
    free(path);
    return matched;
}

/* Drop indices that no longer address a live entry, preserving order.
   The caller must hold index_lock. A USN delete can shrink the index between
   the collect phase and the sort phase, and the sort comparator dereferences
   app->entries directly. */
static int search_clamp_indices(APP_STATE *app, int *indices, int count)
{
    int valid = 0;

    for (int n = 0; n < count; n++) {
        if (indices[n] >= 0 && indices[n] < app->entry_count)
            indices[valid++] = indices[n];
    }
    return valid;
}

void search_sort_results(APP_STATE *app)
{
    search_sort_indices(app, &app->query, app->filtered_indices, app->filtered_count);
}

typedef struct {
    int index;
    wchar_t *path;
} PATH_SORT_ITEM;

static int compare_path_items(void *ctx, const void *lhs, const void *rhs)
{
    SORT_CONTEXT *sort = (SORT_CONTEXT *)ctx;
    const PATH_SORT_ITEM *a = (const PATH_SORT_ITEM *)lhs;
    const PATH_SORT_ITEM *b = (const PATH_SORT_ITEM *)rhs;
    const INDEX_ENTRY *ea = &sort->app->entries[a->index];
    const INDEX_ENTRY *eb = &sort->app->entries[b->index];
    int result;

    if (ea->is_directory != eb->is_directory)
        return ea->is_directory ? -1 : 1;

    result = _wcsicmp(a->path ? a->path : L"", b->path ? b->path : L"");
    if (result == 0)
        result = compare_int64(ea->file_ref, eb->file_ref);

    return sort->query->sort_ascending ? result : -result;
}

/* Sorting by path needs every entry's full path. Building them once is O(n)
   rebuilds; the generic comparator rebuilt two paths (with two heap
   allocations) on every one of the O(n log n) comparisons. Returns 0 if the
   temporary cannot be allocated, leaving the caller to use the slow path. */
static int search_sort_by_path(APP_STATE *app, const SEARCH_QUERY *query,
                               int *indices, int count)
{
    PATH_SORT_ITEM *items;
    SORT_CONTEXT ctx;

    items = (PATH_SORT_ITEM *)malloc((size_t)count * sizeof(*items));
    if (!items)
        return 0;

    for (int i = 0; i < count; i++) {
        items[i].index = indices[i];
        items[i].path = index_duplicate_entry_path_locked(app, indices[i]);
    }

    ctx.app = app;
    ctx.query = query;
    qsort_s(items, (size_t)count, sizeof(*items), compare_path_items, &ctx);

    for (int i = 0; i < count; i++) {
        indices[i] = items[i].index;
        free(items[i].path);
    }
    free(items);
    return 1;
}

void search_sort_indices(APP_STATE *app, const SEARCH_QUERY *query, int *indices, int count)
{
    SORT_CONTEXT ctx;

    if (count <= 1)
        return;

    ctx.app = app;
    ctx.query = query;
    if (query->sort_column == COL_NAME && query->sort_ascending &&
        search_sort_nearly_ordered(indices, count, &ctx))
        return;
    if (query->sort_column == COL_PATH &&
        search_sort_by_path(app, query, indices, count))
        return;
    qsort_s(indices, count, sizeof(int), compare_filtered_indices_ctx, &ctx);
}

void search_execute(APP_STATE *app)
{
    search_prepare_query(&app->query);
    
    EnterCriticalSection(&app->index_lock);
    if (app->query.folder_scope[0] && !app->query.scope_is_volume_root)
        search_build_scope_index_locked(app, &app->query);
    
    app->filtered_count = 0;
    
    for (int i = 0; i < app->entry_count && app->filtered_count < SEARCH_MAX_RESULTS; i++) {
        if (search_match_entry(app, i, &app->query)) {
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
    int volume_phase = 0;
    int fast_slot = -1;
    int fast_filter = -1;
    int fast_scope = 0;
    int fast_volume = -1;
    
    if (!out_indices || max_results <= 0)
        return 0;

    if (!query->text[0] && !query->folder_scope[0] &&
        query->filter_id >= FILTER_EVERYTHING && query->filter_id < FILTER_COUNT) {
        int ready;
        int indexed_count;
        int directory_count;

        EnterCriticalSection(&app->index_lock);
        ready = app->filter_index_ready;
        indexed_count = ready ? app->filter_counts[query->filter_id] : 0;
        directory_count = ready ? app->filter_counts[FILTER_FOLDER] : 0;
        if (ready && query->filter_id == FILTER_EVERYTHING) {
            int folder_copy = directory_count < max_results
                ? directory_count : max_results;
            if (folder_copy > 0)
                memcpy(out_indices, app->filter_indices[FILTER_FOLDER],
                       (size_t)folder_copy * sizeof(*out_indices));
            count = folder_copy;
            for (int n = 0; n < app->entry_count && count < max_results; n++) {
                if (!app->entries[n].is_directory)
                    out_indices[count++] = n;
            }
        } else {
            count = indexed_count < max_results ? indexed_count : max_results;
            if (ready && count > 0)
                memcpy(out_indices, app->filter_indices[query->filter_id],
                       (size_t)count * sizeof(*out_indices));
        }
        LeaveCriticalSection(&app->index_lock);

        if (ready) {
            if (query->sort_column == COL_NAME) {
                if (!query->sort_ascending) {
                    int boundary = query->filter_id == FILTER_EVERYTHING
                        ? (directory_count < count ? directory_count : count) : count;
                    for (int left = 0, right = boundary - 1;
                         left < right; left++, right--) {
                        int swap = out_indices[left];
                        out_indices[left] = out_indices[right];
                        out_indices[right] = swap;
                    }
                    for (int left = boundary, right = count - 1;
                         left < right; left++, right--) {
                        int swap = out_indices[left];
                        out_indices[left] = out_indices[right];
                        out_indices[right] = swap;
                    }
                }
                EnterCriticalSection(&app->index_lock);
                count = search_clamp_indices(app, out_indices, count);
                LeaveCriticalSection(&app->index_lock);
            } else if (generation == app->search_generation) {
                EnterCriticalSection(&app->index_lock);
                count = search_clamp_indices(app, out_indices, count);
                search_sort_indices(app, query, out_indices, count);
                LeaveCriticalSection(&app->index_lock);
            }
            return count;
        }
    }
    
    if (!query->text[0] && query->filter_id == FILTER_EVERYTHING &&
        !query->folder_scope[0]) {
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
        EnterCriticalSection(&app->index_lock);
        /* out_indices is the identity map built outside the lock; if the index
           shrank meanwhile, clamping the count is enough to keep every
           remaining index valid. */
        if (count > app->entry_count)
            count = app->entry_count;
        search_sort_indices(app, query, out_indices, count);
        LeaveCriticalSection(&app->index_lock);
        return count;
    }

    EnterCriticalSection(&app->index_lock);
    if (query->folder_scope[0] && !query->scope_is_volume_root)
        search_build_scope_index_locked(app, query);
    if (!base_indices && !base_identity) {
        int best_count = INT_MAX;

        if (search_query_has_plain_name_fast_path(query)) {
            fast_slot = search_best_name_char_slot(app, query);
            if (fast_slot >= 0)
                best_count = app->name_char_counts[fast_slot];
        }
        if (query->filter_id > FILTER_EVERYTHING &&
            query->filter_id < FILTER_COUNT && app->filter_index_ready) {
            int filter_count = app->filter_counts[query->filter_id];
            if (filter_count <= best_count) {
                fast_filter = query->filter_id;
                fast_slot = -1;
                best_count = filter_count;
            }
        }
        if (query->scope_is_volume_root && query->include_subfolders &&
            app->volume_index_ready) {
            int volume = search_find_scope_volume_locked(app, query);
            if (volume >= 0 && app->volume_counts[volume] <= best_count) {
                fast_volume = volume;
                fast_filter = -1;
                fast_slot = -1;
            }
        }
        if (!query->scope_is_volume_root &&
            search_scope_cache_matches_locked(app, query) &&
            app->scope_index_count <= best_count) {
            fast_scope = 1;
            fast_volume = -1;
            fast_filter = -1;
            fast_slot = -1;
        }
    }
    LeaveCriticalSection(&app->index_lock);
    
    if (fast_scope) {
        for (;;) {
            int end;
            int scope_count;
            int *scope_indices;

            EnterCriticalSection(&app->index_lock);
            scope_count = app->scope_index_count;
            scope_indices = app->scope_index_pool;
            if (!app->scope_index_ready || !scope_indices ||
                i >= scope_count || count >= max_results) {
                LeaveCriticalSection(&app->index_lock);
                break;
            }
            end = i + SEARCH_LOCK_CHUNK;
            if (end > scope_count)
                end = scope_count;
            for (; i < end && count < max_results; i++) {
                int idx = scope_indices[i];
                if (idx >= 0 && idx < app->entry_count &&
                    search_match_entry(app, idx, query))
                    out_indices[count++] = idx;
            }
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
        }
    } else if (fast_volume >= 0) {
        for (;;) {
            int end;
            int entry_count;
            int volume_entry_count;
            int *volume_indices;
            int done = 0;

            EnterCriticalSection(&app->index_lock);
            entry_count = app->entry_count;
            volume_entry_count = app->volume_counts[fast_volume];
            volume_indices = app->volume_indices[fast_volume];
            if (!app->volume_index_ready || volume_entry_count <= 0 ||
                count >= max_results) {
                done = 1;
            } else if (volume_indices) {
                if (i >= volume_entry_count) {
                    done = 1;
                } else {
                    end = i + SEARCH_LOCK_CHUNK;
                    if (end > volume_entry_count)
                        end = volume_entry_count;
                    for (; i < end && count < max_results; i++) {
                        int idx = volume_indices[i];
                        if (idx >= 0 && idx < entry_count &&
                            search_match_entry(app, idx, query))
                            out_indices[count++] = idx;
                    }
                }
            } else if (volume_phase >= 2) {
                done = 1;
            } else {
                end = i + SEARCH_LOCK_CHUNK;
                if (end > entry_count)
                    end = entry_count;
                for (; i < end && count < max_results; i++) {
                    INDEX_ENTRY *entry = &app->entries[i];
                    if ((volume_phase == 0) != (entry->is_directory != 0) ||
                        entry->volume_index != fast_volume)
                        continue;
                    if (search_match_entry(app, i, query))
                        out_indices[count++] = i;
                }
                if (i >= entry_count) {
                    volume_phase++;
                    i = 0;
                }
            }

            LeaveCriticalSection(&app->index_lock);
            if (done || generation != app->search_generation)
                break;
        }
    } else if (fast_filter >= 0) {
        for (;;) {
            int end;
            int entry_count;
            int filter_count;
            int *filter_indices;

            EnterCriticalSection(&app->index_lock);
            entry_count = app->entry_count;
            filter_count = app->filter_counts[fast_filter];
            filter_indices = app->filter_indices[fast_filter];
            if (!app->filter_index_ready ||
                i >= filter_count || count >= max_results) {
                LeaveCriticalSection(&app->index_lock);
                break;
            }

            end = i + SEARCH_LOCK_CHUNK;
            if (end > filter_count)
                end = filter_count;
            for (; i < end && count < max_results; i++) {
                int idx = filter_indices ? filter_indices[i] : -1;
                if (idx >= 0 && idx < entry_count &&
                    search_match_entry(app, idx, query))
                    out_indices[count++] = idx;
            }

            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
        }
    } else if (fast_slot >= 0) {
        for (;;) {
            int end;
            int entry_count;
            int slot_count;
            int *slot_indices;
            
            EnterCriticalSection(&app->index_lock);
            
            entry_count = app->entry_count;
            slot_count = app->name_char_counts[fast_slot];
            slot_indices = app->name_char_indices[fast_slot];
            if (!app->name_char_index_ready ||
                i >= slot_count || count >= max_results) {
                LeaveCriticalSection(&app->index_lock);
                break;
            }
            
            end = i + SEARCH_LOCK_CHUNK;
            if (end > slot_count)
                end = slot_count;
            
            for (; i < end && count < max_results; i++) {
                int idx = slot_indices ? slot_indices[i] : -1;
                if (idx >= 0 && idx < entry_count &&
                    search_match_entry(app, idx, query))
                    out_indices[count++] = idx;
            }
            
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
        }
    } else if ((base_indices || base_identity) && base_count > 0) {
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
                    search_match_entry(app, idx, query))
                    out_indices[count++] = idx;
            }
            
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
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
                if (search_match_entry(app, i, query))
                    out_indices[count++] = i;
            }
            
            LeaveCriticalSection(&app->index_lock);
            if (generation != app->search_generation)
                break;
        }
    }
    
    if (generation != app->search_generation)
        return count;
    
    EnterCriticalSection(&app->index_lock);
    count = search_clamp_indices(app, out_indices, count);
    search_sort_indices(app, query, out_indices, count);
    LeaveCriticalSection(&app->index_lock);
    return count;
}

int search_execute_to_buffer(APP_STATE *app, const SEARCH_QUERY *query, int *out_indices, int max_results)
{
    return search_execute_subset_to_buffer(app, query, out_indices, max_results,
                                           NULL, 0, 0, app->search_generation);
}
