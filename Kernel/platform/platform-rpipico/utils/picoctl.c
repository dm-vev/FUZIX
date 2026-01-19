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
    } else {
        usage();
        close(fd);
        exit(1);
    }
    close(fd);
    return 0;
}
