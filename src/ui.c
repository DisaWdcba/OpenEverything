#include "ui.h"
#include "config.h"
#include "ipc.h"
#include "ntfs.h"
#include "search.h"
#include "index.h"
#include "cache.h"
#include "resource.h"

/* Window procedure */
static LRESULT CALLBACK everything_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK search_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclass_id, DWORD_PTR ref_data);

static HINSTANCE g_hInst;
static HWND g_hwndSearch;
static HWND g_hwndList;
static HWND g_hwndStatus;
static APP_STATE *g_app_ptr;
static HFONT g_font_ui;
static HFONT g_font_search;
static HBRUSH g_brush_window;
static int g_icon_file = I_IMAGENONE;
static int g_icon_folder = I_IMAGENONE;
static HANDLE g_cache_thread;
static IContextMenu2 *g_shell_menu2;
static IContextMenu3 *g_shell_menu3;
static volatile LONG g_reindexing = 0;
static volatile LONG g_search_running = 0;
static volatile LONG g_cache_loading = 0;

typedef struct {
    wchar_t *extension;
    int icon_index;
} ICON_CACHE_ENTRY;

static ICON_CACHE_ENTRY *g_icon_cache;
static int g_icon_cache_count;
static int g_icon_cache_capacity;

#define UI_SEARCH_TOP 4
#define UI_SEARCH_HEIGHT 36
#define IDT_SEARCH_DEBOUNCE 1
#define IDT_STARTUP_SYNC 2
#define SEARCH_DEBOUNCE_MS 120
#define STARTUP_SYNC_DELAY_MS 2500
#define STARTUP_SYNC_MAX_CHANGES 16
#define IDM_CTX_OPEN 20001
#define IDM_CTX_OPEN_PATH 20002
#define IDM_CTX_COPY_FULL_NAME 20003
#define IDM_CTX_SET_RUN_COUNT 20004
#define IDM_CTX_SHELL_FIRST 21000
#define IDM_CTX_SHELL_LAST 24000
struct SearchJob {
    APP_STATE *app;
    HWND hwnd;
    SEARCH_QUERY query;
    LONG generation;
    int max_results;
    int result_count;
    int *results;
    int *base_indices;
    int base_count;
    int base_identity;
};

struct UsnStartupSyncCtx {
    APP_STATE *app;
    HWND hwnd;
};

struct CacheLoadCtx {
    APP_STATE *app;
    HWND hwnd;
};

static void ui_init_visual_resources(void);
static void ui_free_visual_resources(void);
static void ui_format_count(wchar_t *buf, size_t buf_size, int value);
static void ui_init_system_icons(HWND hwndList);
static int ui_icon_for_type(int is_directory, const wchar_t *extension);
static void ui_clear_icon_cache(void);
static int ui_copy_text_to_clipboard(HWND hwnd, const wchar_t *text);
static int ui_copy_entry_snapshot(APP_STATE *app, int row, wchar_t **out_name,
                                  wchar_t **out_path, int *out_is_dir);
static void ui_open_entry_path(HWND hwnd, const wchar_t *path);
static void ui_open_entry_parent(HWND hwnd, const wchar_t *path);
static IContextMenu *ui_create_shell_context_menu(HWND hwnd, const wchar_t *path);
static int ui_append_shell_context_menu(HWND hwnd, HMENU menu, const wchar_t *path,
                                        IContextMenu **out_menu);
static void ui_release_shell_menu_handlers(void);
static void ui_invoke_shell_context_command(HWND hwnd, IContextMenu *menu,
                                            int cmd, POINT pt);
static void ui_ensure_row_metadata(APP_STATE *app, int row);
static void ui_format_filetime(long long ft64, wchar_t *buf, size_t buf_size);
static void ui_queue_search(HWND hwnd);
static void ui_start_search(HWND hwnd);
static void ui_start_cache_load(HWND hwnd);
static int ui_search_can_refine(const SEARCH_QUERY *old_query, const SEARCH_QUERY *new_query);
static DWORD WINAPI search_thread_proc(void *p);
static DWORD WINAPI cache_load_thread_proc(void *p);
static void usn_start_startup_sync(HWND hwnd);
static DWORD WINAPI usn_startup_sync_thread_proc(void *p);
static int usn_sync_once(APP_STATE *app, int *needs_rebuild, int *changed_count);
static INDEX_ENTRY *ui_entry_from_row(APP_STATE *app, int row);
static void ui_get_parent_path(const INDEX_ENTRY *entry, wchar_t *buf, size_t buf_size);

/* Reindex context */
struct ReindexCtx {
    APP_STATE *app;
    HWND hwnd;
};

