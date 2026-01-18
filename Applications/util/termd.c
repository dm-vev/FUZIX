#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/graphics.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#include "termd_font8x8.h"

#define FONT_W 8
#define FONT_H 8
#define MAX_PARAMS 16
#define MAX_PTY 8

#define ATTR_BOLD 0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_INVERSE 0x04

enum parser_state {
    ST_NORMAL = 0,
    ST_ESC,
    ST_CSI,
    ST_CHARSET_G0,
    ST_CHARSET_G1
};

enum charset_id {
    CHARSET_ASCII = 0,
    CHARSET_DEC_SPECIAL
};

struct cell {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
    unsigned char attr;
};

struct gfx_box {
    uint16_t size;
    uint16_t y;
    uint16_t x;
    uint16_t h;
    uint16_t w;
};

struct color {
    unsigned char b;
    unsigned char g;
    unsigned char r;
};

static const struct color palette[16] = {
    {0x00, 0x00, 0x00},
    {0x80, 0x00, 0x00},
    {0x00, 0x00, 0x80},
    {0x80, 0x00, 0x80},
    {0x00, 0x80, 0x00},
    {0x80, 0x80, 0x00},
    {0x00, 0x80, 0x80},
    {0xc0, 0xc0, 0xc0},
    {0x80, 0x80, 0x80},
    {0xff, 0x00, 0x00},
    {0x00, 0x00, 0xff},
    {0xff, 0x00, 0xff},
    {0x00, 0xff, 0x00},
    {0xff, 0xff, 0x00},
    {0x00, 0xff, 0xff},
    {0xff, 0xff, 0xff}
};

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t child_dead = 0;

static int fbfd = -1;
static int ptyfd = -1;
static int kbdfd = -1;

static struct display fb_disp;
static struct termios kbd_term_saved;

static struct cell *cells;
static struct cell *cells_alt;
static int cols;
static int rows;
static int scroll_top;
static int scroll_bottom;
static int cursor_row;
static int cursor_col;
static int saved_row;
static int saved_col;
static unsigned char saved_attr;
static unsigned char saved_fg;
static unsigned char saved_bg;
static unsigned char cur_attr;
static unsigned char cur_fg;
static unsigned char cur_bg;
static unsigned char *tabs;
static int *dirty_min;
static int *dirty_max;
static int wrap_pending;
static int wrap_enabled = 1;
static int insert_mode;
static int origin_mode;
static int cursor_visible;
static int last_cursor_row = -1;
static int last_cursor_col = -1;
static int last_cursor_visible = 1;
static int use_alt;
static enum charset_id charset_g0 = CHARSET_ASCII;
static enum charset_id charset_g1 = CHARSET_ASCII;
static int charset_shift;

static enum parser_state parse_state = ST_NORMAL;
static int params[MAX_PARAMS];
static int param_count;
static int param_value;
static int param_seen;
static int dec_private;

static unsigned char *render_buf;
static unsigned char *out_buf;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void sig_child(int sig)
{
    (void)sig;
    child_dead = 1;
}

static void mark_dirty(int row, int col)
{
    if (row < 0 || row >= rows)
        return;
    if (dirty_min[row] < 0 || col < dirty_min[row])
        dirty_min[row] = col;
    if (dirty_max[row] < 0 || col > dirty_max[row])
        dirty_max[row] = col;
}

static void mark_dirty_span(int row, int start, int end)
{
    if (row < 0 || row >= rows)
        return;
    if (start < 0)
        start = 0;
    if (end >= cols)
        end = cols - 1;
    if (start > end)
        return;
    if (dirty_min[row] < 0 || start < dirty_min[row])
        dirty_min[row] = start;
    if (dirty_max[row] < 0 || end > dirty_max[row])
        dirty_max[row] = end;
}

static void mark_dirty_rows(int start, int end)
{
    int r;
    if (start < 0)
        start = 0;
    if (end >= rows)
        end = rows - 1;
    for (r = start; r <= end; r++)
        mark_dirty_span(r, 0, cols - 1);
}

