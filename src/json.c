#include "json.h"

#include <windows.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int json_allocate_token(JSON_TOKEN *tokens, int capacity, int *next,
                               JSON_TYPE type, int start, int parent)
{
    JSON_TOKEN *token;

    if (*next >= capacity)
        return -1;
    token = &tokens[*next];
    token->type = type;
    token->start = start;
    token->end = -1;
    token->size = 0;
    token->parent = parent;
    if (parent >= 0)
        tokens[parent].size++;
    return (*next)++;
}

static int json_is_delimiter(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == ',' || ch == ']' || ch == '}';
}

int json_parse(const char *json, size_t length, JSON_TOKEN *tokens, int capacity)
{
    int next = 0;
    int current = -1;
    int root_count = 0;

    if (!json || !tokens || capacity <= 0 || length > INT_MAX)
        return -1;

    for (int pos = 0; pos < (int)length;) {
        char ch = json[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
            ch == ':' || ch == ',') {
            pos++;
            continue;
        }
        if (ch == '{' || ch == '[') {
            int index = json_allocate_token(tokens, capacity, &next,
                                            ch == '{' ? JSON_OBJECT : JSON_ARRAY,
                                            pos, current);
            if (index < 0)
                return -2;
            if (current < 0)
                root_count++;
            current = index;
            pos++;
            continue;
        }
        if (ch == '}' || ch == ']') {
            JSON_TYPE expected = ch == '}' ? JSON_OBJECT : JSON_ARRAY;
            if (current < 0 || tokens[current].type != expected)
                return -1;
            tokens[current].end = pos + 1;
            current = tokens[current].parent;
            pos++;
            continue;
        }
        if (ch == '"') {
            int start = ++pos;
            int closed = 0;
            while (pos < (int)length) {
                unsigned char byte = (unsigned char)json[pos];
                if (byte == '"') {
                    int index = json_allocate_token(tokens, capacity, &next,
                                                    JSON_STRING, start, current);
                    if (index < 0)
                        return -2;
                    tokens[index].end = pos;
                    if (current < 0)
                        root_count++;
                    pos++;
                    closed = 1;
                    break;
                }
                if (byte < 0x20)
                    return -1;
                if (byte == '\\') {
                    if (++pos >= (int)length)
                        return -1;
                    ch = json[pos];
                    if (ch == 'u') {
                        if (pos + 4 >= (int)length)
                            return -1;
                        for (int digit = 1; digit <= 4; digit++) {
                            char hex = json[pos + digit];
                            if (!((hex >= '0' && hex <= '9') ||
                                  (hex >= 'a' && hex <= 'f') ||
                                  (hex >= 'A' && hex <= 'F')))
                                return -1;
                        }
                        pos += 4;
                    } else if (!strchr("\"\\/bfnrt", ch)) {
                        return -1;
                    }
                }
                pos++;
            }
            if (!closed)
                return -1;
            continue;
        }
        if (ch == ':') {
            pos++;
            continue;
        }
        {
            int start = pos;
            int index;
            while (pos < (int)length && !json_is_delimiter(json[pos]) &&
                   json[pos] != ':') {
                unsigned char byte = (unsigned char)json[pos];
                if (byte < 0x20 || byte == '"' || byte == '{' || byte == '[')
                    return -1;
                pos++;
            }
            if (pos == start)
                return -1;
            index = json_allocate_token(tokens, capacity, &next,
                                        JSON_PRIMITIVE, start, current);
            if (index < 0)
                return -2;
            tokens[index].end = pos;
            if (current < 0)
                root_count++;
        }
    }

    if (current >= 0 || root_count != 1)
        return -1;
    return next;
}

int json_token_equals(const char *json, const JSON_TOKEN *token, const char *value)
{
    size_t length;

    if (!json || !token || !value || token->start < 0 || token->end < token->start)
        return 0;
    length = strlen(value);
    return (size_t)(token->end - token->start) == length &&
           memcmp(json + token->start, value, length) == 0;
}

