#include "cache.h"
#include "index.h"
#include "search.h"

#include <psapi.h>
#include <stdint.h>

#define TEST_QUERY_COUNT 5

typedef char index_entry_must_be_64_bytes[
    sizeof(INDEX_ENTRY) == 64 ? 1 : -1];

static wchar_t g_test_config_path[MAX_PATH];

typedef struct {
    uint64_t hash;
    uint64_t name_chars;
    uint64_t path_chars;
    uint64_t extension_chars;
    int entry_count;
} INDEX_FINGERPRINT;

typedef struct {
    int count;
    uint64_t hash;
} QUERY_FINGERPRINT;

void config_get_path(wchar_t *buf, size_t size)
{
    wcscpy_s(buf, size, g_test_config_path);
}

static void set_cache_directory(const wchar_t *directory)
{
    size_t len = wcslen(directory);
    swprintf_s(g_test_config_path, MAX_PATH, L"%ls%lsconfig.ini",
               directory,
               len > 0 && (directory[len - 1] == L'\\' || directory[len - 1] == L'/')
                   ? L"" : L"\\");
}

static double elapsed_milliseconds(LARGE_INTEGER start, LARGE_INTEGER end,
                                   LARGE_INTEGER frequency)
{
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 /
           (double)frequency.QuadPart;
}

static void hash_bytes(uint64_t *hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < size; i++) {
        *hash ^= bytes[i];
        *hash *= 1099511628211ULL;
    }
}

static void hash_wstring(uint64_t *hash, const wchar_t *text, uint64_t *char_count)
{
    size_t len = text ? wcslen(text) : 0;
    hash_bytes(hash, &len, sizeof(len));
    if (len > 0)
        hash_bytes(hash, text, len * sizeof(wchar_t));
    if (char_count)
        *char_count += len;
}

static INDEX_FINGERPRINT fingerprint_index(APP_STATE *app)
{
    INDEX_FINGERPRINT result;
    memset(&result, 0, sizeof(result));
    result.hash = 1469598103934665603ULL;
    result.entry_count = app->entry_count;
    hash_bytes(&result.hash, &app->volume_count, sizeof(app->volume_count));
    hash_bytes(&result.hash, app->volumes,
               (size_t)app->volume_count * sizeof(VOLUME_INFO));

    for (int i = 0; i < app->entry_count; i++) {
        const INDEX_ENTRY *entry = &app->entries[i];
        wchar_t *name = index_duplicate_entry_name_locked(app, entry);
        const wchar_t *extension = L"";
        const wchar_t *dot = name ? wcsrchr(name, L'.') : NULL;
        if (dot && dot[1])
            extension = dot + 1;
        hash_bytes(&result.hash, &entry->size, sizeof(entry->size));
        hash_bytes(&result.hash, &entry->creation_time, sizeof(entry->creation_time));
        hash_bytes(&result.hash, &entry->modification_time, sizeof(entry->modification_time));
        hash_bytes(&result.hash, &entry->attributes, sizeof(entry->attributes));
        hash_bytes(&result.hash, &entry->file_ref, sizeof(entry->file_ref));
        hash_bytes(&result.hash, &entry->parent_ref, sizeof(entry->parent_ref));
        hash_bytes(&result.hash, &entry->is_directory, sizeof(entry->is_directory));
        hash_bytes(&result.hash, &entry->volume_index, sizeof(entry->volume_index));
        hash_bytes(&result.hash, &entry->metadata_loaded, sizeof(entry->metadata_loaded));
        hash_bytes(&result.hash, &entry->filter_type, sizeof(entry->filter_type));
        hash_wstring(&result.hash, name, &result.name_chars);
        hash_wstring(&result.hash, extension, &result.extension_chars);
        {
            wchar_t *path = index_duplicate_entry_path_locked(app, i);
            hash_wstring(&result.hash, path, &result.path_chars);
            free(path);
        }
        free(name);
    }
    return result;
}

