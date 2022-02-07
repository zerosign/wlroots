/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_CONFIGURABLE_H
#define WLR_TYPES_CONFIGURABLE_H

#include <wayland-server-core.h>
#include <wlr/util/addon.h>

struct wlr_configure {
	uint32_t serial;
	struct wlr_addon_set addons;
	struct wl_list link; // wlr_configurable.configures
};

struct wlr_configurable;

struct wlr_configurable_interface {
	void (*configure)(struct wlr_configurable *configurable,
		struct wlr_configure *configure);
	void (*ack_configure)(struct wlr_configurable *configurable,
		struct wlr_configure *configure);
};

struct wlr_configurable {
	const struct wlr_configurable_interface *impl;
	struct wl_resource *resource;
	int invalid_serial_error;

	struct wl_event_source *event_idle;
	uint32_t next_serial;

	struct wl_list configures; // wlr_configure.link
};

uint32_t wlr_configurable_schedule_configure(
	struct wlr_configurable *configurable);

void wlr_configurable_ack_configure(
	struct wlr_configurable *configurable, uint32_t serial);

void wlr_configurable_init(struct wlr_configurable *configurable,
	const struct wlr_configurable_interface *impl,
	struct wl_resource *resource, int invalid_serial_error);

void wlr_configurable_finish(struct wlr_configurable *configurable);

#endif
