#ifndef RF_DRAW_H
#define RF_DRAW_H

#include "rf_fb.h"
#include "rf_task.h"

#include <stdint.h>

enum { RF_FONT_W = 6, RF_FONT_H = 8 };

struct rf_color rf_color_bg(void);
struct rf_color rf_color_panel_bg(void);
struct rf_color rf_color_header_bg(void);
struct rf_color rf_color_status_bg(void);
struct rf_color rf_color_border(void);
struct rf_color rf_color_fg(void);
struct rf_color rf_color_dim(void);

void rf_draw_clear(const struct rf_task *t, struct rf_color c);
void rf_draw_header(const struct rf_task *t);
void rf_draw_status(const struct rf_task *t);
void rf_draw_present(const struct rf_task *t);

#endif
