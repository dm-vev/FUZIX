#include <kernel.h>
#include <kdata.h>

#define DEVRD_PRIVATE
#include <devrd.h>

/*
 * Stub backend for /dev/rd when PicoCalc PSRAM is disabled.
 *
 * Devices are not expected to be usable (DEV_RD_* sizes are 0), but we still
 * need symbols for the devsw table and for the generic devrd driver.
 */
uint32_t rd_src_address;
uaddr_t rd_dst_address;
bool rd_dst_userspace;
uint16_t rd_cpy_count;
uint8_t rd_reverse;

void rd_plt_copy(void) {}

int rd_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(minor);
	used(rawflag);
	used(flag);
	udata.u_error = ENODEV;
	return -1;
}

int rd_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(minor);
	used(rawflag);
	used(flag);
	udata.u_error = ENODEV;
	return -1;
}
