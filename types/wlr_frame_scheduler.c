#include <assert.h>
#include <backend/headless.h>
#include <stdlib.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_frame_scheduler.h>
#include <wlr/types/wlr_frame_scheduler.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

void wlr_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *scheduler) {
	scheduler->impl->schedule_frame(scheduler);
}

void wlr_frame_scheduler_destroy(struct wlr_frame_scheduler *scheduler) {
	wl_list_remove(&scheduler->needs_frame.link);
	scheduler->impl->destroy(scheduler);
}

static void frame_scheduler_handle_needs_frame(struct wl_listener *listener, void *data) {
	struct wlr_frame_scheduler *scheduler = wl_container_of(listener, scheduler, needs_frame);
	wlr_frame_scheduler_schedule_frame(scheduler);
}

static void frame_scheduler_init(struct wlr_frame_scheduler *scheduler,
		struct wlr_frame_scheduler_impl *impl, struct wlr_output *output) {
	*scheduler = (struct wlr_frame_scheduler){0};
	assert(impl->schedule_frame);
	assert(impl->destroy);

	wl_signal_init(&scheduler->frame);
	scheduler->impl = impl;
	scheduler->output = output;
	scheduler->needs_frame.notify = frame_scheduler_handle_needs_frame;
	wl_signal_add(&output->events.needs_frame, &scheduler->needs_frame);
}

// This struct and its methods are a common base for frame schedulers that restart their render loop
// via an idle event source, which fires "soon", instead of using a more complex schedule. Deferring
// the frame to an idle event is a crude way of ensuring that work done after scheduling the frame
// gets picked up by the renderer, rather than rendering happening inside the schedule call and
// missing out on any immediately following updates.
struct idle_frame_scheduler {
	struct wlr_frame_scheduler base;
	struct wl_event_source *idle;
	// Whether the render loop is already awake, i.e. whether frames from idle events should be
	// inhibited.
	bool frame_pending;
	// Whether the render loop should be kept awake. True iff wlr_frame_scheduler_schedule_frame() was
	// called since the last frame event.
	bool needs_frame;
};

static void idle_frame_scheduler_handle_idle(void *data) {
	struct idle_frame_scheduler *scheduler = data;
	if (!scheduler->frame_pending) {
		wl_signal_emit_mutable(&scheduler->base.frame, NULL);
	}
	scheduler->idle = NULL;
}

static void idle_frame_scheduler_schedule_frame(struct wlr_frame_scheduler *wlr_scheduler) {
	struct idle_frame_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base);
	scheduler->needs_frame = true;
	if (scheduler->idle != NULL || scheduler->frame_pending) {
		// Either we are already set up to restart the render loop or it is already running.
		return;
	}

	struct wl_event_loop *ev = wl_display_get_event_loop(scheduler->base.output->display);
	scheduler->idle = wl_event_loop_add_idle(ev, idle_frame_scheduler_handle_idle, scheduler);
}

static void idle_frame_scheduler_set_frame_pending(struct idle_frame_scheduler *scheduler) {
	scheduler->frame_pending = true;
	if (scheduler->idle) {
		wl_event_source_remove(scheduler->idle);
		scheduler->idle = NULL;
	}
}

static void idle_frame_scheduler_emit_frame(struct idle_frame_scheduler *scheduler) {
	if (!scheduler->needs_frame) {
		return;
	}
	scheduler->needs_frame = false;
	wl_signal_emit_mutable(&scheduler->base.frame, NULL);
}

static void idle_frame_scheduler_finish(struct idle_frame_scheduler *scheduler) {
	if (scheduler->idle) {
		wl_event_source_remove(scheduler->idle);
	}
}

// The present idle scheduler builds on the idle_frame_scheduler's logic for restarting the render
// loop, and drives the render loop using `wlr_output.events.present`.
struct present_idle_scheduler {
	struct idle_frame_scheduler base;
	struct wl_listener commit;
	struct wl_listener present;
};

static void present_idle_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct present_idle_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_list_remove(&scheduler->commit.link);
	wl_list_remove(&scheduler->present.link);
	free(scheduler);
}

static void present_idle_scheduler_handle_commit(struct wl_listener *listener, void *data) {
	struct present_idle_scheduler *scheduler = wl_container_of(listener, scheduler, commit);
	struct wlr_output_event_commit *event = data;

	if (event->committed & WLR_OUTPUT_STATE_BUFFER) {
		idle_frame_scheduler_set_frame_pending(&scheduler->base);
	}
}

static void present_idle_scheduler_handle_present(struct wl_listener *listener, void *data) {
	struct present_idle_scheduler *scheduler = wl_container_of(listener, scheduler, present);
	struct wlr_output_event_present *present = data;
	if (present->presented) {
		scheduler->base.frame_pending = false;
		idle_frame_scheduler_emit_frame(&scheduler->base);
	}
}

struct wlr_frame_scheduler_impl present_idle_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = present_idle_scheduler_destroy,
};

struct wlr_frame_scheduler *wlr_present_idle_scheduler_create(struct wlr_output *output) {
	struct present_idle_scheduler *scheduler = calloc(1, sizeof(struct present_idle_scheduler));
	if (!scheduler) {
		return NULL;
	}
	frame_scheduler_init(&scheduler->base.base, &present_idle_scheduler_impl, output);
	scheduler->commit.notify = present_idle_scheduler_handle_commit;
	wl_signal_add(&output->events.commit, &scheduler->commit);
	scheduler->present.notify = present_idle_scheduler_handle_present;
	wl_signal_add(&output->events.present, &scheduler->present);
	return &scheduler->base.base;
}

