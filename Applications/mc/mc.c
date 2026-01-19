#include <curses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Minimal 2-panel file manager in the spirit of Midnight Commander.
 *
 * Target: FUZIX (small systems, limited RAM, no fancy deps).
 */

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

enum {
	KEY_ESC = 27
};

enum keycode {
	K_NONE = 0,
	K_ESC = 0x100,
	K_UP,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_F1,
	K_F3,
	K_F4,
	K_F5,
	K_F6,
	K_F7,
	K_F8,
	K_F10,
};

enum sort_mode {
	SORT_NAME = 0,
	SORT_MTIME = 1,
	SORT_SIZE = 2
};

struct entry {
	char *name;
	mode_t mode;
	off_t size;
	time_t mtime;
	bool is_dir;
};

struct panel {
	char cwd[PATH_MAX];
	struct entry *entries;
	size_t count;
	size_t cap;
	size_t selected;
	size_t top;
	bool show_hidden;
	enum sort_mode sort;
};

static struct panel panels[2];
static int active_panel;

static char status_msg[128];
static time_t status_ts;

static void status_set(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
	va_end(ap);
	status_ts = time(NULL);
}

static void entry_free(struct entry *e)
{
	free(e->name);
	e->name = NULL;
}

static void panel_clear(struct panel *p)
{
	for (size_t i = 0; i < p->count; i++)
		entry_free(&p->entries[i]);
	free(p->entries);
	p->entries = NULL;
	p->count = 0;
	p->cap = 0;
	p->selected = 0;
	p->top = 0;
}

static int ensure_cap(struct panel *p, size_t need)
{
	if (need <= p->cap)
		return 0;
	size_t ncap = p->cap ? p->cap * 2 : 64;
	while (ncap < need)
		ncap *= 2;
	struct entry *ne = realloc(p->entries, ncap * sizeof(*ne));
	if (!ne)
		return -1;
	p->entries = ne;
	p->cap = ncap;
	return 0;
}

static void path_clean(char *path)
{
	/* In-place, best-effort cleanup of repeated slashes and trailing slash (except "/"). */
	size_t w = 0;
	bool prev_slash = FALSE;
	for (size_t r = 0; path[r]; r++) {
		char c = path[r];
		if (c == '/') {
			if (prev_slash)
				continue;
			prev_slash = TRUE;
		} else {
			prev_slash = FALSE;
		}
		path[w++] = c;
	}
	path[w] = 0;
	while (w > 1 && path[w - 1] == '/') {
		path[w - 1] = 0;
		w--;
	}
	if (!path[0]) {
		path[0] = '/';
		path[1] = 0;
	}
}

static int path_join(char *out, size_t outsz, const char *a, const char *b)
{
	if (!a || !b || !out || !outsz)
		return -1;
	if (b[0] == '/') {
		if (snprintf(out, outsz, "%s", b) >= (int)outsz)
			return -1;
	} else if (!strcmp(a, "/")) {
		if (snprintf(out, outsz, "/%s", b) >= (int)outsz)
			return -1;
	} else {
		if (snprintf(out, outsz, "%s/%s", a, b) >= (int)outsz)
			return -1;
	}
	path_clean(out);
	return 0;
}

