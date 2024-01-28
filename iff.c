#define _DEFAULT_SOURCE

#include "iff.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HDR_SIZE ID_SIZE + sizeof(int32_t)
#ifdef DEBUG_LBM
static int depth = 0;
#define PRINT_DEPTH()                 \
    for (int d = depth; d > 0; d--) { \
        printf("  ");                 \
    }
#endif

#ifdef DEBUG_LBM
const char *chunk_name[N_CHUNKS] = {"FORM", "BMHD", "CMAP", "CRNG", "BODY", "UNKNOWN"};
#endif

struct chunk *parse(const void *data, size_t size);

chunk_id get_id(const char *id) {
    if (strcmp(id, "FORM") == 0) {
        return FORM;
    } else if (strcmp(id, "BMHD") == 0) {
        return BMHD;
    } else if (strcmp(id, "CMAP") == 0) {
        return CMAP;
    } else if (strcmp(id, "CRNG") == 0) {
        return CRNG;
    } else if (strcmp(id, "BODY") == 0) {
        return BODY;
    } else {
        return UNKNOWN;
    }
}

void free_BODY(struct ck_BODY *c) { free(c->body); }

void free_chunk(struct chunk *c) {
    if (!c) {
        return;
    }

    struct chunk *child = c->child;
    while (child != NULL) {
        struct chunk *next = child->next;
        free_chunk(child);
        child = next;
    }
#ifdef DEBUG_LBM
    printf("Freeing chunk %s\n", chunk_name[c->id]);
#endif
    switch (c->id) {
        case BODY:
            free_BODY((struct ck_BODY *)c);
            // fall-through
        default:
            free(c);
    }
}

void list_add(struct chunk **head, struct chunk *new) {
    while (*head != NULL) {
        head = &((*head)->next);
    }
    *head = new;
}

