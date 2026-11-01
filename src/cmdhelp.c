/* cmdhelp.c -- Help > Command Line Options.
 *
 * Self-contained on purpose: this translation unit depends only on
 * cmdhelp.h (which pulls in common.h), windows.h and the C runtime.  It does
 * not touch lang.c or any other project source, so the caller passes the
 * localised window title and close-button label in.
 *
 * cmdhelp_text() returns one merged document: the Everything-compatible
 * OpenEverything.exe option set followed by the OpenEverything CLI/MCP
 * commands that already shipped.
 *
 * cmdhelp_show() displays that document in a modal, scrollable, read-only
 * window with a Close button.  The window is DPI scaled from a 96-DPI design
 * and is resizable down to 480x320 logical pixels.
 */

#include "cmdhelp.h"

#define IDC_CMDHELP_EDIT        1001
#define CMDHELP_TEXT_CAPACITY   32768
#define CMDHELP_BASE_DPI        96

#define CMDHELP_BASE_WIDTH      1040
#define CMDHELP_BASE_HEIGHT     680
#define CMDHELP_MIN_WIDTH       480
#define CMDHELP_MIN_HEIGHT      320

typedef struct {
    const wchar_t *option;
    const wchar_t *description;
} CMDHELP_OPTION;

/* Everything-compatible OpenEverything.exe options.  The order is the one
   requested for OpenEverything V2.3.4; descriptions are kept verbatim. */
