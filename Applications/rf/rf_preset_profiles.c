#include "rf_preset_profiles.h"

#include "rf_draw.h"
#include "rf_fs.h"
#include "rf_keys.h"
#include "rf_presets.h"
#include "rf_recording.h"
#include "rf_task.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const preset_dir = "/rf";
static const char *const presets_dir = "/rf/presets";
static const char *const preset_ext = ".cfg";
static const char *const autoload_path = "/rf/presets/autoload.txt";
static const char *const exports_dir = "/rf/exports";

static int ends_with(const char *s, const char *suffix)
{
	if (!s || !suffix)
		return 0;
	size_t sl = strlen(s);
	size_t tl = strlen(suffix);
	if (tl > sl)
		return 0;
	return memcmp(s + sl - tl, suffix, tl) == 0;
}

static void trim_inplace(char *s)
{
	if (!s)
		return;
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
		s[n - 1] = 0;
		n--;
	}
}

static int ensure_presets_dir(char *err, size_t errsz)
{
	if (rf_fs_ensure_dir(preset_dir, 0755, err, errsz) != 0)
		return -1;
	if (rf_fs_ensure_dir(presets_dir, 0755, err, errsz) != 0)
		return -1;
	return 0;
}

static int ensure_exports_dir(char *err, size_t errsz)
{
	if (rf_fs_ensure_dir(preset_dir, 0755, err, errsz) != 0)
		return -1;
	if (rf_fs_ensure_dir(exports_dir, 0755, err, errsz) != 0)
		return -1;
	return 0;
}

static int presets_list_rows(const struct rf_task *t)
{
	int rows = 10;
	if (t)
		rows = t->rows - RF_HEADER_ROWS - RF_STATUS_ROWS - 10;
	if (rows < 6)
		rows = 6;
	if (rows > 14)
		rows = 14;
	return rows;
}

static int cmp_name32(const void *a, const void *b)
{
	const char *sa = (const char *)a;
	const char *sb = (const char *)b;
	return strcmp(sa, sb);
}

static void read_autoload_preset(struct rf_task *t)
{
	if (!t)
		return;

	t->autoload_preset[0] = 0;

	int fd = open(autoload_path, O_RDONLY);
	if (fd < 0)
		return;

	char buf[72];
	memset(buf, 0, sizeof(buf));
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	(void)close(fd);
	if (n <= 0)
		return;

	buf[sizeof(buf) - 1] = 0;
	trim_inplace(buf);
	if (!buf[0])
		return;

	if (ends_with(buf, preset_ext))
		buf[strlen(buf) - strlen(preset_ext)] = 0;

	char safe[32];
	rf_sanitize_name(buf, safe, sizeof(safe));
	if (!safe[0])
		return;

	snprintf(t->autoload_preset, sizeof(t->autoload_preset), "%s", safe);
}

static int write_autoload_preset(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad task");
		return -1;
	}
	if (ensure_presets_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	rf_sanitize_name(name ? name : "", safe, sizeof(safe));

	char line[40];
	snprintf(line, sizeof(line), "%s\n", safe);

	int fd = open(autoload_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", autoload_path, strerror(errno));
		return -1;
	}
	int rc = rf_fs_write_all(fd, line, strlen(line), err, errsz);
	(void)close(fd);
	if (rc != 0)
		return -1;

	snprintf(t->autoload_preset, sizeof(t->autoload_preset), "%s", safe);
	return 0;
}

