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
#include <spa/debug/format.h>

#include <obs-module.h>
#include <obs-output.h>

#include "pipewire-common.h"

#define OBS_PWVC_ALIGN 16
#define OBS_PWVC_BUFFERS 4

struct obs_pipewire_virtualcam_data {
	obs_output_t *output;

	struct obs_pw_core pw_core;
	struct obs_pw_stream pw_stream;

	int device;

	struct spa_video_info_raw pw_format;
	struct obs_pipewire_formatinfo formatinfo;
};

/***********************************************************************************/

static void on_state_changed_cb(void *user_data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);
	UNUSED_PARAMETER(error);

	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)user_data;

	obs_pwvc->pw_stream.node_id =
		pw_stream_get_node_id(obs_pwvc->pw_stream.stream);

	blog(LOG_DEBUG, "[pipewire] stream %p state: \"%s\" (error: %s)",
	     obs_pwvc->pw_stream.stream, pw_stream_state_as_string(state),
	     error ? error : "none");

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		obs_pwvc->pw_stream.pw_stream_state = true;
		obs_output_begin_data_capture(obs_pwvc->output,
					      OBS_OUTPUT_VIDEO);
		break;
	default:
		obs_pwvc->pw_stream.pw_stream_state = false;
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

	// TODO: generalize for planar formats
	int blocks = obs_pwvc->formatinfo.planes;
	int size = obs_pwvc->formatinfo.sizes[0];
	int stride = obs_pwvc->formatinfo.strides[0];
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
	pw_stream_update_params(obs_pwvc->pw_stream.stream, params, 2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
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
	obs_pwvc->pw_stream.pw_core = &obs_pwvc->pw_core;
	UNUSED_PARAMETER(settings);
	return obs_pwvc;
}

static bool virtualcam_start(void *data)
{
	blog(LOG_INFO, "Virtual camera started");
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;

	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	if (!obs_pw_create_loop(&obs_pwvc->pw_core,
				"obs-pipewire-virtualcam-thread-loop")) {
		blog(LOG_WARNING, "[pipewire]: Failed to create loop");
		goto error;
	}

	if (!obs_pw_start_loop(&obs_pwvc->pw_core)) {
		blog(LOG_WARNING, "[pipewire]: Failed to start loop");
		goto error;
	}

	if (!obs_pw_create_context(&obs_pwvc->pw_core, -1, NULL, NULL)) {
		blog(LOG_WARNING, "[pipewire]: Failed to create context");
		goto error;
	}

	int width = obs_output_get_width(obs_pwvc->output);
	int height = obs_output_get_height(obs_pwvc->output);
	params[0] = build_format(&b, width, height, SPA_VIDEO_FORMAT_RGBA);
	params[1] = build_format(&b, width, height, SPA_VIDEO_FORMAT_YUY2);
	obs_pwvc->pw_stream.type = OBS_PW_STREAM_TYPE_OUTPUT;

	if (!obs_pw_create_stream(
		    &obs_pwvc->pw_stream, "obs-pipewire-virtualcam",
		    pw_properties_new(PW_KEY_NODE_DESCRIPTION,
				      "OBS Virtual Camera", PW_KEY_MEDIA_CLASS,
				      "Video/Source", PW_KEY_MEDIA_ROLE,
				      "Camera", NULL),
		    PW_ID_ANY,
		    (PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
		    &stream_events, params, 2, obs_pwvc)) {
		blog(LOG_WARNING, "[pipewire]: Failed to create stream");
		goto error;
	}
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
	// stop pipewire stuff
	obs_pw_stop_loop(&obs_pwvc->pw_core);
	obs_pw_destroy_stream(&obs_pwvc->pw_stream);

	obs_pw_destroy_context(&obs_pwvc->pw_core);
	obs_pw_destroy_loop(&obs_pwvc->pw_core);
}

static void virtual_video(void *data, struct video_data *frame)
{
	struct obs_pipewire_virtualcam_data *obs_pwvc =
		(struct obs_pipewire_virtualcam_data *)data;

	// check if we have a running pipewire stream
	if (!obs_pwvc->pw_stream.pw_stream_state) {
		// blog(LOG_INFO, "No node connected");
		return;
	}

	blog(LOG_DEBUG, "exporting frame to pipewire");
	// get buffer
	struct pw_buffer *pw_buf;
	if ((pw_buf = pw_stream_dequeue_buffer(obs_pwvc->pw_stream.stream)) ==
	    NULL) {
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
		h->seq = obs_pwvc->pw_stream.seq++;
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

	pw_stream_queue_buffer(obs_pwvc->pw_stream.stream, pw_buf);
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