static void ui_init_visual_resources(void)
{
    if (!g_font_ui) {
        g_font_ui = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    
    if (!g_font_search) {
        g_font_search = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    
    if (!g_brush_window)
        g_brush_window = CreateSolidBrush(RGB(255, 255, 255));
}

static void ui_free_visual_resources(void)
{
    if (g_font_ui) { DeleteObject(g_font_ui); g_font_ui = NULL; }
    if (g_font_search) { DeleteObject(g_font_search); g_font_search = NULL; }
    if (g_brush_window) { DeleteObject(g_brush_window); g_brush_window = NULL; }
    ui_clear_icon_cache();
}

static void ui_clear_icon_cache(void)
{
    for (int i = 0; i < g_icon_cache_count; i++)
        free(g_icon_cache[i].extension);
    
    free(g_icon_cache);
    g_icon_cache = NULL;
    g_icon_cache_count = 0;
    g_icon_cache_capacity = 0;
}

static void ui_format_count(wchar_t *buf, size_t buf_size, int value)
{
    wchar_t raw[32];
    wchar_t formatted[48];
    int len, out = 0;
    
    swprintf_s(raw, 32, L"%d", value);
    len = (int)wcslen(raw);
    
    for (int i = 0; i < len && out < 47; i++) {
        if (i > 0 && ((len - i) % 3) == 0 && out < 47)
            formatted[out++] = L',';
        formatted[out++] = raw[i];
    }
    formatted[out] = L'\0';
    
    wcscpy_s(buf, buf_size, formatted);
}

static void ui_init_system_icons(HWND hwndList)
{
    SHFILEINFOW sfi;
    HIMAGELIST images;
    
    ZeroMemory(&sfi, sizeof(sfi));
    images = (HIMAGELIST)SHGetFileInfoW(
        L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (images) {
        ListView_SetImageList(hwndList, images, LVSIL_SMALL);
        g_icon_folder = sfi.iIcon;
    }
    
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(
            L"file", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        g_icon_file = sfi.iIcon;
    }
}

static int ui_icon_from_extension(const wchar_t *extension)
{
    SHFILEINFOW sfi;
    wchar_t fake_name[320];
    int icon_index = I_IMAGENONE;
    
    if (!extension || !extension[0])
        return g_icon_file;
    
    for (int i = 0; i < g_icon_cache_count; i++) {
        if (g_icon_cache[i].extension &&
            _wcsicmp(g_icon_cache[i].extension, extension) == 0)
            return g_icon_cache[i].icon_index;
    }
    
    swprintf_s(fake_name, 320, L"file.%ls", extension);
    
    ZeroMemory(&sfi, sizeof(sfi));
    if (SHGetFileInfoW(fake_name, FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        icon_index = sfi.iIcon;
    } else {
        icon_index = g_icon_file;
    }
    
    if (g_icon_cache_count >= g_icon_cache_capacity) {
        int new_capacity = g_icon_cache_capacity ? g_icon_cache_capacity * 2 : 64;
        ICON_CACHE_ENTRY *new_cache = (ICON_CACHE_ENTRY *)realloc(
            g_icon_cache, new_capacity * sizeof(ICON_CACHE_ENTRY));
        if (new_cache) {
            memset(new_cache + g_icon_cache_capacity, 0,
                   (new_capacity - g_icon_cache_capacity) * sizeof(ICON_CACHE_ENTRY));
            g_icon_cache = new_cache;
            g_icon_cache_capacity = new_capacity;
        }
    }
    
    if (g_icon_cache_count < g_icon_cache_capacity) {
        wchar_t *cached_ext = _wcsdup(extension);
        if (cached_ext) {
            g_icon_cache[g_icon_cache_count].extension = cached_ext;
            g_icon_cache[g_icon_cache_count].icon_index = icon_index;
            g_icon_cache_count++;
        }
    }
    
    return icon_index;
}

static int ui_icon_for_type(int is_directory, const wchar_t *extension)
{
    if (is_directory)
        return g_icon_folder;
    
    return ui_icon_from_extension(extension);
}

static INDEX_ENTRY *ui_entry_from_row(APP_STATE *app, int row)
{
    if (!app || row < 0 || row >= app->filtered_count)
        return NULL;
    
    int idx = app->filtered_identity ? row : app->filtered_indices[row];
    if (idx < 0 || idx >= app->entry_count)
        return NULL;
    
    return &app->entries[idx];
}

static void ui_get_parent_path(const INDEX_ENTRY *entry, wchar_t *buf, size_t buf_size)
{
    if (!entry || !entry->path || !entry->path[0]) {
        wcscpy_s(buf, buf_size, L"");
        return;
    }
    
    wcscpy_s(buf, buf_size, entry->path);
    wchar_t *last = wcsrchr(buf, L'\\');
    if (last)
        *last = L'\0';
}

static void ui_ensure_row_metadata(APP_STATE *app, int row)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    ULARGE_INTEGER size;
    wchar_t *path = NULL;
    int is_directory = 0;
    int idx = -1;
    long long file_ref = 0;
    int volume_index = -1;
    int loaded = 0;
    
    if (!app || row < 0)
        return;
    
    EnterCriticalSection(&app->index_lock);
    if (row >= 0 && row < app->filtered_count) {
        idx = app->filtered_identity ? row : app->filtered_indices[row];
        if (idx >= 0 && idx < app->entry_count) {
            INDEX_ENTRY *entry = &app->entries[idx];
            loaded = entry->metadata_loaded;
            is_directory = entry->is_directory;
            file_ref = entry->file_ref;
            volume_index = entry->volume_index;
            if (!loaded && entry->path && entry->path[0])
                path = _wcsdup(entry->path);
        }
    }
    LeaveCriticalSection(&app->index_lock);
    
    if (loaded || !path)
        return;
    
    memset(&data, 0, sizeof(data));
    loaded = GetFileAttributesExW(path, GetFileExInfoStandard, &data) ? 1 : 0;
    
    EnterCriticalSection(&app->index_lock);
    if (idx >= 0 && idx < app->entry_count) {
        INDEX_ENTRY *entry = &app->entries[idx];
        if (entry->file_ref == file_ref &&
            entry->volume_index == volume_index &&
            entry->path && wcscmp(entry->path, path) == 0) {
            if (loaded) {
                entry->attributes = data.dwFileAttributes;
                entry->creation_time = ((long long)data.ftCreationTime.dwHighDateTime << 32) |
                                       data.ftCreationTime.dwLowDateTime;
                entry->modification_time = ((long long)data.ftLastWriteTime.dwHighDateTime << 32) |
                                           data.ftLastWriteTime.dwLowDateTime;
                entry->access_time = ((long long)data.ftLastAccessTime.dwHighDateTime << 32) |
                                     data.ftLastAccessTime.dwLowDateTime;
                
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                entry->size = is_directory ? 0 : (long long)size.QuadPart;
            } else {
                entry->size = 0;
                entry->creation_time = 0;
                entry->modification_time = 0;
                entry->access_time = 0;
            }
            entry->metadata_loaded = 1;
        }
    }
    LeaveCriticalSection(&app->index_lock);
    
    free(path);
}

static void ui_format_filetime(long long ft64, wchar_t *buf, size_t buf_size)
{
    FILETIME ft;
    FILETIME local;
    SYSTEMTIME st;
    
    if (!buf || buf_size == 0)
        return;
    
    ft.dwLowDateTime = (DWORD)ft64;
    ft.dwHighDateTime = (DWORD)(ft64 >> 32);
    if (ft64 &&
        FileTimeToLocalFileTime(&ft, &local) &&
        FileTimeToSystemTime(&local, &st)) {
        swprintf_s(buf, buf_size, L"%04d-%02d-%02d %02d:%02d:%02d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
    } else {
        wcscpy_s(buf, buf_size, L"");
    }
}

static int ui_copy_text_to_clipboard(HWND hwnd, const wchar_t *text)
{
    size_t len;
    HGLOBAL hMem;
    wchar_t *dst;
    
    if (!text)
        text = L"";
    
    if (!OpenClipboard(hwnd))
        return 0;
    
    EmptyClipboard();
    len = (wcslen(text) + 1) * sizeof(wchar_t);
    hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) {
        CloseClipboard();
        return 0;
    }
    
    dst = (wchar_t *)GlobalLock(hMem);
    if (!dst) {
        GlobalFree(hMem);
        CloseClipboard();
        return 0;
    }
    
    wcscpy_s(dst, len / sizeof(wchar_t), text);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return 1;
}

static int ui_copy_entry_snapshot(APP_STATE *app, int row, wchar_t **out_name,
                                  wchar_t **out_path, int *out_is_dir)
{
    INDEX_ENTRY *entry;
    
    if (out_name) *out_name = NULL;
    if (out_path) *out_path = NULL;
    if (out_is_dir) *out_is_dir = 0;
    if (!app)
        return 0;
    
    EnterCriticalSection(&app->index_lock);
    entry = ui_entry_from_row(app, row);
    if (entry) {
        if (out_name)
            *out_name = _wcsdup(entry->name ? entry->name : L"");
        if (out_path)
            *out_path = _wcsdup(entry->path ? entry->path : L"");
        if (out_is_dir)
            *out_is_dir = entry->is_directory;
    }
    LeaveCriticalSection(&app->index_lock);
    
    if (!entry)
        return 0;
    if ((out_name && !*out_name) || (out_path && !*out_path)) {
        if (out_name) { free(*out_name); *out_name = NULL; }
        if (out_path) { free(*out_path); *out_path = NULL; }
        return 0;
    }
    
    return 1;
}

static void ui_open_entry_path(HWND hwnd, const wchar_t *path)
{
    if (path && path[0])
        ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOW);
}

static void ui_open_entry_parent(HWND hwnd, const wchar_t *path)
{
    wchar_t dir[MAX_PATH * 2];
    wchar_t *last;
    
    if (!path || !path[0])
        return;
    
    wcscpy_s(dir, MAX_PATH * 2, path);
    last = wcsrchr(dir, L'\\');
    if (last) {
        *last = L'\0';
        ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOW);
    }
}

static void ui_release_shell_menu_handlers(void)
{
    if (g_shell_menu3) {
        g_shell_menu3->lpVtbl->Release(g_shell_menu3);
        g_shell_menu3 = NULL;
    }
    if (g_shell_menu2) {
        g_shell_menu2->lpVtbl->Release(g_shell_menu2);
        g_shell_menu2 = NULL;
    }
}

static IContextMenu *ui_create_shell_context_menu(HWND hwnd, const wchar_t *path)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    PCUITEMID_CHILD child = NULL;
    IShellFolder *parent = NULL;
    IContextMenu *menu = NULL;
    HRESULT hr;
    
    if (!path || !path[0])
        return NULL;
    
    hr = SHParseDisplayName(path, NULL, &pidl, 0, NULL);
    if (FAILED(hr) || !pidl)
        return NULL;
    
    hr = SHBindToParent(pidl, &IID_IShellFolder, (void **)&parent, &child);
    if (SUCCEEDED(hr) && parent && child) {
        parent->lpVtbl->GetUIObjectOf(parent, hwnd, 1, &child,
                                      &IID_IContextMenu, NULL, (void **)&menu);
    }
    
    if (parent)
        parent->lpVtbl->Release(parent);
    CoTaskMemFree(pidl);
    return menu;
}

