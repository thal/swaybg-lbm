#include "lbm.h"

#ifdef DEBUG_LBM
#include <stdio.h>
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "iff.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

static void prepare_pixel_lists(struct lbm_image *image) {
    image->range_pixels = calloc(image->n_ranges, sizeof(struct pixel_list));

    // For each range, track the pixels that are affected
    // count pixels in each range
    // allocate pixel lists
    // loop over all pixels again
    // fill pixel lists with offset
    // while we're at it, store the bounding box of the pixels in this range

    for (unsigned int i = 0; i < image->n_ranges; i++) {
        struct color_range *range = &image->ranges[i];
        unsigned int pixels_in_range = 0;
        for (unsigned int row = 0; row < image->height; row++) {
            for (unsigned int col = 0; col < image->width; col++) {
                unsigned int p_index = row * image->width + col;
                unsigned char p = image->pixels[p_index];
                if (p >= range->low && p <= range->high) {
                    pixels_in_range++;
                }
            }
        }
        struct pixel_list *this_range = &image->range_pixels[i];
        this_range->bbox.min_x = INT_MAX;
        this_range->bbox.min_y = INT_MAX;
        this_range->n_pixels = pixels_in_range;
        this_range->pixels = calloc(pixels_in_range, sizeof(unsigned int));

        unsigned int range_idx = 0;
        unsigned int *pixels = this_range->pixels;
        for (int row = 0; row < (int)image->height; row++) {
            for (int col = 0; col < (int)image->width; col++) {
                unsigned int p_index = row * image->width + col;
                uint8_t p = image->pixels[p_index];
                if (p >= range->low && p <= range->high) {
                    pixels[range_idx++] = p_index;
                    this_range->bbox.min_x = MIN(this_range->bbox.min_x, col);
                    this_range->bbox.min_y = MIN(this_range->bbox.min_y, row);
                    this_range->bbox.max_x = MAX(this_range->bbox.max_x, col);
                    this_range->bbox.max_y = MAX(this_range->bbox.max_y, row);
                }
            }
        }
        assert(range_idx == pixels_in_range);
#ifdef DEBUG_LBM
        printf("%s Range %d: %ld pixels, {%04d,%04d} to {%04d,%04d}", __FUNCTION__, i,
               this_range->n_pixels, this_range->bbox.min_x, this_range->bbox.min_y, this_range->bbox.max_x,
               this_range->bbox.max_y);
#endif
    }
}

static void unpack(uint8_t *dest, const int8_t *src, const size_t size, const int compression) {
    if (compression == 0) {
        // compression value of 0 in the header means no compression.
        memcpy(dest, src, size);
    } else if (compression == 1) {
        // compression value of 1 means ByteRun1 Encoding from the ILBM specification
        size_t read = 0;
        size_t write = 0;
        while (write < size) {
            const int8_t n = src[read++];
            assert(n != -128);
            if (0 <= n) {
                memcpy(&dest[write], &src[read], n + 1);
                read += n + 1;
                write += n + 1;
            } else if (-127 <= n) {
                memset(&dest[write], src[read], -n + 1);
                read++;
                write += (-n + 1);
            }
        }
    }
}

