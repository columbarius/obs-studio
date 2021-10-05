/* camera-portal.c
 *
 * Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#include "pipewire.h"
#include "portal.h"

#include <util/dstr.h>

#include <fcntl.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <spa/node/keys.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>
#include <spa/utils/keys.h>

struct camera_portal {
	int pipewire_fd;

	GCancellable *cancellable;

	char *session_handle;

	obs_pipewire_registry_data *registry;
};

struct camera_portal camera_portal = {.pipewire_fd = -1, .registry = NULL};

struct obs_pipewire_camera {
	obs_source_t *source;
	obs_data_t *settings;

	obs_pipewire_stream_data *obs_pw;

	struct camera_portal *camera_portal;

	obs_property_t *device_list;

	int camera_node;
};

/* ------------------------------------------------- */

static void camera_device_added(void *user_data,
				obs_pipewire_registry_device *device)
{
	struct obs_pipewire_camera *pw_camera = user_data;

	blog(LOG_INFO, "Device added %u: %s (%s)", device->id, device->name,
	     device->role);

	obs_property_list_add_int(pw_camera->device_list, device->name,
				  device->id);
}

static void camera_device_removed(void *user_data, int id)
{
	blog(LOG_INFO, "Device removed %d", id);
}

static obs_pipewire_registry_callbacks registry_callbacks = {
	.device_added = camera_device_added,
	.device_removed = camera_device_removed,
};

/* ------------------------------------------------- */

static void populate_cameras_list(struct obs_pipewire_camera *pw_camera,
				  obs_property_t *device_list)
{
	pw_camera->device_list = device_list;

	obs_pipewire_registry_callbacks *callback =
		obs_pipewire_registry_register_callback(
			pw_camera->camera_portal->registry, &registry_callbacks,
			pw_camera);

	obs_pipewire_registry_remove_callback(
		pw_camera->camera_portal->registry, callback);

	pw_camera->device_list = NULL;
}

/* Settings callbacks */

static bool device_selected(void *data, obs_properties_t *props,
			    obs_property_t *p, obs_data_t *settings)
{
	struct obs_pipewire_camera *pw_camera = data;
	int device;

	device = obs_data_get_int(settings, "device_id");

	blog(LOG_INFO, "[pipewire] selected device %d", device);

	if (device == 0) {
		return false;
	}

	if (pw_camera->obs_pw) {
		obs_pipewire_stream_destroy(pw_camera->obs_pw);
	}

	pw_camera->obs_pw = obs_pipewire_stream_create(
		dup(pw_camera->camera_portal->pipewire_fd), device,
		"OBS PipeWire Camera",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Camera", NULL),
		&stream_events_media, IMPORT_API_MEDIA, pw_camera->source);

	return true;
}

/* obs_source_info methods */

static const char *pipewire_camera_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireCamera");
}

static void *pipewire_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct obs_pipewire_camera *pw_camera;

	pw_camera = bzalloc(sizeof(struct obs_pipewire_camera));
	pw_camera->source = source;

	pw_camera->camera_portal = &camera_portal;

	return pw_camera;
}

static void pipewire_camera_destroy(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw) {
		obs_pipewire_stream_destroy(pw_camera->obs_pw);
	}

	bfree(pw_camera);
}

static void pipewire_camera_get_defaults(obs_data_t *settings) {
	obs_data_set_int(settings, "device_id", 0);
}

