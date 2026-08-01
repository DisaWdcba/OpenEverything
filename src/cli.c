#include "cache.h"
#include "cli.h"
#include "index.h"
#include "json.h"
#include "search.h"
#include "service_client.h"
#include "version.h"

#include <fcntl.h>
#include <io.h>
#include <stdint.h>

#define CLI_VERSION OE_VERSION_STRING
#define MCP_MAX_MESSAGE (4U * 1024U * 1024U)
#define MCP_DEFAULT_LIMIT 50
#define MCP_MAX_RESULTS 1000

typedef struct {
    APP_STATE app;
    wchar_t *index_path;
    int initialized;
    int loaded;
    int cache_result;
    int service_available;
    int using_service;
    OE_SERVICE_RESPONSE service_status;
} SEARCH_RUNTIME;

typedef struct {
    SEARCH_QUERY query;
    int limit;
} SEARCH_REQUEST;

typedef struct {
    char *data;
    size_t length;
    int content_length_framing;
} MCP_MESSAGE;

static void runtime_destroy(SEARCH_RUNTIME *runtime);
static const wchar_t *g_cli_program_name = L"OpenEverythingCLI.exe";
static int g_cli_attached_parent_console;

static const wchar_t *filter_name(int filter)
{
    static const wchar_t *names[FILTER_COUNT] = {
        L"everything", L"audio", L"compressed", L"document",
        L"executable", L"folder", L"image", L"video"
    };
    return filter >= 0 && filter < FILTER_COUNT ? names[filter] : L"everything";
}

static int parse_filter(const wchar_t *text, int *filter)
{
    if (!text || !filter)
        return 0;
    for (int i = FILTER_EVERYTHING; i < FILTER_COUNT; i++) {
        if (_wcsicmp(text, filter_name(i)) == 0) {
            *filter = i;
            return 1;
        }
    }
    return 0;
}

static int parse_sort_column(const wchar_t *text, int *column)
{
    static const struct {
        const wchar_t *name;
        int column;
    } values[] = {
        { L"name", COL_NAME }, { L"path", COL_PATH },
        { L"size", COL_SIZE }, { L"modified", COL_DATE_MODIFIED },
        { L"created", COL_DATE_CREATED }, { L"attributes", COL_ATTRIBUTES }
    };

    if (!text || !column)
        return 0;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (_wcsicmp(text, values[i].name) == 0) {
            *column = values[i].column;
            return 1;
        }
    }
    return 0;
}

static void search_request_init(SEARCH_REQUEST *request, int limit)
{
    memset(request, 0, sizeof(*request));
    request->limit = limit;
    request->query.filter_id = FILTER_EVERYTHING;
    request->query.include_subfolders = 1;
    request->query.sort_column = COL_NAME;
    request->query.sort_ascending = 1;
}

static int runtime_load(SEARCH_RUNTIME *runtime)
{
    int loaded;

    if (!runtime || !runtime->initialized)
        return 0;
    loaded = runtime->index_path
        ? cache_load_index_from_path(&runtime->app, runtime->index_path)
        : cache_load_index(&runtime->app);
    if (loaded == CACHE_LOAD_FAILED)
        return 0;

    runtime->cache_result = loaded;
    runtime->loaded = 1;
    /* These are accelerators. Searches remain correct with their linear
       fallbacks if memory pressure prevents one from being built. */
    index_build_ref_index(&runtime->app);
    index_build_filter_index(&runtime->app);
    index_build_name_char_index(&runtime->app);
    return 1;
}

static int runtime_init(SEARCH_RUNTIME *runtime, const wchar_t *index_path)
{
    wchar_t service_path[MAX_PATH];

    memset(runtime, 0, sizeof(*runtime));
    index_init(&runtime->app);
    runtime->initialized = 1;
    if (!runtime->app.entries || !runtime->app.filtered_indices) {
        runtime_destroy(runtime);
        return 0;
    }
    if (index_path) {
        runtime->index_path = _wcsdup(index_path);
        if (!runtime->index_path) {
            runtime_destroy(runtime);
            return 0;
        }
        return runtime_load(runtime);
    }

    if (oe_service_get_status(&runtime->service_status)) {
        runtime->service_available = 1;
        oe_service_get_index_path(service_path, MAX_PATH);
        runtime->index_path = _wcsdup(service_path);
        if (!runtime->index_path) {
            runtime_destroy(runtime);
            return 0;
        }
        runtime->using_service = 1;
        if (runtime_load(runtime))
            return 1;
        free(runtime->index_path);
        runtime->index_path = NULL;
        runtime->using_service = 0;
    }
    return runtime_load(runtime);
}

static int runtime_refresh_from_service(SEARCH_RUNTIME *runtime,
                                        DWORD timeout_ms)
{
    OE_SERVICE_RESPONSE before;
    OE_SERVICE_RESPONSE after;
    wchar_t service_path[MAX_PATH];

    if (!runtime)
        return 0;
    if (!oe_service_get_status(&before)) {
        runtime->service_available = 0;
        return 0;
    }
    runtime->service_available = 1;
    runtime->service_status = before;
    if (!oe_service_request_refresh(&after))
        return 0;
    if (!oe_service_wait_for_update(before.update_sequence, timeout_ms,
                                    &after)) {
        runtime->service_status = after;
        return 0;
    }
    runtime->service_status = after;

    if (!runtime->using_service) {
        oe_service_get_index_path(service_path, MAX_PATH);
        free(runtime->index_path);
        runtime->index_path = _wcsdup(service_path);
        if (!runtime->index_path)
            return 0;
        runtime->using_service = 1;
    }
    return runtime_load(runtime);
}

static void runtime_destroy(SEARCH_RUNTIME *runtime)
{
    if (!runtime)
        return;
    if (runtime->initialized) {
        index_clear(&runtime->app);
        free(runtime->app.entries);
        free(runtime->app.filtered_indices);
        runtime->app.entries = NULL;
        runtime->app.filtered_indices = NULL;
        DeleteCriticalSection(&runtime->app.index_lock);
    }
    free(runtime->index_path);
    memset(runtime, 0, sizeof(*runtime));
}

