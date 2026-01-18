#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#define DEVRD_PRIVATE
#include <devrd.h>

#include "config.h"

#define MANGLED 0
#include "mangle.h"
#include "psram_spi.h"
#define MANGLED 1
#include "mangle.h"

uint32_t rd_src_address;
uint16_t rd_dst_address;
bool rd_dst_userspace;
uint16_t rd_cpy_count;
uint8_t rd_reverse;

psram_spi_inst_t psram_spi;
bool psram_ready;

void psram_dev_init(void)
{
	if (psram_ready)
		return;
	psram_spi = psram_spi_init_clkdiv(pio1, -1, 1.0f, true);
	psram_ready = true;
}

void psram_bus_write(uint32_t psram_addr, const uint8_t *src, uint16_t count)
{
	while (count) {
		uint16_t chunk = count > 256 ? 256 : count;
		psram_write(&psram_spi, psram_addr, src, chunk);
		psram_addr += chunk;
		src += chunk;
		count -= chunk;
	}
}

void psram_bus_read(uint32_t psram_addr, uint8_t *dst, uint16_t count)
{
	while (count) {
		uint16_t chunk = count > 256 ? 256 : count;
		psram_read(&psram_spi, psram_addr, dst, chunk);
		psram_addr += chunk;
		dst += chunk;
		count -= chunk;
	}
}

void rd_plt_copy(void)
{
	uint8_t scratch[256];
	uint32_t psram_addr = rd_src_address;
	uint8_t *dst = (uint8_t *)(uintptr_t)rd_dst_address;
	uint16_t remaining = rd_cpy_count;

	if (!psram_ready)
		psram_dev_init();

	if (rd_reverse) {
		/* write: user/kernel buffer -> psram */
		while (remaining) {
			uint16_t chunk = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
			if (rd_dst_userspace) {
				uget(dst, scratch, chunk);
				psram_bus_write(psram_addr, scratch, chunk);
			} else {
				psram_bus_write(psram_addr, dst, chunk);
			}
			psram_addr += chunk;
			dst += chunk;
			remaining -= chunk;
		}
	} else {
		/* read: psram -> user/kernel buffer */
		while (remaining) {
			uint16_t chunk = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
			if (rd_dst_userspace) {
				psram_bus_read(psram_addr, scratch, chunk);
				uput(scratch, dst, chunk);
			} else {
				psram_bus_read(psram_addr, dst, chunk);
			}
			psram_addr += chunk;
			dst += chunk;
			remaining -= chunk;
		}
	}
}

int rd_read(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(flag);
	rd_reverse = 0;
	rd_cpy_count = rawflag ? udata.u_count : BLKSIZE;
	return rd_transfer(minor, rawflag, 0);
}

int rd_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	used(flag);
	rd_reverse = 1;
	rd_cpy_count = rawflag ? udata.u_count : BLKSIZE;
	return rd_transfer(minor, rawflag, 0);
}