static int fingerprint_queries(APP_STATE *app,
                               QUERY_FINGERPRINT results[TEST_QUERY_COUNT],
                               double elapsed_ms[TEST_QUERY_COUNT])
{
    static const struct {
        const wchar_t *text;
        int match_path;
        int filter_id;
    } cases[TEST_QUERY_COUNT] = {
        { L"exe", 0, FILTER_EVERYTHING },
        { L"ext:exe", 0, FILTER_EVERYTHING },
        { L"windows", 1, FILTER_EVERYTHING },
        { L"", 0, FILTER_IMAGE },
        { L"*.dll", 0, FILTER_EVERYTHING }
    };
    int *indices = (int *)malloc(SEARCH_MAX_RESULTS * sizeof(int));
    LARGE_INTEGER frequency;

    if (!indices)
        return 0;
    QueryPerformanceFrequency(&frequency);

    for (int i = 0; i < TEST_QUERY_COUNT; i++) {
        SEARCH_QUERY query;
        uint64_t hash = 1469598103934665603ULL;
        double samples[3];
        int count = 0;

        memset(&query, 0, sizeof(query));
        wcscpy_s(query.text, 512, cases[i].text);
        query.match_path = cases[i].match_path;
        query.filter_id = cases[i].filter_id;
        query.include_subfolders = 1;
        query.sort_column = COL_NAME;
        query.sort_ascending = 1;
        search_prepare_query(&query);
        for (int sample = 0; sample < 3; sample++) {
            LARGE_INTEGER start;
            LARGE_INTEGER end;
            QueryPerformanceCounter(&start);
            count = search_execute_to_buffer(
                app, &query, indices, SEARCH_MAX_RESULTS);
            QueryPerformanceCounter(&end);
            samples[sample] = elapsed_milliseconds(start, end, frequency);
        }
        if (samples[0] > samples[1]) {
            double swap = samples[0]; samples[0] = samples[1]; samples[1] = swap;
        }
        if (samples[1] > samples[2]) {
            double swap = samples[1]; samples[1] = samples[2]; samples[2] = swap;
        }
        if (samples[0] > samples[1]) {
            double swap = samples[0]; samples[0] = samples[1]; samples[1] = swap;
        }
        if (elapsed_ms)
            elapsed_ms[i] = samples[1];
        hash_bytes(&hash, &count, sizeof(count));
        for (int j = 0; j < count; j++) {
            const INDEX_ENTRY *entry = &app->entries[indices[j]];
            wchar_t *path;
            hash_bytes(&hash, &entry->volume_index, sizeof(entry->volume_index));
            hash_bytes(&hash, &entry->file_ref, sizeof(entry->file_ref));
            path = index_duplicate_entry_path_locked(app, indices[j]);
            hash_wstring(&hash, path, NULL);
            free(path);
        }
        results[i].count = count;
        results[i].hash = hash;
    }

    free(indices);
    return 1;
}

static void destroy_app(APP_STATE *app)
{
    index_clear(app);
    free(app->entries);
    free(app->filtered_indices);
    app->entries = NULL;
    app->filtered_indices = NULL;
    DeleteCriticalSection(&app->index_lock);
}

static int add_ref_test_entry(APP_STATE *app, const wchar_t *name,
                              long long file_ref, long long parent_ref,
                              int is_directory)
{
    INDEX_ENTRY entry;

    memset(&entry, 0, sizeof(entry));
    entry.file_ref = file_ref;
    entry.parent_ref = parent_ref;
    entry.volume_index = 0;
    entry.is_directory = (unsigned char)(is_directory != 0);
    entry.attributes = is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    entry.parent_index = INDEX_PARENT_UNKNOWN;
    if (!index_add_entry(app, &entry, name)) {
        return 0;
    }
    return 1;
}

static int find_ref_test_entry(const APP_STATE *app, long long file_ref)
{
    for (int i = 0; i < app->entry_count; i++) {
        if (app->entries[i].volume_index == 0 &&
            app->entries[i].file_ref == file_ref)
            return i;
    }
    return -1;
}

static int ref_test_path_equals(APP_STATE *app, long long file_ref,
                                const wchar_t *expected)
{
    int index = find_ref_test_entry(app, file_ref);
    wchar_t *path;
    int equal;

    if (index < 0)
        return 0;
    path = index_duplicate_entry_path_locked(app, index);
    equal = path && wcscmp(path, expected) == 0;
    free(path);
    return equal;
}

static int run_ref_index_mutation_probe(void)
{
    APP_STATE app;
    USN_CHANGE change;
    int ok = 1;

    index_init(&app);
    app.volume_count = 1;
    wcscpy_s(app.volumes[0].drive_letter, 4, L"C:\\");

    ok = add_ref_test_entry(&app, L".", 5, 5, 1) &&
         add_ref_test_entry(&app, L"parent", 10, 5, 1) &&
         add_ref_test_entry(&app, L"victim.txt", 11, 10, 0) &&
         add_ref_test_entry(&app, L"moved", 12, 10, 1) &&
         index_build_ref_index(&app) &&
         ref_test_path_equals(&app, 12, L"C:\\parent\\moved");

    memset(&change, 0, sizeof(change));
    change.file_ref = 11;
    change.volume_index = 0;
    change.reason = USN_REASON_FILE_DELETE;
    if (ok)
        ok = index_apply_usn_changes(&app, &change, 1) == 1 &&
             ref_test_path_equals(&app, 12, L"C:\\parent\\moved");

    memset(&change, 0, sizeof(change));
    change.name = L"renamed";
    change.file_ref = 12;
    change.parent_ref = 10;
    change.volume_index = 0;
    change.is_directory = 1;
    change.attributes = FILE_ATTRIBUTE_DIRECTORY;
    change.reason = USN_REASON_RENAME_NEW_NAME;
    if (ok)
        ok = index_apply_usn_changes(&app, &change, 1) == 1 &&
             ref_test_path_equals(&app, 12, L"C:\\parent\\renamed");

    memset(&change, 0, sizeof(change));
    change.name = L"child.bin";
    change.file_ref = 13;
    change.parent_ref = 12;
    change.volume_index = 0;
    change.attributes = FILE_ATTRIBUTE_NORMAL;
    change.reason = USN_REASON_FILE_CREATE;
    if (ok)
        ok = index_apply_usn_changes(&app, &change, 1) == 1 &&
             ref_test_path_equals(&app, 13,
                                  L"C:\\parent\\renamed\\child.bin");

    wprintf(L"ref_index_mutation=%ls\n", ok ? L"PASS" : L"FAIL");
    destroy_app(&app);
    return ok ? 0 : 1;
}

