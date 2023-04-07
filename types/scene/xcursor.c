#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/interfaces/wlr_buffer.h>

struct xcursor_buffer {
	struct wlr_buffer base;
	struct wlr_xcursor_image image;
};

static void xcursor_buffer_handle_destroy(struct wlr_buffer *wlr_buffer) {
	struct xcursor_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	free(buffer);
}

static bool xcursor_buffer_handle_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct xcursor_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = buffer->image.buffer;
	*stride = buffer->image.width * 4;
	*format = DRM_FORMAT_ARGB8888;
	return true;
}

static void xcursor_buffer_handle_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// This space is intentionally left blank
}

static const struct wlr_buffer_impl xcursor_buffer_impl = {
	.destroy = xcursor_buffer_handle_destroy,
	.begin_data_ptr_access = xcursor_buffer_handle_begin_data_ptr_access,
	.end_data_ptr_access = xcursor_buffer_handle_end_data_ptr_access,
};

struct wlr_scene_xcursor {
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *buffer;
	struct wlr_xcursor_manager *manager;
	char *name;
	float scale;

	struct wl_list outputs; // struct wlr_scene_xcursor_output.link

	struct wl_listener output_enter;
	struct wl_listener output_leave;
	struct wl_listener tree_destroy;
};

struct wlr_scene_xcursor_output {
	struct wlr_scene_xcursor *xcursor;
	struct wlr_output *output;
	struct wl_list link;
	struct wl_listener commit;
};

static void scene_xcursor_update_cursor(struct wlr_scene_xcursor *cursor) {
	float scale = 1;
	struct wlr_scene_xcursor_output *output;
	wl_list_for_each(output, &cursor->outputs, link) {
		if (output->output->scale > scale) {
			output->output->scale = scale;
		}
	}

	if (scale == cursor->scale) {
		return;
	}

	cursor->scale = scale;

	if (!wlr_xcursor_manager_load(cursor->manager, scale)) {
		return;
	}

	struct wlr_xcursor *xcursor =
		wlr_xcursor_manager_get_xcursor(cursor->manager, cursor->name, scale);
	if (!xcursor) {
		return;
	}

	struct wlr_xcursor_image *image = xcursor->images[0];

	struct xcursor_buffer *xcursor_buffer = calloc(1, sizeof(*xcursor_buffer));
	if (!xcursor_buffer) {
		return;
	}

	wlr_buffer_init(&xcursor_buffer->base, &xcursor_buffer_impl, image->width, image->height);
	xcursor_buffer->image = *image;

	wlr_scene_node_set_position(&cursor->buffer->node,
		-image->hotspot_x, -image->hotspot_y);
	wlr_scene_buffer_set_dest_size(cursor->buffer,
		image->width / scale, image->height / scale);
	wlr_scene_buffer_set_buffer(cursor->buffer, &xcursor_buffer->base);

	wlr_buffer_drop(&xcursor_buffer->base);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct wlr_scene_xcursor_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = data;

	if (event->committed & WLR_OUTPUT_STATE_SCALE) {
		scene_xcursor_update_cursor(output->xcursor);
	}
}

static void handle_output_enter(struct wl_listener *listener, void *data) {
	struct wlr_scene_xcursor *cursor = wl_container_of(listener, cursor, output_enter);
	struct wlr_scene_output *scene_output = data;

	struct wlr_scene_xcursor_output *output = calloc(1, sizeof(*output));
	if (!output) {
		return;
	}

	output->output = scene_output->output;
	output->xcursor = cursor;

	output->commit.notify = handle_output_commit;
	wl_signal_add(&output->output->events.commit, &output->commit);

	wl_list_insert(&cursor->outputs, &output->link);
	scene_xcursor_update_cursor(cursor);
}

static void output_destroy(struct wlr_scene_xcursor_output *output) {
	wl_list_remove(&output->link);
	wl_list_remove(&output->commit.link);
	free(output);
}

static void handle_output_leave(struct wl_listener *listener, void *data) {
	struct wlr_scene_xcursor *cursor = wl_container_of(listener, cursor, output_leave);
	struct wlr_scene_output *scene_output = data;

	struct wlr_scene_xcursor_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &cursor->outputs, link) {
		if (output->output == scene_output->output) {
			output_destroy(output);
			scene_xcursor_update_cursor(cursor);
			return;
		}
	}
}

static void scene_xcursor_handle_tree_destroy(struct wl_listener *listener, void *data) {
	struct wlr_scene_xcursor *cursor = wl_container_of(listener, cursor, tree_destroy);

	struct wlr_scene_xcursor_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &cursor->outputs, link) {
		output_destroy(output);
	}

	wl_list_remove(&cursor->tree_destroy.link);
	free(cursor->name);
	free(cursor);
}

struct wlr_scene_tree *wlr_scene_xcursor_create(struct wlr_scene_tree *parent,
		struct wlr_xcursor_manager *manager, const char *name) {
	struct wlr_scene_xcursor *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		return NULL;
	}

	cursor->tree = wlr_scene_tree_create(parent);
	if (!cursor->tree) {
		free(cursor);
		return NULL;
	}

	cursor->buffer = wlr_scene_buffer_create(cursor->tree, NULL);
	if (!cursor->buffer) {
		wlr_scene_node_destroy(&cursor->tree->node);
		free(cursor);
		return NULL;
	}

	cursor->name = strdup(name);
	if (!cursor->name) {
		wlr_scene_node_destroy(&cursor->tree->node);
		free(cursor);
		return NULL;
	}

	wl_list_init(&cursor->outputs);
	cursor->manager = manager;

	cursor->output_enter.notify = handle_output_enter;
	wl_signal_add(&cursor->buffer->events.output_enter, &cursor->output_enter);
	cursor->output_leave.notify = handle_output_leave;
	wl_signal_add(&cursor->buffer->events.output_leave, &cursor->output_leave);
	cursor->tree_destroy.notify = scene_xcursor_handle_tree_destroy;
	wl_signal_add(&cursor->tree->node.events.destroy, &cursor->tree_destroy);

	scene_xcursor_update_cursor(cursor);

	return cursor->tree;
}
