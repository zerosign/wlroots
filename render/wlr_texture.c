#include <assert.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_texture.h>
#include "render/pixel_format.h"
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/log.h>
#include "types/wlr_buffer.h"
#include "backend/multi.h"
#include "backend/drm/drm.h"
#include "render/drm_format_set.h"
#include "render/wlr_renderer.h"

void wlr_texture_init(struct wlr_texture *texture, struct wlr_renderer *renderer,
		const struct wlr_texture_impl *impl, uint32_t width, uint32_t height) {
	assert(renderer);

	*texture = (struct wlr_texture){
		.renderer = renderer,
		.impl = impl,
		.width = width,
		.height = height,
	};
}

void wlr_texture_destroy(struct wlr_texture *texture) {
	if (texture && texture->impl && texture->impl->destroy) {
		texture->impl->destroy(texture);
	} else {
		free(texture);
	}
}

void wlr_texture_read_pixels_options_get_src_box(
		const struct wlr_texture_read_pixels_options *options,
		const struct wlr_texture *texture, struct wlr_box *box) {
	if (wlr_box_empty(&options->src_box)) {
		*box = (struct wlr_box){
			.x = 0,
			.y = 0,
			.width = texture->width,
			.height = texture->height,
		};
		return;
	}

	*box = options->src_box;
}

void *wlr_texture_read_pixel_options_get_data(
		const struct wlr_texture_read_pixels_options *options) {
	const struct wlr_pixel_format_info *fmt = drm_get_pixel_format_info(options->format);

	return (char *)options->data +
		pixel_format_info_min_stride(fmt, options->dst_x) +
		options->dst_y * options->stride;
}

bool wlr_texture_read_pixels(struct wlr_texture *texture,
		const struct wlr_texture_read_pixels_options *options) {
	if (!texture->impl->read_pixels) {
		return false;
	}

	return texture->impl->read_pixels(texture, options);
}

uint32_t wlr_texture_preferred_read_format(struct wlr_texture *texture) {
	if (!texture->impl->preferred_read_format) {
		return DRM_FORMAT_INVALID;
	}

	return texture->impl->preferred_read_format(texture);
}

struct wlr_texture *wlr_texture_from_pixels(struct wlr_renderer *renderer,
		uint32_t fmt, uint32_t stride, uint32_t width, uint32_t height,
		const void *data) {
	assert(width > 0);
	assert(height > 0);
	assert(stride > 0);
	assert(data);

	struct wlr_readonly_data_buffer *buffer =
		readonly_data_buffer_create(fmt, stride, width, height, data);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	readonly_data_buffer_drop(buffer);

	return texture;
}

struct wlr_texture *wlr_texture_from_dmabuf(struct wlr_renderer *renderer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_create(attribs);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	dmabuf_buffer_drop(buffer);

	return texture;
}

struct wlr_texture *wlr_texture_from_buffer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	if (!renderer->impl->texture_from_buffer) {
		return NULL;
	}

	struct wlr_dmabuf_attributes dmabuf;
	/*
	 * If this is a dmabuf backed buffer then get the format/modifier for it and
	 * compare it with the set supported by the renderer
	 */
	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		const struct wlr_drm_format_set *formats = wlr_renderer_get_dmabuf_texture_formats(renderer);
		if (!formats) {
			wlr_log(WLR_DEBUG, "Could not get DRM format set for renderer");
			return NULL;
		}

		if (!wlr_drm_format_set_has(formats, dmabuf.format, dmabuf.modifier)) {
			wlr_log(WLR_DEBUG, "Renderer could not import buffer with format 0x%x and modifier 0x%lx",
					dmabuf.format, dmabuf.modifier);
			return NULL;
		}
	}

	return renderer->impl->texture_from_buffer(renderer, buffer);
}

