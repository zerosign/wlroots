#include <assert.h>
#include <wlr/types/wlr_raster.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include "types/wlr_buffer.h"
#include "util/signal.h"

struct wlr_raster *wlr_raster_create(struct wlr_buffer *buffer) {
	struct wlr_raster *raster = calloc(1, sizeof(*raster));
	if (!raster) {
		return NULL;
	}

	wl_list_init(&raster->sources);
	wl_signal_init(&raster->events.destroy);

	assert(buffer);
	raster->width = buffer->width;
	raster->height = buffer->height;
	raster->buffer = wlr_buffer_lock(buffer);
	
	raster->n_locks = 1;

	return raster;
}

void wlr_raster_remove_buffer(struct wlr_raster *raster) {
	assert(raster->buffer);
	assert(!wl_list_empty(&raster->sources));

	wlr_buffer_unlock(raster->buffer);
	raster->buffer = NULL;
}

static void raster_consider_destroy(struct wlr_raster *raster) {
	if (raster->n_locks > 0) {
		return;
	}

	wlr_signal_emit_safe(&raster->events.destroy, NULL);

	struct wlr_texture *texture, *texture_tmp;
	wl_list_for_each_safe(texture, texture_tmp, &raster->sources, link) {
		wlr_texture_destroy(texture);
	}

	wlr_buffer_unlock(raster->buffer);
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

void wlr_raster_attach(struct wlr_raster *raster, struct wlr_texture *texture) {
	assert(texture);
	assert(!texture->raster);
	assert(texture->width == raster->width && texture->height == raster->height);

	wl_list_insert(&raster->sources, &texture->link);
	texture->raster = raster;
}

void wlr_raster_detach(struct wlr_raster *raster, struct wlr_texture *texture) {
	if (!texture) {
		return;
	}
	
	assert(texture->raster == raster);
	texture->raster = NULL;
	wl_list_remove(&texture->link);
}

struct wlr_raster *wlr_raster_from_pixels(uint32_t fmt, uint32_t stride,
		uint32_t width, uint32_t height, const void *data) {
	assert(data);

	struct wlr_readonly_data_buffer *buffer =
		readonly_data_buffer_create(fmt, stride, width, height, data);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_raster *raster = wlr_raster_create(&buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	readonly_data_buffer_drop(buffer);

	return raster;
}
