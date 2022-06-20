/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_WLR_TEXTURE_H
#define WLR_RENDER_WLR_TEXTURE_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_raster.h>

struct wlr_renderer;
struct wlr_texture_impl;

struct wlr_texture {
	const struct wlr_texture_impl *impl;
	uint32_t width, height;

	struct wlr_raster *raster;
	struct wl_list link;
};

/**
  * Update a texture with a struct wlr_raster's contents.
  *
  * The update might be rejected (in case the texture is immutable, the raster
  * doesn't have a compatible source, unsupported type/format, etc), so callers
  * must be prepared to fall back.
  *
  * The damage can be used by the renderer as an optimization: only the supplied
  * region needs to be updated.
  */
bool wlr_texture_update_from_raster(struct wlr_texture *texture,
	struct wlr_raster *raster, pixman_region32_t *damage);

/**
 * Destroys the texture.
 */
void wlr_texture_destroy(struct wlr_texture *texture);

#endif
