#include <assert.h>
#include <pixman.h>
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

struct raster_update_state {
	struct wlr_buffer *buffer;
	pixman_region32_t damage;

	struct wlr_raster *new_raster;
	struct wlr_raster *old_raster;

	struct wl_listener old_raster_destroy;
	struct wl_listener new_raster_destroy;
	struct wl_listener buffer_release;
};

static void destroy_raster_update_state(struct raster_update_state *state) {
	wl_list_remove(&state->old_raster_destroy.link);
	wl_list_remove(&state->new_raster_destroy.link);
	wl_list_remove(&state->buffer_release.link);
	pixman_region32_fini(&state->damage);
	free(state);
}

static void raster_update_handle_new_raster_destroy(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, new_raster_destroy);
	destroy_raster_update_state(state);
}

static void raster_update_handle_old_raster_destroy(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, old_raster_destroy);

	// if the new raster already has a texture, there's nothing we can do to help.
	if (state->new_raster->texture) {
		assert(state->new_raster->texture->renderer == state->old_raster->texture->renderer);
		destroy_raster_update_state(state);
		return;
	}

	struct wlr_texture *texture = state->old_raster->texture;
	if (!texture) {
		destroy_raster_update_state(state);
		return;
	}

	if (wlr_texture_update_from_buffer(texture, state->buffer, &state->damage)) {
		wlr_raster_detach(state->old_raster, texture);
		wlr_raster_attach(state->new_raster, texture);
	}

	destroy_raster_update_state(state);
}

static void raster_update_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct raster_update_state *state = wl_container_of(listener, state, buffer_release);
	destroy_raster_update_state(state);
}

struct wlr_raster *wlr_raster_update(struct wlr_raster *raster,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	struct raster_update_state *state = calloc(1, sizeof(*state));
	if (!state) {
		return NULL;
	}

	struct wlr_raster *new_raster = wlr_raster_create(buffer);
	if (!new_raster) {
		free(state);
		return NULL;
	}

	state->old_raster_destroy.notify = raster_update_handle_old_raster_destroy;
	wl_signal_add(&raster->events.destroy, &state->old_raster_destroy);
	state->new_raster_destroy.notify = raster_update_handle_new_raster_destroy;
	wl_signal_add(&new_raster->events.destroy, &state->new_raster_destroy);
	state->buffer_release.notify = raster_update_handle_buffer_release;
	wl_signal_add(&buffer->events.release, &state->buffer_release);

	state->new_raster = new_raster;
	state->old_raster = raster;
	state->buffer = buffer;

	pixman_region32_init(&state->damage);
	pixman_region32_copy(&state->damage, damage);

	return new_raster;
}
