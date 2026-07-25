#include "cache.h"
#include "config.h"
#include "index.h"
#include <io.h>
#include <stdint.h>

#define CACHE_MAGIC_V3 "ECIDX3"
#define CACHE_MAGIC_V4 "ECIDX4"
#define CACHE_VERSION_V3 3
#define CACHE_VERSION 4
#define CACHE_MAX_STRING_LEN 32767U
#define CACHE_IO_BATCH 2048
#define CACHE_PARENT_NONE (-1)
#define CACHE_ENTRY_FLAG_DIRECTORY 0x01
#define CACHE_ENTRY_FLAG_METADATA_LOADED 0x02

typedef struct {
    char magic[8];
    unsigned int version;
    int volume_count;
    int entry_count;
} CACHE_HEADER_V3;

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
} CACHE_ENTRY_V3;

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    int32_t volume_count;
    int32_t entry_count;
    uint32_t volume_record_size;
    uint32_t entry_record_size;
    uint64_t name_chars;
    uint64_t path_chars;
    uint64_t reserved[2];
} CACHE_HEADER_V4;

typedef struct {
    int64_t size;
    int64_t creation_time;
    int64_t modification_time;
    int64_t access_time;
    int64_t file_ref;
    int64_t parent_ref;
    int64_t usn;
    uint32_t attributes;
    int32_t parent_index;
    uint16_t name_len;
    uint16_t path_len;
    uint8_t volume_index;
    uint8_t flags;
} CACHE_ENTRY_V4;
#pragma pack(pop)

typedef char cache_header_v4_size_must_be_64[
    sizeof(CACHE_HEADER_V4) == 64 ? 1 : -1];
typedef char cache_entry_v4_size_must_be_70[
    sizeof(CACHE_ENTRY_V4) == 70 ? 1 : -1];

typedef struct {
    long long file_ref;
    int volume_index;
    int index;
} CACHE_REF_LOOKUP;

typedef struct {
    VOLUME_INFO volumes[26];
    INDEX_ENTRY *entries;
    wchar_t *string_pool;
    int *filtered;
    int volume_count;
    int entry_count;
    int entry_capacity;
} CACHE_LOAD_DATA;

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

static wchar_t *cache_copy_raw_wstring_to_pool(const unsigned char **cursor,
                                               const unsigned char *end,
                                               unsigned int len,
                                               wchar_t **pool_cursor)
{
    wchar_t *text;
    size_t bytes;

    if (len > CACHE_MAX_STRING_LEN || !pool_cursor || !*pool_cursor)
        return NULL;
    if ((size_t)len > SIZE_MAX / sizeof(wchar_t))
        return NULL;

    bytes = (size_t)len * sizeof(wchar_t);
    if (!cursor || !*cursor || *cursor > end || (size_t)(end - *cursor) < bytes)
        return NULL;

    text = *pool_cursor;
    if (bytes > 0)
        memcpy(text, *cursor, bytes);
    text[len] = L'\0';
    *pool_cursor += (size_t)len + 1;
    *cursor += bytes;
    return text;
}

static int cache_add_chars(size_t *total, size_t count)
{
    if (!total || count > SIZE_MAX - *total)
        return 0;
    *total += count;
    return 1;
}

static int cache_load_capacity(int entry_count)
{
    int capacity = 1;

    if (entry_count > 0) {
        int reserve = entry_count / 16;
        if (reserve < 65536)
            reserve = 65536;
        capacity = entry_count <= INT_MAX - reserve
            ? entry_count + reserve : entry_count;
    }
    return capacity;
}

static void cache_discard_load_data(CACHE_LOAD_DATA *data)
{
    if (!data)
        return;
    cache_free_entry_array(data->entries, data->entry_count);
    free(data->string_pool);
    free(data->filtered);
    memset(data, 0, sizeof(*data));
}

static int cache_ref_compare(const void *a, const void *b)
{
    const CACHE_REF_LOOKUP *ra = (const CACHE_REF_LOOKUP *)a;
    const CACHE_REF_LOOKUP *rb = (const CACHE_REF_LOOKUP *)b;

    if (ra->volume_index != rb->volume_index)
        return ra->volume_index < rb->volume_index ? -1 : 1;
    if (ra->file_ref != rb->file_ref)
        return ra->file_ref < rb->file_ref ? -1 : 1;
    return 0;
}

