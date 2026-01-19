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

/* Get/Set PicoCalc I2C poll config */
#define PICOIOC_GET_POLL_CONFIG 0x0005
#define PICOIOC_SET_POLL_CONFIG 0x0006

/* Set PicoCalc keyboard backlight (percent 0-100) */
#define PICOIOC_SET_KBD_BACKLIGHT 0x0007

/* Power off PicoCalc (seconds >=6 recommended; 0 => default) */
#define PICOIOC_POWEROFF 0x0008

/* Reset PicoCalc keyboard/PMU MCU (delay seconds; 0 => default 1s) */
#define PICOIOC_RESET_KBD 0x0009

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

struct picocalc_poll_config {
	uint32_t kbd_poll_us;
	uint32_t status_poll_us;
	uint32_t backoff_min_us;
	uint32_t backoff_max_us;
	uint32_t fifo_max_per_poll;
	uint32_t reserved[3];
};

#endif
