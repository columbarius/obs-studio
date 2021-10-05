/* pipewire-common.c
 *
 * Copyright 2021 columbarius <co1umbarius@protonmail.com>
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

#include <obs-module.h>
#include <pipewire/pipewire.h>

#include <fcntl.h>

#include "pipewire-common.h"

/**********************************************************************/

struct spa_pod *build_format(struct spa_pod_builder *b, uint32_t width,
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

bool get_obs_formatinfo_from_pw_format(
	struct obs_pipewire_formatinfo *formatinfo,
	struct spa_video_info_raw *pw_video_info)
{
	switch (pw_video_info->format) {
	case SPA_VIDEO_FORMAT_YUY2:
		formatinfo->bpp = 2;
		formatinfo->pw_format = pw_video_info->format;
		formatinfo->obs_format = VIDEO_FORMAT_YUY2;
		formatinfo->width = pw_video_info->size.width;
		formatinfo->height = pw_video_info->size.height;
		formatinfo->planes = 1;
		formatinfo->strides[0] =
			SPA_ROUND_UP_N(formatinfo->width * formatinfo->bpp, 4);
		formatinfo->sizes[0] =
			formatinfo->height * formatinfo->strides[0];
		break;
	case SPA_VIDEO_FORMAT_RGBA:
		formatinfo->bpp = 4;
		formatinfo->pw_format = pw_video_info->format;
		formatinfo->obs_format = VIDEO_FORMAT_RGBA;
		formatinfo->width = pw_video_info->size.width;
		formatinfo->height = pw_video_info->size.height;
		formatinfo->planes = 1;
		formatinfo->strides[0] =
			SPA_ROUND_UP_N(formatinfo->width * formatinfo->bpp, 4);
		formatinfo->sizes[0] =
			formatinfo->height * formatinfo->strides[0];
		break;
	default:
		return false;
	}
	return true;
}

void filter_pw_stream_events(
	struct pw_stream_events *pw_stream_events,
	const struct pw_stream_events *pw_stream_events_default)
{
	if (!pw_stream_events->add_buffer) {
		pw_stream_events->add_buffer =
			pw_stream_events_default->add_buffer;
	}
	if (!pw_stream_events->control_info) {
		pw_stream_events->control_info =
			pw_stream_events_default->control_info;
	}
	if (!pw_stream_events->destroy) {
		pw_stream_events->destroy = pw_stream_events_default->destroy;
	}
	if (!pw_stream_events->drained) {
		pw_stream_events->drained = pw_stream_events_default->drained;
	}
	if (!pw_stream_events->io_changed) {
		pw_stream_events->io_changed =
			pw_stream_events_default->io_changed;
	}
	if (!pw_stream_events->param_changed) {
		pw_stream_events->param_changed =
			pw_stream_events_default->param_changed;
	}
	if (!pw_stream_events->process) {
		pw_stream_events->process = pw_stream_events_default->process;
	}
	if (!pw_stream_events->remove_buffer) {
		pw_stream_events->remove_buffer =
			pw_stream_events_default->remove_buffer;
	}
	if (!pw_stream_events->state_changed) {
		pw_stream_events->state_changed =
			pw_stream_events_default->state_changed;
	}
	if (pw_stream_events->version == 0) {
		pw_stream_events->version = pw_stream_events_default->version;
	}
}

/**********************************************************************/

bool obs_pw_start_loop(struct obs_pw_core *pw_core)
{
	if (pw_thread_loop_start(pw_core->thread_loop) < 0) {
		return false;
	}
	return true;
}

bool obs_pw_stop_loop(struct obs_pw_core *pw_core)
{
	pw_thread_loop_wait(pw_core->thread_loop);
	pw_thread_loop_stop(pw_core->thread_loop);
	return true;
}

bool obs_pw_create_loop(struct obs_pw_core *pw_core, char *name)
{
	pw_core->thread_loop = pw_thread_loop_new(name, NULL);
	if (!pw_core->thread_loop) {
		return false;
	}
	return true;
}

bool obs_pw_destroy_loop(struct obs_pw_core *pw_core)
{
	pw_thread_loop_destroy(pw_core->thread_loop);
	return true;
}

bool obs_pw_create_stream(struct obs_pw_stream *pw_stream, char *name,
			  struct pw_properties *pw_props,
			  uint32_t target_node_id, enum pw_stream_flags flags,
			  const struct pw_stream_events *pw_stream_events,
			  const struct spa_pod **params, uint32_t n_params,
			  void *data)
{
	if (pw_stream->type == OBS_PW_STREAM_TYPE_NONE)
		return false;

	pw_thread_loop_lock(pw_stream->pw_core->thread_loop);

	pw_stream->stream =
		pw_stream_new(pw_stream->pw_core->core, name, pw_props);
	if (!pw_stream->stream) {
		goto error;
	}
	pw_stream->pw_stream_state = false;

	pw_stream_add_listener(pw_stream->stream, &pw_stream->stream_listener,
			       pw_stream_events, data);

	enum spa_direction direction = pw_stream->type ==
						       OBS_PW_STREAM_TYPE_INPUT
					       ? SPA_DIRECTION_INPUT
					       : SPA_DIRECTION_OUTPUT;
	pw_stream_connect(pw_stream->stream, direction, target_node_id, flags,
			  params, n_params);

	pw_thread_loop_unlock(pw_stream->pw_core->thread_loop);
	return true;

error:
	pw_thread_loop_unlock(pw_stream->pw_core->thread_loop);
	return false;
}

bool obs_pw_destroy_stream(struct obs_pw_stream *pw_stream)
{
	pw_stream_disconnect(pw_stream->stream);
	pw_stream_destroy(pw_stream->stream);
	pw_stream->stream = NULL;
	pw_stream->pw_stream_state = false;
	return true;
}

bool obs_pw_create_context(struct obs_pw_core *pw_core, int pipewire_fd,
			   const struct pw_core_events *core_events, void *data)
{
	pw_thread_loop_lock(pw_core->thread_loop);

	pw_core->context = pw_context_new(
		pw_thread_loop_get_loop(pw_core->thread_loop), NULL, 0);
	if (!pw_core->context) {
		blog(LOG_WARNING, "[pipewire]: Failed to create context");
		goto error;
	}

	if (pipewire_fd == -1) {
		pw_core->core = pw_context_connect(pw_core->context, NULL, 0);
	} else {
		pw_core->core = pw_context_connect_fd(
			pw_core->context,
			fcntl(pipewire_fd, F_DUPFD_CLOEXEC, 5), NULL, 0);
	}
	if (!pw_core->core) {
		blog(LOG_WARNING, "[pipewire]: Failed to connect to context");
		goto error;
	}

	if (core_events) {
		pw_core_add_listener(pw_core->core, &pw_core->core_listener,
				     core_events, data);
	}

	pw_thread_loop_unlock(pw_core->thread_loop);
	return true;

error:
	pw_thread_loop_unlock(pw_core->thread_loop);
	return false;
}

bool obs_pw_destroy_context(struct obs_pw_core *pw_core)
{
	pw_core_disconnect(pw_core->core);
	pw_core->core = NULL;
	pw_context_destroy(pw_core->context);
	pw_core->context = NULL;
	return true;
}

/**********************************************************************/

void obs_pipewire_load(void)
{
	pw_init(NULL, NULL);
}

void obs_pipewire_unload(void)
{
	pw_deinit();
}
