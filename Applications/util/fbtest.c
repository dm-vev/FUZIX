#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include <sys/graphics.h>

#include "termd_font8x8.h"

/* 8x8 bitmap font rendering (BGR888). */
#define FBTEST_FONT_W 8
#define FBTEST_FONT_H 8
#define FBTEST_LOG_CAP 2048

struct gfx_box {
	uint16_t size;
	uint16_t y;
	uint16_t x;
	uint16_t h;
	uint16_t w;
};

struct bgr {
	uint8_t b;
	uint8_t g;
	uint8_t r;
};

static volatile sig_atomic_t running = 1;

static void on_sig(int sig)
{
	(void)sig;
	running = 0;
}

struct vlog {
	char buf[FBTEST_LOG_CAP];
	size_t off;
	int enabled;
};

static void vlogf(struct vlog *l, const char *fmt, ...)
{
	if (!l || !l->enabled || l->off >= sizeof(l->buf))
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(l->buf + l->off, sizeof(l->buf) - l->off, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	size_t nn = (size_t)n;
	if (nn > sizeof(l->buf) - l->off)
		nn = sizeof(l->buf) - l->off;
	l->off += nn;
}

static int xioctl(int fd, int req, void *arg, const char *name, struct vlog *l)
{
	errno = 0;
	int rc = ioctl(fd, req, arg);
	int err = errno;
	vlogf(l, "ioctl %-12s req=0x%04x rc=%d errno=%d (%s)\n",
	      name ? name : "?", (unsigned)req, rc, err, strerror(err));
	return rc;
}

static uint64_t ts_to_ns(struct timespec t)
{
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void fill_bgr888(uint8_t *dst, size_t pixels, struct bgr c)
{
	size_t i;
	for (i = 0; i < pixels; i++) {
		*dst++ = c.b;
		*dst++ = c.g;
		*dst++ = c.r;
	}
}

static int write_solid_bands_bgr888(int fd, uint16_t width, uint16_t height, uint16_t band_h,
				    struct bgr c, uint8_t *buf, size_t cap, struct vlog *l)
{
	size_t payload_max = cap - sizeof(struct gfx_box);
	size_t payload_need = (size_t)width * (size_t)band_h * 3;
	if (payload_need > payload_max)
		return -1;

	struct gfx_box *box = (struct gfx_box *)buf;
	uint8_t *payload = buf + sizeof(*box);

	uint16_t y;
	for (y = 0; y < height; y = (uint16_t)(y + band_h)) {
		uint16_t h = band_h;
		if ((uint16_t)(y + h) > height)
			h = (uint16_t)(height - y);

		size_t pbytes = (size_t)width * (size_t)h * 3;
		size_t total = sizeof(*box) + pbytes;
		if (total > UINT16_MAX)
			return -1;

		box->size = (uint16_t)total;
		box->x = 0;
		box->y = y;
		box->w = width;
		box->h = h;

		fill_bgr888(payload, (size_t)width * (size_t)h, c);
		if (xioctl(fd, GFXIOC_WRITE, buf, "GFXIOC_WRITE", l) < 0) {
			vlogf(l, "  box size=%u x=%u y=%u w=%u h=%u\n",
			      (unsigned)box->size, (unsigned)box->x, (unsigned)box->y,
			      (unsigned)box->w, (unsigned)box->h);
			return -1;
		}
	}
	return 0;
}

static void put_px(uint8_t **dst, struct bgr c)
{
	uint8_t *p = *dst;
	*p++ = c.b;
	*p++ = c.g;
	*p++ = c.r;
	*dst = p;
}

static int draw_text_row_bgr888(int fd, uint16_t width, int row, const char *s, struct bgr fg,
				struct bgr bg, uint8_t *buf, size_t cap, struct vlog *l)
{
	if (row < 0)
		return -1;

	int cols = (int)width / FBTEST_FONT_W;
	if (cols <= 0)
		return -1;

	uint16_t w = (uint16_t)(cols * FBTEST_FONT_W);
	uint16_t y = (uint16_t)(row * FBTEST_FONT_H);

	size_t payload = (size_t)w * FBTEST_FONT_H * 3;
	size_t total = sizeof(struct gfx_box) + payload;
	if (total > cap || total > UINT16_MAX)
		return -1;

	struct gfx_box *box = (struct gfx_box *)buf;
	box->size = (uint16_t)total;
	box->x = 0;
	box->y = y;
	box->w = w;
	box->h = FBTEST_FONT_H;

	uint8_t *pix = buf + sizeof(*box);
	size_t slen = s ? strlen(s) : 0;
	int py, cx, bit;
	for (py = 0; py < FBTEST_FONT_H; py++) {
		uint8_t *dst = pix + (size_t)py * (size_t)w * 3;
		for (cx = 0; cx < cols; cx++) {
			unsigned char ch = ' ';
			if (slen && (size_t)cx < slen)
				ch = (unsigned char)s[cx];
			unsigned char glyph = termd_font8x8[ch * 8 + py];
			for (bit = 0; bit < FBTEST_FONT_W; bit++) {
				int on = (glyph & (0x80 >> bit)) != 0;
				put_px(&dst, on ? fg : bg);
			}
		}
	}

	return xioctl(fd, GFXIOC_WRITE, buf, "GFXIOC_WRITE", l) >= 0 ? 0 : -1;
}

static void usage(FILE *out)
{
	fprintf(out, "fbtest: fill framebuffer with solid colours and show FPS\n");
	fprintf(out, "usage: fbtest [-b band_lines] [-i /dev/ttyX|-] [-1] [-v] [-m direct|memory]\n");
	fprintf(out, "  -b N   band height in lines (default 16)\n");
	fprintf(out, "  -i DEV input device for quit key (default /dev/tty)\n");
	fprintf(out, "  -1     one-shot: show RGB bands then exit\n");
	fprintf(out, "  -v     verbose ioctl logging (printed after exit)\n");
	fprintf(out, "  -m M   mode: direct (default) or memory\n");
	fprintf(out, "exit: press 'q' (or Ctrl-C)\n");
}

static int set_raw_nonblock(int fd, struct termios *saved)
{
	if (fd < 0 || !saved)
		return -1;
	if (tcgetattr(fd, saved) != 0)
		return -1;
	{
		struct termios t = *saved;
		cfmakeraw(&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 1;
		if (tcsetattr(fd, TCSANOW, &t) != 0)
			return -1;
	}
	{
		int flags = fcntl(fd, F_GETFL);
		if (flags >= 0)
			(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int band = 16;
	int oneshot = 0;
	int verbose = 0;
	uint8_t mode = FB_MODE_DIRECT;
	const char *inpath = "/dev/tty";
	int infd = -1;
	struct termios in_saved;
	int in_have_saved = 0;
	int in_need_close = 0;
	struct vlog vlog;
	memset(&vlog, 0, sizeof(vlog));

	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		}
		if (!strcmp(argv[i], "-b")) {
			if (i + 1 >= argc) {
				usage(stderr);
				return 2;
			}
			band = atoi(argv[++i]);
			continue;
		}
		if (!strcmp(argv[i], "-i")) {
			if (i + 1 >= argc) {
				usage(stderr);
				return 2;
			}
			inpath = argv[++i];
			continue;
		}
		if (!strcmp(argv[i], "-1")) {
			oneshot = 1;
			continue;
		}
		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
			continue;
		}
		if (!strcmp(argv[i], "-m")) {
			if (i + 1 >= argc) {
				usage(stderr);
				return 2;
			}
			const char *m = argv[++i];
			if (!strcmp(m, "direct"))
				mode = FB_MODE_DIRECT;
			else if (!strcmp(m, "memory"))
				mode = FB_MODE_MEMORY;
			else {
				usage(stderr);
				return 2;
			}
			continue;
		}
		usage(stderr);
		return 2;
	}
	if (band <= 0)
		band = 1;
	vlog.enabled = verbose;

	int fd = open("/dev/fb", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "fbtest: open /dev/fb: errno=%d %s\n", errno, strerror(errno));
		return 1;
	}
	if (ioctl(fd, FBIOC_LOCK, 0) < 0) {
		fprintf(stderr, "fbtest: lock /dev/fb: errno=%d %s\n", errno, strerror(errno));
		close(fd);
		return 1;
	}

	struct display disp;
	memset(&disp, 0, sizeof(disp));
	disp.mode = mode;
	if (xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog) < 0) {
		fprintf(stderr, "fbtest: set mode: errno=%d %s\n", errno, strerror(errno));
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}
	if (xioctl(fd, GFXIOC_GETINFO, &disp, "GFXIOC_GETINFO", &vlog) < 0) {
		fprintf(stderr, "fbtest: getinfo: errno=%d %s\n", errno, strerror(errno));
		memset(&disp, 0, sizeof(disp));
		disp.mode = FB_MODE_TEXT;
		(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}
	if (disp.format != FMT_BGR888) {
		fprintf(stderr, "fbtest: unsupported format %u (need %u)\n",
			(unsigned)disp.format, (unsigned)FMT_BGR888);
		memset(&disp, 0, sizeof(disp));
		disp.mode = FB_MODE_TEXT;
		(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}

	uint16_t width = disp.width;
	uint16_t height = disp.height;
	if (!width || !height) {
		fprintf(stderr, "fbtest: bad size %ux%u\n", (unsigned)width, (unsigned)height);
		memset(&disp, 0, sizeof(disp));
		disp.mode = FB_MODE_TEXT;
		(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}

	if (((uint32_t)width * 3u + (uint32_t)sizeof(struct gfx_box)) > (uint32_t)UINT16_MAX) {
		fprintf(stderr, "fbtest: unsupported width %u (box too large)\n", (unsigned)width);
		memset(&disp, 0, sizeof(disp));
		disp.mode = FB_MODE_TEXT;
		(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}

	uint32_t denom = (uint32_t)width * 3u;
	uint32_t numer = (uint32_t)UINT16_MAX - (uint32_t)sizeof(struct gfx_box);
	uint16_t max_band = 1;
	if (denom != 0) {
		uint32_t mb = numer / denom;
		if (mb == 0)
			mb = 1;
		if (mb > UINT16_MAX)
			mb = UINT16_MAX;
		max_band = (uint16_t)mb;
	}
	uint16_t band_h = (uint16_t)band;
	if (band_h > max_band)
		band_h = max_band;

	size_t bufcap = sizeof(struct gfx_box) + (size_t)width * (size_t)band_h * 3;
	if (bufcap < sizeof(struct gfx_box) + (size_t)width * FBTEST_FONT_H * 3)
		bufcap = sizeof(struct gfx_box) + (size_t)width * FBTEST_FONT_H * 3;

	uint8_t *buf = malloc(bufcap);
	if (!buf) {
		fprintf(stderr, "fbtest: out of memory\n");
		memset(&disp, 0, sizeof(disp));
		disp.mode = FB_MODE_TEXT;
		(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
		(void)ioctl(fd, FBIOC_UNLOCK, 0);
		close(fd);
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	if (!strcmp(inpath, "-")) {
		infd = 0;
		in_need_close = 0;
	} else {
		infd = open(inpath, O_RDONLY | O_NOCTTY);
		in_need_close = 1;
	}
	if (infd >= 0) {
		if (set_raw_nonblock(infd, &in_saved) == 0)
			in_have_saved = 1;
	}

	static const struct bgr colors[] = {
		{ 0x00, 0x00, 0x00 }, /* black */
		{ 0x00, 0x00, 0xFF }, /* red */
		{ 0x00, 0xFF, 0x00 }, /* green */
		{ 0xFF, 0x00, 0x00 }, /* blue */
		{ 0x00, 0xFF, 0xFF }, /* yellow */
		{ 0xFF, 0x00, 0xFF }, /* magenta */
		{ 0xFF, 0xFF, 0x00 }, /* cyan */
		{ 0xFF, 0xFF, 0xFF }, /* white */
	};
	struct bgr fg = { 0xFF, 0xFF, 0xFF };
	struct bgr bg = { 0x00, 0x00, 0x00 };

	struct timespec t_last;
	clock_gettime(CLOCK_MONOTONIC, &t_last);
	uint64_t last_ns = ts_to_ns(t_last);
	unsigned long frames = 0;
	int fail = 0;
	char failmsg[96];
	failmsg[0] = 0;

	char overlay[64];
	snprintf(overlay, sizeof(overlay), "fbtest %ux%u band=%u (q=exit)", (unsigned)width, (unsigned)height,
		 (unsigned)band_h);
	(void)draw_text_row_bgr888(fd, width, 0, overlay, fg, bg, buf, bufcap, &vlog);

	size_t ncolors = sizeof(colors) / sizeof(colors[0]);
	struct fb_rect rect;
	memset(&rect, 0, sizeof(rect));

	if (oneshot) {
		/* Show a simple RGB sequence (each for ~1s) then return to text mode. */
		static const size_t seq[] = { 1, 2, 3 }; /* red, green, blue */
		for (size_t si = 0; si < (sizeof(seq) / sizeof(seq[0])) && running; si++) {
			struct bgr c = colors[seq[si]];
			if (write_solid_bands_bgr888(fd, width, height, band_h, c, buf, bufcap, &vlog) != 0) {
				fail = errno ? errno : EIO;
				snprintf(failmsg, sizeof(failmsg), "fbtest: write failed: errno=%d %s",
					 fail, strerror(fail));
				break;
			}
			if (mode == FB_MODE_MEMORY) {
				if (xioctl(fd, FBIOC_FLUSH, &rect, "FBIOC_FLUSH", &vlog) < 0) {
					fail = errno ? errno : EIO;
					snprintf(failmsg, sizeof(failmsg), "fbtest: flush failed: errno=%d %s",
						 fail, strerror(fail));
					break;
				}
			}
			sleep(1);
		}
		goto out;
	}

	while (running) {
		char ch;
		if (infd >= 0) {
			if (read(infd, &ch, 1) > 0) {
				if (ch == 'q' || ch == 'Q')
					break;
			}
		}

		struct bgr c = colors[frames % ncolors];
		if (write_solid_bands_bgr888(fd, width, height, band_h, c, buf, bufcap, &vlog) != 0) {
			fail = errno ? errno : EIO;
			snprintf(failmsg, sizeof(failmsg), "fbtest: write failed: errno=%d %s",
				 fail, strerror(fail));
			break;
		}
		if (mode == FB_MODE_MEMORY) {
			if (xioctl(fd, FBIOC_FLUSH, &rect, "FBIOC_FLUSH", &vlog) < 0) {
				fail = errno ? errno : EIO;
				snprintf(failmsg, sizeof(failmsg), "fbtest: flush failed: errno=%d %s",
					 fail, strerror(fail));
				break;
			}
		}

		frames++;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t now_ns = ts_to_ns(now);
		uint64_t delta_ns = now_ns - last_ns;
			if (delta_ns >= 1000000000ull) {
				double fps = (double)frames * (1000000000.0 / (double)delta_ns);
				snprintf(overlay, sizeof(overlay), "FPS: %lu (%.2f)  %ux%u  band=%u  (key=exit)",
					 frames, fps, (unsigned)width, (unsigned)height, (unsigned)band_h);
				(void)draw_text_row_bgr888(fd, width, 0, overlay, fg, bg, buf, bufcap, &vlog);
				if (mode == FB_MODE_MEMORY)
					(void)xioctl(fd, FBIOC_FLUSH, &rect, "FBIOC_FLUSH", &vlog);

			frames = 0;
			last_ns = now_ns;
		}
	}

out:
	free(buf);
	memset(&disp, 0, sizeof(disp));
	disp.mode = FB_MODE_TEXT;
	(void)xioctl(fd, GFXIOC_SETMODE, &disp, "GFXIOC_SETMODE", &vlog);
	(void)ioctl(fd, FBIOC_UNLOCK, 0);
	close(fd);
	if (infd >= 0 && in_have_saved)
		(void)tcsetattr(infd, TCSANOW, &in_saved);
	if (in_need_close && infd >= 0)
		close(infd);
	if (verbose && vlog.off) {
		fwrite(vlog.buf, 1, vlog.off, stderr);
		if (vlog.buf[vlog.off - 1] != '\n')
			fputc('\n', stderr);
	}
	if (failmsg[0]) {
		fprintf(stderr, "%s\n", failmsg);
		return 1;
	}
	return 0;
}
