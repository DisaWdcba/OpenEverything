#include "ui.h"
#include "config.h"
#include "ipc.h"
#include "ntfs.h"
#include "search.h"
#include "index.h"
#include "cache.h"
#include "resource.h"
#include <dwmapi.h>

/* Window procedure */
static LRESULT CALLBACK everything_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK search_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclass_id, DWORD_PTR ref_data);
static LRESULT CALLBACK list_view_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR subclass_id, DWORD_PTR ref_data);
static LRESULT CALLBACK panel_header_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclass_id, DWORD_PTR ref_data);
static LRESULT CALLBACK status_bar_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclass_id, DWORD_PTR ref_data);
static LRESULT CALLBACK subfolders_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclass_id, DWORD_PTR ref_data);

static HINSTANCE g_hInst;
static HWND g_hwndSearch;
static HWND g_hwndList;
static HWND g_hwndStatus;
static HWND g_hwndFolderHeader;
static HWND g_hwndFolderTree;
static HWND g_hwndSubfolders;
static HWND g_hwndFilterHeader;
static HWND g_hwndFilterList;
static HMENU g_menu_view;
static HMENU g_menu_theme;
static APP_STATE *g_app_ptr;
static HFONT g_font_ui;
static HFONT g_font_search;
static HBRUSH g_brush_window;
static HBRUSH g_brush_panel;
static COLORREF g_color_window;
static COLORREF g_color_panel;
static COLORREF g_color_text;
static int g_theme_is_dark;
static int g_icon_file = I_IMAGENONE;
static int g_icon_folder = I_IMAGENONE;
static HANDLE g_cache_thread;
static IContextMenu2 *g_shell_menu2;
static IContextMenu3 *g_shell_menu3;
static volatile LONG g_reindexing = 0;
static volatile LONG g_search_running = 0;
static volatile LONG g_cache_loading = 0;
static volatile LONG g_metadata_running = 0;
static volatile LONG g_metadata_redraw_pending = 0;
static int g_in_size_move;
static int g_in_scroll_thumb;
static HANDLE g_metadata_thread;
static HANDLE g_metadata_event;
static CRITICAL_SECTION g_metadata_queue_lock;
static int g_metadata_queue_count;
static int g_tree_folder_icon = I_IMAGENONE;
static int g_tree_drive_icon = I_IMAGENONE;
static int g_splitter_drag;
static int g_panel_drag;
static POINT g_panel_drag_start;
static wchar_t g_status_text[256];

typedef struct {
    wchar_t path[SEARCH_FOLDER_SCOPE_MAX];
    int loaded;
    int loading;
    int is_root;
} FOLDER_NODE;

typedef struct {
    HWND owner;
    HTREEITEM item;
    FOLDER_NODE *node;
    wchar_t path[SEARCH_FOLDER_SCOPE_MAX];
} FOLDER_ENUM_JOB;

typedef struct {
    HTREEITEM item;
    FOLDER_NODE *node;
    wchar_t **names;
    int count;
} FOLDER_ENUM_RESULT;

static const wchar_t *g_filter_names[FILTER_COUNT] = {
    L"Everything", L"Audio", L"Compressed", L"Document",
    L"Executable", L"Folder", L"Image", L"Video"
};

typedef struct {
    wchar_t *extension;
    int icon_index;
} ICON_CACHE_ENTRY;

static ICON_CACHE_ENTRY *g_icon_cache;
static int g_icon_cache_count;
static int g_icon_cache_capacity;

#define UI_SEARCH_TOP 4
#define UI_SEARCH_HEIGHT 36
#define UI_SPLITTER_SIZE 6
#define IDT_SEARCH_DEBOUNCE 1
#define IDT_STARTUP_SYNC 2
#define SEARCH_DEBOUNCE_MS 120
#define STARTUP_SYNC_DELAY_MS 2500
#define USN_SYNC_MAX_CHANGES 16384
#define USN_MONITOR_INTERVAL_MS 750
#define METADATA_QUEUE_MAX 2048
#define IDM_CTX_OPEN 20001
#define IDM_CTX_OPEN_PATH 20002
#define IDM_CTX_COPY_FULL_NAME 20003
#define IDM_CTX_SET_RUN_COUNT 20004
#define IDM_CTX_SHELL_FIRST 21000
#define IDM_CTX_SHELL_LAST 24000
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092

typedef struct {
    HMENU menu;
    HDC dc;
    DWORD flags;
} UAH_MENU;

typedef struct {
    int position;
} UAH_MENU_ITEM;

typedef struct {
    DRAWITEMSTRUCT draw;
    UAH_MENU menu;
    UAH_MENU_ITEM item;
} UAH_DRAW_MENU_ITEM;

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

struct OpenPathJob {
    wchar_t *path;
    int open_parent;
};

struct MetadataJob {
    struct MetadataJob *next;
    APP_STATE *app;
    HWND hwnd;
    wchar_t *path;
    long long file_ref;
    int volume_index;
    int entry_index;
    int is_directory;
};

static struct MetadataJob *g_metadata_queue_head;

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
static DWORD WINAPI ui_open_path_thread_proc(void *p);
static IContextMenu *ui_create_shell_context_menu(HWND hwnd, const wchar_t *path);
static int ui_append_shell_context_menu(HWND hwnd, HMENU menu, const wchar_t *path,
                                        IContextMenu **out_menu);
static void ui_release_shell_menu_handlers(void);
static void ui_invoke_shell_context_command(HWND hwnd, IContextMenu *menu,
                                            int cmd, POINT pt);
static void ui_queue_row_metadata(APP_STATE *app, int row);
static int ui_start_metadata_worker(APP_STATE *app, HWND hwnd);
static DWORD WINAPI ui_metadata_thread_proc(void *p);
static void ui_format_filetime(long long ft64, wchar_t *buf, size_t buf_size);
static void ui_queue_search(HWND hwnd);
static void ui_queue_filter_search(HWND hwnd);
static void ui_apply_layout(HWND hwnd);
static int ui_splitter_hit_test(HWND hwnd, POINT point);
static void ui_dock_panel(int panel, int side);
static void ui_apply_theme(HWND hwnd);
static void ui_update_view_menu(void);
static void ui_populate_folder_roots(void);
static void ui_start_folder_enum(HTREEITEM item, FOLDER_NODE *node);
static void ui_handle_folder_enum_result(FOLDER_ENUM_RESULT *result);
static void ui_free_folder_result(FOLDER_ENUM_RESULT *result);
static void ui_hide_horizontal_scrollbar(HWND hwndList);
static void ui_update_sort_indicator(HWND hwndList, int column, int ascending);
static void ui_change_sort(HWND hwnd, APP_STATE *app, int column, int toggle_same);
static LRESULT ui_custom_draw_list_header(NMCUSTOMDRAW *draw);
static void ui_start_search(HWND hwnd);
static void ui_start_cache_load(HWND hwnd);
static int ui_search_can_refine(const SEARCH_QUERY *old_query, const SEARCH_QUERY *new_query);
static DWORD WINAPI search_thread_proc(void *p);
static DWORD WINAPI cache_load_thread_proc(void *p);
static void usn_start_startup_sync(HWND hwnd);
static DWORD WINAPI usn_startup_sync_thread_proc(void *p);
static int usn_sync_once(APP_STATE *app, int *needs_rebuild, int *changed_count);
static INDEX_ENTRY *ui_entry_from_row(APP_STATE *app, int row);
static void ui_get_parent_path(APP_STATE *app, int entry_index,
                               wchar_t *buf, size_t buf_size);

/* Reindex context */
struct ReindexCtx {
    APP_STATE *app;
    HWND hwnd;
};

static void ui_init_visual_resources(void)
{
    g_color_window = RGB(255, 255, 255);
    g_color_panel = RGB(245, 245, 245);
    g_color_text = RGB(32, 37, 45);
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
        g_brush_window = CreateSolidBrush(g_color_window);
    if (!g_brush_panel)
        g_brush_panel = CreateSolidBrush(g_color_panel);
}