static int ui_append_shell_context_menu(HWND hwnd, HMENU menu, const wchar_t *path,
                                        IContextMenu **out_menu)
{
    IContextMenu *shell_menu;
    HRESULT hr;
    
    if (out_menu)
        *out_menu = NULL;
    
    shell_menu = ui_create_shell_context_menu(hwnd, path);
    if (!shell_menu)
        return 0;
    
    hr = shell_menu->lpVtbl->QueryContextMenu(shell_menu, menu,
                                             GetMenuItemCount(menu),
                                             IDM_CTX_SHELL_FIRST,
                                             IDM_CTX_SHELL_LAST,
                                             CMF_NORMAL);
    if (FAILED(hr)) {
        shell_menu->lpVtbl->Release(shell_menu);
        return 0;
    }
    
    ui_release_shell_menu_handlers();
    shell_menu->lpVtbl->QueryInterface(shell_menu, &IID_IContextMenu2, (void **)&g_shell_menu2);
    shell_menu->lpVtbl->QueryInterface(shell_menu, &IID_IContextMenu3, (void **)&g_shell_menu3);
    
    if (out_menu)
        *out_menu = shell_menu;
    else
        shell_menu->lpVtbl->Release(shell_menu);
    return 1;
}

static void ui_invoke_shell_context_command(HWND hwnd, IContextMenu *menu,
                                            int cmd, POINT pt)
{
    CMINVOKECOMMANDINFOEX info;
    
    if (!menu || cmd < IDM_CTX_SHELL_FIRST || cmd > IDM_CTX_SHELL_LAST)
        return;
    
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    info.hwnd = hwnd;
    info.lpVerb = MAKEINTRESOURCEA(cmd - IDM_CTX_SHELL_FIRST);
    info.lpVerbW = MAKEINTRESOURCEW(cmd - IDM_CTX_SHELL_FIRST);
    info.nShow = SW_SHOWNORMAL;
    info.ptInvoke = pt;
    menu->lpVtbl->InvokeCommand(menu, (LPCMINVOKECOMMANDINFO)&info);
}

static DWORD WINAPI search_thread_proc(void *p)
{
    struct SearchJob *job = (struct SearchJob *)p;
    
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    
    job->result_count = search_execute_subset_to_buffer(
        job->app, &job->query, job->results, job->max_results,
        job->base_indices, job->base_count, job->base_identity,
        job->generation);
    
    if (job->app->shutting_down ||
        !PostMessageW(job->hwnd, WM_SEARCH_DONE, (WPARAM)job->generation, (LPARAM)job)) {
        free(job->base_indices);
        free(job->results);
        free(job);
        InterlockedExchange(&g_search_running, 0);
    }
    
    return 0;
}

static int ui_query_is_plain(const wchar_t *text)
{
    if (!text || !text[0])
        return 0;
    if (wcschr(text, L'*') || wcschr(text, L'?'))
        return 0;
    if (StrStrIW(text, L"ext:") || StrStrIW(text, L"folder:"))
        return 0;
    return 1;
}

static int ui_search_can_refine(const SEARCH_QUERY *old_query, const SEARCH_QUERY *new_query)
{
    size_t old_len;
    size_t new_len;
    
    if (!old_query || !new_query)
        return 0;
    if (!old_query->text[0] || !new_query->text[0])
        return 0;
    if (!ui_query_is_plain(old_query->text) || !ui_query_is_plain(new_query->text))
        return 0;
    if (old_query->match_case != new_query->match_case ||
        old_query->match_whole_word != new_query->match_whole_word ||
        old_query->match_path != new_query->match_path ||
        old_query->use_regex != new_query->use_regex ||
        old_query->sort_column != new_query->sort_column ||
        old_query->sort_ascending != new_query->sort_ascending)
        return 0;
    if (new_query->match_whole_word)
        return 0;
    
    old_len = wcslen(old_query->text);
    new_len = wcslen(new_query->text);
    if (new_len <= old_len)
        return 0;
    
    return new_query->match_case
        ? (wcsncmp(new_query->text, old_query->text, old_len) == 0)
        : (_wcsnicmp(new_query->text, old_query->text, old_len) == 0);
}

static DWORD WINAPI cache_load_thread_proc(void *p)
{
    struct CacheLoadCtx *ctx = (struct CacheLoadCtx *)p;
    int loaded;
    
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    loaded = cache_load_index(ctx->app);
    if (loaded)
        index_build_name_char_index(ctx->app);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    
    InterlockedExchange(&g_cache_loading, 0);
    if (!ctx->app->shutting_down)
        PostMessageW(ctx->hwnd, loaded ? WM_CACHE_LOADED : WM_REFRESH, 0, 0);
    free(ctx);
    return 0;
}

static void ui_start_cache_load(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    struct CacheLoadCtx *ctx;
    HANDLE thread;
    
    if (!app || InterlockedCompareExchange(&g_cache_loading, 1, 0) != 0)
        return;
    
    if (g_cache_thread && WaitForSingleObject(g_cache_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_cache_thread);
        g_cache_thread = NULL;
    }
    
    ctx = (struct CacheLoadCtx *)calloc(1, sizeof(struct CacheLoadCtx));
    if (!ctx) {
        InterlockedExchange(&g_cache_loading, 0);
        PostMessageW(hwnd, WM_REFRESH, 0, 0);
        return;
    }
    
    ctx->app = app;
    ctx->hwnd = hwnd;
    
    thread = CreateThread(NULL, 0, cache_load_thread_proc, ctx, 0, NULL);
    if (thread) {
        g_cache_thread = thread;
    } else {
        free(ctx);
        InterlockedExchange(&g_cache_loading, 0);
        PostMessageW(hwnd, WM_REFRESH, 0, 0);
    }
}

