#include <assert.h>

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_surface_damage_tracker.h>
#include "wlr/util/log.h"

struct wlr_surface_damage_tracker_subsurface {
	struct wlr_surface_damage_tracker_surface base;
	struct wlr_subsurface *subsurface;
	int32_t x, y;
	bool has_pending_commit;
	struct wl_list link;
	struct wl_listener destroy;
};

// If desynced, x and y are the surface position in the tree
static bool surface_synced(struct wlr_surface *surface, int *x, int *y) {
	*x = 0;
	*y = 0;
	struct wlr_subsurface *subsurface;
	while ((subsurface = wlr_subsurface_try_from_wlr_surface(surface)) != NULL) {
		if (subsurface->synchronized) {
			return true;
		}
		surface = subsurface->parent;
		*x += subsurface->current.x;
		*y += subsurface->current.y;
	}
	return false;
}

static void tracker_surface_finish(struct wlr_surface_damage_tracker_surface *tracker_surface);

static void tracker_subsurface_destroy(
		struct wlr_surface_damage_tracker_subsurface *tracker_subsurface) {
	tracker_surface_finish(&tracker_subsurface->base);
	wl_list_remove(&tracker_subsurface->link);
	wl_list_remove(&tracker_subsurface->destroy.link);
	free(tracker_subsurface);
}

static void tracker_surface_finish(struct wlr_surface_damage_tracker_surface *tracker_surface) {
	wl_list_remove(&tracker_surface->map.link);
	wl_list_remove(&tracker_surface->unmap.link);
	wl_list_remove(&tracker_surface->commit.link);
	wl_list_remove(&tracker_surface->new_subsurface.link);

	struct wlr_surface_damage_tracker_subsurface *tracker_subsurface, *tmp;
	wl_list_for_each_safe(tracker_subsurface, tmp, &tracker_surface->subsurfaces, link) {
		tracker_subsurface_destroy(tracker_subsurface);
	}
}

static void add_tree(struct wlr_surface_damage_tracker_surface *tracker_surface,
		int32_t x, int32_t y, pixman_region32_t *region) {
	pixman_region32_union_rect(region, region, x, y,
		tracker_surface->width, tracker_surface->height);

	struct wlr_surface_damage_tracker_subsurface *child;
	wl_list_for_each(child, &tracker_surface->subsurfaces, link) {
		struct wlr_subsurface *subsurface = child->subsurface;
		child->base.mapped = subsurface->surface->mapped;
		if (!subsurface->surface->mapped) {
			continue;
		}
		add_tree(&child->base, x + child->x, y + child->y, region);
	}
}

static void process_map_update(struct wlr_surface_damage_tracker_surface *tracker_surface, int x, int y) {
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	add_tree(tracker_surface, x, y, &damage);

	struct wlr_surface_damage_tracker_damage_event event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&tracker_surface->tracker->events.damage, &event);

	pixman_region32_fini(&damage);
}

static void tracker_surface_handle_map(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker_surface *tracker_surface =
		wl_container_of(listener, tracker_surface, map);

	int x, y;
	if (surface_synced(tracker_surface->surface, &x, &y)) {
		// Will be handled later
		return;
	}
	assert(!tracker_surface->mapped);
	tracker_surface->mapped = true;

	process_map_update(tracker_surface, x, y);
}

static void tracker_surface_handle_unmap(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker_surface *tracker_surface =
		wl_container_of(listener, tracker_surface, unmap);
	if (!tracker_surface->mapped) {
		// Already handled
		return;
	}
	tracker_surface->mapped = false;

	int x = 0, y = 0;
	struct wlr_surface *surface = tracker_surface->surface;
	struct wlr_subsurface *subsurface;
	while ((subsurface = wlr_subsurface_try_from_wlr_surface(surface)) != NULL) {
		x += subsurface->current.x;
		y += subsurface->current.y;
		surface = subsurface->parent;
	}

	process_map_update(tracker_surface, x, y);
}