static void ui_free_visual_resources(void)
{
    if (g_font_ui) { DeleteObject(g_font_ui); g_font_ui = NULL; }
    if (g_font_search) { DeleteObject(g_font_search); g_font_search = NULL; }
    if (g_brush_window) { DeleteObject(g_brush_window); g_brush_window = NULL; }
    if (g_brush_panel) { DeleteObject(g_brush_panel); g_brush_panel = NULL; }
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

static void ui_get_parent_path(APP_STATE *app, int entry_index,
                               wchar_t *buf, size_t buf_size)
{
    wchar_t *path = index_duplicate_entry_path_locked(app, entry_index);
    if (!path || !path[0]) {
        wcscpy_s(buf, buf_size, L"");
        free(path);
        return;
    }
    
    wcsncpy_s(buf, buf_size, path, _TRUNCATE);
    free(path);
    wchar_t *last = wcsrchr(buf, L'\\');
    if (last)
        *last = L'\0';
}

static void ui_reset_queued_metadata(APP_STATE *app, int entry_index,
                                     long long file_ref, int volume_index)
{
    EnterCriticalSection(&app->index_lock);
    if (entry_index >= 0 && entry_index < app->entry_count) {
        INDEX_ENTRY *entry = &app->entries[entry_index];
        if (entry->file_ref == file_ref && entry->volume_index == volume_index)
            entry->metadata_queued = 0;
    }
    LeaveCriticalSection(&app->index_lock);
}

static DWORD WINAPI ui_metadata_thread_proc(void *p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    
    while (g_metadata_running) {
        struct MetadataJob *job;
        WIN32_FILE_ATTRIBUTE_DATA data;
        ULARGE_INTEGER size;
        int loaded;
        int updated = 0;
        
        WaitForSingleObject(g_metadata_event, INFINITE);
        if (!g_metadata_running)
            break;
        
        for (;;) {
            EnterCriticalSection(&g_metadata_queue_lock);
            job = g_metadata_queue_head;
            if (job) {
                g_metadata_queue_head = job->next;
                g_metadata_queue_count--;
            }
            LeaveCriticalSection(&g_metadata_queue_lock);
            if (!job)
                break;
            
            updated = 0;
            memset(&data, 0, sizeof(data));
            loaded = GetFileAttributesExW(job->path, GetFileExInfoStandard, &data) ? 1 : 0;
            
            EnterCriticalSection(&job->app->index_lock);
            if (job->entry_index >= 0 && job->entry_index < job->app->entry_count) {
                INDEX_ENTRY *entry = &job->app->entries[job->entry_index];
                wchar_t *current_path = index_duplicate_entry_path_locked(
                    job->app, job->entry_index);
                if (entry->file_ref == job->file_ref &&
                    entry->volume_index == job->volume_index &&
                    current_path && wcscmp(current_path, job->path) == 0) {
                    if (loaded) {
                        entry->attributes = data.dwFileAttributes;
                        entry->creation_time = ((long long)data.ftCreationTime.dwHighDateTime << 32) |
                                               data.ftCreationTime.dwLowDateTime;
                        entry->modification_time = ((long long)data.ftLastWriteTime.dwHighDateTime << 32) |
                                                   data.ftLastWriteTime.dwLowDateTime;
                        size.LowPart = data.nFileSizeLow;
                        size.HighPart = data.nFileSizeHigh;
                        entry->size = job->is_directory ? 0 : (long long)size.QuadPart;
                    } else {
                        entry->size = 0;
                        entry->creation_time = 0;
                        entry->modification_time = 0;
                    }
                    entry->metadata_loaded = 1;
                    entry->metadata_queued = 0;
                    updated = 1;
                }
                free(current_path);
            }
            LeaveCriticalSection(&job->app->index_lock);
            
            if (updated &&
                InterlockedCompareExchange(&g_metadata_redraw_pending, 1, 0) == 0) {
                if (!PostMessageW(job->hwnd, WM_METADATA_READY, 0, 0))
                    InterlockedExchange(&g_metadata_redraw_pending, 0);
            }
            free(job->path);
            free(job);
        }
    }
    return 0;
}

static int ui_start_metadata_worker(APP_STATE *app, HWND hwnd)
{
    (void)app;
    (void)hwnd;
    if (g_metadata_thread)
        return 1;
    
    InitializeCriticalSection(&g_metadata_queue_lock);
    g_metadata_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_metadata_event)
        return 0;
    InterlockedExchange(&g_metadata_running, 1);
    g_metadata_thread = CreateThread(NULL, 0, ui_metadata_thread_proc, NULL, 0, NULL);
    if (!g_metadata_thread) {
        InterlockedExchange(&g_metadata_running, 0);
        CloseHandle(g_metadata_event);
        g_metadata_event = NULL;
        return 0;
    }
    return 1;
}

