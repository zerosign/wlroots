/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_ACTION_BINDER_V1_H
#define WLR_TYPES_WLR_ACTION_BINDER_V1_H

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "ext-action-binder-v1-protocol.h"

struct wlr_action_binder_v1 {
	struct wl_global *global;
	struct wl_list states; // wlr_action_binder_v1_state.link
	struct wl_listener display_destroy;

	struct {
		struct wl_signal bind;
		struct wl_signal destroy;
	} events;

	void *data;
};

struct wlr_action_binder_v1_state {
	struct wl_list binds; // wlr_action_binding_v1.link
	struct wl_list bind_queue; // wlr_action_binding_v1.link
	struct wlr_action_binder_v1 *binder;
	struct wl_resource *resource;

	struct wl_list link;
};

struct wlr_action_binding_v1 {
	struct wl_resource *resource;
	struct wlr_action_binder_v1_state *state;

	char *namespace, *name;

	char *description; // may be NULL when the client doesn't set a description
	char *trigger_kind, *trigger; // may be NULL when the client doesn't set a trigger hint
	char *app_id; // may be NULL when the client doesn't set an app_id
	struct wlr_seat *seat; // may be NULL when the client doesn't set a seat
	struct wl_listener seat_destroy;

	struct {
		struct wl_signal destroy;
	} events;

	bool bound;
	struct wl_list link;
};

struct wlr_action_binder_v1 *wlr_action_binder_v1_create(struct wl_display *display);
void wlr_action_binding_v1_bind(struct wlr_action_binding_v1 *bind, const char *trigger);
void wlr_action_binding_v1_reject(struct wlr_action_binding_v1 *bind);
void wlr_action_binding_v1_trigger(struct wlr_action_binding_v1 *binding, uint32_t trigger_type, uint32_t time_msec);
void wlr_action_binding_v1_trigger_now(struct wlr_action_binding_v1 *binding, uint32_t trigger_type);

#endif
