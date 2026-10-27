/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* validate.h: CLI configuration sanity checks. */

#ifndef PACT_VALIDATE_H
#define PACT_VALIDATE_H

#include "config.h"

/* Validate the parsed pact_config_t. Emits warnings to stderr for non-
 * fatal issues; returns 0 if acceptable, -1 if fatally inconsistent. */
int pact_validate_configuration(const pact_config_t *config);

#endif /* PACT_VALIDATE_H */