static int add_volume_test_entry(APP_STATE *app, int volume_index,
                                 const wchar_t *name, long long file_ref,
                                 int is_directory)
{
    INDEX_ENTRY entry;

    memset(&entry, 0, sizeof(entry));
    entry.file_ref = file_ref;
    entry.parent_ref = NTFS_ROOT_FRN;
    entry.volume_index = (signed char)volume_index;
    entry.is_directory = (unsigned char)(is_directory != 0);
    entry.attributes = is_directory ? FILE_ATTRIBUTE_DIRECTORY
                                    : FILE_ATTRIBUTE_NORMAL;
    entry.parent_index = INDEX_PARENT_UNKNOWN;
    return index_add_entry(app, &entry, name);
}

static int add_scope_test_entry(APP_STATE *app, int volume_index,
                                const wchar_t *name, long long file_ref,
                                long long parent_ref, int is_directory)
{
    INDEX_ENTRY entry;

    memset(&entry, 0, sizeof(entry));
    entry.file_ref = file_ref;
    entry.parent_ref = parent_ref;
    entry.volume_index = (signed char)volume_index;
    entry.is_directory = (unsigned char)(is_directory != 0);
    entry.attributes = is_directory ? FILE_ATTRIBUTE_DIRECTORY
                                    : FILE_ATTRIBUTE_NORMAL;
    entry.parent_index = INDEX_PARENT_UNKNOWN;
    return index_add_entry(app, &entry, name);
}

static int volume_results_equal(const APP_STATE *app, const int *indices,
                                int count, int volume_index)
{
    for (int i = 0; i < count; i++) {
        int index = indices[i];
        if (index < 0 || index >= app->entry_count ||
            app->entries[index].volume_index != volume_index ||
            app->entries[index].file_ref == NTFS_ROOT_FRN) {
            return 0;
        }
    }
    return 1;
}

static int run_volume_index_probe(void)
{
    APP_STATE app;
    SEARCH_QUERY query;
    int results[16];
    int count;
    int ok;

    index_init(&app);
    app.volume_count = 3;
    wcscpy_s(app.volumes[0].drive_letter, 4, L"C:\\");
    wcscpy_s(app.volumes[1].drive_letter, 4, L"D:\\");
    wcscpy_s(app.volumes[2].drive_letter, 4, L"E:\\");

    ok = add_volume_test_entry(&app, 0, L".", NTFS_ROOT_FRN, 1) &&
         add_volume_test_entry(&app, 1, L".", NTFS_ROOT_FRN, 1) &&
         add_volume_test_entry(&app, 2, L".", NTFS_ROOT_FRN, 1) &&
         add_volume_test_entry(&app, 0, L"alpha.txt", 101, 0) &&
         add_volume_test_entry(&app, 2, L"bravo.txt", 301, 0) &&
         add_volume_test_entry(&app, 1, L"charlie.exe", 201, 0) &&
         add_volume_test_entry(&app, 0, L"delta.exe", 102, 0) &&
         add_volume_test_entry(&app, 1, L"echo.txt", 202, 0) &&
         add_volume_test_entry(&app, 2, L"foxtrot.exe", 302, 0) &&
         index_build_filter_index(&app) && app.volume_index_ready &&
         app.volume_counts[0] == 3 && app.volume_counts[1] == 3 &&
         app.volume_counts[2] == 3 && app.volume_indices[0] == NULL &&
         app.volume_indices[1] != NULL && app.volume_indices[2] != NULL;

    memset(&query, 0, sizeof(query));
    query.filter_id = FILTER_EVERYTHING;
    query.include_subfolders = 1;
    query.sort_column = COL_NAME;
    query.sort_ascending = 1;
    wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, L"D:\\");
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 2 && volume_results_equal(&app, results, count, 1);

    query.filter_id = FILTER_DOCUMENT;
    wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, L"E:\\");
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 1 && volume_results_equal(&app, results, count, 2);

    if (ok)
        ok = add_volume_test_entry(&app, 1, L"golf.txt", 203, 0) &&
             !app.volume_index_ready && index_build_filter_index(&app) &&
             app.volume_counts[1] == 4;
    query.filter_id = FILTER_EVERYTHING;
    wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, L"D:\\");
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 3 && volume_results_equal(&app, results, count, 1);

    wprintf(L"volume_index_search=%ls\n", ok ? L"PASS" : L"FAIL");
    destroy_app(&app);
    return ok ? 0 : 1;
}

