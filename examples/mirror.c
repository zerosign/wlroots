#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_mirror.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

/**
 * Demonstrates wlr_mirror. Comments describe mirror specific code.
 *
 * Mirrors the source output (src) on the destination output (dst).
 *
 * A moving square portion of the src (blue) is rendered on the dst (initially
 * red). The cursor is included in the mirrored content.
 */
static const char usage[] =
"usage: mirror <src> <dst>\n"
"    e.g. mirror eDP-1 HDMI-A-1\n"
"keys:\n"
"    m: toggle mirroring\n"
"    esc: exit\n";

struct sample_state {
	struct wl_display *display;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wlr_cursor *cursor;
	struct wlr_output_layout *layout;

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener cursor_motion;

	struct sample_mirror *mirror;
	struct sample_output *output_src;
	struct sample_output *output_dst;

	char *src_name;
	char *dst_name;
};

// lifetime: mirror session
struct sample_mirror {
	struct sample_state *state;

	struct wlr_mirror *wlr_mirror;

	struct wlr_mirror_params params;

	struct wl_listener ready;
	struct wl_listener destroy;

	int dx, dy;
	struct wlr_box box;
	struct timespec last_request;
};

struct sample_output {
	struct sample_state *state;
	struct wlr_output *wlr_output;

	int width;
	int height;

	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener restore;
	struct wl_listener remove;
};

struct sample_keyboard {
	struct sample_state *state;
	struct wlr_input_device *device;
	struct wl_listener key;
	struct wl_listener destroy;
};

void start_mirror(struct sample_state *state);
void end_mirror(struct sample_state *state);
void handle_mirror_ready(struct wl_listener *listener, void *data);
void handle_mirror_destroy(struct wl_listener *listener, void *data);
void handle_output_remove(struct wl_listener *listener, void *data);
void handle_output_restore(struct wl_listener *listener, void *data);
void handle_output_frame(struct wl_listener *listener, void *data);
void handle_output_destroy(struct wl_listener *listener, void *data);
void handle_new_input(struct wl_listener *listener, void *data);
void handle_new_output(struct wl_listener *listener, void *data);
void handle_cursor_motion(struct wl_listener *listener, void *data);
void handle_keyboard_key(struct wl_listener *listener, void *data);
void handle_keyboard_destroy(struct wl_listener *listener, void *data);
void render_rects(struct wlr_renderer *renderer, struct sample_output *output, float colour[]);

// start a mirror session
void start_mirror(struct sample_state *state) {
	struct sample_output *output_src = state->output_src;
	struct sample_output *output_dst = state->output_dst;
	if (!output_src || !output_dst) {
		return;
	}

	wlr_log(WLR_DEBUG, "mirror start dst '%s'", state->output_dst->wlr_output->name);

	struct sample_mirror *mirror = calloc(1, sizeof(struct sample_mirror));
	mirror->state = state;
	state->mirror = mirror;

	int wh = MIN(output_src->width, output_src->height) * 3 / 4;
	mirror->box.width = wh;
	mirror->box.height = wh;
	mirror->dx = 1;
	mirror->dy = 1;

	// params immutable over the session
	mirror->params.overlay_cursor = true;
	mirror->params.output_dst = state->output_dst->wlr_output;

	wl_array_init(&mirror->params.output_srcs);
	struct wlr_output **output_src_ptr =
		wl_array_add(&mirror->params.output_srcs, sizeof(struct output_src_ptr*));
	*output_src_ptr = state->output_src->wlr_output;

	// stop rendering frames on this output
	wl_list_remove(&state->output_dst->frame.link);
	wl_list_init(&state->output_dst->frame.link);

	struct wlr_mirror *wlr_mirror = wlr_mirror_create(&mirror->params);
	mirror->wlr_mirror = wlr_mirror;

	// ready events enabling us to make requests for the upcoming commit
	wl_signal_add(&wlr_mirror->events.ready, &mirror->ready);
	mirror->ready.notify = handle_mirror_ready;

	// destroy at session end
	wl_signal_add(&wlr_mirror->events.destroy, &mirror->destroy);
	mirror->destroy.notify = handle_mirror_destroy;
}

// request that we end the session
void end_mirror(struct sample_state *state) {
	wlr_log(WLR_DEBUG, "mirror end dst '%s'", state->output_dst->wlr_output->name);

	if (state->mirror) {
		// immediately emits wlr_mirror::events::destroy
		wlr_mirror_destroy(state->mirror->wlr_mirror);
	}
}

