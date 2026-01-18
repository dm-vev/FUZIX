#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <graphics.h>
#include <string.h>
#include "config.h"
#include "globals.h"

#define MANGLED 0
#include "mangle.h"
#include "lcdspi.h"
#define MANGLED 1
#include "mangle.h"

#define FB_WIDTH PSRAM_FB_WIDTH
#define FB_HEIGHT PSRAM_FB_HEIGHT
#define FB_BPP PSRAM_FB_BPP
#define FB_STRIDE_BYTES (FB_WIDTH * FB_BPP)
#define FB_SIZE_BYTES (FB_STRIDE_BYTES * FB_HEIGHT)

static uint8_t fb_mode = FB_MODE_TEXT;
static uint16_t fb_owner_pid;
static struct fb_rect fb_dirty;
static bool fb_dirty_valid;

static const struct display fb_text_mode = {
	FB_MODE_TEXT,
	FB_WIDTH, FB_HEIGHT,
	FB_WIDTH, FB_HEIGHT,
	0xFF, 0xFF,
	FMT_TEXT,
	HW_UNACCEL,
	GFX_TEXT,
	0,
	0,
	0, 0
};

static const struct display fb_direct_mode = {
	FB_MODE_DIRECT,
	FB_WIDTH, FB_HEIGHT,
	FB_WIDTH, FB_HEIGHT,
	0xFF, 0xFF,
	FMT_BGR888,
	HW_UNACCEL,
	0,
	0,
	GFX_READ | GFX_WRITE,
	0, 0
};

static const struct display fb_memory_mode = {
	FB_MODE_MEMORY,
	FB_WIDTH, FB_HEIGHT,
	FB_WIDTH, FB_HEIGHT,
	0xFF, 0xFF,
	FMT_BGR888,
	HW_UNACCEL,
	GFX_OFFSCREEN,
	0,
	GFX_READ | GFX_WRITE,
	0, 0
};

static struct display fb_get_mode(uint8_t mode)
{
	switch (mode) {
	case FB_MODE_DIRECT:
		return fb_direct_mode;
	case FB_MODE_MEMORY:
		return fb_memory_mode;
	case FB_MODE_TEXT:
	default:
		return fb_text_mode;
	}
}

static void fb_mark_dirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	if (!w || !h)
		return;
	if (!fb_dirty_valid) {
		fb_dirty.x = x;
		fb_dirty.y = y;
		fb_dirty.w = w;
		fb_dirty.h = h;
		fb_dirty_valid = true;
		return;
	}
	if (x < fb_dirty.x) {
		fb_dirty.w += fb_dirty.x - x;
		fb_dirty.x = x;
	}
	if (y < fb_dirty.y) {
		fb_dirty.h += fb_dirty.y - y;
		fb_dirty.y = y;
	}
	if (x + w > fb_dirty.x + fb_dirty.w)
		fb_dirty.w = (x + w) - fb_dirty.x;
	if (y + h > fb_dirty.y + fb_dirty.h)
		fb_dirty.h = (y + h) - fb_dirty.y;
}

static void fb_flush_rect(const struct fb_rect *r)
{
	uint8_t linebuf[FB_STRIDE_BYTES];
	uint16_t y;
	uint16_t w = r->w;
	uint16_t h = r->h;
	uint16_t x = r->x;
	uint16_t y0 = r->y;

	if (!w || !h)
		return;
	if (x >= FB_WIDTH || y0 >= FB_HEIGHT)
		return;
	if (x + w > FB_WIDTH)
		w = FB_WIDTH - x;
	if (y0 + h > FB_HEIGHT)
		h = FB_HEIGHT - y0;

	for (y = 0; y < h; y++) {
		uint32_t off = (uint32_t)(y0 + y) * FB_STRIDE_BYTES + (uint32_t)x * FB_BPP;
		psram_bus_read(off, linebuf, w * FB_BPP);
		lcd_draw_rect_bgr(x, y0 + y, w, 1, linebuf);
	}
}

static int fb_set_mode(uint8_t mode)
{
	if (mode > FB_MODE_MEMORY) {
		udata.u_error = EINVAL;
		return -1;
	}
	if (fb_owner_pid && fb_owner_pid != udata.u_ptab->p_pid) {
		udata.u_error = EBUSY;
		return -1;
	}
	if (mode == fb_mode)
		return 0;

	switch (mode) {
	case FB_MODE_TEXT:
		lcd_text_enable(true);
		lcd_clear();
		lcd_text_reset();
		break;
	case FB_MODE_DIRECT:
	case FB_MODE_MEMORY:
		lcd_text_enable(false);
		lcd_clear();
		break;
	default:
		break;
	}

	fb_mode = mode;
	return 0;
}

