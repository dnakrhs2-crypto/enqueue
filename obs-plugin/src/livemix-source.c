/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "livemix-obs.h"
#include "source-names.h"
#include <util/platform.h>
#include <util/dstr.h>

#include <stdlib.h>

#define LM_SOURCE_MAX_FRAMES 4096
#define LM_NANOSECONDS UINT64_C(1000000000)
#define LM_NAME_POLL_NS (LM_NANOSECONDS / 4)
#define LM_BASE_NAME "base_name"
#define LM_AUTO_NAME "auto_name"

typedef struct livemix_source {
	obs_source_t *source;
	lm_receiver *receiver;
	lm_connection *connection;
	unsigned slot; /* 0 master, 1/2 collab partner */
	HANDLE thread;
	volatile LONG stop;
	uint64_t name_last_poll; /* Video thread only. */
	volatile LONG name_task_pending;
	uint64_t name_empty_since; /* UI thread only; never persisted across OBS runs. */
	float left[LM_SOURCE_MAX_FRAMES], right[LM_SOURCE_MAX_FRAMES];
} livemix_source;

static bool source_name_used(void *context, const char *name)
{
	obs_source_t *other = obs_get_source_by_name(name);
	bool used = other && other != context;
	obs_source_release(other);
	return used;
}

static void peer_source_update_name(livemix_source *s)
{
	lm_obs_metadata metadata;
	bool connected = lm_connection_read_metadata(s->connection, s->slot, &metadata) && metadata.participant_id &&
			 metadata.state == LM_OBS_PARTICIPANT_CONNECTED && metadata.nickname_bytes;
	uint64_t now = os_gettime_ns();
	if (connected)
		s->name_empty_since = 0;
	else if (!s->name_empty_since)
		s->name_empty_since = now;

	const char *const *known = lm_peer_default_names[s->slot - 1];
	const char *defaults[] = {obs_module_text(s->slot == 1 ? "PeerSource.Name" : "Peer2Source.Name"),
				 known[0], known[1], known[2], known[3]};
	obs_data_t *settings = obs_source_get_settings(s->source);
	lm_source_name_request request = {
		.current_name = obs_source_get_name(s->source),
		.base_name = obs_data_get_string(settings, LM_BASE_NAME),
		.auto_name = obs_data_get_string(settings, LM_AUTO_NAME),
		.default_names = defaults,
		.default_count = sizeof(defaults) / sizeof(defaults[0]),
		.connected = connected,
		.nickname = metadata.nickname_utf8,
		.empty_ns = connected ? 0 : now - s->name_empty_since,
		.name_used = source_name_used,
		.context = s->source,
	};
	char *name = lm_source_name_next(&request);
	if (name) {
		if (!*request.base_name)
			obs_data_set_string(settings, LM_BASE_NAME, request.current_name);
		/* Set these before rename signals schedule the scene collection save. */
		obs_data_set_string(settings, LM_AUTO_NAME, name);
		obs_source_set_name(s->source, name);
		free(name);
	}
	obs_data_release(settings);
}

static void peer_source_name_task(void *opaque)
{
	obs_weak_source_t *weak = opaque;
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;
	livemix_source *s = obs_obj_get_data(source);
	if (s) {
		if (!obs_source_removed(source))
			peer_source_update_name(s);
		InterlockedExchange(&s->name_task_pending, 0);
	}
	obs_source_release(source);
}

static void peer_source_tick(void *data, float seconds)
{
	livemix_source *s = data;
	(void)seconds;
	uint64_t now = os_gettime_ns();
	if (now - s->name_last_poll < LM_NAME_POLL_NS)
		return;
	s->name_last_poll = now;
	if (obs_source_removed(s->source) || InterlockedCompareExchange(&s->name_task_pending, 1, 0))
		return;
	/* OBS ticks audio-only sources too. Only the UI task reads metadata and
	 * changes names/settings; queued work holds no source or instance pointer.
	 * A strong reference obtained from the weak ref keeps data alive in the task. */
	obs_queue_task(OBS_TASK_UI, peer_source_name_task, obs_source_get_weak_source(s->source), false);
}

static uint64_t frames_to_ns(uint64_t frames, uint32_t rate)
{
	return (frames / rate) * LM_NANOSECONDS + (frames % rate) * LM_NANOSECONDS / rate;
}