static int run_scope_index_probe(void)
{
    APP_STATE app;
    SEARCH_QUERY query;
    int results[16];
    int count;
    int ok;

    index_init(&app);
    app.volume_count = 1;
    wcscpy_s(app.volumes[0].drive_letter, 4, L"D:\\");
    ok = add_scope_test_entry(&app, 0, L".", 5, 5, 1) &&
         add_scope_test_entry(&app, 0, L"level1", 101, 5, 1) &&
         add_scope_test_entry(&app, 0, L"level2", 102, 101, 1) &&
         add_scope_test_entry(&app, 0, L"level3", 103, 102, 1) &&
         add_scope_test_entry(&app, 0, L"level4", 104, 103, 1) &&
         add_scope_test_entry(&app, 0, L"level5", 105, 104, 1) &&
         add_scope_test_entry(&app, 0, L"deep.txt", 106, 105, 0) &&
         add_scope_test_entry(&app, 0, L"child", 107, 105, 1) &&
         add_scope_test_entry(&app, 0, L"child.txt", 108, 107, 0) &&
         add_scope_test_entry(&app, 0, L"outside.txt", 109, 104, 0) &&
         index_build_ref_index(&app) && index_build_filter_index(&app);

    memset(&query, 0, sizeof(query));
    query.filter_id = FILTER_EVERYTHING;
    query.include_subfolders = 1;
    query.sort_column = COL_NAME;
    query.sort_ascending = 1;
    wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX,
             L"D:\\level1\\level2\\level3\\level4\\level5");
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 3 && app.scope_index_capacity <= 256 &&
         volume_results_equal(&app, results, count, 0);

    query.include_subfolders = 0;
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 2 && volume_results_equal(&app, results, count, 0);

    query.include_subfolders = 1;
    query.filter_id = FILTER_DOCUMENT;
    search_prepare_query(&query);
    count = ok ? search_execute_to_buffer(&app, &query, results, 16) : 0;
    ok = ok && count == 2 && volume_results_equal(&app, results, count, 0);

    wprintf(L"nested_scope_search=%ls\n", ok ? L"PASS" : L"FAIL");
    destroy_app(&app);
    return ok ? 0 : 1;
}

static int fingerprints_equal(const INDEX_FINGERPRINT *a,
                              const INDEX_FINGERPRINT *b)
{
    return a->hash == b->hash &&
           a->entry_count == b->entry_count &&
           a->name_chars == b->name_chars &&
           a->path_chars == b->path_chars &&
           a->extension_chars == b->extension_chars;
}

