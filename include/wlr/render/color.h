/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_COLOR_H
#define WLR_RENDER_COLOR_H

#include <stdbool.h>
#include <sys/types.h>

/**
 * A color transformation formula.
 *
 * The formula is approximated via a 3D look-up table. A 3D LUT is a
 * three-dimensional array where each element is an RGB triplet. The flat lut_3d
 * array has a length of dim_len³.
 *
 * Color channel values in the range [0.0, 1.0] are mapped linearly to
 * 3D LUT indices such that 0.0 maps exactly to the first element and 1.0 maps
 * exactly to the last element in each dimension.
 *
 * The offset of the RGB triplet given red, green and blue indices r_index,
 * g_index and b_index is:
 *
 *     offset = 3 * (r_index + dim_len * g_index + dim_len * dim_len * b_index)
 */
struct wlr_color_transform {
	float *lut_3d;
	size_t dim_len;
};

/**
 * Initialize a color transformation to convert sRGB to an ICC profile.
 */
bool wlr_color_transform_init_srgb_to_icc(struct wlr_color_transform *tr,
	const void *data, size_t size);

void wlr_color_transform_finish(struct wlr_color_transform *tr);

#endif
