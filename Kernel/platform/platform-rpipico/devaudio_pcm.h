#ifndef DEVAUDIO_PCM_H
#define DEVAUDIO_PCM_H

#include <kernel.h>

int audio0_open(uint_fast8_t minor, uint16_t flag);
int audio0_close(uint_fast8_t minor);
int audio0_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);

#endif
