/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_TYPES_WLR_DAMAGE_RING_H
#define WLR_TYPES_WLR_DAMAGE_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pixman.h>
#include <wayland-server-core.h>

/* Keep track of as many buffers as a swapchain can hold */
#define WLR_DAMAGE_RING_BUFFERS_LEN 4

struct wlr_box;

struct wlr_damage_ring_buffer {
	struct wlr_buffer *buffer;
	struct wl_listener destroy;
	pixman_region32_t damage;
	uint64_t seq;
};

struct wlr_damage_ring {
	int32_t width, height;

	// Difference between the current buffer and the previous one
	pixman_region32_t current;

	// private state

	uint64_t last_buffer_seq;
	struct wlr_damage_ring_buffer buffers[WLR_DAMAGE_RING_BUFFERS_LEN];
};

void wlr_damage_ring_init(struct wlr_damage_ring *ring);

void wlr_damage_ring_finish(struct wlr_damage_ring *ring);

/**
 * Set ring bounds and damage the ring fully.
 *
 * Next time damage will be added, it will be cropped to the ring bounds.
 * If at least one of the dimensions is 0, bounds are removed.
 *
 * By default, a damage ring doesn't have bounds.
 */
void wlr_damage_ring_set_bounds(struct wlr_damage_ring *ring,
	int32_t width, int32_t height);

/**
 * Add a region to the current damage.
 *
 * Returns true if the region intersects the ring bounds, false otherwise.
 */
bool wlr_damage_ring_add(struct wlr_damage_ring *ring,
	const pixman_region32_t *damage);

/**
 * Add a box to the current damage.
 *
 * Returns true if the box intersects the ring bounds, false otherwise.
 */
bool wlr_damage_ring_add_box(struct wlr_damage_ring *ring,
	const struct wlr_box *box);

/**
 * Damage the ring fully.
 */
void wlr_damage_ring_add_whole(struct wlr_damage_ring *ring);

/**
 * Get accumulated buffer damage and rotate the damage ring.
 *
 * The accumulated buffer damage is the difference between the to-be-painted
 * buffer and the passed-in buffer. In other words, this is the region that
 * needs to be redrawn.
 *
 * Users should damage the ring if an error occurs while rendering or
 * submitting the new buffer to the backend.
 */
void wlr_damage_ring_rotate_buffer(struct wlr_damage_ring *ring,
	struct wlr_buffer *buffer, pixman_region32_t *damage);

#endif
