#include <assert.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_source_v1.h>
#include <wlr/types/wlr_ext_image_source_v1.h>
#include "ext-image-source-v1-protocol.h"

static void source_handle_destroy(struct wl_client *client,
		struct wl_resource *source_resource) {
	wl_resource_destroy(source_resource);
}

static const struct ext_image_source_v1_interface source_impl = {
	.destroy = source_handle_destroy,
};

struct wlr_ext_image_source_v1 *wlr_ext_image_source_v1_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_source_v1_interface, &source_impl));
	return wl_resource_get_user_data(resource);
}

static void source_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void wlr_ext_image_source_v1_init(struct wlr_ext_image_source_v1 *source,
		const struct wlr_ext_image_source_v1_interface *impl) {
	*source = (struct wlr_ext_image_source_v1){
		.impl = impl,
	};
	wl_list_init(&source->resources);
	wl_signal_init(&source->events.destroy);
	wl_signal_init(&source->events.constraints_update);
	wl_signal_init(&source->events.frame);
}

void wlr_ext_image_source_v1_finish(struct wlr_ext_image_source_v1 *source) {
	wl_signal_emit_mutable(&source->events.destroy, NULL);

	struct wl_resource *resource;
	wl_resource_for_each(resource, &source->resources) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}

	free(source->shm_formats);
	wlr_drm_format_set_finish(&source->dmabuf_formats);
}

bool wlr_ext_image_source_v1_create_resource(struct wlr_ext_image_source_v1 *source,
		struct wl_client *client, uint32_t new_id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_source_v1_interface, 1, new_id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return false;
	}
	wl_resource_set_implementation(resource, &source_impl, source,
		source_handle_resource_destroy);
	if (source != NULL) {
		wl_list_insert(&source->resources, wl_resource_get_link(resource));
	} else {
		wl_list_init(wl_resource_get_link(resource));
	}
	return true;
}
