#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_frame_scheduler.h>
#include <wlr/types/wlr_frame_scheduler.h>
#include <wlr/types/wlr_output.h>

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

struct wlr_frame_scheduler *wlr_frame_scheduler_autocreate(struct wlr_output *output) {
	// TODO: check for presentation-time in wayland backend and work with frame cbs if absent
	return wlr_present_idle_scheduler_create(output);
}