static void ui_start_search(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    struct SearchJob *job;
    HANDLE thread;
    int entry_count;
    SEARCH_QUERY next_query;
    int identity_result = 0;
    int search_limit;
    
    if (!app || InterlockedCompareExchange(&g_search_running, 1, 0) != 0)
        return;
    
    memset(&next_query, 0, sizeof(next_query));
    GetWindowTextW(g_hwndSearch, next_query.text, 512);
    next_query.match_case = app->match_case;
    next_query.match_whole_word = app->match_whole_word;
    next_query.match_path = app->match_path;
    next_query.use_regex = app->use_regex;
    next_query.sort_column = app->query.sort_column;
    next_query.sort_ascending = app->query.sort_ascending;
    search_prepare_query(&next_query);
    
    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    identity_result = (!next_query.text[0] &&
                       next_query.sort_column == COL_NAME &&
                       next_query.sort_ascending);
    if (identity_result) {
        app->filtered_identity = 1;
        app->filtered_count = entry_count;
        app->query = next_query;
    }
    LeaveCriticalSection(&app->index_lock);
    
    if (identity_result) {
        app->is_searching = 0;
        InterlockedExchange(&g_search_running, 0);
        ui_update_listview(g_hwndList, app);
        ui_update_status(g_hwndStatus, app);
        return;
    }
    
    job = (struct SearchJob *)calloc(1, sizeof(struct SearchJob));
    if (!job) {
        InterlockedExchange(&g_search_running, 0);
        return;
    }
    
    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    if (ui_search_can_refine(&app->query, &next_query) && app->filtered_count > 0) {
        job->base_count = app->filtered_count;
        job->base_identity = app->filtered_identity;
        if (!job->base_identity) {
            job->base_indices = (int *)malloc(job->base_count * sizeof(int));
            if (job->base_indices)
                memcpy(job->base_indices, app->filtered_indices, job->base_count * sizeof(int));
            else
                job->base_count = 0;
        }
    }
    LeaveCriticalSection(&app->index_lock);
    
    search_limit = entry_count < SEARCH_MAX_RESULTS ? entry_count : SEARCH_MAX_RESULTS;
    job->results = (int *)malloc((search_limit > 0 ? search_limit : 1) * sizeof(int));
    if (!job->results) {
        free(job->base_indices);
        free(job);
        InterlockedExchange(&g_search_running, 0);
        return;
    }
    
    job->query = next_query;
    job->generation = app->search_generation;
    job->max_results = search_limit;
    job->app = app;
    job->hwnd = hwnd;
    app->is_searching = 1;
    SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"Searching...");
    
    thread = CreateThread(NULL, 0, search_thread_proc, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        free(job->base_indices);
        free(job->results);
        free(job);
        app->is_searching = 0;
        InterlockedExchange(&g_search_running, 0);
    }
}

static void ui_queue_search(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    if (!app)
        return;
    
    InterlockedIncrement(&app->search_generation);
    KillTimer(hwnd, IDT_SEARCH_DEBOUNCE);
    SetTimer(hwnd, IDT_SEARCH_DEBOUNCE, SEARCH_DEBOUNCE_MS, NULL);
}

static int usn_sync_once(APP_STATE *app, int *needs_rebuild, int *changed_count)
{
    VOLUME_INFO current[26];
    int current_count;
    int local_volume_count;
    int total_applied = 0;
    
    if (needs_rebuild) *needs_rebuild = 0;
    if (changed_count) *changed_count = 0;
    
    current_count = ntfs_enumerate_volumes(current, 26);
    
    EnterCriticalSection(&app->index_lock);
    local_volume_count = app->volume_count;
    if (current_count != local_volume_count) {
        LeaveCriticalSection(&app->index_lock);
        if (needs_rebuild) *needs_rebuild = 1;
        return 1;
    }
    
    for (int i = 0; i < local_volume_count; i++) {
        if (_wcsicmp(current[i].drive_letter, app->volumes[i].drive_letter) != 0) {
            LeaveCriticalSection(&app->index_lock);
            if (needs_rebuild) *needs_rebuild = 1;
            return 1;
        }
    }
    LeaveCriticalSection(&app->index_lock);
    
    for (int i = 0; i < local_volume_count; i++) {
        wchar_t volume_path[64];
        long long saved_journal_id;
        long long saved_next_usn;
        
        EnterCriticalSection(&app->index_lock);
        wcscpy_s(volume_path, 64, app->volumes[i].volume_path);
        saved_journal_id = app->volumes[i].usn_journal_id;
        saved_next_usn = app->volumes[i].usn_next_usn;
        LeaveCriticalSection(&app->index_lock);
        
        HANDLE hVol = ntfs_open_volume(volume_path);
        if (!hVol)
            continue;
        
        USN_JOURNAL_DATA_BUF journal;
        if (!ntfs_query_usn_journal(hVol, &journal)) {
            ntfs_close_volume(hVol);
            continue;
        }
        
        if (saved_journal_id == 0 ||
            saved_journal_id != journal.UsnJournalId ||
            saved_next_usn < journal.LowestValidUsn ||
            saved_next_usn > journal.NextUsn) {
            ntfs_close_volume(hVol);
            if (needs_rebuild) *needs_rebuild = 1;
            return 1;
        }
        
        if (saved_next_usn < journal.NextUsn) {
            USN_CHANGE *changes = NULL;
            int change_count = 0;
            long long next_usn = saved_next_usn;
            
            int read_changes = ntfs_read_usn_changes(hVol, saved_next_usn, journal.UsnJournalId,
                                                     journal.NextUsn, i, &changes, &change_count,
                                                     &next_usn, STARTUP_SYNC_MAX_CHANGES);
            if (read_changes == 2) {
                ntfs_close_volume(hVol);
                continue;
            }
            if (!read_changes) {
                ntfs_close_volume(hVol);
                continue;
            }
            
            if (change_count > 0) {
                InterlockedIncrement(&app->search_generation);
                total_applied += index_apply_usn_changes(app, changes, change_count);
            }
            
            ntfs_free_usn_changes(changes, change_count);
            
            EnterCriticalSection(&app->index_lock);
            app->volumes[i].usn_journal_id = journal.UsnJournalId;
            app->volumes[i].usn_next_usn = next_usn;
            app->volumes[i].usn_lowest_valid_usn = journal.LowestValidUsn;
            LeaveCriticalSection(&app->index_lock);
        }
        
        ntfs_close_volume(hVol);
    }
    
    if (changed_count)
        *changed_count = total_applied;
    
    return 1;
}

static DWORD WINAPI usn_startup_sync_thread_proc(void *p)
{
    struct UsnStartupSyncCtx *ctx = (struct UsnStartupSyncCtx *)p;
    APP_STATE *app = ctx->app;
    HWND hwnd = ctx->hwnd;
    int needs_rebuild = 0;
    int changed_count = 0;
    
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    
    if (usn_sync_once(app, &needs_rebuild, &changed_count)) {
        if (app->shutting_down) {
            /* App is exiting; skip UI work. */
        } else if (needs_rebuild) {
            /* Keep the cached index usable; never auto-rebuild during startup. */
        } else if (changed_count > 0) {
            PostMessageW(hwnd, WM_INDEX_SYNCED, (WPARAM)changed_count, 0);
        }
    }
    
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    InterlockedExchange(&app->monitor_running, 0);
    free(ctx);
    return 0;
}

