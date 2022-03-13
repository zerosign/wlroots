#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <string.h>
#include <xf86drm.h>
#include "backend/session/dev.h"

// TODO move to util?
bool is_drm_card(const char *devname) {
	const char prefix[] = DRM_PRIMARY_MINOR_NAME;
	const char *name = strrchr(devname, '/');
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
