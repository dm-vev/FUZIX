#include "rf_presets.h"

#include "rf.h"
#include "rf_fs.h"
#include "rf_task.h"
#include "rf_types.h"
#include "rf_waterfall.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { RF_PRESET_MAX = 16 * 1024 };

static const char *const preset_dir = "/rf";
static const char *const presets_dir = "/rf/presets";
static const char *const preset_ext = ".cfg";

struct rf_cfg_snapshot {
	int channel_range_lo;
	int channel_range_hi;
	int dwell_time_ms;
	int scan_step;
	enum rf_data_rate data_rate;
	enum rf_crc_mode crc_mode;
	int auto_ack;
	enum rf_power_level power_level;
	enum rf_wf_palette wf_palette;
};

static int str_ieq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b) {
		int ca = tolower((unsigned char)*a++);
		int cb = tolower((unsigned char)*b++);
		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static char *trim_ws(char *s)
{
	if (!s)
		return s;
	while (*s && isspace((unsigned char)*s))
		s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = 0;
	return s;
}

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

static int ensure_presets_dir(char *err, size_t errsz)
{
	if (rf_fs_ensure_dir(preset_dir, 0755, err, errsz) != 0)
		return -1;
	if (rf_fs_ensure_dir(presets_dir, 0755, err, errsz) != 0)
		return -1;
	return 0;
}

static int build_preset_path(const char *name_in, char safe[32], char path[96], char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!safe || !path) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}
	safe[0] = 0;
	path[0] = 0;

	if (!name_in) {
		if (err && errsz)
			snprintf(err, errsz, "empty preset name");
		return -1;
	}

	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%s", name_in);
	char *name = trim_ws(tmp);
	if (!name[0]) {
		if (err && errsz)
			snprintf(err, errsz, "empty preset name");
		return -1;
	}
	if (strchr(name, '/')) {
		if (err && errsz)
			snprintf(err, errsz, "preset name may not contain '/'");
		return -1;
	}

	rf_sanitize_name(name, safe, 32);
	if (!safe[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid preset name");
		return -1;
	}
	if (ends_with(safe, preset_ext))
		safe[strlen(safe) - strlen(preset_ext)] = 0;
	if (!safe[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid preset name");
		return -1;
	}

	if (snprintf(path, 96, "%s/%s%s", presets_dir, safe, preset_ext) >= 96) {
		if (err && errsz)
			snprintf(err, errsz, "preset path too long");
		return -1;
	}
	return 0;
}

static struct rf_cfg_snapshot snapshot_config(const struct rf_task *t)
{
	struct rf_cfg_snapshot cfg;
	memset(&cfg, 0, sizeof(cfg));
	if (!t)
		return cfg;
	cfg.channel_range_lo = t->channel_range_lo;
	cfg.channel_range_hi = t->channel_range_hi;
	cfg.dwell_time_ms = t->dwell_time_ms;
	cfg.scan_step = rf_clamp_int(t->scan_speed_scalar, 1, 10);
	cfg.data_rate = t->data_rate;
	cfg.crc_mode = t->crc_mode;
	cfg.auto_ack = t->auto_ack;
	cfg.power_level = t->power_level;
	cfg.wf_palette = t->wf_palette;
	return cfg;
}

static void apply_config(struct rf_task *t, struct rf_cfg_snapshot cfg)
{
	if (!t)
		return;

	t->channel_range_lo = rf_clamp_int(cfg.channel_range_lo, 0, RF_MAX_CHANNEL);
	t->channel_range_hi = rf_clamp_int(cfg.channel_range_hi, 0, RF_MAX_CHANNEL);
	if (t->channel_range_lo > t->channel_range_hi) {
		int tmp = t->channel_range_lo;
		t->channel_range_lo = t->channel_range_hi;
		t->channel_range_hi = tmp;
	}
	t->dwell_time_ms = rf_clamp_int(cfg.dwell_time_ms, 1, 50);
	t->scan_speed_scalar = rf_clamp_int(cfg.scan_step, 1, 10);
	t->data_rate = cfg.data_rate;
	t->crc_mode = cfg.crc_mode;
	t->auto_ack = cfg.auto_ack ? 1 : 0;
	t->power_level = cfg.power_level;
	t->wf_palette = cfg.wf_palette;
	rf_waterfall_rebuild_palette(t);
	t->scan_next_tick = 0;
}

static int parse_int_def(const char *s, int def)
{
	if (!s)
		return def;
	s = trim_ws((char *)s);
	if (!s[0])
		return def;

	int sign = 1;
	if (s[0] == '-') {
		sign = -1;
		s++;
	}
	long n = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return def;
		n = n * 10 + (*p - '0');
		if (n > 0x7fffffffL)
			n = 0x7fffffffL;
	}
	return (int)(n * sign);
}

