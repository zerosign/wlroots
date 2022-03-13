#include <libudev.h>
#include <wayland-server-core.h>

#ifndef BACKEND_SESSION_DEV_UDEV_H
#define BACKEND_SESSION_DEV_UDEV_H

struct dev {
	struct udev *udev;
	struct udev_monitor *mon;
	struct wl_event_source *udev_event;
};

#endif