static DWORD WINAPI source_worker(void *opaque)
{
	livemix_source *s = opaque;
	uint32_t rate = 0;
	uint64_t t0 = os_gettime_ns(), sent = 0;
	bool connected = false;
	while (!InterlockedCompareExchange(&s->stop, 0, 0)) {
		audio_t *obs_audio = obs_get_audio();
		uint32_t current_rate = obs_audio ? audio_output_get_sample_rate(obs_audio) : 0;
		if (current_rate < 8000 || current_rate > 384000) {
			os_sleep_ms(10);
			continue;
		}
		if (rate != current_rate) {
			rate = current_rate;
			t0 = os_gettime_ns();
			sent = 0;
		}
		uint32_t frames = rate / 100;
		uint64_t deadline = t0 + frames_to_ns(sent + frames, rate);
		os_sleepto_ns(deadline);
		if (InterlockedCompareExchange(&s->stop, 0, 0))
			break;
		uint64_t now = os_gettime_ns();
		if (now > deadline && now - deadline > LM_NANOSECONDS / 10) {
			/* Drop missed periods: the next iteration is one period away. */
			t0 = now - frames_to_ns(frames, rate);
			sent = 0;
			lm_receiver_resync(s->receiver);
		}
		lm_receiver_pull(s->receiver, s->left, s->right, (int)frames, rate);
		/* Re-read the layout each period. Submit mono ourselves so OBS's
		 * stereo downmix cannot add 3 dB to a centred L=R signal. */
		obs_audio = obs_get_audio();
		size_t channels = obs_audio ? audio_output_get_channels(obs_audio) : 2;
		size_t planes = lm_mix_stereo_for_layout(s->left, s->right, (int)frames, channels);
		struct obs_source_audio audio = {0};
		audio.data[0] = (const uint8_t *)s->left;
		if (planes == 2)
			audio.data[1] = (const uint8_t *)s->right;
		audio.frames = frames;
		audio.speakers = planes == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = rate;
		audio.timestamp = t0 + frames_to_ns(sent, rate);
		obs_source_output_audio(s->source, &audio);
		sent += frames;
		bool current_connected = lm_receiver_connected(s->receiver);
		if (connected != current_connected) {
			connected = current_connected;
			obs_source_update_properties(s->source);
		}
	}
	return 0;
}

static const char *source_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Source.Name");
}

static const char *peer_source_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("PeerSource.Name");
}

static const char *peer2_source_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Peer2Source.Name");
}

static void source_destroy(void *data)
{
	livemix_source *s = data;
	if (!s)
		return;
	if (s->thread) {
		InterlockedExchange(&s->stop, 1);
		WaitForSingleObject(s->thread, INFINITE);
		CloseHandle(s->thread);
	}
	lm_receiver_destroy(s->receiver);
	free(s);
}

static void *source_create_for_connection(obs_source_t *source, lm_connection *connection, unsigned slot)
{
	livemix_source *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->source = source;
	s->connection = connection;
	s->slot = slot;
	audio_t *audio = obs_get_audio();
	uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
	s->receiver = lm_receiver_create(connection, rate);
	if (s->receiver)
		s->thread = CreateThread(NULL, 0, source_worker, s, 0, NULL);
	if (!s->thread) {
		source_destroy(s);
		return NULL;
	}
	return s;
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source_create_for_connection(source, livemix_obs_connection(), 0);
}

static void *peer_source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source_create_for_connection(source, livemix_obs_peer_connection(1), 1);
}

static void *peer2_source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source_create_for_connection(source, livemix_obs_peer_connection(2), 2);
}

static obs_properties_t *source_properties_for_status(void *data, const char *connected_key, const char *waiting_key)
{
	livemix_source *s = data;
	obs_properties_t *properties = obs_properties_create();
	bool connected = s && lm_receiver_connected(s->receiver);
	struct dstr status = {0};
	dstr_copy(&status, obs_module_text(connected ? connected_key : waiting_key));
	lm_obs_metadata metadata;
	if (connected && s->slot && lm_connection_read_metadata(s->connection, s->slot, &metadata) &&
	    metadata.participant_id && metadata.state == LM_OBS_PARTICIPANT_CONNECTED && metadata.nickname_bytes) {
		dstr_cat(&status, " \xc2\xb7 ");
		dstr_cat(&status, metadata.nickname_utf8);
	}
	obs_properties_add_text(properties, "status", status.array, OBS_TEXT_INFO);
	dstr_free(&status);
	return properties;
}

static obs_properties_t *source_properties(void *data)
{
	return source_properties_for_status(data, "Status.Connected", "Status.Waiting");
}

static obs_properties_t *peer_source_properties(void *data)
{
	return source_properties_for_status(data, "PeerStatus.Connected", "PeerStatus.Waiting");
}

struct obs_source_info livemix_master_source = {
	.id = "livemix_master_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = source_name,
	.create = source_create,
	.destroy = source_destroy,
	.get_properties = source_properties,
};

struct obs_source_info livemix_peer_source = {
	.id = "livemix_peer_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = peer_source_name,
	.create = peer_source_create,
	.destroy = source_destroy,
	.video_tick = peer_source_tick,
	.get_properties = peer_source_properties,
};

struct obs_source_info livemix_peer2_source = {
	.id = "livemix_peer2_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = peer2_source_name,
	.create = peer2_source_create,
	.destroy = source_destroy,
	.video_tick = peer_source_tick,
	.get_properties = peer_source_properties,
};