static int parse_rate(const char *s, enum rf_data_rate *out)
{
	if (!s || !out)
		return 0;
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%s", s);
	char *v = trim_ws(tmp);
	for (char *p = v; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	if (!strcmp(v, "250K")) {
		*out = RF_RATE_250K;
		return 1;
	}
	if (!strcmp(v, "1M")) {
		*out = RF_RATE_1M;
		return 1;
	}
	if (!strcmp(v, "2M")) {
		*out = RF_RATE_2M;
		return 1;
	}
	return 0;
}

static int parse_crc(const char *s, enum rf_crc_mode *out)
{
	if (!s || !out)
		return 0;
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%s", s);
	char *v = trim_ws(tmp);
	for (char *p = v; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	if (!strcmp(v, "OFF")) {
		*out = RF_CRC_OFF;
		return 1;
	}
	if (!strcmp(v, "1B")) {
		*out = RF_CRC_1B;
		return 1;
	}
	if (!strcmp(v, "2B")) {
		*out = RF_CRC_2B;
		return 1;
	}
	return 0;
}

static int parse_power(const char *s, enum rf_power_level *out)
{
	if (!s || !out)
		return 0;
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%s", s);
	char *v = trim_ws(tmp);
	for (char *p = v; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	if (!strcmp(v, "MIN")) {
		*out = RF_PWR_MIN;
		return 1;
	}
	if (!strcmp(v, "LOW")) {
		*out = RF_PWR_LOW;
		return 1;
	}
	if (!strcmp(v, "HIGH")) {
		*out = RF_PWR_HIGH;
		return 1;
	}
	if (!strcmp(v, "MAX")) {
		*out = RF_PWR_MAX;
		return 1;
	}
	return 0;
}

static int parse_palette(const char *s, enum rf_wf_palette *out)
{
	if (!s || !out)
		return 0;
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%s", s);
	char *v = trim_ws(tmp);
	for (char *p = v; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	if (!strcmp(v, "CYAN")) {
		*out = RF_WF_PAL_CYAN;
		return 1;
	}
	if (!strcmp(v, "FIRE")) {
		*out = RF_WF_PAL_FIRE;
		return 1;
	}
	if (!strcmp(v, "GRAY")) {
		*out = RF_WF_PAL_GRAY;
		return 1;
	}
	if (!strcmp(v, "CUBIC") || !strcmp(v, "SDR")) {
		*out = RF_WF_PAL_CUBIC;
		return 1;
	}
	return 0;
}

static void apply_kv(struct rf_cfg_snapshot *cfg, const char *key_in, const char *val_in)
{
	if (!cfg || !key_in || !val_in)
		return;

	char keybuf[32];
	char valbuf[32];
	snprintf(keybuf, sizeof(keybuf), "%s", key_in);
	snprintf(valbuf, sizeof(valbuf), "%s", val_in);
	char *key = trim_ws(keybuf);
	char *val = trim_ws(valbuf);

	if (!strcmp(key, "range_lo"))
		cfg->channel_range_lo = parse_int_def(val, cfg->channel_range_lo);
	else if (!strcmp(key, "range_hi"))
		cfg->channel_range_hi = parse_int_def(val, cfg->channel_range_hi);
	else if (!strcmp(key, "dwell_ms"))
		cfg->dwell_time_ms = parse_int_def(val, cfg->dwell_time_ms);
	else if (!strcmp(key, "scan_step"))
		cfg->scan_step = parse_int_def(val, cfg->scan_step);
	else if (!strcmp(key, "rate"))
		(void)parse_rate(val, &cfg->data_rate);
	else if (!strcmp(key, "crc"))
		(void)parse_crc(val, &cfg->crc_mode);
	else if (!strcmp(key, "auto_ack"))
		cfg->auto_ack = (val[0] == '1') || str_ieq(val, "true") || str_ieq(val, "on");
	else if (!strcmp(key, "pwr"))
		(void)parse_power(val, &cfg->power_level);
	else if (!strcmp(key, "wf_palette"))
		(void)parse_palette(val, &cfg->wf_palette);
}

int rf_presets_save(struct rf_task *t, const char *name, char *err, size_t errsz)
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
	char path[96];
	if (build_preset_path(name, safe, path, err, errsz) != 0)
		return -1;

	char data[512];
	int n = 0;
	n += snprintf(data + n, sizeof(data) - (size_t)n, "# rfanalyzer preset v1\n");
	n += snprintf(data + n, sizeof(data) - (size_t)n, "range_lo=%d\n", t->channel_range_lo);
	n += snprintf(data + n, sizeof(data) - (size_t)n, "range_hi=%d\n", t->channel_range_hi);
	n += snprintf(data + n, sizeof(data) - (size_t)n, "dwell_ms=%d\n", t->dwell_time_ms);
	n += snprintf(data + n, sizeof(data) - (size_t)n, "scan_step=%d\n", rf_clamp_int(t->scan_speed_scalar, 1, 10));
	n += snprintf(data + n, sizeof(data) - (size_t)n, "rate=%s\n", rf_data_rate_str(t->data_rate));
	n += snprintf(data + n, sizeof(data) - (size_t)n, "crc=%s\n", rf_crc_mode_str(t->crc_mode));
	n += snprintf(data + n, sizeof(data) - (size_t)n, "auto_ack=%d\n", t->auto_ack ? 1 : 0);
	n += snprintf(data + n, sizeof(data) - (size_t)n, "pwr=%s\n", rf_power_level_str(t->power_level));
	n += snprintf(data + n, sizeof(data) - (size_t)n, "wf_palette=%s\n", rf_wf_palette_str(t->wf_palette));

	if (n < 0 || (size_t)n >= sizeof(data)) {
		if (err && errsz)
			snprintf(err, errsz, "preset too large");
		return -1;
	}

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return -1;
	}
	int rc = rf_fs_write_all(fd, data, (size_t)n, err, errsz);
	(void)close(fd);
	if (rc != 0)
		return -1;

	snprintf(t->active_preset, sizeof(t->active_preset), "%s", safe);
	t->preset_dirty = 0;
	return 0;
}

int rf_presets_load(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad task");
		return -1;
	}

	char safe[32];
	char path[96];
	if (build_preset_path(name, safe, path, err, errsz) != 0)
		return -1;

	struct stat st;
	if (stat(path, &st) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "stat %s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		if (err && errsz)
			snprintf(err, errsz, "%s: not a file", path);
		return -1;
	}
	if (st.st_size < 0 || st.st_size > RF_PRESET_MAX) {
		if (err && errsz)
			snprintf(err, errsz, "preset too large");
		return -1;
	}

	size_t max = (size_t)st.st_size;
	if (max == 0)
		max = 1;
	char *buf = (char *)malloc(max + 1);
	if (!buf) {
		if (err && errsz)
			snprintf(err, errsz, "out of memory");
		return -1;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		free(buf);
		return -1;
	}

	size_t len = 0;
	while (len < (size_t)RF_PRESET_MAX) {
		ssize_t n = read(fd, buf + len, RF_PRESET_MAX - len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (err && errsz)
				snprintf(err, errsz, "read %s: %s", path, strerror(errno));
			(void)close(fd);
			free(buf);
			return -1;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	(void)close(fd);
	if (len >= (size_t)RF_PRESET_MAX) {
		if (err && errsz)
			snprintf(err, errsz, "preset too large");
		free(buf);
		return -1;
	}
	buf[len] = 0;

	struct rf_cfg_snapshot cfg = snapshot_config(t);

	char *p = buf;
	while (*p) {
		char *line = p;
		char *nl = strchr(p, '\n');
		if (nl) {
			*nl = 0;
			p = nl + 1;
		} else {
			p = line + strlen(line);
		}

		line = trim_ws(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *key = trim_ws(line);
		char *val = trim_ws(eq + 1);
		if (!key[0])
			continue;
		apply_kv(&cfg, key, val);
	}

	free(buf);

	apply_config(t, cfg);
	snprintf(t->active_preset, sizeof(t->active_preset), "%s", safe);
	t->preset_dirty = 0;
	rf_task_invalidate(t, RF_DIRTY_ALL);
	return 0;
}
