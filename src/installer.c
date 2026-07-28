#include "service_client.h"
#include "installer_resources.h"
#include "resource.h"
#include "version.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <string.h>
#include <wchar.h>

#define OE_INSTALL_DIRECTORY_NAME L"OpenEverything"
#define OE_SERVICE_CONFIGURATION_ACCESS \
    (SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS | DELETE)
#define OE_GUI_INSTALL_BUTTON 1001
#define OE_GUI_UNINSTALL_BUTTON 1002
#define OE_GUI_PROGRESS_DONE_BUTTON 1003

typedef enum {
    OE_GUI_OPERATION_INSTALL = 1,
    OE_GUI_OPERATION_UNINSTALL = 2
} OE_GUI_OPERATION;

typedef struct {
    OE_GUI_OPERATION operation;
    int create_desktop_shortcut;
    HWND dialog;
    HANDLE worker;
    volatile LONG result;
} OE_GUI_PROGRESS_CONTEXT;

static DWORD installer_join_path(wchar_t *path, size_t path_size,
                                 const wchar_t *directory,
                                 const wchar_t *filename)
{
    if (_snwprintf_s(path, path_size, _TRUNCATE, L"%s\\%s", directory,
                     filename) < 0)
        return ERROR_INSUFFICIENT_BUFFER;
    return ERROR_SUCCESS;
}

static DWORD installer_get_payload(HMODULE module, WORD resource_id,
                                   const void **data, DWORD *data_size)
{
    HRSRC resource;
    HGLOBAL loaded;

    resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource)
        return GetLastError();
    loaded = LoadResource(module, resource);
    if (!loaded)
        return GetLastError();
    *data_size = SizeofResource(module, resource);
    *data = LockResource(loaded);
    if (!*data || *data_size == 0)
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    return ERROR_SUCCESS;
}

static DWORD installer_write_payload(HMODULE module, WORD resource_id,
                                     const wchar_t *target_directory,
                                     const wchar_t *filename)
{
    const unsigned char *data;
    DWORD data_size;
    DWORD offset = 0;
    wchar_t target[MAX_PATH];
    wchar_t temporary[MAX_PATH];
    HANDLE file;
    DWORD error;

    error = installer_get_payload(module, resource_id,
                                  (const void **)&data, &data_size);
    if (error != ERROR_SUCCESS)
        return error;
    error = installer_join_path(target, MAX_PATH, target_directory, filename);
    if (error != ERROR_SUCCESS)
        return error;
    if (_snwprintf_s(temporary, MAX_PATH, _TRUNCATE, L"%s.setup.tmp",
                     target) < 0)
        return ERROR_INSUFFICIENT_BUFFER;

    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    while (offset < data_size) {
        BOOL write_ok;
        DWORD written = 0;
        write_ok = WriteFile(file, data + offset, data_size - offset,
                             &written, NULL);
        if (!write_ok || written == 0) {
            error = write_ok ? ERROR_WRITE_FAULT : GetLastError();
            CloseHandle(file);
            DeleteFileW(temporary);
            return error;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        error = GetLastError();
        CloseHandle(file);
        DeleteFileW(temporary);
        return error;
    }
    CloseHandle(file);
    if (!MoveFileExW(temporary, target,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary);
        return error;
    }
    return ERROR_SUCCESS;
}

static DWORD installer_get_install_directory(wchar_t *directory,
                                              size_t directory_size)
{
    wchar_t program_files[MAX_PATH];

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL,
                                SHGFP_TYPE_CURRENT, program_files)))
        return ERROR_PATH_NOT_FOUND;
    return installer_join_path(directory, directory_size, program_files,
                               OE_INSTALL_DIRECTORY_NAME);
}

static DWORD installer_hresult_error(HRESULT result)
{
    if (HRESULT_FACILITY(result) == FACILITY_WIN32 && HRESULT_CODE(result) != 0)
        return HRESULT_CODE(result);
    return ERROR_GEN_FAILURE;
}