static int export_preset_to_exports(const struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad task");
		return -1;
	}
	if (ensure_exports_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	rf_sanitize_name(name ? name : "", safe, sizeof(safe));
	if (!safe[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid preset");
		return -1;
	}

	char src[96];
	char dst[96];
	if (snprintf(src, sizeof(src), "%s/%s%s", presets_dir, safe, preset_ext) >= (int)sizeof(src)) {
		if (err && errsz)
			snprintf(err, errsz, "path too long");
		return -1;
	}
	if (snprintf(dst, sizeof(dst), "%s/%s%s", exports_dir, safe, preset_ext) >= (int)sizeof(dst)) {
		if (err && errsz)
			snprintf(err, errsz, "path too long");
		return -1;
	}

	int sfd = open(src, O_RDONLY);
	if (sfd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", src, strerror(errno));
		return -1;
	}
	int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (dfd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", dst, strerror(errno));
		(void)close(sfd);
		return -1;
	}

	char buf[256];
	for (;;) {
		ssize_t n = read(sfd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (err && errsz)
				snprintf(err, errsz, "read %s: %s", src, strerror(errno));
			(void)close(sfd);
			(void)close(dfd);
			return -1;
		}
		if (n == 0)
			break;
		if (rf_fs_write_all(dfd, buf, (size_t)n, err, errsz) != 0) {
			(void)close(sfd);
			(void)close(dfd);
			return -1;
		}
	}
	(void)close(sfd);
	(void)close(dfd);
	return 0;
}

static void refresh_preset_list(struct rf_task *t)
{
	if (!t)
		return;

	t->preset_list_count = 0;
	t->autoload_err[0] = 0;
	t->autoload_preset[0] = 0;

	char err[96];
	if (ensure_presets_dir(err, sizeof(err)) != 0) {
		snprintf(t->autoload_err, sizeof(t->autoload_err), "%s", err[0] ? err : "presets dir error");
		return;
	}

	DIR *d = opendir(presets_dir);
	if (!d) {
		snprintf(t->autoload_err, sizeof(t->autoload_err), "opendir %s: %s", presets_dir, strerror(errno));
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		const char *name = ent->d_name;
		if (!name || !name[0])
			continue;
		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		if (!ends_with(name, preset_ext))
			continue;

		char base[64];
		snprintf(base, sizeof(base), "%s", name);
		base[strlen(base) - strlen(preset_ext)] = 0;
		char safe[32];
		rf_sanitize_name(base, safe, sizeof(safe));
		if (!safe[0])
			continue;

		if (t->preset_list_count >= (int)(sizeof(t->preset_list) / sizeof(t->preset_list[0])))
			break;
		snprintf(t->preset_list[t->preset_list_count], sizeof(t->preset_list[t->preset_list_count]), "%s", safe);
		t->preset_list_count++;
	}
	(void)closedir(d);

	if (t->preset_list_count > 1) {
		qsort(t->preset_list, (size_t)t->preset_list_count, sizeof(t->preset_list[0]), cmp_name32);
	}

	read_autoload_preset(t);

	if (t->preset_sel < 0)
		t->preset_sel = 0;
	if (t->preset_sel >= t->preset_list_count) {
		t->preset_sel = t->preset_list_count - 1;
		if (t->preset_sel < 0)
			t->preset_sel = 0;
	}
	t->preset_top = 0;
}

void rf_preset_profiles_maybe_autoload(struct rf_task *t)
{
	if (!t)
		return;
	if (t->active_preset[0])
		return;

	read_autoload_preset(t);
	if (!t->autoload_preset[0])
		return;

	char err[96];
	if (rf_presets_load(t, t->autoload_preset, err, sizeof(err)) != 0)
		return;
	rf_recording_record_config(t, t->now_tick);
}

void rf_preset_profiles_open(struct rf_task *t)
{
	if (!t)
		return;
	t->show_presets = 1;
	t->preset_sel = 0;
	t->preset_top = 0;
	refresh_preset_list(t);
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
}

void rf_preset_profiles_close(struct rf_task *t)
{
	if (!t || !t->show_presets)
		return;
	t->show_presets = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
}

void rf_preset_profiles_handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_preset_profiles_close(t);
		return;
	case RF_KEY_RUNE:
		switch (k->r) {
		case 'r':
		case 'R':
			refresh_preset_list(t);
			rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
			return;
		case 'd':
		case 'D':
			if (t->preset_sel >= 0 && t->preset_sel < t->preset_list_count) {
				const char *name = t->preset_list[t->preset_sel];
				char err[96];
				if (write_autoload_preset(t, name, err, sizeof(err)) != 0)
					snprintf(t->autoload_err, sizeof(t->autoload_err), "%s", err[0] ? err : "default set failed");
				else
					snprintf(t->autoload_err, sizeof(t->autoload_err), "default set: %s", t->autoload_preset);
				rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
			}
			return;
		case 'c':
		case 'C': {
			char err[96];
			if (write_autoload_preset(t, "", err, sizeof(err)) != 0)
				snprintf(t->autoload_err, sizeof(t->autoload_err), "%s", err[0] ? err : "default clear failed");
			else
				snprintf(t->autoload_err, sizeof(t->autoload_err), "default cleared");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
			return;
		}
		case 'x':
		case 'X':
			if (t->preset_sel >= 0 && t->preset_sel < t->preset_list_count) {
				const char *name = t->preset_list[t->preset_sel];
				char err[96];
				if (export_preset_to_exports(t, name, err, sizeof(err)) != 0)
					snprintf(t->autoload_err, sizeof(t->autoload_err), "export: %s", err[0] ? err : "failed");
				else
					snprintf(t->autoload_err, sizeof(t->autoload_err), "exported: %s", name);
				rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
			}
			return;
		default:
			break;
		}
		break;
	case RF_KEY_UP:
		if (t->preset_list_count == 0)
			return;
		if (t->preset_sel <= 0)
			t->preset_sel = t->preset_list_count - 1;
		else
			t->preset_sel--;
		break;
	case RF_KEY_DOWN:
		if (t->preset_list_count == 0)
			return;
		if (t->preset_sel >= t->preset_list_count - 1)
			t->preset_sel = 0;
		else
			t->preset_sel++;
		break;
	case RF_KEY_ENTER:
		if (t->preset_sel < 0 || t->preset_sel >= t->preset_list_count)
			return;
		{
			const char *name = t->preset_list[t->preset_sel];
			char err[96];
			if (rf_presets_load(t, name, err, sizeof(err)) != 0)
				snprintf(t->autoload_err, sizeof(t->autoload_err), "%s", err[0] ? err : "load failed");
			else
				t->autoload_err[0] = 0;
			rf_recording_record_config(t, t->now_tick);
			rf_task_invalidate(t, RF_DIRTY_ALL | RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
			return;
		}
	default:
		return;
	}

	int rows = presets_list_rows(t);
	if (t->preset_sel < t->preset_top)
		t->preset_top = t->preset_sel;
	if (t->preset_sel >= t->preset_top + rows)
		t->preset_top = t->preset_sel - rows + 1;
	if (t->preset_top < 0)
		t->preset_top = 0;
	if (t->preset_top > t->preset_list_count - 1) {
		t->preset_top = t->preset_list_count - 1;
		if (t->preset_top < 0)
			t->preset_top = 0;
	}
	rf_task_invalidate(t, RF_DIRTY_OVERLAY);
}

