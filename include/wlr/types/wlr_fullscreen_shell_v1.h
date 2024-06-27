/*
 * This is a deprecated interface of wlroots. It will be removed in a future
 * version.
 */

#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FULLSCREEN_SHELL_V1_H
#define WLR_TYPES_WLR_FULLSCREEN_SHELL_V1_H

#include <wayland-server-core.h>
#include <wlr/api.h>
#include "fullscreen-shell-unstable-v1-protocol.h"

struct wlr_fullscreen_shell_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
		// struct wlr_fullscreen_shell_v1_present_surface_event
		struct wl_signal present_surface;
	} events;

	struct wl_listener display_destroy;

	void *data;
};

struct wlr_fullscreen_shell_v1_present_surface_event {
	struct wl_client *client;
	struct wlr_surface *surface; // can be NULL
	enum zwp_fullscreen_shell_v1_present_method method;
	struct wlr_output *output; // can be NULL
};

WLR_DEPRECATED("fullscreen-shell support will be removed in the next version")
struct wlr_fullscreen_shell_v1 *wlr_fullscreen_shell_v1_create(
	struct wl_display *display);

#endif
