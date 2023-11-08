#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_timing.h>
#include <wlr/util/log.h>
#include "commit-timing-v1-protocol.h"
#include "util/time.h"

#define TIMING_MANAGER_VERSION 1

static int handle_commit_timer(void *data) {
	struct timer_commit *commit = data;
	wlr_surface_unlock_cached(commit->timer->surface, commit->pending_seq);

	wl_list_remove(&commit->link);
	free(commit);
	return 0;
}

static uint64_t timer_get_target_present_nsec(const struct wlr_timer *timer,
		uint64_t timestamp_nsec) {
	uint64_t refresh_nsec = timer->primary_output->refresh;
	uint64_t cycle_phase_nsec = timer->primary_output->last_present_nsec % refresh_nsec;

	uint64_t round_to_nearest_refresh_nsec = timestamp_nsec;
	round_to_nearest_refresh_nsec -= cycle_phase_nsec;
	round_to_nearest_refresh_nsec += refresh_nsec/2;
	round_to_nearest_refresh_nsec -= (round_to_nearest_refresh_nsec % refresh_nsec);
	round_to_nearest_refresh_nsec += cycle_phase_nsec;

	return round_to_nearest_refresh_nsec;
}

static bool is_valid_timestamp(const struct wlr_timer *timer, uint64_t time_nsec) {
	uint64_t target_present_nsec = timer_get_target_present_nsec(timer, time_nsec);
	struct timespec now;
	uint64_t now_nsec;

	clock_gettime(CLOCK_MONOTONIC, &now);
	now_nsec = timespec_to_nsec(&now);
	if (target_present_nsec <= now_nsec)
		return false;

	struct timer_commit *commit;
	wl_list_for_each(commit, &timer->commits, link) {
		if (commit->timestamp_nsec == time_nsec)
			return false;
	}

	return true;
}

static void timer_output_handle_output_present(struct wl_listener *listener, void *data) {
	struct timer_output *timer_output =
		wl_container_of(listener, timer_output, output_present);
	struct wlr_output_event_present *event = data;

	if (event->presented) {
		timer_output->last_present_nsec = timespec_to_nsec(event->when);
		timer_output->refresh = event->refresh;
	}
}

static void timer_handle_surface_output_enter(struct wl_listener *listener, void *data) {
	struct wlr_timer *timer =
		wl_container_of(listener, timer, surface_output_enter);
	struct wlr_output *output = data;

	struct timer_output *timer_output, *tmp, *last_output = NULL;
	wl_list_for_each_reverse_safe(timer_output, tmp, &timer->outputs, link) {
		/* we got a 'leave' event: remove the output from list */
		if (timer_output->output == output) {
			if (timer->primary_output == timer_output) {
				timer->primary_output = last_output;
				wl_list_remove(&timer_output->output_present.link);
			}
			wl_list_remove(&timer_output->link);
			free(timer_output);
			return;
		}
		last_output = timer_output;
	}

	/* we got an 'enter' event */
	timer_output = calloc(1, sizeof(*timer_output));
	timer_output->output = output;
	timer_output->output_present.notify = timer_output_handle_output_present;
	wl_signal_add(&timer_output->output->events.present, &timer_output->output_present);
	wl_list_insert(&timer->outputs, &timer_output->link);

	if (!timer->primary_output) {
		timer->primary_output = timer_output;
	}
}

static void timer_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_timer *timer =
		wl_container_of(listener, timer, surface_client_commit);

	if (!timer->primary_output || !timer->curr_timestamp_nsec) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint64_t target_present_nsec =
		timer_get_target_present_nsec(timer, timer->curr_timestamp_nsec);
	uint64_t now_msec = timespec_to_msec(&now);

	/* How much time we have left until the latch point for the targeted presentation. */
	int64_t delay_target_latch_msec = target_present_nsec; // time of target presentation
	delay_target_latch_msec -= timer->primary_output->refresh; // time of previous presentation
	delay_target_latch_msec += timer->manager->latch_delay_nsec; // delay until latch
	delay_target_latch_msec -= 1000000; // give a 1msec slop
	delay_target_latch_msec /= 1000000; // to msec
	delay_target_latch_msec -= now_msec; // delay time
	/* If we are too close to the target latch time, don't bother and
	 * just commit. */
	if (delay_target_latch_msec < 1) {
		timer->curr_timestamp_nsec = 0;
		return;
	}

	struct timer_commit *commit = calloc(1, sizeof(*commit));
	if (!commit) {
		return;
	}
	commit->timer = timer;
	commit->stage = timer->curr_stage;
	commit->timestamp_nsec = timer->curr_timestamp_nsec;
	timer->curr_timestamp_nsec = 0;
	commit->commit_unlock_timer =
		wl_event_loop_add_timer(wl_display_get_event_loop(timer->wl_display), handle_commit_timer,
			commit);
	if (!commit->commit_unlock_timer) {
		free(commit);
		return;
	}
	wl_list_insert(&timer->commits, &commit->link);

	/* lock the current commit */
	commit->pending_seq = wlr_surface_lock_pending(timer->surface);

	/* unlock commit just before the latch point */
	wl_event_source_timer_update(commit->commit_unlock_timer, delay_target_latch_msec);
}

static const struct wp_commit_timer_v1_interface timer_impl;

static struct wlr_timer *wlr_timer_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timer_v1_interface,
		&timer_impl));
	return wl_resource_get_user_data(resource);
}

