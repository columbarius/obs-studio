/* pipewire.c
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

#include "pipewire.h"

#include <libdrm/drm_fourcc.h>
#include <fcntl.h>
#include <glad/glad.h>
#include <linux/dma-buf.h>
#include <libdrm/drm_fourcc.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#include <gio/gio.h>

#define CURSOR_META_SIZE(width, height)                                    \
	(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + \
	 width * height * 4)

struct modifier_info {
	uint32_t spa_format;
	uint32_t drm_format;
	int32_t n_modifiers;
	uint64_t *modifiers;
};

struct _obs_pipewire_data {
	uint32_t pipewire_node;
	int pipewire_fd;

	gs_texture_t *texture;

	obs_source_t *source;

	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info format;

	struct {
		bool valid;
		int x, y;
		uint32_t width, height;
	} crop;

	struct {
		bool visible;
		bool valid;
		int x, y;
		int hotspot_x, hotspot_y;
		int width, height;
		gs_texture_t *texture;
	} cursor;

	struct obs_video_info video_info;
	bool negotiated;

	int n_formats;
	struct modifier_info *modifier_info;

	enum import_type import_type;
};

/* auxiliary methods */

static void teardown_pipewire(obs_pipewire_data *obs_pw)
{
	if (obs_pw->thread_loop) {
		pw_thread_loop_wait(obs_pw->thread_loop);
		pw_thread_loop_stop(obs_pw->thread_loop);
	}

	if (obs_pw->stream)
		pw_stream_disconnect(obs_pw->stream);
	g_clear_pointer(&obs_pw->stream, pw_stream_destroy);
	g_clear_pointer(&obs_pw->context, pw_context_destroy);
	g_clear_pointer(&obs_pw->thread_loop, pw_thread_loop_destroy);

	if (obs_pw->pipewire_fd > 0) {
		close(obs_pw->pipewire_fd);
		obs_pw->pipewire_fd = 0;
	}

	obs_pw->negotiated = false;
}

static void destroy_session(obs_pipewire_data *obs_pw)
{
	g_clear_pointer(&obs_pw->cursor.texture, gs_texture_destroy);
	g_clear_pointer(&obs_pw->texture, gs_texture_destroy);
}

static inline bool has_effective_crop(obs_pipewire_data *obs_pw)
{
	return obs_pw->crop.valid &&
	       (obs_pw->crop.x != 0 || obs_pw->crop.y != 0 ||
		obs_pw->crop.width < obs_pw->format.info.raw.size.width ||
		obs_pw->crop.height < obs_pw->format.info.raw.size.height);
}

static bool spa_pixel_format_to_drm_format(uint32_t spa_format,
					   uint32_t *out_format)
{
	switch (spa_format) {
	case SPA_VIDEO_FORMAT_RGBA:
		*out_format = DRM_FORMAT_ABGR8888;
		break;

	case SPA_VIDEO_FORMAT_RGBx:
		*out_format = DRM_FORMAT_XBGR8888;
		break;

	case SPA_VIDEO_FORMAT_BGRA:
		*out_format = DRM_FORMAT_ARGB8888;
		break;

	case SPA_VIDEO_FORMAT_BGRx:
		*out_format = DRM_FORMAT_XRGB8888;
		break;

	case SPA_VIDEO_FORMAT_YUY2:
		*out_format = DRM_FORMAT_YUYV;
		break;

	default:
		return false;
	}

	return true;
}

static bool spa_pixel_format_to_obs_format(uint32_t spa_format,
					   enum gs_color_format *out_format,
					   bool *swap_red_blue)
{
	switch (spa_format) {
	case SPA_VIDEO_FORMAT_RGBA:
		*out_format = GS_RGBA;
		*swap_red_blue = false;
		break;

	case SPA_VIDEO_FORMAT_RGBx:
		*out_format = GS_BGRX;
		*swap_red_blue = true;
		break;

	case SPA_VIDEO_FORMAT_BGRA:
		*out_format = GS_BGRA;
		*swap_red_blue = false;
		break;

	case SPA_VIDEO_FORMAT_BGRx:
		*out_format = GS_BGRX;
		*swap_red_blue = false;
		break;

	case SPA_VIDEO_FORMAT_YUY2:
		*out_format = GS_BGRA;
		*swap_red_blue = false;
		break;

	default:
		return false;
	}

	return true;
}

