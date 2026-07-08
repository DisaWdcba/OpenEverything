#include "cache.h"
#include "config.h"
#include "index.h"

#define CACHE_MAGIC "ECIDX3"
#define CACHE_VERSION 3

typedef struct {
    char magic[8];
    unsigned int version;
    int volume_count;
    int entry_count;
} CACHE_HEADER;

typedef struct {
    long long size;
    long long creation_time;
    long long modification_time;
    long long access_time;
    unsigned int attributes;
    long long file_ref;
    long long parent_ref;
    long long usn;
    int is_directory;
    int volume_index;
    unsigned int name_len;
    unsigned int path_len;
    unsigned int extension_len;
} CACHE_ENTRY;

void cache_get_index_path(wchar_t *buf, size_t size)
{
    config_get_path(buf, size);
    
    wchar_t *last = wcsrchr(buf, L'\\');
    if (last)
        wcscpy_s(last + 1, size - (last + 1 - buf), L"index.dat");
    else
        wcscpy_s(buf, size, L"index.dat");
}

static int cache_ensure_dir(const wchar_t *path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *last;
    
    wcscpy_s(dir, MAX_PATH, path);
    last = wcsrchr(dir, L'\\');
    if (!last)
        return 1;
    
    *last = L'\0';
    int rc = SHCreateDirectoryExW(NULL, dir, NULL);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS;
}

static int cache_write_wstring(FILE *f, const wchar_t *text, unsigned int len)
{
    wchar_t nul = L'\0';
    
    if (len > 0 && fwrite(text, sizeof(wchar_t), len, f) != len)
        return 0;
    
    return fwrite(&nul, sizeof(wchar_t), 1, f) == 1;
}

static wchar_t *cache_read_wstring(FILE *f, unsigned int len)
{
    wchar_t *text;
    
    if (len > 32767)
        return NULL;
    
    text = (wchar_t *)calloc(len + 1, sizeof(wchar_t));
    if (!text)
        return NULL;
    
    if (len > 0 && fread(text, sizeof(wchar_t), len, f) != len) {
        free(text);
        return NULL;
    }
    
    /* Consume the stored terminator, but force our own as a guard. */
    wchar_t ignored;
    if (fread(&ignored, sizeof(wchar_t), 1, f) != 1) {
        free(text);
        return NULL;
    }
    
    text[len] = L'\0';
    return text;
}

static void cache_free_entry_array(INDEX_ENTRY *entries, int count)
{
    if (!entries)
        return;
    
    for (int i = 0; i < count; i++)
        index_free_entry(&entries[i]);
    free(entries);
}

static int cache_take_bytes(const unsigned char **cursor, const unsigned char *end,
                            void *dst, size_t size)
{
    if (!cursor || !*cursor || *cursor > end || (size_t)(end - *cursor) < size)
        return 0;
    
    memcpy(dst, *cursor, size);
    *cursor += size;
    return 1;
}

static wchar_t *cache_dup_wstring_from_buffer(const unsigned char **cursor,
                                              const unsigned char *end,
                                              unsigned int len)
{
    wchar_t *text;
    size_t bytes;
    
    if (len > 32767)
        return NULL;
    
    bytes = ((size_t)len + 1) * sizeof(wchar_t);
    if (!cursor || !*cursor || *cursor > end || (size_t)(end - *cursor) < bytes)
        return NULL;
    
    text = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!text)
        return NULL;
    
    if (len > 0)
        memcpy(text, *cursor, (size_t)len * sizeof(wchar_t));
    text[len] = L'\0';
    *cursor += bytes;
    return text;
}

static int cache_skip_wstring(const unsigned char **cursor, const unsigned char *end,
                              unsigned int len)
{
    size_t bytes;
    
    if (len > 32767)
        return 0;
    
    bytes = ((size_t)len + 1) * sizeof(wchar_t);
    if (!cursor || !*cursor || *cursor > end || (size_t)(end - *cursor) < bytes)
        return 0;
    
    *cursor += bytes;
    return 1;
}

static wchar_t *cache_copy_wstring_to_pool(const unsigned char **cursor,
                                           const unsigned char *end,
                                           unsigned int len,
                                           wchar_t **pool_cursor)
{
    wchar_t *text;
    size_t bytes;
    
    if (len > 32767 || !pool_cursor || !*pool_cursor)
        return NULL;
    
    bytes = ((size_t)len + 1) * sizeof(wchar_t);
    if (!cursor || !*cursor || *cursor > end || (size_t)(end - *cursor) < bytes)
        return NULL;
    
    text = *pool_cursor;
    if (len > 0)
        memcpy(text, *cursor, (size_t)len * sizeof(wchar_t));
    text[len] = L'\0';
    
    *pool_cursor += (size_t)len + 1;
    *cursor += bytes;
    return text;
}

static wchar_t cache_lower_char(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z')
        return ch + (L'a' - L'A');
    if (ch < 128)
        return ch;
    return (wchar_t)towlower(ch);
}

