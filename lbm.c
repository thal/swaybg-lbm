#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "iff.h"
#include "lbm.h"

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
	struct color_range *head = image->range;
	while(head != NULL) {
		struct color_range *next = head->next;
		free(head);
		head = next;
	}
	free(image);
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

		struct color_range **tail = &ret->range;
		void *body = NULL;
		int compression = 0;

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
				ret->n_ranges++;
				struct ck_CRNG *crng = (struct ck_CRNG*)child;
				struct color_range *range = calloc(1, sizeof(struct color_range));
				if(crng->rate > 0) {
					range->low = crng->low;
					range->high = crng->high;
					range->rate = crng->rate;
					*tail = range;
					tail = &((*tail)->next);
				}
			} else if(child->id == BODY) {
				struct ck_BODY *body_chunk = (struct ck_BODY*)child;
				body = body_chunk->body;
			}
			child = child->next;
		}

		size_t n_pixels = ret->width * ret->height;
		ret->pixels = calloc(n_pixels,  sizeof(uint8_t));
		unpack(ret->pixels, body, n_pixels, compression);

	}
exit:
	return ret;
}