static obs_properties_t *pipewire_camera_get_properties(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	obs_properties_t *properties = obs_properties_create();

	obs_property_t *device_list = obs_properties_add_list(
		properties, "device_id",
		obs_module_text("PipeWireCameraDevice"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	populate_cameras_list(pw_camera, device_list);
	obs_property_set_modified_callback2(device_list, device_selected,
					    pw_camera);

	return properties;
}

static void pipewire_camera_update(void *data, obs_data_t *settings) {}

static void pipewire_camera_show(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		obs_pipewire_stream_show(pw_camera->obs_pw);
}

static void pipewire_camera_hide(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		obs_pipewire_stream_hide(pw_camera->obs_pw);
}

static uint32_t pipewire_camera_get_width(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		return obs_pipewire_stream_get_width(pw_camera->obs_pw);
	else
		return 0;
}

static uint32_t pipewire_camera_get_height(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		return obs_pipewire_stream_get_height(pw_camera->obs_pw);
	else
		return 0;
}

static void register_camera_plugin(void)
{
	const struct obs_source_info pipewire_camera_info = {
		.id = "pipewire-camera-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO,
		.get_name = pipewire_camera_get_name,
		.create = pipewire_camera_create,
		.destroy = pipewire_camera_destroy,
		.get_defaults = pipewire_camera_get_defaults,
		.get_properties = pipewire_camera_get_properties,
		.update = pipewire_camera_update,
		.show = pipewire_camera_show,
		.hide = pipewire_camera_hide,
		.get_width = pipewire_camera_get_width,
		.get_height = pipewire_camera_get_height,
		.icon_type = OBS_ICON_TYPE_CAMERA,
	};
	obs_register_source(&pipewire_camera_info);
}

/* ------------------------------------------------- */

static GDBusProxy *camera_proxy = NULL;

static void ensure_camera_portal_proxy(void)
{
	g_autoptr(GError) error = NULL;
	if (!camera_proxy) {
		camera_proxy = g_dbus_proxy_new_sync(
			portal_get_dbus_connection(), G_DBUS_PROXY_FLAGS_NONE,
			NULL, "org.freedesktop.portal.Desktop",
			"/org/freedesktop/portal/desktop",
			"org.freedesktop.portal.Camera", NULL, &error);

		if (error) {
			blog(LOG_WARNING,
			     "[portals] Error retrieving D-Bus proxy: %s",
			     error->message);
			return;
		}
	}
}

static GDBusProxy *get_camera_portal_proxy(void)
{
	ensure_camera_portal_proxy();
	return camera_proxy;
}

static bool get_is_camera_present(void)
{
	g_autoptr(GVariant) cached_camera_present = NULL;
	bool camera_present;

	ensure_camera_portal_proxy();

	if (!camera_proxy)
		return 0;

	cached_camera_present = g_dbus_proxy_get_cached_property(
		camera_proxy, "IsCameraPresent");
	camera_present = cached_camera_present
				 ? g_variant_get_boolean(cached_camera_present)
				 : false;

	return camera_present;
}

static uint32_t get_camera_version(void)
{
	g_autoptr(GVariant) cached_version = NULL;
	uint32_t version;

	ensure_camera_portal_proxy();

	if (!camera_proxy)
		return 0;

	cached_version =
		g_dbus_proxy_get_cached_property(camera_proxy, "version");
	version = cached_version ? g_variant_get_uint32(cached_version) : 0;

	return version;
}

/* ------------------------------------------------- */

struct dbus_call_data {
	struct camera_portal *camera_portal;
	char *request_path;
	guint signal_id;
	gulong cancelled_id;
};

static void on_cancelled_cb(GCancellable *cancellable, void *data)
{
	UNUSED_PARAMETER(cancellable);

	struct dbus_call_data *call = data;

	blog(LOG_INFO, "[pipewire] camera session cancelled");

	g_dbus_connection_call(
		portal_get_dbus_connection(), "org.freedesktop.portal.Desktop",
		call->request_path, "org.freedesktop.portal.Request", "Close",
		NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static struct dbus_call_data *
subscribe_to_signal(struct camera_portal *camera_portal, const char *path,
		    GDBusSignalCallback callback)
{
	struct dbus_call_data *call;

	call = bzalloc(sizeof(struct dbus_call_data));
	call->camera_portal = camera_portal;
	call->request_path = bstrdup(path);
	call->cancelled_id =
		g_signal_connect(camera_portal->cancellable, "cancelled",
				 G_CALLBACK(on_cancelled_cb), call);
	call->signal_id = g_dbus_connection_signal_subscribe(
		portal_get_dbus_connection(), "org.freedesktop.portal.Desktop",
		"org.freedesktop.portal.Request", "Response",
		call->request_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
		callback, call, NULL);

	return call;
}

static void dbus_call_data_free(struct dbus_call_data *call)
{
	if (!call)
		return;

	if (call->signal_id)
		g_dbus_connection_signal_unsubscribe(
			portal_get_dbus_connection(), call->signal_id);

	if (call->cancelled_id > 0)
		g_signal_handler_disconnect(call->camera_portal->cancellable,
					    call->cancelled_id);

	g_clear_pointer(&call->request_path, bfree);
	bfree(call);
}

/* ------------------------------------------------- */

static void on_pipewire_remote_opened_cb(GObject *source, GAsyncResult *res,
					 void *user_data)
{
	struct camera_portal *camera_portal;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	int pipewire_fd;
	int fd_index;

	camera_portal = user_data;

	result = g_dbus_proxy_call_with_unix_fd_list_finish(
		G_DBUS_PROXY(source), &fd_list, res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error retrieving pipewire fd: %s",
			     error->message);
		return;
	}

	g_variant_get(result, "(h)", &fd_index, &error);

	pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error retrieving pipewire fd: %s",
			     error->message);
		return;
	}

	camera_portal->pipewire_fd = pipewire_fd;

	camera_portal->registry = obs_pipewire_registry_create(pipewire_fd);
	if (!camera_portal->registry) {
		return;
	}

	// obs_pipewire_registry_register_callback(camera_portal->registry, &registry_callbacks, camera_portal);

	register_camera_plugin();
}

static void open_pipewire_remote(struct camera_portal *camera_portal)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

	g_dbus_proxy_call_with_unix_fd_list(
		get_camera_portal_proxy(), "OpenPipeWireRemote",
		g_variant_new("(a{sv})", &builder), G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, camera_portal->cancellable, on_pipewire_remote_opened_cb,
		camera_portal);
}

