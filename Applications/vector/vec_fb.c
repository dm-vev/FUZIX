#include "vec_fb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct gfx_box {
	uint16_t size;
	uint16_t y;
	uint16_t x;
	uint16_t h;
	uint16_t w;
};

static int fb_ensure_cap(struct vec_fb *fb, size_t need)
{
	if (need <= fb->cap)
		return 0;
	size_t ncap = fb->cap ? fb->cap : 1024;
	while (ncap < need)
		ncap *= 2;
	uint8_t *nb = realloc(fb->buf, ncap);
	if (!nb)
		return -1;
	fb->buf = nb;
	fb->cap = ncap;
	return 0;
}

int vec_fb_open(struct vec_fb *fb, int mode, char *err, size_t errsz)
{
	struct display disp;

	if (!fb) {
		snprintf(err, errsz, "fb: bad args");
		return -1;
	}
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->mode = mode;
	fb->active = 0;

	fb->fd = open("/dev/fb", O_RDWR);
	if (fb->fd < 0) {
		snprintf(err, errsz, "fb: open /dev/fb: %s", strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOC_LOCK, 0) < 0) {
		snprintf(err, errsz, "fb: lock: errno=%d %s", errno, strerror(errno));
		vec_fb_close(fb);
		return -1;
	}

	memset(&disp, 0, sizeof(disp));
	disp.mode = (uint8_t)mode;
	/*
	 * Don't switch modes yet; query the mode info so the caller can compute
	 * geometry and prepare a first frame while the text console is still
	 * visible.
	 */
	if (ioctl(fb->fd, GFXIOC_GETMODE, &disp) < 0) {
		snprintf(err, errsz, "fb: getmode: errno=%d %s", errno, strerror(errno));
		vec_fb_close(fb);
		return -1;
	}
	fb->disp = disp;

	if (fb->disp.format != FMT_BGR888) {
		snprintf(err, errsz, "fb: unsupported format %u", (unsigned)fb->disp.format);
		vec_fb_close(fb);
		return -1;
	}
	return 0;
}

int vec_fb_activate(struct vec_fb *fb, char *err, size_t errsz)
{
	if (!fb || fb->fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "fb: bad args");
		return -1;
	}
	if (fb->active)
		return 0;

	struct display disp;
	memset(&disp, 0, sizeof(disp));
	disp.mode = (uint8_t)fb->mode;
	if (ioctl(fb->fd, GFXIOC_SETMODE, &disp) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "fb: set mode: errno=%d %s", errno, strerror(errno));
		return -1;
	}
	fb->active = 1;
	return 0;
}

void vec_fb_close(struct vec_fb *fb)
{
	struct display disp;

	if (!fb)
		return;
	if (fb->fd >= 0) {
		if (fb->active) {
			memset(&disp, 0, sizeof(disp));
			disp.mode = FB_MODE_TEXT;
			(void)ioctl(fb->fd, GFXIOC_SETMODE, &disp);
		}
		(void)ioctl(fb->fd, FBIOC_UNLOCK, 0);
		close(fb->fd);
		fb->fd = -1;
	}
	free(fb->buf);
	fb->buf = NULL;
	fb->cap = 0;
}

uint8_t *vec_fb_begin_box(struct vec_fb *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	if (!fb || fb->fd < 0)
		return NULL;
	if (!fb->active)
		return NULL;
	if (!w || !h)
		return NULL;
	size_t payload = (size_t)w * (size_t)h * 3;
	size_t total = sizeof(struct gfx_box) + payload;
	if (fb_ensure_cap(fb, total) != 0)
		return NULL;
	struct gfx_box *box = (struct gfx_box *)fb->buf;
	box->size = (uint16_t)total;
	box->y = y;
	box->x = x;
	box->h = h;
	box->w = w;
	return fb->buf + sizeof(*box);
}

int vec_fb_write_box(struct vec_fb *fb)
{
	if (!fb || fb->fd < 0 || !fb->buf)
		return -1;
	if (!fb->active)
		return -1;
	if (ioctl(fb->fd, GFXIOC_WRITE, fb->buf) < 0)
		return -1;
	return 0;
}

int vec_fb_flush(struct vec_fb *fb, const struct fb_rect *r)
{
	struct fb_rect rect;
	if (!fb || fb->fd < 0)
		return -1;
	if (!fb->active)
		return -1;
	if (r)
		rect = *r;
	else
		memset(&rect, 0, sizeof(rect));
	if (ioctl(fb->fd, FBIOC_FLUSH, &rect) < 0)
		return -1;
	return 0;
}