static int cache_ref_find(const CACHE_REF_LOOKUP *lookup, int count,
                          int volume_index, long long file_ref)
{
    int lo = 0;
    int hi = count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const CACHE_REF_LOOKUP *entry = &lookup[mid];

        if (entry->volume_index == volume_index && entry->file_ref == file_ref)
            return entry->index;
        if (entry->volume_index < volume_index ||
            (entry->volume_index == volume_index && entry->file_ref < file_ref))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return CACHE_PARENT_NONE;
}

static int cache_index_snapshot_matches(APP_STATE *app, int entry_count,
                                        int volume_count, LONG revision)
{
    return !app->shutting_down &&
           app->entry_count == entry_count &&
           app->volume_count == volume_count &&
           app->index_revision == revision;
}

int cache_save_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    wchar_t temp_path[MAX_PATH];
    FILE *f = NULL;
    CACHE_HEADER_V4 header;
    VOLUME_INFO volumes[26];
    CACHE_REF_LOOKUP *lookup = NULL;
    CACHE_ENTRY_V4 *record_batch = NULL;
    unsigned short *path_lengths = NULL;
    wchar_t *name_batch = NULL;
    size_t name_batch_capacity = 0;
    LONG revision;
    int entry_count;
    int volume_count;
    uint64_t name_chars = 0;
    uint64_t path_chars = 0;
    uint64_t written_name_chars = 0;
    int ok = 1;

    if (!app)
        return 0;

    path[0] = L'\0';
    temp_path[0] = L'\0';

    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    volume_count = app->volume_count;
    revision = app->index_revision;
    if (entry_count < 0 || volume_count < 0 || volume_count > 26)
        ok = 0;
    if (ok)
        memcpy(volumes, app->volumes, sizeof(VOLUME_INFO) * (size_t)volume_count);
    LeaveCriticalSection(&app->index_lock);

    if (!ok)
        return 0;

    lookup = (CACHE_REF_LOOKUP *)malloc(
        (size_t)(entry_count > 0 ? entry_count : 1) * sizeof(*lookup));
    record_batch = (CACHE_ENTRY_V4 *)malloc(
        CACHE_IO_BATCH * sizeof(*record_batch));
    path_lengths = (unsigned short *)malloc(
        (size_t)(entry_count > 0 ? entry_count : 1) * sizeof(*path_lengths));
    if (!lookup || !record_batch || !path_lengths) {
        free(lookup);
        free(record_batch);
        free(path_lengths);
        return 0;
    }

    EnterCriticalSection(&app->index_lock);
    if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
        ok = 0;
    } else {
        for (int i = 0; i < entry_count; i++) {
            lookup[i].file_ref = app->entries[i].file_ref;
            lookup[i].volume_index = app->entries[i].volume_index;
            lookup[i].index = i;
        }
    }
    LeaveCriticalSection(&app->index_lock);
    if (!ok)
        goto cleanup;

    qsort(lookup, (size_t)entry_count, sizeof(*lookup), cache_ref_compare);

    for (int i = 0; ok && i < entry_count; ) {
        int batch_end = i + CACHE_IO_BATCH;
        if (batch_end > entry_count)
            batch_end = entry_count;

        EnterCriticalSection(&app->index_lock);
        if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
            ok = 0;
        } else {
            for (; ok && i < batch_end; i++) {
                INDEX_ENTRY *entry = &app->entries[i];
                wchar_t *entry_path;
                size_t name_len;
                size_t path_len;

                if (!entry->name ||
                    entry->volume_index < 0 || entry->volume_index >= volume_count) {
                    ok = 0;
                    break;
                }
                name_len = wcslen(entry->name);
                entry_path = index_duplicate_entry_path_locked(app, i);
                if (!entry_path) {
                    ok = 0;
                    break;
                }
                path_len = wcslen(entry_path);
                free(entry_path);
                if (name_len > CACHE_MAX_STRING_LEN ||
                    path_len > CACHE_MAX_STRING_LEN ||
                    name_chars > UINT64_MAX - name_len ||
                    path_chars > UINT64_MAX - path_len) {
                    ok = 0;
                    break;
                }
                path_lengths[i] = (unsigned short)path_len;
                name_chars += name_len;
                path_chars += path_len;
            }
        }
        LeaveCriticalSection(&app->index_lock);
        Sleep(0);
    }
    if (!ok)
        goto cleanup;

    cache_get_index_path(path, MAX_PATH);
    if (!cache_ensure_dir(path)) {
        ok = 0;
        goto cleanup;
    }
    swprintf_s(temp_path, MAX_PATH, L"%s.tmp", path);
    DeleteFileW(temp_path);

    if (_wfopen_s(&f, temp_path, L"wb") != 0 || !f) {
        ok = 0;
        goto cleanup;
    }

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, CACHE_MAGIC_V4, sizeof(CACHE_MAGIC_V4));
    header.version = CACHE_VERSION;
    header.header_size = sizeof(header);
    header.volume_count = volume_count;
    header.entry_count = entry_count;
    header.volume_record_size = sizeof(VOLUME_INFO);
    header.entry_record_size = sizeof(CACHE_ENTRY_V4);
    header.name_chars = name_chars;
    header.path_chars = path_chars;

    if (ok && fwrite(&header, sizeof(header), 1, f) != 1)
        ok = 0;
    if (ok && volume_count > 0 &&
        fwrite(volumes, sizeof(VOLUME_INFO), (size_t)volume_count, f) !=
            (size_t)volume_count)
        ok = 0;

    for (int i = 0; ok && i < entry_count; ) {
        int batch_start = i;
        int batch_end = i + CACHE_IO_BATCH;
        if (batch_end > entry_count)
            batch_end = entry_count;

        EnterCriticalSection(&app->index_lock);
        if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
            ok = 0;
        } else {
            for (; ok && i < batch_end; i++) {
                INDEX_ENTRY *entry = &app->entries[i];
                CACHE_ENTRY_V4 *ce = &record_batch[i - batch_start];
                size_t name_len = wcslen(entry->name);
                size_t path_len = path_lengths[i];

                if (name_len > CACHE_MAX_STRING_LEN ||
                    path_len > CACHE_MAX_STRING_LEN ||
                    entry->volume_index < 0 || entry->volume_index >= volume_count) {
                    ok = 0;
                    break;
                }

                memset(ce, 0, sizeof(*ce));
                ce->size = entry->size;
                ce->creation_time = entry->creation_time;
                ce->modification_time = entry->modification_time;
                ce->access_time = 0;
                ce->file_ref = entry->file_ref;
                ce->parent_ref = entry->parent_ref;
                ce->usn = 0;
                ce->attributes = entry->attributes;
                ce->parent_index = CACHE_PARENT_NONE;
                if (entry->parent_ref != 0 && entry->parent_ref != 5 &&
                    entry->parent_ref != entry->file_ref) {
                    ce->parent_index = cache_ref_find(
                        lookup, entry_count, entry->volume_index, entry->parent_ref);
                }
                ce->name_len = (uint16_t)name_len;
                ce->path_len = (uint16_t)path_len;
                ce->volume_index = (uint8_t)entry->volume_index;
                if (entry->is_directory)
                    ce->flags |= CACHE_ENTRY_FLAG_DIRECTORY;
                if (entry->metadata_loaded)
                    ce->flags |= CACHE_ENTRY_FLAG_METADATA_LOADED;
            }
        }
        LeaveCriticalSection(&app->index_lock);

        if (ok) {
            size_t batch_count = (size_t)(batch_end - batch_start);
            if (fwrite(record_batch, sizeof(*record_batch), batch_count, f) != batch_count)
                ok = 0;
        }
        Sleep(0);
    }

    for (int i = 0; ok && i < entry_count; ) {
        int batch_start = i;
        int batch_end = i + CACHE_IO_BATCH;
        size_t batch_chars = 0;
        if (batch_end > entry_count)
            batch_end = entry_count;

        EnterCriticalSection(&app->index_lock);
        if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
            ok = 0;
        } else {
            for (int n = batch_start; ok && n < batch_end; n++) {
                INDEX_ENTRY *entry = &app->entries[n];
                size_t name_len = wcslen(entry->name);

                if (name_len > CACHE_MAX_STRING_LEN ||
                    batch_chars > SIZE_MAX - name_len) {
                    ok = 0;
                    break;
                }
                batch_chars += name_len;
            }
        }
        LeaveCriticalSection(&app->index_lock);

        if (ok && batch_chars > name_batch_capacity) {
            wchar_t *next = (wchar_t *)realloc(
                name_batch, (batch_chars > 0 ? batch_chars : 1) * sizeof(wchar_t));
            if (!next) {
                ok = 0;
            } else {
                name_batch = next;
                name_batch_capacity = batch_chars;
            }
        }

        if (ok) {
            wchar_t *dst = name_batch;
            EnterCriticalSection(&app->index_lock);
            if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
                ok = 0;
            } else {
                for (int n = batch_start; n < batch_end; n++) {
                    INDEX_ENTRY *entry = &app->entries[n];
                    size_t name_len = wcslen(entry->name);
                    if (name_len > 0) {
                        memcpy(dst, entry->name, name_len * sizeof(wchar_t));
                        dst += name_len;
                    }
                }
            }
            LeaveCriticalSection(&app->index_lock);
        }

        if (ok && batch_chars > 0 &&
            fwrite(name_batch, sizeof(wchar_t), batch_chars, f) != batch_chars)
            ok = 0;
        if (ok) {
            if (written_name_chars > UINT64_MAX - batch_chars)
                ok = 0;
            else
                written_name_chars += batch_chars;
        }
        i = batch_end;
        Sleep(0);
    }

    if (written_name_chars != name_chars)
        ok = 0;
    if (ok && fflush(f) != 0)
        ok = 0;
    if (ok && _commit(_fileno(f)) != 0)
        ok = 0;

