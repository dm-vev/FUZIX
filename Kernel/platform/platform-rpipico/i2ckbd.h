#ifndef I2C_KEYBOARD_H
#define I2C_KEYBOARD_H
#include "picosdk.h"
#include <hardware/i2c.h>

#define I2C_KBD_MOD i2c1
#define I2C_KBD_SDA 6
#define I2C_KBD_SCL 7

#define I2C_KBD_SPEED  400000 // if dual i2c, then the speed of keyboard i2c should be 10khz

#define I2C_KBD_ADDR 0x1F

#define I2C_KBD_TIMEOUT_US 1000

void init_i2c_kbd();
int read_i2c_kbd();
int I2C_Send_RegData(int i2caddr,int reg,char command);

struct picocalc_status;
void picocalc_status_snapshot(struct picocalc_status *out);

#endif
