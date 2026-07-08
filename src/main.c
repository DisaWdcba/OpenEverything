#include "common.h"
#include "ui.h"
#include "config.h"
#include "ipc.h"
#include "index.h"

APP_STATE g_app;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPWSTR lpCmdLine, int nCmdShow)
{
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
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
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
