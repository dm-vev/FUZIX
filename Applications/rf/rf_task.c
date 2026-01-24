#include "rf_task.h"

#include "rf_draw.h"
#include "rf_keys.h"
#include "rf_term.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t rf_running = 1;

static void on_sig(int sig)
{
	(void)sig;
	rf_running = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	memset(&ts, 0, sizeof(ts));
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int rf_task_init(struct rf_task *t, int fb_mode, char *err, size_t errsz)
{
	if (!t) {
		snprintf(err, errsz, "rf: bad args");
		return -1;
	}
	memset(t, 0, sizeof(*t));

	t->active = 1;
	t->focus = RF_FOCUS_SPECTRUM;
	t->dirty = RF_DIRTY_ALL;

	if (rf_fb_open(&t->fb, fb_mode, err, errsz) != 0)
		return -1;

	t->cols = (int)t->fb.disp.width / RF_FONT_W;
	t->rows = (int)t->fb.disp.height / RF_FONT_H;
	t->main_rows = t->rows - RF_HEADER_ROWS - RF_STATUS_ROWS;
	if (t->cols <= 0 || t->rows <= 0 || t->main_rows <= 0) {
		snprintf(err, errsz, "rf: unsupported fb geometry %ux%u",
			 (unsigned)t->fb.disp.width, (unsigned)t->fb.disp.height);
		rf_fb_close(&t->fb);
		return -1;
	}

	char fb_err[128];
	if (rf_fb_activate(&t->fb, fb_err, sizeof(fb_err)) != 0) {
		snprintf(err, errsz, "%s", fb_err);
		rf_fb_close(&t->fb);
		return -1;
	}

	if (rf_term_setup_stdin(err, errsz) != 0) {
		rf_fb_close(&t->fb);
		return -1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	return 0;
}

int rf_task_run(struct rf_task *t)
{
	if (!t)
		return 1;

	/* Initial frame. */
	rf_draw_clear(t, rf_color_bg());
	rf_draw_header(t);
	rf_draw_status(t);
	rf_draw_present(t);
	t->dirty = 0;

	uint8_t inbuf[128];
	size_t inlen = 0;

	t->now_tick = now_ms();
	t->next_render_tick = t->now_tick;

	while (rf_running) {
		uint8_t tmp[32];
		ssize_t n = read(0, tmp, sizeof(tmp));
		if (n > 0) {
			if (inlen + (size_t)n > sizeof(inbuf))
				inlen = 0;
			memcpy(inbuf + inlen, tmp, (size_t)n);
			inlen += (size_t)n;
		}

		for (;;) {
			struct rf_key k;
			size_t consumed = 0;
			if (!rf_next_key(inbuf, inlen, &consumed, &k))
				break;
			if (consumed == 0 || consumed > inlen)
				break;
			memmove(inbuf, inbuf + consumed, inlen - consumed);
			inlen -= consumed;
			if (k.kind == RF_KEY_RUNE && (k.r == 'q' || k.r == 'Q'))
				rf_running = 0;
			t->dirty |= RF_DIRTY_STATUS;
		}

		uint64_t tick = now_ms();
		t->now_tick = tick;

		if (t->dirty && tick >= t->next_render_tick) {
			rf_draw_header(t);
			rf_draw_status(t);
			rf_draw_present(t);
			t->dirty = 0;
			t->next_render_tick = tick + 33;
		}

		if (inlen == 0)
			usleep(10000);
	}

	return 0;
}

void rf_task_destroy(struct rf_task *t)
{
	if (!t)
		return;
	rf_term_restore_stdin();
	rf_fb_close(&t->fb);
}