static void usn_start_startup_sync(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    struct UsnStartupSyncCtx *ctx;
    HANDLE thread;
    
    if (!app)
        return;
    
    if (InterlockedCompareExchange(&app->monitor_running, 1, 0) != 0)
        return;
    
    if (app->monitor_thread) {
        if (WaitForSingleObject(app->monitor_thread, 0) == WAIT_OBJECT_0) {
            CloseHandle(app->monitor_thread);
            app->monitor_thread = NULL;
        }
    }
    
    ctx = (struct UsnStartupSyncCtx *)calloc(1, sizeof(struct UsnStartupSyncCtx));
    if (!ctx) {
        InterlockedExchange(&app->monitor_running, 0);
        return;
    }
    
    ctx->app = app;
    ctx->hwnd = hwnd;
    
    thread = CreateThread(NULL, 0, usn_startup_sync_thread_proc, ctx, 0, NULL);
    if (thread) {
        app->monitor_thread = thread;
    } else {
        free(ctx);
        InterlockedExchange(&app->monitor_running, 0);
    }
}

static DWORD WINAPI reindex_thread_proc(void *p)
{
    struct ReindexCtx *c = (struct ReindexCtx *)p;
    APP_STATE *a = c->app;
    int v;
    int indexed_volumes = 0;
    int failed_volumes = 0;
    
    index_clear(a);
    a->indexed_volume_count = 0;
    a->index_error_count = 0;
    a->volume_count = ntfs_enumerate_volumes(a->volumes, 26);
    
    for (v = 0; v < a->volume_count; v++) {
        if (!a->volumes[v].is_ntfs) continue;
        
        HANDLE hVol = ntfs_open_volume(a->volumes[v].volume_path);
        if (!hVol) {
            failed_volumes++;
            continue;
        }
        
        INDEX_ENTRY *vol_entries = NULL;
        int vol_count = 0;
        if (ntfs_read_usn_index(hVol, &vol_entries, &vol_count, v, c->hwnd) > 0 ||
            ntfs_read_mft(hVol, &vol_entries, &vol_count, v, c->hwnd) > 0)
            indexed_volumes++;
        else
            failed_volumes++;
        ntfs_update_volume_usn_info(hVol, &a->volumes[v]);
        ntfs_close_volume(hVol);
        
        index_add_entries(a, vol_entries, vol_count);
        free(vol_entries);
    }
    
    a->indexed_volume_count = indexed_volumes;
    a->index_error_count = failed_volumes;
    index_build_paths(a);
    index_sort_entries_by_name(a);
    index_build_name_char_index(a);
    if (a->entry_count > 0)
        cache_save_index(a);
    PostMessageW(c->hwnd, WM_INDEX_DONE, 0, 0);
    InterlockedExchange(&g_reindexing, 0);
    free(c);
    return 0;
}

/* =============================================================
 * Window class registration
 * ============================================================= */
int ui_init(HINSTANCE hInst)
{
    g_hInst = hInst;
    
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = everything_wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = WC_EVERYTHING;
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    
    if (!RegisterClassExW(&wc))
        return 0;
    
    return 1;
}

/* =============================================================
 * Create main window
 * ============================================================= */
HWND ui_create_main_window(HINSTANCE hInst, int nCmdShow, APP_STATE *app)
{
    g_app_ptr = app;
    app->hinst = hInst;
    
    HWND hwnd = CreateWindowExW(
        0,
        WC_EVERYTHING,
        L"OpenEverything",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
        NULL, NULL, hInst, NULL);
    
    if (!hwnd) return NULL;
    
    app->hwnd_main = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    return hwnd;
}

/* =============================================================
 * Update list view with filtered results
 * ============================================================= */
