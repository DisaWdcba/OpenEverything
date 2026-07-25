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
    app->show_folders = config_read_int(path, L"View", L"Folders", 0) != 0;
    app->show_filters = config_read_int(path, L"View", L"Filters", 0) != 0;
    app->theme_mode = config_read_int(path, L"View", L"Theme", THEME_SYSTEM);
    app->sidebar_width = config_read_int(path, L"View", L"SidebarWidth", 280);
    app->sidebar_split_percent = config_read_int(path, L"View", L"SidebarSplit", 65);
    app->folder_panel_width = config_read_int(path, L"View", L"FoldersWidth",
                                              app->sidebar_width);
    app->filter_panel_width = config_read_int(path, L"View", L"FiltersWidth", 260);
    app->folder_panel_side = config_read_int(path, L"View", L"FoldersSide",
                                             PANEL_DOCK_LEFT);
    app->filter_panel_side = config_read_int(path, L"View", L"FiltersSide",
                                             PANEL_DOCK_RIGHT);
    app->selected_filter = config_read_int(path, L"View", L"Filter", FILTER_EVERYTHING);
    app->include_subfolders = config_read_int(path, L"View", L"Subfolders", 1) != 0;
    app->folder_scope[0] = L'\0';
    if (app->column_width_name < 40) app->column_width_name = 380;
    if (app->column_width_path < 40) app->column_width_path = 520;
    if (app->column_width_size < 40) app->column_width_size = 96;
    if (app->column_width_modified < 40) app->column_width_modified = 150;
    if (app->theme_mode < THEME_SYSTEM || app->theme_mode > THEME_DARK)
        app->theme_mode = THEME_SYSTEM;
    if (app->sidebar_width < 180 || app->sidebar_width > 600)
        app->sidebar_width = 280;
    if (app->sidebar_split_percent < 30 || app->sidebar_split_percent > 80)
        app->sidebar_split_percent = 65;
    if (app->folder_panel_width < 160 || app->folder_panel_width > 600)
        app->folder_panel_width = 280;
    if (app->filter_panel_width < 160 || app->filter_panel_width > 600)
        app->filter_panel_width = 260;
    if (app->folder_panel_side < PANEL_DOCK_LEFT ||
        app->folder_panel_side > PANEL_DOCK_RIGHT)
        app->folder_panel_side = PANEL_DOCK_LEFT;
    if (app->filter_panel_side < PANEL_DOCK_LEFT ||
        app->filter_panel_side > PANEL_DOCK_RIGHT)
        app->filter_panel_side = PANEL_DOCK_RIGHT;
    if (app->folder_panel_side == app->filter_panel_side)
        app->filter_panel_side = app->folder_panel_side == PANEL_DOCK_LEFT
            ? PANEL_DOCK_RIGHT : PANEL_DOCK_LEFT;
    if (app->selected_filter < FILTER_EVERYTHING || app->selected_filter >= FILTER_COUNT)
        app->selected_filter = FILTER_EVERYTHING;
    
    /* Apply to query */
    app->query.match_case = app->match_case;
    app->query.match_whole_word = app->match_whole_word;
    app->query.match_path = app->match_path;
    app->query.use_regex = app->use_regex;
    app->query.filter_id = app->selected_filter;
    app->query.include_subfolders = app->include_subfolders;
    app->query.folder_scope[0] = L'\0';
    app->query.sort_column = COL_NAME;
    app->query.sort_ascending = 1;
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
    config_write_int(path, L"View", L"Folders", app->show_folders);
    config_write_int(path, L"View", L"Filters", app->show_filters);
    config_write_int(path, L"View", L"Theme", app->theme_mode);
    config_write_int(path, L"View", L"SidebarWidth", app->sidebar_width);
    config_write_int(path, L"View", L"SidebarSplit", app->sidebar_split_percent);
    config_write_int(path, L"View", L"FoldersWidth", app->folder_panel_width);
    config_write_int(path, L"View", L"FiltersWidth", app->filter_panel_width);
    config_write_int(path, L"View", L"FoldersSide", app->folder_panel_side);
    config_write_int(path, L"View", L"FiltersSide", app->filter_panel_side);
    config_write_int(path, L"View", L"Filter", app->selected_filter);
    config_write_int(path, L"View", L"Subfolders", app->include_subfolders);
}
