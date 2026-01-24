#include "rf_utf8.h"

/*
 * This is intentionally small and allocation-free.
 * It accepts malformed sequences by returning U+FFFD and consuming 1 byte.
 */

size_t rf_utf8_decode(const uint8_t *s, size_t len, uint32_t *out_rune)
{
	if (!out_rune)
		return 0;
	*out_rune = 0;
	if (!s || len == 0)
		return 0;

	uint8_t c0 = s[0];
	if (c0 < 0x80) {
		*out_rune = c0;
		return 1;
	}

	/* 2-byte sequence */
	if ((c0 & 0xE0) == 0xC0) {
		if (len < 2)
			return 0;
		uint8_t c1 = s[1];
		if ((c1 & 0xC0) != 0x80) {
			*out_rune = 0xFFFD;
			return 1;
		}
		uint32_t r = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
		*out_rune = r;
		return 2;
	}

	/* 3-byte sequence */
	if ((c0 & 0xF0) == 0xE0) {
		if (len < 3)
			return 0;
		uint8_t c1 = s[1];
		uint8_t c2 = s[2];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
			*out_rune = 0xFFFD;
			return 1;
		}
		uint32_t r = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) | (uint32_t)(c2 & 0x3F);
		*out_rune = r;
		return 3;
	}

	/* 4-byte sequence */
	if ((c0 & 0xF8) == 0xF0) {
		if (len < 4)
			return 0;
		uint8_t c1 = s[1];
		uint8_t c2 = s[2];
		uint8_t c3 = s[3];
		if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
			*out_rune = 0xFFFD;
			return 1;
		}
		uint32_t r = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(c1 & 0x3F) << 12) |
			     ((uint32_t)(c2 & 0x3F) << 6) | (uint32_t)(c3 & 0x3F);
		*out_rune = r;
		return 4;
	}

	*out_rune = 0xFFFD;
	return 1;
}

