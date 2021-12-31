/* pipewire-virtualcam.c
 *
 * Copyright 2021 columbarius <co1umbarius@protonmail.com>
 *
 * This code is heavily inspired by the pipewire-capture in the
 * linux-capture plugin done by
 * Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/debug/format.h>

#include <obs-module.h>
#include <obs-output.h>
#include <stdint.h>

#include "pipewire-common.h"

#define OBS_PWVC_ALIGN 16
#define OBS_PWVC_BUFFERS 4

struct obs_pipewire_formatinfo {
	enum video_format obs_format;
	enum spa_video_format pw_format;
	uint32_t bpp; // bytes per pixel
	uint32_t width;
	uint32_t height;
	uint32_t planes;
	uint32_t strides[MAX_AV_PLANES];
	uint32_t sizes[MAX_AV_PLANES];
};

struct obs_pipewire_virtualcam_data {
	obs_output_t *output;

	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	uint32_t node_id;
	bool pw_stream_state;
	uint32_t seq;

	struct spa_video_info_raw pw_format;
	struct obs_pipewire_formatinfo formatinfo;
};

/****************************************************************************************/

static const struct {
	uint32_t spa_format;
	enum video_format obs_format;
	uint32_t bpp;
	uint32_t planes;
	const char *pretty_name;
} supported_media_formats[] = {{
				       SPA_VIDEO_FORMAT_YUY2,
				       VIDEO_FORMAT_YUY2,
				       2,
				       1,
				       "BLA2",
			       },
			       {
				       SPA_VIDEO_FORMAT_RGBA,
				       VIDEO_FORMAT_RGBA,
				       4,
				       1,
				       "BLA",
			       }};

#define N_SUPPORTED_MEDIA_FORMATS \
	(sizeof(supported_media_formats) / sizeof(supported_media_formats[0]))

static bool
get_obs_formatinfo_from_pw_format(struct obs_pipewire_formatinfo *formatinfo,
				  struct spa_video_info_raw *pw_video_info)
{
	for (size_t i = 0; i < N_SUPPORTED_MEDIA_FORMATS; i++) {
		if (pw_video_info->format !=
		    supported_media_formats[i].spa_format)
			continue;

		formatinfo->pw_format = pw_video_info->format;
		formatinfo->obs_format = supported_media_formats[i].obs_format;
		formatinfo->bpp = supported_media_formats[i].bpp;
		formatinfo->width = pw_video_info->size.width;
		formatinfo->height = pw_video_info->size.height;
		formatinfo->planes = supported_media_formats[i].planes;

		switch (formatinfo->obs_format) {
		default:
			formatinfo->strides[0] = SPA_ROUND_UP_N(
				formatinfo->width * formatinfo->bpp, 4);
			formatinfo->sizes[0] =
				formatinfo->height * formatinfo->strides[0];
		}
		return true;
	}
	return false;
}

/***********************************************************************************/

static struct spa_pod *build_format(struct spa_pod_builder *b, uint32_t width,
				    uint32_t height, uint32_t format)
{
	struct spa_pod_frame f[2];

	/* make an object of type SPA_TYPE_OBJECT_Format and id SPA_PARAM_EnumFormat.
	* The object type is important because it defines the properties that are
	* acceptable. The id gives more context about what the object is meant to
	* contain. In this case we enumerate supported formats. */
	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format,
				    SPA_PARAM_EnumFormat);
	/* add media type and media subtype properties */
	spa_pod_builder_prop(b, SPA_FORMAT_mediaType, 0);
	spa_pod_builder_id(b, SPA_MEDIA_TYPE_video);
	spa_pod_builder_prop(b, SPA_FORMAT_mediaSubtype, 0);
	spa_pod_builder_id(b, SPA_MEDIA_SUBTYPE_raw);

	/* formats */
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
	spa_pod_builder_id(b, format);

	/* add size and framerate ranges */
	spa_pod_builder_add(
		b, SPA_FORMAT_VIDEO_size,
		SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(width, height),
					       &SPA_RECTANGLE(640, 480),
					       &SPA_RECTANGLE(width, height)),
		SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

static bool build_format_params(struct obs_pipewire_virtualcam_data *obs_pwvc,
				struct spa_pod_builder *pod_builder,
				const struct spa_pod ***param_list,
				uint32_t *n_params)
{
	uint32_t params_count = 0;

	const struct spa_pod **params;
	params = bzalloc(N_SUPPORTED_MEDIA_FORMATS * sizeof(struct spa_pod *));

	if (!params) {
		blog(LOG_ERROR,
		     "[pipewire] Failed to allocate memory for param pointers");
		return false;
	}

	int width = obs_output_get_width(obs_pwvc->output);
	int height = obs_output_get_height(obs_pwvc->output);

	for (size_t i = 0; i < N_SUPPORTED_MEDIA_FORMATS; i++) {
		params[params_count++] =
			build_format(pod_builder, width, height,
				     supported_media_formats[i].spa_format);
	}
	*param_list = params;
	*n_params = params_count;
	return true;
}

