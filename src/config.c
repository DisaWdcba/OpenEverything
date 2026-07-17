#include "config.h"

void config_get_path(wchar_t *buf, size_t size)
{
    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
        swprintf_s(buf, size, L"%s\\OpenEverything\\config.ini", appdata);
    } else {
        wcscpy_s(buf, size, L"config.ini");
    }
}

static int config_read_int(const wchar_t *path, const wchar_t *section,
                            const wchar_t *key, int default_val)
{
    return GetPrivateProfileIntW(section, key, default_val, path);
}

static void config_write_int(const wchar_t *path, const wchar_t *section,
                              const wchar_t *key, int val)
{
    wchar_t buf[32];
    swprintf_s(buf, 32, L"%d", val);
    WritePrivateProfileStringW(section, key, buf, path);
}

void config_load(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    config_get_path(path, MAX_PATH);
    
    /* Create directory if needed */
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, path);
    wchar_t *last = wcsrchr(dir, L'\\');
    if (last) {
        *last = L'\0';
        SHCreateDirectoryExW(NULL, dir, NULL);
    }
    
    app->match_case = config_read_int(path, L"Search", L"MatchCase", 0);
    app->match_whole_word = config_read_int(path, L"Search", L"MatchWholeWord", 0);
    app->match_path = config_read_int(path, L"Search", L"MatchPath", 0);
    app->use_regex = config_read_int(path, L"Search", L"UseRegex", 0);
    app->close_to_tray = config_read_int(path, L"UI", L"CloseToTray", 0);
    app->minimize_to_tray = config_read_int(path, L"UI", L"MinimizeToTray", 0);
    app->column_width_name = config_read_int(path, L"Columns", L"Name", 380);
    app->column_width_path = config_read_int(path, L"Columns", L"Path", 520);
    app->column_width_size = config_read_int(path, L"Columns", L"Size", 96);
    app->column_width_modified = config_read_int(path, L"Columns", L"DateModified", 150);
    if (app->column_width_name < 40) app->column_width_name = 380;
    if (app->column_width_path < 40) app->column_width_path = 520;
    if (app->column_width_size < 40) app->column_width_size = 96;
    if (app->column_width_modified < 40) app->column_width_modified = 150;
    
    /* Apply to query */
    app->query.match_case = app->match_case;
    app->query.match_whole_word = app->match_whole_word;
    app->query.match_path = app->match_path;
    app->query.use_regex = app->use_regex;
}

void config_save(APP_STATE *app)
{
    wchar_t path[MAX_PATH];
    config_get_path(path, MAX_PATH);
    
    config_write_int(path, L"Search", L"MatchCase", app->match_case);
    config_write_int(path, L"Search", L"MatchWholeWord", app->match_whole_word);
    config_write_int(path, L"Search", L"MatchPath", app->match_path);
    config_write_int(path, L"Search", L"UseRegex", app->use_regex);
    config_write_int(path, L"UI", L"CloseToTray", app->close_to_tray);
    config_write_int(path, L"UI", L"MinimizeToTray", app->minimize_to_tray);
    config_write_int(path, L"Columns", L"Name", app->column_width_name);
    config_write_int(path, L"Columns", L"Path", app->column_width_path);
    config_write_int(path, L"Columns", L"Size", app->column_width_size);
    config_write_int(path, L"Columns", L"DateModified", app->column_width_modified);
}
