/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_XDG_TOPLEVEL_ICON_V1_H
#define WLR_TYPES_WLR_XDG_TOPLEVEL_ICON_V1_H

#include <wayland-server-core.h>

#include <wlr/util/addon.h>

struct wlr_xdg_toplevel_icon_manager_v1 {
	struct wl_global *global;

	struct wl_list resources;

	int *sizes;
	size_t n_sizes;

	struct {
		struct wl_signal set_icon; // struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event {
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_icon_v1 *icon; // May be NULL
};

struct wlr_xdg_toplevel_icon_v1_buffer {
	struct wlr_buffer *buffer;
	int scale;

	struct wl_list link; // wlr_xdg_toplevel_icon_v1.buffers
};

struct wlr_xdg_toplevel_icon_v1 {
	char *name; // May be NULL
	struct wl_list buffers; // wlr_xdg_toplevel_icon_v1_buffer.link

	// private state

	int n_locks;
	bool immutable;
};

struct wlr_xdg_toplevel_icon_manager_v1 *wlr_xdg_toplevel_icon_manager_v1_create(
		struct wl_display *display, uint32_t version);

void wlr_xdg_toplevel_icon_manager_v1_set_sizes(struct wlr_xdg_toplevel_icon_manager_v1 *manager,
		int *sizes, size_t n_sizes);

void wlr_xdg_toplevel_icon_v1_lock(struct wlr_xdg_toplevel_icon_v1 *icon);
void wlr_xdg_toplevel_icon_v1_unlock(struct wlr_xdg_toplevel_icon_v1 *icon);

#endif
