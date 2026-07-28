#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_object_values(void)
{
    static const char input[] =
        "{\"query\":\"\",\"limit\":7,\"nested\":{\"enabled\":true}}";
    JSON_TOKEN tokens[32];
    wchar_t *query;
    int count;
    int query_index;
    int limit_index;
    int nested_index;
    int enabled_index;
    int value = 0;

    count = json_parse(input, strlen(input), tokens, 32);
    if (count <= 0)
        return 0;
    query_index = json_object_get(input, tokens, count, 0, "query");
    limit_index = json_object_get(input, tokens, count, 0, "limit");
    nested_index = json_object_get(input, tokens, count, 0, "nested");
    enabled_index = json_object_get(input, tokens, count, nested_index, "enabled");
    query = query_index >= 0 ? json_token_to_wstring(input, &tokens[query_index]) : NULL;
    if (!query || query[0] ||
        !json_token_to_int(input, &tokens[limit_index], &value) || value != 7 ||
        !json_token_to_bool(input, &tokens[enabled_index], &value) || value != 1) {
        free(query);
        return 0;
    }
    free(query);
    return 1;
}

static int test_unicode_escapes(void)
{
    static const char input[] = "\"A\\u4e2d\\u6587\\ud83d\\ude00\\n\"";
    static const wchar_t expected[] = {
        L'A', 0x4e2d, 0x6587, 0xd83d, 0xde00, L'\n', L'\0'
    };
    JSON_TOKEN tokens[4];
    wchar_t *decoded;
    int count = json_parse(input, strlen(input), tokens, 4);

    if (count != 1)
        return 0;
    decoded = json_token_to_wstring(input, &tokens[0]);
    if (!decoded)
        return 0;
    count = wcscmp(decoded, expected) == 0;
    free(decoded);
    return count;
}

static int test_wstring_round_trip(void)
{
    static const wchar_t original[] = {
        L'q', L'u', L'o', L't', L'e', L'"', L'\\', L'\n', 0x4e2d, L'\0'
    };
    JSON_BUFFER buffer;
    JSON_TOKEN tokens[8];
    wchar_t *decoded;
    int count;
    int ok;

    json_buffer_init(&buffer);
    if (!json_buffer_append_wstring(&buffer, original)) {
        json_buffer_free(&buffer);
        return 0;
    }
    count = json_parse(buffer.data, buffer.length, tokens, 8);
    decoded = count == 1 ? json_token_to_wstring(buffer.data, &tokens[0]) : NULL;
    ok = decoded && wcscmp(decoded, original) == 0;
    free(decoded);
    json_buffer_free(&buffer);
    return ok;
}

int main(void)
{
    int ok = test_object_values() &&
             test_unicode_escapes() &&
             test_wstring_round_trip();
    printf("json_tests=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
