#include "service_client.h"

#include <stdio.h>
#include <wchar.h>

int main(void)
{
    wchar_t path[MAX_PATH];
    int ok;

    oe_service_get_index_path(path, MAX_PATH);
    ok = sizeof(OE_SERVICE_REQUEST) == 16 &&
         sizeof(OE_SERVICE_RESPONSE) == 56 &&
         wcsstr(path, L"\\OpenEverything\\index.dat") != NULL &&
         wcscmp(oe_service_state_name(OE_SERVICE_STATE_READY),
                L"ready") == 0 &&
         wcscmp(oe_service_state_name(0), L"unknown") == 0;
    printf("service_client_tests=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