static void swap_texture_red_blue(gs_texture_t *texture)
{
	GLuint gl_texure = *(GLuint *)gs_texture_get_obj(texture);

	glBindTexture(GL_TEXTURE_2D, gl_texure);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static enum video_colorspace
get_colorspace_from_spa_color_matrix(enum spa_video_color_matrix matrix)
{
	switch (matrix) {
	case SPA_VIDEO_COLOR_MATRIX_RGB:
		return VIDEO_CS_DEFAULT;
	case SPA_VIDEO_COLOR_MATRIX_BT601:
		return VIDEO_CS_601;
	case SPA_VIDEO_COLOR_MATRIX_BT709:
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_DEFAULT;
	}
}

static enum video_range_type
get_colorrange_from_spa_color_range(enum spa_video_color_range colorrange)
{
	switch (colorrange) {
	case SPA_VIDEO_COLOR_RANGE_0_255:
		return VIDEO_RANGE_FULL;
	case SPA_VIDEO_COLOR_RANGE_16_235:
		return VIDEO_RANGE_PARTIAL;
	default:
		return VIDEO_RANGE_DEFAULT;
	}
}

static bool prepare_obs_frame(obs_pipewire_data *obs_pw,
			      struct obs_source_frame *frame)
{
	frame->width = obs_pw->format.info.raw.size.width;
	frame->height = obs_pw->format.info.raw.size.height;
	video_format_get_parameters(
		get_colorspace_from_spa_color_matrix(
			obs_pw->format.info.raw.color_matrix),
		get_colorrange_from_spa_color_range(
			obs_pw->format.info.raw.color_range),
		frame->color_matrix, frame->color_range_min,
		frame->color_range_max);
	switch (obs_pw->format.info.raw.format) {
	case SPA_VIDEO_FORMAT_RGBA:
		frame->format = VIDEO_FORMAT_RGBA;
		frame->linesize[0] = SPA_ROUND_UP_N(frame->width * 4, 4);
		break;
	case SPA_VIDEO_FORMAT_YUY2:
		frame->format = VIDEO_FORMAT_YUY2;
		frame->linesize[0] = SPA_ROUND_UP_N(frame->width * 2, 4);
		break;
	default:
		return false;
	}
	return true;
}

static inline struct spa_pod *build_format(struct spa_pod_builder *b,
					   struct obs_video_info *ovi,
					   uint32_t format, uint64_t *modifiers,
					   size_t modifier_count)
{
	uint32_t i, c;
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

	/* modifier */
	if (modifier_count > 0) {
		/* build an enumeration of modifiers */
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
				     SPA_POD_PROP_FLAG_MANDATORY |
					     SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
		/* modifiers from  an array */
		for (i = 0, c = 0; i < modifier_count; i++) {
			uint64_t modifier = modifiers[i];
			spa_pod_builder_long(b, modifier);
			if (c++ == 0)
				spa_pod_builder_long(b, modifier);
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	/* add size and framerate ranges */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
			    SPA_POD_CHOICE_RANGE_Rectangle(
				    &SPA_RECTANGLE(320, 240), // Arbitrary
				    &SPA_RECTANGLE(1, 1),
				    &SPA_RECTANGLE(8192, 4320)),
			    SPA_FORMAT_VIDEO_framerate,
			    SPA_POD_CHOICE_RANGE_Fraction(
				    &SPA_FRACTION(ovi->fps_num, ovi->fps_den),
				    &SPA_FRACTION(0, 1), &SPA_FRACTION(360, 1)),
			    0);
	return spa_pod_builder_pop(b, &f[0]);
}

static uint32_t build_format_params(obs_pipewire_data *obs_pw,
				    struct spa_pod_builder *pod_builder,
				    const struct spa_pod ***param_list)
{
	uint32_t n_params = 0;

	const struct spa_pod **params;
	params = bzalloc(2 * obs_pw->n_formats * sizeof(struct spa_pod *));

	for (int i = 0; i < obs_pw->n_formats; i++) {
		if (obs_pw->modifier_info[i].n_modifiers == 0) {
			continue;
		}
		params[n_params++] =
			build_format(pod_builder, &obs_pw->video_info,
				     obs_pw->modifier_info[i].spa_format,
				     obs_pw->modifier_info[i].modifiers,
				     obs_pw->modifier_info[i].n_modifiers);
	}
	for (int i = 0; i < obs_pw->n_formats; i++) {
		params[n_params++] = build_format(
			pod_builder, &obs_pw->video_info,
			obs_pw->modifier_info[i].spa_format, NULL, 0);
	}
	*param_list = params;
	return n_params;
}

static uint32_t
create_modifier_info_texture(struct modifier_info **modifier_info)
{
	uint32_t formats[] = {
		SPA_VIDEO_FORMAT_BGRA,
		SPA_VIDEO_FORMAT_RGBA,
		SPA_VIDEO_FORMAT_BGRx,
		SPA_VIDEO_FORMAT_RGBx,
	};

	int32_t n_formats = sizeof(formats) / sizeof(formats[0]);

	struct modifier_info *info =
		bzalloc(n_formats * sizeof(struct modifier_info));
	obs_enter_graphics();
	for (int i = 0; i < n_formats; i++) {
		info[i].spa_format = formats[i];
		spa_pixel_format_to_drm_format(formats[i], &info[i].drm_format);
		info[i].n_modifiers = gs_query_dmabuf_modifiers(
			info[i].drm_format, &info[i].modifiers);
	}
	obs_leave_graphics();

	*modifier_info = info;
	return n_formats;
}

static uint32_t create_modifier_info_media(struct modifier_info **modifier_info)
{
	uint32_t formats[] = {
		SPA_VIDEO_FORMAT_RGBA,
		SPA_VIDEO_FORMAT_YUY2,
	};

	int32_t n_formats = sizeof(formats) / sizeof(formats[0]);

	struct modifier_info *info =
		bzalloc(n_formats * sizeof(struct modifier_info));
	for (int i = 0; i < n_formats; i++) {
		info[i].spa_format = formats[i];
		spa_pixel_format_to_drm_format(formats[i], &info[i].drm_format);
		info[i].n_modifiers = 0;
		info[i].modifiers = NULL;
	}

	*modifier_info = info;
	return n_formats;
}

static void destroy_modifier_info(int32_t n_formats,
				  struct modifier_info *modifier_info)
{
	for (int i = 0; i < n_formats; i++) {
		bfree(modifier_info[i].modifiers);
		modifier_info[i].modifiers = NULL;
		modifier_info[i].n_modifiers = 0;
	}
	bfree(modifier_info);
}

/* ------------------------------------------------- */

static void on_process_media_cb(void *user_data)
{
	obs_pipewire_data *obs_pw = user_data;
	struct spa_buffer *buffer;
	struct pw_buffer *b;
	struct spa_data *d;
	struct obs_source_frame out = {0};

	/* Find the most recent buffer */
	b = NULL;
	while (true) {
		struct pw_buffer *aux =
			pw_stream_dequeue_buffer(obs_pw->stream);
		if (!aux)
			break;
		if (b)
			pw_stream_queue_buffer(obs_pw->stream, b);
		b = aux;
	}

	if (!b) {
		blog(LOG_DEBUG, "[pipewire] Out of buffers!");
		return;
	}

	buffer = b->buffer;
	d = buffer->datas;

	blog(LOG_DEBUG, "[pipewire] buffertype: %u", d[0].type);

	prepare_obs_frame(obs_pw, &out);
	for (unsigned int i = 0; i < buffer->n_datas && i < MAX_AV_PLANES;
	     i++) {
		if (d[i].data == SPA_DATA_DmaBuf) {
			sync_dma_buf(d[i].fd, DMA_BUF_SYNC_START);
			sync_dma_buf(d[i].fd, DMA_BUF_SYNC_READ);
			d[i].data == mmap(NULL, d[i].maxsize, PROT_READ,
					  MAP_SHARED, d[i].mapoffset);
		} else {
			out.data[i] = d[i].data;
		}
	}

	blog(LOG_DEBUG, "[pipewire] Camera frame info:");
	blog(LOG_DEBUG, "[pipewire] format: %s",
	     get_video_format_name(out.format));
	blog(LOG_DEBUG, "[pipewire] planes: %u", buffer->n_datas);
	for (unsigned int i = 0; i < buffer->n_datas && i < MAX_AV_PLANES;
	     i++) {
		blog(LOG_DEBUG, "[pipewire] dataptr: %p", out.data[i]);
		blog(LOG_DEBUG, "[pipewire] linesize: %d", out.linesize[i]);
	}

	obs_source_output_video(obs_pw->source, &out);

	for (unsigned int i = 0; i < buffer->n_datas && i < MAX_AV_PLANES;
	     i++) {
		if (d[i].data == SPA_DATA_DmaBuf) {
			munmap(out[i].data, d[i].maxsize);
			sync_dma_buf(d[i].fd, DMA_BUF_SYNC_END);
		}
	}

	pw_stream_queue_buffer(obs_pw->stream, b);
}

static void on_process_texture_cb(void *user_data)
{
	obs_pipewire_data *obs_pw = user_data;
	struct spa_meta_cursor *cursor;
	uint32_t drm_format;
	struct spa_meta_region *region;
	struct spa_buffer *buffer;
	struct pw_buffer *b;
	bool swap_red_blue = false;
	bool has_buffer;

	/* Find the most recent buffer */
	b = NULL;
	while (true) {
		struct pw_buffer *aux =
			pw_stream_dequeue_buffer(obs_pw->stream);
		if (!aux)
			break;
		if (b)
			pw_stream_queue_buffer(obs_pw->stream, b);
		b = aux;
	}

	if (!b) {
		blog(LOG_DEBUG, "[pipewire] Out of buffers!");
		return;
	}

	buffer = b->buffer;
	has_buffer = buffer->datas[0].chunk->size != 0;

	obs_enter_graphics();

	if (!has_buffer)
		goto read_metadata;

	if (buffer->datas[0].type == SPA_DATA_DmaBuf) {
		uint32_t planes = buffer->n_datas;
		uint32_t offsets[planes];
		uint32_t strides[planes];
		uint64_t modifiers[planes];
		int fds[planes];
		bool modifierless; // DMA-BUF without explicit modifier

		blog(LOG_DEBUG,
		     "[pipewire] DMA-BUF info: fd:%ld, stride:%d, offset:%u, size:%dx%d",
		     buffer->datas[0].fd, buffer->datas[0].chunk->stride,
		     buffer->datas[0].chunk->offset,
		     obs_pw->format.info.raw.size.width,
		     obs_pw->format.info.raw.size.height);

		if (!spa_pixel_format_to_drm_format(
			    obs_pw->format.info.raw.format, &drm_format)) {
			blog(LOG_ERROR,
			     "[pipewire] unsupported DMA buffer format: %d",
			     obs_pw->format.info.raw.format);
			goto read_metadata;
		}

		for (uint32_t plane = 0; plane < planes; plane++) {
			fds[plane] = buffer->datas[plane].fd;
			offsets[plane] = buffer->datas[plane].chunk->offset;
			strides[plane] = buffer->datas[plane].chunk->stride;
			modifiers[plane] = obs_pw->format.info.raw.modifier;
		}

		g_clear_pointer(&obs_pw->texture, gs_texture_destroy);

		modifierless = obs_pw->format.info.raw.modifier ==
			       DRM_FORMAT_MOD_INVALID;
		obs_pw->texture = gs_texture_create_from_dmabuf(
			obs_pw->format.info.raw.size.width,
			obs_pw->format.info.raw.size.height, drm_format,
			GS_BGRX, planes, fds, strides, offsets,
			modifierless ? NULL : modifiers);
	} else {
		blog(LOG_DEBUG, "[pipewire] Buffer has memory texture");
		enum gs_color_format obs_format;

		if (!spa_pixel_format_to_obs_format(
			    obs_pw->format.info.raw.format, &obs_format,
			    &swap_red_blue)) {
			blog(LOG_ERROR,
			     "[pipewire] unsupported DMA buffer format: %d",
			     obs_pw->format.info.raw.format);
			goto read_metadata;
		}

		g_clear_pointer(&obs_pw->texture, gs_texture_destroy);
		obs_pw->texture = gs_texture_create(
			obs_pw->format.info.raw.size.width,
			obs_pw->format.info.raw.size.height, obs_format, 1,
			(const uint8_t **)&buffer->datas[0].data, GS_DYNAMIC);
	}

	if (swap_red_blue)
		swap_texture_red_blue(obs_pw->texture);

	/* Video Crop */
	region = spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop,
					   sizeof(*region));
	if (region && spa_meta_region_is_valid(region)) {
		blog(LOG_DEBUG,
		     "[pipewire] Crop Region available (%dx%d+%d+%d)",
		     region->region.position.x, region->region.position.y,
		     region->region.size.width, region->region.size.height);

		obs_pw->crop.x = region->region.position.x;
		obs_pw->crop.y = region->region.position.y;
		obs_pw->crop.width = region->region.size.width;
		obs_pw->crop.height = region->region.size.height;
		obs_pw->crop.valid = true;
	} else {
		obs_pw->crop.valid = false;
	}

read_metadata:

	/* Cursor */
	cursor = spa_buffer_find_meta_data(buffer, SPA_META_Cursor,
					   sizeof(*cursor));
	obs_pw->cursor.valid = cursor && spa_meta_cursor_is_valid(cursor);
	if (obs_pw->cursor.visible && obs_pw->cursor.valid) {
		struct spa_meta_bitmap *bitmap = NULL;
		enum gs_color_format format;

		if (cursor->bitmap_offset)
			bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset,
					    struct spa_meta_bitmap);

		if (bitmap && bitmap->size.width > 0 &&
		    bitmap->size.height > 0 &&
		    spa_pixel_format_to_obs_format(bitmap->format, &format,
						   &swap_red_blue)) {
			const uint8_t *bitmap_data;

			bitmap_data =
				SPA_MEMBER(bitmap, bitmap->offset, uint8_t);
			obs_pw->cursor.hotspot_x = cursor->hotspot.x;
			obs_pw->cursor.hotspot_y = cursor->hotspot.y;
			obs_pw->cursor.width = bitmap->size.width;
			obs_pw->cursor.height = bitmap->size.height;

			g_clear_pointer(&obs_pw->cursor.texture,
					gs_texture_destroy);
			obs_pw->cursor.texture = gs_texture_create(
				obs_pw->cursor.width, obs_pw->cursor.height,
				format, 1, &bitmap_data, GS_DYNAMIC);

			if (swap_red_blue)
				swap_texture_red_blue(obs_pw->cursor.texture);
		}

		obs_pw->cursor.x = cursor->position.x;
		obs_pw->cursor.y = cursor->position.y;
	}

	pw_stream_queue_buffer(obs_pw->stream, b);

	obs_leave_graphics();
}