/* ------------------------------------------------- */

static void on_access_camera_response_received_cb(
	GDBusConnection *connection, const char *sender_name,
	const char *object_path, const char *interface_name,
	const char *signal_name, GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(connection);
	UNUSED_PARAMETER(sender_name);
	UNUSED_PARAMETER(object_path);
	UNUSED_PARAMETER(interface_name);
	UNUSED_PARAMETER(signal_name);

	struct camera_portal *camera_portal;
	g_autoptr(GVariant) result = NULL;
	struct dbus_call_data *call = user_data;
	uint32_t response;

	camera_portal = call->camera_portal;

	g_clear_pointer(&call, dbus_call_data_free);

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to create session, denied or cancelled by user");
		return;
	}

	blog(LOG_INFO, "[pipewire] Successfully accessed cameras");

	open_pipewire_remote(camera_portal);
}

static void on_access_camera_finished_cb(GObject *source, GAsyncResult *res,
					 void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error accessing camera: %s",
			     error->message);
		return;
	}
}

static void access_camera(struct camera_portal *camera_portal)
{
	struct dbus_call_data *call;
	GVariantBuilder builder;
	char *request_token;
	char *request_path;

	portal_create_request_path(&request_path, &request_token);

	call = subscribe_to_signal(camera_portal, request_path,
				   on_access_camera_response_received_cb);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token",
			      g_variant_new_string(request_token));

	g_dbus_proxy_call(get_camera_portal_proxy(), "AccessCamera",
			  g_variant_new("(a{sv})", &builder),
			  G_DBUS_CALL_FLAGS_NONE, -1,
			  camera_portal->cancellable,
			  on_access_camera_finished_cb, call);

	bfree(request_token);
	bfree(request_path);
}

/* ------------------------------------------------- */

static gboolean init_camera_portal(struct camera_portal *camera_portal)
{
	GDBusConnection *connection;
	GDBusProxy *proxy;

	camera_portal->cancellable = g_cancellable_new();
	connection = portal_get_dbus_connection();
	if (!connection) {
		blog(LOG_WARNING, "PipeWire Camera Portal no connection");
		return FALSE;
	}
	proxy = get_camera_portal_proxy();
	if (!proxy) {
		blog(LOG_WARNING, "PipeWire Camera Portal no proxy");
		return FALSE;
	}

	blog(LOG_INFO, "PipeWire Camera Portal initialized");

	access_camera(camera_portal);

	return TRUE;
}

void camera_portal_load(void)
{
	blog(LOG_INFO, "Initialize PipeWire Camera Portal");

	init_camera_portal(&camera_portal);
}

void camera_portal_unload(void)
{
	if (camera_portal.registry) {
		// obs_pipewire_registry_remove_callback(camera_portal.registry, camera_portal.registry_callbacks);
		obs_pipewire_registry_destroy(camera_portal.registry);
	}
	if (camera_portal.session_handle) {
		bfree(camera_portal.session_handle);
	}
}
