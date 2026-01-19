#ifndef VEC_KEYS_H
#define VEC_KEYS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	VEC_KEY_RUNE = 0,
	VEC_KEY_ENTER,
	VEC_KEY_BACKSPACE,
	VEC_KEY_TAB,
	VEC_KEY_ESC,
	VEC_KEY_UP,
	VEC_KEY_DOWN,
	VEC_KEY_LEFT,
	VEC_KEY_RIGHT,
	VEC_KEY_DELETE,
	VEC_KEY_HOME,
	VEC_KEY_END,
	VEC_KEY_CTRL,
	VEC_KEY_PGUP,
	VEC_KEY_PGDN,
	VEC_KEY_F1,
	VEC_KEY_F2,
	VEC_KEY_F3,
} vec_key_kind;

typedef struct {
	vec_key_kind kind;
	uint32_t r;
	uint8_t ctrl;
} vec_key;

/* Returns 1 if a key was parsed, 0 if need more bytes. On success *consumed>0. */
int vec_next_key(const uint8_t *b, size_t n, size_t *consumed, vec_key *out);

#endif