// This scheduler builds on idle_frame_scheduler and uses Wayland's frame callbacks for driving the
// render loop.
struct wl_idle_scheduler {
	struct idle_frame_scheduler base;
	struct wl_callback *frame;
	struct wl_listener precommit;
};

static void wl_idle_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct wl_idle_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_callback_destroy(scheduler->frame);
	free(scheduler);
}

static void wl_idle_scheduler_handle_frame(void *data, struct wl_callback *cb, uint32_t time) {
	struct wl_idle_scheduler *scheduler = data;
	assert(scheduler->frame == cb);
	scheduler->base.frame_pending = false;
	idle_frame_scheduler_emit_frame(&scheduler->base);
}

struct wl_callback_listener wl_idle_scheduler_frame_listener = {
	.done = wl_idle_scheduler_handle_frame,
};

static void wl_idle_scheduler_handle_precommit(struct wl_listener *listener, void *data) {
	struct wl_idle_scheduler *scheduler = wl_container_of(listener, scheduler, precommit);
	struct wlr_output_event_precommit *precommit = data;
	if (!(precommit->state->committed & (WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_LAYERS))) {
		return;
	}
	idle_frame_scheduler_set_frame_pending(&scheduler->base);
	if (scheduler->frame != NULL) {
		wl_callback_destroy(scheduler->frame);
	}
	struct wl_surface *surface = wlr_wl_output_get_surface(scheduler->base.base.output);
	scheduler->frame = wl_surface_frame(surface);
	wl_callback_add_listener(scheduler->frame, &wl_idle_scheduler_frame_listener, scheduler);
}

struct wlr_frame_scheduler_impl wl_idle_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = wl_idle_scheduler_destroy,
};

static struct wlr_frame_scheduler *wl_idle_scheduler_create(struct wlr_output *output) {
	if (!wlr_output_is_wl(output)) {
		return NULL;
	}

	struct wl_idle_scheduler *scheduler = calloc(1, sizeof(struct wl_idle_scheduler));
	if (!scheduler) {
		return NULL;
	}
	frame_scheduler_init(&scheduler->base.base, &wl_idle_scheduler_impl, output);
	scheduler->precommit.notify = wl_idle_scheduler_handle_precommit;
	wl_signal_add(&output->events.precommit, &scheduler->precommit);

	return &scheduler->base.base;
}

#define HEADLESS_DEFAULT_REFRESH (60 * 1000) // 60 Hz

struct headless_idle_scheduler {
	struct idle_frame_scheduler base;
	struct wl_event_source *frame_timer;
	struct wl_listener commit;

	int32_t frame_delay;
};

static void headless_idle_scheduler_destroy(struct wlr_frame_scheduler *wlr_scheduler) {
	struct headless_idle_scheduler *scheduler = wl_container_of(wlr_scheduler, scheduler, base.base);
	idle_frame_scheduler_finish(&scheduler->base);
	wl_event_source_remove(scheduler->frame_timer);
	free(scheduler);
}

static void headless_idle_scheduler_handle_commit(struct wl_listener *listener, void *data) {
	struct headless_idle_scheduler *scheduler = wl_container_of(listener, scheduler, commit);
	struct wlr_output_event_commit *commit = data;
	struct wlr_output *output = commit->output;
	if (commit->committed & WLR_OUTPUT_STATE_MODE) {
		int32_t refresh = output->refresh ? output->refresh : HEADLESS_DEFAULT_REFRESH;
		scheduler->frame_delay = 1000 * 1000 / refresh;
	}

	if (commit->committed & WLR_OUTPUT_STATE_BUFFER) {
		assert(scheduler->frame_delay != 0);
		wl_event_source_timer_update(scheduler->frame_timer, scheduler->frame_delay);
		idle_frame_scheduler_set_frame_pending(&scheduler->base);
	}
}

static int headless_idle_scheduler_handle_timer(void *data) {
	struct headless_idle_scheduler *scheduler = data;
	scheduler->base.frame_pending = false;
	idle_frame_scheduler_emit_frame(&scheduler->base);
	return 0;
}

struct wlr_frame_scheduler_impl headless_idle_scheduler_impl = {
	.schedule_frame = idle_frame_scheduler_schedule_frame,
	.destroy = headless_idle_scheduler_destroy,
};

struct wlr_frame_scheduler *wlr_headless_idle_scheduler_create(struct wlr_output *output) {
	if (!wlr_output_is_headless(output)) {
		return NULL;
	}

	struct headless_idle_scheduler *scheduler = calloc(1, sizeof(struct headless_idle_scheduler));
	if (!scheduler) {
		return NULL;
	}
	frame_scheduler_init(&scheduler->base.base, &headless_idle_scheduler_impl, output);

	scheduler->frame_delay = 1000 * 1000 / HEADLESS_DEFAULT_REFRESH;

	struct wl_event_loop *ev = wl_display_get_event_loop(output->display);
	scheduler->frame_timer = wl_event_loop_add_timer(ev, headless_idle_scheduler_handle_timer,
		scheduler);

	scheduler->commit.notify = headless_idle_scheduler_handle_commit;
	wl_signal_add(&output->events.commit, &scheduler->commit);

	return &scheduler->base.base;
}

struct wlr_frame_scheduler *wlr_frame_scheduler_autocreate(struct wlr_output *output) {
	if (wlr_output_is_wl(output) && !wlr_wl_backend_has_presentation_time(output->backend)) {
		wlr_log(WLR_INFO, "wp_presentation not available, falling back to frame callbacks");
		return wl_idle_scheduler_create(output);
	}

	if (wlr_output_is_headless(output)) {
		return wlr_headless_idle_scheduler_create(output);
	}

	return wlr_present_idle_scheduler_create(output);
}
