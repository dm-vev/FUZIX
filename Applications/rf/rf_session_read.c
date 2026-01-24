#include "rf_session.h"

#include "rf.h"
#include "rf_fs.h"
#include "rf_hash.h"
#include "rf_sniffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char session_magic[] = "RFLOGv1\n";
static const char *const session_dir = "/rf/sessions";
static const char *const session_ext = ".rflog";

enum {
	session_magic_len = 8,
	max_record_len = 64 * 1024,
	max_payload_buf = 512,
	max_sweeps = 16384,
	max_packets = 32768,
	max_configs = 2048,
};

enum rf_session_record_type {
	RF_REC_CONFIG = 1,
	RF_REC_SWEEP,
	RF_REC_PACKET,
	RF_REC_ANNOTATION,
	RF_REC_EVENT,
};

struct dev_track {
	uint8_t used;
	uint8_t addr_len;
	uint8_t addr[5];

	uint64_t last_tick;

	uint32_t last_payload_hash;
	uint64_t last_payload_tick;
};

static uint16_t rd_u16_le(const uint8_t in[2])
{
	return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t in[4])
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static uint64_t rd_u64_le(const uint8_t in[8])
{
	return (uint64_t)in[0] | ((uint64_t)in[1] << 8) | ((uint64_t)in[2] << 16) | ((uint64_t)in[3] << 24) |
	       ((uint64_t)in[4] << 32) | ((uint64_t)in[5] << 40) | ((uint64_t)in[6] << 48) | ((uint64_t)in[7] << 56);
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

static int read_exact(int fd, void *buf, size_t len, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (fd < 0 || (!buf && len))
		return -1;

	unsigned char *p = (unsigned char *)buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = read(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (err && errsz)
				snprintf(err, errsz, "read: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			if (err && errsz)
				snprintf(err, errsz, "read: unexpected EOF");
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static int ensure_capacity(void **ptr, size_t *cap, size_t want, size_t elem_size, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!cap || elem_size == 0)
		return -1;
	if (want <= *cap)
		return 0;

	size_t new_cap = (*cap == 0) ? 64 : (*cap * 2);
	while (new_cap < want)
		new_cap *= 2;

	void *np = realloc(*ptr, new_cap * elem_size);
	if (!np) {
		if (err && errsz)
			snprintf(err, errsz, "out of memory");
		return -1;
	}
	*ptr = np;
	*cap = new_cap;
	return 0;
}

static void note_tick(struct rf_session *s, uint64_t tick)
{
	if (!s || tick == 0)
		return;
	if (s->start_tick == 0 || tick < s->start_tick)
		s->start_tick = tick;
	if (tick > s->end_tick)
		s->end_tick = tick;
}

static void derive_packet_meta(struct dev_track devs[RF_MAX_DEVICES], struct rf_session_packet_meta *m)
{
	if (!devs || !m || m->addr_len == 0)
		return;

	uint8_t addr_len = m->addr_len;
	if (addr_len > 5)
		addr_len = 5;

	int match_idx = -1;
	for (int i = 0; i < RF_MAX_DEVICES; i++) {
		if (!devs[i].used || devs[i].addr_len != addr_len)
			continue;
		int match = 1;
		for (int j = 0; j < (int)addr_len; j++) {
			if (devs[i].addr[j] != m->addr[j]) {
				match = 0;
				break;
			}
		}
		if (match) {
			match_idx = i;
			break;
		}
	}

	if (match_idx < 0) {
		int evict = -1;
		uint64_t oldest = 0;
		for (int i = 0; i < RF_MAX_DEVICES; i++) {
			if (!devs[i].used) {
				evict = i;
				break;
			}
			if (evict == -1 || devs[i].last_tick < oldest) {
				oldest = devs[i].last_tick;
				evict = i;
			}
		}
		if (evict < 0)
			return;
		struct dev_track *d = &devs[evict];
		memset(d, 0, sizeof(*d));
		d->used = 1;
		d->addr_len = addr_len;
		memcpy(d->addr, m->addr, 5);
		d->last_tick = m->tick;
		d->last_payload_hash = m->payload_hash;
		d->last_payload_tick = m->tick;
		return;
	}

	struct dev_track *d = &devs[match_idx];
	uint16_t delta_ms = 0;
	uint8_t flags = 0;

	if (d->last_tick != 0 && m->tick > d->last_tick) {
		uint64_t dt = m->tick - d->last_tick;
		if (dt > 0xFFFFu)
			dt = 0xFFFFu;
		delta_ms = (uint16_t)dt;
		if (dt <= 12)
			flags |= RF_PKT_FLAG_BURST;
	}

	if (d->last_payload_hash != 0 && d->last_payload_hash == m->payload_hash && m->tick > d->last_payload_tick &&
	    (m->tick - d->last_payload_tick) <= RF_ANA_RETRY_WINDOW_TICKS) {
		flags |= RF_PKT_FLAG_RETRY;
	}

	d->last_tick = m->tick;
	d->last_payload_hash = m->payload_hash;
	d->last_payload_tick = m->tick;

	m->delta_ms = delta_ms;
	m->flags = flags;
}

static int resolve_session_input(const char *input, char name_out[32], char path_out[96], char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!name_out || !path_out)
		return -1;

	name_out[0] = 0;
	path_out[0] = 0;

	if (!input) {
		if (err && errsz)
			snprintf(err, errsz, "empty session name");
		return -1;
	}

	char tmp[96];
	snprintf(tmp, sizeof(tmp), "%s", input);
	trim_inplace(tmp);
	if (!tmp[0]) {
		if (err && errsz)
			snprintf(err, errsz, "empty session name");
		return -1;
	}

	/* If a path is provided, use it as-is. */
	if (strchr(tmp, '/')) {
		snprintf(path_out, 96, "%s", tmp);
		const char *base = strrchr(tmp, '/');
		base = base ? base + 1 : tmp;
		snprintf(name_out, 32, "%s", base);
		return 0;
	}

	char safe[32];
	rf_sanitize_name(tmp, safe, sizeof(safe));
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

	snprintf(name_out, 32, "%s", safe);
	if (snprintf(path_out, 96, "%s/%s%s", session_dir, safe, session_ext) >= 96) {
		if (err && errsz)
			snprintf(err, errsz, "session path too long");
		return -1;
	}
	return 0;
}

struct rf_session *rf_session_load(const char *input, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;

	char name[32];
	char path[96];
	if (resolve_session_input(input, name, path, err, errsz) != 0)
		return NULL;

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", path, strerror(errno));
		return NULL;
	}

	struct stat st;
	if (fstat(fd, &st) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "stat %s: %s", path, strerror(errno));
		(void)close(fd);
		return NULL;
	}
	if (!S_ISREG(st.st_mode)) {
		if (err && errsz)
			snprintf(err, errsz, "%s: not a file", path);
		(void)close(fd);
		return NULL;
	}
	if (st.st_size < (off_t)session_magic_len) {
		if (err && errsz)
			snprintf(err, errsz, "too small: %ld bytes", (long)st.st_size);
		(void)close(fd);
		return NULL;
	}
	if (st.st_size > 0xFFFFFFFFL) {
		if (err && errsz)
			snprintf(err, errsz, "too large");
		(void)close(fd);
		return NULL;
	}

	uint8_t magic[session_magic_len];
	if (lseek(fd, 0, SEEK_SET) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "seek: %s", strerror(errno));
		(void)close(fd);
		return NULL;
	}
	char rerr[96];
	if (read_exact(fd, magic, sizeof(magic), rerr, sizeof(rerr)) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "%s", rerr);
		(void)close(fd);
		return NULL;
	}
	if (memcmp(magic, session_magic, session_magic_len) != 0) {
		if (err && errsz)
			snprintf(err, errsz, "bad session header");
		(void)close(fd);
		return NULL;
	}

	struct rf_session *s = (struct rf_session *)calloc(1, sizeof(*s));
	if (!s) {
		if (err && errsz)
			snprintf(err, errsz, "out of memory");
		(void)close(fd);
		return NULL;
	}
	snprintf(s->name, sizeof(s->name), "%s", name);
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->fd = fd;
	s->size = (uint32_t)st.st_size;

	s->start_tick = 0;
	s->end_tick = 0;

	struct dev_track devs[RF_MAX_DEVICES];
	memset(devs, 0, sizeof(devs));

	uint32_t off = session_magic_len;
	if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "seek: %s", strerror(errno));
		rf_session_free(s);
		return NULL;
	}

	while (off < s->size) {
		uint8_t hdr[5];
		uint32_t rec_off = off;
		if (read_exact(fd, hdr, sizeof(hdr), rerr, sizeof(rerr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", rerr);
			rf_session_free(s);
			return NULL;
		}
		off += 5;

		uint8_t rec_type = hdr[0];
		uint32_t rec_len = rd_u32_le(hdr + 1);
		if (rec_len == 0)
			continue;
		if (rec_len > max_record_len) {
			if (err && errsz)
				snprintf(err, errsz, "record too large: %lu bytes", (unsigned long)rec_len);
			rf_session_free(s);
			return NULL;
		}
		if (off + rec_len > s->size) {
			if (err && errsz)
				snprintf(err, errsz, "truncated record at %lu", (unsigned long)off);
			rf_session_free(s);
			return NULL;
		}

		if (rec_type != RF_REC_CONFIG && rec_type != RF_REC_SWEEP && rec_type != RF_REC_PACKET) {
			if (lseek(fd, (off_t)rec_len, SEEK_CUR) < 0) {
				if (err && errsz)
					snprintf(err, errsz, "seek: %s", strerror(errno));
				rf_session_free(s);
				return NULL;
			}
			off += rec_len;
			continue;
		}

		if (rec_len > max_payload_buf) {
			if (err && errsz)
				snprintf(err, errsz, "record payload too large: %lu bytes", (unsigned long)rec_len);
			rf_session_free(s);
			return NULL;
		}
		uint8_t payload[max_payload_buf];
		memset(payload, 0, sizeof(payload));
		if (read_exact(fd, payload, rec_len, rerr, sizeof(rerr)) != 0) {
			if (err && errsz)
				snprintf(err, errsz, "%s", rerr);
			rf_session_free(s);
			return NULL;
		}
		off += rec_len;

		switch (rec_type) {
		case RF_REC_CONFIG: {
			if (rec_len < 19)
				break;
			struct rf_session_config_event ev;
			memset(&ev, 0, sizeof(ev));
			ev.tick = rd_u64_le(payload + 0);
			ev.cfg.channel_range_lo = payload[8];
			ev.cfg.channel_range_hi = payload[9];
			ev.selected_channel = payload[10];
			ev.cfg.dwell_time_ms = (int)rd_u16_le(payload + 11);
			ev.cfg.scan_step = payload[13];
			ev.cfg.data_rate = (enum rf_data_rate)payload[14];
			ev.cfg.crc_mode = (enum rf_crc_mode)payload[15];
			ev.cfg.auto_ack = payload[16] != 0;
			ev.cfg.power_level = (enum rf_power_level)payload[17];
			ev.cfg.wf_palette = (enum rf_wf_palette)payload[18];

			if (s->config_count >= max_configs) {
				if (err && errsz)
					snprintf(err, errsz, "too many config records");
				rf_session_free(s);
				return NULL;
			}
			if (ensure_capacity((void **)&s->configs, &s->config_cap, s->config_count + 1, sizeof(s->configs[0]), err,
					    errsz) != 0) {
				rf_session_free(s);
				return NULL;
			}
			s->configs[s->config_count++] = ev;
			note_tick(s, ev.tick);
			break;
		}
		case RF_REC_SWEEP: {
			if (rec_len < 8 + RF_NUM_CHANNELS)
				break;
			uint64_t tick = rd_u64_le(payload + 0);
			if (s->sweep_count >= max_sweeps) {
				if (err && errsz)
					snprintf(err, errsz, "too many sweeps");
				rf_session_free(s);
				return NULL;
			}
			if (ensure_capacity((void **)&s->sweeps, &s->sweep_cap, s->sweep_count + 1, sizeof(s->sweeps[0]), err,
					    errsz) != 0) {
				rf_session_free(s);
				return NULL;
			}
			s->sweeps[s->sweep_count++] = (struct rf_session_sweep_index){.off = rec_off, .tick = tick};
			note_tick(s, tick);
			break;
		}
		case RF_REC_PACKET: {
			struct rf_session_packet_meta m;
			memset(&m, 0, sizeof(m));
			if (rec_len < 8 + 4 + 1 + 1 + 1 + 1 + 1)
				break;
			int poff = 0;
			m.tick = rd_u64_le(payload + poff);
			poff += 8;
			m.seq = rd_u32_le(payload + poff);
			poff += 4;
			m.channel = payload[poff++];
			m.rate = (enum rf_data_rate)payload[poff++];
			m.addr_len = payload[poff++];
			if (m.addr_len > 5)
				m.addr_len = 5;
			if (poff + (int)m.addr_len > (int)rec_len)
				break;
			memcpy(m.addr, payload + poff, m.addr_len);
			poff += (int)m.addr_len;
			if (poff >= (int)rec_len)
				break;
			m.length = payload[poff++];
			if (m.length > 32)
				m.length = 32;
			if (poff + (int)m.length > (int)rec_len)
				break;
			const uint8_t *pl = payload + poff;
			m.payload_hash = rf_fnv1a32(pl, m.length);
			memcpy(m.payload_prefix, pl, (m.length < RF_PAYLOAD_PREFIX_BYTES) ? m.length : RF_PAYLOAD_PREFIX_BYTES);
			poff += (int)m.length;
			if (poff >= (int)rec_len)
				break;
			m.crc_len = payload[poff++];
			if (m.crc_len > 2)
				m.crc_len = 2;
			if (poff + (int)m.crc_len > (int)rec_len)
				break;
			poff += (int)m.crc_len;
			if (poff >= (int)rec_len)
				break;
			m.crc_ok = payload[poff] != 0;
			m.off = rec_off;
			derive_packet_meta(devs, &m);

			if (s->packet_count >= max_packets) {
				if (err && errsz)
					snprintf(err, errsz, "too many packets");
				rf_session_free(s);
				return NULL;
			}
			if (ensure_capacity((void **)&s->packets, &s->packet_cap, s->packet_count + 1, sizeof(s->packets[0]),
					    err, errsz) != 0) {
				rf_session_free(s);
				return NULL;
			}
			s->packets[s->packet_count++] = m;
			note_tick(s, m.tick);
			break;
		}
		default:
			break;
		}
	}

	if (s->sweep_count == 0 && s->packet_count == 0) {
		if (err && errsz)
			snprintf(err, errsz, "empty session");
		rf_session_free(s);
		return NULL;
	}

	return s;
}

void rf_session_free(struct rf_session *s)
{
	if (!s)
		return;
	if (s->fd >= 0) {
		(void)close(s->fd);
		s->fd = -1;
	}
	free(s->sweeps);
	s->sweeps = NULL;
	s->sweep_count = 0;
	s->sweep_cap = 0;
	free(s->packets);
	s->packets = NULL;
	s->packet_count = 0;
	s->packet_cap = 0;
	free(s->configs);
	s->configs = NULL;
	s->config_count = 0;
	s->config_cap = 0;
	free(s);
}

static int read_at(const struct rf_session *s, uint32_t off, void *buf, size_t len, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!s || s->fd < 0 || (!buf && len))
		return -1;
	if (off > s->size || len > s->size - off) {
		if (err && errsz)
			snprintf(err, errsz, "read past EOF");
		return -1;
	}
	if (lseek(s->fd, (off_t)off, SEEK_SET) < 0) {
		if (err && errsz)
			snprintf(err, errsz, "seek: %s", strerror(errno));
		return -1;
	}
	return read_exact(s->fd, buf, len, err, errsz);
}