// mirror is ready to display content from an output; called at src precommit
void handle_mirror_ready(struct wl_listener *listener, void *data) {
	struct sample_mirror *mirror = wl_container_of(listener, mirror, ready);
	struct sample_state *state = mirror->state;
	struct sample_output *output_src = state->output_src;
	struct wlr_mirror *wlr_mirror = state->mirror->wlr_mirror;
	struct wlr_output *wlr_output = data;

	// only request for src
	if (wlr_output != state->output_src->wlr_output) {
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - mirror->last_request.tv_sec) * 1000 +
		(now.tv_nsec - mirror->last_request.tv_nsec) / 1000000;
	if (ms > 10) {
		mirror->last_request = now;

		// request a portion of src
		wlr_mirror_request_box(wlr_mirror, wlr_output, mirror->box);

		if ((mirror->box.x + mirror->box.width + mirror->dx) > output_src->width) {
			mirror->dx = -1;
		} else if ((mirror->box.x + mirror->dx) < 0) {
			mirror->dx = 1;
		}
		if ((mirror->box.y + mirror->box.height + mirror->dy) > output_src->height) {
			mirror->dy = -1;
		} else if ((mirror->box.y + mirror->dy) < 0) {
			mirror->dy = 1;
		}
		mirror->box.x += mirror->dx;
		mirror->box.y += mirror->dy;
	}
}

// mirror session is over
void handle_mirror_destroy(struct wl_listener *listener, void *data) {
	struct sample_mirror *mirror = wl_container_of(listener, mirror, destroy);
	struct sample_state *state = mirror->state;

	wlr_log(WLR_DEBUG, "mirror destroy dst '%s'", state->output_dst->wlr_output->name);

	wl_list_remove(&mirror->ready.link);
	wl_list_remove(&mirror->destroy.link);

	wl_array_release(&mirror->params.output_srcs);
	free(mirror);
	state->mirror = NULL;

	// start rendering our frames again
	struct sample_output *output = state->output_dst;
	if (output) {
		wl_signal_add(&output->wlr_output->events.frame, &output->frame);
		output->frame.notify = handle_output_frame;
	}
}

// shrinking rects alternating colour / grey
void render_rects(struct wlr_renderer *renderer, struct sample_output *output,
		float colour[]) {
	struct wlr_output *wlr_output = output->wlr_output;

	static float colour_black[] = { 0, 0, 0, 1 };
	wlr_renderer_clear(renderer, colour_black);

	struct wlr_box box_rect = {
		.x = 0,
		.y = 0,
		.width = output->width,
		.height = output->height,
	};
	int delta_box = MIN(output->width / 16, output->height / 16);

	static float colour_grey[] = { 0.05, 0.05, 0.05, 1 };
	static float delta_grey = 0.002;
	static struct timespec last_grey = { 0 };
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - last_grey.tv_sec) * 1000 +
		(now.tv_nsec - last_grey.tv_nsec) / 1000000;
	if (ms > 10) {
		last_grey = now;
		if (colour_grey[0] + delta_grey > 0.2) {
			delta_grey = -0.002;
		} else if (colour_grey[0] - delta_grey < 0.05) {
			delta_grey = 0.002;
		}
		colour_grey[0] += delta_grey;
		colour_grey[1] += delta_grey;
		colour_grey[2] += delta_grey;
	}

	bool grey = false;
	while (box_rect.x < output->width / 2 && box_rect.y < output->height / 2) {
		wlr_render_rect(renderer, &box_rect, grey ? colour_grey : colour,
				wlr_output->transform_matrix);
		grey = !grey;
		box_rect.x += delta_box;
		box_rect.y += delta_box;
		box_rect.width -= 2 * delta_box;
		box_rect.height -= 2 * delta_box;
	}
}

// will not be invoked for dst during mirror session
void handle_output_frame(struct wl_listener *listener, void *data) {
	struct sample_output *output = wl_container_of(listener, output, frame);
	struct sample_state *state = output->state;
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_renderer *renderer = state->renderer;

	static float colour_red[] = { 0.75, 0, 0, 1 };
	static float colour_blue[] = { 0, 0, 0.75, 1 };

	wlr_output_attach_render(wlr_output, NULL);
	wlr_renderer_begin(renderer, wlr_output->width, wlr_output->height);

	render_rects(renderer, output, output == state->output_src ? colour_blue : colour_red);

	wlr_output_render_software_cursors(wlr_output, NULL);
	wlr_renderer_end(renderer);
	wlr_output_commit(wlr_output);
}

void handle_cursor_motion(struct wl_listener *listener, void *data) {
	struct sample_state *state = wl_container_of(listener, state, cursor_motion);
	struct wlr_event_pointer_motion *event = data;
	wlr_cursor_move(state->cursor, event->device, event->delta_x, event->delta_y);
}

void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct sample_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct sample_state *state = keyboard->state;
	struct wlr_event_keyboard_key *event = data;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->device->keyboard->xkb_state, keycode, &syms);

	for (int i = 0; i < nsyms; i++) {
		xkb_keysym_t sym = syms[i];
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			switch (sym) {
				case XKB_KEY_Escape:
					wl_display_terminate(state->display);
					break;
				case XKB_KEY_m:
					if (state->mirror) {
						end_mirror(state);
					} else {
						start_mirror(state);
					}
					break;
				default:
					break;
			}
		}
	}
}

void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct sample_output *output = wl_container_of(listener, output, destroy);
	struct sample_state *state = output->state;

	wlr_log(WLR_DEBUG, "output destroyed '%s'", output->wlr_output->name);

	wlr_output_layout_remove(state->layout, output->wlr_output);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	if (output == state->output_dst) {
		state->output_dst = NULL;
	} else if (output == state->output_src) {
		state->output_src = NULL;
	}
	free(output);
}

