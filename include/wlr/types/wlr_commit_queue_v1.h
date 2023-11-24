#ifndef WLR_TYPES_WLR_COMMIT_QUEUE_V1_H
#define WLR_TYPES_WLR_COMMIT_QUEUE_V1_H

#include <wayland-server-core.h>
#include "commit-queue-v1-protocol.h"

struct wlr_surface;

struct wlr_commit_queue_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_commit_queue_manager_v1 *wlr_commit_queue_manager_v1_create(struct wl_display *display, uint32_t version);
enum wp_commit_queue_v1_queue_mode wlr_commit_queue_v1_get_surface_mode(struct wlr_surface *surface);

#endif