static wchar_t *cache_copy_folded_to_pool(const wchar_t *text, unsigned int len,
                                          wchar_t **pool_cursor)
{
    wchar_t *folded;
    
    if (!pool_cursor || !*pool_cursor)
        return NULL;
    
    folded = *pool_cursor;
    for (unsigned int i = 0; i < len; i++)
        folded[i] = cache_lower_char(text ? text[i] : L'\0');
    folded[len] = L'\0';
    
    *pool_cursor += (size_t)len + 1;
    return folded;
}

int cache_save_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    FILE *f = NULL;
    CACHE_HEADER header;
    VOLUME_INFO volumes[26];
    int ok = 1;
    
    cache_get_index_path(path, MAX_PATH);
    cache_ensure_dir(path);
    
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return 0;
    
    EnterCriticalSection(&app->index_lock);
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, CACHE_MAGIC, sizeof(CACHE_MAGIC));
    header.version = CACHE_VERSION;
    header.volume_count = app->volume_count;
    header.entry_count = app->entry_count;
    if (header.volume_count >= 0 && header.volume_count <= 26)
        memcpy(volumes, app->volumes, sizeof(VOLUME_INFO) * header.volume_count);
    else
        ok = 0;
    LeaveCriticalSection(&app->index_lock);
    
    if (ok && fwrite(&header, sizeof(header), 1, f) != 1)
        ok = 0;
    if (ok && fwrite(volumes, sizeof(VOLUME_INFO), header.volume_count, f) != (size_t)header.volume_count)
        ok = 0;
    
    for (int i = 0; ok && i < header.entry_count; ) {
        int end = i + 2048;
        if (end > header.entry_count)
            end = header.entry_count;
        
        EnterCriticalSection(&app->index_lock);
        if (app->entry_count != header.entry_count ||
            app->volume_count != header.volume_count) {
            ok = 0;
        }
        
        for (; ok && i < end; i++) {
            INDEX_ENTRY *e = &app->entries[i];
            CACHE_ENTRY ce;
            
            memset(&ce, 0, sizeof(ce));
            ce.size = e->size;
            ce.creation_time = e->creation_time;
            ce.modification_time = e->modification_time;
            ce.access_time = e->access_time;
            ce.attributes = e->attributes;
            ce.file_ref = e->file_ref;
            ce.parent_ref = e->parent_ref;
            ce.usn = e->usn;
            ce.is_directory = e->is_directory;
            ce.volume_index = e->volume_index;
            ce.name_len = e->name ? (unsigned int)wcslen(e->name) : 0;
            ce.path_len = e->path ? (unsigned int)wcslen(e->path) : 0;
            ce.extension_len = e->extension ? (unsigned int)wcslen(e->extension) : 0;
            
            if (fwrite(&ce, sizeof(ce), 1, f) != 1)
                ok = 0;
            if (ok) ok = cache_write_wstring(f, e->name ? e->name : L"", ce.name_len);
            if (ok) ok = cache_write_wstring(f, e->path ? e->path : L"", ce.path_len);
            if (ok) ok = cache_write_wstring(f, e->extension ? e->extension : L"", ce.extension_len);
        }
        
        LeaveCriticalSection(&app->index_lock);
        Sleep(0);
    }
    fclose(f);
    
    if (!ok)
        DeleteFileW(path);
    
    return ok;
}

