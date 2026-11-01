#include "lang.h"

/* Runtime UI localisation.
 *
 * English text is the lookup key: lang_tr(L"Open") returns the current
 * language's translation, or L"Open" itself when the string is not translated.
 * The two built-in languages (en and zh-CN) always exist, so the UI keeps
 * working when the optional external file is missing, empty or corrupt.
 *
 * External file: %LOCALAPPDATA%\OpenEverything\languages.ini
 * It may add languages (a non-[Languages] section with key=value lines) and
 * may override individual built-in strings for en / zh-CN.
 *
 * Storage is deliberately simple: a fixed array of languages (capped by
 * LANG_MAX_LANGUAGES) and, per language, a small linear array of string
 * pairs.  The UI string table is only a few hundred entries, so a linear scan
 * is cheap and keeps the "later definition wins" override order obvious.
 */

#define LANG_MAX_FILE_BYTES (1024u * 1024u) /* 1 MB is plenty for a string table */

#define LANG_BUILTIN_COUNT \
    ((int)(sizeof(g_builtin_languages) / sizeof(g_builtin_languages[0])))

typedef struct {
    const wchar_t *key;
    const wchar_t *value;
} LANG_BUILTIN_STRING;

typedef struct {
    wchar_t *key;
    wchar_t *value;
} LANG_PAIR;

typedef struct {
    LANG_ENTRY entry;
    const LANG_BUILTIN_STRING *builtin_pairs;
    int builtin_pair_count;
    LANG_PAIR *pairs; /* external definitions / built-in overrides */
    int pair_count;
    int pair_capacity;
} LANG_DATA;

typedef struct {
    wchar_t *id;
    wchar_t *name;
} LANG_ALIAS;

typedef struct {
    LANG_ALIAS *items;
    int count;
    int capacity;
} LANG_ALIAS_LIST;

/* =============================================================
 * Built-in data
 * ============================================================= */

/* Simplified Chinese strings.  Keys are the exact English UI strings the
 * application passes to L("..."); tabs and printf-style escapes in them are
 * part of the key and must not be changed. */