static int compare_double(const void *lhs, const void *rhs)
{
    double a = *(const double *)lhs;
    double b = *(const double *)rhs;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int benchmark_load_directory(const wchar_t *directory, int expected_result,
                                    LARGE_INTEGER frequency, double *milliseconds)
{
    APP_STATE app;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    int result;

    set_cache_directory(directory);
    index_init(&app);
    QueryPerformanceCounter(&start);
    result = cache_load_index(&app);
    QueryPerformanceCounter(&end);
    *milliseconds = elapsed_milliseconds(start, end, frequency);
    destroy_app(&app);
    return result == expected_result;
}

static int benchmark_search_directory(
    const wchar_t *directory, int expected_result,
    QUERY_FINGERPRINT results[TEST_QUERY_COUNT],
    double elapsed_ms[TEST_QUERY_COUNT])
{
    APP_STATE app;
    int result;
    int ok;

    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    ok = result == expected_result &&
         index_build_filter_index(&app) &&
         index_build_ref_index(&app) &&
         index_build_name_char_index(&app) &&
         fingerprint_queries(&app, results, elapsed_ms);
    destroy_app(&app);
    return ok;
}

static int run_load_benchmark(const wchar_t *v3_directory,
                              const wchar_t *v4_directory)
{
    enum { ITERATIONS = 5 };
    LARGE_INTEGER frequency;
    double v3_times[ITERATIONS];
    double v4_times[ITERATIONS];
    double v3_sorted[ITERATIONS];
    double v4_sorted[ITERATIONS];

    QueryPerformanceFrequency(&frequency);
    for (int i = 0; i < ITERATIONS; i++) {
        int ok;
        if ((i & 1) == 0) {
            ok = benchmark_load_directory(v3_directory, CACHE_LOAD_LEGACY,
                                          frequency, &v3_times[i]) &&
                 benchmark_load_directory(v4_directory, CACHE_LOAD_CURRENT,
                                          frequency, &v4_times[i]);
        } else {
            ok = benchmark_load_directory(v4_directory, CACHE_LOAD_CURRENT,
                                          frequency, &v4_times[i]) &&
                 benchmark_load_directory(v3_directory, CACHE_LOAD_LEGACY,
                                          frequency, &v3_times[i]);
        }
        if (!ok) {
            fwprintf(stderr, L"Benchmark load failed at iteration %d\n", i);
            return 1;
        }
        wprintf(L"iteration_%d_v3_ms=%.3f\n", i, v3_times[i]);
        wprintf(L"iteration_%d_v4_ms=%.3f\n", i, v4_times[i]);
    }

    memcpy(v3_sorted, v3_times, sizeof(v3_times));
    memcpy(v4_sorted, v4_times, sizeof(v4_times));
    qsort(v3_sorted, ITERATIONS, sizeof(double), compare_double);
    qsort(v4_sorted, ITERATIONS, sizeof(double), compare_double);
    wprintf(L"v3_median_ms=%.3f\n", v3_sorted[ITERATIONS / 2]);
    wprintf(L"v4_median_ms=%.3f\n", v4_sorted[ITERATIONS / 2]);
    wprintf(L"load_ratio=%.4f\n",
            v4_sorted[ITERATIONS / 2] / v3_sorted[ITERATIONS / 2]);
    return 0;
}

static int run_search_benchmark(const wchar_t *v3_directory,
                                const wchar_t *v4_directory)
{
    enum { ITERATIONS = 3 };
    double v3_times[TEST_QUERY_COUNT][ITERATIONS];
    double v4_times[TEST_QUERY_COUNT][ITERATIONS];
    QUERY_FINGERPRINT v3_results[TEST_QUERY_COUNT];
    QUERY_FINGERPRINT v4_results[TEST_QUERY_COUNT];

    for (int i = 0; i < ITERATIONS; i++) {
        double v3_sample[TEST_QUERY_COUNT];
        double v4_sample[TEST_QUERY_COUNT];
        int ok;

        if ((i & 1) == 0) {
            ok = benchmark_search_directory(
                     v3_directory, CACHE_LOAD_LEGACY, v3_results, v3_sample) &&
                 benchmark_search_directory(
                     v4_directory, CACHE_LOAD_CURRENT, v4_results, v4_sample);
        } else {
            ok = benchmark_search_directory(
                     v4_directory, CACHE_LOAD_CURRENT, v4_results, v4_sample) &&
                 benchmark_search_directory(
                     v3_directory, CACHE_LOAD_LEGACY, v3_results, v3_sample);
        }
        if (!ok) {
            fwprintf(stderr, L"Search benchmark failed at iteration %d\n", i);
            return 1;
        }
        for (int query = 0; query < TEST_QUERY_COUNT; query++) {
            if (v3_results[query].count != v4_results[query].count ||
                v3_results[query].hash != v4_results[query].hash) {
                fwprintf(stderr, L"Search result mismatch at query %d\n", query);
                return 1;
            }
            v3_times[query][i] = v3_sample[query];
            v4_times[query][i] = v4_sample[query];
        }
    }

    for (int query = 0; query < TEST_QUERY_COUNT; query++) {
        qsort(v3_times[query], ITERATIONS, sizeof(double), compare_double);
        qsort(v4_times[query], ITERATIONS, sizeof(double), compare_double);
        wprintf(L"query_%d_count=%d\n", query, v4_results[query].count);
        wprintf(L"query_%d_v3_median_ms=%.3f\n",
                query, v3_times[query][ITERATIONS / 2]);
        wprintf(L"query_%d_v4_median_ms=%.3f\n",
                query, v4_times[query][ITERATIONS / 2]);
        wprintf(L"query_%d_ratio=%.4f\n", query,
                v4_times[query][ITERATIONS / 2] /
                    v3_times[query][ITERATIONS / 2]);
    }
    return 0;
}

static int run_load_probe(const wchar_t *directory, int expected_result)
{
    APP_STATE app;
    int result;

    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    destroy_app(&app);
    wprintf(L"load_result=%d\n", result);
    return result == expected_result ? 0 : 1;
}

static int run_memory_probe(const wchar_t *directory)
{
    APP_STATE app;
    PROCESS_MEMORY_COUNTERS_EX counters;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    double sort_ms;
    double compact_ms;
    double filter_ms;
    double ref_ms;
    double char_ms;
    int result;

    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    index_sort_entries_by_name(&app);
    QueryPerformanceCounter(&end);
    sort_ms = elapsed_milliseconds(start, end, frequency);
    if (result == CACHE_LOAD_CURRENT && app.name_pool.mapped_view) {
        compact_ms = 0.0;
    } else {
        QueryPerformanceCounter(&start);
        result = result != CACHE_LOAD_FAILED && index_compact_entry_names(&app);
        QueryPerformanceCounter(&end);
        compact_ms = elapsed_milliseconds(start, end, frequency);
    }
    QueryPerformanceCounter(&start);
    result = result && index_build_filter_index(&app);
    QueryPerformanceCounter(&end);
    filter_ms = elapsed_milliseconds(start, end, frequency);
    QueryPerformanceCounter(&start);
    result = result && index_build_ref_index(&app);
    QueryPerformanceCounter(&end);
    ref_ms = elapsed_milliseconds(start, end, frequency);
    QueryPerformanceCounter(&start);
    result = result && index_build_name_char_index(&app);
    QueryPerformanceCounter(&end);
    char_ms = elapsed_milliseconds(start, end, frequency);
    if (!result) {
        destroy_app(&app);
        return 1;
    }
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&counters,
                              sizeof(counters))) {
        destroy_app(&app);
        return 1;
    }
    wprintf(L"entries=%d\n", app.entry_count);
    wprintf(L"entry_size=%zu\n", sizeof(INDEX_ENTRY));
    wprintf(L"entry_capacity=%d\n", app.entry_capacity);
    wprintf(L"ref_capacity=%d\n", app.ref_index_capacity);
    wprintf(L"mapped_name_mb=%.2f\n",
            (double)app.name_pool.mapped_size / (1024.0 * 1024.0));
    wprintf(L"sort_ms=%.3f\n", sort_ms);
    wprintf(L"compact_ms=%.3f\n", compact_ms);
    wprintf(L"filter_ms=%.3f\n", filter_ms);
    wprintf(L"ref_ms=%.3f\n", ref_ms);
    wprintf(L"char_ms=%.3f\n", char_ms);
    wprintf(L"working_set_mb=%.2f\n",
            (double)counters.WorkingSetSize / (1024.0 * 1024.0));
    wprintf(L"private_mb=%.2f\n",
            (double)counters.PrivateUsage / (1024.0 * 1024.0));
    destroy_app(&app);
    return 0;
}

