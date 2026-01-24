#include "rf_prompt.h"

#include "rf_keys.h"
#include "rf_presets.h"
#include "rf_recording.h"
#include "rf_task.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int is_numeric_prompt(enum rf_prompt_kind k)
{
	switch (k) {
	case RF_PROMPT_SET_CHANNEL:
	case RF_PROMPT_SET_RANGE_LO:
	case RF_PROMPT_SET_RANGE_HI:
	case RF_PROMPT_SET_DWELL:
	case RF_PROMPT_SET_SCAN_STEP:
	case RF_PROMPT_SET_FILTER_AGE:
	case RF_PROMPT_SET_FILTER_BURST:
	case RF_PROMPT_REPLAY_SEEK:
	case RF_PROMPT_AUTO_START_DELAY:
	case RF_PROMPT_AUTO_DURATION:
	case RF_PROMPT_AUTO_STOP_SWEEPS:
	case RF_PROMPT_AUTO_STOP_PACKETS:
	case RF_PROMPT_STRESS_PPS:
	case RF_PROMPT_STRESS_DURATION:
	case RF_PROMPT_ANNOT_DURATION:
		return 1;
	default:
		return 0;
	}
}

static void invalidate_overlay(struct rf_task *t)
{
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_STATUS);
}

void rf_prompt_open(struct rf_task *t, enum rf_prompt_kind kind, const char *title, const char *initial)
{
	if (!t)
		return;
	t->show_prompt = 1;
	t->prompt_kind = kind;
	snprintf(t->prompt_title, sizeof(t->prompt_title), "%s", title ? title : "Input");
	t->prompt_err[0] = 0;

	t->prompt_len = 0;
	t->prompt_cursor = 0;
	memset(t->prompt_buf, 0, sizeof(t->prompt_buf));

	if (!initial)
		initial = "";
	for (const unsigned char *p = (const unsigned char *)initial; *p && t->prompt_len < (int)(sizeof(t->prompt_buf) / sizeof(t->prompt_buf[0])); p++) {
		t->prompt_buf[t->prompt_len++] = (uint32_t)*p;
	}
	t->prompt_cursor = t->prompt_len;

	invalidate_overlay(t);
}

void rf_prompt_close(struct rf_task *t)
{
	if (!t || !t->show_prompt)
		return;
	t->show_prompt = 0;
	t->prompt_err[0] = 0;
	t->prompt_title[0] = 0;
	t->prompt_len = 0;
	t->prompt_cursor = 0;
	memset(t->prompt_buf, 0, sizeof(t->prompt_buf));
	invalidate_overlay(t);
}

static void prompt_to_cstr(const struct rf_task *t, char *out, unsigned outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t)
		return;
	unsigned n = 0;
	for (int i = 0; i < t->prompt_len && n + 1 < outsz; i++) {
		uint32_t r = t->prompt_buf[i];
		if (r < 0x20 || r > 0x7e)
			r = '?';
		out[n++] = (char)r;
	}
	out[n] = 0;
}

static void trim_inplace(char *s)
{
	if (!s)
		return;
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) {
		s[n - 1] = 0;
		n--;
	}
}

static int parse_int_strict(const char *s, int *out)
{
	if (!out)
		return 0;
	*out = 0;
	if (!s)
		return 0;
	if (*s == 0)
		return 0;

	int sign = 1;
	size_t i = 0;
	if (s[0] == '-') {
		sign = -1;
		i = 1;
		if (s[1] == 0)
			return 0;
	}
	int n = 0;
	for (; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		n = n * 10 + (int)(s[i] - '0');
	}
	*out = sign * n;
	return 1;
}

static int parse_hex_nibble(char c, uint8_t *out)
{
	if (!out)
		return 0;
	if (c >= '0' && c <= '9') {
		*out = (uint8_t)(c - '0');
		return 1;
	}
	if (c >= 'a' && c <= 'f') {
		*out = (uint8_t)(10 + (c - 'a'));
		return 1;
	}
	if (c >= 'A' && c <= 'F') {
		*out = (uint8_t)(10 + (c - 'A'));
		return 1;
	}
	return 0;
}

