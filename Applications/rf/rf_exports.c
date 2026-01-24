#include "rf_exports.h"

#include "rf.h"
#include "rf_fs.h"
#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const export_dir = "/rf/exports";

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

static int ensure_export_dir(char *err, size_t errsz)
{
	if (rf_fs_ensure_dir("/rf", 0755, err, errsz) != 0)
		return -1;
	if (rf_fs_ensure_dir(export_dir, 0755, err, errsz) != 0)
		return -1;
	return 0;
}

static int export_path(const char *name_in, const char *ext, char safe_out[32], char path_out[96], char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!ext || !safe_out || !path_out) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}
	safe_out[0] = 0;
	path_out[0] = 0;

	if (!name_in) {
		if (err && errsz)
			snprintf(err, errsz, "empty export name");
		return -1;
	}

	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%s", name_in);
	/* trim */
	char *name = tmp;
	while (*name == ' ' || *name == '\t' || *name == '\r' || *name == '\n')
		name++;
	size_t n = strlen(name);
	while (n && (name[n - 1] == ' ' || name[n - 1] == '\t' || name[n - 1] == '\r' || name[n - 1] == '\n')) {
		name[n - 1] = 0;
		n--;
	}
	if (!name[0]) {
		if (err && errsz)
			snprintf(err, errsz, "empty export name");
		return -1;
	}
	if (strchr(name, '/')) {
		if (err && errsz)
			snprintf(err, errsz, "export name may not contain '/'");
		return -1;
	}

	rf_sanitize_name(name, safe_out, 32);
	if (!safe_out[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid export name");
		return -1;
	}
	if (ends_with(safe_out, ext))
		safe_out[strlen(safe_out) - strlen(ext)] = 0;
	if (!safe_out[0]) {
		if (err && errsz)
			snprintf(err, errsz, "invalid export name");
		return -1;
	}

	if (snprintf(path_out, 96, "%s/%s%s", export_dir, safe_out, ext) >= 96) {
		if (err && errsz)
			snprintf(err, errsz, "export path too long");
		return -1;
	}
	return 0;
}

