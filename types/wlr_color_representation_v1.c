#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>

#define COLOR_REPRESENTATION_VERSION 1

struct wlr_color_representation_manager_v1 {
	struct wl_global *global;

	struct wl_listener display_destroy;
};

struct wlr_color_representation_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;

	struct wlr_color_representation_v1_state current, pending;

	struct wl_listener surface_commit;
};

static void destroy_resource(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_color_representation_v1_interface color_repr_impl;
static const struct wp_color_representation_manager_v1_interface manager_impl;

/**
 * Get a color representation from a resource.
 *
 * Returns NULL if the object is inert.
 */
static struct wlr_color_representation_v1 *color_repr_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_color_representation_v1_interface,
		&color_repr_impl));
	return wl_resource_get_user_data(resource);
}

static struct wlr_color_representation_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_color_representation_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void color_repr_handle_set_range(struct wl_client *client,
		struct wl_resource *resource, uint32_t range) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		return;
	}

	switch (range) {
	case WP_COLOR_REPRESENTATION_V1_RANGE_ITU_FULL:
	case WP_COLOR_REPRESENTATION_V1_RANGE_ITU_NARROW:
		break;
	default:
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_V1_ERROR_INVALID_RANGE,
			"Invalid range");
		return;
	}

	color_repr->pending.range = range;
}

static void color_repr_handle_set_coefficients(struct wl_client *client,
		struct wl_resource *resource, uint32_t coefficients) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		return;
	}

	switch (coefficients) {
	case WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_IDENTITY:
	case WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT709:
	case WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT601:
	case WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_BT2020_NONCONST:
		break;
	default:
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_V1_ERROR_INVALID_COEFFICIENTS,
			"Invalid coefficients");
		return;
	}

	color_repr->pending.coefficients = coefficients;
}

static bool check_chroma_location(uint32_t loc) {
	switch (loc) {
	case WP_COLOR_REPRESENTATION_V1_CHROMA_LOCATION_COSITED_EVEN:
	case WP_COLOR_REPRESENTATION_V1_CHROMA_LOCATION_MIDPOINT:
		return true;
	default:
		return false;
	}
}

static void color_repr_handle_set_chroma_location(struct wl_client *client,
		struct wl_resource *resource, uint32_t x_chroma_offset,
		uint32_t y_chroma_offset) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	if (color_repr == NULL) {
		return;
	}

	if (!check_chroma_location(x_chroma_offset)) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_V1_ERROR_INVALID_CHROMA_LOCATION,
			"Invalid X chroma offset");
		return;
	}
	if (!check_chroma_location(y_chroma_offset)) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_V1_ERROR_INVALID_CHROMA_LOCATION,
			"Invalid Y chroma offset");
		return;
	}

	color_repr->pending.x_chroma_offset = x_chroma_offset;
	color_repr->pending.y_chroma_offset = y_chroma_offset;
}

static const struct wp_color_representation_v1_interface color_repr_impl = {
	.destroy = destroy_resource,
	.set_range = color_repr_handle_set_range,
	.set_coefficients = color_repr_handle_set_coefficients,
	.set_chroma_location = color_repr_handle_set_chroma_location,
};

static void color_repr_destroy(struct wlr_color_representation_v1 *color_repr) {
	wl_resource_set_user_data(color_repr->resource, NULL); // make inert
	wl_list_remove(&color_repr->surface_commit.link);
	wlr_addon_finish(&color_repr->addon);
	free(color_repr);
}

static void color_repr_handle_surface_commit(struct wl_listener *listener,
		void *data) {
	struct wlr_color_representation_v1 *color_repr =
		wl_container_of(listener, color_repr, surface_commit);
	color_repr->current = color_repr->pending;
}

static void color_repr_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_color_representation_v1 *color_repr =
		color_repr_from_resource(resource);
	color_repr_destroy(color_repr);
}

static void color_repr_state_init(struct wlr_color_representation_v1_state *state) {
	state->range = WP_COLOR_REPRESENTATION_V1_RANGE_ITU_FULL;
	state->coefficients = WP_COLOR_REPRESENTATION_V1_COEFFICIENTS_IDENTITY;
	state->x_chroma_offset = WP_COLOR_REPRESENTATION_V1_CHROMA_LOCATION_COSITED_EVEN;
	state->y_chroma_offset = WP_COLOR_REPRESENTATION_V1_CHROMA_LOCATION_MIDPOINT;
}

static void color_repr_addon_destroy(struct wlr_addon *addon) {
	struct wlr_color_representation_v1 *color_repr =
		wl_container_of(addon, color_repr, addon);
	color_repr_destroy(color_repr);
}

static const struct wlr_addon_interface color_repr_addon_impl = {
	.name = "wp_color_representation_v1",
	.destroy = color_repr_addon_destroy,
};

static void manager_handle_create_surface(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_color_representation_manager_v1 *manager =
		manager_from_resource(manager_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	struct wlr_color_representation_v1 *color_repr =
		calloc(1, sizeof(*color_repr));
	if (color_repr == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	color_repr_state_init(&color_repr->current);
	color_repr_state_init(&color_repr->pending);

	uint32_t version = wl_resource_get_version(manager_resource);
	color_repr->resource = wl_resource_create(client,
		&wp_color_representation_v1_interface, version, id);
	if (color_repr->resource == NULL) {
		return;
	}
	wl_resource_set_implementation(color_repr->resource,
		&color_repr_impl, color_repr, color_repr_handle_resource_destroy);

	color_repr->surface = surface;
	wlr_addon_init(&color_repr->addon, &surface->addons, manager,
		&color_repr_addon_impl);

	color_repr->surface_commit.notify = color_repr_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &color_repr->surface_commit);
}

static const struct wp_color_representation_manager_v1_interface manager_impl = {
	.destroy = destroy_resource,
	.create_surface = manager_handle_create_surface,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_color_representation_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_representation_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_color_representation_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_color_representation_manager_v1 *
wlr_color_representation_manager_v1_create(struct wl_display *display) {
	struct wlr_color_representation_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&wp_color_representation_manager_v1_interface,
		COLOR_REPRESENTATION_VERSION, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

bool wlr_color_representation_manager_v1_get_surface_state(
		struct wlr_color_representation_manager_v1 *manager,
		struct wlr_surface *surface,
		struct wlr_color_representation_v1_state *out) {
	struct wlr_addon *addon =
		wlr_addon_find(&surface->addons, manager, &color_repr_addon_impl);
	if (addon == NULL) {
		return false;
	}

	struct wlr_color_representation_v1 *color_repr =
		wl_container_of(addon, color_repr, addon);
	*out = color_repr->current;
	return true;
}