static const char *path_basename(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static int stat_path(const char *path, struct stat *st)
{
	if (lstat(path, st) == 0)
		return 0;
	return -1;
}

static int entry_cmp_name(const void *aa, const void *bb)
{
	const struct entry *a = aa;
	const struct entry *b = bb;
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	return strcasecmp(a->name, b->name);
}

static int entry_cmp_mtime(const void *aa, const void *bb)
{
	const struct entry *a = aa;
	const struct entry *b = bb;
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	if (a->mtime == b->mtime)
		return strcasecmp(a->name, b->name);
	return (a->mtime > b->mtime) ? -1 : 1;
}

static int entry_cmp_size(const void *aa, const void *bb)
{
	const struct entry *a = aa;
	const struct entry *b = bb;
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	if (a->size == b->size)
		return strcasecmp(a->name, b->name);
	return (a->size > b->size) ? -1 : 1;
}

static void panel_sort(struct panel *p)
{
	int (*cmp)(const void *, const void *) = entry_cmp_name;
	switch (p->sort) {
	case SORT_MTIME:
		cmp = entry_cmp_mtime;
		break;
	case SORT_SIZE:
		cmp = entry_cmp_size;
		break;
	case SORT_NAME:
	default:
		cmp = entry_cmp_name;
		break;
	}
	if (p->count)
		qsort(p->entries, p->count, sizeof(p->entries[0]), cmp);
}

static int panel_load(struct panel *p)
{
	DIR *d = opendir(p->cwd);
	if (!d) {
		status_set("opendir: %s", strerror(errno));
		return -1;
	}

	panel_clear(p);

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (!p->show_hidden && name[0] == '.' && strcmp(name, "..") && strcmp(name, "."))
			continue;

		if (ensure_cap(p, p->count + 1) < 0) {
			closedir(d);
			status_set("out of memory");
			return -1;
		}

		char full[PATH_MAX];
		if (path_join(full, sizeof(full), p->cwd, name) < 0)
			continue;

		struct stat st;
		memset(&st, 0, sizeof(st));
		(void)stat_path(full, &st);

		struct entry *e = &p->entries[p->count++];
		memset(e, 0, sizeof(*e));
		e->name = strdup(name);
		if (!e->name) {
			closedir(d);
			status_set("out of memory");
			return -1;
		}
		e->mode = st.st_mode;
		e->size = st.st_size;
		e->mtime = st.st_mtime;
		e->is_dir = S_ISDIR(st.st_mode);
	}

	closedir(d);
	panel_sort(p);
	if (p->selected >= p->count)
		p->selected = p->count ? p->count - 1 : 0;
	if (p->top > p->selected)
		p->top = p->selected;
	return 0;
}

static void addnstr_compat(const char *s, int n)
{
	if (!s || n <= 0)
		return;
	for (int i = 0; i < n && s[i]; i++)
		addch((unsigned char)s[i]);
}

static void draw_boxed(int y, int x, int h, int w, const char *title)
{
	mvaddch(y, x, ACS_ULCORNER);
	mvaddch(y, x + w - 1, ACS_URCORNER);
	mvaddch(y + h - 1, x, ACS_LLCORNER);
	mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
	for (int i = 1; i < w - 1; i++) {
		mvaddch(y, x + i, ACS_HLINE);
		mvaddch(y + h - 1, x + i, ACS_HLINE);
	}
	for (int i = 1; i < h - 1; i++) {
		mvaddch(y + i, x, ACS_VLINE);
		mvaddch(y + i, x + w - 1, ACS_VLINE);
	}
	if (title && *title) {
		int tl = (int)strlen(title);
		int tx = x + 2;
		if (tx + tl < x + w - 2)
			mvprintw(y, tx, "%s", title);
	}
}

static void format_size(char *out, size_t outsz, const struct entry *e)
{
	if (e->is_dir) {
		snprintf(out, outsz, " <DIR> ");
		return;
	}
	long long s = (long long)e->size;
	if (s < 1024)
		snprintf(out, outsz, "%6lldB", s);
	else if (s < 1024LL * 1024)
		snprintf(out, outsz, "%5lldK", (s + 1023) / 1024);
	else
		snprintf(out, outsz, "%5lldM", (s + (1024LL * 1024 - 1)) / (1024LL * 1024));
}