static const LANG_BUILTIN_STRING g_builtin_zh_cn[] = {
    { L"File", L"文件" },
    { L"Edit", L"编辑" },
    { L"View", L"视图" },
    { L"Search", L"搜索" },
    { L"Index", L"索引" },
    { L"Help", L"帮助" },

    { L"New Search Window\tCtrl+N", L"新建搜索窗口\tCtrl+N" },
    { L"Open\tEnter", L"打开\tEnter" },
    { L"Open Path\tCtrl+Enter", L"打开路径\tCtrl+Enter" },
    { L"Copy Full Name to Clipboard\tCtrl+Shift+C", L"复制完整名称到剪贴板\tCtrl+Shift+C" },
    { L"Delete\tDel", L"删除\tDel" },
    { L"Rename\tF2", L"重命名\tF2" },
    { L"Properties\tAlt+Enter", L"属性\tAlt+Enter" },
    { L"Exit\tAlt+F4", L"退出\tAlt+F4" },
    { L"Copy\tCtrl+C", L"复制\tCtrl+C" },
    { L"Copy Full Path\tCtrl+Shift+C", L"复制完整路径\tCtrl+Shift+C" },
    { L"Select All\tCtrl+A", L"全选\tCtrl+A" },
    { L"Refresh\tF5", L"刷新\tF5" },

    { L"Folders", L"文件夹" },
    { L"Filters", L"筛选器" },
    { L"Status Bar", L"状态栏" },
    { L"Themes", L"主题" },
    { L"Light", L"浅色" },
    { L"Dark", L"深色" },
    { L"System", L"跟随系统" },

    { L"Match Case", L"区分大小写" },
    { L"Match Whole Word", L"全字匹配" },
    { L"Match Path", L"匹配路径" },
    { L"Use Regex", L"使用正则表达式" },

    { L"Sort By", L"排序方式" },
    { L"Name", L"名称" },
    { L"Path", L"路径" },
    { L"Size", L"大小" },
    { L"Date Modified", L"修改日期" },
    { L"Date Created", L"创建日期" },
    { L"Attributes", L"特性" },
    { L"Extension", L"扩展名" },
    { L"Ascending", L"升序" },
    { L"Descending", L"降序" },

    { L"Details", L"详细信息" },
    { L"Medium Thumbnails", L"中等缩略图" },
    { L"Large Thumbnails", L"大缩略图" },
    { L"Extra Large Thumbnails", L"超大缩略图" },

    { L"Language", L"语言" },
    { L"Choose Language...", L"选择语言..." },
    { L"Command Line Options", L"命令行选项" },
    { L"About OpenEverything", L"关于 OpenEverything" },
    { L"Add to Bookmarks...", L"添加到书签..." },
    { L"Organize Filters...", L"管理筛选器..." },
    { L"Update Index", L"更新索引" },
    { L"Rebuild Index", L"重建索引" },

    { L"Close to Tray", L"关闭时最小化到托盘" },
    { L"Minimize to Tray", L"最小化到托盘" },
    { L"Tray", L"托盘" },
    { L"Open OpenEverything", L"打开 OpenEverything" },
    { L"Show Window", L"显示窗口" },
    { L"Hide Window", L"隐藏窗口" },
    { L"Exit", L"退出" },

    { L"Searching...", L"正在搜索..." },
    { L"Loading index cache...", L"正在加载索引缓存..." },
    { L"%s objects", L"%s 个对象" },
    { L"%s objects (%s total)", L"%s 个对象（共 %s 个）" },
    { L"0 objects - unable to read NTFS volumes; try running as Administrator",
      L"0 个对象 - 无法读取 NTFS 卷；请尝试以管理员身份运行" },
    { L"Indexing %s %d%%", L"正在索引 %s %d%%" },
    { L"Indexing %s", L"正在索引 %s" },
    { L"Indexing %s (compatibility scan) %d%%", L"正在索引 %s（兼容性扫描）%d%%" },
    { L"Sorting file names...", L"正在排序文件名..." },
    { L"Building search index...", L"正在构建搜索索引..." },
    { L"Saving index...", L"正在保存索引..." },
    { L"Indexing...", L"正在建立索引..." },
    { L"  Indexing...", L"  正在建立索引..." },
    { L"  Indexing already in progress", L"  索引已在进行中" },
    { L"  Failed to start indexing", L"  无法启动索引" },

    { L"Everything", L"所有文件" },
    { L"Audio", L"音频" },
    { L"Compressed", L"压缩文件" },
    { L"Document", L"文档" },
    { L"Executable", L"可执行文件" },
    { L"Folder", L"文件夹" },
    { L"Image", L"图像" },
    { L"Video", L"视频" },
    { L"Subfolders", L"包含子文件夹" },

    { L"Delete", L"删除" },
    { L"Delete \"%s\"?", L"确定删除“%s”？" },
    { L"Delete failed", L"删除失败" },
    { L"Rename", L"重命名" },
    { L"New name:", L"新名称：" },
    { L"OK", L"确定" },
    { L"Cancel", L"取消" },
    { L"Close", L"关闭" },
    { L"Error", L"错误" },
    { L"Failed to rename:", L"重命名失败：" },
    { L"The new name contains invalid characters.", L"新名称包含无效字符。" },
    { L"A file with that name already exists.", L"该名称的文件已存在。" },
    { L"Set Run Count is not implemented yet.", L"“设置运行次数”尚未实现。" },
    { L"OpenEverything is still running in the notification area.", L"OpenEverything 仍在通知区域中运行。" },
    { L"Failed to create a new search window.", L"无法创建新的搜索窗口。" },
    { L"Failed to open the selected item.", L"无法打开所选项目。" },
    { L"Select a language:", L"选择语言：" },
    { L"Reload Language File", L"重新加载语言文件" },

    /* Multi-line About text.  The key itself contains newlines; the external
     * language file can override it with \n escape sequences. */
    { L"OpenEverything v%s\n\nAn open-source Everything clone.\n\nHow it works:\nA fast file-name index is built from the NTFS MFT/USN Journal,\nlocal caching avoids a full rebuild on every start,\nand searching runs on a background thread with a virtual list.\n\nProject:\nhttps://github.com/DisaWdcba",
      L"OpenEverything v%s\n\nEverything 的开源复刻版本。\n\n原理介绍：\n通过 NTFS MFT/USN Journal 快速建立文件名索引，\n使用本地缓存避免每次启动全量重建，\n搜索阶段使用后台线程和虚拟列表保持界面响应。\n\n项目主页：\nhttps://github.com/DisaWdcba" },

    /* A few plain variants of translated strings above (some UI elements use
     * the text without the keyboard-shortcut suffix). */
    { L"New Search Window", L"新建搜索窗口" },
    { L"Open", L"打开" },
    { L"Open Path", L"打开路径" },
    { L"Copy Full Name to Clipboard", L"复制完整名称到剪贴板" },
    { L"Copy Full Path", L"复制完整路径" },
    { L"Copy", L"复制" },
    { L"Select All", L"全选" },
    { L"Properties", L"属性" },
    { L"Refresh", L"刷新" }
};

