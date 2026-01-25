#include "rf_fb.h"

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

static int fb_ensure_cap(struct rf_fb *fb, size_t need)
{
	if (need <= fb->cap)
		return 0;

	size_t ncap = fb->cap ? fb->cap : 4096;
	while (ncap < need)
		ncap *= 2;

	uint8_t *nb = realloc(fb->buf, ncap);
	if (!nb)
		return -1;
	fb->buf = nb;
	fb->cap = ncap;
	return 0;
}

static int rf_fb_memory_roundtrip(struct rf_fb *fb, char *err, size_t errsz)
{
	if (!fb || fb->fd < 0 || !fb->active) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: bad args");
		return -1;
	}
	if (fb->mode != RF_FB_MODE_MEMORY) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: not in memory mode");
		return -1;
	}

	/* Tiny write+readback to validate the PSRAM path. */
	const uint16_t x = 0;
	const uint16_t y = 0;
	uint16_t w = fb->disp.width;
	if (!w) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: bad fb width");
		return -1;
	}
	/* Keep the test small (FUZIX user stacks are tiny). */
	if (w > 320)
		w = 320;
	const uint16_t h = 1;
	const size_t pbytes = (size_t)w * (size_t)h * 3u;

	uint8_t *payload = rf_fb_begin_box(fb, x, y, w, h);
	if (!payload) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: alloc");
		return -1;
	}
	for (size_t i = 0; i < pbytes; i++)
		payload[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 37u));
	if (rf_fb_write_box(fb) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: write: %s", strerror(errno));
		return -1;
	}

	if (ioctl(fb->fd, GFXIOC_READ, fb->buf) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "memtest: read: %s", strerror(errno));
		return -1;
	}
	for (size_t i = 0; i < pbytes; i++) {
		uint8_t exp = (uint8_t)(0xA5u ^ (uint8_t)(i * 37u));
		if (payload[i] != exp) {
			if (err && errsz)
				snprintf(err, errsz, "memtest: mismatch (PSRAM?)");
			return -1;
		}
	}
	return 0;
}

int rf_fb_open(struct rf_fb *fb, int mode, char *err, size_t errsz)
{
	struct display disp;

	if (!fb) {
		snprintf(err, errsz, "rf: fb: bad args");
		return -1;
	}
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	fb->mode = mode;
	fb->active = 0;
	fb->warn[0] = 0;

	fb->fd = open("/dev/fb", O_RDWR);
	if (fb->fd < 0) {
		snprintf(err, errsz, "rf: fb: open /dev/fb: %s", strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOC_LOCK, 0) < 0) {
		snprintf(err, errsz, "rf: fb: lock: %s", strerror(errno));
		rf_fb_close(fb);
		return -1;
	}

	memset(&disp, 0, sizeof(disp));
	disp.mode = (uint8_t)mode;
	if (ioctl(fb->fd, GFXIOC_GETMODE, &disp) < 0) {
		snprintf(err, errsz, "rf: fb: getmode: %s", strerror(errno));
		rf_fb_close(fb);
		return -1;
	}
	fb->disp = disp;

	if (fb->disp.format != FMT_BGR888) {
		snprintf(err, errsz, "rf: fb: unsupported pixel format %u", (unsigned)fb->disp.format);
		rf_fb_close(fb);
		return -1;
	}
	return 0;
}

int rf_fb_activate(struct rf_fb *fb, char *err, size_t errsz)
{
	if (!fb || fb->fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "rf: fb: bad args");
		return -1;
	}
	if (fb->active)
		return 0;

	struct display disp;
	memset(&disp, 0, sizeof(disp));
	disp.mode = (uint8_t)fb->mode;
	if (ioctl(fb->fd, GFXIOC_SETMODE, &disp) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "rf: fb: setmode: %s", strerror(errno));
		return -1;
	}
	fb->active = 1;

	if (fb->mode == RF_FB_MODE_MEMORY) {
		char test_err[96];
		if (rf_fb_memory_roundtrip(fb, test_err, sizeof(test_err)) != 0) {
			struct display direct;
			memset(&direct, 0, sizeof(direct));
			direct.mode = FB_MODE_DIRECT;
			if (ioctl(fb->fd, GFXIOC_SETMODE, &direct) == 0) {
				struct display direct_info;
				memset(&direct_info, 0, sizeof(direct_info));
				direct_info.mode = FB_MODE_DIRECT;
				if (ioctl(fb->fd, GFXIOC_GETMODE, &direct_info) == 0)
					fb->disp = direct_info;
				fb->mode = RF_FB_MODE_DIRECT;
				snprintf(fb->warn, sizeof(fb->warn), "FB:DIRECT (%s)", test_err);
			} else {
				if (err && errsz)
					snprintf(err, errsz, "%s; fallback direct failed: %s", test_err,
						 strerror(errno));
				return -1;
			}
		}
	}

	return 0;
}

void rf_fb_close(struct rf_fb *fb)
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
	fb->active = 0;
}

uint8_t *rf_fb_begin_box(struct rf_fb *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	if (!fb || fb->fd < 0 || !fb->active)
		return NULL;
	if (!w || !h)
		return NULL;

	size_t payload = (size_t)w * (size_t)h * 3;
	if (payload > (size_t)UINT16_MAX - 8u)
		return NULL;
	size_t total = sizeof(struct gfx_box) + payload;
	if (fb_ensure_cap(fb, total) != 0)
		return NULL;

	struct gfx_box *box = (struct gfx_box *)fb->buf;
	/*
	 * Kernel expects `size` to cover the 8-byte header + pixel payload,
	 * excluding this uint16_t field itself.
	 */
	box->size = (uint16_t)(8u + payload);
	box->y = y;
	box->x = x;
	box->h = h;
	box->w = w;
	return fb->buf + sizeof(*box);
}

int rf_fb_write_box(struct rf_fb *fb)
{
	if (!fb || fb->fd < 0 || !fb->active || !fb->buf)
		return -1;
	if (ioctl(fb->fd, GFXIOC_WRITE, fb->buf) < 0)
		return -1;
	return 0;
}

int rf_fb_flush(struct rf_fb *fb, const struct fb_rect *r)
{
	struct fb_rect rect;
	if (!fb || fb->fd < 0 || !fb->active)
		return -1;

	if (r)
		rect = *r;
	else
		memset(&rect, 0, sizeof(rect));

	if (ioctl(fb->fd, FBIOC_FLUSH, &rect) < 0)
		return -1;
	return 0;
}