static void runtime_index_path(const SEARCH_RUNTIME *runtime,
                               wchar_t *path, size_t path_size)
{
    if (runtime && runtime->index_path)
        wcsncpy_s(path, path_size, runtime->index_path, _TRUNCATE);
    else
        cache_get_index_path(path, path_size);
}

static long long filetime_to_unix(long long filetime)
{
    const long long epoch = 116444736000000000LL;
    if (filetime <= epoch)
        return 0;
    return (filetime - epoch) / 10000000LL;
}

static int execute_search(SEARCH_RUNTIME *runtime, SEARCH_REQUEST *request,
                          int **indices, int *count)
{
    int *result;

    if (!runtime || !runtime->loaded || !request || !indices || !count ||
        request->limit <= 0)
        return 0;
    result = (int *)malloc((size_t)request->limit * sizeof(*result));
    if (!result)
        return 0;
    search_prepare_query(&request->query);
    *count = search_execute_to_buffer(&runtime->app, &request->query,
                                      result, request->limit);
    *indices = result;
    return 1;
}

static int append_entry_json(JSON_BUFFER *buffer, SEARCH_RUNTIME *runtime,
                             int entry_index)
{
    INDEX_ENTRY *entry = &runtime->app.entries[entry_index];
    wchar_t *name = index_duplicate_entry_name_locked(&runtime->app, entry);
    wchar_t *path = index_duplicate_entry_path_locked(&runtime->app, entry_index);
    long long modified = filetime_to_unix(entry->modification_time);

    if (!json_buffer_append(buffer, "{\"name\":")) goto failed;
    if (!json_buffer_append_wstring(buffer, name ? name : L"")) goto failed;
    if (!json_buffer_append(buffer, ",\"path\":")) goto failed;
    if (!json_buffer_append_wstring(buffer, path ? path : L"")) goto failed;
    if (!json_buffer_append_format(buffer,
            ",\"size\":%lld,\"modified\":%lld,\"directory\":%s,\"attributes\":%u}",
            entry->size, modified, entry->is_directory ? "true" : "false",
            entry->attributes)) goto failed;
    free(name);
    free(path);
    return 1;

failed:
    free(name);
    free(path);
    return 0;
}

static int append_search_json(JSON_BUFFER *buffer, SEARCH_RUNTIME *runtime,
                              const SEARCH_REQUEST *request,
                              const int *indices, int count)
{
    if (!json_buffer_append(buffer, "{\"query\":"))
        return 0;
    if (!json_buffer_append_wstring(buffer, request->query.text) ||
        !json_buffer_append_format(buffer, ",\"count\":%d,\"results\":[", count))
        return 0;

    EnterCriticalSection(&runtime->app.index_lock);
    for (int i = 0; i < count; i++) {
        if (i > 0 && !json_buffer_append_char(buffer, ','))
            break;
        if (indices[i] < 0 || indices[i] >= runtime->app.entry_count ||
            !append_entry_json(buffer, runtime, indices[i]))
            break;
    }
    LeaveCriticalSection(&runtime->app.index_lock);
    return !buffer->failed && json_buffer_append(buffer, "]}");
}

static int append_stats_json(JSON_BUFFER *buffer, SEARCH_RUNTIME *runtime)
{
    wchar_t path[MAX_PATH];
    const char *source;
    const char *cache_format = runtime->cache_result == CACHE_LOAD_CURRENT
        ? "v5"
        : runtime->cache_result == CACHE_LOAD_LEGACY ? "legacy" : "unavailable";

    if (runtime->service_available)
        oe_service_get_status(&runtime->service_status);
    source = runtime->using_service
        ? "service"
        : runtime->index_path ? "custom" : "local";
    runtime_index_path(runtime, path, MAX_PATH);
    if (!json_buffer_append(buffer, "{\"loaded\":")) return 0;
    if (!json_buffer_append(buffer, runtime->loaded ? "true" : "false")) return 0;
    if (!json_buffer_append_format(buffer,
            ",\"entries\":%d,\"volumes\":%d,\"indexedVolumes\":%d,\"indexErrors\":%d,\"cacheFormat\":\"%s\",\"indexPath\":" ,
            runtime->app.entry_count, runtime->app.volume_count,
            runtime->app.indexed_volume_count, runtime->app.index_error_count,
            cache_format)) return 0;
    if (!json_buffer_append_wstring(buffer, path)) return 0;
    if (!json_buffer_append_format(
            buffer, ",\"source\":\"%s\",\"serviceAvailable\":%s",
            source, runtime->service_available ? "true" : "false")) return 0;
    if (runtime->service_available) {
        if (!json_buffer_append(buffer, ",\"serviceState\":")) return 0;
        if (!json_buffer_append_wstring(
                buffer, oe_service_state_name(runtime->service_status.state)))
            return 0;
        if (!json_buffer_append_format(
                buffer,
                ",\"serviceUpdateSequence\":%u,\"serviceLastError\":%u",
                runtime->service_status.update_sequence,
                runtime->service_status.last_error))
            return 0;
    }
    return json_buffer_append_char(buffer, '}');
}