static const CMDHELP_OPTION g_cmdhelp_options[] = {
    { L"-?", L"Show this help." },
    { L"-admin", L"Run \"Everything\" as Administrator." },
    { L"-app-data", L"Store data in application data." },
    { L"-bookmark <name>", L"Open a bookmark." },
    { L"-case", L"Enable case matching." },
    { L"-choose-language", L"Show the language selection page." },
    { L"-choose-volumes", L"Do not automatically index volumes." },
    { L"-close", L"Close the current search window." },
    { L"-config <filename>", L"The filename of the ini file." },
    { L"-console", L"Show the debugging console." },
    { L"-copyto <filename1> <...>", L"Show the multi-file renamer with the specified filenames." },
    { L"-create-file-list <filename> <path>", L"Create a file list of a path." },
    { L"-create-file-list-exclude-files <list>", L"exclude the semicolon delimited wildcard filter for files." },
    { L"-create-file-list-exclude-folders <list>", L"exclude the semicolon delimited wildcard filter for folders." },
    { L"-create-file-list-include-only-files <list>", L"include only the semicolon delimited wildcard filter for files." },
    { L"-create-usn-journal <volume> <max-size-bytes> <allocation-delta-bytes>", L"" },
    { L"-db <filename>", L"The filename of the database." },
    { L"-debug", L"Show the debugging console." },
    { L"-debug-log", L"Log debugging information to disk" },
    { L"-delete-usn-journal <volume>", L"Delete a USN Journal." },
    { L"-details", L"Show results in detail view." },
    { L"-diacritics", L"Enable diacritics matching." },
    { L"-disable-run-as-admin", L"Disable run as administrator." },
    { L"-disable-update-notification", L"Disable update notification on startup." },
    { L"-drive-links", L"Use C: links for ETP connections." },
    { L"-edit <filename>", L"Open a file list with the file list editor." },
    { L"-enable-run-as-admin", L"Enable run as administrator." },
    { L"-enable-update-notification", L"Enable update notification on startup." },
    { L"-exit", L"Exit \"Everything\"." },
    { L"-first-instance", L"Only run if this is the first instance of \"Everything\"." },
    { L"-filelist <filename>", L"Open a file list." },
    { L"-filename <filename>", L"Search for a file or folder by filename." },
    { L"-filter <name>", L"Select a search filter." },
    { L"-focus-bottom-result", L"Focus the bottom result." },
    { L"-focus-last-run-result", L"Focus the last run result." },
    { L"-focus-most-run-result", L"Focus the most run result." },
    { L"-focus-results", L"Focus the result list." },
    { L"-focus-top-result", L"Focus the top result." },
    { L"-fullscreen", L"Show the search window fullscreen." },
    { L"-h", L"Show this help." },
    { L"-help", L"Show this help." },
    { L"-home", L"Open the home search." },
    { L"-install <location>", L"Install \"Everything\" to a new location." },
    { L"-install-client-service", L"Install the \"Everything\" client as a service." },
    { L"-install-config <filename>", L"Install the specified ini file." },
    { L"-install-desktop-shortcut", L"Install desktop shortcut." },
    { L"-install-folder-context-menu", L"Install folder context menus." },
    { L"-install-quick-launch-shortcut", L"Install Quick Launch shortcut." },
    { L"-install-run-on-system-startup", L"Install \"Everything\" from the system startup." },
    { L"-install-service", L"Install and start the \"OpenEverything\" service." },
    { L"-install-service-pipe-name <name>", L"Use the specified name for the \"OpenEverything\" service pipe name." },
    { L"-install-service-security-descriptor", L"Specify the pipe security descriptor." },
    { L"-install-start-menu-shortcuts", L"Install \"Everything\" shortcuts from the Start menu." },
    { L"-install-url-protocol", L"Install URL Protocol." },
    { L"-instance <name>", L"The name of the \"Everything\" instance." },
    { L"-l", L"Load the local database." },
    { L"-language <langID>", L"Set the language to the specified language ID." },
    { L"-load-delay <milliseconds>", L"The delay in milliseconds before loading the database." },
    { L"-local", L"Load the local database." },
    { L"-matchpath", L"Enable full path matching." },
    { L"-maximized", L"Maximize the search window." },
    { L"-minimized", L"Minimize the search window." },
    { L"-moveto <filename1> <...>", L"Show the multi-file renamer with the specified filenames." },
    { L"-name-part <filename>", L"Search for the name part of a filename." },
    { L"-newwindow", L"Create a new search window." },
    { L"-noapp-data", L"Store data in executable location." },
    { L"-nocase", L"Disable case matching." },
    { L"-nodb", L"Do not save to or load from the \"Everything\" database file." },
    { L"-nodiacritics", L"Disable diacritics matching." },
    { L"-nofullscreen", L"Show the search window in a window." },
    { L"-nomatchpath", L"Disable full path matching." },
    { L"-nomaximized", L"Unmaximize the search window." },
    { L"-nominimized", L"Unminimize the search window." },
    { L"-nonewwindow", L"Show an existing search window." },
    { L"-noontop", L"Disable always ontop." },
    { L"-noregex", L"Disable Regex." },
    { L"-noverbose", L"Display only basic debug messages." },
    { L"-nowholeword", L"Disable match whole word." },
    { L"-noww", L"Disable match whole word." },
    { L"-ontop", L"Enable always ontop." },
    { L"-p <path>", L"Search for a path." },
    { L"-parent <path>", L"Search for files and folders in the specified folder." },
    { L"-parentpath <path>", L"Search for the parent of a path." },
    { L"-path <path>", L"Search for a path." },
    { L"-quit", L"Exit \"Everything\"." },
    { L"-read-only", L"Loads the database in read-only mode." },
    { L"-regex", L"Enable Regex." },
    { L"-reindex", L"Force database rebuild." },
    { L"-rename <filename1> <...>", L"Show the multi-file renamer with the specified filenames." },
    { L"-s <text>", L"Set the search." },
    { L"-search <text>", L"Set the search." },
    { L"-search-file-list <filename>", L"Search the specified text file for a list of file names." },
    { L"-select <filename>", L"Focus and select the specified result." },
    { L"-server-share-links", L"Use \\\\Server\\C: links for ETP connections." },
    { L"-service-pipe-name <name>", L"connect to the service pipe with the specified name." },
    { L"-sort <name>", L"Set the sort to the specified name." },
    { L"-sort-ascending", L"Sort ascending." },
    { L"-sort-descending", L"Sort descending." },
    { L"-start-client-service", L"Start the \"Everything\" client service." },
    { L"-start-service", L"Start the \"Everything\" service." },
    { L"-startup", L"Run \"Everything\" in the background." },
    { L"-stop-client-service", L"Stop the \"Everything\" client service." },
    { L"-stop-service", L"Stop the \"Everything\" service." },
    { L"-svc", L"Run \"Everything\" as a service." },
    { L"-svc-pipe-name <name>", L"Host the pipe server with the specified name." },
    { L"-svc-security-descriptor <sd>", L"Host the pipe server with the security descriptor." },
    { L"-thumbnail-size <size>", L"Specify the size of thumbnails in pixels." },
    { L"-thumbnails", L"Show results in thumbnail view." },
    { L"-toggle-window", L"Hides the current foreground search window or shows the search window." },
    { L"-uninstall [path]", L"Uninstall \"Everything\" from the specified path." },
    { L"-uninstall-client-service", L"Uninstall the \"Everything\" client service." },
    { L"-uninstall-desktop-shortcut", L"Uninstall desktop shortcut." },
    { L"-uninstall-efu-association", L"Uninstall EFU file association." },
    { L"-uninstall-folder-context-menu", L"Uninstall folder context menus." },
    { L"-uninstall-quick-launch-shortcut", L"Uninstall Quick Launch shortcut." },
    { L"-uninstall-run-on-system-startup", L"Remove \"Everything\" from the system startup." },
    { L"-uninstall-service", L"Uninstall the \"Everything\" service." },
    { L"-uninstall-start-menu-shortcuts", L"Uninstall \"Everything\" shortcuts from the Start menu." },
    { L"-uninstall-url-protocol", L"Uninstall URL Protocol." },
    { L"-uninstall-user", L"Uninstall user files." },
    { L"-update", L"Save the database to disk." },
    { L"-verbose", L"Display all debug messages." },
    { L"-wholeword", L"Enable match whole word." },
    { L"-ww", L"Enable match whole word." }
};