void handle_new_output(struct wl_listener *listener, void *data) {
	struct wlr_output *wlr_output = data;
	struct sample_state *state = wl_container_of(listener, state, new_output);

	struct sample_output *output = calloc(1, sizeof(struct sample_output));
	output->wlr_output = wlr_output;
	output->state = state;

	if (strcmp(wlr_output->name, state->src_name) == 0) {
		wlr_log(WLR_DEBUG, "found src '%s'", wlr_output->name);
		state->output_src = output;
	} else if (strcmp(wlr_output->name, state->dst_name) == 0) {
		wlr_log(WLR_DEBUG, "found dst '%s'", wlr_output->name);
		state->output_dst = output;
	} else {
		free(output);
		wlr_log(WLR_DEBUG, "ignoring extraneous output '%s'", wlr_output->name);
		return;
	}

	wlr_output_init_render(wlr_output, state->allocator, state->renderer);

	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->destroy.notify = handle_output_destroy;
	wlr_output_enable(wlr_output, true);
	wlr_output_layout_add_auto(state->layout, output->wlr_output);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_set_mode(wlr_output, mode);
	}
	wlr_xcursor_manager_load(state->xcursor_manager, wlr_output->scale);
	wlr_xcursor_manager_set_cursor_image(state->xcursor_manager, "left_ptr", state->cursor);

	// draw frames, stopping for dst when we start the mirror session
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->frame.notify = handle_output_frame;

	if (!wlr_output_commit(wlr_output)) {
		wlr_log(WLR_ERROR, "Failed to setup output %s, exiting", wlr_output->name);
		exit(1);
	}

	wlr_output_transformed_resolution(wlr_output, &output->width, &output->height);
}

void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	struct sample_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->key.link);
	free(keyboard);
}

void handle_new_input(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	struct sample_state *state = wl_container_of(listener, state, new_input);
	switch (device->type) {
		case WLR_INPUT_DEVICE_POINTER:
		case WLR_INPUT_DEVICE_TOUCH:
		case WLR_INPUT_DEVICE_TABLET_TOOL:
			wlr_cursor_attach_input_device(state->cursor, device);
			break;
		case WLR_INPUT_DEVICE_KEYBOARD:
			;
			struct sample_keyboard *keyboard = calloc(1, sizeof(struct sample_keyboard));
			keyboard->device = device;
			keyboard->state = state;
			wl_signal_add(&device->events.destroy, &keyboard->destroy);
			keyboard->destroy.notify = handle_keyboard_destroy;
			wl_signal_add(&device->keyboard->events.key, &keyboard->key);
			keyboard->key.notify = handle_keyboard_key;
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
			wlr_keyboard_set_keymap(device->keyboard, keymap);
			xkb_keymap_unref(keymap);
			xkb_context_unref(context);
			break;
		default:
			break;
	}
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "%s", usage);
		exit(1);
	}

	wlr_log_init(WLR_DEBUG, NULL);

	struct wl_display *display = wl_display_create();
	struct sample_state state = {
		.display = display,
		.src_name = strdup(argv[1]),
		.dst_name = strdup(argv[2]),
	};

	struct wlr_backend *backend = wlr_backend_autocreate(display);
	if (!backend) {
		exit(1);
	}

	state.renderer = wlr_renderer_autocreate(backend);
	state.allocator = wlr_allocator_autocreate(backend, state.renderer);
	state.cursor = wlr_cursor_create();
	state.layout = wlr_output_layout_create();
	wlr_cursor_attach_output_layout(state.cursor, state.layout);
	state.xcursor_manager = wlr_xcursor_manager_create("default", 24);
	if (!state.xcursor_manager) {
		wlr_log(WLR_ERROR, "Failed to load left_ptr cursor");
		exit(1);
	}

	wl_signal_add(&backend->events.new_output, &state.new_output);
	state.new_output.notify = handle_new_output;
	wl_signal_add(&backend->events.new_input, &state.new_input);
	state.new_input.notify = handle_new_input;
	wl_signal_add(&state.cursor->events.motion, &state.cursor_motion);
	state.cursor_motion.notify = handle_cursor_motion;

	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "Failed to start backend");
		wlr_backend_destroy(backend);
		exit(1);
	}

	if (!state.output_src) {
		wlr_log(WLR_ERROR, "missing src %s, exiting", state.src_name);
		exit(1);
	}
	if (!state.output_dst) {
		wlr_log(WLR_ERROR, "missing dst %s, exiting", state.dst_name);
		exit(1);
	}

	// restrict cursor to src
	wlr_cursor_warp_absolute(state.cursor, NULL, 1, 1);
	wlr_cursor_map_to_output(state.cursor, state.output_src->wlr_output);
	wlr_cursor_warp_absolute(state.cursor, NULL, 0, 0);

	wl_display_run(display);

	// stops and destroys the mirror
	wl_display_destroy(display);

	wlr_xcursor_manager_destroy(state.xcursor_manager);
	wlr_cursor_destroy(state.cursor);
	wlr_output_layout_destroy(state.layout);

	free(state.src_name);
	free(state.dst_name);
	return 0;
}

