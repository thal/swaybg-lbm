#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H
#include <stdbool.h>
#include "cairo_util.h"
#include "lbm.h"

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

struct pixel_list {
	size_t n_pixels;	// Number of pixels in this range
	unsigned int *pixels; // A list of indices into the image data buffer in bytes
	int32_t min_x, min_y, max_x, max_y;
	bool damaged;			// If this range was affected by a call to cycle_palette.
							// Users should clear this after reading the updated values
};

struct animated_image {
	struct lbm_image lbm_image;
	uint16_t *cycle_idxs; // The position of each cycle as an array of 14-bit unsigned ints.
						  // Stored in the order of the list in struct image::range
	struct pixel_list *pixels_for_cycle; // List of pixels in each range
};

struct swaybg_image {
	struct wl_list link;
	const char *path;
	bool load_required;
	struct animated_image *anim;
};

enum background_mode parse_background_mode(const char *mode);
cairo_surface_t *load_background_image(const char *path);
struct animated_image *load_animated_background_image(const char *path);
void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height);
bool cycle_palette(struct animated_image *anim);
void prepare_native_buffer( void **buffer, struct animated_image *src_img, int width, int height, int origin, int scale);

#endif
