#include "index.h"
#include "common.h"

typedef struct {
    long long file_ref;
    int volume_index;
    int index;
} REF_LOOKUP;

static int ref_lookup_compare(const void *a, const void *b)
{
    const REF_LOOKUP *ra = (const REF_LOOKUP *)a;
    const REF_LOOKUP *rb = (const REF_LOOKUP *)b;
    
    if (ra->volume_index < rb->volume_index) return -1;
    if (ra->volume_index > rb->volume_index) return 1;
    if (ra->file_ref < rb->file_ref) return -1;
    if (ra->file_ref > rb->file_ref) return 1;
    return 0;
}

static int ref_lookup_find(REF_LOOKUP *lookup, int count, int volume_index, long long file_ref)
{
    int lo = 0;
    int hi = count - 1;
    
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        REF_LOOKUP *cur = &lookup[mid];
        
        if (cur->volume_index == volume_index && cur->file_ref == file_ref)
            return cur->index;
        
        if (cur->volume_index < volume_index ||
            (cur->volume_index == volume_index && cur->file_ref < file_ref)) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    
    return -1;
}

static int index_mask_slot(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z')
        ch += L'a' - L'A';
    
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

static wchar_t index_lower_char(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z')
        return ch + (L'a' - L'A');
    if (ch < 128)
        return ch;
    return (wchar_t)towlower(ch);
}

static wchar_t *index_dup_folded(const wchar_t *text)
{
    size_t len;
    wchar_t *folded;
    
    if (!text)
        text = L"";
    
    len = wcslen(text);
    folded = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!folded)
        return NULL;
    
    for (size_t i = 0; i < len; i++)
        folded[i] = index_lower_char(text[i]);
    folded[len] = L'\0';
    return folded;
}

static unsigned long long index_compute_char_mask(const wchar_t *text)
{
    unsigned long long mask = 0;
    
    if (!text)
        return 0;
    
    for (const wchar_t *p = text; *p; p++) {
        int slot = index_mask_slot(*p);
        if (slot >= 0)
            mask |= 1ULL << slot;
    }
    
    return mask;
}

void index_prepare_entry(INDEX_ENTRY *entry)
{
    if (!entry)
        return;
    
    entry->name_char_mask = index_compute_char_mask(entry->name);
    entry->path_char_mask = index_compute_char_mask(entry->path);
    
    if (!entry->folded_name)
        entry->folded_name = index_dup_folded(entry->name);
}

static int index_find_by_ref_locked(APP_STATE *app, int volume_index, long long file_ref)
{
    for (int i = 0; i < app->entry_count; i++) {
        if (app->entries[i].volume_index == volume_index &&
            app->entries[i].file_ref == file_ref)
            return i;
    }
    
    return -1;
}

static int index_ensure_capacity_locked(APP_STATE *app, int needed)
{
    if (needed <= app->entry_capacity)
        return 1;
    
    int new_cap = app->entry_capacity > 0 ? app->entry_capacity : 65536;
    while (new_cap < needed)
        new_cap *= 2;
    
    INDEX_ENTRY *new_entries = (INDEX_ENTRY *)calloc(new_cap, sizeof(INDEX_ENTRY));
    int *new_filtered = (int *)calloc(new_cap, sizeof(int));
    if (!new_entries || !new_filtered) {
        free(new_entries);
        free(new_filtered);
        return 0;
    }
    
    memcpy(new_entries, app->entries, app->entry_count * sizeof(INDEX_ENTRY));
    memcpy(new_filtered, app->filtered_indices, app->filtered_count * sizeof(int));
    free(app->entries);
    free(app->filtered_indices);
    app->entries = new_entries;
    app->filtered_indices = new_filtered;
    app->entry_capacity = new_cap;
    return 1;
}

static int index_set_entry_from_change(INDEX_ENTRY *entry, const USN_CHANGE *change)
{
    memset(entry, 0, sizeof(*entry));
    
    entry->name = _wcsdup(change->name ? change->name : L"");
    entry->path = _wcsdup(L"");
    
    if (!entry->name || !entry->path)
        return 0;
    
    const wchar_t *dot = wcsrchr(entry->name, L'.');
    if (dot && dot[1])
        entry->extension = _wcsdup(dot + 1);
    else
        entry->extension = _wcsdup(L"");
    
    if (!entry->extension)
        return 0;
    
    entry->size = 0;
    entry->creation_time = 0;
    entry->modification_time = change->timestamp;
    entry->access_time = 0;
    entry->attributes = change->attributes;
    entry->file_ref = change->file_ref;
    entry->parent_ref = change->parent_ref;
    entry->is_directory = change->is_directory;
    entry->volume_index = change->volume_index;
    entry->usn = change->usn;
    index_prepare_entry(entry);
    return 1;
}

