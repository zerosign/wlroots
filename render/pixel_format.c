#include <assert.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "render/pixel_format.h"

static const struct wlr_pixel_format_info pixel_format_info[] = {
	{
		.drm_format = DRM_FORMAT_XRGB8888,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_ARGB8888,
		.opaque_substitute = DRM_FORMAT_XRGB8888,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_XBGR8888,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_ABGR8888,
		.opaque_substitute = DRM_FORMAT_XBGR8888,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_RGBX8888,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_RGBA8888,
		.opaque_substitute = DRM_FORMAT_RGBX8888,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_BGRX8888,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_BGRA8888,
		.opaque_substitute = DRM_FORMAT_BGRX8888,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_R8,
		.bpp = 8,
	},
	{
		.drm_format = DRM_FORMAT_GR88,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_RGB888,
		.bpp = 24,
	},
	{
		.drm_format = DRM_FORMAT_BGR888,
		.bpp = 24,
	},
	{
		.drm_format = DRM_FORMAT_RGBX4444,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_RGBA4444,
		.opaque_substitute = DRM_FORMAT_RGBX4444,
		.bpp = 16,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_BGRX4444,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_BGRA4444,
		.opaque_substitute = DRM_FORMAT_BGRX4444,
		.bpp = 16,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_RGBX5551,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_RGBA5551,
		.opaque_substitute = DRM_FORMAT_RGBX5551,
		.bpp = 16,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_BGRX5551,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_BGRA5551,
		.opaque_substitute = DRM_FORMAT_BGRX5551,
		.bpp = 16,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_XRGB1555,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_ARGB1555,
		.opaque_substitute = DRM_FORMAT_XRGB1555,
		.bpp = 16,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_RGB565,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_BGR565,
		.bpp = 16,
	},
	{
		.drm_format = DRM_FORMAT_XRGB2101010,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_ARGB2101010,
		.opaque_substitute = DRM_FORMAT_XRGB2101010,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_XBGR2101010,
		.bpp = 32,
	},
	{
		.drm_format = DRM_FORMAT_ABGR2101010,
		.opaque_substitute = DRM_FORMAT_XBGR2101010,
		.bpp = 32,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616F,
		.bpp = 64,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616F,
		.opaque_substitute = DRM_FORMAT_XBGR16161616F,
		.bpp = 64,
		.has_alpha = true,
	},
	{
		.drm_format = DRM_FORMAT_XBGR16161616,
		.bpp = 64,
	},
	{
		.drm_format = DRM_FORMAT_ABGR16161616,
		.opaque_substitute = DRM_FORMAT_XBGR16161616,
		.bpp = 64,
		.has_alpha = true,
	},
};

static const size_t pixel_format_info_size =
	sizeof(pixel_format_info) / sizeof(pixel_format_info[0]);

const struct wlr_pixel_format_info *drm_get_pixel_format_info(uint32_t fmt) {
	for (size_t i = 0; i < pixel_format_info_size; ++i) {
		if (pixel_format_info[i].drm_format == fmt) {
			return &pixel_format_info[i];
		}
	}

	return NULL;
}

uint32_t convert_wl_shm_format_to_drm(enum wl_shm_format fmt) {
	switch (fmt) {
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	default:
		return (uint32_t)fmt;
	}
}

enum wl_shm_format convert_drm_format_to_wl_shm(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	default:
		return (enum wl_shm_format)fmt;
	}
}

bool pixel_format_info_check_stride(const struct wlr_pixel_format_info *fmt,
		int32_t stride, int32_t width) {
	assert(fmt->bpp > 0 && fmt->bpp % 8 == 0);
	int32_t bytes_per_pixel = (int32_t)(fmt->bpp / 8);
	if (stride % bytes_per_pixel != 0) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (incompatible with %d "
			"bytes-per-pixel)", stride, bytes_per_pixel);
		return false;
	}
	if (stride / bytes_per_pixel < width) {
		wlr_log(WLR_DEBUG, "Invalid stride %d (too small for %d "
			"bytes-per-pixel and width %d)", stride, bytes_per_pixel, width);
		return false;
	}
	return true;
}

static char *format_str(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (len < 0) {
		return NULL;
	}

	char *str = malloc(len + 1);
	if (str == NULL) {
		return NULL;
	}

	va_start(args, fmt);
	vsnprintf(str, len + 1, fmt, args);
	va_end(args);

	return str;
}

char *get_drm_format_description(uint32_t format) {
	char *name = drmGetFormatName(format);
	char *str = format_str("%s (0x%08"PRIX32")", name ? name : "<unknown>", format);
	free(name);
	return str;
}

char *get_drm_modifier_description(uint64_t modifier) {
	char *name = drmGetFormatModifierName(modifier);
	char *str = format_str("%s (0x%016"PRIX64")", name ? name : "<unknown>", modifier);
	free(name);
	return str;
}
