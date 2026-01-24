#include "rf_task.h"

#include "rf_draw.h"
#include "rf_keys.h"
#include "rf_layout.h"
#include "rf_render.h"
#include "rf_term.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

	/* Spark defaults (task.go New()). */
	t->selected_channel = 37;
	t->channel_range_lo = 0;
	t->channel_range_hi = RF_MAX_CHANNEL;
	t->dwell_time_ms = 5;
	t->scan_speed_scalar = 1;
	t->data_rate = RF_RATE_2M;
	t->crc_mode = RF_CRC_2B;
	t->auto_ack = 0;
	t->power_level = RF_PWR_MAX;
	t->wf_palette = RF_WF_PAL_CYAN;
	t->rng = 0xA341316Cu;
	t->replay_speed = 1;
	t->menu_cat = RF_MENU_RF;

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

	t->now_tick = now_ms();
	t->next_render_tick = t->now_tick;

	/* Initial frame. */
	t->dirty = RF_DIRTY_ALL;
	rf_render_dirty(t);

	while (rf_running) {
		uint8_t tmp[32];
		ssize_t n = read(0, tmp, sizeof(tmp));
		if (n > 0) {
			if (t->inlen + (size_t)n > sizeof(t->inbuf))
				t->inlen = 0;
			memcpy(t->inbuf + t->inlen, tmp, (size_t)n);
			t->inlen += (size_t)n;
		}

		for (;;) {
			struct rf_key k;
			size_t consumed = 0;
			if (!rf_next_key(t->inbuf, t->inlen, &consumed, &k))
				break;
			if (consumed == 0 || consumed > t->inlen)
				break;
			memmove(t->inbuf, t->inbuf + consumed, t->inlen - consumed);
			t->inlen -= consumed;

			/* TODO: replace with full Spark key handling. */
			if (k.kind == RF_KEY_RUNE && (k.r == 'q' || k.r == 'Q'))
				rf_running = 0;
			t->dirty |= RF_DIRTY_STATUS;
		}

		uint64_t tick = now_ms();
		t->now_tick = tick;

		if (t->dirty && tick >= t->next_render_tick) {
			rf_render_dirty(t);
			t->next_render_tick = tick + RF_RENDER_INTERVAL_TICKS;
		}

		if (t->inlen == 0)
			usleep(10000);
	}

	return 0;
}

void rf_task_destroy(struct rf_task *t)
{
	if (!t)
		return;
	rf_term_restore_stdin();
	free(t->wf_buf);
	t->wf_buf = NULL;
	t->wf_cap = 0;
	free(t->record_buf);
	t->record_buf = NULL;
	t->record_cap = 0;
	t->record_len = 0;
	rf_fb_close(&t->fb);
}