void index_init(APP_STATE *app)
{
    memset(app, 0, sizeof(*app));
    InitializeCriticalSection(&app->index_lock);
    app->entry_capacity = 65536;
    app->entries = (INDEX_ENTRY *)calloc(app->entry_capacity, sizeof(INDEX_ENTRY));
    app->filtered_indices = (int *)calloc(app->entry_capacity, sizeof(int));
    app->query.sort_column = COL_NAME;
    app->query.sort_ascending = 1;
}

void index_free_entry(INDEX_ENTRY *entry)
{
    if (!entry) return;
    if (!(entry->string_flags & ENTRY_STRING_NAME_POOLED))
        free(entry->name);
    if (!(entry->string_flags & ENTRY_STRING_PATH_POOLED))
        free(entry->path);
    if (!(entry->string_flags & ENTRY_STRING_EXTENSION_POOLED))
        free(entry->extension);
    if (!(entry->string_flags & ENTRY_STRING_FOLDED_NAME_POOLED))
        free(entry->folded_name);
    entry->name = NULL;
    entry->path = NULL;
    entry->extension = NULL;
    entry->folded_name = NULL;
    entry->string_flags = 0;
}

void index_clear(APP_STATE *app)
{
    EnterCriticalSection(&app->index_lock);
    
    for (int i = 0; i < app->entry_count; i++) {
        index_free_entry(&app->entries[i]);
    }
    app->entry_count = 0;
    app->filtered_count = 0;
    app->filtered_identity = 0;
    free(app->entry_string_pool);
    app->entry_string_pool = NULL;
    
    LeaveCriticalSection(&app->index_lock);
}

