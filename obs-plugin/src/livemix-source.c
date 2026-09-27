/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "livemix-obs.h"
#include <util/platform.h>

#include <stdlib.h>

#define LM_SOURCE_MAX_FRAMES 4096
#define LM_NANOSECONDS UINT64_C(1000000000)

typedef struct livemix_source {
	obs_source_t *source;
	lm_receiver *receiver;
	HANDLE thread;
	volatile LONG stop;
	float left[LM_SOURCE_MAX_FRAMES], right[LM_SOURCE_MAX_FRAMES];
} livemix_source;

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

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	livemix_source *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->source = source;
	audio_t *audio = obs_get_audio();
	uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
	s->receiver = lm_receiver_create(livemix_obs_connection(), LM_RECEIVER_SOURCE, rate);
	if (s->receiver)
		s->thread = CreateThread(NULL, 0, source_worker, s, 0, NULL);
	if (!s->thread) {
		source_destroy(s);
		return NULL;
	}
	return s;
}

static obs_properties_t *source_properties(void *data)
{
	livemix_source *s = data;
	obs_properties_t *properties = obs_properties_create();
	const char *key = s && lm_receiver_connected(s->receiver) ? "Status.Connected" : "Status.Waiting";
	obs_properties_add_text(properties, "status", obs_module_text(key), OBS_TEXT_INFO);
	return properties;
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