static int write_bytes(FILE *stream, const char *data, size_t length)
{
    intptr_t os_handle;
    HANDLE console;
    DWORD mode;
    int is_console = 0;

    if (length == 0)
        return 1;

    if (stream == stdout)
        console = GetStdHandle(STD_OUTPUT_HANDLE);
    else if (stream == stderr)
        console = GetStdHandle(STD_ERROR_HANDLE);
    else
        console = INVALID_HANDLE_VALUE;
    if (console && console != INVALID_HANDLE_VALUE)
        is_console = GetConsoleMode(console, &mode) != 0;
    if (!is_console) {
        os_handle = _get_osfhandle(_fileno(stream));
        console = os_handle == -1 || os_handle == -2
            ? INVALID_HANDLE_VALUE : (HANDLE)os_handle;
        if (console != INVALID_HANDLE_VALUE)
            is_console = GetConsoleMode(console, &mode) != 0;
    }
    if (length <= INT_MAX && is_console) {
        wchar_t *wide;
        int wide_length;
        DWORD written = 0;

        if (g_cli_attached_parent_console) {
            if (!WriteConsoleW(console, L"\r\n", 2, &written, NULL) ||
                written != 2)
                return 0;
            g_cli_attached_parent_console = 0;
        }

        wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          data, (int)length, NULL, 0);
        if (wide_length <= 0)
            wide_length = MultiByteToWideChar(CP_UTF8, 0, data, (int)length,
                                              NULL, 0);
        if (wide_length <= 0)
            return 0;
        wide = (wchar_t *)malloc((size_t)wide_length * sizeof(*wide));
        if (!wide)
            return 0;
        MultiByteToWideChar(CP_UTF8, 0, data, (int)length, wide, wide_length);
        fflush(stream);
        if (!WriteConsoleW(console, wide, (DWORD)wide_length, &written, NULL) ||
            written != (DWORD)wide_length) {
            free(wide);
            return 0;
        }
        free(wide);
        return 1;
    }

    return fwrite(data, 1, length, stream) == length;
}

static int write_wstring_utf8(FILE *stream, const wchar_t *text, int newline)
{
    int length;
    char *utf8;
    int ok;

    if (!text)
        text = L"";
    length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (length <= 0)
        return 0;
    utf8 = (char *)malloc((size_t)length);
    if (!utf8)
        return 0;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, length, NULL, NULL);
    ok = write_bytes(stream, utf8, (size_t)length - 1);
    if (ok && newline)
        ok = write_bytes(stream, "\n", 1);
    free(utf8);
    return ok;
}

static void write_usage_command(const char *arguments)
{
    write_bytes(stdout, "  ", 2);
    write_wstring_utf8(stdout, g_cli_program_name, 0);
    write_bytes(stdout, arguments, strlen(arguments));
}

static void print_usage(void)
{
    static const char heading[] =
        "OpenEverything CLI/MCP " CLI_VERSION "\n\nUsage:\n";
    static const char options[] =
        "\n"
        "Search options:\n"
        "  --limit N                 Maximum results (default 50)\n"
        "  --json                    Emit one JSON document\n"
        "  --match-path              Match against the complete path\n"
        "  --case-sensitive          Enable case-sensitive matching\n"
        "  --whole-word              Match a complete word\n"
        "  --folder PATH             Restrict results to a folder\n"
        "  --no-subfolders           Exclude nested folders\n"
        "  --filter TYPE             everything, audio, compressed, document,\n"
        "                            executable, folder, image, or video\n"
        "  --sort FIELD              name, path, size, modified, created, attributes\n"
        "  --descending              Reverse the sort order\n"
        "  --index FILE              Read a specific index.dat\n";

    write_bytes(stdout, heading, sizeof(heading) - 1);
    write_usage_command(" search [QUERY] [options]\n");
    write_usage_command(" stats [--json] [--index FILE]\n");
    write_usage_command(" update [--timeout SECONDS] [--json]\n");
    write_usage_command(" mcp [--index FILE]\n");
    write_usage_command(" --mcp [--index FILE]\n");
    write_bytes(stdout, options, sizeof(options) - 1);
}

static int parse_positive_int(const wchar_t *text, int maximum, int *value)
{
    wchar_t *end = NULL;
    long parsed;

    if (!text || !text[0])
        return 0;
    parsed = wcstol(text, &end, 10);
    if (!end || *end || parsed <= 0 || parsed > maximum)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int append_query_word(wchar_t *query, size_t capacity, const wchar_t *word)
{
    size_t current = wcslen(query);
    size_t length = wcslen(word);
    size_t separator = current > 0 ? 1 : 0;

    if (current + separator + length >= capacity)
        return 0;
    if (separator)
        query[current++] = L' ';
    memcpy(query + current, word, (length + 1) * sizeof(*query));
    return 1;
}

static int parse_search_args(int argc, wchar_t **argv, int start,
                             SEARCH_REQUEST *request, int *json_output,
                             const wchar_t **index_path)
{
    for (int i = start; i < argc; i++) {
        const wchar_t *arg = argv[i];
        if (wcscmp(arg, L"--json") == 0) {
            *json_output = 1;
        } else if (wcscmp(arg, L"--match-path") == 0) {
            request->query.match_path = 1;
        } else if (wcscmp(arg, L"--case-sensitive") == 0) {
            request->query.match_case = 1;
        } else if (wcscmp(arg, L"--whole-word") == 0) {
            request->query.match_whole_word = 1;
        } else if (wcscmp(arg, L"--no-subfolders") == 0) {
            request->query.include_subfolders = 0;
        } else if (wcscmp(arg, L"--descending") == 0) {
            request->query.sort_ascending = 0;
        } else if (wcscmp(arg, L"--limit") == 0) {
            if (++i >= argc || !parse_positive_int(argv[i], SEARCH_MAX_RESULTS,
                                                    &request->limit)) {
                fprintf(stderr, "Invalid --limit value.\n");
                return 0;
            }
        } else if (wcscmp(arg, L"--folder") == 0) {
            if (++i >= argc || wcslen(argv[i]) >= SEARCH_FOLDER_SCOPE_MAX) {
                fprintf(stderr, "Invalid --folder value.\n");
                return 0;
            }
            wcscpy_s(request->query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, argv[i]);
        } else if (wcscmp(arg, L"--filter") == 0) {
            if (++i >= argc || !parse_filter(argv[i], &request->query.filter_id)) {
                fprintf(stderr, "Invalid --filter value.\n");
                return 0;
            }
        } else if (wcscmp(arg, L"--sort") == 0) {
            if (++i >= argc || !parse_sort_column(argv[i], &request->query.sort_column)) {
                fprintf(stderr, "Invalid --sort value.\n");
                return 0;
            }
        } else if (wcscmp(arg, L"--index") == 0) {
            if (++i >= argc || !argv[i][0]) {
                fprintf(stderr, "Invalid --index value.\n");
                return 0;
            }
            *index_path = argv[i];
        } else if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
            return 2;
        } else if (wcsncmp(arg, L"--", 2) == 0) {
            fprintf(stderr, "Unknown search option.\n");
            return 0;
        } else if (!append_query_word(request->query.text, 512, arg)) {
            fprintf(stderr, "Query is longer than 511 characters.\n");
            return 0;
        }
    }
    return 1;
}

