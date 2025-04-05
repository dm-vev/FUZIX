#include <stdio.h>
#include <pico/stdio.h>
#include "i2ckbd.h"

static uint8_t i2c_inited = 0;
static uint8_t keycheck = 0;

void init_i2c_kbd(){
    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);
    i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);
    gpio_pull_up(I2C_KBD_SCL);
    gpio_pull_up(I2C_KBD_SDA);

    i2c_inited = 1;
    keycheck = 0;
}
int write_i2c_kbd(){
    int retval;
    unsigned char msg[2];
    msg[0] = 0x09;

    if(i2c_inited == 0) return -1;

    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, msg, 1, false, 500000);
    if ( retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf( "i2c write error\n");
        return -1;
    }
    return 0;
}

int read_i2c_kbd(){
    int retval;
	static int ctrlheld=0;
    uint16_t buff = 0;
    unsigned char msg[2];
    int c  = -1;
    msg[0] = 0x09;

    if(i2c_inited == 0) return -1;

    if(keycheck == 0){
        retval = write_i2c_kbd();
        keycheck = 1;
        return retval;
    }else {
        retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, (unsigned char *) &buff, 2, false, 500000);
        if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
            printf("i2c read error read\n");
            return -1;
        }
        keycheck = 0;
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

int I2C_Send_RegData(int i2caddr,int reg,char command){
    int retval;
    unsigned char I2C_Send_Buffer[2];
    I2C_Send_Buffer[0]=reg;
    I2C_Send_Buffer[1]=command;
    uint8_t I2C_Sendlen=2;
    uint16_t  I2C_Timeout=1000;

    retval=i2c_write_timeout_us(I2C_KBD_MOD, (uint8_t)i2caddr, (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,false, I2C_Timeout*1000);

    if ( retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        printf( "I2C_Send_RegData write error\n");
        return -1;
    }
    return 0;
}