bool wlr_texture_update_from_buffer(struct wlr_texture *texture,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	if (!texture->impl->update_from_buffer) {
		return false;
	}
	if (texture->width != (uint32_t)buffer->width ||
			texture->height != (uint32_t)buffer->height) {
		return false;
	}
	const pixman_box32_t *extents = pixman_region32_extents(damage);
	if (extents->x1 < 0 || extents->y1 < 0 || extents->x2 > buffer->width ||
			extents->y2 > buffer->height) {
		return false;
	}
	return texture->impl->update_from_buffer(texture, buffer, damage);
}

struct wlr_texture_set *wlr_texture_set_from_dmabuf(struct wlr_renderer *renderer,
	struct wlr_dmabuf_attributes *attribs) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_create(attribs);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture_set *set =
		wlr_texture_set_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	dmabuf_buffer_drop(buffer);

	return set;
}

static void texture_set_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct wlr_texture_set *set = wl_container_of(listener, set, buffer_release);
	set->buffer = NULL;
	wl_list_remove(&set->buffer_release.link);
}

static void wlr_texture_set_add_pair(struct wlr_texture_set *set, struct wlr_renderer *renderer,
		struct wlr_allocator *allocator) {

	set->pairings = realloc(set->pairings,
			sizeof(struct wlr_texture_renderer_pair) * (set->pairing_count + 1));
	if (!set->pairings) {
		return;
	}

	memset(&set->pairings[set->pairing_count], 0, sizeof(struct wlr_texture_renderer_pair));
	set->pairings[set->pairing_count].renderer = renderer;
	set->pairings[set->pairing_count].allocator = allocator;
	set->pairing_count++;
}

void wlr_texture_set_add_renderer(struct wlr_texture_set *set, struct wlr_renderer *renderer,
		struct wlr_allocator *allocator) {
	if (!renderer) {
		return;
	}

	wlr_texture_set_add_pair(set, renderer, allocator);

	if (renderer->multi_gpu) {
		set->multi_gpu = renderer->multi_gpu;
		/* Now add each mgpu renderer to the set */
		struct wlr_multi_gpu_device *device;
		wl_list_for_each(device, &renderer->multi_gpu->devices, link) {
			wlr_texture_set_add_pair(set, device->renderer, device->allocator);
		}
	}
}

/*
 * When setting up our wlr_multi_gpu struct we put all renderers into a list. This lets us
 * iterate them from here. If this request is made on a renderer not in the multi-GPU set,
 * then the list will be of length 1, and the renderer will be the only entry in the set.
 */
struct wlr_texture_set *wlr_texture_set_create(struct wlr_renderer *renderer,
		struct wlr_allocator *allocator) {
	struct wlr_texture_set *set = calloc(1, sizeof(struct wlr_texture_set));
	if (!set) {
		return NULL;
	}
	set->native_pair = -1;

	wlr_texture_set_add_renderer(set, renderer, allocator);

	return set;
}

/*
 * Helper for importing a buffer into the texture set. This initializes
 * the native_pair internal state.
 */
bool wlr_texture_set_import_buffer(struct wlr_texture_set *set, struct wlr_buffer *buffer) {
	set->buffer = buffer;
	// Don't lock our buffer since it gets in the way of releasing shm buffers immediately
	// Instead keep a reference to the buffer but register a handler to notify us when
	// it is released and clear the pointer.
	set->buffer_release.notify = texture_set_handle_buffer_release;
	wl_signal_add(&set->buffer->events.release, &set->buffer_release);

	buffer = wlr_buffer_lock(buffer);
	bool ret = false;

	/*
	 * For each renderer, try to create a texture. Go in order, since the first 
	 * entry is always the "primary" renderer that the user created this texture set with.
	 * The odds are highest that it is importable into that renderer, so start with that
	 * one.
	 */
	for (int i = 0; i < set->pairing_count; i++) {
		assert(!set->pairings[i].texture);
		set->pairings[i].texture = wlr_texture_from_buffer(set->pairings[i].renderer, buffer);
		/* If we got a match, mark this renderer as the "native" one the buffer is local to */
		if (set->pairings[i].texture) {
			/* Cache the width and height so other places don't have to search for it in pairings */
			set->width = set->pairings[i].texture->width;
			set->height = set->pairings[i].texture->height;
			set->native_pair = i;
			ret = true;
			goto buffer_unlock;
		}
	}

buffer_unlock:
	wlr_buffer_unlock(buffer);
	return ret;
}

