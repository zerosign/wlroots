/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_UTIL_ADDON_H
#define WLR_UTIL_ADDON_H

#include <stdint.h>

struct wlr_addon_set {
	// private state
	struct wlr_addon *root;
};

struct wlr_addon;

struct wlr_addon_interface {
	const char *name;
	void (*destroy)(struct wlr_addon *addon);
};

struct wlr_addon {
	// private state
	struct wlr_addon_set *set;
	const void *owner;
	const struct wlr_addon_interface *impl;
	struct wlr_addon *parent; // NULL if it's set->root
	struct wlr_addon *left;
	struct wlr_addon *right;
	int8_t balance;
};

void wlr_addon_set_init(struct wlr_addon_set *set);
void wlr_addon_set_finish(struct wlr_addon_set *set);

void wlr_addon_init(struct wlr_addon *addon, struct wlr_addon_set *set,
	const void *owner, const struct wlr_addon_interface *impl);
void wlr_addon_finish(struct wlr_addon *addon);

struct wlr_addon *wlr_addon_find(struct wlr_addon_set *set, const void *owner,
	const struct wlr_addon_interface *impl);

#endif
