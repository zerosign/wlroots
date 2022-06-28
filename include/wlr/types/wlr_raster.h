/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_RASTER_H
#define WLR_TYPES_WLR_RASTER_H

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>

struct wlr_buffer;
struct wlr_texture;

struct wlr_raster {
	// May be NULL
	struct wlr_buffer *buffer;
	uint32_t width, height;

	struct wl_list sources; // struct wlr_texture

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	size_t n_locks;
};

/**
 * Creates a new wlr_raster being backed by the given buffer.
 * The creation funciton is referenced: once the creator is done with the raster,
 * wlr_raster_unlock must be called.
 */
struct wlr_raster *wlr_raster_create(struct wlr_buffer *buffer);

/**
 * Removes and unlocks the buffer assolated with this raster. A raster must be
 * created with a buffer so that there is a source of information for textures
 * to be created from it, but once there is a texture, that can be used
 * as the source of truth and so the buffer can be removed early for other
 * purposes.
 */
void wlr_raster_remove_buffer(struct wlr_raster *raster);

/**
 * Lock the raster for use. As long as the raster has at least one lock, it
 * will not be destroyed. The raster will be created with a reference count at 1
 * meaning that whatever produced the raster, must call this funciton.
 */
struct wlr_raster *wlr_raster_lock(struct wlr_raster *raster);

/**
 * Unlock the raster. This must be called after wlr_raster_lock once the raster
 * has been finished being used.
 */
void wlr_raster_unlock(struct wlr_raster *raster);

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

#endif
