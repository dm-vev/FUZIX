#include "rf_sniffer.h"

#include "rf_hash.h"
#include "rf_layout.h"
#include "rf_recording.h"
#include "rf_session.h"
#include "rf_task.h"

#include <stdio.h>
#include <string.h>

char rf_rate_short(enum rf_data_rate r)
{
	switch (r) {
	case RF_RATE_2M:
		return '2';
	case RF_RATE_1M:
		return '1';
	case RF_RATE_250K:
		return '0';
	default:
		return '?';
	}
}

void rf_addr_suffix3(uint8_t addr_len, const uint8_t addr[5], char out[7])
{
	if (!out)
		return;
	if (addr_len == 0) {
		snprintf(out, 7, "------");
		return;
	}
	int start = (int)addr_len - 3;
	if (start < 0)
		start = 0;
	while (start + 3 > (int)addr_len) {
		start--;
		if (start < 0) {
			start = 0;
			break;
		}
	}
	uint8_t b0 = 0;
	uint8_t b1 = 0;
	uint8_t b2 = 0;
	if (start < (int)addr_len)
		b0 = addr[start];
	if (start + 1 < (int)addr_len)
		b1 = addr[start + 1];
	if (start + 2 < (int)addr_len)
		b2 = addr[start + 2];
	snprintf(out, 7, "%02X%02X%02X", b0, b1, b2);
}

void rf_crc_text(uint8_t crc_len, uint8_t crc_ok, char out[3])
{
	if (!out)
		return;
	if (crc_len == 0) {
		snprintf(out, 3, "--");
		return;
	}
	snprintf(out, 3, "%s", crc_ok ? "OK" : "!!");
}

static struct rf_packet_summary summary_from_packet(const struct rf_packet *p)
{
	struct rf_packet_summary s;
	memset(&s, 0, sizeof(s));
	if (!p)
		return s;

	s.seq = p->seq;
	s.tick = p->tick;
	s.delta_ms = p->delta_ms;
	s.flags = p->flags;
	s.channel = p->channel;
	s.rate = p->rate;
	s.addr_len = p->addr_len;
	memcpy(s.addr, p->addr, sizeof(s.addr));
	s.length = p->length;
	s.payload_hash = p->payload_hash;
	int n = (int)p->length;
	if (n > RF_PAYLOAD_PREFIX_BYTES)
		n = RF_PAYLOAD_PREFIX_BYTES;
	memcpy(s.payload_prefix, p->payload, (size_t)n);
	s.crc_len = p->crc_len;
	s.crc_ok = p->crc_ok;
	return s;
}

static struct rf_packet_summary summary_from_meta(const struct rf_session_packet_meta *m)
{
	struct rf_packet_summary s;
	memset(&s, 0, sizeof(s));
	if (!m)
		return s;

	s.seq = m->seq;
	s.tick = m->tick;
	s.delta_ms = m->delta_ms;
	s.flags = m->flags;
	s.channel = m->channel;
	s.rate = m->rate;
	s.addr_len = m->addr_len;
	memcpy(s.addr, m->addr, sizeof(s.addr));
	s.length = m->length;
	s.payload_hash = m->payload_hash;
	memcpy(s.payload_prefix, m->payload_prefix, sizeof(s.payload_prefix));
	s.crc_len = m->crc_len;
	s.crc_ok = m->crc_ok;
	return s;
}

static const struct rf_packet *packet_by_display_index(const struct rf_task *t, int i)
{
	if (!t || i < 0 || i >= t->pkt_count)
		return NULL;
	int idx = t->pkt_head - 1 - i;
	while (idx < 0)
		idx += RF_MAX_PACKETS;
	if (idx >= RF_MAX_PACKETS)
		idx %= RF_MAX_PACKETS;
	return &t->packets[idx];
}

static int replay_limit(const struct rf_task *t)
{
	if (!t || !t->replay_active || !t->replay)
		return 0;
	int limit = t->replay_pkt_limit;
	if (limit < 0)
		limit = 0;
	if ((size_t)limit > t->replay->packet_count)
		limit = (int)t->replay->packet_count;
	return limit;
}

static int packet_visible_count(const struct rf_task *t)
{
	if (!t)
		return 0;
	if (t->replay_active && t->replay)
		return replay_limit(t);
	return t->pkt_count;
}

static struct rf_packet_summary packet_summary_by_display_index(const struct rf_task *t, int i, int *ok)
{
	if (ok)
		*ok = 0;
	if (!t || i < 0)
		return (struct rf_packet_summary){0};

	if (t->replay_active && t->replay) {
		int limit = replay_limit(t);
		if (i >= limit)
			return (struct rf_packet_summary){0};
		int meta_idx = limit - 1 - i;
		if (meta_idx < 0 || (size_t)meta_idx >= t->replay->packet_count)
			return (struct rf_packet_summary){0};
		if (ok)
			*ok = 1;
		return summary_from_meta(&t->replay->packets[meta_idx]);
	}

