/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_COLOR_REPRESENTATION_V1
#define WLR_TYPES_WLR_COLOR_REPRESENTATION_V1

#include <wayland-server-core.h>
#include "color-representation-v1-protocol.h"

struct wlr_surface;

struct wlr_color_representation_v1_state {
	enum wp_color_representation_v1_range range;
	enum wp_color_representation_v1_coefficients coefficients;
	enum wp_color_representation_v1_chroma_location x_chroma_offset, y_chroma_offset;
};

struct wlr_color_representation_manager_v1;

struct wlr_color_representation_manager_v1 *
wlr_color_representation_manager_v1_create(struct wl_display *display);

bool wlr_color_representation_manager_v1_get_surface_state(
	struct wlr_color_representation_manager_v1 *manager,
	struct wlr_surface *surface,
	struct wlr_color_representation_v1_state *out);

#endif