static int run_cli_search(int argc, wchar_t **argv)
{
    SEARCH_RUNTIME runtime;
    SEARCH_REQUEST request;
    const wchar_t *index_path = NULL;
    int json_output = 0;
    int *indices = NULL;
    int count = 0;
    int parsed;
    int exit_code = 1;

    search_request_init(&request, 50);
    parsed = parse_search_args(argc, argv, 2, &request, &json_output, &index_path);
    if (parsed == 2) {
        print_usage();
        return 0;
    }
    if (!parsed)
        return 2;
    if (!runtime_init(&runtime, index_path)) {
        wchar_t path[MAX_PATH];
        runtime_index_path(&runtime, path, MAX_PATH);
        fprintf(stderr, "Unable to load the OpenEverything index: ");
        write_wstring_utf8(stderr, path, 1);
        runtime_destroy(&runtime);
        return 1;
    }
    if (!execute_search(&runtime, &request, &indices, &count)) {
        fprintf(stderr, "Search failed.\n");
        goto done;
    }

    if (json_output) {
        JSON_BUFFER output;
        json_buffer_init(&output);
        if (!append_search_json(&output, &runtime, &request, indices, count) ||
            !write_bytes(stdout, output.data, output.length) ||
            !write_bytes(stdout, "\n", 1)) {
            fprintf(stderr, "Unable to write search results.\n");
            json_buffer_free(&output);
            goto done;
        }
        json_buffer_free(&output);
    } else {
        EnterCriticalSection(&runtime.app.index_lock);
        for (int i = 0; i < count; i++) {
            wchar_t *path = index_duplicate_entry_path_locked(&runtime.app, indices[i]);
            if (!write_wstring_utf8(stdout, path ? path : L"", 1)) {
                free(path);
                LeaveCriticalSection(&runtime.app.index_lock);
                fprintf(stderr, "Unable to write search results.\n");
                goto done;
            }
            free(path);
        }
        LeaveCriticalSection(&runtime.app.index_lock);
    }
    exit_code = 0;

done:
    free(indices);
    runtime_destroy(&runtime);
    return exit_code;
}

static int run_cli_stats(int argc, wchar_t **argv)
{
    SEARCH_RUNTIME runtime;
    const wchar_t *index_path = NULL;
    int json_output = 0;

    for (int i = 2; i < argc; i++) {
        if (wcscmp(argv[i], L"--json") == 0) {
            json_output = 1;
        } else if (wcscmp(argv[i], L"--index") == 0 && i + 1 < argc) {
            index_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown stats option.\n");
            return 2;
        }
    }
    if (!runtime_init(&runtime, index_path)) {
        fprintf(stderr, "Unable to load the OpenEverything index.\n");
        runtime_destroy(&runtime);
        return 1;
    }
    if (json_output) {
        JSON_BUFFER output;
        json_buffer_init(&output);
        append_stats_json(&output, &runtime);
        write_bytes(stdout, output.data, output.length);
        write_bytes(stdout, "\n", 1);
        json_buffer_free(&output);
    } else {
        wchar_t path[MAX_PATH];
        const char *source = runtime.using_service
            ? "service"
            : runtime.index_path ? "custom" : "local";
        const char *cache_format = runtime.cache_result == CACHE_LOAD_CURRENT
            ? "v5"
            : runtime.cache_result == CACHE_LOAD_LEGACY ? "legacy" : "unavailable";
        if (runtime.service_available)
            oe_service_get_status(&runtime.service_status);
        runtime_index_path(&runtime, path, MAX_PATH);
        printf("Entries: %d\nVolumes: %d\nIndexed volumes: %d\n"
               "Cache format: %s\nSource: %s\nIndex: ",
               runtime.app.entry_count, runtime.app.volume_count,
               runtime.app.indexed_volume_count, cache_format, source);
        write_wstring_utf8(stdout, path, 1);
        if (runtime.service_available) {
            printf("Service: ");
            write_wstring_utf8(stdout,
                oe_service_state_name(runtime.service_status.state), 1);
            printf("Service update sequence: %u\nService last error: %u\n",
                   runtime.service_status.update_sequence,
                   runtime.service_status.last_error);
        }
    }
    runtime_destroy(&runtime);
    return 0;
}

static int run_cli_update(int argc, wchar_t **argv)
{
    OE_SERVICE_RESPONSE before;
    OE_SERVICE_RESPONSE accepted;
    OE_SERVICE_RESPONSE after;
    DWORD timeout_ms = 60000;
    int json_output = 0;

    for (int i = 2; i < argc; i++) {
        int seconds;
        if (wcscmp(argv[i], L"--json") == 0) {
            json_output = 1;
        } else if (wcscmp(argv[i], L"--timeout") == 0 &&
                   i + 1 < argc &&
                   parse_positive_int(argv[++i], 600, &seconds)) {
            timeout_ms = (DWORD)seconds * 1000;
        } else {
            fprintf(stderr, "Unknown or invalid update option.\n");
            return 2;
        }
    }
    if (!oe_service_get_status(&before)) {
        fprintf(stderr,
                "OpenEverythingService is not running. Install it with "
                "OpenEverythingService.exe install.\n");
        return 1;
    }
    if (!oe_service_request_refresh(&accepted)) {
        fprintf(stderr, "The service rejected the update request.\n");
        return 1;
    }
    if (!oe_service_wait_for_update(before.update_sequence, timeout_ms, &after)) {
        fprintf(stderr, "Timed out waiting for the service update.\n");
        return 1;
    }

    if (json_output) {
        JSON_BUFFER output;
        json_buffer_init(&output);
        json_buffer_append(&output, "{\"updated\":true,\"state\":");
        json_buffer_append_wstring(&output,
                                   oe_service_state_name(after.state));
        json_buffer_append_format(
            &output,
            ",\"entries\":%d,\"volumes\":%d,\"updateSequence\":%u,"
            "\"lastError\":%u}\n",
            after.entry_count, after.volume_count,
            after.update_sequence, after.last_error);
        write_bytes(stdout, output.data, output.length);
        json_buffer_free(&output);
    } else {
        printf("Index update completed.\nEntries: %d\nUpdate sequence: %u\n",
               after.entry_count, after.update_sequence);
    }
    return 0;
}