cleanup:
    if (f) {
        if (fclose(f) != 0)
            ok = 0;
        f = NULL;
    }
    free(name_batch);
    free(path_lengths);
    free(record_batch);
    free(lookup);

    if (ok && !MoveFileExW(temp_path, path,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ok = 0;
    if (!ok && temp_path[0])
        DeleteFileW(temp_path);

    return ok;
}

static int cache_load_v3_data(const unsigned char *view, size_t file_size,
                              CACHE_LOAD_DATA *data)
{
    const unsigned char *cursor = view;
    const unsigned char *entry_start;
    const unsigned char *end = view + file_size;
    CACHE_HEADER_V3 header;
    size_t string_chars = 0;
    wchar_t *pool_cursor;
    int ok = 1;

    memset(&header, 0, sizeof(header));
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC_V3, sizeof(CACHE_MAGIC_V3)) != 0 ||
        header.version != CACHE_VERSION_V3 ||
        header.volume_count < 0 || header.volume_count > 26 ||
        header.entry_count < 0) {
        return 0;
    }

    data->volume_count = header.volume_count;
    data->entry_count = header.entry_count;
    data->entry_capacity = cache_load_capacity(header.entry_count);
    if (header.volume_count > 0) {
        size_t volume_bytes = sizeof(VOLUME_INFO) * (size_t)header.volume_count;
        if (!cache_take_bytes(&cursor, end, data->volumes, volume_bytes))
            return 0;
    }

    entry_start = cursor;
    for (int i = 0; ok && i < header.entry_count; i++) {
        CACHE_ENTRY_V3 ce;

        if (!cache_take_bytes(&cursor, end, &ce, sizeof(ce)) ||
            ce.volume_index < 0 || ce.volume_index >= header.volume_count ||
            ce.name_len > CACHE_MAX_STRING_LEN ||
            ce.path_len > CACHE_MAX_STRING_LEN ||
            ce.extension_len > CACHE_MAX_STRING_LEN) {
            ok = 0;
            break;
        }
        if (!cache_skip_wstring(&cursor, end, ce.name_len) ||
            !cache_skip_wstring(&cursor, end, ce.path_len) ||
            !cache_skip_wstring(&cursor, end, ce.extension_len) ||
            !cache_add_chars(&string_chars, (size_t)ce.name_len + 1)) {
            ok = 0;
            break;
        }
    }
    if (!ok || cursor != end || string_chars > SIZE_MAX / sizeof(wchar_t))
        goto fail;

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->string_pool = (wchar_t *)malloc(
        (string_chars > 0 ? string_chars : 1) * sizeof(wchar_t));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->string_pool || !data->filtered)
        goto fail;

    pool_cursor = data->string_pool;
    cursor = entry_start;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V3 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        if (!cache_take_bytes(&cursor, end, &ce, sizeof(ce)))
            goto fail;

        entry->string_flags = ENTRY_STRING_NAME_POOLED;
        entry->size = ce.size;
        entry->creation_time = ce.creation_time;
        entry->modification_time = ce.modification_time;
        entry->attributes = ce.attributes;
        entry->file_ref = ce.file_ref;
        entry->parent_ref = ce.parent_ref;
        entry->is_directory = ce.is_directory;
        entry->volume_index = ce.volume_index;
        entry->metadata_loaded =
            (ce.size != 0 || ce.creation_time != 0 || ce.modification_time != 0);

        entry->name = cache_copy_wstring_to_pool(
            &cursor, end, ce.name_len, &pool_cursor);
        if (!cache_skip_wstring(&cursor, end, ce.path_len) ||
            !cache_skip_wstring(&cursor, end, ce.extension_len) ||
            !entry->name)
            goto fail;
        index_prepare_entry(entry);
    }

    if (cursor != end || pool_cursor != data->string_pool + string_chars)
        goto fail;
    return 1;