static DWORD installer_create_shortcut_file(const wchar_t *link_path,
                                            const wchar_t *target,
                                            const wchar_t *working_directory)
{
    IShellLinkW *shell_link = NULL;
    IPersistFile *persist = NULL;
    HRESULT result;

    result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IShellLinkW, (void **)&shell_link);
    if (FAILED(result))
        return installer_hresult_error(result);
    result = shell_link->lpVtbl->SetPath(shell_link, target);
    if (SUCCEEDED(result))
        result = shell_link->lpVtbl->SetWorkingDirectory(
            shell_link, working_directory);
    if (SUCCEEDED(result))
        result = shell_link->lpVtbl->SetDescription(
            shell_link, L"OpenEverything 文件搜索");
    if (SUCCEEDED(result))
        result = shell_link->lpVtbl->SetIconLocation(shell_link, target, 0);
    if (SUCCEEDED(result))
        result = shell_link->lpVtbl->QueryInterface(
            shell_link, &IID_IPersistFile, (void **)&persist);
    if (SUCCEEDED(result))
        result = persist->lpVtbl->Save(persist, link_path, TRUE);
    if (persist)
        persist->lpVtbl->Release(persist);
    shell_link->lpVtbl->Release(shell_link);
    return SUCCEEDED(result) ? ERROR_SUCCESS : installer_hresult_error(result);
}

static DWORD installer_create_shortcuts(const wchar_t *install_directory,
                                        int create_desktop)
{
    wchar_t target[MAX_PATH];
    wchar_t folder[MAX_PATH];
    wchar_t link_path[MAX_PATH];
    HRESULT com_result;
    DWORD error;
    int uninitialize = 0;

    error = installer_join_path(target, MAX_PATH, install_directory,
                                L"OpenEverything.exe");
    if (error != ERROR_SUCCESS)
        return error;
    com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com_result))
        uninitialize = 1;
    else if (com_result != RPC_E_CHANGED_MODE)
        return installer_hresult_error(com_result);

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL,
                                SHGFP_TYPE_CURRENT, folder))) {
        error = ERROR_PATH_NOT_FOUND;
        goto done;
    }
    error = installer_join_path(link_path, MAX_PATH, folder,
                                L"OpenEverything.lnk");
    if (error != ERROR_SUCCESS)
        goto done;
    error = installer_create_shortcut_file(link_path, target,
                                           install_directory);
    if (error != ERROR_SUCCESS)
        goto done;

    if (create_desktop) {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL,
                                    SHGFP_TYPE_CURRENT, folder))) {
            error = ERROR_PATH_NOT_FOUND;
            goto done;
        }
        error = installer_join_path(link_path, MAX_PATH, folder,
                                    L"OpenEverything.lnk");
        if (error != ERROR_SUCCESS)
            goto done;
        error = installer_create_shortcut_file(link_path, target,
                                               install_directory);
    }

done:
    if (uninitialize)
        CoUninitialize();
    return error;
}

static void installer_remove_shortcuts(void)
{
    static const int folders[] = {
        CSIDL_COMMON_PROGRAMS,
        CSIDL_DESKTOPDIRECTORY
    };
    wchar_t folder[MAX_PATH];
    wchar_t link_path[MAX_PATH];
    size_t i;

    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        if (SUCCEEDED(SHGetFolderPathW(NULL, folders[i], NULL,
                                      SHGFP_TYPE_CURRENT, folder)) &&
            installer_join_path(link_path, MAX_PATH, folder,
                                L"OpenEverything.lnk") == ERROR_SUCCESS)
            DeleteFileW(link_path);
    }
}

static DWORD installer_prepare_cache_directory(void)
{
    wchar_t common_app_data[MAX_PATH];
    wchar_t directory[MAX_PATH];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD error = ERROR_SUCCESS;
    int directory_result;

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL,
                                SHGFP_TYPE_CURRENT, common_app_data)))
        return ERROR_PATH_NOT_FOUND;
    error = installer_join_path(directory, MAX_PATH, common_app_data,
                                OE_INSTALL_DIRECTORY_NAME);
    if (error != ERROR_SUCCESS)
        return error;
    directory_result = SHCreateDirectoryExW(NULL, directory, NULL);
    if (directory_result != ERROR_SUCCESS &&
        directory_result != ERROR_ALREADY_EXISTS)
        return (DWORD)directory_result;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;AU)",
            SDDL_REVISION_1, &descriptor, NULL))
        return GetLastError();
    if (!SetFileSecurityW(directory, DACL_SECURITY_INFORMATION, descriptor))
        error = GetLastError();
    LocalFree(descriptor);
    return error;
}

