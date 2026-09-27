/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "livemix-obs.h"

#include <stdlib.h>
#include <string.h>

#define LM_FILTER_FRAMES 4096
enum filter_mode { LM_REPLACE = 0, LM_MIX = 1 };

typedef struct livemix_filter {
	lm_receiver *receiver;
	volatile LONG mode;
	float left[LM_FILTER_FRAMES], right[LM_FILTER_FRAMES];
} livemix_filter;

static const char *filter_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Filter.Name");
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", LM_REPLACE);
}

static void filter_update(void *data, obs_data_t *settings)
{
	livemix_filter *f = data;
	InterlockedExchange(&f->mode, obs_data_get_int(settings, "mode") == LM_MIX ? LM_MIX : LM_REPLACE);
}

static void filter_destroy(void *data)
{
	livemix_filter *f = data;
	if (!f)
		return;
	lm_receiver_destroy(f->receiver);
	free(f);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	livemix_filter *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	audio_t *audio = obs_get_audio();
	uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
	f->receiver = lm_receiver_create(livemix_obs_connection(), LM_RECEIVER_FILTER, rate);
	if (!f->receiver) {
		filter_destroy(f);
		return NULL;
	}
	filter_update(f, settings);
	return f;
}

static obs_properties_t *filter_properties(void *data)
{
	(void)data;
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(properties, "mode", obs_module_text("Filter.Mode"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Filter.Mode.Replace"), LM_REPLACE);
	obs_property_list_add_int(mode, obs_module_text("Filter.Mode.Mix"), LM_MIX);
	return properties;
}

static struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
	livemix_filter *f = data;
	audio_t *output = obs_get_audio();
	uint32_t rate = output ? audio_output_get_sample_rate(output) : 48000;
	size_t channels = output ? audio_output_get_channels(output) : 2;
	if (channels > MAX_AV_PLANES)
		channels = MAX_AV_PLANES;
	bool mix = InterlockedCompareExchange(&f->mode, 0, 0) == LM_MIX;
	for (uint32_t offset = 0; offset < audio->frames;) {
		uint32_t frames = audio->frames - offset;
		if (frames > LM_FILTER_FRAMES)
			frames = LM_FILTER_FRAMES;
		lm_receiver_pull(f->receiver, f->left, f->right, (int)frames, rate);
		lm_mix_stereo_for_layout(f->left, f->right, (int)frames, channels);
		for (size_t channel = 0; channel < channels; ++channel) {
			if (!audio->data[channel])
				continue;
			float *plane = (float *)audio->data[channel] + offset;
			if (channel >= 2) {
				if (!mix)
					memset(plane, 0, (size_t)frames * sizeof(float));
				continue;
			}
			for (uint32_t i = 0; i < frames; ++i) {
				float sample = channel == 0 ? f->left[i] : f->right[i];
				plane[i] = mix ? plane[i] + sample : sample;
			}
		}
		offset += frames;
	}
	return audio;
}

struct obs_source_info livemix_master_filter = {
	.id = "livemix_master_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = filter_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.update = filter_update,
	.filter_audio = filter_audio,
};
