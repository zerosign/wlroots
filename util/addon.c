#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

static ptrdiff_t compare(struct wlr_addon *addon, const void *owner,
		const struct wlr_addon_interface *impl) {
	ptrdiff_t owner_diff = (ptrdiff_t)addon->owner - (ptrdiff_t)owner;
	if (owner_diff != 0) {
		return owner_diff;
	}
	return (ptrdiff_t)addon->impl - (ptrdiff_t)impl;
}

// A (addon (child)) => A (child)
static void replace(struct wlr_addon *addon, struct wlr_addon *child) {
	if (addon->parent == NULL) {
		addon->set->root = child;
	} else if (addon->parent->left == addon) {
		addon->parent->left = child;
	} else { // addon->parent->right == addon
		addon->parent->right = child;
	}
	if (child != NULL) {
		child->parent = addon->parent;
	}
}

// parent (A, addon (B, C)) => addon (parent (A, B), C)
static struct wlr_addon *rotate_left(struct wlr_addon *parent,
		struct wlr_addon *addon) {
	parent->right = addon->left;
	if (addon->left != NULL) {
		addon->left->parent = parent;
	}
	addon->left = parent;
	parent->parent = addon;
	if (addon->balance == 0) {
		addon->balance = -1;
		parent->balance = 1;
	} else {
		addon->balance = 0;
		parent->balance = 0;
	}
	return addon;
}

// parent (addon (A, B), C) => addon (A, parent (B, C))
static struct wlr_addon *rotate_right(struct wlr_addon *parent,
		struct wlr_addon *addon) {
	parent->left = addon->right;
	if (addon->right != NULL) {
		addon->right->parent = parent;
	}
	addon->right = parent;
	parent->parent = addon;
	if (addon->balance == 0) {
		addon->balance = -1;
		parent->balance = 1;
	} else {
		addon->balance = 0;
		parent->balance = 0;
	}
	return addon;
}

// parent (addon (A, mid (B, C)), D) => mid (addon (A, B), parent (C, D))
static struct wlr_addon *rotate_leftright(struct wlr_addon *parent,
		struct wlr_addon *addon) {
	struct wlr_addon *mid = addon->right;
	addon->right = mid->left;
	if (mid->left != NULL) {
		mid->left->parent = addon;
	}
	mid->left = addon;
	addon->parent = mid;
	parent->left = mid->right;
	if (mid->right != NULL) {
		mid->right->parent = parent;
	}
	mid->right = parent;
	parent->parent = mid;
	parent->balance = mid->balance > 0 ? -1 : 0;
	addon->balance = mid->balance < 0 ? 1 : 0;
	return mid;
}

// parent (A, addon (mid (B, C), D)) => mid (parent (A, B), addon (C, D))
static struct wlr_addon *rotate_rightleft(struct wlr_addon *parent,
		struct wlr_addon *addon) {
	struct wlr_addon *mid = addon->left;
	addon->left = mid->right;
	if (mid->right != NULL) {
		mid->right->parent = addon;
	}
	mid->right = addon;
	addon->parent = mid;
	parent->right = mid->left;
	if (mid->left != NULL) {
		mid->left->parent = parent;
	}
	mid->left = parent;
	parent->parent = mid;
	parent->balance = mid->balance > 0 ? -1 : 0;
	addon->balance = mid->balance < 0 ? 1 : 0;
	return mid;
}

void wlr_addon_set_init(struct wlr_addon_set *set) {
	memset(set, 0, sizeof(*set));
}

void wlr_addon_set_finish(struct wlr_addon_set *set) {
	while (set->root != NULL) {
		set->root->impl->destroy(set->root);
	}

	wl_list_for_each(addon, &set->addons, link) {
		wlr_log(WLR_ERROR, "Dangling addon: %s", addon->impl->name);
	}

	assert(wl_list_empty(&set->addons));
}

