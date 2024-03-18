/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_HEADLESS_H
#define WLR_BACKEND_HEADLESS_H

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

/**
 * Set phase of the vblank refresh cycle.
 *
 * The phase is calculated as (vblank_nsec%vblank_period), where vblank_period
 * is the refresh time in nanoseconds set in the wlr_output's mode.
 * This is used to calculate when to send the wlr_output's 'present' event signals.
 */
void wlr_headless_output_set_vblank_phase(struct wlr_backend *wlr_backend,
	struct wlr_output *wlr_output, uint64_t vblank_nsec);

/**
 * Creates a headless backend. A headless backend has no outputs or inputs by
 * default.
 */
struct wlr_backend *wlr_headless_backend_create(struct wl_event_loop *loop);
/**
 * Create a new headless output.
 *
 * The buffers presented on the output won't be displayed to the user.
 */
struct wlr_output *wlr_headless_add_output(struct wlr_backend *backend,
	unsigned int width, unsigned int height);

bool wlr_backend_is_headless(struct wlr_backend *backend);
bool wlr_output_is_headless(struct wlr_output *output);

#endif
