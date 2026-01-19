#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/audiopcm.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 8000
#endif

#ifndef MAX_AUDIO_STREAMS
#define MAX_AUDIO_STREAMS 4
#endif

#ifndef AUDIO_MIXER_STREAM_BUFFER_SIZE
#define AUDIO_MIXER_STREAM_BUFFER_SIZE 256
#endif

#ifndef AUDIO_MIXER_CHUNK_SAMPLES
#define AUDIO_MIXER_CHUNK_SAMPLES 128
#endif

struct stream_state {
	uint8_t active;
	uint16_t head;
	uint16_t tail;
	uint8_t buf[AUDIO_MIXER_STREAM_BUFFER_SIZE];
};

static struct stream_state streams[MAX_AUDIO_STREAMS];

static uint16_t stream_count(const struct stream_state *s)
{
	if (s->head >= s->tail)
		return s->head - s->tail;
	return AUDIO_MIXER_STREAM_BUFFER_SIZE - (s->tail - s->head);
}

static uint16_t stream_space(const struct stream_state *s)
{
	return (AUDIO_MIXER_STREAM_BUFFER_SIZE - 1) - stream_count(s);
}

static void stream_reset(struct stream_state *s)
{
	s->head = 0;
	s->tail = 0;
}

static void stream_write(struct stream_state *s, const uint8_t *data, uint16_t len)
{
	uint16_t head = s->head;
	uint16_t first = len;

	if (first > (uint16_t)(AUDIO_MIXER_STREAM_BUFFER_SIZE - head))
		first = AUDIO_MIXER_STREAM_BUFFER_SIZE - head;
	memcpy(s->buf + head, data, first);
	head += first;
	if (head == AUDIO_MIXER_STREAM_BUFFER_SIZE)
		head = 0;
	if (len != first) {
		memcpy(s->buf, data + first, len - first);
		head = len - first;
	}
	s->head = head;
}

static uint8_t stream_read1(struct stream_state *s)
{
	uint8_t v;

	if (!stream_count(s))
		return 128;
	v = s->buf[s->tail];
	s->tail++;
	if (s->tail == AUDIO_MIXER_STREAM_BUFFER_SIZE)
		s->tail = 0;
	return v;
}

static int any_stream_has_data(void)
{
	uint_fast8_t i;

	for (i = 0; i < MAX_AUDIO_STREAMS; i++) {
		if (streams[i].active && stream_count(&streams[i]))
			return 1;
	}
	return 0;
}

static void mix(uint8_t *out, uint16_t n)
{
	uint16_t i;
	uint_fast8_t s;

	for (i = 0; i < n; i++) {
		int sum = 0;
		for (s = 0; s < MAX_AUDIO_STREAMS; s++) {
			uint8_t in;
			if (!streams[s].active)
				continue;
			in = stream_read1(&streams[s]);
			sum += (int)in - 128;
		}
		sum += 128;
		if (sum < 0)
			sum = 0;
		else if (sum > 255)
			sum = 255;
		out[i] = (uint8_t)sum;
	}
}

int main(int argc, char **argv)
{
	int fd_in;
	int fd_out;
	uint8_t msgbuf[sizeof(struct audiopcm_msg) + 128];
	uint8_t outbuf[AUDIO_MIXER_CHUNK_SAMPLES];

	(void)argc;
	(void)argv;

	fd_in = open("/dev/audio", O_RDONLY | O_NDELAY);
	if (fd_in < 0)
		return 1;

	fd_out = open("/dev/audio0", O_WRONLY);
	if (fd_out < 0)
		return 1;

	for (;;) {
		int did_work = 0;
		int i;

		for (i = 0; i < 16; i++) {
			ssize_t n = read(fd_in, msgbuf, sizeof(msgbuf));
			struct audiopcm_msg *m = (struct audiopcm_msg *)msgbuf;
			uint16_t len;
			uint16_t max;

			if (n < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN)
					break;
				return 1;
			}
			if (n == 0)
				break;
			if ((size_t)n < sizeof(*m))
				continue;
			if (m->stream >= MAX_AUDIO_STREAMS)
				continue;

			max = (uint16_t)(n - sizeof(*m));
			len = m->len;
			if (len > max)
				len = max;

			switch (m->type) {
			case AUDIOPCM_EVT_OPEN:
				streams[m->stream].active = 1;
				stream_reset(&streams[m->stream]);
				break;
			case AUDIOPCM_EVT_CLOSE:
				streams[m->stream].active = 0;
				stream_reset(&streams[m->stream]);
				break;
			case AUDIOPCM_EVT_DATA:
			{
				struct stream_state *s = &streams[m->stream];
				uint16_t space = stream_space(s);

				if (!s->active)
					s->active = 1;
				if (len > space)
					len = space;
				if (len)
					stream_write(s, m->data, len);
				break;
			}
			default:
				break;
			}
			did_work = 1;
		}

		if (any_stream_has_data()) {
			ssize_t w;

			mix(outbuf, sizeof(outbuf));
			do {
				w = write(fd_out, outbuf, sizeof(outbuf));
			} while (w < 0 && errno == EINTR);
			if (w < 0 && errno != EAGAIN)
				return 1;
			did_work = 1;
		}

		if (!did_work)
			usleep(5000);
	}
}

