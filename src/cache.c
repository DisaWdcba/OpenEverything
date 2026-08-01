#include "cache.h"
#include "config.h"
#include "index.h"
#include <io.h>
#include <stdint.h>

#define CACHE_MAGIC_V3 "ECIDX3"
#define CACHE_MAGIC_V4 "ECIDX4"
#define CACHE_MAGIC_V5 "ECIDX5"
#define CACHE_MAGIC_V6 "ECIDX6"
#define CACHE_VERSION_V3 3
#define CACHE_VERSION_V4 4
#define CACHE_VERSION_V5 5
#define CACHE_VERSION 6
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

typedef CACHE_HEADER_V4 CACHE_HEADER_V5;
typedef CACHE_ENTRY_V4 CACHE_ENTRY_V5;
typedef CACHE_HEADER_V4 CACHE_HEADER_V6;
typedef CACHE_ENTRY_V4 CACHE_ENTRY_V6;

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
    INDEX_NAME_POOL names;
    int *filtered;
    int volume_count;
    int entry_count;
    int entry_capacity;
    size_t mapped_name_offset;
    size_t mapped_name_bytes;
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
    index_name_pool_free(&data->names);
    free(data->filtered);
    memset(data, 0, sizeof(*data));
}

static int cache_append_wide_name(const unsigned char **cursor,
                                  const unsigned char *end,
                                  unsigned int length, int has_terminator,
                                  INDEX_NAME_POOL *pool, INDEX_ENTRY *entry)
{
    const wchar_t *name;
    size_t stored_chars;
    size_t bytes;

    if (!cursor || !*cursor || !pool || !entry ||
        length > CACHE_MAX_STRING_LEN)
        return 0;
    stored_chars = (size_t)length + (has_terminator ? 1u : 0u);
    if (stored_chars > SIZE_MAX / sizeof(wchar_t))
        return 0;
    bytes = stored_chars * sizeof(wchar_t);
    if (*cursor > end || (size_t)(end - *cursor) < bytes)
        return 0;
    name = (const wchar_t *)*cursor;
    if ((has_terminator && name[length] != L'\0') ||
        (length > 0 && wmemchr(name, L'\0', length) != NULL)) {
        return 0;
    }
    entry->name_offset = INDEX_NAME_OFFSET_NONE;
    entry->name_length = 0;
    if (!index_name_pool_append_wide(pool, name, length,
                                     &entry->name_offset,
                                     &entry->name_length)) {
        return 0;
    }
    *cursor += bytes;
    return 1;
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

int cache_save_index_to_path(APP_STATE *app, const wchar_t *target_path)
{
    wchar_t path[MAX_PATH];
    wchar_t temp_path[MAX_PATH];
    FILE *f = NULL;
    CACHE_HEADER_V6 header;
    VOLUME_INFO volumes[26];
    CACHE_REF_LOOKUP *lookup = NULL;
    CACHE_ENTRY_V6 *record_batch = NULL;
    unsigned short *path_lengths = NULL;
    char *name_batch = NULL;
    size_t name_batch_capacity = 0;
    LONG revision;
    int entry_count;
    int volume_count;
    uint64_t name_bytes = 0;
    uint64_t path_chars = 0;
    uint64_t written_name_bytes = 0;
    int ok = 1;

    if (!app || !target_path || !target_path[0] ||
        wcslen(target_path) >= MAX_PATH - 4)
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
    record_batch = (CACHE_ENTRY_V6 *)malloc(
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
                const char *name = index_entry_name_utf8_locked(app, entry);
                size_t name_len = entry->name_length;
                size_t path_len;

                if (entry->volume_index < 0 ||
                    entry->volume_index >= volume_count ||
                    (name_len > 0 && name[0] == '\0') ||
                    name[name_len] != '\0') {
                    ok = 0;
                    break;
                }
                /* Only the length is stored, so measure the path instead of
                   building and freeing one string per entry under the lock. */
                path_len = index_entry_path_length_locked(app, i);
                if (name_len > CACHE_MAX_STRING_LEN ||
                    path_len > CACHE_MAX_STRING_LEN ||
                    name_bytes > UINT64_MAX - (name_len + 1) ||
                    path_chars > UINT64_MAX - path_len) {
                    ok = 0;
                    break;
                }
                path_lengths[i] = (unsigned short)path_len;
                name_bytes += name_len + 1;
                path_chars += path_len;
            }
        }
        LeaveCriticalSection(&app->index_lock);
        Sleep(0);
    }
    if (!ok)
        goto cleanup;

    wcscpy_s(path, MAX_PATH, target_path);
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
    memcpy(header.magic, CACHE_MAGIC_V6, sizeof(CACHE_MAGIC_V6));
    header.version = CACHE_VERSION;
    header.header_size = sizeof(header);
    header.volume_count = volume_count;
    header.entry_count = entry_count;
    header.volume_record_size = sizeof(VOLUME_INFO);
    header.entry_record_size = sizeof(CACHE_ENTRY_V6);
    header.name_chars = name_bytes;
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
                CACHE_ENTRY_V6 *ce = &record_batch[i - batch_start];
                const char *name = index_entry_name_utf8_locked(app, entry);
                size_t name_len = entry->name_length;
                size_t path_len = path_lengths[i];

                if (name_len > CACHE_MAX_STRING_LEN ||
                    (name_len > 0 && name[0] == '\0') ||
                    name[name_len] != '\0' ||
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
        size_t batch_bytes = 0;
        if (batch_end > entry_count)
            batch_end = entry_count;

        EnterCriticalSection(&app->index_lock);
        if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
            ok = 0;
        } else {
            for (int n = batch_start; ok && n < batch_end; n++) {
                INDEX_ENTRY *entry = &app->entries[n];
                const char *name = index_entry_name_utf8_locked(app, entry);
                size_t name_len = entry->name_length;

                if (name_len > CACHE_MAX_STRING_LEN ||
                    (name_len > 0 && name[0] == '\0') ||
                    name[name_len] != '\0' ||
                    batch_bytes > SIZE_MAX - (name_len + 1)) {
                    ok = 0;
                    break;
                }
                batch_bytes += name_len + 1;
            }
        }
        LeaveCriticalSection(&app->index_lock);

        if (ok && batch_bytes > name_batch_capacity) {
            char *next = (char *)realloc(
                name_batch, batch_bytes > 0 ? batch_bytes : 1);
            if (!next) {
                ok = 0;
            } else {
                name_batch = next;
                name_batch_capacity = batch_bytes;
            }
        }

        if (ok) {
            char *dst = name_batch;
            EnterCriticalSection(&app->index_lock);
            if (!cache_index_snapshot_matches(app, entry_count, volume_count, revision)) {
                ok = 0;
            } else {
                for (int n = batch_start; n < batch_end; n++) {
                    INDEX_ENTRY *entry = &app->entries[n];
                    const char *name = index_entry_name_utf8_locked(app, entry);
                    size_t stored = (size_t)entry->name_length + 1;

                    if ((entry->name_length > 0 && name[0] == '\0') ||
                        name[entry->name_length] != '\0') {
                        ok = 0;
                        break;
                    }
                    memcpy(dst, name, stored);
                    dst += stored;
                }
            }
            LeaveCriticalSection(&app->index_lock);
        }

        if (ok && batch_bytes > 0 &&
            fwrite(name_batch, 1, batch_bytes, f) != batch_bytes)
            ok = 0;
        if (ok) {
            if (written_name_bytes > UINT64_MAX - batch_bytes)
                ok = 0;
            else
                written_name_bytes += batch_bytes;
        }
        i = batch_end;
        Sleep(0);
    }

    if (written_name_bytes != name_bytes)
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