static char *read_line(FILE *stream, size_t *length)
{
    JSON_BUFFER buffer;
    int ch;

    json_buffer_init(&buffer);
    while ((ch = fgetc(stream)) != EOF) {
        if (ch == '\n')
            break;
        if (!json_buffer_append_char(&buffer, (char)ch))
            break;
        if (buffer.length > MCP_MAX_MESSAGE) {
            buffer.failed = 1;
            break;
        }
    }
    if (ch == EOF && buffer.length == 0) {
        json_buffer_free(&buffer);
        return NULL;
    }
    if (buffer.failed) {
        json_buffer_free(&buffer);
        return NULL;
    }
    if (buffer.length > 0 && buffer.data[buffer.length - 1] == '\r')
        buffer.data[--buffer.length] = '\0';
    if (!buffer.data) {
        buffer.data = (char *)calloc(1, 1);
        if (!buffer.data)
            return NULL;
    }
    *length = buffer.length;
    return buffer.data;
}

static int header_content_length(const char *line, size_t *length)
{
    const char *prefix = "Content-Length:";
    char *end = NULL;
    unsigned long value;

    if (_strnicmp(line, prefix, strlen(prefix)) != 0)
        return 0;
    line += strlen(prefix);
    while (*line == ' ' || *line == '\t')
        line++;
    value = strtoul(line, &end, 10);
    while (end && (*end == ' ' || *end == '\t'))
        end++;
    if (!end || *end || value == 0 || value > MCP_MAX_MESSAGE)
        return -1;
    *length = (size_t)value;
    return 1;
}

