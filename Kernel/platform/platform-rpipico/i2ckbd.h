#ifndef I2C_KEYBOARD_H
#define I2C_KEYBOARD_H
#include "picosdk.h"
#include <hardware/i2c.h>

#define I2C_KBD_MOD i2c1
#define I2C_KBD_SDA 6
#define I2C_KBD_SCL 7

#define I2C_KBD_SPEED  400000 // if dual i2c, then the speed of keyboard i2c should be 10khz

#define I2C_KBD_ADDR 0x1F

/*
 * Some PicoCalc firmware revisions may stretch the clock or be slow to ACK
 * after longer idle periods; keep this high enough to avoid permanent "no key"
 * states after inactivity.
 */
#define I2C_KBD_TIMEOUT_US 5000

void init_i2c_kbd();
int read_i2c_kbd();
int I2C_Send_RegData(int i2caddr,int reg,char command);

struct picocalc_status;
void picocalc_status_snapshot(struct picocalc_status *out);
int picocalc_set_lcd_backlight(uint8_t level);
void picocalc_status_poll_once(void);
void picocalc_kbd_poll(void);
struct picocalc_i2c_stats;
void picocalc_i2c_stats_snapshot(struct picocalc_i2c_stats *out);
uint32_t picocalc_i2c_backoff_us(void);
struct picocalc_poll_config;
void picocalc_poll_config_snapshot(struct picocalc_poll_config *out);
int picocalc_poll_config_set(const struct picocalc_poll_config *in);
uint32_t picocalc_kbd_poll_us(void);
uint32_t picocalc_status_poll_us(void);
int picocalc_set_kbd_backlight(uint8_t level);
int picocalc_poweroff(uint8_t seconds);
int picocalc_reset_kbd(uint8_t seconds);

#endif