static DWORD installer_wait_stopped(SC_HANDLE service, DWORD timeout_ms)
{
    SERVICE_STATUS_PROCESS status;
    DWORD bytes;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    for (;;) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  (BYTE *)&status, sizeof(status), &bytes))
            return GetLastError();
        if (status.dwCurrentState == SERVICE_STOPPED)
            return ERROR_SUCCESS;
        if (GetTickCount64() >= deadline)
            return ERROR_TIMEOUT;
        Sleep(200);
    }
}

static DWORD installer_stop_service_for_upgrade(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS control_status;
    DWORD error = ERROR_SUCCESS;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return GetLastError();
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        error = GetLastError();
        CloseServiceHandle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : error;
    }
    if (!ControlService(service, SERVICE_CONTROL_STOP, &control_status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        error = GetLastError();
    } else {
        error = installer_wait_stopped(service, 30000);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
}

static DWORD installer_configure_service(const wchar_t *service_path)
{
    wchar_t binary_path[MAX_PATH + 4];
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_DESCRIPTIONW description;
    SERVICE_DELAYED_AUTO_START_INFO delayed;
    SERVICE_FAILURE_ACTIONSW no_recovery;
    SERVICE_SID_INFO service_sid;
    int created = 0;
    DWORD error = ERROR_SUCCESS;

    if (_snwprintf_s(binary_path, MAX_PATH + 4, _TRUNCATE, L"\"%s\"",
                     service_path) < 0)
        return ERROR_INSUFFICIENT_BUFFER;

    manager = OpenSCManagerW(NULL, NULL,
                             SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager)
        return GetLastError();
    service = CreateServiceW(
        manager, OE_SERVICE_NAME, OE_SERVICE_DISPLAY_NAME,
        OE_SERVICE_CONFIGURATION_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary_path,
        NULL, NULL, NULL, L"LocalSystem", NULL);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(manager, OE_SERVICE_NAME,
                               OE_SERVICE_CONFIGURATION_ACCESS);
    } else if (service) {
        created = 1;
    }
    if (!service) {
        error = GetLastError();
        CloseServiceHandle(manager);
        return error;
    }
    if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                              SERVICE_ERROR_NORMAL, binary_path, NULL, NULL,
                              NULL, L"LocalSystem", NULL,
                              OE_SERVICE_DISPLAY_NAME)) {
        error = GetLastError();
        goto done;
    }

    description.lpDescription =
        L"Maintains the OpenEverything NTFS/USN file index for unprivileged clients.";
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION,
                               &description)) {
        error = GetLastError();
        goto done;
    }
    delayed.fDelayedAutostart = TRUE;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                               &delayed)) {
        error = GetLastError();
        goto done;
    }

    memset(&service_sid, 0, sizeof(service_sid));
    service_sid.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO,
                               &service_sid)) {
        error = GetLastError();
        goto done;
    }

    /* Clear the legacy automatic-restart policy. The SCM still starts this
       delayed auto-start service at boot, but a failed process is not silently
       restarted in a loop. */
    memset(&no_recovery, 0, sizeof(no_recovery));
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &no_recovery)) {
        error = GetLastError();
        goto done;
    }
    if (!StartServiceW(service, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        error = GetLastError();

done:
    if (error != ERROR_SUCCESS && created)
        DeleteService(service);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
}

static DWORD installer_control_service(int start);

static DWORD installer_install(int create_desktop_shortcut)
{
    static const WORD payloads[] = {
        IDR_PAYLOAD_SERVICE,
        IDR_PAYLOAD_CLI,
        IDR_PAYLOAD_GUI
    };
    HMODULE module;
    wchar_t target_directory[MAX_PATH];
    wchar_t service_path[MAX_PATH];
    DWORD error;
    int directory_result;
    size_t i;

    module = GetModuleHandleW(NULL);
    if (!module)
        return GetLastError();
    for (i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++) {
        const void *data;
        DWORD data_size;
        error = installer_get_payload(module, payloads[i], &data, &data_size);
        if (error != ERROR_SUCCESS)
            return error;
    }
    error = installer_get_install_directory(target_directory, MAX_PATH);
    if (error != ERROR_SUCCESS)
        return error;

    directory_result = SHCreateDirectoryExW(NULL, target_directory, NULL);
    if (directory_result != ERROR_SUCCESS &&
        directory_result != ERROR_ALREADY_EXISTS)
        return (DWORD)directory_result;
    error = installer_prepare_cache_directory();
    if (error != ERROR_SUCCESS)
        return error;
    error = installer_stop_service_for_upgrade();
    if (error != ERROR_SUCCESS)
        return error;

    error = installer_write_payload(module, IDR_PAYLOAD_SERVICE,
                                    target_directory,
                                    L"OpenEverythingService.exe");
    if (error != ERROR_SUCCESS)
        goto restore_service;
    error = installer_write_payload(module, IDR_PAYLOAD_CLI,
                                    target_directory,
                                    L"OpenEverythingCLI.exe");
    if (error != ERROR_SUCCESS)
        goto restore_service;
    error = installer_write_payload(module, IDR_PAYLOAD_GUI,
                                    target_directory,
                                    L"OpenEverything.exe");
    if (error != ERROR_SUCCESS)
        goto restore_service;
    error = installer_join_path(service_path, MAX_PATH, target_directory,
                                L"OpenEverythingService.exe");
    if (error != ERROR_SUCCESS)
        goto restore_service;
    error = installer_configure_service(service_path);
    if (error == ERROR_SUCCESS)
        return installer_create_shortcuts(target_directory,
                                          create_desktop_shortcut);

restore_service:
    installer_control_service(1);
    return error;
}

static DWORD installer_control_service(int start)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS status;
    DWORD error = ERROR_SUCCESS;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return GetLastError();
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           start ? SERVICE_START : SERVICE_STOP);
    if (!service) {
        error = GetLastError();
    } else if (start) {
        if (!StartServiceW(service, 0, NULL) &&
            GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            error = GetLastError();
    } else if (!ControlService(service, SERVICE_CONTROL_STOP, &status) &&
               GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        error = GetLastError();
    }
    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
}

