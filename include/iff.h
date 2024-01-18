#ifndef _IFF_H_
#define _IFF_H_
#include <stddef.h>
#include <stdint.h>
#define ID_SIZE 4

struct chunk *read_iff_file(const char *path);
void free_chunk(struct chunk *c);

typedef enum { FORM, BMHD, CMAP, CRNG, BODY, UNKNOWN, N_CHUNKS } chunk_id;

struct chunk {
    chunk_id id;
    size_t size;  // useless?
    struct chunk *child;
    struct chunk *next;
};

struct ck_FORM {
    struct chunk base;
    char formType[ID_SIZE];
};

struct ck_BMHD {
    struct chunk base;
    uint16_t w;
    uint16_t h;
    int16_t x;
    int16_t y;
    uint8_t nPlanes;
    uint8_t masking;
    uint8_t compression;
    uint8_t pad1;
    uint16_t transparentColor;
    uint8_t xAspect;
    uint8_t yAspect;
    int16_t pageWidth;
    int16_t pageHeight;
};

struct ck_CMAP_ColorRegister {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct ck_CMAP {
    struct chunk base;
    struct ck_CMAP_ColorRegister ColorMap[256];
};

struct ck_CRNG {
    struct chunk base;
    int16_t pad1;
    int16_t rate;
    int16_t flags;
    uint8_t low;
    uint8_t high;
};

struct ck_BODY {
    struct chunk base;
    void *body;
};
#endif
