/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FRACTIONAL_SCALE_V1
#define WLR_TYPES_WLR_FRACTIONAL_SCALE_V1

#include <wayland-server-core.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

struct wlr_fractional_scale_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_fractional_scale_manager_v1 *wlr_fractional_scale_manager_v1_create(
	struct wl_display *display);

/**
 * Send a new server scale factor to the surface.
 *
 * If the surface doesn't have a corresponding wp_fractional_scale_v1 object,
 * this function is no-op.
 */
void wlr_fractional_scale_v1_send_scale_factor(
	struct wlr_surface *surface, double factor);

#endif
