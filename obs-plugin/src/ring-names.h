/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIVEMIX_OBS_RING_NAMES_H
#define LIVEMIX_OBS_RING_NAMES_H

#include <stddef.h>

typedef struct lm_ring_names {
	wchar_t master[128];
	wchar_t peer[2][128];
} lm_ring_names;

/* Control/startup path. Explicit nonempty peer overrides win over base suffixes. */
void lm_resolve_ring_names(lm_ring_names *names);

#endif
