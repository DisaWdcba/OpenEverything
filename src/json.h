#ifndef OPENEVERYTHING_JSON_H
#define OPENEVERYTHING_JSON_H

#include <stddef.h>
#include <wchar.h>

typedef enum {
    JSON_UNDEFINED = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_PRIMITIVE
} JSON_TYPE;

typedef struct {
    JSON_TYPE type;
    int start;
    int end;
    int size;
    int parent;
} JSON_TOKEN;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} JSON_BUFFER;

int json_parse(const char *json, size_t length, JSON_TOKEN *tokens, int capacity);
int json_object_get(const char *json, const JSON_TOKEN *tokens, int token_count,
                    int object_index, const char *key);
int json_token_equals(const char *json, const JSON_TOKEN *token, const char *value);
int json_token_to_int(const char *json, const JSON_TOKEN *token, int *value);
int json_token_to_bool(const char *json, const JSON_TOKEN *token, int *value);
wchar_t *json_token_to_wstring(const char *json, const JSON_TOKEN *token);

void json_buffer_init(JSON_BUFFER *buffer);
void json_buffer_free(JSON_BUFFER *buffer);
int json_buffer_append(JSON_BUFFER *buffer, const char *text);
int json_buffer_append_n(JSON_BUFFER *buffer, const char *text, size_t length);
int json_buffer_append_char(JSON_BUFFER *buffer, char ch);
int json_buffer_append_format(JSON_BUFFER *buffer, const char *format, ...);
int json_buffer_append_quoted_utf8(JSON_BUFFER *buffer, const char *text);
int json_buffer_append_wstring(JSON_BUFFER *buffer, const wchar_t *text);

#endif
