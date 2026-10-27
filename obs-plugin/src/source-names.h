/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIVEMIX_OBS_SOURCE_NAMES_H
#define LIVEMIX_OBS_SOURCE_NAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LM_SOURCE_NAME_GRACE_NS UINT64_C(3000000000)

/* Existing ko-KR/en-US display names, including the pre-slot 0.15 name. */
static const char *const lm_peer_default_names[2][4] = {
	{"LiveMix 합방 상대 1", "LiveMix Collab Partner 1", "LiveMix 합방 상대", "LiveMix Collab Partner"},
	{"LiveMix 합방 상대 2", "LiveMix Collab Partner 2", "LiveMix 합방 상대", "LiveMix Collab Partner"},
};

typedef struct lm_source_name_request {
	const char *current_name, *base_name, *auto_name;
	const char *const *default_names;
	size_t default_count;
	bool connected;
	const char *nickname;
	uint64_t empty_ns;
	bool (*name_used)(void *context, const char *name);
	void *context;
} lm_source_name_request;

static inline bool lm_source_name_is_default(const lm_source_name_request *request)
{
	for (size_t i = 0; i < request->default_count; ++i) {
		const char *base = request->default_names[i];
		size_t length = strlen(base);
		if (strncmp(request->current_name, base, length) != 0)
			continue;
		const char *suffix = request->current_name + length;
		if (!*suffix)
			return true;
		/* OBS appends " 2", " 3", ... when adding another source. */
		if (*suffix++ != ' ' || *suffix < '1' || *suffix > '9')
			continue;
		const char *digits = suffix;
		while (*suffix >= '0' && *suffix <= '9')
			++suffix;
		if (!*suffix && (suffix - digits > 1 || *digits >= '2'))
			return true;
	}
	return false;
}

/* OBS-independent decision. Strings are non-NULL (empty means not saved).
 * The callback reports occupied names; current_name is always available to
 * this source. Returns a malloc-owned name, or NULL when no rename is needed.
 * After a rename, save the original base once and the returned name as auto,
 * including restorations. A saved auto mismatch must never reclaim a name. */
static inline char *lm_source_name_next(const lm_source_name_request *request)
{
	bool owned = *request->base_name && *request->auto_name;
	if (*request->base_name || *request->auto_name) {
		if (!owned || strcmp(request->current_name, request->auto_name) != 0)
			return NULL;
	} else if (!lm_source_name_is_default(request)) {
		return NULL;
	}

	const char *target;
	if (request->connected && *request->nickname) {
		target = request->nickname;
	} else if (owned && request->empty_ns > LM_SOURCE_NAME_GRACE_NS) {
		target = request->base_name;
	} else {
		return NULL;
	}
	if (strcmp(target, request->current_name) == 0)
		return NULL;

	size_t capacity = strlen(target) + 32;
	char *name = malloc(capacity);
	if (!name)
		return NULL;
	strcpy(name, target);
	for (size_t suffix = 2; strcmp(name, request->current_name) != 0 && request->name_used &&
				request->name_used(request->context, name); ++suffix)
		snprintf(name, capacity, "%s (%zu)", target, suffix);
	if (strcmp(name, request->current_name) == 0) {
		free(name);
		return NULL;
	}
	return name;
}

#endif
