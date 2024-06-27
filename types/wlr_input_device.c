
#include <stdlib.h>
#include <string.h>
#include <wlr/util/addon.h>
#include "interfaces/wlr_input_device.h"

void wlr_input_device_init(struct wlr_input_device *dev,
		enum wlr_input_device_type type, const char *name) {
	*dev = (struct wlr_input_device){
		.type = type,
		.name = strdup(name),
	};

	wlr_addon_set_init(&dev->addons);
	wl_signal_init(&dev->events.destroy);
}

void wlr_input_device_finish(struct wlr_input_device *wlr_device) {
	if (!wlr_device) {
		return;
	}

	wl_signal_emit_mutable(&wlr_device->events.destroy, wlr_device);

	wlr_addon_set_finish(&wlr_device->addons);
	wl_list_remove(&wlr_device->events.destroy.listener_list);

	free(wlr_device->name);
}
