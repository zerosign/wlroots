#ifndef UTIL_ARRAY_H
#define UTIL_ARRAY_H

#include <stdlib.h>
#include <stdbool.h>
#include <wayland-util.h>

/**
 * Remove a chunk of memory of the specified size at the specified offset.
 */
void array_remove_at(struct wl_array *arr, size_t offset, size_t size);

/**
 * Grow or shrink the array to fit the specifized size.
 */
bool array_realloc(struct wl_array *arr, size_t size);

/**
 * Returns a pointer to the first valid element in a reversed array.
 */
void *array_reversed_start(struct wl_array *arr);

/**
 * Adds a new element to the array inserting them starting from a higher
 * memory address effectively inserting them in reverse order.
 */
void *array_reversed_add(struct wl_array *arr, size_t size);

#endif
