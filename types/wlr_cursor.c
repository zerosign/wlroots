#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

static void output_cursor_destroy(struct wlr_cursor_output_cursor *output_cursor) {
	wl_list_remove(&output_cursor->layout_output_destroy.link);
	wl_list_remove(&output_cursor->link);
	wlr_output_cursor_destroy(output_cursor->output_cursor);
	free(output_cursor);
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	struct wlr_cursor *cursor = wl_container_of(listener, cursor, layout_destroy);
	wlr_cursor_destroy(cursor);
}

static void handle_layout_output_destroy(struct wl_listener *listener, void *data) {
	struct wlr_cursor_output_cursor *output_cursor =
		wl_container_of(listener, output_cursor, layout_output_destroy);
	output_cursor_destroy(output_cursor);
}

static void layout_add(struct wlr_cursor *cursor, struct wlr_output_layout_output *l_output) {
	struct wlr_cursor_output_cursor *output_cursor = calloc(1, sizeof(*output_cursor));
	if (output_cursor == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_cursor_output_cursor");
		return;
	}
	output_cursor->cursor = cursor;

	output_cursor->output_cursor = wlr_output_cursor_create(l_output->output);
	if (output_cursor->output_cursor == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_output_cursor");
		free(output_cursor);
		return;
	}

	output_cursor->layout_output_destroy.notify = handle_layout_output_destroy;
	wl_signal_add(&l_output->events.destroy, &output_cursor->layout_output_destroy);

	wl_list_insert(&cursor->output_cursors, &output_cursor->link);
}

static void handle_layout_add(struct wl_listener *listener, void *data) {
	struct wlr_cursor *cursor = wl_container_of(listener, cursor, layout_add);
	struct wlr_output_layout_output *l_output = data;
	layout_add(cursor, l_output);
}

struct wlr_cursor *wlr_cursor_create(struct wlr_output_layout *layout) {
	struct wlr_cursor *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_cursor");
		return NULL;
	}

	wl_list_init(&cursor->output_cursors);

	wl_signal_init(&cursor->events.destroy);

	wl_signal_add(&layout->events.add, &cursor->layout_add);
	cursor->layout_add.notify = handle_layout_add;
	wl_signal_add(&layout->events.destroy, &cursor->layout_destroy);
	cursor->layout_destroy.notify = handle_layout_destroy;

	cursor->layout = layout;

	struct wlr_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		layout_add(cursor, l_output);
	}

	return cursor;
}

void wlr_cursor_destroy(struct wlr_cursor *cursor) {
	wl_signal_emit_mutable(&cursor->events.destroy, NULL);

	struct wlr_cursor_output_cursor *output_cursor, *tmp;
	wl_list_for_each_safe(output_cursor, tmp, &cursor->output_cursors, link) {
		output_cursor_destroy(output_cursor);
	}

	wl_list_remove(&cursor->layout_destroy.link);
	wl_list_remove(&cursor->layout_add.link);

	free(cursor);
}

void wlr_cursor_warp(struct wlr_cursor *cursor, double lx, double ly) {
	if (!isfinite(lx) || !isfinite(ly)) {
		assert(false);
		return;
	}

	struct wlr_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &cursor->output_cursors, link) {
		double output_x = lx, output_y = ly;
		wlr_output_layout_output_coords(cursor->layout,
			output_cursor->output_cursor->output, &output_x, &output_y);
		wlr_output_cursor_move(output_cursor->output_cursor,
			output_x, output_y);
	}

	cursor->x = lx;
	cursor->y = ly;
}

void wlr_cursor_set_image(struct wlr_cursor *cursor, const uint8_t *pixels,
		int32_t stride, uint32_t width, uint32_t height, int32_t hotspot_x,
		int32_t hotspot_y, float scale) {
	struct wlr_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &cursor->output_cursors, link) {
		float output_scale = output_cursor->output_cursor->output->scale;
		if (scale > 0 && output_scale != scale) {
			continue;
		}

		wlr_output_cursor_set_image(output_cursor->output_cursor, pixels,
			stride, width, height, hotspot_x, hotspot_y);
	}
}

void wlr_cursor_set_surface(struct wlr_cursor *cursor, struct wlr_surface *surface,
		int32_t hotspot_x, int32_t hotspot_y) {
	struct wlr_cursor_output_cursor *output_cursor;
	wl_list_for_each(output_cursor, &cursor->output_cursors, link) {
		wlr_output_cursor_set_surface(output_cursor->output_cursor, surface,
			hotspot_x, hotspot_y);
	}
}
