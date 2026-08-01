#include "index.h"
#include "common.h"
#include <limits.h>
#include <stdint.h>

#define REF_INDEX_EMPTY (-1)
#define REF_INDEX_TOMBSTONE (-2)

static unsigned long long index_compute_char_mask_utf8(const char *text,
                                                       size_t length);

void index_name_pool_init(INDEX_NAME_POOL *pool)
{
    if (pool)
        memset(pool, 0, sizeof(*pool));
}

void index_name_pool_free(INDEX_NAME_POOL *pool)
{
    if (!pool)
        return;
    free(pool->data);
    memset(pool, 0, sizeof(*pool));
}

static int index_name_pool_reserve(INDEX_NAME_POOL *pool, size_t needed)
{
    char *grown;
    size_t capacity;

    if (!pool || needed > UINT32_MAX)
        return 0;
    if (needed <= pool->capacity)
        return 1;

    capacity = pool->capacity > 0 ? pool->capacity : 65536;
    while (capacity < needed) {
        size_t next = capacity + capacity / 2 + 4096;
        if (next <= capacity || next > UINT32_MAX) {
            capacity = UINT32_MAX;
            break;
        }
        capacity = next;
    }
    if (capacity < needed)
        return 0;
    grown = (char *)realloc(pool->data, capacity);
    if (!grown)
        return 0;
    pool->data = grown;
    pool->capacity = capacity;
    return 1;
}

int index_name_pool_append_utf8(INDEX_NAME_POOL *pool, const char *name,
                                size_t name_length, unsigned int *offset,
                                unsigned short *utf8_length)
{
    size_t needed;

    if (!pool || !offset || !utf8_length || name_length > USHRT_MAX ||
        (name_length > 0 && !name) ||
        pool->size > UINT32_MAX || name_length + 1 > UINT32_MAX - pool->size) {
        return 0;
    }
    needed = pool->size + name_length + 1;
    if (!index_name_pool_reserve(pool, needed))
        return 0;

    *offset = (unsigned int)pool->size;
    *utf8_length = (unsigned short)name_length;
    if (name_length > 0)
        memcpy(pool->data + pool->size, name, name_length);
    pool->data[pool->size + name_length] = '\0';
    pool->size = needed;
    return 1;
}

int index_name_pool_append_wide(INDEX_NAME_POOL *pool, const wchar_t *name,
                                size_t name_length, unsigned int *offset,
                                unsigned short *utf8_length)
{
    int bytes;
    size_t needed;

    if (!pool || !offset || !utf8_length || name_length > INT_MAX ||
        (name_length > 0 && !name)) {
        return 0;
    }
    if (name_length == 0)
        return index_name_pool_append_utf8(pool, "", 0, offset, utf8_length);

    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name,
                                (int)name_length, NULL, 0, NULL, NULL);
    if (bytes <= 0) {
        bytes = WideCharToMultiByte(CP_UTF8, 0, name, (int)name_length,
                                    NULL, 0, NULL, NULL);
    }
    if (bytes <= 0 || bytes > USHRT_MAX || pool->size > UINT32_MAX ||
        (size_t)bytes + 1 > UINT32_MAX - pool->size) {
        return 0;
    }

    needed = pool->size + (size_t)bytes + 1;
    if (!index_name_pool_reserve(pool, needed))
        return 0;
    *offset = (unsigned int)pool->size;
    *utf8_length = (unsigned short)bytes;
    if (WideCharToMultiByte(CP_UTF8, 0, name, (int)name_length,
                            pool->data + pool->size, bytes, NULL, NULL) != bytes) {
        return 0;
    }
    pool->data[pool->size + bytes] = '\0';
    pool->size = needed;
    return 1;
}

const char *index_name_pool_at(const INDEX_NAME_POOL *pool,
                               unsigned int offset, unsigned short length)
{
    if (!pool || !pool->data || offset == INDEX_NAME_OFFSET_NONE ||
        offset > pool->size || (size_t)length >= pool->size - offset ||
        pool->data[offset + length] != '\0') {
        return "";
    }
    return pool->data + offset;
}

void index_build_init(INDEX_BUILD *build)
{
    if (!build)
        return;
    memset(build, 0, sizeof(*build));
    index_name_pool_init(&build->names);
}

void index_build_free(INDEX_BUILD *build)
{
    if (!build)
        return;
    free(build->entries);
    index_name_pool_free(&build->names);
    memset(build, 0, sizeof(*build));
}

const char *index_entry_name_utf8_locked(const APP_STATE *app,
                                         const INDEX_ENTRY *entry)
{
    if (!app || !entry)
        return "";
    return index_name_pool_at(&app->name_pool, entry->name_offset,
                              entry->name_length);
}

int index_copy_entry_name_locked(const APP_STATE *app, const INDEX_ENTRY *entry,
                                 wchar_t *buffer, size_t capacity)
{
    const char *name;
    int chars;

    if (!buffer || capacity == 0 || capacity > INT_MAX)
        return 0;
    buffer[0] = L'\0';
    if (!app || !entry)
        return 1;
    name = index_entry_name_utf8_locked(app, entry);
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                entry->name_length, NULL, 0);
    if ((entry->name_length > 0 && chars == 0) || (size_t)chars >= capacity)
        return 0;
    if (chars > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                         entry->name_length, buffer, chars) != chars) {
        buffer[0] = L'\0';
        return 0;
    }
    buffer[chars] = L'\0';
    return 1;
}

wchar_t *index_duplicate_entry_name_locked(const APP_STATE *app,
                                            const INDEX_ENTRY *entry)
{
    const char *name;
    wchar_t *wide;
    int chars;

    if (!app || !entry)
        return NULL;
    name = index_entry_name_utf8_locked(app, entry);
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                entry->name_length, NULL, 0);
    if ((entry->name_length > 0 && chars == 0) ||
        (size_t)chars > (SIZE_MAX / sizeof(wchar_t)) - 1)
        return NULL;
    wide = (wchar_t *)malloc(((size_t)chars + 1) * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (chars > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                         entry->name_length, wide, chars) != chars) {
        free(wide);
        return NULL;
    }
    wide[chars] = L'\0';
    return wide;
}

int index_copy_entry_extension_locked(const APP_STATE *app,
                                      const INDEX_ENTRY *entry,
                                      wchar_t *buffer, size_t capacity)
{
    const char *name;
    const char *dot = NULL;
    size_t extension_length;
    int chars;

    if (!buffer || capacity == 0 || capacity > INT_MAX)
        return 0;
    buffer[0] = L'\0';
    if (!app || !entry)
        return 1;
    name = index_entry_name_utf8_locked(app, entry);
    for (size_t i = 0; i < entry->name_length; i++) {
        if (name[i] == '.')
            dot = name + i + 1;
    }
    if (!dot || dot >= name + entry->name_length)
        return 1;
    extension_length = (size_t)(name + entry->name_length - dot);
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dot,
                                (int)extension_length, NULL, 0);
    if ((extension_length > 0 && chars == 0) || (size_t)chars >= capacity)
        return 0;
    if (chars > 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dot,
                                         (int)extension_length,
                                         buffer, chars) != chars) {
        return 0;
    }
    buffer[chars] = L'\0';
    return 1;
}