int cache_save_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];

    cache_get_index_path(path, MAX_PATH);
    return cache_save_index_to_path(app, path);
}

static int cache_load_v3_data(const unsigned char *view, size_t file_size,
                              CACHE_LOAD_DATA *data)
{
    const unsigned char *cursor = view;
    const unsigned char *entry_start;
    const unsigned char *end = view + file_size;
    CACHE_HEADER_V3 header;
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
            !cache_skip_wstring(&cursor, end, ce.extension_len)) {
            ok = 0;
            break;
        }
    }
    if (!ok || cursor != end)
        goto fail;

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->filtered)
        goto fail;

    cursor = entry_start;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V3 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        if (!cache_take_bytes(&cursor, end, &ce, sizeof(ce)))
            goto fail;

        entry->size = ce.size;
        entry->creation_time = ce.creation_time;
        entry->modification_time = ce.modification_time;
        entry->attributes = ce.attributes;
        entry->file_ref = ce.file_ref;
        entry->parent_ref = ce.parent_ref;
        entry->is_directory = (unsigned char)(ce.is_directory != 0);
        entry->volume_index = (signed char)ce.volume_index;
        /* V3 predates the stored parent row; the hint fills in on first use. */
        entry->parent_index = INDEX_PARENT_UNKNOWN;
        entry->metadata_loaded =
            (ce.size != 0 || ce.creation_time != 0 || ce.modification_time != 0);

        if (!cache_append_wide_name(&cursor, end, ce.name_len, 1,
                                    &data->names, entry) ||
            !cache_skip_wstring(&cursor, end, ce.path_len) ||
            !cache_skip_wstring(&cursor, end, ce.extension_len))
            goto fail;
    }

    if (cursor != end)
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
    uint64_t name_chars = 0;
    uint64_t path_chars = 0;
    size_t record_bytes;
    size_t name_bytes;
    int ok = 1;

    memset(&header, 0, sizeof(header));
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC_V4, sizeof(CACHE_MAGIC_V4)) != 0 ||
        header.version != CACHE_VERSION_V4 ||
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

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->filtered)
        goto fail;

    cursor = names;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V4 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
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
        /* The V4 format already stores the parent's row; carrying it into the
           runtime entry is what makes path rebuilds O(depth) instead of a
           lookup per level. The loop below verifies every one of these. */
        entry->parent_index = ce.parent_index;
        if (!cache_append_wide_name(&cursor, end, ce.name_len, 0,
                                    &data->names, entry))
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
    return 1;