static int write_all_fd(int fd, const void *buf, size_t len, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (fd < 0 || (!buf && len))
		return -1;
	const unsigned char *p = (const unsigned char *)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (err && errsz)
				snprintf(err, errsz, "write: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			if (err && errsz)
				snprintf(err, errsz, "write: short write");
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void u32_le(uint8_t out[4], uint32_t v)
{
	out[0] = (uint8_t)(v & 0xFF);
	out[1] = (uint8_t)((v >> 8) & 0xFF);
	out[2] = (uint8_t)((v >> 16) & 0xFF);
	out[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void u16_le(uint8_t out[2], uint16_t v)
{
	out[0] = (uint8_t)(v & 0xFF);
	out[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void u64_dec(uint64_t v, char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;

	char tmp[24];
	unsigned n = 0;
	if (v == 0) {
		tmp[n++] = '0';
	} else {
		while (v && n + 1 < sizeof(tmp)) {
			tmp[n++] = (char)('0' + (v % 10u));
			v /= 10u;
		}
	}

	unsigned w = 0;
	while (n && w + 1 < outsz) {
		out[w++] = tmp[--n];
	}
	out[w] = 0;
}

static char hex_digit(uint8_t v)
{
	v &= 0x0F;
	if (v < 10)
		return (char)('0' + v);
	return (char)('A' + (v - 10));
}

static void addr_hex(uint8_t addr_len, const uint8_t addr[5], char *out, size_t outsz)
{
	if (!out || outsz == 0)
		return;
	out[0] = 0;
	if (!addr || addr_len == 0)
		return;
	if (addr_len > 5)
		addr_len = 5;
	if (outsz < (size_t)addr_len * 2 + 1)
		return;
	for (unsigned i = 0; i < addr_len; i++) {
		uint8_t v = addr[i];
		out[i * 2 + 0] = hex_digit(v >> 4);
		out[i * 2 + 1] = hex_digit(v);
	}
	out[addr_len * 2] = 0;
}

static uint8_t packet_export_flags(const struct rf_session_packet_meta *m)
{
	if (!m)
		return 0;
	uint8_t f = 0;
	if (m->crc_len > 0)
		f |= 1u << 1;
	if (m->crc_ok)
		f |= 1u << 0;
	if ((m->flags & RF_PKT_FLAG_RETRY) != 0)
		f |= 1u << 2;
	if ((m->flags & RF_PKT_FLAG_BURST) != 0)
		f |= 1u << 3;
	return f;
}

static int rf24_raw_frame(const struct rf_packet *p, uint8_t out[1 + 5 + 32 + 2], uint8_t *out_len)
{
	if (out_len)
		*out_len = 0;
	if (!p || !out || !out_len)
		return -1;

	uint8_t n = 0;
	out[n++] = 0x55;
	for (int i = 0; i < p->addr_len && i < 5; i++)
		out[n++] = p->addr[i];
	for (int i = 0; i < p->length && i < 32; i++)
		out[n++] = p->payload[i];
	for (int i = 0; i < p->crc_len && i < 2; i++)
		out[n++] = p->crc[i];
	*out_len = n;
	return 0;
}

int rf_exports_export_csv(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}
	if (!t->replay_active || !t->replay) {
		if (err && errsz)
			snprintf(err, errsz, "replay not active");
		return -1;
	}

	if (ensure_export_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	char path[96];
	if (export_path(name, ".csv", safe, path, err, errsz) != 0)
		return -1;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return -1;
	}

	const char header[] = "t_ms,abs_tick,seq,ch,rate,len,addr,crc_len,crc_ok,delta_ms,flags,payload_hash\n";
	char werr[96];
	if (write_all_fd(fd, header, sizeof(header) - 1, werr, sizeof(werr)) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "%s", werr);
		(void)close(fd);
		return -1;
	}

	char buf[16 * 1024];
	size_t blen = 0;
	uint64_t base = t->replay->start_tick;

	for (size_t i = 0; i < t->replay->packet_count; i++) {
		const struct rf_session_packet_meta *m = &t->replay->packets[i];
		uint64_t rel = 0;
		if (m->tick >= base)
			rel = m->tick - base;

		char rel_s[24];
		char tick_s[24];
		u64_dec(rel, rel_s, sizeof(rel_s));
		u64_dec(m->tick, tick_s, sizeof(tick_s));

		char addr[11];
		addr_hex(m->addr_len, m->addr, addr, sizeof(addr));

		char line[160];
		snprintf(line, sizeof(line), "%s,%s,%lu,%u,%s,%u,%s,%u,%u,%u,%u,%08lX\n", rel_s, tick_s,
			 (unsigned long)m->seq, (unsigned)m->channel, rf_data_rate_str(m->rate), (unsigned)m->length, addr,
			 (unsigned)m->crc_len, (unsigned)(m->crc_ok ? 1 : 0), (unsigned)m->delta_ms, (unsigned)m->flags,
			 (unsigned long)m->payload_hash);

		size_t ln = strlen(line);
		if (ln > sizeof(buf))
			ln = sizeof(buf);
		if (blen + ln > sizeof(buf)) {
			if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
				if (err && errsz)
					snprintf(err, errsz, "%s", werr);
				(void)close(fd);
				return -1;
			}
			blen = 0;
		}
		memcpy(buf + blen, line, ln);
		blen += ln;
	}

	if (blen) {
		if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", werr);
			(void)close(fd);
			return -1;
		}
	}

	(void)close(fd);
	return 0;
}

int rf_exports_export_rfpkt(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}
	if (!t->replay_active || !t->replay) {
		if (err && errsz)
			snprintf(err, errsz, "replay not active");
		return -1;
	}

	if (ensure_export_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	char path[96];
	if (export_path(name, ".rfpkt", safe, path, err, errsz) != 0)
		return -1;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return -1;
	}

	char werr[96];
	if (write_all_fd(fd, "RFPKTv1\n", 8, werr, sizeof(werr)) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "%s", werr);
		(void)close(fd);
		return -1;
	}

	uint64_t base = t->replay->start_tick;

	uint8_t buf[16 * 1024];
	size_t blen = 0;

	for (size_t i = 0; i < t->replay->packet_count; i++) {
		const struct rf_session_packet_meta *meta = &t->replay->packets[i];

		struct rf_packet p;
		char perr[96];
		if (rf_session_read_packet(t->replay, meta->off, &p, perr, sizeof(perr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", perr[0] ? perr : "read packet failed");
			(void)close(fd);
			return -1;
		}

		uint64_t rel = 0;
		if (meta->tick >= base)
			rel = meta->tick - base;
		if (rel > 0xFFFFFFFFu)
			rel = 0xFFFFFFFFu;

		uint8_t raw[1 + 5 + 32 + 2];
		uint8_t raw_len = 0;
		(void)rf24_raw_frame(&p, raw, &raw_len);

		uint8_t hdr[8];
		u32_le(hdr + 0, (uint32_t)rel);
		hdr[4] = meta->channel;
		hdr[5] = (uint8_t)meta->rate;
		hdr[6] = packet_export_flags(meta);
		hdr[7] = raw_len;

		size_t rec_len = sizeof(hdr) + (size_t)raw_len;
		if (rec_len > sizeof(buf)) {
			if (err && errsz)
				snprintf(err, errsz, "record too large");
			(void)close(fd);
			return -1;
		}
		if (blen + rec_len > sizeof(buf)) {
			if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
				if (err && errsz)
					snprintf(err, errsz, "%s", werr);
				(void)close(fd);
				return -1;
			}
			blen = 0;
		}
		memcpy(buf + blen, hdr, sizeof(hdr));
		blen += sizeof(hdr);
		memcpy(buf + blen, raw, raw_len);
		blen += raw_len;
	}

	if (blen) {
		if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", werr);
			(void)close(fd);
			return -1;
		}
	}

	(void)close(fd);
	return 0;
}