#define PRINT_PROP(CHUNK, PROP) \
    PRINT_DEPTH()               \
    printf("" #PROP ": %d\n", (CHUNK)->PROP);

#define UWORD(var, src, rem)              \
    (var) = be16toh(*(uint16_t *)(data)); \
    (src) += sizeof(var);                 \
    (rem) -= sizeof(var);

#define WORD(var, src, rem)              \
    (var) = be16toh(*(int16_t *)(data)); \
    (src) += sizeof(var);                \
    (rem) -= sizeof(var);

#define UBYTE(var, src, rem)    \
    (var) = *(uint8_t *)(data); \
    (src) += sizeof(var);       \
    (rem) -= sizeof(var);

#define ID(var, src, rem)          \
    memcpy(var, src, sizeof(var)); \
    (src) += sizeof(var);          \
    (rem) -= sizeof(var);

struct chunk *parseCMAP(const void *data, size_t size) {
    assert(size == 768);  // Only support 256 color palettes
    struct ck_CMAP *ck = calloc(1, sizeof(struct ck_CMAP));
    ck->base.id = CMAP;
    ck->base.size = size;

    const int map_length = sizeof(ck->ColorMap) / sizeof(ck->ColorMap[0]);
    for (int i = 0; i < map_length; i++) {
        struct ck_CMAP_ColorRegister creg;
        UBYTE(creg.r, data, size);
        UBYTE(creg.g, data, size);
        UBYTE(creg.b, data, size);
        ck->ColorMap[i] = creg;
    }
#ifdef DEBUG_LBM
    PRINT_DEPTH() printf("ColorMap:\n");
    const int print_length = 5;
    for (int i = 0; i < print_length; i++) {
        PRINT_DEPTH()
        printf("  { r: %02x, g: %02x, b: %02x }\n", ck->ColorMap[i].r, ck->ColorMap[i].g,
               ck->ColorMap[i].b);
    }
    PRINT_DEPTH() printf("  ...%d additional entries...\n", map_length - print_length);
#endif
    return &ck->base;
}

struct chunk *parseBODY(const void *data, size_t size) {
    struct ck_BODY *ck = calloc(1, sizeof(struct ck_BODY));
    ck->base.id = BODY;
    ck->base.size = size;

    size_t alloc_size = size;
    ck->body = calloc(1, alloc_size);
    memcpy(ck->body, data, alloc_size);

#ifdef DEBUG_LBM
    PRINT_DEPTH() printf("Body:\n");
    const int print_length = 16;
    PRINT_DEPTH()
    for (int i = 0; i < print_length; i++) {
        printf("  %02x", ((uint8_t *)ck->body)[i]);
    }
    printf("\n");
    PRINT_DEPTH()
    printf("  ...%ld additional bytes...\n", alloc_size - print_length);
#endif

    return &ck->base;
}

struct chunk *parseCRNG(const void *data, size_t size) {
    struct ck_CRNG *ck = calloc(1, sizeof(struct ck_CRNG));
    ck->base.id = CRNG;
    ck->base.size = size;

    WORD(ck->pad1, data, size);
    WORD(ck->rate, data, size);
    WORD(ck->flags, data, size);
    UBYTE(ck->low, data, size);
    UBYTE(ck->high, data, size);

#ifdef DEBUG_LBM
    PRINT_PROP(ck, pad1);
    PRINT_PROP(ck, rate);
    PRINT_PROP(ck, flags);
    PRINT_PROP(ck, low);
    PRINT_PROP(ck, high);
#endif
    return &ck->base;
}

struct chunk *parse_BMHD(const void *data, size_t size) {
    struct ck_BMHD *ck = calloc(1, sizeof(struct ck_BMHD));
    ck->base.id = BMHD;
    ck->base.size = size;

    UWORD(ck->w, data, size);
    UWORD(ck->h, data, size);
    WORD(ck->x, data, size);
    WORD(ck->y, data, size);
    UBYTE(ck->nPlanes, data, size);
    UBYTE(ck->masking, data, size);
    UBYTE(ck->compression, data, size);
    UBYTE(ck->pad1, data, size);
    UWORD(ck->transparentColor, data, size);
    UBYTE(ck->xAspect, data, size);
    UBYTE(ck->yAspect, data, size);
    UWORD(ck->pageWidth, data, size);
    UWORD(ck->pageHeight, data, size);

#ifdef DEBUG_LBM
    PRINT_PROP(ck, w);
    PRINT_PROP(ck, h);
    PRINT_PROP(ck, x);
    PRINT_PROP(ck, y);
    PRINT_PROP(ck, nPlanes);
    PRINT_PROP(ck, masking);
    PRINT_PROP(ck, compression);
    PRINT_PROP(ck, pad1);
    PRINT_PROP(ck, transparentColor);
    PRINT_PROP(ck, xAspect);
    PRINT_PROP(ck, yAspect);
    PRINT_PROP(ck, pageWidth);
    PRINT_PROP(ck, pageHeight);
#endif
    return &ck->base;
}

// data is set to the beginning of the chunk data, after the chunk id and chunk size.
struct chunk *parse_FORM(const void *data, size_t size) {
    // FORM consists of a form type, then zero or more chunks
    struct ck_FORM *ck = calloc(1, sizeof(struct ck_FORM));
    ck->base.id = FORM;
    ck->base.size = size;

    ID(ck->formType, data, size);

#ifdef DEBUG_LBM
    PRINT_DEPTH()
    printf("FormType: %.4s\n", ck->formType);
#endif

    while (size > 1) {  // Acount for odd number of bytes, in which case there is a 0 padding byte at the end
        struct chunk *next = parse(data, size);
        size -= next->size + HDR_SIZE;
        data += next->size + HDR_SIZE;
        list_add(&ck->base.child, next);
#ifdef DEBUG_LBM
        PRINT_DEPTH()
        printf("%ld remaining bytes\n", size);
#endif
    }
    return &ck->base;
}

// data is set to the beginning of a chunk, before chunk id and size
struct chunk *parse(const void *const data, const size_t size) {
    const char *ckId = data;
    int32_t ckSize = be32toh(*(uint32_t *)(data + ID_SIZE));
    ckSize = ckSize % 2 ? ckSize + 1 : ckSize;  // All chunks are 2-byte aligned
    const void *chunkStart = data + ID_SIZE + sizeof(ckSize);

#ifdef DEBUG_LBM
    PRINT_DEPTH()
    printf("Chunk %.4s: size %d bytes\n", ckId, ckSize);
    depth++;
#endif

    struct chunk *c = NULL;
    chunk_id id = get_id(ckId);

    switch (id) {
        case FORM:
            c = parse_FORM(chunkStart, ckSize);
            break;
        case BMHD:
            c = parse_BMHD(chunkStart, ckSize);
            break;
        case CMAP:
            c = parseCMAP(chunkStart, ckSize);
            break;
        case CRNG:
            c = parseCRNG(chunkStart, ckSize);
            break;
        case BODY:
            c = parseBODY(chunkStart, ckSize);
            break;
        case UNKNOWN:
#ifdef DEBUG_LBM
            PRINT_DEPTH()
            printf("Skipping unknown chunk type \"%.4s\"\n", (const char *)data);
#endif
            c = calloc(1, sizeof(struct chunk));
            c->id = UNKNOWN;
            c->size = ckSize;
            break;
        default:
            break;
    }
#ifdef DEBUG_LBM
    depth--;
#endif
    return c;
}

struct chunk *read_iff_file(const char *path) {
    struct chunk *ret = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("Could not open %s: %s\n", path, strerror(errno));
        goto exit;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        printf("Could not stat file: %s\n", strerror(errno));
        goto exit;
    }
    const off_t size = sb.st_size;

    void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        goto exit;
    }

    ret = parse(data, size);

    munmap(data, size);
    close(fd);

exit:
    return ret;
}

#ifdef STANDALONE
int main(int argc, const char **argv) {
    if (argc > 1) {
        struct chunk *c = read_iff_file(argv[1]);
        if (c) {
            free_chunk(c);
        }
    } else {
        printf("No file specified\n");
        exit(1);
    }
}
#endif