static const LANG_ENTRY g_builtin_languages[] = {
    { L"en", L"English", 1 },
    { L"zh-CN", L"简体中文", 1 }
};

static const LANG_BUILTIN_STRING *const g_builtin_tables[] = {
    NULL,
    g_builtin_zh_cn
};

static const int g_builtin_table_counts[] = {
    0,
    (int)(sizeof(g_builtin_zh_cn) / sizeof(g_builtin_zh_cn[0]))
};

/* =============================================================
 * State
 * ============================================================= */

static LANG_DATA g_languages[LANG_MAX_LANGUAGES];
static int g_language_count;
static int g_builtin_count;
static int g_current_language;
static int g_external_loaded;
static int g_initialized;

/* =============================================================
 * Small string helpers
 * ============================================================= */

static wchar_t *lang_dup(const wchar_t *text)
{
    size_t length;
    wchar_t *copy;

    if (!text)
        return NULL;
    length = wcslen(text);
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (!copy)
        return NULL;
    memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    return copy;
}

static void lang_free_pairs(LANG_DATA *language)
{
    int index;

    for (index = 0; index < language->pair_count; index++) {
        free(language->pairs[index].key);
        free(language->pairs[index].value);
    }
    free(language->pairs);
    language->pairs = NULL;
    language->pair_count = 0;
    language->pair_capacity = 0;
}

static int lang_find_index(const wchar_t *id)
{
    int index;

    if (!id || !*id)
        return -1;
    for (index = 0; index < g_language_count; index++) {
        if (_wcsicmp(g_languages[index].entry.id, id) == 0)
            return index;
    }
    return -1;
}

/* Find or create a language.  External language definitions are created on
 * first use; en / zh-CN already exist and are extended in place. */
static int lang_ensure_language(const wchar_t *id)
{
    wchar_t truncated[LANG_ID_MAX];
    size_t length;
    int index;

    if (!id || !*id)
        return -1;
    length = wcslen(id);
    if (length >= LANG_ID_MAX)
        length = LANG_ID_MAX - 1;
    wmemcpy(truncated, id, length);
    truncated[length] = L'\0';

    index = lang_find_index(truncated);
    if (index >= 0)
        return index;
    if (g_language_count >= LANG_MAX_LANGUAGES)
        return -1;

    index = g_language_count;
    ZeroMemory(&g_languages[index], sizeof(g_languages[index]));
    wmemcpy(g_languages[index].entry.id, truncated, length + 1);
    wmemcpy(g_languages[index].entry.native_name, truncated, length + 1);
    g_languages[index].entry.builtin = 0;
    g_language_count++;
    return index;
}

static int lang_add_pair(LANG_DATA *language, const wchar_t *key,
                         const wchar_t *value)
{
    wchar_t *key_copy;
    wchar_t *value_copy;
    LANG_PAIR *grown;
    int capacity;

    if (!language || !key || !*key || !value)
        return 0;

    if (language->pair_count >= language->pair_capacity) {
        capacity = language->pair_capacity ? language->pair_capacity * 2 : 16;
        grown = (LANG_PAIR *)realloc(language->pairs,
                                     (size_t)capacity * sizeof(LANG_PAIR));
        if (!grown)
            return 0;
        language->pairs = grown;
        language->pair_capacity = capacity;
    }

    key_copy = lang_dup(key);
    value_copy = lang_dup(value);
    if (!key_copy || !value_copy) {
        free(key_copy);
        free(value_copy);
        return 0;
    }
    language->pairs[language->pair_count].key = key_copy;
    language->pairs[language->pair_count].value = value_copy;
    language->pair_count++;
    return 1;
}

