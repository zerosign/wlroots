#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_mapper.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct sample_state {
	struct wl_display *display;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wlr_cursor *cursor;
	float default_color[4];
	float clear_color[4];
	struct wlr_output_layout *layout;
	struct wlr_input_mapper *input_mapper;

	struct wl_listener new_output;
	struct wl_listener new_input;
};

struct touch_point {
	int32_t touch_id;
	double x, y;
	struct wl_list link;
};

struct sample_output {
	struct sample_state *state;
	struct wlr_output *output;
	struct wl_listener frame;
	struct wl_listener destroy;
};

struct sample_keyboard {
	struct sample_state *state;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct sample_pointer {
	struct sample_state *state;
	struct wlr_pointer *wlr_pointer;
	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener frame;
	struct wl_listener destroy;
};

struct sample_touch {
	struct sample_state *state;
	struct wlr_touch *wlr_touch;
	struct wl_list points;
	struct wl_listener motion;
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener destroy;
};

struct sample_tablet {
	struct sample_state *state;
	struct wlr_tablet *wlr_tablet;
	double x, y;
	struct wl_listener axis;
	struct wl_listener destroy;
};

static void warp_absolute(struct sample_state *state,
		struct wlr_input_device *device, double x, double y) {
	double lx, ly;
	wlr_input_mapper_absolute_to_layout(state->input_mapper,
		device, x, y, &lx, &ly);
	wlr_cursor_warp(state->cursor, lx, ly);
}

static void warp_to_touch(struct sample_touch *touch) {
	if (wl_list_empty(&touch->points)) {
		return;
	}

	double x = 0, y = 0;
	size_t n = 0;
	struct touch_point *point;
	wl_list_for_each(point, &touch->points, link) {
		x += point->x;
		y += point->y;
		n++;
	}
	x /= n;
	y /= n;
	warp_absolute(touch->state, &touch->wlr_touch->base, x, y);
}

static void output_frame_notify(struct wl_listener *listener, void *data) {
	struct sample_output *sample_output = wl_container_of(listener, sample_output, frame);
	struct sample_state *state = sample_output->state;
	struct wlr_output *wlr_output = sample_output->output;
	struct wlr_renderer *renderer = state->renderer;
	assert(renderer);

	wlr_output_attach_render(wlr_output, NULL);
	wlr_renderer_begin(renderer, wlr_output->width, wlr_output->height);
	wlr_renderer_clear(renderer, state->clear_color);
	wlr_output_render_software_cursors(wlr_output, NULL);
	wlr_renderer_end(renderer);
	wlr_output_commit(wlr_output);
}

static void pointer_motion_notify(struct wl_listener *listener, void *data) {
	struct sample_pointer *pointer = wl_container_of(listener, pointer, motion);
	struct wlr_pointer_motion_event *event = data;

	struct sample_state *sample = pointer->state;

	wlr_cursor_warp(sample->cursor, sample->cursor->x + event->delta_x,
		sample->cursor->y + event->delta_y);
}

static void pointer_motion_absolute_notify(struct wl_listener *listener, void *data) {
	struct sample_pointer *pointer =
		wl_container_of(listener, pointer, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;

	warp_absolute(pointer->state, &pointer->wlr_pointer->base, event->x, event->y);
}

static void pointer_button_notify(struct wl_listener *listener, void *data) {
	struct sample_pointer *pointer = wl_container_of(listener, pointer, button);
	struct wlr_pointer_button_event *event = data;

	struct sample_state *sample = pointer->state;

	float (*color)[4];
	if (event->state == WLR_BUTTON_RELEASED) {
		color = &sample->default_color;
		memcpy(&sample->clear_color, color, sizeof(*color));
	} else {
		float red[4] = { 0.25f, 0.25f, 0.25f, 1 };
		red[event->button % 3] = 1;
		color = &red;
		memcpy(&sample->clear_color, color, sizeof(*color));
	}
}

static void pointer_axis_notify(struct wl_listener *listener, void *data) {
	struct sample_pointer *pointer = wl_container_of(listener, pointer, axis);
	struct wlr_pointer_axis_event *event = data;

	struct sample_state *sample = pointer->state;

	for (size_t i = 0; i < 3; ++i) {
		sample->default_color[i] += event->delta > 0 ? -0.05f : 0.05f;
		if (sample->default_color[i] > 1.0f) {
			sample->default_color[i] = 1.0f;
		}
		if (sample->default_color[i] < 0.0f) {
			sample->default_color[i] = 0.0f;
		}
	}

	memcpy(&sample->clear_color, &sample->default_color,
			sizeof(sample->clear_color));
}

static void pointer_destroy_notify(struct wl_listener *listener, void *data) {
	struct sample_pointer *pointer = wl_container_of(listener, pointer, destroy);
	wl_list_remove(&pointer->destroy.link);
	wl_list_remove(&pointer->motion.link);
	wl_list_remove(&pointer->motion_absolute.link);
	wl_list_remove(&pointer->button.link);
	wl_list_remove(&pointer->axis.link);
	free(pointer);
}

static void touch_up_notify(struct wl_listener *listener, void *data) {
	struct sample_touch *touch = wl_container_of(listener, touch, up);
	struct wlr_touch_up_event *event = data;

	struct touch_point *point, *tmp;
	wl_list_for_each_safe(point, tmp, &touch->points, link) {
		if (point->touch_id == event->touch_id) {
			wl_list_remove(&point->link);
			break;
		}
	}

	warp_to_touch(touch);
}

static void touch_down_notify(struct wl_listener *listener, void *data) {
	struct sample_touch *touch = wl_container_of(listener, touch, down);
	struct wlr_touch_down_event *event = data;

	struct touch_point *point = calloc(1, sizeof(struct touch_point));
	point->touch_id = event->touch_id;
	point->x = event->x;
	point->y = event->y;
	wl_list_insert(&touch->points, &point->link);

	warp_to_touch(touch);
}

static void touch_motion_notify(struct wl_listener *listener, void *data) {
	struct sample_touch *touch = wl_container_of(listener, touch, motion);
	struct wlr_touch_motion_event *event = data;

	struct touch_point *point;
	wl_list_for_each(point, &touch->points, link) {
		if (point->touch_id == event->touch_id) {
			point->x = event->x;
			point->y = event->y;
			break;
		}
	}

	warp_to_touch(touch);
}

static void touch_destroy_notify(struct wl_listener *listener, void *data) {
	struct sample_touch *touch = wl_container_of(listener, touch, destroy);
	wl_list_remove(&touch->destroy.link);
	wl_list_remove(&touch->up.link);
	wl_list_remove(&touch->down.link);
	wl_list_remove(&touch->motion.link);
	free(touch);
}

static void tablet_axis_notify(struct wl_listener *listener, void *data) {
	struct sample_tablet *tablet = wl_container_of(listener, tablet, axis);
	struct wlr_tablet_tool_axis_event *event = data;

	if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_X) != 0) {
		tablet->x = event->x;
	}
	if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) != 0) {
		tablet->y = event->y;
	}
	warp_absolute(tablet->state, &tablet->wlr_tablet->base, tablet->x, tablet->y);
}

