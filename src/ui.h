#ifndef UI_H
#define UI_H

#include "common.h"

int ui_init(HINSTANCE hInst);
HWND ui_create_main_window(HINSTANCE hInst, int nCmdShow, APP_STATE *app);
void ui_run_message_loop(void);
void ui_update_listview(HWND hwndList, APP_STATE *app);
void ui_update_status(HWND hwndStatus, APP_STATE *app);
#endif
