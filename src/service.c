#include "cache.h"
#include "index.h"
#include "ntfs.h"
#include "service_client.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
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
static volatile LONG g_rebuild_requested;
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

static int service_pipe_client_is_admin(HANDLE pipe)
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = NULL;
    BOOL is_member = FALSE;
    int result = 0;

    if (!ImpersonateNamedPipeClient(pipe))
        return 0;
    if (AllocateAndInitializeSid(&authority, 2,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0,
                                 &administrators)) {
        if (CheckTokenMembership(NULL, administrators, &is_member))
            result = is_member != FALSE;
        FreeSid(administrators);
    }
    RevertToSelf();
    return result;
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
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
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

static void service_serve_pipe_client(HANDLE pipe)
{
    OE_SERVICE_REQUEST request;
    OE_SERVICE_RESPONSE response;
    DWORD status = ERROR_SUCCESS;
    DWORD written = 0;

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
        case OE_SERVICE_COMMAND_REBUILD:
            if (!service_pipe_client_is_admin(pipe)) {
                status = ERROR_ACCESS_DENIED;
            } else {
                InterlockedExchange(&g_rebuild_requested, 1);
                SetEvent(g_refresh_event);
            }
            break;
        default:
            status = ERROR_INVALID_FUNCTION;
            break;
        }
    }

    service_fill_response(&response, status);
    WriteFile(pipe, &response, sizeof(response), &written, NULL);
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
    PSECURITY_DESCRIPTOR descriptor = NULL;
    int result;

    wcscpy_s(directory, MAX_PATH, index_path);
    last = wcsrchr(directory, L'\\');
    if (!last)
        return 0;
    *last = L'\0';
    result = SHCreateDirectoryExW(NULL, directory, NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS)
        return 0;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;AU)",
            SDDL_REVISION_1, &descriptor, NULL)) {
        SetFileSecurityW(directory, DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
    }
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
        INDEX_ENTRY *entries = NULL;
        int entry_count = 0;

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
            return 0;
        if (!app->volumes[i].is_ntfs)
            continue;
        volume = ntfs_open_volume(app->volumes[i].volume_path);
        if (!volume) {
            failed_volumes++;
            continue;
        }
        if (ntfs_read_usn_index(volume, &entries, &entry_count, i, NULL) > 0 ||
            ntfs_read_mft(volume, &entries, &entry_count, i, NULL) > 0) {
            indexed_volumes++;
        } else {
            failed_volumes++;
        }
        ntfs_update_volume_usn_info(volume, &app->volumes[i]);
        ntfs_close_volume(volume);
        if (!index_add_entries(app, entries, entry_count)) {
            for (int n = 0; n < entry_count; n++)
                index_free_entry(&entries[n]);
            free(entries);
            service_set_runtime_state(OE_SERVICE_STATE_ERROR,
                                      ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        free(entries);
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
        if (InterlockedExchange(&g_rebuild_requested, 0)) {
            service_rebuild_index(&g_app, index_path);
            dirty = 0;
            refresh_pending = 0;
            last_save = GetTickCount64();
            continue;
        }
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

static int service_is_elevated(void)
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

static int service_relaunch_elevated(const wchar_t *operation)
{
    wchar_t module[MAX_PATH];
    SHELLEXECUTEINFOW execute;
    DWORD exit_code = 1;

    if (!GetModuleFileNameW(NULL, module, MAX_PATH))
        return 1;
    memset(&execute, 0, sizeof(execute));
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = operation;
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute))
        return 1;
    WaitForSingleObject(execute.hProcess, INFINITE);
    GetExitCodeProcess(execute.hProcess, &exit_code);
    CloseHandle(execute.hProcess);
    return (int)exit_code;
}

static int service_installed_binary_path(wchar_t *path, size_t path_size)
{
    wchar_t program_files[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0,
                                program_files)))
        return 0;
    _snwprintf_s(path, path_size, _TRUNCATE,
                 L"%s\\OpenEverything\\OpenEverythingService.exe",
                 program_files);
    return path[0] != L'\0';
}