static int parse_hex_mask_pattern(const char *s, uint8_t *val, uint8_t *mask, int max_bytes, int *out_len,
				  char *err, unsigned errsz)
{
	if (out_len)
		*out_len = 0;
	if (!s || !val || !mask || max_bytes <= 0) {
		snprintf(err, errsz, "bad args");
		return 0;
	}
	size_t n = strlen(s);
	if (n == 0) {
		if (out_len)
			*out_len = 0;
		return 1;
	}
	if ((n & 1) != 0) {
		snprintf(err, errsz, "odd hex length");
		return 0;
	}
	int bytes = (int)(n / 2);
	if (bytes > max_bytes) {
		snprintf(err, errsz, "too long (max %d bytes)", max_bytes);
		return 0;
	}
	for (int i = 0; i < bytes; i++) {
		char hi = s[i * 2];
		char lo = s[i * 2 + 1];
		uint8_t v = 0;
		uint8_t m = 0;
		uint8_t nh = 0;
		uint8_t nl = 0;
		if (hi != '?') {
			if (!parse_hex_nibble(hi, &nh)) {
				snprintf(err, errsz, "bad hex");
				return 0;
			}
			v |= (uint8_t)(nh << 4);
			m |= 0xF0;
		}
		if (lo != '?') {
			if (!parse_hex_nibble(lo, &nl)) {
				snprintf(err, errsz, "bad hex");
				return 0;
			}
			v |= nl;
			m |= 0x0F;
		}
		val[i] = v;
		mask[i] = m;
	}
	if (out_len)
		*out_len = bytes;
	return 1;
}

static void insert_prompt_rune(struct rf_task *t, uint32_t r)
{
	if (!t || r == 0)
		return;
	if (r == '\n' || r == '\r' || r == '\t')
		return;

	if (is_numeric_prompt(t->prompt_kind)) {
		if (r < '0' || r > '9')
			return;
	} else {
		if (r < 0x20 || r > 0x7e)
			return;
	}

	if (t->prompt_len >= (int)(sizeof(t->prompt_buf) / sizeof(t->prompt_buf[0])))
		return;
	if (t->prompt_cursor < 0)
		t->prompt_cursor = 0;
	if (t->prompt_cursor > t->prompt_len)
		t->prompt_cursor = t->prompt_len;

	for (int i = t->prompt_len; i > t->prompt_cursor; i--)
		t->prompt_buf[i] = t->prompt_buf[i - 1];
	t->prompt_buf[t->prompt_cursor] = r;
	t->prompt_len++;
	t->prompt_cursor++;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY);
}