#define CMDHELP_OPTION_COUNT \
    (sizeof(g_cmdhelp_options) / sizeof(g_cmdhelp_options[0]))

/* =============================================================
 * Merged help text
 * ============================================================= */

static void cmdhelp_append(wchar_t *dst, size_t capacity, size_t *used,
                           const wchar_t *src)
{
    size_t room;
    size_t count;

    if (dst == NULL || used == NULL || src == NULL || capacity == 0)
        return;
    if (*used >= capacity)
        return;

    room = capacity - 1 - *used;
    count = wcslen(src);
    if (count > room)
        count = room;

    wmemcpy(dst + *used, src, count);
    *used += count;
    dst[*used] = L'\0';
}

static void cmdhelp_fill(wchar_t *dst, size_t capacity, size_t *used,
                         wchar_t ch, size_t repeat)
{
    size_t room;
    size_t i;

    if (dst == NULL || used == NULL || capacity == 0 || repeat == 0)
        return;
    if (*used >= capacity)
        return;

    room = capacity - 1 - *used;
    if (repeat > room)
        repeat = room;

    for (i = 0; i < repeat; i++)
        dst[*used + i] = ch;
    *used += repeat;
    dst[*used] = L'\0';
}

static void cmdhelp_append_n(wchar_t *dst, size_t capacity, size_t *used,
                             const wchar_t *src, size_t count)
{
    size_t room;

    if (dst == NULL || used == NULL || src == NULL || capacity == 0)
        return;
    if (*used >= capacity)
        return;

    room = capacity - 1 - *used;
    if (count > room)
        count = room;
    wmemcpy(dst + *used, src, count);
    *used += count;
    dst[*used] = L'\0';
}

/* Append a description, wrapping on spaces so a long explanation never runs
   past the right edge of the read-only edit control.  Continuation lines are
   indented to the description column. */
static void cmdhelp_append_description(wchar_t *dst, size_t capacity,
                                       size_t *used, const wchar_t *text,
                                       size_t column)
{
    const size_t max_width = 54;
    const wchar_t *cursor = text;
    size_t remaining;
    int first = 1;

    if (text == NULL)
        return;
    remaining = wcslen(text);
    while (remaining > 0) {
        size_t take = remaining < max_width ? remaining : max_width;

        if (take < remaining) {
            size_t cut = take;
            while (cut > 0 && cursor[cut] != L' ')
                cut--;
            if (cut > 0)
                take = cut;
        }
        if (!first) {
            cmdhelp_append(dst, capacity, used, L"\r\n");
            cmdhelp_fill(dst, capacity, used, L' ', column);
        }
        cmdhelp_append_n(dst, capacity, used, cursor, take);
        while (take < remaining && cursor[take] == L' ')
            take++;
        cursor += take;
        remaining -= take;
        first = 0;
    }
}