int json_object_get(const char *json, const JSON_TOKEN *tokens, int token_count,
                    int object_index, const char *key)
{
    int direct_index = 0;
    int key_index = -1;

    if (!json || !tokens || !key || object_index < 0 ||
        object_index >= token_count || tokens[object_index].type != JSON_OBJECT)
        return -1;

    for (int i = object_index + 1; i < token_count; i++) {
        if (tokens[i].start >= tokens[object_index].end)
            break;
        if (tokens[i].parent != object_index)
            continue;
        if ((direct_index & 1) == 0) {
            key_index = i;
        } else if (key_index >= 0 && tokens[key_index].type == JSON_STRING &&
                   json_token_equals(json, &tokens[key_index], key)) {
            return i;
        }
        direct_index++;
    }
    return -1;
}

int json_token_to_int(const char *json, const JSON_TOKEN *token, int *value)
{
    char buffer[32];
    char *end = NULL;
    long parsed;
    int length;

    if (!json || !token || !value || token->type != JSON_PRIMITIVE)
        return 0;
    length = token->end - token->start;
    if (length <= 0 || length >= (int)sizeof(buffer))
        return 0;
    memcpy(buffer, json + token->start, (size_t)length);
    buffer[length] = '\0';
    parsed = strtol(buffer, &end, 10);
    if (!end || *end || parsed < INT_MIN || parsed > INT_MAX)
        return 0;
    *value = (int)parsed;
    return 1;
}

int json_token_to_bool(const char *json, const JSON_TOKEN *token, int *value)
{
    if (!json || !token || !value || token->type != JSON_PRIMITIVE)
        return 0;
    if (json_token_equals(json, token, "true")) {
        *value = 1;
        return 1;
    }
    if (json_token_equals(json, token, "false")) {
        *value = 0;
        return 1;
    }
    return 0;
}

static int json_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static unsigned int json_read_hex4(const char *text)
{
    unsigned int value = 0;
    for (int i = 0; i < 4; i++)
        value = (value << 4) | (unsigned int)json_hex_value(text[i]);
    return value;
}