int rf_exports_export_pcap(struct rf_task *t, const char *name, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}
	if (!t->replay_active || !t->replay) {
		if (err && errsz)
			snprintf(err, errsz, "replay not active");
		return -1;
	}

	if (ensure_export_dir(err, errsz) != 0)
		return -1;

	char safe[32];
	char path[96];
	if (export_path(name, ".pcap", safe, path, err, errsz) != 0)
		return -1;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return -1;
	}

	uint8_t gh[24];
	memset(gh, 0, sizeof(gh));
	u32_le(gh + 0, 0xa1b2c3d4u);
	u16_le(gh + 4, 2);
	u16_le(gh + 6, 4);
	u32_le(gh + 16, 96);
	u32_le(gh + 20, 147); /* DLT_USER0 */

	char werr[96];
	if (write_all_fd(fd, gh, sizeof(gh), werr, sizeof(werr)) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "%s", werr);
		(void)close(fd);
		return -1;
	}

	uint64_t base = t->replay->start_tick;

	uint8_t buf[16 * 1024];
	size_t blen = 0;

	for (size_t i = 0; i < t->replay->packet_count; i++) {
		const struct rf_session_packet_meta *meta = &t->replay->packets[i];

		struct rf_packet p;
		char perr[96];
		if (rf_session_read_packet(t->replay, meta->off, &p, perr, sizeof(perr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", perr[0] ? perr : "read packet failed");
			(void)close(fd);
			return -1;
		}

		uint64_t rel = 0;
		if (meta->tick >= base)
			rel = meta->tick - base;
		uint32_t ts_sec = (uint32_t)(rel / 1000u);
		uint32_t ts_usec = (uint32_t)((rel % 1000u) * 1000u);

		uint8_t raw[1 + 5 + 32 + 2];
		uint8_t raw_len = 0;
		(void)rf24_raw_frame(&p, raw, &raw_len);

		uint8_t pkt[12 + sizeof(raw)];
		size_t pkt_len = 0;
		pkt[pkt_len++] = 'R';
		pkt[pkt_len++] = 'F';
		pkt[pkt_len++] = '2';
		pkt[pkt_len++] = '4';
		pkt[pkt_len++] = 1;
		pkt[pkt_len++] = meta->channel;
		pkt[pkt_len++] = (uint8_t)meta->rate;
		pkt[pkt_len++] = packet_export_flags(meta);
		pkt[pkt_len++] = meta->addr_len;
		pkt[pkt_len++] = meta->length;
		pkt[pkt_len++] = meta->crc_len;
		pkt[pkt_len++] = 0;
		memcpy(pkt + pkt_len, raw, raw_len);
		pkt_len += raw_len;

		uint8_t ph[16];
		u32_le(ph + 0, ts_sec);
		u32_le(ph + 4, ts_usec);
		u32_le(ph + 8, (uint32_t)pkt_len);
		u32_le(ph + 12, (uint32_t)pkt_len);

		size_t rec_len = sizeof(ph) + pkt_len;
		if (rec_len > sizeof(buf)) {
			if (err && errsz)
				snprintf(err, errsz, "pcap record too large");
			(void)close(fd);
			return -1;
		}
		if (blen + rec_len > sizeof(buf)) {
			if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
				if (err && errsz)
					snprintf(err, errsz, "%s", werr);
				(void)close(fd);
				return -1;
			}
			blen = 0;
		}

		memcpy(buf + blen, ph, sizeof(ph));
		blen += sizeof(ph);
		memcpy(buf + blen, pkt, pkt_len);
		blen += pkt_len;
	}

	if (blen) {
		if (write_all_fd(fd, buf, blen, werr, sizeof(werr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", werr);
			(void)close(fd);
			return -1;
		}
	}

	(void)close(fd);
	return 0;
}