static void timer_handle_set_timestamp(struct wl_client *client,
		struct wl_resource *resource, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
		uint32_t tv_nsec, uint32_t stage, uint32_t rounding_mode) {
	struct wlr_timer *timer = wlr_timer_from_resource(resource);

	if (!timer->primary_output) {
		return;
	}

	if (timer->curr_timestamp_nsec) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_ALREADY_HAS_TIMESTAMP,
			"surface already has a timestamp");
		return;
	}

	uint64_t timestamp_nsec = timespec_to_nsec(&(struct timespec)
								{ .tv_sec = (uint64_t)tv_sec_hi<<32 |
								tv_sec_lo, .tv_nsec = tv_nsec });
	if (!is_valid_timestamp(timer, timestamp_nsec)) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
			"client provided an invalid timestamp");
		return;
	}
	timer->curr_timestamp_nsec = timestamp_nsec;
	timer->curr_stage = stage;
	timer->curr_rounding_mode = rounding_mode;
}

static void timer_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_commit_timer_v1_interface timer_impl = {
	.destroy = timer_handle_destroy,
	.set_timestamp = timer_handle_set_timestamp
};

static void timer_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_timer *timer = wlr_timer_from_resource(resource);

	wl_list_remove(&timer->surface_client_commit.link);
	struct timer_output *timer_output, *tmp_to;
	wl_list_for_each_safe(timer_output, tmp_to, &timer->outputs, link) {
		wl_list_remove(&timer_output->output_present.link);
		free(timer_output);
	}
	struct timer_commit *timer_commit, *tmp_co;
	wl_list_for_each_safe(timer_commit, tmp_co, &timer->commits, link) {
		free(timer_commit);
	}

	free(timer);
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl;

static struct wlr_timing_client *timing_client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timing_manager_v1_interface,
		&timing_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void timing_manager_get_timer(struct wl_client *wl_client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_timing_client *client = timing_client_from_resource(resource);

	struct wlr_timer *timer = calloc(1, sizeof(struct wlr_timer));
	if (!timer) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	timer->wl_display = wl_client_get_display(wl_client);
	timer->manager = client->timing_manager;
	wl_list_init(&timer->commits);
	wl_list_init(&timer->outputs);

	timer->resource = wl_resource_create(wl_client, &wp_commit_timer_v1_interface,
		wl_resource_get_version(resource), id);
	if (timer->resource == NULL) {
		free(timer);
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(timer->resource, &timer_impl,
		timer, timer_handle_resource_destroy);

	client->timer = timer;

	/* We need to keep track of the outputs' timings and when a surface enters/leaves
	 * them. */
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		struct timer_output *timer_output = calloc(1, sizeof(*timer_output));
		if (!timer->primary_output) {
			timer->primary_output = timer_output;
		}
		timer_output->output = surface_output->output;
		timer_output->output_present.notify = timer_output_handle_output_present;
		wl_signal_add(&surface_output->output->events.present, &timer_output->output_present);
	}
	timer->surface_output_enter.notify = timer_handle_surface_output_enter;
	wl_signal_add(&surface->events.surface_output_enter, &timer->surface_output_enter);

	timer->surface = surface;
	timer->surface_client_commit.notify = timer_handle_client_commit;
	wl_signal_add(&timer->surface->events.client_commit, &timer->surface_client_commit);

	wlr_log(WLR_DEBUG, "New wlr_timer %p (res %p)", timer, timer->resource);
}

static void timing_manager_destroy(struct wl_client *wl_client,
		struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_commit_timing_manager_v1_interface timing_manager_impl = {
	.get_timer = timing_manager_get_timer,
	.destroy = timing_manager_destroy,
};

static void timing_client_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_timing_client *client = timing_client_from_resource(resource);
	wl_list_remove(&client->link);
	free(client);
}

static void timing_manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_timing_manager *timing_manager = data;

	struct wlr_timing_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	client->resource =
		wl_resource_create(wl_client, &wp_commit_timing_manager_v1_interface, version, id);
	if (client->resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}
	client->timing_manager = timing_manager;

	wl_resource_set_implementation(client->resource, &timing_manager_impl,
		client, timing_client_handle_resource_destroy);

	wl_list_insert(&timing_manager->clients, &client->link);
}

static void timing_manager_handle_display_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_timing_manager *timing_manager =
		wl_container_of(listener, timing_manager, display_destroy);
	wl_signal_emit_mutable(&timing_manager->events.destroy, NULL);
	wl_list_remove(&timing_manager->display_destroy.link);
	wl_global_destroy(timing_manager->global);
	free(timing_manager);
}

void wlr_timing_manager_set_latch_delay(struct wlr_timing_manager *timing_manager,
		uint64_t latch_delay_nsec) {
	timing_manager->latch_delay_nsec = latch_delay_nsec;
}

struct wlr_timing_manager *wlr_timing_manager_create(struct wl_display *display,
		uint32_t version) {
	assert(version <= TIMING_MANAGER_VERSION);

	struct wlr_timing_manager *timing_manager = calloc(1, sizeof(*timing_manager));
	if (!timing_manager) {
		return NULL;
	}

	timing_manager->global = wl_global_create(display, &wp_commit_timing_manager_v1_interface,
		version, timing_manager, timing_manager_bind);
	if (!timing_manager->global) {
		free(timing_manager);
		return NULL;
	}

	wl_list_init(&timing_manager->clients);
	wl_signal_init(&timing_manager->events.destroy);

	timing_manager->display_destroy.notify = timing_manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &timing_manager->display_destroy);

	return timing_manager;
}