fail:
    cache_discard_load_data(data);
    return 0;
}

static int cache_load_v5_data(const unsigned char *view, size_t file_size,
                              CACHE_LOAD_DATA *data)
{
    const unsigned char *cursor = view;
    const unsigned char *end = view + file_size;
    const unsigned char *records;
    const unsigned char *names;
    CACHE_HEADER_V5 header;
    uint64_t name_cursor = 0;
    uint64_t path_chars = 0;
    size_t record_bytes;
    size_t name_bytes;

    memset(&header, 0, sizeof(header));
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC_V5, sizeof(CACHE_MAGIC_V5)) != 0 ||
        header.version != CACHE_VERSION_V5 ||
        header.header_size != sizeof(CACHE_HEADER_V5) ||
        header.volume_record_size != sizeof(VOLUME_INFO) ||
        header.entry_record_size != sizeof(CACHE_ENTRY_V5) ||
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

    if ((size_t)header.entry_count > SIZE_MAX / sizeof(CACHE_ENTRY_V5))
        return 0;
    record_bytes = (size_t)header.entry_count * sizeof(CACHE_ENTRY_V5);
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
        CACHE_ENTRY_V5 ce;
        const wchar_t *name;
        uint64_t stored_chars;

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
        stored_chars = (uint64_t)ce.name_len + 1;
        if (ce.volume_index >= header.volume_count ||
            ce.name_len > CACHE_MAX_STRING_LEN ||
            ce.path_len > CACHE_MAX_STRING_LEN ||
            ce.parent_index < CACHE_PARENT_NONE ||
            ce.parent_index >= header.entry_count ||
            ce.parent_index == i ||
            name_cursor > header.name_chars ||
            stored_chars > header.name_chars - name_cursor ||
            path_chars > UINT64_MAX - ce.path_len) {
            return 0;
        }
        name = (const wchar_t *)(names + (size_t)name_cursor * sizeof(wchar_t));
        if (name[ce.name_len] != L'\0' ||
            (ce.name_len > 0 && wmemchr(name, L'\0', ce.name_len) != NULL)) {
            return 0;
        }
        name_cursor += stored_chars;
        path_chars += ce.path_len;
    }
    if (name_cursor != header.name_chars || path_chars != header.path_chars)
        return 0;

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->filtered)
        goto fail;

    cursor = names;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V5 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
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
        entry->parent_index = ce.parent_index;
        if (!cache_append_wide_name(&cursor, end, ce.name_len, 1,
                                    &data->names, entry))
            goto fail;
    }

    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V5 ce;
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

    return 1;

fail:
    cache_discard_load_data(data);
    return 0;
}

