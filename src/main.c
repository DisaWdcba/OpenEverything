#include "common.h"
#include "cli.h"
#include "ui.h"
#include "config.h"
#include "ipc.h"
#include "index.h"
#include "lang.h"
#include "cmdhelp.h"

APP_STATE g_app;

typedef struct {
    int new_window;
    int show_help;
    int show_language;
    int start_reindex;
    int topmost;
    int view_mode;          /* -1 when not specified */
    int filter;             /* -1 when not specified */
    int sort_column;        /* -1 when not specified */
    int sort_ascending;     /* -1 when not specified */
    int match_case;
    int match_whole_word;
    int match_path;
    int use_regex;
    int focus_results;
    int start_minimized;
    int start_maximized;
    int has_options;
    int exit_after_start;
    wchar_t config_path[MAX_PATH];
    wchar_t language[32];
    wchar_t search[512];
    wchar_t select[260];
} GUI_OPTIONS;

static int process_is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    int result = 0;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &size))
            result = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return result;
}

/* Development/testing escape hatch: OPEN_EVERYTHING_NO_ELEVATE=1 starts the
   GUI with the current token instead of re-launching through UAC.  The normal
   launch path is unchanged. */
static int elevation_disabled(void)
{
    wchar_t value[8];
    return GetEnvironmentVariableW(L"OPEN_EVERYTHING_NO_ELEVATE", value, 8) > 0;
}

static int relaunch_gui_elevated(const wchar_t *parameters, int nCmdShow)
{
    wchar_t module[MAX_PATH];
    SHELLEXECUTEINFOW execute;
    DWORD error;

    if (!GetModuleFileNameW(NULL, module, MAX_PATH))
        return 0;
    memset(&execute, 0, sizeof(execute));
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = parameters && parameters[0] ? parameters : NULL;
    execute.nShow = nCmdShow;
    if (!ShellExecuteExW(&execute)) {
        error = GetLastError();
        if (error != ERROR_CANCELLED)
            MessageBoxW(NULL, L"Failed to start OpenEverything as administrator.",
                        L"OpenEverything", MB_OK | MB_ICONERROR);
        return 0;
    }
    CloseHandle(execute.hProcess);
    return 1;
}

static int arg_is(const wchar_t *arg, const wchar_t *name)
{
    return arg && _wcsicmp(arg, name) == 0;
}

static int parse_sort_name(const wchar_t *name, int *column)
{
    if (!name || !column)
        return 0;
    if (arg_is(name, L"name")) *column = COL_NAME;
    else if (arg_is(name, L"path")) *column = COL_PATH;
    else if (arg_is(name, L"size")) *column = COL_SIZE;
    else if (arg_is(name, L"modified") || arg_is(name, L"date-modified") ||
             arg_is(name, L"datemodified")) *column = COL_DATE_MODIFIED;
    else if (arg_is(name, L"created") || arg_is(name, L"date-created") ||
             arg_is(name, L"datecreated")) *column = COL_DATE_CREATED;
    else if (arg_is(name, L"attributes")) *column = COL_ATTRIBUTES;
    else if (arg_is(name, L"extension") || arg_is(name, L"ext")) *column = COL_EXTENSION;
    else return 0;
    return 1;
}

static int parse_filter_name(const wchar_t *name)
{
    if (!name)
        return -1;
    if (arg_is(name, L"everything")) return FILTER_EVERYTHING;
    if (arg_is(name, L"audio")) return FILTER_AUDIO;
    if (arg_is(name, L"compressed")) return FILTER_COMPRESSED;
    if (arg_is(name, L"document")) return FILTER_DOCUMENT;
    if (arg_is(name, L"executable")) return FILTER_EXECUTABLE;
    if (arg_is(name, L"folder")) return FILTER_FOLDER;
    if (arg_is(name, L"image")) return FILTER_IMAGE;
    if (arg_is(name, L"video")) return FILTER_VIDEO;
    return -1;
}

static void append_search_text(GUI_OPTIONS *options, const wchar_t *text)
{
    size_t used;

    if (!text || !text[0])
        return;
    used = wcslen(options->search);
    if (used + wcslen(text) + 2 >= sizeof(options->search) / sizeof(wchar_t))
        return;
    if (used)
        options->search[used++] = L' ';
    wcscpy_s(options->search + used,
             sizeof(options->search) / sizeof(wchar_t) - used, text);
}

static const wchar_t *option_value(int argc, wchar_t **argv, int *index)
{
    if (*index + 1 >= argc)
        return NULL;
    (*index)++;
    return argv[*index];
}

/* Parse the Everything-compatible command line into GUI options.  Unknown
   switches are ignored rather than being mistaken for a search query, which
   is what used to happen: `OpenEverything.exe -newwindow` searched for the
   literal text "-newwindow". */
