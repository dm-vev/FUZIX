#ifndef RF_CP1251_H
#define RF_CP1251_H

#include <stdint.h>

/* Convert Unicode rune to CP1251 byte (falls back to '?'). */
uint8_t rf_cp1251_from_rune(uint32_t r);

#endif

