/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_EXT_IMAGE_SOURCE_V1_H
#define WLR_TYPES_WLR_EXT_IMAGE_SOURCE_V1_H

#include <pixman.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_format_set.h>

/**
 * A screen capture source.
 *
 * When the size, device or formats change, the constraints_update event is
 * emitted.
 *
 * The device and formats advertised are suitable for copying into a
 * struct wlr_buffer.
 */
struct wlr_ext_image_source_v1 {
	const struct wlr_ext_image_source_v1_interface *impl;
	struct wl_list resources; // wl_resource_get_link()

	uint32_t width, height;

	uint32_t *shm_formats;
	size_t shm_formats_len;

	dev_t dmabuf_device;
	struct wlr_drm_format_set dmabuf_formats;

	struct {
		struct wl_signal constraints_update;
		struct wl_signal frame; // struct wlr_ext_image_source_v1_frame_event
		struct wl_signal destroy;
	} events;
};

/**
 * Event indicating that the source has produced a new frame.
 */
struct wlr_ext_image_source_v1_frame_event {
	const pixman_region32_t *damage;
};

/**
 * A cursor capture source.
 *
 * Provides additional cursor-specific functionality on top of
 * struct wlr_ext_image_source_v1.
 */
struct wlr_ext_image_source_v1_cursor {
	struct wlr_ext_image_source_v1 base;

	bool entered;
	int32_t x, y;
	struct {
		int32_t x, y;
	} hotspot;

	struct {
		struct wl_signal update;
	} events;
};

/**
 * Interface exposing one screen capture source per output.
 */
struct wlr_ext_output_image_source_manager_v1 {
	struct wl_global *global;

	// private state

	struct wl_listener display_destroy;
};

/**
 * Obtain a struct wlr_ext_image_source_v1 from an ext_image_source_v1
 * resource.
 *
 * Asserts that the resource has the correct type. Returns NULL if the resource
 * is inert.
 */
struct wlr_ext_image_source_v1 *wlr_ext_image_source_v1_from_resource(struct wl_resource *resource);

struct wlr_ext_output_image_source_manager_v1 *wlr_ext_output_image_source_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif
