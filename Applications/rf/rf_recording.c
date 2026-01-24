#include "rf_recording.h"

#include "rf.h"
#include "rf_fs.h"
#include "rf_sniffer.h"
#include "rf_task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char session_magic[] = "RFLOGv1\n";
static const char *const session_dir = "/rf/sessions";
static const char *const session_ext = ".rflog";

enum {
	record_flush_interval_ticks = 250,
	max_record_buf = 32 * 1024,
};

enum rf_session_record_type {
	RF_REC_CONFIG = 1,
	RF_REC_SWEEP,
	RF_REC_PACKET,
	RF_REC_ANNOTATION,
	RF_REC_EVENT,
};

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

static void stop_recording_due_to_error(struct rf_task *t, const char *msg)
{
	if (!t)
		return;
	if (msg && msg[0])
		snprintf(t->record_err, sizeof(t->record_err), "%s", msg);
	t->recording = 0;
	if (t->record_fd >= 0) {
		(void)close(t->record_fd);
		t->record_fd = -1;
	}
	t->record_len = 0;
	t->record_next_flush_tick = 0;
	rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
}

static int ensure_session_dir(char *err, size_t errsz)
{
	if (rf_fs_ensure_dir("/rf", 0755, err, errsz) != 0)
		return -1;
	if (rf_fs_ensure_dir(session_dir, 0755, err, errsz) != 0)
		return -1;
	return 0;
}