static void format_mtime(char *out, size_t outsz, const struct entry *e)
{
	time_t t = e->mtime;
	struct tm *tm = localtime(&t);
	if (!tm) {
		snprintf(out, outsz, "---- --:--");
		return;
	}
	snprintf(out, outsz, "%02d-%02d %02d:%02d",
		tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
}

static void draw_panel(const struct panel *p, int y, int x, int h, int w, bool active)
{
	char title[PATH_MAX + 32];
	snprintf(title, sizeof(title), "%s%s", active ? "* " : "  ", p->cwd);
	draw_boxed(y, x, h, w, title);

	int list_y = y + 1;
	int list_h = h - 2;
	int name_w = w - 2 - 7 - 1 - 11; /* borders + size + space + mtime */
	if (name_w < 8)
		name_w = 8;

	for (int i = 0; i < list_h; i++) {
		int idx = (int)p->top + i;
		move(list_y + i, x + 1);
		clrtoeol();
		if (idx >= (int)p->count)
			continue;

		const struct entry *e = &p->entries[idx];
		bool sel = (idx == (int)p->selected);

		char sz[16];
		char mt[16];
		format_size(sz, sizeof(sz), e);
		format_mtime(mt, sizeof(mt), e);

		if (sel) {
			attron(A_REVERSE);
		}

		char namebuf[PATH_MAX];
		snprintf(namebuf, sizeof(namebuf), "%s%s", e->name, e->is_dir ? "/" : "");

		if ((int)strlen(namebuf) > name_w) {
			namebuf[name_w - 1] = '~';
			namebuf[name_w] = 0;
		}

		mvprintw(list_y + i, x + 1, "%-*s %7s %11s", name_w, namebuf, sz, mt);

		if (sel) {
			attroff(A_REVERSE);
		}
	}
}

static int prompt_line(const char *title, char *buf, size_t bufsz)
{
	int rows, cols;
	getmaxyx(stdscr, rows, cols);

	int w = cols - 4;
	if (w > 70)
		w = 70;
	int h = 5;
	int x = (cols - w) / 2;
	int y = (rows - h) / 2;

	draw_boxed(y, x, h, w, title);
	mvprintw(y + 2, x + 2, "> ");
	move(y + 2, x + 4);
	refresh();

	curs_set(1);
	size_t len = strlen(buf);
	if (len >= bufsz)
		len = bufsz ? bufsz - 1 : 0;

	for (;;) {
		move(y + 2, x + 4);
		for (int i = 0; i < w - 6; i++)
			addch(' ');
		move(y + 2, x + 4);
		addnstr_compat(buf, (int)len);
		move(y + 2, x + 4 + (int)len);
		refresh();

		int ch = getch();
		if (ch == '\n' || ch == '\r')
			break;
		if (ch == KEY_ESC) {
			buf[0] = 0;
			len = 0;
			break;
		}
		if (ch == 8 || ch == 127) {
			if (len) {
				len--;
				buf[len] = 0;
			}
			continue;
		}
		if (isprint(ch) && len + 1 < bufsz) {
			buf[len++] = (char)ch;
			buf[len] = 0;
		}
	}
	curs_set(0);
	return 0;
}

static int confirm_box(const char *msg)
{
	int rows, cols;
	getmaxyx(stdscr, rows, cols);

	int w = cols - 4;
	if (w > 70)
		w = 70;
	int h = 5;
	int x = (cols - w) / 2;
	int y = (rows - h) / 2;

	draw_boxed(y, x, h, w, "Confirm");
	mvprintw(y + 2, x + 2, "%s (y/n)", msg);
	refresh();

	for (;;) {
		int ch = getch();
		if (ch == 'y' || ch == 'Y')
			return 1;
		if (ch == 'n' || ch == 'N' || ch == KEY_ESC)
			return 0;
	}
}

static int run_external(const char *path, char *const argv[])
{
	endwin();
	fflush(stdout);

	pid_t pid = fork();
	if (pid == 0) {
		execv(path, argv);
		_exit(127);
	}

	int st = 0;
	if (pid > 0)
		(void)waitpid(pid, &st, 0);

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	return st;
}

static bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static int view_file(const char *path)
{
	const char *pager = NULL;
	if (file_exists("/usr/bin/less"))
		pager = "/usr/bin/less";
	else if (file_exists("/bin/less"))
		pager = "/bin/less";
	else if (file_exists("/bin/more"))
		pager = "/bin/more";

	if (!pager) {
		status_set("no pager (less/more)");
		return -1;
	}

	char *argv[] = { (char *)pager, (char *)path, NULL };
	return run_external(pager, argv);
}

static int edit_file(const char *path)
{
	const char *editor = NULL;
	if (file_exists("/bin/vi"))
		editor = "/bin/vi";
	else if (file_exists("/usr/bin/vi"))
		editor = "/usr/bin/vi";

	if (!editor) {
		status_set("no editor (vi)");
		return -1;
	}

	char *argv[] = { (char *)editor, (char *)path, NULL };
	return run_external(editor, argv);
}

static int spawn_shell(const char *cwd)
{
	const char *sh = file_exists("/bin/sh") ? "/bin/sh" : "/usr/bin/sh";
	char *argv[] = { (char *)sh, NULL };

	char prev[PATH_MAX];
	if (getcwd(prev, sizeof(prev)) == NULL)
		prev[0] = 0;
	(void)chdir(cwd);
	int r = run_external(sh, argv);
	if (prev[0])
		(void)chdir(prev);
	return r;
}

static int copy_file_data(int srcfd, int dstfd)
{
	char buf[1024];
	for (;;) {
		ssize_t n = read(srcfd, buf, sizeof(buf));
		if (n == 0)
			return 0;
		if (n < 0)
			return -1;
		char *p = buf;
		ssize_t left = n;
		while (left) {
			ssize_t w = write(dstfd, p, left);
			if (w < 0)
				return -1;
			p += w;
			left -= w;
		}
	}
}

static int ensure_dir(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0)
		return 0;
	if (errno == EEXIST)
		return 0;
	return -1;
}

