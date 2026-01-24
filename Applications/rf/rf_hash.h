#ifndef RF_HASH_H
#define RF_HASH_H

#include <stddef.h>
#include <stdint.h>

uint32_t rf_fnv1a32(const uint8_t *data, size_t len);

#endif