static int cache_load_v6_data(const unsigned char *view, size_t file_size,
                              CACHE_LOAD_DATA *data)
{
    const unsigned char *cursor = view;
    const unsigned char *end = view + file_size;
    const unsigned char *records;
    const unsigned char *names;
    CACHE_HEADER_V6 header;
    uint64_t name_cursor = 0;
    uint64_t path_chars = 0;
    size_t record_bytes;
    size_t name_bytes;

    memset(&header, 0, sizeof(header));
    if (!cache_take_bytes(&cursor, end, &header, sizeof(header)) ||
        memcmp(header.magic, CACHE_MAGIC_V6, sizeof(CACHE_MAGIC_V6)) != 0 ||
        header.version != CACHE_VERSION ||
        header.header_size != sizeof(CACHE_HEADER_V6) ||
        header.volume_record_size != sizeof(VOLUME_INFO) ||
        header.entry_record_size != sizeof(CACHE_ENTRY_V6) ||
        header.volume_count < 0 || header.volume_count > 26 ||
        header.entry_count < 0 || header.name_chars > UINT32_MAX) {
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

    if ((size_t)header.entry_count > SIZE_MAX / sizeof(CACHE_ENTRY_V6))
        return 0;
    record_bytes = (size_t)header.entry_count * sizeof(CACHE_ENTRY_V6);
    if ((size_t)(end - cursor) < record_bytes)
        return 0;
    records = cursor;
    cursor += record_bytes;
    names = cursor;
    name_bytes = (size_t)header.name_chars;
    if ((size_t)(end - names) != name_bytes)
        return 0;

    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V6 ce;
        const char *name;
        uint64_t stored_bytes;

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
        stored_bytes = (uint64_t)ce.name_len + 1;
        if (ce.volume_index >= header.volume_count ||
            ce.name_len > CACHE_MAX_STRING_LEN ||
            ce.path_len > CACHE_MAX_STRING_LEN ||
            ce.parent_index < CACHE_PARENT_NONE ||
            ce.parent_index >= header.entry_count ||
            ce.parent_index == i ||
            name_cursor > header.name_chars ||
            stored_bytes > header.name_chars - name_cursor ||
            path_chars > UINT64_MAX - ce.path_len) {
            return 0;
        }
        name = (const char *)(names + (size_t)name_cursor);
        if (name[ce.name_len] != '\0' ||
            (ce.name_len > 0 && memchr(name, '\0', ce.name_len) != NULL)) {
            return 0;
        }
        name_cursor += stored_bytes;
        path_chars += ce.path_len;
    }
    if (name_cursor != header.name_chars || path_chars != header.path_chars)
        return 0;

    data->entries = (INDEX_ENTRY *)calloc(
        (size_t)data->entry_capacity, sizeof(INDEX_ENTRY));
    data->filtered = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    if (!data->entries || !data->filtered)
        goto fail;

    name_cursor = 0;
    for (int i = 0; i < header.entry_count; i++) {
        CACHE_ENTRY_V6 ce;
        INDEX_ENTRY *entry = &data->entries[i];

        memcpy(&ce, records + (size_t)i * sizeof(ce), sizeof(ce));
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
        entry->parent_index = ce.parent_index;
        entry->name_offset = (unsigned int)name_cursor;
        entry->name_length = ce.name_len;
        name_cursor += (uint64_t)ce.name_len + 1;
    }

    for (int i = 0; i < header.entry_count; i++) {
        INDEX_ENTRY *entry = &data->entries[i];
        int parent_index = entry->parent_index;
        if (parent_index >= 0 &&
            (data->entries[parent_index].volume_index != entry->volume_index ||
             data->entries[parent_index].file_ref != entry->parent_ref)) {
            goto fail;
        }
    }

    data->mapped_name_offset = (size_t)(names - view);
    data->mapped_name_bytes = name_bytes;
    return 1;

fail:
    cache_discard_load_data(data);
    return 0;
}

