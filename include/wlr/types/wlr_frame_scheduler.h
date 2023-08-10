/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FRAME_SCHEDULER_H
#define WLR_TYPES_WLR_FRAME_SCHEDULER_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_frame_scheduler_impl;

struct wlr_frame_scheduler {
	const struct wlr_frame_scheduler_impl *impl;
	struct wlr_output *output;
	struct wl_signal frame;
	struct wl_listener needs_frame;
};

/*
 * The present-idle scheduler maintains a render loop based on `wlr_output.events.present`. To wake
 * the render loop, it emits the frame signal when the compositor's event loop is idle.
 */
struct wlr_frame_scheduler *wlr_present_idle_scheduler_create(struct wlr_output *output);
/*
 * The Wayland-idle scheduler maintains a render loop based on wl_surface.frame callbacks. To wake
 * the render loop, it emits the frame signal when the compositor's event loop is idle.
 */
struct wlr_frame_scheduler *wlr_wl_idle_scheduler_create(struct wlr_output *output);

/*
 * Creates an appropriate frame scheduler for the given output's backend capabilities.
 */
struct wlr_frame_scheduler *wlr_frame_scheduler_autocreate(struct wlr_output *output);
/*
 * Inform the scheduler that a frame signal is needed. The scheduler implementation will choose a
 * good time to emit the signal. The signal is emitted only if this function has been called at
 * least once since the last signal.
 */
void wlr_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *scheduler);
void wlr_frame_scheduler_destroy(struct wlr_frame_scheduler *scheduler);

#endif