/* =============================================================
 * [Languages] aliases
 * ============================================================= */

static void lang_alias_add(LANG_ALIAS_LIST *list, const wchar_t *id,
                           const wchar_t *name)
{
    LANG_ALIAS *grown;
    wchar_t *id_copy;
    wchar_t *name_copy;
    int capacity;

    if (!list || !id || !*id || !name || !*name)
        return;

    if (list->count >= list->capacity) {
        capacity = list->capacity ? list->capacity * 2 : 16;
        grown = (LANG_ALIAS *)realloc(list->items,
                                      (size_t)capacity * sizeof(LANG_ALIAS));
        if (!grown)
            return;
        list->items = grown;
        list->capacity = capacity;
    }

    id_copy = lang_dup(id);
    name_copy = lang_dup(name);
    if (!id_copy || !name_copy) {
        free(id_copy);
        free(name_copy);
        return;
    }
    list->items[list->count].id = id_copy;
    list->items[list->count].name = name_copy;
    list->count++;
}

static void lang_alias_free(LANG_ALIAS_LIST *list)
{
    int index;

    for (index = 0; index < list->count; index++) {
        free(list->items[index].id);
        free(list->items[index].name);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Native names are only a display label; applying them after the whole file
 * has been parsed makes the order of [Languages] and language sections
 * irrelevant. */
static void lang_apply_aliases(const LANG_ALIAS_LIST *list)
{
    size_t length;
    int index;
    int language;

    for (index = 0; index < list->count; index++) {
        language = lang_find_index(list->items[index].id);
        if (language < 0)
            continue;
        length = wcslen(list->items[index].name);
        if (length >= LANG_NAME_MAX)
            length = LANG_NAME_MAX - 1;
        wmemcpy(g_languages[language].entry.native_name, list->items[index].name,
                length);
        g_languages[language].entry.native_name[length] = L'\0';
    }
}

/* =============================================================
 * INI text helpers
 * ============================================================= */

static wchar_t *lang_trim(wchar_t *text)
{
    wchar_t *end;

    while (*text == L' ' || *text == L'\t')
        text++;
    end = text + wcslen(text);
    while (end > text && (end[-1] == L' ' || end[-1] == L'\t'))
        end--;
    *end = L'\0';
    return text;
}

/* Expand the escapes the format documents: \n, \t and \\.  Any other
 * backslash sequence is copied through unchanged.  Escapes are expanded after
 * trimming so a value can start or end with escaped whitespace. */
static void lang_unescape(wchar_t *text)
{
    wchar_t *read_cursor;
    wchar_t *write_cursor;
    wchar_t next;

    read_cursor = text;
    write_cursor = text;
    while (*read_cursor) {
        if (*read_cursor == L'\\' && read_cursor[1]) {
            next = read_cursor[1];
            if (next == L'n') {
                *write_cursor++ = L'\n';
                read_cursor += 2;
                continue;
            }
            if (next == L't') {
                *write_cursor++ = L'\t';
                read_cursor += 2;
                continue;
            }
            if (next == L'\\') {
                *write_cursor++ = L'\\';
                read_cursor += 2;
                continue;
            }
        }
        *write_cursor++ = *read_cursor++;
    }
    *write_cursor = L'\0';
}

/* =============================================================
 * File decoding
 * ============================================================= */

/* Drop a partial UTF-8 sequence cut off by the size cap.  Only used when the
 * file was bigger than LANG_MAX_FILE_BYTES. */
static DWORD lang_utf8_safe_length(const unsigned char *bytes, DWORD length)
{
    DWORD index;
    unsigned int continuation;
    unsigned int needed;
    unsigned char lead;

    index = length;
    continuation = 0;
    while (index > 0 && continuation < 3 &&
           (bytes[index - 1] & 0xC0u) == 0x80u) {
        index--;
        continuation++;
    }
    if (index == 0)
        return length;

    lead = bytes[index - 1];
    if ((lead & 0x80u) == 0)
        needed = 1;
    else if ((lead & 0xE0u) == 0xC0u)
        needed = 2;
    else if ((lead & 0xF0u) == 0xE0u)
        needed = 3;
    else if ((lead & 0xF8u) == 0xF0u)
        needed = 4;
    else
        return length; /* invalid lead byte; let the decoder decide */

    if (continuation + 1 >= needed)
        return length;
    return index - 1;
}

static wchar_t *lang_decode_utf8(const unsigned char *bytes, DWORD length)
{
    wchar_t *text;
    int characters;

    if (length == 0) {
        text = (wchar_t *)malloc(sizeof(wchar_t));
        if (text)
            text[0] = L'\0';
        return text;
    }

    characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     (const char *)bytes, (int)length, NULL, 0);
    if (characters > 0) {
        text = (wchar_t *)malloc(((size_t)characters + 1) * sizeof(wchar_t));
        if (!text)
            return NULL;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char *)bytes, (int)length, text,
                                characters) != characters) {
            free(text);
            return NULL;
        }
        text[characters] = L'\0';
        return text;
    }

    /* No BOM and not valid UTF-8: treat the file as the ANSI code page
     * (e.g. GBK on a Chinese system) instead of failing silently. */
    characters = MultiByteToWideChar(CP_ACP, 0, (const char *)bytes,
                                     (int)length, NULL, 0);
    if (characters <= 0)
        return NULL;
    text = (wchar_t *)malloc(((size_t)characters + 1) * sizeof(wchar_t));
    if (!text)
        return NULL;
    if (MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, (int)length, text,
                            characters) != characters) {
        free(text);
        return NULL;
    }
    text[characters] = L'\0';
    return text;
}