static void update_surface(struct wlr_surface_damage_tracker_surface *tracker_surface) {
	struct wlr_surface *surface = tracker_surface->surface;
	tracker_surface->width = surface->current.width;
	tracker_surface->height = surface->current.height;
	if (surface->current.viewport.has_src) {
		tracker_surface->viewport_src = surface->current.viewport.src;
	}
}

static void add_old_tree_and_update(
		struct wlr_surface_damage_tracker_subsurface *tracker_subsurface, int32_t x, int32_t y,
		pixman_region32_t *region) {
	struct wlr_surface_damage_tracker_surface *tracker_surface = &tracker_subsurface->base;
	pixman_region32_union_rect(region, region, x, y,
		tracker_surface->width, tracker_surface->height);

	struct wlr_surface_damage_tracker_subsurface *child;
	wl_list_for_each(child, &tracker_surface->subsurfaces, link) {
		struct wlr_subsurface *subsurface = child->subsurface;
		if (!subsurface->surface->mapped) {
			continue;
		}
		add_old_tree_and_update(child, x + child->x, y + child->y, region);
	}

	tracker_subsurface->x = tracker_subsurface->subsurface->current.x;
	tracker_subsurface->y = tracker_subsurface->subsurface->current.y;
	update_surface(tracker_surface);
}

static void add_committed(struct wlr_surface_damage_tracker_surface *tracker_surface,
		int32_t x, int32_t y, pixman_region32_t *damage) {
	struct wlr_surface *surface = tracker_surface->surface;
	if (tracker_surface->width != surface->current.width ||
			tracker_surface->height != surface->current.height) {
		pixman_region32_union_rect(damage, damage, x, y,
				tracker_surface->width, tracker_surface->height);
		pixman_region32_union_rect(damage, damage, x, y,
				surface->current.width, surface->current.height);
	} else if (surface->current.viewport.has_src &&
			!wlr_fbox_equal(&surface->current.viewport.src, &tracker_surface->viewport_src)) {
		pixman_region32_union_rect(damage, damage, x, y,
				surface->current.width, surface->current.height);
	} else {
		pixman_region32_t surface_damage;
		pixman_region32_init(&surface_damage);
		wlr_surface_get_effective_damage(surface, &surface_damage);
		pixman_region32_translate(&surface_damage, x, y);
		pixman_region32_union(damage, damage, &surface_damage);
		pixman_region32_fini(&surface_damage);
	}

	update_surface(tracker_surface);

	struct wlr_surface_damage_tracker_subsurface *child;
	wl_list_for_each(child, &tracker_surface->subsurfaces, link) {
		struct wlr_subsurface *subsurface = child->subsurface;

		bool prev_mapped = child->base.mapped;
		child->base.mapped = subsurface->surface->mapped;

		bool has_pending_commit = child->has_pending_commit;
		child->has_pending_commit = false;

		if (!subsurface->surface->mapped) {
			continue;
		}

		if (subsurface->current.committed & WLR_SUBSURFACE_PARENT_STATE_POSITION) {
			add_old_tree_and_update(child, x + child->x, y + child->y, damage);
			add_tree(&child->base, x + child->x, y + child->y, damage);
			continue;
		}

		if (!prev_mapped || (subsurface->current.committed & WLR_SUBSURFACE_PARENT_STATE_ORDER)) {
			add_tree(&child->base, x + child->x, y + child->y, damage);
			continue;
		}

		if (!has_pending_commit) {
			continue;
		}

		add_committed(&child->base, x + child->x, y + child->y, damage);
	}
}

static void tracker_surface_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker_surface *tracker_surface =
		wl_container_of(listener, tracker_surface, commit);
	if (!tracker_surface->mapped) {
		return;
	}

	int x, y;
	if (surface_synced(tracker_surface->surface, &x, &y)) {
		struct wlr_surface_damage_tracker_subsurface *tracker_subsurface =
			wl_container_of(tracker_surface, tracker_subsurface, base);
		tracker_subsurface->has_pending_commit = true;
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	add_committed(tracker_surface, x, y, &damage);

	struct wlr_surface_damage_tracker_damage_event event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&tracker_surface->tracker->events.damage, &event);

	pixman_region32_fini(&damage);
}

