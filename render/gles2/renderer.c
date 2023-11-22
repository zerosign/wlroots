#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <drm_fourcc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/render/egl.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "render/egl.h"
#include "render/gles2.h"
#include "render/pixel_format.h"
#include "types/wlr_matrix.h"
#include "util/time.h"

#include "common_vert_src.h"
#include "quad_frag_src.h"
#include "tex_rgba_frag_src.h"
#include "tex_rgbx_frag_src.h"
#include "tex_external_frag_src.h"

static const struct wlr_renderer_impl renderer_impl;
static const struct wlr_render_timer_impl render_timer_impl;

bool wlr_renderer_is_gles2(struct wlr_renderer *wlr_renderer) {
	return wlr_renderer->impl == &renderer_impl;
}

struct wlr_gles2_renderer *gles2_get_renderer(
		struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer_is_gles2(wlr_renderer));
	struct wlr_gles2_renderer *renderer = wl_container_of(wlr_renderer, renderer, wlr_renderer);
	return renderer;
}

static struct wlr_gles2_renderer *gles2_get_renderer_in_context(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	assert(wlr_egl_is_current(renderer->egl));
	assert(renderer->current_buffer != NULL);
	return renderer;
}

bool wlr_render_timer_is_gles2(struct wlr_render_timer *timer) {
	return timer->impl == &render_timer_impl;
}

struct wlr_gles2_render_timer *gles2_get_render_timer(struct wlr_render_timer *wlr_timer) {
	assert(wlr_render_timer_is_gles2(wlr_timer));
	struct wlr_gles2_render_timer *timer = wl_container_of(wlr_timer, timer, base);
	return timer;
}

static void destroy_buffer(struct wlr_gles2_buffer *buffer) {
	wl_list_remove(&buffer->link);
	wlr_addon_finish(&buffer->addon);

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(buffer->renderer->egl);

	push_gles2_debug(buffer->renderer);

	glDeleteFramebuffers(1, &buffer->fbo);
	glDeleteRenderbuffers(1, &buffer->rbo);

	pop_gles2_debug(buffer->renderer);

	wlr_egl_destroy_image(buffer->renderer->egl, buffer->image);

	wlr_egl_restore_context(&prev_ctx);

	free(buffer);
}

static void handle_buffer_destroy(struct wlr_addon *addon) {
	struct wlr_gles2_buffer *buffer =
		wl_container_of(addon, buffer, addon);
	destroy_buffer(buffer);
}

static const struct wlr_addon_interface buffer_addon_impl = {
	.name = "wlr_gles2_buffer",
	.destroy = handle_buffer_destroy,
};

static struct wlr_gles2_buffer *get_or_create_buffer(struct wlr_gles2_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_addon *addon =
		wlr_addon_find(&wlr_buffer->addons, renderer, &buffer_addon_impl);
	if (addon) {
		struct wlr_gles2_buffer *buffer = wl_container_of(addon, buffer, addon);
		return buffer;
	}

	struct wlr_gles2_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->buffer = wlr_buffer;
	buffer->renderer = renderer;

	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		goto error_buffer;
	}

	bool external_only;
	buffer->image = wlr_egl_create_image_from_dmabuf(renderer->egl,
		&dmabuf, &external_only);
	if (buffer->image == EGL_NO_IMAGE_KHR) {
		goto error_buffer;
	}

	push_gles2_debug(renderer);

	glGenRenderbuffers(1, &buffer->rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, buffer->rbo);
	renderer->procs.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
		buffer->image);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &buffer->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, buffer->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_RENDERBUFFER, buffer->rbo);
	GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	pop_gles2_debug(renderer);

	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "Failed to create FBO");
		goto error_image;
	}

	wlr_addon_init(&buffer->addon, &wlr_buffer->addons, renderer,
		&buffer_addon_impl);

	wl_list_insert(&renderer->buffers, &buffer->link);

	wlr_log(WLR_DEBUG, "Created GL FBO for buffer %dx%d",
		wlr_buffer->width, wlr_buffer->height);

	return buffer;

error_image:
	wlr_egl_destroy_image(renderer->egl, buffer->image);
error_buffer:
	free(buffer);
	return NULL;
}

