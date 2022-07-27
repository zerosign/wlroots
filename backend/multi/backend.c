#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <wlr/backend/interface.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include "render/wlr_renderer.h"
#include "backend/backend.h"
#include "backend/multi.h"
#include "render/allocator/allocator.h"

struct subbackend_state {
	struct wlr_backend *backend;
	struct wlr_backend *container;
	struct wl_listener new_input;
	struct wl_listener new_output;
	struct wl_listener destroy;
	struct wl_list link;
};

static struct wlr_multi_backend *multi_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_multi(wlr_backend));
	struct wlr_multi_backend *backend = wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static bool multi_backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_multi_backend *backend = multi_backend_from_backend(wlr_backend);
	struct subbackend_state *sub;
	wl_list_for_each(sub, &backend->backends, link) {
		if (!wlr_backend_start(sub->backend)) {
			wlr_log(WLR_ERROR, "Failed to initialize backend.");
			return false;
		}
	}
	return true;
}

static void subbackend_state_destroy(struct subbackend_state *sub) {
	wl_list_remove(&sub->new_input.link);
	wl_list_remove(&sub->new_output.link);
	wl_list_remove(&sub->destroy.link);
	wl_list_remove(&sub->link);
	free(sub);
}

static void multi_backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_multi_backend *backend = multi_backend_from_backend(wlr_backend);

	wl_list_remove(&backend->event_loop_destroy.link);

	wlr_backend_finish(wlr_backend);

	// Some backends may depend on other backends, ie. destroying a backend may
	// also destroy other backends
	while (!wl_list_empty(&backend->backends)) {
		struct subbackend_state *sub =
			wl_container_of(backend->backends.next, sub, link);
		wlr_backend_destroy(sub->backend);
	}
	wlr_multi_gpu_destroy(backend->multi_gpu);

	free(backend);
}

static int multi_backend_get_drm_fd(struct wlr_backend *backend) {
	struct wlr_multi_backend *multi = multi_backend_from_backend(backend);

	struct subbackend_state *sub;
	wl_list_for_each(sub, &multi->backends, link) {
		if (sub->backend->impl->get_drm_fd) {
			return wlr_backend_get_drm_fd(sub->backend);
		}
	}

	return -1;
}

static uint32_t multi_backend_get_buffer_caps(struct wlr_backend *backend) {
	struct wlr_multi_backend *multi = multi_backend_from_backend(backend);

	if (wl_list_empty(&multi->backends)) {
		return 0;
	}

	uint32_t caps = WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_DMABUF
			| WLR_BUFFER_CAP_SHM;

	struct subbackend_state *sub;
	wl_list_for_each(sub, &multi->backends, link) {
		uint32_t backend_caps = backend_get_buffer_caps(sub->backend);
		if (backend_caps != 0) {
			// only count backend capable of presenting a buffer
			caps = caps & backend_caps;
		}
	}

	return caps;
}

static const struct wlr_backend_impl backend_impl = {
	.start = multi_backend_start,
	.destroy = multi_backend_destroy,
	.get_drm_fd = multi_backend_get_drm_fd,
	.get_buffer_caps = multi_backend_get_buffer_caps,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct wlr_multi_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	multi_backend_destroy((struct wlr_backend*)backend);
}

struct wlr_backend *wlr_multi_backend_create(struct wl_event_loop *loop) {
	struct wlr_multi_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Backend allocation failed");
		return NULL;
	}

	wl_list_init(&backend->backends);
	backend->multi_gpu = wlr_multi_gpu_create();
	wlr_backend_init(&backend->backend, &backend_impl);

	wl_signal_init(&backend->events.backend_add);
	wl_signal_init(&backend->events.backend_remove);

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;
}

bool wlr_backend_is_multi(struct wlr_backend *b) {
	return b->impl == &backend_impl;
}

static void new_input_reemit(struct wl_listener *listener, void *data) {
	struct subbackend_state *state = wl_container_of(listener,
			state, new_input);
	wl_signal_emit_mutable(&state->container->events.new_input, data);
}

static void new_output_reemit(struct wl_listener *listener, void *data) {
	struct subbackend_state *state = wl_container_of(listener,
			state, new_output);
	wl_signal_emit_mutable(&state->container->events.new_output, data);
}

static void handle_subbackend_destroy(struct wl_listener *listener,
		void *data) {
	struct subbackend_state *state = wl_container_of(listener, state, destroy);
	subbackend_state_destroy(state);
}

static struct subbackend_state *multi_backend_get_subbackend(struct wlr_multi_backend *multi,
		struct wlr_backend *backend) {
	struct subbackend_state *sub = NULL;
	wl_list_for_each(sub, &multi->backends, link) {
		if (sub->backend == backend) {
			return sub;
		}
	}
	return NULL;
}

