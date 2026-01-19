#include <kernel.h>
#include <kdata.h>
#include <string.h>

#include "devaudio_stream.h"

#ifndef CONFIG_AUDIO_PCM
#error "devaudio_stream.c requires CONFIG_AUDIO_PCM"
#endif

#if MAX_AUDIO_STREAMS < 1
#error "MAX_AUDIO_STREAMS must be >= 1"
#endif

#if AUDIO_STREAM_BUFFER_SIZE < 2
#error "AUDIO_STREAM_BUFFER_SIZE must be >= 2"
#endif

enum {
	AUMUX_EVT_OPEN = 1,
	AUMUX_EVT_CLOSE = 2,
	AUMUX_EVT_DATA = 3,
};

struct aumux_hdr {
	uint8_t type;
	uint8_t stream;
	uint16_t len;
};

struct aumux_evt {
	uint8_t type;
	uint8_t stream;
};

struct aumux_stream {
	uint8_t inuse;
	uint16_t pid;
	uint16_t head;
	uint16_t tail;
	uint8_t buf[AUDIO_STREAM_BUFFER_SIZE];
};

static struct aumux_stream streams[MAX_AUDIO_STREAMS];

static uint8_t mixer_open;
static uint16_t mixer_pid;

#define AUMUX_EVTQ_SIZE (MAX_AUDIO_STREAMS * 4)
static struct aumux_evt evtq[AUMUX_EVTQ_SIZE];
static uint8_t evt_head;
static uint8_t evt_tail;

static uint8_t rr_stream;
static uint8_t mixer_wait;

static bool evtq_empty(void)
{
	return evt_head == evt_tail;
}

static uint8_t evtq_next(uint8_t idx)
{
	idx++;
	if (idx == AUMUX_EVTQ_SIZE)
		idx = 0;
	return idx;
}

static bool evtq_full(void)
{
	return evtq_next(evt_head) == evt_tail;
}

static bool evtq_push(uint8_t type, uint8_t stream)
{
	if (evtq_full())
		return false;
	evtq[evt_head].type = type;
	evtq[evt_head].stream = stream;
	evt_head = evtq_next(evt_head);
	return true;
}

static bool evtq_pop(struct aumux_evt *e)
{
	if (evtq_empty())
		return false;
	*e = evtq[evt_tail];
	evt_tail = evtq_next(evt_tail);
	return true;
}

static void evtq_reset(void)
{
	evt_head = 0;
	evt_tail = 0;
}

static uint16_t stream_count(const struct aumux_stream *s)
{
	if (s->head >= s->tail)
		return s->head - s->tail;
	return AUDIO_STREAM_BUFFER_SIZE - (s->tail - s->head);
}

static uint16_t stream_space(const struct aumux_stream *s)
{
	return (AUDIO_STREAM_BUFFER_SIZE - 1) - stream_count(s);
}

static void stream_write(struct aumux_stream *s, const uint8_t *data, uint16_t len)
{
	uint16_t head = s->head;
	uint16_t first = min(len, (uint16_t)(AUDIO_STREAM_BUFFER_SIZE - head));

	memcpy(s->buf + head, data, first);
	head += first;
	if (head == AUDIO_STREAM_BUFFER_SIZE)
		head = 0;
	if (len != first) {
		memcpy(s->buf, data + first, len - first);
		head = len - first;
	}
	s->head = head;
}

static void stream_read(struct aumux_stream *s, uint8_t *dst, uint16_t len)
{
	uint16_t tail = s->tail;
	uint16_t first = min(len, (uint16_t)(AUDIO_STREAM_BUFFER_SIZE - tail));

	memcpy(dst, s->buf + tail, first);
	tail += first;
	if (tail == AUDIO_STREAM_BUFFER_SIZE)
		tail = 0;
	if (len != first) {
		memcpy(dst + first, s->buf, len - first);
		tail = len - first;
	}
	s->tail = tail;
}

int audmux_openi(struct oft *ofp, uint16_t flag)
{
	irqflags_t irq;
	uint_fast8_t m = O_ACCMODE(flag);
	uint_fast8_t i;

	irq = di();
	if (m == O_RDONLY) {
		if (mixer_open) {
			irqrestore(irq);
			udata.u_error = EBUSY;
			return -1;
		}
		mixer_open = 1;
		mixer_pid = udata.u_ptab->p_pid;
		ofp->o_ptr = 0;
		rr_stream = 0;
		evtq_reset();
		for (i = 0; i < MAX_AUDIO_STREAMS; i++) {
			if (streams[i].inuse)
				evtq_push(AUMUX_EVT_OPEN, i);
		}
		irqrestore(irq);
		return 0;
	}
	if (m == O_WRONLY) {
		for (i = 0; i < MAX_AUDIO_STREAMS; i++) {
			if (!streams[i].inuse) {
				streams[i].inuse = 1;
				streams[i].pid = udata.u_ptab->p_pid;
				streams[i].head = 0;
				streams[i].tail = 0;
				ofp->o_ptr = i + 1;
				if (mixer_open)
					evtq_push(AUMUX_EVT_OPEN, i);
				wakeup(&mixer_wait);
				irqrestore(irq);
				return 0;
			}
		}
		irqrestore(irq);
		udata.u_error = EBUSY;
		return -1;
	}
	irqrestore(irq);
	udata.u_error = EINVAL;
	return -1;
}

