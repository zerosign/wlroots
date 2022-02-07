#include <stdlib.h>
#include <wlr/types/wlr_configurable.h>

static void configure_destroy(struct wlr_configure *configure) {
	wlr_addon_set_finish(&configure->addons);
	wl_list_remove(&configure->link);
	free(configure);
}

static void send_configure(void *user_data) {
	struct wlr_configurable *configurable = user_data;
	configurable->event_idle = NULL;

	struct wlr_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		wl_resource_post_no_memory(configurable->resource);
		return;
	}

	configure->serial = configurable->next_serial;
	wlr_addon_set_init(&configure->addons);
	wl_list_insert(configurable->configures.prev, &configure->link);

	configurable->impl->configure(configurable, configure);
}

uint32_t wlr_configurable_schedule_configure(
		struct wlr_configurable *configurable) {
	if (configurable->event_idle == NULL) {
		struct wl_display *display = wl_client_get_display(
			wl_resource_get_client(configurable->resource));
		struct wl_event_loop *loop = wl_display_get_event_loop(display);
		configurable->next_serial = wl_display_next_serial(display);
		configurable->event_idle = wl_event_loop_add_idle(loop,
			send_configure, configurable);
		if (configurable->event_idle == NULL) {
			wl_resource_post_no_memory(configurable->resource);
		}
	}
	return configurable->next_serial;
}

void wlr_configurable_ack_configure(
		struct wlr_configurable *configurable, uint32_t serial) {
	bool found = false;

	struct wlr_configure *configure;
	wl_list_for_each(configure, &configurable->configures, link) {
		if (configure->serial == serial) {
			found = true;
			break;
		}
	}

	if (!found) {
		if (configurable->invalid_serial_error != -1) {
			wl_resource_post_error(configurable->resource,
				configurable->invalid_serial_error,
				"ack_configure serial %u doesn't "
				"match any configure serial", serial);
		}
		return;
	}

	struct wlr_configure *tmp;
	wl_list_for_each_safe(configure, tmp, &configurable->configures, link) {
		uint32_t configure_serial = configure->serial;
		configurable->impl->ack_configure(configurable, configure);
		configure_destroy(configure);
		if (configure_serial == serial) {
			break;
		}
	}
}

void wlr_configurable_init(struct wlr_configurable *configurable,
		const struct wlr_configurable_interface *impl,
		struct wl_resource *resource, int invalid_serial_error) {
	configurable->impl = impl;
	configurable->resource = resource;
	configurable->invalid_serial_error = invalid_serial_error;
	wl_list_init(&configurable->configures);
}

void wlr_configurable_finish(struct wlr_configurable *configurable) {
	struct wlr_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &configurable->configures, link) {
		configure_destroy(configure);
	}
	if (configurable->event_idle != NULL) {
		wl_event_source_remove(configurable->event_idle);
		configurable->event_idle = NULL;
	}
}