static unsigned int index_utf8_next(const unsigned char **cursor,
                                    const unsigned char *end)
{
    unsigned int first;
    unsigned int value;
    int extra;

    if (*cursor >= end)
        return 0;
    first = *(*cursor)++;
    if (first < 0x80)
        return first;
    if ((first & 0xE0) == 0xC0) {
        value = first & 0x1F;
        extra = 1;
    } else if ((first & 0xF0) == 0xE0) {
        value = first & 0x0F;
        extra = 2;
    } else if ((first & 0xF8) == 0xF0) {
        value = first & 0x07;
        extra = 3;
    } else {
        return 0xFFFD;
    }
    for (int i = 0; i < extra; i++) {
        unsigned int next;
        if (*cursor >= end)
            return 0xFFFD;
        next = *(*cursor)++;
        if ((next & 0xC0) != 0x80)
            return 0xFFFD;
        value = (value << 6) | (next & 0x3F);
    }
    return value;
}

int index_compare_entry_names_locked(const APP_STATE *app,
                                     const INDEX_ENTRY *left,
                                     const INDEX_ENTRY *right)
{
    const unsigned char *a;
    const unsigned char *b;
    const unsigned char *a_end;
    const unsigned char *b_end;

    if (!app || !left || !right)
        return left ? 1 : right ? -1 : 0;
    a = (const unsigned char *)index_entry_name_utf8_locked(app, left);
    b = (const unsigned char *)index_entry_name_utf8_locked(app, right);
    a_end = a + left->name_length;
    b_end = b + right->name_length;
    while (a < a_end && b < b_end) {
        unsigned int ca;
        unsigned int cb;

        if (*a < 0x80 && *b < 0x80) {
            ca = *a++;
            cb = *b++;
            if (ca >= 'A' && ca <= 'Z')
                ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z')
                cb += 'a' - 'A';
            if (ca != cb)
                return ca < cb ? -1 : 1;
            continue;
        }

        ca = index_utf8_next(&a, a_end);
        cb = index_utf8_next(&b, b_end);
        if (ca <= 0xFFFF)
            ca = (unsigned int)towlower((wchar_t)ca);
        if (cb <= 0xFFFF)
            cb = (unsigned int)towlower((wchar_t)cb);
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (a < a_end)
        return 1;
    if (b < b_end)
        return -1;
    return 0;
}

int index_set_entry_name_locked(APP_STATE *app, INDEX_ENTRY *entry,
                                const wchar_t *name, size_t name_length)
{
    if (!app || !entry)
        return 0;
    return index_name_pool_append_wide(&app->name_pool, name, name_length,
                                       &entry->name_offset,
                                       &entry->name_length);
}

static unsigned long long index_ref_key(int volume_index, long long file_ref)
{
    unsigned long long ref = (unsigned long long)file_ref & 0x0000FFFFFFFFFFFFULL;
    return ref | ((unsigned long long)(volume_index + 1) << 48);
}

static unsigned int index_ref_hash(unsigned long long key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return (unsigned int)key;
}

static int index_ref_insert_snapshot(int *values, int capacity,
                                     const unsigned long long *entry_keys,
                                     int entry_count,
                                     unsigned long long key, int value)
{
    unsigned int pos;
    int tombstone = -1;

    if (!values || !entry_keys || capacity <= 0 ||
        value < 0 || value >= entry_count)
        return 0;

    pos = index_ref_hash(key) & (unsigned int)(capacity - 1);
    for (int probe = 0; probe < capacity; probe++) {
        int current = values[pos];
        if (current >= 0 && current < entry_count && entry_keys[current] == key) {
            values[pos] = value;
            return 1;
        }
        if (current == REF_INDEX_TOMBSTONE && tombstone < 0)
            tombstone = (int)pos;
        if (current == REF_INDEX_EMPTY) {
            if (tombstone >= 0)
                pos = (unsigned int)tombstone;
            values[pos] = value;
            return 1;
        }
        pos = (pos + 1) & (unsigned int)(capacity - 1);
    }
    return 0;
}

static int index_ref_find_locked(const APP_STATE *app, unsigned long long key)
{
    unsigned int pos;

    if (!app || !app->ref_index_values || app->ref_index_capacity <= 0)
        return -1;

    pos = index_ref_hash(key) & (unsigned int)(app->ref_index_capacity - 1);
    for (int probe = 0; probe < app->ref_index_capacity; probe++) {
        int current = app->ref_index_values[pos];
        if (current == REF_INDEX_EMPTY)
            return -1;
        if (current >= 0 && current < app->entry_count) {
            const INDEX_ENTRY *entry = &app->entries[current];
            if (index_ref_key(entry->volume_index, entry->file_ref) == key)
                return current;
        }
        pos = (pos + 1) & (unsigned int)(app->ref_index_capacity - 1);
    }
    return -1;
}

static int index_ref_insert_locked(APP_STATE *app, unsigned long long key, int value)
{
    unsigned int pos;
    int tombstone = -1;

    if (!app || !app->ref_index_values || app->ref_index_capacity <= 0 ||
        value < 0 || value >= app->entry_count)
        return 0;

    pos = index_ref_hash(key) & (unsigned int)(app->ref_index_capacity - 1);
    for (int probe = 0; probe < app->ref_index_capacity; probe++) {
        int current = app->ref_index_values[pos];
        if (current >= 0 && current < app->entry_count) {
            const INDEX_ENTRY *entry = &app->entries[current];
            if (index_ref_key(entry->volume_index, entry->file_ref) == key) {
                app->ref_index_values[pos] = value;
                return 1;
            }
        }
        if (current == REF_INDEX_TOMBSTONE && tombstone < 0)
            tombstone = (int)pos;
        if (current == REF_INDEX_EMPTY) {
            if (tombstone >= 0)
                pos = (unsigned int)tombstone;
            app->ref_index_values[pos] = value;
            return 1;
        }
        pos = (pos + 1) & (unsigned int)(app->ref_index_capacity - 1);
    }
    return 0;
}

static void index_ref_remove_locked(APP_STATE *app, int volume_index, long long file_ref)
{
    unsigned long long key;
    unsigned int pos;
    
    if (!app->ref_index_ready || app->ref_index_capacity <= 0)
        return;
    
    key = index_ref_key(volume_index, file_ref);
    pos = index_ref_hash(key) & (unsigned int)(app->ref_index_capacity - 1);
    for (int probe = 0; probe < app->ref_index_capacity; probe++) {
        int current = app->ref_index_values[pos];
        if (current == REF_INDEX_EMPTY)
            return;
        if (current >= 0 && current < app->entry_count) {
            const INDEX_ENTRY *entry = &app->entries[current];
            if (index_ref_key(entry->volume_index, entry->file_ref) == key) {
                app->ref_index_values[pos] = REF_INDEX_TOMBSTONE;
                return;
            }
        }
        pos = (pos + 1) & (unsigned int)(app->ref_index_capacity - 1);
    }
}

#define INDEX_MAX_PATH_DEPTH 1024

static int index_entry_is_volume_root(const APP_STATE *app,
                                      const INDEX_ENTRY *entry)
{
    const char *name;

    if (!app || !entry || entry->file_ref != NTFS_ROOT_FRN)
        return 0;
    name = index_entry_name_utf8_locked(app, entry);
    return entry && entry->file_ref == 5 &&
           (entry->parent_ref == 5 || entry->name_length == 0 ||
            (entry->name_length == 1 && name[0] == '.'));
}

static int index_find_parent_locked(APP_STATE *app, int volume_index,
                                    long long file_ref)
{
    if (!app || file_ref == 0)
        return -1;
    if (app->ref_index_ready) {
        return index_ref_find_locked(app, index_ref_key(volume_index, file_ref));
    }
    for (int i = 0; i < app->entry_count; i++) {
        if (app->entries[i].volume_index == volume_index &&
            app->entries[i].file_ref == file_ref)
            return i;
    }
    return -1;
}

/* Resolve an entry's parent row, using the cached hint when it still points at
   the right file and repairing it when it does not. The hint turns each step of
   a path walk into an array access; without it every step falls back to
   index_find_parent_locked, which degrades to a full scan whenever the ref
   index is not built. The caller must hold index_lock. */
static int index_resolve_parent_locked(APP_STATE *app, INDEX_ENTRY *entry)
{
    int hint = entry->parent_index;
    int found;

    if (hint >= 0 && hint < app->entry_count) {
        const INDEX_ENTRY *candidate = &app->entries[hint];
        if (candidate->volume_index == entry->volume_index &&
            candidate->file_ref == entry->parent_ref)
            return hint;
    }

    found = index_find_parent_locked(app, entry->volume_index, entry->parent_ref);
    entry->parent_index = found;
    return found;
}

/* Walk from an entry up to its volume root, recording the rows visited and the
   drive prefix. Returns the number of parts, or 0 for an invalid entry. */
static int index_collect_path_parts_locked(APP_STATE *app, int entry_index,
                                           int *parts, const wchar_t **out_drive)
{
    int part_count = 0;
    int current = entry_index;
    signed char volume_index;

    *out_drive = L"";
    if (!app || entry_index < 0 || entry_index >= app->entry_count)
        return 0;

    while (current >= 0 && current < app->entry_count &&
           part_count < INDEX_MAX_PATH_DEPTH) {
        INDEX_ENTRY *entry = &app->entries[current];
        int parent;

        parts[part_count++] = current;
        if (index_entry_is_volume_root(app, entry) || entry->parent_ref == 0 ||
            entry->parent_ref == entry->file_ref)
            break;
        /* The volume root contributes no path component -- the drive letter
           already stands in for it, and the write loop skips it anyway. Stop
           here instead of resolving it: the cache stores no parent row for
           these, so every path was ending in a full-index scan. */
        if (entry->parent_ref == NTFS_ROOT_FRN)
            break;
        parent = index_resolve_parent_locked(app, entry);
        if (parent < 0 || parent == current)
            break;
        current = parent;
    }

    volume_index = app->entries[entry_index].volume_index;
    if (volume_index >= 0 && volume_index < app->volume_count)
        *out_drive = app->volumes[volume_index].drive_letter;
    return part_count;
}

/* Character count of the joined path, excluding the terminator. Kept in step
   with the write loop in index_duplicate_entry_path_locked. */
static size_t index_measure_path_locked(APP_STATE *app, const int *parts,
                                        int part_count, const wchar_t *drive)
{
    size_t length = wcslen(drive);
    wchar_t last_char = length > 0 ? drive[length - 1] : L'\0';

    for (int i = part_count - 1; i >= 0; i--) {
        const INDEX_ENTRY *entry = &app->entries[parts[i]];
        const char *name;
        int name_chars;

        if (index_entry_is_volume_root(app, entry))
            continue;
        name = index_entry_name_utf8_locked(app, entry);
        if (entry->name_length == 0)
            continue;
        if (length > 0 && last_char != L'\\' && last_char != L'/')
            length++;
        name_chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                         entry->name_length, NULL, 0);
        if (name_chars <= 0 || (size_t)name_chars > SIZE_MAX - length)
            return 0;
        length += (size_t)name_chars;
        last_char = L'x';
    }
    return length;
}

