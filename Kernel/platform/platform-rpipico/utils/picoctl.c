#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pico_ioctl.h>

static void usage(void)
{
    puts("usage: picoctl [ --help ] <command>");
    puts("Command list:");
    puts("\tflash\tReset into flash mode.");
    puts("\tstatus\tShow PicoCalc status.");
    puts("\tbacklight <0-100>\tSet LCD backlight.");
    puts("\tkbdlight <0-100>\tSet keyboard backlight.");
    puts("\tpollcfg\tShow poll config.");
    puts("\tpollcfg set kbd_us=<n> status_us=<n> backoff_min_us=<n> backoff_max_us=<n> fifo_max=<n>");
    puts("\tpoweroff [seconds]\tPower off (default 6s).");
    puts("\trebootkbd [seconds]\tReset keyboard MCU (default 1s).");
    puts("\ti2cstats\tShow PicoCalc I2C stats.");
}

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "--help") == 0)
    {
        usage();
        return 0;
    }
    int fd = open("/dev/sys", O_RDWR, 0);
    if (fd == -1)
    {
        perror("Failed to open /dev/sys");
        exit(1);
    }
    if (strcmp(argv[1], "flash") == 0) {
        if (ioctl(fd, PICOIOC_FLASH) != 0)
        {
            perror("Failed to perform operation");
            close(fd);
            exit(1);
        }
    } else if (strcmp(argv[1], "status") == 0) {
        struct picocalc_status st;
        if (ioctl(fd, PICOIOC_GET_STATUS, &st) != 0) {
            perror("Failed to read status");
            close(fd);
            exit(1);
        }

        if (st.battery_percent == 0xFF) {
            puts("Battery: unknown");
        } else {
            printf("Battery: %u%%", st.battery_percent);
            if (st.battery_flags & PICOCALC_BATF_CHARGING)
                printf(" (charging)");
            putchar('\n');
        }

        if (st.fw_version != 0xFF) {
            printf("KBD FW: 0x%02X (%u.%u)\n",
                   st.fw_version,
                   st.fw_version >> 4,
                   st.fw_version & 0x0F);
        }

        if (st.lcd_backlight != 0xFF)
            printf("LCD backlight: %u/255\n", st.lcd_backlight);
        if (st.kbd_backlight != 0xFF)
            printf("KBD backlight: %u/255\n", st.kbd_backlight);
    } else if (strcmp(argv[1], "backlight") == 0) {
        if (argc < 3) {
            usage();
            close(fd);
            exit(1);
        }
        long pct = strtol(argv[2], NULL, 10);
        if (pct < 0 || pct > 100) {
            fprintf(stderr, "backlight: expected 0-100\n");
            close(fd);
            exit(1);
        }
        uint8_t v = (uint8_t)pct;
        if (ioctl(fd, PICOIOC_SET_LCD_BACKLIGHT, &v) != 0) {
            perror("Failed to set backlight");
            close(fd);
            exit(1);
        }
    } else if (strcmp(argv[1], "kbdlight") == 0) {
        if (argc < 3) {
            usage();
            close(fd);
            exit(1);
        }
        long pct = strtol(argv[2], NULL, 10);
        if (pct < 0 || pct > 100) {
            fprintf(stderr, "kbdlight: expected 0-100\n");
            close(fd);
            exit(1);
        }
        uint8_t v = (uint8_t)pct;
        if (ioctl(fd, PICOIOC_SET_KBD_BACKLIGHT, &v) != 0) {
            perror("Failed to set kbdlight");
            close(fd);
            exit(1);
        }
    } else if (strcmp(argv[1], "pollcfg") == 0) {
        if (argc >= 3 && strcmp(argv[2], "set") == 0) {
            struct picocalc_poll_config cfg;
            if (ioctl(fd, PICOIOC_GET_POLL_CONFIG, &cfg) != 0) {
                perror("Failed to read pollcfg");
                close(fd);
                exit(1);
            }
            for (int i = 3; i < argc; i++) {
                char *eq = strchr(argv[i], '=');
                if (!eq)
                    continue;
                *eq++ = 0;
                unsigned long v = strtoul(eq, NULL, 10);
                if (strcmp(argv[i], "kbd_us") == 0)
                    cfg.kbd_poll_us = (uint32_t)v;
                else if (strcmp(argv[i], "status_us") == 0)
                    cfg.status_poll_us = (uint32_t)v;
                else if (strcmp(argv[i], "backoff_min_us") == 0)
                    cfg.backoff_min_us = (uint32_t)v;
                else if (strcmp(argv[i], "backoff_max_us") == 0)
                    cfg.backoff_max_us = (uint32_t)v;
                else if (strcmp(argv[i], "fifo_max") == 0)
                    cfg.fifo_max_per_poll = (uint32_t)v;
            }
            if (ioctl(fd, PICOIOC_SET_POLL_CONFIG, &cfg) != 0) {
                perror("Failed to set pollcfg");
                close(fd);
                exit(1);
            }
        } else {
            struct picocalc_poll_config cfg;
            if (ioctl(fd, PICOIOC_GET_POLL_CONFIG, &cfg) != 0) {
                perror("Failed to read pollcfg");
                close(fd);
                exit(1);
            }
            printf("kbd_poll_us=%lu status_poll_us=%lu backoff_min_us=%lu backoff_max_us=%lu fifo_max=%lu\n",
                   (unsigned long)cfg.kbd_poll_us,
                   (unsigned long)cfg.status_poll_us,
                   (unsigned long)cfg.backoff_min_us,
                   (unsigned long)cfg.backoff_max_us,
                   (unsigned long)cfg.fifo_max_per_poll);
        }
    } else if (strcmp(argv[1], "poweroff") == 0) {
        uint8_t secs = 0;
        if (argc >= 3) {
            long v = strtol(argv[2], NULL, 10);
            if (v < 0 || v > 255) {
                fprintf(stderr, "poweroff: bad seconds\n");
                close(fd);
                exit(1);
            }
            secs = (uint8_t)v;
        }
        if (ioctl(fd, PICOIOC_POWEROFF, &secs) != 0) {
            perror("Failed to poweroff");
            close(fd);
            exit(1);
        }
    } else if (strcmp(argv[1], "rebootkbd") == 0) {
        uint8_t secs = 0;
        if (argc >= 3) {
            long v = strtol(argv[2], NULL, 10);
            if (v < 0 || v > 255) {
                fprintf(stderr, "rebootkbd: bad seconds\n");
                close(fd);
                exit(1);
            }
            secs = (uint8_t)v;
        }
        if (ioctl(fd, PICOIOC_RESET_KBD, &secs) != 0) {
            perror("Failed to reset kbd");
            close(fd);
            exit(1);
        }
    } else if (strcmp(argv[1], "i2cstats") == 0) {
        struct picocalc_i2c_stats st;
        if (ioctl(fd, PICOIOC_GET_I2C_STATS, &st) != 0) {
            perror("Failed to read i2c stats");
            close(fd);
            exit(1);
        }
        printf("fifo: reads=%lu errors=%lu\n",
               (unsigned long)st.fifo_reads, (unsigned long)st.fifo_errors);
        printf("reg:  reads=%lu errors=%lu\n",
               (unsigned long)st.reg_reads, (unsigned long)st.reg_errors);
        printf("wr:   writes=%lu errors=%lu\n",
               (unsigned long)st.writes, (unsigned long)st.write_errors);
        printf("backoff_us=%lu last_ok_ms=%lu last_err_ms=%lu\n",
               (unsigned long)st.backoff_us,
               (unsigned long)st.last_ok_ms,
               (unsigned long)st.last_err_ms);
    } else {
        usage();
        close(fd);
        exit(1);
    }
    close(fd);
    return 0;
}