int cache_load_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = NULL;
    unsigned char *view = NULL;
    const unsigned char *cursor;
    const unsigned char *entry_start;
    const unsigned char *end;
    LARGE_INTEGER file_size;
    CACHE_HEADER header;
    VOLUME_INFO volumes[26];
    INDEX_ENTRY *entries = NULL;
    INDEX_ENTRY *old_entries = NULL;
    wchar_t *string_pool = NULL;
    wchar_t *pool_cursor = NULL;
    void *old_string_pool = NULL;
    size_t string_chars = 0;
    int *filtered = NULL;
    int *old_filtered = NULL;
    int old_entry_count = 0;
    int ok = 1;
    
    memset(&header, 0, sizeof(header));
    cache_get_index_path(path, MAX_PATH);
    
    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return 0;
    
    if (!GetFileSizeEx(hFile, &file_size) ||
        file_size.QuadPart < (LONGLONG)sizeof(CACHE_HEADER)) {
        CloseHandle(hFile);
        return 0;
    }
    
    hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return 0;
    }
    
    view = (unsigned char *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return 0;
    }
    
    cursor = view;
    end = view + (size_t)file_size.QuadPart;
    
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0 ||
        header.version != CACHE_VERSION ||
        header.volume_count < 0 || header.volume_count > 26 ||
        header.entry_count < 0) {
        ok = 0;
        goto cleanup;
    }
    
    memset(volumes, 0, sizeof(volumes));
    if (header.volume_count > 0) {
        size_t volume_bytes = sizeof(VOLUME_INFO) * (size_t)header.volume_count;
        if (!cache_take_bytes(&cursor, end, volumes, volume_bytes)) {
            ok = 0;
            goto cleanup;
        }
    }
    
    entry_start = cursor;
    {
        const unsigned char *scan = entry_start;
        for (int i = 0; ok && i < header.entry_count; i++) {
            CACHE_ENTRY ce;
            
            if (!cache_take_bytes(&scan, end, &ce, sizeof(ce))) {
                ok = 0;
                break;
            }
            if (ce.volume_index < 0 || ce.volume_index >= header.volume_count ||
                ce.name_len > 32767 || ce.path_len > 32767 || ce.extension_len > 32767) {
                ok = 0;
                break;
            }
            if (!cache_skip_wstring(&scan, end, ce.name_len) ||
                !cache_skip_wstring(&scan, end, ce.path_len) ||
                !cache_skip_wstring(&scan, end, ce.extension_len)) {
                ok = 0;
                break;
            }
            
            string_chars += (size_t)ce.name_len + 1;
            string_chars += (size_t)ce.path_len + 1;
            string_chars += (size_t)ce.extension_len + 1;
            string_chars += (size_t)ce.name_len + 1;
            
            if ((i & 8191) == 0)
                Sleep(0);
        }
    }
    
    if (!ok)
        goto cleanup;
    
    entries = (INDEX_ENTRY *)calloc(header.entry_count > 0 ? header.entry_count : 1, sizeof(INDEX_ENTRY));
    if (!entries) {
        ok = 0;
        goto cleanup;
    }
    
    string_pool = (wchar_t *)malloc((string_chars > 0 ? string_chars : 1) * sizeof(wchar_t));
    if (!string_pool) {
        ok = 0;
        goto cleanup;
    }
    pool_cursor = string_pool;
    cursor = entry_start;
    
    for (int i = 0; ok && i < header.entry_count; i++) {
        CACHE_ENTRY ce;
        INDEX_ENTRY *e = &entries[i];
        
        if (!cache_take_bytes(&cursor, end, &ce, sizeof(ce))) {
            ok = 0;
            break;
        }
        if (ce.volume_index < 0 || ce.volume_index >= header.volume_count) {
            ok = 0;
            break;
        }
        
        e->size = ce.size;
        e->creation_time = ce.creation_time;
        e->modification_time = ce.modification_time;
        e->access_time = ce.access_time;
        e->attributes = ce.attributes;
        e->file_ref = ce.file_ref;
        e->parent_ref = ce.parent_ref;
        e->usn = ce.usn;
        e->is_directory = ce.is_directory;
        e->volume_index = ce.volume_index;
        e->metadata_loaded = (ce.size != 0 || ce.creation_time != 0 || ce.access_time != 0);
        
        e->name = cache_copy_wstring_to_pool(&cursor, end, ce.name_len, &pool_cursor);
        e->path = cache_copy_wstring_to_pool(&cursor, end, ce.path_len, &pool_cursor);
        e->extension = cache_copy_wstring_to_pool(&cursor, end, ce.extension_len, &pool_cursor);
        e->folded_name = cache_copy_folded_to_pool(e->name, ce.name_len, &pool_cursor);
        e->string_flags = ENTRY_STRING_NAME_POOLED |
                          ENTRY_STRING_PATH_POOLED |
                          ENTRY_STRING_EXTENSION_POOLED |
                          ENTRY_STRING_FOLDED_NAME_POOLED;
        index_prepare_entry(e);
        
        if (!e->name || !e->path || !e->extension || !e->folded_name)
            ok = 0;
        
        if ((i & 4095) == 0)
            Sleep(0);
    }

    if (!ok) {
        goto cleanup;
    }
    
    filtered = (int *)calloc(header.entry_count > 0 ? header.entry_count : 1, sizeof(int));
    if (!filtered) {
        ok = 0;
        goto cleanup;
    }
    
    EnterCriticalSection(&app->index_lock);
    old_entries = app->entries;
    old_entry_count = app->entry_count;
    old_filtered = app->filtered_indices;
    old_string_pool = app->entry_string_pool;
    index_clear_name_char_index(app);
    
    app->entries = entries;
    app->entry_count = header.entry_count;
    app->entry_capacity = header.entry_count > 0 ? header.entry_count : 1;
    app->filtered_indices = filtered;
    app->entry_string_pool = string_pool;
    app->filtered_count = 0;
    app->filtered_identity = 0;
    app->volume_count = header.volume_count;
    memcpy(app->volumes, volumes, sizeof(VOLUME_INFO) * header.volume_count);
    app->indexed_volume_count = header.volume_count;
    app->index_error_count = 0;
    app->cache_loaded = 1;
    InterlockedIncrement(&app->search_generation);
    LeaveCriticalSection(&app->index_lock);
    
    entries = NULL;
    filtered = NULL;
    string_pool = NULL;
    cache_free_entry_array(old_entries, old_entry_count);
    free(old_string_pool);
    free(old_filtered);
    
cleanup:
    if (view)
        UnmapViewOfFile(view);
    if (hMap)
        CloseHandle(hMap);
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
    
    if (!ok) {
        cache_free_entry_array(entries, header.entry_count);
        free(string_pool);
        free(filtered);
        return 0;
    }
    
    return 1;
}
