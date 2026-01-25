#include "rf_annotations.h"

#include "rf_fs.h"
#include "rf_prompt.h"
#include "rf_recording.h"
#include "rf_session.h"
#include "rf_session_format.h"
#include "rf_task.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	max_live_annotations = 64,
	max_session_annotations = 4096,
};

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

static void sanitize_annotation(struct rf_annotation *a)
{
	if (!a)
		return;

	a->tag[sizeof(a->tag) - 1] = 0;
	a->note[sizeof(a->note) - 1] = 0;

	trim_inplace(a->tag);
	trim_inplace(a->note);

	if (!a->tag[0])
		snprintf(a->tag, sizeof(a->tag), "note");

	if (a->end_tick < a->start_tick)
		a->end_tick = a->start_tick;
}

static void push_live_annotation(struct rf_task *t, struct rf_annotation a)
{
	if (!t)
		return;

	t->annotations[t->annot_head] = a;
	t->annot_head++;
	if (t->annot_head >= max_live_annotations)
		t->annot_head = 0;
	if (t->annot_count < max_live_annotations)
		t->annot_count++;
}

static void session_note_tick(struct rf_session *s, uint64_t tick)
{
	if (!s || tick == 0)
		return;
	if (s->start_tick == 0 || tick < s->start_tick)
		s->start_tick = tick;
	if (tick > s->end_tick)
		s->end_tick = tick;
}

static int ensure_session_annotation_capacity(struct rf_session *s, size_t want, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!s)
		return -1;
	if (want <= s->annotation_cap)
		return 0;

	size_t new_cap = (s->annotation_cap == 0) ? 64 : (s->annotation_cap * 2);
	while (new_cap < want)
		new_cap *= 2;

	struct rf_annotation *np = (struct rf_annotation *)realloc(s->annotations, new_cap * sizeof(s->annotations[0]));
	if (!np) {
		if (err && errsz)
			snprintf(err, errsz, "out of memory");
		return -1;
	}
	s->annotations = np;
	s->annotation_cap = new_cap;
	return 0;
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

static int append_annotation_to_session_file(struct rf_session *s, struct rf_annotation a, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!s || !s->path[0]) {
		if (err && errsz)
			snprintf(err, errsz, "session unavailable");
		return -1;
	}

	const size_t tag_len = rf_clamp_int((int)strlen(a.tag), 0, 32);
	const size_t note_len = rf_clamp_int((int)strlen(a.note), 0, 64);

	const uint32_t payload_len = (uint32_t)(8 + 8 + 1 + tag_len + 1 + note_len);
	const size_t rec_len = (size_t)5 + (size_t)payload_len;

	uint8_t buf[5 + 8 + 8 + 1 + 32 + 1 + 64];
	size_t off = 0;
	buf[off++] = (uint8_t)RF_REC_ANNOTATION;
	put_u32_le(buf + off, payload_len);
	off += 4;
	put_u64_le(buf + off, a.start_tick);
	off += 8;
	put_u64_le(buf + off, a.end_tick);
	off += 8;
	buf[off++] = (uint8_t)tag_len;
	memcpy(buf + off, a.tag, tag_len);
	off += tag_len;
	buf[off++] = (uint8_t)note_len;
	memcpy(buf + off, a.note, note_len);
	off += note_len;

	if (off != rec_len) {
		if (err && errsz)
			snprintf(err, errsz, "internal length mismatch");
		return -1;
	}

	int fd = open(s->path, O_WRONLY | O_APPEND);
	if (fd < 0) {
		if (err && errsz)
			snprintf(err, errsz, "open %s: %s", s->path, strerror(errno));
		return -1;
	}
	int rc = rf_fs_write_all(fd, buf, rec_len, err, errsz);
	(void)close(fd);
	if (rc != 0)
		return -1;

	if (s->size <= 0xFFFFFFFFu - (uint32_t)rec_len)
		s->size += (uint32_t)rec_len;
	else
		s->size = 0xFFFFFFFFu;
	return 0;
}

void rf_annotations_begin(struct rf_task *t, uint64_t start_tick)
{
	if (!t)
		return;
	memset(&t->annot_pending, 0, sizeof(t->annot_pending));
	t->annot_pending.start_tick = start_tick;
	t->annot_pending.end_tick = start_tick;
	snprintf(t->annot_pending.tag, sizeof(t->annot_pending.tag), "%s", t->annot_last_tag);
	trim_inplace(t->annot_pending.tag);
	if (!t->annot_pending.tag[0])
		snprintf(t->annot_pending.tag, sizeof(t->annot_pending.tag), "note");

	rf_prompt_open(t, RF_PROMPT_ANNOT_TAG, "Annotation tag", t->annot_pending.tag);
}

int rf_annotations_add(struct rf_task *t, struct rf_annotation a, char *err, size_t errsz)
{
	if (err && errsz)
		err[0] = 0;
	if (!t) {
		if (err && errsz)
			snprintf(err, errsz, "bad task");
		return -1;
	}

	sanitize_annotation(&a);
	snprintf(t->annot_last_tag, sizeof(t->annot_last_tag), "%s", a.tag);

	if (t->replay_active && t->replay) {
		if (t->replay->annotation_count >= max_session_annotations) {
			if (err && errsz)
				snprintf(err, errsz, "too many annotations");
			snprintf(t->replay_err, sizeof(t->replay_err), "%s", err ? err : "too many annotations");
			return -1;
		}

		if (append_annotation_to_session_file(t->replay, a, err, errsz) != 0) {
			snprintf(t->replay_err, sizeof(t->replay_err), "%s", err ? err : "append failed");
			return -1;
		}
		if (ensure_session_annotation_capacity(t->replay, t->replay->annotation_count + 1, err, errsz) != 0)
			return -1;
		t->replay->annotations[t->replay->annotation_count++] = a;
		session_note_tick(t->replay, a.start_tick);
		session_note_tick(t->replay, a.end_tick);
		rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_STATUS);
		return 0;
	}

	push_live_annotation(t, a);
	if (t->recording)
		rf_recording_record_annotation(t, &a);
	rf_task_invalidate(t, RF_DIRTY_ANALYSIS | RF_DIRTY_STATUS);
	return 0;
}

int rf_annotations_visible(const struct rf_task *t, uint64_t now, struct rf_annotation *out, int limit)
{
	if (!out || limit <= 0)
		return 0;
	if (!t)
		return 0;

	if (t->replay_active && t->replay && t->replay->annotation_count > 0) {
		size_t n = t->replay->annotation_count;
		int last = -1;
		for (size_t i = 0; i < n; i++) {
			if (t->replay->annotations[i].start_tick != 0 && t->replay->annotations[i].start_tick <= now)
				last = (int)i;
		}
		if (last < 0) {
			last = (int)n - 1;
			if (last < 0)
				return 0;
		}
		int start = last - (limit - 1);
		if (start < 0)
			start = 0;
		int out_n = 0;
		for (int i = last; i >= start && out_n < limit; i--)
			out[out_n++] = t->replay->annotations[i];
		return out_n;
	}

	if (t->annot_count <= 0)
		return 0;
	if (limit > t->annot_count)
		limit = t->annot_count;
	if (limit > max_live_annotations)
		limit = max_live_annotations;

	int out_n = 0;
	int idx = t->annot_head - 1;
	if (idx < 0)
		idx = max_live_annotations - 1;
	for (int i = 0; i < limit; i++) {
		out[out_n++] = t->annotations[idx];
		idx--;
		if (idx < 0)
			idx = max_live_annotations - 1;
	}
	return out_n;
}
