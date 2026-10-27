/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* usage.h: CLI usage/help printer. */

#ifndef PACT_USAGE_H
#define PACT_USAGE_H

/* Print full --help text to stdout. */
void pact_print_usage(const char *prog_name);

/* Print git version + build timestamp from generated src/build-info.h. */
void pact_print_version(void);

#endif /* PACT_USAGE_H */