/* Measure without building. cache_save_index needs only the length, and
   materializing a string per entry just to call wcslen on it dominated the
   save while holding index_lock. */
size_t index_entry_path_length_locked(APP_STATE *app, int entry_index)
{
    int parts[INDEX_MAX_PATH_DEPTH];
    const wchar_t *drive;
    int part_count;

    part_count = index_collect_path_parts_locked(app, entry_index, parts, &drive);
    if (part_count == 0)
        return 0;
    return index_measure_path_locked(app, parts, part_count, drive);
}

wchar_t *index_duplicate_entry_path_locked(APP_STATE *app, int entry_index)
{
    int parts[INDEX_MAX_PATH_DEPTH];
    const wchar_t *drive;
    int part_count;
    size_t length;
    wchar_t *path;
    size_t cursor;

    part_count = index_collect_path_parts_locked(app, entry_index, parts, &drive);
    if (part_count == 0)
        return NULL;

    length = index_measure_path_locked(app, parts, part_count, drive);

    path = (wchar_t *)malloc((length + 1) * sizeof(*path));
    if (!path)
        return NULL;
    cursor = 0;
    if (drive[0]) {
        size_t drive_len = wcslen(drive);
        memcpy(path, drive, drive_len * sizeof(*path));
        cursor = drive_len;
    }
    for (int i = part_count - 1; i >= 0; i--) {
        const INDEX_ENTRY *entry = &app->entries[parts[i]];
        const char *name;
        int name_chars;
        if (index_entry_is_volume_root(app, entry))
            continue;
        name = index_entry_name_utf8_locked(app, entry);
        if (entry->name_length == 0)
            continue;
        if (cursor > 0 && path[cursor - 1] != L'\\' && path[cursor - 1] != L'/')
            path[cursor++] = L'\\';
        name_chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name,
                                         entry->name_length, path + cursor,
                                         (int)(length - cursor));
        if (name_chars <= 0) {
            free(path);
            return NULL;
        }
        cursor += (size_t)name_chars;
    }
    path[cursor] = L'\0';
    return path;
}

