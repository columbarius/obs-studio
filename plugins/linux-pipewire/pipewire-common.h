/* pipewire-common.h
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

#pragma once

#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/param/video/format-utils.h>

#include <stdint.h>

struct obs_pw_core {
	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;
};

enum obs_pw_stream_type {
	OBS_PW_STREAM_TYPE_NONE = 0,
	OBS_PW_STREAM_TYPE_INPUT,
	OBS_PW_STREAM_TYPE_OUTPUT,
};

struct obs_pw_stream {
	uint32_t node_id;
	bool pw_stream_state;
	enum obs_pw_stream_type type;
	uint32_t seq;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct obs_pw_core *pw_core;
};

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

typedef struct _obs_pipewire_data obs_pipewire_data;

/**********************************************************************/

struct spa_pod *build_format(struct spa_pod_builder *b, uint32_t width,
			     uint32_t height, uint32_t format);

bool get_obs_formatinfo_from_pw_format(
	struct obs_pipewire_formatinfo *formatinfo,
	struct spa_video_info_raw *pw_video_info);

void filter_pw_stream_events(
	struct pw_stream_events *pw_stream_events,
	const struct pw_stream_events *pw_stream_events_default);

/**********************************************************************/

bool obs_pw_start_loop(struct obs_pw_core *pw_core);
bool obs_pw_stop_loop(struct obs_pw_core *pw_core);
bool obs_pw_create_loop(struct obs_pw_core *pw_core, char *name);
bool obs_pw_destroy_loop(struct obs_pw_core *pw_core);
bool obs_pw_create_stream(struct obs_pw_stream *pw_stream, char *name,
			  struct pw_properties *pw_props,
			  uint32_t target_node_id, enum pw_stream_flags flags,
			  const struct pw_stream_events *pw_stream_events,
			  const struct spa_pod **params, uint32_t n_params,
			  void *data);
bool obs_pw_destroy_stream(struct obs_pw_stream *pw_stream);
bool obs_pw_create_context(struct obs_pw_core *pw_core, int pipewire_fd,
			   const struct pw_core_events *core_events,
			   void *data);
bool obs_pw_destroy_context(struct obs_pw_core *pw_core);

/**********************************************************************/

void obs_pipewire_load(void);
void obs_pipewire_unload(void);
