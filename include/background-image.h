#ifndef _SWAY_BACKGROUND_IMAGE_H
#define _SWAY_BACKGROUND_IMAGE_H
#include <stdbool.h>
#include "cairo_util.h"
#include "imagelbm.h"

enum background_mode {
	BACKGROUND_MODE_STRETCH,
	BACKGROUND_MODE_FILL,
	BACKGROUND_MODE_FIT,
	BACKGROUND_MODE_CENTER,
	BACKGROUND_MODE_TILE,
	BACKGROUND_MODE_SOLID_COLOR,
	BACKGROUND_MODE_INVALID,
};

struct animated_image {
	struct image lbm_image;
	uint16_t *cycle_idxs;
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
bool cycle_palette(struct animated_image* anim);

#endif