static unsigned char map_dec_special(unsigned char c)
{
    switch (c) {
    case '`':
        return 0x04;
    case 'a':
        return 0xb1;
    case 'f':
        return 0xf8;
    case 'g':
        return 0xf1;
    case 'j':
        return 0xd9;
    case 'k':
        return 0xbf;
    case 'l':
        return 0xda;
    case 'm':
        return 0xc0;
    case 'n':
        return 0xc5;
    case 'q':
        return 0xc4;
    case 't':
        return 0xc3;
    case 'u':
        return 0xb4;
    case 'v':
        return 0xc1;
    case 'w':
        return 0xc2;
    case 'x':
        return 0xb3;
    case 'y':
        return 0xf3;
    case 'z':
        return 0xf2;
    case '{':
        return 0xe3;
    case '|':
        return 0xf7;
    case '}':
        return 0x9c;
    case '~':
        return 0xfa;
    default:
        return c;
    }
}

static unsigned char translate_char(unsigned char c)
{
    enum charset_id set = charset_shift ? charset_g1 : charset_g0;
    if (set == CHARSET_DEC_SPECIAL && c >= 0x60 && c <= 0x7e)
        return map_dec_special(c);
    return c;
}

static void blank_cell(struct cell *cell)
{
    cell->ch = ' ';
    cell->fg = cur_fg;
    cell->bg = cur_bg;
    cell->attr = cur_attr;
}

static void clear_row(int row)
{
    int c;
    struct cell *line = cells + row * cols;
    for (c = 0; c < cols; c++)
        blank_cell(&line[c]);
    mark_dirty_span(row, 0, cols - 1);
}

static void clear_screen(void)
{
    int r;
    for (r = 0; r < rows; r++)
        clear_row(r);
}

static void scroll_up(int top, int bottom, int count)
{
    int height = bottom - top + 1;
    int r;
    if (count <= 0 || count > height)
        return;
    memmove(cells + top * cols, cells + (top + count) * cols,
            (height - count) * cols * sizeof(*cells));
    for (r = bottom - count + 1; r <= bottom; r++)
        clear_row(r);
    mark_dirty_rows(top, bottom);
}

static void scroll_down(int top, int bottom, int count)
{
    int height = bottom - top + 1;
    int r;
    if (count <= 0 || count > height)
        return;
    memmove(cells + (top + count) * cols, cells + top * cols,
            (height - count) * cols * sizeof(*cells));
    for (r = top; r < top + count; r++)
        clear_row(r);
    mark_dirty_rows(top, bottom);
}

static void cursor_fix(void)
{
    int max_row = origin_mode ? scroll_bottom : rows - 1;
    int min_row = origin_mode ? scroll_top : 0;

    if (cursor_col < 0)
        cursor_col = 0;
    if (cursor_col >= cols)
        cursor_col = cols - 1;
    if (cursor_row < min_row)
        cursor_row = min_row;
    if (cursor_row > max_row)
        cursor_row = max_row;
}

static void do_newline(void)
{
    int bottom = origin_mode ? scroll_bottom : rows - 1;
    int top = origin_mode ? scroll_top : 0;

    if (cursor_row == bottom) {
        scroll_up(top, bottom, 1);
    } else {
        cursor_row++;
    }
    wrap_pending = 0;
}

static void set_cell(int row, int col, unsigned char ch)
{
    struct cell *cell;

    if (row < 0 || row >= rows || col < 0 || col >= cols)
        return;
    cell = &cells[row * cols + col];
    cell->ch = ch;
    cell->fg = cur_fg;
    cell->bg = cur_bg;
    cell->attr = cur_attr;
    mark_dirty(row, col);
}

static void insert_blank_cells(int row, int col, int count)
{
    int c;
    struct cell *line = cells + row * cols;

    if (count <= 0)
        return;
    if (count > cols - col)
        count = cols - col;
    memmove(&line[col + count], &line[col], (cols - col - count) * sizeof(*line));
    for (c = 0; c < count; c++)
        blank_cell(&line[col + c]);
    mark_dirty_span(row, col, cols - 1);
}

static void delete_cells(int row, int col, int count)
{
    int c;
    struct cell *line = cells + row * cols;

    if (count <= 0)
        return;
    if (count > cols - col)
        count = cols - col;
    memmove(&line[col], &line[col + count], (cols - col - count) * sizeof(*line));
    for (c = cols - count; c < cols; c++)
        blank_cell(&line[c]);
    mark_dirty_span(row, col, cols - 1);
}