static bool gles2_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	if (renderer->current_buffer != NULL) {
		assert(wlr_egl_is_current(renderer->egl));

		push_gles2_debug(renderer);
		glFlush();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		pop_gles2_debug(renderer);

		wlr_buffer_unlock(renderer->current_buffer->buffer);
		renderer->current_buffer = NULL;
	}

	if (wlr_buffer == NULL) {
		wlr_egl_unset_current(renderer->egl);
		return true;
	}

	wlr_egl_make_current(renderer->egl);

	struct wlr_gles2_buffer *buffer = get_or_create_buffer(renderer, wlr_buffer);
	if (buffer == NULL) {
		return false;
	}

	wlr_buffer_lock(wlr_buffer);
	renderer->current_buffer = buffer;

	push_gles2_debug(renderer);
	glBindFramebuffer(GL_FRAMEBUFFER, renderer->current_buffer->fbo);
	pop_gles2_debug(renderer);

	return true;
}

static const char *reset_status_str(GLenum status) {
	switch (status) {
	case GL_GUILTY_CONTEXT_RESET_KHR:
		return "guilty";
	case GL_INNOCENT_CONTEXT_RESET_KHR:
		return "innocent";
	case GL_UNKNOWN_CONTEXT_RESET_KHR:
		return "unknown";
	default:
		return "<invalid>";
	}
}

static bool gles2_begin(struct wlr_renderer *wlr_renderer, uint32_t width,
		uint32_t height) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);

	if (renderer->procs.glGetGraphicsResetStatusKHR) {
		GLenum status = renderer->procs.glGetGraphicsResetStatusKHR();
		if (status != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "GPU reset (%s)", reset_status_str(status));
			wl_signal_emit_mutable(&wlr_renderer->events.lost, NULL);
			return false;
		}
	}

	glViewport(0, 0, width, height);
	renderer->viewport_width = width;
	renderer->viewport_height = height;

	// refresh projection matrix
	matrix_projection(renderer->projection, width, height,
		WL_OUTPUT_TRANSFORM_FLIPPED_180);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// XXX: maybe we should save output projection and remove some of the need
	// for users to sling matricies themselves

	pop_gles2_debug(renderer);

	return true;
}

static void gles2_end(struct wlr_renderer *wlr_renderer) {
	gles2_get_renderer_in_context(wlr_renderer);
	// no-op
}

static const uint32_t *gles2_get_shm_texture_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return get_gles2_shm_formats(renderer, len);
}

static const struct wlr_drm_format_set *gles2_get_dmabuf_texture_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_texture_formats(renderer->egl);
}

static const struct wlr_drm_format_set *gles2_get_render_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_render_formats(renderer->egl);
}

static uint32_t gles2_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	push_gles2_debug(renderer);

	GLint gl_format = -1, gl_type = -1, alpha_size = -1;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &gl_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &gl_type);
	glGetIntegerv(GL_ALPHA_BITS, &alpha_size);

	pop_gles2_debug(renderer);

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_gl(gl_format, gl_type, alpha_size > 0);
	if (fmt != NULL) {
		return fmt->drm_format;
	}

	if (renderer->exts.EXT_read_format_bgra) {
		return DRM_FORMAT_XRGB8888;
	}
	return DRM_FORMAT_XBGR8888;
}

static bool gles2_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_drm(drm_format);
	if (fmt == NULL || !is_gles2_pixel_format_supported(renderer, fmt)) {
		wlr_log(WLR_ERROR, "Cannot read pixels: unsupported pixel format 0x%"PRIX32, drm_format);
		return false;
	}

	if (fmt->gl_format == GL_BGRA_EXT && !renderer->exts.EXT_read_format_bgra) {
		wlr_log(WLR_ERROR,
			"Cannot read pixels: missing GL_EXT_read_format_bgra extension");
		return false;
	}

	const struct wlr_pixel_format_info *drm_fmt =
		drm_get_pixel_format_info(fmt->drm_format);
	assert(drm_fmt);
	if (pixel_format_info_pixels_per_block(drm_fmt) != 1) {
		wlr_log(WLR_ERROR, "Cannot read pixels: block formats are not supported");
		return false;
	}

	push_gles2_debug(renderer);

	// Make sure any pending drawing is finished before we try to read it
	glFinish();

	glGetError(); // Clear the error flag

	unsigned char *p = (unsigned char *)data + dst_y * stride;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	uint32_t pack_stride = pixel_format_info_min_stride(drm_fmt, width);
	if (pack_stride == stride && dst_x == 0) {
		// Under these particular conditions, we can read the pixels with only
		// one glReadPixels call

		glReadPixels(src_x, src_y, width, height, fmt->gl_format, fmt->gl_type, p);
	} else {
		// Unfortunately GLES2 doesn't support GL_PACK_ROW_LENGTH, so we have to read
		// the lines out row by row
		for (size_t i = 0; i < height; ++i) {
			uint32_t y = src_y + i;
			glReadPixels(src_x, y, width, 1, fmt->gl_format,
				fmt->gl_type, p + i * stride + dst_x * drm_fmt->bytes_per_block);
		}
	}

	pop_gles2_debug(renderer);

	return glGetError() == GL_NO_ERROR;
}

