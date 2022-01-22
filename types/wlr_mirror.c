#include <stdio.h>
#include <stdlib.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_mirror.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <util/signal.h>

struct wlr_mirror_output_src {
	struct wl_list link;

	struct wlr_mirror_state *state;

	struct wlr_output *output;

	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener precommit;
	struct wl_listener destroy;
};

struct wlr_mirror_stats {
	long requested_boxes;
	long rendered_boxes;

	long requested_blanks;
	long rendered_blanks;

	long frames_dropped;
	long buffers_incomplete;
	long dmabufs_unavailable;
};

/**
 * All immutable during session, except noted.
 */
struct wlr_mirror_state {
	struct wlr_mirror *mirror;

	struct wlr_mirror_params params;

	struct wlr_addon output_dst_addon;

	struct wlr_output *output_src; // mutable
	struct wlr_output *output_dst;

	struct wl_list m_output_srcs; // wlr_mirror_output_src::link

	struct wlr_texture *texture; // mutable
	struct wlr_box box_src; // mutable
	bool needs_blank; // mutable
	bool cursor_locked; // mutable

	// events (ready) may result in a call to wlr_mirror_destroy.
	// During emission, wlr_mirror_destroy will not free mirror (specifically
	// the wl_signal) and state.
	// mirror and state will be free'd after wlr_signal_emit_safe is complete
	// and has cleaned up the signal's list.
	bool signal_emitting, needs_state_mirror_free;

	struct wl_listener output_dst_enable;
	struct wl_listener output_dst_frame;

	struct wlr_mirror_stats stats;
};

/**
 * Swaps v, h depending on rotation of the transform.
 */
static void rotate_v_h(int32_t *v_rotated, int32_t *h_rotated,
		enum wl_output_transform transform, int32_t v, int32_t h) {
	if (transform % 2 == 0) {
		*v_rotated = v;
		*h_rotated = h;
	} else {
		*v_rotated = h;
		*h_rotated = v;
	}
}

/**
 * Updates a box with absolute coordinates inside a (0, 0, width, height)
 * box without rotating or translating it.
 */
static void calculate_absolute_box(struct wlr_box *absolute,
		struct wlr_box *relative, enum wl_output_transform transform,
		int32_t width, int32_t height) {
	rotate_v_h(&absolute->x, &absolute->y, transform, relative->x, relative->y);
	rotate_v_h(&absolute->width, &absolute->height, transform, relative->width, relative->height);

	switch (transform) {
		case WL_OUTPUT_TRANSFORM_180:
		case WL_OUTPUT_TRANSFORM_270:
		case WL_OUTPUT_TRANSFORM_FLIPPED:
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			absolute->x = width - absolute->width - absolute->x;
			break;
		case WL_OUTPUT_TRANSFORM_NORMAL:
		case WL_OUTPUT_TRANSFORM_90:
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		default:
			break;
	}
	switch (transform) {
		case WL_OUTPUT_TRANSFORM_90:
		case WL_OUTPUT_TRANSFORM_180:
		case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		case WL_OUTPUT_TRANSFORM_FLIPPED_270:
			absolute->y = height - absolute->height - absolute->y;
			break;
		case WL_OUTPUT_TRANSFORM_NORMAL:
		case WL_OUTPUT_TRANSFORM_270:
		case WL_OUTPUT_TRANSFORM_FLIPPED:
		case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		default:
			break;
	}
}

/**
 * Position a box scaled to fit the width or height of dst. It will be rotated
 * from src to dst.
 */
static void calculate_dst_box(struct wlr_box *box_dst,
		enum wl_output_transform transform_src,
		enum wl_output_transform transform_dst,
		int32_t width_src, int32_t height_src,
		int32_t width_dst, int32_t height_dst) {
	double width_scaled, height_scaled;
	int32_t width_src_rotated, height_src_rotated;
	int32_t width_dst_rotated, height_dst_rotated;

	bool src_rotated = transform_src % 2 != 0;

	rotate_v_h(&width_src_rotated, &height_src_rotated, transform_src, width_src, height_src);
	rotate_v_h(&width_dst_rotated, &height_dst_rotated, transform_dst, width_dst, height_dst);

	if (width_dst_rotated * height_src_rotated > height_dst_rotated * width_src_rotated) {
		// expand to dst height
		width_scaled = ((double) width_src_rotated) * height_dst_rotated / height_src_rotated;
		height_scaled = height_dst_rotated;
	} else {
		// expand to dst width
		width_scaled = width_dst_rotated;
		height_scaled = ((double) height_src_rotated) * width_dst_rotated / width_src_rotated;
	}
	if (src_rotated) {
		box_dst->width = round(height_scaled);
		box_dst->height = round(width_scaled);
	} else {
		box_dst->width = round(width_scaled);
		box_dst->height = round(height_scaled);
	}
	box_dst->x = round((((double) width_dst_rotated) - width_scaled) / 2);
	box_dst->y = round((((double) height_dst_rotated) - height_scaled) / 2);
}