const wchar_t *cmdhelp_text(void)
{
    static wchar_t text[CMDHELP_TEXT_CAPACITY];
    static int built = 0;
    const size_t capacity = CMDHELP_TEXT_CAPACITY;
    size_t used = 0;
    size_t column = 0;
    size_t i;
    const CMDHELP_OPTION *entry;
    size_t option_len;

    if (built)
        return text;

    /* Align descriptions just past the longest option that has one. */
    for (i = 0; i < CMDHELP_OPTION_COUNT; i++) {
        if (g_cmdhelp_options[i].description[0] != L'\0') {
            option_len = wcslen(g_cmdhelp_options[i].option);
            if (option_len > column)
                column = option_len;
        }
    }
    column += 2;

    text[0] = L'\0';

    cmdhelp_append(text, capacity, &used,
                   L"OpenEverything.exe [filename] [-options]\r\n");
    cmdhelp_append(text, capacity, &used, L"\r\n");
    cmdhelp_append(text, capacity, &used,
                   L"filename    Open the specified file list.\r\n");
    cmdhelp_append(text, capacity, &used, L"\r\n");

    for (i = 0; i < CMDHELP_OPTION_COUNT; i++) {
        entry = &g_cmdhelp_options[i];
        option_len = wcslen(entry->option);

        cmdhelp_append(text, capacity, &used, entry->option);
        if (entry->description[0] != L'\0') {
            if (option_len < column)
                cmdhelp_fill(text, capacity, &used, L' ', column - option_len);
            cmdhelp_append_description(text, capacity, &used,
                                       entry->description, column);
        }
        cmdhelp_append(text, capacity, &used, L"\r\n");
    }

    cmdhelp_append(text, capacity, &used, L"\r\n");
    cmdhelp_append(text, capacity, &used, L"OpenEverything CLI and MCP\r\n");
    cmdhelp_append(text, capacity, &used, L"\r\n");
    cmdhelp_append(text, capacity, &used,
        L"OpenEverythingCLI.exe search [QUERY] [--limit N] [--json]\r\n"
        L"    [--match-path] [--case-sensitive] [--whole-word] [--folder PATH]\r\n"
        L"    [--no-subfolders] [--filter TYPE] [--sort FIELD] [--descending]\r\n"
        L"    [--index FILE]\r\n");
    cmdhelp_append(text, capacity, &used,
        L"OpenEverythingCLI.exe stats [--json] [--index FILE]\r\n");
    cmdhelp_append(text, capacity, &used,
        L"OpenEverythingCLI.exe update [--timeout SECONDS] [--json]\r\n");
    cmdhelp_append(text, capacity, &used,
        L"OpenEverythingCLI.exe mcp [--index FILE]\r\n");
    cmdhelp_append(text, capacity, &used,
        L"OpenEverything.exe --mcp [--index FILE]\r\n");
    cmdhelp_append(text, capacity, &used, L"\r\n");
    cmdhelp_append(text, capacity, &used,
        L"Filter values: everything, audio, compressed, document, executable,\r\n"
        L"    folder, image, video.\r\n");
    cmdhelp_append(text, capacity, &used,
        L"Sort values: name, path, size, modified, created, attributes.\r\n");
    cmdhelp_append(text, capacity, &used,
        L"--limit defaults to 50; --index reads a specific index.dat;\r\n"
        L"    MCP uses stdin/stdout JSON-RPC.\r\n");
    cmdhelp_append(text, capacity, &used, L"\r\n");
    cmdhelp_append(text, capacity, &used,
        L"Note: options for the Windows service and setup are provided by\r\n"
        L"    OpenEverythingSetup.exe and OpenEverythingService.exe.\r\n");

    built = 1;
    return text;
}

/* =============================================================
 * Modal help window
 * ============================================================= */

static const wchar_t CMDHELP_CLASS_NAME[] = L"OECmdHelpWindow";

static HWND g_cmdhelp_hwnd = NULL;
static HWND g_cmdhelp_edit = NULL;
static HWND g_cmdhelp_button = NULL;
static HFONT g_cmdhelp_edit_font = NULL;
static HFONT g_cmdhelp_button_font = NULL;
static int g_cmdhelp_dpi = CMDHELP_BASE_DPI;
static int g_cmdhelp_class_registered = 0;
static const wchar_t *g_cmdhelp_close_label = L"Close";