static void tablet_destroy_notify(struct wl_listener *listener, void *data) {
	struct sample_tablet *tablet = wl_container_of(listener, tablet, destroy);
	wl_list_remove(&tablet->destroy.link);
	wl_list_remove(&tablet->axis.link);
	free(tablet);
}

static void keyboard_key_notify(struct wl_listener *listener, void *data) {
	struct sample_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct sample_state *sample = keyboard->state;
	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
			keycode, &syms);
	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		if (sym == XKB_KEY_Escape) {
			wl_display_terminate(sample->display);
		}
	}
}

static void keyboard_destroy_notify(struct wl_listener *listener, void *data) {
	struct sample_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->key.link);
	free(keyboard);
}

static void output_remove_notify(struct wl_listener *listener, void *data) {
	struct sample_output *sample_output = wl_container_of(listener, sample_output, destroy);
	struct sample_state *sample = sample_output->state;
	wlr_output_layout_remove(sample->layout, sample_output->output);
	wl_list_remove(&sample_output->frame.link);
	wl_list_remove(&sample_output->destroy.link);
	free(sample_output);
}

static void new_output_notify(struct wl_listener *listener, void *data) {
	struct wlr_output *output = data;
	struct sample_state *sample = wl_container_of(listener, sample, new_output);

	wlr_output_init_render(output, sample->allocator, sample->renderer);

	struct sample_output *sample_output = calloc(1, sizeof(struct sample_output));
	sample_output->output = output;
	sample_output->state = sample;
	wl_signal_add(&output->events.frame, &sample_output->frame);
	sample_output->frame.notify = output_frame_notify;
	wl_signal_add(&output->events.destroy, &sample_output->destroy);
	sample_output->destroy.notify = output_remove_notify;
	wlr_output_layout_add_auto(sample->layout, sample_output->output);

	wlr_xcursor_manager_load(sample->xcursor_manager, output->scale);
	wlr_xcursor_manager_set_cursor_image(sample->xcursor_manager, "left_ptr",
		sample->cursor);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	if (mode != NULL) {
		wlr_output_set_mode(output, mode);
	}

	wlr_output_commit(output);
}

