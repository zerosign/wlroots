#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "types/wlr_data_device.h"

static void drag_handle_seat_client_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_drag *drag =
		wl_container_of(listener, drag, seat_client_destroy);

	drag->focus_client = NULL;
	wl_list_remove(&drag->seat_client_destroy.link);
}

static void drag_icon_destroy(struct wlr_drag_icon *icon) {
	icon->drag->icon = NULL;
	wl_list_remove(&icon->surface_destroy.link);
	wl_signal_emit_mutable(&icon->events.destroy, icon);
	free(icon);
}

static void drag_destroy(struct wlr_drag *drag) {
	if (drag->started) {
		wlr_seat_drag_clear_focus(drag->seat);

		assert(drag->seat->drag == drag);
		drag->seat->drag = NULL;
	}

	// We wait until after clearing the drag focus to ensure that
	// wl_data_device.leave is sent before emitting the signal.
	wl_signal_emit_mutable(&drag->events.destroy, drag);

	if (drag->source) {
		wl_list_remove(&drag->source_destroy.link);
	}

	if (drag->icon != NULL) {
		drag_icon_destroy(drag->icon);
	}
	free(drag);
}

static void drag_handle_icon_destroy(struct wl_listener *listener, void *data) {
	struct wlr_drag *drag = wl_container_of(listener, drag, icon_destroy);
	drag->icon = NULL;
}

static void drag_handle_drag_source_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_drag *drag = wl_container_of(listener, drag, source_destroy);
	drag_destroy(drag);
}

static void drag_icon_surface_role_commit(struct wlr_surface *surface) {
	assert(surface->role == &drag_icon_surface_role);

	pixman_region32_clear(&surface->input_region);
	if (wlr_surface_has_buffer(surface)) {
		wlr_surface_map(surface);
	}
}

const struct wlr_surface_role drag_icon_surface_role = {
	.name = "wl_data_device-icon",
	.no_object = true,
	.commit = drag_icon_surface_role_commit,
};

static void drag_icon_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_drag_icon *icon =
		wl_container_of(listener, icon, surface_destroy);
	drag_icon_destroy(icon);
}

static struct wlr_drag_icon *drag_icon_create(struct wlr_drag *drag,
		struct wlr_surface *surface) {
	struct wlr_drag_icon *icon = calloc(1, sizeof(struct wlr_drag_icon));
	if (!icon) {
		return NULL;
	}

	icon->drag = drag;
	icon->surface = surface;

	wl_signal_init(&icon->events.destroy);

	icon->surface_destroy.notify = drag_icon_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &icon->surface_destroy);

	drag_icon_surface_role_commit(surface);

	return icon;
}

struct wlr_drag *wlr_drag_create(struct wlr_seat_client *seat_client,
		struct wlr_data_source *source, struct wlr_surface *icon_surface) {
	struct wlr_drag *drag = calloc(1, sizeof(struct wlr_drag));
	if (drag == NULL) {
		return NULL;
	}

	wl_signal_init(&drag->events.focus);
	wl_signal_init(&drag->events.motion);
	wl_signal_init(&drag->events.drop);
	wl_signal_init(&drag->events.destroy);

	drag->seat = seat_client->seat;
	drag->seat_client = seat_client;

	if (icon_surface) {
		struct wlr_drag_icon *icon = drag_icon_create(drag, icon_surface);
		if (icon == NULL) {
			free(drag);
			return NULL;
		}

		drag->icon = icon;

		drag->icon_destroy.notify = drag_handle_icon_destroy;
		wl_signal_add(&icon->events.destroy, &drag->icon_destroy);
	}

	drag->source = source;
	if (source != NULL) {
		drag->source_destroy.notify = drag_handle_drag_source_destroy;
		wl_signal_add(&source->events.destroy, &drag->source_destroy);
	}

	return drag;
}

void wlr_seat_request_start_drag(struct wlr_seat *seat, struct wlr_drag *drag,
		struct wlr_surface *origin, uint32_t serial) {
	assert(drag->seat == seat);

	if (seat->drag != NULL) {
		wlr_log(WLR_DEBUG, "Rejecting start_drag request, "
			"another drag-and-drop operation is already in progress");
		return;
	}

	struct wlr_seat_request_start_drag_event event = {
		.drag = drag,
		.origin = origin,
		.serial = serial,
	};
	wl_signal_emit_mutable(&seat->events.request_start_drag, &event);
}

static void seat_handle_drag_source_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_seat *seat =
		wl_container_of(listener, seat, drag_source_destroy);
	wl_list_remove(&seat->drag_source_destroy.link);
	seat->drag_source = NULL;
}

void wlr_seat_start_drag(struct wlr_seat *seat, struct wlr_drag *drag,
		uint32_t serial) {
	assert(drag->seat == seat);
	assert(!drag->started);
	drag->started = true;

	seat->drag = drag;
	seat->drag_serial = serial;

	// We need to destroy the previous source, because listeners only expect one
	// active drag source at a time.
	wlr_data_source_destroy(seat->drag_source);
	seat->drag_source = drag->source;
	if (drag->source != NULL) {
		seat->drag_source_destroy.notify = seat_handle_drag_source_destroy;
		wl_signal_add(&drag->source->events.destroy, &seat->drag_source_destroy);
	}

	wl_signal_emit_mutable(&seat->events.start_drag, drag);
}

