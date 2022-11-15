#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include "fractional-scale-v1-protocol.h"

#define FRACTIONAL_SCALE_V1_VERSION 1

struct wlr_fractional_scale_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;
};

static void resource_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl;

static const char fractional_scale_v1_addon_owner = 0;

static struct wlr_fractional_scale_v1 *fractional_scale_v1_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_fractional_scale_v1_interface, &fractional_scale_impl));
	return wl_resource_get_user_data(resource);
}

static void fractional_scale_v1_addon_destroy(struct wlr_addon *addon) {
	struct wlr_fractional_scale_v1 *scale =
		wl_container_of(addon, scale, addon);
	wl_resource_destroy(scale->resource);
}

static const struct wlr_addon_interface fractional_scale_v1_addon_impl = {
	.name = "wlr_fractional_scale_v1",
	.destroy = fractional_scale_v1_addon_destroy,
};

static void fractional_scale_handle_set_scale_factor(struct wl_client *client,
		struct wl_resource *resource, uint32_t scale_8_24) {
	struct wlr_fractional_scale_v1 *scale =
		fractional_scale_v1_from_resource(resource);
	if (scale == NULL) {
		return;
	}
	if (scale_8_24 == 0) {
		wl_resource_post_error(resource,
			WP_FRACTIONAL_SCALE_V1_ERROR_INVALID_SCALE,
			"scale value is not valid");
		return;
	}
	scale->surface->client_scale_factor = (double)scale_8_24 / (1 << 24);
}

static const struct wp_fractional_scale_v1_interface fractional_scale_impl = {
	.destroy = resource_handle_destroy,
	.set_scale_factor = fractional_scale_handle_set_scale_factor,
};

static void fractional_scale_resource_destroy(struct wl_resource *resource) {
	struct wlr_fractional_scale_v1 *scale =
		fractional_scale_v1_from_resource(resource);
	if (scale == NULL) {
		return;
	}
	scale->surface->client_scale_factor = 1.0;
	scale->surface->server_scale_factor = 1.0;
	wlr_addon_finish(&scale->addon);
	free(scale);
}

static void fractional_scale_manager_v1_handle_get_fractional_scale(
		struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct wlr_fractional_scale_v1 *scale = calloc(1, sizeof(*scale));
	if (scale == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	scale->resource = wl_resource_create(client, &wp_fractional_scale_v1_interface,
		wl_resource_get_version(resource), id);
	if (scale->resource == NULL) {
		free(scale);
		wl_client_post_no_memory(client);
		return;
	}

	struct wlr_surface *surface =
		wlr_surface_from_resource(surface_resource);
	scale->surface = surface;
	wlr_addon_init(&scale->addon, &surface->addons,
		&fractional_scale_v1_addon_owner,
		&fractional_scale_v1_addon_impl);

	wl_resource_set_implementation(scale->resource,
		&fractional_scale_impl, scale, fractional_scale_resource_destroy);
}

static const struct wp_fractional_scale_manager_v1_interface fractional_scale_manager_v1_impl = {
	.destroy = resource_handle_destroy,
	.get_fractional_scale = fractional_scale_manager_v1_handle_get_fractional_scale,
};

static void global_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_fractional_scale_manager_v1 *global = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_fractional_scale_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource,
		&fractional_scale_manager_v1_impl, global, NULL);
}

static void fractional_scale_manager_v1_handle_display_destroy(
		struct wl_listener *listener, void *data) {
	struct wlr_fractional_scale_manager_v1 *global =
		wl_container_of(listener, global, display_destroy);
	wl_signal_emit_mutable(&global->events.destroy, NULL);
	wl_list_remove(&global->display_destroy.link);
	wl_global_destroy(global->global);
	free(global);
}

struct wlr_fractional_scale_manager_v1 *wlr_fractional_scale_manager_v1_create(
		struct wl_display *display) {
	struct wlr_fractional_scale_manager_v1 *global = calloc(1, sizeof(*global));
	if (global == NULL) {
		return NULL;
	}

	global->global = wl_global_create(display, &wp_fractional_scale_manager_v1_interface,
		FRACTIONAL_SCALE_V1_VERSION, global, global_bind);
	if (global->global == NULL) {
		free(global);
		return NULL;
	}

	wl_signal_init(&global->events.destroy);

	global->display_destroy.notify = fractional_scale_manager_v1_handle_display_destroy;
	wl_display_add_destroy_listener(display, &global->display_destroy);

	return global;
}

void wlr_fractional_scale_v1_send_scale_factor(
		struct wlr_surface *surface, double factor) {
	assert(factor > 0.0);
	struct wlr_addon *addon = wlr_addon_find(&surface->addons,
		&fractional_scale_v1_addon_owner,
		&fractional_scale_v1_addon_impl);
	if (addon != NULL) {
		struct wlr_fractional_scale_v1 *scale =
			wl_container_of(addon, scale, addon);
		wp_fractional_scale_v1_send_scale_factor(scale->resource,
			(uint32_t)(factor * (1 << 24)));
		surface->server_scale_factor = factor;
	}
}
