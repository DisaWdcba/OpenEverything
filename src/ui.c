#include "ui.h"
#include "version.h"
#include "config.h"
#include "ipc.h"
#include "ntfs.h"
#include "search.h"
#include "index.h"
#include "cache.h"
#include "resource.h"
#include "lang.h"
#include "cmdhelp.h"
#include <dwmapi.h>
#include <shobjidl.h>

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
static HMENU g_menu_file;
static HMENU g_menu_sort;
static HMENU g_menu_language;
static HMENU g_menu_theme;
static APP_STATE *g_app_ptr;
static HFONT g_font_ui;
static HFONT g_font_search;
static HFONT g_font_bold;
static HIMAGELIST g_image_small;
static HACCEL g_accel;
static int g_tray_added;
static int g_secondary_offset;
static HICON g_tray_icon;
/* The menu bar is laid out by Windows using the system menu font.  Keep a
   separate handle for UAH dark-mode drawing so a large content font cannot
   make top-level menu labels run into each other. */
static HFONT g_font_menu;
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

/* Thumbnail cache.  Row images are generated on demand from the shell and
   kept in an image list, keyed by full path.  A full clear on overflow keeps
   the memory ceiling predictable for the extra-large 256 px mode. */
typedef struct {
    wchar_t *path;
    int image;
} THUMB_ENTRY;

static THUMB_ENTRY *g_thumbs;
static int g_thumb_count;
static int g_thumb_capacity;
static HIMAGELIST g_thumb_images;
static int g_thumb_pixels;

#define UI_SEARCH_TOP 4
#define UI_SEARCH_HEIGHT 40
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
#define IDM_CTX_RENAME 20005
#define IDM_CTX_DELETE 20006
#define IDM_CTX_PROPERTIES 20007
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
static void ui_disable_horizontal_scrollbar(HWND hwndList);
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
static void ui_build_menu(HWND hwnd);
static void ui_update_file_menu(void);
static int ui_selected_row(void);
static void ui_open_new_search_window(HWND hwnd);
static void ui_delete_selected(HWND hwnd, APP_STATE *app, int row);
static void ui_rename_selected(HWND hwnd, APP_STATE *app, int row);
static void ui_show_properties(HWND hwnd, APP_STATE *app, int row);
static void ui_apply_view_mode(HWND hwnd);
static int ui_view_mode_pixels(int mode);
static int ui_thumb_image_for_path(const wchar_t *path);
static void ui_clear_thumb_cache(void);
static LRESULT ui_custom_draw_list_item(NMLVCUSTOMDRAW *draw);
static void ui_refresh_language(HWND hwnd);
static void ui_choose_language(HWND hwnd);
static int ui_prompt_text(HWND owner, const wchar_t *title,
                          const wchar_t *label, const wchar_t *initial,
                          wchar_t *out, size_t out_capacity);
static void ui_tray_add(HWND hwnd);
static void ui_tray_remove(void);
static void ui_tray_menu(HWND hwnd);
static void ui_show_window(HWND hwnd);
static void ui_hide_to_tray(HWND hwnd);
static void ui_select_initial_result(APP_STATE *app);
static int ui_find_text_ci(const wchar_t *haystack, const wchar_t *needle);

/* Reindex context */
struct ReindexCtx {
    APP_STATE *app;
    HWND hwnd;
};

/* System DPI, sampled once at startup. The process is system-DPI aware (see
   wWinMain), so this is fixed for the window's lifetime. Sizes stored in the
   config are kept in 96-DPI logical units and scaled through these helpers, so
   a layout tuned on a 4K display stays sane on a 1080p one. */
static int g_dpi = 96;

static void ui_init_dpi(void)
{
    HDC screen = GetDC(NULL);
    if (screen) {
        int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
        if (dpi >= 96 && dpi <= 960)
            g_dpi = dpi;
    }
}

static int ui_scale(int logical)
{
    return MulDiv(logical, g_dpi, 96);
}

static int ui_unscale(int physical)
{
    return MulDiv(physical, 96, g_dpi);
}