static int gles2_get_drm_fd(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);

	if (renderer->drm_fd < 0) {
		renderer->drm_fd = wlr_egl_dup_drm_fd(renderer->egl);
	}

	return renderer->drm_fd;
}

static uint32_t gles2_get_render_buffer_caps(struct wlr_renderer *wlr_renderer) {
	return WLR_BUFFER_CAP_DMABUF;
}

struct wlr_egl *wlr_gles2_renderer_get_egl(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);
	return renderer->egl;
}

static void gles2_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	wlr_egl_make_current(renderer->egl);

	struct wlr_gles2_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		destroy_buffer(buffer);
	}

	struct wlr_gles2_texture *tex, *tex_tmp;
	wl_list_for_each_safe(tex, tex_tmp, &renderer->textures, link) {
		gles2_texture_destroy(tex);
	}

	push_gles2_debug(renderer);
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);
	pop_gles2_debug(renderer);

	if (renderer->exts.KHR_debug) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		renderer->procs.glDebugMessageCallbackKHR(NULL, NULL);
	}

	wlr_egl_unset_current(renderer->egl);
	wlr_egl_destroy(renderer->egl);

	if (renderer->drm_fd >= 0) {
		close(renderer->drm_fd);
	}

	free(renderer);
}

static struct wlr_render_pass *gles2_begin_buffer_pass(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer, const struct wlr_buffer_pass_options *options) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	if (!wlr_egl_make_current(renderer->egl)) {
		return NULL;
	}

	struct wlr_gles2_render_timer *timer = NULL;
	if (options->timer) {
		timer = gles2_get_render_timer(options->timer);
		clock_gettime(CLOCK_MONOTONIC, &timer->cpu_start);
	}

	struct wlr_gles2_buffer *buffer = get_or_create_buffer(renderer, wlr_buffer);
	if (!buffer) {
		return NULL;
	}

	struct wlr_gles2_render_pass *pass = begin_gles2_buffer_pass(buffer, timer);
	if (!pass) {
		return NULL;
	}
	return &pass->base;
}

static struct wlr_render_timer *gles2_render_timer_create(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	if (!renderer->exts.EXT_disjoint_timer_query) {
		wlr_log(WLR_ERROR, "can't create timer, EXT_disjoint_timer_query not available");
		return NULL;
	}

	struct wlr_gles2_render_timer *timer = calloc(1, sizeof(*timer));
	if (!timer) {
		return NULL;
	}
	timer->base.impl = &render_timer_impl;
	timer->renderer = renderer;

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(renderer->egl);
	renderer->procs.glGenQueriesEXT(1, &timer->id);
	wlr_egl_restore_context(&prev_ctx);

	return &timer->base;
}