static unsigned long long index_compute_char_mask_utf8(const char *text,
                                                       size_t length)
{
    unsigned long long mask = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    const unsigned char *end = cursor + length;
    
    if (!text)
        return 0;
    
    while (cursor < end) {
        unsigned int codepoint = index_utf8_next(&cursor, end);
        wchar_t units[2];
        int unit_count = 1;

        if (codepoint <= 0xFFFF) {
            units[0] = (wchar_t)codepoint;
        } else if (codepoint <= 0x10FFFF) {
            codepoint -= 0x10000;
            units[0] = (wchar_t)(0xD800 + (codepoint >> 10));
            units[1] = (wchar_t)(0xDC00 + (codepoint & 0x3FF));
            unit_count = 2;
        } else {
            units[0] = 0xFFFD;
        }
        for (int i = 0; i < unit_count; i++) {
            int slot = index_char_mask_slot(units[i]);
            if (slot >= 0)
                mask |= 1ULL << slot;
        }
    }

    return mask;
}

static int index_filter_type_for_extension_utf8(const char *extension,
                                                size_t length)
{
#define EXT_IS(value) \
    (length == sizeof(value) - 1 && _strnicmp(extension, value, sizeof(value) - 1) == 0)
    unsigned char first;

    if (!extension || length == 0)
        return FILTER_EVERYTHING;

    first = (unsigned char)extension[0];
    if (first >= 'A' && first <= 'Z')
        first = (unsigned char)(first + ('a' - 'A'));
    switch (first) {
    case '3': return EXT_IS("3gp") ? FILTER_VIDEO : FILTER_EVERYTHING;
    case '7': return EXT_IS("7z") ? FILTER_COMPRESSED : FILTER_EVERYTHING;
    case 'a':
        if (EXT_IS("aac")) return FILTER_AUDIO;
        if (EXT_IS("avif")) return FILTER_IMAGE;
        if (EXT_IS("avi")) return FILTER_VIDEO;
        break;
    case 'b':
        if (EXT_IS("bat")) return FILTER_EXECUTABLE;
        if (EXT_IS("bmp")) return FILTER_IMAGE;
        if (EXT_IS("bz2")) return FILTER_COMPRESSED;
        break;
    case 'c':
        if (EXT_IS("cab")) return FILTER_COMPRESSED;
        if (EXT_IS("cmd") || EXT_IS("com")) return FILTER_EXECUTABLE;
        if (EXT_IS("csv")) return FILTER_DOCUMENT;
        break;
    case 'd':
        if (EXT_IS("dng")) return FILTER_IMAGE;
        if (EXT_IS("doc") || EXT_IS("docx")) return FILTER_DOCUMENT;
        break;
    case 'e': return EXT_IS("exe") ? FILTER_EXECUTABLE : FILTER_EVERYTHING;
    case 'f': return EXT_IS("flac") ? FILTER_AUDIO : FILTER_EVERYTHING;
    case 'g':
        if (EXT_IS("gif")) return FILTER_IMAGE;
        if (EXT_IS("gz")) return FILTER_COMPRESSED;
        break;
    case 'h': return EXT_IS("heic") ? FILTER_IMAGE : FILTER_EVERYTHING;
    case 'i':
        if (EXT_IS("ico")) return FILTER_IMAGE;
        if (EXT_IS("iso")) return FILTER_COMPRESSED;
        break;
    case 'j':
        if (EXT_IS("jpg") || EXT_IS("jpeg")) return FILTER_IMAGE;
        break;
    case 'm':
        if (EXT_IS("m4a") || EXT_IS("mp3")) return FILTER_AUDIO;
        if (EXT_IS("md")) return FILTER_DOCUMENT;
        if (EXT_IS("msi")) return FILTER_EXECUTABLE;
        if (EXT_IS("m4v") || EXT_IS("mkv") || EXT_IS("mov") ||
            EXT_IS("mp4") || EXT_IS("mpeg") || EXT_IS("mpg") || EXT_IS("mts"))
            return FILTER_VIDEO;
        break;
    case 'o':
        if (EXT_IS("ods") || EXT_IS("odp") || EXT_IS("odt")) return FILTER_DOCUMENT;
        if (EXT_IS("ogg") || EXT_IS("opus")) return FILTER_AUDIO;
        break;
    case 'p':
        if (EXT_IS("pdf") || EXT_IS("ppt") || EXT_IS("pptx")) return FILTER_DOCUMENT;
        if (EXT_IS("png")) return FILTER_IMAGE;
        if (EXT_IS("ps1")) return FILTER_EXECUTABLE;
        break;
    case 'r':
        if (EXT_IS("rar")) return FILTER_COMPRESSED;
        if (EXT_IS("raw")) return FILTER_IMAGE;
        if (EXT_IS("rtf")) return FILTER_DOCUMENT;
        break;
    case 's':
        if (EXT_IS("scr")) return FILTER_EXECUTABLE;
        if (EXT_IS("svg")) return FILTER_IMAGE;
        break;
    case 't':
        if (EXT_IS("tar")) return FILTER_COMPRESSED;
        if (EXT_IS("tif") || EXT_IS("tiff")) return FILTER_IMAGE;
        if (EXT_IS("ts")) return FILTER_VIDEO;
        if (EXT_IS("txt")) return FILTER_DOCUMENT;
        break;
    case 'w':
        if (EXT_IS("wav") || EXT_IS("wma")) return FILTER_AUDIO;
        if (EXT_IS("webm") || EXT_IS("wmv")) return FILTER_VIDEO;
        if (EXT_IS("webp")) return FILTER_IMAGE;
        break;
    case 'x':
        if (EXT_IS("xls") || EXT_IS("xlsx")) return FILTER_DOCUMENT;
        if (EXT_IS("xz")) return FILTER_COMPRESSED;
        break;
    case 'z': return EXT_IS("zip") ? FILTER_COMPRESSED : FILTER_EVERYTHING;
    }
#undef EXT_IS
    return FILTER_EVERYTHING;
}

void index_prepare_entry(APP_STATE *app, INDEX_ENTRY *entry)
{
    const char *name;
    const char *extension = NULL;

    if (!app || !entry)
        return;
    name = index_entry_name_utf8_locked(app, entry);
    for (size_t i = 0; i < entry->name_length; i++) {
        if (name[i] == '.')
            extension = name + i + 1;
    }

    entry->filter_type = (unsigned char)(entry->is_directory
        ? FILTER_FOLDER
        : extension && extension < name + entry->name_length
            ? index_filter_type_for_extension_utf8(
                extension, (size_t)(name + entry->name_length - extension))
            : FILTER_EVERYTHING);
}

