#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>

#include <wlr/backend/headless.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_keyboard.h>
#include "backend/backend.h"
#include "backend/multi.h"
#include "render/allocator/allocator.h"
#include "util/env.h"
#include "util/time.h"

#if WLR_HAS_SESSION
#include <wlr/backend/session.h>
#endif

#if WLR_HAS_DRM_BACKEND
#include <wlr/backend/drm.h>
#include "backend/drm/monitor.h"
#endif

#if WLR_HAS_LIBINPUT_BACKEND
#include <wlr/backend/libinput.h>
#endif

#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif

#define WAIT_SESSION_TIMEOUT 10000 // ms

void wlr_backend_init(struct wlr_backend *backend,
		const struct wlr_backend_impl *impl) {
	*backend = (struct wlr_backend){
		.impl = impl,
	};
	wl_signal_init(&backend->events.destroy);
	wl_signal_init(&backend->events.new_input);
	wl_signal_init(&backend->events.new_output);
}

void wlr_backend_finish(struct wlr_backend *backend) {
	wl_signal_emit_mutable(&backend->events.destroy, backend);
}

bool wlr_backend_start(struct wlr_backend *backend) {
	if (backend->impl->start) {
		return backend->impl->start(backend);
	}
	return true;
}

void wlr_backend_destroy(struct wlr_backend *backend) {
	if (!backend) {
		return;
	}

	if (backend->impl && backend->impl->destroy) {
		backend->impl->destroy(backend);
	} else {
		free(backend);
	}
}

static struct wlr_session *session_create_and_wait(struct wl_display *disp) {
#if WLR_HAS_SESSION
	struct wlr_session *session = wlr_session_create(disp);

	if (!session) {
		wlr_log(WLR_ERROR, "Failed to start a session");
		return NULL;
	}

	if (!session->active) {
		wlr_log(WLR_INFO, "Waiting for a session to become active");

		int64_t started_at = get_current_time_msec();
		int64_t timeout = WAIT_SESSION_TIMEOUT;
		struct wl_event_loop *event_loop =
			wl_display_get_event_loop(session->display);

		while (!session->active) {
			int ret = wl_event_loop_dispatch(event_loop, (int)timeout);
			if (ret < 0) {
				wlr_log_errno(WLR_ERROR, "Failed to wait for session active: "
					"wl_event_loop_dispatch failed");
				return NULL;
			}

			int64_t now = get_current_time_msec();
			if (now >= started_at + WAIT_SESSION_TIMEOUT) {
				break;
			}
			timeout = started_at + WAIT_SESSION_TIMEOUT - now;
		}

		if (!session->active) {
			wlr_log(WLR_ERROR, "Timeout waiting session to become active");
			return NULL;
		}
	}

	return session;
#else
	wlr_log(WLR_ERROR, "Cannot create session: disabled at compile-time");
	return NULL;
#endif
}

int wlr_backend_get_drm_fd(struct wlr_backend *backend) {
	if (!backend->impl->get_drm_fd) {
		return -1;
	}
	return backend->impl->get_drm_fd(backend);
}

uint32_t backend_get_buffer_caps(struct wlr_backend *backend) {
	if (!backend->impl->get_buffer_caps) {
		return 0;
	}

	return backend->impl->get_buffer_caps(backend);
}

static size_t parse_outputs_env(const char *name) {
	const char *outputs_str = getenv(name);
	if (outputs_str == NULL) {
		return 1;
	}

	char *end;
	int outputs = (int)strtol(outputs_str, &end, 10);
	if (*end || outputs < 0) {
		wlr_log(WLR_ERROR, "%s specified with invalid integer, ignoring", name);
		return 1;
	}

	return outputs;
}

// This code was copied from sway wm source code,
// from https://github.com/swaywm/sway/blob/7cf4e1d/sway/input/keyboard.c
// ____________________________________________________
static struct modifier_key {
	char *name;
	uint32_t mod;
} modifiers[] = {
	{ XKB_MOD_NAME_SHIFT, WLR_MODIFIER_SHIFT },
	{ XKB_MOD_NAME_CAPS , WLR_MODIFIER_CAPS  },
	{ XKB_MOD_NAME_CTRL , WLR_MODIFIER_CTRL  },
	{ "Ctrl"            , WLR_MODIFIER_CTRL  },
	{ XKB_MOD_NAME_ALT  , WLR_MODIFIER_ALT   },
	{ "Alt"             , WLR_MODIFIER_ALT   },
	{ XKB_MOD_NAME_NUM  , WLR_MODIFIER_MOD2  },
	{ "Mod3"            , WLR_MODIFIER_MOD3  },
	{ XKB_MOD_NAME_LOGO , WLR_MODIFIER_LOGO  },
	{ "Mod5"            , WLR_MODIFIER_MOD5  },
};