void wlr_seat_drag_enter(struct wlr_seat *seat, struct wlr_surface *surface,
		double sx, double sy) {
	struct wlr_drag *drag = seat->drag;
	assert(drag != NULL);

	if (drag->focus == surface) {
		return;
	}

	if (drag->focus_client) {
		wl_list_remove(&drag->seat_client_destroy.link);

		// If we're switching focus to another client, we want to destroy all
		// offers without destroying the source. If the drag operation ends, we
		// want to keep the offer around for the data transfer.
		struct wlr_data_offer *offer, *tmp;
		wl_list_for_each_safe(offer, tmp,
				&drag->focus_client->seat->drag_offers, link) {
			struct wl_client *client = wl_resource_get_client(offer->resource);
			if (!drag->dropped && offer->source == drag->source &&
					client == drag->focus_client->client) {
				offer->source = NULL;
				data_offer_destroy(offer);
			}
		}

		struct wl_resource *resource;
		wl_resource_for_each(resource, &drag->focus_client->data_devices) {
			wl_data_device_send_leave(resource);
		}

		drag->focus_client = NULL;
		drag->focus = NULL;
	}

	if (!surface) {
		goto out;
	}

	if (!drag->source && drag->seat_client &&
			wl_resource_get_client(surface->resource) !=
			drag->seat_client->client) {
		goto out;
	}

	struct wlr_seat_client *focus_client = wlr_seat_client_for_wl_client(
		drag->seat, wl_resource_get_client(surface->resource));
	if (!focus_client) {
		goto out;
	}

	if (drag->source != NULL) {
		drag->source->accepted = false;

		uint32_t serial =
			wl_display_next_serial(drag->seat->display);

		struct wl_resource *device_resource;
		wl_resource_for_each(device_resource, &focus_client->data_devices) {
			struct wlr_data_offer *offer = data_offer_create(device_resource,
				drag->source, WLR_DATA_OFFER_DRAG);
			if (offer == NULL) {
				wl_resource_post_no_memory(device_resource);
				return;
			}

			data_offer_update_action(offer);

			if (wl_resource_get_version(offer->resource) >=
					WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
				wl_data_offer_send_source_actions(offer->resource,
					drag->source->actions);
			}

			wl_data_device_send_enter(device_resource, serial,
				surface->resource,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy),
				offer->resource);
		}
	}

	drag->focus = surface;
	drag->focus_client = focus_client;
	drag->seat_client_destroy.notify = drag_handle_seat_client_destroy;
	wl_signal_add(&focus_client->events.destroy, &drag->seat_client_destroy);

out:
	wl_signal_emit_mutable(&drag->events.focus, drag);
}

void wlr_seat_drag_clear_focus(struct wlr_seat *seat) {
	wlr_seat_drag_enter(seat, NULL, 0, 0);
}

void wlr_seat_drag_send_motion(struct wlr_seat *seat, uint32_t time_msec,
		double sx, double sy) {
	struct wlr_drag *drag = seat->drag;
	assert(drag != NULL);

	if (drag->focus != NULL && drag->focus_client != NULL) {
		struct wl_resource *resource;
		wl_resource_for_each(resource, &drag->focus_client->data_devices) {
			wl_data_device_send_motion(resource, time_msec,
				wl_fixed_from_double(sx), wl_fixed_from_double(sy));
		}

		struct wlr_drag_motion_event event = {
			.drag = drag,
			.time = time_msec,
			.sx = sx,
			.sy = sy,
		};
		wl_signal_emit_mutable(&drag->events.motion, &event);
	}
}

void wlr_seat_drag_drop_and_destroy(struct wlr_seat *seat, uint32_t time_msec) {
	struct wlr_drag *drag = seat->drag;
	assert(drag != NULL);

	if (drag->source != NULL) {
		if (drag->focus_client != NULL &&
				drag->source->current_dnd_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
				drag->source->accepted) {
			drag->dropped = true;

			struct wl_resource *resource;
			wl_resource_for_each(resource, &drag->focus_client->data_devices) {
				wl_data_device_send_drop(resource);
			}
			wlr_data_source_dnd_drop(drag->source);

			struct wlr_drag_drop_event event = {
				.drag = drag,
				.time = time_msec,
			};
			wl_signal_emit_mutable(&drag->events.drop, &event);
		} else if (drag->source->impl->dnd_finish != NULL) {
			// This will call drag_destroy()
			wlr_data_source_destroy(drag->source);
			return;
		}
	}

	drag_destroy(drag);
}

void wlr_seat_drag_destroy(struct wlr_seat *seat) {
	struct wlr_drag *drag = seat->drag;
	assert(drag != NULL);

	drag_destroy(drag);
}
