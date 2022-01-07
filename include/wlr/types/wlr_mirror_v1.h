/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_wlr_mirror_v1_V1_H
#define WLR_TYPES_wlr_mirror_v1_V1_H

#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

/**
 * Allows mirroring: rendering some contents of one output (the src) on another
 * output (the dst). dst is fixed for the duration of the session, src may vary.
 *
 * On output_srcs precommit, wlr_mirror_v1::ready is emitted. The compositor may
 * call wlr_mirror_v1_request_ to request to render a frame on dst.
 *
 * Compositor must not render on dst for the duration of the session.
 *
 * Multiple sessions may run concurrently and one session may mirror another.
 *
 * Session will end:
 *   disable/destroy of dst or all srcs
 *   wlr_mirror_v1_request_box called with box outside of src
 *   wlr_mirror_v1_destroy
 */

enum wlr_mirror_v1_scale {
	/**
	 * src will be stretched to cover dst, distorting if necessary.
	 */
	WLR_MIRROR_V1_SCALE_FULL,
	/**
	 * src will be stretched to the width or the height of dst, preserving the
	 * aspect ratio.
	 */
	WLR_MIRROR_V1_SCALE_ASPECT,
	/**
	 * src will be rendered 1:1 at the center of dst. Content may be lost.
	 */
	WLR_MIRROR_V1_SCALE_CENTER,
};

/**
 * Immutable over session.
 */
struct wlr_mirror_v1_params {

	enum wlr_mirror_v1_scale scale;

	/**
	 * Render the src cursor on dst.
	 */
	bool overlay_cursor;

	/**
	 * srcs to send wlr_mirror_v1::events::ready
	 */
	struct wl_array output_srcs;

	/**
	 * dst, will have mirror_dst set for the duration of the session.
	 */
	struct wlr_output *output_dst;
};

struct wlr_mirror_v1_state;
struct wlr_mirror_v1 {

	struct {
		/**
		 * Ready to render a frame. Handler should call wlr_mirror_v1_request_
		 * Emitted at precommit, passes potential src.
		 */
		struct wl_signal ready;

		/**
		 * Mirror session is over.
		 */
		struct wl_signal destroy;
	} events;

	// private state
	struct wlr_mirror_v1_state *state;
};

/**
 * Create a mirror session.
 *
 * Compositor must stop rendering on dst immediately after this.
 */
struct wlr_mirror_v1 *wlr_mirror_v1_create(struct wlr_mirror_v1_params *params);

/**
 * Destroy a mirror session.
 *
 * Compositor may resume rendering on dst.
 */
void wlr_mirror_v1_destroy(struct wlr_mirror_v1 *mirror);

/**
 * Request a blank frame on dst.
 *
 * Should be invoked during the wlr_mirror_v1::events::ready handler.
 */
void wlr_mirror_v1_request_blank(struct wlr_mirror_v1 *mirror);

/**
 * Request a frame to render a box within src on dst. box is in output local
 * coordinates, with respect to its transformation.
 *
 * Should be invoked during the wlr_mirror_v1::events::ready handler.
 */
void wlr_mirror_v1_request_box(struct wlr_mirror_v1 *mirror,
		struct wlr_output *output_src, struct wlr_box box);

#endif