static uint32_t get_modifier_mask_by_name(const char *name) {
	int i;
	for (i = 0; i < (int)(sizeof(modifiers) / sizeof(struct modifier_key)); ++i) {
		if (strcasecmp(modifiers[i].name, name) == 0) {
			return modifiers[i].mod;
		}
	}

	return 0;
}
// ____________________________________________________)

static struct wlr_backend *attempt_wl_backend(struct wl_display *display) {
	struct wlr_backend *backend = wlr_wl_backend_create(display, NULL);
	if (backend == NULL) {
		return NULL;
	}

	size_t outputs = parse_outputs_env("WLR_WL_OUTPUTS");
	for (size_t i = 0; i < outputs; ++i) {
		wlr_wl_output_create(backend);
	}

	char* keyboard_shortcut = getenv("WLR_WL_GRAB_INPUT_SHORTCUT");
	if (keyboard_shortcut) {
		wlr_log(WLR_INFO, "Loading user-specified input grab keyboard shortcut: %s",
			keyboard_shortcut);
		xkb_keysym_t input_grab_keysym = 0;
		uint32_t input_grab_modifier_mask = 0;
		keyboard_shortcut = strdup(keyboard_shortcut);

		char *saveptr;
		char *key_name = strtok_r(keyboard_shortcut, "+", &saveptr);
		while (key_name  != NULL) {

			uint32_t modifier_mask = get_modifier_mask_by_name(key_name);
			input_grab_modifier_mask = input_grab_modifier_mask | modifier_mask;
			if(modifier_mask == 0) {
				input_grab_keysym = xkb_keysym_from_name(key_name,XKB_KEYSYM_CASE_INSENSITIVE);
				if(input_grab_keysym == 0) {
					wlr_log(WLR_ERROR,
							"The key shortcut contains an unrecognized key name. ignoring key shortcut");
					input_grab_keysym = 0;
					input_grab_modifier_mask = 0;
					free(keyboard_shortcut);
					return backend;
				}
			}
			key_name  = strtok_r(NULL, "+", &saveptr);
		}
	free(keyboard_shortcut);
	wlr_wl_backend_set_grab_input_shortcut(backend, input_grab_modifier_mask, input_grab_keysym);
	}

	return backend;
}

static struct wlr_backend *attempt_x11_backend(struct wl_display *display,
		const char *x11_display) {
#if WLR_HAS_X11_BACKEND
	struct wlr_backend *backend = wlr_x11_backend_create(display, x11_display);
	if (backend == NULL) {
		return NULL;
	}

	size_t outputs = parse_outputs_env("WLR_X11_OUTPUTS");
	for (size_t i = 0; i < outputs; ++i) {
		wlr_x11_output_create(backend);
	}

	return backend;
#else
	wlr_log(WLR_ERROR, "Cannot create X11 backend: disabled at compile-time");
	return NULL;
#endif
}

static struct wlr_backend *attempt_headless_backend(
		struct wl_display *display) {
	struct wlr_backend *backend = wlr_headless_backend_create(display);
	if (backend == NULL) {
		return NULL;
	}

	size_t outputs = parse_outputs_env("WLR_HEADLESS_OUTPUTS");
	for (size_t i = 0; i < outputs; ++i) {
		wlr_headless_add_output(backend, 1280, 720);
	}

	return backend;
}

static bool attempt_drm_backend(struct wl_display *display,
		struct wlr_backend *backend, struct wlr_session *session) {
#if WLR_HAS_DRM_BACKEND
	struct wlr_device *gpus[8];
	ssize_t num_gpus = wlr_session_find_gpus(session, 8, gpus);
	if (num_gpus < 0) {
		wlr_log(WLR_ERROR, "Failed to find GPUs");
		return false;
	}

	if (num_gpus == 0) {
		wlr_log(WLR_ERROR, "Found 0 GPUs, cannot create backend");
		return false;
	} else {
		wlr_log(WLR_INFO, "Found %zu GPUs", num_gpus);
	}

	struct wlr_backend *primary_drm = NULL;
	for (size_t i = 0; i < (size_t)num_gpus; ++i) {
		struct wlr_backend *drm = wlr_drm_backend_create(display, session,
			gpus[i], primary_drm);
		if (!drm) {
			wlr_log(WLR_ERROR, "Failed to create DRM backend");
			continue;
		}

		if (!primary_drm) {
			primary_drm = drm;
		}

		wlr_multi_backend_add(backend, drm);
	}
	if (!primary_drm) {
		wlr_log(WLR_ERROR, "Could not successfully create backend on any GPU");
		return NULL;
	}

	if (getenv("WLR_DRM_DEVICES") == NULL) {
		drm_backend_monitor_create(backend, primary_drm, session);
	}

	return true;
#else
	wlr_log(WLR_ERROR, "Cannot create DRM backend: disabled at compile-time");
	return false;
#endif
}

