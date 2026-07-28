#include "ipc.h"

#define IPC_BUFFER_BYTES     4096
#define IPC_MAX_COMMAND_CHARS 511
#define IPC_STOP_WAIT_MS     2000

static HANDLE g_ipc_stop_event = NULL;
static HANDLE g_ipc_thread = NULL;
static volatile LONG g_ipc_running = 0;

/* Decode the received payload. Length-bounded rather than NUL-bounded: the
   sender writes raw bytes, so a truncated or non-UTF-8 payload must not be
   able to run the decoder off the end of the buffer. */
static wchar_t *ipc_decode_command(const char *utf8, DWORD bytes)
{
    wchar_t *text;
    int chars;

    if (!utf8 || bytes == 0 || bytes > IPC_BUFFER_BYTES)
        return NULL;

    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                utf8, (int)bytes, NULL, 0);
    if (chars <= 0 || chars > IPC_MAX_COMMAND_CHARS)
        return NULL;

    text = (wchar_t *)malloc(((size_t)chars + 1) * sizeof(wchar_t));
    if (!text)
        return NULL;

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8, (int)bytes, text, chars) != chars) {
        free(text);
        return NULL;
    }
    text[chars] = L'\0';
    return text;
}

static void ipc_serve_client(APP_STATE *app, HANDLE pipe, HANDLE stop_event)
{
    char buffer[IPC_BUFFER_BYTES];
    OVERLAPPED ov;
    HANDLE wait[2];
    DWORD read = 0;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent)
        return;

    if (!ReadFile(pipe, buffer, sizeof(buffer), NULL, &ov) &&
        GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return;
    }

    wait[0] = ov.hEvent;
    wait[1] = stop_event;
    if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        CloseHandle(ov.hEvent);
        return;
    }

    if (GetOverlappedResult(pipe, &ov, &read, FALSE) && read > 0) {
        wchar_t *command = ipc_decode_command(buffer, read);
        if (command) {
            /* The UI thread takes ownership and frees the string. */
            if (!PostMessageW(app->hwnd_main, WM_SEARCH_UPDATE, 0,
                              (LPARAM)command))
                free(command);
        }
    }

    CloseHandle(ov.hEvent);
}

static DWORD WINAPI ipc_thread_proc(void *param)
{
    APP_STATE *app = (APP_STATE *)param;

    while (InterlockedCompareExchange(&g_ipc_running, 1, 1)) {
        HANDLE pipe;
        OVERLAPPED ov;
        BOOL connected = FALSE;

        pipe = CreateNamedPipeW(
            IPC_PIPE_NAME,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, IPC_BUFFER_BYTES, IPC_BUFFER_BYTES, 0, NULL);

        if (pipe == INVALID_HANDLE_VALUE)
            break;

        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) {
            CloseHandle(pipe);
            break;
        }

        if (ConnectNamedPipe(pipe, &ov)) {
            connected = TRUE;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE wait[2];
                wait[0] = ov.hEvent;
                wait[1] = g_ipc_stop_event;
                connected = WaitForMultipleObjects(2, wait, FALSE, INFINITE)
                            == WAIT_OBJECT_0;
                if (!connected)
                    CancelIoEx(pipe, &ov);
            }
        }

        if (connected)
            ipc_serve_client(app, pipe, g_ipc_stop_event);

        CloseHandle(ov.hEvent);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    return 0;
}

int ipc_start_server(APP_STATE *app)
{
    if (g_ipc_thread)
        return 1;

    g_ipc_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ipc_stop_event)
        return 0;

    InterlockedExchange(&g_ipc_running, 1);
    g_ipc_thread = CreateThread(NULL, 0, ipc_thread_proc, app, 0, NULL);
    if (!g_ipc_thread) {
        InterlockedExchange(&g_ipc_running, 0);
        CloseHandle(g_ipc_stop_event);
        g_ipc_stop_event = NULL;
        return 0;
    }
    return 1;
}

void ipc_stop_server(APP_STATE *app)
{
    (void)app;

    InterlockedExchange(&g_ipc_running, 0);
    if (g_ipc_stop_event)
        SetEvent(g_ipc_stop_event);

    if (g_ipc_thread) {
        WaitForSingleObject(g_ipc_thread, IPC_STOP_WAIT_MS);
        CloseHandle(g_ipc_thread);
        g_ipc_thread = NULL;
    }
    if (g_ipc_stop_event) {
        CloseHandle(g_ipc_stop_event);
        g_ipc_stop_event = NULL;
    }
}

int ipc_send_command(const wchar_t *command)
{
    HANDLE pipe;
    char *utf8;
    int bytes;
    DWORD written = 0;
    BOOL ok;

    if (!command || !command[0] || wcslen(command) > IPC_MAX_COMMAND_CHARS)
        return 0;

    bytes = WideCharToMultiByte(CP_UTF8, 0, command, -1, NULL, 0, NULL, NULL);
    if (bytes <= 1)
        return 0;

    utf8 = (char *)malloc((size_t)bytes);
    if (!utf8)
        return 0;

    if (WideCharToMultiByte(CP_UTF8, 0, command, -1, utf8, bytes,
                            NULL, NULL) != bytes) {
        free(utf8);
        return 0;
    }

    pipe = CreateFileW(IPC_PIPE_NAME, GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        free(utf8);
        return 0;
    }

    /* Send without the terminator; the receiver bounds the decode by length. */
    ok = WriteFile(pipe, utf8, (DWORD)(bytes - 1), &written, NULL);
    CloseHandle(pipe);
    free(utf8);

    return ok && written == (DWORD)(bytes - 1);
}