static void teardown_pipewire(struct obs_pipewire_virtualcam_data *obs_pwvc)
{
	if (obs_pwvc->thread_loop) {
		pw_thread_loop_wait(obs_pwvc->thread_loop);
		pw_thread_loop_stop(obs_pwvc->thread_loop);
	}
	if (obs_pwvc->stream) {
		pw_stream_disconnect(obs_pwvc->stream);
		pw_stream_destroy(obs_pwvc->stream);
	}
	if (obs_pwvc->context)
		pw_context_destroy(obs_pwvc->context);
	if (obs_pwvc->thread_loop)
		pw_thread_loop_destroy(obs_pwvc->thread_loop);
}

/***********************************************************************************/

static void on_state_changed_cb(void *user_data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);
	UNUSED_PARAMETER(error);

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)user_data;

	obs_pwvc->node_id = pw_stream_get_node_id(obs_pwvc->stream);

	blog(LOG_DEBUG, "[pipewire] stream %p state: \"%s\" (error: %s)",
	     obs_pwvc->stream, pw_stream_state_as_string(state),
	     error ? error : "none");

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		if (old == PW_STREAM_STATE_CONNECTING)
			blog(LOG_INFO,
			     "[pipewire] Virtual camera connected (%d)",
			     (int)obs_pwvc->node_id);
		obs_pwvc->pw_stream_state = false;
		break;
	case PW_STREAM_STATE_STREAMING:
		obs_pwvc->pw_stream_state = true;
		obs_output_begin_data_capture(obs_pwvc->output,
					      OBS_OUTPUT_VIDEO);
		break;
	default:
		obs_pwvc->pw_stream_state = false;
		break;
	}
}

static void on_param_changed_cb(void *data, uint32_t id,
				const struct spa_pod *param)
{
	blog(LOG_DEBUG, "[pipewire]: param_changed callback");

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];
	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &obs_pwvc->pw_format);
	spa_debug_format(2, NULL, param);

	if (!get_obs_formatinfo_from_pw_format(&obs_pwvc->formatinfo,
					       &obs_pwvc->pw_format)) {
		blog(LOG_ERROR, "[pipewire]: unsupported format");
	}

	struct video_scale_info vsi = {0};
	vsi.format = obs_pwvc->formatinfo.obs_format;
	vsi.width = obs_pwvc->formatinfo.width;
	vsi.height = obs_pwvc->formatinfo.height;
	obs_output_set_video_conversion(obs_pwvc->output, &vsi);

	int blocks = obs_pwvc->formatinfo.planes;
	int size = 0;
	for (size_t i = 0; i < obs_pwvc->formatinfo.planes; i++)
		size = SPA_MAX(size, obs_pwvc->formatinfo.sizes[i]);
	int stride = 0;
	for (size_t i = 0; i < obs_pwvc->formatinfo.planes; i++)
		stride = SPA_MAX(stride, obs_pwvc->formatinfo.strides[i]);
	int buffertypes = (1 << SPA_DATA_MemPtr);

	params[0] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
		SPA_POD_CHOICE_RANGE_Int(OBS_PWVC_BUFFERS, 1, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
		SPA_PARAM_BUFFERS_align, SPA_POD_Int(OBS_PWVC_ALIGN),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffertypes));

	params[1] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size,
		SPA_POD_Int(sizeof(struct spa_meta_header)));

	blog(LOG_DEBUG, "[pipewire]: params updated");
	pw_stream_update_params(obs_pwvc->stream, params, 2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
};

/**********************************************************************/

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res,
			     const char *message)
{
	UNUSED_PARAMETER(seq);

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)user_data;

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s", id,
	     seq, res, strerror(res), message);

	pw_thread_loop_signal(obs_pwvc->thread_loop, false);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq)
{
	UNUSED_PARAMETER(seq);

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)user_data;

	if (id == PW_ID_CORE)
		pw_thread_loop_signal(obs_pwvc->thread_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

/****************************************************************************************/

static const char *virtualcam_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PipeWireVirtualCamera");
}

static void virtualcam_destroy(void *data)
{
	blog(LOG_INFO, "Virtual camera destroyed");
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;
	bfree(obs_pwvc);
}

static void *virtualcam_create(obs_data_t *settings, obs_output_t *output)
{
	blog(LOG_INFO, "Virtual camera created");

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)bzalloc(
			sizeof(struct obs_pipewire_virtualcam_data));
	obs_pwvc->output = output;
	UNUSED_PARAMETER(settings);
	return obs_pwvc;
}

