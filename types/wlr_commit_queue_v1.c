#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_commit_queue_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

#define COMMIT_QUEUE_MANAGER_V1_VERSION 1

struct wlr_commit_queue_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;

	struct {
		enum wp_commit_queue_v1_queue_mode mode;
	} current, pending;

	struct wlr_addon surface_addon;
	struct wl_listener surface_commit;
};

static const struct wp_commit_queue_v1_interface queue_impl;

// Returns NULL if inert
static struct wlr_commit_queue_v1 *queue_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_commit_queue_v1_interface, &queue_impl));
	return wl_resource_get_user_data(resource);
}

static void resource_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void queue_destroy(struct wlr_commit_queue_v1 *queue) {
	if (queue == NULL) {
		return;
	}
	wl_list_remove(&queue->surface_commit.link);
	wlr_addon_finish(&queue->surface_addon);
	wl_resource_set_user_data(queue->resource, NULL); // make inert
	free(queue);
}

static void surface_addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_commit_queue_v1 *queue = wl_container_of(addon, queue, surface_addon);
	queue_destroy(queue);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "wp_commit_queue_v1",
	.destroy = surface_addon_handle_destroy,
};

static void queue_handle_set_queue_mode(struct wl_client *client,
		struct wl_resource *resource, uint32_t mode) {
	struct wlr_commit_queue_v1 *queue = queue_from_resource(resource);

	if (mode > WP_COMMIT_QUEUE_V1_QUEUE_MODE_FIFO) {
		wl_resource_post_error(resource, WP_COMMIT_QUEUE_V1_ERROR_INVALID_QUEUE_MODE,
			"Invalid queue mode");
		return;
	}

	queue->pending.mode = mode;
}

static const struct wp_commit_queue_v1_interface queue_impl = {
	.destroy = resource_handle_destroy,
	.set_queue_mode = queue_handle_set_queue_mode,
};

static void queue_handle_surface_commit(struct wl_listener *listener, void *data) {
	struct wlr_commit_queue_v1 *queue = wl_container_of(listener, queue, surface_commit);
	queue->current = queue->pending;
}

static void queue_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_commit_queue_v1 *queue = queue_from_resource(resource);
	queue_destroy(queue);
}

static void manager_handle_get_queue_controller(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	if (wlr_addon_find(&surface->addons, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COMMIT_QUEUE_MANAGER_V1_ERROR_QUEUE_CONTROLLER_ALREADY_EXISTS,
			"A wp_commit_queue_v1 object already exists for this surface");
		return;
	}

	struct wlr_commit_queue_v1 *queue = calloc(1, sizeof(*queue));
	if (queue == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	queue->surface = surface;

	uint32_t version = wl_resource_get_version(manager_resource);
	queue->resource = wl_resource_create(client,
		&wp_commit_queue_v1_interface, version, id);
	if (queue->resource == NULL) {
		free(queue);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(queue->resource,
		&queue_impl, queue, queue_handle_resource_destroy);

	wlr_addon_init(&queue->surface_addon, &surface->addons, NULL, &surface_addon_impl);

	queue->surface_commit.notify = queue_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &queue->surface_commit);
}

static const struct wp_commit_queue_manager_v1_interface manager_impl = {
	.destroy = resource_handle_destroy,
	.get_queue_controller = manager_handle_get_queue_controller,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_commit_queue_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void manager_handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_commit_queue_manager_v1 *manager = wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
}

struct wlr_commit_queue_manager_v1 *wlr_commit_queue_manager_v1_create(struct wl_display *display, uint32_t version) {
	assert(version <= COMMIT_QUEUE_MANAGER_V1_VERSION);

	struct wlr_commit_queue_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&wp_commit_queue_manager_v1_interface, version, NULL, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_signal_init(&manager->events.destroy);

	manager->display_destroy.notify = manager_handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

enum wp_commit_queue_v1_queue_mode wlr_commit_queue_v1_get_surface_mode(struct wlr_surface *surface) {
	struct wlr_addon *addon =
		wlr_addon_find(&surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return WP_COMMIT_QUEUE_V1_QUEUE_MODE_MAILBOX;
	}
	struct wlr_commit_queue_v1 *queue = wl_container_of(addon, queue, surface_addon);
	return queue->current.mode;
}
