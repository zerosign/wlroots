#include <sys/types.h>
#include <wlr/backend/session.h>
#include <wayland-server-core.h>

#ifndef BACKEND_SESSION_DEV_H
#define BACKEND_SESSION_DEV_H

int dev_init(struct wlr_session *session, struct wl_display *display);
void dev_finish(struct wlr_session *session);

ssize_t dev_find_gpus(struct wlr_session *session, size_t ret_len,
		struct wlr_device **ret);
#endif