static const char *or_text(const char *s, const char *fallback)
{
	if (!s || !s[0])
		return fallback;
	return s;
}

void rf_preset_profiles_render_overlay(const struct rf_task *t)
{
	if (!t)
		return;

	int box_cols = t->cols - 10;
	if (box_cols > 64)
		box_cols = 64;
	if (box_cols < 34)
		box_cols = 34;

	const int list_rows = presets_list_rows(t);
	const int box_rows = 4 + list_rows;

	int16_t px = (int16_t)(5 * RF_FONT_W);
	int16_t py = (int16_t)((RF_HEADER_ROWS + 2) * RF_FONT_H);
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
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + 1), "Preset Profiles (Esc close)", fg, header, box_cols);

	char status[96];
	if (t->autoload_err[0])
		snprintf(status, sizeof(status), "msg: %s", t->autoload_err);
	else
		snprintf(status, sizeof(status), "active:%s  autoload:%s", or_text(t->active_preset, "(none)"),
			 or_text(t->autoload_preset, "(off)"));
	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + RF_FONT_H + 2), status, dim, panel, box_cols);

	int16_t y0 = (int16_t)(py + 2 * RF_FONT_H + 2);
	for (int row = 0; row < list_rows; row++) {
		int i = t->preset_top + row;
		if (i < 0 || i >= t->preset_list_count)
			continue;
		const char *name = t->preset_list[i];

		char prefix[3];
		prefix[0] = (name[0] && !strcmp(name, t->active_preset)) ? '>' : ' ';
		prefix[1] = (name[0] && !strcmp(name, t->autoload_preset)) ? '*' : ' ';
		prefix[2] = 0;

		char line[96];
		snprintf(line, sizeof(line), "%s %s", prefix, name);

		int16_t yy = (int16_t)(y0 + (int16_t)row * RF_FONT_H);
		struct rf_color line_fg = fg;
		struct rf_color line_bg = panel;
		if (i == t->preset_sel) {
			line_fg = sel_fg;
			line_bg = sel_bg;
		}
		rf_draw_fill_rect(t, (int16_t)(px + 1), yy, (int16_t)(pw - 2), RF_FONT_H, line_bg);
		rf_draw_text(t, (int16_t)(px + 2), yy, line, line_fg, line_bg, box_cols);
	}

	rf_draw_text(t, (int16_t)(px + 2), (int16_t)(py + ph - RF_FONT_H - 1), "Enter load  d default  c clear  x export  r refresh",
		     dim, panel, box_cols);
}