fail:
    cache_discard_load_data(data);
    return 0;
}

static int cache_load_v4_data(const unsigned char *view, size_t file_size,
                              CACHE_LOAD_DATA *data)
{
    const unsigned char *cursor = view;
    const unsigned char *end = view + file_size;
    const unsigned char *records;
    const unsigned char *names;
    CACHE_HEADER_V4 header;
    wchar_t *pool_cursor;
    size_t string_chars = 0;
    uint64_t name_chars = 0;
    uint64_t path_chars = 0;
    size_t record_bytes;
    size_t name_bytes;
    int ok = 1;

    memset(&header, 0, sizeof(header));
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC_V4, sizeof(CACHE_MAGIC_V4)) != 0 ||
        header.version != CACHE_VERSION ||
        header.header_size != sizeof(CACHE_HEADER_V4) ||
        header.volume_record_size != sizeof(VOLUME_INFO) ||
        header.entry_record_size != sizeof(CACHE_ENTRY_V4) ||
        header.volume_count < 0 || header.volume_count > 26 ||
        header.entry_count < 0) {
        return 0;
    }

    data->volume_count = header.volume_count;
    data->entry_count = header.entry_count;
    data->entry_capacity = cache_load_capacity(header.entry_count);
    if (header.volume_count > 0) {
        size_t volume_bytes = sizeof(VOLUME_INFO) * (size_t)header.volume_count;
        if (!cache_take_bytes(&cursor, end, data->volumes, volume_bytes))
            return 0;
    }

    if ((size_t)header.entry_count > SIZE_MAX / sizeof(CACHE_ENTRY_V4))
        return 0;
    record_bytes = (size_t)header.entry_count * sizeof(CACHE_ENTRY_V4);
    if ((size_t)(end - cursor) < record_bytes)
        return 0;
    records = cursor;
    cursor += record_bytes;
    names = cursor;

    if (header.name_chars > SIZE_MAX / sizeof(wchar_t))
        return 0;
    name_bytes = (size_t)header.name_chars * sizeof(wchar_t);
    if ((size_t)(end - names) != name_bytes)
        return 0;

    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V4 ce;
        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));

        if (ce.volume_index >= header.volume_count ||
            ce.name_len > CACHE_MAX_STRING_LEN ||
            ce.path_len > CACHE_MAX_STRING_LEN ||
            ce.parent_index < CACHE_PARENT_NONE ||
            ce.parent_index >= header.entry_count ||
            ce.parent_index == i ||
            name_chars > UINT64_MAX - ce.name_len ||
            path_chars > UINT64_MAX - ce.path_len) {
            ok = 0;
            break;
        }
        name_chars += ce.name_len;
        path_chars += ce.path_len;
    }
    if (!ok || name_chars != header.name_chars || path_chars != header.path_chars)
        return 0;

    if (header.name_chars > SIZE_MAX)
        return 0;
    if (!cache_add_chars(&string_chars, (size_t)header.name_chars) ||
        !cache_add_chars(&string_chars, (size_t)header.entry_count) ||
        string_chars > SIZE_MAX / sizeof(wchar_t)) {
        return 0;
    }

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->string_pool = (wchar_t *)malloc(
        (string_chars > 0 ? string_chars : 1) * sizeof(wchar_t));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->string_pool || !data->filtered)
        goto fail;

    pool_cursor = data->string_pool;
    cursor = names;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V4 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
        entry->string_flags = ENTRY_STRING_NAME_POOLED;
        entry->size = ce.size;
        entry->creation_time = ce.creation_time;
        entry->modification_time = ce.modification_time;
        entry->attributes = ce.attributes;
        entry->file_ref = ce.file_ref;
        entry->parent_ref = ce.parent_ref;
        entry->is_directory = (ce.flags & CACHE_ENTRY_FLAG_DIRECTORY) != 0;
        entry->volume_index = ce.volume_index;
        entry->metadata_loaded =
            (ce.flags & CACHE_ENTRY_FLAG_METADATA_LOADED) != 0;
        entry->name = cache_copy_raw_wstring_to_pool(
            &cursor, end, ce.name_len, &pool_cursor);
        if (!entry->name)
            goto fail;
    }
    if (cursor != end)
        goto fail;

    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V4 ce;
        INDEX_ENTRY *entry = &data->entries[i];
        int parent_index;

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
        parent_index = ce.parent_index;
        if (parent_index >= 0 &&
            (data->entries[parent_index].volume_index != entry->volume_index ||
             data->entries[parent_index].file_ref != entry->parent_ref)) {
            goto fail;
        }
    }
    if (pool_cursor != data->string_pool + string_chars)
        goto fail;

    for (int i = 0; i < header.entry_count; i++)
        index_prepare_entry(&data->entries[i]);

    return 1;

