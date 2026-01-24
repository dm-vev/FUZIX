#include "rf_font6x8cp1251.h"

#include <stddef.h>

/* Placeholder: real glyph data is imported in a follow-up commit. */
static const uint8_t rf_font6x8_data[224 * 8] = {0};

uint8_t rf_font6x8_cp1251_row(uint8_t cp1251, uint8_t row)
{
	if (row >= 8)
		return 0;
	if (cp1251 < 0x20)
		cp1251 = 0x20;
	size_t idx = (size_t)(cp1251 - 0x20);
	if (idx >= 224)
		idx = (size_t)('?' - 0x20);
	return rf_font6x8_data[idx * 8 + row];
}

