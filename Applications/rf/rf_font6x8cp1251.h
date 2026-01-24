#ifndef RF_FONT6X8CP1251_H
#define RF_FONT6X8CP1251_H

#include <stdint.h>

/*
 * Spark font6x8cp1251 stores glyphs for CP1251 bytes 0x20..0xFF.
 * Each glyph is 8 rows; each row stores bits as 0b00xxxxxx where bit5
 * is the leftmost pixel.
 */

uint8_t rf_font6x8_cp1251_row(uint8_t cp1251, uint8_t row);

#endif