static void erase_line(int mode)
{
    int c;
    struct cell *line = cells + cursor_row * cols;

    if (mode == 2) {
        for (c = 0; c < cols; c++)
            blank_cell(&line[c]);
        mark_dirty_span(cursor_row, 0, cols - 1);
        return;
    }
    if (mode == 1) {
        for (c = 0; c <= cursor_col; c++)
            blank_cell(&line[c]);
        mark_dirty_span(cursor_row, 0, cursor_col);
        return;
    }
    for (c = cursor_col; c < cols; c++)
        blank_cell(&line[c]);
    mark_dirty_span(cursor_row, cursor_col, cols - 1);
}

static void erase_display(int mode)
{
    int r;

    if (mode == 2) {
        clear_screen();
        return;
    }
    if (mode == 1) {
        for (r = 0; r < cursor_row; r++)
            clear_row(r);
        erase_line(1);
        return;
    }
    erase_line(0);
    for (r = cursor_row + 1; r < rows; r++)
        clear_row(r);
}

static void set_cursor(int row, int col)
{
    cursor_row = row;
    cursor_col = col;
    cursor_fix();
}

static void reset_terminal(void)
{
    cur_attr = 0;
    cur_fg = 7;
    cur_bg = 0;
    cursor_row = 0;
    cursor_col = 0;
    saved_row = 0;
    saved_col = 0;
    saved_attr = 0;
    saved_fg = 7;
    saved_bg = 0;
    scroll_top = 0;
    scroll_bottom = rows - 1;
    wrap_pending = 0;
    wrap_enabled = 1;
    insert_mode = 0;
    origin_mode = 0;
    cursor_visible = 1;
    last_cursor_row = -1;
    last_cursor_col = -1;
    last_cursor_visible = cursor_visible;
    charset_g0 = CHARSET_ASCII;
    charset_g1 = CHARSET_ASCII;
    charset_shift = 0;
    clear_screen();
}

static int cell_with_cursor(int row, int col)
{
    return cursor_visible && row == cursor_row && col == cursor_col;
}

static void render_span(int row, int start_col, int end_col, unsigned char *buf)
{
    int col;
    int y;
    int span_cols = end_col - start_col + 1;
    int line_bytes = span_cols * FONT_W * 3;

    for (y = 0; y < FONT_H; y++) {
        unsigned char *dst = buf + y * line_bytes;
        for (col = start_col; col <= end_col; col++) {
            struct cell *cell = &cells[row * cols + col];
            unsigned char glyph = termd_font8x8[cell->ch * 8 + y];
            unsigned char fg = cell->fg;
            unsigned char bg = cell->bg;
            unsigned char attr = cell->attr;
            int bold = (attr & ATTR_BOLD) && fg < 8;
            int underline = (attr & ATTR_UNDERLINE) && (y == FONT_H - 1);
            int inverse = (attr & ATTR_INVERSE);
            struct color fgc;
            struct color bgc;
            int bit;

            if (bold)
                fg += 8;
            fgc = palette[fg & 0x0f];
            bgc = palette[bg & 0x0f];
            if (inverse) {
                struct color tmp = fgc;
                fgc = bgc;
                bgc = tmp;
            }
            if (cell_with_cursor(row, col)) {
                struct color tmp = fgc;
                fgc = bgc;
                bgc = tmp;
            }
            for (bit = 0; bit < FONT_W; bit++) {
                int on = (glyph & (0x80 >> bit)) != 0;
                const struct color *c = on || underline ? &fgc : &bgc;
                *dst++ = c->b;
                *dst++ = c->g;
                *dst++ = c->r;
            }
        }
    }
}

static void flush_dirty(void)
{
    int r;
    if (!render_buf || !out_buf)
        return;
    if (cursor_visible != last_cursor_visible ||
        cursor_row != last_cursor_row ||
        cursor_col != last_cursor_col) {
        if (last_cursor_visible && last_cursor_row >= 0 && last_cursor_col >= 0)
            mark_dirty(last_cursor_row, last_cursor_col);
        if (cursor_visible)
            mark_dirty(cursor_row, cursor_col);
        last_cursor_row = cursor_row;
        last_cursor_col = cursor_col;
        last_cursor_visible = cursor_visible;
    }
    for (r = 0; r < rows; r++) {
        int start = dirty_min[r];
        int end = dirty_max[r];
        if (start < 0 || end < 0)
            continue;
        render_span(r, start, end, render_buf);
        {
            int span_cols = end - start + 1;
            int line_bytes = span_cols * FONT_W * 3;
            int data_bytes = line_bytes * FONT_H;
            struct gfx_box *box = (struct gfx_box *)out_buf;
            unsigned char *payload = out_buf + sizeof(*box);
            int payload_bytes = data_bytes;

            memcpy(payload, render_buf, data_bytes);
            box->size = sizeof(*box) + payload_bytes;
            box->y = r * FONT_H;
            box->x = start * FONT_W;
            box->h = FONT_H;
            box->w = span_cols * FONT_W;
            ioctl(fbfd, GFXIOC_WRITE, box);
        }
        dirty_min[r] = -1;
        dirty_max[r] = -1;
    }
}