/**
 * Produce a transformation matrix that un-transforms from src and transforms to dst.
 */
static void calculate_render_matrix(float mat[static 9], struct wlr_box *box_dst,
		enum wl_output_transform transform_src, float transform_matrix_dst[static 9]) {
	// account for the rotated dimensions of dst
	struct wlr_box box_rotated = *box_dst;
	rotate_v_h(&box_rotated.width, &box_rotated.height, transform_src,
			box_rotated.width, box_rotated.height);

	// both transforms
	wlr_matrix_project_box(mat, &box_rotated, wlr_output_transform_invert(transform_src),
			0.0, transform_matrix_dst);
}

static void schedule_frame_dst(struct wlr_mirror_state *state) {
	wlr_output_schedule_frame(state->output_dst);

	wl_list_remove(&state->output_dst_frame.link);
	wl_signal_add(&state->output_dst->events.frame, &state->output_dst_frame);
}

/**
 * Remove all listeners for a src and remove it from state::m_output_srcs
 * Invoke wlr_mirror_destroy if no other srcs remain.
 */
static void remove_output_src(struct wlr_mirror_output_src *src) {
	struct wlr_mirror_state *state = src->state;

	wl_list_remove(&src->commit.link);
	wl_list_remove(&src->enable.link);
	wl_list_remove(&src->precommit.link);
	wl_list_remove(&src->destroy.link);
	wl_list_remove(&src->link);
	free(src);

	if (wl_list_length(&state->m_output_srcs) == 0) {
		wlr_mirror_destroy(state->mirror);
	}
}

static void output_src_handle_precommit(struct wl_listener *listener, void *data) {
	struct wlr_mirror_output_src *m_output_src =
		wl_container_of(listener, m_output_src, precommit);
	struct wlr_mirror_state *state = m_output_src->state;
	struct wlr_mirror *mirror = state->mirror;

	state->signal_emitting = true;
	wlr_signal_emit_safe(&mirror->events.ready, m_output_src->output);
	state->signal_emitting = false;
	if (state->needs_state_mirror_free) {
		free(state);
		free(mirror);
	}
}

static void output_src_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_mirror_output_src *m_output_src = wl_container_of(listener, m_output_src, commit);
	struct wlr_mirror_state *state = m_output_src->state;
	struct wlr_output *output_src = m_output_src->output;
	struct wlr_output_event_commit *event = data;

	state->output_src = output_src;

	wl_list_remove(&m_output_src->commit.link);
	wl_list_init(&m_output_src->commit.link);

	if (state->texture) {
		state->stats.frames_dropped++;
		wlr_texture_destroy(state->texture);
		state->texture = NULL;
	}

	if (!(event->committed & WLR_OUTPUT_STATE_BUFFER)) {
		state->stats.buffers_incomplete++;
		return;
	}

	if (state->params.overlay_cursor) {
		wlr_output_lock_software_cursors(output_src, true);
	}

	struct wlr_dmabuf_attributes attribs = {0};

	wlr_output_lock_attach_render(output_src, true);

	if (wlr_buffer_get_dmabuf(event->buffer, &attribs)) {
		state->texture = wlr_texture_from_dmabuf(output_src->renderer, &attribs);
		schedule_frame_dst(state);
	} else {
		state->stats.dmabufs_unavailable++;
	}

	wlr_output_lock_attach_render(output_src, false);

	if (state->params.overlay_cursor) {
		wlr_output_lock_software_cursors(output_src, false);
	}
}