static wchar_t *lang_decode_utf16(const unsigned char *bytes, DWORD length,
                                  int big_endian)
{
    size_t characters;
    size_t index;
    wchar_t *text;

    if (length & 1u)
        length--; /* ignore a dangling odd byte */
    characters = (size_t)(length / 2);
    text = (wchar_t *)malloc((characters + 1) * sizeof(wchar_t));
    if (!text)
        return NULL;
    for (index = 0; index < characters; index++) {
        unsigned int low = bytes[index * 2];
        unsigned int high = bytes[index * 2 + 1];
        unsigned int code = big_endian ? ((low << 8) | high)
                                       : ((high << 8) | low);
        text[index] = (wchar_t)code;
    }
    text[characters] = L'\0';
    return text;
}

static wchar_t *lang_decode_file(const unsigned char *bytes, DWORD length,
                                 int truncated)
{
    if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
        bytes[2] == 0xBF) {
        return lang_decode_utf8(bytes + 3, length - 3);
    }
    if (length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        return lang_decode_utf16(bytes + 2, length - 2, 0);
    if (length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
        return lang_decode_utf16(bytes + 2, length - 2, 1);

    if (truncated)
        length = lang_utf8_safe_length(bytes, length);
    return lang_decode_utf8(bytes, length);
}

/* =============================================================
 * Parsing
 * ============================================================= */

enum {
    LANG_SECTION_NONE = 0,
    LANG_SECTION_LANGUAGES,
    LANG_SECTION_LANGUAGE
};

/* Parse the decoded file in place.  Returns the number of external languages
 * that exist after parsing.  Lines are split on CRLF / LF / CR; section names
 * and keys are trimmed and compared case-insensitively. */
static int lang_process_file_text(wchar_t *text)
{
    LANG_ALIAS_LIST aliases;
    wchar_t *cursor;
    wchar_t *section_id;
    int section_language;
    int mode;
    int external;

    ZeroMemory(&aliases, sizeof(aliases));
    section_id = NULL;
    section_language = -1;
    mode = LANG_SECTION_NONE;
    cursor = text;

    while (*cursor) {
        wchar_t *line;
        wchar_t *delimiter;
        wchar_t *equals;
        wchar_t *key;
        wchar_t *value;
        wchar_t newline;

        line = cursor;
        while (*cursor && *cursor != L'\r' && *cursor != L'\n')
            cursor++;
        if (*cursor) {
            newline = *cursor;
            *cursor = L'\0';
            cursor++;
            if (newline == L'\r' && *cursor == L'\n')
                cursor++;
        }

        line = lang_trim(line);
        if (!*line || *line == L';' || *line == L'#')
            continue;

        if (*line == L'[') {
            delimiter = wcschr(line + 1, L']');
            if (!delimiter) {
                mode = LANG_SECTION_NONE;
                section_id = NULL;
                section_language = -1;
                continue;
            }
            *delimiter = L'\0';
            section_id = lang_trim(line + 1);
            section_language = -1;
            if (_wcsicmp(section_id, L"Languages") == 0)
                mode = LANG_SECTION_LANGUAGES;
            else if (*section_id)
                mode = LANG_SECTION_LANGUAGE;
            else
                mode = LANG_SECTION_NONE;
            continue;
        }

        equals = wcschr(line, L'=');
        if (!equals)
            continue;
        *equals = L'\0';
        key = lang_trim(line);
        value = lang_trim(equals + 1);
        if (!*key)
            continue;
        lang_unescape(key);
        lang_unescape(value);
        if (!*key)
            continue;

        if (mode == LANG_SECTION_LANGUAGES) {
            lang_alias_add(&aliases, key, value);
        } else if (mode == LANG_SECTION_LANGUAGE) {
            if (section_language < 0)
                section_language = lang_ensure_language(section_id);
            if (section_language >= 0)
                lang_add_pair(&g_languages[section_language], key, value);
        }
    }

    lang_apply_aliases(&aliases);
    lang_alias_free(&aliases);

    external = g_language_count - g_builtin_count;
    return external;
}

/* =============================================================
 * External file
 * ============================================================= */

void lang_get_file_path(wchar_t *buf, size_t size)
{
    wchar_t appdata[MAX_PATH];
    DWORD override_length;

    if (!buf || size == 0)
        return;

    /* Portable/testing override; the normal per-user path stays the default. */
    override_length = GetEnvironmentVariableW(
        L"OPEN_EVERYTHING_LANGUAGE_FILE", buf, (DWORD)size);
    if (override_length > 0 && override_length < (DWORD)size)
        return;

    /* _snwprintf_s truncates instead of invoking the invalid parameter handler
     * when the joined path is longer than the caller's buffer. */
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0,
                                   appdata))) {
        _snwprintf_s(buf, size, _TRUNCATE,
                     L"%s\\OpenEverything\\languages.ini", appdata);
    } else {
        wcsncpy_s(buf, size, L"languages.ini", _TRUNCATE);
    }
}

