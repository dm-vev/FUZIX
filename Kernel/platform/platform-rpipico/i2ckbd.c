#include <kernel.h>
#include "i2ckbd.h"
#include "pico_ioctl.h"

static uint8_t i2c_inited = 0;

static struct picocalc_status status_cache = {
    .battery_percent = 0xFF,
    .battery_flags = 0,
    .lcd_backlight = 0xFF,
    .kbd_backlight = 0xFF,
    .fw_version = 0xFF,
    .reserved = { 0 },
};

static uint16_t status_ticks;
static uint8_t status_phase;

void init_i2c_kbd(){
    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);
    i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);
    gpio_pull_up(I2C_KBD_SCL);
    gpio_pull_up(I2C_KBD_SDA);

    i2c_inited = 1;
}

static int i2c_kbd_read_reg_u8(uint8_t reg, uint8_t *out)
{
    int retval;
    uint8_t resp[2] = {0};

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, &reg, 1, false, I2C_KBD_TIMEOUT_US);
    if (retval != 1)
        return -1;
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, resp, sizeof(resp), false, I2C_KBD_TIMEOUT_US);
    if (retval != (int)sizeof(resp))
        return -1;
    if (resp[0] != reg)
        return -1;
    *out = resp[1];
    return 0;
}

static void status_poll_tick(void)
{
    uint8_t val;

    if (++status_ticks < TICKSPERSEC)
        return;
    status_ticks = 0;

    switch (status_phase) {
    case 0:
        if (i2c_kbd_read_reg_u8(0x0B, &val) == 0) {
            uint8_t flags = 0;
            if (val & 0x80)
                flags |= PICOCALC_BATF_CHARGING;
            uint8_t pcnt = val & 0x7F;
            if (pcnt > 100)
                pcnt = 100;
            status_cache.battery_percent = pcnt;
            status_cache.battery_flags = flags;
        }
        break;
    case 1:
        if (i2c_kbd_read_reg_u8(0x01, &val) == 0)
            status_cache.fw_version = val;
        break;
    case 2:
        if (i2c_kbd_read_reg_u8(0x05, &val) == 0)
            status_cache.lcd_backlight = val;
        break;
    case 3:
        if (i2c_kbd_read_reg_u8(0x0A, &val) == 0)
            status_cache.kbd_backlight = val;
        break;
    }

    status_phase = (status_phase + 1) & 3;
}

int read_i2c_kbd(){
	static int ctrlheld=0;
    int retval;
    uint16_t buff = 0;
    uint8_t reg = 0x09;
    uint8_t resp[2] = {0};
    int c = -1;

    if(i2c_inited == 0) return -1;

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, &reg, 1, false, I2C_KBD_TIMEOUT_US);
    if (retval != 1)
        return -1;
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, resp, sizeof(resp), false, I2C_KBD_TIMEOUT_US);
    if (retval != (int)sizeof(resp))
        return -1;
    buff = (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);

    if (buff == 0) {
        status_poll_tick();
        return -1;
    }
    if(buff!=0) {
        if (buff == 0xA503)ctrlheld = 0;
        else if (buff == 0xA502) {
            ctrlheld = 1;
        }else if((buff & 0xff)==1) {//pressed
            c = buff >> 8;
            int realc = -1;
            switch (c) {
                case 0xA1:
                case 0xA2:
                case 0xA3:
                case 0xA4:
                case 0xA5:
                    realc = -1;//skip shift alt ctrl keys
                    break;
                default:
                    realc = c;
                    break;
            }
            c = realc;
            if(c>='a' && c<='z' && ctrlheld)c=c-'a'+1;
        }
        return c;
    }
    return -1;
}

void picocalc_status_snapshot(struct picocalc_status *out)
{
    irqflags_t irq = di();
    *out = status_cache;
    irqrestore(irq);
}

int picocalc_set_lcd_backlight(uint8_t level)
{
    irqflags_t irq = di();
    int r = I2C_Send_RegData(I2C_KBD_ADDR, 0x05, (char)level);
    if (r == 0)
        status_cache.lcd_backlight = level;
    irqrestore(irq);
    return r;
}

int I2C_Send_RegData(int i2caddr,int reg,char command){
    int retval;
    unsigned char I2C_Send_Buffer[2];
    I2C_Send_Buffer[0]=reg | 0x80;
    I2C_Send_Buffer[1]=command;
    uint8_t I2C_Sendlen=2;

    retval=i2c_write_timeout_us(I2C_KBD_MOD, (uint8_t)i2caddr, (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,false, I2C_KBD_TIMEOUT_US);

    if (retval != I2C_Sendlen)
        return -1;
    return 0;
}