static void send_response(const char *s)
{
    if (ptyfd >= 0 && s)
        write(ptyfd, s, strlen(s));
}

static int get_param_raw(int idx, int defval)
{
    if (idx >= param_count)
        return defval;
    return params[idx];
}

static int get_param_or(int idx, int defval)
{
    int val = get_param_raw(idx, defval);
    if (val == 0)
        return defval;
    return val;
}

static void apply_sgr(void)
{
    int i;

    if (param_count == 0) {
        cur_attr = 0;
        cur_fg = 7;
        cur_bg = 0;
        return;
    }
    for (i = 0; i < param_count; i++) {
        int p = params[i];
        if (p == 0) {
            cur_attr = 0;
            cur_fg = 7;
            cur_bg = 0;
        } else if (p == 1) {
            cur_attr |= ATTR_BOLD;
        } else if (p == 4) {
            cur_attr |= ATTR_UNDERLINE;
        } else if (p == 7) {
            cur_attr |= ATTR_INVERSE;
        } else if (p == 22) {
            cur_attr &= (unsigned char)~ATTR_BOLD;
        } else if (p == 24) {
            cur_attr &= (unsigned char)~ATTR_UNDERLINE;
        } else if (p == 27) {
            cur_attr &= (unsigned char)~ATTR_INVERSE;
        } else if (p >= 30 && p <= 37) {
            cur_fg = (unsigned char)(p - 30);
        } else if (p >= 40 && p <= 47) {
            cur_bg = (unsigned char)(p - 40);
        } else if (p == 39) {
            cur_fg = 7;
        } else if (p == 49) {
            cur_bg = 0;
        } else if (p >= 90 && p <= 97) {
            cur_fg = (unsigned char)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            cur_bg = (unsigned char)(p - 100 + 8);
        }
    }
}

