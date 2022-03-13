#define _POSIX_C_SOURCE 200809L
#include <demi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include "backend/session/session.h"
#include "backend/session/dev.h"
#include "backend/session/dev_demi.h"
#include "util/signal.h"

static bool is_drm_card(const char *devname) {
	const char prefix[] = DRM_PRIMARY_MINOR_NAME;
	const char *name = strrchr(devname);
	name = name ? name + 1 : devname;
	if (strncmp(name, prefix, strlen(prefix)) != 0) {
		return false;
	}
	for (size_t i = strlen(prefix); name[i] != '\0'; i++) {
		if (name[i] < '0' || name[i] > '9') {
			return false;
		}
	}
	return true;
}

static int handle_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;

	struct demi_event event;
	if (demi_read(fd, &event) == -1) {
		// TODO log
		return 1;
	}

	const char *devname = event.de_devname;
	enum demi_event_type event_type = event.de_type;

	if (event_type == DEMI_UNKNOWN) {
		// TODO log
		return 1;
	}

	if (!is_drm_card(devname)) {
		// TODO log
		return 1;
	}

	char devnode[sizeof("/dev/") + sizeof(event.de_devname)];
	snprintf(devnode, sizeof(devnode), "/dev/%s", devname);

	wlr_log(WLR_DEBUG, "kernel event for %s (code %d)", devnode, event_type);

	// TODO https://todo.sr.ht/~kennylevinsen/seatd/1
	const char *seat = "seat0";
	if (session->seat[0] != '\0' && strcmp(session->seat, seat) != 0) {
		return 1;
	}

	if (event_type == DEMI_ATTACH) {
		wlr_log(WLR_DEBUG, "DRM device %s added", devnode);
		struct wlr_session_add_event event = {
			.path = devnode,
		};
		wlr_signal_emit_safe(&session->events.add_drm_card, &event);
	} else if (event_type == DEMI_CHANGE || event_type == DEMI_DETACH) {
		struct stat st;
		// FIXME stat will fail on DEMI_DETACH
		if (stat(devnode, &st) == -1) {
			// FIXME fallback to comparing devnode
			return 1;
		}

		struct wlr_device *dev;
		wl_list_for_each(dev, &session->devices, link) {
			if (dev->dev != st.st_rdev) {
				continue;
			}

			if (event_type == DEMI_CHANGE) {
				wlr_log(WLR_DEBUG, "DRM device %s changed", devnode);
				// TODO
				// struct wlr_device_change_event event = {0};
				// read_udev_change_event(&event, udev_dev);
				wlr_signal_emit_safe(&dev->events.change, NULL);
			} else if (event_type == DEMI_DETACH) {
				wlr_log(WLR_DEBUG, "DRM device %s removed", devnode);
				wlr_signal_emit_safe(&dev->events.remove, NULL);
			} else {
				abort();
			}
			break;
		}
	}

	return 1;
}

int dev_init(struct wlr_session *session, struct wl_display *disp) {
	struct dev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return -1;
	}

	dev->fd = demi_init(DEMI_CLOEXEC | DEMI_NONBLOCK);
	if (dev->fd == -1) {
		wlr_log_errno(WLR_ERROR, "Failed to subscribe to kernel events");
		goto error_dev;
	}

	struct wl_event_loop *event_loop = wl_display_get_event_loop(disp);

	dev->event = wl_event_loop_add_fd(event_loop, dev->fd,
		WL_EVENT_READABLE, handle_event, session);
	if (!dev->event) {
		wlr_log_errno(WLR_ERROR, "Failed to create gpu hotplugging event source");
		goto error_fd;
	}

	session->dev = dev;
	return 0;

error_fd:
	close(dev->fd);
error_dev:
	free(dev);
	return -1;
}

void dev_finish(struct wlr_session *session) {
	if (!session) {
		return;
	}

	wl_event_source_remove(session->dev->event);
	close(session->dev->fd);
	free(session->dev);
}
