#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_raster.h>
#include "types/wlr_buffer.h"

void wlr_texture_init(struct wlr_texture *texture,
		const struct wlr_texture_impl *impl, uint32_t width, uint32_t height) {
	memset(texture, 0, sizeof(*texture));
	texture->impl = impl;
	texture->width = width;
	texture->height = height;
}

void wlr_texture_destroy(struct wlr_texture *texture) {
	if (!texture) {
		 return;
	}
	
	if (texture->raster) {
		wlr_raster_detach(texture->raster, texture);
		texture->raster = NULL;
	}

	if (texture->impl && texture->impl->destroy) {
		texture->impl->destroy(texture);
	} else {
		free(texture);
	}
}

struct wlr_texture *wlr_texture_from_buffer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	if (!renderer->impl->texture_from_buffer) {
		return NULL;
	}
	return renderer->impl->texture_from_buffer(renderer, buffer);
}

bool wlr_texture_update_from_buffer(struct wlr_texture *texture,
		struct wlr_buffer *buffer, pixman_region32_t *damage) {
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
