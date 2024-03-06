#ifndef TYPES_WLR_SCENE_H
#define TYPES_WLR_SCENE_H

#include <wlr/types/wlr_scene.h>

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node);

void scene_surface_set_clip(struct wlr_scene_surface *surface, struct wlr_box *clip);

void scene_buffer_set_buffer_and_texture(struct wlr_scene_buffer *scene_buffer,
	struct wlr_buffer *buffer, struct wlr_texture *texture,
	const pixman_region32_t *damage);

#endif
