#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <exec.h>
#include "picosdk.h"
#include "pico_ioctl.h"
#include "i2ckbd.h"
#include <pico/multicore.h>
#include <pico/bootrom.h>
#include <hardware/watchdog.h>

uint8_t sys_cpu = A_ARM;
uint8_t sys_cpu_feat = AF_CORTEX_M0;
uint8_t need_resched;
uaddr_t ramtop = (uaddr_t) PROGTOP;
uint8_t sys_stubs[sizeof(struct exec)];
uint16_t swap_dev = 0xffff;

/* Unused on this port */

void set_cpu_type(void) {}
void map_init(void) {}
void plt_discard(void) {}
void program_vectors(uint16_t* pageptr) {}

void plt_reboot(void)
{
    multicore_reset_core1();
    watchdog_reboot(0, 0, 0);
}

void plt_monitor(void)
{
    sleep_ms(1); // wait to print any remaining messages
    multicore_reset_core1();
    for(;;) { sleep_until(at_the_end_of_time); }
}

int plt_dev_ioctl(uarg_t request, char *data)
{
    switch (request) {
    case PICOIOC_FLASH:
        reset_usb_boot(0, 0);
        return 0;
    case PICOIOC_GET_STATUS: {
        struct picocalc_status st;
        if (!valaddr_w((unsigned char *)data, sizeof(st)))
            return -1;
        picocalc_status_snapshot(&st);
        if (uput(&st, data, sizeof(st)))
            return -1;
        return 0;
    }
    case PICOIOC_SET_LCD_BACKLIGHT: {
        uint8_t pct;
        if (!valaddr_r((unsigned char *)data, sizeof(pct)))
            return -1;
        if (uget(data, &pct, sizeof(pct)))
            return -1;
        if (pct > 100) {
            udata.u_error = EINVAL;
            return -1;
        }
        uint16_t level = (uint16_t)pct * 255;
        level /= 100;
        if (picocalc_set_lcd_backlight((uint8_t)level) < 0) {
            udata.u_error = EIO;
            return -1;
        }
        return 0;
    }
    case PICOIOC_GET_I2C_STATS: {
        struct picocalc_i2c_stats st;
        if (!valaddr_w((unsigned char *)data, sizeof(st)))
            return -1;
        picocalc_i2c_stats_snapshot(&st);
        if (uput(&st, data, sizeof(st)))
            return -1;
        return 0;
    }
    case PICOIOC_GET_POLL_CONFIG: {
        struct picocalc_poll_config cfg;
        if (!valaddr_w((unsigned char *)data, sizeof(cfg)))
            return -1;
        picocalc_poll_config_snapshot(&cfg);
        if (uput(&cfg, data, sizeof(cfg)))
            return -1;
        return 0;
    }
    case PICOIOC_SET_POLL_CONFIG: {
        struct picocalc_poll_config cfg;
        if (!valaddr_r((unsigned char *)data, sizeof(cfg)))
            return -1;
        if (uget(data, &cfg, sizeof(cfg)))
            return -1;
        if (picocalc_poll_config_set(&cfg) < 0) {
            udata.u_error = EINVAL;
            return -1;
        }
        return 0;
    }
    case PICOIOC_SET_KBD_BACKLIGHT: {
        uint8_t pct;
        if (!valaddr_r((unsigned char *)data, sizeof(pct)))
            return -1;
        if (uget(data, &pct, sizeof(pct)))
            return -1;
        if (pct > 100) {
            udata.u_error = EINVAL;
            return -1;
        }
        uint16_t level = (uint16_t)pct * 255;
        level /= 100;
        if (picocalc_set_kbd_backlight((uint8_t)level) < 0) {
            udata.u_error = EIO;
            return -1;
        }
        return 0;
    }
    case PICOIOC_POWEROFF: {
        uint8_t secs;
        if (!valaddr_r((unsigned char *)data, sizeof(secs)))
            return -1;
        if (uget(data, &secs, sizeof(secs)))
            return -1;
        if (picocalc_poweroff(secs) < 0) {
            udata.u_error = EIO;
            return -1;
        }
        return 0;
    }
    case PICOIOC_RESET_KBD: {
        uint8_t secs;
        if (!valaddr_r((unsigned char *)data, sizeof(secs)))
            return -1;
        if (uget(data, &secs, sizeof(secs)))
            return -1;
        if (picocalc_reset_kbd(secs) < 0) {
            udata.u_error = EIO;
            return -1;
        }
        return 0;
    }
    default:
        udata.u_error = EINVAL;
        return -1;
    }
}

uaddr_t pagemap_base(void)
{
    return PROGBASE;
}

usize_t valaddr(const uint8_t *base, usize_t size, uint_fast8_t is_write)
{
        if (base + size < base)
                size = MAXUSIZE - (usize_t)base + 1;
        if (!base || base < (const uint8_t *)PROGBASE)
                size = 0;
        else if (base + size > (const uint8_t *)(size_t)udata.u_ptab->p_top)
                size = (uint8_t *)(size_t)udata.u_ptab->p_top - base;
        if (size == 0)
                udata.u_error = EFAULT;
        return size;
}

usize_t valaddr_r(const uint8_t *pp, usize_t l)
{
	return valaddr(pp, l, 0);
}

usize_t valaddr_w(const uint8_t *pp, usize_t l)
{
	return valaddr(pp, l, 1);
}

/* vim: sw=4 ts=4 et: */