static void submit_prompt(struct rf_task *t)
{
	if (!t)
		return;

	char s[64];
	prompt_to_cstr(t, s, sizeof(s));
	trim_inplace(s);

	switch (t->prompt_kind) {
	case RF_PROMPT_SET_CHANNEL: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "channel: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		if (n < 0 || n > RF_MAX_CHANNEL) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "channel: must be 0..%d", RF_MAX_CHANNEL);
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->selected_channel = n;
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_RANGE_LO: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "range lo: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->channel_range_lo = rf_clamp_int(n, 0, RF_MAX_CHANNEL);
		if (t->channel_range_lo > t->channel_range_hi)
			t->channel_range_hi = t->channel_range_lo;
		t->preset_dirty = 1;
		t->scan_next_tick = 0;
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_RANGE_HI: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "range hi: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->channel_range_hi = rf_clamp_int(n, 0, RF_MAX_CHANNEL);
		if (t->channel_range_hi < t->channel_range_lo)
			t->channel_range_lo = t->channel_range_hi;
		t->preset_dirty = 1;
		t->scan_next_tick = 0;
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_SPECTRUM | RF_DIRTY_WATERFALL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_DWELL: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "dwell: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->dwell_time_ms = rf_clamp_int(n, 1, 50);
		t->preset_dirty = 1;
		t->scan_next_tick = 0;
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_SPECTRUM | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_SCAN_STEP: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "scan step: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->scan_speed_scalar = rf_clamp_int(n, 1, 10);
		t->preset_dirty = 1;
		t->scan_next_tick = 0;
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_SPECTRUM | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SAVE_PRESET: {
		char perr[96];
		if (rf_presets_save(t, s, perr, sizeof(perr)) != 0) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "save: %s", perr[0] ? perr : "failed");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_LOAD_PRESET: {
		char perr[96];
		if (rf_presets_load(t, s, perr, sizeof(perr)) != 0) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "load: %s", perr[0] ? perr : "failed");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_ALL);
		return;
	}
	case RF_PROMPT_START_RECORDING: {
		char rerr[96];
		if (rf_recording_start(t, s, rerr, sizeof(rerr)) != 0) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "rec: %s", rerr[0] ? rerr : "failed");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		rf_prompt_close(t);
		rf_task_invalidate(t, RF_DIRTY_RFCONTROL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_FILTER_ADDR: {
		/* Remove spaces */
		char tmp[64];
		unsigned w = 0;
		for (unsigned i = 0; s[i] && w + 1 < sizeof(tmp); i++) {
			if (s[i] != ' ')
				tmp[w++] = s[i];
		}
		tmp[w] = 0;
		if (tmp[0] == 0) {
			t->filter_addr_len = 0;
			rf_prompt_close(t);
			rf_sniffer_reconcile_selection(t);
			rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
			return;
		}
		uint8_t val[5];
		uint8_t mask[5];
		int n = 0;
		char perr[64];
		if (!parse_hex_mask_pattern(tmp, val, mask, 5, &n, perr, sizeof(perr))) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "addr: %s", perr);
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->filter_addr_len = n;
		for (int i = 0; i < n; i++) {
			t->filter_addr[i] = val[i];
			t->filter_addr_mask[i] = mask[i];
		}
		for (int i = n; i < 5; i++) {
			t->filter_addr[i] = 0;
			t->filter_addr_mask[i] = 0;
		}
		rf_prompt_close(t);
		rf_sniffer_reconcile_selection(t);
		rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_FILTER_PAYLOAD: {
		char tmp[64];
		unsigned w = 0;
		for (unsigned i = 0; s[i] && w + 1 < sizeof(tmp); i++) {
			if (s[i] != ' ')
				tmp[w++] = s[i];
		}
		tmp[w] = 0;
		if (tmp[0] == 0) {
			t->filter_payload_len = 0;
			memset(t->filter_payload, 0, sizeof(t->filter_payload));
			memset(t->filter_payload_mask, 0, sizeof(t->filter_payload_mask));
			rf_prompt_close(t);
			rf_sniffer_reconcile_selection(t);
			rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
			return;
		}
		uint8_t val[RF_PAYLOAD_PREFIX_BYTES];
		uint8_t mask[RF_PAYLOAD_PREFIX_BYTES];
		int n = 0;
		char perr[64];
		if (!parse_hex_mask_pattern(tmp, val, mask, RF_PAYLOAD_PREFIX_BYTES, &n, perr, sizeof(perr))) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "payload: %s", perr);
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->filter_payload_len = n;
		for (int i = 0; i < n; i++) {
			t->filter_payload[i] = val[i];
			t->filter_payload_mask[i] = mask[i];
		}
		for (int i = n; i < RF_PAYLOAD_PREFIX_BYTES; i++) {
			t->filter_payload[i] = 0;
			t->filter_payload_mask[i] = 0;
		}
		rf_prompt_close(t);
		rf_sniffer_reconcile_selection(t);
		rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_FILTER_AGE: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "age: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->filter_age_ms = rf_clamp_int(n, 0, 1000000);
		rf_prompt_close(t);
		rf_sniffer_reconcile_selection(t);
		rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
		return;
	}
	case RF_PROMPT_SET_FILTER_BURST: {
		int n = 0;
		if (!parse_int_strict(s, &n)) {
			snprintf(t->prompt_err, sizeof(t->prompt_err), "burst: invalid");
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
			return;
		}
		t->filter_burst_max_ms = rf_clamp_int(n, 0, 1000000);
		rf_prompt_close(t);
		rf_sniffer_reconcile_selection(t);
		rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
		return;
	}
	default:
		snprintf(t->prompt_err, sizeof(t->prompt_err), "not implemented");
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	}
}

void rf_prompt_handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	switch (k->kind) {
	case RF_KEY_ESC:
		rf_prompt_close(t);
		return;
	case RF_KEY_ENTER:
		submit_prompt(t);
		return;
	case RF_KEY_LEFT:
		if (t->prompt_cursor > 0) {
			t->prompt_cursor--;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		}
		return;
	case RF_KEY_RIGHT:
		if (t->prompt_cursor < t->prompt_len) {
			t->prompt_cursor++;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		}
		return;
	case RF_KEY_BACKSPACE:
		if (t->prompt_cursor > 0 && t->prompt_len > 0) {
			for (int i = t->prompt_cursor - 1; i + 1 < t->prompt_len; i++)
				t->prompt_buf[i] = t->prompt_buf[i + 1];
			t->prompt_len--;
			t->prompt_cursor--;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		}
		return;
	case RF_KEY_DELETE:
		if (t->prompt_cursor >= 0 && t->prompt_cursor < t->prompt_len) {
			for (int i = t->prompt_cursor; i + 1 < t->prompt_len; i++)
				t->prompt_buf[i] = t->prompt_buf[i + 1];
			t->prompt_len--;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		}
		return;
	case RF_KEY_RUNE:
		insert_prompt_rune(t, k->r);
		return;
	default:
		return;
	}
}
