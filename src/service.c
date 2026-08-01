#include "cache.h"
#include "index.h"
#include "ntfs.h"
#include "service_client.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#define SERVICE_SYNC_MAX_CHANGES 16384
#define SERVICE_SYNC_INTERVAL_MS 1000
#define SERVICE_SAVE_QUIET_MS 3000
#define SERVICE_SAVE_MAX_DELAY_MS 30000
#define SERVICE_PIPE_BUFFER 4096
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

APP_STATE g_app;

static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS g_service_status;
static HANDLE g_stop_event;
static HANDLE g_refresh_event;
static HANDLE g_pipe_thread;
static volatile LONG g_runtime_state = OE_SERVICE_STATE_STARTING;
static volatile LONG g_last_error;
static volatile LONG g_update_sequence;
static volatile LONG64 g_last_update_unix;

static long long service_unix_now(void)
{
    FILETIME filetime;
    ULARGE_INTEGER value;

    GetSystemTimeAsFileTime(&filetime);
    value.LowPart = filetime.dwLowDateTime;
    value.HighPart = filetime.dwHighDateTime;
    return (long long)((value.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static void service_set_runtime_state(OE_SERVICE_STATE state, DWORD error)
{
    InterlockedExchange(&g_runtime_state, (LONG)state);
    InterlockedExchange(&g_last_error, (LONG)error);
}

static void service_report_status(DWORD state, DWORD exit_code, DWORD wait_hint)
{
    static DWORD checkpoint = 1;

    if (!g_status_handle)
        return;
    memset(&g_service_status, 0, sizeof(g_service_status));
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = exit_code;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
            : 0;
    g_service_status.dwCheckPoint =
        state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING
            ? checkpoint++ : 0;
    SetServiceStatus(g_status_handle, &g_service_status);
}

static void service_fill_response(OE_SERVICE_RESPONSE *response, DWORD status)
{
    memset(response, 0, sizeof(*response));
    response->magic = OE_SERVICE_PROTOCOL_MAGIC;
    response->version = OE_SERVICE_PROTOCOL_VERSION;
    response->status = status;
    response->state = (uint32_t)InterlockedCompareExchange(
        &g_runtime_state, 0, 0);
    response->process_id = GetCurrentProcessId();
    response->last_error = (uint32_t)InterlockedCompareExchange(
        &g_last_error, 0, 0);
    response->update_sequence = (uint32_t)InterlockedCompareExchange(
        &g_update_sequence, 0, 0);
    response->last_update_unix = InterlockedCompareExchange64(
        &g_last_update_unix, 0, 0);

    EnterCriticalSection(&g_app.index_lock);
    response->entry_count = g_app.entry_count;
    response->volume_count = g_app.volume_count;
    response->indexed_volume_count = g_app.indexed_volume_count;
    response->index_error_count = g_app.index_error_count;
    LeaveCriticalSection(&g_app.index_lock);
}

static int service_read_request(HANDLE pipe, OE_SERVICE_REQUEST *request)
{
    OVERLAPPED overlapped;
    HANDLE waits[2];
    DWORD read = 0;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent)
        return 0;
    if (!ReadFile(pipe, request, sizeof(*request), NULL, &overlapped) &&
        GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    waits[0] = overlapped.hEvent;
    waits[1] = g_stop_event;
    if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) ||
        read != sizeof(*request)) {
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    CloseHandle(overlapped.hEvent);
    return 1;
}

static int service_write_response(HANDLE pipe, const OE_SERVICE_RESPONSE *response)
{
    OVERLAPPED overlapped;
    DWORD written = 0;
    DWORD wait_result;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent)
        return 0;
    if (!WriteFile(pipe, response, sizeof(*response), NULL, &overlapped) &&
        GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    wait_result = WaitForSingleObject(overlapped.hEvent, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE) ||
        written != sizeof(*response)) {
        CloseHandle(overlapped.hEvent);
        return 0;
    }
    CloseHandle(overlapped.hEvent);
    return 1;
}

static void service_serve_pipe_client(HANDLE pipe)
{
    OE_SERVICE_REQUEST request;
    OE_SERVICE_RESPONSE response;
    DWORD status = ERROR_SUCCESS;

    memset(&request, 0, sizeof(request));
    if (!service_read_request(pipe, &request))
        return;
    if (request.magic != OE_SERVICE_PROTOCOL_MAGIC ||
        request.version != OE_SERVICE_PROTOCOL_VERSION) {
        status = ERROR_INVALID_DATA;
    } else {
        switch ((OE_SERVICE_COMMAND)request.command) {
        case OE_SERVICE_COMMAND_STATUS:
            break;
        case OE_SERVICE_COMMAND_REFRESH:
            SetEvent(g_refresh_event);
            break;
        default:
            status = ERROR_INVALID_FUNCTION;
            break;
        }
    }

    service_fill_response(&response, status);
    service_write_response(pipe, &response);
    FlushFileBuffers(pipe);
}

static DWORD WINAPI service_pipe_thread_proc(void *unused)
{
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES attributes;
    (void)unused;

    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
            SDDL_REVISION_1, &descriptor, NULL)) {
        attributes.lpSecurityDescriptor = descriptor;
    }

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        HANDLE pipe;
        OVERLAPPED overlapped;
        HANDLE waits[2];
        int connected = 0;

        pipe = CreateNamedPipeW(
            OE_SERVICE_PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            4, SERVICE_PIPE_BUFFER, SERVICE_PIPE_BUFFER, 0,
            attributes.lpSecurityDescriptor ? &attributes : NULL);
        if (pipe == INVALID_HANDLE_VALUE)
            break;

        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent) {
            CloseHandle(pipe);
            break;
        }
        if (ConnectNamedPipe(pipe, &overlapped)) {
            connected = 1;
        } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = 1;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            waits[0] = overlapped.hEvent;
            waits[1] = g_stop_event;
            connected = WaitForMultipleObjects(2, waits, FALSE, INFINITE)
                        == WAIT_OBJECT_0;
            if (!connected)
                CancelIoEx(pipe, &overlapped);
        }

        if (connected)
            service_serve_pipe_client(pipe);
        CloseHandle(overlapped.hEvent);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (descriptor)
        LocalFree(descriptor);
    return 0;
}

