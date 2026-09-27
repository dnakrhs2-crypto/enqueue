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
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("livemix-obs", "en-US")

static lm_connection *connection;

lm_connection *livemix_obs_connection(void)
{
	return connection;
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
	lm_connection_config config = {0};
	config.log = receiver_log;
	/* Test instances only: LIVEMIX_OBS_RING=<name> reads another ring (a fake LiveMix) so a long unattended test never
	 * shares the real ring with a LiveMix running on the same PC. Unset in normal use. */
	wchar_t test_ring[128];
	DWORD test_ring_length = GetEnvironmentVariableW(L"LIVEMIX_OBS_RING", test_ring, 128);
	if (test_ring_length > 0 && test_ring_length < 128) {
		config.ring_name = test_ring;
		blog(LOG_INFO, "[livemix-obs] Test ring from LIVEMIX_OBS_RING");
	}
	connection = lm_connection_create(&config);
	if (!connection || !lm_connection_start(connection)) {
		lm_connection_destroy(connection);
		connection = NULL;
		blog(LOG_ERROR, "[livemix-obs] Could not start the connection manager");
		return false;
	}
	obs_register_source(&livemix_master_source);
	obs_register_source(&livemix_master_filter);
	blog(LOG_INFO, "[livemix-obs] Loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	lm_connection_destroy(connection);
	connection = NULL;
}
