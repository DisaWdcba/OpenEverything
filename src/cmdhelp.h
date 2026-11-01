#ifndef OPENEVERYTHING_CMDHELP_H
#define OPENEVERYTHING_CMDHELP_H

#include "common.h"

/* Help > Command Line Options.
 *
 * cmdhelp_text() returns the complete, merged documentation: the
 * Everything-compatible OpenEverything.exe options plus the OpenEverything
 * CLI/MCP commands that already existed (`search`, `stats`, `update`, `mcp`
 * and their `--` options).  The returned pointer is static and must not be
 * freed.
 *
 * cmdhelp_show() displays that text in a scrollable, read-only modal window
 * owned by `owner`.  `title` and `close_label` are supplied by the caller so
 * they can be localised.  Either may be NULL for the English defaults.
 */
const wchar_t *cmdhelp_text(void);
void cmdhelp_show(HWND owner, const wchar_t *title, const wchar_t *close_label);

#endif /* OPENEVERYTHING_CMDHELP_H */