static int service_prepare_cache_directory(const wchar_t *index_path)
{
    wchar_t directory[MAX_PATH];
    wchar_t *last;
    int result;

    wcscpy_s(directory, MAX_PATH, index_path);
    last = wcsrchr(directory, L'\\');
    if (!last)
        return 0;
    *last = L'\0';
    result = SHCreateDirectoryExW(NULL, directory, NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS)
        return 0;
    return 1;
}

static int service_sync_once(APP_STATE *app, int *needs_rebuild,
                             int *changed_count)
{
    VOLUME_INFO current[26];
    int current_count;
    int local_volume_count;
    int total_applied = 0;

    *needs_rebuild = 0;
    *changed_count = 0;
    current_count = ntfs_enumerate_volumes(current, 26);

    EnterCriticalSection(&app->index_lock);
    local_volume_count = app->volume_count;
    if (current_count != local_volume_count) {
        LeaveCriticalSection(&app->index_lock);
        *needs_rebuild = 1;
        return 1;
    }
    for (int i = 0; i < local_volume_count; i++) {
        if (_wcsicmp(current[i].drive_letter,
                     app->volumes[i].drive_letter) != 0) {
            LeaveCriticalSection(&app->index_lock);
            *needs_rebuild = 1;
            return 1;
        }
    }
    LeaveCriticalSection(&app->index_lock);

    for (int i = 0; i < local_volume_count; i++) {
        wchar_t volume_path[64];
        long long saved_journal_id;
        long long saved_next_usn;
        HANDLE volume;
        USN_JOURNAL_DATA_BUF journal;

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
            return 0;
        EnterCriticalSection(&app->index_lock);
        wcscpy_s(volume_path, 64, app->volumes[i].volume_path);
        saved_journal_id = app->volumes[i].usn_journal_id;
        saved_next_usn = app->volumes[i].usn_next_usn;
        LeaveCriticalSection(&app->index_lock);

        volume = ntfs_open_volume(volume_path);
        if (!volume)
            continue;
        if (!ntfs_query_usn_journal(volume, &journal)) {
            ntfs_close_volume(volume);
            continue;
        }
        if (saved_journal_id == 0 ||
            saved_journal_id != journal.UsnJournalId ||
            saved_next_usn < journal.LowestValidUsn ||
            saved_next_usn > journal.NextUsn) {
            ntfs_close_volume(volume);
            *needs_rebuild = 1;
            return 1;
        }

        if (saved_next_usn < journal.NextUsn) {
            USN_CHANGE *changes = NULL;
            int count = 0;
            long long next_usn = saved_next_usn;
            int read_ok = ntfs_read_usn_changes(
                volume, saved_next_usn, journal.UsnJournalId,
                journal.NextUsn, i, &changes, &count, &next_usn,
                SERVICE_SYNC_MAX_CHANGES);

            if (!read_ok) {
                ntfs_close_volume(volume);
                continue;
            }
            for (int offset = 0; offset < count; offset += 512) {
                int batch = count - offset;
                if (batch > 512)
                    batch = 512;
                total_applied += index_apply_usn_changes(
                    app, changes + offset, batch);
            }
            ntfs_free_usn_changes(changes, count);

            EnterCriticalSection(&app->index_lock);
            app->volumes[i].usn_journal_id = journal.UsnJournalId;
            app->volumes[i].usn_next_usn = next_usn;
            app->volumes[i].usn_lowest_valid_usn =
                journal.LowestValidUsn;
            LeaveCriticalSection(&app->index_lock);
        }
        ntfs_close_volume(volume);
    }
    *changed_count = total_applied;
    return 1;
}