void free_lbm_image(struct lbm_image *image) {
    if (image) {
        for (unsigned int i = 0; i < image->n_ranges; i++) {
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

    if (!c) {
        goto exit;
    }

    ret = calloc(1, sizeof(struct lbm_image));
    if (c->id == FORM) {
        struct ck_FORM *form = (struct ck_FORM *)c;
        struct chunk *child = form->base.child;

        void *body = NULL;
        int compression = 0;

        // loop through once to count the CRNGs
        while (child != NULL) {
            if (child->id == CRNG) {
                if (((struct ck_CRNG *)child)->rate > 0) {
                    ret->n_ranges++;
                }
            }
            child = child->next;
        }

        child = form->base.child;

        ret->ranges = calloc(ret->n_ranges, sizeof(struct color_range));
        unsigned int range_idx = 0;

        while (child != NULL) {
            if (child->id == BMHD) {
                struct ck_BMHD *bmhd = (struct ck_BMHD *)child;
                ret->width = bmhd->w;
                ret->height = bmhd->h;
                compression = bmhd->compression;

            } else if (child->id == CMAP) {
                static const int cmap_size = 256;
                color_register *palette = ret->palette;
                for (int i = 0; i < cmap_size; i++) {
                    struct ck_CMAP *cmap = (struct ck_CMAP *)child;
                    static const uint8_t a = 0xff;
                    color_register creg = a << 24 |
                        cmap->ColorMap[i].r << 16 |
                        cmap->ColorMap[i].g << 8  |
                        cmap->ColorMap[i].b;
                    palette[i] = creg;
                }
            } else if (child->id == CRNG) {
                struct ck_CRNG *crng = (struct ck_CRNG *)child;
                struct color_range *range = &ret->ranges[range_idx];
                if (crng->rate > 0) {
                    range->low = crng->low;
                    range->high = crng->high;
                    range->rate = crng->rate;
                    range_idx++;
                }
            } else if (child->id == BODY) {
                struct ck_BODY *body_chunk = (struct ck_BODY *)child;
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
bool cycle_palette(struct lbm_image *image) {
    static const uint16_t mod = 1 << 14;

    bool ret = false;
    for (unsigned int i = 0; i < image->n_ranges; i++) {
        struct color_range *range = &image->ranges[i];
        // Increment each color range by its rate mod 2^14. If it overflows, perform the cycle
        uint16_t newidx = (image->range_pixels[i].cycle_idx + range->rate) % mod;
        if (newidx < image->range_pixels[i].cycle_idx) {
            color_register last = image->palette[range->high];
            memmove(&image->palette[range->low + 1], &image->palette[range->low],
                    (range->high - range->low) * sizeof(color_register));
            image->palette[range->low] = last;
            image->range_pixels[i].damaged = true;
            ret = true;
        }
        image->range_pixels[i].cycle_idx = newidx;
    }
    if (ret) {
        image->frame_count++;
    }
    return ret;
}

// Render the image into a buffer at a given origin and (integer) scale factor.
// The visible area of the buffer is defined by dst_width and dst_height
// The resulting image after translating and scaling is clipped to the visible area of the buffer
void render_lbm_image(void *buffer, struct lbm_image *image, unsigned int dst_width,
                      unsigned int dst_height, int origin_x, int origin_y, int scale) {
    const int dst_stride = dst_width;
    uint32_t *row_start = buffer + (origin_y * dst_stride * sizeof(uint32_t));
    for (int row = origin_y; row < (int)dst_height; row++) {
        if ((row-origin_y) / scale >= (int)image->height) break;

        if ( row >= 0 ) {
            for (int col = origin_x; col < (int)dst_width; col++) {
                if ((col-origin_x) / scale >= (int)image->width) break;

                if( col >= 0 ) {
                    unsigned char pixel = image->pixels[(((row-origin_y) / scale) * image->width) + (col-origin_x) / scale];
                    uint32_t color = *(uint32_t *)(&image->palette[pixel]);
                    row_start[col] = color;
                }
            }
        }
        row_start += dst_stride;
    }
}

// Update the pixels in a buffer that have been damaged as a result of cycle_palette.
// Interpretation of the arguments is the same as render_lbm_image.
// Extent of damage (in dest. buffer coordinates) is returned through the damage out parameter.
// If no pixels were damaged, then damage->min_x is set to be greater than damage->max_x (and likewise for min_y, max_y)
// This clears the damaged flag of any affected pixel ranges
void render_delta(void *buffer, struct lbm_image *image, unsigned int dst_width,
                  unsigned int dst_height, int origin_x, int origin_y,
                  int scale, struct bounding_box *damage, bool clear) {

    damage->min_x = INT_MAX;
    damage->min_y = INT_MAX;
    damage->max_x = 0;
    damage->max_y = 0;

    const unsigned int dst_stride = dst_width;
    uint32_t *dst_buf = buffer;

    for (unsigned int i = 0; i < image->n_ranges; i++) {
        const struct pixel_list *range_pixels = &image->range_pixels[i];
        if (clear) {
            if (!range_pixels->damaged) {
                continue;
            } else {
                image->range_pixels[i].damaged = false;
            }
        }

        for (unsigned int list_idx = 0; list_idx < range_pixels->n_pixels; list_idx++) {
            const uint32_t pixel_idx = range_pixels->pixels[list_idx];
            const unsigned char pixel = image->pixels[pixel_idx];
            const uint32_t newcolor = *(uint32_t *)&image->palette[pixel];

            const unsigned int src_row = pixel_idx / image->width;
            const unsigned int src_col = pixel_idx % image->width;

            // Draw each pixel from the source image scale^2 times
            for (int square_y = 0; square_y < scale; square_y++) {
                unsigned int dst_idx =
                    (((src_row * scale) + (square_y + origin_y)) * dst_stride) +
                    ((src_col * scale) + origin_x);

                if (dst_idx >= dst_width * dst_height) {
                    continue;
                }

                // TODO: profile and see if there is any point unrolling this
                dst_buf[dst_idx] = newcolor;
                if (scale < 2) continue;
                dst_buf[dst_idx + 1] = newcolor;
                if (scale < 3) continue;
                dst_buf[dst_idx + 2] = newcolor;
                if (scale < 4) continue;
                dst_buf[dst_idx + 3] = newcolor;
            }
        }
        damage->max_x = MAX(damage->max_x, range_pixels->bbox.max_x);
        damage->max_y = MAX(damage->max_y, range_pixels->bbox.max_y);
        damage->min_x = MIN(damage->min_x, range_pixels->bbox.min_x);
        damage->min_y = MIN(damage->min_y, range_pixels->bbox.min_y);
    }

    // Damage bounding box is in source image coordinates. Transform it once at the end
    if (damage->min_y != INT_MAX) {
        damage->max_x *= scale;
        damage->max_y *= scale;
        damage->min_x *= scale;
        damage->min_y *= scale;

        damage->max_x += origin_x;
        damage->max_y += origin_y;
        damage->min_x += origin_x;
        damage->min_y += origin_y;

        // Account for the fact that dest. pixels are `scale` pixels wide and tall
        damage->max_x += scale;
        damage->max_y += scale;

        // Clip the result to the size of the destination
        damage->max_x = MIN(damage->max_x, (int)dst_width);
        damage->max_y = MIN(damage->max_y, (int)dst_height);
        damage->min_x = MAX(damage->min_x, 0);
        damage->min_y = MAX(damage->min_y, 0);
    }
}
