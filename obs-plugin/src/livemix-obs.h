/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIVEMIX_OBS_H
#define LIVEMIX_OBS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <obs-module.h>
#include "receiver.h"

lm_connection *livemix_obs_connection(void);
lm_connection *livemix_obs_peer_connection(unsigned slot);
extern struct obs_source_info livemix_master_source;
extern struct obs_source_info livemix_master_filter;
extern struct obs_source_info livemix_peer_source;
extern struct obs_source_info livemix_peer2_source;

#endif