	const struct rf_packet *p = packet_by_display_index(t, i);
	if (!p)
		return (struct rf_packet_summary){0};
	if (ok)
		*ok = 1;
	return summary_from_packet(p);
}

static int packet_summary_passes_filters(const struct rf_task *t, struct rf_packet_summary p)
{
	if (!t)
		return 0;

	switch (t->filter_crc) {
	case RF_FILTER_CRC_OK:
		if (!p.crc_ok)
			return 0;
		break;
	case RF_FILTER_CRC_BAD:
		if (p.crc_ok)
			return 0;
		break;
	default:
		break;
	}

	switch (t->filter_channel) {
	case RF_FILTER_CH_SELECTED:
		if ((int)p.channel != t->selected_channel)
			return 0;
		break;
	case RF_FILTER_CH_RANGE:
		if ((int)p.channel < t->channel_range_lo || (int)p.channel > t->channel_range_hi)
			return 0;
		break;
	default:
		break;
	}

	if (t->filter_min_len > 0 && (int)p.length < t->filter_min_len)
		return 0;
	if (t->filter_max_len > 0 && (int)p.length > t->filter_max_len)
		return 0;

	if (t->filter_addr_len > 0) {
		if ((int)p.addr_len < t->filter_addr_len)
			return 0;
		for (int i = 0; i < t->filter_addr_len; i++) {
			uint8_t mask = t->filter_addr_mask[i];
			if (mask == 0)
				continue;
			if ((p.addr[i] & mask) != (t->filter_addr[i] & mask))
				return 0;
		}
	}

	if (t->filter_payload_len > 0) {
		if ((int)p.length < t->filter_payload_len)
			return 0;
		for (int i = 0; i < t->filter_payload_len && i < RF_PAYLOAD_PREFIX_BYTES; i++) {
			uint8_t mask = t->filter_payload_mask[i];
			if (mask == 0)
				continue;
			if ((p.payload_prefix[i] & mask) != (t->filter_payload[i] & mask))
				return 0;
		}
	}

	if (t->filter_age_ms > 0) {
		uint64_t now = t->now_tick;
		if (t->replay_active)
			now = t->replay_now_tick;
		if (now > p.tick && (now - p.tick) > (uint64_t)t->filter_age_ms)
			return 0;
	}

	if (t->filter_burst_max_ms > 0) {
		if (p.delta_ms == 0 || (int)p.delta_ms > t->filter_burst_max_ms)
			return 0;
	}

	return 1;
}

int rf_sniffer_filtered_count(const struct rf_task *t)
{
	if (!t)
		return 0;
	int n = 0;
	int total = packet_visible_count(t);
	for (int i = 0; i < total; i++) {
		int ok = 0;
		struct rf_packet_summary p = packet_summary_by_display_index(t, i, &ok);
		if (!ok)
			continue;
		if (packet_summary_passes_filters(t, p))
			n++;
	}
	return n;
}

int rf_sniffer_filtered_packet_summary_by_index(const struct rf_task *t, int idx, struct rf_packet_summary *out)
{
	if (!t || !out || idx < 0)
		return 0;
	int seen = 0;
	int total = packet_visible_count(t);
	for (int i = 0; i < total; i++) {
		int ok = 0;
		struct rf_packet_summary p = packet_summary_by_display_index(t, i, &ok);
		if (!ok || !packet_summary_passes_filters(t, p))
			continue;
		if (seen == idx) {
			*out = p;
			return 1;
		}
		seen++;
	}
	return 0;
}

const struct rf_packet *rf_sniffer_filtered_live_packet_by_index(const struct rf_task *t, int idx)
{
	if (!t || idx < 0)
		return NULL;
	if (t->replay_active)
		return NULL;
	int seen = 0;
	for (int i = 0; i < t->pkt_count; i++) {
		const struct rf_packet *p = packet_by_display_index(t, i);
		if (!p)
			continue;
		if (!packet_summary_passes_filters(t, summary_from_packet(p)))
			continue;
		if (seen == idx)
			return p;
		seen++;
	}
	return NULL;
}

int rf_sniffer_filtered_replay_packet_meta_by_index(const struct rf_task *t, int idx, struct rf_session_packet_meta *out)
{
	if (!out)
		return 0;
	memset(out, 0, sizeof(*out));
	if (!t || !t->replay_active || !t->replay || idx < 0)
		return 0;

	int seen = 0;
	int limit = replay_limit(t);
	for (int i = 0; i < limit; i++) {
		int meta_idx = limit - 1 - i;
		if (meta_idx < 0 || (size_t)meta_idx >= t->replay->packet_count)
			continue;
		struct rf_session_packet_meta meta = t->replay->packets[meta_idx];
		if (!packet_summary_passes_filters(t, summary_from_meta(&meta)))
			continue;
		if (seen == idx) {
			*out = meta;
			return 1;
		}
		seen++;
	}
	return 0;
}

