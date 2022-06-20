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

bool wlr_texture_update_from_raster(struct wlr_texture *texture,
		struct wlr_raster *raster, pixman_region32_t *damage) {
	if (!texture->impl->update_from_raster) {
		return false;
	}
	if (texture->width != raster->width || texture->height != raster->height) {
		return false;
	}
	const pixman_box32_t *extents = pixman_region32_extents(damage);
	if (extents->x1 < 0 || extents->y1 < 0 || extents->x2 > (int32_t)raster->width ||
			extents->y2 > (int32_t)raster->height) {
		return false;
	}
	return texture->impl->update_from_raster(texture, raster, damage);
}
