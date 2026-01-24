#include "rf_filters.h"

#include "rf_keys.h"
#include "rf_prompt.h"
#include "rf_sniffer.h"
#include "rf_task.h"

#include <stdio.h>
#include <string.h>

enum { RF_FILTER_LINES = 8 };

static void hex_mask_byte(uint8_t v, uint8_t mask, char out[3])
{
	char hi = '?';
	char lo = '?';
	if ((mask & 0xF0) != 0) {
		uint8_t n = (v >> 4) & 0x0F;
		hi = (char)((n < 10) ? ('0' + n) : ('A' + (n - 10)));
	}
	if ((mask & 0x0F) != 0) {
		uint8_t n = v & 0x0F;
		lo = (char)((n < 10) ? ('0' + n) : ('A' + (n - 10)));
	}
	out[0] = hi;
	out[1] = lo;
	out[2] = 0;
}

static void build_hex_mask_string(char *out, unsigned outsz, const uint8_t *val, const uint8_t *mask, int len)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!val || !mask || len <= 0)
		return;

	unsigned n = 0;
	for (int i = 0; i < len && n + 2 < outsz; i++) {
		char b[3];
		hex_mask_byte(val[i], mask[i], b);
		if (n + 2 >= outsz)
			break;
		out[n++] = b[0];
		out[n++] = b[1];
	}
	if (n >= outsz)
		n = outsz - 1;
	out[n] = 0;
}

static void adjust_filter(struct rf_task *t, int delta)
{
	if (!t || delta == 0)
		return;

	int changed = 0;
	switch (t->filter_sel) {
	case 0: /* CRC */
		t->filter_crc = (enum rf_filter_crc)rf_wrap_enum((int)t->filter_crc + delta, 3);
		changed = 1;
		break;
	case 1: /* CH mode */
		t->filter_channel = (enum rf_filter_channel)rf_wrap_enum((int)t->filter_channel + delta, 3);
		changed = 1;
		break;
	case 2: /* MINLEN */
		t->filter_min_len = rf_clamp_int(t->filter_min_len + delta, 0, 32);
		if (t->filter_max_len > 0 && t->filter_min_len > t->filter_max_len)
			t->filter_max_len = t->filter_min_len;
		changed = 1;
		break;
	case 3: /* MAXLEN */
		t->filter_max_len = rf_clamp_int(t->filter_max_len + delta, 0, 32);
		if (t->filter_max_len > 0 && t->filter_max_len < t->filter_min_len)
			t->filter_min_len = t->filter_max_len;
		changed = 1;
		break;
	case 6: /* AGEms */
		t->filter_age_ms = rf_clamp_int(t->filter_age_ms + delta * 100, 0, 1000000);
		changed = 1;
		break;
	case 7: /* BURSTΔ */
		t->filter_burst_max_ms = rf_clamp_int(t->filter_burst_max_ms + delta, 0, 1000000);
		changed = 1;
		break;
	default:
		break;
	}

	if (!changed)
		return;

	rf_sniffer_reconcile_selection(t);
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
}

void rf_filters_toggle(struct rf_task *t)
{
	if (!t)
		return;
	t->show_filters = !t->show_filters;
	if (t->show_filters)
		t->filter_sel = 0;
	rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_SNIFFER | RF_DIRTY_STATUS);
}

void rf_filters_handle_key(struct rf_task *t, const struct rf_key *k)
{
	if (!t || !k)
		return;

	switch (k->kind) {
	case RF_KEY_ESC:
		t->show_filters = 0;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_SNIFFER | RF_DIRTY_STATUS);
		return;
	case RF_KEY_RUNE:
		if (k->r == 'f' || k->r == 'F') {
			t->show_filters = 0;
			rf_task_invalidate(t, RF_DIRTY_OVERLAY | RF_DIRTY_SNIFFER | RF_DIRTY_STATUS);
			return;
		}
		return;
	case RF_KEY_UP:
		t->filter_sel--;
		if (t->filter_sel < 0)
			t->filter_sel = RF_FILTER_LINES - 1;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	case RF_KEY_DOWN:
		t->filter_sel++;
		if (t->filter_sel >= RF_FILTER_LINES)
			t->filter_sel = 0;
		rf_task_invalidate(t, RF_DIRTY_OVERLAY);
		return;
	case RF_KEY_LEFT:
		adjust_filter(t, -1);
		return;
	case RF_KEY_RIGHT:
		adjust_filter(t, +1);
		return;
	case RF_KEY_ENTER:
		break;
	default:
		return;
	}

	switch (t->filter_sel) {
	case 4: {
		char initial[16];
		build_hex_mask_string(initial, sizeof(initial), t->filter_addr, t->filter_addr_mask, t->filter_addr_len);
		rf_prompt_open(t, RF_PROMPT_SET_FILTER_ADDR, "Filter address mask (hex, use ??, empty clears)", initial);
		t->show_filters = 0;
		return;
	}
	case 5: {
		char initial[32];
		build_hex_mask_string(initial, sizeof(initial), t->filter_payload, t->filter_payload_mask, t->filter_payload_len);
		rf_prompt_open(t, RF_PROMPT_SET_FILTER_PAYLOAD, "Filter payload prefix (hex, use ??, empty clears)", initial);
		t->show_filters = 0;
		return;
	}
	case 6: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->filter_age_ms);
		rf_prompt_open(t, RF_PROMPT_SET_FILTER_AGE, "Filter age window (ms, 0 disables)", initial);
		t->show_filters = 0;
		return;
	}
	case 7: {
		char initial[16];
		snprintf(initial, sizeof(initial), "%d", t->filter_burst_max_ms);
		rf_prompt_open(t, RF_PROMPT_SET_FILTER_BURST, "Filter burst Δt max (ms, 0 disables)", initial);
		t->show_filters = 0;
		return;
	}
	default:
		return;
	}
}

