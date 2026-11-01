#ifndef OPENEVERYTHING_LANG_H
#define OPENEVERYTHING_LANG_H

#include "common.h"

/* Runtime UI localisation.
 *
 * The English UI string is the lookup key: lang_tr(L"Open") returns the
 * translation for the current language, or L"Open" itself when the string is
 * not translated.  That keeps every caller working even when a language file
 * is missing, truncated, or does not cover a newly added string.
 *
 * Built-in languages (English and Simplified Chinese) always exist.  An
 * optional external file extends or overrides them:
 *
 *     %LOCALAPPDATA%\OpenEverything\languages.ini
 *
 * Format (UTF-8 with or without BOM, or UTF-16LE/BE with BOM):
 *
 *     [Languages]
 *     en=English
 *     zh-CN=简体中文
 *     ja=日本語
 *
 *     [ja]
 *     Open=開く
 *     File=ファイル
 *
 * Built-in and external entries must never disappear just because the file is
 * absent; missing keys fall back to the English key text.
 */

#define LANG_ID_MAX 32
#define LANG_NAME_MAX 64
#define LANG_MAX_LANGUAGES 32

typedef struct {
    wchar_t id[LANG_ID_MAX];
    wchar_t native_name[LANG_NAME_MAX];
    int builtin;
} LANG_ENTRY;

/* Load built-in languages (no external file yet). Safe to call once. */
void lang_init(void);

/* Re-read the external language file and merge it over the built-ins.
   Returns the number of external languages now known. */
int lang_reload(void);

int lang_count(void);
const LANG_ENTRY *lang_entry(int index);

/* Returns an index >= 0 when the id exists, otherwise -1. */
int lang_find(const wchar_t *id);

/* Select a language by index. Returns 1 on success, 0 for an invalid index.
   This only changes the current language; callers persist it in config. */
int lang_set(int index);

int lang_current(void);
const wchar_t *lang_current_id(void);

/* The best built-in language for the current Windows UI language. */
int lang_system_default(void);

/* Translate an English UI string. Never returns NULL; returns the input (or
   L"" for NULL) when there is no translation. */
const wchar_t *lang_tr(const wchar_t *english);

/* Path of the external language file. */
void lang_get_file_path(wchar_t *buf, size_t size);

/* 1 when an external file was read on the last reload. */
int lang_external_loaded(void);

#define L(s) lang_tr(L##s)

#endif /* OPENEVERYTHING_LANG_H */
