#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "installer_resources.h"

static int compare_resource_to_file(HMODULE module, WORD resource_id,
                                    const wchar_t *path)
{
    HRSRC resource;
    HGLOBAL loaded;
    const void *resource_data;
    DWORD resource_size;
    HANDLE file;
    LARGE_INTEGER file_size;
    unsigned char *file_data;
    DWORD offset = 0;
    int equal = 0;

    resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource)
        return 0;
    loaded = LoadResource(module, resource);
    if (!loaded)
        return 0;
    resource_data = LockResource(loaded);
    resource_size = SizeofResource(module, resource);
    if (!resource_data || resource_size == 0)
        return 0;

    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart != resource_size) {
        CloseHandle(file);
        return 0;
    }
    file_data = (unsigned char *)malloc(resource_size);
    if (!file_data) {
        CloseHandle(file);
        return 0;
    }
    while (offset < resource_size) {
        DWORD read = 0;
        if (!ReadFile(file, file_data + offset, resource_size - offset,
                      &read, NULL) || read == 0)
            goto done;
        offset += read;
    }
    equal = memcmp(resource_data, file_data, resource_size) == 0;

done:
    free(file_data);
    CloseHandle(file);
    return equal;
}

int wmain(int argc, wchar_t **argv)
{
    HMODULE module;
    int ok;

    if (argc != 5) {
        fprintf(stderr,
                "Usage: installer_resources_tests.exe Setup.exe Service.exe CLI.exe GUI.exe\n");
        return 2;
    }
    module = LoadLibraryExW(argv[1], NULL,
                            LOAD_LIBRARY_AS_DATAFILE |
                            LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) {
        printf("installer_resources=FAIL\n");
        return 1;
    }
    ok = compare_resource_to_file(module, IDR_PAYLOAD_SERVICE, argv[2]) &&
         compare_resource_to_file(module, IDR_PAYLOAD_CLI, argv[3]) &&
         compare_resource_to_file(module, IDR_PAYLOAD_GUI, argv[4]);
    FreeLibrary(module);
    printf("installer_resources=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