static int run_query_probe(const wchar_t *directory)
{
    APP_STATE app;
    QUERY_FINGERPRINT results[TEST_QUERY_COUNT];
    double elapsed_ms[TEST_QUERY_COUNT];
    int result;

    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    if (result == CACHE_LOAD_FAILED ||
        !index_build_filter_index(&app) ||
        !index_build_ref_index(&app) ||
        !index_build_name_char_index(&app) ||
        !fingerprint_queries(&app, results, elapsed_ms)) {
        destroy_app(&app);
        return 1;
    }
    for (int i = 0; i < TEST_QUERY_COUNT; i++) {
        wprintf(L"query_%d_count=%d\n", i, results[i].count);
        wprintf(L"query_%d_ms=%.3f\n", i, elapsed_ms[i]);
        wprintf(L"query_%d_hash=%016llx\n", i, results[i].hash);
    }
    destroy_app(&app);
    return 0;
}

static double median_three(double values[3])
{
    if (values[0] > values[1]) {
        double swap = values[0]; values[0] = values[1]; values[1] = swap;
    }
    if (values[1] > values[2]) {
        double swap = values[1]; values[1] = values[2]; values[2] = swap;
    }
    if (values[0] > values[1]) {
        double swap = values[0]; values[0] = values[1]; values[1] = swap;
    }
    return values[1];
}

static int run_volume_search_probe(const wchar_t *directory)
{
    APP_STATE app;
    SEARCH_QUERY query;
    LARGE_INTEGER frequency;
    int *indices;
    int result;

    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    if (result == CACHE_LOAD_FAILED || !index_build_filter_index(&app)) {
        destroy_app(&app);
        return 1;
    }
    indices = (int *)malloc(SEARCH_MAX_RESULTS * sizeof(*indices));
    if (!indices) {
        destroy_app(&app);
        return 1;
    }
    QueryPerformanceFrequency(&frequency);

    for (int volume = 0; volume < app.volume_count && volume < 26; volume++) {
        static const int filters[] = { FILTER_EVERYTHING, FILTER_DOCUMENT };
        for (int filter_index = 0;
             filter_index < (int)(sizeof(filters) / sizeof(filters[0]));
             filter_index++) {
            double samples[3];
            int count = 0;

            memset(&query, 0, sizeof(query));
            query.filter_id = filters[filter_index];
            query.include_subfolders = 1;
            query.sort_column = COL_NAME;
            query.sort_ascending = 1;
            wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX,
                     app.volumes[volume].drive_letter);
            search_prepare_query(&query);
            for (int sample = 0; sample < 3; sample++) {
                LARGE_INTEGER start;
                LARGE_INTEGER end;
                QueryPerformanceCounter(&start);
                count = search_execute_to_buffer(
                    &app, &query, indices, SEARCH_MAX_RESULTS);
                QueryPerformanceCounter(&end);
                samples[sample] = elapsed_milliseconds(start, end, frequency);
            }
            wprintf(L"volume_%lc_%ls_count=%d\n",
                    app.volumes[volume].drive_letter[0],
                    filters[filter_index] == FILTER_EVERYTHING
                        ? L"everything" : L"document",
                    count);
            wprintf(L"volume_%lc_%ls_median_ms=%.3f\n",
                    app.volumes[volume].drive_letter[0],
                    filters[filter_index] == FILTER_EVERYTHING
                        ? L"everything" : L"document",
                    median_three(samples));
        }
    }

    free(indices);
    destroy_app(&app);
    return 0;
}

static int run_folder_search_probe(const wchar_t *directory,
                                   const wchar_t *folder)
{
    APP_STATE app;
    SEARCH_QUERY query;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    double warm_samples[3];
    double filter_samples[3];
    int *indices;
    int count = 0;
    int result;

    if (wcslen(folder) >= SEARCH_FOLDER_SCOPE_MAX)
        return 1;
    set_cache_directory(directory);
    index_init(&app);
    result = cache_load_index(&app);
    if (result == CACHE_LOAD_FAILED || !index_build_filter_index(&app) ||
        !index_build_ref_index(&app)) {
        destroy_app(&app);
        return 1;
    }
    indices = (int *)malloc(SEARCH_MAX_RESULTS * sizeof(*indices));
    if (!indices) {
        destroy_app(&app);
        return 1;
    }
    memset(&query, 0, sizeof(query));
    query.filter_id = FILTER_EVERYTHING;
    query.include_subfolders = 1;
    query.sort_column = COL_NAME;
    query.sort_ascending = 1;
    wcscpy_s(query.folder_scope, SEARCH_FOLDER_SCOPE_MAX, folder);
    search_prepare_query(&query);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    count = search_execute_to_buffer(&app, &query, indices, SEARCH_MAX_RESULTS);
    QueryPerformanceCounter(&end);
    wprintf(L"folder_scope_cold_count=%d\n", count);
    wprintf(L"folder_scope_cold_ms=%.3f\n",
            elapsed_milliseconds(start, end, frequency));
    wprintf(L"folder_scope_cache_capacity=%d\n", app.scope_index_capacity);
    for (int sample = 0; sample < 3; sample++) {
        QueryPerformanceCounter(&start);
        count = search_execute_to_buffer(&app, &query, indices, SEARCH_MAX_RESULTS);
        QueryPerformanceCounter(&end);
        warm_samples[sample] = elapsed_milliseconds(start, end, frequency);
    }
    wprintf(L"folder_scope_warm_count=%d\n", count);
    wprintf(L"folder_scope_warm_median_ms=%.3f\n",
            median_three(warm_samples));

    query.filter_id = FILTER_DOCUMENT;
    search_prepare_query(&query);
    for (int sample = 0; sample < 3; sample++) {
        QueryPerformanceCounter(&start);
        count = search_execute_to_buffer(&app, &query, indices, SEARCH_MAX_RESULTS);
        QueryPerformanceCounter(&end);
        filter_samples[sample] = elapsed_milliseconds(start, end, frequency);
    }
    wprintf(L"folder_scope_document_count=%d\n", count);
    wprintf(L"folder_scope_document_median_ms=%.3f\n",
            median_three(filter_samples));

    free(indices);
    destroy_app(&app);
    return 0;
}

