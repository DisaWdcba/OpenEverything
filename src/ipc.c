#include "ipc.h"

static HANDLE g_ipc_pipe = INVALID_HANDLE_VALUE;
static volatile LONG g_ipc_running = 0;

static DWORD WINAPI ipc_thread_proc(void *param)
{
    APP_STATE *app = (APP_STATE *)param;
    
    while (InterlockedCompareExchange(&g_ipc_running, 1, 1)) {
        g_ipc_pipe = CreateNamedPipeW(
            IPC_PIPE_NAME,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, NULL);
        
        if (g_ipc_pipe == INVALID_HANDLE_VALUE)
            break;
        
        if (ConnectNamedPipe(g_ipc_pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buffer[4096];
            DWORD read;
            if (ReadFile(g_ipc_pipe, buffer, sizeof(buffer) - 1, &read, NULL)) {
                buffer[read] = '\0';
                
                /* Convert to wide and set as search text */
                wchar_t wbuf[512];
                MultiByteToWideChar(CP_UTF8, 0, buffer, -1, wbuf, 512);
                
                wcscpy_s(app->query.text, 512, wbuf);
                PostMessageW(app->hwnd_main, WM_SEARCH_UPDATE, 0, 0);
            }
        }
        
        DisconnectNamedPipe(g_ipc_pipe);
        CloseHandle(g_ipc_pipe);
        g_ipc_pipe = INVALID_HANDLE_VALUE;
    }
    
    return 0;
}

int ipc_start_server(APP_STATE *app)
{
    InterlockedExchange(&g_ipc_running, 1);
    HANDLE h = CreateThread(NULL, 0, ipc_thread_proc, app, 0, NULL);
    if (h) {
        CloseHandle(h);
        return 1;
    }
    return 0;
}

void ipc_stop_server(APP_STATE *app)
{
    InterlockedExchange(&g_ipc_running, 0);
    if (g_ipc_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_ipc_pipe, NULL);
        CloseHandle(g_ipc_pipe);
        g_ipc_pipe = INVALID_HANDLE_VALUE;
    }
}

int ipc_send_command(const wchar_t *command)
{
    HANDLE hPipe = CreateFileW(
        IPC_PIPE_NAME, GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    
    if (hPipe == INVALID_HANDLE_VALUE)
        return 0;
    
    char utf8[4096];
    WideCharToMultiByte(CP_UTF8, 0, command, -1, utf8, 4096, NULL, NULL);
    
    DWORD written;
    WriteFile(hPipe, utf8, (DWORD)strlen(utf8), &written, NULL);
    
    CloseHandle(hPipe);
    return 1;
}
