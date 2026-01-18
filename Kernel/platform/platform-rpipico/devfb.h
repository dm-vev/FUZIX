#ifndef DEVFB_H
#define DEVFB_H

#include <kernel.h>

int fb_open(uint_fast8_t minor, uint16_t flags);
int fb_close(uint_fast8_t minor);
int fb_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);
int fb_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag);
int fb_ioctl(uint_fast8_t minor, uarg_t request, char *ptr);

#endif