static int mcp_read_message(MCP_MESSAGE *message)
{
    char *line;
    size_t line_length;
    size_t content_length = 0;
    int header_result;

    memset(message, 0, sizeof(*message));
    do {
        line = read_line(stdin, &line_length);
        if (!line)
            return 0;
        if (line_length == 0)
            free(line);
    } while (line_length == 0);

    header_result = header_content_length(line, &content_length);
    if (header_result < 0) {
        free(line);
        return -1;
    }
    if (header_result == 1) {
        free(line);
        for (;;) {
            line = read_line(stdin, &line_length);
            if (!line)
                return -1;
            free(line);
            if (line_length == 0)
                break;
        }
        message->data = (char *)malloc(content_length + 1);
        if (!message->data)
            return -1;
        if (fread(message->data, 1, content_length, stdin) != content_length) {
            free(message->data);
            memset(message, 0, sizeof(*message));
            return -1;
        }
        message->data[content_length] = '\0';
        message->length = content_length;
        message->content_length_framing = 1;
        return 1;
    }

    if (line_length >= 3 &&
        (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb &&
        (unsigned char)line[2] == 0xbf) {
        memmove(line, line + 3, line_length - 2);
        line_length -= 3;
    }
    message->data = line;
    message->length = line_length;
    return 1;
}

static int mcp_write_message(const JSON_BUFFER *response, int content_length_framing)
{
    if (!response || response->failed || !response->data)
        return 0;
    if (content_length_framing)
        fprintf(stdout, "Content-Length: %zu\r\n\r\n", response->length);
    if (!write_bytes(stdout, response->data, response->length))
        return 0;
    if (!content_length_framing && !write_bytes(stdout, "\n", 1))
        return 0;
    return fflush(stdout) == 0;
}

static int mcp_append_id(JSON_BUFFER *response, const char *json,
                         const JSON_TOKEN *tokens, int id_index)
{
    const JSON_TOKEN *id;

    if (id_index < 0)
        return json_buffer_append(response, "null");
    id = &tokens[id_index];
    if (id->type == JSON_STRING) {
        return json_buffer_append_char(response, '"') &&
               json_buffer_append_n(response, json + id->start,
                                    (size_t)(id->end - id->start)) &&
               json_buffer_append_char(response, '"');
    }
    if (id->type == JSON_PRIMITIVE)
        return json_buffer_append_n(response, json + id->start,
                                    (size_t)(id->end - id->start));
    return json_buffer_append(response, "null");
}

static int mcp_response_start(JSON_BUFFER *response, const char *json,
                              const JSON_TOKEN *tokens, int id_index)
{
    return json_buffer_append(response, "{\"jsonrpc\":\"2.0\",\"id\":") &&
           mcp_append_id(response, json, tokens, id_index);
}

static int mcp_error_response(JSON_BUFFER *response, const char *json,
                              const JSON_TOKEN *tokens, int id_index,
                              int code, const char *message)
{
    return mcp_response_start(response, json, tokens, id_index) &&
           json_buffer_append_format(response, ",\"error\":{\"code\":%d,\"message\":", code) &&
           json_buffer_append_quoted_utf8(response, message) &&
           json_buffer_append(response, "}}");
}

static int mcp_tool_error(JSON_BUFFER *response, const char *json,
                          const JSON_TOKEN *tokens, int id_index,
                          const char *message)
{
    return mcp_response_start(response, json, tokens, id_index) &&
           json_buffer_append(response, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":") &&
           json_buffer_append_quoted_utf8(response, message) &&
           json_buffer_append(response, "}],\"isError\":true}}");
}

static int mcp_tool_json_result(JSON_BUFFER *response, const char *json,
                                const JSON_TOKEN *tokens, int id_index,
                                const JSON_BUFFER *data)
{
    return mcp_response_start(response, json, tokens, id_index) &&
           json_buffer_append(response, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":") &&
           json_buffer_append_quoted_utf8(response, data->data) &&
           json_buffer_append(response, "}],\"structuredContent\":") &&
           json_buffer_append_n(response, data->data, data->length) &&
           json_buffer_append(response, "}}");
}

static int mcp_read_string_argument(const char *json, const JSON_TOKEN *tokens,
                                    int token_count, int arguments,
                                    const char *name, wchar_t **value)
{
    int index = json_object_get(json, tokens, token_count, arguments, name);
    if (index < 0)
        return 0;
    if (tokens[index].type != JSON_STRING)
        return -1;
    *value = json_token_to_wstring(json, &tokens[index]);
    return *value ? 1 : -1;
}

static int mcp_read_bool_argument(const char *json, const JSON_TOKEN *tokens,
                                  int token_count, int arguments,
                                  const char *name, int *value)
{
    int index = json_object_get(json, tokens, token_count, arguments, name);
    if (index < 0)
        return 0;
    return json_token_to_bool(json, &tokens[index], value) ? 1 : -1;
}

static int mcp_parse_search_request(const char *json, const JSON_TOKEN *tokens,
                                    int token_count, int arguments,
                                    SEARCH_REQUEST *request, const char **error)
{
    wchar_t *text = NULL;
    int result;
    int value = 0;
    int index;

    search_request_init(request, MCP_DEFAULT_LIMIT);
    if (arguments < 0)
        return 1;
    if (tokens[arguments].type != JSON_OBJECT) {
        *error = "arguments must be an object";
        return 0;
    }

    result = mcp_read_string_argument(json, tokens, token_count, arguments,
                                      "query", &text);
    if (result < 0 || (text && wcslen(text) >= 512)) {
        free(text);
        *error = "query must be a string of at most 511 characters";
        return 0;
    }
    if (text) {
        wcscpy_s(request->query.text, 512, text);
        free(text);
        text = NULL;
    }

    index = json_object_get(json, tokens, token_count, arguments, "limit");
    if (index >= 0 && (!json_token_to_int(json, &tokens[index], &value) ||
                       value <= 0 || value > MCP_MAX_RESULTS)) {
        *error = "limit must be an integer between 1 and 1000";
        return 0;
    }
    if (index >= 0)
        request->limit = value;

#define READ_BOOL_ARG(json_name, field) \
    do { \
        result = mcp_read_bool_argument(json, tokens, token_count, arguments, \
                                        json_name, &value); \
        if (result < 0) { *error = json_name " must be a boolean"; return 0; } \
        if (result > 0) request->query.field = value; \
    } while (0)
    READ_BOOL_ARG("match_path", match_path);
    READ_BOOL_ARG("case_sensitive", match_case);
    READ_BOOL_ARG("whole_word", match_whole_word);
    READ_BOOL_ARG("include_subfolders", include_subfolders);
#undef READ_BOOL_ARG

    result = mcp_read_string_argument(json, tokens, token_count, arguments,
                                      "folder", &text);
    if (result < 0 || (text && wcslen(text) >= SEARCH_FOLDER_SCOPE_MAX)) {
        free(text);
        *error = "folder must be a string shorter than 1024 characters";
        return 0;
    }
    if (text) {
        wcscpy_s(request->query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, text);
        free(text);
        text = NULL;
    }

    result = mcp_read_string_argument(json, tokens, token_count, arguments,
                                      "filter", &text);
    if (result < 0 || (text && !parse_filter(text, &request->query.filter_id))) {
        free(text);
        *error = "filter has an unsupported value";
        return 0;
    }
    free(text);
    text = NULL;

    result = mcp_read_string_argument(json, tokens, token_count, arguments,
                                      "sort_by", &text);
    if (result < 0 || (text && !parse_sort_column(text, &request->query.sort_column))) {
        free(text);
        *error = "sort_by has an unsupported value";
        return 0;
    }
    free(text);

    result = mcp_read_bool_argument(json, tokens, token_count, arguments,
                                    "descending", &value);
    if (result < 0) {
        *error = "descending must be a boolean";
        return 0;
    }
    if (result > 0)
        request->query.sort_ascending = !value;
    return 1;
}

static const char *mcp_tools_json =
    "[{\"name\":\"search_files\",\"description\":\"Search the local OpenEverything file index. Returns file metadata and complete paths.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Name query. Supports * and ? wildcards plus ext:EXT.\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":50},"
    "\"match_path\":{\"type\":\"boolean\",\"default\":false},"
    "\"case_sensitive\":{\"type\":\"boolean\",\"default\":false},"
    "\"whole_word\":{\"type\":\"boolean\",\"default\":false},"
    "\"folder\":{\"type\":\"string\"},"
    "\"include_subfolders\":{\"type\":\"boolean\",\"default\":true},"
    "\"filter\":{\"type\":\"string\",\"enum\":[\"everything\",\"audio\",\"compressed\",\"document\",\"executable\",\"folder\",\"image\",\"video\"],\"default\":\"everything\"},"
    "\"sort_by\":{\"type\":\"string\",\"enum\":[\"name\",\"path\",\"size\",\"modified\",\"created\",\"attributes\"],\"default\":\"name\"},"
    "\"descending\":{\"type\":\"boolean\",\"default\":false}},\"additionalProperties\":false}},"
    "{\"name\":\"get_index_stats\",\"description\":\"Return index status, entry counts, volume counts, cache format, and cache path.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
    "{\"name\":\"reload_index\",\"description\":\"Ask OpenEverythingService to synchronize the NTFS/USN index, wait for completion, and reload index.dat. Without the service, only reloads the local cache.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}]";