static void csi_dispatch(unsigned char c)
{
    int p1 = get_param_or(0, 1);
    int p2 = get_param_or(1, 1);

    switch (c) {
    case 'A':
        cursor_row -= p1;
        cursor_fix();
        break;
    case 'B':
        cursor_row += p1;
        cursor_fix();
        break;
    case 'C':
        cursor_col += p1;
        cursor_fix();
        break;
    case 'D':
        cursor_col -= p1;
        cursor_fix();
        break;
    case 'H':
    case 'f':
        set_cursor((origin_mode ? scroll_top : 0) + p1 - 1, p2 - 1);
        break;
    case 'J':
        erase_display(get_param_raw(0, 0));
        break;
    case 'K':
        erase_line(get_param_raw(0, 0));
        break;
    case 'L':
        scroll_down(cursor_row, scroll_bottom, p1);
        break;
    case 'M':
        scroll_up(cursor_row, scroll_bottom, p1);
        break;
    case 'P':
        delete_cells(cursor_row, cursor_col, p1);
        break;
    case '@':
        insert_blank_cells(cursor_row, cursor_col, p1);
        break;
    case 'X':
    {
        int i;
        for (i = 0; i < p1 && cursor_col + i < cols; i++)
            blank_cell(&cells[cursor_row * cols + cursor_col + i]);
        mark_dirty_span(cursor_row, cursor_col, cursor_col + p1 - 1);
        break;
    }
    case 'S':
        scroll_up(scroll_top, scroll_bottom, p1);
        break;
    case 'T':
        scroll_down(scroll_top, scroll_bottom, p1);
        break;
    case 'r':
        scroll_top = get_param_or(0, 1) - 1;
        scroll_bottom = get_param_or(1, rows) - 1;
        if (scroll_top < 0)
            scroll_top = 0;
        if (scroll_bottom >= rows)
            scroll_bottom = rows - 1;
        if (scroll_top >= scroll_bottom) {
            scroll_top = 0;
            scroll_bottom = rows - 1;
        }
        set_cursor(origin_mode ? scroll_top : 0, 0);
        break;
    case 'm':
        apply_sgr();
        break;
    case 's':
        saved_row = cursor_row;
        saved_col = cursor_col;
        saved_attr = cur_attr;
        saved_fg = cur_fg;
        saved_bg = cur_bg;
        break;
    case 'u':
        cursor_row = saved_row;
        cursor_col = saved_col;
        cur_attr = saved_attr;
        cur_fg = saved_fg;
        cur_bg = saved_bg;
        cursor_fix();
        break;
    case 'g':
        if (get_param_raw(0, 0) == 0 && tabs)
            tabs[cursor_col] = 0;
        if (get_param_raw(0, 0) == 3 && tabs)
            memset(tabs, 0, cols);
        break;
    case 'n':
        if (get_param_raw(0, 0) == 5)
            send_response("\033[0n");
        if (get_param_raw(0, 0) == 6) {
            char resp[32];
            snprintf(resp, sizeof(resp), "\033[%d;%dR", cursor_row + 1, cursor_col + 1);
            send_response(resp);
        }
        break;
    case 'c':
        send_response("\033[?1;0c");
        break;
    case 'h':
        if (dec_private) {
            int i;
            for (i = 0; i < param_count; i++) {
                int p = params[i];
                if (p == 6)
                    origin_mode = 1;
                else if (p == 7) {
                    wrap_enabled = 1;
                    wrap_pending = 0;
                }
                else if (p == 25)
                    cursor_visible = 1;
                else if (p == 47 || p == 1047 || p == 1049) {
                    if (!cells_alt)
                        cells_alt = calloc(rows * cols, sizeof(*cells));
                    if (cells_alt) {
                        if (!use_alt) {
                            memcpy(cells_alt, cells, rows * cols * sizeof(*cells));
                            use_alt = 1;
                            clear_screen();
                            set_cursor(0, 0);
                        }
                    }
                }
            }
        } else {
            if (get_param_raw(0, 0) == 4)
                insert_mode = 1;
        }
        break;
    case 'l':
        if (dec_private) {
            int i;
            for (i = 0; i < param_count; i++) {
                int p = params[i];
                if (p == 6)
                    origin_mode = 0;
                else if (p == 7) {
                    wrap_enabled = 0;
                    wrap_pending = 0;
                }
                else if (p == 25)
                    cursor_visible = 0;
                else if (p == 47 || p == 1047 || p == 1049) {
                    if (cells_alt && use_alt) {
                        memcpy(cells, cells_alt, rows * cols * sizeof(*cells));
                        use_alt = 0;
                        set_cursor(saved_row, saved_col);
                        mark_dirty_rows(0, rows - 1);
                    }
                }
            }
        } else {
            if (get_param_raw(0, 0) == 4)
                insert_mode = 0;
        }
        break;
    default:
        break;
    }
}

static void push_param(void)
{
    if (param_count >= MAX_PARAMS)
        return;
    if (param_seen)
        params[param_count++] = param_value;
    else
        params[param_count++] = 0;
    param_value = 0;
    param_seen = 0;
}

static void reset_params(void)
{
    param_count = 0;
    param_value = 0;
    param_seen = 0;
    dec_private = 0;
}

