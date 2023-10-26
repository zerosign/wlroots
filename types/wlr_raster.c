#include <assert.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_raster.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include "types/wlr_buffer.h"
#include "types/wlr_raster.h"

static void raster_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct wlr_raster *raster = wl_container_of(listener, raster, buffer_release);
	raster->buffer = NULL;
	wl_list_remove(&raster->buffer_release.link);
	wl_list_init(&raster->buffer_release.link);
}

struct wlr_raster *wlr_raster_create(struct wlr_buffer *buffer) {
	struct wlr_raster *raster = calloc(1, sizeof(*raster));
	if (!raster) {
		return NULL;
	}

	wl_signal_init(&raster->events.destroy);

	assert(buffer);
	raster->opaque = buffer_is_opaque(buffer);
	raster->width = buffer->width;
	raster->height = buffer->height;
	raster->buffer = buffer;

	raster->n_locks = 1;

	raster->buffer_release.notify = raster_handle_buffer_release;
	wl_signal_add(&raster->buffer->events.release, &raster->buffer_release);

	return raster;
}

static void raster_consider_destroy(struct wlr_raster *raster) {
	if (raster->n_locks > 0) {
		return;
	}

	wl_signal_emit_mutable(&raster->events.destroy, NULL);

	if (raster->texture) {
		wl_list_remove(&raster->renderer_destroy.link);
	}

	wl_list_remove(&raster->buffer_release.link);
	free(raster);
}

struct wlr_raster *wlr_raster_lock(struct wlr_raster *raster) {
	raster->n_locks++;
	return raster;
}

void wlr_raster_unlock(struct wlr_raster *raster) {
	if (!raster) {
		return;
	}

	assert(raster->n_locks > 0);

	raster->n_locks--;
	raster_consider_destroy(raster);
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_raster *raster = wl_container_of(listener, raster, renderer_destroy);
	wlr_raster_detach(raster, raster->texture);
}

void wlr_raster_attach(struct wlr_raster *raster, struct wlr_texture *texture) {
	assert(texture->width == raster->width && texture->height == raster->height);
	assert(!raster->texture);

	raster->renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&texture->renderer->events.destroy, &raster->renderer_destroy);

	raster->texture = texture;
}

void wlr_raster_detach(struct wlr_raster *raster, struct wlr_texture *texture) {
	assert(texture);
	assert(raster->texture == texture);

	wl_list_remove(&raster->renderer_destroy.link);
	raster->texture = NULL;
}

struct wlr_texture *wlr_raster_create_texture(struct wlr_raster *raster,
		struct wlr_renderer *renderer) {
	if (raster->texture) {
		assert(raster->texture->renderer == renderer);
		return raster->texture;
	}

	assert(raster->buffer);

	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(raster->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, raster->buffer);
	if (texture) {
		wlr_raster_attach(raster, texture);
	}

	return texture;
}
