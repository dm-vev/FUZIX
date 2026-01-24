#ifndef RF_DRAW_H
#define RF_DRAW_H

#include "rf_fb.h"
#include "rf_task.h"

#include <stdint.h>

struct rf_color rf_color_bg(void);
struct rf_color rf_color_panel_bg(void);
struct rf_color rf_color_header_bg(void);
struct rf_color rf_color_status_bg(void);
struct rf_color rf_color_border(void);
struct rf_color rf_color_fg(void);
struct rf_color rf_color_dim(void);
struct rf_color rf_color_accent(void);
struct rf_color rf_color_warn(void);
struct rf_color rf_color_sel_bg(void);
struct rf_color rf_color_sel_fg(void);
struct rf_color rf_color_focus_mark(void);
struct rf_color rf_color_menu_bg(void);
struct rf_color rf_color_menu_fg(void);

void rf_draw_clear(const struct rf_task *t, struct rf_color c);
void rf_draw_fill_rect(const struct rf_task *t, int16_t x, int16_t y, int16_t w, int16_t h, struct rf_color c);
void rf_draw_text(const struct rf_task *t, int16_t x, int16_t y, const char *utf8, struct rf_color fg,
		  struct rf_color bg, int cols);
void rf_draw_present(const struct rf_task *t);

#endif
