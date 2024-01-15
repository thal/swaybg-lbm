#include <assert.h>
#include "background-image.h"
#include "cairo_util.h"
#include "log.h"

enum background_mode parse_background_mode(const char *mode) {
	if (strcmp(mode, "stretch") == 0) {
		return BACKGROUND_MODE_STRETCH;
	} else if (strcmp(mode, "fill") == 0) {
		return BACKGROUND_MODE_FILL;
	} else if (strcmp(mode, "fit") == 0) {
		return BACKGROUND_MODE_FIT;
	} else if (strcmp(mode, "center") == 0) {
		return BACKGROUND_MODE_CENTER;
	} else if (strcmp(mode, "tile") == 0) {
		return BACKGROUND_MODE_TILE;
	} else if (strcmp(mode, "solid_color") == 0) {
		return BACKGROUND_MODE_SOLID_COLOR;
	}
	swaybg_log(LOG_ERROR, "Unsupported background mode: %s", mode);
	return BACKGROUND_MODE_INVALID;
}

cairo_surface_t *load_background_image(const char *path) {
	cairo_surface_t *image;
#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		swaybg_log(LOG_INFO, "Failed to load background image (%s).",
				err->message);
		return NULL;
	}
	image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
#else
	image = cairo_image_surface_create_from_png(path);
#endif // HAVE_GDK_PIXBUF
	if (!image) {
		swaybg_log(LOG_ERROR, "Failed to read background image.");
		return NULL;
	}
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to read background image: %s."
#if !HAVE_GDK_PIXBUF
				"\nSway was compiled without gdk_pixbuf support, so only"
				"\nPNG images can be loaded. This is the likely cause."
#endif // !HAVE_GDK_PIXBUF
				, cairo_status_to_string(cairo_surface_status(image)));
		return NULL;
	}
	return image;
}

struct animated_image *load_animated_background_image(const char *path) {

	struct animated_image *ret = calloc(1, sizeof(struct animated_image));

	if(!ret) {
		goto error;
	}

	// TODO: dont do this. Move struct animated_image fields into struct lbm_image instead. Allocate it all in one
	struct lbm_image *tmp = read_lbm_image(path);
	if(!tmp) {
		goto error;
	}
	// TODO: dont do this. Move struct animated_image fields into struct lbm_image instead. Allocate it all in one
	ret->lbm_image = *tmp;
	free(tmp);

	ret->cycle_idxs = calloc(ret->lbm_image.n_ranges, sizeof(ret->cycle_idxs[0]));
	if( ret->cycle_idxs == NULL ) {
		goto error;
	}
	// For each range, track the pixels that are affected
	ret->pixels_for_cycle = calloc(ret->lbm_image.n_ranges, sizeof(struct pixel_list));

	// count pixels in each range
	// allocate pixel lists
	// loop over all pixels again
	// fill pixel lists with offset
	// while we're at it, store the bounding box of the pixels in this range

	int i = 0;
	for(struct color_range* range = ret->lbm_image.range;
			range != NULL; range=range->next) {
		unsigned int pixels_in_range = 0;
		for(int row = 0; row < ret->lbm_image.height; row++) {
			for( int col = 0; col < ret->lbm_image.width; col++) {
				unsigned int p_index = row*ret->lbm_image.width + col;
				unsigned char p = ret->lbm_image.pixels[p_index];
				if( p >= range->low && p <= range->high ) {
					pixels_in_range++;
				}
			}
		}
		struct pixel_list *this_range = &ret->pixels_for_cycle[i];
		this_range->min_x = INT_MAX;
		this_range->min_y = INT_MAX;
		this_range->n_pixels = pixels_in_range;
		this_range->pixels = calloc(pixels_in_range, sizeof(unsigned int));

		unsigned int range_idx = 0;
		unsigned int* pixels = this_range->pixels;
		for(int row = 0; row < ret->lbm_image.height; row++) {
			for( int col = 0; col < ret->lbm_image.width; col++) {
				unsigned int p_index = row*ret->lbm_image.width + col;
				uint8_t p = ret->lbm_image.pixels[p_index];
				if( p >= range->low && p <= range->high ) {
					pixels[range_idx++] = p_index;
					if(row > this_range->max_y ) {
						this_range->max_y = row;
					}
					if(row < this_range->min_y ) {
						this_range->min_y = row;
					}
					if(col > this_range->max_x ) {
						this_range->max_x = col;
					}
					if(col < this_range->min_x ) {
						this_range->min_x = col;
					}
				}
			}
		}
		assert(range_idx == pixels_in_range);
		swaybg_log(LOG_DEBUG, "%s Range %d: %ld pixels, {%04d,%04d} to {%04d,%04d}",
				__FUNCTION__, i, this_range->n_pixels, this_range->min_x, this_range->min_y, this_range->max_x, this_range->max_y);
		i++;
	}
#if 0
	swaybg_log(LOG_DEBUG, "Loaded LBM image with %d ranges:\n", ret->lbm_image.n_ranges);
	for(struct color_range* range = ret->lbm_image.range;
			range != NULL; range=range->next)
	{
		swaybg_log(LOG_DEBUG, "%03d-%03d, rate %d (%.3f Hz)",
				range->low, range->high,
				range->rate, (float)(1<<14) / range->rate);
	}
	int pixcnt = 0;
	for(int row = 0; row < ret->lbm_image.height; row++) {
		for( int col = 0; col < ret->lbm_image.width; col++) {
			unsigned char p = ret->lbm_image.pixels[row*ret->lbm_image.width + col];
			for(struct color_range* range = ret->lbm_image.range;
					range != NULL; range=range->next) {
				if( p >= range->low && p <= range->high ) pixcnt++;
			}
		}
	}
	int pixtotal = ret->lbm_image.width * ret->lbm_image.height;
	swaybg_log(LOG_DEBUG, "%d/%d pixels shall cycle (%.3f%%)", pixcnt, pixtotal, ((float)pixcnt / pixtotal) *100);
#endif
	goto exit;

error:
	swaybg_log(LOG_ERROR, "Failed to open file %s, error: %s", path, strerror(errno));
	free(ret->cycle_idxs);
	free(ret);
	ret = NULL;
exit:
	return ret;
}