static void tracker_subsurface_handle_destroy(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker_subsurface *tracker_subsurface =
		wl_container_of(listener, tracker_subsurface, destroy);
	tracker_subsurface_destroy(tracker_subsurface);
}

static void tracker_surface_init(struct wlr_surface_damage_tracker_surface *tracker_surface,
	struct wlr_surface_damage_tracker *tracker, struct wlr_surface *surface);

static void tracker_subsurface_create(struct wlr_subsurface *subsurface,
		struct wlr_surface_damage_tracker_surface *parent) {
	struct wlr_surface_damage_tracker_subsurface *tracker_subsurface =
		calloc(1, sizeof(*tracker_subsurface));
	if (tracker_subsurface == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return;
	}

	tracker_surface_init(&tracker_subsurface->base, parent->tracker, subsurface->surface);
	tracker_subsurface->subsurface = subsurface;
	tracker_subsurface->x = subsurface->current.x;
	tracker_subsurface->y = subsurface->current.y;
	wl_list_insert(&parent->subsurfaces, &tracker_subsurface->link);

	tracker_subsurface->destroy.notify = tracker_subsurface_handle_destroy;
	wl_signal_add(&subsurface->events.destroy, &tracker_subsurface->destroy);
}

static void tracker_surface_handle_new_subsurface(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker_surface *tracker_surface =
		wl_container_of(listener, tracker_surface, new_subsurface);
	struct wlr_subsurface *subsurface = data;
	tracker_subsurface_create(subsurface, tracker_surface);
}

static void tracker_surface_init(struct wlr_surface_damage_tracker_surface *tracker_surface,
		struct wlr_surface_damage_tracker *tracker, struct wlr_surface *surface) {
	tracker_surface->tracker = tracker;
	tracker_surface->surface = surface;

	tracker_surface->mapped = surface->mapped;
	tracker_surface->width = surface->current.width;
	tracker_surface->height = surface->current.height;
	if (surface->current.viewport.has_src) {
		tracker_surface->viewport_src = surface->current.viewport.src;
	}

	wl_list_init(&tracker_surface->subsurfaces);

	tracker_surface->map.notify = tracker_surface_handle_map;
	wl_signal_add(&surface->events.map, &tracker_surface->map);
	tracker_surface->unmap.notify = tracker_surface_handle_unmap;
	wl_signal_add(&surface->events.unmap, &tracker_surface->unmap);
	tracker_surface->commit.notify = tracker_surface_handle_commit;
	wl_signal_add(&surface->events.commit, &tracker_surface->commit);
	tracker_surface->new_subsurface.notify = tracker_surface_handle_new_subsurface;
	wl_signal_add(&surface->events.new_subsurface, &tracker_surface->new_subsurface);

	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below, current.link) {
		tracker_subsurface_create(subsurface, tracker_surface);
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above, current.link) {
		tracker_subsurface_create(subsurface, tracker_surface);
	}
}

static void tracker_detach_surface(struct wlr_surface_damage_tracker *tracker) {
	tracker_surface_finish(&tracker->surface);
	wl_list_remove(&tracker->surface_destroy.link);
	tracker->has_surface = false;
}

static void tracker_handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_surface_damage_tracker *tracker =
		wl_container_of(listener, tracker, surface_destroy);
	tracker_detach_surface(tracker);
}

struct wlr_surface_damage_tracker *wlr_surface_damage_tracker_create(struct wlr_surface *surface) {
	struct wlr_surface_damage_tracker *tracker = calloc(1, sizeof(*tracker));
	if (tracker == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return false;
	}

	wl_signal_init(&tracker->events.damage);

	tracker_surface_init(&tracker->surface, tracker, surface);
	tracker->surface_destroy.notify = tracker_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &tracker->surface_destroy);

	tracker->has_surface = true;

	return tracker;
}

void wlr_surface_damage_tracker_destroy(struct wlr_surface_damage_tracker *tracker) {
	if (tracker == NULL) {
		return;
	}
	if (tracker->has_surface) {
		tracker_detach_surface(tracker);
	}
	free(tracker);
}
