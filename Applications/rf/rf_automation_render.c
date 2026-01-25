#include "rf_automation_render.h"

#include "rf_automation.h"
#include "rf_draw.h"
#include "rf_task.h"

#include <stdio.h>
#include <string.h>

enum { automation_lines = 7 };

void rf_automation_render_overlay(const struct rf_task *t)
{
	if (!t)
		return;

	int box_cols = t->cols - 10;
	if (box_cols > 64)
		box_cols = 64;
	if (box_cols < 30)
		box_cols = 30;

	const int box_rows = 3 + automation_lines;

	int16_t px = (int16_t)(5 * RF_FONT_W);
	int16_t py = (int16_t)((RF_HEADER_ROWS + 3) * RF_FONT_H);
	int16_t pw = (int16_t)(box_cols * RF_FONT_W);
	int16_t ph = (int16_t)(box_rows * RF_FONT_H);

	struct rf_color border = rf_color_border();
	struct rf_color panel = rf_color_panel_bg();
	struct rf_color header = rf_color_header_bg();
	struct rf_color fg = rf_color_fg();
	struct rf_color dim = rf_color_dim();
	struct rf_color sel_fg = rf_color_sel_fg();
	struct rf_color sel_bg = rf_color_sel_bg();

	rf_draw_fill_rect(t, px, py, pw, ph, border);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(ph - 2), panel);
	rf_draw_fill_rect(t, (int16_t)(px + 1), (int16_t)(py + 1), (int16_t)(pw - 2), (int16_t)(RF_FONT_H + 1), header);
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + 1), "Automation (Esc close)", fg, header, box_cols);

	char status[96];
	rf_automation_status_line(t, t->now_tick, status, sizeof(status));
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + RF_FONT_H + 2), status, dim, panel, box_cols);

	char lines[automation_lines][96];
	memset(lines, 0, sizeof(lines));

	const char *arm = "OFF";
	if (t->auto_armed)
		arm = t->auto_started ? "RUN" : "ARM";
	snprintf(lines[0], sizeof(lines[0]), "ARM     <%s>", arm);
	snprintf(lines[1], sizeof(lines[1]), "START+ms [%d]", rf_clamp_int(t->auto_start_delay_ms, 0, 1000000));

	if (t->auto_duration_ms > 0)
		snprintf(lines[2], sizeof(lines[2]), "DURms   [%d]", rf_clamp_int(t->auto_duration_ms, 0, 1000000));
	else
		snprintf(lines[2], sizeof(lines[2]), "DURms   [off]");

	if (t->auto_stop_sweeps > 0)
		snprintf(lines[3], sizeof(lines[3]), "STOP_SW [%d]", rf_clamp_int(t->auto_stop_sweeps, 0, 1000000));
	else
		snprintf(lines[3], sizeof(lines[3]), "STOP_SW [off]");

	if (t->auto_stop_packets > 0)
		snprintf(lines[4], sizeof(lines[4]), "STOP_PK [%d]", rf_clamp_int(t->auto_stop_packets, 0, 1000000));
	else
		snprintf(lines[4], sizeof(lines[4]), "STOP_PK [off]");

	snprintf(lines[5], sizeof(lines[5]), "RECORD  <%s>", t->auto_record ? "ON" : "OFF");

	const char *name = t->auto_session_base[0] ? t->auto_session_base : "(auto)";
	snprintf(lines[6], sizeof(lines[6]), "NAME    <%s>", name);

	int16_t y0 = (int16_t)(py + 2 * RF_FONT_H + 2);
	for (int i = 0; i < automation_lines; i++) {
		int16_t yy = (int16_t)(y0 + (int16_t)i * RF_FONT_H);
		struct rf_color line_fg = fg;
		struct rf_color line_bg = panel;
		if (i == t->auto_sel) {
			line_fg = sel_fg;
			line_bg = sel_bg;
		}
		rf_draw_fill_rect(t, (int16_t)(px + 1), yy, (int16_t)(pw - 2), RF_FONT_H, line_bg);
		rf_draw_text(t, (int16_t)(px + 2), yy, lines[i], line_fg, line_bg, box_cols);
	}

	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + ph - RF_FONT_H - 1),
		     "Up/Down sel  Left/Right adj  Enter edit/toggle", dim, panel, box_cols);
}