static void cmdhelp_release_fonts(void)
{
    HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (g_cmdhelp_edit_font != NULL && g_cmdhelp_edit_font != stock)
        DeleteObject(g_cmdhelp_edit_font);
    g_cmdhelp_edit_font = NULL;

    if (g_cmdhelp_button_font != NULL && g_cmdhelp_button_font != stock)
        DeleteObject(g_cmdhelp_button_font);
    g_cmdhelp_button_font = NULL;
}

static void cmdhelp_layout(HWND hwnd)
{
    RECT rc;
    int pad;
    int gap;
    int button_w;
    int button_h;
    int client_w;
    int client_h;
    int button_x;
    int button_y;
    int edit_w;
    int edit_h;

    if (hwnd == NULL || !GetClientRect(hwnd, &rc))
        return;

    client_w = rc.right - rc.left;
    client_h = rc.bottom - rc.top;
    if (client_w < 0)
        client_w = 0;
    if (client_h < 0)
        client_h = 0;

    pad = MulDiv(10, g_cmdhelp_dpi, CMDHELP_BASE_DPI);
    gap = MulDiv(8, g_cmdhelp_dpi, CMDHELP_BASE_DPI);
    button_w = MulDiv(90, g_cmdhelp_dpi, CMDHELP_BASE_DPI);
    button_h = MulDiv(26, g_cmdhelp_dpi, CMDHELP_BASE_DPI);

    button_x = client_w - pad - button_w;
    button_y = client_h - pad - button_h;
    if (button_x < pad)
        button_x = pad;
    if (button_y < pad)
        button_y = pad;

    edit_w = client_w - pad * 2;
    edit_h = button_y - gap - pad;
    if (edit_w < 0)
        edit_w = 0;
    if (edit_h < 0)
        edit_h = 0;

    if (IsWindow(g_cmdhelp_edit))
        MoveWindow(g_cmdhelp_edit, pad, pad, edit_w, edit_h, TRUE);
    if (IsWindow(g_cmdhelp_button))
        MoveWindow(g_cmdhelp_button, button_x, button_y, button_w, button_h, TRUE);
}

