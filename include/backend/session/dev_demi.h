#include <wayland-server-core.h>

#ifndef BACKEND_SESSION_DEV_DEMI_H
#define BACKEND_SESSION_DEV_DEMI_H

struct dev {
	int fd;
	struct wl_event_source *event;
};

#endif