int index_add_entry(APP_STATE *app, INDEX_ENTRY *entry)
{
    EnterCriticalSection(&app->index_lock);
    
    if (app->entry_count >= app->entry_capacity) {
        if (!index_ensure_capacity_locked(app, app->entry_count + 1)) {
            LeaveCriticalSection(&app->index_lock);
            return 0;
        }
    }
    
    int idx = app->entry_count;
    index_prepare_entry(entry);
    app->entries[idx] = *entry;
    app->entry_count++;
    
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

int index_add_entries(APP_STATE *app, INDEX_ENTRY *entries, int count)
{
    if (!entries || count <= 0)
        return 1;
    
    EnterCriticalSection(&app->index_lock);
    
    if (app->entry_count + count > app->entry_capacity) {
        if (!index_ensure_capacity_locked(app, app->entry_count + count)) {
            LeaveCriticalSection(&app->index_lock);
            return 0;
        }
    }
    
    for (int i = 0; i < count; i++)
        index_prepare_entry(&entries[i]);
    
    memcpy(app->entries + app->entry_count, entries, count * sizeof(INDEX_ENTRY));
    app->entry_count += count;
    
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

int index_apply_usn_changes(APP_STATE *app, USN_CHANGE *changes, int count)
{
    int applied = 0;
    
    if (!changes || count <= 0)
        return 0;
    
    EnterCriticalSection(&app->index_lock);
    
    for (int i = 0; i < count; i++) {
        USN_CHANGE *change = &changes[i];
        int idx = index_find_by_ref_locked(app, change->volume_index, change->file_ref);
        
        if (change->reason & USN_REASON_FILE_DELETE) {
            if (idx >= 0) {
                index_free_entry(&app->entries[idx]);
                if (idx != app->entry_count - 1) {
                    app->entries[idx] = app->entries[app->entry_count - 1];
                }
                memset(&app->entries[app->entry_count - 1], 0, sizeof(INDEX_ENTRY));
                app->entry_count--;
                applied++;
            }
            continue;
        }
        
        if (change->reason & USN_REASON_RENAME_OLD_NAME)
            continue;
        
        if (!(change->reason & (USN_REASON_FILE_CREATE |
                                USN_REASON_RENAME_NEW_NAME |
                                USN_REASON_BASIC_INFO_CHANGE |
                                USN_REASON_SECURITY_CHANGE |
                                USN_REASON_INDEXABLE_CHANGE)))
            continue;
        
        if (!change->name || !change->name[0])
            continue;
        
        if (idx >= 0) {
            INDEX_ENTRY replacement;
            memset(&replacement, 0, sizeof(replacement));
            if (index_set_entry_from_change(&replacement, change)) {
                index_free_entry(&app->entries[idx]);
                app->entries[idx] = replacement;
                applied++;
            } else {
                index_free_entry(&replacement);
            }
        } else if (index_ensure_capacity_locked(app, app->entry_count + 1)) {
            INDEX_ENTRY *entry = &app->entries[app->entry_count];
            if (index_set_entry_from_change(entry, change)) {
                app->entry_count++;
                applied++;
            } else {
                index_free_entry(entry);
                memset(entry, 0, sizeof(*entry));
            }
        }
    }
    
    if (applied)
        app->filtered_count = 0;
    if (applied)
        app->filtered_identity = 0;
    
    LeaveCriticalSection(&app->index_lock);
    return applied;
}

void index_build_paths(APP_STATE *app)
{
    EnterCriticalSection(&app->index_lock);
    
    int i, j;
    REF_LOOKUP *lookup = (REF_LOOKUP *)malloc(app->entry_count * sizeof(REF_LOOKUP));
    if (!lookup) {
        LeaveCriticalSection(&app->index_lock);
        return;
    }
    
    for (i = 0; i < app->entry_count; i++) {
        lookup[i].file_ref = app->entries[i].file_ref;
        lookup[i].volume_index = app->entries[i].volume_index;
        lookup[i].index = i;
    }
    qsort(lookup, app->entry_count, sizeof(REF_LOOKUP), ref_lookup_compare);
    
    for (i = 0; i < app->entry_count; i++) {
        INDEX_ENTRY *e = &app->entries[i];
        if (e->path && !(e->string_flags & ENTRY_STRING_PATH_POOLED))
            free(e->path);
        e->path = NULL;
        e->string_flags &= ~ENTRY_STRING_PATH_POOLED;
        
        /* Follow parent chain to build path */
        wchar_t path_parts[64][256];
        int depth = 0;
        long long current_frn = e->parent_ref;
        int is_root = 0;
        
        /* Push this entry's name */
        wcscpy_s(path_parts[depth], 256, e->name ? e->name : L"");
        depth++;
        
        /* Walk up parent chain */
        while (depth < 64 && !is_root) {
            if (current_frn == 5 || current_frn == 0)
                break;
            
            int parent_index = ref_lookup_find(lookup, app->entry_count, e->volume_index, current_frn);
            /* Look for the parent FRN in our entries */
            if (parent_index >= 0) {
                INDEX_ENTRY *parent = &app->entries[parent_index];
                wcscpy_s(path_parts[depth], 256, parent->name ? parent->name : L"");
                depth++;
                current_frn = parent->parent_ref;
                
                /* Check if root (FRN 5 is root directory) */
                if (current_frn == 5 || current_frn == 0 || parent->file_ref == current_frn) {
                    is_root = 1;
                }
            } else {
                break;
            }
        }
        
        /* Build path string: drive:\parent\...\name */
        wchar_t buf[MAX_PATH * 2];
        swprintf_s(buf, MAX_PATH * 2, L"%s", app->volumes[e->volume_index].drive_letter);
        
        /* Add parts in reverse (root to leaf) */
        for (j = depth - 1; j >= 0; j--) {
            if (j < depth - 1) wcscat_s(buf, MAX_PATH * 2, L"\\");
            wcscat_s(buf, MAX_PATH * 2, path_parts[j]);
        }
        
        e->path = _wcsdup(buf);
        e->path_char_mask = index_compute_char_mask(e->path);
    }
    
    free(lookup);
    LeaveCriticalSection(&app->index_lock);
}

/* Sort comparators */
int index_sort_by_name(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    int result = _wcsicmp(ea->name ? ea->name : L"", eb->name ? eb->name : L"");
    if (result == 0)
        result = _wcsicmp(ea->path ? ea->path : L"", eb->path ? eb->path : L"");
    if (result == 0) {
        if (ea->file_ref < eb->file_ref) return -1;
        if (ea->file_ref > eb->file_ref) return 1;
    }
    return result;
}

int index_sort_by_path(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    return _wcsicmp(ea->path ? ea->path : L"", eb->path ? eb->path : L"");
}

int index_sort_by_size(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    if (ea->size < eb->size) return -1;
    if (ea->size > eb->size) return 1;
    return 0;
}

int index_sort_by_date_modified(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    if (ea->modification_time < eb->modification_time) return -1;
    if (ea->modification_time > eb->modification_time) return 1;
    return 0;
}

int index_sort_by_date_created(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    if (ea->creation_time < eb->creation_time) return -1;
    if (ea->creation_time > eb->creation_time) return 1;
    return 0;
}

int index_sort_by_attributes(const void *a, const void *b)
{
    INDEX_ENTRY *ea = (INDEX_ENTRY *)a;
    INDEX_ENTRY *eb = (INDEX_ENTRY *)b;
    if (ea->attributes < eb->attributes) return -1;
    if (ea->attributes > eb->attributes) return 1;
    return 0;
}

void index_sort_entries_by_name(APP_STATE *app)
{
    EnterCriticalSection(&app->index_lock);
    if (app->entry_count > 1)
        qsort(app->entries, app->entry_count, sizeof(INDEX_ENTRY), index_sort_by_name);
    LeaveCriticalSection(&app->index_lock);
}