static void on_param_changed_cb(void *user_data, uint32_t id,
				const struct spa_pod *param)
{
	obs_pipewire_data *obs_pw = user_data;
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[3];
	uint8_t params_buffer[1024];
	int result;
	bool has_modifier;
	int buffertypes = (1 << SPA_DATA_MemPtr);

	if (!param || id != SPA_PARAM_Format)
		return;

	result = spa_format_parse(param, &obs_pw->format.media_type,
				  &obs_pw->format.media_subtype);
	if (result < 0)
		return;

	if (!spa_pod_is_fixated(param))
		return;

	if (obs_pw->format.media_type != SPA_MEDIA_TYPE_video ||
	    obs_pw->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_format_video_raw_parse(param, &obs_pw->format.info.raw);

	has_modifier = spa_pod_find_prop(param, NULL,
					 SPA_FORMAT_VIDEO_modifier) != NULL;

	// While DMABUFs on the GPU as used by screensharing is only valid with modifier
	// DMABUFs from webcams don't have any modifiers and can be mmapped
	if ((has_modifier && obs_pw->import_type == IMPORT_API_TEXTURE) ||
	    (!has_modifier && obs_pw->import_type == IMPORT_API_MEDIA)) {
		buffertypes |= (1 << SPA_DATA_DmaBuf);
	}

	blog(LOG_DEBUG, "[pipewire] Negotiated format:");

	blog(LOG_DEBUG, "[pipewire]     Format: %d (%s)",
	     obs_pw->format.info.raw.format,
	     spa_debug_type_find_name(spa_type_video_format,
				      obs_pw->format.info.raw.format));

	blog(LOG_DEBUG, "[pipewire]     Size: %dx%d",
	     obs_pw->format.info.raw.size.width,
	     obs_pw->format.info.raw.size.height);

	blog(LOG_DEBUG, "[pipewire]     Framerate: %d/%d",
	     obs_pw->format.info.raw.framerate.num,
	     obs_pw->format.info.raw.framerate.denom);

	/* Video crop */
	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	params[0] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size,
		SPA_POD_Int(sizeof(struct spa_meta_region)));

	/* Cursor */
	params[1] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size,
		SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64),
					 CURSOR_META_SIZE(1, 1),
					 CURSOR_META_SIZE(1024, 1024)));

	/* Buffer options */
	params[2] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffertypes));

	pw_stream_update_params(obs_pw->stream, params, 3);

	obs_pw->negotiated = true;
}

