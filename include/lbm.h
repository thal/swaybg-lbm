#ifndef _LBM_H_
#define _LBM_H_
#include <stdint.h>

struct color_range {
    int low;
    int high;
    int rate;
    struct color_range *next;
};

// ARGB8888 in native byte order
typedef uint32_t color_register;

struct lbm_image {
    int width;
    int height;
    color_register palette[256];
    struct color_range *range;
    int n_ranges;
    uint8_t *pixels;
};

struct lbm_image *read_lbm_image(const char* path);
void free_lbm_image(struct lbm_image *image);
#endif