int audmux_open(uint_fast8_t minor, uint16_t flag)
{
	used(flag);
	if (minor != 0) {
		udata.u_error = ENODEV;
		return -1;
	}
	return 0;
}

int audmux_close(uint_fast8_t minor)
{
	irqflags_t irq;
	uint16_t token = (uint16_t)udata.u_offset;
	uint16_t pid = udata.u_ptab->p_pid;
	uint8_t stream;

	used(minor);
	irq = di();
	if (token == 0) {
		if (mixer_open && mixer_pid == pid) {
			mixer_open = 0;
			mixer_pid = 0;
			evtq_reset();
			wakeup(&mixer_wait);
		}
		irqrestore(irq);
		return 0;
	}
	stream = (uint8_t)(token - 1);
	if (stream < MAX_AUDIO_STREAMS && streams[stream].inuse && streams[stream].pid == pid) {
		streams[stream].inuse = 0;
		streams[stream].pid = 0;
		streams[stream].head = 0;
		streams[stream].tail = 0;
		if (mixer_open)
			evtq_push(AUMUX_EVT_CLOSE, stream);
		wakeup(&mixer_wait);
		wakeup(&streams[stream].tail);
	}
	irqrestore(irq);
	return 0;
}

int audmux_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	uint16_t token = (uint16_t)udata.u_offset;
	uint16_t pid = udata.u_ptab->p_pid;
	uint8_t stream;
	uint16_t written = 0;
	uint8_t scratch[64];

	used(minor);
	used(rawflag);

	if (token == 0) {
		udata.u_error = EBADF;
		return -1;
	}
	if (!mixer_open) {
		udata.u_error = EPIPE;
		return -1;
	}

	stream = (uint8_t)(token - 1);
	if (stream >= MAX_AUDIO_STREAMS || !streams[stream].inuse || streams[stream].pid != pid) {
		udata.u_error = EBADF;
		return -1;
	}

	while (written < udata.u_count) {
		irqflags_t irq;
		struct aumux_stream *s = &streams[stream];
		uint16_t space;
		uint16_t want = udata.u_count - written;
		uint16_t chunk;
		bool was_empty;

		irq = di();
		space = stream_space(s);
		was_empty = (stream_count(s) == 0);
		irqrestore(irq);

		if (space == 0) {
			if (written)
				break;
			if (flag & O_NDELAY) {
				udata.u_error = EAGAIN;
				return -1;
			}
			if (psleep_flags(&s->tail, flag))
				return -1;
			continue;
		}

		chunk = min(want, (uint16_t)sizeof(scratch));
		chunk = min(chunk, space);
		if (uget(udata.u_base, scratch, chunk))
			return -1;

		irq = di();
		if (!s->inuse || s->pid != pid) {
			irqrestore(irq);
			udata.u_error = EBADF;
			return -1;
		}
		stream_write(s, scratch, chunk);
		if (was_empty)
			wakeup(&mixer_wait);
		irqrestore(irq);

		udata.u_base += chunk;
		written += chunk;
	}

	return written;
}

int audmux_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	struct aumux_hdr hdr;
	struct aumux_evt e;
	uint16_t token = (uint16_t)udata.u_offset;
	uint8_t payload[128];
	uint16_t payload_len;
	uint8_t stream;
	uint_fast8_t i;
	irqflags_t irq;

	used(rawflag);

	if (minor != 0) {
		udata.u_error = ENODEV;
		return -1;
	}
	if (token != 0) {
		udata.u_error = EBADF;
		return -1;
	}
	if (udata.u_count < sizeof(hdr)) {
		udata.u_error = EINVAL;
		return -1;
	}

	for (;;) {
		irq = di();
		if (evtq_pop(&e)) {
			irqrestore(irq);
			hdr.type = e.type;
			hdr.stream = e.stream;
			hdr.len = 0;
			if (uput(&hdr, udata.u_base, sizeof(hdr)))
				return -1;
			return sizeof(hdr);
		}

		for (i = 0; i < MAX_AUDIO_STREAMS; i++) {
			stream = rr_stream + i;
			if (stream >= MAX_AUDIO_STREAMS)
				stream -= MAX_AUDIO_STREAMS;
			if (stream_count(&streams[stream])) {
				rr_stream = stream + 1;
				if (rr_stream >= MAX_AUDIO_STREAMS)
					rr_stream = 0;
				goto found_data;
			}
		}

		if (flag & O_NDELAY) {
			irqrestore(irq);
			udata.u_error = EAGAIN;
			return -1;
		}
		irqrestore(irq);
		if (psleep_flags(&mixer_wait, flag))
			return -1;
	}

found_data:
	{
		struct aumux_stream *s = &streams[stream];
		uint16_t avail = stream_count(s);
		uint16_t max_payload = udata.u_count - sizeof(hdr);
		bool was_full = (stream_space(s) == 0);

		payload_len = min(avail, max_payload);
		payload_len = min(payload_len, (uint16_t)sizeof(payload));
		stream_read(s, payload, payload_len);
		if (was_full)
			wakeup(&s->tail);
		irqrestore(irq);
	}

	hdr.type = AUMUX_EVT_DATA;
	hdr.stream = stream;
	hdr.len = payload_len;
	if (uput(&hdr, udata.u_base, sizeof(hdr)))
		return -1;
	if (payload_len && uput(payload, udata.u_base + sizeof(hdr), payload_len))
		return -1;
	return sizeof(hdr) + payload_len;
}

/* vim: sw=4 ts=4 et: */
