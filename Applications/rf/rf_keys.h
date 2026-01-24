#ifndef RF_KEYS_H
#define RF_KEYS_H

#include <stddef.h>
#include <stdint.h>

enum rf_key_kind {
	RF_KEY_RUNE = 0,
	RF_KEY_ENTER,
	RF_KEY_BACKSPACE,
	RF_KEY_TAB,
	RF_KEY_ESC,
	RF_KEY_UP,
	RF_KEY_DOWN,
	RF_KEY_LEFT,
	RF_KEY_RIGHT,
	RF_KEY_DELETE,
	RF_KEY_HOME,
	RF_KEY_END,
	RF_KEY_CTRL,
	RF_KEY_PAGE_UP,
	RF_KEY_PAGE_DOWN,
	RF_KEY_F1,
	RF_KEY_F2,
	RF_KEY_F3,
};

struct rf_key {
	enum rf_key_kind kind;
	uint32_t r;
	uint8_t ctrl;
};

int rf_next_key(const uint8_t *b, size_t len, size_t *consumed, struct rf_key *out);

#endif