static void parse_gui_options(int argc, wchar_t **argv, GUI_OPTIONS *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    options->view_mode = -1;
    options->filter = -1;
    options->sort_column = -1;
    options->sort_ascending = -1;
    options->match_case = -1;
    options->match_whole_word = -1;
    options->match_path = -1;
    options->use_regex = -1;

    for (i = 1; argv && i < argc; i++) {
        const wchar_t *arg = argv[i];

        if (!arg || !arg[0])
            continue;
        if (arg[0] != L'-') {
            append_search_text(options, arg);
            continue;
        }

        /* Search-text switches are still a plain search request; any other
           switch means the launch needs the full GUI rather than a forward. */
        if (!arg_is(arg, L"-s") && !arg_is(arg, L"-search") &&
            !arg_is(arg, L"-filename") && !arg_is(arg, L"-p") &&
            !arg_is(arg, L"-path") && !arg_is(arg, L"-parent") &&
            !arg_is(arg, L"-name-part"))
            options->has_options = 1;

        if (arg_is(arg, L"-newwindow")) {
            options->new_window = 1;
        } else if (arg_is(arg, L"-nonewwindow")) {
            options->new_window = 0;
        } else if (arg_is(arg, L"-help") || arg_is(arg, L"-h") ||
                   arg_is(arg, L"-?")) {
            options->show_help = 1;
        } else if (arg_is(arg, L"-choose-language")) {
            options->show_language = 1;
        } else if (arg_is(arg, L"-language")) {
            const wchar_t *value = option_value(argc, argv, &i);
            if (value)
                wcsncpy_s(options->language, 32, value, _TRUNCATE);
        } else if (arg_is(arg, L"-config")) {
            const wchar_t *value = option_value(argc, argv, &i);
            if (value)
                wcsncpy_s(options->config_path, MAX_PATH, value, _TRUNCATE);
        } else if (arg_is(arg, L"-s") || arg_is(arg, L"-search") ||
                   arg_is(arg, L"-filename")) {
            const wchar_t *value = option_value(argc, argv, &i);
            if (value) {
                wcsncpy_s(options->search,
                          sizeof(options->search) / sizeof(wchar_t),
                          value, _TRUNCATE);
            }
        } else if (arg_is(arg, L"-p") || arg_is(arg, L"-path") ||
                   arg_is(arg, L"-parent") || arg_is(arg, L"-name-part")) {
            const wchar_t *value = option_value(argc, argv, &i);
            append_search_text(options, value);
        } else if (arg_is(arg, L"-select")) {
            const wchar_t *value = option_value(argc, argv, &i);
            if (value)
                wcsncpy_s(options->select,
                          sizeof(options->select) / sizeof(wchar_t),
                          value, _TRUNCATE);
        } else if (arg_is(arg, L"-filter")) {
            int filter = parse_filter_name(option_value(argc, argv, &i));
            if (filter >= 0)
                options->filter = filter;
        } else if (arg_is(arg, L"-sort")) {
            int column;
            if (parse_sort_name(option_value(argc, argv, &i), &column))
                options->sort_column = column;
        } else if (arg_is(arg, L"-sort-ascending")) {
            options->sort_ascending = 1;
        } else if (arg_is(arg, L"-sort-descending")) {
            options->sort_ascending = 0;
        } else if (arg_is(arg, L"-details")) {
            options->view_mode = VIEWMODE_DETAILS;
        } else if (arg_is(arg, L"-thumbnails")) {
            options->view_mode = VIEWMODE_LARGE_THUMBNAILS;
        } else if (arg_is(arg, L"-thumbnail-size")) {
            const wchar_t *value = option_value(argc, argv, &i);
            int size = value ? _wtoi(value) : 0;
            if (size > 0) {
                options->view_mode = size <= 48 ? VIEWMODE_MEDIUM_THUMBNAILS :
                                     size <= 128 ? VIEWMODE_LARGE_THUMBNAILS :
                                                   VIEWMODE_EXTRA_LARGE_THUMBNAILS;
            }
        } else if (arg_is(arg, L"-maximized")) {
            options->start_maximized = 1;
        } else if (arg_is(arg, L"-minimized")) {
            options->start_minimized = 1;
        } else if (arg_is(arg, L"-fullscreen")) {
            options->start_maximized = 1;
        } else if (arg_is(arg, L"-ontop")) {
            options->topmost = 1;
        } else if (arg_is(arg, L"-noontop")) {
            options->topmost = 0;
        } else if (arg_is(arg, L"-case")) {
            options->match_case = 1;
        } else if (arg_is(arg, L"-nocase")) {
            options->match_case = 0;
        } else if (arg_is(arg, L"-wholeword") || arg_is(arg, L"-ww")) {
            options->match_whole_word = 1;
        } else if (arg_is(arg, L"-nowholeword") || arg_is(arg, L"-noww")) {
            options->match_whole_word = 0;
        } else if (arg_is(arg, L"-matchpath")) {
            options->match_path = 1;
        } else if (arg_is(arg, L"-nomatchpath")) {
            options->match_path = 0;
        } else if (arg_is(arg, L"-regex")) {
            options->use_regex = 1;
        } else if (arg_is(arg, L"-noregex")) {
            options->use_regex = 0;
        } else if (arg_is(arg, L"-focus-results")) {
            options->focus_results = 1;
        } else if (arg_is(arg, L"-reindex")) {
            options->start_reindex = 1;
        } else if (arg_is(arg, L"-exit") || arg_is(arg, L"-quit") ||
                   arg_is(arg, L"-close")) {
            options->exit_after_start = 1;
        }
    }
}