/* Read and merge the external language file.  Returns the number of external
 * languages present afterwards; missing / unreadable files simply leave the
 * built-in languages untouched. */
static int lang_load_file(void)
{
    wchar_t path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER file_size;
    unsigned long long wanted;
    unsigned char *bytes;
    DWORD total;
    wchar_t *text;
    int truncated;
    int external;

    g_external_loaded = 0;
    lang_get_file_path(path, MAX_PATH);

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    if (!GetFileSizeEx(file, &file_size)) {
        CloseHandle(file);
        return 0;
    }
    if (file_size.QuadPart <= 0) {
        g_external_loaded = 1;
        CloseHandle(file);
        return 0;
    }

    wanted = (unsigned long long)file_size.QuadPart;
    truncated = 0;
    if (wanted > LANG_MAX_FILE_BYTES) {
        wanted = LANG_MAX_FILE_BYTES;
        truncated = 1;
    }

    bytes = (unsigned char *)malloc((size_t)wanted + 1);
    if (!bytes) {
        CloseHandle(file);
        return 0;
    }

    total = 0;
    while (total < (DWORD)wanted) {
        DWORD got = 0;
        if (!ReadFile(file, bytes + total, (DWORD)wanted - total, &got, NULL)) {
            free(bytes);
            CloseHandle(file);
            return 0;
        }
        if (got == 0)
            break;
        total += got;
    }
    CloseHandle(file);

    g_external_loaded = 1;
    if (total == 0) {
        free(bytes);
        return 0;
    }
    bytes[total] = 0;

    text = lang_decode_file(bytes, total, truncated);
    free(bytes);
    if (!text)
        return 0;

    external = lang_process_file_text(text);
    free(text);
    return external;
}

/* =============================================================
 * Lifecycle
 * ============================================================= */

/* Drop every external language and every external override, then restore the
 * built-ins to their pristine state. */
