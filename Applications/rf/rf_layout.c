#include "rf_layout.h"

#include "rf_draw.h"
#include "rf_task.h"

/*
 * Port of Spark rfanalyzer computeLayout() (render.go).
 * The layout is in pixels, but uses font cell multiples for panel widths/heights.
 */
struct rf_layout rf_compute_layout(const struct rf_task *t)
{
	if (!t)
		return (struct rf_layout){0};

	const int16_t w = (int16_t)t->fb.disp.width;
	const int16_t h = (int16_t)t->fb.disp.height;

	const int16_t header_h = (int16_t)RF_HEADER_ROWS * RF_FONT_H;
	const int16_t status_h = (int16_t)RF_STATUS_ROWS * RF_FONT_H;
	const int16_t main_y = header_h;

	int left_cols = 34;
	if (left_cols < 20)
		left_cols = 20;
	if (left_cols > t->cols - 10) {
		left_cols = t->cols - 10;
		if (left_cols < 20)
			left_cols = t->cols / 2;
	}
	const int right_cols = t->cols - left_cols;
	const int16_t left_w = (int16_t)left_cols * RF_FONT_W;
	const int16_t right_w = (int16_t)(w - left_w);

	int analysis_cols = 28;
	if (analysis_cols > right_cols - 24)
		analysis_cols = right_cols / 3;
	if (analysis_cols < 20)
		analysis_cols = 20;
	int right_main_cols = right_cols - analysis_cols;
	if (right_main_cols < 24) {
		analysis_cols = 0;
		right_main_cols = right_cols;
	}
	const int16_t right_main_w = (int16_t)right_main_cols * RF_FONT_W;
	const int16_t analysis_w = (int16_t)(right_w - right_main_w);

	int spectrum_rows = 16;
	if (spectrum_rows > t->main_rows - 8)
		spectrum_rows = t->main_rows / 3;
	if (spectrum_rows < 10)
		spectrum_rows = 10;

	int waterfall_rows = t->main_rows - spectrum_rows;
	if (waterfall_rows < 8) {
		waterfall_rows = 8;
		spectrum_rows = t->main_rows - waterfall_rows;
	}

	int control_rows = 16;
	int sniffer_rows = 19;
	if (control_rows + sniffer_rows > t->main_rows - 8) {
		control_rows = t->main_rows / 3;
		sniffer_rows = t->main_rows / 3;
	}
	if (control_rows < 10)
		control_rows = 10;
	if (sniffer_rows < 10)
		sniffer_rows = 10;
	int proto_rows = t->main_rows - control_rows - sniffer_rows;
	if (proto_rows < 8) {
		proto_rows = 8;
		if (control_rows + sniffer_rows + proto_rows > t->main_rows)
			sniffer_rows = t->main_rows - control_rows - proto_rows;
	}

	const struct rf_rect menu = {.x = 0, .y = 0, .w = w, .h = RF_FONT_H};
	const struct rf_rect toolbar = {.x = 0, .y = RF_FONT_H, .w = w, .h = RF_FONT_H};
	const struct rf_rect status1 = {.x = 0, .y = (int16_t)(h - status_h), .w = w, .h = RF_FONT_H};
	const struct rf_rect status2 = {.x = 0, .y = (int16_t)(h - status_h + RF_FONT_H), .w = w, .h = RF_FONT_H};

	const struct rf_rect spectrum = {.x = 0, .y = main_y, .w = left_w, .h = (int16_t)spectrum_rows * RF_FONT_H};
	const struct rf_rect waterfall = {.x = 0, .y = (int16_t)(main_y + spectrum.h), .w = left_w, .h = (int16_t)waterfall_rows * RF_FONT_H};

	const struct rf_rect rf = {.x = left_w, .y = main_y, .w = right_main_w, .h = (int16_t)control_rows * RF_FONT_H};
	const struct rf_rect sniffer = {.x = left_w, .y = (int16_t)(rf.y + rf.h), .w = right_main_w, .h = (int16_t)sniffer_rows * RF_FONT_H};
	const struct rf_rect proto = {.x = left_w, .y = (int16_t)(sniffer.y + sniffer.h), .w = right_main_w, .h = (int16_t)proto_rows * RF_FONT_H};
	const struct rf_rect analysis = {.x = (int16_t)(left_w + right_main_w), .y = main_y, .w = analysis_w, .h = (int16_t)t->main_rows * RF_FONT_H};

	return (struct rf_layout){
		.menu = menu,
		.toolbar = toolbar,
		.status1 = status1,
		.status2 = status2,
		.spectrum = spectrum,
		.waterfall = waterfall,
		.rf = rf,
		.sniffer = sniffer,
		.proto = proto,
		.analysis = analysis,
		.left_cols = left_cols,
		.right_main_cols = right_main_cols,
		.analysis_cols = analysis_cols,
	};
}