static int gles2_get_render_time(struct wlr_render_timer *wlr_timer) {
	struct wlr_gles2_render_timer *timer = gles2_get_render_timer(wlr_timer);
	struct wlr_gles2_renderer *renderer = timer->renderer;

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(renderer->egl);

	GLint64 disjoint;
	renderer->procs.glGetInteger64vEXT(GL_GPU_DISJOINT_EXT, &disjoint);
	if (disjoint) {
		wlr_log(WLR_ERROR, "a disjoint operation occurred and the render timer is invalid");
		wlr_egl_restore_context(&prev_ctx);
		return -1;
	}

	GLint available;
	renderer->procs.glGetQueryObjectivEXT(timer->id,
		GL_QUERY_RESULT_AVAILABLE_EXT, &available);
	if (!available) {
		wlr_log(WLR_ERROR, "timer was read too early, gpu isn't done!");
		wlr_egl_restore_context(&prev_ctx);
		return -1;
	}

	GLuint64 gl_render_end;
	renderer->procs.glGetQueryObjectui64vEXT(timer->id, GL_QUERY_RESULT_EXT,
		&gl_render_end);

	int64_t cpu_nsec_total = timespec_to_nsec(&timer->cpu_end) - timespec_to_nsec(&timer->cpu_start);

	wlr_egl_restore_context(&prev_ctx);
	return gl_render_end - timer->gl_cpu_end + cpu_nsec_total;
}

static void gles2_render_timer_destroy(struct wlr_render_timer *wlr_timer) {
	struct wlr_gles2_render_timer *timer = wl_container_of(wlr_timer, timer, base);
	struct wlr_gles2_renderer *renderer = timer->renderer;

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(renderer->egl);
	renderer->procs.glDeleteQueriesEXT(1, &timer->id);
	wlr_egl_restore_context(&prev_ctx);
	free(timer);
}

static const struct wlr_renderer_impl renderer_impl = {
	.destroy = gles2_destroy,
	.bind_buffer = gles2_bind_buffer,
	.begin = gles2_begin,
	.end = gles2_end,
	.get_shm_texture_formats = gles2_get_shm_texture_formats,
	.get_dmabuf_texture_formats = gles2_get_dmabuf_texture_formats,
	.get_render_formats = gles2_get_render_formats,
	.preferred_read_format = gles2_preferred_read_format,
	.read_pixels = gles2_read_pixels,
	.get_drm_fd = gles2_get_drm_fd,
	.get_render_buffer_caps = gles2_get_render_buffer_caps,
	.texture_from_buffer = gles2_texture_from_buffer,
	.begin_buffer_pass = gles2_begin_buffer_pass,
	.render_timer_create = gles2_render_timer_create,
};

static const struct wlr_render_timer_impl render_timer_impl = {
	.get_duration_ns = gles2_get_render_time,
	.destroy = gles2_render_timer_destroy,
};

void push_gles2_debug_(struct wlr_gles2_renderer *renderer,
		const char *file, const char *func) {
	if (!renderer->procs.glPushDebugGroupKHR) {
		return;
	}

	int len = snprintf(NULL, 0, "%s:%s", file, func) + 1;
	char str[len];
	snprintf(str, len, "%s:%s", file, func);
	renderer->procs.glPushDebugGroupKHR(GL_DEBUG_SOURCE_APPLICATION_KHR, 1, -1, str);
}

void pop_gles2_debug(struct wlr_gles2_renderer *renderer) {
	if (renderer->procs.glPopDebugGroupKHR) {
		renderer->procs.glPopDebugGroupKHR();
	}
}

static enum wlr_log_importance gles2_log_importance_to_wlr(GLenum type) {
	switch (type) {
	case GL_DEBUG_TYPE_ERROR_KHR:               return WLR_ERROR;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR: return WLR_DEBUG;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR:  return WLR_ERROR;
	case GL_DEBUG_TYPE_PORTABILITY_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_PERFORMANCE_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_OTHER_KHR:               return WLR_DEBUG;
	case GL_DEBUG_TYPE_MARKER_KHR:              return WLR_DEBUG;
	case GL_DEBUG_TYPE_PUSH_GROUP_KHR:          return WLR_DEBUG;
	case GL_DEBUG_TYPE_POP_GROUP_KHR:           return WLR_DEBUG;
	default:                                    return WLR_DEBUG;
	}
}

static void gles2_log(GLenum src, GLenum type, GLuint id, GLenum severity,
		GLsizei len, const GLchar *msg, const void *user) {
	_wlr_log(gles2_log_importance_to_wlr(type), "[GLES2] %s", msg);
}

static GLuint compile_shader(struct wlr_gles2_renderer *renderer,
		GLenum type, const GLchar *src) {
	push_gles2_debug(renderer);

	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		wlr_log(WLR_ERROR, "Failed to compile shader");
		glDeleteShader(shader);
		shader = 0;
	}

	pop_gles2_debug(renderer);
	return shader;
}