int fb_open(uint_fast8_t minor, uint16_t flags)
{
	used(flags);
	if (minor != 0) {
		udata.u_error = ENODEV;
		return -1;
	}
	psram_dev_init();
	return 0;
}

int fb_close(uint_fast8_t minor)
{
	used(minor);
	if (fb_owner_pid == udata.u_ptab->p_pid) {
		fb_owner_pid = 0;
		fb_set_mode(FB_MODE_TEXT);
	}
	return 0;
}

static int fb_do_rect_write(const struct fb_rect *r, const uint8_t *data)
{
	uint16_t y;
	uint16_t w = r->w;
	uint16_t h = r->h;
	uint16_t x = r->x;
	uint16_t y0 = r->y;

	if (!w || !h)
		return 0;
	if (x >= FB_WIDTH || y0 >= FB_HEIGHT || x + w > FB_WIDTH || y0 + h > FB_HEIGHT) {
		udata.u_error = EINVAL;
		return -1;
	}

	for (y = 0; y < h; y++) {
		uint32_t off = (uint32_t)(y0 + y) * FB_STRIDE_BYTES + (uint32_t)x * FB_BPP;
		const uint8_t *line = data + (uint32_t)y * w * FB_BPP;
		psram_bus_write(off, line, w * FB_BPP);
	}
	fb_mark_dirty(x, y0, w, h);
	return 0;
}

static int fb_do_rect_read(const struct fb_rect *r, uint8_t *data)
{
	uint16_t y;
	uint16_t w = r->w;
	uint16_t h = r->h;
	uint16_t x = r->x;
	uint16_t y0 = r->y;

	if (!w || !h)
		return 0;
	if (x >= FB_WIDTH || y0 >= FB_HEIGHT || x + w > FB_WIDTH || y0 + h > FB_HEIGHT) {
		udata.u_error = EINVAL;
		return -1;
	}

	for (y = 0; y < h; y++) {
		uint32_t off = (uint32_t)(y0 + y) * FB_STRIDE_BYTES + (uint32_t)x * FB_BPP;
		uint8_t *line = data + (uint32_t)y * w * FB_BPP;
		psram_bus_read(off, line, w * FB_BPP);
	}
	return 0;
}

int fb_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	uint8_t scratch[256];
	uint32_t off = udata.u_offset;
	uint16_t count = udata.u_count;
	uint16_t total;
	uint8_t *dst = (uint8_t *)udata.u_base;

	used(minor);
	used(rawflag);
	used(flag);

	if (fb_mode != FB_MODE_MEMORY) {
		udata.u_error = EINVAL;
		return -1;
	}
	if (off >= FB_SIZE_BYTES)
		return 0;
	if (off + count > FB_SIZE_BYTES)
		count = FB_SIZE_BYTES - off;
	total = count;

	while (count) {
		uint16_t chunk = count > sizeof(scratch) ? sizeof(scratch) : count;
		psram_bus_read(off, scratch, chunk);
		if (uput(scratch, dst, chunk))
			return -1;
		off += chunk;
		dst += chunk;
		count -= chunk;
	}
	return total;
}

int fb_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	uint8_t scratch[256];
	uint32_t off = udata.u_offset;
	uint16_t count = udata.u_count;
	uint16_t total;
	uint8_t *src = (uint8_t *)udata.u_base;

	used(minor);
	used(rawflag);
	used(flag);

	if (fb_mode != FB_MODE_MEMORY) {
		udata.u_error = EINVAL;
		return -1;
	}
	if (off >= FB_SIZE_BYTES)
		return 0;
	if (off + count > FB_SIZE_BYTES)
		count = FB_SIZE_BYTES - off;
	total = count;

	while (count) {
		uint16_t chunk = count > sizeof(scratch) ? sizeof(scratch) : count;
		if (uget(src, scratch, chunk))
			return -1;
		psram_bus_write(off, scratch, chunk);
		off += chunk;
		src += chunk;
		count -= chunk;
	}
	fb_mark_dirty(0, 0, FB_WIDTH, FB_HEIGHT);
	return total;
}