static void on_state_changed_cb(void *user_data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);
	UNUSED_PARAMETER(error);

	obs_pipewire_data *obs_pw = user_data;

	blog(LOG_DEBUG, "[pipewire] stream %p state: \"%s\" (error: %s)",
	     obs_pw->stream, pw_stream_state_as_string(state),
	     error ? error : "none");
}

static const struct pw_stream_events stream_events_media = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
	.process = on_process_media_cb,
};

static const struct pw_stream_events stream_events_texture = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
	.process = on_process_texture_cb,
};

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res,
			     const char *message)
{
	UNUSED_PARAMETER(seq);

	obs_pipewire_data *obs_pw = user_data;

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s", id,
	     seq, res, g_strerror(res), message);

	pw_thread_loop_signal(obs_pw->thread_loop, FALSE);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq)
{
	UNUSED_PARAMETER(seq);

	obs_pipewire_data *obs_pw = user_data;

	if (id == PW_ID_CORE)
		pw_thread_loop_signal(obs_pw->thread_loop, FALSE);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

static void connect_stream(obs_pipewire_data *obs_pw, uint32_t node)
{
	struct spa_pod_builder pod_builder;
	const struct spa_pod **params = NULL;
	uint32_t n_params;
	uint8_t params_buffer[2048];
	struct obs_video_info ovi;

	/* Stream parameters */
	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

	obs_get_video_info(&ovi);

	obs_pw->video_info = ovi;

	n_params = build_format_params(obs_pw, &pod_builder, &params);

	pw_stream_connect(obs_pw->stream, PW_DIRECTION_INPUT, node,
			  PW_STREAM_FLAG_AUTOCONNECT |
				  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, n_params);
	bfree(params);
}

obs_pipewire_data *obs_pipewire_new_for_node(int fd, uint32_t node)
{
	obs_pipewire_data *obs_pw;

	obs_pw = bzalloc(sizeof(obs_pipewire_data));
	obs_pw->pipewire_fd = fd;
	obs_pw->n_formats =
		create_modifier_info_texture(&obs_pw->modifier_info);
	obs_pw->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
	obs_pw->context = pw_context_new(
		pw_thread_loop_get_loop(obs_pw->thread_loop), NULL, 0);

	if (pw_thread_loop_start(obs_pw->thread_loop) < 0) {
		blog(LOG_WARNING, "Error starting threaded mainloop");
		goto fail;
	}

	pw_thread_loop_lock(obs_pw->thread_loop);

	/* Core */
	obs_pw->core = pw_context_connect_fd(
		obs_pw->context, fcntl(obs_pw->pipewire_fd, F_DUPFD_CLOEXEC, 5),
		NULL, 0);
	if (!obs_pw->core) {
		blog(LOG_WARNING, "Error creating PipeWire core: %m");
		pw_thread_loop_unlock(obs_pw->thread_loop);
		goto fail;
	}

	pw_core_add_listener(obs_pw->core, &obs_pw->core_listener, &core_events,
			     obs_pw);

	/* Stream */
	obs_pw->stream = pw_stream_new(
		obs_pw->core, "OBS Studio",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Screen", NULL));
	pw_stream_add_listener(obs_pw->stream, &obs_pw->stream_listener,
			       &stream_events_texture, obs_pw);
	blog(LOG_INFO, "[pipewire] created stream %p", obs_pw->stream);

	connect_stream(obs_pw, node);

	blog(LOG_INFO, "[pipewire] playing stream…");

	pw_thread_loop_unlock(obs_pw->thread_loop);

	return obs_pw;

fail:
	obs_pipewire_destroy(obs_pw);
	return NULL;
}

obs_pipewire_data *obs_pipewire_new_full(struct pw_core *core,
					 struct pw_properties *stream_props,
					 uint32_t node, enum import_type type,
					 obs_source_t *source)
{
	obs_pipewire_data *obs_pw;

	obs_pw = bzalloc(sizeof(obs_pipewire_data));

	obs_pw->source = source;

	obs_pw->import_type = type;

	switch (type) {
	case IMPORT_API_TEXTURE:
		obs_pw->n_formats =
			create_modifier_info_texture(&obs_pw->modifier_info);
		break;
	case IMPORT_API_MEDIA:
		obs_pw->n_formats =
			create_modifier_info_media(&obs_pw->modifier_info);
		break;
	}

	/* Stream */
	obs_pw->stream = pw_stream_new(core, "OBS Studio", stream_props);
	switch (type) {
	case IMPORT_API_TEXTURE:
		pw_stream_add_listener(obs_pw->stream, &obs_pw->stream_listener,
				       &stream_events_texture, obs_pw);
		break;
	case IMPORT_API_MEDIA:
		pw_stream_add_listener(obs_pw->stream, &obs_pw->stream_listener,
				       &stream_events_media, obs_pw);
		break;
	}
	blog(LOG_INFO, "[pipewire] created stream %p", obs_pw->stream);

	connect_stream(obs_pw, node);

	blog(LOG_INFO, "[pipewire] playing stream…");

	return obs_pw;
}

/* obs_source_info methods */

void obs_pipewire_destroy(obs_pipewire_data *obs_pw)
{
	if (!obs_pw)
		return;

	teardown_pipewire(obs_pw);
	destroy_session(obs_pw);

	destroy_modifier_info(obs_pw->n_formats, obs_pw->modifier_info);

	bfree(obs_pw);
}

void obs_pipewire_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "ShowCursor", true);
}

