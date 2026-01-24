#ifndef RF_UTF8_H
#define RF_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Minimal UTF-8 decoder: returns bytes consumed (0 if incomplete). */
size_t rf_utf8_decode(const uint8_t *s, size_t len, uint32_t *out_rune);

#endif

