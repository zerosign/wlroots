#ifndef TYPES_WLR_RASTER_H
#define TYPES_WLR_RASTER_H

#include <wlr/types/wlr_raster.h>

/**
 * Attaches a wlr_texture to the raster. Consumers of the raster can use the
 * given texture for their rendering if needed. The pixel contents of the texture
 * must be the same as the source buffer and other textures in the raster.
 */
void wlr_raster_attach(struct wlr_raster *raster, struct wlr_texture *texture);

/**
 * Detaches a wlr_texture from the raster. Once the texture is detached, ownership
 * of the texture is given to the caller such that the caller may mutate the
 * raster if it wishes.
 */
void wlr_raster_detach(struct wlr_raster *raster, struct wlr_texture *texture);

/**
 * Creates a wlr_raster that will attempt to reuse textures from the current
 * raster only doing partial texture uploads.
 */
struct wlr_raster *wlr_raster_update(struct wlr_raster *raster,
	struct wlr_buffer *buffer, const pixman_region32_t *damage);

#endif