struct wlr_texture_set *wlr_texture_set_from_buffer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	/* Get an empty texture set */
	struct wlr_texture_set *set = wlr_texture_set_create(renderer, NULL);
	if (!set) {
		return NULL;
	}

	if (!wlr_texture_set_import_buffer(set, buffer)) {
		goto fail;
	}

	return set;

fail:
	/* If the buffer couldn't be imported into any renderer in the system, return NULL */
	wlr_texture_set_destroy(set);
	return NULL;
}

static struct wlr_buffer *texture_set_blit_gpu_buffer(struct wlr_texture_set *set,
	struct wlr_renderer *renderer) {
	struct wlr_renderer *native_renderer = set->pairings[set->native_pair].renderer;
	struct wlr_allocator *native_allocator = set->pairings[set->native_pair].allocator;
	struct wlr_texture *native_texture = set->pairings[set->native_pair].texture;
	assert(native_texture);

	// If the user didn't give us an allocator for this renderer then this path can't be used.
	if (!native_allocator) {
		return NULL;
	}

	// Now intersect our DRM formats
	const struct wlr_drm_format_set *src_formats = wlr_renderer_get_render_formats(native_renderer);
	if (!src_formats) {
		wlr_log(WLR_ERROR, "Failed to get primary renderer DRM formats");
		return NULL;
	}

	const struct wlr_drm_format_set *dst_formats = wlr_renderer_get_dmabuf_texture_formats(renderer);
	if (!dst_formats) {
		wlr_log(WLR_ERROR, "Failed to get destination renderer DRM formats");
		return NULL;
	}

	// Get the argb8 mods to use for our new buffer
	struct wlr_drm_format argb_format = {0};
	if (!wlr_drm_format_intersect(&argb_format,
				wlr_drm_format_set_get(dst_formats, DRM_FORMAT_ARGB8888),
				wlr_drm_format_set_get(src_formats, DRM_FORMAT_ARGB8888))
			|| argb_format.len == 0) {
		wlr_log(WLR_ERROR, "Failed to intersect DRM formats");
		return NULL;
	}

	// Allocate a new buffer on the source renderer, we will blit the original texture
	// to this and then return it so the caller can import it.
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(
		native_allocator, set->width, set->height, &argb_format);
	wlr_drm_format_finish(&argb_format);
	if (!buffer) {
		wlr_log(WLR_ERROR, "Failed to allocate buffer on source GPU");
		return NULL;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(native_renderer, buffer, NULL);
	if (!pass) {
		wlr_log(WLR_ERROR, "Failed to create a render pass");
		goto drop_buffer;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options) {
		.texture = native_texture,
	});

	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "Failed to render to buffer");
		goto drop_buffer;
	}

	return buffer;

drop_buffer:
	wlr_buffer_drop(buffer);
	return NULL;
}

void *wlr_texture_set_get_linear_data(struct wlr_texture_set *set) {
	struct wlr_renderer *native_renderer = set->pairings[set->native_pair].renderer;
	struct wlr_texture *native_texture = set->pairings[set->native_pair].texture;
	assert(native_texture);
	int stride = native_texture->width * 4;

	if (set->pixel_data) {
		return set->pixel_data;
	}

	set->pixel_data = malloc(native_texture->height * stride);
	if (!set->pixel_data) {
		return NULL;
	}

	struct wlr_buffer *buffer = set->buffer;
	if (!set->buffer) {
		// If the buffer this set was created with has already been released, blit ourselves
		// a new one.
		buffer = texture_set_blit_gpu_buffer(set, native_renderer);
		if (!buffer) {
			wlr_log(WLR_DEBUG, "Cannot get linear data, wlr_texture_set's buffer was released");
			return NULL;
		}
	}
	wlr_buffer_lock(buffer);

	/* Make a buffer with a linear layout and the same format */
	set->format = wlr_texture_preferred_read_format(native_texture);
    if (set->format == DRM_FORMAT_INVALID) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}

	bool result = wlr_texture_read_pixels(native_texture, &(struct wlr_texture_read_pixels_options) {
		.format = DRM_FORMAT_ARGB8888,
		.stride = stride,
		.data = set->pixel_data,
	});
	wlr_buffer_unlock(buffer);
	if (!result) {
		return NULL;
	}

	wlr_log(WLR_DEBUG, "Copied GPU vidmem buffer to linear sysmem buffer");
	return set->pixel_data;
}

