#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/util/box.h>

#define WLR_DAMAGE_RING_MAX_RECTS 20

void wlr_damage_ring_init(struct wlr_damage_ring *ring) {
	*ring = (struct wlr_damage_ring){
		.width = INT_MAX,
		.height = INT_MAX,
	};

	pixman_region32_init(&ring->current);
	wl_list_init(&ring->previous);
}

static void entry_destroy(struct wlr_damage_ring_entry *entry) {
	if (entry->buffer) {
		wl_list_remove(&entry->buffer_destroy.link);
	}

	wl_list_remove(&entry->link);
	pixman_region32_fini(&entry->damage);
	free(entry);
}

void wlr_damage_ring_finish(struct wlr_damage_ring *ring) {
	pixman_region32_fini(&ring->current);

	struct wlr_damage_ring_entry *entry, *tmp_entry;
	wl_list_for_each_safe(entry, tmp_entry, &ring->previous, link) {
		entry_destroy(entry);
	}
}

void wlr_damage_ring_set_bounds(struct wlr_damage_ring *ring,
		int32_t width, int32_t height) {
	if (width == 0 || height == 0) {
		width = INT_MAX;
		height = INT_MAX;
	}

	if (ring->width == width && ring->height == height) {
		return;
	}

	ring->width = width;
	ring->height = height;
	wlr_damage_ring_add_whole(ring);
}

bool wlr_damage_ring_add(struct wlr_damage_ring *ring,
		const pixman_region32_t *damage) {
	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage,
		0, 0, ring->width, ring->height);
	bool intersects = pixman_region32_not_empty(&clipped);
	if (intersects) {
		pixman_region32_union(&ring->current, &ring->current, &clipped);
	}
	pixman_region32_fini(&clipped);
	return intersects;
}

bool wlr_damage_ring_add_box(struct wlr_damage_ring *ring,
		const struct wlr_box *box) {
	struct wlr_box clipped = {
		.x = 0,
		.y = 0,
		.width = ring->width,
		.height = ring->height,
	};
	if (wlr_box_intersection(&clipped, &clipped, box)) {
		pixman_region32_union_rect(&ring->current,
			&ring->current, clipped.x, clipped.y,
			clipped.width, clipped.height);
		return true;
	}
	return false;
}

void wlr_damage_ring_add_whole(struct wlr_damage_ring *ring) {
	pixman_region32_union_rect(&ring->current,
		&ring->current, 0, 0, ring->width, ring->height);
}

void wlr_damage_ring_rotate(struct wlr_damage_ring *ring) {
	struct wlr_damage_ring_entry *last =
		wl_container_of(ring->previous.prev, last, link);
	wl_list_remove(&last->link);
	wl_list_insert(&ring->previous, &last->link);

	pixman_region32_copy(&last->damage, &ring->current);
	pixman_region32_clear(&ring->current);
}

void wlr_damage_ring_get_buffer_damage(struct wlr_damage_ring *ring,
		int buffer_age, pixman_region32_t *damage) {
	pixman_region32_copy(damage, &ring->current);

	// Accumulate damage from old buffers
	struct wlr_damage_ring_entry *entry;
	wl_list_for_each(entry, &ring->previous, link) {
		if (--buffer_age <= 0) {
			break;
		}

		pixman_region32_union(damage, damage, &entry->damage);
	}

	// if our buffer age is older than anything we are keeping track of, increase
	// the size
	if (buffer_age > 0) {
		pixman_region32_clear(damage);
		pixman_region32_union_rect(damage, damage,
			0, 0, ring->width, ring->height);

		struct wlr_damage_ring_entry *entry = calloc(1, sizeof(*entry));
		if (entry) {
			pixman_region32_init(&entry->damage);
			wl_list_insert(ring->previous.prev, &entry->link);
		}
	}

	// Check the number of rectangles
	int n_rects = pixman_region32_n_rects(damage);
	if (n_rects > WLR_DAMAGE_RING_MAX_RECTS) {
		pixman_box32_t *extents = pixman_region32_extents(damage);
		pixman_region32_union_rect(damage, damage,
			extents->x1, extents->y1,
			extents->x2 - extents->x1,
			extents->y2 - extents->y1);
	}
}

static void entry_squash_damage(struct wlr_damage_ring_entry *entry) {
	pixman_region32_t *prev;
	if (entry->link.prev == &entry->ring->previous) {
		// this entry is the first in the list
		prev = &entry->ring->current;
	} else {
		struct wlr_damage_ring_entry *last =
			wl_container_of(entry->link.prev, last, link);
		prev = &last->damage;
	}

	pixman_region32_union(prev, prev, &entry->damage);
}

static void handle_buffer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_damage_ring_entry *entry =
		wl_container_of(listener, entry, buffer_destroy);
	entry_squash_damage(entry);
	entry_destroy(entry);
}

void wlr_damage_ring_damage_for_buffer(struct wlr_damage_ring *ring,
		struct wlr_buffer *buffer, pixman_region32_t *damage) {
	pixman_region32_copy(damage, &ring->current);

	struct wlr_damage_ring_entry *entry;
	wl_list_for_each(entry, &ring->previous, link) {
		if (entry->buffer != buffer) {
			pixman_region32_union(damage, damage, &entry->damage);
			continue;
		}

		// Check the number of rectangles
		int n_rects = pixman_region32_n_rects(damage);
		if (n_rects > WLR_DAMAGE_RING_MAX_RECTS) {
			pixman_box32_t *extents = pixman_region32_extents(damage);
			pixman_region32_union_rect(damage, damage,
				extents->x1, extents->y1,
				extents->x2 - extents->x1,
				extents->y2 - extents->y1);
		}

		// rotate
		entry_squash_damage(entry);
		pixman_region32_copy(&entry->damage, &ring->current);
		pixman_region32_clear(&ring->current);

		wl_list_remove(&entry->link);
		wl_list_insert(&ring->previous, &entry->link);
		return;
	}

	pixman_region32_clear(damage);
	pixman_region32_union_rect(damage, damage,
		0, 0, ring->width, ring->height);

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return;
	}

	pixman_region32_init(&entry->damage);
	pixman_region32_copy(&entry->damage, &ring->current);
	pixman_region32_clear(&ring->current);

	wl_list_insert(&ring->previous, &entry->link);
	entry->buffer = buffer;
	entry->ring = ring;

	entry->buffer_destroy.notify = handle_buffer_destroy;
	wl_signal_add(&buffer->events.destroy, &entry->buffer_destroy);
}
