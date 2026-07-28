#include "service_client.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oe_service_get_index_path(wchar_t *path, size_t path_size)
{
    wchar_t program_data[MAX_PATH];

    if (!path || path_size == 0)
        return;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0,
                                   program_data))) {
        _snwprintf_s(path, path_size, _TRUNCATE,
                     L"%s\\OpenEverything\\index.dat", program_data);
    } else {
        wcsncpy_s(path, path_size, L"index.dat", _TRUNCATE);
    }
}

static int oe_service_write_all(HANDLE pipe, const void *data, DWORD size)
{
    const unsigned char *cursor = (const unsigned char *)data;
    DWORD total = 0;

    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, cursor + total, size - total, &written, NULL) ||
            written == 0)
            return 0;
        total += written;
    }
    return 1;
}

static int oe_service_read_all(HANDLE pipe, void *data, DWORD size)
{
    unsigned char *cursor = (unsigned char *)data;
    DWORD total = 0;

    while (total < size) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor + total, size - total, &read, NULL) ||
            read == 0)
            return 0;
        total += read;
    }
    return 1;
}

int oe_service_request(OE_SERVICE_COMMAND command,
                       OE_SERVICE_RESPONSE *response,
                       DWORD timeout_ms)
{
    OE_SERVICE_REQUEST request;
    HANDLE pipe;
    DWORD mode = PIPE_READMODE_BYTE;
    ULONGLONG deadline;

    if (!response)
        return 0;
    memset(response, 0, sizeof(*response));

    deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        DWORD error;
        DWORD remaining;
        ULONGLONG now;

        pipe = CreateFileW(OE_SERVICE_PIPE_NAME,
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE)
            break;

        error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
            return 0;
        now = GetTickCount64();
        if (now >= deadline)
            return 0;
        remaining = (DWORD)(deadline - now);

        if (!WaitNamedPipeW(OE_SERVICE_PIPE_NAME, remaining)) {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND &&
                error != ERROR_SEM_TIMEOUT &&
                error != ERROR_PIPE_BUSY)
                return 0;
            Sleep(10);
        }
    }
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);

    memset(&request, 0, sizeof(request));
    request.magic = OE_SERVICE_PROTOCOL_MAGIC;
    request.version = OE_SERVICE_PROTOCOL_VERSION;
    request.command = (uint32_t)command;

    if (!oe_service_write_all(pipe, &request, sizeof(request)) ||
        !oe_service_read_all(pipe, response, sizeof(*response))) {
        CloseHandle(pipe);
        memset(response, 0, sizeof(*response));
        return 0;
    }
    CloseHandle(pipe);

    return response->magic == OE_SERVICE_PROTOCOL_MAGIC &&
           response->version == OE_SERVICE_PROTOCOL_VERSION;
}

int oe_service_get_status(OE_SERVICE_RESPONSE *response)
{
    return oe_service_request(OE_SERVICE_COMMAND_STATUS, response, 500);
}

int oe_service_request_refresh(OE_SERVICE_RESPONSE *response)
{
    return oe_service_request(OE_SERVICE_COMMAND_REFRESH, response, 2000) &&
           response->status == ERROR_SUCCESS;
}

int oe_service_wait_for_update(uint32_t previous_sequence,
                               DWORD timeout_ms,
                               OE_SERVICE_RESPONSE *response)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    if (!response)
        return 0;
    for (;;) {
        if (oe_service_get_status(response) &&
            response->update_sequence != previous_sequence &&
            (response->state == OE_SERVICE_STATE_READY ||
             response->state == OE_SERVICE_STATE_ERROR)) {
            return response->state == OE_SERVICE_STATE_READY;
        }
        if (GetTickCount64() >= deadline)
            return 0;
        Sleep(200);
    }
}

const wchar_t *oe_service_state_name(uint32_t state)
{
    switch (state) {
    case OE_SERVICE_STATE_STARTING: return L"starting";
    case OE_SERVICE_STATE_LOADING: return L"loading";
    case OE_SERVICE_STATE_BUILDING: return L"building";
    case OE_SERVICE_STATE_READY: return L"ready";
    case OE_SERVICE_STATE_SYNCING: return L"syncing";
    case OE_SERVICE_STATE_SAVING: return L"saving";
    case OE_SERVICE_STATE_ERROR: return L"error";
    case OE_SERVICE_STATE_STOPPING: return L"stopping";
    default: return L"unknown";
    }
}
