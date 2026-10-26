/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ring-names.h"
#include "lm_obs_protocol.h"
#include "lm_obs_metadata.h"

#include <stdbool.h>
#include <wchar.h>

static bool environment_name(const wchar_t *key, wchar_t name[128])
{
	DWORD length = GetEnvironmentVariableW(key, name, 128);
	return length > 0 && length < 128;
}

void lm_resolve_ring_names(lm_ring_names *names)
{
	if (!environment_name(L"LIVEMIX_OBS_RING", names->master))
		wcscpy(names->master, LM_OBS_RING_NAME);
	const wchar_t *keys[2] = {L"LIVEMIX_OBS_PEER_RING", L"LIVEMIX_OBS_PEER2_RING"};
	const wchar_t *suffixes[2] = {L".Peer", L".Peer2"};
	const wchar_t *defaults[2] = {LM_OBS_PEER_RING_NAME, LM_OBS_PEER2_RING_NAME};
	for (unsigned i = 0; i < 2; ++i) {
		if (environment_name(keys[i], names->peer[i]))
			continue;
		/* Contract test bases are <=104 UTF-16 units, leaving suffix space. */
		if (wcslen(names->master) + wcslen(suffixes[i]) < 128)
			swprintf(names->peer[i], 128, L"%ls%ls", names->master, suffixes[i]);
		else
			wcscpy(names->peer[i], defaults[i]);
	}
}