static int mcp_handle_tools_call(JSON_BUFFER *response, SEARCH_RUNTIME *runtime,
                                 const char *json, const JSON_TOKEN *tokens,
                                 int token_count, int id_index, int params_index)
{
    int name_index;
    int arguments;

    if (params_index < 0 || tokens[params_index].type != JSON_OBJECT)
        return mcp_error_response(response, json, tokens, id_index,
                                  -32602, "tools/call requires object params");
    name_index = json_object_get(json, tokens, token_count, params_index, "name");
    arguments = json_object_get(json, tokens, token_count, params_index, "arguments");
    if (name_index < 0 || tokens[name_index].type != JSON_STRING)
        return mcp_error_response(response, json, tokens, id_index,
                                  -32602, "tools/call requires a tool name");

    if (json_token_equals(json, &tokens[name_index], "search_files")) {
        SEARCH_REQUEST request;
        JSON_BUFFER data;
        int *indices = NULL;
        int count = 0;
        const char *error = NULL;
        int ok;

        if (!runtime->loaded)
            return mcp_tool_error(response, json, tokens, id_index,
                                  "The OpenEverything index is not loaded. Run the GUI once to build index.dat, then call reload_index.");
        if (!mcp_parse_search_request(json, tokens, token_count, arguments,
                                      &request, &error))
            return mcp_tool_error(response, json, tokens, id_index, error);
        if (!execute_search(runtime, &request, &indices, &count))
            return mcp_tool_error(response, json, tokens, id_index,
                                  "The index search failed.");
        json_buffer_init(&data);
        ok = append_search_json(&data, runtime, &request, indices, count) &&
             mcp_tool_json_result(response, json, tokens, id_index, &data);
        free(indices);
        json_buffer_free(&data);
        return ok;
    }

    if (json_token_equals(json, &tokens[name_index], "get_index_stats")) {
        JSON_BUFFER data;
        int ok;
        json_buffer_init(&data);
        ok = append_stats_json(&data, runtime) &&
             mcp_tool_json_result(response, json, tokens, id_index, &data);
        json_buffer_free(&data);
        return ok;
    }

    if (json_token_equals(json, &tokens[name_index], "reload_index")) {
        JSON_BUFFER data;
        int ok;
        if (runtime->service_available) {
            if (!runtime_refresh_from_service(runtime, 60000))
                return mcp_tool_error(
                    response, json, tokens, id_index,
                    "The indexing service did not complete the update within 60 seconds.");
        } else if (!runtime_load(runtime)) {
            return mcp_tool_error(response, json, tokens, id_index,
                                  "index.dat could not be reloaded and OpenEverythingService is unavailable.");
        }
        json_buffer_init(&data);
        ok = append_stats_json(&data, runtime) &&
             mcp_tool_json_result(response, json, tokens, id_index, &data);
        json_buffer_free(&data);
        return ok;
    }

    return mcp_error_response(response, json, tokens, id_index,
                              -32602, "Unknown tool name");
}

static int mcp_handle_message(SEARCH_RUNTIME *runtime, const MCP_MESSAGE *message,
                              JSON_BUFFER *response, int *has_response)
{
    JSON_TOKEN *tokens;
    int capacity;
    int token_count;
    int method_index;
    int id_index;
    int params_index;

    *has_response = 1;
    if (message->length > (size_t)(INT_MAX / 2))
        return mcp_error_response(response, "", NULL, -1, -32700,
                                  "Message is too large");
    capacity = (int)(message->length / 2) + 32;
    tokens = (JSON_TOKEN *)calloc((size_t)capacity, sizeof(*tokens));
    if (!tokens)
        return 0;
    token_count = json_parse(message->data, message->length, tokens, capacity);
    if (token_count <= 0 || tokens[0].type != JSON_OBJECT) {
        int ok = mcp_error_response(response, message->data, tokens, -1,
                                    -32700, "Invalid JSON-RPC message");
        free(tokens);
        return ok;
    }

    method_index = json_object_get(message->data, tokens, token_count, 0, "method");
    id_index = json_object_get(message->data, tokens, token_count, 0, "id");
    params_index = json_object_get(message->data, tokens, token_count, 0, "params");
    if (method_index < 0 || tokens[method_index].type != JSON_STRING) {
        int ok = mcp_error_response(response, message->data, tokens, id_index,
                                    -32600, "JSON-RPC method is required");
        free(tokens);
        return ok;
    }
    if (id_index < 0) {
        *has_response = 0;
        free(tokens);
        return 1;
    }

    if (json_token_equals(message->data, &tokens[method_index], "initialize")) {
        wchar_t *protocol = NULL;
        int protocol_index = params_index >= 0
            ? json_object_get(message->data, tokens, token_count,
                              params_index, "protocolVersion") : -1;
        int ok;
        if (protocol_index >= 0 && tokens[protocol_index].type == JSON_STRING)
            protocol = json_token_to_wstring(message->data, &tokens[protocol_index]);
        ok = mcp_response_start(response, message->data, tokens, id_index) &&
             json_buffer_append(response, ",\"result\":{\"protocolVersion\":") &&
             json_buffer_append_wstring(response, protocol ? protocol : L"2024-11-05") &&
             json_buffer_append(response,
                 ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                 "\"serverInfo\":{\"name\":\"open-everything\",\"version\":\"" CLI_VERSION "\"},"
                 "\"instructions\":\"Searches the local OpenEverything index. When OpenEverythingService is installed, reload_index requests an incremental USN update before reloading the cache.\"}}");
        free(protocol);
        free(tokens);
        return ok;
    }

    if (json_token_equals(message->data, &tokens[method_index], "ping")) {
        int ok = mcp_response_start(response, message->data, tokens, id_index) &&
                 json_buffer_append(response, ",\"result\":{}}");
        free(tokens);
        return ok;
    }

    if (json_token_equals(message->data, &tokens[method_index], "tools/list")) {
        int ok = mcp_response_start(response, message->data, tokens, id_index) &&
                 json_buffer_append(response, ",\"result\":{\"tools\":") &&
                 json_buffer_append(response, mcp_tools_json) &&
                 json_buffer_append(response, "}}");
        free(tokens);
        return ok;
    }

    if (json_token_equals(message->data, &tokens[method_index], "tools/call")) {
        int ok = mcp_handle_tools_call(response, runtime, message->data, tokens,
                                       token_count, id_index, params_index);
        free(tokens);
        return ok;
    }

    {
        int ok = mcp_error_response(response, message->data, tokens, id_index,
                                    -32601, "Method not found");
        free(tokens);
        return ok;
    }
}

