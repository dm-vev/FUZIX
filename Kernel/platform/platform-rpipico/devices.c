#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <devsys.h>
#include <blkdev.h>
#include <tty.h>
#include <devtty.h>
#include <devrd.h>
#include <dev/devsd.h>
#include <printf.h>
#include "globals.h"
#include "devfb.h"
#include "devaudio_pcm.h"
#include "devaudio_stream.h"
#include "picosdk.h"
#include <hardware/irq.h>
#include <hardware/structs/timer.h>
#include <pico/multicore.h>
#include "core1.h"

struct devsw dev_tab[] =  /* The device driver switch table */
{
// minor    open         close        read      write           ioctl
// ---------------------------------------------------------------------
  /* 0: /dev/hd - block device interface */
  {  blkdev_open,   no_close,   blkdev_read,    blkdev_write,	blkdev_ioctl},
  /* 1: /dev/fd - Floppy disk block devices */
  {  no_open,	    no_close,	no_rdwr,	no_rdwr,	no_ioctl},
  /* 2: /dev/tty	TTY devices */
  {  tty_open,     tty_close,   tty_read,  tty_write,  tty_ioctl },
  /* 3: /dev/lpr	Printer devices */
  {  no_open,     no_close,   no_rdwr,   no_rdwr,  no_ioctl  },
  /* 4: /dev/mem etc	System devices (one offs) */
  {  no_open,      sys_close,    sys_read, sys_write, sys_ioctl  },
  /* 5: /dev/audio0 - PCM output */
  {  audio0_open,  audio0_close, no_rdwr,  audio0_write, no_ioctl },
  /* 6: /dev/audio - userspace mixer streams */
  {  audmux_open,  audmux_close, audmux_read, audmux_write, no_ioctl },
  {  no_open,      no_close,   no_rdwr,   no_rdwr,  no_ioctl  },
  /* 8: /dev/rd? - PSRAM-backed RAM disk */
  {  rd_open,      no_close,   rd_read,   rd_write, no_ioctl  },
  /* 9: /dev/fb - framebuffer control */
  {  fb_open,      fb_close,   fb_read,   fb_write, fb_ioctl },
  /* 10: /dev/pty? - PTY master devices */
  {  pty_open,     pty_close,  pty_read,  pty_write, pty_ioctl },
  /* 11: /dev/ptty? - PTY slave devices */
  {  ptty_open,    ptty_close, ptty_read, ptty_write, ptty_ioctl },
};

static absolute_time_t now;

bool validdev(uint16_t dev)
{
    /* This is a bit uglier than needed but the right hand side is
       a constant this way */
    if(dev > ((sizeof(dev_tab)/sizeof(struct devsw)) << 8) - 1)
	return false;
    else
        return true;
}

static void timer_tick_cb(unsigned alarm)
{
    irqflags_t irq = di();
    udata.u_ininterrupt = 1;

    absolute_time_t next;
    update_us_since_boot(&next, to_us_since_boot(now) + (1000000 / TICKSPERSEC));

    tty_interrupt();
    timer_interrupt();

    if (hardware_alarm_set_target(0, next))
    {
        update_us_since_boot(&next, time_us_64() + (1000000 / TICKSPERSEC));
        hardware_alarm_set_target(0, next);
    }

    udata.u_ininterrupt = 0;
    irqrestore(irq);
}

#ifdef CONFIG_NET
extern void netdev_init(void);
#endif

void device_init(void)
{
    /* Timer interrup must be initialized before blcok devices.
       set_boot_line uses pause syscall which will not be operational otherwise. */
    hardware_alarm_claim(0);
    update_us_since_boot(&now, time_us_64());
    hardware_alarm_set_callback(0, timer_tick_cb);
    hardware_alarm_force_irq(0);

    /* The flash device is too small to be useful, and a corrupt flash will
     * cause a crash on startup... oddly. */
#ifdef CONFIG_PICO_FLASH
    flash_dev_init();
#endif
#ifdef CONFIG_NET
	netdev_init();
#endif
	psram_dev_init();
    sd_rawinit();
    devsd_init();
}

/* vim: sw=4 ts=4 et: */