static GLuint link_program(struct wlr_gles2_renderer *renderer,
		const GLchar *vert_src, const GLchar *frag_src) {
	push_gles2_debug(renderer);

	GLuint vert = compile_shader(renderer, GL_VERTEX_SHADER, vert_src);
	if (!vert) {
		goto error;
	}

	GLuint frag = compile_shader(renderer, GL_FRAGMENT_SHADER, frag_src);
	if (!frag) {
		glDeleteShader(vert);
		goto error;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	glDetachShader(prog, vert);
	glDetachShader(prog, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		wlr_log(WLR_ERROR, "Failed to link shader");
		glDeleteProgram(prog);
		goto error;
	}

	pop_gles2_debug(renderer);
	return prog;

error:
	pop_gles2_debug(renderer);
	return 0;
}

static bool check_gl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (exts[0] == ' ') {
			exts++;
			continue;
		}
		size_t n = strcspn(exts, " ");
		if (n == extlen && strncmp(ext, exts, n) == 0) {
			return true;
		}
		exts += n;
	}
	return false;
}

static void load_gl_proc(void *proc_ptr, const char *name) {
	void *proc = (void *)eglGetProcAddress(name);
	if (proc == NULL) {
		wlr_log(WLR_ERROR, "eglGetProcAddress(%s) failed", name);
		abort();
	}
	*(void **)proc_ptr = proc;
}

static bool process_upload_task(struct wlr_gles2_worker_task *task) {
	struct wlr_buffer *buffer = task->buffer;
	struct wlr_gles2_texture *texture = task->texture;

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return false;
	}

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_drm(texture->drm_format);
	assert(fmt);

	const struct wlr_pixel_format_info *drm_fmt =
		drm_get_pixel_format_info(texture->drm_format);
	assert(drm_fmt);

	push_gles2_debug(texture->renderer);

	glBindTexture(GL_TEXTURE_2D, texture->tex);

	int rects_len = 0;
	const pixman_box32_t *rects = pixman_region32_rectangles(&task->region, &rects_len);
	for (int i = 0; i < rects_len; i++) {
		pixman_box32_t rect = rects[i];

		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / drm_fmt->bytes_per_block);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rect.x1);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rect.y1);

		int width = rect.x2 - rect.x1;
		int height = rect.y2 - rect.y1;
		glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x1, rect.y1, width, height,
			fmt->gl_format, fmt->gl_type, data);
	}

	glBindTexture(GL_TEXTURE_2D, 0);

	wlr_buffer_end_data_ptr_access(buffer);

	return true;
}

static bool read_worker_task(struct wlr_gles2_worker_task *task, int fd) {
	while (true) {
		errno = 0;
		ssize_t n = read(fd, task, sizeof(*task));
		if (errno == EINTR) {
			continue;
		}
		if (n == sizeof(*task)) {
			return true;
		} else if (n < 0) {
			wlr_log_errno(WLR_ERROR, "read() failed");
		} else if (n > 0) {
			wlr_log(WLR_ERROR, "Unexpected partial read");
		}
		return false;
	}
}

static bool write_worker_task(const struct wlr_gles2_worker_task *task, int fd) {
	while (true) {
		errno = 0;
		ssize_t n = write(fd, task, sizeof(*task));
		if (errno == EINTR) {
			continue;
		}
		if (n == sizeof(*task)) {
			return true;
		} else if (n < 0) {
			wlr_log_errno(WLR_ERROR, "write() failed");
		} else if (n > 0) {
			wlr_log(WLR_ERROR, "Unexpected partial write");
		}
		return false;
	}
}

static void *run_uploads(void *data) {
	struct wlr_gles2_worker *worker = data;

	wlr_egl_make_current(worker->egl);

	while (true) {
		struct wlr_gles2_worker_task task = {0};
		if (!read_worker_task(&task, worker->worker_fd)) {
			break;
		}
		task.ok = process_upload_task(&task);
		if (!write_worker_task(&task, worker->worker_fd)) {
			break;
		}
	}

	close(worker->worker_fd);

	return NULL;
}

bool gles2_queue_upload(struct wlr_gles2_renderer *renderer,
		struct wlr_gles2_worker_task *task) {
	return write_worker_task(task, renderer->upload_worker.control_fd);
}