static void new_input_notify(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	struct sample_state *state = wl_container_of(listener, state, new_input);
	switch (device->type) {
	case WLR_INPUT_DEVICE_POINTER:;
		struct sample_pointer *pointer = calloc(1, sizeof(struct sample_pointer));
		pointer->wlr_pointer = wlr_pointer_from_input_device(device);
		pointer->state = state;
		wl_signal_add(&device->events.destroy, &pointer->destroy);
		pointer->destroy.notify = pointer_destroy_notify;
		wl_signal_add(&pointer->wlr_pointer->events.motion, &pointer->motion);
		pointer->motion.notify = pointer_motion_notify;
		wl_signal_add(&pointer->wlr_pointer->events.motion_absolute,
			&pointer->motion_absolute);
		pointer->motion_absolute.notify = pointer_motion_absolute_notify;
		wl_signal_add(&pointer->wlr_pointer->events.button, &pointer->button);
		pointer->button.notify = pointer_button_notify;
		wl_signal_add(&pointer->wlr_pointer->events.axis, &pointer->axis);
		pointer->axis.notify = pointer_axis_notify;
		break;
	case WLR_INPUT_DEVICE_TOUCH:;
		struct sample_touch *touch = calloc(1, sizeof(struct sample_touch));
		touch->wlr_touch = wlr_touch_from_input_device(device);
		touch->state = state;
		wl_list_init(&touch->points);
		wl_signal_add(&device->events.destroy, &touch->destroy);
		touch->destroy.notify = touch_destroy_notify;
		wl_signal_add(&touch->wlr_touch->events.up, &touch->up);
		touch->up.notify = touch_up_notify;
		wl_signal_add(&touch->wlr_touch->events.down, &touch->down);
		touch->down.notify = touch_down_notify;
		wl_signal_add(&touch->wlr_touch->events.motion, &touch->motion);
		touch->motion.notify = touch_motion_notify;
		break;
	case WLR_INPUT_DEVICE_TABLET_TOOL:;
		struct sample_tablet *tablet = calloc(1, sizeof(struct sample_tablet));
		tablet->wlr_tablet = wlr_tablet_from_input_device(device);
		tablet->state = state;
		wl_signal_add(&device->events.destroy, &tablet->destroy);
		tablet->destroy.notify = tablet_destroy_notify;
		wl_signal_add(&tablet->wlr_tablet->events.axis, &tablet->axis);
		tablet->axis.notify = tablet_axis_notify;
		break;
	case WLR_INPUT_DEVICE_KEYBOARD:;
		struct sample_keyboard *keyboard = calloc(1, sizeof(struct sample_keyboard));
		keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);
		keyboard->state = state;
		wl_signal_add(&device->events.destroy, &keyboard->destroy);
		keyboard->destroy.notify = keyboard_destroy_notify;
		wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
		keyboard->key.notify = keyboard_key_notify;
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (!context) {
			wlr_log(WLR_ERROR, "Failed to create XKB context");
			exit(1);
		}
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) {
			wlr_log(WLR_ERROR, "Failed to create XKB keymap");
			exit(1);
		}
		wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
		break;
	default:
		break;
	}
}


int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	struct wl_display *display = wl_display_create();
	struct sample_state state = {
		.default_color = { 0.25f, 0.25f, 0.25f, 1 },
		.clear_color = { 0.25f, 0.25f, 0.25f, 1 },
		.display = display,
	};

	struct wlr_backend *wlr = wlr_backend_autocreate(display, NULL);
	if (!wlr) {
		exit(1);
	}

	state.renderer = wlr_renderer_autocreate(wlr);
	state.allocator = wlr_allocator_autocreate(wlr, state.renderer);

	state.layout = wlr_output_layout_create();
	state.cursor = wlr_cursor_create(state.layout);

	state.input_mapper = wlr_input_mapper_create();
	wlr_input_mapper_attach_output_layout(state.input_mapper, state.layout);

	wl_signal_add(&wlr->events.new_input, &state.new_input);
	state.new_input.notify = new_input_notify;

	wl_signal_add(&wlr->events.new_output, &state.new_output);
	state.new_output.notify = new_output_notify;

	state.xcursor_manager = wlr_xcursor_manager_create("default", 24);
	if (!state.xcursor_manager) {
		wlr_log(WLR_ERROR, "Failed to load left_ptr cursor");
		return 1;
	}

	wlr_xcursor_manager_set_cursor_image(state.xcursor_manager, "left_ptr",
		state.cursor);

	if (!wlr_backend_start(wlr)) {
		wlr_log(WLR_ERROR, "Failed to start backend");
		wlr_backend_destroy(wlr);
		exit(1);
	}
	wl_display_run(display);
	wl_display_destroy(display);

	wlr_xcursor_manager_destroy(state.xcursor_manager);
	wlr_cursor_destroy(state.cursor);
	wlr_output_layout_destroy(state.layout);
}