static int session_path(const char *name_in, char safe[32], char path[96], char *err, size_t errsz)
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
			snprintf(err, errsz, "empty session name");
		return -1;
	}
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%s", name_in);
	trim_inplace(tmp);
	if (!tmp[0]) {
		if (err && errsz)
			snprintf(err, errsz, "empty session name");
		return -1;
	}
	if (strchr(tmp, '/')) {
		if (err && errsz)
			snprintf(err, errsz, "session name may not contain '/'");
		return -1;
	}
	rf_sanitize_name(tmp, safe, 32);
	if (!safe[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid session name");
		return -1;
	}
	if (ends_with(safe, session_ext))
		safe[strlen(safe) - strlen(session_ext)] = 0;
	if (!safe[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid session name");
		return -1;
	}
	if (snprintf(path, 96, "%s/%s%s", session_dir, safe, session_ext) >= 96) {
		if (err && errsz)
			snprintf(err, errsz, "session path too long");
		return -1;
	}
	return 0;
}

static void put_u16_le(uint8_t out[2], uint16_t v)
{
	out[0] = (uint8_t)(v & 0xffu);
	out[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32_le(uint8_t out[4], uint32_t v)
{
	out[0] = (uint8_t)(v & 0xffu);
	out[1] = (uint8_t)((v >> 8) & 0xffu);
	out[2] = (uint8_t)((v >> 16) & 0xffu);
	out[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64_le(uint8_t out[8], uint64_t v)
{
	out[0] = (uint8_t)(v & 0xffu);
	out[1] = (uint8_t)((v >> 8) & 0xffu);
	out[2] = (uint8_t)((v >> 16) & 0xffu);
	out[3] = (uint8_t)((v >> 24) & 0xffu);
	out[4] = (uint8_t)((v >> 32) & 0xffu);
	out[5] = (uint8_t)((v >> 40) & 0xffu);
	out[6] = (uint8_t)((v >> 48) & 0xffu);
	out[7] = (uint8_t)((v >> 56) & 0xffu);
}

static int ensure_space(struct rf_task *t, uint64_t now, size_t need)
{
	if (!t || !t->recording)
		return 0;
	if (!t->record_buf || t->record_cap == 0)
		return -1;
	if (need > t->record_cap)
		return -1;
	if (t->record_len + need <= t->record_cap)
		return 0;
	rf_recording_flush(t, now, 1);
	if (!t->recording)
		return -1;
	return (t->record_len + need <= t->record_cap) ? 0 : -1;
}

static size_t rec_start(struct rf_task *t, uint8_t kind)
{
	size_t start = t->record_len;
	t->record_buf[t->record_len++] = kind;
	t->record_buf[t->record_len++] = 0;
	t->record_buf[t->record_len++] = 0;
	t->record_buf[t->record_len++] = 0;
	t->record_buf[t->record_len++] = 0;
	return start;
}

static void rec_finish(struct rf_task *t, size_t start)
{
	if (!t || !t->record_buf)
		return;
	if (start + 5 > t->record_len)
		return;
	uint32_t payload_len = (uint32_t)(t->record_len - (start + 5));
	put_u32_le(t->record_buf + start + 1, payload_len);
}

static void rec_u8(struct rf_task *t, uint8_t v)
{
	t->record_buf[t->record_len++] = v;
}

static void rec_u16(struct rf_task *t, uint16_t v)
{
	uint8_t b[2];
	put_u16_le(b, v);
	memcpy(t->record_buf + t->record_len, b, sizeof(b));
	t->record_len += sizeof(b);
}

static void rec_u32(struct rf_task *t, uint32_t v)
{
	uint8_t b[4];
	put_u32_le(b, v);
	memcpy(t->record_buf + t->record_len, b, sizeof(b));
	t->record_len += sizeof(b);
}

static void rec_u64(struct rf_task *t, uint64_t v)
{
	uint8_t b[8];
	put_u64_le(b, v);
	memcpy(t->record_buf + t->record_len, b, sizeof(b));
	t->record_len += sizeof(b);
}

int rf_recording_start(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad task");
		return -1;
	}
	if (t->recording) {
		if (err && errsz)
			snprintf(err, errsz, "recording already active");
		return -1;
	}

	if (ensure_session_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	char path[96];
	if (session_path(name, safe, path, err, errsz) != 0)
		return -1;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return -1;
	}
	if (rf_fs_write_all(fd, session_magic, sizeof(session_magic) - 1, err, errsz) != 0) {
		(void)close(fd);
		return -1;
	}

	if (!t->record_buf) {
		t->record_buf = (uint8_t *)malloc(max_record_buf);
		if (!t->record_buf) {
			(void)close(fd);
			if (err && errsz)
				snprintf(err, errsz, "out of memory");
			return -1;
		}
		t->record_cap = max_record_buf;
	}

	t->recording = 1;
	t->record_fd = fd;
	snprintf(t->record_name, sizeof(t->record_name), "%s", safe);
	snprintf(t->record_path, sizeof(t->record_path), "%s", path);
	t->record_len = 0;
	t->record_next_flush_tick = t->now_tick + record_flush_interval_ticks;
	t->record_sweeps = 0;
	t->record_packets = 0;
	t->record_bytes = (uint32_t)(sizeof(session_magic) - 1);
	t->record_err[0] = 0;

	rf_recording_record_config(t, t->now_tick);
	rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
	return 0;
}

int rf_recording_stop(struct rf_task *t, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t)
		return -1;
	if (!t->recording)
		return 0;

	rf_recording_flush(t, t->now_tick, 1);
	if (t->record_fd >= 0) {
		(void)close(t->record_fd);
		t->record_fd = -1;
	}
	t->recording = 0;
	t->record_name[0] = 0;
	t->record_path[0] = 0;
	t->record_len = 0;
	t->record_next_flush_tick = 0;
	rf_task_invalidate(t, RF_DIRTY_STATUS | RF_DIRTY_RFCONTROL);
	return 0;
}

void rf_recording_flush(struct rf_task *t, uint64_t now, int force)
{
	if (!t || !t->recording)
		return;
	if (t->record_fd < 0 || !t->record_buf || t->record_cap == 0)
		return;

	if (!force) {
		if (now < t->record_next_flush_tick && t->record_len < max_record_buf)
			return;
	}
	if (t->record_len == 0) {
		t->record_next_flush_tick = now + record_flush_interval_ticks;
		return;
	}

	char werr[96];
	if (rf_fs_write_all(t->record_fd, t->record_buf, t->record_len, werr, sizeof(werr)) != 0) {
		stop_recording_due_to_error(t, werr);
		return;
	}
	t->record_bytes += (uint32_t)t->record_len;
	t->record_len = 0;
	t->record_next_flush_tick = now + record_flush_interval_ticks;
}

void rf_recording_record_config(struct rf_task *t, uint64_t now)
{
	if (!t || !t->recording)
		return;

	const size_t payload = 19;
	const size_t need = 5 + payload;
	if (ensure_space(t, now, need) != 0)
		return;

	size_t start = rec_start(t, RF_REC_CONFIG);
	rec_u64(t, now);
	rec_u8(t, (uint8_t)t->channel_range_lo);
	rec_u8(t, (uint8_t)t->channel_range_hi);
	rec_u8(t, (uint8_t)t->selected_channel);
	rec_u16(t, (uint16_t)t->dwell_time_ms);
	rec_u8(t, (uint8_t)rf_clamp_int(t->scan_speed_scalar, 1, 10));
	rec_u8(t, (uint8_t)t->data_rate);
	rec_u8(t, (uint8_t)t->crc_mode);
	rec_u8(t, t->auto_ack ? 1u : 0u);
	rec_u8(t, (uint8_t)t->power_level);
	rec_u8(t, (uint8_t)t->wf_palette);
	rec_finish(t, start);
}

void rf_recording_record_sweep(struct rf_task *t, uint64_t now)
{
	if (!t || !t->recording)
		return;

	const size_t payload = 8 + RF_NUM_CHANNELS;
	const size_t need = 5 + payload;
	if (ensure_space(t, now, need) != 0)
		return;

	size_t start = rec_start(t, RF_REC_SWEEP);
	rec_u64(t, now);
	for (int i = 0; i < RF_NUM_CHANNELS; i++)
		rec_u8(t, t->energy_avg[i]);
	rec_finish(t, start);
	t->record_sweeps++;
}

void rf_recording_record_packet(struct rf_task *t, const struct rf_packet *p)
{
	if (!t || !t->recording || !p)
		return;

	uint8_t addr_len = p->addr_len;
	if (addr_len > 5)
		addr_len = 5;
	uint8_t len = p->length;
	if (len > 32)
		len = 32;
	uint8_t crc_len = p->crc_len;
	if (crc_len > 2)
		crc_len = 2;

	const size_t payload = 8 + 4 + 1 + 1 + 1 + addr_len + 1 + len + 1 + crc_len + 1;
	const size_t need = 5 + payload;
	if (ensure_space(t, p->tick, need) != 0)
		return;

	size_t start = rec_start(t, RF_REC_PACKET);
	rec_u64(t, p->tick);
	rec_u32(t, p->seq);
	rec_u8(t, p->channel);
	rec_u8(t, (uint8_t)p->rate);
	rec_u8(t, addr_len);
	for (int i = 0; i < (int)addr_len; i++)
		rec_u8(t, p->addr[i]);
	rec_u8(t, len);
	for (int i = 0; i < (int)len; i++)
		rec_u8(t, p->payload[i]);
	rec_u8(t, crc_len);
	for (int i = 0; i < (int)crc_len; i++)
		rec_u8(t, p->crc[i]);
	rec_u8(t, p->crc_ok ? 1u : 0u);

	rec_finish(t, start);
	t->record_packets++;
}
