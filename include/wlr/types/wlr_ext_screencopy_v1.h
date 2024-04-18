/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_EXT_SCREENCOPY_V1_H
#define WLR_TYPES_WLR_EXT_SCREENCOPY_V1_H

#include <wayland-server.h>
#include <time.h>
#include "ext-screencopy-v1-protocol.h"

struct wlr_ext_screencopy_manager_v1 {
	struct wl_global *global;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_ext_screencopy_frame_v1 {
	struct wl_resource *resource;
	bool capturing;
	struct wlr_buffer *buffer;
	pixman_region32_t buffer_damage;

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wlr_ext_screencopy_session_v1 *session;
};

struct wlr_ext_screencopy_manager_v1 *wlr_ext_screencopy_manager_v1_create(
	struct wl_display *display, uint32_t version);

void wlr_ext_screencopy_frame_v1_ready(struct wlr_ext_screencopy_frame_v1 *frame,
	enum wl_output_transform transform, const struct timespec *presentation_time);
void wlr_ext_screencopy_frame_v1_fail(struct wlr_ext_screencopy_frame_v1 *frame,
	enum ext_screencopy_frame_v1_failure_reason reason);

#endif