fail:
    cache_discard_load_data(data);
    return 0;
}

static void cache_install_load_data(APP_STATE *app, CACHE_LOAD_DATA *data)
{
    INDEX_ENTRY *old_entries;
    int *old_filtered;
    void *old_string_pool;
    int old_entry_count;

    EnterCriticalSection(&app->index_lock);
    old_entries = app->entries;
    old_entry_count = app->entry_count;
    old_filtered = app->filtered_indices;
    old_string_pool = app->entry_string_pool;
    index_clear_name_char_index(app);
    index_clear_filter_index(app);
    index_clear_ref_index(app);

    app->entries = data->entries;
    app->entry_count = data->entry_count;
    app->entry_capacity = data->entry_capacity;
    app->filtered_indices = data->filtered;
    app->entry_string_pool = data->string_pool;
    app->filtered_count = 0;
    app->filtered_identity = 0;
    app->filtered_stale = 0;
    app->volume_count = data->volume_count;
    memcpy(app->volumes, data->volumes,
           sizeof(VOLUME_INFO) * (size_t)data->volume_count);
    app->indexed_volume_count = data->volume_count;
    app->index_error_count = 0;
    app->cache_loaded = 1;
    InterlockedIncrement(&app->index_revision);
    InterlockedIncrement(&app->search_generation);
    LeaveCriticalSection(&app->index_lock);

    data->entries = NULL;
    data->filtered = NULL;
    data->string_pool = NULL;
    cache_free_entry_array(old_entries, old_entry_count);
    free(old_string_pool);
    free(old_filtered);
}

