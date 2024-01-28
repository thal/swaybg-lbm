#ifndef _LBM_H_
#define _LBM_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct color_range {
    int low;
    int high;
    int rate;
};

// ARGB8888 in native byte order
typedef uint32_t color_register;

struct lbm_image {
    // Fields parsed from ILBM file
    unsigned int width;
    unsigned int height;
    color_register palette[256];
    struct color_range *ranges;
    unsigned int n_ranges;
    uint8_t *pixels;

    // Look up table for the pixels in a given range
    struct pixel_list *range_pixels;

    unsigned long frame_count;
    void *userdata;
};

struct bounding_box {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
};

struct pixel_list {
    // Number of pixels in this range
    size_t n_pixels;
    // A list of indices into the image data buffer in bytes
    unsigned int *pixels;
    // Bounding box of pixel range
    struct bounding_box bbox;
    // Progress through current step in the cycle
    uint16_t cycle_idx;
    // True if this range was affected by a call to cycle_palette.
    // Users should clear this after reading the updated values.
    bool damaged;
};

struct lbm_image *read_lbm_image(const char *path);
void free_lbm_image(struct lbm_image *image);

bool cycle_palette(struct lbm_image *anim);
void render_lbm_image(void *buffer, struct lbm_image *image, unsigned int width,
                      unsigned int height, int origin_x, int origin_y, int scale);
void render_delta(void *buffer, struct lbm_image *image, unsigned int dst_width,
                  unsigned int dst_height, int origin_x, int origin_y, int scale, struct bounding_box *damage, bool clear);
#endif