static void term_putc(unsigned char c)
{
    if (parse_state == ST_CHARSET_G0) {
        charset_g0 = (c == '0') ? CHARSET_DEC_SPECIAL : CHARSET_ASCII;
        parse_state = ST_NORMAL;
        return;
    }
    if (parse_state == ST_CHARSET_G1) {
        charset_g1 = (c == '0') ? CHARSET_DEC_SPECIAL : CHARSET_ASCII;
        parse_state = ST_NORMAL;
        return;
    }

    switch (parse_state) {
    case ST_ESC:
        if (c == '[') {
            parse_state = ST_CSI;
            reset_params();
            return;
        }
        if (c == '(') {
            parse_state = ST_CHARSET_G0;
            return;
        }
        if (c == ')') {
            parse_state = ST_CHARSET_G1;
            return;
        }
        if (c == '7') {
            saved_row = cursor_row;
            saved_col = cursor_col;
            saved_attr = cur_attr;
            saved_fg = cur_fg;
            saved_bg = cur_bg;
        } else if (c == '8') {
            cursor_row = saved_row;
            cursor_col = saved_col;
            cur_attr = saved_attr;
            cur_fg = saved_fg;
            cur_bg = saved_bg;
            cursor_fix();
        } else if (c == 'D') {
            do_newline();
        } else if (c == 'M') {
            if (cursor_row == scroll_top)
                scroll_down(scroll_top, scroll_bottom, 1);
            else if (cursor_row > 0)
                cursor_row--;
        } else if (c == 'E') {
            cursor_col = 0;
            do_newline();
        } else if (c == 'H') {
            if (tabs)
                tabs[cursor_col] = 1;
        } else if (c == 'Z') {
            send_response("\033[?1;0c");
        } else if (c == 'c') {
            reset_terminal();
        }
        parse_state = ST_NORMAL;
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            param_value = param_value * 10 + (c - '0');
            param_seen = 1;
            return;
        }
        if (c == ';') {
            push_param();
            return;
        }
        if (c == '?' && !param_seen && param_count == 0) {
            dec_private = 1;
            return;
        }
        push_param();
        csi_dispatch(c);
        parse_state = ST_NORMAL;
        return;
    case ST_NORMAL:
    default:
        break;
    }

    if (c == 0x1b) {
        parse_state = ST_ESC;
        return;
    }
    if (c == 0x0e) {
        charset_shift = 1;
        return;
    }
    if (c == 0x0f) {
        charset_shift = 0;
        return;
    }
    if (c == '\r') {
        cursor_col = 0;
        wrap_pending = 0;
        return;
    }
    if (c == '\n' || c == '\v' || c == '\f') {
        do_newline();
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0)
            cursor_col--;
        wrap_pending = 0;
        return;
    }
    if (c == '\t') {
        int next = cursor_col + 1;
        if (tabs) {
            while (next < cols && !tabs[next])
                next++;
        } else {
            next = (cursor_col + 8) & ~7;
        }
        if (next >= cols)
            next = cols - 1;
        cursor_col = next;
        return;
    }
    if (c < 0x20)
        return;

    if (wrap_pending && wrap_enabled) {
        cursor_col = 0;
        do_newline();
        wrap_pending = 0;
    }

    c = translate_char(c);

    if (insert_mode)
        insert_blank_cells(cursor_row, cursor_col, 1);
    set_cell(cursor_row, cursor_col, c);

    if (cursor_col == cols - 1) {
        if (!wrap_enabled)
            return;
        wrap_pending = 1;
    } else {
        cursor_col++;
    }
}

static int open_kbd(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    struct termios t;

    if (fd < 0)
        return -1;
    if (tcgetattr(fd, &kbd_term_saved) == 0) {
        t = kbd_term_saved;
        cfmakeraw(&t);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;
        tcsetattr(fd, TCSANOW, &t);
    }
    return fd;
}

static void restore_kbd(void)
{
    if (kbdfd >= 0)
        tcsetattr(kbdfd, TCSANOW, &kbd_term_saved);
}

static int open_fb(int mode)
{
    struct display disp;

    fbfd = open("/dev/fb", O_RDWR);
    if (fbfd < 0)
        return -1;
    if (ioctl(fbfd, FBIOC_LOCK, 0) != 0)
        goto fail;

    memset(&disp, 0, sizeof(disp));
    disp.mode = (unsigned char)mode;
    if (ioctl(fbfd, GFXIOC_SETMODE, &disp) != 0)
        goto fail;
    if (ioctl(fbfd, GFXIOC_GETINFO, &fb_disp) != 0)
        goto fail;
    return 0;

fail:
    close(fbfd);
    fbfd = -1;
    return -1;
}

static void shutdown_fb(void)
{
    struct display disp;

    if (fbfd < 0)
        return;
    memset(&disp, 0, sizeof(disp));
    disp.mode = FB_MODE_TEXT;
    ioctl(fbfd, GFXIOC_SETMODE, &disp);
    ioctl(fbfd, FBIOC_UNLOCK, 0);
    close(fbfd);
    fbfd = -1;
}

static int open_pty(int *idx)
{
    int i;
    char path[32];

    for (i = 0; i < MAX_PTY; i++) {
        snprintf(path, sizeof(path), "/dev/pty%d", i);
        ptyfd = open(path, O_RDWR | O_NDELAY);
        if (ptyfd >= 0) {
            if (idx)
                *idx = i;
            return 0;
        }
    }
    return -1;
}