static DWORD installer_remove_payload(const wchar_t *directory,
                                      const wchar_t *filename)
{
    wchar_t path[MAX_PATH];

    if (installer_join_path(path, MAX_PATH, directory, filename) == ERROR_SUCCESS)
        if (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND)
            return ERROR_SUCCESS;
        else
            return GetLastError();
    return ERROR_INSUFFICIENT_BUFFER;
}

static DWORD installer_uninstall(void)
{
    wchar_t directory[MAX_PATH];
    SC_HANDLE manager;
    SC_HANDLE service = NULL;
    SERVICE_STATUS status;
    DWORD error = ERROR_SUCCESS;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return GetLastError();
    service = OpenServiceW(manager, OE_SERVICE_NAME,
                           SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST)
            goto done;
        error = ERROR_SUCCESS;
    } else {
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            error = GetLastError();
            goto done;
        }
        error = installer_wait_stopped(service, 30000);
        if (error != ERROR_SUCCESS)
            goto done;
        if (!DeleteService(service) &&
            GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            error = GetLastError();
            goto done;
        }
        CloseServiceHandle(service);
        service = NULL;
    }

    error = installer_get_install_directory(directory, MAX_PATH);
    if (error != ERROR_SUCCESS)
        goto done;
    {
        DWORD remove_error;
        remove_error = installer_remove_payload(directory,
                                                 L"OpenEverythingService.exe");
        if (remove_error != ERROR_SUCCESS)
            error = remove_error;
        remove_error = installer_remove_payload(directory,
                                                L"OpenEverythingCLI.exe");
        if (remove_error != ERROR_SUCCESS && error == ERROR_SUCCESS)
            error = remove_error;
        remove_error = installer_remove_payload(directory,
                                                L"OpenEverything.exe");
        if (remove_error != ERROR_SUCCESS && error == ERROR_SUCCESS)
            error = remove_error;
        if (!RemoveDirectoryW(directory) &&
            GetLastError() != ERROR_PATH_NOT_FOUND &&
            error == ERROR_SUCCESS)
            error = GetLastError();
    }
    installer_remove_shortcuts();