struct wlr_texture *wlr_texture_set_get_tex_for_renderer(struct wlr_texture_set *set,
	struct wlr_renderer *renderer) {
	/* Find the entry for this renderer */
	struct wlr_texture_renderer_pair *pair = NULL;
	for (int i = 0; i < set->pairing_count; i++) {
		if (set->pairings[i].renderer == renderer) {
			pair = &set->pairings[i];
		}
	}

	/*
	 * If we have not seen this renderer then add an entry for it so
	 * we can cache the results of this copy.
	 */
	if (!pair) {
		wlr_texture_set_add_pair(set, renderer, NULL);
		pair = &set->pairings[set->pairing_count - 1];
	}

	/* If we already have a texture for this renderer, return it */
	if (pair->texture) {
        return pair->texture;
	}

	/*
	 * First try to directly import the texture. We must have a valid buffer
	 * to lock in order to do this. If the buffer has been released (as is the
	 * case with shm buffers) then we will have to perform a fallback copy.
	 */
	if (set->buffer) {
		wlr_buffer_lock(set->buffer);
		pair->texture = wlr_texture_from_buffer(renderer, set->buffer);
		wlr_buffer_unlock(set->buffer);
		if (pair->texture) {
	        return pair->texture;
		}
	}

	/*
	 * Directly importing didn't work. The next thing to try is blitting to a compatible
	 * GPU texture and then importing that.
	 */
	struct wlr_buffer *buffer = texture_set_blit_gpu_buffer(set, renderer);
	if (buffer) {
		pair->texture = wlr_texture_from_buffer(renderer, buffer);
		wlr_buffer_drop(buffer);
		if (pair->texture) {
	        return pair->texture;
		}
	}

	/*
	 * If the above didn't work then we can try a CPU fallback. This is much more expensive
	 * but should always work. The reason we need this is that sometimes we have to copy
	 * from GPU A to GPU B, but GPU A can't render to any modifiers that GPU B supports. This
	 * happens on NVIDIA (among others) where you cannot render to a linear texture, but need
	 * to convert to linear so that you can import it anywhere.
	 *
	 * Get our linear pixel data so we can import it into the target renderer.
	 * */
	void *pixel_data = wlr_texture_set_get_linear_data(set);
	if (!pixel_data) {
        return NULL;
	}

	/* import the linear texture into our renderer */
	uint32_t stride = set->width * 4;
	pair->texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888, stride, set->width,
			set->height, pixel_data);

    return pair->texture;
}

struct wlr_texture *wlr_texture_set_get_native_texture(struct wlr_texture_set *set) {
	return set->pairings[set->native_pair].texture;
}

bool wlr_texture_set_update_from_buffer(struct wlr_texture_set *set,
		struct wlr_buffer *next, const pixman_region32_t *damage) {
	/* Call wlr_texture_write_pixels on each valid texture in the set */
	for (int i = 0; i < set->pairing_count; i++) {
		if (set->pairings[i].texture) {
			if (!wlr_texture_update_from_buffer(set->pairings[i].texture,
						next, damage)) {
				return false;
			}
		}
	}

	return true;
}

void wlr_texture_set_destroy(struct wlr_texture_set *set) {
	if (set->buffer) {
		wl_list_remove(&set->buffer_release.link);
	}
	free(set->pixel_data);

	for (int i = 0; i < set->pairing_count; i++) {
		if (set->pairings[i].texture) {
			wlr_texture_destroy(set->pairings[i].texture);
		}
	}

	if (set) {
		free(set->pairings);
		free(set);
	}
}
