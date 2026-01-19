#ifndef DEVAUDIO_STREAM_H
#define DEVAUDIO_STREAM_H

#include <kernel.h>

int audmux_openi(struct oft *ofp, uint16_t flag);

int audmux_open(uint_fast8_t minor, uint16_t flag);
int audmux_close(uint_fast8_t minor);
int audmux_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);
int audmux_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);

#endif