static void lang_reset_languages(void)
{
    int index;

    for (index = 0; index < g_language_count; index++)
        lang_free_pairs(&g_languages[index]);

    g_language_count = g_builtin_count;
    for (index = 0; index < g_builtin_count; index++) {
        g_languages[index].entry = g_builtin_languages[index];
        g_languages[index].builtin_pairs = g_builtin_tables[index];
        g_languages[index].builtin_pair_count = g_builtin_table_counts[index];
    }
}

void lang_init(void)
{
    int index;

    if (g_initialized)
        return;
    g_initialized = 1;

    ZeroMemory(g_languages, sizeof(g_languages));
    g_builtin_count = LANG_BUILTIN_COUNT;
    g_language_count = g_builtin_count;
    for (index = 0; index < g_builtin_count; index++) {
        g_languages[index].entry = g_builtin_languages[index];
        g_languages[index].builtin_pairs = g_builtin_tables[index];
        g_languages[index].builtin_pair_count = g_builtin_table_counts[index];
    }

    g_current_language = 0;
    g_external_loaded = 0;
    /* The UI is localised out of the box: start with the language that
     * matches the Windows display language.  lang_system_default() only ever
     * returns a built-in language, so this cannot fail. */
    g_current_language = lang_system_default();
    if (g_current_language < 0)
        g_current_language = 0;
}

int lang_reload(void)
{
    wchar_t keep_id[LANG_ID_MAX];
    int keep_valid;
    int index;
    int external;

    lang_init();

    keep_valid = 0;
    if (g_current_language >= 0 && g_current_language < g_language_count) {
        wcsncpy_s(keep_id, LANG_ID_MAX, g_languages[g_current_language].entry.id,
                  _TRUNCATE);
        keep_valid = 1;
    }

    lang_reset_languages();
    external = lang_load_file();

    index = -1;
    if (keep_valid)
        index = lang_find_index(keep_id);
    if (index < 0)
        index = lang_find_index(L"en");
    if (index < 0 && g_language_count > 0)
        index = 0;
    g_current_language = (index >= 0) ? index : 0;

    return external;
}

/* =============================================================
 * Public API
 * ============================================================= */

int lang_count(void)
{
    lang_init();
    return g_language_count;
}

const LANG_ENTRY *lang_entry(int index)
{
    lang_init();
    if (index < 0 || index >= g_language_count)
        return NULL;
    return &g_languages[index].entry;
}

int lang_find(const wchar_t *id)
{
    lang_init();
    return lang_find_index(id);
}

int lang_set(int index)
{
    lang_init();
    if (index < 0 || index >= g_language_count)
        return 0;
    g_current_language = index;
    return 1;
}

int lang_current(void)
{
    lang_init();
    if (g_current_language < 0 || g_current_language >= g_language_count)
        return 0;
    return g_current_language;
}

const wchar_t *lang_current_id(void)
{
    lang_init();
    if (g_current_language < 0 || g_current_language >= g_language_count)
        return L"";
    return g_languages[g_current_language].entry.id;
}

int lang_system_default(void)
{
    LANGID ui_language;

    lang_init();

    ui_language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(ui_language) == LANG_CHINESE &&
        SUBLANGID(ui_language) == SUBLANG_CHINESE_SIMPLIFIED) {
        int zh_index = lang_find_index(L"zh-CN");
        if (zh_index >= 0)
            return zh_index;
    }

    {
        int en_index = lang_find_index(L"en");
        if (en_index >= 0)
            return en_index;
    }
    return (g_language_count > 0) ? 0 : -1;
}

const wchar_t *lang_tr(const wchar_t *english)
{
    LANG_DATA *language;
    int index;

    if (!english)
        return L"";

    lang_init();
    if (g_current_language < 0 || g_current_language >= g_language_count)
        return english;

    language = &g_languages[g_current_language];

    /* Later definitions win, so scan the external pairs backwards. */
    for (index = language->pair_count - 1; index >= 0; index--) {
        if (_wcsicmp(language->pairs[index].key, english) == 0)
            return language->pairs[index].value;
    }
    for (index = 0; index < language->builtin_pair_count; index++) {
        if (_wcsicmp(language->builtin_pairs[index].key, english) == 0)
            return language->builtin_pairs[index].value;
    }
    return english;
}

int lang_external_loaded(void)
{
    lang_init();
    return g_external_loaded;
}
