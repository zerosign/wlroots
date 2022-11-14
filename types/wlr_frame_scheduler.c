#define _POSIX_C_SOURCE 199309L
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_frame_scheduler.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "util/time.h"

static int handle_timer(void *data) {
	struct wlr_frame_scheduler *scheduler = data;

	if (clock_gettime(CLOCK_MONOTONIC, &scheduler->queued.frame_emitted) != 0) {
		wlr_log_errno(WLR_ERROR, "clock_gettime() failed");
		return 0;
	}

	wl_signal_emit_mutable(&scheduler->events.frame, NULL);

	return 0;
}

static void schedule_frame(struct wlr_frame_scheduler *scheduler, int64_t delay_ns) {
	if (delay_ns <= 0) {
		handle_timer(scheduler);
		return;
	}

	int delay_ms = delay_ns / 1000000;
	wl_event_source_timer_update(scheduler->timer, delay_ms);
}

static void handle_output_present(struct wl_listener *listener, void *data) {
	struct wlr_frame_scheduler *scheduler =
		wl_container_of(listener, scheduler, output_present);
	const struct wlr_output_event_present *event = data;

	if (scheduler->queued.render_complete == NULL ||
			event->commit_seq != scheduler->queued.commit_seq) {
		goto out;
	}

	struct timespec render_complete;
	if (!wlr_render_timestamp_get_time(scheduler->queued.render_complete,
			&render_complete)) {
		goto out;
	}

	struct timespec cpu_duration;
	timespec_sub(&cpu_duration, &scheduler->queued.frame_submitted,
		&scheduler->queued.frame_emitted);
	int64_t cpu_duration_ns = timespec_to_nsec(&cpu_duration);
	if (cpu_duration_ns <= 0) {
		wlr_log(WLR_ERROR, "CPU duration is negative (%f ms)",
			(float)cpu_duration_ns / 1000 / 1000);
		goto out;
	}

	struct timespec gpu_duration;
	timespec_sub(&gpu_duration, &render_complete,
		&scheduler->queued.render_submitted);
	int64_t gpu_duration_ns = timespec_to_nsec(&gpu_duration);
	if (gpu_duration_ns <= 0) {
		wlr_log(WLR_ERROR, "GPU duration is negative (%f ms)",
			(float)gpu_duration_ns / 1000 / 1000);
		goto out;
	}

	wlr_log(WLR_INFO, "Render duration: CPU %f ns, GPU %f ms",
		(float)cpu_duration_ns / 1000000,
		(float)gpu_duration_ns / 1000000);

	scheduler->histogram[scheduler->histogram_cur] = (struct wlr_frame_scheduler_bucket){
		.cpu_duration_ns = cpu_duration_ns,
		.gpu_duration_ns = gpu_duration_ns,
	};
	scheduler->histogram_cur++;
	scheduler->histogram_cur %= WLR_FRAME_SCHEDULER_HISTOGRAM_LEN;

out:
	wlr_render_timestamp_destroy(scheduler->queued.render_complete);
	memset(&scheduler->queued, 0, sizeof(scheduler->queued));

	if (event->when == NULL || event->refresh <= 0) {
		schedule_frame(scheduler, 2 * 1000 * 1000);
		return;
	}

	size_t n_samples = 0;
	int64_t max_cpu_duration_ns = 0, max_gpu_duration_ns = 0;
	for (size_t i = 0; i < WLR_FRAME_SCHEDULER_HISTOGRAM_LEN; i++) {
		const struct wlr_frame_scheduler_bucket bucket = scheduler->histogram[i];
		if (bucket.cpu_duration_ns <= 0 || bucket.gpu_duration_ns <= 0) {
			continue;
		}
		n_samples++;
		if (bucket.cpu_duration_ns > max_cpu_duration_ns) {
			max_cpu_duration_ns = bucket.cpu_duration_ns;
		}
		if (bucket.gpu_duration_ns > max_gpu_duration_ns) {
			max_gpu_duration_ns = bucket.gpu_duration_ns;
		}
	}
	if (n_samples < WLR_FRAME_SCHEDULER_HISTOGRAM_LEN) {
		schedule_frame(scheduler, 2 * 1000 * 1000);
		return;
	}

	int64_t max_duration_ns = max_cpu_duration_ns + max_gpu_duration_ns + 2000000;
	struct timespec max_duration;
	timespec_from_nsec(&max_duration, max_duration_ns);

	if (max_duration_ns > event->refresh) {
		wlr_log(WLR_ERROR, "Max render duration (%f ms) exceeds "
			"refresh period (%f ms)", (float)max_duration_ns / 1000 / 1000,
			(float)event->refresh / 1000 / 1000);
		schedule_frame(scheduler, 2 * 1000 * 1000);
		return;
	}

	wlr_log(WLR_INFO, "Max duration: %f ms", (float)max_duration_ns / 1000000);

	struct timespec predicted_refresh_duration;
	timespec_from_nsec(&predicted_refresh_duration, event->refresh);

	struct timespec predicted_next_refresh;
	timespec_add(&predicted_next_refresh, event->when, &predicted_refresh_duration);

	struct timespec next_frame;
	timespec_sub(&next_frame, &predicted_next_refresh, &max_duration);

	struct timespec now;
	if (clock_gettime(scheduler->presentation_clock, &now) != 0) {
		wlr_log_errno(WLR_ERROR, "clock_gettime() failed");
		schedule_frame(scheduler, 2 * 1000 * 1000);
		return;
	}

	struct timespec delay;
	timespec_sub(&delay, &next_frame, &now);
	int64_t delay_ns = timespec_to_nsec(&delay);
	schedule_frame(scheduler, delay_ns);
}

bool wlr_frame_scheduler_init(struct wlr_frame_scheduler *scheduler,
		struct wlr_output *output) {
	memset(scheduler, 0, sizeof(*scheduler));

	scheduler->output = output;
	scheduler->presentation_clock = wlr_backend_get_presentation_clock(output->backend);

	struct wl_event_loop *loop = wl_display_get_event_loop(output->display);
	scheduler->timer = wl_event_loop_add_timer(loop, handle_timer, scheduler);
	if (!scheduler->timer) {
		wlr_log(WLR_ERROR, "wl_event_loop_add_timer() failed");
		return false;
	}

	wl_signal_init(&scheduler->events.frame);

	scheduler->output_present.notify = handle_output_present;
	wl_signal_add(&output->events.present, &scheduler->output_present);

	schedule_frame(scheduler, 2 * 1000 * 1000);

	return true;
}

void wlr_frame_scheduler_finish(struct wlr_frame_scheduler *scheduler) {
	wl_list_remove(&scheduler->output_present.link);
	wlr_render_timestamp_destroy(scheduler->queued.render_complete);
	wl_event_source_remove(scheduler->timer);
}

void wlr_frame_scheduler_mark_render_submitted(
		struct wlr_frame_scheduler *scheduler, struct wlr_renderer *renderer) {
	wlr_render_timestamp_destroy(scheduler->queued.render_complete);
	scheduler->queued.render_complete = NULL;

	if (clock_gettime(CLOCK_MONOTONIC, &scheduler->queued.frame_submitted) != 0) {
		wlr_log_errno(WLR_ERROR, "clock_gettime() failed");
		return;
	}

	if (!wlr_renderer_get_time(renderer, &scheduler->queued.render_submitted)) {
		return;
	}

	scheduler->queued.render_complete = wlr_renderer_create_timestamp(renderer);
	scheduler->queued.commit_seq = scheduler->output->commit_seq;
}
