/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* cli.h: argv parsing for PACT runtime.
 *
 * CLI module: config.c, usage.c, validate.c, cli.c.
 */

#ifndef PACT_CLI_H
#define PACT_CLI_H

#include "config.h"

/* Parse argc/argv into config. Returns:
 *   0   parse OK; caller proceeds.
 *   1   help requested (printed to stdout); caller should exit(0).
 *  -1   parse error (already printed to stderr); caller should exit(1). */
int pact_parse_command_line_args(int argc, char *argv[], pact_config_t *config);

#endif /* PACT_CLI_H */
