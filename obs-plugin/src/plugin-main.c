/*
LiveMix for OBS
Copyright (C) 2026 LiveMix contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "livemix-obs.h"
#include "ring-names.h"
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("livemix-obs", "en-US")

static lm_connection *connection;
static lm_connection *peer_connections[2];

lm_connection *livemix_obs_connection(void)
{
	return connection;
}

lm_connection *livemix_obs_peer_connection(unsigned slot)
{
	return slot >= 1 && slot <= 2 ? peer_connections[slot - 1] : NULL;
}

static void receiver_log(void *context, enum lm_log_level level, const char *format, const char *message)
{
	(void)context;
	blog(level == LM_LOG_WARNING ? LOG_WARNING : LOG_INFO, format, message);
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "LiveMix master audio for OBS";
}

bool obs_module_load(void)
{
	lm_ring_names names;
	lm_resolve_ring_names(&names);
	lm_connection_config config = {0};
	config.log = receiver_log;
	config.ring_name = names.master;
	connection = lm_connection_create(&config);
	if (!connection || !lm_connection_start(connection)) {
		lm_connection_destroy(connection);
		connection = NULL;
		blog(LOG_ERROR, "[livemix-obs] Could not start the connection manager");
	}
	for (unsigned i = 0; i < 2; ++i) {
		lm_connection_config peer_config = {0};
		peer_config.ring_name = names.peer[i];
		peer_config.log = receiver_log;
		peer_connections[i] = lm_connection_create(&peer_config);
		if (!peer_connections[i] || !lm_connection_start(peer_connections[i])) {
			lm_connection_destroy(peer_connections[i]);
			peer_connections[i] = NULL;
			blog(LOG_ERROR, "[livemix-obs] Could not start peer %u connection manager", i + 1);
		}
	}
	if (!connection && !peer_connections[0] && !peer_connections[1])
		return false;
	obs_register_source(&livemix_master_source);
	obs_register_source(&livemix_master_filter);
	obs_register_source(&livemix_peer_source);
	obs_register_source(&livemix_peer2_source);
	blog(LOG_INFO, "[livemix-obs] Loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	lm_connection_destroy(connection);
	connection = NULL;
	for (unsigned i = 0; i < 2; ++i) {
		lm_connection_destroy(peer_connections[i]);
		peer_connections[i] = NULL;
	}
}