void rf_sniffer_reconcile_selection(struct rf_task *t)
{
	if (!t)
		return;

	if (t->sniffer_sel_seq == 0) {
		struct rf_packet_summary p;
		if (rf_sniffer_filtered_packet_summary_by_index(t, 0, &p) && p.seq != 0) {
			t->sniffer_sel = 0;
			t->sniffer_sel_seq = p.seq;
		}
		return;
	}

	int seen = 0;
	int total = packet_visible_count(t);
	for (int i = 0; i < total; i++) {
		int ok = 0;
		struct rf_packet_summary p = packet_summary_by_display_index(t, i, &ok);
		if (!ok || !packet_summary_passes_filters(t, p))
			continue;
		if (p.seq == t->sniffer_sel_seq) {
			t->sniffer_sel = seen;
			return;
		}
		seen++;
	}

	struct rf_packet_summary p0;
	if (rf_sniffer_filtered_packet_summary_by_index(t, 0, &p0) && p0.seq != 0) {
		t->sniffer_sel = 0;
		t->sniffer_sel_seq = p0.seq;
	} else {
		t->sniffer_sel = 0;
		t->sniffer_sel_seq = 0;
	}
	if (t->replay_active)
		t->replay_pkt_cache_ok = 0;
}

static int sniffer_list_rows(const struct rf_task *t)
{
	struct rf_layout l = rf_compute_layout(t);
	struct rf_rect inner = rf_rect_inset(l.sniffer, 2, 2);
	int rows = (int)((inner.h - 2 * RF_FONT_H) / RF_FONT_H);
	if (rows < 1)
		rows = 1;
	return rows;
}

void rf_sniffer_move_selection(struct rf_task *t, int delta)
{
	if (!t || delta == 0)
		return;

	int total = rf_sniffer_filtered_count(t);
	if (total <= 0)
		return;

	int sel = t->sniffer_sel + delta;
	if (sel < 0)
		sel = 0;
	if (sel >= total)
		sel = total - 1;
	if (sel == t->sniffer_sel)
		return;

	t->sniffer_sel = sel;
	struct rf_packet_summary p;
	if (rf_sniffer_filtered_packet_summary_by_index(t, t->sniffer_sel, &p) && p.seq != 0)
		t->sniffer_sel_seq = p.seq;

	int rows = sniffer_list_rows(t);
	int max_top = total - rows;
	if (max_top < 0)
		max_top = 0;
	if (t->sniffer_top < 0)
		t->sniffer_top = 0;
	if (t->sniffer_top > max_top)
		t->sniffer_top = max_top;
	if (t->sniffer_sel < t->sniffer_top)
		t->sniffer_top = t->sniffer_sel;
	if (t->sniffer_sel >= t->sniffer_top + rows) {
		t->sniffer_top = t->sniffer_sel - rows + 1;
		if (t->sniffer_top > max_top)
			t->sniffer_top = max_top;
	}

	rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
}

void rf_sniffer_tick_pps(struct rf_task *t, uint64_t now)
{
	if (!t)
		return;
	if (t->pkt_sec_start == 0) {
		t->pkt_sec_start = now;
		return;
	}
	if (now - t->pkt_sec_start < 1000)
		return;
	t->pkts_per_sec = t->pkt_sec_count;
	t->pkt_sec_count = 0;
	t->pkt_sec_start = now;
	rf_task_invalidate(t, RF_DIRTY_STATUS);
}

void rf_sniffer_append_packet(struct rf_task *t, struct rf_packet p)
{
	if (!t)
		return;

	if (t->pkt_seq == 0)
		t->pkt_seq = 1;
	p.seq = t->pkt_seq++;

	p.payload_hash = rf_fnv1a32(p.payload, p.length);
	p.delta_ms = 0;
	p.flags = 0;

	rf_recording_record_packet(t, &p);

	if (t->pkt_count < RF_MAX_PACKETS) {
		t->packets[t->pkt_head] = p;
		t->pkt_head++;
		if (t->pkt_head >= RF_MAX_PACKETS)
			t->pkt_head = 0;
		t->pkt_count++;
	} else {
		t->packets[t->pkt_head] = p;
		t->pkt_head++;
		if (t->pkt_head >= RF_MAX_PACKETS)
			t->pkt_head = 0;
		t->pkt_dropped++;
	}

	t->pkt_sec_count++;
	rf_task_invalidate(t, RF_DIRTY_SNIFFER | RF_DIRTY_PROTOCOL | RF_DIRTY_STATUS);
	rf_sniffer_reconcile_selection(t);
}

