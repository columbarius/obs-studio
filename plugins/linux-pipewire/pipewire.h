/* pipewire.h
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#include <obs-module.h>

#include <pipewire/keys.h>
#include <pipewire/properties.h>

/* PipeWire Streams */

enum obs_import_type {
	IMPORT_API_TEXTURE,
	IMPORT_API_MEDIA,
};

typedef struct _obs_pipewire_stream_data obs_pipewire_stream_data;

extern const struct pw_stream_events stream_events_texture;
extern const struct pw_stream_events stream_events_media;

obs_pipewire_stream_data *
obs_pipewire_stream_create(int pipewire_fd, int pipewire_node, const char *name,
			   struct pw_properties *props,
			   const struct pw_stream_events *stream_events,
			   enum obs_import_type type, obs_source_t *source);
void obs_pipewire_stream_destroy(obs_pipewire_stream_data *obs_pw);

void obs_pipewire_stream_show(obs_pipewire_stream_data *obs_pw);
void obs_pipewire_stream_hide(obs_pipewire_stream_data *obs_pw);
uint32_t obs_pipewire_stream_get_width(obs_pipewire_stream_data *obs_pw);
uint32_t obs_pipewire_stream_get_height(obs_pipewire_stream_data *obs_pw);
void obs_pipewire_stream_video_render(obs_pipewire_stream_data *obs_pw,
				      gs_effect_t *effect);

void obs_pipewire_stream_set_cursor_visible(obs_pipewire_stream_data *obs_pw,
					    bool cursor_visible);

/* PipeWire Registry */

typedef struct _obs_pipewire_registry_device obs_pipewire_registry_device;
typedef struct _obs_pipewire_registry_callbacks obs_pipewire_registry_callbacks;
typedef struct _obs_pipewire_registry_data obs_pipewire_registry_data;

struct _obs_pipewire_registry_device {
	uint32_t id;
	uint32_t version;

	char *name;
	char *description;
	char *path;
	char *nick;
	char *class;
	char *role;
};

struct _obs_pipewire_registry_callbacks {
	void (*device_added)(void *, obs_pipewire_registry_device *);
	void (*device_removed)(void *, uint32_t);
};

void *obs_pipewire_registry_register_callback(
	obs_pipewire_registry_data *obs_pw,
	obs_pipewire_registry_callbacks *callbacks, void *user_data);
void *obs_pipewire_registry_remove_callback(obs_pipewire_registry_data *obs_pw,
					    void *registry_callback);

obs_pipewire_registry_data *obs_pipewire_registry_create(int pipewire_fd);
void obs_pipewire_registry_destroy(obs_pipewire_registry_data *obs_pw);
