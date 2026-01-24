#include "rf_hash.h"

uint32_t rf_fnv1a32(const uint8_t *data, size_t len)
{
	uint32_t h = 2166136261u;
	if (!data)
		return h;
	for (size_t i = 0; i < len; i++) {
		h ^= (uint32_t)data[i];
		h *= 16777619u;
	}
	return h;
}