static LRESULT CALLBACK cmdhelp_wndproc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE instance = GetModuleHandleW(NULL);

        g_cmdhelp_hwnd = hwnd;

        g_cmdhelp_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_TABSTOP,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CMDHELP_EDIT,
            instance, NULL);
        g_cmdhelp_button = CreateWindowExW(
            0, L"BUTTON", g_cmdhelp_close_label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDOK,
            instance, NULL);

        if (g_cmdhelp_edit == NULL || g_cmdhelp_button == NULL)
            return -1;

        SendMessageW(g_cmdhelp_edit, WM_SETFONT,
                     (WPARAM)g_cmdhelp_edit_font, TRUE);
        SendMessageW(g_cmdhelp_button, WM_SETFONT,
                     (WPARAM)g_cmdhelp_button_font, TRUE);
        SetWindowTextW(g_cmdhelp_edit, cmdhelp_text());
        cmdhelp_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        cmdhelp_layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;

        info->ptMinTrackSize.x = MulDiv(CMDHELP_MIN_WIDTH, g_cmdhelp_dpi, CMDHELP_BASE_DPI);
        info->ptMinTrackSize.y = MulDiv(CMDHELP_MIN_HEIGHT, g_cmdhelp_dpi, CMDHELP_BASE_DPI);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wparam == (WPARAM)VK_ESCAPE || wparam == (WPARAM)VK_RETURN) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (IsWindow(g_cmdhelp_edit))
            SetFocus(g_cmdhelp_edit);
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lparam == g_cmdhelp_edit) {
            HDC hdc = (HDC)wparam;

            SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)(INT_PTR)GetSysColorBrush(COLOR_WINDOW);
        }
        break;

    case WM_DESTROY:
        cmdhelp_release_fonts();
        /* Do not PostQuitMessage here: the modal loop below exits on
           IsWindow() becoming false. */
        g_cmdhelp_hwnd = NULL;
        g_cmdhelp_edit = NULL;
        g_cmdhelp_button = NULL;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void cmdhelp_register_class(HINSTANCE instance)
{
    WNDCLASSW wc;

    if (g_cmdhelp_class_registered)
        return;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = cmdhelp_wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (wc.hIcon == NULL)
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = CMDHELP_CLASS_NAME;

    if (RegisterClassW(&wc) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        g_cmdhelp_class_registered = 1;
}

void cmdhelp_show(HWND owner, const wchar_t *title, const wchar_t *close_label)
{
    HINSTANCE instance;
    HDC screen;
    HMONITOR monitor;
    MONITORINFO monitor_info;
    RECT work;
    RECT owner_rect;
    LONG width;
    LONG height;
    LONG x;
    LONG y;
    HICON icon;
    HWND hwnd;
    MSG msg;
    int got_message;
    int dpi;
    int sampled;

    if (g_cmdhelp_hwnd != NULL && IsWindow(g_cmdhelp_hwnd)) {
        SetForegroundWindow(g_cmdhelp_hwnd);
        return;
    }

    instance = GetModuleHandleW(NULL);

    dpi = CMDHELP_BASE_DPI;
    screen = GetDC(NULL);
    if (screen != NULL) {
        sampled = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
        if (sampled >= CMDHELP_BASE_DPI && sampled <= 960)
            dpi = sampled;
    }
    g_cmdhelp_dpi = dpi;

    cmdhelp_register_class(instance);
    if (!g_cmdhelp_class_registered)
        return;

    cmdhelp_release_fonts();
    g_cmdhelp_edit_font = CreateFontW(
        -MulDiv(9, g_cmdhelp_dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    if (g_cmdhelp_edit_font == NULL)
        g_cmdhelp_edit_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_cmdhelp_button_font = CreateFontW(
        -MulDiv(9, g_cmdhelp_dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (g_cmdhelp_button_font == NULL)
        g_cmdhelp_button_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_cmdhelp_close_label =
        (close_label != NULL && close_label[0] != L'\0') ? close_label : L"Close";

    /* Center on the owner when there is one, otherwise on the primary work
       area; clamp the result so the window is never off screen. */
    monitor = MonitorFromWindow(
        (owner != NULL) ? owner : GetDesktopWindow(),
        MONITOR_DEFAULTTOPRIMARY);
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = (DWORD)sizeof(monitor_info);
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        work = monitor_info.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    width = (LONG)MulDiv(CMDHELP_BASE_WIDTH, dpi, CMDHELP_BASE_DPI);
    height = (LONG)MulDiv(CMDHELP_BASE_HEIGHT, dpi, CMDHELP_BASE_DPI);
    if (width > work.right - work.left)
        width = work.right - work.left;
    if (height > work.bottom - work.top)
        height = work.bottom - work.top;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    x = work.left + ((work.right - work.left) - width) / 2;
    y = work.top + ((work.bottom - work.top) - height) / 2;
    if (owner != NULL && GetWindowRect(owner, &owner_rect)) {
        x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    }
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;
    if (x + width > work.right)
        x = work.right - width;
    if (y + height > work.bottom)
        y = work.bottom - height;

    g_cmdhelp_hwnd = NULL;
    g_cmdhelp_edit = NULL;
    g_cmdhelp_button = NULL;

    hwnd = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        CMDHELP_CLASS_NAME,
        (title != NULL && title[0] != L'\0') ? title : L"Command Line Options",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height,
        owner, NULL, instance, NULL);
    if (hwnd == NULL) {
        g_cmdhelp_hwnd = NULL;
        cmdhelp_release_fonts();
        return;
    }

    icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (icon == NULL)
        icon = LoadIconW(NULL, IDI_APPLICATION);
    if (icon != NULL) {
        SendMessageW(hwnd, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)icon);
        SendMessageW(hwnd, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)icon);
    }

    cmdhelp_layout(hwnd);

    if (owner != NULL && IsWindow(owner))
        EnableWindow(owner, FALSE);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (IsWindow(g_cmdhelp_edit))
        SetFocus(g_cmdhelp_edit);

    /* Local modal loop: IsDialogMessageW gives the edit/button tab order;
       escape and enter close the window from any focused child. */
    while (IsWindow(hwnd)) {
        got_message = GetMessageW(&msg, NULL, 0, 0);
        if (got_message == 0) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (got_message == -1)
            break;

        if (msg.message == WM_KEYDOWN &&
            (msg.wParam == (WPARAM)VK_ESCAPE ||
             msg.wParam == (WPARAM)VK_RETURN)) {
            DestroyWindow(hwnd);
            continue;
        }

        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (IsWindow(hwnd))
        DestroyWindow(hwnd);

    if (owner != NULL && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}
