/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_SURFACE_DAMAGE_TRACKER_H
#define WLR_TYPES_WLR_SURFACE_DAMAGE_TRACKER_H

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct wlr_surface;

struct wlr_surface_damage_tracker_surface {
	// private state
	struct wlr_surface_damage_tracker *tracker;
	struct wlr_surface *surface;

	bool mapped;
	int32_t width, height;
	struct wlr_fbox viewport_src;

	struct wl_list subsurfaces;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener new_subsurface;
};

struct wlr_surface_damage_tracker {
	struct {
		struct wl_signal damage; // struct wlr_surface_damage_tracker_damage_event
	} events;

	// private state
	struct wlr_surface_damage_tracker_surface surface;
	bool has_surface;
	struct wl_listener surface_destroy;
};

struct wlr_surface_damage_tracker_damage_event {
	pixman_region32_t *damage;
};

struct wlr_surface_damage_tracker *wlr_surface_damage_tracker_create(
	struct wlr_surface *surface);

void wlr_surface_damage_tracker_destroy(struct wlr_surface_damage_tracker *tracker);

#endif