int rf_session_read_sweep(const struct rf_session *s, uint32_t off, uint64_t *tick_out, uint8_t energy_avg[RF_NUM_CHANNELS],
			  char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!s || !tick_out || !energy_avg) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}

	uint8_t hdr[5];
	if (read_at(s, off, hdr, sizeof(hdr), err, errsz) != 0)
		return -1;
	if (hdr[0] != RF_REC_SWEEP) {
		if (err && errsz)
			snprintf(err, errsz, "bad sweep record");
		return -1;
	}
	uint32_t rec_len = rd_u32_le(hdr + 1);
	if (rec_len < 8 + RF_NUM_CHANNELS || rec_len > max_payload_buf) {
		if (err && errsz)
			snprintf(err, errsz, "bad sweep payload");
		return -1;
	}
	uint8_t payload[max_payload_buf];
	if (read_at(s, off + 5, payload, rec_len, err, errsz) != 0)
		return -1;
	*tick_out = rd_u64_le(payload + 0);
	memcpy(energy_avg, payload + 8, RF_NUM_CHANNELS);
	return 0;
}

int rf_session_read_packet(const struct rf_session *s, uint32_t off, struct rf_packet *out, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!s || !out) {
		if (err && errsz)
			snprintf(err, errsz, "bad args");
		return -1;
	}

	uint8_t hdr[5];
	if (read_at(s, off, hdr, sizeof(hdr), err, errsz) != 0)
		return -1;
	if (hdr[0] != RF_REC_PACKET) {
		if (err && errsz)
			snprintf(err, errsz, "bad packet record");
		return -1;
	}
	uint32_t rec_len = rd_u32_le(hdr + 1);
	if (rec_len == 0 || rec_len > max_payload_buf) {
		if (err && errsz)
			snprintf(err, errsz, "bad packet payload");
		return -1;
	}
	uint8_t payload[max_payload_buf];
	if (read_at(s, off + 5, payload, rec_len, err, errsz) != 0)
		return -1;

	memset(out, 0, sizeof(*out));
	int poff = 0;
	if (rec_len < 8 + 4 + 1 + 1 + 1 + 1 + 1)
		return -1;
	out->tick = rd_u64_le(payload + poff);
	poff += 8;
	out->seq = rd_u32_le(payload + poff);
	poff += 4;
	out->channel = payload[poff++];
	out->rate = (enum rf_data_rate)payload[poff++];
	out->addr_len = payload[poff++];
	if (out->addr_len > 5)
		out->addr_len = 5;
	if (poff + (int)out->addr_len > (int)rec_len)
		return -1;
	memcpy(out->addr, payload + poff, out->addr_len);
	poff += (int)out->addr_len;
	if (poff >= (int)rec_len)
		return -1;
	out->length = payload[poff++];
	if (out->length > 32)
		out->length = 32;
	if (poff + (int)out->length > (int)rec_len)
		return -1;
	memcpy(out->payload, payload + poff, out->length);
	out->payload_hash = rf_fnv1a32(payload + poff, out->length);
	poff += (int)out->length;
	if (poff >= (int)rec_len)
		return -1;
	out->crc_len = payload[poff++];
	if (out->crc_len > 2)
		out->crc_len = 2;
	if (poff + (int)out->crc_len > (int)rec_len)
		return -1;
	memcpy(out->crc, payload + poff, out->crc_len);
	poff += (int)out->crc_len;
	if (poff >= (int)rec_len)
		return -1;
	out->crc_ok = payload[poff] != 0;
	return 0;
}