done:
    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
}

static int installer_service_is_installed(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    int installed = 0;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return 0;
    service = OpenServiceW(manager, OE_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (service) {
        installed = 1;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return installed;
}

static HICON installer_load_icon(HINSTANCE instance)
{
    return (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
                             IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
}

static int installer_gui_choose_operation(HINSTANCE instance, int installed,
                                          int *create_desktop_shortcut)
{
    static const TASKDIALOG_BUTTON install_buttons[] = {
        { OE_GUI_INSTALL_BUTTON,
          L"安装 OpenEverything\n安装图形界面、CLI/MCP 和后台索引服务" }
    };
    static const TASKDIALOG_BUTTON maintenance_buttons[] = {
        { OE_GUI_INSTALL_BUTTON,
          L"更新或修复\n重新安装全部组件并重新启动索引服务" },
        { OE_GUI_UNINSTALL_BUTTON,
          L"卸载\n停止索引服务并删除已安装的程序" }
    };
    TASKDIALOGCONFIG dialog;
    HICON icon;
    BOOL verification_checked = TRUE;
    int selected = IDCANCEL;
    HRESULT result;

    memset(&dialog, 0, sizeof(dialog));
    icon = installer_load_icon(instance);
    dialog.cbSize = sizeof(dialog);
    dialog.hInstance = instance;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_SIZE_TO_CONTENT |
                     TDF_USE_COMMAND_LINKS |
                     TDF_VERIFICATION_FLAG_CHECKED;
    if (icon) {
        dialog.dwFlags |= TDF_USE_HICON_MAIN;
        dialog.hMainIcon = icon;
    }
    dialog.pszWindowTitle = L"OpenEverything 安装程序";
    dialog.pszMainInstruction = installed
        ? L"OpenEverything 已安装"
        : L"安装 OpenEverything " OE_VERSION_WSTRING;
    dialog.pszContent = installed
        ? L"可以更新或修复现有安装，也可以从这台电脑卸载 OpenEverything。"
        : L"快速搜索本机文件，并为普通用户的 GUI、CLI 和 MCP 提供后台索引。\n\n安装位置：C:\\Program Files\\OpenEverything";
    dialog.pszVerificationText = L"在桌面创建快捷方式";
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pButtons = installed ? maintenance_buttons : install_buttons;
    dialog.cButtons = installed
        ? (UINT)(sizeof(maintenance_buttons) / sizeof(maintenance_buttons[0]))
        : (UINT)(sizeof(install_buttons) / sizeof(install_buttons[0]));
    dialog.nDefaultButton = OE_GUI_INSTALL_BUTTON;
    dialog.pszFooter = L"安装后可从桌面或开始菜单打开 OpenEverything。";

    result = TaskDialogIndirect(&dialog, &selected, NULL,
                                &verification_checked);
    if (icon)
        DestroyIcon(icon);
    if (FAILED(result)) {
        int fallback;
        fallback = MessageBoxW(
            NULL,
            installed
                ? L"OpenEverything 已安装。\n\n选择“是”更新或修复，选择“否”卸载。"
                : L"是否安装 OpenEverything？",
            L"OpenEverything 安装程序",
            installed ? MB_YESNOCANCEL | MB_ICONINFORMATION
                      : MB_OKCANCEL | MB_ICONINFORMATION);
        if (fallback == IDYES || (!installed && fallback == IDOK))
            selected = OE_GUI_INSTALL_BUTTON;
        else if (installed && fallback == IDNO)
            selected = OE_GUI_UNINSTALL_BUTTON;
        else
            selected = IDCANCEL;
    }
    *create_desktop_shortcut = verification_checked != FALSE;
    return selected;
}

static DWORD WINAPI installer_gui_worker(void *parameter)
{
    OE_GUI_PROGRESS_CONTEXT *context =
        (OE_GUI_PROGRESS_CONTEXT *)parameter;
    DWORD result;

#ifdef OE_SETUP_PREVIEW
    Sleep(800);
    result = ERROR_SUCCESS;
#else
    if (context->operation == OE_GUI_OPERATION_INSTALL)
        result = installer_install(context->create_desktop_shortcut);
    else
        result = installer_uninstall();
#endif
    InterlockedExchange(&context->result, (LONG)result);
    PostMessageW(context->dialog, TDM_ENABLE_BUTTON,
                 OE_GUI_PROGRESS_DONE_BUTTON, TRUE);
    PostMessageW(context->dialog, TDM_CLICK_BUTTON,
                 OE_GUI_PROGRESS_DONE_BUTTON, 0);
    return 0;
}

static HRESULT CALLBACK installer_gui_progress_callback(
    HWND dialog, UINT notification, WPARAM wparam, LPARAM lparam,
    LONG_PTR callback_data)
{
    OE_GUI_PROGRESS_CONTEXT *context =
        (OE_GUI_PROGRESS_CONTEXT *)callback_data;
    (void)wparam;
    (void)lparam;

    if (notification == TDN_CREATED) {
        context->dialog = dialog;
        SendMessageW(dialog, TDM_SET_MARQUEE_PROGRESS_BAR, TRUE, 0);
        SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        SendMessageW(dialog, TDM_ENABLE_BUTTON,
                     OE_GUI_PROGRESS_DONE_BUTTON, FALSE);
        context->worker = CreateThread(NULL, 0, installer_gui_worker,
                                       context, 0, NULL);
        if (!context->worker) {
            InterlockedExchange(&context->result, (LONG)GetLastError());
            PostMessageW(dialog, TDM_ENABLE_BUTTON,
                         OE_GUI_PROGRESS_DONE_BUTTON, TRUE);
            PostMessageW(dialog, TDM_CLICK_BUTTON,
                         OE_GUI_PROGRESS_DONE_BUTTON, 0);
        }
    }
    return S_OK;
}

static DWORD installer_gui_run_operation(HINSTANCE instance,
                                         OE_GUI_OPERATION operation,
                                         int create_desktop_shortcut)
{
    static const TASKDIALOG_BUTTON progress_button[] = {
        { OE_GUI_PROGRESS_DONE_BUTTON, L"正在处理..." }
    };
    OE_GUI_PROGRESS_CONTEXT context;
    TASKDIALOGCONFIG dialog;
    HICON icon;
    HRESULT task_result;

    memset(&context, 0, sizeof(context));
    context.operation = operation;
    context.create_desktop_shortcut = create_desktop_shortcut;
    context.result = ERROR_IO_PENDING;
    memset(&dialog, 0, sizeof(dialog));
    icon = installer_load_icon(instance);
    dialog.cbSize = sizeof(dialog);
    dialog.hInstance = instance;
    dialog.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_SIZE_TO_CONTENT;
    if (icon) {
        dialog.dwFlags |= TDF_USE_HICON_MAIN;
        dialog.hMainIcon = icon;
    }
    dialog.pszWindowTitle = L"OpenEverything 安装程序";
    dialog.pszMainInstruction = operation == OE_GUI_OPERATION_INSTALL
        ? L"正在安装 OpenEverything"
        : L"正在卸载 OpenEverything";
    dialog.pszContent = operation == OE_GUI_OPERATION_INSTALL
        ? L"正在写入程序文件、配置索引服务和创建快捷方式，请稍候。"
        : L"正在停止索引服务并删除已安装的程序，请稍候。";
    dialog.cButtons = 1;
    dialog.pButtons = progress_button;
    dialog.nDefaultButton = OE_GUI_PROGRESS_DONE_BUTTON;
    dialog.pfCallback = installer_gui_progress_callback;
    dialog.lpCallbackData = (LONG_PTR)&context;

    task_result = TaskDialogIndirect(&dialog, NULL, NULL, NULL);
    if (context.worker) {
        WaitForSingleObject(context.worker, INFINITE);
        CloseHandle(context.worker);
    }
    if (icon)
        DestroyIcon(icon);
    if (FAILED(task_result) &&
        InterlockedCompareExchange(&context.result, 0, 0) ==
            ERROR_IO_PENDING)
        return installer_hresult_error(task_result);
    return (DWORD)InterlockedCompareExchange(&context.result, 0, 0);
}

static void installer_gui_show_result(HINSTANCE instance,
                                      OE_GUI_OPERATION operation,
                                      DWORD error)
{
    TASKDIALOGCONFIG dialog;
    wchar_t system_message[512];
    wchar_t content[1024];
    wchar_t *end;
    HICON icon;

    memset(&dialog, 0, sizeof(dialog));
    memset(system_message, 0, sizeof(system_message));
    memset(content, 0, sizeof(content));
    icon = installer_load_icon(instance);
    dialog.cbSize = sizeof(dialog);
    dialog.hInstance = instance;
    dialog.dwFlags = TDF_SIZE_TO_CONTENT;
    if (icon) {
        dialog.dwFlags |= TDF_USE_HICON_MAIN;
        dialog.hMainIcon = icon;
    }
    dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    dialog.pszWindowTitle = L"OpenEverything 安装程序";
    if (error == ERROR_SUCCESS) {
        dialog.pszMainInstruction = operation == OE_GUI_OPERATION_INSTALL
            ? L"安装完成"
            : L"卸载完成";
        dialog.pszContent = operation == OE_GUI_OPERATION_INSTALL
            ? L"OpenEverything 已安装并启动后台索引服务。\n\n现在可以从桌面或开始菜单打开程序。"
            : L"OpenEverything 服务和程序文件已删除。索引缓存会保留，方便以后重新安装。";
    } else {
        if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL, error, 0, system_message,
                            (DWORD)(sizeof(system_message) /
                                    sizeof(system_message[0])), NULL))
            wcscpy_s(system_message,
                     sizeof(system_message) / sizeof(system_message[0]),
                     L"Windows 未提供错误说明。" );
        end = system_message + wcslen(system_message);
        while (end > system_message &&
               (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' '))
            *--end = L'\0';
        _snwprintf_s(content,
                     sizeof(content) / sizeof(content[0]), _TRUNCATE,
                     L"操作没有完成。\n\n错误代码：%lu\n%s",
                     error, system_message);
        dialog.pszMainInstruction = L"操作失败";
        dialog.pszContent = content;
        dialog.pszMainIcon = TD_ERROR_ICON;
        dialog.dwFlags &= ~TDF_USE_HICON_MAIN;
    }
    TaskDialogIndirect(&dialog, NULL, NULL, NULL);
    if (icon)
        DestroyIcon(icon);
}

static DWORD installer_run_gui_for_state(HINSTANCE instance, int installed)
{
    int create_desktop_shortcut = 1;
    int selected;
    OE_GUI_OPERATION operation;
    DWORD error;

    selected = installer_gui_choose_operation(
        instance, installed, &create_desktop_shortcut);
    if (selected == IDCANCEL)
        return ERROR_CANCELLED;
    operation = selected == OE_GUI_UNINSTALL_BUTTON
        ? OE_GUI_OPERATION_UNINSTALL
        : OE_GUI_OPERATION_INSTALL;
    error = installer_gui_run_operation(instance, operation,
                                        create_desktop_shortcut);
    installer_gui_show_result(instance, operation, error);
    return error;
}

static DWORD installer_run_gui(HINSTANCE instance)
{
    return installer_run_gui_for_state(instance,
                                       installer_service_is_installed());
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    wchar_t *command_line, int show_command)
{
    LPWSTR *argv;
    int argc;
    DWORD error;

    (void)instance;
    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    INITCOMMONCONTROLSEX controls;

    memset(&controls, 0, sizeof(controls));
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return ERROR_NOT_ENOUGH_MEMORY;
#ifdef OE_SETUP_PREVIEW
    error = installer_run_gui_for_state(
        instance,
        argc > 1 && _wcsicmp(argv[1], L"maintenance") == 0);
    LocalFree(argv);
    return (int)error;
#else
    if (argc == 1) {
        LocalFree(argv);
        return (int)installer_run_gui(instance);
    }
    if (_wcsicmp(argv[1], L"install") == 0 ||
        _wcsicmp(argv[1], L"update") == 0)
        error = installer_install(0);
    else if (_wcsicmp(argv[1], L"uninstall") == 0)
        error = installer_uninstall();
    else if (_wcsicmp(argv[1], L"start") == 0)
        error = installer_control_service(1);
    else if (_wcsicmp(argv[1], L"stop") == 0)
        error = installer_control_service(0);
    else
        error = ERROR_INVALID_PARAMETER;
    LocalFree(argv);
    return (int)error;
#endif
}