static void service_stop_existing_for_upgrade(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS control_status;
    SERVICE_STATUS_PROCESS status;
    DWORD bytes;
    ULONGLONG deadline;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return;
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return;
    }
    ControlService(service, SERVICE_CONTROL_STOP, &control_status);
    deadline = GetTickCount64() + 30000;
    while (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                (BYTE *)&status, sizeof(status), &bytes) &&
           status.dwCurrentState != SERVICE_STOPPED &&
           GetTickCount64() < deadline) {
        Sleep(200);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

static int service_install(void)
{
    wchar_t source[MAX_PATH];
    wchar_t target[MAX_PATH];
    wchar_t directory[MAX_PATH];
    wchar_t cli_source[MAX_PATH];
    wchar_t cli_target[MAX_PATH];
    wchar_t binary_path[MAX_PATH + 4];
    wchar_t *last;
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_DESCRIPTIONW description;
    SERVICE_DELAYED_AUTO_START_INFO delayed;
    SC_ACTION actions[3];
    SERVICE_FAILURE_ACTIONSW failure;
    DWORD error;

    if (!GetModuleFileNameW(NULL, source, MAX_PATH) ||
        !service_installed_binary_path(target, MAX_PATH))
        return 1;
    service_stop_existing_for_upgrade();
    wcscpy_s(directory, MAX_PATH, target);
    last = wcsrchr(directory, L'\\');
    if (!last)
        return 1;
    *last = L'\0';
    if (SHCreateDirectoryExW(NULL, directory, NULL) != ERROR_SUCCESS &&
        GetLastError() != ERROR_ALREADY_EXISTS &&
        GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES)
        return 1;
    if (_wcsicmp(source, target) != 0 &&
        !CopyFileW(source, target, FALSE)) {
        fwprintf(stderr, L"Unable to copy service binary: %lu\n",
                 GetLastError());
        return 1;
    }
    wcscpy_s(cli_source, MAX_PATH, source);
    last = wcsrchr(cli_source, L'\\');
    if (last) {
        wcscpy_s(last + 1, MAX_PATH - (size_t)(last + 1 - cli_source),
                 L"OpenEverythingCLI.exe");
        _snwprintf_s(cli_target, MAX_PATH, _TRUNCATE,
                     L"%s\\OpenEverythingCLI.exe", directory);
        if (GetFileAttributesW(cli_source) != INVALID_FILE_ATTRIBUTES &&
            _wcsicmp(cli_source, cli_target) != 0 &&
            !CopyFileW(cli_source, cli_target, FALSE)) {
            fwprintf(stderr, L"Warning: unable to install CLI binary: %lu\n",
                     GetLastError());
        }
    }
    _snwprintf_s(binary_path, MAX_PATH + 4, _TRUNCATE, L"\"%s\"", target);

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }
    service = CreateServiceW(
        manager, OE_SERVICE_NAME, OE_SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary_path,
        NULL, NULL, NULL, L"LocalSystem", NULL);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(manager, OE_SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (service)
            ChangeServiceConfigW(service, SERVICE_NO_CHANGE,
                                 SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                 binary_path, NULL, NULL, NULL,
                                 L"LocalSystem", NULL,
                                 OE_SERVICE_DISPLAY_NAME);
    }
    if (!service) {
        error = GetLastError();
        CloseServiceHandle(manager);
        fwprintf(stderr, L"CreateService failed: %lu\n", error);
        return 1;
    }

    description.lpDescription =
        L"Maintains the OpenEverything NTFS/USN file index for unprivileged clients.";
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    delayed.fDelayedAutostart = TRUE;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                          &delayed);
    actions[0].Type = SC_ACTION_RESTART; actions[0].Delay = 5000;
    actions[1].Type = SC_ACTION_RESTART; actions[1].Delay = 15000;
    actions[2].Type = SC_ACTION_RESTART; actions[2].Delay = 30000;
    memset(&failure, 0, sizeof(failure));
    failure.dwResetPeriod = 86400;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    if (!StartServiceW(service, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        fwprintf(stderr, L"Service installed but start failed: %lu\n", error);
        return 1;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    wprintf(L"OpenEverythingService installed and started.\n");
    return 0;
}

static int service_wait_stopped(SC_HANDLE service, DWORD timeout_ms)
{
    SERVICE_STATUS_PROCESS status;
    DWORD bytes;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    for (;;) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  (BYTE *)&status, sizeof(status), &bytes))
            return 0;
        if (status.dwCurrentState == SERVICE_STOPPED)
            return 1;
        if (GetTickCount64() >= deadline)
            return 0;
        Sleep(200);
    }
}

