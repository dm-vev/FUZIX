#ifndef _SYS_AUDIOPCM_H
#define _SYS_AUDIOPCM_H

#include <stdint.h>

enum {
	AUDIOPCM_EVT_OPEN = 1,
	AUDIOPCM_EVT_CLOSE = 2,
	AUDIOPCM_EVT_DATA = 3,
};

struct audiopcm_msg {
	uint8_t type;
	uint8_t stream;
	uint16_t len;
	uint8_t data[0];
};

#endif