void obs_pipewire_show(obs_pipewire_data *obs_pw)
{
	if (obs_pw->stream)
		pw_stream_set_active(obs_pw->stream, true);
}

void obs_pipewire_hide(obs_pipewire_data *obs_pw)
{
	if (obs_pw->stream)
		pw_stream_set_active(obs_pw->stream, false);
}

uint32_t obs_pipewire_get_width(obs_pipewire_data *obs_pw)
{
	if (!obs_pw->negotiated)
		return 0;

	if (obs_pw->crop.valid)
		return obs_pw->crop.width;
	else
		return obs_pw->format.info.raw.size.width;
}

uint32_t obs_pipewire_get_height(obs_pipewire_data *obs_pw)
{
	if (!obs_pw->negotiated)
		return 0;

	if (obs_pw->crop.valid)
		return obs_pw->crop.height;
	else
		return obs_pw->format.info.raw.size.height;
}

void obs_pipewire_video_render(obs_pipewire_data *obs_pw, gs_effect_t *effect)
{
	gs_eparam_t *image;

	if (!obs_pw->texture)
		return;

	image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, obs_pw->texture);

	if (has_effective_crop(obs_pw)) {
		gs_draw_sprite_subregion(obs_pw->texture, 0, obs_pw->crop.x,
					 obs_pw->crop.y, obs_pw->crop.width,
					 obs_pw->crop.height);
	} else {
		gs_draw_sprite(obs_pw->texture, 0, 0, 0);
	}

	if (obs_pw->cursor.visible && obs_pw->cursor.valid &&
	    obs_pw->cursor.texture) {
		float cursor_x = obs_pw->cursor.x - obs_pw->cursor.hotspot_x;
		float cursor_y = obs_pw->cursor.y - obs_pw->cursor.hotspot_y;

		gs_matrix_push();
		gs_matrix_translate3f(cursor_x, cursor_y, 0.0f);

		gs_effect_set_texture(image, obs_pw->cursor.texture);
		gs_draw_sprite(obs_pw->texture, 0, obs_pw->cursor.width,
			       obs_pw->cursor.height);

		gs_matrix_pop();
	}
}

void obs_pipewire_set_show_cursor(obs_pipewire_data *obs_pw, bool show_cursor)
{
	obs_pw->cursor.visible = show_cursor;
}
