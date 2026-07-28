#ifndef OPENEVERYTHING_SERVICE_CLIENT_H
#define OPENEVERYTHING_SERVICE_CLIENT_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define OE_SERVICE_NAME L"OpenEverythingService"
#define OE_SERVICE_DISPLAY_NAME L"OpenEverything Index Service"
#define OE_SERVICE_PIPE_NAME L"\\\\.\\pipe\\OpenEverythingService"
#define OE_SERVICE_PROTOCOL_MAGIC 0x4353454fU
#define OE_SERVICE_PROTOCOL_VERSION 1U

typedef enum {
    OE_SERVICE_COMMAND_STATUS = 1,
    OE_SERVICE_COMMAND_REFRESH = 2,
    OE_SERVICE_COMMAND_REBUILD = 3
} OE_SERVICE_COMMAND;

typedef enum {
    OE_SERVICE_STATE_STARTING = 1,
    OE_SERVICE_STATE_LOADING = 2,
    OE_SERVICE_STATE_BUILDING = 3,
    OE_SERVICE_STATE_READY = 4,
    OE_SERVICE_STATE_SYNCING = 5,
    OE_SERVICE_STATE_SAVING = 6,
    OE_SERVICE_STATE_ERROR = 7,
    OE_SERVICE_STATE_STOPPING = 8
} OE_SERVICE_STATE;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command;
    uint32_t reserved;
} OE_SERVICE_REQUEST;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t status;
    uint32_t state;
    int32_t entry_count;
    int32_t volume_count;
    int32_t indexed_volume_count;
    int32_t index_error_count;
    uint32_t update_sequence;
    uint32_t process_id;
    uint32_t last_error;
    uint32_t reserved;
    int64_t last_update_unix;
} OE_SERVICE_RESPONSE;
#pragma pack(pop)

void oe_service_get_index_path(wchar_t *path, size_t path_size);
int oe_service_request(OE_SERVICE_COMMAND command,
                       OE_SERVICE_RESPONSE *response,
                       DWORD timeout_ms);
int oe_service_get_status(OE_SERVICE_RESPONSE *response);
int oe_service_request_refresh(OE_SERVICE_RESPONSE *response);
int oe_service_wait_for_update(uint32_t previous_sequence,
                               DWORD timeout_ms,
                               OE_SERVICE_RESPONSE *response);
const wchar_t *oe_service_state_name(uint32_t state);

#endif
