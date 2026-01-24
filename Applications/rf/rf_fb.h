#ifndef RF_FB_H
#define RF_FB_H

#include <stddef.h>
#include <stdint.h>

#include <sys/graphics.h>

enum {
	RF_FB_MODE_TEXT = FB_MODE_TEXT,
	RF_FB_MODE_DIRECT = FB_MODE_DIRECT,
	RF_FB_MODE_MEMORY = FB_MODE_MEMORY,
};

struct rf_fb {
	int fd;
	int mode;
	struct display disp;
	int active;

	uint8_t *buf;
	size_t cap;
};

int rf_fb_open(struct rf_fb *fb, int mode, char *err, size_t errsz);
int rf_fb_activate(struct rf_fb *fb, char *err, size_t errsz);
void rf_fb_close(struct rf_fb *fb);

/* Returns pointer to payload (BGR888), or NULL on error. */
uint8_t *rf_fb_begin_box(struct rf_fb *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
int rf_fb_write_box(struct rf_fb *fb);
int rf_fb_flush(struct rf_fb *fb, const struct fb_rect *r);

#endif

