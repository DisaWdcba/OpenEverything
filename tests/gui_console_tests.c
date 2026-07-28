#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void report_result(HANDLE report, const char *message)
{
    DWORD written;
    if (report && report != INVALID_HANDLE_VALUE)
        WriteFile(report, message, (DWORD)strlen(message), &written, NULL);
}

static void report_number(HANDLE report, const char *name, DWORD value)
{
    char message[128];
    _snprintf_s(message, sizeof(message), _TRUNCATE, "%s=%lu\n", name, value);
    report_result(report, message);
}

static void report_console_rows(HANDLE report, const wchar_t *screen,
                                SHORT width, SHORT height)
{
    char utf8[4096];

    for (SHORT row = 0; row < height; row++) {
        const wchar_t *line = screen + (size_t)row * (size_t)width;
        int start = 0;
        int end = width;
        int length;

        while (start < end && line[start] == L' ')
            start++;
        while (end > start && line[end - 1] == L' ')
            end--;
        if (start == end)
            continue;
        length = WideCharToMultiByte(CP_UTF8, 0, line + start, end - start,
                                     utf8, (int)sizeof(utf8) - 1, NULL, NULL);
        if (length > 0) {
            utf8[length] = '\0';
            report_result(report, "console: ");
            report_result(report, utf8);
            report_result(report, "\n");
        }
    }
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE report = INVALID_HANDLE_VALUE;
    HANDLE console_input = INVALID_HANDLE_VALUE;
    HANDLE console_output = INVALID_HANDLE_VALUE;
    HANDLE current = GetCurrentProcess();
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    CONSOLE_SCREEN_BUFFER_INFO info;
    SECURITY_ATTRIBUTES security;
    COORD buffer_size = { 240, 200 };
    COORD origin = { 0, 0 };
    wchar_t command[32768];
    wchar_t *screen = NULL;
    DWORD cells;
    DWORD read = 0;
    DWORD exit_code = 1;
    int ok = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: gui_console_tests.exe OpenEverything.exe\n");
        return 2;
    }

    DuplicateHandle(current, GetStdHandle(STD_OUTPUT_HANDLE), current,
                    &report, 0, FALSE, DUPLICATE_SAME_ACCESS);
    FreeConsole();
    if (!AllocConsole())
        goto cleanup;
    if (GetConsoleWindow())
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    SetConsoleCP(936);
    SetConsoleOutputCP(936);

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    console_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                OPEN_EXISTING, 0, NULL);
    console_output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 OPEN_EXISTING, 0, NULL);
    if (console_input == INVALID_HANDLE_VALUE ||
        console_output == INVALID_HANDLE_VALUE)
        goto cleanup;
    SetConsoleScreenBufferSize(console_output, buffer_size);

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = console_input;
    startup.hStdOutput = console_output;
    startup.hStdError = console_output;
    _snwprintf_s(command, _countof(command), _TRUNCATE,
                 L"\"%s\" search \"7.10通知.pdf\" --limit 1", argv[1]);

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL,
                        &startup, &process))
        goto cleanup;
    if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        goto cleanup;
    }
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != 0 ||
        !GetConsoleScreenBufferInfo(console_output, &info))
        goto cleanup;

    cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
    screen = (wchar_t *)calloc((size_t)cells + 1, sizeof(*screen));
    if (!screen ||
        !ReadConsoleOutputCharacterW(console_output, screen, cells,
                                     origin, &read))
        goto cleanup;
    screen[read] = L'\0';
    ok = wcsstr(screen, L"7.10通知.pdf") != NULL &&
         wcsstr(screen, L"7.10閫氱煡.pdf") == NULL;
    if (!ok) {
        report_number(report, "child_exit", exit_code);
        report_console_rows(report, screen, info.dwSize.X, info.dwSize.Y);
    }

cleanup:
    free(screen);
    if (console_input != INVALID_HANDLE_VALUE)
        CloseHandle(console_input);
    if (console_output != INVALID_HANDLE_VALUE)
        CloseHandle(console_output);
    FreeConsole();
    report_result(report, ok ? "gui_console_unicode=PASS\n"
                             : "gui_console_unicode=FAIL\n");
    if (report != INVALID_HANDLE_VALUE)
        CloseHandle(report);
    return ok ? 0 : 1;
}
