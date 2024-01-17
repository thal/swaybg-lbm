#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include "iff.h"
#include "lbm.h"

static void prepare_pixel_lists(struct lbm_image *image) {
	// For each range, track the pixels that are affected
	image->range_pixels = calloc(image->n_ranges, sizeof(struct pixel_list));

	// count pixels in each range
	// allocate pixel lists
	// loop over all pixels again
	// fill pixel lists with offset
	// while we're at it, store the bounding box of the pixels in this range

	for(unsigned int i = 0; i < image->n_ranges; i++) {
		struct color_range *range = &image->ranges[i];
		unsigned int pixels_in_range = 0;
		for(unsigned int row = 0; row < image->height; row++) {
			for(unsigned int col = 0; col < image->width; col++) {
				unsigned int p_index = row*image->width + col;
				unsigned char p = image->pixels[p_index];
				if( p >= range->low && p <= range->high ) {
					pixels_in_range++;
				}
			}
		}
		struct pixel_list *this_range = &image->range_pixels[i];
		this_range->min_x = INT_MAX;
		this_range->min_y = INT_MAX;
		this_range->n_pixels = pixels_in_range;
		this_range->pixels = calloc(pixels_in_range, sizeof(unsigned int));

		unsigned int range_idx = 0;
		unsigned int* pixels = this_range->pixels;
		for(unsigned int row = 0; row < image->height; row++) {
			for(unsigned int col = 0; col < image->width; col++) {
				unsigned int p_index = row*image->width + col;
				uint8_t p = image->pixels[p_index];
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
		printf("%s Range %d: %ld pixels, {%04d,%04d} to {%04d,%04d}",
				__FUNCTION__, i, this_range->n_pixels, this_range->min_x, this_range->min_y, this_range->max_x, this_range->max_y);
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
	return;
}

static void unpack(uint8_t *dest, const int8_t *src, const size_t size, const int compression) {
	if(compression == 0) {
		// compression value of 0 in the header means no compression.
		memcpy(dest, src, size);
	} else if(compression == 1) {
		// compression value of 1 means ByteRun1 Encoding from the ILBM specification
		size_t read = 0;
		size_t write = 0;
		while(write < size) {
			const int8_t n = src[read++];
			assert(n != -128);
			if(0 <= n) {
				memcpy(&dest[write], &src[read], n+1);
				read+=n+1;
				write+=n+1;
			}
			else if(-127 <= n) {
				memset(&dest[write], src[read], -n+1);
				read++;
				write+=(-n+1);
			}
		}
	}
}

void free_lbm_image(struct lbm_image *image) {
	if(image) {
		for(unsigned int i = 0; i < image->n_ranges; i++) {
			free(image->range_pixels[i].pixels);
		}
		free(image->ranges);
		free(image->range_pixels);
		free(image->pixels);
		free(image);
	}
}
struct lbm_image *read_lbm_image(const char *path) {
	struct lbm_image *ret = NULL;
	struct chunk *c = read_iff_file(path);

	if(!c) {
		goto exit;
	}

	ret = calloc(1, sizeof(struct lbm_image));
	if( c->id == FORM ) {
		struct ck_FORM *form = (struct ck_FORM*)c;
		struct chunk *child = form->base.child;

		void *body = NULL;
		int compression = 0;

		// loop through once to count the CRNGs
		while(child != NULL) {
			if(child->id == CRNG) {
				if(((struct ck_CRNG*)child)->rate > 0) {
					ret->n_ranges++;
				}
			}
			child = child->next;
		}

		child = form->base.child;

		ret->ranges = calloc(ret->n_ranges, sizeof(struct color_range));
		unsigned int range_idx = 0;

		while( child != NULL ) {
			if(child->id == BMHD) {
				struct ck_BMHD *bmhd = (struct ck_BMHD*)child;
				ret->width = bmhd->w;
				ret->height = bmhd->h;
				compression = bmhd->compression;

			} else if(child->id == CMAP) {
				static const int cmap_size = 256;
				color_register *palette = ret->palette;
				for(int i = 0; i<cmap_size; i++) {
					struct ck_CMAP *cmap = (struct ck_CMAP*)child;
					static const uint8_t a = 0xff;
					color_register creg = a << 24 |
						cmap->ColorMap[i].r << 16 |
						cmap->ColorMap[i].g << 8  |
						cmap->ColorMap[i].b;
					palette[i] = creg;
				}
			} else if(child->id == CRNG) {
				struct ck_CRNG *crng = (struct ck_CRNG*)child;
				struct color_range *range = &ret->ranges[range_idx];
				if(crng->rate > 0) {
					range->low = crng->low;
					range->high = crng->high;
					range->rate = crng->rate;
					range_idx++;
				}
			} else if(child->id == BODY) {
				struct ck_BODY *body_chunk = (struct ck_BODY*)child;
				body = body_chunk->body;
			}
			child = child->next;
		}

		size_t n_pixels = ret->width * ret->height;
		ret->pixels = calloc(n_pixels, sizeof(uint8_t));
		unpack(ret->pixels, body, n_pixels, compression);
		prepare_pixel_lists(ret);
	}
exit:
	free_chunk(c);
	return ret;
}

// Advance the animation of the color ranges in the image.
// This function should be called at rate of 60Hz for the rate of the animation to agree with the specification.
// Return true if the contents of any pixels changed, and thus whether a new frame needs to be drawn.
// This modifies the contents of struct lbm_image::palette
bool cycle_palette( struct lbm_image *image )
{
	static const uint16_t mod = 1 << 14;

	bool ret = false;
	for(unsigned int i = 0; i < image->n_ranges; i++) {
		struct color_range *range = &image->ranges[i];
		// Increment each color range by its rate mod 2^14. If it overflows, perform the cycle
		uint16_t newidx = (image->range_pixels[i].cycle_idx + range->rate) % mod;
		if(newidx < image->range_pixels[i].cycle_idx) {
			color_register last = image->palette[range->high];
			memmove( &image->palette[range->low+1],
						&image->palette[range->low],
						(range->high - range->low) * sizeof(color_register) );
			image->palette[range->low] = last;
			image->range_pixels[i].damaged = true;
			ret = true;
		}
		image->range_pixels[i].cycle_idx = newidx;
	}
	return ret;
}

// Render the image to a new buffer at a given (integer) scale factor.
// The caller is responsible for freeing the buffer.
void render_lbm_image( void **buffer,
							struct lbm_image *image,
							unsigned int dst_width,
							unsigned int dst_height,
							int scale) {
	// Assumes 4 bytes per pixel, ARGB, little-endian
	void *dst_buf = calloc( dst_width * dst_height, 4 );
	assert(dst_buf);

	const int dst_stride = dst_width;
	uint32_t *row_start = dst_buf;
	for(unsigned int row = 0; row < dst_height; row++) {
		if( row / scale >= image->height) break;

		for(unsigned int col = 0; col < dst_width; col++) {
			if( col / scale >= image->width) break;

			unsigned char pixel = image->pixels[((row/scale) * image->width) + col/scale];
			uint32_t color = *(uint32_t*)(&image->palette[pixel]);
			row_start[col] = color;
		}
		row_start += dst_stride;
	}
	*buffer = dst_buf;
}