static int service_save_index(APP_STATE *app, const wchar_t *index_path)
{
    service_set_runtime_state(OE_SERVICE_STATE_SAVING, ERROR_SUCCESS);
    if (!cache_save_index_to_path(app, index_path)) {
        service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                  GetLastError() ? GetLastError()
                                                 : ERROR_WRITE_FAULT);
        return 0;
    }
    /* Reopen the V6 cache so the immutable UTF-8 name prefix is file-backed.
       Later USN changes append to the pool's small writable overlay. */
    if (cache_load_index_from_path(app, index_path) == CACHE_LOAD_CURRENT)
        index_build_ref_index(app);
    InterlockedExchange64(&g_last_update_unix, service_unix_now());
    InterlockedIncrement(&g_update_sequence);
    return 1;
}

static int service_rebuild_index(APP_STATE *app, const wchar_t *index_path)
{
    int indexed_volumes = 0;
    int failed_volumes = 0;

    service_set_runtime_state(OE_SERVICE_STATE_BUILDING, ERROR_SUCCESS);
    index_clear(app);
    app->indexed_volume_count = 0;
    app->index_error_count = 0;
    app->volume_count = ntfs_enumerate_volumes(app->volumes, 26);

    for (int i = 0; i < app->volume_count; i++) {
        HANDLE volume;
        INDEX_BUILD build;

        index_build_init(&build);

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
            return 0;
        if (!app->volumes[i].is_ntfs)
            continue;
        volume = ntfs_open_volume(app->volumes[i].volume_path);
        if (!volume) {
            failed_volumes++;
            continue;
        }
        if (ntfs_read_mft(volume, &build, i, NULL) > 0 ||
            ntfs_read_usn_index(volume, &build, i, NULL) > 0) {
            indexed_volumes++;
        } else {
            failed_volumes++;
        }
        ntfs_update_volume_usn_info(volume, &app->volumes[i]);
        ntfs_close_volume(volume);
        if (!index_add_entries(app, &build)) {
            index_build_free(&build);
            service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                      ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        index_build_free(&build);
    }

    app->indexed_volume_count = indexed_volumes;
    app->index_error_count = failed_volumes;
    if (indexed_volumes == 0 || app->entry_count == 0) {
        service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                  ERROR_ACCESS_DENIED);
        return 0;
    }
    index_sort_entries_by_name(app);
    if (!index_compact_entry_names(app) ||
        !index_build_ref_index(app)) {
        service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                  ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (!service_save_index(app, index_path))
        return 0;
    service_set_runtime_state(OE_SERVICE_STATE_READY, ERROR_SUCCESS);
    return 1;
}

static int service_load_or_build(APP_STATE *app, const wchar_t *index_path)
{
    int loaded;
    int needs_rebuild = 0;
    int changed_count = 0;

    service_set_runtime_state(OE_SERVICE_STATE_LOADING, ERROR_SUCCESS);
    loaded = cache_load_index_from_path(app, index_path);
    if (loaded == CACHE_LOAD_FAILED)
        return service_rebuild_index(app, index_path);
    if (!index_build_ref_index(app))
        return service_rebuild_index(app, index_path);

    service_set_runtime_state(OE_SERVICE_STATE_SYNCING, ERROR_SUCCESS);
    if (!service_sync_once(app, &needs_rebuild, &changed_count) ||
        needs_rebuild) {
        return service_rebuild_index(app, index_path);
    }
    if (loaded == CACHE_LOAD_LEGACY || changed_count > 0) {
        if (!service_save_index(app, index_path))
            return 0;
    }
    service_set_runtime_state(OE_SERVICE_STATE_READY, ERROR_SUCCESS);
    return 1;
}

static void service_worker_loop(void)
{
    wchar_t index_path[MAX_PATH];
    ULONGLONG dirty_since = 0;
    ULONGLONG last_change = 0;
    ULONGLONG last_save = GetTickCount64();
    int dirty = 0;
    int refresh_pending = 0;

    oe_service_get_index_path(index_path, MAX_PATH);
    if (!service_prepare_cache_directory(index_path)) {
        service_set_runtime_state(OE_SERVICE_STATE_ERROR, GetLastError());
    } else {
        service_load_or_build(&g_app, index_path);
        last_save = GetTickCount64();
    }

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        HANDLE waits[2] = { g_stop_event, g_refresh_event };
        DWORD wait_result = WaitForMultipleObjects(
            2, waits, FALSE, SERVICE_SYNC_INTERVAL_MS);
        int forced = wait_result == WAIT_OBJECT_0 + 1;
        int needs_rebuild = 0;
        int changed_count = 0;
        int cycle_ok = 1;
        ULONGLONG now;

        if (wait_result == WAIT_OBJECT_0)
            break;
        if (forced)
            refresh_pending = 1;
        if (g_app.entry_count == 0) {
            service_rebuild_index(&g_app, index_path);
            continue;
        }

        service_set_runtime_state(OE_SERVICE_STATE_SYNCING, ERROR_SUCCESS);
        if (!service_sync_once(&g_app, &needs_rebuild, &changed_count)) {
            if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
                break;
            service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                      ERROR_READ_FAULT);
            continue;
        }
        if (needs_rebuild) {
            service_rebuild_index(&g_app, index_path);
            dirty = 0;
            refresh_pending = 0;
            last_save = GetTickCount64();
            continue;
        }

        now = GetTickCount64();
        if (changed_count > 0) {
            if (!dirty)
                dirty_since = now;
            dirty = 1;
            last_change = now;
        }
        if (dirty &&
            ((refresh_pending && now - last_save >= SERVICE_SAVE_QUIET_MS) ||
             now - last_change >= SERVICE_SAVE_QUIET_MS ||
             now - dirty_since >= SERVICE_SAVE_MAX_DELAY_MS)) {
            if (service_save_index(&g_app, index_path)) {
                dirty = 0;
                refresh_pending = 0;
                last_save = now;
            } else {
                cycle_ok = 0;
            }
        } else if (refresh_pending && !dirty) {
            InterlockedExchange64(&g_last_update_unix, service_unix_now());
            InterlockedIncrement(&g_update_sequence);
            refresh_pending = 0;
        }
        if (cycle_ok)
            service_set_runtime_state(OE_SERVICE_STATE_READY, ERROR_SUCCESS);
    }
    if (dirty && !g_app.shutting_down)
        service_save_index(&g_app, index_path);
}

