/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_FRAME_SCHEDULER_H
#define WLR_TYPES_WLR_FRAME_SCHEDULER_H

#include <wayland-server-core.h>
#include <time.h>

struct wlr_renderer;
struct wlr_render_timestamp;

#define WLR_FRAME_SCHEDULER_HISTOGRAM_LEN 128

struct wlr_frame_scheduler_bucket {
	int64_t cpu_duration_ns, gpu_duration_ns;
};

struct wlr_frame_scheduler {
	struct wlr_output *output;
	clockid_t presentation_clock;

	struct {
		struct wl_signal frame;
	} events;

	// private state

	struct wl_event_source *timer;

	struct wlr_frame_scheduler_bucket histogram[WLR_FRAME_SCHEDULER_HISTOGRAM_LEN];
	size_t histogram_cur;

	struct wl_listener output_present;

	struct {
		uint32_t commit_seq;
		struct timespec frame_emitted, frame_submitted; // CPU time
		struct timespec render_submitted; // GPU time
		struct wlr_render_timestamp *render_complete;
	} queued;
};

bool wlr_frame_scheduler_init(struct wlr_frame_scheduler *scheduler,
	struct wlr_output *output);
void wlr_frame_scheduler_finish(struct wlr_frame_scheduler *scheduler);
void wlr_frame_scheduler_mark_render_submitted(
	struct wlr_frame_scheduler *scheduler, struct wlr_renderer *renderer);

#endif