bool wlr_multi_backend_add(struct wlr_backend *_multi,
		struct wlr_backend *backend) {
	assert(_multi && backend);
	assert(_multi != backend);

	struct wlr_multi_backend *multi = multi_backend_from_backend(_multi);

	if (multi_backend_get_subbackend(multi, backend)) {
		// already added
		return true;
	}

	struct subbackend_state *sub = calloc(1, sizeof(*sub));
	if (sub == NULL) {
		wlr_log(WLR_ERROR, "Could not add backend: allocation failed");
		return false;
	}
	wl_list_insert(multi->backends.prev, &sub->link);

	sub->backend = backend;
	sub->container = &multi->backend;

	wl_signal_add(&backend->events.destroy, &sub->destroy);
	sub->destroy.notify = handle_subbackend_destroy;

	wl_signal_add(&backend->events.new_input, &sub->new_input);
	sub->new_input.notify = new_input_reemit;

	wl_signal_add(&backend->events.new_output, &sub->new_output);
	sub->new_output.notify = new_output_reemit;

	wl_signal_emit_mutable(&multi->events.backend_add, backend);
	return true;
}

void wlr_multi_backend_remove(struct wlr_backend *_multi,
		struct wlr_backend *backend) {
	struct wlr_multi_backend *multi = multi_backend_from_backend(_multi);

	struct subbackend_state *sub =
		multi_backend_get_subbackend(multi, backend);

	if (sub) {
		wl_signal_emit_mutable(&multi->events.backend_remove, backend);
		subbackend_state_destroy(sub);
	}
}

bool wlr_multi_is_empty(struct wlr_backend *_backend) {
	assert(wlr_backend_is_multi(_backend));
	struct wlr_multi_backend *backend = (struct wlr_multi_backend *)_backend;
	return wl_list_length(&backend->backends) < 1;
}

void wlr_multi_for_each_backend(struct wlr_backend *_backend,
		void (*callback)(struct wlr_backend *backend, void *data), void *data) {
	assert(wlr_backend_is_multi(_backend));
	struct wlr_multi_backend *backend = (struct wlr_multi_backend *)_backend;
	struct subbackend_state *sub;
	wl_list_for_each(sub, &backend->backends, link) {
		callback(sub->backend, data);
	}
}

/*
 * Create a wlr_multi_gpu struct and populate it with a renderer and allocator for each
 * device in the system. This is done by finding all DRM nodes using drmGetDevices2.
 */
struct wlr_multi_gpu *wlr_multi_gpu_create(void) {
	int flags = 0;
	struct wlr_multi_gpu *multi_gpu = NULL;
	int devices_len = drmGetDevices2(flags, NULL, 0);

	if (devices_len < 0) {
		wlr_log(WLR_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		return NULL;
	}
	drmDevice **devices = calloc(devices_len, sizeof(*devices));
	if (devices == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto out;
	}
	devices_len = drmGetDevices2(flags, devices, devices_len);
	if (devices_len < 0) {
		wlr_log(WLR_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		goto out;
	}

	multi_gpu = calloc(1, sizeof(struct wlr_multi_gpu));
	if (!multi_gpu) {
		goto out;
	}
	wl_list_init(&multi_gpu->devices);

	for (int i = 0; i < devices_len; i++) {
		drmDevice *dev = devices[i];
		if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
			const char *name = dev->nodes[DRM_NODE_RENDER];
			wlr_log(WLR_DEBUG, "Opening DRM render node '%s'", name);
			int fd = open(name, O_RDWR | O_CLOEXEC);
			if (fd < 0) {
				wlr_log_errno(WLR_ERROR, "Failed to open '%s'", name);
				goto out;
			}

			// Create a renderer/allocator and add it as a new device
			struct wlr_renderer *renderer = renderer_autocreate_with_drm_fd(fd);
			if (!renderer) {
				wlr_log(WLR_ERROR, "Failed to create multi-GPU renderer");
				goto fail;
			}

			struct wlr_allocator *allocator =
				allocator_autocreate_with_drm_fd(WLR_BUFFER_CAP_DMABUF, renderer, fd);
			if (!allocator) {
				wlr_log(WLR_ERROR, "Failed to create multi-GPU allocator");
				wlr_renderer_destroy(renderer);
				goto fail;
			}

			struct wlr_multi_gpu_device *device = calloc(1, sizeof(struct wlr_multi_gpu_device));
			if (!device) {
				wlr_allocator_destroy(allocator);
				wlr_renderer_destroy(renderer);
				goto fail;
			}
			wl_list_insert(&multi_gpu->devices, &device->link);
			device->renderer = renderer;
			device->allocator = allocator;
		}
	}

	goto out;

fail:
	wlr_multi_gpu_destroy(multi_gpu);
	multi_gpu = NULL;

out:
	for (int i = 0; i < devices_len; i++) {
		drmFreeDevice(&devices[i]);
	}
	if (devices) {
		free(devices);
	}

	return multi_gpu;
}

void wlr_multi_gpu_destroy(struct wlr_multi_gpu *multi_gpu) {
	struct wlr_multi_gpu_device *device;
	// Remove and destroy all devices
	wl_list_for_each(device, &multi_gpu->devices, link) {
		wlr_allocator_destroy(device->allocator);
		wlr_renderer_destroy(device->renderer);
		wl_list_remove(&device->link);
		free(device);
	}

	free(multi_gpu);
}
