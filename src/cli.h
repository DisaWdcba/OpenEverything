#ifndef OPENEVERYTHING_CLI_H
#define OPENEVERYTHING_CLI_H

#include <wchar.h>

int oe_cli_should_run(int argc, wchar_t **argv);
int oe_cli_run(int argc, wchar_t **argv);

#endif