static int handle_upload_worker_result(int fd, uint32_t mask, void *data) {
	struct wlr_egl *parent_egl = data;

	if (mask & WL_EVENT_ERROR) {
		wlr_log(WLR_ERROR, "Upload worker FD error");
		return 0;
	}
	if (mask & WL_EVENT_HANGUP) {
		return 0;
	}

	if (mask & WL_EVENT_READABLE) {
		struct wlr_gles2_worker_task task = {0};
		if (!read_worker_task(&task, fd)) {
			return 0;
		}
		if (task.texture->upload_sync == task.sync) {
			task.texture->upload_sync = EGL_NO_SYNC_KHR;
		}
		// Destroying the sync object implicitly signals it
		wlr_egl_destroy_sync(parent_egl, task.sync);
		wlr_buffer_unlock(task.buffer);
	}

	return 0;
}

static bool init_upload_worker(struct wlr_gles2_worker *worker,
		struct wlr_egl *parent_egl, struct wl_event_loop *loop) {
	EGLint attrs[8] = {0};
	size_t attrs_len = 0;

	attrs[attrs_len++] = EGL_CONTEXT_CLIENT_VERSION;
	attrs[attrs_len++] = 2;

	if (parent_egl->exts.EXT_create_context_robustness) {
		attrs[attrs_len++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
		attrs[attrs_len++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
	}

	attrs[attrs_len++] = EGL_NONE;
	assert(attrs_len <= sizeof(attrs) / sizeof(attrs[0]));

	EGLContext context = eglCreateContext(parent_egl->display,
		EGL_NO_CONFIG_KHR, parent_egl->context, attrs);
	if (context == EGL_NO_CONTEXT) {
		wlr_log(WLR_ERROR, "eglCreateContext failed");
		return false;
	}

	worker->egl = wlr_egl_create_with_context(parent_egl->display, context);
	if (worker->egl == NULL) {
		eglDestroyContext(parent_egl->display, context);
		return false;
	}

	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
		wlr_log_errno(WLR_ERROR, "pipe() failed");
		goto error_egl;
	}
	worker->worker_fd = sockets[0];
	worker->control_fd = sockets[1];

	worker->event_source = wl_event_loop_add_fd(loop, worker->control_fd,
		WL_EVENT_READABLE, handle_upload_worker_result, parent_egl);
	if (worker->event_source == NULL) {
		wlr_log(WLR_ERROR, "wl_event_loop_add_fd() failed");
		goto error_fds;
	}

	if (pthread_create(&worker->thread, NULL, run_uploads, worker) != 0) {
		wlr_log_errno(WLR_ERROR, "pthread_create failed");
		goto error_event_source;
	}

	return true;

error_event_source:
	wl_event_source_remove(worker->event_source);
error_fds:
	close(worker->worker_fd);
	close(worker->control_fd);
error_egl:
	wlr_egl_destroy(worker->egl);
	return false;
}

struct wlr_renderer *wlr_gles2_renderer_create_with_drm_fd(int drm_fd,
		struct wl_event_loop *loop) {
	struct wlr_egl *egl = wlr_egl_create_with_drm_fd(drm_fd);
	if (egl == NULL) {
		wlr_log(WLR_ERROR, "Could not initialize EGL");
		return NULL;
	}

	struct wlr_renderer *renderer = wlr_gles2_renderer_create(egl, loop);
	if (!renderer) {
		wlr_log(WLR_ERROR, "Failed to create GLES2 renderer");
		wlr_egl_destroy(egl);
		return NULL;
	}

	return renderer;
}

struct wlr_renderer *wlr_gles2_renderer_create(struct wlr_egl *egl,
		struct wl_event_loop *loop) {
	if (!wlr_egl_make_current(egl)) {
		return NULL;
	}

	const char *exts_str = (const char *)glGetString(GL_EXTENSIONS);
	if (exts_str == NULL) {
		wlr_log(WLR_ERROR, "Failed to get GL_EXTENSIONS");
		return NULL;
	}

	struct wlr_gles2_renderer *renderer = calloc(1, sizeof(*renderer));
	if (renderer == NULL) {
		return NULL;
	}
	wlr_renderer_init(&renderer->wlr_renderer, &renderer_impl);

	wl_list_init(&renderer->buffers);
	wl_list_init(&renderer->textures);

	renderer->egl = egl;
	renderer->exts_str = exts_str;
	renderer->drm_fd = -1;

	wlr_log(WLR_INFO, "Creating GLES2 renderer");
	wlr_log(WLR_INFO, "Using %s", glGetString(GL_VERSION));
	wlr_log(WLR_INFO, "GL vendor: %s", glGetString(GL_VENDOR));
	wlr_log(WLR_INFO, "GL renderer: %s", glGetString(GL_RENDERER));
	wlr_log(WLR_INFO, "Supported GLES2 extensions: %s", exts_str);

	if (!renderer->egl->exts.EXT_image_dma_buf_import) {
		wlr_log(WLR_ERROR, "EGL_EXT_image_dma_buf_import not supported");
		free(renderer);
		return NULL;
	}
	if (!check_gl_ext(exts_str, "GL_EXT_texture_format_BGRA8888")) {
		wlr_log(WLR_ERROR, "BGRA8888 format not supported by GLES2");
		free(renderer);
		return NULL;
	}
	if (!check_gl_ext(exts_str, "GL_EXT_unpack_subimage")) {
		wlr_log(WLR_ERROR, "GL_EXT_unpack_subimage not supported");
		free(renderer);
		return NULL;
	}

	renderer->exts.EXT_read_format_bgra =
		check_gl_ext(exts_str, "GL_EXT_read_format_bgra");

	renderer->exts.EXT_texture_type_2_10_10_10_REV =
		check_gl_ext(exts_str, "GL_EXT_texture_type_2_10_10_10_REV");

	renderer->exts.OES_texture_half_float_linear =
		check_gl_ext(exts_str, "GL_OES_texture_half_float_linear");

	renderer->exts.EXT_texture_norm16 =
		check_gl_ext(exts_str, "GL_EXT_texture_norm16");

	if (check_gl_ext(exts_str, "GL_KHR_debug")) {
		renderer->exts.KHR_debug = true;
		load_gl_proc(&renderer->procs.glDebugMessageCallbackKHR,
			"glDebugMessageCallbackKHR");
		load_gl_proc(&renderer->procs.glDebugMessageControlKHR,
			"glDebugMessageControlKHR");
	}

	if (check_gl_ext(exts_str, "GL_OES_EGL_image_external")) {
		renderer->exts.OES_egl_image_external = true;
		load_gl_proc(&renderer->procs.glEGLImageTargetTexture2DOES,
			"glEGLImageTargetTexture2DOES");
	}

	if (check_gl_ext(exts_str, "GL_OES_EGL_image")) {
		renderer->exts.OES_egl_image = true;
		load_gl_proc(&renderer->procs.glEGLImageTargetRenderbufferStorageOES,
			"glEGLImageTargetRenderbufferStorageOES");
	}

	if (check_gl_ext(exts_str, "GL_KHR_robustness")) {
		GLint notif_strategy = 0;
		glGetIntegerv(GL_RESET_NOTIFICATION_STRATEGY_KHR, &notif_strategy);
		switch (notif_strategy) {
		case GL_LOSE_CONTEXT_ON_RESET_KHR:
			wlr_log(WLR_DEBUG, "GPU reset notifications are enabled");
			load_gl_proc(&renderer->procs.glGetGraphicsResetStatusKHR,
				"glGetGraphicsResetStatusKHR");
			break;
		case GL_NO_RESET_NOTIFICATION_KHR:
			wlr_log(WLR_DEBUG, "GPU reset notifications are disabled");
			break;
		}
	}

	if (check_gl_ext(exts_str, "GL_EXT_disjoint_timer_query")) {
		renderer->exts.EXT_disjoint_timer_query = true;
		load_gl_proc(&renderer->procs.glGenQueriesEXT, "glGenQueriesEXT");
		load_gl_proc(&renderer->procs.glDeleteQueriesEXT, "glDeleteQueriesEXT");
		load_gl_proc(&renderer->procs.glQueryCounterEXT, "glQueryCounterEXT");
		load_gl_proc(&renderer->procs.glGetQueryObjectivEXT, "glGetQueryObjectivEXT");
		load_gl_proc(&renderer->procs.glGetQueryObjectui64vEXT, "glGetQueryObjectui64vEXT");
		load_gl_proc(&renderer->procs.glGetInteger64vEXT, "glGetInteger64vEXT");
	}

	if (renderer->exts.KHR_debug) {
		glEnable(GL_DEBUG_OUTPUT_KHR);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
		renderer->procs.glDebugMessageCallbackKHR(gles2_log, NULL);

		// Silence unwanted message types
		renderer->procs.glDebugMessageControlKHR(GL_DONT_CARE,
			GL_DEBUG_TYPE_POP_GROUP_KHR, GL_DONT_CARE, 0, NULL, GL_FALSE);
		renderer->procs.glDebugMessageControlKHR(GL_DONT_CARE,
			GL_DEBUG_TYPE_PUSH_GROUP_KHR, GL_DONT_CARE, 0, NULL, GL_FALSE);
	}

	push_gles2_debug(renderer);

	GLuint prog;
	renderer->shaders.quad.program = prog =
		link_program(renderer, common_vert_src, quad_frag_src);
	if (!renderer->shaders.quad.program) {
		goto error;
	}
	renderer->shaders.quad.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.quad.color = glGetUniformLocation(prog, "color");
	renderer->shaders.quad.pos_attrib = glGetAttribLocation(prog, "pos");

	renderer->shaders.tex_rgba.program = prog =
		link_program(renderer, common_vert_src, tex_rgba_frag_src);
	if (!renderer->shaders.tex_rgba.program) {
		goto error;
	}
	renderer->shaders.tex_rgba.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.tex_rgba.tex_proj = glGetUniformLocation(prog, "tex_proj");
	renderer->shaders.tex_rgba.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.tex_rgba.alpha = glGetUniformLocation(prog, "alpha");
	renderer->shaders.tex_rgba.pos_attrib = glGetAttribLocation(prog, "pos");

	renderer->shaders.tex_rgbx.program = prog =
		link_program(renderer, common_vert_src, tex_rgbx_frag_src);
	if (!renderer->shaders.tex_rgbx.program) {
		goto error;
	}
	renderer->shaders.tex_rgbx.proj = glGetUniformLocation(prog, "proj");
	renderer->shaders.tex_rgbx.tex_proj = glGetUniformLocation(prog, "tex_proj");
	renderer->shaders.tex_rgbx.tex = glGetUniformLocation(prog, "tex");
	renderer->shaders.tex_rgbx.alpha = glGetUniformLocation(prog, "alpha");
	renderer->shaders.tex_rgbx.pos_attrib = glGetAttribLocation(prog, "pos");

	if (renderer->exts.OES_egl_image_external) {
		renderer->shaders.tex_ext.program = prog =
			link_program(renderer, common_vert_src, tex_external_frag_src);
		if (!renderer->shaders.tex_ext.program) {
			goto error;
		}
		renderer->shaders.tex_ext.proj = glGetUniformLocation(prog, "proj");
		renderer->shaders.tex_ext.tex_proj = glGetUniformLocation(prog, "tex_proj");
		renderer->shaders.tex_ext.tex = glGetUniformLocation(prog, "tex");
		renderer->shaders.tex_ext.alpha = glGetUniformLocation(prog, "alpha");
		renderer->shaders.tex_ext.pos_attrib = glGetAttribLocation(prog, "pos");
	}

	pop_gles2_debug(renderer);

	wlr_egl_unset_current(renderer->egl);

	if (!init_upload_worker(&renderer->upload_worker, renderer->egl, loop)) {
		goto error;
	}

	return &renderer->wlr_renderer;

error:
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);

	pop_gles2_debug(renderer);

	if (renderer->exts.KHR_debug) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		renderer->procs.glDebugMessageCallbackKHR(NULL, NULL);
	}

	wlr_egl_unset_current(renderer->egl);

	free(renderer);
	return NULL;
}

bool wlr_gles2_renderer_check_ext(struct wlr_renderer *wlr_renderer,
		const char *ext) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return check_gl_ext(renderer->exts_str, ext);
}

GLuint wlr_gles2_renderer_get_current_fbo(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	assert(renderer->current_buffer);
	return renderer->current_buffer->fbo;
}