static void output_dst_handle_frame(struct wl_listener *listener, void *data) {
	struct wlr_mirror_state *state = wl_container_of(listener, state, output_dst_frame);

	wl_list_remove(&state->output_dst_frame.link);
	wl_list_init(&state->output_dst_frame.link);

	struct wlr_output *output_dst = state->output_dst;
	struct wlr_output *output_src = state->output_src;

	wlr_output_attach_render(output_dst, NULL);

	wlr_renderer_begin(output_dst->renderer, output_dst->width, output_dst->height);

	static float col_blank[] = { 0, 0, 0, 1 };
	wlr_renderer_clear(output_dst->renderer, col_blank);

	if (state->needs_blank) {
		state->stats.rendered_blanks++;

		state->needs_blank = false;

	} else if (output_src && state->texture) {
		state->stats.rendered_boxes++;

		// tranform src box to real coordinates for the src
		struct wlr_box box_src;
		calculate_absolute_box(&box_src, &state->box_src, output_src->transform,
				output_src->width, output_src->height);

		// scale and position a box for the dst
		struct wlr_box box_dst = {0};
		calculate_dst_box(&box_dst,
				output_src->transform, output_dst->transform,
				box_src.width, box_src.height,
				output_dst->width, output_dst->height);

		// transform from src to dst
		float mat[9];
		calculate_render_matrix(mat, &box_dst, output_src->transform, output_dst->transform_matrix);

		// render the subtexture
		struct wlr_fbox fbox_sub = {
			.x = box_src.x,
			.y = box_src.y,
			.width = box_src.width,
			.height = box_src.height,
		};
		wlr_render_subtexture_with_matrix(output_dst->renderer, state->texture, &fbox_sub, mat, 1.0f);

		wlr_texture_destroy(state->texture);
		state->texture = NULL;
	}

	wlr_renderer_end(output_dst->renderer);
	wlr_output_commit(output_dst);

	state->output_src = NULL;
}

static void output_src_handle_enable(struct wl_listener *listener, void *data) {
	struct wlr_mirror_output_src *src = wl_container_of(listener, src, enable);

	if (!src->output->enabled) {
		wlr_log(WLR_DEBUG, "Mirror src '%s' disabled", src->output->name);
		remove_output_src(src);
	}
}

static void output_src_handle_destroy(struct wl_listener *listener, void *data) {
	struct wlr_mirror_output_src *src = wl_container_of(listener, src, destroy);

	wlr_log(WLR_DEBUG, "Mirror src '%s' destroyed", src->output->name);

	remove_output_src(src);
}

static void output_dst_handle_enable(struct wl_listener *listener, void *data) {
	struct wlr_mirror_state *state = wl_container_of(listener, state, output_dst_enable);
	struct wlr_mirror *mirror = state->mirror;

	if (!state->output_dst->enabled) {
		wlr_log(WLR_DEBUG, "Mirror dst '%s' disabled", state->output_dst->name);
		wlr_mirror_destroy(mirror);
	}
}

static void output_dst_addon_handle_destroy(struct wlr_addon *addon) {
	struct wlr_mirror_state *state = wl_container_of(addon, state, output_dst_addon);
	struct wlr_mirror *mirror = state->mirror;

	wlr_log(WLR_DEBUG, "Mirror dst '%s' destroyed", state->output_dst->name);

	wlr_mirror_destroy(mirror);
}

static const struct wlr_addon_interface output_dst_addon_impl = {
	.name = "wlr_mirror_output_dst",
	.destroy = output_dst_addon_handle_destroy,
};

struct wlr_mirror *wlr_mirror_create(struct wlr_mirror_params *params) {
	if (!params->output_dst->enabled) {
		wlr_log(WLR_ERROR, "Mirror dst '%s' not enabled", params->output_dst->name);
		return NULL;
	}
	if (wlr_mirror_v1_output_is_dst(params->output_dst)) {
		wlr_log(WLR_ERROR, "Mirror dst '%s' in use by another mirror session",
				params->output_dst->name);
		return NULL;
	}
	struct wlr_output **output_src_ptr;
	wl_array_for_each(output_src_ptr, &params->output_srcs) {
		if (!(*output_src_ptr)->enabled) {
			wlr_log(WLR_ERROR, "Mirror src '%s' not enabled", (*output_src_ptr)->name);
			return NULL;
		}
	}

	struct wlr_mirror *mirror = calloc(1, sizeof(struct wlr_mirror));
	mirror->state = calloc(1, sizeof(struct wlr_mirror_state));
	struct wlr_mirror_state *state = mirror->state;
	state->mirror = mirror;
	state->output_dst = params->output_dst;

	wl_list_init(&state->m_output_srcs);
	wl_signal_init(&mirror->events.ready);
	wl_signal_init(&mirror->events.destroy);

	// clone params
	memcpy(&state->params, params, sizeof(struct wlr_mirror_params));
	wl_array_init(&state->params.output_srcs);
	wl_array_copy(&state->params.output_srcs, &params->output_srcs);

	// dst events
	wl_list_init(&state->output_dst_frame.link);
	state->output_dst_frame.notify = output_dst_handle_frame;

	wl_list_init(&state->output_dst_enable.link);
	wl_signal_add(&state->output_dst->events.enable, &state->output_dst_enable);
	state->output_dst_enable.notify = output_dst_handle_enable;

	wlr_log(WLR_DEBUG, "Mirror creating dst '%s'", state->output_dst->name);