static int run_mcp_server(const wchar_t *index_path)
{
    SEARCH_RUNTIME runtime;
    int running = 1;

    if (!runtime_init(&runtime, index_path)) {
        if (!runtime.initialized) {
            fprintf(stderr, "Unable to initialize the MCP search runtime.\n");
            return 1;
        }
        fprintf(stderr, "OpenEverything MCP: index.dat is not available yet; reload_index can retry later.\n");
    }

    while (running) {
        MCP_MESSAGE message;
        JSON_BUFFER response;
        int read_result = mcp_read_message(&message);
        int has_response = 0;
        int ok;

        if (read_result == 0)
            break;
        if (read_result < 0) {
            fprintf(stderr, "OpenEverything MCP: invalid input framing.\n");
            break;
        }
        json_buffer_init(&response);
        ok = mcp_handle_message(&runtime, &message, &response, &has_response);
        if (ok && has_response)
            ok = mcp_write_message(&response, message.content_length_framing);
        if (!ok) {
            fprintf(stderr, "OpenEverything MCP: unable to process a request.\n");
            running = 0;
        }
        json_buffer_free(&response);
        free(message.data);
    }

    runtime_destroy(&runtime);
    return running ? 0 : 1;
}

static int parse_mcp_args(int argc, wchar_t **argv, int start,
                          const wchar_t **index_path)
{
    for (int i = start; i < argc; i++) {
        if (wcscmp(argv[i], L"--index") == 0 && i + 1 < argc) {
            *index_path = argv[++i];
        } else if (wcscmp(argv[i], L"--help") == 0 ||
                   wcscmp(argv[i], L"-h") == 0) {
            return 2;
        } else {
            fprintf(stderr, "Unknown MCP option.\n");
            return 0;
        }
    }
    return 1;
}

static int cli_stream_has_handle(FILE *stream)
{
    int descriptor = _fileno(stream);
    intptr_t handle;

    if (descriptor < 0)
        return 0;
    handle = _get_osfhandle(descriptor);
    return handle != -1 && handle != -2;
}

static int cli_stream_is_redirected(FILE *stream)
{
    int descriptor = _fileno(stream);
    intptr_t handle;
    DWORD type;

    if (descriptor < 0)
        return 0;
    handle = _get_osfhandle(descriptor);
    if (handle == -1 || handle == -2)
        return 0;
    type = GetFileType((HANDLE)handle);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

static int cli_stream_is_console(FILE *stream)
{
    int descriptor = _fileno(stream);
    intptr_t handle;
    DWORD mode;

    if (descriptor < 0)
        return 0;
    handle = _get_osfhandle(descriptor);
    return handle != -1 && handle != -2 &&
           GetConsoleMode((HANDLE)handle, &mode);
}

static int cli_process_has_console(void)
{
    DWORD process_id;
    return GetConsoleProcessList(&process_id, 1) != 0;
}

static void cli_prepare_standard_streams(void)
{
    int has_input = cli_stream_has_handle(stdin);
    int has_output = cli_stream_has_handle(stdout);
    int has_error = cli_stream_has_handle(stderr);
    int input_redirected = cli_stream_is_redirected(stdin);
    int output_redirected = cli_stream_is_redirected(stdout);
    int error_redirected = cli_stream_is_redirected(stderr);
    int has_console = cli_process_has_console();

    if (!has_console && AttachConsole(ATTACH_PARENT_PROCESS)) {
        has_console = 1;
        g_cli_attached_parent_console = 1;
    }
    if (has_console) {
        FILE *replacement;
        if (!input_redirected &&
            (!has_input || !cli_stream_is_console(stdin)))
            freopen_s(&replacement, "CONIN$", "rb", stdin);
        if (!output_redirected &&
            (!has_output || !cli_stream_is_console(stdout)))
            freopen_s(&replacement, "CONOUT$", "wb", stdout);
        if (!error_redirected &&
            (!has_error || !cli_stream_is_console(stderr)))
            freopen_s(&replacement, "CONOUT$", "wb", stderr);
    }

    if (_fileno(stdin) >= 0)
        _setmode(_fileno(stdin), _O_BINARY);
    if (_fileno(stdout) >= 0)
        _setmode(_fileno(stdout), _O_BINARY);
    if (_fileno(stderr) >= 0)
        _setmode(_fileno(stderr), _O_BINARY);
}

static int cli_command_name(const wchar_t *command)
{
    return command &&
           (wcscmp(command, L"--help") == 0 ||
            wcscmp(command, L"-h") == 0 ||
            wcscmp(command, L"search") == 0 ||
            wcscmp(command, L"stats") == 0 ||
            wcscmp(command, L"update") == 0 ||
            wcscmp(command, L"mcp") == 0 ||
            wcscmp(command, L"--mcp") == 0);
}

int oe_cli_should_run(int argc, wchar_t **argv)
{
    return argc >= 2 && argv && cli_command_name(argv[1]);
}

int oe_cli_run(int argc, wchar_t **argv)
{
    const wchar_t *index_path = NULL;
    const wchar_t *separator;
    int parsed;

    if (argc > 0 && argv && argv[0] && argv[0][0]) {
        separator = wcsrchr(argv[0], L'\\');
        if (!separator)
            separator = wcsrchr(argv[0], L'/');
        g_cli_program_name = separator ? separator + 1 : argv[0];
    }
    cli_prepare_standard_streams();

    if (argc < 2) {
        fprintf(stderr, "Missing command. Use --help for usage.\n");
        return 2;
    }
    if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0) {
        print_usage();
        return 0;
    }
    if (wcscmp(argv[1], L"search") == 0)
        return run_cli_search(argc, argv);
    if (wcscmp(argv[1], L"stats") == 0)
        return run_cli_stats(argc, argv);
    if (wcscmp(argv[1], L"update") == 0)
        return run_cli_update(argc, argv);
    if (wcscmp(argv[1], L"mcp") == 0 || wcscmp(argv[1], L"--mcp") == 0) {
        parsed = parse_mcp_args(argc, argv, 2, &index_path);
        if (parsed == 2) {
            print_usage();
            return 0;
        }
        if (!parsed)
            return 2;
        return run_mcp_server(index_path);
    }

    fprintf(stderr, "Unknown command. Use --help for usage.\n");
    return 2;
}