static void ui_queue_row_metadata(APP_STATE *app, int row)
{
    struct MetadataJob *job = NULL;
    INDEX_ENTRY *entry;
    wchar_t *path = NULL;
    long long file_ref = 0;
    int volume_index = -1;
    int entry_index = -1;
    int is_directory = 0;
    int queue_full = 0;
    
    if (!app || row < 0 || !g_metadata_running || !g_metadata_event)
        return;
    
    EnterCriticalSection(&app->index_lock);
    entry = ui_entry_from_row(app, row);
    if (!entry || entry->metadata_loaded || entry->metadata_queued) {
        LeaveCriticalSection(&app->index_lock);
        return;
    }
    entry_index = (int)(entry - app->entries);
    path = index_duplicate_entry_path_locked(app, entry_index);
    if (!path) {
        LeaveCriticalSection(&app->index_lock);
        return;
    }
    file_ref = entry->file_ref;
    volume_index = entry->volume_index;
    is_directory = entry->is_directory;
    entry->metadata_queued = 1;
    LeaveCriticalSection(&app->index_lock);
    
    job = (struct MetadataJob *)calloc(1, sizeof(*job));
    if (!job) {
        ui_reset_queued_metadata(app, entry_index, file_ref, volume_index);
        free(path);
        return;
    }
    job->path = path;
    job->app = app;
    job->hwnd = app->hwnd_main;
    job->file_ref = file_ref;
    job->volume_index = volume_index;
    job->entry_index = entry_index;
    job->is_directory = is_directory;
    
    EnterCriticalSection(&g_metadata_queue_lock);
    if (g_metadata_queue_count >= METADATA_QUEUE_MAX) {
        queue_full = 1;
    } else {
        job->next = g_metadata_queue_head;
        g_metadata_queue_head = job;
        g_metadata_queue_count++;
    }
    LeaveCriticalSection(&g_metadata_queue_lock);
    
    if (queue_full) {
        ui_reset_queued_metadata(app, job->entry_index,
                                 job->file_ref, job->volume_index);
        free(job->path);
        free(job);
        return;
    }
    SetEvent(g_metadata_event);
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
        int entry_index = (int)(entry - app->entries);
        if (out_name)
            *out_name = _wcsdup(entry->name ? entry->name : L"");
        if (out_path)
            *out_path = index_duplicate_entry_path_locked(app, entry_index);
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

static DWORD WINAPI ui_open_path_thread_proc(void *p)
{
    struct OpenPathJob *job = (struct OpenPathJob *)p;
    HRESULT com_result;
    SHELLEXECUTEINFOW info;
    wchar_t *target;
    wchar_t *last;
    
    if (!job)
        return 0;
    
    target = job->path;
    if (job->open_parent && target) {
        last = wcsrchr(target, L'\\');
        if (last && last > target) {
            if (last == target + 2 && target[1] == L':')
                last[1] = L'\0';
            else
                *last = L'\0';
        }
    }
    
    com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_ASYNCOK;
    info.lpVerb = L"open";
    info.lpFile = target;
    info.nShow = SW_SHOWNORMAL;
    if (target && target[0])
        ShellExecuteExW(&info);
    if (SUCCEEDED(com_result))
        CoUninitialize();
    
    free(job->path);
    free(job);
    return 0;
}

static void ui_queue_open_path(const wchar_t *path, int open_parent)
{
    struct OpenPathJob *job;
    HANDLE thread;
    
    if (!path || !path[0])
        return;
    
    job = (struct OpenPathJob *)calloc(1, sizeof(*job));
    if (!job)
        return;
    job->path = _wcsdup(path);
    job->open_parent = open_parent;
    if (!job->path) {
        free(job);
        return;
    }
    
    thread = CreateThread(NULL, 0, ui_open_path_thread_proc, job, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else {
        free(job->path);
        free(job);
    }
}

static void ui_open_entry_path(HWND hwnd, const wchar_t *path)
{
    (void)hwnd;
    ui_queue_open_path(path, 0);
}

static void ui_open_entry_parent(HWND hwnd, const wchar_t *path)
{
    (void)hwnd;
    ui_queue_open_path(path, 1);
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
        old_query->filter_id != new_query->filter_id ||
        old_query->include_subfolders != new_query->include_subfolders ||
        _wcsicmp(old_query->folder_scope, new_query->folder_scope) != 0 ||
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
    
    loaded = cache_load_index(ctx->app);
    if (loaded && !ctx->app->shutting_down) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        index_build_filter_index(ctx->app);
        index_build_ref_index(ctx->app);
    }
    InterlockedExchange(&g_cache_loading, 0);
    if (!ctx->app->shutting_down)
        PostMessageW(ctx->hwnd, loaded ? WM_CACHE_LOADED : WM_REFRESH,
                     (WPARAM)loaded, 0);
    if (loaded == CACHE_LOAD_LEGACY && !ctx->app->shutting_down) {
        cache_save_index(ctx->app);
        if (!ctx->app->shutting_down)
            PostMessageW(ctx->hwnd, WM_CACHE_UPGRADE_DONE, 0, 0);
    }
    if (loaded && !ctx->app->shutting_down) {
        index_build_name_char_index(ctx->app);
    }
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
    int search_limit;
    
    if (!app || InterlockedCompareExchange(&g_search_running, 1, 0) != 0)
        return;
    
    memset(&next_query, 0, sizeof(next_query));
    GetWindowTextW(g_hwndSearch, next_query.text, 512);
    next_query.match_case = app->match_case;
    next_query.match_whole_word = app->match_whole_word;
    next_query.match_path = app->match_path;
    next_query.use_regex = app->use_regex;
    next_query.filter_id = app->selected_filter;
    next_query.include_subfolders = app->include_subfolders;
    wcscpy_s(next_query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, app->folder_scope);
    next_query.sort_column = app->query.sort_column;
    next_query.sort_ascending = app->query.sort_ascending;
    search_prepare_query(&next_query);
    
    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    LeaveCriticalSection(&app->index_lock);
    
    job = (struct SearchJob *)calloc(1, sizeof(struct SearchJob));
    if (!job) {
        InterlockedExchange(&g_search_running, 0);
        return;
    }
    
    EnterCriticalSection(&app->index_lock);
    entry_count = app->entry_count;
    if (!app->filtered_stale &&
        ui_search_can_refine(&app->query, &next_query) && app->filtered_count > 0) {
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

static void ui_queue_filter_search(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    if (!app)
        return;

    InterlockedIncrement(&app->search_generation);
    KillTimer(hwnd, IDT_SEARCH_DEBOUNCE);

    EnterCriticalSection(&app->index_lock);
    app->filtered_count = 0;
    app->filtered_identity = 0;
    app->filtered_stale = 0;
    LeaveCriticalSection(&app->index_lock);
    ui_update_listview(g_hwndList, app);
    SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"Searching...");

    if (InterlockedCompareExchange(&g_search_running, 0, 0) == 0)
        ui_start_search(hwnd);
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
                                                     &next_usn, USN_SYNC_MAX_CHANGES);
            if (!read_changes) {
                ntfs_close_volume(hVol);
                continue;
            }
            
            if (change_count > 0) {
                InterlockedIncrement(&app->search_generation);
                for (int offset = 0; offset < change_count && !app->shutting_down;
                     offset += 512) {
                    int batch = change_count - offset;
                    if (batch > 512)
                        batch = 512;
                    total_applied += index_apply_usn_changes(
                        app, changes + offset, batch);
                    if (offset + batch < change_count)
                        Sleep(1);
                }
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
    int idle_cycles = 0;
    int filter_index_pending = 0;
    
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    
    while (app->monitor_running && !app->shutting_down) {
        int needs_rebuild = 0;
        int changed_count = 0;
        int sync_ok = usn_sync_once(app, &needs_rebuild, &changed_count);
        
        if (!sync_ok || needs_rebuild)
            break;
        if (changed_count > 0) {
            idle_cycles = 0;
            filter_index_pending = !index_build_filter_index(app);
            PostMessageW(hwnd, WM_INDEX_SYNCED, (WPARAM)changed_count, 0);
        } else {
            int name_index_ready;
            idle_cycles++;
            if (filter_index_pending && index_build_filter_index(app))
                filter_index_pending = 0;
            EnterCriticalSection(&app->index_lock);
            name_index_ready = app->name_char_index_ready;
            LeaveCriticalSection(&app->index_lock);
            if (idle_cycles >= 4 && !name_index_ready) {
                index_build_name_char_index(app);
                idle_cycles = 0;
            }
        }
        
        for (int waited = 0;
             waited < USN_MONITOR_INTERVAL_MS &&
             app->monitor_running && !app->shutting_down;
             waited += 50) {
            Sleep(50);
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
    
    if (a->monitor_thread) {
        WaitForSingleObject(a->monitor_thread, INFINITE);
        CloseHandle(a->monitor_thread);
        a->monitor_thread = NULL;
    }
    
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
    index_compact_entry_names(a);
    index_build_filter_index(a);
    index_build_ref_index(a);
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
    wc.style = 0;
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
    ui_hide_horizontal_scrollbar(hwndList);
    InvalidateRect(hwndList, NULL, FALSE);
}

static void ui_update_listview_count(HWND hwndList, APP_STATE *app)
{
    ListView_SetItemCountEx(hwndList, app->filtered_count,
                            LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    ui_hide_horizontal_scrollbar(hwndList);
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

static FOLDER_NODE *ui_folder_node_new(const wchar_t *path, int is_root)
{
    FOLDER_NODE *node = (FOLDER_NODE *)calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    wcsncpy_s(node->path, SEARCH_FOLDER_SCOPE_MAX, path ? path : L"", _TRUNCATE);
    node->is_root = is_root;
    node->loaded = is_root;
    return node;
}

static HTREEITEM ui_tree_insert(HTREEITEM parent, const wchar_t *text,
                                FOLDER_NODE *node, int image, int has_children)
{
    TVINSERTSTRUCTW insert;
    HTREEITEM item;
    memset(&insert, 0, sizeof(insert));
    insert.hParent = parent;
    insert.hInsertAfter = TVI_SORT;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    if (image != I_IMAGENONE)
        insert.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText = (wchar_t *)text;
    insert.item.lParam = (LPARAM)node;
    insert.item.cChildren = has_children ? 1 : 0;
    insert.item.iImage = image;
    insert.item.iSelectedImage = image;
    item = TreeView_InsertItem(g_hwndFolderTree, &insert);
    if (!item)
        free(node);
    return item;
}

static void ui_populate_folder_roots(void)
{
    SHFILEINFOW sfi;
    HIMAGELIST images;
    wchar_t drives[512];
    wchar_t *drive;
    HTREEITEM root;

    TreeView_DeleteAllItems(g_hwndFolderTree);
    memset(&sfi, 0, sizeof(sfi));
    images = (HIMAGELIST)SHGetFileInfoW(
        L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (images) {
        TreeView_SetImageList(g_hwndFolderTree, images, TVSIL_NORMAL);
        g_tree_folder_icon = sfi.iIcon;
    }
    root = ui_tree_insert(TVI_ROOT, L"Everything",
                          ui_folder_node_new(L"", 1), g_tree_folder_icon, 1);
    drive = drives;
    if (GetLogicalDriveStringsW(512, drives)) {
        while (*drive) {
            UINT type = GetDriveTypeW(drive);
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
                memset(&sfi, 0, sizeof(sfi));
                if (SHGetFileInfoW(drive, 0, &sfi, sizeof(sfi),
                                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                    g_tree_drive_icon = sfi.iIcon;
                ui_tree_insert(root, drive, ui_folder_node_new(drive, 0),
                               g_tree_drive_icon, 1);
            }
            drive += wcslen(drive) + 1;
        }
    }
    TreeView_Expand(g_hwndFolderTree, root, TVE_EXPAND);
    TreeView_SelectItem(g_hwndFolderTree, root);
}

static int __cdecl ui_compare_folder_names(const void *lhs, const void *rhs)
{
    const wchar_t *a = *(const wchar_t *const *)lhs;
    const wchar_t *b = *(const wchar_t *const *)rhs;
    return _wcsicmp(a, b);
}

static void ui_free_folder_result(FOLDER_ENUM_RESULT *result)
{
    if (!result)
        return;
    for (int i = 0; i < result->count; i++)
        free(result->names[i]);
    free(result->names);
    free(result);
}

static DWORD WINAPI ui_folder_enum_thread_proc(void *parameter)
{
    FOLDER_ENUM_JOB *job = (FOLDER_ENUM_JOB *)parameter;
    FOLDER_ENUM_RESULT *result;
    WIN32_FIND_DATAW data;
    wchar_t pattern[SEARCH_FOLDER_SCOPE_MAX];
    HANDLE find;
    int capacity = 0;
    size_t path_len;

    result = (FOLDER_ENUM_RESULT *)calloc(1, sizeof(*result));
    if (!result) {
        free(job);
        return 0;
    }
    result->item = job->item;
    result->node = job->node;
    path_len = wcslen(job->path);
    swprintf_s(pattern, SEARCH_FOLDER_SCOPE_MAX, L"%ls%ls*",
               job->path, path_len && job->path[path_len - 1] == L'\\' ? L"" : L"\\");
    find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *name;
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;
            if (result->count >= capacity) {
                int next_capacity = capacity ? capacity * 2 : 32;
                wchar_t **next = (wchar_t **)realloc(
                    result->names, (size_t)next_capacity * sizeof(wchar_t *));
                if (!next)
                    break;
                result->names = next;
                capacity = next_capacity;
            }
            name = _wcsdup(data.cFileName);
            if (name)
                result->names[result->count++] = name;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    if (result->count > 1)
        qsort(result->names, result->count, sizeof(wchar_t *), ui_compare_folder_names);
    if (!g_app_ptr || g_app_ptr->shutting_down ||
        !PostMessageW(job->owner, WM_FOLDER_ENUM_READY, 0, (LPARAM)result))
        ui_free_folder_result(result);
    free(job);
    return 0;
}

static void ui_start_folder_enum(HTREEITEM item, FOLDER_NODE *node)
{
    FOLDER_ENUM_JOB *job;
    HANDLE thread;
    if (!node || node->loaded || node->loading || !node->path[0])
        return;
    node->loading = 1;
    job = (FOLDER_ENUM_JOB *)calloc(1, sizeof(*job));
    if (!job) {
        node->loading = 0;
        return;
    }
    job->owner = g_app_ptr->hwnd_main;
    job->item = item;
    job->node = node;
    wcscpy_s(job->path, SEARCH_FOLDER_SCOPE_MAX, node->path);
    thread = CreateThread(NULL, 0, ui_folder_enum_thread_proc, job, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else {
        node->loading = 0;
        free(job);
    }
}

static void ui_handle_folder_enum_result(FOLDER_ENUM_RESULT *result)
{
    TVITEMW item;
    if (!result)
        return;
    memset(&item, 0, sizeof(item));
    item.mask = TVIF_PARAM;
    item.hItem = result->item;
    if (!TreeView_GetItem(g_hwndFolderTree, &item) ||
        (FOLDER_NODE *)item.lParam != result->node) {
        ui_free_folder_result(result);
        return;
    }
    result->node->loaded = 1;
    result->node->loading = 0;
    item.mask = TVIF_CHILDREN;
    item.cChildren = result->count > 0 ? 1 : 0;
    TreeView_SetItem(g_hwndFolderTree, &item);
    for (int i = 0; i < result->count; i++) {
        wchar_t path[SEARCH_FOLDER_SCOPE_MAX];
        size_t path_len = wcslen(result->node->path);
        swprintf_s(path, SEARCH_FOLDER_SCOPE_MAX, L"%ls%ls%ls",
                   result->node->path,
                   path_len && result->node->path[path_len - 1] == L'\\' ? L"" : L"\\",
                   result->names[i]);
        ui_tree_insert(result->item, result->names[i],
                       ui_folder_node_new(path, 0), g_tree_folder_icon, 1);
    }
    ui_free_folder_result(result);
}

static int ui_system_theme_is_dark(void)
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
}

static void ui_update_view_menu(void)
{
    if (!g_menu_view || !g_app_ptr)
        return;
    CheckMenuItem(g_menu_view, IDM_VIEW_FOLDERS,
                  MF_BYCOMMAND | (g_app_ptr->show_folders ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu_view, IDM_VIEW_FILTERS,
                  MF_BYCOMMAND | (g_app_ptr->show_filters ? MF_CHECKED : MF_UNCHECKED));
    if (g_menu_theme)
        CheckMenuRadioItem(g_menu_theme, IDM_VIEW_THEME_LIGHT, IDM_VIEW_THEME_SYSTEM,
                           g_app_ptr->theme_mode == THEME_LIGHT ? IDM_VIEW_THEME_LIGHT :
                           g_app_ptr->theme_mode == THEME_DARK ? IDM_VIEW_THEME_DARK :
                                                                  IDM_VIEW_THEME_SYSTEM,
                           MF_BYCOMMAND);
}

static void ui_apply_theme(HWND hwnd)
{
    typedef int (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)(void);
    typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
    HMODULE uxtheme;
    SetPreferredAppModeFn set_preferred_mode;
    FlushMenuThemesFn flush_menu_themes;
    AllowDarkModeForWindowFn allow_dark_window;
    BOOL dark_title;
    int dark;
    HWND themed[] = {
        g_hwndSearch, g_hwndList, ListView_GetHeader(g_hwndList), g_hwndStatus,
        g_hwndFolderTree, g_hwndFilterList, g_hwndSubfolders,
        g_hwndFolderHeader, g_hwndFilterHeader
    };

    if (!g_app_ptr)
        return;
    dark = g_app_ptr->theme_mode == THEME_DARK ||
           (g_app_ptr->theme_mode == THEME_SYSTEM && ui_system_theme_is_dark());
    g_theme_is_dark = dark;
    g_color_window = dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    g_color_panel = dark ? RGB(31, 31, 31) : RGB(245, 245, 245);
    g_color_text = dark ? RGB(245, 245, 245) : RGB(32, 37, 45);
    uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (uxtheme) {
        set_preferred_mode = (SetPreferredAppModeFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)135);
        flush_menu_themes = (FlushMenuThemesFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)136);
        allow_dark_window = (AllowDarkModeForWindowFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)133);
        if (set_preferred_mode)
            set_preferred_mode(dark ? 2 : 3);
        if (allow_dark_window) {
            allow_dark_window(hwnd, dark);
            for (int i = 0; i < (int)(sizeof(themed) / sizeof(themed[0])); i++) {
                if (themed[i])
                    allow_dark_window(themed[i], dark);
            }
        }
        if (flush_menu_themes)
            flush_menu_themes();
        FreeLibrary(uxtheme);
    }
    if (g_brush_window) DeleteObject(g_brush_window);
    if (g_brush_panel) DeleteObject(g_brush_panel);
    g_brush_window = CreateSolidBrush(g_color_window);
    g_brush_panel = CreateSolidBrush(g_color_panel);
    for (int i = 0; i < (int)(sizeof(themed) / sizeof(themed[0])); i++) {
        if (themed[i])
            SetWindowTheme(themed[i], dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    }
    SetWindowTheme(g_hwndStatus, L"", NULL);
    SetWindowTheme(g_hwndSubfolders, dark ? L"" : L"Explorer", NULL);
    ListView_SetBkColor(g_hwndList, g_color_window);
    ListView_SetTextBkColor(g_hwndList, g_color_window);
    ListView_SetTextColor(g_hwndList, g_color_text);
    if (g_hwndFolderTree) {
        TreeView_SetBkColor(g_hwndFolderTree, g_color_window);
        TreeView_SetTextColor(g_hwndFolderTree, g_color_text);
    }
    InvalidateRect(ListView_GetHeader(g_hwndList), NULL, TRUE);
    InvalidateRect(g_hwndStatus, NULL, TRUE);
    InvalidateRect(g_hwndSubfolders, NULL, TRUE);
    dark_title = dark;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark_title, sizeof(dark_title))))
        DwmSetWindowAttribute(hwnd, 19, &dark_title, sizeof(dark_title));
    ui_update_view_menu();
    DrawMenuBar(hwnd);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void ui_apply_layout(HWND hwnd)
{
    RECT rc;
    RECT rc_status;
    int status_h = 22;
    int list_top = UI_SEARCH_TOP + UI_SEARCH_HEIGHT;
    int content_bottom;
    int gap = UI_SPLITTER_SIZE;
    int header_h = 30;
    int check_h = 30;
    int left_panel = 0;
    int right_panel = 0;
    int left_width = 0;
    int right_width = 0;
    int visible_count = 0;
    int max_panel_total;
    int list_left;
    int list_right;
    APP_STATE *app = g_app_ptr;
    
    if (!g_hwndSearch || !g_hwndList || !g_hwndStatus)
        return;
    GetClientRect(hwnd, &rc);
    SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
    GetWindowRect(g_hwndStatus, &rc_status);
    status_h = rc_status.bottom - rc_status.top;
    if (status_h <= 0)
        status_h = 22;
    
    content_bottom = rc.bottom - status_h;
    if (app && app->show_folders && app->show_filters &&
        app->folder_panel_side == app->filter_panel_side) {
        app->filter_panel_side = app->folder_panel_side == PANEL_DOCK_LEFT
            ? PANEL_DOCK_RIGHT : PANEL_DOCK_LEFT;
    }
    if (app && app->show_folders) {
        visible_count++;
        if (app->folder_panel_side == PANEL_DOCK_LEFT) {
            left_panel = 1;
            left_width = app->folder_panel_width;
        } else {
            right_panel = 1;
            right_width = app->folder_panel_width;
        }
    }
    if (app && app->show_filters) {
        visible_count++;
        if (app->filter_panel_side == PANEL_DOCK_LEFT) {
            left_panel = 2;
            left_width = app->filter_panel_width;
        } else {
            right_panel = 2;
            right_width = app->filter_panel_width;
        }
    }

    max_panel_total = rc.right - 280 - visible_count * gap;
    if (max_panel_total < visible_count * 160)
        max_panel_total = visible_count * 160;
    if (left_panel && right_panel && left_width + right_width > max_panel_total) {
        int available = max_panel_total / 2;
        left_width = available;
        right_width = max_panel_total - available;
    } else if (left_panel && left_width > max_panel_total) {
        left_width = max_panel_total;
    } else if (right_panel && right_width > max_panel_total) {
        right_width = max_panel_total;
    }
    if (left_panel && left_width < 160) left_width = 160;
    if (right_panel && right_width < 160) right_width = 160;

    MoveWindow(g_hwndSearch, 4, UI_SEARCH_TOP,
               rc.right - 8, UI_SEARCH_HEIGHT - 8, TRUE);
    list_left = left_panel ? left_width + gap : 0;
    list_right = right_panel ? rc.right - right_width - gap : rc.right;
    if (list_right < list_left)
        list_right = list_left;
    MoveWindow(g_hwndList, list_left, list_top,
               list_right - list_left, content_bottom - list_top, TRUE);

    ShowWindow(g_hwndFolderHeader, app && app->show_folders ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_hwndFolderTree, app && app->show_folders ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_hwndSubfolders, app && app->show_folders ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_hwndFilterHeader, app && app->show_filters ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_hwndFilterList, app && app->show_filters ? SW_SHOWNA : SW_HIDE);

    if (app && app->show_folders) {
        int width = app->folder_panel_side == PANEL_DOCK_LEFT
            ? left_width : right_width;
        int x = app->folder_panel_side == PANEL_DOCK_LEFT ? 0 : rc.right - width;
        int tree_height = content_bottom - list_top - header_h - check_h;
        if (tree_height < 0) tree_height = 0;
        MoveWindow(g_hwndFolderHeader, x, list_top, width, header_h, TRUE);
        MoveWindow(g_hwndFolderTree, x, list_top + header_h,
                   width, tree_height, TRUE);
        MoveWindow(g_hwndSubfolders, x, content_bottom - check_h,
                   width, check_h, TRUE);
    }
    if (app && app->show_filters) {
        int width = app->filter_panel_side == PANEL_DOCK_LEFT
            ? left_width : right_width;
        int x = app->filter_panel_side == PANEL_DOCK_LEFT ? 0 : rc.right - width;
        MoveWindow(g_hwndFilterHeader, x, list_top, width, header_h, TRUE);
        MoveWindow(g_hwndFilterList, x, list_top + header_h,
                   width, content_bottom - list_top - header_h, TRUE);
    }
    ui_hide_horizontal_scrollbar(g_hwndList);
}

static int ui_splitter_hit_test(HWND hwnd, POINT point)
{
    RECT list_rect;
    APP_STATE *app = g_app_ptr;

    if (!app || (!app->show_folders && !app->show_filters))
        return 0;

    GetWindowRect(g_hwndList, &list_rect);
    MapWindowPoints(NULL, hwnd, (POINT *)&list_rect, 2);
    if (point.y >= list_rect.top && point.y < list_rect.bottom &&
        point.x >= list_rect.left - UI_SPLITTER_SIZE && point.x < list_rect.left)
        return 1;
    if (point.y >= list_rect.top && point.y < list_rect.bottom &&
        point.x >= list_rect.right && point.x < list_rect.right + UI_SPLITTER_SIZE)
        return 2;
    return 0;
}

static void ui_dock_panel(int panel, int side)
{
    APP_STATE *app = g_app_ptr;
    int *moving;
    int *other;

    if (!app || (panel != 1 && panel != 2) ||
        (side != PANEL_DOCK_LEFT && side != PANEL_DOCK_RIGHT))
        return;
    moving = panel == 1 ? &app->folder_panel_side : &app->filter_panel_side;
    other = panel == 1 ? &app->filter_panel_side : &app->folder_panel_side;
    if (*moving == side)
        return;
    if (app->show_folders && app->show_filters && *other == side)
        *other = side == PANEL_DOCK_LEFT ? PANEL_DOCK_RIGHT : PANEL_DOCK_LEFT;
    *moving = side;
    ui_apply_layout(app->hwnd_main);
}

static void ui_hide_horizontal_scrollbar(HWND hwndList)
{
    if (hwndList)
        ShowScrollBar(hwndList, SB_HORZ, FALSE);
}

static void ui_update_sort_indicator(HWND hwndList, int column, int ascending)
{
    HWND header;
    int count;

    if (!hwndList)
        return;
    header = ListView_GetHeader(hwndList);
    if (!header)
        return;

    count = Header_GetItemCount(header);
    for (int i = 0; i < count; i++) {
        HDITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = HDI_FORMAT;
        if (!SendMessageW(header, HDM_GETITEMW, i, (LPARAM)&item))
            continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == column)
            item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        SendMessageW(header, HDM_SETITEMW, i, (LPARAM)&item);
    }
}

static void ui_change_sort(HWND hwnd, APP_STATE *app, int column, int toggle_same)
{
    if (!app || column < COL_NAME || column > COL_ATTRIBUTES)
        return;

    if (toggle_same && app->query.sort_column == column)
        app->query.sort_ascending = !app->query.sort_ascending;
    else {
        app->query.sort_column = column;
        app->query.sort_ascending = 1;
    }

    ui_update_sort_indicator(g_hwndList, app->query.sort_column,
                             app->query.sort_ascending);
    ui_queue_search(hwnd);
}

static LRESULT ui_custom_draw_list_header(NMCUSTOMDRAW *draw)
{
    if (!g_theme_is_dark)
        return CDRF_DODEFAULT;
    if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
        wchar_t text[128] = L"";
        HDITEMW item;
        RECT rc = draw->rc;
        RECT text_rc = rc;
        HBRUSH background;
        HPEN separator;
        HGDIOBJ old_pen;
        HGDIOBJ old_font;
        UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;

        memset(&item, 0, sizeof(item));
        item.mask = HDI_TEXT | HDI_FORMAT;
        item.pszText = text;
        item.cchTextMax = (int)(sizeof(text) / sizeof(text[0]));
        Header_GetItem(draw->hdr.hwndFrom, (int)draw->dwItemSpec, &item);

        background = CreateSolidBrush((draw->uItemState & CDIS_HOT)
                                      ? RGB(48, 48, 48) : g_color_panel);
        FillRect(draw->hdc, &rc, background);
        DeleteObject(background);

        text_rc.left += 8;
        text_rc.right -= (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) ? 24 : 8;
        if (item.fmt & HDF_RIGHT)
            format |= DT_RIGHT;
        else if (item.fmt & HDF_CENTER)
            format |= DT_CENTER;
        else
            format |= DT_LEFT;
        SetBkMode(draw->hdc, TRANSPARENT);
        SetTextColor(draw->hdc, g_color_text);
        old_font = SelectObject(draw->hdc,
                                g_font_ui ? g_font_ui : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(draw->hdc, text, -1, &text_rc, format);
        SelectObject(draw->hdc, old_font);

        if (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
            POINT triangle[3];
            int x = rc.right - 12;
            int y = (rc.top + rc.bottom) / 2;
            HBRUSH arrow = CreateSolidBrush(g_color_text);
            HRGN region;
            if (item.fmt & HDF_SORTUP) {
                triangle[0].x = x; triangle[0].y = y - 3;
                triangle[1].x = x - 4; triangle[1].y = y + 2;
                triangle[2].x = x + 4; triangle[2].y = y + 2;
            } else {
                triangle[0].x = x; triangle[0].y = y + 3;
                triangle[1].x = x - 4; triangle[1].y = y - 2;
                triangle[2].x = x + 4; triangle[2].y = y - 2;
            }
            region = CreatePolygonRgn(triangle, 3, WINDING);
            FillRgn(draw->hdc, region, arrow);
            DeleteObject(region);
            DeleteObject(arrow);
        }

        separator = CreatePen(PS_SOLID, 1, RGB(68, 68, 68));
        old_pen = SelectObject(draw->hdc, separator);
        MoveToEx(draw->hdc, rc.right - 1, rc.top, NULL);
        LineTo(draw->hdc, rc.right - 1, rc.bottom);
        SelectObject(draw->hdc, old_pen);
        DeleteObject(separator);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* =============================================================
 * Main window procedure
 * ============================================================= */
static LRESULT CALLBACK everything_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    APP_STATE *app = g_app_ptr;
    
    switch (msg) {
    case WM_UAHDRAWMENU:
        if (g_theme_is_dark && lParam) {
            UAH_MENU *menu = (UAH_MENU *)lParam;
            MENUBARINFO info;
            RECT window_rect;
            RECT bar_rect;
            memset(&info, 0, sizeof(info));
            info.cbSize = sizeof(info);
            if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info) &&
                GetWindowRect(hwnd, &window_rect)) {
                bar_rect = info.rcBar;
                OffsetRect(&bar_rect, -window_rect.left, -window_rect.top);
                FillRect(menu->dc, &bar_rect, g_brush_panel);
            }
            return 0;
        }
        break;

    case WM_UAHDRAWMENUITEM:
        if (g_theme_is_dark && lParam) {
            UAH_DRAW_MENU_ITEM *draw = (UAH_DRAW_MENU_ITEM *)lParam;
            MENUITEMINFOW item;
            wchar_t text[128] = L"";
            RECT text_rect = draw->draw.rcItem;
            HBRUSH background;
            HGDIOBJ old_font;
            memset(&item, 0, sizeof(item));
            item.cbSize = sizeof(item);
            item.fMask = MIIM_STRING;
            item.dwTypeData = text;
            item.cch = (UINT)(sizeof(text) / sizeof(text[0]));
            GetMenuItemInfoW(draw->menu.menu, draw->item.position, TRUE, &item);
            background = CreateSolidBrush(
                draw->draw.itemState & (ODS_HOTLIGHT | ODS_SELECTED)
                ? RGB(55, 55, 55) : g_color_panel);
            FillRect(draw->draw.hDC, &draw->draw.rcItem, background);
            DeleteObject(background);
            SetBkMode(draw->draw.hDC, TRANSPARENT);
            SetTextColor(draw->draw.hDC, g_color_text);
            old_font = SelectObject(draw->draw.hDC,
                                    g_font_ui ? g_font_ui : GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(draw->draw.hDC, text, -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(draw->draw.hDC, old_font);
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        ui_update_view_menu();
        /* fall through */
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
        HMENU hTheme = CreatePopupMenu();
        
        AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"Exit\tAlt+F4");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY, L"Copy\tCtrl+C");
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_COPY_PATH, L"Copy Full Path\tCtrl+Shift+C");
        AppendMenuW(hEdit, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hEdit, MF_STRING, IDM_EDIT_SELECT_ALL, L"Select All\tCtrl+A");
        
        /* View panels and theme */
        g_menu_view = hView;
        g_menu_theme = hTheme;
        AppendMenuW(hView, MF_STRING | (app->show_folders ? MF_CHECKED : 0),
                    IDM_VIEW_FOLDERS, L"Folders");
        AppendMenuW(hView, MF_STRING | (app->show_filters ? MF_CHECKED : 0),
                    IDM_VIEW_FILTERS, L"Filters");
        AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hTheme, MF_STRING, IDM_VIEW_THEME_LIGHT, L"Light");
        AppendMenuW(hTheme, MF_STRING, IDM_VIEW_THEME_DARK, L"Dark");
        AppendMenuW(hTheme, MF_STRING, IDM_VIEW_THEME_SYSTEM, L"System");
        AppendMenuW(hView, MF_POPUP, (UINT_PTR)hTheme, L"Themes");
        AppendMenuW(hView, MF_SEPARATOR, 0, NULL);

        /* Existing search options */
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
        SetWindowSubclass(g_hwndList, list_view_proc, 1, 0);
        
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
        
        col.pszText = L"Name";          col.cx = app->column_width_name;     col.iSubItem = 0; ListView_InsertColumn(g_hwndList, 0, &col);
        col.pszText = L"Path";          col.cx = app->column_width_path;     col.iSubItem = 1; ListView_InsertColumn(g_hwndList, 1, &col);
        col.pszText = L"Size";          col.cx = app->column_width_size;     col.iSubItem = 2; ListView_InsertColumn(g_hwndList, 2, &col);
        col.pszText = L"Date Modified"; col.cx = app->column_width_modified; col.iSubItem = 3; ListView_InsertColumn(g_hwndList, 3, &col);
        col.pszText = L"Date Created";  col.cx = 0;   col.iSubItem = 4; ListView_InsertColumn(g_hwndList, 4, &col);
        col.pszText = L"Attributes";    col.cx = 0;   col.iSubItem = 5; ListView_InsertColumn(g_hwndList, 5, &col);
        
        /* Add extension column (hidden) */
        col.pszText = L"Extension";     col.cx = 0;   col.iSubItem = 6; ListView_InsertColumn(g_hwndList, 6, &col);
        ui_update_sort_indicator(g_hwndList, app->query.sort_column,
                                 app->query.sort_ascending);
        ui_hide_horizontal_scrollbar(g_hwndList);
        
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

        g_hwndFolderHeader = CreateWindowExW(
            0, L"STATIC", L"Folders",
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
            0, 0, 0, 0, hwnd, NULL, cs->hInstance, NULL);
        g_hwndFolderTree = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_FOLDER_TREE, cs->hInstance, NULL);
        g_hwndSubfolders = CreateWindowExW(
            0, L"BUTTON", L"Subfolders",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SUBFOLDERS, cs->hInstance, NULL);
        g_hwndFilterHeader = CreateWindowExW(
            0, L"STATIC", L"Filters",
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
            0, 0, 0, 0, hwnd, NULL, cs->hInstance, NULL);
        g_hwndFilterList = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_FILTER_LIST, cs->hInstance, NULL);

        SetWindowSubclass(g_hwndFolderHeader, panel_header_proc, 1, 1);
        SetWindowSubclass(g_hwndFilterHeader, panel_header_proc, 1, 2);
        SetWindowSubclass(g_hwndStatus, status_bar_proc, 1, 0);
        SetWindowSubclass(g_hwndSubfolders, subfolders_proc, 1, 0);

        SendMessageW(g_hwndFolderHeader, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_hwndFolderTree, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_hwndSubfolders, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_hwndFilterHeader, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_hwndFilterList, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(g_hwndSubfolders, BM_SETCHECK,
                     app->include_subfolders ? BST_CHECKED : BST_UNCHECKED, 0);
        for (int i = 0; i < FILTER_COUNT; i++)
            SendMessageW(g_hwndFilterList, LB_ADDSTRING, 0, (LPARAM)g_filter_names[i]);
        SendMessageW(g_hwndFilterList, LB_SETCURSEL, app->selected_filter, 0);
        ui_populate_folder_roots();
        ui_update_view_menu();
        ui_apply_theme(hwnd);
        ui_apply_layout(hwnd);
        ui_start_metadata_worker(app, hwnd);
        
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L"Loading index cache...");
        ui_start_cache_load(hwnd);
        
        return 0;
    }
    
    /* ---- Window sizing ---- */
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lParam;
        info->ptMinTrackSize.x = 640;
        info->ptMinTrackSize.y = 450;
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        g_in_size_move = 1;
        return 0;
    
    case WM_SIZE:
        ui_apply_layout(hwnd);
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (app && app->theme_mode == THEME_SYSTEM)
            ui_apply_theme(hwnd);
        return 0;
    
    case WM_EXITSIZEMOVE:
        ui_apply_layout(hwnd);
        g_in_size_move = 0;
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            int splitter = ui_splitter_hit_test(hwnd, point);
            if (splitter) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN:
    {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        g_splitter_drag = ui_splitter_hit_test(hwnd, point);
        if (g_splitter_drag) {
            SetCapture(hwnd);
            SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (g_splitter_drag && GetCapture() == hwnd) {
            POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            RECT rc;
            int width;
            int maximum = 600;
            int other_width = 0;
            int visible_count = (app->show_folders ? 1 : 0) +
                                (app->show_filters ? 1 : 0);
            int side = g_splitter_drag == 1 ? PANEL_DOCK_LEFT : PANEL_DOCK_RIGHT;
            GetClientRect(hwnd, &rc);
            width = side == PANEL_DOCK_LEFT ? point.x : rc.right - point.x;
            if (visible_count == 2) {
                if (app->folder_panel_side != side)
                    other_width = app->folder_panel_width;
                else
                    other_width = app->filter_panel_width;
                maximum = rc.right - 280 - visible_count * UI_SPLITTER_SIZE - other_width;
                if (maximum > 600) maximum = 600;
                if (maximum < 160) maximum = 160;
            }
            if (width < 160) width = 160;
            if (width > maximum) width = maximum;
            if (app->show_folders && app->folder_panel_side == side) {
                app->folder_panel_width = width;
            } else {
                app->filter_panel_width = width;
            }
            ui_apply_layout(hwnd);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_splitter_drag) {
            g_splitter_drag = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd)
            g_splitter_drag = 0;
        break;
    
    /* ---- Menu and command handling ---- */
    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD notify = HIWORD(wParam);
        
        if (id == IDC_SEARCH_EDIT && notify == EN_CHANGE) {
            ui_queue_search(hwnd);
            return 0;
        }
        if (id == IDC_FILTER_LIST && notify == LBN_SELCHANGE) {
            int selected = (int)SendMessageW(g_hwndFilterList, LB_GETCURSEL, 0, 0);
            if (selected >= FILTER_EVERYTHING && selected < FILTER_COUNT) {
                app->selected_filter = selected;
                ui_queue_filter_search(hwnd);
            }
            return 0;
        }
        if (id == IDC_SUBFOLDERS && notify == BN_CLICKED) {
            app->include_subfolders =
                SendMessageW(g_hwndSubfolders, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ui_queue_search(hwnd);
            return 0;
        }
        
        switch (id) {
        case IDM_FILE_EXIT:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
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
                wchar_t *path = NULL;
                if (ui_copy_entry_snapshot(app, sel, NULL, &path, NULL))
                    ui_copy_text_to_clipboard(hwnd, path);
                free(path);
            }
            break;
        }
        
        case IDM_EDIT_SELECT_ALL:
            for (int i = 0; i < app->filtered_count; i++) {
                ListView_SetItemState(g_hwndList, i, LVIS_SELECTED, LVIS_SELECTED);
            }
            break;

        case IDM_VIEW_FOLDERS:
            app->show_folders = !app->show_folders;
            ui_update_view_menu();
            ui_apply_layout(hwnd);
            break;

        case IDM_VIEW_FILTERS:
            app->show_filters = !app->show_filters;
            ui_update_view_menu();
            ui_apply_layout(hwnd);
            break;

        case IDM_VIEW_THEME_LIGHT:
            app->theme_mode = THEME_LIGHT;
            ui_apply_theme(hwnd);
            break;

        case IDM_VIEW_THEME_DARK:
            app->theme_mode = THEME_DARK;
            ui_apply_theme(hwnd);
            break;

        case IDM_VIEW_THEME_SYSTEM:
            app->theme_mode = THEME_SYSTEM;
            ui_apply_theme(hwnd);
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
            
        case IDM_VIEW_SORT_NAME:          ui_change_sort(hwnd, app, COL_NAME, 1);          break;
        case IDM_VIEW_SORT_PATH:          ui_change_sort(hwnd, app, COL_PATH, 1);          break;
        case IDM_VIEW_SORT_SIZE:          ui_change_sort(hwnd, app, COL_SIZE, 1);          break;
        case IDM_VIEW_SORT_DATE_MODIFIED: ui_change_sort(hwnd, app, COL_DATE_MODIFIED, 1); break;
        case IDM_VIEW_SORT_DATE_CREATED:  ui_change_sort(hwnd, app, COL_DATE_CREATED, 1);  break;
        case IDM_VIEW_SORT_ATTRIBUTES:    ui_change_sort(hwnd, app, COL_ATTRIBUTES, 1);    break;
        
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
                L"OpenEverything v0.1.9\n\n"
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
        if (nm->hwndFrom == ListView_GetHeader(g_hwndList) &&
            nm->code == NM_CUSTOMDRAW)
            return ui_custom_draw_list_header((NMCUSTOMDRAW *)lParam);
        if (nm->idFrom == IDC_FOLDER_TREE) {
            if (nm->code == TVN_ITEMEXPANDINGW) {
                NMTREEVIEWW *tree = (NMTREEVIEWW *)lParam;
                if (tree->action == TVE_EXPAND)
                    ui_start_folder_enum(tree->itemNew.hItem,
                                         (FOLDER_NODE *)tree->itemNew.lParam);
                return 0;
            }
            if (nm->code == TVN_SELCHANGEDW) {
                NMTREEVIEWW *tree = (NMTREEVIEWW *)lParam;
                FOLDER_NODE *node = (FOLDER_NODE *)tree->itemNew.lParam;
                if (node) {
                    wcscpy_s(app->folder_scope, SEARCH_FOLDER_SCOPE_MAX, node->path);
                    SetWindowTextW(g_hwndSearch, L"");
                    ui_queue_search(hwnd);
                }
                return 0;
            }
            if (nm->code == TVN_DELETEITEMW) {
                NMTREEVIEWW *tree = (NMTREEVIEWW *)lParam;
                free((FOLDER_NODE *)tree->itemOld.lParam);
                return 0;
            }
        }
        if (nm->idFrom == IDC_LISTVIEW) {
            switch (nm->code) {
            case LVN_COLUMNCLICK:
            {
                NMLISTVIEW *click = (NMLISTVIEW *)lParam;
                ui_change_sort(hwnd, app, click->iSubItem, 1);
                break;
            }

            case LVN_GETDISPINFOW:
            {
                NMLVDISPINFOW *di = (NMLVDISPINFOW *)lParam;
                static wchar_t text_buf[512];
                wchar_t icon_ext[64];
                INDEX_ENTRY *e;
                int has_entry = 0;
                int icon_is_dir = 0;
                icon_ext[0] = L'\0';
                
                if (!g_in_scroll_thumb && (di->item.mask & LVIF_TEXT) &&
                    (di->item.iSubItem == COL_SIZE ||
                     di->item.iSubItem == COL_DATE_MODIFIED ||
                     di->item.iSubItem == COL_DATE_CREATED ||
                     di->item.iSubItem == COL_ATTRIBUTES)) {
                    ui_queue_row_metadata(app, di->item.iItem);
                }
                
                EnterCriticalSection(&app->index_lock);
                e = ui_entry_from_row(app, di->item.iItem);
                
                if (e) {
                    int entry_index = (int)(e - app->entries);
                    has_entry = 1;
                    icon_is_dir = e->is_directory;
                    wcsncpy_s(icon_ext, 64, index_entry_extension(e), _TRUNCATE);
                    
                    if (di->item.mask & LVIF_TEXT) {
                        switch (di->item.iSubItem) {
                        case COL_NAME:
                            wcscpy_s(text_buf, 512, e->name ? e->name : L"");
                            di->item.pszText = text_buf;
                            break;
                        case COL_PATH:
                            ui_get_parent_path(app, entry_index, text_buf, 512);
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
                
                if (has_entry && (di->item.mask & LVIF_IMAGE)) {
                    di->item.iImage = g_in_scroll_thumb
                        ? (icon_is_dir ? g_icon_folder : g_icon_file)
                        : ui_icon_for_type(icon_is_dir, icon_ext);
                }
                break;
            }
            
            case NM_DBLCLK:
            {
                int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < app->filtered_count) {
                    wchar_t *path = NULL;
                    if (ui_copy_entry_snapshot(app, sel, NULL, &path, NULL))
                        ui_open_entry_path(hwnd, path);
                    free(path);
                }
                break;
            }
            
            case LVN_KEYDOWN:
            {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lParam;
                if (kd->wVKey == VK_DELETE) {
                    int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < app->filtered_count) {
                        wchar_t *name = NULL;
                        wchar_t *path = NULL;
                        int is_directory = 0;
                        wchar_t msg[1024];
                        if (!ui_copy_entry_snapshot(app, sel, &name, &path,
                                                    &is_directory))
                            break;
                        swprintf_s(msg, 1024, L"Delete \"%s\"?", name);
                        if (MessageBoxW(hwnd, msg, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                            if (is_directory) {
                                /* Recycle bin delete */
                                SHFILEOPSTRUCTW op = {0};
                                op.wFunc = FO_DELETE;
                                op.pFrom = path;
                                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                                SHFileOperationW(&op);
                            } else {
                                DeleteFileW(path);
                            }
                        }
                        free(path);
                        free(name);
                    }
                }
                else if (kd->wVKey == VK_RETURN) {
                    int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
                    if (sel >= 0 && sel < app->filtered_count) {
                        wchar_t *path = NULL;
                        if (ui_copy_entry_snapshot(app, sel, NULL, &path, NULL))
                            ui_open_entry_path(hwnd, path);
                        free(path);
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

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HWND control = (HWND)lParam;
        int panel = control == g_hwndFolderHeader ||
                    control == g_hwndSubfolders || control == g_hwndFilterHeader;
        SetTextColor((HDC)wParam, g_color_text);
        SetBkColor((HDC)wParam, panel ? g_color_panel : g_color_window);
        return (LRESULT)(panel ? g_brush_panel : g_brush_window);
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
            if (job->result_count <= SEARCH_MAX_RESULTS && app->filtered_indices) {
                memcpy(app->filtered_indices, job->results, job->result_count * sizeof(int));
                app->filtered_count = job->result_count;
                app->filtered_identity = 0;
                app->filtered_stale = 0;
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
    
    case WM_METADATA_READY:
    {
        int top;
        int last;
        int item_count;
        InterlockedExchange(&g_metadata_redraw_pending, 0);
        item_count = ListView_GetItemCount(g_hwndList);
        top = ListView_GetTopIndex(g_hwndList);
        last = top + ListView_GetCountPerPage(g_hwndList) + 1;
        if (last >= item_count)
            last = item_count - 1;
        if (!g_in_size_move && top >= 0 && last >= top)
            ListView_RedrawItems(g_hwndList, top, last);
        return 0;
    }

    case WM_FOLDER_ENUM_READY:
        ui_handle_folder_enum_result((FOLDER_ENUM_RESULT *)lParam);
        return 0;
    
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
        KillTimer(hwnd, IDT_STARTUP_SYNC);
        SetTimer(hwnd, IDT_STARTUP_SYNC, STARTUP_SYNC_DELAY_MS, NULL);
        return 0;
    }
    
    case WM_INDEX_SYNCED:
    {
        int identity;
        EnterCriticalSection(&app->index_lock);
        identity = app->filtered_identity;
        LeaveCriticalSection(&app->index_lock);
        if (identity)
            ui_update_listview_count(g_hwndList, app);
        else
            ui_queue_search(hwnd);
        ui_update_status(g_hwndStatus, app);
        return 0;
    }
    
    case WM_CACHE_LOADED:
    {
        ui_update_status(g_hwndStatus, app);
        ui_queue_search(hwnd);
        KillTimer(hwnd, IDT_STARTUP_SYNC);
        if ((int)wParam != CACHE_LOAD_LEGACY)
            SetTimer(hwnd, IDT_STARTUP_SYNC, STARTUP_SYNC_DELAY_MS, NULL);
        return 0;
    }

    case WM_CACHE_UPGRADE_DONE:
    {
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
        KillTimer(hwnd, IDT_STARTUP_SYNC);
        InterlockedExchange(&app->monitor_running, 0);
        InterlockedIncrement(&app->search_generation);
        app->filtered_count = 0;
        app->filtered_identity = 0;
        app->filtered_stale = 0;
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
        if (g_hwndList) {
            app->column_width_name = ListView_GetColumnWidth(g_hwndList, 0);
            app->column_width_path = ListView_GetColumnWidth(g_hwndList, 1);
            app->column_width_size = ListView_GetColumnWidth(g_hwndList, 2);
            app->column_width_modified = ListView_GetColumnWidth(g_hwndList, 3);
        }
        config_save(app);
        InterlockedExchange(&app->shutting_down, 1);
        InterlockedExchange(&g_metadata_running, 0);
        if (g_metadata_event)
            SetEvent(g_metadata_event);
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

static LRESULT CALLBACK panel_header_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    int panel = (int)ref_data;
    const int close_width = 32;

    switch (msg) {
    case WM_SETCURSOR:
    {
        POINT point;
        RECT rc;
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        GetClientRect(hwnd, &rc);
        SetCursor(LoadCursorW(NULL,
                              point.x >= rc.right - close_width
                                  ? IDC_HAND : IDC_SIZEALL));
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint;
        RECT rc;
        RECT text_rc;
        HDC dc = BeginPaint(hwnd, &paint);
        HPEN cross_pen;
        HGDIOBJ old_font;
        int center_x;
        int center_y;

        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_brush_panel ? g_brush_panel :
                 (HBRUSH)(COLOR_BTNFACE + 1));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_color_text);
        old_font = SelectObject(dc, g_font_ui ? g_font_ui :
                                GetStockObject(DEFAULT_GUI_FONT));
        text_rc = rc;
        text_rc.left += 10;
        text_rc.right -= close_width;
        DrawTextW(dc, panel == 1 ? L"Folders" : L"Filters", -1, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        center_x = rc.right - close_width / 2;
        center_y = (rc.top + rc.bottom) / 2;
        cross_pen = CreatePen(PS_SOLID, 2, g_color_text);
        if (cross_pen) {
            HGDIOBJ old_pen = SelectObject(dc, cross_pen);
            MoveToEx(dc, center_x - 5, center_y - 5, NULL);
            LineTo(dc, center_x + 5, center_y + 5);
            MoveToEx(dc, center_x + 5, center_y - 5, NULL);
            LineTo(dc, center_x - 5, center_y + 5);
            SelectObject(dc, old_pen);
            DeleteObject(cross_pen);
        }
        SelectObject(dc, old_font);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (point.x >= rc.right - close_width) {
            HWND parent = GetParent(hwnd);
            if (parent)
                SendMessageW(parent, WM_COMMAND,
                             MAKEWPARAM(panel == 1 ? IDM_VIEW_FOLDERS
                                                    : IDM_VIEW_FILTERS, 0), 0);
            return 0;
        }
        g_panel_drag = panel;
        GetCursorPos(&g_panel_drag_start);
        SetCapture(hwnd);
        SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_panel_drag == panel && GetCapture() == hwnd && (wParam & MK_LBUTTON)) {
            POINT point;
            RECT rc;
            GetCursorPos(&point);
            if (abs(point.x - g_panel_drag_start.x) >= 4 ||
                abs(point.y - g_panel_drag_start.y) >= 4) {
                ScreenToClient(g_app_ptr->hwnd_main, &point);
                GetClientRect(g_app_ptr->hwnd_main, &rc);
                ui_dock_panel(panel, point.x < rc.right / 2
                              ? PANEL_DOCK_LEFT : PANEL_DOCK_RIGHT);
            }
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_panel_drag == panel) {
            g_panel_drag = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd && g_panel_drag == panel)
            g_panel_drag = 0;
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, panel_header_proc, subclass_id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK status_bar_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    (void)ref_data;

    switch (msg) {
    case SB_SETTEXTW:
        if (LOWORD(wParam) == 0) {
            wcsncpy_s(g_status_text, 256,
                      lParam ? (const wchar_t *)lParam : L"", _TRUNCATE);
            InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &paint);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_brush_panel ? g_brush_panel : (HBRUSH)(COLOR_BTNFACE + 1));
        rc.left += 4;
        rc.right -= 4;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_color_text);
        SelectObject(dc, g_font_ui ? g_font_ui : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, g_status_text, -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, status_bar_proc, subclass_id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK subfolders_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    (void)wParam;
    (void)lParam;
    (void)ref_data;

    if (msg == WM_PAINT && g_theme_is_dark) {
        PAINTSTRUCT paint;
        RECT rc;
        RECT box;
        RECT text_rect;
        HBRUSH border;
        HGDIOBJ old_font;
        HDC dc = BeginPaint(hwnd, &paint);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_brush_panel);

        box.left = 4;
        box.top = (rc.bottom - 16) / 2;
        box.right = box.left + 16;
        box.bottom = box.top + 16;
        FillRect(dc, &box, g_brush_window);
        border = CreateSolidBrush(RGB(150, 150, 150));
        FrameRect(dc, &box, border);
        DeleteObject(border);

        if (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            HPEN pen = CreatePen(PS_SOLID, 2, g_color_text);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            MoveToEx(dc, box.left + 3, box.top + 8, NULL);
            LineTo(dc, box.left + 7, box.bottom - 3);
            LineTo(dc, box.right - 2, box.top + 3);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }

        text_rect = rc;
        text_rect.left = box.right + 6;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(hwnd) ? g_color_text : RGB(145, 145, 145));
        old_font = SelectObject(dc,
                                g_font_ui ? g_font_ui : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, L"Subfolders", -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old_font);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (msg == WM_ERASEBKGND && g_theme_is_dark)
        return 1;
    if (msg == WM_NCDESTROY) {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        RemoveWindowSubclass(hwnd, subfolders_proc, subclass_id);
        return result;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
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

static LRESULT CALLBACK list_view_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    LRESULT result;
    int scroll_code;

    (void)ref_data;

    switch (msg) {
    case WM_VSCROLL:
        scroll_code = LOWORD(wParam);
        if (scroll_code == SB_THUMBTRACK)
            g_in_scroll_thumb = 1;

        result = DefSubclassProc(hwnd, msg, wParam, lParam);

        if (scroll_code == SB_ENDSCROLL && g_in_scroll_thumb) {
            g_in_scroll_thumb = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        ui_hide_horizontal_scrollbar(hwnd);
        return result;

    case WM_HSCROLL:
        ui_hide_horizontal_scrollbar(hwnd);
        return 0;

    case WM_NOTIFY:
        if (lParam) {
            NMHDR *notification = (NMHDR *)lParam;
            if (notification->hwndFrom == ListView_GetHeader(hwnd) &&
                notification->code == NM_CUSTOMDRAW)
                return ui_custom_draw_list_header((NMCUSTOMDRAW *)lParam);
        }
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        ui_hide_horizontal_scrollbar(hwnd);
        return result;

    case WM_SIZE:
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        ui_hide_horizontal_scrollbar(hwnd);
        return result;

    case WM_CAPTURECHANGED:
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (g_in_scroll_thumb) {
            g_in_scroll_thumb = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return result;

    case WM_NCDESTROY:
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        RemoveWindowSubclass(hwnd, list_view_proc, subclass_id);
        return result;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