	// srcs events
	wl_array_for_each(output_src_ptr, &state->params.output_srcs) {
		struct wlr_mirror_output_src *m_output_src =
			calloc(1, sizeof(struct wlr_mirror_output_src));
		wl_list_insert(state->m_output_srcs.prev, &m_output_src->link);

		m_output_src->state = state;
		m_output_src->output = *output_src_ptr;

		wl_list_init(&m_output_src->commit.link);

		wl_list_init(&m_output_src->enable.link);
		wl_signal_add(&m_output_src->output->events.enable, &m_output_src->enable);
		m_output_src->enable.notify = output_src_handle_enable;

		wl_list_init(&m_output_src->precommit.link);
		wl_signal_add(&m_output_src->output->events.precommit, &m_output_src->precommit);
		m_output_src->precommit.notify = output_src_handle_precommit;

		wl_list_init(&m_output_src->destroy.link);
		wl_signal_add(&m_output_src->output->events.destroy, &m_output_src->destroy);
		m_output_src->destroy.notify = output_src_handle_destroy;

		wlr_log(WLR_DEBUG, "                src '%s'", m_output_src->output->name);
	}

	// blank initially, in case compositor delays requests
	state->needs_blank = true;
	schedule_frame_dst(state);

	wlr_addon_init(&state->output_dst_addon, &state->output_dst->addons, mirror,
			&output_dst_addon_impl);

	return mirror;
}

void wlr_mirror_destroy(struct wlr_mirror *mirror) {
	if (!mirror) {
		return;
	}
	struct wlr_mirror_state *state = mirror->state;

	wlr_log(WLR_DEBUG, "Mirror destroying dst '%s': "
			"requested_boxes:%ld, rendered_boxes:%ld, "
			"requested_blanks:%ld, rendered_blanks:%ld, "
			"frames_dropped:%ld, buffers_incomplete:%ld, "
			"dmabufs_unavailable:%ld",
			state->output_dst->name,
			state->stats.requested_boxes, state->stats.rendered_boxes,
			state->stats.requested_blanks, state->stats.rendered_blanks,
			state->stats.frames_dropped, state->stats.buffers_incomplete,
			state->stats.dmabufs_unavailable);

	// dst output events
	wl_list_remove(&state->output_dst_enable.link);
	wl_list_remove(&state->output_dst_frame.link);

	// all src output events
	struct wlr_mirror_output_src *src, *next;
	wl_list_for_each_safe(src, next, &state->m_output_srcs, link) {
		wl_list_remove(&src->commit.link);
		wl_list_remove(&src->enable.link);
		wl_list_remove(&src->precommit.link);
		wl_list_remove(&src->destroy.link);
		wl_list_remove(&src->link);
		free(src);
	}

	// destroy any frames in flight
	if (state->texture) {
		wlr_texture_destroy(state->texture);
		state->texture = NULL;
	}

	// the compositor may reclaim dst
	wlr_addon_finish(&state->output_dst_addon);

	// end the user's mirror "session"
	wlr_signal_emit_safe(&mirror->events.destroy, mirror);

	wl_array_release(&state->params.output_srcs);
	if (state->signal_emitting) {
		state->needs_state_mirror_free = true;
	} else {
		free(state);
		free(mirror);
	}
}

void wlr_mirror_request_blank(struct wlr_mirror *mirror) {
	struct wlr_mirror_state *state = mirror->state;

	state->needs_blank = true;

	schedule_frame_dst(state);

	mirror->state->stats.requested_blanks++;
}

void wlr_mirror_request_box(struct wlr_mirror *mirror,
		struct wlr_output *output_src, struct wlr_box box) {
	struct wlr_mirror_state *state = mirror->state;

	state->needs_blank = false;

	// restrict the box to the src
	struct wlr_box box_output = { 0 };
	wlr_output_transformed_resolution(output_src, &box_output.width, &box_output.height);
	if (!wlr_box_intersection(&state->box_src, &box_output, &box)) {
		wlr_log(WLR_ERROR, "Mirror box not within src, ending session.");
		wlr_mirror_destroy(mirror);
		return;
	}

	// listen for a commit on the specified output only
	struct wlr_mirror_output_src *m_output_src;
	wl_list_for_each(m_output_src, &state->m_output_srcs, link) {
		if (m_output_src->output == output_src) {
			wl_list_remove(&m_output_src->commit.link);
			wl_signal_add(&m_output_src->output->events.commit, &m_output_src->commit);
			m_output_src->commit.notify = output_src_handle_commit;
		}
	}

	state->stats.requested_boxes++;
}

bool wlr_mirror_v1_output_is_dst(struct wlr_output *output) {
	struct wl_array addons;
	wlr_addon_find_all(&addons, &output->addons, &output_dst_addon_impl);
	bool is_dst = addons.size > 0;
	wl_array_release(&addons);
	return is_dst;
}

