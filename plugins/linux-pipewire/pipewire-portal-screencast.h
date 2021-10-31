/* pipewire-portal-camera.h
 *
 * Copyright 2021 columbarius <co1umbarius@protonmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "portal.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <stdbool.h>

enum obs_pw_capture_type {
	DESKTOP_CAPTURE = 1,
	WINDOW_CAPTURE = 2,
};

struct obs_pipewire_portal_screencast_data {
	GCancellable *cancellable;

	char *sender_name;
	char *session_handle;

	char *request_path_template;
	char *session_path_template;

	bool negotiated;
	uint32_t pipewire_node;
	int pipewire_fd;

	void (*play_stream)(void *);
	void *data;

	uint32_t capture_type;
	uint32_t available_cursor_modes;
	bool show_cursor;
};

uint32_t portal_get_available_capture_types();
gboolean init_xdg_portal_screencast(
	struct obs_pipewire_portal_screencast_data *portal_handle);
void close_xdg_portal_screencast(
	struct obs_pipewire_portal_screencast_data *portal_handle);
