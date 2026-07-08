#ifndef IPC_H
#define IPC_H

#include "common.h"

int ipc_start_server(APP_STATE *app);
void ipc_stop_server(APP_STATE *app);
int ipc_send_command(const wchar_t *command);
#endif
