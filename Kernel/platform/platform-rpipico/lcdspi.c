#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <timer.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <blkdev.h>

#include "picosdk.h"
#include "config.h"
#include <hardware/spi.h>
#include "hardware/timer.h"
#include <ctype.h>
#include "lcdspi.h"
#include "i2ckbd.h"
#include "pico/multicore.h"
////////////////////**************************************fonts
#define FONT_TABLE_SIZE 16

#include "fonts/font1.h"
#include "fonts/Misc_12x20_LE.h"
#include "fonts/Hom_16x24_LE.h"
#include "fonts/Fnt_10x16.h"
#include "fonts/Inconsola.h"
#include "fonts/ArialNumFontPlus.h"
#include "fonts/Font_8x6.h"
#include "fonts/arial_bold.h"
#include "fonts/smallfont.h"
#include "fonts/font6x8_cp1251.h"

/* Forward declarations used by the VT100/ANSI LCD console. */
void DrawBufferSPI(int x1, int y1, int x2, int y2, unsigned char *p);
void DrawRectangleSPI(int x1, int y1, int x2, int y2, int c);
void ScrollLCDSPI(int lines);
#ifdef HARDWARE_SCROLL
void HWScroll(uint16_t pixels);
#endif
static void ReadBufferSPI_mapped(int x1, int y1, int x2, int y2, unsigned char *p);

unsigned char *FontTable[FONT_TABLE_SIZE] = {(unsigned char *) font1,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
};

static short gui_font;
static int gui_fcolour;
static int gui_bcolour;
static short CurrentX = 0, CurrentY = 0; // the current default position for the next char to be written
static short gui_font_width, gui_font_height;
static short HRes = 0;
static short VRes = 0;
static char S_Height;
static char S_Width;
static bool lcd_text_enabled = true;

#define LCD_TERM_FONT_W 6
#define LCD_TERM_FONT_H 8
#define LCD_TERM_COLS   (LCD_WIDTH / LCD_TERM_FONT_W)
#define LCD_TERM_ROWS   (LCD_HEIGHT / LCD_TERM_FONT_H)
#define LCD_TERM_TABSTOP 8

static const uint32_t lcd_term_ansi16[16] = {
    RGB(0, 0, 0),         /* 0 black */
    RGB(205, 0, 0),       /* 1 red */
    RGB(0, 205, 0),       /* 2 green */
    RGB(205, 205, 0),     /* 3 yellow */
    RGB(0, 0, 238),       /* 4 blue */
    RGB(205, 0, 205),     /* 5 magenta */
    RGB(0, 205, 205),     /* 6 cyan */
    RGB(229, 229, 229),   /* 7 white (light gray) */
    RGB(127, 127, 127),   /* 8 bright black (gray) */
    RGB(255, 0, 0),       /* 9 bright red */
    RGB(0, 255, 0),       /* 10 bright green */
    RGB(255, 255, 0),     /* 11 bright yellow */
    RGB(92, 92, 255),     /* 12 bright blue */
    RGB(255, 0, 255),     /* 13 bright magenta */
    RGB(0, 255, 255),     /* 14 bright cyan */
    RGB(255, 255, 255),   /* 15 bright white */
};

enum lcd_term_state {
    LCD_TERM_NORMAL = 0,
    LCD_TERM_ESC,
    LCD_TERM_CSI,
    LCD_TERM_CHARSET,
};

static struct {
    uint8_t cx;
    uint8_t cy;
    uint8_t saved_cx;
    uint8_t saved_cy;
    uint32_t fg;
    uint32_t bg;
    uint8_t bold;
    uint8_t inverse;
    uint8_t cursor_visible;
    uint8_t line_drawing;

    enum lcd_term_state state;
    int params[8];
    uint8_t nparams;
    uint8_t have_param;
    uint8_t csi_question;
    int cur_param;
} lcd_term;

static short offsetY = 0;
#ifdef HARDWARE_SCROLL
static short viewY = 0;
static uint8_t view_lines = 0;
static uint8_t history_lines = 0;
#define LCD_TERM_SCROLLBACK_LINES ((LCD_REAL_HEIGHT - LCD_HEIGHT) / LCD_TERM_FONT_H)
#endif

static uint64_t lcd_term_cursor_next_blink_us;
static uint8_t lcd_term_cursor_blink_on;
static uint8_t lcd_term_cursor_drawn;
static uint8_t lcd_term_cursor_pxbuf[LCD_TERM_FONT_W * LCD_TERM_FONT_H * 3];
static uint8_t lcd_term_cursor_x;
static uint8_t lcd_term_cursor_y;

static void lcd_term_cursor_hide(void);
static void lcd_term_cursor_maybe_draw(void);
static void lcd_term_cursor_tick(void);

static inline uint32_t lcd_term_resolve_fg(void)
{
    if (lcd_term.bold && (lcd_term.fg & 0x100u) == 0)
        return lcd_term_ansi16[(lcd_term.fg & 0x0Fu) | 8u];
    if (lcd_term.fg & 0x100u)
        return lcd_term.fg & 0x00FFFFFFu;
    return lcd_term_ansi16[lcd_term.fg & 0x0Fu];
}

static inline uint32_t lcd_term_resolve_bg(void)
{
    if (lcd_term.bg & 0x100u)
        return lcd_term.bg & 0x00FFFFFFu;
    return lcd_term_ansi16[lcd_term.bg & 0x0Fu];
}

static void lcd_term_clear_rect_cells(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    uint16_t px = (uint16_t)x * LCD_TERM_FONT_W;
    uint16_t py = (uint16_t)y * LCD_TERM_FONT_H;
    uint16_t pw = (uint16_t)w * LCD_TERM_FONT_W;
    uint16_t ph = (uint16_t)h * LCD_TERM_FONT_H;
    if (!pw || !ph)
        return;
    DrawRectangleSPI(px, py, px + pw - 1, py + ph - 1, (int)lcd_term_resolve_bg());
}

static void lcd_term_scroll_up_one(void)
{
    ScrollLCDSPI(LCD_TERM_FONT_H);
    lcd_term_clear_rect_cells(0, LCD_TERM_ROWS - 1, LCD_TERM_COLS, 1);
    if (lcd_term.cy)
        lcd_term.cy--;
}

static void lcd_term_newline(void)
{
    lcd_term.cx = 0;
    lcd_term.cy++;
    if (lcd_term.cy >= LCD_TERM_ROWS) {
        lcd_term_scroll_up_one();
        lcd_term.cy = LCD_TERM_ROWS - 1;
    }
}

static void lcd_term_draw_glyph(uint8_t x, uint8_t y, uint8_t ch)
{
    const uint32_t fg = lcd_term.inverse ? lcd_term_resolve_bg() : lcd_term_resolve_fg();
    const uint32_t bg = lcd_term.inverse ? lcd_term_resolve_fg() : lcd_term_resolve_bg();

    uint8_t cellbuf[LCD_TERM_FONT_W * LCD_TERM_FONT_H * 3];
    uint16_t px = (uint16_t)x * LCD_TERM_FONT_W;
    uint16_t py = (uint16_t)y * LCD_TERM_FONT_H;

    uint8_t fg_b = (uint8_t)(fg & 0xFF);
    uint8_t fg_g = (uint8_t)((fg >> 8) & 0xFF);
    uint8_t fg_r = (uint8_t)((fg >> 16) & 0xFF);
    uint8_t bg_b = (uint8_t)(bg & 0xFF);
    uint8_t bg_g = (uint8_t)((bg >> 8) & 0xFF);
    uint8_t bg_r = (uint8_t)((bg >> 16) & 0xFF);

    if (ch < 0x20)
        ch = ' ';
    size_t glyph_count = sizeof(font6x8_cp1251) / sizeof(font6x8_cp1251[0]);
    size_t glyph_index = (size_t)(ch - 0x20);
    if (glyph_index >= glyph_count)
        glyph_index = ('?' - 0x20);

    for (int row = 0; row < LCD_TERM_FONT_H; row++) {
        uint8_t bits = font6x8_cp1251[glyph_index][row];
        for (int col = 0; col < LCD_TERM_FONT_W; col++) {
            uint8_t on = (bits & (1u << (LCD_TERM_FONT_W - 1 - col))) ? 1 : 0;
            uint8_t *p = &cellbuf[(row * LCD_TERM_FONT_W + col) * 3];
            p[0] = on ? fg_b : bg_b;
            p[1] = on ? fg_g : bg_g;
            p[2] = on ? fg_r : bg_r;
        }
    }
    DrawBufferSPI(px, py, px + LCD_TERM_FONT_W - 1, py + LCD_TERM_FONT_H - 1, cellbuf);
}

