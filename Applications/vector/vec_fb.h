#ifndef VEC_FB_H
#define VEC_FB_H

#include <stdint.h>
#include <stddef.h>

#include <sys/graphics.h>

struct vec_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

struct vec_fb {
	int fd;
	int mode;
	struct display disp;

	uint8_t *buf;
	size_t cap;
};

int vec_fb_open(struct vec_fb *fb, int mode, char *err, size_t errsz);
void vec_fb_close(struct vec_fb *fb);

/* Returns a pointer to pixel payload (BGR888), or NULL on error. */
uint8_t *vec_fb_begin_box(struct vec_fb *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
int vec_fb_write_box(struct vec_fb *fb);

int vec_fb_flush(struct vec_fb *fb, const struct fb_rect *r);

#endif

