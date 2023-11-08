/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#include <stdint.h>
#include <wayland-server.h>
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_TIMING_H
#define WLR_TYPES_WLR_TIMING_H

struct wlr_timing_manager {
	struct wl_global *global;
	struct wl_resource *resource;
	struct wl_list clients;

	uint64_t latch_delay_nsec;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal destroy;
	} events;
};

struct wlr_timing_client {
	struct wlr_timing_manager *timing_manager;
	struct wl_resource *resource;
	struct wl_list link; // wlr_timing_manager.clients

	struct wlr_timer *timer;

	struct wl_listener surface_destroy;
};

enum timer_stage {
	TIMER_STAGE_LATCH = 0,
	TIMER_STAGE_PRESENT = 1
};

enum timer_rounding_mode {
	TIMER_ROUNDING_NEAREST = 0,
	TIMER_ROUNDING_NOT_BEFORE = 1
};

struct timer_output {
	struct wlr_output *output;
	uint64_t last_present_nsec;
	int refresh;
	struct wl_listener output_present;

	struct wl_list link; // wlr_timer::outputs
};

struct timer_commit {
	struct wlr_timer *timer;

	enum timer_stage stage;
	enum timer_rounding_mode rounding_mode;
	uint64_t timestamp_nsec;

	/**
	 * wlr_surface's pending sequence when locking through
	 * wlr_surface_lock_pending().
	 */
	uint32_t pending_seq;

	struct wl_event_source *commit_unlock_timer;

	struct wl_list link; // wlr_timer::commits
};

struct wlr_timer {
	struct wlr_timing_manager *manager;

	struct wl_resource *resource;
	struct wl_display *wl_display;

	struct wlr_surface *surface;
	struct wl_listener surface_client_commit;

	struct wl_listener surface_output_enter;

	enum timer_stage curr_stage;
	enum timer_rounding_mode curr_rounding_mode;
	uint64_t curr_timestamp_nsec;

	struct wl_list commits; // timer_commit::link
	struct timer_output *primary_output;
	struct wl_list outputs; // timer_output::link
};

/**
 * Used by the compositor to set the time offset after the beginning of a refresh cycle
 * when it will render and commit a new frame.
 */
void wlr_timing_manager_set_latch_delay(struct wlr_timing_manager *timing_manager,
	uint64_t latch_delay_nsec);

/**
 * Create the wp_commit_timing_manager_v1_interface global, which can be used by clients to
 * set timestamps for surface commit request presentation.
 */
struct wlr_timing_manager *wlr_timing_manager_create(struct wl_display *display,
	uint32_t version);

#endif