int fb_ioctl(uint_fast8_t minor, uarg_t request, char *ptr)
{
	uint16_t total;
	struct display disp;
	struct fb_rect rect;
	uint8_t header[8];
	uint8_t linebuf[FB_STRIDE_BYTES];
	uint32_t data_offset;
	uint32_t expected;
	uint16_t w;
	uint16_t h;
	uint16_t y;

	if (minor != 0) {
		udata.u_error = ENODEV;
		return -1;
	}

	switch (request) {
	case GFXIOC_GETINFO:
		disp = fb_get_mode(fb_mode);
		return uput(&disp, ptr, sizeof(disp));
	case GFXIOC_GETMODE:
	case GFXIOC_SETMODE:
	{
		int16_t mode = ugetc(ptr);
		if (mode < 0)
			return -1;
		if (request == GFXIOC_GETMODE) {
			disp = fb_get_mode((uint8_t)mode);
			return uput(&disp, ptr, sizeof(disp));
		}
		return fb_set_mode((uint8_t)mode);
	}
	case FBIOC_LOCK:
		if (fb_owner_pid && fb_owner_pid != udata.u_ptab->p_pid) {
			udata.u_error = EBUSY;
			return -1;
		}
		fb_owner_pid = udata.u_ptab->p_pid;
		return 0;
	case FBIOC_UNLOCK:
		if (fb_owner_pid && fb_owner_pid != udata.u_ptab->p_pid) {
			udata.u_error = EPERM;
			return -1;
		}
		fb_owner_pid = 0;
		return 0;
	case FBIOC_GETDIRTY:
		if (!fb_dirty_valid) {
			memset(&rect, 0, sizeof(rect));
			return uput(&rect, ptr, sizeof(rect));
		}
		return uput(&fb_dirty, ptr, sizeof(fb_dirty));
	case FBIOC_CLEARDIRTY:
		fb_dirty_valid = false;
		memset(&fb_dirty, 0, sizeof(fb_dirty));
		return 0;
	case FBIOC_FLUSH:
		if (fb_mode != FB_MODE_MEMORY) {
			udata.u_error = EINVAL;
			return -1;
		}
		if (uget(ptr, &rect, sizeof(rect)))
			return -1;
		if (!rect.w || !rect.h) {
			if (fb_dirty_valid)
				rect = fb_dirty;
			else {
				rect.x = 0;
				rect.y = 0;
				rect.w = FB_WIDTH;
				rect.h = FB_HEIGHT;
			}
		}
		fb_flush_rect(&rect);
		return 0;
	case GFXIOC_WRITE:
	case GFXIOC_READ:
		if (ugetw(ptr) < 8) {
			udata.u_error = EINVAL;
			return -1;
		}
		total = ugetw(ptr);
		if (uget(ptr + 2, header, sizeof(header)))
			return -1;
		rect.y = header[0] | (header[1] << 8);
		rect.x = header[2] | (header[3] << 8);
		rect.h = header[4] | (header[5] << 8);
		rect.w = header[6] | (header[7] << 8);
		if (!rect.w || !rect.h) {
			udata.u_error = EINVAL;
			return -1;
		}
		expected = 8 + (uint32_t)rect.w * rect.h * FB_BPP;
		if (total < expected) {
			udata.u_error = EINVAL;
			return -1;
		}
		data_offset = 2 + 8;
		w = rect.w;
		h = rect.h;
		if (request == GFXIOC_WRITE) {
			if (fb_mode == FB_MODE_DIRECT) {
				for (y = 0; y < h; y++) {
					uint32_t line_off = data_offset + (uint32_t)y * w * FB_BPP;
					if (uget(ptr + line_off, linebuf, w * FB_BPP))
						return -1;
					lcd_draw_rect_bgr(rect.x, rect.y + y, w, 1, linebuf);
				}
				return 0;
			}
			if (fb_mode == FB_MODE_MEMORY) {
				for (y = 0; y < h; y++) {
					uint32_t line_off = data_offset + (uint32_t)y * w * FB_BPP;
					if (uget(ptr + line_off, linebuf, w * FB_BPP))
						return -1;
					if (fb_do_rect_write(&(struct fb_rect){rect.x, rect.y + y, w, 1}, linebuf))
						return -1;
				}
				return 0;
			}
			udata.u_error = EINVAL;
			return -1;
		}
		if (fb_mode != FB_MODE_MEMORY) {
			udata.u_error = EINVAL;
			return -1;
		}
		for (y = 0; y < h; y++) {
			struct fb_rect line = {rect.x, rect.y + y, w, 1};
			if (fb_do_rect_read(&line, linebuf))
				return -1;
			if (uput(linebuf, ptr + data_offset + (uint32_t)y * w * FB_BPP, w * FB_BPP))
				return -1;
		}
		return 0;
	default:
		break;
	}

	udata.u_error = EINVAL;
	return -1;
}