void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height) {
	double width = cairo_image_surface_get_width(image);
	double height = cairo_image_surface_get_height(image);

	cairo_save(cairo);
	switch (mode) {
	case BACKGROUND_MODE_STRETCH:
		cairo_scale(cairo,
				(double)buffer_width / width,
				(double)buffer_height / height);
		cairo_set_source_surface(cairo, image, 0, 0);
		break;
	case BACKGROUND_MODE_FILL: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		} else {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		}
		break;
	}
	case BACKGROUND_MODE_FIT: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		} else {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		}
		break;
	}
	case BACKGROUND_MODE_CENTER:
		cairo_set_source_surface(cairo, image,
				(double)buffer_width / 2 - width / 2,
				(double)buffer_height / 2 - height / 2);
		break;
	case BACKGROUND_MODE_TILE: {
		cairo_pattern_t *pattern = cairo_pattern_create_for_surface(image);
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
		cairo_set_source(cairo, pattern);
		cairo_pattern_destroy(pattern);
		break;
	}
	case BACKGROUND_MODE_SOLID_COLOR:
	case BACKGROUND_MODE_INVALID:
		assert(0);
		break;
	}
	cairo_paint(cairo);
	cairo_restore(cairo);
}

// Cycle all color ranges in the image.
// This is assumed to be called once per frame, and that the frame rate is 60Hz
// Return true if the contents of any pixels changed, and thus whether a new frame needs to be drawn
bool cycle_palette( struct animated_image *anim )
{
	static const uint16_t mod = 1 << 14;

	bool ret = false;
	int i = 0;
	for(struct color_range* range = anim->lbm_image.range;
			range != NULL; range=range->next) {
		// Increment each color range by its rate mod 2^14. If it overflows, perform the cycle
		uint16_t newidx = (anim->cycle_idxs[i] + range->rate) % mod;
		if(newidx < anim->cycle_idxs[i]) {
			color_register last = anim->lbm_image.palette[range->high];
			memmove( &anim->lbm_image.palette[range->low+1],
						&anim->lbm_image.palette[range->low],
						(range->high - range->low) * sizeof(anim->lbm_image.palette[0]) );
			anim->lbm_image.palette[range->low] = last;
			anim->pixels_for_cycle[i].damaged = true;
			ret = true;
		}
		anim->cycle_idxs[i] = newidx;
		i++;
	}
	return ret;
}

// Prepare a buffer that matches the size, layout, and format of the output's wl_buffer,
// so that it can be efficiently memcpy'd
void prepare_native_buffer( void **buffer,
							struct animated_image *src_img,
							int dst_width,
							int dst_height,
							int origin,
							int scale) {
	// Assumes 4 bytes per pixel, ARGB, little-endian
	void *dst_buf = calloc( dst_width * dst_height, 4 );
	const struct lbm_image *lbm_image = &src_img->lbm_image;
	assert(dst_buf);

	const int dst_stride = dst_width;
	uint32_t *row_start = dst_buf;
	for(int row = 0; row < dst_height; row++) {
		if( row / scale >= lbm_image->height) break;

		for( int col = 0; col < dst_width; col++) {
			if( col / scale >= lbm_image->width) break;

			unsigned char pixel = lbm_image->pixels[((row/scale) * src_img->lbm_image.width) + col/scale];
			uint32_t color = *(uint32_t*)(&lbm_image->palette[pixel]);
			row_start[col] = color;
			
		}
		row_start += dst_stride;
	}
	
	*buffer = dst_buf;
}