static void apply_gui_options(APP_STATE *app, const GUI_OPTIONS *options)
{
    if (!app || !options)
        return;

    app->secondary_window = options->new_window;
    app->topmost = options->topmost;
    app->start_reindex = options->start_reindex;
    app->focus_results_on_start = options->focus_results;
    app->show_language_on_start = options->show_language;

    if (options->language[0]) {
        int index = lang_find(options->language);
        if (index >= 0) {
            lang_set(index);
            wcsncpy_s(app->language, 32, lang_current_id(), _TRUNCATE);
        }
    }
    if (options->view_mode >= 0)
        app->view_mode = options->view_mode;
    if (options->filter >= 0) {
        app->selected_filter = options->filter;
        app->query.filter_id = options->filter;
    }
    if (options->sort_column >= 0) {
        app->sort_column = options->sort_column;
        app->query.sort_column = options->sort_column;
    }
    if (options->sort_ascending >= 0) {
        app->sort_ascending = options->sort_ascending;
        app->query.sort_ascending = options->sort_ascending;
    }
    if (options->match_case >= 0) {
        app->match_case = options->match_case;
        app->query.match_case = options->match_case;
    }
    if (options->match_whole_word >= 0) {
        app->match_whole_word = options->match_whole_word;
        app->query.match_whole_word = options->match_whole_word;
    }
    if (options->match_path >= 0) {
        app->match_path = options->match_path;
        app->query.match_path = options->match_path;
    }
    if (options->use_regex >= 0) {
        app->use_regex = options->use_regex;
        app->query.use_regex = options->use_regex;
    }
    if (options->search[0])
        wcsncpy_s(app->initial_search, 512, options->search, _TRUNCATE);
    if (options->select[0])
        wcsncpy_s(app->initial_select, 260, options->select, _TRUNCATE);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPWSTR lpCmdLine, int nCmdShow)
{
    wchar_t **argv;
    int argc = 0;
    int result;
    GUI_OPTIONS options;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && oe_cli_should_run(argc, argv)) {
        result = oe_cli_run(argc, argv);
        LocalFree(argv);
        return result;
    }

    parse_gui_options(argc, argv, &options);
    if (argv)
        LocalFree(argv);

    /* This help is a pure Win32 window and needs neither elevation nor the
       search index. */
    if (options.show_help) {
        cmdhelp_show(NULL, L"Command Line Options", L"Close");
        return 0;
    }

    if (options.config_path[0])
        config_set_path_override(options.config_path);

    /* Forward a plain search request to the already running instance so the
       common "search for this now" path stays instant.  Option-bearing
       launches (for example -newwindow or -sort) must stay in this process. */
    if (!options.new_window && options.search[0] && !options.has_options) {
        if (ipc_send_command(options.search))
            return 0;
    }

    /* CLI and MCP stay unprivileged. The legacy GUI still elevates because it
       can rebuild NTFS indexes directly when the service is unavailable. */
    if (!elevation_disabled() && !process_is_elevated())
        return relaunch_gui_elevated(lpCmdLine, nCmdShow) ? 0 : 1;

    SetProcessDPIAware();

    /* Initialize COM */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    /* Initialize common controls */
    INITCOMMONCONTROLSEX icc = {
        sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES
    };
    InitCommonControlsEx(&icc);

    /* Built-in languages must exist before config_load picks one. */
    lang_init();

    /* Init application state */
    memset(&g_app, 0, sizeof(g_app));
    index_init(&g_app);
    config_load(&g_app);
    {
        int language = g_app.language[0] ? lang_find(g_app.language) : -1;
        if (language < 0)
            language = lang_system_default();
        lang_set(language);
        wcsncpy_s(g_app.language, 32, lang_current_id(), _TRUNCATE);
    }
    apply_gui_options(&g_app, &options);

    if (options.start_maximized)
        nCmdShow = SW_SHOWMAXIMIZED;
    else if (options.start_minimized)
        nCmdShow = SW_SHOWMINIMIZED;

    /* Register window class */
    if (!ui_init(hInstance)) {
        MessageBoxW(NULL, L"Failed to register window class.",
                    L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create main window */
    HWND hwnd = ui_create_main_window(hInstance, nCmdShow, &g_app);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.",
                    L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Start IPC server for cross-instance communication.  Secondary search
       windows do not own the single-instance pipe. */
    if (!g_app.secondary_window)
        ipc_start_server(&g_app);

    if (g_app.topmost)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (options.exit_after_start)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (ui_translate_accelerator(hwnd, &msg))
            continue;
        /* Handle dialog-style keyboard navigation */
        if (IsDialogMessageW(hwnd, &msg))
            continue;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* The process owns the large index lifetime; avoid a slow full free on exit. */
    CoUninitialize();

    return (int)msg.wParam;
}