static int index_find_by_ref_locked(APP_STATE *app, int volume_index, long long file_ref)
{
    return index_find_parent_locked(app, volume_index, file_ref);
}

static int index_ensure_capacity_locked(APP_STATE *app, int needed)
{
    if (needed <= app->entry_capacity)
        return 1;
    
    int new_cap = app->entry_capacity > 0 ? app->entry_capacity : 65536;
    while (new_cap < needed)
        new_cap *= 2;
    
    INDEX_ENTRY *new_entries = (INDEX_ENTRY *)calloc(new_cap, sizeof(INDEX_ENTRY));
    if (!new_entries) {
        free(new_entries);
        return 0;
    }
    
    memcpy(new_entries, app->entries, app->entry_count * sizeof(INDEX_ENTRY));
    free(app->entries);
    app->entries = new_entries;
    app->entry_capacity = new_cap;
    return 1;
}

static int index_set_entry_from_change(APP_STATE *app, INDEX_ENTRY *entry,
                                       const USN_CHANGE *change)
{
    memset(entry, 0, sizeof(*entry));
    entry->name_offset = INDEX_NAME_OFFSET_NONE;

    if (!index_set_entry_name_locked(app, entry,
                                     change->name ? change->name : L"",
                                     change->name ? wcslen(change->name) : 0))
        return 0;
    
    entry->size = 0;
    entry->creation_time = 0;
    entry->modification_time = 0;
    entry->attributes = change->attributes;
    entry->file_ref = change->file_ref;
    entry->parent_ref = change->parent_ref;
    entry->is_directory = (unsigned char)(change->is_directory != 0);
    entry->volume_index = (signed char)change->volume_index;
    entry->metadata_loaded = 0;
    /* memset left this at 0, which is a valid row; say "unknown" explicitly. */
    entry->parent_index = INDEX_PARENT_UNKNOWN;
    index_prepare_entry(app, entry);
    return 1;
}

void index_init(APP_STATE *app)
{
    memset(app, 0, sizeof(*app));
    InitializeCriticalSection(&app->index_lock);
    index_name_pool_init(&app->name_pool);
    app->entry_capacity = 65536;
    app->entries = (INDEX_ENTRY *)calloc(app->entry_capacity, sizeof(INDEX_ENTRY));
    app->filtered_indices = (int *)calloc(SEARCH_MAX_RESULTS, sizeof(int));
    app->query.sort_column = COL_NAME;
    app->query.sort_ascending = 1;
}

int index_compact_entry_names(APP_STATE *app)
{
    INDEX_NAME_POOL compacted;
    unsigned int *offsets = NULL;
    int ok = 1;

    if (!app)
        return 0;
    index_name_pool_init(&compacted);
    EnterCriticalSection(&app->index_lock);
    offsets = (unsigned int *)malloc(
        (size_t)(app->entry_count > 0 ? app->entry_count : 1) * sizeof(*offsets));
    if (!offsets)
        ok = 0;
    for (int i = 0; ok && i < app->entry_count; i++) {
        const INDEX_ENTRY *entry = &app->entries[i];
        const char *name = index_entry_name_utf8_locked(app, entry);
        unsigned short copied_length;
        if (!index_name_pool_append_utf8(&compacted, name, entry->name_length,
                                         &offsets[i], &copied_length) ||
            copied_length != entry->name_length) {
            ok = 0;
        }
    }
    if (ok) {
        INDEX_NAME_POOL old = app->name_pool;
        for (int i = 0; i < app->entry_count; i++)
            app->entries[i].name_offset = offsets[i];
        app->name_pool = compacted;
        app->name_pool_live_size = app->name_pool.size;
        memset(&compacted, 0, sizeof(compacted));
        index_name_pool_free(&old);
    }
    LeaveCriticalSection(&app->index_lock);
    free(offsets);
    index_name_pool_free(&compacted);
    return ok;
}

void index_free_entry(INDEX_ENTRY *entry)
{
    if (!entry)
        return;
    entry->name_offset = INDEX_NAME_OFFSET_NONE;
    entry->name_length = 0;
}

void index_clear_ref_index(APP_STATE *app)
{
    if (!app)
        return;
    free(app->ref_index_values);
    app->ref_index_values = NULL;
    app->ref_index_capacity = 0;
    app->ref_index_ready = 0;
}

int index_build_ref_index(APP_STATE *app)
{
    unsigned long long *entry_keys = NULL;
    int *values = NULL;
    int capacity = 1024;
    int ok = 1;
    int entry_count;
    LONG revision;
    
    if (!app)
        return 0;
    
    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    revision = app->index_revision;
    LeaveCriticalSection(&app->index_lock);

    while (capacity < entry_count * 2 && capacity < (1 << 30))
        capacity *= 2;
    entry_keys = (unsigned long long *)malloc(
        (size_t)(entry_count > 0 ? entry_count : 1) * sizeof(*entry_keys));
    values = (int *)malloc((size_t)capacity * sizeof(int));
    if (!entry_keys || !values) {
        free(entry_keys);
        free(values);
        return 0;
    }
    memset(values, 0xff, (size_t)capacity * sizeof(*values));

    EnterCriticalSection(&app->index_lock);
    if (app->index_revision != revision || app->entry_count != entry_count) {
        LeaveCriticalSection(&app->index_lock);
        free(entry_keys);
        free(values);
        return 0;
    }
    for (int i = 0; i < entry_count; i++) {
        INDEX_ENTRY *entry = &app->entries[i];
        entry_keys[i] = index_ref_key(entry->volume_index, entry->file_ref);
    }
    LeaveCriticalSection(&app->index_lock);

    for (int i = 0; i < entry_count; i++) {
        if (!index_ref_insert_snapshot(values, capacity, entry_keys,
                                       entry_count, entry_keys[i], i)) {
            ok = 0;
            break;
        }
    }

    EnterCriticalSection(&app->index_lock);
    if (ok && app->index_revision == revision && app->entry_count == entry_count) {
        index_clear_ref_index(app);
        app->ref_index_values = values;
        app->ref_index_capacity = capacity;
        app->ref_index_ready = 1;
        values = NULL;
    } else {
        ok = 0;
    }
    LeaveCriticalSection(&app->index_lock);
    free(entry_keys);
    free(values);
    return ok;
}