static int cache_remap_v6_names(HANDLE mapping,
                                CACHE_LOAD_DATA *data)
{
    SYSTEM_INFO system_info;
    uint64_t name_offset;
    uint64_t aligned_offset;
    size_t delta;
    size_t view_bytes;
    unsigned char *name_view;

    if (!mapping || !data)
        return 0;
    if (data->mapped_name_bytes == 0)
        return data->entry_count == 0;

    GetSystemInfo(&system_info);
    if (system_info.dwAllocationGranularity == 0)
        return 0;
    name_offset = data->mapped_name_offset;
    aligned_offset = name_offset -
        (name_offset % system_info.dwAllocationGranularity);
    delta = (size_t)(name_offset - aligned_offset);
    if (data->mapped_name_bytes > SIZE_MAX - delta)
        return 0;
    view_bytes = delta + data->mapped_name_bytes;

    name_view = (unsigned char *)MapViewOfFile(
        mapping, FILE_MAP_READ,
        (DWORD)(aligned_offset >> 32), (DWORD)aligned_offset, view_bytes);
    if (!name_view)
        return 0;

    data->names.mapped_data = (const char *)(name_view + delta);
    data->names.mapped_size = data->mapped_name_bytes;
    data->names.size = data->mapped_name_bytes;
    data->names.capacity = data->mapped_name_bytes;
    data->names.mapped_view = name_view;
    return 1;
}

static void cache_install_load_data(APP_STATE *app, CACHE_LOAD_DATA *data)
{
    INDEX_ENTRY *old_entries;
    int *old_filtered;
    INDEX_NAME_POOL old_names;
    int old_entry_count;

    EnterCriticalSection(&app->index_lock);
    old_entries = app->entries;
    old_entry_count = app->entry_count;
    old_filtered = app->filtered_indices;
    old_names = app->name_pool;
    index_clear_name_char_index(app);
    index_clear_filter_index(app);
    index_clear_ref_index(app);

    app->entries = data->entries;
    app->entry_count = data->entry_count;
    app->entry_capacity = data->entry_capacity;
    app->filtered_indices = data->filtered;
    app->name_pool = data->names;
    app->name_pool_live_size = app->name_pool.size;
    app->filtered_count = 0;
    app->filtered_identity = 0;
    app->filtered_stale = 0;
    app->volume_count = data->volume_count;
    memcpy(app->volumes, data->volumes,
           sizeof(VOLUME_INFO) * (size_t)data->volume_count);
    app->indexed_volume_count = data->volume_count;
    app->index_error_count = 0;
    app->cache_loaded = 1;
    for (int i = 0; i < app->entry_count; i++)
        index_prepare_entry(app, &app->entries[i]);
    InterlockedIncrement(&app->index_revision);
    InterlockedIncrement(&app->search_generation);
    LeaveCriticalSection(&app->index_lock);

    data->entries = NULL;
    data->filtered = NULL;
    memset(&data->names, 0, sizeof(data->names));
    cache_free_entry_array(old_entries, old_entry_count);
    index_name_pool_free(&old_names);
    free(old_filtered);
}

int cache_load_index_from_path(APP_STATE *app, const wchar_t *path)
{
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = NULL;
    unsigned char *view = NULL;
    LARGE_INTEGER file_size;
    CACHE_HEADER_V3 prefix;
    CACHE_LOAD_DATA data;
    int result = CACHE_LOAD_FAILED;

    if (!app || !path || !path[0])
        return CACHE_LOAD_FAILED;

    memset(&data, 0, sizeof(data));
    memset(&prefix, 0, sizeof(prefix));
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
               prefix.version == CACHE_VERSION_V4) {
        if (cache_load_v4_data(view, (size_t)file_size.QuadPart, &data))
            result = CACHE_LOAD_LEGACY;
    } else if (memcmp(prefix.magic, CACHE_MAGIC_V5, sizeof(CACHE_MAGIC_V5)) == 0 &&
               prefix.version == CACHE_VERSION_V5) {
        if (cache_load_v5_data(view, (size_t)file_size.QuadPart, &data)) {
            result = CACHE_LOAD_LEGACY;
        }
    } else if (memcmp(prefix.magic, CACHE_MAGIC_V6, sizeof(CACHE_MAGIC_V6)) == 0 &&
               prefix.version == CACHE_VERSION) {
        if (cache_load_v6_data(view, (size_t)file_size.QuadPart, &data) &&
            cache_remap_v6_names(hMap, &data)) {
            result = CACHE_LOAD_CURRENT;
        }
    }

    if (view)
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

int cache_load_index(APP_STATE *app)
{
    wchar_t path[MAX_PATH];

    cache_get_index_path(path, MAX_PATH);
    return cache_load_index_from_path(app, path);
}
