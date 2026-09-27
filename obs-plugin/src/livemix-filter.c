/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "livemix-obs.h"

#include <stdlib.h>

static const char *filter_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("Filter.Name");
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	(void)source;
	return calloc(1, 1);
}

static void filter_destroy(void *data)
{
	free(data);
}

static obs_properties_t *filter_properties(void *data)
{
	(void)data;
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *warning = obs_properties_add_text(properties, "retired", obs_module_text("Filter.Retired"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
	return properties;
}

/* Keep old scenes readable. With no audio callback, the parent's audio passes through. */
struct obs_source_info livemix_master_filter = {
	.id = "livemix_master_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DEPRECATED,
	.get_name = filter_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_properties = filter_properties,
};