static void lcd_term_draw_line_drawing(uint8_t x, uint8_t y, uint8_t ch)
{
    /* VT100 line drawing set (ESC ( 0 + SO). We approximate with 6x8 glyphs. */
    const uint32_t fg = lcd_term.inverse ? lcd_term_resolve_bg() : lcd_term_resolve_fg();
    const uint32_t bg = lcd_term.inverse ? lcd_term_resolve_fg() : lcd_term_resolve_bg();

    uint8_t cellbuf[LCD_TERM_FONT_W * LCD_TERM_FONT_H * 3];
    uint16_t px = (uint16_t)x * LCD_TERM_FONT_W;
    uint16_t py = (uint16_t)y * LCD_TERM_FONT_H;

    uint8_t fg_b = (uint8_t)(fg & 0xFF);
    uint8_t fg_g = (uint8_t)((fg >> 8) & 0xFF);
    uint8_t fg_r = (uint8_t)((fg >> 16) & 0xFF);
    uint8_t bg_b = (uint8_t)(bg & 0xFF);
    uint8_t bg_g = (uint8_t)((bg >> 8) & 0xFF);
    uint8_t bg_r = (uint8_t)((bg >> 16) & 0xFF);

    memset(cellbuf, 0, sizeof(cellbuf));
    for (int i = 0; i < LCD_TERM_FONT_W * LCD_TERM_FONT_H; i++) {
        cellbuf[i * 3 + 0] = bg_b;
        cellbuf[i * 3 + 1] = bg_g;
        cellbuf[i * 3 + 2] = bg_r;
    }

    /* Draw a simple 1px line style within the 6x8 cell. */
    int mid_x = LCD_TERM_FONT_W / 2;
    int mid_y = LCD_TERM_FONT_H / 2;

#define LCD_TERM_SETPX(cx, cy) do { \
        if ((cx) >= 0 && (cy) >= 0 && (cx) < LCD_TERM_FONT_W && (cy) < LCD_TERM_FONT_H) { \
            uint8_t *p = &cellbuf[((cy) * LCD_TERM_FONT_W + (cx)) * 3]; \
            p[0] = fg_b; \
            p[1] = fg_g; \
            p[2] = fg_r; \
        } \
    } while (0)

    /* Mapping per VT100 line drawing set. */
    switch (ch) {
    case 'q': /* horizontal line */
        for (int cx = 0; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        break;
    case 'x': /* vertical line */
        for (int cy = 0; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'j': /* lower right corner */
        for (int cx = 0; cx <= mid_x; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy <= mid_y; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'k': /* upper right corner */
        for (int cx = 0; cx <= mid_x; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = mid_y; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'l': /* upper left corner */
        for (int cx = mid_x; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = mid_y; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'm': /* lower left corner */
        for (int cx = mid_x; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy <= mid_y; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'n': /* crossing */
        for (int cx = 0; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 't': /* tee left */
        for (int cx = mid_x; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'u': /* tee right */
        for (int cx = 0; cx <= mid_x; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'v': /* tee up */
        for (int cx = 0; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = 0; cy <= mid_y; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    case 'w': /* tee down */
        for (int cx = 0; cx < LCD_TERM_FONT_W; cx++) LCD_TERM_SETPX(cx, mid_y);
        for (int cy = mid_y; cy < LCD_TERM_FONT_H; cy++) LCD_TERM_SETPX(mid_x, cy);
        break;
    default:
        /* Fallback: draw the raw character. */
        lcd_term_draw_glyph(x, y, ch);
        return;
    }

    DrawBufferSPI(px, py, px + LCD_TERM_FONT_W - 1, py + LCD_TERM_FONT_H - 1, cellbuf);

#undef LCD_TERM_SETPX
}

static void lcd_term_reset(uint8_t clear)
{
    lcd_term_cursor_hide();
    lcd_term.cx = 0;
    lcd_term.cy = 0;
    lcd_term.saved_cx = 0;
    lcd_term.saved_cy = 0;
    lcd_term.fg = 7;
    lcd_term.bg = 0;
    lcd_term.bold = 0;
    lcd_term.inverse = 0;
    lcd_term.cursor_visible = 1;
    lcd_term.line_drawing = 0;
    lcd_term.state = LCD_TERM_NORMAL;
    lcd_term.nparams = 0;
    lcd_term.have_param = 0;
    lcd_term.csi_question = 0;
    lcd_term.cur_param = 0;
#ifdef HARDWARE_SCROLL
    offsetY = 0;
    viewY = 0;
    view_lines = 0;
    history_lines = 0;
    HWScroll(0);
#endif
    lcd_term_cursor_next_blink_us = time_us_64() + 500000u;
    lcd_term_cursor_blink_on = 1;
    lcd_term_cursor_drawn = 0;
    if (clear)
        DrawRectangleSPI(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, (int)lcd_term_resolve_bg());
    lcd_term_cursor_maybe_draw();
}

static void lcd_term_cursor_hide(void)
{
    if (!lcd_term_cursor_drawn)
        return;
    uint16_t px = (uint16_t)lcd_term_cursor_x * LCD_TERM_FONT_W;
    uint16_t py = (uint16_t)lcd_term_cursor_y * LCD_TERM_FONT_H;
    DrawBufferSPI(px, py, px + LCD_TERM_FONT_W - 1, py + LCD_TERM_FONT_H - 1, lcd_term_cursor_pxbuf);
    lcd_term_cursor_drawn = 0;
}

static void lcd_term_cursor_maybe_draw(void)
{
    if (!lcd_term.cursor_visible || !lcd_term_cursor_blink_on) {
        lcd_term_cursor_hide();
        return;
    }
#ifdef HARDWARE_SCROLL
    if (view_lines) {
        lcd_term_cursor_hide();
        return;
    }
#endif

    if (lcd_term_cursor_drawn &&
        lcd_term_cursor_x == lcd_term.cx &&
        lcd_term_cursor_y == lcd_term.cy) {
        return;
    }

    lcd_term_cursor_hide();

    uint16_t px = (uint16_t)lcd_term.cx * LCD_TERM_FONT_W;
    uint16_t py = (uint16_t)lcd_term.cy * LCD_TERM_FONT_H;
    ReadBufferSPI_mapped(px, py, px + LCD_TERM_FONT_W - 1, py + LCD_TERM_FONT_H - 1, lcd_term_cursor_pxbuf);

    uint8_t inv[LCD_TERM_FONT_W * LCD_TERM_FONT_H * 3];
    for (size_t i = 0; i < sizeof(inv); i++)
        inv[i] = (uint8_t)~lcd_term_cursor_pxbuf[i];
    DrawBufferSPI(px, py, px + LCD_TERM_FONT_W - 1, py + LCD_TERM_FONT_H - 1, inv);

    lcd_term_cursor_x = lcd_term.cx;
    lcd_term_cursor_y = lcd_term.cy;
    lcd_term_cursor_drawn = 1;
}

static void lcd_term_cursor_tick(void)
{
    uint64_t now = time_us_64();
    if ((int64_t)(now - lcd_term_cursor_next_blink_us) < 0)
        return;
    lcd_term_cursor_next_blink_us = now + 500000u;
    lcd_term_cursor_blink_on ^= 1u;
    if (lcd_term_cursor_blink_on)
        lcd_term_cursor_maybe_draw();
    else
        lcd_term_cursor_hide();
}

static void lcd_term_csi_finish(uint8_t final)
{
    int p0 = lcd_term.nparams > 0 ? lcd_term.params[0] : 0;
    int p1 = lcd_term.nparams > 1 ? lcd_term.params[1] : 0;
    if (final == 'A') {
        int n = p0 ? p0 : 1;
        lcd_term.cy = (lcd_term.cy > (uint8_t)n) ? (lcd_term.cy - (uint8_t)n) : 0;
    } else if (final == 'B') {
        int n = p0 ? p0 : 1;
        uint8_t ny = lcd_term.cy + (uint8_t)n;
        lcd_term.cy = ny >= LCD_TERM_ROWS ? (LCD_TERM_ROWS - 1) : ny;
    } else if (final == 'C') {
        int n = p0 ? p0 : 1;
        uint8_t nx = lcd_term.cx + (uint8_t)n;
        lcd_term.cx = nx >= LCD_TERM_COLS ? (LCD_TERM_COLS - 1) : nx;
    } else if (final == 'D') {
        int n = p0 ? p0 : 1;
        lcd_term.cx = (lcd_term.cx > (uint8_t)n) ? (lcd_term.cx - (uint8_t)n) : 0;
    } else if (final == 'H' || final == 'f') {
        int row = p0 ? p0 : 1;
        int col = p1 ? p1 : 1;
        if (row < 1) row = 1;
        if (col < 1) col = 1;
        lcd_term.cy = (row > LCD_TERM_ROWS) ? (LCD_TERM_ROWS - 1) : (uint8_t)(row - 1);
        lcd_term.cx = (col > LCD_TERM_COLS) ? (LCD_TERM_COLS - 1) : (uint8_t)(col - 1);
    } else if (final == 'J') {
        if (p0 == 2 || lcd_term.nparams == 0) {
            lcd_term_reset(1);
        } else if (p0 == 0) {
            /* clear from cursor to end */
            lcd_term_clear_rect_cells(lcd_term.cx, lcd_term.cy, LCD_TERM_COLS - lcd_term.cx, 1);
            if (lcd_term.cy + 1 < LCD_TERM_ROWS)
                lcd_term_clear_rect_cells(0, lcd_term.cy + 1, LCD_TERM_COLS, LCD_TERM_ROWS - (lcd_term.cy + 1));
        } else if (p0 == 1) {
            /* clear from start to cursor */
            if (lcd_term.cy)
                lcd_term_clear_rect_cells(0, 0, LCD_TERM_COLS, lcd_term.cy);
            lcd_term_clear_rect_cells(0, lcd_term.cy, lcd_term.cx + 1, 1);
        }
    } else if (final == 'K') {
        if (p0 == 2) {
            lcd_term_clear_rect_cells(0, lcd_term.cy, LCD_TERM_COLS, 1);
        } else if (p0 == 0 || lcd_term.nparams == 0) {
            lcd_term_clear_rect_cells(lcd_term.cx, lcd_term.cy, LCD_TERM_COLS - lcd_term.cx, 1);
        } else if (p0 == 1) {
            lcd_term_clear_rect_cells(0, lcd_term.cy, lcd_term.cx + 1, 1);
        }
    } else if (final == 'm') {
        if (lcd_term.nparams == 0) {
            lcd_term.fg = 7;
            lcd_term.bg = 0;
            lcd_term.bold = 0;
            lcd_term.inverse = 0;
        }
        for (uint8_t i = 0; i < lcd_term.nparams; i++) {
            int v = lcd_term.params[i];
            if (v == 0) {
                lcd_term.fg = 7;
                lcd_term.bg = 0;
                lcd_term.bold = 0;
                lcd_term.inverse = 0;
            } else if (v == 1) {
                lcd_term.bold = 1;
            } else if (v == 22) {
                lcd_term.bold = 0;
            } else if (v == 7) {
                lcd_term.inverse = 1;
            } else if (v == 27) {
                lcd_term.inverse = 0;
            } else if (v >= 30 && v <= 37) {
                lcd_term.fg = (uint8_t)(v - 30);
            } else if (v == 39) {
                lcd_term.fg = 7;
            } else if (v >= 40 && v <= 47) {
                lcd_term.bg = (uint8_t)(v - 40);
            } else if (v == 49) {
                lcd_term.bg = 0;
            } else if (v >= 90 && v <= 97) {
                lcd_term.fg = (uint8_t)((v - 90) | 8);
            } else if (v >= 100 && v <= 107) {
                lcd_term.bg = (uint8_t)((v - 100) | 8);
            }
        }
    } else if (final == 's') {
        lcd_term.saved_cx = lcd_term.cx;
        lcd_term.saved_cy = lcd_term.cy;
    } else if (final == 'u') {
        lcd_term.cx = lcd_term.saved_cx;
        lcd_term.cy = lcd_term.saved_cy;
    } else if ((final == 'h' || final == 'l') && lcd_term.csi_question) {
        /* DEC private mode set/reset. We only care about cursor visibility (?25h/?25l). */
        if (p0 == 25) {
            lcd_term.cursor_visible = (final == 'h') ? 1 : 0;
            if (!lcd_term.cursor_visible)
                lcd_term_cursor_hide();
        }
    }
}


static void lcd_term_putc(uint8_t c)
{
    switch (lcd_term.state) {
    case LCD_TERM_NORMAL:
        if (c == 0x1B) {
            lcd_term.state = LCD_TERM_ESC;
            return;
        }
        if (c == '\r') {
            lcd_term.cx = 0;
            return;
        }
        if (c == '\n') {
            lcd_term_newline();
            return;
        }
        if (c == '\b') {
            if (lcd_term.cx)
                lcd_term.cx--;
            return;
        }
        if (c == '\t') {
            uint8_t next = (uint8_t)((lcd_term.cx + LCD_TERM_TABSTOP) & ~(LCD_TERM_TABSTOP - 1));
            if (next >= LCD_TERM_COLS)
                next = LCD_TERM_COLS - 1;
            while (lcd_term.cx < next) {
                lcd_term_draw_glyph(lcd_term.cx, lcd_term.cy, ' ');
                lcd_term.cx++;
            }
            return;
        }
        if (c == 0x0E) { /* SO */
            lcd_term.line_drawing = 1;
            return;
        }
        if (c == 0x0F) { /* SI */
            lcd_term.line_drawing = 0;
            return;
        }
        if (c < 0x20)
            return;
        if (lcd_term.line_drawing)
            lcd_term_draw_line_drawing(lcd_term.cx, lcd_term.cy, c);
        else
            lcd_term_draw_glyph(lcd_term.cx, lcd_term.cy, c);
        lcd_term.cx++;
        if (lcd_term.cx >= LCD_TERM_COLS)
            lcd_term_newline();
        return;

    case LCD_TERM_ESC:
        if (c == '[') {
            lcd_term.state = LCD_TERM_CSI;
            lcd_term.nparams = 0;
            lcd_term.have_param = 0;
            lcd_term.csi_question = 0;
            lcd_term.cur_param = 0;
            memset(lcd_term.params, 0, sizeof(lcd_term.params));
            return;
        }
        if (c == '(') {
            lcd_term.state = LCD_TERM_CHARSET;
            return;
        }
        if (c == 'c') { /* RIS */
            lcd_term_reset(1);
            lcd_term.state = LCD_TERM_NORMAL;
            return;
        }
        if (c == '7') { /* DECSC */
            lcd_term.saved_cx = lcd_term.cx;
            lcd_term.saved_cy = lcd_term.cy;
            lcd_term.state = LCD_TERM_NORMAL;
            return;
        }
        if (c == '8') { /* DECRC */
            lcd_term.cx = lcd_term.saved_cx;
            lcd_term.cy = lcd_term.saved_cy;
            lcd_term.state = LCD_TERM_NORMAL;
            return;
        }
        if (c == 'D') { /* IND */
            lcd_term.cy++;
            if (lcd_term.cy >= LCD_TERM_ROWS) {
                lcd_term_scroll_up_one();
                lcd_term.cy = LCD_TERM_ROWS - 1;
            }
            lcd_term.state = LCD_TERM_NORMAL;
            return;
        }
        lcd_term.state = LCD_TERM_NORMAL;
        return;

    case LCD_TERM_CHARSET:
        if (c == '0')
            lcd_term.line_drawing = 1;
        else if (c == 'B')
            lcd_term.line_drawing = 0;
        lcd_term.state = LCD_TERM_NORMAL;
        return;

    case LCD_TERM_CSI:
        if (c == '?' && lcd_term.nparams == 0 && !lcd_term.have_param) {
            lcd_term.csi_question = 1;
            return;
        }
        if (c >= '0' && c <= '9') {
            lcd_term.cur_param = lcd_term.cur_param * 10 + (c - '0');
            lcd_term.have_param = 1;
            return;
        }
        if (c == ';') {
            if (lcd_term.nparams < (uint8_t)(sizeof(lcd_term.params) / sizeof(lcd_term.params[0]))) {
                lcd_term.params[lcd_term.nparams++] = lcd_term.have_param ? lcd_term.cur_param : 0;
            }
            lcd_term.cur_param = 0;
            lcd_term.have_param = 0;
            return;
        }

        /* final byte */
        if (lcd_term.nparams < (uint8_t)(sizeof(lcd_term.params) / sizeof(lcd_term.params[0]))) {
            lcd_term.params[lcd_term.nparams++] = lcd_term.have_param ? lcd_term.cur_param : 0;
        }
        lcd_term_csi_finish(c);
        lcd_term.state = LCD_TERM_NORMAL;
        return;
    }
}

#ifdef HARDWARE_SCROLL
static inline int wrap_lcd_y(int y)
{
    y %= LCD_REAL_HEIGHT;
    if (y < 0)
        y += LCD_REAL_HEIGHT;
    return y;
}
#endif
unsigned char LCDBuffer[320 * 3] = {0};// 1440 = 480*3, 320*3 = 960

void __not_in_flash_func(spi_write_fast)(spi_inst_t *spi, const uint8_t *src, size_t len) {
    // Write to TX FIFO whilst ignoring RX, then clean up afterward. When RX
    // is full, PL022 inhibits RX pushes, and sets a sticky flag on
    // push-on-full, but continues shifting. Safe if SSPIMSC_RORIM is not set.
    for (size_t i = 0; i < len; ++i) {
        while (!spi_is_writable(spi))
            tight_loop_contents();
        spi_get_hw(spi)->dr = (uint32_t) src[i];
    }
}

void __not_in_flash_func(spi_finish)(spi_inst_t *spi) {
    // Drain RX FIFO, then wait for shifting to finish (which may be *after*
    // TX FIFO drains), then drain RX FIFO again
    while (spi_is_readable(spi))
        (void) spi_get_hw(spi)->dr;
    while (spi_get_hw(spi)->sr & SPI_SSPSR_BSY_BITS)
        tight_loop_contents();
    while (spi_is_readable(spi))
        (void) spi_get_hw(spi)->dr;

    // Don't leave overrun flag set
    spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;
}

int GetFontWidth(int fnt) {
    return FontTable[fnt >> 4][0] * (fnt & 0b1111);
}

int GetFontHeight(int fnt) {
    return FontTable[fnt >> 4][1] * (fnt & 0b1111);
}

void init_fonts() {
    FontTable[0] = (unsigned char *) font1;

}

void SetFont(int fnt) {
    if (FontTable[fnt >> 4] == NULL) panic("Invalid font number,max is 15");
    gui_font_width = FontTable[fnt >> 4][0] * (fnt & 0b1111);
    gui_font_height = FontTable[fnt >> 4][1] * (fnt & 0b1111);


    S_Height = VRes / gui_font_height;
    S_Width = HRes / gui_font_width;

    gui_font = fnt;

}

void DefineRegionSPI(int xstart, int ystart, int xend, int yend, int rw) {
    unsigned char coord[4];
    lcd_spi_lower_cs();
    gpio_put(Pico_LCD_DC, 0);//gpio_put(Pico_LCD_DC,0);
    HWSendSPI(&(uint8_t) {ILI9341_COLADDRSET}, 1);
    gpio_put(Pico_LCD_DC, 1);
    coord[0] = xstart >> 8;
    coord[1] = xstart;
    coord[2] = xend >> 8;
    coord[3] = xend;
    HWSendSPI(coord, 4);//		HAL_SPI_Transmit(&hspi3,coord,4,500);
    gpio_put(Pico_LCD_DC, 0);
    HWSendSPI(&(uint8_t) {ILI9341_PAGEADDRSET}, 1);
    gpio_put(Pico_LCD_DC, 1);
    coord[0] = ystart >> 8;
    coord[1] = ystart;
    coord[2] = yend >> 8;
    coord[3] = yend;
    HWSendSPI(coord, 4);//		HAL_SPI_Transmit(&hspi3,coord,4,500);
    gpio_put(Pico_LCD_DC, 0);
    if (rw) {
        HWSendSPI(&(uint8_t) {ILI9341_MEMORYWRITE}, 1);
    } else {
        HWSendSPI(&(uint8_t) {ILI9341_RAMRD}, 1);
    }
    gpio_put(Pico_LCD_DC, 1);
}

void ReadBufferSPI(int x1, int y1, int x2, int y2, unsigned char *p) {
    int r, N, t;
    unsigned char h, l;
//	PInt(x1);PIntComma(y1);PIntComma(x2);PIntComma(y2);PRet();
    // make sure the coordinates are kept within the display area
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    N = (x2 - x1 + 1) * (y2 - y1 + 1) * 3;

    DefineRegionSPI(x1, y1, x2, y2, 0);

    //spi_init(Pico_LCD_SPI_MOD, 6000000);
    spi_set_baudrate(Pico_LCD_SPI_MOD, 6000000);
    //spi_read_data_len(p, 1);
    HWReadSPI((uint8_t *) p, 1);
    r = 0;
    HWReadSPI((uint8_t *) p, N);
    gpio_put(Pico_LCD_DC, 0);
    lcd_spi_raise_cs();
    spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
    r = 0;

    while (N) {
        h = (uint8_t) p[r + 2];
        l = (uint8_t) p[r];
        p[r] = h;//(h & 0xF8);
        p[r + 2] = l;//(l & 0xF8);
        r += 3;
        N -= 3;
    }
}

static void ReadBufferSPI_mapped(int x1, int y1, int x2, int y2, unsigned char *p)
{
#ifdef HARDWARE_SCROLL
    int t;
    int width, height;
    int y1p;

    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;

    width = x2 - x1 + 1;
    height = y2 - y1 + 1;
    if (width <= 0 || height <= 0)
        return;

    y1p = wrap_lcd_y(y1 + offsetY);
    if (y1p + height <= LCD_REAL_HEIGHT) {
        ReadBufferSPI(x1, y1p, x2, y1p + height - 1, p);
        return;
    }

    int h1 = LCD_REAL_HEIGHT - y1p;
    int h2 = height - h1;
    size_t row_bytes = (size_t)width * 3u;

    ReadBufferSPI(x1, y1p, x2, LCD_REAL_HEIGHT - 1, p);
    ReadBufferSPI(x1, 0, x2, h2 - 1, p + (size_t)h1 * row_bytes);
    return;
#else
    ReadBufferSPI(x1, y1, x2, y2, p);
#endif
}

static void DrawBufferSPI_raw(int x1, int y1, int x2, int y2, unsigned char *p) {
    int i, t;
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    i = (x2 - x1 + 1) * (y2 - y1 + 1);
    DefineRegionSPI(x1, y1, x2, y2, 1);

    /*
     * Performance: the old code issued one SPI write per pixel (3 bytes),
     * which is extremely slow on RP2040/RP2350. Instead, stream pixels in
     * chunks. Input is BGR888; ILI9488 expects RGB888.
     */
#ifdef ILI9488
    {
        static unsigned char txbuf[3 * 256]; /* 256 pixels per chunk */
        size_t pixels = (size_t)i;
        while (pixels) {
            size_t n = pixels;
            if (n > (sizeof(txbuf) / 3u))
                n = (sizeof(txbuf) / 3u);
            for (size_t k = 0; k < n; k++) {
                unsigned char b = *p++;
                unsigned char g = *p++;
                unsigned char r = *p++;
                txbuf[k * 3u + 0] = r;
                txbuf[k * 3u + 1] = g;
                txbuf[k * 3u + 2] = b;
            }
            HWSendSPI(txbuf, (int)(n * 3u));
            pixels -= n;
        }
    }
#else
    HWSendSPI(p, i * 3);
#endif
    lcd_spi_raise_cs();

}

void DrawBufferSPI(int x1, int y1, int x2, int y2, unsigned char *p) {
#ifdef HARDWARE_SCROLL
    int t;
    int width, height;
    int y1p;

    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;

    width = x2 - x1 + 1;
    height = y2 - y1 + 1;
    if (width <= 0 || height <= 0)
        return;

    y1p = wrap_lcd_y(y1 + offsetY);
    if (y1p + height <= LCD_REAL_HEIGHT) {
        DrawBufferSPI_raw(x1, y1p, x2, y1p + height - 1, p);
        return;
    }

    int h1 = LCD_REAL_HEIGHT - y1p;
    int h2 = height - h1;
    DrawBufferSPI_raw(x1, y1p, x2, LCD_REAL_HEIGHT - 1, p);
    DrawBufferSPI_raw(x1, 0, x2, h2 - 1, p + (width * h1 * 3));
    return;
#else
    DrawBufferSPI_raw(x1, y1, x2, y2, p);
#endif
}

//Print the bitmap of a char on the video output
//    x, y - the top left of the char
//    width, height - size of the char's bitmap
//    scale - how much to scale the bitmap
//	  fc, bc - foreground and background colour
//    bitmap - pointer to the bitmap
void DrawBitmapSPI(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) {
    int i, j, k, m, n;
    char f[3], b[3];
    int vertCoord, horizCoord, XStart, XEnd, YEnd;
    char *p = 0;
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
#ifdef HARDWARE_SCROLL
    y1 += offsetY; y1 = y1 %LCD_REAL_HEIGHT;
#endif
    if (x1 >= HRes || y1 >= VRes || x1 + width * scale < 0 || y1 + height * scale < 0)return;
    // adjust when part of the bitmap is outside the displayable coordinates
    vertCoord = y1;
    if (y1 < 0) y1 = 0;                                 // the y coord is above the top of the screen
    XStart = x1;
    if (XStart < 0) XStart = 0;                            // the x coord is to the left of the left marginn
    XEnd = x1 + (width * scale) - 1;
    if (XEnd >= HRes) XEnd = HRes - 1; // the width of the bitmap will extend beyond the right margin
    YEnd = y1 + (height * scale) - 1;
    if (YEnd >= VRes) YEnd = VRes - 1;// the height of the bitmap will extend beyond the bottom margin

#ifdef ILI9488
    // convert the colours to 565 format
    f[0] = (fc >> 16);
    f[1] = (fc >> 8) & 0xFF;
    f[2] = (fc & 0xFF);
    b[0] = (bc >> 16);
    b[1] = (bc >> 8) & 0xFF;
    b[2] = (bc & 0xFF);

#endif
    //printf("DrawBitmapSPI-> XStart %d, y1 %d, XEnd %d, YEnd %d\n",XStart,y1,XEnd,YEnd);
    DefineRegionSPI(XStart, y1, XEnd, YEnd, 1);

    n = 0;
    for (i = 0; i < height; i++) {                                   // step thru the font scan line by line
        for (j = 0; j < scale; j++) {                                // repeat lines to scale the font
            if (vertCoord++ < 0) continue;                           // we are above the top of the screen
            if (vertCoord > VRes) {                                  // we have extended beyond the bottom of the screen
                lcd_spi_raise_cs();                                  //set CS high
                return;
            }
            horizCoord = x1;
            for (k = 0; k < width; k++) {                            // step through each bit in a scan line
                for (m = 0; m < scale; m++) {                        // repeat pixels to scale in the x axis
                    if (horizCoord++ < 0) continue;                  // we have not reached the left margin
                    if (horizCoord > HRes) continue;                 // we are beyond the right margin
                    if ((bitmap[((i * width) + k) / 8] >> (((height * width) - ((i * width) + k) - 1) % 8)) & 1) {
                        HWSendSPI((uint8_t *) &f, 3);
                    } else {
                        if (bc == -1) {
                            c.rgbbytes[0] = p[n];
                            c.rgbbytes[1] = p[n + 1];
                            c.rgbbytes[2] = p[n + 2];
#ifdef ILI9488
                            b[0] = c.rgbbytes[2];
                            b[1] = c.rgbbytes[1];
                            b[2] = c.rgbbytes[0];
#endif
                        }
                        HWSendSPI((uint8_t *) &b, 3);
                    }
                    n += 3;
                }
            }
        }
    }
    lcd_spi_raise_cs();                                  //set CS high

}

// Draw a filled rectangle with hardware scroll
// auto split y2 coord over LCD_REAL_HEIGHT rect to be two rects
// all the Y coordinates should be circled
//    x1, y1, x2, y2 - the coordinates
//    c - the colour
//    finish -- whether should finish the spi and disable LCD_CS
static void DrawRectangleSPI_raw(int x1, int y1, int x2, int y2, int c) {
    // convert the colours to 565 format
    unsigned char col[3];
    if (x1 == x2 && y1 == y2) {
        if (x1 < 0) return;
        if (x1 >= HRes) return;
        if (y1 < 0) return;
        if (y1 >= VRes) return;
        y2 = y1;
        DefineRegionSPI(x1, y1, x2, y2, 1);
#ifdef ILI9488
        col[0] = (c >> 16);
        col[1] = (c >> 8) & 0xFF;
        col[2] = (c & 0xFF);
#endif
        HWSendSPI(col, 3);
    } else {
        int i, t, y;
        unsigned char *p;
        // make sure the coordinates are kept within the display area
        if (x2 <= x1) {
            t = x1;
            x1 = x2;
            x2 = t;
        }
        if (y2 <= y1) {
            t = y1;
            y1 = y2;
            y2 = t;
        }
        if (x1 < 0) x1 = 0;
        if (x1 >= HRes) x1 = HRes - 1;
        if (x2 < 0) x2 = 0;
        if (x2 >= HRes) x2 = HRes - 1;

        if (y1 < 0) y1 = 0;
        if (y1 >= VRes) y1 = VRes - 1;
        if (y2 < 0) y2 = 0;
        if (y2 >= VRes) y2 = VRes - 1;

        DefineRegionSPI(x1, y1, x2, y2, 1);
#ifdef ILI9488
        i = x2 - x1 + 1;
        i *= 3;
        p = LCDBuffer;
        col[0] = (c >> 16);
        col[1] = (c >> 8) & 0xFF;
        col[2] = (c & 0xFF);
        for (t = 0; t < i; t += 3) {
            p[t] = col[0];
            p[t + 1] = col[1];
            p[t + 2] = col[2];
        }
        for (y = y1; y <= y2; y++) {
            spi_write_fast(Pico_LCD_SPI_MOD, p, i);
        }
#endif
    }

    spi_finish(Pico_LCD_SPI_MOD);
    lcd_spi_raise_cs();

}

void DrawRectangleSPI(int x1, int y1, int x2, int y2, int c) {
#ifdef HARDWARE_SCROLL
    int t;
    int height;
    int y1p;

    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;

    height = y2 - y1 + 1;
    if (height <= 0)
        return;

    y1p = wrap_lcd_y(y1 + offsetY);
    if (y1p + height <= LCD_REAL_HEIGHT) {
        DrawRectangleSPI_raw(x1, y1p, x2, y1p + height - 1, c);
        return;
    }

    int h1 = LCD_REAL_HEIGHT - y1p;
    int h2 = height - h1;
    DrawRectangleSPI_raw(x1, y1p, x2, LCD_REAL_HEIGHT - 1, c);
    DrawRectangleSPI_raw(x1, 0, x2, h2 - 1, c);
    return;
#else
    DrawRectangleSPI_raw(x1, y1, x2, y2, c);
#endif
}

/******************************************************************************************
 Print a char on the LCD display
 Any characters not in the font will print as a space.
 The char is printed at the current location defined by CurrentX and CurrentY
*****************************************************************************************/
void GUIPrintChar(int fnt, int fc, int bc, char c, int orientation) {
    unsigned char *p, *fp, *np = NULL;
    int  modx, mody, scale = fnt & 0b1111;
    int height, width;

    // to get the +, - and = chars for font 6 we fudge them by scaling up font 1
    if ((fnt & 0xf0) == 0x50 && (c == '-' || c == '+' || c == '=')) {
        fp = (unsigned char *) FontTable[0];
        //scale = scale * 4;
    } else
        fp = (unsigned char *) FontTable[fnt >> 4];

    height = fp[1];
    width = fp[0];
    modx = mody = 0;
    //printf("fp %d, c %d ,height %d width %d\n",fp,c, height,width);

    if (c >= fp[2] && c < fp[2] + fp[3]) {
        p = fp + 4 + (int) (((c - fp[2]) * height * width) / 8);
        //printf("p = %d\n",p);
        np = p;

        DrawBitmapSPI(CurrentX + modx, CurrentY+mody,width, height, scale, fc, bc, np);
    } else {
        DrawRectangleSPI(CurrentX + modx,  CurrentY+mody, CurrentX + modx + (width * scale),
                          mody + (height * scale), bc);
    }

    if (orientation == ORIENT_NORMAL) CurrentX += width * scale;

}
#ifdef HARDWARE_SCROLL
void setScrollArea(uint16_t topFixedArea, uint16_t bottomFixedArea) {

  spi_write_command(0x33); // Vertical HWScroll definition
  spi_write_data(topFixedArea >> 8);
  spi_write_data(topFixedArea);
  spi_write_data(LCD_REAL_HEIGHT >> 8);
  spi_write_data(LCD_REAL_HEIGHT & 0xff);
  spi_write_data(bottomFixedArea >> 8);
  spi_write_data(bottomFixedArea);

}

void HWScroll(uint16_t pixels) {
    spi_write_command(0x37); // Vertical scrolling start address
    spi_write_data(pixels >> 8);
    spi_write_data(pixels & 0xFF);
}

#else
unsigned char scrollbuff[LCD_WIDTH*3];
#endif


void ScrollLCDSPI(int lines) {
    if (lines == 0)return;
#ifdef HARDWARE_SCROLL
    offsetY += lines;
    offsetY = (short)wrap_lcd_y(offsetY);
    if (lines > 0 && LCD_TERM_SCROLLBACK_LINES) {
        uint8_t add = (uint8_t)(lines / LCD_TERM_FONT_H);
        if (add) {
            uint8_t maxh = (uint8_t)LCD_TERM_SCROLLBACK_LINES;
            uint8_t nh = history_lines + add;
            history_lines = (nh > maxh) ? maxh : nh;
            if (view_lines) {
                uint8_t nv = view_lines + add;
                view_lines = (nv > maxh) ? maxh : nv;
            }
        }
    }
    if (view_lines == 0) {
        viewY = offsetY;
        HWScroll((uint16_t)viewY);
    }

    spi_finish(Pico_LCD_SPI_MOD);
    lcd_spi_raise_cs();
#else

    if (lines >= 0) {
        for (int i = 0; i < VRes - lines; i++) {
            ReadBufferSPI(0, i + lines, HRes - 1, i + lines, scrollbuff);
            DrawBufferSPI(0, i, HRes - 1, i, scrollbuff);
        }
        DrawRectangleSPI(0, VRes - lines, HRes - 1, VRes - 1, gui_bcolour); // erase the lines to be scrolled off
    } else {
        lines = -lines;
        for (int i = VRes - 1; i >= lines; i--) {
            ReadBufferSPI(0, i - lines, HRes - 1, i - lines, scrollbuff);
            DrawBufferSPI(0, i, HRes - 1, i, scrollbuff);
        }
        DrawRectangleSPI(0, 0, HRes - 1, lines - 1, gui_bcolour); // erase the lines introduced at the top
    }
#endif

}

static void lcd_term_view_follow(void)
{
#ifdef HARDWARE_SCROLL
    if (view_lines == 0)
        return;
    lcd_term_cursor_hide();
    view_lines = 0;
    viewY = offsetY;
    HWScroll((uint16_t)viewY);
    lcd_term_cursor_maybe_draw();
#endif
}

static void lcd_term_view_scroll_up(void)
{
#ifdef HARDWARE_SCROLL
    if (history_lines == 0)
        return;
    if (view_lines >= history_lines)
        return;
    if (view_lines >= (uint8_t)LCD_TERM_SCROLLBACK_LINES)
        return;
    lcd_term_cursor_hide();
    view_lines++;
    viewY = (short)wrap_lcd_y(viewY - LCD_TERM_FONT_H);
    HWScroll((uint16_t)viewY);
#endif
}

static void lcd_term_view_scroll_down(void)
{
#ifdef HARDWARE_SCROLL
    if (view_lines == 0)
        return;
    lcd_term_cursor_hide();
    view_lines--;
    if (view_lines == 0) {
        viewY = offsetY;
    } else {
        viewY = (short)wrap_lcd_y(viewY + LCD_TERM_FONT_H);
    }
    HWScroll((uint16_t)viewY);
    if (view_lines == 0)
        lcd_term_cursor_maybe_draw();
#endif
}

void DisplayPutC(char c) {
    // if it is printable and it is going to take us off the right hand end of the screen do a CRLF
    if (c >= FontTable[gui_font >> 4][2] && c < FontTable[gui_font >> 4][2] + FontTable[gui_font >> 4][3]) {
        if (CurrentX + gui_font_width > HRes) {
            DisplayPutC('\r');
            DisplayPutC('\n');
        }
    }

    // handle the standard control chars
    switch (c) {
        case '\b':
            CurrentX -= gui_font_width;
            //if (CurrentX < 0) CurrentX = 0;
            if (CurrentX < 0) {  //Go to end of previous line
                CurrentY -= gui_font_height;                  //Go up one line
                if (CurrentY < 0) CurrentY = 0;
                CurrentX = (S_Width - 1) * gui_font_width;  //go to last character
            }
            return;
        case '\r':
            CurrentX = 0;
            return;
        case '\n':
            CurrentY += gui_font_height;
            if (CurrentY + gui_font_height >= LCD_HEIGHT) {
#ifdef HARDWARE_SCROLL
                int lines= 0;
                lines = CurrentY + gui_font_height - LCD_HEIGHT;
                ScrollLCDSPI(lines);
                CurrentY -= lines;
                DrawRectangleSPI(0, LCD_HEIGHT - lines, HRes - 1, LCD_HEIGHT - 1, BLACK);
#else
                ScrollLCDSPI(CurrentY + gui_font_height - VRes);
                CurrentY -= (CurrentY + gui_font_height - VRes);
#endif
            }
            return;
        case '\t':
            do {
                DisplayPutC(' ');
            } while ((CurrentX / gui_font_width) % 2);// 2 3 4 8
            return;
    }
    GUIPrintChar(gui_font, gui_fcolour, gui_bcolour, c, ORIENT_NORMAL);// print it
}

///////=----------------------------------------===//////
void lcd_clear() {
    DrawRectangleSPI(0, 0, HRes - 1, VRes - 1, BLACK);
}

void lcd_putc(uint8_t devn, uint8_t c) {
    if (!lcd_text_enabled)
        return;
    used(devn);
#ifdef HARDWARE_SCROLL
    lcd_term_cursor_tick();
#endif
    lcd_term_cursor_hide();
    lcd_term_putc(c);
    lcd_term_cursor_maybe_draw();
}

int  lcd_getc(uint8_t devn){
    //i2c keyboard
    used(devn);
    for (;;) {
#ifdef HARDWARE_SCROLL
        /*
         * In framebuffer/graphics mode we disable the text console output.
         * Avoid any LCD read/modify/write for cursor blinking or view scrolling
         * here, otherwise we can interfere with /dev/fb drawing (shared SPI bus)
         * and also "eat" keys for viewback.
         */
        if (lcd_text_enabled)
            lcd_term_cursor_tick();
#endif
        int c = read_i2c_kbd();
        if (c < 0)
            return -1;
#ifdef HARDWARE_SCROLL
        if (lcd_text_enabled) {
            if (c == 0xF0) { /* PICOCALC_KEY_VIEW_UP */
                lcd_term_view_scroll_up();
                continue;
            }
            if (c == 0xF1) { /* PICOCALC_KEY_VIEW_DOWN */
                lcd_term_view_scroll_down();
                continue;
            }
            if (view_lines)
                lcd_term_view_follow();
        }
#endif
        return c;
    }
}
void lcd_sleeping(uint8_t devn){

}
ttyready_t lcd_ready(uint8_t devn){
    return TTY_READY_NOW;
}

void lcd_text_enable(bool enable)
{
    lcd_text_enabled = enable;
}

void lcd_text_reset(void)
{
    CurrentX = 0;
    CurrentY = 0;
    lcd_term_reset(1);
}

void lcd_draw_rect_bgr(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bgr)
{
    if (!w || !h)
        return;
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if (x + w > LCD_WIDTH)
        w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT)
        h = LCD_HEIGHT - y;
    DrawBufferSPI(x, y, x + w - 1, y + h - 1, (unsigned char *)bgr);
}

unsigned char __not_in_flash_func(HW1SwapSPI)(unsigned char data_out){
	unsigned char data_in=0;
	spi_write_read_blocking(spi1,&data_out,&data_in,1);
	return data_in;
}

void HWReadSPI(unsigned char *buff, int cnt) {
    spi_read_blocking(Pico_LCD_SPI_MOD, 0xff, buff, cnt);
}

void HWSendSPI(const unsigned char *buff, int cnt) {

    spi_write_blocking(Pico_LCD_SPI_MOD, buff, cnt);

}


void PinSetBit(int pin, unsigned int offset) {
    switch (offset) {
        case LATCLR:
            gpio_set_pulls(pin, false, false);
            gpio_pull_down(pin);
            gpio_put(pin, 0);
            return;
        case LATSET:
            gpio_set_pulls(pin, false, false);
            gpio_pull_up(pin);
            gpio_put(pin, 1);
            return;
        case LATINV:
            gpio_xor_mask(1 << pin);
            return;
        case TRISSET:
            gpio_set_dir(pin, GPIO_IN);
            sleep_us(2);
            return;
        case TRISCLR:
            gpio_set_dir(pin, GPIO_OUT);
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
            sleep_us(2);
            return;
        case CNPUSET:
            gpio_set_pulls(pin, true, false);
            return;
        case CNPDSET:
            gpio_set_pulls(pin, false, true);
            return;
        case CNPUCLR:
        case CNPDCLR:
            gpio_set_pulls(pin, false, false);
            return;
        case ODCCLR:
            gpio_set_dir(pin, GPIO_OUT);
            gpio_put(pin, 0);
            sleep_us(2);
            return;
        case ODCSET:
            gpio_set_pulls(pin, true, false);
            gpio_set_dir(pin, GPIO_IN);
            sleep_us(2);
            return;
        case ANSELCLR:
            gpio_set_function(pin, GPIO_FUNC_SIO);
            gpio_set_dir(pin, GPIO_IN);
            return;
        default:
            break;
            //printf("Unknown PinSetBit command");
    }
}

//important for read lcd memory
void ResetController(void) {
    PinSetBit(Pico_LCD_RST, LATSET);
    sleep_us(10000);
    PinSetBit(Pico_LCD_RST, LATCLR);
    sleep_us(10000);
    PinSetBit(Pico_LCD_RST, LATSET);
    sleep_us(200000);
}


void pico_lcd_init() {
#ifdef ILI9488
    ResetController();

    HRes = 320;
#ifdef HARDWARE_SCROLL
    VRes = 480; //logic vertical height
#else
    VRes = 320;
#endif
#if defined(CONFIG_PICOCALC)
    spi_write_command(0xF0);
    spi_write_data(0xC3);
    spi_write_command(0xF0);
    spi_write_data(0x96);
    spi_write_command(TFT_MADCTL);
    spi_write_data(0x48);
    spi_write_command(0x3A);
    spi_write_data(0x06);
    spi_write_command(0xB4);
    spi_write_data(0x00);
    //spi_write_command(0xB6); //RGB Control
    //spi_write_data(0x8A);
    //spi_write_data(0x07);
    //spi_write_data(0x27);	 //320 Gates
    spi_write_command(0xB7);
    spi_write_data(0xC6);
    spi_write_command(0xB9);
    spi_write_data(0x02);
    spi_write_data(0xE0);
    spi_write_command(0xC0);
    spi_write_data(0x80);
    spi_write_data(0x06);
    spi_write_command(0xC1);
    spi_write_data(0x15);
    spi_write_command(0xC2);
    spi_write_data(0xA7);
    spi_write_command(0xC5);//VCOM
    spi_write_data(0x04);
    spi_write_command(0xE8);
    spi_write_data(0x40);
    spi_write_data(0x8A);
    spi_write_data(0x00);
    spi_write_data(0x00);
    spi_write_data(0x29);
    spi_write_data(0x19);
    spi_write_data(0xAA);
    spi_write_data(0x33);
    spi_write_command(0xE0);
    spi_write_data(0xF0);
    spi_write_data(0x06);
    spi_write_data(0x0F);
    spi_write_data(0x05);
    spi_write_data(0x04);
    spi_write_data(0x20);
    spi_write_data(0x37);
    spi_write_data(0x33);
    spi_write_data(0x4C);
    spi_write_data(0x37);
    spi_write_data(0x13);
    spi_write_data(0x14);
    spi_write_data(0x2B);
    spi_write_data(0x31);
    spi_write_command(0xE1);
    spi_write_data(0xF0);
    spi_write_data(0x11);
    spi_write_data(0x1B);
    spi_write_data(0x11);
    spi_write_data(0x0F);
    spi_write_data(0x0A);
    spi_write_data(0x37);
    spi_write_data(0x43);
    spi_write_data(0x4C);
    spi_write_data(0x37);
    spi_write_data(0x13);
    spi_write_data(0x13);
    spi_write_data(0x2C);
    spi_write_data(0x32);
    spi_write_command(0xF0);
    spi_write_data(0x3C);
    spi_write_command(0xF0);
    spi_write_data(0x69);
    spi_write_command(0x35);
    spi_write_data(0x00);
    spi_write_command(TFT_SLPOUT);
    sleep_ms(120); 			   //ms
    spi_write_command(TFT_DISPON);
    sleep_ms(20);
    spi_write_command(TFT_INVON);
    spi_write_command(0x2A);
    spi_write_data(0x00);
    spi_write_data(0x00);
    spi_write_data(0x01);
    spi_write_data(0x3F);
    spi_write_command(0x2B);
    spi_write_data(0x00);
    spi_write_data(0x00);
    spi_write_data(0x01);
    spi_write_data(0x3F);
    spi_write_command(0x2C);
#else
    spi_write_command(0xE0); // Positive Gamma Control
    spi_write_data(0x00);
    spi_write_data(0x03);
    spi_write_data(0x09);
    spi_write_data(0x08);
    spi_write_data(0x16);
    spi_write_data(0x0A);
    spi_write_data(0x3F);
    spi_write_data(0x78);
    spi_write_data(0x4C);
    spi_write_data(0x09);
    spi_write_data(0x0A);
    spi_write_data(0x08);
    spi_write_data(0x16);
    spi_write_data(0x1A);
    spi_write_data(0x0F);

    spi_write_command(0XE1); // Negative Gamma Control
    spi_write_data(0x00);
    spi_write_data(0x16);
    spi_write_data(0x19);
    spi_write_data(0x03);
    spi_write_data(0x0F);
    spi_write_data(0x05);
    spi_write_data(0x32);
    spi_write_data(0x45);
    spi_write_data(0x46);
    spi_write_data(0x04);
    spi_write_data(0x0E);
    spi_write_data(0x0D);
    spi_write_data(0x35);
    spi_write_data(0x37);
    spi_write_data(0x0F);

    spi_write_command(0XC0); // Power Control 1
    spi_write_data(0x17);
    spi_write_data(0x15);

    spi_write_command(0xC1); // Power Control 2
    spi_write_data(0x41);

    spi_write_command(0xC5); // VCOM Control
    spi_write_data(0x00);
    spi_write_data(0x12);
    spi_write_data(0x80);

    spi_write_command(TFT_MADCTL); // Memory Access Control
    spi_write_data(0x48); // MX, BGR

    spi_write_command(0x3A); // Pixel Interface Format
    spi_write_data(0x66); // 18 bit colour for SPI

    spi_write_command(0xB0); // Interface Mode Control
    spi_write_data(0x00);

    spi_write_command(0xB1); // Frame Rate Control
    spi_write_data(0xA0);

    spi_write_command(TFT_INVON);

    spi_write_command(0xB4); // Display Inversion Control
    spi_write_data(0x02);

    spi_write_command(0xB6); // Display Function Control
    spi_write_data(0x02);
    spi_write_data(0x02);
    spi_write_data(0x3B);

    spi_write_command(0xB7); // Entry Mode Set
    spi_write_data(0xC6);
    spi_write_command(0xE9);
    spi_write_data(0x00);

    spi_write_command(0xF7); // Adjust Control 3
    spi_write_data(0xA9);
    spi_write_data(0x51);
    spi_write_data(0x2C);
    spi_write_data(0x82);

    spi_write_command(TFT_SLPOUT); //Exit Sleep
    sleep_ms(120);

    spi_write_command(TFT_DISPON); //Display on
    sleep_ms(120);

    spi_write_command(TFT_MADCTL);
    spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);
#endif
#ifdef HARDWARE_SCROLL
    setScrollArea(0, 0);
#endif
#endif
}

void lcd_spi_raise_cs(void) {
    gpio_put(Pico_LCD_CS, 1);
}

void lcd_spi_lower_cs(void) {

    gpio_put(Pico_LCD_CS, 0);

}

void spi_write_data(unsigned char data) {
    gpio_put(Pico_LCD_DC, 1);
    lcd_spi_lower_cs();
    HWSendSPI(&data, 1);
    lcd_spi_raise_cs();
}

void spi_write_data24(uint32_t data) {
    uint8_t data_array[3];
    data_array[0] = data >> 16;
    data_array[1] = (data >> 8) & 0xFF;
    data_array[2] = data & 0xFF;


    gpio_put(Pico_LCD_DC, 1); // Data mode
    gpio_put(Pico_LCD_CS, 0);
    spi_write_blocking(Pico_LCD_SPI_MOD, data_array, 3);
    gpio_put(Pico_LCD_CS, 1);
}

void spi_write_command(unsigned char data) {
    gpio_put(Pico_LCD_DC, 0);
    gpio_put(Pico_LCD_CS, 0);

    spi_write_blocking(Pico_LCD_SPI_MOD, &data, 1);

    gpio_put(Pico_LCD_CS, 1);
}

void spi_write_cd(unsigned char command, int data, ...) {
    int i;
    va_list ap;
    va_start(ap, data);
    spi_write_command(command);
    for (i = 0; i < data; i++) spi_write_data((char) va_arg(ap, int));
    va_end(ap);
}

void lcd_spi_init() {
    // 初始化 GPIO
    gpio_init(Pico_LCD_SCK);
    gpio_init(Pico_LCD_TX);
    gpio_init(Pico_LCD_RX);
    gpio_init(Pico_LCD_CS);
    gpio_init(Pico_LCD_DC);
    gpio_init(Pico_LCD_RST);

    gpio_set_dir(Pico_LCD_SCK, GPIO_OUT);
    gpio_set_dir(Pico_LCD_TX, GPIO_OUT);
    //gpio_set_dir(Pico_LCD_RX, GPIO_IN);
    gpio_set_dir(Pico_LCD_CS, GPIO_OUT);
    gpio_set_dir(Pico_LCD_DC, GPIO_OUT);
    gpio_set_dir(Pico_LCD_RST, GPIO_OUT);

    // 初始化 SPI
    spi_init(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
    gpio_set_function(Pico_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(Pico_LCD_TX, GPIO_FUNC_SPI);
    gpio_set_function(Pico_LCD_RX, GPIO_FUNC_SPI);
    gpio_set_input_hysteresis_enabled(Pico_LCD_RX, true);

    gpio_put(Pico_LCD_CS, 1);
    gpio_put(Pico_LCD_RST, 1);
}

void setBacklight(int level){//STM32: i2c reg is REG_ID_BKL(0x05)
    //level is 0-100%
    level*=255;
    level/=100;
    I2C_Send_RegData(I2C_KBD_ADDR,0x05,(uint8_t)level);
}

void lcd_init() {

    lcd_spi_init();
    pico_lcd_init();
    init_fonts();
    SetFont(0x01);
    gui_fcolour = GREEN;
    gui_bcolour = BLACK;
    lcd_term_reset(1);

}
