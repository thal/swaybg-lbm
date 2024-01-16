#ifndef _LBM_H_
#define _LBM_H_
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct color_range {
    int low;
    int high;
    int rate;
    struct color_range *next;
};

// ARGB8888 in native byte order
typedef uint32_t color_register;

struct lbm_image {
    // Fields parsed from ILBM file
    unsigned int width;
    unsigned int height;
    color_register palette[256];
    // TODO: array instead of list?
    struct color_range *range;
    unsigned int n_ranges;
    uint8_t *pixels;

    // Array of the animation state of each range.
    // TODO: move to struct pixel_list?
    uint16_t *cycle_idxs;
    // Look up table for the pixels in a given range
    struct pixel_list *range_pixels;
};

struct pixel_list {
    // Number of pixels in this range
    size_t n_pixels;
    // A list of indices into the image data buffer in bytes
    unsigned int *pixels;
    // Bounding box of pixel range
    unsigned int min_x;
    unsigned int min_y;
    unsigned int max_x;
    unsigned int max_y;
    // If this range was affected by a call to cycle_palette.
    // Users should clear this after reading the updated values.
    bool damaged;
};

struct lbm_image *read_lbm_image(const char* path);
void free_lbm_image(struct lbm_image *image);

bool cycle_palette(struct lbm_image *anim);
void render_lbm_image( void **buffer, struct lbm_image *image, unsigned int width, unsigned int height, int scale);
#endif