int cache_load_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = NULL;
    unsigned char *view = NULL;
    LARGE_INTEGER file_size;
    CACHE_HEADER_V3 prefix;
    CACHE_LOAD_DATA data;
    int result = CACHE_LOAD_FAILED;

    if (!app)
        return CACHE_LOAD_FAILED;

    memset(&data, 0, sizeof(data));
    memset(&prefix, 0, sizeof(prefix));
    cache_get_index_path(path, MAX_PATH);

    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return CACHE_LOAD_FAILED;

    if (!GetFileSizeEx(hFile, &file_size) ||
        file_size.QuadPart < (LONGLONG)sizeof(prefix) ||
        (uint64_t)file_size.QuadPart > (uint64_t)SIZE_MAX) {
        CloseHandle(hFile);
        return CACHE_LOAD_FAILED;
    }

    hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return CACHE_LOAD_FAILED;
    }
    view = (unsigned char *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return CACHE_LOAD_FAILED;
    }

    memcpy(&prefix, view, sizeof(prefix));
    if (memcmp(prefix.magic, CACHE_MAGIC_V3, sizeof(CACHE_MAGIC_V3)) == 0 &&
        prefix.version == CACHE_VERSION_V3) {
        if (cache_load_v3_data(view, (size_t)file_size.QuadPart, &data))
            result = CACHE_LOAD_LEGACY;
    } else if (memcmp(prefix.magic, CACHE_MAGIC_V4, sizeof(CACHE_MAGIC_V4)) == 0 &&
               prefix.version == CACHE_VERSION) {
        if (cache_load_v4_data(view, (size_t)file_size.QuadPart, &data))
            result = CACHE_LOAD_CURRENT;
    }

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    CloseHandle(hFile);

    if (result == CACHE_LOAD_FAILED) {
        cache_discard_load_data(&data);
        return result;
    }

    cache_install_load_data(app, &data);
    return result;
}