static bool virtualcam_start(void *data)
{
	blog(LOG_INFO, "Virtual camera started");
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;

	const struct spa_pod **params;
	uint32_t n_params;
	uint8_t buffer[1024];

	obs_pwvc->thread_loop =
		pw_thread_loop_new("PipeWire thread loop", NULL);
	obs_pwvc->context = pw_context_new(
		pw_thread_loop_get_loop(obs_pwvc->thread_loop), NULL, 0);

	if (pw_thread_loop_start(obs_pwvc->thread_loop) < 0) {
		blog(LOG_WARNING, "Error starting threaded mainloop");
		goto error;
	}

	pw_thread_loop_lock(obs_pwvc->thread_loop);

	/* Core */
	obs_pwvc->core = pw_context_connect(obs_pwvc->context, NULL, 0);
	if (!obs_pwvc->core) {
		blog(LOG_WARNING, "Error creating PipeWire core: %m");
		pw_thread_loop_unlock(obs_pwvc->thread_loop);
		goto error;
	}

	pw_core_add_listener(obs_pwvc->core, &obs_pwvc->core_listener,
			     &core_events, obs_pwvc);

	/* Stream */
	obs_pwvc->stream = pw_stream_new(
		obs_pwvc->core, "OBS Studio",
		pw_properties_new(PW_KEY_NODE_DESCRIPTION, "OBS Virtual Camera",
				  PW_KEY_MEDIA_CLASS, "Video/Source",
				  PW_KEY_MEDIA_ROLE, "Camera", NULL));
	pw_stream_add_listener(obs_pwvc->stream, &obs_pwvc->stream_listener,
			       &stream_events, obs_pwvc);
	blog(LOG_INFO, "[pipewire] created stream %p", obs_pwvc->stream);

	/* Stream parameters */

	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	if (!build_format_params(obs_pwvc, &b, &params, &n_params)) {
		blog(LOG_WARNING, "Failed to create format params");
		pw_thread_loop_unlock(obs_pwvc->thread_loop);
		goto error;
	};

	pw_stream_connect(obs_pwvc->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			  PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
			  params, n_params);

	blog(LOG_INFO, "[pipewire] output started");

	pw_thread_loop_unlock(obs_pwvc->thread_loop);

	bfree(params);
	return true;
error:
	blog(LOG_WARNING, "Failed to start virtual camera");
	return false;
}

static void virtualcam_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	blog(LOG_INFO, "Virtual camera stopped");
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;

	obs_output_end_data_capture(obs_pwvc->output);
	teardown_pipewire(obs_pwvc);
}

static void virtual_video(void *data, struct video_data *frame)
{
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;

	// check if we have a running pipewire stream
	if (!obs_pwvc->pw_stream_state) {
		// blog(LOG_INFO, "No node connected");
		return;
	}

	blog(LOG_DEBUG, "exporting frame to pipewire");
	// get buffer
	struct pw_buffer *pw_buf;
	if ((pw_buf = pw_stream_dequeue_buffer(obs_pwvc->stream)) == NULL) {
		blog(LOG_WARNING, "pipewire: out of buffers");
		return;
	}

	struct spa_buffer *spa_buf = pw_buf->buffer;
	struct spa_data *d = spa_buf->datas;

	if (spa_buf->n_datas != obs_pwvc->formatinfo.planes) {
		blog(LOG_INFO,
		     "pipewire: number of buffer planes is different");
		return;
	}
	// copy data
	for (unsigned int i = 0; i < obs_pwvc->formatinfo.planes; i++) {
		if (d[i].data == NULL) {
			blog(LOG_WARNING, "pipewire: buffer not mapped");
			continue;
		}
		memcpy(d[i].data, frame->data[i],
		       obs_pwvc->formatinfo.sizes[i]);
		d[i].mapoffset = 0;
		d[i].maxsize = obs_pwvc->formatinfo.sizes[i];
		d[i].flags = SPA_DATA_FLAG_READABLE;
		d[i].type = SPA_DATA_MemPtr;
		d[i].chunk->offset = 0;
		d[i].chunk->stride = obs_pwvc->formatinfo.strides[i];
		d[i].chunk->size = obs_pwvc->formatinfo.sizes[i];
	}

	// add metadata
	struct spa_meta_header *h;
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header,
					   sizeof(*h)))) {
		h->pts = -1;
		h->flags = 0;
		h->seq = obs_pwvc->seq++;
		h->dts_offset = 0;
	}

	// return buffer

	blog(LOG_DEBUG, "********************");
	blog(LOG_DEBUG, "pipewire: fd %lu", d[0].fd);
	blog(LOG_DEBUG, "pipewire: dataptr %p", d[0].data);
	blog(LOG_DEBUG, "pipewire: size %d", d[0].maxsize);
	blog(LOG_DEBUG, "pipewire: stride %d", d[0].chunk->stride);
	blog(LOG_DEBUG, "pipewire: width %d", obs_pwvc->formatinfo.width);
	blog(LOG_DEBUG, "pipewire: height %d", obs_pwvc->formatinfo.height);
	blog(LOG_DEBUG, "********************");

	pw_stream_queue_buffer(obs_pwvc->stream, pw_buf);
}

void virtual_cam_register_output(void)
{
	struct obs_output_info pipewire_virtualcam_info = {
		.id = "pw_vcam_output",
		.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_VIRTUALCAM,
		.get_name = virtualcam_name,
		.create = virtualcam_create,
		.destroy = virtualcam_destroy,
		.start = virtualcam_start,
		.stop = virtualcam_stop,
		.raw_video = virtual_video,
	};

	obs_register_output(&pipewire_virtualcam_info);
}