static void service_cleanup(void)
{
    InterlockedExchange(&g_app.shutting_down, 1);
    service_set_runtime_state(OE_SERVICE_STATE_STOPPING, ERROR_SUCCESS);
    if (g_stop_event)
        SetEvent(g_stop_event);
    if (g_pipe_thread) {
        WaitForSingleObject(g_pipe_thread, 5000);
        CloseHandle(g_pipe_thread);
        g_pipe_thread = NULL;
    }
    index_clear(&g_app);
    free(g_app.entries);
    free(g_app.filtered_indices);
    DeleteCriticalSection(&g_app.index_lock);
    if (g_refresh_event) {
        CloseHandle(g_refresh_event);
        g_refresh_event = NULL;
    }
    if (g_stop_event) {
        CloseHandle(g_stop_event);
        g_stop_event = NULL;
    }
}

static int service_initialize_runtime(void)
{
    index_init(&g_app);
    if (!g_app.entries || !g_app.filtered_indices)
        return 0;
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_refresh_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_stop_event || !g_refresh_event)
        return 0;
    g_pipe_thread = CreateThread(NULL, 0, service_pipe_thread_proc,
                                 NULL, 0, NULL);
    return g_pipe_thread != NULL;
}

static DWORD WINAPI service_control_handler(DWORD control, DWORD event_type,
                                             void *event_data, void *context)
{
    (void)event_type;
    (void)event_data;
    (void)context;
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        service_report_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 10000);
        service_set_runtime_state(OE_SERVICE_STATE_STOPPING, ERROR_SUCCESS);
        SetEvent(g_stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE)
        return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    g_status_handle = RegisterServiceCtrlHandlerExW(
        OE_SERVICE_NAME, service_control_handler, NULL);
    if (!g_status_handle)
        return;
    service_report_status(SERVICE_START_PENDING, ERROR_SUCCESS, 10000);
    if (!service_initialize_runtime()) {
        service_report_status(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY, 0);
        return;
    }
    service_report_status(SERVICE_RUNNING, ERROR_SUCCESS, 0);
    service_worker_loop();
    service_cleanup();
    service_report_status(SERVICE_STOPPED, ERROR_SUCCESS, 0);
}

static void service_print_usage(void)
{
    wprintf(L"Usage:\n"
            L"  OpenEverythingService.exe\n\n"
            L"This binary is launched by the Windows Service Control Manager.\n"
            L"Use OpenEverythingSetup.exe to install, update, start, stop, or remove it.\n");
}

int wmain(int argc, wchar_t **argv)
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (wchar_t *)OE_SERVICE_NAME, service_main },
        { NULL, NULL }
    };

    if (argc > 1) {
        service_print_usage();
        return 2;
    }
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            service_print_usage();
        return 1;
    }
    return 0;
}
