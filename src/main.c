#include "common.h"
#include "cli.h"
#include "ui.h"
#include "config.h"
#include "ipc.h"
#include "index.h"

APP_STATE g_app;

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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPWSTR lpCmdLine, int nCmdShow)
{
    wchar_t **argv;
    int argc = 0;
    int result;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && oe_cli_should_run(argc, argv)) {
        result = oe_cli_run(argc, argv);
        LocalFree(argv);
        return result;
    }
    if (argv)
        LocalFree(argv);

    /* CLI and MCP stay unprivileged. The legacy GUI still elevates because it
       can rebuild NTFS indexes directly when the service is unavailable. */
    if (!process_is_elevated())
        return relaunch_gui_elevated(lpCmdLine, nCmdShow) ? 0 : 1;

    SetProcessDPIAware();
    
    /* Check for command-line arguments to forward to existing instance */
    if (lpCmdLine && lpCmdLine[0]) {
        if (ipc_send_command(lpCmdLine)) {
            return 0; /* Forwarded to existing instance */
        }
    }
    
    /* Initialize COM */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    /* Initialize common controls */
    INITCOMMONCONTROLSEX icc = {
        sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES
    };
    InitCommonControlsEx(&icc);
    
    /* Init application state */
    memset(&g_app, 0, sizeof(g_app));
    index_init(&g_app);
    config_load(&g_app);
    
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
    
    /* Start IPC server for cross-instance communication */
    ipc_start_server(&g_app);
    
    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
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
