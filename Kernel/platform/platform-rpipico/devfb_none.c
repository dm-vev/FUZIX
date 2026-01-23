#include <kernel.h>
#include <kdata.h>

int fb_open(uint_fast8_t minor, uint16_t flags)
{
	used(minor);
	used(flags);
	udata.u_error = ENODEV;
	return -1;
}

int fb_close(uint_fast8_t minor)
{
	used(minor);
	return 0;
}

int fb_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(minor);
	used(rawflag);
	used(flag);
	udata.u_error = ENODEV;
	return -1;
}

int fb_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(minor);
	used(rawflag);
	used(flag);
	udata.u_error = ENODEV;
	return -1;
}

int fb_ioctl(uint_fast8_t minor, uarg_t request, char *ptr)
{
	used(minor);
	used(request);
	used(ptr);
	udata.u_error = ENODEV;
	return -1;
}

