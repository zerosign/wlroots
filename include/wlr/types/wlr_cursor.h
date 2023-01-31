/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_CURSOR_H
#define WLR_TYPES_WLR_CURSOR_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>

/**
 * A helper to keep track of a cursor's position in an output layout and
 * manage output cursors.
 *
 * When the output layout is destroyed, the cursor is destroyed as well.
 */
struct wlr_cursor {
	double x, y;

	struct wl_list output_cursors; // wlr_cursor_output_cursor.link
	struct wlr_output_layout *layout;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	// private state

	struct wl_listener layout_add;
	struct wl_listener layout_destroy;
};

struct wlr_cursor_output_cursor {
	struct wlr_cursor *cursor;
	struct wlr_output_cursor *output_cursor;
	struct wl_list link; // wlr_cursor.output_cursors

	// private state

	struct wl_listener layout_output_destroy;
};

struct wlr_cursor *wlr_cursor_create(struct wlr_output_layout *layout);

void wlr_cursor_destroy(struct wlr_cursor *cursor);

/**
 * Warp the cursor to the given x and y in layout coordinates.
 */
void wlr_cursor_warp(struct wlr_cursor *cursor, double lx, double ly);

/**
 * Set the cursor image. stride is given in bytes. If pixels is NULL, hides the
 * cursor.
 *
 * If scale isn't zero, the image is only set on outputs having the provided
 * scale.
 */
void wlr_cursor_set_image(struct wlr_cursor *cursor, const uint8_t *pixels,
	int32_t stride, uint32_t width, uint32_t height, int32_t hotspot_x,
	int32_t hotspot_y, float scale);

/**
 * Set the cursor surface. The surface can be committed to update the cursor
 * image. The surface position is subtracted from the hotspot. A NULL surface
 * commit hides the cursor.
 */
void wlr_cursor_set_surface(struct wlr_cursor *cursor, struct wlr_surface *surface,
	int32_t hotspot_x, int32_t hotspot_y);

#endif