static int json_append_codepoint_utf8(char *output, int cursor, unsigned int cp)
{
    if (cp <= 0x7f) {
        output[cursor++] = (char)cp;
    } else if (cp <= 0x7ff) {
        output[cursor++] = (char)(0xc0 | (cp >> 6));
        output[cursor++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0xffff) {
        output[cursor++] = (char)(0xe0 | (cp >> 12));
        output[cursor++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        output[cursor++] = (char)(0x80 | (cp & 0x3f));
    } else {
        output[cursor++] = (char)(0xf0 | (cp >> 18));
        output[cursor++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        output[cursor++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        output[cursor++] = (char)(0x80 | (cp & 0x3f));
    }
    return cursor;
}

wchar_t *json_token_to_wstring(const char *json, const JSON_TOKEN *token)
{
    char *utf8;
    wchar_t *wide;
    int input_length;
    int cursor = 0;
    int wide_length;

    if (!json || !token || token->type != JSON_STRING ||
        token->start < 0 || token->end < token->start)
        return NULL;
    input_length = token->end - token->start;
    utf8 = (char *)malloc((size_t)input_length * 3 + 1);
    if (!utf8)
        return NULL;

    for (int i = 0; i < input_length; i++) {
        char ch = json[token->start + i];
        if (ch != '\\') {
            utf8[cursor++] = ch;
            continue;
        }
        ch = json[token->start + ++i];
        switch (ch) {
        case '"': utf8[cursor++] = '"'; break;
        case '\\': utf8[cursor++] = '\\'; break;
        case '/': utf8[cursor++] = '/'; break;
        case 'b': utf8[cursor++] = '\b'; break;
        case 'f': utf8[cursor++] = '\f'; break;
        case 'n': utf8[cursor++] = '\n'; break;
        case 'r': utf8[cursor++] = '\r'; break;
        case 't': utf8[cursor++] = '\t'; break;
        case 'u':
        {
            unsigned int cp = json_read_hex4(json + token->start + i + 1);
            i += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && i + 6 < input_length &&
                json[token->start + i + 1] == '\\' &&
                json[token->start + i + 2] == 'u') {
                unsigned int low = json_read_hex4(json + token->start + i + 3);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    i += 6;
                }
            }
            cursor = json_append_codepoint_utf8(utf8, cursor, cp);
            break;
        }
        default:
            free(utf8);
            return NULL;
        }
    }
    utf8[cursor] = '\0';

    if (cursor == 0) {
        wide = (wchar_t *)calloc(1, sizeof(*wide));
        free(utf8);
        return wide;
    }

    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      utf8, cursor, NULL, 0);
    if (wide_length <= 0) {
        free(utf8);
        return NULL;
    }
    wide = (wchar_t *)malloc(((size_t)wide_length + 1) * sizeof(*wide));
    if (!wide) {
        free(utf8);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, cursor,
                        wide, wide_length);
    wide[wide_length] = L'\0';
    free(utf8);
    return wide;
}

void json_buffer_init(JSON_BUFFER *buffer)
{
    if (!buffer)
        return;
    memset(buffer, 0, sizeof(*buffer));
}

void json_buffer_free(JSON_BUFFER *buffer)
{
    if (!buffer)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int json_buffer_reserve(JSON_BUFFER *buffer, size_t extra)
{
    size_t needed;
    size_t capacity;
    char *data;

    if (!buffer || buffer->failed || extra > SIZE_MAX - buffer->length - 1) {
        if (buffer)
            buffer->failed = 1;
        return 0;
    }
    needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity)
        return 1;
    capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    data = (char *)realloc(buffer->data, capacity);
    if (!data) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

int json_buffer_append_n(JSON_BUFFER *buffer, const char *text, size_t length)
{
    if (!text || !json_buffer_reserve(buffer, length))
        return 0;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

int json_buffer_append(JSON_BUFFER *buffer, const char *text)
{
    return text ? json_buffer_append_n(buffer, text, strlen(text)) : 0;
}

int json_buffer_append_char(JSON_BUFFER *buffer, char ch)
{
    return json_buffer_append_n(buffer, &ch, 1);
}

int json_buffer_append_format(JSON_BUFFER *buffer, const char *format, ...)
{
    va_list args;
    va_list copy;
    int length;

    if (!buffer || !format || buffer->failed)
        return 0;
    va_start(args, format);
    va_copy(copy, args);
    length = _vscprintf(format, copy);
    va_end(copy);
    if (length < 0 || !json_buffer_reserve(buffer, (size_t)length)) {
        va_end(args);
        return 0;
    }
    vsnprintf_s(buffer->data + buffer->length,
                buffer->capacity - buffer->length, _TRUNCATE, format, args);
    va_end(args);
    buffer->length += (size_t)length;
    return 1;
}

int json_buffer_append_quoted_utf8(JSON_BUFFER *buffer, const char *text)
{
    static const char hex[] = "0123456789abcdef";

    if (!buffer || !text || !json_buffer_append_char(buffer, '"'))
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char escaped[6];
        switch (*p) {
        case '"': if (!json_buffer_append(buffer, "\\\"")) return 0; break;
        case '\\': if (!json_buffer_append(buffer, "\\\\")) return 0; break;
        case '\b': if (!json_buffer_append(buffer, "\\b")) return 0; break;
        case '\f': if (!json_buffer_append(buffer, "\\f")) return 0; break;
        case '\n': if (!json_buffer_append(buffer, "\\n")) return 0; break;
        case '\r': if (!json_buffer_append(buffer, "\\r")) return 0; break;
        case '\t': if (!json_buffer_append(buffer, "\\t")) return 0; break;
        default:
            if (*p < 0x20) {
                memcpy(escaped, "\\u00", 4);
                escaped[4] = hex[*p >> 4];
                escaped[5] = hex[*p & 0x0f];
                if (!json_buffer_append_n(buffer, escaped, sizeof(escaped)))
                    return 0;
            } else if (!json_buffer_append_char(buffer, (char)*p)) {
                return 0;
            }
        }
    }
    return json_buffer_append_char(buffer, '"');
}

int json_buffer_append_wstring(JSON_BUFFER *buffer, const wchar_t *text)
{
    char *utf8;
    int length;
    int result;

    if (!text)
        text = L"";
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                 NULL, 0, NULL, NULL);
    if (length <= 0)
        length = WideCharToMultiByte(CP_UTF8, 0, text, -1,
                                     NULL, 0, NULL, NULL);
    if (length <= 0)
        return json_buffer_append(buffer, "\"\"");
    utf8 = (char *)malloc((size_t)length);
    if (!utf8) {
        buffer->failed = 1;
        return 0;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, length, NULL, NULL);
    result = json_buffer_append_quoted_utf8(buffer, utf8);
    free(utf8);
    return result;
}