static struct wlr_backend *attempt_libinput_backend(struct wl_display *display,
		struct wlr_session *session) {
#if WLR_HAS_LIBINPUT_BACKEND
	return wlr_libinput_backend_create(display, session);
#else
	wlr_log(WLR_ERROR, "Cannot create libinput backend: disabled at compile-time");
	return NULL;
#endif
}

static bool attempt_backend_by_name(struct wl_display *display,
		struct wlr_backend *multi, char *name,
		struct wlr_session **session_ptr) {
	struct wlr_backend *backend = NULL;
	if (strcmp(name, "wayland") == 0) {
		backend = attempt_wl_backend(display);
	} else if (strcmp(name, "x11") == 0) {
		backend = attempt_x11_backend(display, NULL);
	} else if (strcmp(name, "headless") == 0) {
		backend = attempt_headless_backend(display);
	} else if (strcmp(name, "drm") == 0 || strcmp(name, "libinput") == 0) {
		// DRM and libinput need a session
		if (*session_ptr == NULL) {
			*session_ptr = session_create_and_wait(display);
			if (*session_ptr == NULL) {
				wlr_log(WLR_ERROR, "failed to start a session");
				return false;
			}
		}

		if (strcmp(name, "libinput") == 0) {
			backend = attempt_libinput_backend(display, *session_ptr);
		} else {
			// attempt_drm_backend() adds the multi drm backends itself
			return attempt_drm_backend(display, multi, *session_ptr);
		}
	} else {
		wlr_log(WLR_ERROR, "unrecognized backend '%s'", name);
		return false;
	}
	if (backend == NULL) {
		return false;
	}

	return wlr_multi_backend_add(multi, backend);
}

struct wlr_backend *wlr_backend_autocreate(struct wl_display *display,
		struct wlr_session **session_ptr) {
	if (session_ptr != NULL) {
		*session_ptr = NULL;
	}

	struct wlr_session *session = NULL;
	struct wlr_backend *multi = wlr_multi_backend_create(display);
	if (!multi) {
		wlr_log(WLR_ERROR, "could not allocate multibackend");
		return NULL;
	}

	char *names = getenv("WLR_BACKENDS");
	if (names) {
		wlr_log(WLR_INFO, "Loading user-specified backends due to WLR_BACKENDS: %s",
			names);

		names = strdup(names);
		if (names == NULL) {
			wlr_log(WLR_ERROR, "allocation failed");
			goto error;
		}

		char *saveptr;
		char *name = strtok_r(names, ",", &saveptr);
		while (name != NULL) {
			if (!attempt_backend_by_name(display, multi, name, &session)) {
				wlr_log(WLR_ERROR, "failed to add backend '%s'", name);
				free(names);
				goto error;
			}

			name = strtok_r(NULL, ",", &saveptr);
		}

		free(names);
		goto success;
	}

	if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET")) {
		struct wlr_backend *wl_backend = attempt_wl_backend(display);
		if (!wl_backend) {
			goto error;
		}

		wlr_multi_backend_add(multi, wl_backend);
		goto success;
	}

	const char *x11_display = getenv("DISPLAY");
	if (x11_display) {
		struct wlr_backend *x11_backend =
			attempt_x11_backend(display, x11_display);
		if (!x11_backend) {
			goto error;
		}

		wlr_multi_backend_add(multi, x11_backend);
		goto success;
	}

	// Attempt DRM+libinput
	session = session_create_and_wait(display);
	if (!session) {
		wlr_log(WLR_ERROR, "Failed to start a DRM session");
		goto error;
	}

	struct wlr_backend *libinput = attempt_libinput_backend(display, session);
	if (libinput) {
		wlr_multi_backend_add(multi, libinput);
	} else if (env_parse_bool("WLR_LIBINPUT_NO_DEVICES")) {
		wlr_log(WLR_INFO, "WLR_LIBINPUT_NO_DEVICES is set, "
			"starting without libinput backend");
	} else {
		wlr_log(WLR_ERROR, "Failed to start libinput backend");
		wlr_log(WLR_ERROR, "Set WLR_LIBINPUT_NO_DEVICES=1 to skip libinput");
		goto error;
	}

	if (!attempt_drm_backend(display, multi, session)) {
		wlr_log(WLR_ERROR, "Failed to open any DRM device");
		goto error;
	}

success:
	if (session_ptr != NULL) {
		*session_ptr = session;
	}
	return multi;

error:
	wlr_backend_destroy(multi);
#if WLR_HAS_SESSION
	wlr_session_destroy(session);
#endif
	return NULL;
}
