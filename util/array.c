#include "util/array.h"
#include <assert.h>
#include <string.h>

void array_remove_at(struct wl_array *arr, size_t offset, size_t size) {
	assert(arr->size >= offset + size);

	char *data = arr->data;
	memmove(&data[offset], &data[offset + size], arr->size - offset - size);
	arr->size -= size;
}

bool array_realloc(struct wl_array *arr, size_t size) {
	// If the size is less than 1/4th of the allocation size, we shrink it.
	// 1/4th is picked to provide hysteresis, without which an array with size
	// arr->alloc would constantly reallocate if an element is added and then
	// removed continously.
	size_t alloc;
	if (arr->alloc > 0 && size > arr->alloc / 4) {
		alloc = arr->alloc;
	} else {
		alloc = 16;
	}

	while (alloc < size) {
		alloc *= 2;
	}

	if (alloc == arr->alloc) {
		return true;
	}

	void *data = realloc(arr->data, alloc);
	if (data == NULL) {
		return false;
	}
	arr->data = data;
	arr->alloc = alloc;
	return true;
}

void *array_reversed_start(struct wl_array *arr) {
	char *data = arr->data;
	return data + arr->alloc - arr->size;
}

void *array_reversed_add(struct wl_array *arr, size_t size) {
	if (arr->size + size > arr->alloc) {
		size_t new_alloc = arr->alloc * 2;
		char *new = malloc(new_alloc);
		if (!new) {
			return NULL;
		}

		memcpy(new + (new_alloc - arr->size), array_reversed_start(arr), arr->size);
		free(arr->data);
		arr->data = new;
		arr->alloc = new_alloc;
	}

	arr->size += size;
	return array_reversed_start(arr);
}