static pid_t spawn_child(int idx, char **argv)
{
    char path[32];
    pid_t pid;

    pid = fork();
    if (pid != 0)
        return pid;

    setsid();
    if (ptyfd >= 0)
        close(ptyfd);
    if (kbdfd >= 0)
        close(kbdfd);
    snprintf(path, sizeof(path), "/dev/ptty%d", idx);
    kbdfd = open(path, O_RDWR);
    if (kbdfd < 0)
        _exit(1);
    {
        struct winsize ws;
        ws.ws_row = rows;
        ws.ws_col = cols;
        ws.ws_xpixel = fb_disp.width;
        ws.ws_ypixel = fb_disp.height;
        ioctl(kbdfd, TIOCSWINSZ, &ws);
    }
    dup(kbdfd);
    dup(kbdfd);
    if (kbdfd > 2)
        close(kbdfd);
    setenv("TERM", "vt100", 1);
    if (argv && argv[0])
        execvp(argv[0], argv);
    execl("/bin/sh", "sh", (char *)0);
    _exit(1);
    return -1;
}

static void init_tabs(void)
{
    int i;
    tabs = calloc(cols, sizeof(*tabs));
    if (!tabs)
        return;
    for (i = 0; i < cols; i += 8)
        tabs[i] = 1;
}

static int termd_init(int fb_mode, int *pty_idx)
{
    if (open_fb(fb_mode) != 0) {
        perror("termd: /dev/fb");
        return -1;
    }
    cols = fb_disp.width / FONT_W;
    rows = fb_disp.height / FONT_H;
    if (cols <= 0 || rows <= 0)
        return -1;

    cells = calloc(rows * cols, sizeof(*cells));
    if (!cells)
        return -1;
    dirty_min = malloc(rows * sizeof(*dirty_min));
    dirty_max = malloc(rows * sizeof(*dirty_max));
    if (!dirty_min || !dirty_max)
        return -1;
    {
        int buf_bytes = cols * FONT_W * FONT_H * 3;
        render_buf = malloc(buf_bytes);
        out_buf = malloc(buf_bytes + sizeof(struct gfx_box));
    }
    if (!render_buf || !out_buf)
        return -1;
    memset(dirty_min, 0xff, rows * sizeof(*dirty_min));
    memset(dirty_max, 0xff, rows * sizeof(*dirty_max));
    init_tabs();

    scroll_top = 0;
    scroll_bottom = rows - 1;
    reset_terminal();

    if (open_pty(pty_idx) != 0) {
        perror("termd: /dev/pty");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int fb_mode = FB_MODE_DIRECT;
    const char *kbd_path = "/dev/tty1";
    int i;
    int pty_idx = -1;
    char **cmd = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            fb_mode = FB_MODE_MEMORY;
        } else if (strcmp(argv[i], "-d") == 0) {
            fb_mode = FB_MODE_DIRECT;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            kbd_path = argv[++i];
        } else if (strcmp(argv[i], "--") == 0) {
            cmd = &argv[i + 1];
            break;
        } else if (argv[i][0] != '-') {
            cmd = &argv[i];
            break;
        }
    }

    kbdfd = open_kbd(kbd_path);
    if (kbdfd < 0) {
        perror("termd: keyboard");
        return 1;
    }
    if (fcntl(kbdfd, F_SETFL, O_NDELAY) < 0)
        perror("termd: fcntl kbd");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sig_handler);
    signal(SIGCHLD, sig_child);

    if (termd_init(fb_mode, &pty_idx) != 0) {
        restore_kbd();
        shutdown_fb();
        return 1;
    }

    if (spawn_child(pty_idx, cmd) < 0) {
        perror("termd: spawn");
        running = 0;
    }

    while (running) {
        unsigned char buf[128];
        ssize_t n;
        int idle = 1;

        if (child_dead)
        {
            int status;
            while (waitpid(-1, &status, WNOHANG) > 0) {
            }
            break;
        }

        n = read(ptyfd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t k;
            for (k = 0; k < n; k++)
                term_putc(buf[k]);
            flush_dirty();
            idle = 0;
        }

        n = read(kbdfd, buf, sizeof(buf));
        if (n > 0) {
            write(ptyfd, buf, n);
            idle = 0;
        }

        if (idle)
            usleep(2000);
    }

    restore_kbd();
    shutdown_fb();
    if (ptyfd >= 0)
        close(ptyfd);
    if (kbdfd >= 0)
        close(kbdfd);
    free(render_buf);
    free(out_buf);
    free(cells);
    free(cells_alt);
    free(dirty_min);
    free(dirty_max);
    free(tabs);
    return 0;
}