static int service_uninstall(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS status;
    wchar_t installed[MAX_PATH];
    wchar_t installed_cli[MAX_PATH];
    wchar_t current[MAX_PATH];
    int ok = 1;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return 1;
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        CloseServiceHandle(manager);
        fwprintf(stderr, L"OpenService failed: %lu\n", GetLastError());
        return 1;
    }
    if (ControlService(service, SERVICE_CONTROL_STOP, &status) ||
        GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        service_wait_stopped(service, 30000);
    if (!DeleteService(service) &&
        GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
        ok = 0;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    if (service_installed_binary_path(installed, MAX_PATH) &&
        GetModuleFileNameW(NULL, current, MAX_PATH) &&
        _wcsicmp(installed, current) != 0)
        DeleteFileW(installed);
    if (service_installed_binary_path(installed, MAX_PATH)) {
        wchar_t *last = wcsrchr(installed, L'\\');
        if (last) {
            wcscpy_s(last + 1,
                     MAX_PATH - (size_t)(last + 1 - installed),
                     L"OpenEverythingCLI.exe");
            wcscpy_s(installed_cli, MAX_PATH, installed);
            DeleteFileW(installed_cli);
        }
    }
    if (ok)
        wprintf(L"OpenEverythingService stopped and removed.\n");
    return ok ? 0 : 1;
}

static int service_control_start_stop(int start)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS status;
    int ok;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return 1;
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           start ? SERVICE_START : SERVICE_STOP |
                           SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return 1;
    }
    if (start) {
        ok = StartServiceW(service, 0, NULL) ||
             GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    } else {
        ok = ControlService(service, SERVICE_CONTROL_STOP, &status) ||
             GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok ? 0 : 1;
}

static int service_print_status(void)
{
    OE_SERVICE_RESPONSE response;
    if (!oe_service_get_status(&response)) {
        wprintf(L"OpenEverythingService is not reachable.\n");
        return 1;
    }
    wprintf(L"State: %s\nEntries: %d\nVolumes: %d\n"
            L"Update sequence: %u\nLast error: %u\n",
            oe_service_state_name(response.state),
            response.entry_count, response.volume_count,
            response.update_sequence, response.last_error);
    return 0;
}

static BOOL WINAPI service_console_handler(DWORD control)
{
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT) {
        if (g_stop_event)
            SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

static int service_run_console(void)
{
    SetConsoleCtrlHandler(service_console_handler, TRUE);
    if (!service_initialize_runtime())
        return 1;
    wprintf(L"OpenEverythingService console mode. Press Ctrl+C to stop.\n");
    service_worker_loop();
    service_cleanup();
    return 0;
}

static void service_print_usage(void)
{
    wprintf(L"Usage:\n"
            L"  OpenEverythingService install\n"
            L"  OpenEverythingService uninstall\n"
            L"  OpenEverythingService start|stop|status\n"
            L"  OpenEverythingService console\n");
}

int wmain(int argc, wchar_t **argv)
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (wchar_t *)OE_SERVICE_NAME, service_main },
        { NULL, NULL }
    };

    if (argc > 1) {
        if (_wcsicmp(argv[1], L"install") == 0)
            return service_is_elevated()
                ? service_install()
                : service_relaunch_elevated(L"install-elevated");
        if (_wcsicmp(argv[1], L"install-elevated") == 0)
            return service_is_elevated() ? service_install() : 1;
        if (_wcsicmp(argv[1], L"uninstall") == 0)
            return service_is_elevated()
                ? service_uninstall()
                : service_relaunch_elevated(L"uninstall-elevated");
        if (_wcsicmp(argv[1], L"uninstall-elevated") == 0)
            return service_is_elevated() ? service_uninstall() : 1;
        if (_wcsicmp(argv[1], L"start") == 0)
            return service_is_elevated()
                ? service_control_start_stop(1)
                : service_relaunch_elevated(L"start-elevated");
        if (_wcsicmp(argv[1], L"start-elevated") == 0)
            return service_is_elevated() ? service_control_start_stop(1) : 1;
        if (_wcsicmp(argv[1], L"stop") == 0)
            return service_is_elevated()
                ? service_control_start_stop(0)
                : service_relaunch_elevated(L"stop-elevated");
        if (_wcsicmp(argv[1], L"stop-elevated") == 0)
            return service_is_elevated() ? service_control_start_stop(0) : 1;
        if (_wcsicmp(argv[1], L"status") == 0)
            return service_print_status();
        if (_wcsicmp(argv[1], L"console") == 0)
            return service_run_console();
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