void ui_update_listview(HWND hwndList, APP_STATE *app)
{
    int count = app->filtered_count;
    ListView_SetItemCountEx(hwndList, count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    InvalidateRect(hwndList, NULL, TRUE);
}

void ui_update_status(HWND hwndStatus, APP_STATE *app)
{
    wchar_t buf[256];
    wchar_t filtered[48];
    wchar_t total[48];
    
    ui_format_count(filtered, 48, app->filtered_count);
    ui_format_count(total, 48, app->entry_count);
    
    if (app->entry_count == 0 && app->index_error_count > 0) {
        swprintf_s(buf, 256, L"0 objects - unable to read NTFS volumes; try running as Administrator");
    } else if (app->filtered_count == app->entry_count) {
        swprintf_s(buf, 256, L"%s objects", total);
    } else {
        swprintf_s(buf, 256, L"%s objects (%s total)", filtered, total);
    }
    SendMessageW(hwndStatus, SB_SETTEXTW, 0, (LPARAM)buf);
}

/* =============================================================
 * Main window procedure
 * ============================================================= */
static LRESULT CALLBACK everything_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    APP_STATE *app = g_app_ptr;
    
    switch (msg) {
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR:
    {
        LRESULT result = 0;
        if (g_shell_menu3 &&
            SUCCEEDED(g_shell_menu3->lpVtbl->HandleMenuMsg2(g_shell_menu3, msg, wParam, lParam, &result)))
            return result;
        if (g_shell_menu2 &&
            SUCCEEDED(g_shell_menu2->lpVtbl->HandleMenuMsg(g_shell_menu2, msg, wParam, lParam)))
            return 0;
        break;
    }
    
    /* ---- Window creation ---- */
    case WM_CREATE:
    {
        CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        ui_init_visual_resources();
        
        /* Create menu bar */
        HMENU hMenu = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        HMENU hEdit = CreatePopupMenu();
        HMENU hView = CreatePopupMenu();
        HMENU hSearch = CreatePopupMenu();
        HMENU hIndex = CreatePopupMenu();
        HMENU hHelp = CreatePopupMenu();
        
        AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"Exit\tAlt+F4");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY, L"Copy\tCtrl+C");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY_PATH, L"Copy Full Path\tCtrl+Shift+C");
        AppendMenuW(hEdit, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_SELECT_ALL, L"Select All\tCtrl+A");
        
        /* View menu with check marks */
        UINT flags = app ? (app->match_case ? MF_CHECKED : 0) : 0;
        AppendMenuW(hView, MF_STRING | flags, IDM_VIEW_MATCH_CASE, L"Match Case");
        flags = app ? (app->match_whole_word ? MF_CHECKED : 0) : 0;
        AppendMenuW(hView, MF_STRING | flags, IDM_VIEW_MATCH_WHOLE_WORD, L"Match Whole Word");
        flags = app ? (app->match_path ? MF_CHECKED : 0) : 0;
        AppendMenuW(hView, MF_STRING | flags, IDM_VIEW_MATCH_PATH, L"Match Path");
        flags = app ? (app->use_regex ? MF_CHECKED : 0) : 0;
        AppendMenuW(hView, MF_STRING | flags, IDM_VIEW_USE_REGEX, L"Use Regex");
        AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hView, MF_STRING, IDM_VIEW_REFRESH, L"Refresh\tF5");
        AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
        HMENU hSort = CreatePopupMenu();
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_NAME, L"Name");
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_PATH, L"Path");
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_SIZE, L"Size");
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_DATE_MODIFIED, L"Date Modified");
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_DATE_CREATED, L"Date Created");
        AppendMenuW(hSort, MF_STRING, IDM_VIEW_SORT_ATTRIBUTES, L"Attributes");
        AppendMenuW(hView, MF_POPUP, (UINT_PTR)hSort, L"Sort By");
        
        AppendMenuW(hSearch, MF_STRING, 10070, L"Add to Bookmarks...");
        AppendMenuW(hSearch, MF_STRING, 10071, L"Organize Filters...");
        
        AppendMenuW(hIndex, MF_STRING, IDM_INDEX_UPDATE, L"Update Index");
        AppendMenuW(hIndex, MF_STRING, IDM_INDEX_REBUILD, L"Rebuild Index");
        
        AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"About OpenEverything");
        
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"File");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hEdit, L"Edit");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"View");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSearch, L"Search");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hIndex, L"Index");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"Help");
        
        SetMenu(hwnd, hMenu);
        
        g_hwndSearch = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            4, UI_SEARCH_TOP, rc.right - 8, UI_SEARCH_HEIGHT - 8,
            hwnd, (HMENU)IDC_SEARCH_EDIT, cs->hInstance, NULL);
        
        /* Subclass the edit control */
        SetWindowSubclass(g_hwndSearch, search_edit_proc, 1, 0);
        
        /* Create list view */
        g_hwndList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            0, UI_SEARCH_TOP + UI_SEARCH_HEIGHT, rc.right,
            rc.bottom - UI_SEARCH_TOP - UI_SEARCH_HEIGHT - 22,
            hwnd, (HMENU)IDC_LISTVIEW, cs->hInstance, NULL);
        
        /* Enable double buffering */
        ListView_SetExtendedListViewStyleEx(g_hwndList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
        SetWindowTheme(g_hwndList, L"Explorer", NULL);
        SetWindowTheme(g_hwndSearch, L"Explorer", NULL);
        ListView_SetBkColor(g_hwndList, RGB(255, 255, 255));
        ListView_SetTextBkColor(g_hwndList, RGB(255, 255, 255));
        ListView_SetTextColor(g_hwndList, RGB(32, 37, 45));
        ui_init_system_icons(g_hwndList);
        
        /* Insert columns */
        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        
        col.pszText = L"Name";          col.cx = 380; col.iSubItem = 0; ListView_InsertColumn(g_hwndList, 0, &col);
        col.pszText = L"Path";          col.cx = 520; col.iSubItem = 1; ListView_InsertColumn(g_hwndList, 1, &col);
        col.pszText = L"Size";          col.cx = 96;  col.iSubItem = 2; ListView_InsertColumn(g_hwndList, 2, &col);
        col.pszText = L"Date Modified"; col.cx = 150; col.iSubItem = 3; ListView_InsertColumn(g_hwndList, 3, &col);
        col.pszText = L"Date Created";  col.cx = 0;   col.iSubItem = 4; ListView_InsertColumn(g_hwndList, 4, &col);
        col.pszText = L"Attributes";    col.cx = 0;   col.iSubItem = 5; ListView_InsertColumn(g_hwndList, 5, &col);
        
        /* Add extension column (hidden) */
        col.pszText = L"Extension";     col.cx = 0;   col.iSubItem = 6; ListView_InsertColumn(g_hwndList, 6, &col);
        
        /* Set font */
        SendMessageW(g_hwndSearch, WM_SETFONT, (WPARAM)g_font_search, TRUE);
        SendMessageW(g_hwndList, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        
        /* Create status bar */
        g_hwndStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hwnd, (HMENU)IDC_STATUS_BAR, cs->hInstance, NULL);
        
        app->hwnd_search = g_hwndSearch;
        app->hwnd_list = g_hwndList;
        app->hwnd_status = g_hwndStatus;
        
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"Loading index cache...");
        ui_start_cache_load(hwnd);
        
        return 0;
    }
    
    /* ---- Window sizing ---- */
    case WM_SIZE:
    {
        RECT rc;
        RECT rcStatus;
        int status_h = 22;
        int list_top = UI_SEARCH_TOP + UI_SEARCH_HEIGHT;
        
        GetClientRect(hwnd, &rc);
        
        /* Status bar */
        SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
        GetWindowRect(g_hwndStatus, &rcStatus);
        status_h = rcStatus.bottom - rcStatus.top;
        if (status_h <= 0) status_h = 22;
        
        SetWindowPos(g_hwndSearch, NULL, 4, UI_SEARCH_TOP,
                     rc.right - 8, UI_SEARCH_HEIGHT - 8, SWP_NOZORDER);
        
        /* List view */
        SetWindowPos(g_hwndList, NULL, 0, list_top,
                     rc.right, rc.bottom - list_top - status_h,
                     SWP_NOZORDER);
        
        /* Resize list columns proportionally */
        int total_width = rc.right - GetSystemMetrics(SM_CXVSCROLL);
        int size_width = 96;
        int modified_width = 150;
        int text_width;
        if (total_width < 500) total_width = 500;
        text_width = total_width - size_width - modified_width;
        if (text_width < 260) text_width = 260;
        ListView_SetColumnWidth(g_hwndList, 0, text_width * 40 / 100);
        ListView_SetColumnWidth(g_hwndList, 1, text_width - (text_width * 40 / 100));
        ListView_SetColumnWidth(g_hwndList, 2, size_width);
        ListView_SetColumnWidth(g_hwndList, 3, modified_width);
        ListView_SetColumnWidth(g_hwndList, 4, 0);
        ListView_SetColumnWidth(g_hwndList, 5, 0);
        
        return 0;
    }
    
    /* ---- Menu and command handling ---- */
    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD notify = HIWORD(wParam);
        
        if (id == IDC_SEARCH_EDIT && notify == EN_CHANGE) {
            ui_queue_search(hwnd);
            return 0;
        }
        
        switch (id) {
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd);
            break;
            
        case IDM_EDIT_COPY:
        {
            int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < app->filtered_count) {
                INDEX_ENTRY *e = ui_entry_from_row(app, sel);
                if (!e) break;
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t len = (wcslen(e->name) + 1) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                    if (hMem) {
                        wchar_t *p = (wchar_t *)GlobalLock(hMem);
                        wcscpy_s(p, len / sizeof(wchar_t), e->name);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
            break;
        }
        
        case IDM_EDIT_COPY_PATH:
        {
            int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < app->filtered_count) {
                INDEX_ENTRY *e = ui_entry_from_row(app, sel);
                if (!e) break;
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t len = (wcslen(e->path) + 1) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                    if (hMem) {
                        wchar_t *p = (wchar_t *)GlobalLock(hMem);
                        wcscpy_s(p, len / sizeof(wchar_t), e->path);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
            break;
        }
        
        case IDM_EDIT_SELECT_ALL:
            for (int i = 0; i < app->filtered_count; i++) {
                ListView_SetItemState(g_hwndList, i, LVIS_SELECTED, LVIS_SELECTED);
            }
            break;
            
        case IDM_VIEW_MATCH_CASE:
            app->match_case = !app->match_case;
            app->query.match_case = app->match_case;
            CheckMenuItem(GetMenu(hwnd), IDM_VIEW_MATCH_CASE,
                          app->match_case ? MF_CHECKED : MF_UNCHECKED);
            ui_queue_search(hwnd);
            break;
            
        case IDM_VIEW_MATCH_WHOLE_WORD:
            app->match_whole_word = !app->match_whole_word;
            app->query.match_whole_word = app->match_whole_word;
            CheckMenuItem(GetMenu(hwnd), IDM_VIEW_MATCH_WHOLE_WORD,
                          app->match_whole_word ? MF_CHECKED : MF_UNCHECKED);
            ui_queue_search(hwnd);
            break;
            
        case IDM_VIEW_MATCH_PATH:
            app->match_path = !app->match_path;
            app->query.match_path = app->match_path;
            CheckMenuItem(GetMenu(hwnd), IDM_VIEW_MATCH_PATH,
                          app->match_path ? MF_CHECKED : MF_UNCHECKED);
            ui_queue_search(hwnd);
            break;
            
        case IDM_VIEW_USE_REGEX:
            app->use_regex = !app->use_regex;
            app->query.use_regex = app->use_regex;
            CheckMenuItem(GetMenu(hwnd), IDM_VIEW_USE_REGEX,
                          app->use_regex ? MF_CHECKED : MF_UNCHECKED);
            ui_queue_search(hwnd);
            break;
            
        case IDM_VIEW_SORT_NAME:       app->query.sort_column = COL_NAME;          goto do_sort;
        case IDM_VIEW_SORT_PATH:       app->query.sort_column = COL_PATH;          goto do_sort;
        case IDM_VIEW_SORT_SIZE:       app->query.sort_column = COL_SIZE;          goto do_sort;
        case IDM_VIEW_SORT_DATE_MODIFIED: app->query.sort_column = COL_DATE_MODIFIED; goto do_sort;
        case IDM_VIEW_SORT_DATE_CREATED:  app->query.sort_column = COL_DATE_CREATED;  goto do_sort;
        case IDM_VIEW_SORT_ATTRIBUTES: app->query.sort_column = COL_ATTRIBUTES;    goto do_sort;
        
        do_sort:
            ui_queue_search(hwnd);
            break;
        
        case IDM_VIEW_REFRESH:
        case IDM_INDEX_UPDATE:
        case ID_TOOLBAR_REFRESH:
            PostMessageW(hwnd, WM_REFRESH, 0, 0);
            break;
            
        case IDM_INDEX_REBUILD:
            PostMessageW(hwnd, WM_REFRESH, 0, 0);
            break;
            
        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"OpenEverything v1.0\n\n"
                L"Everything的开源复刻版本。\n\n"
                L"原理介绍：\n"
                L"通过 NTFS USN Journal / MFT 快速建立文件名索引，\n"
                L"使用本地缓存避免每次启动全量重建，\n"
                L"搜索阶段使用后台线程和虚拟列表保持界面响应。\n\n"
                L"GitHub主页：\n"
                L"https://github.com/DisaWdcba",
                L"About OpenEverything", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }
    
    case WM_TIMER:
        if (wParam == IDT_SEARCH_DEBOUNCE) {
            KillTimer(hwnd, IDT_SEARCH_DEBOUNCE);
            ui_start_search(hwnd);
            return 0;
        }
        if (wParam == IDT_STARTUP_SYNC) {
            KillTimer(hwnd, IDT_STARTUP_SYNC);
            usn_start_startup_sync(hwnd);
            return 0;
        }
        break;
    
    /* ---- List view notification ---- */
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->idFrom == IDC_LISTVIEW) {
            switch (nm->code) {
            case LVN_GETDISPINFOW:
            {
                NMLVDISPINFOW *di = (NMLVDISPINFOW *)lParam;
                static wchar_t text_buf[512];
                wchar_t icon_ext[64];
                INDEX_ENTRY *e;
                int has_entry = 0;
                int icon_is_dir = 0;
                icon_ext[0] = L'\0';
                
                if ((di->item.mask & LVIF_TEXT) &&
                    (di->item.iSubItem == COL_SIZE ||
                     di->item.iSubItem == COL_DATE_MODIFIED ||
                     di->item.iSubItem == COL_DATE_CREATED ||
                     di->item.iSubItem == COL_ATTRIBUTES)) {
                    ui_ensure_row_metadata(app, di->item.iItem);
                }
                
                EnterCriticalSection(&app->index_lock);
                e = ui_entry_from_row(app, di->item.iItem);
                
                if (e) {
                    has_entry = 1;
                    icon_is_dir = e->is_directory;
                    wcsncpy_s(icon_ext, 64, e->extension ? e->extension : L"", _TRUNCATE);
                    
                    if (di->item.mask & LVIF_TEXT) {
                        switch (di->item.iSubItem) {
                        case COL_NAME:
                            wcscpy_s(text_buf, 512, e->name ? e->name : L"");
                            di->item.pszText = text_buf;
                            break;
                        case COL_PATH:
                            ui_get_parent_path(e, text_buf, 512);
                            di->item.pszText = text_buf;
                            break;
                        case COL_SIZE:
                            if (e->is_directory)
                                wcscpy_s(text_buf, 512, L"");
                            else
                                ntfs_format_size(text_buf, 512, e->size);
                            di->item.pszText = text_buf;
                            break;
                        case COL_DATE_MODIFIED:
                            ui_format_filetime(e->modification_time, text_buf, 512);
                            di->item.pszText = text_buf;
                            break;
                        case COL_DATE_CREATED:
                            ui_format_filetime(e->creation_time, text_buf, 512);
                            di->item.pszText = text_buf;
                            break;
                        case COL_ATTRIBUTES:
                            ntfs_format_attributes(text_buf, 512, e->attributes);
                            di->item.pszText = text_buf;
                            break;
                        default:
                            di->item.pszText = L"";
                            break;
                        }
                    }
                }
                
                LeaveCriticalSection(&app->index_lock);
                
                if (has_entry && (di->item.mask & LVIF_IMAGE))
                    di->item.iImage = ui_icon_for_type(icon_is_dir, icon_ext);
                break;
            }
            
            case NM_DBLCLK:
            {
                int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < app->filtered_count) {
                    INDEX_ENTRY *e = ui_entry_from_row(app, sel);
                    if (e && e->path && e->path[0]) {
                        ShellExecuteW(hwnd, L"open", e->path, NULL, NULL, SW_SHOW);
                    }
                }
                break;
            }
            
            case LVN_KEYDOWN:
            {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lParam;
                if (kd->wVKey == VK_DELETE) {
                    int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < app->filtered_count) {
                        INDEX_ENTRY *e = ui_entry_from_row(app, sel);
                        if (!e) break;
                        wchar_t msg[1024];
                        swprintf_s(msg, 1024, L"Delete \"%s\"?", e->name);
                        if (MessageBoxW(hwnd, msg, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                            if (e->is_directory) {
                                /* Recycle bin delete */
                                SHFILEOPSTRUCTW op = {0};
                                op.wFunc = FO_DELETE;
                                op.pFrom = e->path;
                                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                                SHFileOperationW(&op);
                            } else {
                                DeleteFileW(e->path);
                            }
                        }
                    }
                }
                else if (kd->wVKey == VK_RETURN) {
                    int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < app->filtered_count) {
                        INDEX_ENTRY *e = ui_entry_from_row(app, sel);
                        if (e && e->path && e->path[0]) {
                            ShellExecuteW(hwnd, L"open", e->path, NULL, NULL, SW_SHOW);
                        }
                    }
                }
                break;
            }
            }
        }
        return 0;
    }
    
    /* ---- Context menu ---- */
    case WM_CONTEXTMENU:
    {
        HWND hwndFrom = (HWND)wParam;
        if (hwndFrom == g_hwndList) {
            int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < app->filtered_count) {
                HMENU hPopup = CreatePopupMenu();
                POINT pt;
                wchar_t *entry_name = NULL;
                wchar_t *entry_path = NULL;
                int entry_is_dir = 0;
                IContextMenu *shell_menu = NULL;
                int cmd;
                
                if (!ui_copy_entry_snapshot(app, sel, &entry_name, &entry_path, &entry_is_dir)) {
                    DestroyMenu(hPopup);
                    return 0;
                }
                
                pt.x = LOWORD(lParam);
                pt.y = HIWORD(lParam);
                if (pt.x == -1 && pt.y == -1) {
                    RECT item_rc;
                    item_rc.left = LVIR_BOUNDS;
                    if (ListView_GetItemRect(g_hwndList, sel, &item_rc, LVIR_BOUNDS)) {
                        pt.x = item_rc.left;
                        pt.y = item_rc.bottom;
                        ClientToScreen(g_hwndList, &pt);
                    } else {
                        GetCursorPos(&pt);
                    }
                }
                
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_OPEN, L"Open");
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_OPEN_PATH, L"Open Path");
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_COPY_FULL_NAME, L"Copy Full Name to Clipboard");
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_SET_RUN_COUNT, L"Set Run Count");
                AppendMenuW(hPopup, MF_SEPARATOR, 0, NULL);
                ui_append_shell_context_menu(hwnd, hPopup, entry_path, &shell_menu);
                
                cmd = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, NULL);
                
                switch (cmd) {
                case IDM_CTX_OPEN:
                    ui_open_entry_path(hwnd, entry_path);
                    break;
                case IDM_CTX_OPEN_PATH:
                    ui_open_entry_parent(hwnd, entry_path);
                    break;
                case IDM_CTX_COPY_FULL_NAME:
                    ui_copy_text_to_clipboard(hwnd, entry_path);
                    break;
                case IDM_CTX_SET_RUN_COUNT:
                    MessageBoxW(hwnd, L"Set Run Count is not implemented yet.",
                                L"OpenEverything", MB_OK | MB_ICONINFORMATION);
                    break;
                default:
                    if (cmd >= IDM_CTX_SHELL_FIRST && cmd <= IDM_CTX_SHELL_LAST)
                        ui_invoke_shell_context_command(hwnd, shell_menu, cmd, pt);
                    break;
                }
                
                if (shell_menu)
                    shell_menu->lpVtbl->Release(shell_menu);
                ui_release_shell_menu_handlers();
                free(entry_name);
                free(entry_path);
                DestroyMenu(hPopup);
            }
        }
        return 0;
    }
    
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_brush_window ? g_brush_window : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }
    
    /* ---- User-defined messages ---- */
    case WM_SEARCH_UPDATE:
    {
        ui_queue_search(hwnd);
        return 0;
    }
    
    case WM_SEARCH_DONE:
    {
        struct SearchJob *job = (struct SearchJob *)lParam;
        LONG done_generation = (LONG)wParam;
        int accepted = 0;
        
        if (job && done_generation == app->search_generation) {
            EnterCriticalSection(&app->index_lock);
            if (job->result_count <= app->entry_capacity && app->filtered_indices) {
                memcpy(app->filtered_indices, job->results, job->result_count * sizeof(int));
                app->filtered_count = job->result_count;
                app->filtered_identity = 0;
                app->query = job->query;
                accepted = 1;
            }
            
            LeaveCriticalSection(&app->index_lock);
            
            if (accepted) {
                ui_update_listview(g_hwndList, app);
                ui_update_status(g_hwndStatus, app);
            }
        }
        
        if (job) {
            free(job->base_indices);
            free(job->results);
            free(job);
        }
        
        app->is_searching = 0;
        InterlockedExchange(&g_search_running, 0);
        
        if (done_generation != app->search_generation)
            ui_start_search(hwnd);
        
        return 0;
    }
    
    case WM_INDEX_PROGRESS:
    {
        int pct = (int)wParam;
        int vol = (int)lParam;
        
        wchar_t buf[256];
        wchar_t label[16] = L"";
        if (vol >= 0 && vol < app->volume_count)
            wcscpy_s(label, 16, app->volumes[vol].drive_letter);
        if (pct > 0)
            swprintf_s(buf, 256, L"Indexing %s %d%%", label, pct);
        else
            swprintf_s(buf, 256, L"Indexing %s", label);
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)buf);
        return 0;
    }
    
    case WM_INDEX_DONE:
    {
        ui_queue_search(hwnd);
        ui_update_status(g_hwndStatus, app);
        return 0;
    }
    
    case WM_INDEX_SYNCED:
    {
        ui_queue_search(hwnd);
        ui_update_status(g_hwndStatus, app);
        return 0;
    }
    
    case WM_CACHE_LOADED:
    {
        ui_update_status(g_hwndStatus, app);
        ui_queue_search(hwnd);
        KillTimer(hwnd, IDT_STARTUP_SYNC);
        SetTimer(hwnd, IDT_STARTUP_SYNC, STARTUP_SYNC_DELAY_MS, NULL);
        return 0;
    }
    
    case WM_REFRESH:
    {
        struct ReindexCtx *ctx;
        HANDLE hThread;
        
        if (InterlockedCompareExchange(&g_reindexing, 1, 0) != 0) {
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"  Indexing already in progress");
            return 0;
        }
        
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"  Indexing...");
        InterlockedIncrement(&app->search_generation);
        app->filtered_count = 0;
        app->filtered_identity = 0;
        ui_update_listview(g_hwndList, app);
        
        ctx = (struct ReindexCtx *)calloc(1, sizeof(struct ReindexCtx));
        if (!ctx) {
            InterlockedExchange(&g_reindexing, 0);
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"  Failed to start indexing");
            return 0;
        }
        
        ctx->app = app;
        ctx->hwnd = hwnd;
        
        hThread = CreateThread(NULL, 0, reindex_thread_proc, ctx, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            free(ctx);
            InterlockedExchange(&g_reindexing, 0);
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"  Failed to start indexing");
        }
        return 0;
    }
    
    /* ---- Window close/destroy ---- */
    case WM_CLOSE:
        config_save(app);
        InterlockedExchange(&app->shutting_down, 1);
        KillTimer(hwnd, IDT_SEARCH_DEBOUNCE);
        KillTimer(hwnd, IDT_STARTUP_SYNC);
        InterlockedIncrement(&app->search_generation);
        InterlockedExchange(&g_cache_loading, 0);
        InterlockedExchange(&app->monitor_running, 0);
        if (g_cache_thread) {
            CloseHandle(g_cache_thread);
            g_cache_thread = NULL;
        }
        if (app->monitor_thread) {
            CloseHandle(app->monitor_thread);
            app->monitor_thread = NULL;
        }
        ipc_stop_server(app);
        DestroyWindow(hwnd);
        return 0;
    
    case WM_DESTROY:
        ui_free_visual_resources();
        PostQuitMessage(0);
        return 0;
    }
    
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* =============================================================
 * Search edit subclass procedure
 * ============================================================= */
static LRESULT CALLBACK search_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    APP_STATE *app = g_app_ptr;
    
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            ui_queue_search(app->hwnd_main);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            ui_queue_search(app->hwnd_main);
            return 0;
        }
        if (wParam == VK_DOWN) {
            SetFocus(app->hwnd_list);
            return 0;
        }
        break;
    
    }
    
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