void rf_sniffer_maybe_capture_packet(struct rf_task *t, int ch, uint8_t energy, uint64_t tick)
{
	if (!t)
		return;
	if (t->capture_paused)
		return;
	if (energy < 180)
		return;

	if ((t->rng & 0xFFu) > (uint32_t)energy)
		return;

	if (ch != 76 && ch != 91 && ch != t->selected_channel) {
		if ((t->rng & 0x03u) != 0)
			return;
	}

	struct rf_packet p;
	memset(&p, 0, sizeof(p));
	p.tick = tick;
	p.channel = (uint8_t)ch;
	p.rate = t->data_rate;
	p.addr_len = 5;

	switch (ch) {
	case 76:
		memset(p.addr, 0xE7, 5);
		break;
	case 91:
		p.addr[0] = 0xAA;
		p.addr[1] = 0xBB;
		p.addr[2] = 0xCC;
		p.addr[3] = 0xDD;
		p.addr[4] = 0xEE;
		break;
	default:
		p.addr[0] = 0x11;
		p.addr[1] = 0x22;
		p.addr[2] = 0x33;
		p.addr[3] = 0x44;
		p.addr[4] = 0x55;
		break;
	}

	int ln = (int)((t->rng >> 8) % 25u) + 8;
	if (ln > 32)
		ln = 32;
	p.length = (uint8_t)ln;

	uint32_t x = t->rng;
	for (int i = 0; i < ln; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p.payload[i] = (uint8_t)x;
	}
	t->rng = x;

	switch (t->crc_mode) {
	case RF_CRC_1B:
		p.crc_len = 1;
		break;
	case RF_CRC_2B:
		p.crc_len = 2;
		break;
	default:
		p.crc_len = 0;
		break;
	}
	if (p.crc_len > 0) {
		p.crc[0] = (uint8_t)(x >> 8);
		p.crc[1] = (uint8_t)(x >> 16);
		p.crc_ok = ((x & 0x0Fu) != 0);
	}

	rf_sniffer_append_packet(t, p);
}

static uint8_t hex_digit(uint8_t v)
{
	v &= 0x0F;
	if (v < 10)
		return (uint8_t)('0' + v);
	return (uint8_t)('A' + (v - 10));
}

static void hex_mask_byte(uint8_t v, uint8_t mask, char out[3])
{
	char hi = '?';
	char lo = '?';
	if ((mask & 0xF0) != 0)
		hi = (char)hex_digit(v >> 4);
	if ((mask & 0x0F) != 0)
		lo = (char)hex_digit(v & 0x0F);
	out[0] = hi;
	out[1] = lo;
	out[2] = 0;
}

static void append_str(char *dst, unsigned dstsz, const char *src)
{
	if (!dst || dstsz == 0 || !src)
		return;
	size_t n = strlen(dst);
	if (n >= dstsz)
		return;
	snprintf(dst + n, dstsz - n, "%s", src);
}

void rf_sniffer_filter_summary(const struct rf_task *t, char *out, unsigned outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!t)
		return;

	snprintf(out, outsz, "CRC:%s CH:%s", rf_filter_crc_str(t->filter_crc), rf_filter_channel_str(t->filter_channel));

	if (t->filter_min_len > 0 || t->filter_max_len > 0) {
		char tmp[24];
		snprintf(tmp, sizeof(tmp), " LEN:%d..%d", t->filter_min_len, t->filter_max_len);
		append_str(out, outsz, tmp);
	}

	if (t->filter_addr_len > 0) {
		append_str(out, outsz, " ADDR:");
		for (int i = 0; i < t->filter_addr_len && i < 5; i++) {
			char b[3];
			hex_mask_byte(t->filter_addr[i], t->filter_addr_mask[i], b);
			append_str(out, outsz, b);
		}
	}

	if (t->filter_payload_len > 0) {
		append_str(out, outsz, " PAY:");
		for (int i = 0; i < t->filter_payload_len && i < RF_PAYLOAD_PREFIX_BYTES; i++) {
			char b[3];
			hex_mask_byte(t->filter_payload[i], t->filter_payload_mask[i], b);
			append_str(out, outsz, b);
		}
	}

	if (t->filter_age_ms > 0) {
		char tmp[24];
		snprintf(tmp, sizeof(tmp), " AGE:%dms", t->filter_age_ms);
		append_str(out, outsz, tmp);
	}

	if (t->filter_burst_max_ms > 0) {
		char tmp[24];
		snprintf(tmp, sizeof(tmp), " BΔ<=%dms", t->filter_burst_max_ms);
		append_str(out, outsz, tmp);
	}
}
