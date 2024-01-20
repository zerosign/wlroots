#ifndef WLR_RENDER_SWAPCHAIN_H
#define WLR_RENDER_SWAPCHAIN_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_format_set.h>

#define WLR_SWAPCHAIN_CAP 4

struct wlr_swapchain_slot {
	struct wlr_buffer *buffer;
	bool acquired; // waiting for release

	struct wl_listener release;
};

struct wlr_swapchain {
	struct wlr_allocator *allocator; // NULL if destroyed

	int width, height;
	struct wlr_drm_format format;

	struct wlr_swapchain_slot slots[WLR_SWAPCHAIN_CAP];

	struct wl_listener allocator_destroy;
};

struct wlr_swapchain *wlr_swapchain_create(
	struct wlr_allocator *alloc, int width, int height,
	const struct wlr_drm_format *format);

void wlr_swapchain_destroy(struct wlr_swapchain *swapchain);

/**
 * Acquire a buffer from the swap chain.
 *
 * The returned buffer is locked. When the caller is done with it, they must
 * unlock it by calling wlr_buffer_unlock.
 */
struct wlr_buffer *wlr_swapchain_acquire(struct wlr_swapchain *swapchain);
/**
 * Returns true if this buffer has been created by this swapchain, and false
 * otherwise.
 */
bool wlr_swapchain_has_buffer(struct wlr_swapchain *swapchain,
	struct wlr_buffer *buffer);

#endif