static void ui_init_visual_resources(void)
{
    NONCLIENTMETRICSW metrics;
    int have_nonclient_metrics;

    g_color_window = RGB(255, 255, 255);
    g_color_panel = RGB(245, 245, 245);
    g_color_text = RGB(32, 37, 45);

    memset(&metrics, 0, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    have_nonclient_metrics = SystemParametersInfoW(
        SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);

    if (!g_font_ui) {
        if (have_nonclient_metrics)
            g_font_ui = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!g_font_ui)
            g_font_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }

    if (!g_font_search) {
        g_font_search = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    if (!g_font_bold) {
        LOGFONTW logfont;
        memset(&logfont, 0, sizeof(logfont));
        if (g_font_ui && GetObjectW(g_font_ui, sizeof(logfont), &logfont)) {
            logfont.lfWeight = FW_BOLD;
            g_font_bold = CreateFontIndirectW(&logfont);
        }
        if (!g_font_bold) {
            g_font_bold = CreateFontW(
                -16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
    }

    if (!g_font_menu) {
        if (have_nonclient_metrics)
            g_font_menu = CreateFontIndirectW(&metrics.lfMenuFont);
        if (!g_font_menu)
            g_font_menu = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    
    if (!g_brush_window)
        g_brush_window = CreateSolidBrush(g_color_window);
    if (!g_brush_panel)
        g_brush_panel = CreateSolidBrush(g_color_panel);
}

static void ui_free_visual_resources(void)
{
    if (g_font_ui && g_font_ui != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g_font_ui);
    }
    g_font_ui = NULL;
    if (g_font_search) { DeleteObject(g_font_search); g_font_search = NULL; }
    if (g_font_bold) { DeleteObject(g_font_bold); g_font_bold = NULL; }
    if (g_font_menu && g_font_menu != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(g_font_menu);
    }
    g_font_menu = NULL;
    if (g_brush_window) { DeleteObject(g_brush_window); g_brush_window = NULL; }
    if (g_brush_panel) { DeleteObject(g_brush_panel); g_brush_panel = NULL; }
    ui_clear_thumb_cache();
    if (g_thumb_images) {
        if (g_hwndList)
            ListView_SetImageList(g_hwndList, NULL, LVSIL_NORMAL);
        ImageList_Destroy(g_thumb_images);
        g_thumb_images = NULL;
    }
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
        g_image_small = images;
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
            *out_name = index_duplicate_entry_name_locked(app, entry);
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
                                             CMF_NORMAL | CMF_CANRENAME);
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
    if (loaded == CACHE_LOAD_LEGACY && !ctx->app->shutting_down) {
        /* Parent rows are resolved through the ref index while converting a
           legacy cache, avoiding a linear scan for every directory. */
        index_build_ref_index(ctx->app);
    }
    if (loaded == CACHE_LOAD_LEGACY && !ctx->app->shutting_down &&
        cache_save_index(ctx->app)) {
        int reloaded = cache_load_index(ctx->app);
        if (reloaded == CACHE_LOAD_CURRENT) {
            loaded = reloaded;
            if (!ctx->app->shutting_down)
                PostMessageW(ctx->hwnd, WM_CACHE_UPGRADE_DONE, 0, 0);
        }
    }
    if (loaded && !ctx->app->shutting_down)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    /* Let the window show the persisted snapshot as soon as it is mapped.  The
       filter/ref/name accelerators are optional and can be built after the
       first search has started; each builder publishes under index_lock and
       the search path already falls back to a linear scan while they are cold. */
    InterlockedExchange(&g_cache_loading, 0);
    if (!ctx->app->shutting_down)
        PostMessageW(ctx->hwnd, loaded ? WM_CACHE_LOADED : WM_REFRESH,
                     (WPARAM)loaded, 0);
    if (loaded && !ctx->app->shutting_down) {
        index_build_filter_index(ctx->app);
        index_build_ref_index(ctx->app);
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

    /* A new result set must not inherit the previous rows' selection state:
       ListView keeps item state across ListView_SetItemCountEx, which made a
       fresh search look as if every row had been selected. */
    if (g_hwndList)
        ListView_SetItemState(g_hwndList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    
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
    SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("Searching..."));
    
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
    if (g_hwndList)
        ListView_SetItemState(g_hwndList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ui_update_listview(g_hwndList, app);
    SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("Searching..."));

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

        if (!sync_ok || needs_rebuild) {
            /* The journal was recreated, or it wrapped past the USN we stopped
               at, so the incremental stream can no longer be trusted. Hand the
               rebuild to the UI thread: just ending the loop here left the
               index frozen with nothing to tell the user it had stopped. */
            if (needs_rebuild && !app->shutting_down)
                PostMessageW(hwnd, WM_REFRESH, 0, 0);
            break;
        }
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
        
        INDEX_BUILD build;
        index_build_init(&build);
        if (ntfs_read_mft(hVol, &build, v, c->hwnd) > 0 ||
            ntfs_read_usn_index(hVol, &build, v, c->hwnd) > 0)
            indexed_volumes++;
        else
            failed_volumes++;
        ntfs_update_volume_usn_info(hVol, &a->volumes[v]);
        ntfs_close_volume(hVol);
        
        if (!index_add_entries(a, &build))
            failed_volumes++;
        index_build_free(&build);
    }
    
    a->indexed_volume_count = indexed_volumes;
    a->index_error_count = failed_volumes;
    PostMessageW(c->hwnd, WM_INDEX_PROGRESS,
                 (WPARAM)(INT_PTR)INDEX_PROGRESS_SORTING, 0);
    index_build_paths(a);
    index_sort_entries_by_name(a);
    index_compact_entry_names(a);
    PostMessageW(c->hwnd, WM_INDEX_PROGRESS,
                 (WPARAM)(INT_PTR)INDEX_PROGRESS_BUILDING, 0);
    index_build_filter_index(a);
    index_build_ref_index(a);
    index_build_name_char_index(a);
    if (a->entry_count > 0) {
        PostMessageW(c->hwnd, WM_INDEX_PROGRESS,
                     (WPARAM)(INT_PTR)INDEX_PROGRESS_SAVING, 0);
        if (cache_save_index(a) &&
            cache_load_index(a) == CACHE_LOAD_CURRENT) {
            index_build_filter_index(a);
            index_build_ref_index(a);
            index_build_name_char_index(a);
        }
    }
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

    ui_init_dpi();

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

int ui_translate_accelerator(HWND hwnd, MSG *msg)
{
    if (!g_accel || !msg)
        return 0;
    return TranslateAcceleratorW(hwnd, g_accel, msg) ? 1 : 0;
}

/* =============================================================
 * Create main window
 * ============================================================= */
/* True when the saved rect still overlaps a live monitor. Display setups
   change between runs; restoring onto a monitor that is gone would put the
   window somewhere the user cannot reach it. */
static int ui_placement_is_visible(int x, int y, int width, int height)
{
    RECT rect;
    rect.left = x;
    rect.top = y;
    rect.right = x + width;
    rect.bottom = y + height;
    return MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != NULL;
}

HWND ui_create_main_window(HINSTANCE hInst, int nCmdShow, APP_STATE *app)
{
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int width = ui_scale(DEFAULT_WINDOW_WIDTH);
    int height = ui_scale(DEFAULT_WINDOW_HEIGHT);

    g_app_ptr = app;
    app->hinst = hInst;

    if (!app->secondary_window && app->window_width > 0 && app->window_height > 0) {
        int saved_w = ui_scale(app->window_width);
        int saved_h = ui_scale(app->window_height);
        if (ui_placement_is_visible(app->window_x, app->window_y,
                                    saved_w, saved_h)) {
            x = app->window_x;
            y = app->window_y;
            width = saved_w;
            height = saved_h;
        }
    }
    if (app->secondary_window) {
        /* Cascade relative to an existing search window.  Each window is a
           separate process here, so a process-local counter alone would put
           every new window at the same place. */
        HWND existing = FindWindowW(WC_EVERYTHING, NULL);
        RECT existing_rect;
        RECT work;

        width = ui_scale(DEFAULT_WINDOW_WIDTH);
        height = ui_scale(DEFAULT_WINDOW_HEIGHT);
        if (existing && GetWindowRect(existing, &existing_rect)) {
            x = existing_rect.left + ui_scale(32);
            y = existing_rect.top + ui_scale(32);
        } else {
            g_secondary_offset += 32;
            x = ui_scale(80 + g_secondary_offset);
            y = ui_scale(60 + g_secondary_offset);
        }
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            if (x + width > work.right)
                x = work.left;
            if (y + height > work.bottom)
                y = work.top;
        }
    }

    HWND hwnd = CreateWindowExW(
        0,
        WC_EVERYTHING,
        L"OpenEverything",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y,
        width, height,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return NULL;

    app->hwnd_main = hwnd;
    if (app->secondary_window) {
        wchar_t title[160];
        swprintf_s(title, 160, L"OpenEverything - %s",
                   L("New Search Window"));
        SetWindowTextW(hwnd, title);
    }
    if (app->window_maximized && nCmdShow != SW_SHOWMINIMIZED)
        nCmdShow = SW_SHOWMAXIMIZED;
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
    ui_disable_horizontal_scrollbar(hwndList);
    InvalidateRect(hwndList, NULL, FALSE);
}

static void ui_update_listview_count(HWND hwndList, APP_STATE *app)
{
    ListView_SetItemCountEx(hwndList, app->filtered_count,
                            LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    ui_disable_horizontal_scrollbar(hwndList);
}

void ui_update_status(HWND hwndStatus, APP_STATE *app)
{
    wchar_t buf[256];
    wchar_t filtered[48];
    wchar_t total[48];
    
    ui_format_count(filtered, 48, app->filtered_count);
    ui_format_count(total, 48, app->entry_count);
    
    if (app->entry_count == 0 && app->index_error_count > 0) {
        swprintf_s(buf, 256,
                   L("0 objects - unable to read NTFS volumes; try running as Administrator"));
    } else if (app->filtered_count == app->entry_count) {
        swprintf_s(buf, 256, L("%s objects"), total);
    } else {
        swprintf_s(buf, 256, L("%s objects (%s total)"), filtered, total);
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
    root = ui_tree_insert(TVI_ROOT, L("Everything"),
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
    APP_STATE *app = g_app_ptr;
    int view_id;

    if (!g_menu_view || !app)
        return;
    CheckMenuItem(g_menu_view, IDM_VIEW_FOLDERS,
                  MF_BYCOMMAND | (app->show_folders ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu_view, IDM_VIEW_FILTERS,
                  MF_BYCOMMAND | (app->show_filters ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu_view, IDM_VIEW_STATUS_BAR,
                  MF_BYCOMMAND | (app->show_status_bar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu_view, IDM_VIEW_CLOSE_TO_TRAY,
                  MF_BYCOMMAND | (app->close_to_tray ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu_view, IDM_VIEW_MINIMIZE_TO_TRAY,
                  MF_BYCOMMAND | (app->minimize_to_tray ? MF_CHECKED : MF_UNCHECKED));
    if (g_menu_theme)
        CheckMenuRadioItem(g_menu_theme, IDM_VIEW_THEME_LIGHT, IDM_VIEW_THEME_SYSTEM,
                           app->theme_mode == THEME_LIGHT ? IDM_VIEW_THEME_LIGHT :
                           app->theme_mode == THEME_DARK ? IDM_VIEW_THEME_DARK :
                                                           IDM_VIEW_THEME_SYSTEM,
                           MF_BYCOMMAND);

    switch (app->view_mode) {
    case VIEWMODE_MEDIUM_THUMBNAILS: view_id = IDM_VIEW_THUMBNAILS_MEDIUM; break;
    case VIEWMODE_LARGE_THUMBNAILS: view_id = IDM_VIEW_THUMBNAILS_LARGE; break;
    case VIEWMODE_EXTRA_LARGE_THUMBNAILS: view_id = IDM_VIEW_THUMBNAILS_EXTRA_LARGE; break;
    default: view_id = IDM_VIEW_DETAILS; break;
    }
    CheckMenuRadioItem(g_menu_view, IDM_VIEW_DETAILS,
                       IDM_VIEW_THUMBNAILS_EXTRA_LARGE, view_id, MF_BYCOMMAND);

    if (g_menu_sort) {
        int sort_id = IDM_VIEW_SORT_NAME + app->query.sort_column;
        if (app->query.sort_column == COL_EXTENSION)
            sort_id = IDM_VIEW_SORT_EXTENSION;
        CheckMenuRadioItem(g_menu_sort, IDM_VIEW_SORT_NAME,
                           IDM_VIEW_SORT_EXTENSION, sort_id, MF_BYCOMMAND);
        CheckMenuRadioItem(g_menu_sort, IDM_VIEW_SORT_ASCENDING,
                           IDM_VIEW_SORT_DESCENDING,
                           app->query.sort_ascending ? IDM_VIEW_SORT_ASCENDING :
                                                       IDM_VIEW_SORT_DESCENDING,
                           MF_BYCOMMAND);
    }

    if (g_menu_language) {
        int count = lang_count();
        if (count > 0 && count <= IDM_VIEW_LANGUAGE_LAST - IDM_VIEW_LANGUAGE_FIRST + 1) {
            CheckMenuRadioItem(g_menu_language, IDM_VIEW_LANGUAGE_FIRST,
                               IDM_VIEW_LANGUAGE_FIRST + count - 1,
                               IDM_VIEW_LANGUAGE_FIRST + lang_current(),
                               MF_BYCOMMAND);
        }
    }
}

static void ui_apply_titlebar_theme(HWND hwnd, int dark)
{
    typedef struct {
        int attribute;
        PVOID data;
        SIZE_T data_size;
    } UI_WINDOWCOMPOSITIONATTRIBDATA;
    typedef BOOL (WINAPI *SetWindowCompositionAttributeFn)(
        HWND, UI_WINDOWCOMPOSITIONATTRIBDATA *);
    SetWindowCompositionAttributeFn set_window_composition;
    UI_WINDOWCOMPOSITIONATTRIBDATA composition_data;
    BOOL dark_title = dark ? TRUE : FALSE;
    HRESULT dark_mode_result;
    int nc_rendering_policy;

    dark_mode_result = DwmSetWindowAttribute(
        hwnd, 20, &dark_title, sizeof(dark_title));
    if (FAILED(dark_mode_result))
        DwmSetWindowAttribute(hwnd, 19, &dark_title, sizeof(dark_title));

    set_window_composition = (SetWindowCompositionAttributeFn)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (set_window_composition) {
        composition_data.attribute = 26; /* WCA_USEDARKMODECOLORS */
        composition_data.data = &dark_title;
        composition_data.data_size = sizeof(dark_title);
        set_window_composition(hwnd, &composition_data);
    }

    /* On Windows 10 the dark-mode attribute changes immediately, but DWM can
       keep presenting the old caption surface. Recreate that surface through
       the documented non-client rendering policy; synthesizing WM_NCACTIVATE
       makes DefWindowProc draw a classic caption fragment over the DWM frame. */
    nc_rendering_policy = 1; /* DWMNCRP_DISABLED */
    if (SUCCEEDED(DwmSetWindowAttribute(
            hwnd, 2, &nc_rendering_policy, sizeof(nc_rendering_policy)))) {
        DwmFlush();
        nc_rendering_policy = 2; /* DWMNCRP_ENABLED */
        DwmSetWindowAttribute(
            hwnd, 2, &nc_rendering_policy, sizeof(nc_rendering_policy));
    }

    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    DwmFlush();
}

static void ui_apply_theme(HWND hwnd)
{
    typedef int (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *RefreshImmersiveColorPolicyStateFn)(void);
    typedef void (WINAPI *FlushMenuThemesFn)(void);
    typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
    HMODULE uxtheme;
    SetPreferredAppModeFn set_preferred_mode;
    RefreshImmersiveColorPolicyStateFn refresh_color_policy;
    FlushMenuThemesFn flush_menu_themes = NULL;
    AllowDarkModeForWindowFn allow_dark_window = NULL;
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
        refresh_color_policy = (RefreshImmersiveColorPolicyStateFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)104);
        flush_menu_themes = (FlushMenuThemesFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)136);
        allow_dark_window = (AllowDarkModeForWindowFn)GetProcAddress(
            uxtheme, (LPCSTR)(ULONG_PTR)133);
        if (set_preferred_mode)
            set_preferred_mode(dark ? 2 : 3);
        if (refresh_color_policy)
            refresh_color_policy();
        if (allow_dark_window)
            allow_dark_window(hwnd, dark);
    }

    ui_apply_titlebar_theme(hwnd, dark);

    if (uxtheme) {
        if (allow_dark_window) {
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
    ui_update_view_menu();
    DrawMenuBar(hwnd);
    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                 RDW_UPDATENOW);
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
    if (app && app->show_status_bar) {
        ShowWindow(g_hwndStatus, SW_SHOWNA);
        SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
        GetWindowRect(g_hwndStatus, &rc_status);
        status_h = rc_status.bottom - rc_status.top;
        if (status_h <= 0)
            status_h = 22;
    } else {
        ShowWindow(g_hwndStatus, SW_HIDE);
        status_h = 0;
    }
    
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
    ui_disable_horizontal_scrollbar(g_hwndList);
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

    /* The main list is a clipped child of the frame.  Moving it from one
       side to the other can leave its old header/body pixels behind because
       the frame does not receive an automatic erase for the vacated child
       rectangle.  Repaint the frame and every child immediately so a dock
       operation cannot show a stale duplicate list over the new panel. */
    InvalidateRect(app->hwnd_main, NULL, TRUE);
    RedrawWindow(app->hwnd_main, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                 RDW_FRAME | RDW_UPDATENOW);
}

static void ui_disable_horizontal_scrollbar(HWND hwndList)
{
    LONG_PTR style;
    int refresh_frame;

    if (!hwndList)
        return;

    style = GetWindowLongPtrW(hwndList, GWL_STYLE);
    refresh_frame = (style & WS_HSCROLL) != 0;

    /* Let Windows remove the non-client scrollbar before stripping any
       lingering style bit, otherwise a disabled scrollbar frame can remain. */
    ShowScrollBar(hwndList, SB_HORZ, FALSE);
    style = GetWindowLongPtrW(hwndList, GWL_STYLE);
    if (style & WS_HSCROLL) {
        SetWindowLongPtrW(hwndList, GWL_STYLE, style & ~WS_HSCROLL);
        refresh_frame = 1;
    }
    if (refresh_frame) {
        SetWindowPos(hwndList, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
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
    if (!app || column < COL_NAME || column > COL_EXTENSION)
        return;

    if (toggle_same == 1 && app->query.sort_column == column) {
        app->query.sort_ascending = !app->query.sort_ascending;
    } else {
        app->query.sort_column = column;
        if (toggle_same != 2)
            app->query.sort_ascending = 1;
    }

    ui_update_sort_indicator(g_hwndList, app->query.sort_column,
                             app->query.sort_ascending);
    ui_update_view_menu();
    ui_queue_search(hwnd);
}

static LRESULT ui_custom_draw_list_header(NMCUSTOMDRAW *draw)
{
    if (!g_theme_is_dark)
        return CDRF_DODEFAULT;
    if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
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
    if (draw->dwDrawStage == CDDS_POSTPAINT) {
        RECT client;
        RECT item_rect;
        int item_count = Header_GetItemCount(draw->hdr.hwndFrom);
        int content_right = 0;

        GetClientRect(draw->hdr.hwndFrom, &client);
        for (int i = 0; i < item_count; i++) {
            if (Header_GetItemRect(draw->hdr.hwndFrom, i, &item_rect) &&
                item_rect.right > content_right) {
                content_right = item_rect.right;
            }
        }
        if (content_right < client.right) {
            RECT remainder = client;
            remainder.left = content_right > client.left
                ? content_right : client.left;
            FillRect(draw->hdc, &remainder, g_brush_panel);
        }
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* =============================================================
 * File actions, menus, language selection and thumbnails
 * ============================================================= */

static int ui_selected_row(void)
{
    int row;

    if (!g_hwndList || !g_app_ptr)
        return -1;
    row = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
    if (row < 0 || row >= g_app_ptr->filtered_count)
        return -1;
    return row;
}

static void ui_open_new_search_window(HWND hwnd)
{
    wchar_t module[MAX_PATH];
    wchar_t command[MAX_PATH + 32];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;

    (void)hwnd;
    if (!GetModuleFileNameW(NULL, module, MAX_PATH))
        return;
    swprintf_s(command, MAX_PATH + 32, L"\"%s\" -newwindow", module);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(module, command, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process)) {
        MessageBoxW(hwnd, L("Failed to create a new search window."),
                    L("OpenEverything"), MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

static void ui_delete_selected(HWND hwnd, APP_STATE *app, int row)
{
    wchar_t *name = NULL;
    wchar_t *path = NULL;
    int is_directory = 0;
    wchar_t prompt[1024];

    if (!ui_copy_entry_snapshot(app, row, &name, &path, &is_directory))
        return;

    _snwprintf_s(prompt, 1024, _TRUNCATE, L("Delete \"%s\"?"), name);
    if (MessageBoxW(hwnd, prompt, L("Delete"),
                    MB_YESNO | MB_ICONWARNING) == IDYES) {
        /* pFrom is a list, not a string: the shell scans past the first NUL
           for the list terminator, so a singly-terminated buffer reads beyond
           the allocation -- during a delete, no less. */
        size_t path_len = wcslen(path);
        wchar_t *op_from = (wchar_t *)calloc(path_len + 2, sizeof(wchar_t));
        if (op_from) {
            SHFILEOPSTRUCTW op;
            int result;

            memcpy(op_from, path, path_len * sizeof(wchar_t));
            memset(&op, 0, sizeof(op));
            op.hwnd = hwnd;
            op.wFunc = FO_DELETE;
            op.pFrom = op_from;
            /* Recycle files too, so the same keystroke is undoable for files
               and folders alike. */
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
            result = SHFileOperationW(&op);
            free(op_from);
            if (result != 0) {
                wchar_t message[256];
                swprintf_s(message, 256, L("%s (%d)"),
                           L("Delete failed"), result);
                MessageBoxW(hwnd, message, L("Error"), MB_OK | MB_ICONERROR);
            } else {
                ui_queue_search(hwnd);
            }
        }
    }
    free(path);
    free(name);
}

static int ui_rename_name_is_valid(const wchar_t *name)
{
    if (!name || !name[0])
        return 0;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        return 0;
    if (wcspbrk(name, L"\\/:*?\"<>|"))
        return 0;
    if (name[wcslen(name) - 1] == L' ')
        return 0;
    return 1;
}

static void ui_rename_selected(HWND hwnd, APP_STATE *app, int row)
{
    wchar_t *name = NULL;
    wchar_t *path = NULL;
    wchar_t new_name[260];
    wchar_t new_path[SEARCH_FOLDER_SCOPE_MAX * 2];
    wchar_t *slash;
    int is_directory = 0;

    if (!ui_copy_entry_snapshot(app, row, &name, &path, &is_directory))
        return;
    wcsncpy_s(new_name, 260, name, _TRUNCATE);

    if (!ui_prompt_text(hwnd, L("Rename"), L("New name:"), name,
                        new_name, 260)) {
        free(path);
        free(name);
        return;
    }
    if (_wcsicmp(new_name, name) == 0) {
        free(path);
        free(name);
        return;
    }
    if (!ui_rename_name_is_valid(new_name)) {
        MessageBoxW(hwnd, L("The new name contains invalid characters."),
                    L("Rename"), MB_OK | MB_ICONWARNING);
        free(path);
        free(name);
        return;
    }

    wcsncpy_s(new_path, SEARCH_FOLDER_SCOPE_MAX * 2, path, _TRUNCATE);
    slash = wcsrchr(new_path, L'\\');
    if (!slash || slash[1] == L'\0') {
        free(path);
        free(name);
        return;
    }
    slash[1] = L'\0';
    if (wcslen(new_path) + wcslen(new_name) + 1 >=
        SEARCH_FOLDER_SCOPE_MAX * 2) {
        MessageBoxW(hwnd, L("The new name contains invalid characters."),
                    L("Rename"), MB_OK | MB_ICONWARNING);
        free(path);
        free(name);
        return;
    }
    wcscat_s(new_path, SEARCH_FOLDER_SCOPE_MAX * 2, new_name);

    if (GetFileAttributesW(new_path) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L("A file with that name already exists."),
                    L("Rename"), MB_OK | MB_ICONWARNING);
    } else if (!MoveFileW(path, new_path)) {
        wchar_t message[512];
        swprintf_s(message, 512, L("%s (%lu)"),
                   L("Failed to rename:"), (unsigned long)GetLastError());
        MessageBoxW(hwnd, message, L("Rename"), MB_OK | MB_ICONERROR);
    } else {
        /* The index catches up through the USN monitor; refresh the visible
           results now so the renamed row does not linger. */
        ui_queue_search(hwnd);
    }
    free(path);
    free(name);
}

static void ui_show_properties(HWND hwnd, APP_STATE *app, int row)
{
    wchar_t *path = NULL;

    if (!ui_copy_entry_snapshot(app, row, NULL, &path, NULL))
        return;
    if (!SHObjectProperties(hwnd, SHOP_FILEPATH, path, NULL)) {
        SHELLEXECUTEINFOW info;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_INVOKEIDLIST;
        info.hwnd = hwnd;
        info.lpVerb = L"properties";
        info.lpFile = path;
        info.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&info);
    }
    free(path);
}

/* ---- Simple modal prompt and language chooser ---------------------------- */

#define UI_PROMPT_CLASS   L"OEInputPrompt"
#define UI_LANGUAGE_CLASS L"OELanguagePrompt"

typedef struct {
    HWND hwnd;
    HWND edit;
    const wchar_t *label;
    const wchar_t *initial;
    wchar_t *out;
    size_t out_capacity;
    int accepted;
} PROMPT_BOX;

typedef struct {
    HWND hwnd;
    HWND list;
    int selection;
    int accepted;
} LANGUAGE_BOX;

static void ui_register_modal_class(const wchar_t *name, WNDPROC proc)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
}

static void ui_center_window(HWND hwnd, HWND owner)
{
    RECT box;
    RECT anchor;
    int x;
    int y;

    GetWindowRect(hwnd, &box);
    if (owner && IsWindow(owner) && !IsIconic(owner) &&
        GetWindowRect(owner, &anchor) &&
        MonitorFromRect(&anchor, MONITOR_DEFAULTTONULL) != NULL) {
        x = anchor.left + ((anchor.right - anchor.left) -
                           (box.right - box.left)) / 2;
        y = anchor.top + ((anchor.bottom - anchor.top) -
                          (box.bottom - box.top)) / 2;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
        x = anchor.left + ((anchor.right - anchor.left) -
                           (box.right - box.left)) / 2;
        y = anchor.top + ((anchor.bottom - anchor.top) -
                          (box.bottom - box.top)) / 2;
    }
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK ui_prompt_wndproc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    PROMPT_BOX *box = (PROMPT_BOX *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
    {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lParam;
        RECT rc;
        int dpi = g_dpi;
        int pad = MulDiv(16, dpi, 96);
        int edit_height = MulDiv(26, dpi, 96);
        int button_width = MulDiv(84, dpi, 96);
        int button_height = MulDiv(28, dpi, 96);

        box = (PROMPT_BOX *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)box);
        box->hwnd = hwnd;
        GetClientRect(hwnd, &rc);

        box->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", box->initial ? box->initial : L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            pad, pad + MulDiv(18, dpi, 96),
            rc.right - pad * 2, edit_height,
            hwnd, (HMENU)1001, g_hInst, NULL);
        CreateWindowExW(
            0, L"STATIC", box->label ? box->label : L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, pad, rc.right - pad * 2, MulDiv(18, dpi, 96),
            hwnd, NULL, g_hInst, NULL);
        CreateWindowExW(
            0, L"BUTTON", L("OK"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            rc.right - pad - button_width * 2 - MulDiv(8, dpi, 96),
            rc.bottom - pad - button_height,
            button_width, button_height,
            hwnd, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowExW(
            0, L"BUTTON", L("Cancel"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            rc.right - pad - button_width,
            rc.bottom - pad - button_height,
            button_width, button_height,
            hwnd, (HMENU)IDCANCEL, g_hInst, NULL);

        SendMessageW(box->edit, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(box->edit, EM_SETSEL, 0, -1);
        SetFocus(box->edit);
        return 0;
    }

    case WM_COMMAND:
        if (!box)
            break;
        if (LOWORD(wParam) == IDOK) {
            GetWindowTextW(box->edit, box->out, (int)box->out_capacity);
            box->accepted = box->out[0] != L'\0';
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            box->accepted = 0;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int ui_prompt_text(HWND owner, const wchar_t *title,
                          const wchar_t *label, const wchar_t *initial,
                          wchar_t *out, size_t out_capacity)
{
    PROMPT_BOX box;
    HWND hwnd;
    MSG msg;
    int width = MulDiv(430, g_dpi, 96);
    int height = MulDiv(180, g_dpi, 96);

    if (!out || out_capacity == 0)
        return 0;
    out[0] = L'\0';
    memset(&box, 0, sizeof(box));
    box.label = label;
    box.initial = initial;
    box.out = out;
    box.out_capacity = out_capacity;

    ui_register_modal_class(UI_PROMPT_CLASS, ui_prompt_wndproc);
    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, UI_PROMPT_CLASS, title ? title : L"",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        owner, NULL, g_hInst, &box);
    if (!hwnd)
        return 0;

    ui_center_window(hwnd, owner);
    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(box.edit);

    while (IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got == 0 || got == (BOOL)-1)
            break;
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return box.accepted;
}

static LRESULT CALLBACK ui_language_wndproc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam)
{
    LANGUAGE_BOX *box = (LANGUAGE_BOX *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
    {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lParam;
        RECT rc;
        int pad = MulDiv(16, g_dpi, 96);
        int button_width = MulDiv(84, g_dpi, 96);
        int button_height = MulDiv(28, g_dpi, 96);
        int i;

        box = (LANGUAGE_BOX *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)box);
        box->hwnd = hwnd;
        GetClientRect(hwnd, &rc);
        CreateWindowExW(
            0, L"STATIC", L("Select a language:"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, pad, rc.right - pad * 2, MulDiv(18, g_dpi, 96),
            hwnd, NULL, g_hInst, NULL);
        box->list = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            pad, pad + MulDiv(24, g_dpi, 96),
            rc.right - pad * 2,
            rc.bottom - pad * 2 - button_height - MulDiv(32, g_dpi, 96),
            hwnd, (HMENU)1002, g_hInst, NULL);
        for (i = 0; i < lang_count(); i++) {
            const LANG_ENTRY *entry = lang_entry(i);
            SendMessageW(box->list, LB_ADDSTRING, 0,
                         (LPARAM)(entry ? entry->native_name : L""));
        }
        SendMessageW(box->list, LB_SETCURSEL, (WPARAM)lang_current(), 0);
        CreateWindowExW(
            0, L"BUTTON", L("OK"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            rc.right - pad - button_width * 2 - MulDiv(8, g_dpi, 96),
            rc.bottom - pad - button_height,
            button_width, button_height,
            hwnd, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowExW(
            0, L"BUTTON", L("Cancel"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            rc.right - pad - button_width,
            rc.bottom - pad - button_height,
            button_width, button_height,
            hwnd, (HMENU)IDCANCEL, g_hInst, NULL);
        SendMessageW(box->list, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SetFocus(box->list);
        return 0;
    }

    case WM_COMMAND:
        if (!box)
            break;
        if (LOWORD(wParam) == IDOK ||
            (LOWORD(wParam) == 1002 && HIWORD(wParam) == LBN_DBLCLK)) {
            LRESULT selected = SendMessageW(box->list, LB_GETCURSEL, 0, 0);
            box->selection = selected >= 0 ? (int)selected : -1;
            box->accepted = box->selection >= 0;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            box->accepted = 0;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ui_choose_language(HWND owner)
{
    LANGUAGE_BOX box;
    HWND hwnd;
    MSG msg;
    int width = MulDiv(360, g_dpi, 96);
    int height = MulDiv(360, g_dpi, 96);

    memset(&box, 0, sizeof(box));
    box.selection = -1;
    ui_register_modal_class(UI_LANGUAGE_CLASS, ui_language_wndproc);
    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, UI_LANGUAGE_CLASS, L("Language"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        owner, NULL, g_hInst, &box);
    if (!hwnd)
        return;

    ui_center_window(hwnd, owner);
    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    while (IsWindow(hwnd)) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got == 0 || got == (BOOL)-1)
            break;
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }

    if (box.accepted && box.selection >= 0 && box.selection != lang_current()) {
        lang_set(box.selection);
        if (g_app_ptr)
            wcsncpy_s(g_app_ptr->language, 32, lang_current_id(), _TRUNCATE);
        ui_refresh_language(owner);
    }
}

static void ui_update_file_menu(void)
{
    int row;
    UINT state;

    if (!g_menu_file)
        return;
    row = ui_selected_row();
    state = (row >= 0) ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(g_menu_file, IDM_FILE_OPEN, MF_BYCOMMAND | state);
    EnableMenuItem(g_menu_file, IDM_FILE_OPEN_PATH, MF_BYCOMMAND | state);
    EnableMenuItem(g_menu_file, IDM_FILE_COPY_FULL_NAME, MF_BYCOMMAND | state);
    EnableMenuItem(g_menu_file, IDM_FILE_DELETE, MF_BYCOMMAND | state);
    EnableMenuItem(g_menu_file, IDM_FILE_RENAME, MF_BYCOMMAND | state);
    EnableMenuItem(g_menu_file, IDM_FILE_PROPERTIES, MF_BYCOMMAND | state);
}

static void ui_build_menu(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    HMENU menu;
    HMENU file;
    HMENU edit;
    HMENU view;
    HMENU search;
    HMENU index;
    HMENU help;
    HMENU theme;
    HMENU sort;
    HMENU language;
    HMENU old;
    UINT flags;
    int i;

    menu = CreateMenu();
    file = CreatePopupMenu();
    edit = CreatePopupMenu();
    view = CreatePopupMenu();
    search = CreatePopupMenu();
    index = CreatePopupMenu();
    help = CreatePopupMenu();
    theme = CreatePopupMenu();
    sort = CreatePopupMenu();
    language = CreatePopupMenu();
    if (!menu || !file || !edit || !view || !search || !index || !help ||
        !theme || !sort || !language) {
        if (menu) DestroyMenu(menu);
        return;
    }

    g_menu_view = view;
    g_menu_file = file;
    g_menu_theme = theme;
    g_menu_sort = sort;
    g_menu_language = language;

    AppendMenuW(file, MF_STRING, IDM_FILE_NEW_WINDOW,
                L("New Search Window\tCtrl+N"));
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L("Open\tEnter"));
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN_PATH, L("Open Path\tCtrl+Enter"));
    AppendMenuW(file, MF_STRING, IDM_FILE_COPY_FULL_NAME,
                L("Copy Full Name to Clipboard\tCtrl+Shift+C"));
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_DELETE, L("Delete\tDel"));
    AppendMenuW(file, MF_STRING, IDM_FILE_RENAME, L("Rename\tF2"));
    AppendMenuW(file, MF_STRING, IDM_FILE_PROPERTIES, L("Properties\tAlt+Enter"));
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L("Exit\tAlt+F4"));

    AppendMenuW(edit, MF_STRING, IDM_EDIT_COPY, L("Copy\tCtrl+C"));
    AppendMenuW(edit, MF_STRING, IDM_EDIT_COPY_PATH, L("Copy Full Path\tCtrl+Shift+C"));
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_EDIT_SELECT_ALL, L("Select All\tCtrl+A"));

    AppendMenuW(view, MF_STRING | (app->show_folders ? MF_CHECKED : 0),
                IDM_VIEW_FOLDERS, L("Folders"));
    AppendMenuW(view, MF_STRING | (app->show_filters ? MF_CHECKED : 0),
                IDM_VIEW_FILTERS, L("Filters"));
    AppendMenuW(view, MF_STRING | (app->show_status_bar ? MF_CHECKED : 0),
                IDM_VIEW_STATUS_BAR, L("Status Bar"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_VIEW_DETAILS, L("Details"));
    AppendMenuW(view, MF_STRING, IDM_VIEW_THUMBNAILS_MEDIUM, L("Medium Thumbnails"));
    AppendMenuW(view, MF_STRING, IDM_VIEW_THUMBNAILS_LARGE, L("Large Thumbnails"));
    AppendMenuW(view, MF_STRING, IDM_VIEW_THUMBNAILS_EXTRA_LARGE,
                L("Extra Large Thumbnails"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_LIGHT, L("Light"));
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_DARK, L("Dark"));
    AppendMenuW(theme, MF_STRING, IDM_VIEW_THEME_SYSTEM, L("System"));
    AppendMenuW(view, MF_POPUP, (UINT_PTR)theme, L("Themes"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    flags = app->match_case ? MF_CHECKED : 0;
    AppendMenuW(view, MF_STRING | flags, IDM_VIEW_MATCH_CASE, L("Match Case"));
    flags = app->match_whole_word ? MF_CHECKED : 0;
    AppendMenuW(view, MF_STRING | flags, IDM_VIEW_MATCH_WHOLE_WORD,
                L("Match Whole Word"));
    flags = app->match_path ? MF_CHECKED : 0;
    AppendMenuW(view, MF_STRING | flags, IDM_VIEW_MATCH_PATH, L("Match Path"));
    flags = app->use_regex ? MF_CHECKED : 0;
    AppendMenuW(view, MF_STRING | flags, IDM_VIEW_USE_REGEX, L("Use Regex"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_NAME, L("Name"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_PATH, L("Path"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_SIZE, L("Size"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_DATE_MODIFIED, L("Date Modified"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_DATE_CREATED, L("Date Created"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_ATTRIBUTES, L("Attributes"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_EXTENSION, L("Extension"));
    AppendMenuW(sort, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_ASCENDING, L("Ascending"));
    AppendMenuW(sort, MF_STRING, IDM_VIEW_SORT_DESCENDING, L("Descending"));
    AppendMenuW(view, MF_POPUP, (UINT_PTR)sort, L("Sort By"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < lang_count() &&
                i <= IDM_VIEW_LANGUAGE_LAST - IDM_VIEW_LANGUAGE_FIRST; i++) {
        const LANG_ENTRY *entry = lang_entry(i);
        if (entry)
            AppendMenuW(language, MF_STRING, IDM_VIEW_LANGUAGE_FIRST + i,
                        entry->native_name);
    }
    AppendMenuW(language, MF_SEPARATOR, 0, NULL);
    AppendMenuW(language, MF_STRING, IDM_VIEW_CHOOSE_LANGUAGE,
                L("Choose Language..."));
    AppendMenuW(language, MF_STRING, IDM_VIEW_LANGUAGE_RELOAD,
                L("Reload Language File"));
    AppendMenuW(view, MF_POPUP, (UINT_PTR)language, L("Language"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING | (app->close_to_tray ? MF_CHECKED : 0),
                IDM_VIEW_CLOSE_TO_TRAY, L("Close to Tray"));
    AppendMenuW(view, MF_STRING | (app->minimize_to_tray ? MF_CHECKED : 0),
                IDM_VIEW_MINIMIZE_TO_TRAY, L("Minimize to Tray"));
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_VIEW_REFRESH, L("Refresh\tF5"));

    AppendMenuW(search, MF_STRING, 10070, L("Add to Bookmarks..."));
    AppendMenuW(search, MF_STRING, 10071, L("Organize Filters..."));

    AppendMenuW(index, MF_STRING, IDM_INDEX_UPDATE, L("Update Index"));
    AppendMenuW(index, MF_STRING, IDM_INDEX_REBUILD, L("Rebuild Index"));

    AppendMenuW(help, MF_STRING, IDM_HELP_CMDLINE, L("Command Line Options"));
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L("About OpenEverything"));

    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L("File"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit, L("Edit"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view, L("View"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)search, L("Search"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)index, L("Index"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L("Help"));

    old = GetMenu(hwnd);
    if (old) {
        SetMenu(hwnd, NULL);
        DestroyMenu(old);
    }
    SetMenu(hwnd, menu);
    ui_update_view_menu();

    if (!g_accel) {
        ACCEL accelerators[2];
        accelerators[0].fVirt = FVIRTKEY | FCONTROL;
        accelerators[0].key = L'N';
        accelerators[0].cmd = IDM_FILE_NEW_WINDOW;
        accelerators[1].fVirt = FVIRTKEY;
        accelerators[1].key = VK_F5;
        accelerators[1].cmd = IDM_VIEW_REFRESH;
        g_accel = CreateAcceleratorTableW(accelerators, 2);
    }
    DrawMenuBar(hwnd);
}

static void ui_refresh_language(HWND hwnd)
{
    static const wchar_t *columns[] = {
        L"Name", L"Path", L"Size", L"Date Modified",
        L"Date Created", L"Attributes", L"Extension"
    };
    int i;

    ui_build_menu(hwnd);

    if (g_hwndList) {
        for (i = 0; i < (int)(sizeof(columns) / sizeof(columns[0])); i++) {
            LVCOLUMNW column;
            memset(&column, 0, sizeof(column));
            column.mask = LVCF_TEXT;
            column.pszText = (wchar_t *)lang_tr(columns[i]);
            ListView_SetColumn(g_hwndList, i, &column);
        }
    }
    if (g_hwndFilterList) {
        SendMessageW(g_hwndFilterList, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < FILTER_COUNT; i++)
            SendMessageW(g_hwndFilterList, LB_ADDSTRING, 0,
                         (LPARAM)lang_tr(g_filter_names[i]));
        SendMessageW(g_hwndFilterList, LB_SETCURSEL,
                     g_app_ptr ? g_app_ptr->selected_filter : 0, 0);
    }
    if (g_hwndSubfolders)
        SetWindowTextW(g_hwndSubfolders, L("Subfolders"));
    if (g_hwndFolderTree) {
        HTREEITEM root = TreeView_GetRoot(g_hwndFolderTree);
        if (root) {
            TVITEMW item;
            wchar_t text[64];
            wcsncpy_s(text, 64, L("Everything"), _TRUNCATE);
            memset(&item, 0, sizeof(item));
            item.mask = TVIF_TEXT | TVIF_HANDLE;
            item.hItem = root;
            item.pszText = text;
            TreeView_SetItem(g_hwndFolderTree, &item);
        }
    }
    if (g_hwndFolderHeader) InvalidateRect(g_hwndFolderHeader, NULL, TRUE);
    if (g_hwndFilterHeader) InvalidateRect(g_hwndFilterHeader, NULL, TRUE);
    if (g_hwndStatus) ui_update_status(g_hwndStatus, g_app_ptr);
    if (g_hwndList) InvalidateRect(g_hwndList, NULL, TRUE);
    DrawMenuBar(hwnd);
}

/* ---- Thumbnails ---------------------------------------------------------- */

static int ui_view_mode_pixels(int mode)
{
    switch (mode) {
    case VIEWMODE_MEDIUM_THUMBNAILS: return 48;
    case VIEWMODE_LARGE_THUMBNAILS: return 96;
    case VIEWMODE_EXTRA_LARGE_THUMBNAILS: return 256;
    default: return 0;
    }
}

static void ui_clear_thumb_cache(void)
{
    int i;

    for (i = 0; i < g_thumb_count; i++)
        free(g_thumbs[i].path);
    free(g_thumbs);
    g_thumbs = NULL;
    g_thumb_count = 0;
    g_thumb_capacity = 0;
}

static HBITMAP ui_load_thumbnail_bitmap(const wchar_t *path, int pixels)
{
    IShellItemImageFactory *factory = NULL;
    HBITMAP bitmap = NULL;
    SIZE size;
    HRESULT result;

    if (!path || !path[0])
        return NULL;
    size.cx = pixels;
    size.cy = pixels;
    result = SHCreateItemFromParsingName(
        path, NULL, &IID_IShellItemImageFactory, (void **)&factory);
    if (SUCCEEDED(result) && factory) {
        result = factory->lpVtbl->GetImage(
            factory, size, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &bitmap);
        if (FAILED(result) || !bitmap) {
            factory->lpVtbl->GetImage(
                factory, size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
        }
        factory->lpVtbl->Release(factory);
    }
    return bitmap;
}

static int ui_thumb_image_for_path(const wchar_t *path)
{
    int i;
    int limit;
    HBITMAP bitmap;
    int image;
    wchar_t *copy;

    if (!path || !path[0] || !g_thumb_images)
        return 0;
    for (i = 0; i < g_thumb_count; i++) {
        if (g_thumbs[i].path && _wcsicmp(g_thumbs[i].path, path) == 0)
            return g_thumbs[i].image;
    }

    limit = g_thumb_pixels >= 200 ? 256 : 1024;
    if (g_thumb_count >= limit)
        return 0;

    bitmap = ui_load_thumbnail_bitmap(path, g_thumb_pixels);
    if (!bitmap)
        return 0;
    image = ImageList_Add(g_thumb_images, bitmap, NULL);
    DeleteObject(bitmap);
    if (image < 0)
        return 0;

    if (g_thumb_count >= g_thumb_capacity) {
        int capacity = g_thumb_capacity ? g_thumb_capacity * 2 : 32;
        THUMB_ENTRY *next = (THUMB_ENTRY *)realloc(
            g_thumbs, (size_t)capacity * sizeof(THUMB_ENTRY));
        if (!next)
            return image;
        g_thumbs = next;
        g_thumb_capacity = capacity;
    }
    copy = _wcsdup(path);
    if (copy) {
        g_thumbs[g_thumb_count].path = copy;
        g_thumbs[g_thumb_count].image = image;
        g_thumb_count++;
    }
    return image;
}

static void ui_apply_view_mode(HWND hwnd)
{
    APP_STATE *app = g_app_ptr;
    LONG_PTR style;
    int pixels;

    if (!app || !g_hwndList)
        return;
    style = GetWindowLongPtrW(g_hwndList, GWL_STYLE);

    if (app->view_mode == VIEWMODE_DETAILS) {
        if ((style & LVS_TYPEMASK) != LVS_REPORT) {
            style = (style & ~LVS_TYPEMASK) | LVS_REPORT;
            SetWindowLongPtrW(g_hwndList, GWL_STYLE, style);
            SetWindowPos(g_hwndList, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (g_thumb_images) {
            ListView_SetImageList(g_hwndList, NULL, LVSIL_NORMAL);
            ImageList_Destroy(g_thumb_images);
            g_thumb_images = NULL;
        }
        ui_clear_thumb_cache();
        ListView_SetImageList(g_hwndList, g_image_small, LVSIL_SMALL);
    } else {
        pixels = ui_scale(ui_view_mode_pixels(app->view_mode));
        if (pixels < 16)
            pixels = 48;
        if ((style & LVS_TYPEMASK) != LVS_ICON) {
            style = (style & ~LVS_TYPEMASK) | LVS_ICON;
            SetWindowLongPtrW(g_hwndList, GWL_STYLE, style);
        }
        if (!g_thumb_images || g_thumb_pixels != pixels) {
            HICON icon = NULL;
            SHFILEINFOW info;

            if (g_thumb_images) {
                ListView_SetImageList(g_hwndList, NULL, LVSIL_NORMAL);
                ImageList_Destroy(g_thumb_images);
                g_thumb_images = NULL;
            }
            ui_clear_thumb_cache();
            g_thumb_images = ImageList_Create(pixels, pixels, ILC_COLOR32, 16, 16);
            g_thumb_pixels = pixels;
            if (g_thumb_images) {
                memset(&info, 0, sizeof(info));
                if (SHGetFileInfoW(L"file", FILE_ATTRIBUTE_NORMAL, &info,
                                   sizeof(info), SHGFI_ICON | SHGFI_LARGEICON))
                    icon = info.hIcon;
                if (icon) {
                    /* Index 0 is the fallback image for thumbnails that the
                       shell cannot render. */
                    ImageList_AddIcon(g_thumb_images, icon);
                    DestroyIcon(icon);
                }
                ListView_SetImageList(g_hwndList, g_thumb_images, LVSIL_NORMAL);
            }
        }
        ListView_SetIconSpacing(g_hwndList, pixels + ui_scale(28),
                                pixels + ui_scale(46));
        SetWindowPos(g_hwndList, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    ui_disable_horizontal_scrollbar(g_hwndList);
    ui_apply_layout(hwnd);
    InvalidateRect(g_hwndList, NULL, TRUE);
}

/* ---- Matched-text highlighting ------------------------------------------- */

static int ui_find_text_ci(const wchar_t *haystack, const wchar_t *needle)
{
    int i;
    int j;
    int case_sensitive;

    if (!haystack || !needle || !needle[0])
        return -1;
    case_sensitive = g_app_ptr && g_app_ptr->query.match_case;
    for (i = 0; haystack[i]; i++) {
        for (j = 0; needle[j]; j++) {
            wchar_t a = haystack[i + j];
            wchar_t b = needle[j];
            if (!a)
                return -1;
            if (!case_sensitive) {
                a = (wchar_t)towlower(a);
                b = (wchar_t)towlower(b);
            }
            if (a != b)
                break;
        }
        if (!needle[j])
            return i;
    }
    return -1;
}

static void ui_mark_matched_text(const wchar_t *name, int *flags, int count)
{
    const wchar_t *query;
    wchar_t token[160];
    int length;
    int i;

    if (!g_app_ptr || !name || !flags || count <= 0)
        return;
    query = g_app_ptr->query.text;
    if (!query || !query[0])
        return;

    length = 0;
    for (i = 0; ; i++) {
        wchar_t ch = query[i];
        if (ch == L' ' || ch == L'\t' || ch == L'\0') {
            int start;
            if (length > 0) {
                token[length] = L'\0';
                if (!wcschr(token, L':')) {
                    start = 0;
                    for (int k = 0; k <= length; k++) {
                        if (k == length || token[k] == L'*' || token[k] == L'?') {
                            if (k > start) {
                                wchar_t segment[160];
                                int segment_length = k - start;
                                const wchar_t *cursor = name;
                                int found;

                                if (segment_length >= 160)
                                    segment_length = 159;
                                wcsncpy_s(segment, 160, token + start,
                                          (size_t)segment_length);
                                segment[segment_length] = L'\0';
                                while ((found = ui_find_text_ci(cursor, segment)) >= 0) {
                                    int base = (int)(cursor - name) + found;
                                    for (int m = 0; m < segment_length; m++) {
                                        if (base + m < count)
                                            flags[base + m] = 1;
                                    }
                                    cursor += found + 1;
                                }
                            }
                            start = k + 1;
                        }
                    }
                }
                length = 0;
            }
            if (ch == L'\0')
                break;
        } else if (length < 159) {
            token[length++] = ch;
        }
    }
}

static LRESULT ui_custom_draw_list_item(NMLVCUSTOMDRAW *draw)
{
    APP_STATE *app = g_app_ptr;
    wchar_t *name = NULL;
    wchar_t *path = NULL;
    int is_directory = 0;
    int flags[512];
    int name_length;
    RECT rc;
    RECT text_rc;
    HDC dc;
    HGDIOBJ old_font;
    TEXTMETRICW metrics;
    COLORREF normal_color;
    COLORREF matched_color;
    HBRUSH background;
    int selected;
    int x;
    int y;
    int index;
    int icon_size;

    if (!app || !g_hwndList || app->view_mode != VIEWMODE_DETAILS)
        return CDRF_DODEFAULT;
    /* Nothing to highlight before the first query: let the control draw its
       normal (including selected) rows. */
    if (!app->query.text[0])
        return CDRF_DODEFAULT;
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        return CDRF_NOTIFYSUBITEMDRAW;
    if ((draw->nmcd.dwDrawStage & CDDS_SUBITEM) == 0 ||
        (draw->nmcd.dwDrawStage & CDDS_ITEMPREPAINT) == 0 ||
        draw->iSubItem != COL_NAME)
        return CDRF_DODEFAULT;

    index = (int)draw->nmcd.dwItemSpec;
    if (index < 0 || index >= app->filtered_count)
        return CDRF_DODEFAULT;
    if (!ui_copy_entry_snapshot(app, index, &name, &path, &is_directory))
        return CDRF_DODEFAULT;

    name_length = (int)wcslen(name);
    if (name_length <= 0 || name_length >= 512) {
        free(path);
        free(name);
        return CDRF_DODEFAULT;
    }
    memset(flags, 0, sizeof(flags));
    ui_mark_matched_text(name, flags, 511);

    /* For subitem zero, ListView_GetSubItemRect(...LVIR_BOUNDS) returns the
       whole row, which let long names paint over the Path column.  Custom
       draw already hands us the exact subitem rectangle. */
    rc = draw->nmcd.rc;
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        free(path);
        free(name);
        return CDRF_DODEFAULT;
    }

    dc = draw->nmcd.hdc;
    /* NMLVCUSTOMDRAW's uItemState is not reliable at the SUBITEM stage on all
       comctl32 versions (it made every row paint with the selection brush).
       Query the control so a normal, unselected row stays on the window
       background. */
    selected = (ListView_GetItemState(g_hwndList, index, LVIS_SELECTED) &
                LVIS_SELECTED) != 0;
    background = CreateSolidBrush(selected ? GetSysColor(COLOR_HIGHLIGHT)
                                           : g_color_window);
    FillRect(dc, &rc, background);
    DeleteObject(background);

    icon_size = GetSystemMetrics(SM_CXSMICON);
    x = rc.left + 2;
    if (g_image_small) {
        wchar_t extension[64];
        const wchar_t *dot = wcsrchr(name, L'.');
        extension[0] = L'\0';
        if (!is_directory && dot && dot[1])
            wcsncpy_s(extension, 64, dot + 1, _TRUNCATE);
        ImageList_Draw(g_image_small, ui_icon_for_type(is_directory, extension),
                       dc, x, rc.top + ((rc.bottom - rc.top) - icon_size) / 2,
                       ILD_TRANSPARENT);
    }
    x += icon_size + 6;

    text_rc = rc;
    text_rc.left = x;
    text_rc.right -= 4;
    SetBkMode(dc, TRANSPARENT);
    old_font = SelectObject(dc, g_font_ui ? g_font_ui :
                                GetStockObject(DEFAULT_GUI_FONT));
    GetTextMetricsW(dc, &metrics);
    y = rc.top + ((rc.bottom - rc.top) - metrics.tmHeight) / 2;
    normal_color = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : g_color_text;
    matched_color = selected ? normal_color :
                    (g_theme_is_dark ? RGB(255, 255, 255) : RGB(0, 0, 0));

    index = 0;
    while (index < name_length) {
        int end = index;
        int matched = flags[index];
        SIZE size;

        while (end < name_length && flags[end] == matched)
            end++;
        SelectObject(dc, matched && g_font_bold ? g_font_bold : g_font_ui);
        SetTextColor(dc, matched ? matched_color : normal_color);
        GetTextExtentPoint32W(dc, name + index, end - index, &size);
        if (x + size.cx > text_rc.right) {
            RECT ellipsis_rc = text_rc;
            SelectObject(dc, g_font_ui ? g_font_ui : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, normal_color);
            ellipsis_rc.left = x;
            DrawTextW(dc, L"...", 3, &ellipsis_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            break;
        }
        TextOutW(dc, x, y, name + index, end - index);
        x += size.cx;
        index = end;
    }
    SelectObject(dc, old_font);
    if (selected && (draw->nmcd.uItemState & CDIS_FOCUS))
        DrawFocusRect(dc, &rc);
    free(path);
    free(name);
    return CDRF_SKIPDEFAULT;
}

/* ---- Notification-area icon ---------------------------------------------- */

static void ui_tray_add(HWND hwnd)
{
    NOTIFYICONDATAW data;

    if (g_tray_added || !hwnd)
        return;
    if (!g_tray_icon) {
        g_tray_icon = (HICON)LoadImageW(
            g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR);
    }
    memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_TRAYICON;
    data.hIcon = g_tray_icon ? g_tray_icon : LoadIconW(NULL, IDI_APPLICATION);
    wcsncpy_s(data.szTip, 128, L"OpenEverything", _TRUNCATE);
    {
        BOOL added = Shell_NotifyIconW(NIM_ADD, &data);
        DWORD error = added ? 0 : GetLastError();
        if (added)
            g_tray_added = 1;
        /* Diagnostic/automation hooks: confirm the shell accepted the icon
           even when the user has placed it in the overflow area. */
        SetPropW(hwnd, L"OpenEverythingTrayIcon", (HANDLE)(INT_PTR)g_tray_added);
        SetPropW(hwnd, L"OpenEverythingTrayError", (HANDLE)(INT_PTR)error);
    }
}

static void ui_tray_remove(void)
{
    NOTIFYICONDATAW data;

    if (g_tray_added && g_app_ptr && g_app_ptr->hwnd_main) {
        memset(&data, 0, sizeof(data));
        data.cbSize = sizeof(data);
        data.hWnd = g_app_ptr->hwnd_main;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        RemovePropW(g_app_ptr->hwnd_main, L"OpenEverythingTrayIcon");
        RemovePropW(g_app_ptr->hwnd_main, L"OpenEverythingTrayError");
    }
    g_tray_added = 0;
    if (g_tray_icon) {
        DestroyIcon(g_tray_icon);
        g_tray_icon = NULL;
    }
}

static void ui_show_window(HWND hwnd)
{
    if (!hwnd)
        return;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void ui_hide_to_tray(HWND hwnd)
{
    static int balloon_shown;

    if (!hwnd)
        return;
    ShowWindow(hwnd, SW_HIDE);
    if (!balloon_shown && g_tray_added) {
        NOTIFYICONDATAW data;
        memset(&data, 0, sizeof(data));
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(data.szInfoTitle, 64, L"OpenEverything", _TRUNCATE);
        wcsncpy_s(data.szInfo, 256,
                  L("OpenEverything is still running in the notification area."),
                  _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
        balloon_shown = 1;
    }
}

static void ui_tray_menu(HWND hwnd)
{
    HMENU menu;
    POINT point;
    int command;

    menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L("Show Window"));
    AppendMenuW(menu, MF_STRING, IDM_TRAY_HIDE, L("Hide Window"));
    AppendMenuW(menu, MF_STRING, IDM_TRAY_NEW_WINDOW,
                L("New Search Window\tCtrl+N"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_app_ptr && g_app_ptr->close_to_tray
                                   ? MF_CHECKED : 0),
                IDM_TRAY_CLOSE_TO_TRAY, L("Close to Tray"));
    AppendMenuW(menu, MF_STRING | (g_app_ptr && g_app_ptr->minimize_to_tray
                                   ? MF_CHECKED : 0),
                IDM_TRAY_MINIMIZE_TO_TRAY, L("Minimize to Tray"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L("Exit"));

    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                             TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
    case IDM_TRAY_SHOW:
        ui_show_window(hwnd);
        break;
    case IDM_TRAY_HIDE:
        ui_hide_to_tray(hwnd);
        break;
    case IDM_TRAY_NEW_WINDOW:
        ui_open_new_search_window(hwnd);
        break;
    case IDM_TRAY_CLOSE_TO_TRAY:
        if (g_app_ptr) {
            g_app_ptr->close_to_tray = !g_app_ptr->close_to_tray;
            ui_update_view_menu();
        }
        break;
    case IDM_TRAY_MINIMIZE_TO_TRAY:
        if (g_app_ptr) {
            g_app_ptr->minimize_to_tray = !g_app_ptr->minimize_to_tray;
            ui_update_view_menu();
        }
        break;
    case IDM_TRAY_EXIT:
        if (g_app_ptr) {
            g_app_ptr->force_exit = 1;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    }
}

static void ui_select_initial_result(APP_STATE *app)
{
    int i;

    if (!app || !g_hwndList || !app->initial_select[0])
        return;
    for (i = 0; i < app->filtered_count; i++) {
        wchar_t *name = NULL;
        if (ui_copy_entry_snapshot(app, i, &name, NULL, NULL)) {
            int match = _wcsicmp(name, app->initial_select) == 0;
            free(name);
            if (match) {
                ListView_SetItemState(g_hwndList, i,
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g_hwndList, i, FALSE);
                app->initial_select[0] = L'\0';
                if (app->focus_results_on_start)
                    SetFocus(g_hwndList);
                return;
            }
        }
    }
}

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
                                    g_font_menu ? g_font_menu :
                                    GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(draw->draw.hDC, text, -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(draw->draw.hDC, old_font);
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        ui_update_view_menu();
        if (g_menu_file && (HMENU)wParam == g_menu_file)
            ui_update_file_menu();
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
        
        /* Build the full, localised menu bar. */
        ui_build_menu(hwnd);
        
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
        
        col.pszText = (wchar_t *)L("Name");          col.cx = ui_scale(app->column_width_name);     col.iSubItem = 0; ListView_InsertColumn(g_hwndList, 0, &col);
        col.pszText = (wchar_t *)L("Path");          col.cx = ui_scale(app->column_width_path);     col.iSubItem = 1; ListView_InsertColumn(g_hwndList, 1, &col);
        col.pszText = (wchar_t *)L("Size");          col.cx = ui_scale(app->column_width_size);     col.iSubItem = 2; ListView_InsertColumn(g_hwndList, 2, &col);
        col.pszText = (wchar_t *)L("Date Modified"); col.cx = ui_scale(app->column_width_modified); col.iSubItem = 3; ListView_InsertColumn(g_hwndList, 3, &col);
        col.pszText = (wchar_t *)L("Date Created");  col.cx = 0;   col.iSubItem = 4; ListView_InsertColumn(g_hwndList, 4, &col);
        col.pszText = (wchar_t *)L("Attributes");    col.cx = ui_scale(app->column_width_attributes); col.iSubItem = COL_ATTRIBUTES; ListView_InsertColumn(g_hwndList, COL_ATTRIBUTES, &col);
        col.pszText = (wchar_t *)L("Extension");     col.cx = ui_scale(app->column_width_extension);  col.iSubItem = COL_EXTENSION;  ListView_InsertColumn(g_hwndList, COL_EXTENSION, &col);
        ui_update_sort_indicator(g_hwndList, app->query.sort_column,
                                 app->query.sort_ascending);
        ui_disable_horizontal_scrollbar(g_hwndList);
        
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
            0, L"STATIC", L("Folders"),
            WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
            0, 0, 0, 0, hwnd, NULL, cs->hInstance, NULL);
        g_hwndFolderTree = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_FOLDER_TREE, cs->hInstance, NULL);
        g_hwndSubfolders = CreateWindowExW(
            0, L"BUTTON", L("Subfolders"),
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SUBFOLDERS, cs->hInstance, NULL);
        g_hwndFilterHeader = CreateWindowExW(
            0, L"STATIC", L("Filters"),
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
            SendMessageW(g_hwndFilterList, LB_ADDSTRING, 0,
                         (LPARAM)lang_tr(g_filter_names[i]));
        SendMessageW(g_hwndFilterList, LB_SETCURSEL, app->selected_filter, 0);
        ui_populate_folder_roots();
        ui_update_view_menu();
        ui_apply_theme(hwnd);
        ui_apply_view_mode(hwnd);
        ui_apply_layout(hwnd);
        ui_start_metadata_worker(app, hwnd);
        ui_tray_add(hwnd);
        
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("Loading index cache..."));
        ui_start_cache_load(hwnd);

        if (app->initial_search[0]) {
            SetWindowTextW(g_hwndSearch, app->initial_search);
            SetFocus(g_hwndSearch);
        } else if (app->focus_results_on_start) {
            SetFocus(g_hwndList);
        }
        if (app->show_cmdhelp_on_start)
            PostMessageW(hwnd, WM_COMMAND, IDM_HELP_CMDLINE, 0);
        if (app->show_language_on_start)
            PostMessageW(hwnd, WM_COMMAND, IDM_VIEW_CHOOSE_LANGUAGE, 0);
        
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
        if (wParam == SIZE_MINIMIZED && app && app->minimize_to_tray &&
            !app->force_exit) {
            ui_hide_to_tray(hwnd);
            return 0;
        }
        ui_apply_layout(hwnd);
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (app && app->theme_mode == THEME_SYSTEM)
            ui_apply_theme(hwnd);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    
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
        if (id >= IDM_VIEW_LANGUAGE_FIRST && id <= IDM_VIEW_LANGUAGE_LAST) {
            int index = (int)id - IDM_VIEW_LANGUAGE_FIRST;
            if (index < lang_count() && index != lang_current()) {
                lang_set(index);
                wcsncpy_s(app->language, 32, lang_current_id(), _TRUNCATE);
                ui_refresh_language(hwnd);
            }
            return 0;
        }
        
        switch (id) {
        case IDM_FILE_NEW_WINDOW:
            ui_open_new_search_window(hwnd);
            break;

        case IDM_FILE_OPEN:
        {
            int row = ui_selected_row();
            if (row >= 0) {
                wchar_t *path = NULL;
                if (ui_copy_entry_snapshot(app, row, NULL, &path, NULL))
                    ui_open_entry_path(hwnd, path);
                free(path);
            }
            break;
        }

        case IDM_FILE_OPEN_PATH:
        {
            int row = ui_selected_row();
            if (row >= 0) {
                wchar_t *path = NULL;
                if (ui_copy_entry_snapshot(app, row, NULL, &path, NULL))
                    ui_open_entry_parent(hwnd, path);
                free(path);
            }
            break;
        }

        case IDM_FILE_COPY_FULL_NAME:
        {
            int row = ui_selected_row();
            if (row >= 0) {
                wchar_t *path = NULL;
                if (ui_copy_entry_snapshot(app, row, NULL, &path, NULL))
                    ui_copy_text_to_clipboard(hwnd, path);
                free(path);
            }
            break;
        }

        case IDM_FILE_DELETE:
        {
            int row = ui_selected_row();
            if (row >= 0)
                ui_delete_selected(hwnd, app, row);
            break;
        }

        case IDM_FILE_RENAME:
        {
            int row = ui_selected_row();
            if (row >= 0)
                ui_rename_selected(hwnd, app, row);
            break;
        }

        case IDM_FILE_PROPERTIES:
        {
            int row = ui_selected_row();
            if (row >= 0)
                ui_show_properties(hwnd, app, row);
            break;
        }

        case IDM_FILE_EXIT:
            app->force_exit = 1;
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
            
        case IDM_EDIT_COPY:
        {
            int sel = ListView_GetNextItem(g_hwndList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < app->filtered_count) {
                wchar_t *name = NULL;
                if (ui_copy_entry_snapshot(app, sel, &name, NULL, NULL))
                    ui_copy_text_to_clipboard(hwnd, name);
                free(name);
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

        case IDM_VIEW_STATUS_BAR:
            app->show_status_bar = !app->show_status_bar;
            ui_update_view_menu();
            ui_apply_layout(hwnd);
            break;

        case IDM_VIEW_DETAILS:
            app->view_mode = VIEWMODE_DETAILS;
            ui_update_view_menu();
            ui_apply_view_mode(hwnd);
            break;

        case IDM_VIEW_THUMBNAILS_MEDIUM:
            app->view_mode = VIEWMODE_MEDIUM_THUMBNAILS;
            ui_update_view_menu();
            ui_apply_view_mode(hwnd);
            break;

        case IDM_VIEW_THUMBNAILS_LARGE:
            app->view_mode = VIEWMODE_LARGE_THUMBNAILS;
            ui_update_view_menu();
            ui_apply_view_mode(hwnd);
            break;

        case IDM_VIEW_THUMBNAILS_EXTRA_LARGE:
            app->view_mode = VIEWMODE_EXTRA_LARGE_THUMBNAILS;
            ui_update_view_menu();
            ui_apply_view_mode(hwnd);
            break;

        case IDM_VIEW_CLOSE_TO_TRAY:
            app->close_to_tray = !app->close_to_tray;
            ui_update_view_menu();
            break;

        case IDM_VIEW_MINIMIZE_TO_TRAY:
            app->minimize_to_tray = !app->minimize_to_tray;
            ui_update_view_menu();
            break;

        case IDM_VIEW_CHOOSE_LANGUAGE:
            ui_choose_language(hwnd);
            break;

        case IDM_VIEW_LANGUAGE_RELOAD:
            lang_reload();
            ui_refresh_language(hwnd);
            break;

        case IDM_TRAY_SHOW:
            ui_show_window(hwnd);
            break;

        case IDM_TRAY_HIDE:
            ui_hide_to_tray(hwnd);
            break;

        case IDM_TRAY_NEW_WINDOW:
            ui_open_new_search_window(hwnd);
            break;

        case IDM_TRAY_CLOSE_TO_TRAY:
            app->close_to_tray = !app->close_to_tray;
            ui_update_view_menu();
            break;

        case IDM_TRAY_MINIMIZE_TO_TRAY:
            app->minimize_to_tray = !app->minimize_to_tray;
            ui_update_view_menu();
            break;

        case IDM_TRAY_EXIT:
            app->force_exit = 1;
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
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
            
        case IDM_VIEW_SORT_NAME:          ui_change_sort(hwnd, app, COL_NAME, 2);          break;
        case IDM_VIEW_SORT_PATH:          ui_change_sort(hwnd, app, COL_PATH, 2);          break;
        case IDM_VIEW_SORT_SIZE:          ui_change_sort(hwnd, app, COL_SIZE, 2);          break;
        case IDM_VIEW_SORT_DATE_MODIFIED: ui_change_sort(hwnd, app, COL_DATE_MODIFIED, 2); break;
        case IDM_VIEW_SORT_DATE_CREATED:  ui_change_sort(hwnd, app, COL_DATE_CREATED, 2);  break;
        case IDM_VIEW_SORT_ATTRIBUTES:    ui_change_sort(hwnd, app, COL_ATTRIBUTES, 2);    break;
        case IDM_VIEW_SORT_EXTENSION:     ui_change_sort(hwnd, app, COL_EXTENSION, 2);     break;

        case IDM_VIEW_SORT_ASCENDING:
        case IDM_VIEW_SORT_DESCENDING:
            app->query.sort_ascending = id == IDM_VIEW_SORT_ASCENDING;
            ui_update_sort_indicator(g_hwndList, app->query.sort_column,
                                     app->query.sort_ascending);
            ui_update_view_menu();
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
            
        case IDM_HELP_CMDLINE:
            cmdhelp_show(hwnd, L("Command Line Options"), L("Close"));
            break;

        case IDM_HELP_ABOUT:
        {
            wchar_t about[2048];
            swprintf_s(about, 2048,
                L("OpenEverything v%s\n\n"
                  "An open-source Everything clone.\n\n"
                  "How it works:\n"
                  "A fast file-name index is built from the NTFS MFT/USN Journal,\n"
                  "local caching avoids a full rebuild on every start,\n"
                  "and searching runs on a background thread with a virtual list.\n\n"
                  "Project:\n"
                  "https://github.com/DisaWdcba"),
                OE_VERSION_WSTRING);
            MessageBoxW(hwnd, about, L("About OpenEverything"),
                        MB_OK | MB_ICONINFORMATION);
            break;
        }
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
        if (nm->hwndFrom == g_hwndList && nm->code == NM_CUSTOMDRAW)
            return ui_custom_draw_list_item((NMLVCUSTOMDRAW *)lParam);
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
                wchar_t *thumb_path = NULL;
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
                    index_copy_entry_extension_locked(app, e, icon_ext, 64);
                    if (app->view_mode != VIEWMODE_DETAILS &&
                        (di->item.mask & LVIF_IMAGE) && !g_in_scroll_thumb)
                        thumb_path = index_duplicate_entry_path_locked(app, entry_index);
                    
                    if (di->item.mask & LVIF_TEXT) {
                        switch (di->item.iSubItem) {
                        case COL_NAME:
                            /* Truncate rather than trip the invalid parameter
                               handler: a name longer than the cell buffer would
                               otherwise terminate the process mid-paint. */
                            if (!index_copy_entry_name_locked(app, e,
                                                              text_buf, 512)) {
                                wchar_t *full_name =
                                    index_duplicate_entry_name_locked(app, e);
                                wcsncpy_s(text_buf, 512,
                                          full_name ? full_name : L"", _TRUNCATE);
                                free(full_name);
                            }
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
                        case COL_EXTENSION:
                            index_copy_entry_extension_locked(app, e,
                                                              text_buf, 512);
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
                    if (thumb_path) {
                        di->item.iImage = ui_thumb_image_for_path(thumb_path);
                    } else if (app->view_mode != VIEWMODE_DETAILS) {
                        di->item.iImage = 0;
                    } else {
                        di->item.iImage = g_in_scroll_thumb
                            ? (icon_is_dir ? g_icon_folder : g_icon_file)
                            : ui_icon_for_type(icon_is_dir, icon_ext);
                    }
                }
                free(thumb_path);
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
                int row = ui_selected_row();
                int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                int alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

                if (row >= 0) {
                    if (kd->wVKey == VK_DELETE) {
                        ui_delete_selected(hwnd, app, row);
                    } else if (kd->wVKey == VK_F2) {
                        ui_rename_selected(hwnd, app, row);
                    } else if (kd->wVKey == VK_RETURN) {
                        wchar_t *path = NULL;
                        if (alt) {
                            ui_show_properties(hwnd, app, row);
                        } else if (ui_copy_entry_snapshot(app, row, NULL, &path, NULL)) {
                            if (ctrl)
                                ui_open_entry_parent(hwnd, path);
                            else
                                ui_open_entry_path(hwnd, path);
                        }
                        free(path);
                    } else if (kd->wVKey == L'C' && ctrl && shift) {
                        wchar_t *path = NULL;
                        if (ui_copy_entry_snapshot(app, row, NULL, &path, NULL))
                            ui_copy_text_to_clipboard(hwnd, path);
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
                
                /* The shell menu already provides Open, Rename, Delete and
                   Properties (Rename requires CMF_CANRENAME, set below).
                   Only add the actions the shell does not expose here. */
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_OPEN_PATH, L("Open Path"));
                AppendMenuW(hPopup, MF_STRING, IDM_CTX_COPY_FULL_NAME,
                            L("Copy Full Name to Clipboard"));
                AppendMenuW(hPopup, MF_SEPARATOR, 0, NULL);
                ui_append_shell_context_menu(hwnd, hPopup, entry_path, &shell_menu);
                
                cmd = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, NULL);
                
                switch (cmd) {
                case IDM_CTX_OPEN_PATH:
                    ui_open_entry_parent(hwnd, entry_path);
                    break;
                case IDM_CTX_COPY_FULL_NAME:
                    ui_copy_text_to_clipboard(hwnd, entry_path);
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
        wchar_t *command = (wchar_t *)lParam;

        /* A forwarded command line arrives as a heap string owned by us.
           Route it through the edit control so the visible text matches the
           query that actually runs -- ui_start_search reads the box, not
           app->query.text. EN_CHANGE queues the search. */
        if (command) {
            if (g_hwndSearch) {
                SetWindowTextW(g_hwndSearch, command);
                SetFocus(g_hwndSearch);
            }
            free(command);

            if (IsIconic(hwnd))
                ShowWindow(hwnd, SW_RESTORE);
            else
                ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        }

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
                ListView_SetItemState(g_hwndList, -1, 0,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ui_update_listview(g_hwndList, app);
                ui_update_status(g_hwndStatus, app);
                ui_select_initial_result(app);
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
        if (pct >= INDEX_PROGRESS_FALLBACK_BASE &&
            pct <= INDEX_PROGRESS_FALLBACK_BASE + 100) {
            swprintf_s(buf, 256, L("Indexing %s (compatibility scan) %d%%"),
                       label, pct - INDEX_PROGRESS_FALLBACK_BASE);
        } else if (pct == INDEX_PROGRESS_SORTING) {
            wcsncpy_s(buf, 256, L("Sorting file names..."), _TRUNCATE);
        } else if (pct == INDEX_PROGRESS_BUILDING) {
            wcsncpy_s(buf, 256, L("Building search index..."), _TRUNCATE);
        } else if (pct == INDEX_PROGRESS_SAVING) {
            wcsncpy_s(buf, 256, L("Saving index..."), _TRUNCATE);
        } else if (pct > 0) {
            swprintf_s(buf, 256, L("Indexing %s %d%%"), label, pct);
        } else {
            swprintf_s(buf, 256, L("Indexing %s"), label);
        }
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
        if (app->start_reindex) {
            app->start_reindex = 0;
            PostMessageW(hwnd, WM_REFRESH, 0, 0);
        }
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
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("  Indexing already in progress"));
            return 0;
        }
        
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("  Indexing..."));
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
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("  Failed to start indexing"));
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
            SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, (LPARAM)L("  Failed to start indexing"));
        }
        return 0;
    }
    
    /* ---- Notification-area icon ---- */
    case WM_TRAYICON:
    {
        UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP)
            ui_show_window(hwnd);
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
            ui_tray_menu(hwnd);
        return 0;
    }

    /* ---- Window close/destroy ---- */
    case WM_CLOSE:
        if (!app->force_exit && app->close_to_tray) {
            ui_hide_to_tray(hwnd);
            return 0;
        }
        if (g_hwndList) {
            /* Persist in logical units so a later DPI change keeps proportions. */
            app->column_width_name = ui_unscale(ListView_GetColumnWidth(g_hwndList, 0));
            app->column_width_path = ui_unscale(ListView_GetColumnWidth(g_hwndList, 1));
            app->column_width_size = ui_unscale(ListView_GetColumnWidth(g_hwndList, 2));
            app->column_width_modified = ui_unscale(ListView_GetColumnWidth(g_hwndList, 3));
            app->column_width_attributes = ui_unscale(
                ListView_GetColumnWidth(g_hwndList, COL_ATTRIBUTES));
            app->column_width_extension = ui_unscale(
                ListView_GetColumnWidth(g_hwndList, COL_EXTENSION));
        }
        {
            /* GetWindowPlacement reports the restored rect even when the window
               is currently maximized, which is what we want to store. */
            WINDOWPLACEMENT placement;
            placement.length = sizeof(placement);
            if (GetWindowPlacement(hwnd, &placement)) {
                const RECT *r = &placement.rcNormalPosition;
                app->window_x = r->left;
                app->window_y = r->top;
                app->window_width = ui_unscale(r->right - r->left);
                app->window_height = ui_unscale(r->bottom - r->top);
                app->window_maximized =
                    placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0;
            }
            app->sort_column = app->query.sort_column;
            app->sort_ascending = app->query.sort_ascending;
        }
        if (!app->secondary_window)
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
        ui_tray_remove();
        if (g_accel) {
            DestroyAcceleratorTable(g_accel);
            g_accel = NULL;
        }
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
        DrawTextW(dc, panel == 1 ? L("Folders") : L("Filters"), -1, &text_rc,
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
        DrawTextW(dc, L("Subfolders"), -1, &text_rect,
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
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000) &&
            g_app_ptr) {
            int row = ui_selected_row();
            if (row >= 0)
                ui_show_properties(hwnd, g_app_ptr, row);
            return 0;
        }
        break;

    case WM_STYLECHANGING:
        if ((int)wParam == GWL_STYLE && lParam) {
            STYLESTRUCT *styles = (STYLESTRUCT *)lParam;
            styles->styleNew &= ~WS_HSCROLL;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);

    case WM_VSCROLL:
        scroll_code = LOWORD(wParam);
        if (scroll_code == SB_THUMBTRACK)
            g_in_scroll_thumb = 1;

        result = DefSubclassProc(hwnd, msg, wParam, lParam);

        if (scroll_code == SB_ENDSCROLL && g_in_scroll_thumb) {
            g_in_scroll_thumb = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        ui_disable_horizontal_scrollbar(hwnd);
        return result;

    case WM_HSCROLL:
        ui_disable_horizontal_scrollbar(hwnd);
        return 0;

    case WM_NOTIFY:
        if (lParam) {
            NMHDR *notification = (NMHDR *)lParam;
            if (notification->hwndFrom == ListView_GetHeader(hwnd) &&
                notification->code == NM_CUSTOMDRAW)
                return ui_custom_draw_list_header((NMCUSTOMDRAW *)lParam);
        }
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        ui_disable_horizontal_scrollbar(hwnd);
        return result;

    case WM_SIZE:
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        ui_disable_horizontal_scrollbar(hwnd);
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
