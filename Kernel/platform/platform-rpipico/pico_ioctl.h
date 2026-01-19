#ifndef PICO_IOCTL_H
#define PICO_IOCTL_H

#include <stdint.h>

/* Reboot PI Pico into flash mode */
#define PICOIOC_FLASH 0x0001

/* Get PicoCalc status (battery/backlight/etc) */
#define PICOIOC_GET_STATUS 0x0002

/* Set PicoCalc LCD backlight (percent 0-100) */
#define PICOIOC_SET_LCD_BACKLIGHT 0x0003

/* Get PicoCalc I2C diagnostics */
#define PICOIOC_GET_I2C_STATS 0x0004

enum {
	PICOCALC_BATF_CHARGING = 0x01,
};

struct picocalc_status {
	uint8_t battery_percent; /* 0-100, or 0xFF if unknown */
	uint8_t battery_flags;   /* PICOCALC_BATF_* */
	uint8_t lcd_backlight;   /* 0-255, or 0xFF if unknown */
	uint8_t kbd_backlight;   /* 0-255, or 0xFF if unknown */
	uint8_t fw_version;      /* raw register value, or 0xFF if unknown */
	uint8_t reserved[3];
};

struct picocalc_i2c_stats {
	uint32_t fifo_reads;
	uint32_t fifo_errors;
	uint32_t reg_reads;
	uint32_t reg_errors;
	uint32_t writes;
	uint32_t write_errors;
	uint32_t backoff_us;
	uint32_t last_ok_ms;
	uint32_t last_err_ms;
};

#endif