void wlr_addon_init(struct wlr_addon *addon, struct wlr_addon_set *set,
		const void *owner, const struct wlr_addon_interface *impl) {
	assert(impl);
	memset(addon, 0, sizeof(*addon));

	struct wlr_addon *parent = NULL;
	struct wlr_addon **ptr = &set->root;
	while (*ptr != NULL) {
		parent = *ptr;
		ptrdiff_t diff = compare(*ptr, owner, impl);
		if (diff < 0) {
			ptr = &(*ptr)->left;
		} else if (diff > 0) {
			ptr = &(*ptr)->right;
		} else {
			assert(0 && "Can't have two addons with the same owner and impl");
			return;
		}
	}

	addon->set = set;
	addon->owner = owner;
	addon->impl = impl;
	addon->parent = parent;
	*ptr = addon;

	// Rebalance
	for (; parent != NULL; parent = parent->parent) {
		struct wlr_addon *grandparent = parent->parent;
		if (parent->left == addon) {
			if (parent->balance > 0) {
				parent->balance = 0;
				break;
			} else if (parent->balance == 0) {
				parent->balance = -1;
				addon = parent;
				continue;
			}
			if (addon->balance > 0) {
				addon = rotate_leftright(parent, addon);
			} else {
				addon = rotate_right(parent, addon);
			}
		} else { // parent->right == addon
			if (parent->balance < 0) {
				parent->balance = 0;
				break;
			} else if (parent->balance == 0) {
				parent->balance = 1;
				addon = parent;
				continue;
			}
			if (addon->balance < 0) {
				addon = rotate_rightleft(parent, addon);
			} else {
				addon = rotate_left(parent, addon);
			}
		} 
		// Here, addon is the root of the rotated subtree
		addon->parent = grandparent;
		if (grandparent != NULL) {
			if (grandparent->left == parent) {
				grandparent->left = addon;
			} else { // grandparent->right == parent
				grandparent->right = addon;
			}
		} else {
			set->root = addon;
		}
		break;
	}
}

void wlr_addon_finish(struct wlr_addon *addon) {
	if (addon->left == NULL) {
		replace(addon, addon->right);
	} else if (addon->right == NULL) {
		replace(addon, addon->left);
	} else {
		struct wlr_addon *successor = addon->right;
		while (successor->left != NULL) {
			successor = successor->left;
		}
		if (successor->parent != addon) {
			replace(successor, successor->right);
			successor->right = addon->right;
			successor->right->parent = successor;
		}
		replace(addon, successor);
		successor->left = addon->left;
		successor->left->parent = successor;
	}

	// Rebalance
	for (struct wlr_addon *parent = addon->parent;
			parent != NULL; parent = parent->parent) {
		int balance = 0;
		struct wlr_addon *grandparent = parent->parent;
		if (parent->left == addon) {
			if (parent->balance < 0) {
				parent->balance = 0;
				addon = parent;
				continue;
			} else if (parent->balance == 0) {
				parent->balance = 1;
				break;
			}
			balance = parent->right->balance;
			if (balance < 0) {
				addon = rotate_rightleft(parent, addon);
			} else {
				addon = rotate_left(parent, addon);
			}
		} else { // parent->right == addon
			if (parent->balance < 0) {
				parent->balance = 0;
				addon = parent;
				continue;
			} else if (parent->balance == 0) {
				parent->balance = -1;
				break;
			}
			balance = parent->left->balance;
			if (balance > 0) {
				addon = rotate_leftright(parent, addon);
			} else {
				addon = rotate_right(parent, addon);
			}
		}
		// Here, addon is the root of the rotated subtree
		addon->parent = grandparent;
		if (grandparent != NULL) {
			if (grandparent->left == parent) {
				grandparent->left = addon;
			} else { // grandparent->right == parent
				grandparent->right = addon;
			}
		} else {
			addon->set->root = addon;
		}
		if (balance == 0) {
			break;
		}
	}
}

struct wlr_addon *wlr_addon_find(struct wlr_addon_set *set, const void *owner,
		const struct wlr_addon_interface *impl) {
	struct wlr_addon *addon = set->root;
	while (addon != NULL) {
		ptrdiff_t diff = compare(addon, owner, impl);
		if (diff < 0) {
			addon = addon->left;
		} else if (diff > 0) {
			addon = addon->right;
		} else {
			return addon;
		}
	}
	return NULL;
}