void index_clear_name_char_index(APP_STATE *app)
{
    if (!app)
        return;
    
    free(app->name_char_index_pool);
    app->name_char_index_pool = NULL;
    for (int i = 0; i < SEARCH_CHAR_SLOT_COUNT; i++) {
        app->name_char_indices[i] = NULL;
        app->name_char_counts[i] = 0;
    }
    app->name_char_index_ready = 0;
}

int index_build_name_char_index(APP_STATE *app)
{
    int counts[SEARCH_CHAR_SLOT_COUNT] = {0};
    int offsets[SEARCH_CHAR_SLOT_COUNT] = {0};
    int cursors[SEARCH_CHAR_SLOT_COUNT] = {0};
    int selected[SEARCH_CHAR_SLOT_COUNT] = {0};
    unsigned long long *masks = NULL;
    int *pool = NULL;
    size_t budget;
    size_t total = 0;
    int entry_count;
    LONG revision;

    if (!app)
        return 0;

    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    revision = app->index_revision;
    masks = (unsigned long long *)malloc(
        (size_t)(entry_count > 0 ? entry_count : 1) * sizeof(*masks));
    if (masks) {
        for (int i = 0; i < entry_count; i++) {
            const INDEX_ENTRY *entry = &app->entries[i];
            masks[i] = index_compute_char_mask_utf8(
                index_entry_name_utf8_locked(app, entry), entry->name_length);
        }
    }
    LeaveCriticalSection(&app->index_lock);
    if (!masks)
        return 0;

    for (int i = 0; i < entry_count; i++) {
        unsigned long long mask = masks[i];
        for (int slot = 0; slot < SEARCH_CHAR_SLOT_COUNT; slot++) {
            if (mask & (1ULL << slot))
                counts[slot]++;
        }
    }

    budget = (size_t)entry_count * 2;
    for (;;) {
        int best_slot = -1;
        int best_count = INT_MAX;
        for (int slot = 0; slot < SEARCH_CHAR_SLOT_COUNT; slot++) {
            if (!selected[slot] && counts[slot] < best_count) {
                best_count = counts[slot];
                best_slot = slot;
            }
        }
        if (best_slot < 0 || best_count > entry_count / 2 ||
            (size_t)best_count > budget - total)
            break;
        selected[best_slot] = 1;
        offsets[best_slot] = (int)total;
        total += (size_t)best_count;
    }

    if (total > 0) {
        pool = (int *)malloc(total * sizeof(*pool));
        if (!pool) {
            free(masks);
            return 0;
        }
    }
    for (int i = 0; i < entry_count; i++) {
        unsigned long long mask = masks[i];
        for (int slot = 0; slot < SEARCH_CHAR_SLOT_COUNT; slot++) {
            if (selected[slot] && (mask & (1ULL << slot)))
                pool[offsets[slot] + cursors[slot]++] = i;
        }
    }
    free(masks);

    EnterCriticalSection(&app->index_lock);
    if (app->entry_count != entry_count || app->index_revision != revision) {
        LeaveCriticalSection(&app->index_lock);
        free(pool);
        return 0;
    }
    index_clear_name_char_index(app);
    app->name_char_index_pool = pool;
    for (int slot = 0; slot < SEARCH_CHAR_SLOT_COUNT; slot++) {
        app->name_char_counts[slot] = selected[slot] ? counts[slot] : INT_MAX;
        app->name_char_indices[slot] = selected[slot] && counts[slot] > 0
            ? pool + offsets[slot] : NULL;
    }
    app->name_char_index_ready = 1;
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

void index_clear_filter_index(APP_STATE *app)
{
    if (!app)
        return;

    free(app->filter_index_pool);
    app->filter_index_pool = NULL;
    for (int filter = 0; filter < FILTER_COUNT; filter++) {
        app->filter_indices[filter] = NULL;
        app->filter_counts[filter] = 0;
    }
    app->filter_index_ready = 0;
}

static int index_compare_filter_indices(void *context,
                                        const void *lhs, const void *rhs)
{
    APP_STATE *app = (APP_STATE *)context;
    int left = *(const int *)lhs;
    int right = *(const int *)rhs;
    if (app->entries[left].is_directory != app->entries[right].is_directory)
        return app->entries[left].is_directory ? -1 : 1;
    return index_compare_entry_names_locked(
        app, &app->entries[left], &app->entries[right]);
}

static void index_sort_filter_indices(APP_STATE *app, int *indices, int count)
{
    int repairs = 0;

    for (int i = 1; i < count; i++) {
        int value;
        int lo;
        int hi;

        if (index_compare_filter_indices(app, &indices[i - 1], &indices[i]) <= 0)
            continue;
        if (++repairs > 64) {
            qsort_s(indices, count, sizeof(*indices),
                    index_compare_filter_indices, app);
            return;
        }

        value = indices[i];
        lo = 0;
        hi = i;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (index_compare_filter_indices(app, &indices[mid], &value) <= 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        memmove(indices + lo + 1, indices + lo,
                (size_t)(i - lo) * sizeof(*indices));
        indices[lo] = value;
    }
}

int index_build_filter_index(APP_STATE *app)
{
    int counts[FILTER_COUNT] = {0};
    int offsets[FILTER_COUNT] = {0};
    int cursors[FILTER_COUNT] = {0};
    unsigned char *filter_types = NULL;
    int *pool = NULL;
    int total = 0;
    int entry_count;
    LONG revision;

    if (!app)
        return 0;

    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    revision = app->index_revision;
    if (entry_count > 0) {
        filter_types = (unsigned char *)malloc((size_t)entry_count);
        if (!filter_types) {
            LeaveCriticalSection(&app->index_lock);
            free(filter_types);
            return 0;
        }
    }
    for (int i = 0; i < entry_count; i++) {
        int filter = app->entries[i].filter_type;
        filter_types[i] = (unsigned char)filter;
        if (filter > FILTER_EVERYTHING && filter < FILTER_COUNT)
            counts[filter]++;
    }
    LeaveCriticalSection(&app->index_lock);

    counts[FILTER_EVERYTHING] = entry_count;
    for (int filter = FILTER_AUDIO; filter < FILTER_COUNT; filter++) {
        offsets[filter] = total;
        cursors[filter] = total;
        total += counts[filter];
    }
    if (total > 0) {
        pool = (int *)malloc((size_t)total * sizeof(*pool));
        if (!pool) {
            free(filter_types);
            return 0;
        }
    }
    for (int i = 0; i < entry_count; i++) {
        int filter = filter_types[i];
        if (filter > FILTER_EVERYTHING && filter < FILTER_COUNT)
            pool[cursors[filter]++] = i;
    }
    free(filter_types);

    for (int filter = FILTER_AUDIO; filter < FILTER_COUNT; filter++) {
        if (counts[filter] > 1)
            index_sort_filter_indices(app, pool + offsets[filter], counts[filter]);
    }

    EnterCriticalSection(&app->index_lock);
    if (app->index_revision != revision || app->entry_count != entry_count) {
        LeaveCriticalSection(&app->index_lock);
        free(pool);
        return 0;
    }
    index_clear_filter_index(app);
    app->filter_index_pool = pool;
    app->filter_indices[FILTER_EVERYTHING] = NULL;
    app->filter_counts[FILTER_EVERYTHING] = entry_count;
    for (int filter = FILTER_AUDIO; filter < FILTER_COUNT; filter++) {
        app->filter_indices[filter] = counts[filter] > 0
            ? pool + offsets[filter] : NULL;
        app->filter_counts[filter] = counts[filter];
    }
    app->filter_index_ready = 1;
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

void index_clear(APP_STATE *app)
{
    EnterCriticalSection(&app->index_lock);
    
    index_clear_name_char_index(app);
    index_clear_filter_index(app);
    index_clear_ref_index(app);
    app->entry_count = 0;
    app->filtered_count = 0;
    app->filtered_identity = 0;
    app->filtered_stale = 0;
    index_name_pool_free(&app->name_pool);
    index_name_pool_init(&app->name_pool);
    app->name_pool_live_size = 0;
    InterlockedIncrement(&app->index_revision);
    
    LeaveCriticalSection(&app->index_lock);
}

int index_add_entry(APP_STATE *app, INDEX_ENTRY *entry, const wchar_t *name)
{
    size_t old_name_size;

    if (!app || !entry)
        return 0;
    EnterCriticalSection(&app->index_lock);
    old_name_size = app->name_pool.size;
    entry->name_offset = INDEX_NAME_OFFSET_NONE;
    entry->name_length = 0;
    if (!index_set_entry_name_locked(app, entry, name ? name : L"",
                                     name ? wcslen(name) : 0)) {
        LeaveCriticalSection(&app->index_lock);
        return 0;
    }
    
    if (app->entry_count >= app->entry_capacity) {
        if (!index_ensure_capacity_locked(app, app->entry_count + 1)) {
            app->name_pool.size = old_name_size;
            entry->name_offset = INDEX_NAME_OFFSET_NONE;
            entry->name_length = 0;
            LeaveCriticalSection(&app->index_lock);
            return 0;
        }
    }
    
    int idx = app->entry_count;
    index_prepare_entry(app, entry);
    app->entries[idx] = *entry;
    app->entry_count++;
    app->name_pool_live_size += (size_t)entry->name_length + 1;
    index_clear_name_char_index(app);
    index_clear_filter_index(app);
    if (app->ref_index_ready &&
        !index_ref_insert_locked(app,
                                 index_ref_key(entry->volume_index, entry->file_ref),
                                 idx))
        index_clear_ref_index(app);
    InterlockedIncrement(&app->index_revision);
    
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

int index_add_entries(APP_STATE *app, INDEX_BUILD *build)
{
    unsigned int base_offset;
    size_t merged_size;

    if (!app || !build)
        return 0;
    if (build->count <= 0)
        return 1;
    if (!build->entries || build->count > build->capacity ||
        app->entry_count > INT_MAX - build->count ||
        app->name_pool.size > UINT32_MAX ||
        build->names.size > UINT32_MAX - app->name_pool.size) {
        return 0;
    }
    
    EnterCriticalSection(&app->index_lock);
    for (int i = 0; i < build->count; i++) {
        const INDEX_ENTRY *entry = &build->entries[i];
        if (!build->names.data || entry->name_offset == INDEX_NAME_OFFSET_NONE ||
            entry->name_offset > build->names.size ||
            (size_t)entry->name_length >= build->names.size - entry->name_offset ||
            build->names.data[entry->name_offset + entry->name_length] != '\0') {
            LeaveCriticalSection(&app->index_lock);
            return 0;
        }
    }
    if (!index_ensure_capacity_locked(app, app->entry_count + build->count)) {
        LeaveCriticalSection(&app->index_lock);
        return 0;
    }
    merged_size = app->name_pool.size + build->names.size;
    if (!index_name_pool_reserve(&app->name_pool, merged_size)) {
        LeaveCriticalSection(&app->index_lock);
        return 0;
    }
    base_offset = (unsigned int)app->name_pool.size;
    memcpy(app->name_pool.data + app->name_pool.size,
           build->names.data, build->names.size);
    app->name_pool.size = merged_size;
    for (int i = 0; i < build->count; i++) {
        INDEX_ENTRY entry = build->entries[i];
        entry.name_offset += base_offset;
        index_prepare_entry(app, &entry);
        app->entries[app->entry_count + i] = entry;
    }
    app->entry_count += build->count;
    app->name_pool_live_size += build->names.size;
    index_clear_name_char_index(app);
    index_clear_filter_index(app);
    index_clear_ref_index(app);
    InterlockedIncrement(&app->index_revision);
    
    LeaveCriticalSection(&app->index_lock);
    return 1;
}

int index_apply_usn_changes(APP_STATE *app, USN_CHANGE *changes, int count)
{
    int applied = 0;
    int names_changed = 0;
    int compact_names = 0;
    int has_deletes = 0;
    int filtered_map_capacity = 0;
    int filtered_rows_removed = 0;
    int *filtered_positions = NULL;
    
    if (!changes || count <= 0)
        return 0;
    
    for (int i = 0; i < count; i++) {
        if (changes[i].reason & USN_REASON_FILE_DELETE) {
            has_deletes = 1;
            break;
        }
    }

    EnterCriticalSection(&app->index_lock);
    if (has_deletes && !app->filtered_identity && app->filtered_count > 0) {
        filtered_map_capacity = app->entry_count;
        filtered_positions = (int *)calloc(
            filtered_map_capacity > 0 ? filtered_map_capacity : 1, sizeof(int));
        if (filtered_positions) {
            for (int row = 0; row < app->filtered_count; row++) {
                int entry_index = app->filtered_indices[row];
                if (entry_index >= 0 && entry_index < filtered_map_capacity)
                    filtered_positions[entry_index] = row + 1;
            }
        }
    }
    
    for (int i = 0; i < count; i++) {
        USN_CHANGE *change = &changes[i];
        int idx = index_find_by_ref_locked(app, change->volume_index, change->file_ref);
        
        if (change->reason & USN_REASON_FILE_DELETE) {
            if (idx >= 0) {
                int last = app->entry_count - 1;
                int removed_row = 0;
                int moved_row = 0;
                if (filtered_positions) {
                    if (idx < filtered_map_capacity)
                        removed_row = filtered_positions[idx];
                    if (last < filtered_map_capacity)
                        moved_row = filtered_positions[last];
                    if (removed_row) {
                        app->filtered_indices[removed_row - 1] = -1;
                        filtered_rows_removed++;
                    }
                    if (idx != last && moved_row)
                        app->filtered_indices[moved_row - 1] = idx;
                    if (idx < filtered_map_capacity)
                        filtered_positions[idx] = idx != last ? moved_row : 0;
                    if (last < filtered_map_capacity)
                        filtered_positions[last] = 0;
                }
                index_ref_remove_locked(app, change->volume_index, change->file_ref);
                if (app->name_pool_live_size >=
                    (size_t)app->entries[idx].name_length + 1) {
                    app->name_pool_live_size -=
                        (size_t)app->entries[idx].name_length + 1;
                }
                index_free_entry(&app->entries[idx]);
                if (idx != last) {
                    INDEX_ENTRY *moved;
                    app->entries[idx] = app->entries[last];
                    moved = &app->entries[idx];
                    if (app->ref_index_ready) {
                        index_ref_insert_locked(
                            app,
                            index_ref_key(moved->volume_index, moved->file_ref),
                            idx);
                    }
                }
                memset(&app->entries[last], 0, sizeof(INDEX_ENTRY));
                app->entry_count--;
                applied++;
                names_changed = 1;
            }
            continue;
        }
        
        if (change->reason & USN_REASON_RENAME_OLD_NAME)
            continue;
        
        if (!(change->reason & (USN_REASON_FILE_CREATE |
                                USN_REASON_RENAME_NEW_NAME |
                                USN_REASON_BASIC_INFO_CHANGE |
                                USN_REASON_SECURITY_CHANGE |
                                USN_REASON_INDEXABLE_CHANGE))) {
            if (idx >= 0 &&
                (change->reason & (USN_REASON_DATA_OVERWRITE |
                                   USN_REASON_DATA_EXTEND |
                                   USN_REASON_DATA_TRUNCATION |
                                   USN_REASON_NAMED_DATA_OVERWRITE |
                                   USN_REASON_NAMED_DATA_EXTEND |
                                   USN_REASON_NAMED_DATA_TRUNCATION))) {
                INDEX_ENTRY *entry = &app->entries[idx];
                entry->metadata_loaded = 0;
                entry->size = 0;
                entry->creation_time = 0;
                entry->modification_time = 0;
                applied++;
            }
            continue;
        }
        
        if (idx >= 0 &&
            !(change->reason & (USN_REASON_FILE_CREATE |
                                USN_REASON_RENAME_NEW_NAME))) {
            INDEX_ENTRY *entry = &app->entries[idx];
            entry->attributes = change->attributes;
            entry->metadata_loaded = 0;
            entry->size = 0;
            entry->creation_time = 0;
            entry->modification_time = 0;
            applied++;
            continue;
        }
        
        if (!change->name || !change->name[0])
            continue;
        
        if (idx >= 0) {
            INDEX_ENTRY replacement;
            memset(&replacement, 0, sizeof(replacement));
            if (index_set_entry_from_change(app, &replacement, change)) {
                if (app->name_pool_live_size >=
                    (size_t)app->entries[idx].name_length + 1) {
                    app->name_pool_live_size -=
                        (size_t)app->entries[idx].name_length + 1;
                }
                index_free_entry(&app->entries[idx]);
                app->entries[idx] = replacement;
                app->name_pool_live_size +=
                    (size_t)replacement.name_length + 1;
                applied++;
                names_changed = 1;
            } else {
                index_free_entry(&replacement);
            }
        } else if (index_ensure_capacity_locked(app, app->entry_count + 1)) {
            INDEX_ENTRY *entry = &app->entries[app->entry_count];
            if (index_set_entry_from_change(app, entry, change)) {
                int new_index = app->entry_count;
                app->entry_count++;
                app->name_pool_live_size += (size_t)entry->name_length + 1;
                if (app->ref_index_ready &&
                    !index_ref_insert_locked(
                        app,
                        index_ref_key(entry->volume_index, entry->file_ref),
                        new_index)) {
                    index_clear_ref_index(app);
                }
                applied++;
                names_changed = 1;
            } else {
                index_free_entry(entry);
                memset(entry, 0, sizeof(*entry));
            }
        }
    }
    
    if (filtered_rows_removed > 0) {
        int write_row = 0;
        for (int read_row = 0; read_row < app->filtered_count; read_row++) {
            int entry_index = app->filtered_indices[read_row];
            if (entry_index >= 0)
                app->filtered_indices[write_row++] = entry_index;
        }
        app->filtered_count = write_row;
    }

    if (applied) {
        if (app->filtered_identity) {
            app->filtered_count = app->entry_count;
            app->filtered_stale = 0;
        } else {
            app->filtered_stale = 1;
        }
        if (names_changed)
            index_clear_name_char_index(app);
        if (names_changed)
            index_clear_filter_index(app);
        if (names_changed)
            InterlockedIncrement(&app->index_revision);
        if (names_changed && app->name_pool.size > app->name_pool_live_size) {
            size_t wasted = app->name_pool.size - app->name_pool_live_size;
            compact_names = wasted >= 16 * 1024 * 1024 &&
                            wasted >= app->name_pool_live_size / 2;
        }
    }
    
    LeaveCriticalSection(&app->index_lock);
    free(filtered_positions);
    if (compact_names)
        index_compact_entry_names(app);
    return applied;
}

void index_build_paths(APP_STATE *app)
{
    (void)app;
}

static int index_compare_entries_by_name(void *context,
                                         const void *lhs, const void *rhs)
{
    APP_STATE *app = (APP_STATE *)context;
    const INDEX_ENTRY *left = (const INDEX_ENTRY *)lhs;
    const INDEX_ENTRY *right = (const INDEX_ENTRY *)rhs;
    int result = index_compare_entry_names_locked(app, left, right);

    if (result == 0) {
        if (left->file_ref < right->file_ref)
            return -1;
        if (left->file_ref > right->file_ref)
            return 1;
    }
    return result;
}

void index_sort_entries_by_name(APP_STATE *app)
{
    EnterCriticalSection(&app->index_lock);
    if (app->entry_count > 1) {
        index_clear_name_char_index(app);
        index_clear_filter_index(app);
        index_clear_ref_index(app);
        qsort_s(app->entries, app->entry_count, sizeof(INDEX_ENTRY),
                index_compare_entries_by_name, app);

        /* Reordering the array invalidates every stored position. Without this
           the current result set keeps its pre-sort indices and the list shows
           rows belonging to entirely different files. */
        app->filtered_count = 0;
        app->filtered_identity = 0;
        app->filtered_stale = 1;

        InterlockedIncrement(&app->index_revision);
        InterlockedIncrement(&app->search_generation);
    }
    LeaveCriticalSection(&app->index_lock);
}