static int copy_tree(const char *src, const char *dst)
{
	struct stat st;
	if (lstat(src, &st) < 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		if (ensure_dir(dst, st.st_mode & 0777) < 0)
			return -1;

		DIR *d = opendir(src);
		if (!d)
			return -1;
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char s2[PATH_MAX], d2[PATH_MAX];
			if (path_join(s2, sizeof(s2), src, de->d_name) < 0)
				continue;
			if (path_join(d2, sizeof(d2), dst, de->d_name) < 0)
				continue;
			if (copy_tree(s2, d2) < 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		return 0;
	}

	if (S_ISREG(st.st_mode)) {
		int sfd = open(src, O_RDONLY);
		if (sfd < 0)
			return -1;
		int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
		if (dfd < 0) {
			close(sfd);
			return -1;
		}
		int r = copy_file_data(sfd, dfd);
		close(sfd);
		close(dfd);
		return r;
	}

	/* Fallback: try to create an empty file for unknown types. */
	int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (dfd >= 0) {
		close(dfd);
		return 0;
	}
	return -1;
}

static int rm_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (!d)
			return -1;
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char p2[PATH_MAX];
			if (path_join(p2, sizeof(p2), path, de->d_name) < 0)
				continue;
			if (rm_tree(p2) < 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		return rmdir(path);
	}

	return unlink(path);
}

static struct panel *cur_panel(void) { return &panels[active_panel]; }
static struct panel *other_panel(void) { return &panels[active_panel ^ 1]; }

static const struct entry *cur_entry(void)
{
	struct panel *p = cur_panel();
	if (!p->count || p->selected >= p->count)
		return NULL;
	return &p->entries[p->selected];
}

static int cur_entry_path(char *out, size_t outsz)
{
	const struct entry *e = cur_entry();
	if (!e)
		return -1;
	return path_join(out, outsz, cur_panel()->cwd, e->name);
}

static void clamp_visible(struct panel *p, int list_h)
{
	if (p->selected < p->top)
		p->top = p->selected;
	if (p->selected >= p->top + (size_t)list_h) {
		if (p->selected >= (size_t)list_h)
			p->top = p->selected - (size_t)list_h + 1;
		else
			p->top = 0;
	}
}

