#define _POSIX_C_SOURCE 200809
#include <assert.h>
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "pool-buffer.h"

static int anonymous_shm_open(void) {
	int retries = 100;

	do {
		// try a probably-unique name
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		pid_t pid = getpid();
		char name[50];
		snprintf(name, sizeof(name), "/swaybg-%x-%x",
			(unsigned int)pid, (unsigned int)ts.tv_nsec);

		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}

		--retries;
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

extern struct wl_buffer_listener buffer_listener;

bool create_buffer(struct pool_buffer *buf, struct wl_shm *shm,
		int32_t width, int32_t height, uint32_t format, struct swaybg_output *output) {
	uint32_t stride = width * 4;
	size_t size = stride * height;

	int fd = anonymous_shm_open();
	assert(fd != -1);

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return false;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buf->buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, format);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->size = size;
	buf->data = data;
	buf->surface = cairo_image_surface_create_for_data(data,
			CAIRO_FORMAT_ARGB32, width, height, stride);
	buf->cairo = cairo_create(buf->surface);
	wl_buffer_add_listener(buf->buffer, &buffer_listener, output);
	return true;
}

void destroy_buffer(struct pool_buffer *buffer) {
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
	}
	if (buffer->cairo) {
		cairo_destroy(buffer->cairo);
	}
	if (buffer->surface) {
		cairo_surface_destroy(buffer->surface);
	}
	if (buffer->data) {
		munmap(buffer->data, buffer->size);
	}
	memset(buffer, 0, sizeof(struct pool_buffer));
}