static int run_leak_probe(const wchar_t *directory)
{
    APP_STATE app;

    set_cache_directory(directory);
    index_init(&app);
    for (int iteration = 0; iteration < 5; iteration++) {
        PROCESS_MEMORY_COUNTERS_EX counters;
        QUERY_FINGERPRINT query_results[TEST_QUERY_COUNT];
        double query_ms[TEST_QUERY_COUNT];
        int result;

        index_clear(&app);
        result = cache_load_index(&app);
        if (result == CACHE_LOAD_FAILED ||
            !index_build_filter_index(&app) ||
            !index_build_ref_index(&app) ||
            !index_build_name_char_index(&app) ||
            !fingerprint_queries(&app, query_results, query_ms)) {
            destroy_app(&app);
            return 1;
        }
        HeapCompact(GetProcessHeap(), 0);
        memset(&counters, 0, sizeof(counters));
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                  (PROCESS_MEMORY_COUNTERS *)&counters,
                                  sizeof(counters))) {
            destroy_app(&app);
            return 1;
        }
        wprintf(L"iteration_%d_private_mb=%.2f\n", iteration,
                (double)counters.PrivateUsage / (1024.0 * 1024.0));
        wprintf(L"iteration_%d_working_set_mb=%.2f\n", iteration,
                (double)counters.WorkingSetSize / (1024.0 * 1024.0));
    }
    destroy_app(&app);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    APP_STATE app;
    INDEX_FINGERPRINT v3_fingerprint;
    INDEX_FINGERPRINT v4_fingerprint;
    QUERY_FINGERPRINT v3_queries[TEST_QUERY_COUNT];
    QUERY_FINGERPRINT v4_queries[TEST_QUERY_COUNT];
    double v3_query_ms[TEST_QUERY_COUNT];
    double v4_query_ms[TEST_QUERY_COUNT];
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    WIN32_FILE_ATTRIBUTE_DATA file_data;
    wchar_t index_path[MAX_PATH];
    double v3_load_ms;
    double save_ms;
    double v4_load_ms;
    unsigned long long file_size;
    int load_result;
    int ok = 1;

    if (argc == 4 && wcscmp(argv[1], L"--benchmark") == 0)
        return run_load_benchmark(argv[2], argv[3]);
    if (argc == 4 && wcscmp(argv[1], L"--search-benchmark") == 0)
        return run_search_benchmark(argv[2], argv[3]);
    if (argc == 4 && wcscmp(argv[1], L"--probe") == 0)
        return run_load_probe(argv[2], _wtoi(argv[3]));
    if (argc == 3 && wcscmp(argv[1], L"--memory-probe") == 0)
        return run_memory_probe(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--query-probe") == 0)
        return run_query_probe(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--volume-search-probe") == 0)
        return run_volume_search_probe(argv[2]);
    if (argc == 4 && wcscmp(argv[1], L"--folder-search-probe") == 0)
        return run_folder_search_probe(argv[2], argv[3]);
    if (argc == 3 && wcscmp(argv[1], L"--leak-probe") == 0)
        return run_leak_probe(argv[2]);
    if (argc == 2 && wcscmp(argv[1], L"--ref-index-probe") == 0)
        return run_ref_index_mutation_probe();
    if (argc == 2 && wcscmp(argv[1], L"--volume-index-probe") == 0)
        return run_volume_index_probe();
    if (argc == 2 && wcscmp(argv[1], L"--scope-index-probe") == 0)
        return run_scope_index_probe();

    if (argc != 2) {
        fwprintf(stderr,
                 L"Usage: cache_roundtrip.exe TEST_CACHE_DIRECTORY\n"
                 L"       cache_roundtrip.exe --benchmark V3_DIRECTORY V4_DIRECTORY\n"
                 L"       cache_roundtrip.exe --search-benchmark V3_DIRECTORY V4_DIRECTORY\n"
                 L"       cache_roundtrip.exe --probe DIRECTORY EXPECTED_RESULT\n"
                 L"       cache_roundtrip.exe --volume-search-probe DIRECTORY\n"
                 L"       cache_roundtrip.exe --folder-search-probe DIRECTORY FOLDER\n"
                 L"       cache_roundtrip.exe --ref-index-probe\n"
                 L"       cache_roundtrip.exe --volume-index-probe\n"
                 L"       cache_roundtrip.exe --scope-index-probe\n");
        return 2;
    }

    set_cache_directory(argv[1]);
    QueryPerformanceFrequency(&frequency);

    index_init(&app);
    QueryPerformanceCounter(&start);
    load_result = cache_load_index(&app);
    QueryPerformanceCounter(&end);
    v3_load_ms = elapsed_milliseconds(start, end, frequency);
    if (load_result == CACHE_LOAD_FAILED) {
        fwprintf(stderr, L"Source cache load failed\n");
        destroy_app(&app);
        return 1;
    }

    if (!index_build_ref_index(&app)) {
        fwprintf(stderr, L"Failed to build V3 reference index\n");
        destroy_app(&app);
        return 1;
    }
    v3_fingerprint = fingerprint_index(&app);
    if (!index_build_filter_index(&app) ||
        !index_build_name_char_index(&app) ||
        !fingerprint_queries(&app, v3_queries, v3_query_ms)) {
        fwprintf(stderr, L"Failed to build V3 indexes or query fingerprints\n");
        destroy_app(&app);
        return 1;
    }

    QueryPerformanceCounter(&start);
    ok = cache_save_index(&app);
    QueryPerformanceCounter(&end);
    save_ms = elapsed_milliseconds(start, end, frequency);
    destroy_app(&app);
    if (!ok) {
        fwprintf(stderr, L"V4 cache save failed\n");
        return 1;
    }

    cache_get_index_path(index_path, MAX_PATH);
    if (!GetFileAttributesExW(index_path, GetFileExInfoStandard, &file_data)) {
        fwprintf(stderr, L"Unable to stat V4 cache\n");
        return 1;
    }
    file_size = ((unsigned long long)file_data.nFileSizeHigh << 32) |
                file_data.nFileSizeLow;

    index_init(&app);
    QueryPerformanceCounter(&start);
    load_result = cache_load_index(&app);
    QueryPerformanceCounter(&end);
    v4_load_ms = elapsed_milliseconds(start, end, frequency);
    if (load_result != CACHE_LOAD_CURRENT) {
        fwprintf(stderr, L"Expected V4 cache, load result was %d\n", load_result);
        destroy_app(&app);
        return 1;
    }
    if (!app.name_pool.mapped_view ||
        app.name_pool.mapped_size != app.name_pool.size) {
        fwprintf(stderr, L"Current cache names are not file mapped\n");
        destroy_app(&app);
        return 1;
    }

    if (!index_build_ref_index(&app)) {
        fwprintf(stderr, L"Failed to build V4 reference index\n");
        destroy_app(&app);
        return 1;
    }
    v4_fingerprint = fingerprint_index(&app);
    if (!index_build_filter_index(&app) ||
        !index_build_name_char_index(&app) ||
        !fingerprint_queries(&app, v4_queries, v4_query_ms)) {
        fwprintf(stderr, L"Failed to build V4 indexes or query fingerprints\n");
        destroy_app(&app);
        return 1;
    }

    if (!fingerprints_equal(&v3_fingerprint, &v4_fingerprint)) {
        fwprintf(stderr, L"Entry fingerprint mismatch: V3=%016llx V4=%016llx\n",
                 v3_fingerprint.hash, v4_fingerprint.hash);
        ok = 0;
    }
    for (int i = 0; i < TEST_QUERY_COUNT; i++) {
        if (v3_queries[i].count != v4_queries[i].count ||
            v3_queries[i].hash != v4_queries[i].hash) {
            fwprintf(stderr,
                     L"Query %d mismatch: V3=%d/%016llx V4=%d/%016llx\n",
                     i, v3_queries[i].count, v3_queries[i].hash,
                     v4_queries[i].count, v4_queries[i].hash);
            ok = 0;
        }
    }

    {
        INDEX_ENTRY overlay_entry;
        void *mapped_view = app.name_pool.mapped_view;
        size_t mapped_size = app.name_pool.mapped_size;
        memset(&overlay_entry, 0, sizeof(overlay_entry));
        overlay_entry.file_ref = LLONG_MAX;
        overlay_entry.parent_ref = 5;
        overlay_entry.parent_index = INDEX_PARENT_UNKNOWN;
        if (!index_add_entry(&app, &overlay_entry, L"mapped-overlay-probe") ||
            app.name_pool.mapped_view != mapped_view ||
            app.name_pool.mapped_size != mapped_size ||
            !app.name_pool.data) {
            fwprintf(stderr, L"Mapped name overlay test failed\n");
            ok = 0;
        }
    }

    wprintf(L"entries=%d\n", v4_fingerprint.entry_count);
    wprintf(L"v4_bytes=%llu\n", file_size);
    wprintf(L"v3_load_ms=%.3f\n", v3_load_ms);
    wprintf(L"v4_save_ms=%.3f\n", save_ms);
    wprintf(L"v4_load_ms=%.3f\n", v4_load_ms);
    wprintf(L"entry_hash=%016llx\n", v4_fingerprint.hash);
    for (int i = 0; i < TEST_QUERY_COUNT; i++) {
        wprintf(L"query_%d=%d/%016llx\n",
                i, v4_queries[i].count, v4_queries[i].hash);
        wprintf(L"query_%d_v3_ms=%.3f\n", i, v3_query_ms[i]);
        wprintf(L"query_%d_v4_ms=%.3f\n", i, v4_query_ms[i]);
    }
    wprintf(L"roundtrip=%ls\n", ok ? L"PASS" : L"FAIL");

    destroy_app(&app);
    return ok ? 0 : 1;
}