static void do_chdir_entry(void)
{
	struct panel *p = cur_panel();
	const struct entry *e = cur_entry();
	if (!e)
		return;

	if (!strcmp(e->name, ".")) {
		return;
	}

	char nwd[PATH_MAX];
	if (!strcmp(e->name, "..")) {
		snprintf(nwd, sizeof(nwd), "%s", p->cwd);
		char *slash = strrchr(nwd, '/');
		if (slash && slash != nwd)
			*slash = 0;
		else {
			nwd[0] = '/';
			nwd[1] = 0;
		}
	} else {
		if (!e->is_dir) {
			char full[PATH_MAX];
			if (cur_entry_path(full, sizeof(full)) == 0)
				(void)view_file(full);
			return;
		}
		if (path_join(nwd, sizeof(nwd), p->cwd, e->name) < 0)
			return;
	}
	snprintf(p->cwd, sizeof(p->cwd), "%s", nwd);
	(void)panel_load(p);
}

static void do_copy(void)
{
	char src[PATH_MAX];
	if (cur_entry_path(src, sizeof(src)) < 0)
		return;

	const char *base = path_basename(src);
	char defdst[PATH_MAX];
	if (path_join(defdst, sizeof(defdst), other_panel()->cwd, base) < 0)
		return;

	char dst[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s", defdst);

	if (prompt_line("Copy to", dst, sizeof(dst)) < 0)
		return;
	if (!dst[0])
		return;

	if (!confirm_box("Copy?"))
		return;

	if (copy_tree(src, dst) < 0) {
		status_set("copy failed: %s", strerror(errno));
	} else {
		status_set("copied");
		(void)panel_load(cur_panel());
		(void)panel_load(other_panel());
	}
}

static void do_move(void)
{
	char src[PATH_MAX];
	if (cur_entry_path(src, sizeof(src)) < 0)
		return;
	const char *base = path_basename(src);

	char defdst[PATH_MAX];
	if (path_join(defdst, sizeof(defdst), other_panel()->cwd, base) < 0)
		return;
	char dst[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s", defdst);

	if (prompt_line("Move/Rename to", dst, sizeof(dst)) < 0)
		return;
	if (!dst[0])
		return;

	if (!confirm_box("Move?"))
		return;

	if (rename(src, dst) < 0) {
		if (copy_tree(src, dst) == 0 && rm_tree(src) == 0) {
			status_set("moved");
		} else {
			status_set("move failed: %s", strerror(errno));
			(void)rm_tree(dst);
		}
	} else {
		status_set("moved");
	}

	(void)panel_load(cur_panel());
	(void)panel_load(other_panel());
}

static void do_mkdir(void)
{
	char name[PATH_MAX];
	name[0] = 0;
	if (prompt_line("mkdir (name)", name, sizeof(name)) < 0)
		return;
	if (!name[0])
		return;

	char path[PATH_MAX];
	if (path_join(path, sizeof(path), cur_panel()->cwd, name) < 0)
		return;
	if (mkdir(path, 0777) < 0) {
		status_set("mkdir failed: %s", strerror(errno));
		return;
	}
	status_set("created");
	(void)panel_load(cur_panel());
}

static void do_delete(void)
{
	char path[PATH_MAX];
	if (cur_entry_path(path, sizeof(path)) < 0)
		return;
	const struct entry *e = cur_entry();
	if (!e)
		return;
	if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
		return;

	char msg[96];
	snprintf(msg, sizeof(msg), "Delete %s?", e->name);
	if (!confirm_box(msg))
		return;

	if (rm_tree(path) < 0) {
		status_set("delete failed: %s", strerror(errno));
		return;
	}
	status_set("deleted");
	(void)panel_load(cur_panel());
}

static void draw_ui(void)
{
	int rows, cols;
	getmaxyx(stdscr, rows, cols);
	erase();

	int top_h = 1;
	int bot_h = 2;
	int panel_h = rows - top_h - bot_h;
	if (panel_h < 6) {
		mvprintw(0, 0, "Terminal too small");
		refresh();
		return;
	}

	int w = cols / 2;
	int left_x = 0;
	int right_x = w;
	int left_w = w;
	int right_w = cols - w;

	mvprintw(0, 0, "mc (FUZIX)  Tab switch  F10 quit  ':' shell");

	draw_panel(&panels[0], 1, left_x, panel_h, left_w, active_panel == 0);
	draw_panel(&panels[1], 1, right_x, panel_h, right_w, active_panel == 1);

	int status_y = 1 + panel_h;
	move(status_y, 0);
	for (int i = 0; i < cols; i++)
		addch(' ');
	mvprintw(status_y, 0, "F1 Help  F3 View  F4 Edit  F5 Copy  F6 Move  F7 Mkdir  F8 Del  F10 Quit");

	move(status_y + 1, 0);
	for (int i = 0; i < cols; i++)
		addch(' ');
	if (status_msg[0]) {
		mvprintw(status_y + 1, 0, "%s", status_msg);
	}

	refresh();
}

static int read_key(void)
{
	int ch = getch();
	if (ch != KEY_ESC)
		return ch;

	/* ESC sequence (VT100-ish). */
	nodelay(stdscr, TRUE);
	int c1 = getch();
	if (c1 == ERR) {
		nodelay(stdscr, FALSE);
		return K_ESC;
	}
	int c2 = getch();
	nodelay(stdscr, FALSE);

	/* F1..F4: ESC O P/Q/R/S */
	if (c1 == 'O') {
		switch (c2) {
		case 'P': return K_F1;
		case 'R': return K_F3;
		case 'S': return K_F4;
		default:  return K_ESC;
		}
	}

	/* CSI sequences: ESC [ ... */
	if (c1 == '[') {
		if (c2 == 'A') return K_UP;
		if (c2 == 'B') return K_DOWN;
		if (c2 == 'C') return K_RIGHT;
		if (c2 == 'D') return K_LEFT;
		if (c2 == 'H') return K_HOME;
		if (c2 == 'F') return K_END;

		/* ESC [ <n> ~ */
		if (c2 >= '0' && c2 <= '9') {
			int n = c2 - '0';
			for (;;) {
				nodelay(stdscr, TRUE);
				int cx = getch();
				nodelay(stdscr, FALSE);
				if (cx == ERR)
					break;
				if (cx >= '0' && cx <= '9') {
					n = n * 10 + (cx - '0');
					continue;
				}
				if (cx == '~') {
					switch (n) {
					case 1: return K_HOME;
					case 4: return K_END;
					case 5: return K_PGUP;
					case 6: return K_PGDN;
					case 11: return K_F1;
					case 13: return K_F3;
					case 14: return K_F4;
					case 15: return K_F5;
					case 17: return K_F6;
					case 18: return K_F7;
					case 19: return K_F8;
					case 21: return K_F10;
					default: return K_ESC;
					}
				}
				break;
			}
		}

		return K_ESC;
	}

	/* Alt-x fallback: ESC then char. */
	return (KEY_ESC << 8) | (c1 & 0xFF);
}

static void do_help(void)
{
	int rows, cols;
	getmaxyx(stdscr, rows, cols);
	(void)cols;
	erase();
	mvprintw(0, 0, "mc (FUZIX) - keys");
	mvprintw(2, 0, "Arrows/PgUp/PgDn/Home/End: move");
	mvprintw(3, 0, "Enter: open dir / view file");
	mvprintw(4, 0, "Backspace: parent dir");
	mvprintw(5, 0, "Tab: switch panel");
	mvprintw(6, 0, "F3: view (less)   F4: edit (vi)");
	mvprintw(7, 0, "F5: copy          F6: move/rename");
	mvprintw(8, 0, "F7: mkdir         F8: delete");
	mvprintw(9, 0, "':' : shell");
	mvprintw(10, 0, "F10: quit");
	mvprintw(rows - 1, 0, "Press any key...");
	refresh();
	(void)getch();
}

static void handle_key(int ch)
{
	struct panel *p = cur_panel();
	int rows, cols;
	getmaxyx(stdscr, rows, cols);
	(void)cols;
	int top_h = 1;
	int bot_h = 2;
	int panel_h = rows - top_h - bot_h;
	int list_h = panel_h - 2; /* inside the box */
	if (list_h < 1)
		list_h = 1;

	switch (ch) {
	case '\t':
		active_panel ^= 1;
		break;
	case K_UP:
		if (p->selected)
			p->selected--;
		break;
	case K_DOWN:
		if (p->selected + 1 < p->count)
			p->selected++;
		break;
	case K_PGDN:
		if (p->selected + (size_t)list_h < p->count)
			p->selected += (size_t)list_h;
		else if (p->count)
			p->selected = p->count - 1;
		break;
	case K_PGUP:
		if (p->selected >= (size_t)list_h)
			p->selected -= (size_t)list_h;
		else
			p->selected = 0;
		break;
	case K_HOME:
		p->selected = 0;
		break;
	case K_END:
		if (p->count)
			p->selected = p->count - 1;
		break;
	case 8:
	case 127:
		/* go parent */
		{
			char nwd[PATH_MAX];
			snprintf(nwd, sizeof(nwd), "%s", p->cwd);
			char *slash = strrchr(nwd, '/');
			if (slash && slash != nwd)
				*slash = 0;
			else {
				nwd[0] = '/';
				nwd[1] = 0;
			}
			snprintf(p->cwd, sizeof(p->cwd), "%s", nwd);
			(void)panel_load(p);
		}
		break;
	case '\n':
	case '\r':
		do_chdir_entry();
		break;
	case K_F1:
		do_help();
		break;
	case K_F3:
		{
			char fp[PATH_MAX];
			if (cur_entry_path(fp, sizeof(fp)) == 0)
				(void)view_file(fp);
		}
		break;
	case K_F4:
		{
			char fp[PATH_MAX];
			if (cur_entry_path(fp, sizeof(fp)) == 0)
				(void)edit_file(fp);
			(void)panel_load(cur_panel());
		}
		break;
	case K_F5:
		do_copy();
		break;
	case K_F6:
		do_move();
		break;
	case K_F7:
		do_mkdir();
		break;
	case K_F8:
		do_delete();
		break;
	case K_F10:
		raise(SIGTERM);
		break;
	case ':':
		(void)spawn_shell(cur_panel()->cwd);
		(void)panel_load(cur_panel());
		(void)panel_load(other_panel());
		break;
	default:
		/* Alt-0..9: map to F10..F1 style quick keys */
		if ((ch >> 8) == KEY_ESC) {
			int c = ch & 0xFF;
			if (c == '0') raise(SIGTERM);
			else if (c == '1') do_help();
			else if (c == '3') {
				char fp[PATH_MAX];
				if (cur_entry_path(fp, sizeof(fp)) == 0)
					(void)view_file(fp);
			} else if (c == '4') {
				char fp[PATH_MAX];
				if (cur_entry_path(fp, sizeof(fp)) == 0)
					(void)edit_file(fp);
			} else if (c == '5') do_copy();
			else if (c == '6') do_move();
			else if (c == '7') do_mkdir();
			else if (c == '8') do_delete();
		} else if (ch >= 32 && ch < 127) {
			/* quick search */
			char needle[2] = { (char)ch, 0 };
			for (size_t i = 0; i < p->count; i++) {
				if (!strncasecmp(p->entries[i].name, needle, 1)) {
					p->selected = i;
					break;
				}
			}
		}
		break;
	}

	clamp_visible(p, list_h);
}

static volatile sig_atomic_t want_exit;

static void on_term(int sig)
{
	(void)sig;
	want_exit = 1;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	for (int i = 0; i < 2; i++) {
		panels[i].entries = NULL;
		panels[i].count = 0;
		panels[i].cap = 0;
		panels[i].selected = 0;
		panels[i].top = 0;
		panels[i].show_hidden = FALSE;
		panels[i].sort = SORT_NAME;
	}

	if (getcwd(panels[0].cwd, sizeof(panels[0].cwd)) == NULL)
		strcpy(panels[0].cwd, "/");
	strcpy(panels[1].cwd, panels[0].cwd);

	(void)panel_load(&panels[0]);
	(void)panel_load(&panels[1]);

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);

	while (!want_exit) {
		draw_ui();
		int ch = read_key();
		handle_key(ch);
	}

	endwin();
	panel_clear(&panels[0]);
	panel_clear(&panels[1]);
	return 0;
}
