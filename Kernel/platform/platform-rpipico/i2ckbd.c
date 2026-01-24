#include <kernel.h>
#include "i2ckbd.h"
#include "pico_ioctl.h"

static uint8_t i2c_inited = 0;
static spin_lock_t *i2c_kbd_lock;

#define PICOCALC_KBD_RING_SIZE 256
#define PICOCALC_KBD_RING_MASK (PICOCALC_KBD_RING_SIZE - 1)

#if (PICOCALC_KBD_RING_SIZE & PICOCALC_KBD_RING_MASK) != 0
#error "PICOCALC_KBD_RING_SIZE must be power of two"
#endif

static uint8_t kbd_ring[PICOCALC_KBD_RING_SIZE];
static volatile uint32_t kbd_head;
static volatile uint32_t kbd_tail;

static void kbd_ring_put(uint8_t c)
{
    uint32_t head = kbd_head;
    uint32_t next = (head + 1) & PICOCALC_KBD_RING_MASK;
    if (next == kbd_tail) {
        /* Drop the oldest byte to keep up with fast typing bursts. */
        kbd_tail = (kbd_tail + 1) & PICOCALC_KBD_RING_MASK;
    }
    kbd_ring[head] = c;
    __dmb();
    kbd_head = next;
}

static void kbd_ring_put_seq(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
        kbd_ring_put(bytes[i]);
}

static void kbd_ring_put_esc_csi(uint8_t final)
{
    const uint8_t seq[3] = { 0x1B, '[', final };
    kbd_ring_put_seq(seq, sizeof(seq));
}

static void kbd_ring_put_esc_ss3(uint8_t final)
{
    const uint8_t seq[3] = { 0x1B, 'O', final };
    kbd_ring_put_seq(seq, sizeof(seq));
}

static int kbd_ring_get(void)
{
    uint32_t tail = kbd_tail;
    if (tail == kbd_head)
        return -1;
    uint8_t c = kbd_ring[tail];
    __dmb();
    kbd_tail = (tail + 1) & PICOCALC_KBD_RING_MASK;
    return (int)c;
}

static struct picocalc_status status_cache = {
    .battery_percent = 0xFF,
    .battery_flags = 0,
    .lcd_backlight = 0xFF,
    .kbd_backlight = 0xFF,
    .fw_version = 0xFF,
    .reserved = { 0 },
};

static struct picocalc_i2c_stats i2c_stats;
static uint32_t i2c_backoff = PICOCALC_I2C_BACKOFF_MIN_US;
static struct picocalc_poll_config poll_cfg;

static uint8_t status_phase;
static int ctrlheld;
static uint32_t i2c_consecutive_errors;
static uint32_t i2c_last_recover_ms;

static void i2c_kbd_hw_init_locked(void)
{
    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);
    i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);
    gpio_pull_up(I2C_KBD_SCL);
    gpio_pull_up(I2C_KBD_SDA);
}

static void i2c_kbd_bus_recover_locked(void)
{
    /*
     * Recover a wedged I2C bus (eg SDA held low). This can happen after long
     * idle periods if the PicoCalc keyboard/PMU MCU goes to sleep and the bus
     * is left in a bad state.
     *
     * Strategy:
     * - deinit I2C peripheral
     * - bit-bang SCL pulses to release the slave
     * - re-init the peripheral
     *
     * Must be called with i2c_kbd_lock held.
     */
    i2c_deinit(I2C_KBD_MOD);

    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_SIO);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_SIO);
    gpio_set_pulls(I2C_KBD_SCL, true, false);
    gpio_set_pulls(I2C_KBD_SDA, true, false);

    gpio_set_dir(I2C_KBD_SCL, GPIO_OUT);
    gpio_set_dir(I2C_KBD_SDA, GPIO_IN);
    gpio_put(I2C_KBD_SCL, 1);
    sleep_us(5);

    for (int i = 0; i < 9; i++) {
        gpio_put(I2C_KBD_SCL, 0);
        sleep_us(5);
        gpio_put(I2C_KBD_SCL, 1);
        sleep_us(5);
    }

    /* Attempt to generate a STOP (drive SDA low then release while SCL high). */
    gpio_set_dir(I2C_KBD_SDA, GPIO_OUT);
    gpio_put(I2C_KBD_SDA, 0);
    sleep_us(5);
    gpio_put(I2C_KBD_SCL, 1);
    sleep_us(5);
    gpio_set_dir(I2C_KBD_SDA, GPIO_IN);
    sleep_us(5);

    i2c_kbd_hw_init_locked();
}

/*
 * PicoCalc keyboard FIFO (see upstream ClockworkPi PicoCalc code):
 * - High byte: key code (see Code/picocalc_keyboard/keyboard.h)
 * - Low byte: key_state (pressed/hold/released)
 *
 * Translate non-ASCII keys into VT100-ish escape sequences so curses and
 * full-screen apps work.
 *
 * Keep viewback scrolling available via Ctrl+Up / Ctrl+Down (handled by LCD
 * TTY driver) instead of eating plain Up/Down.
 */
enum {
    KEY_STATE_PRESSED = 1,
    KEY_STATE_HOLD = 2,
    KEY_STATE_RELEASED = 3,

    KEY_MOD_ALT = 0xA1,
    KEY_MOD_SHL = 0xA2,
    KEY_MOD_SHR = 0xA3,
    KEY_MOD_SYM = 0xA4,
    KEY_MOD_CTRL = 0xA5,

    KEY_ESC = 0xB1,
    KEY_LEFT = 0xB4,
    KEY_UP = 0xB5,
    KEY_DOWN = 0xB6,
    KEY_RIGHT = 0xB7,

    KEY_F1 = 0x81,
    KEY_F2 = 0x82,
    KEY_F3 = 0x83,
    KEY_F4 = 0x84,
    KEY_F5 = 0x85,
    KEY_F6 = 0x86,
    KEY_F7 = 0x87,
    KEY_F8 = 0x88,
    KEY_F9 = 0x89,
    KEY_F10 = 0x90,

    /* Internal: consumed by lcd_getc() for viewback scrolling. */
    PICOCALC_KEY_VIEW_UP = 0xF0,
    PICOCALC_KEY_VIEW_DOWN = 0xF1,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(time_us_64() / 1000u);
}

static void i2c_record_ok(void)
{
    i2c_backoff = poll_cfg.backoff_min_us ? poll_cfg.backoff_min_us : PICOCALC_I2C_BACKOFF_MIN_US;
    i2c_stats.backoff_us = i2c_backoff;
    i2c_stats.last_ok_ms = now_ms();
    i2c_consecutive_errors = 0;
}

static void i2c_record_error(void)
{
    i2c_consecutive_errors++;
    uint32_t next = i2c_backoff * 2u;
    uint32_t min_us = poll_cfg.backoff_min_us ? poll_cfg.backoff_min_us : PICOCALC_I2C_BACKOFF_MIN_US;
    uint32_t max_us = poll_cfg.backoff_max_us ? poll_cfg.backoff_max_us : PICOCALC_I2C_BACKOFF_MAX_US;
    if (max_us < min_us)
        max_us = min_us;
    if (next < min_us)
        next = min_us;
    if (next > max_us)
        next = max_us;
    i2c_backoff = next;
    i2c_stats.backoff_us = i2c_backoff;
    i2c_stats.last_err_ms = now_ms();

    /*
     * If we keep failing, attempt a bus recovery. Without this, the keyboard
     * can get stuck permanently after the first error, especially after long
     * idle periods.
     */
    if (i2c_consecutive_errors >= 8) {
        uint32_t now = i2c_stats.last_err_ms;
        if ((uint32_t)(now - i2c_last_recover_ms) >= 1000) {
            i2c_last_recover_ms = now;
            i2c_kbd_bus_recover_locked();
            i2c_backoff = min_us;
            i2c_stats.backoff_us = i2c_backoff;
            i2c_consecutive_errors = 0;
        }
    }
}

void init_i2c_kbd(){
    i2c_kbd_hw_init_locked();

    int spin_id = spin_lock_claim_unused(true);
    i2c_kbd_lock = spin_lock_init(spin_id);
    kbd_head = 0;
    kbd_tail = 0;
    status_phase = 0;
    ctrlheld = 0;
    memset(&i2c_stats, 0, sizeof(i2c_stats));
    i2c_backoff = PICOCALC_I2C_BACKOFF_MIN_US;
    i2c_stats.backoff_us = i2c_backoff;
    i2c_consecutive_errors = 0;
    i2c_last_recover_ms = 0;
    memset(&poll_cfg, 0, sizeof(poll_cfg));
    poll_cfg.kbd_poll_us = PICOCALC_KBD_POLL_US;
    poll_cfg.status_poll_us = PICOCALC_STATUS_POLL_US;
    poll_cfg.backoff_min_us = PICOCALC_I2C_BACKOFF_MIN_US;
    poll_cfg.backoff_max_us = PICOCALC_I2C_BACKOFF_MAX_US;
    poll_cfg.fifo_max_per_poll = 16;
    i2c_inited = 1;
}

static int i2c_kbd_read_fifo_locked(uint16_t *out)
{
    int retval;
    uint8_t reg = 0x09;
    uint8_t resp[2] = {0};

    i2c_stats.fifo_reads++;
    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, &reg, 1, false, I2C_KBD_TIMEOUT_US);
    if (retval != 1) {
        i2c_stats.fifo_errors++;
        i2c_record_error();
        return -1;
    }
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, resp, sizeof(resp), false, I2C_KBD_TIMEOUT_US);
    if (retval != (int)sizeof(resp)) {
        i2c_stats.fifo_errors++;
        i2c_record_error();
        return -1;
    }
    *out = (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);
    i2c_record_ok();
    return 0;
}

static int i2c_kbd_read_fifo(uint16_t *out)
{
    int rc;
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    rc = i2c_kbd_read_fifo_locked(out);
    spin_unlock(i2c_kbd_lock, spin);
    return rc;
}

static int i2c_kbd_read_reg_u8(uint8_t reg, uint8_t *out)
{
    int retval;
    uint8_t resp[2] = {0};

    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    i2c_stats.reg_reads++;
    retval = i2c_write_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, &reg, 1, false, I2C_KBD_TIMEOUT_US);
    if (retval != 1) {
        i2c_stats.reg_errors++;
        i2c_record_error();
        spin_unlock(i2c_kbd_lock, spin);
        return -1;
    }
    retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, resp, sizeof(resp), false, I2C_KBD_TIMEOUT_US);
    spin_unlock(i2c_kbd_lock, spin);
    if (retval != (int)sizeof(resp)) {
        uint32_t spin2 = spin_lock_blocking(i2c_kbd_lock);
        i2c_stats.reg_errors++;
        i2c_record_error();
        spin_unlock(i2c_kbd_lock, spin2);
        return -1;
    }
    if (resp[0] != reg) {
        uint32_t spin2 = spin_lock_blocking(i2c_kbd_lock);
        i2c_stats.reg_errors++;
        i2c_record_error();
        spin_unlock(i2c_kbd_lock, spin2);
        return -1;
    }
    *out = resp[1];
    uint32_t spin3 = spin_lock_blocking(i2c_kbd_lock);
    i2c_record_ok();
    spin_unlock(i2c_kbd_lock, spin3);
    return 0;
}

void picocalc_status_poll_once(void)
{
    uint8_t val;

    switch (status_phase) {
    case 0:
        if (i2c_kbd_read_reg_u8(0x0B, &val) == 0) {
            uint8_t flags = 0;
            if (val & 0x80)
                flags |= PICOCALC_BATF_CHARGING;
            uint8_t pcnt = val & 0x7F;
            if (pcnt > 100)
                pcnt = 100;
            uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
            status_cache.battery_percent = pcnt;
            status_cache.battery_flags = flags;
            spin_unlock(i2c_kbd_lock, spin);
        }
        break;
    case 1:
        if (i2c_kbd_read_reg_u8(0x01, &val) == 0) {
            uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
            status_cache.fw_version = val;
            spin_unlock(i2c_kbd_lock, spin);
        }
        break;
    case 2:
        if (i2c_kbd_read_reg_u8(0x05, &val) == 0) {
            uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
            status_cache.lcd_backlight = val;
            spin_unlock(i2c_kbd_lock, spin);
        }
        break;
    case 3:
        if (i2c_kbd_read_reg_u8(0x0A, &val) == 0) {
            uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
            status_cache.kbd_backlight = val;
            spin_unlock(i2c_kbd_lock, spin);
        }
        break;
    }

    status_phase = (status_phase + 1) & 3;
}

void picocalc_kbd_poll(void)
{
    if (i2c_inited == 0)
        return;

    uint32_t max_per = 4;

    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    if (poll_cfg.fifo_max_per_poll)
        max_per = poll_cfg.fifo_max_per_poll;
    if (max_per > 32)
        max_per = 32;

    for (uint32_t iter = 0; iter < max_per; iter++) {
        uint16_t buff = 0;
        if (i2c_kbd_read_fifo_locked(&buff) < 0) {
            spin_unlock(i2c_kbd_lock, spin);
            return;
        }
        if (buff == 0) {
            spin_unlock(i2c_kbd_lock, spin);
            return;
        }

        uint8_t state = (uint8_t)(buff & 0xFF);
        uint8_t c = (uint8_t)(buff >> 8);

        /* Track modifier press/release if the firmware reports them. */
        if (c == KEY_MOD_CTRL) {
            if (state == KEY_STATE_PRESSED)
                ctrlheld = 1;
            else if (state == KEY_STATE_RELEASED)
                ctrlheld = 0;
            continue;
        }

        /*
         * Ignore releases for non-modifier keys. For holds, treat as pressed
         * so navigation repeats.
         */
        if (state != KEY_STATE_PRESSED && state != KEY_STATE_HOLD)
            continue;

        switch (c) {
        case KEY_ESC:
            kbd_ring_put(0x1B);
            continue;
        case KEY_F1:
            kbd_ring_put_esc_ss3('P');
            continue;
        case KEY_F2:
            kbd_ring_put_esc_ss3('Q');
            continue;
        case KEY_F3:
            kbd_ring_put_esc_ss3('R');
            continue;
        case KEY_F4:
            kbd_ring_put_esc_ss3('S');
            continue;
        case KEY_F5: {
            const uint8_t seq[] = { 0x1B, '[', '1', '5', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_F6: {
            const uint8_t seq[] = { 0x1B, '[', '1', '7', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_F7: {
            const uint8_t seq[] = { 0x1B, '[', '1', '8', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_F8: {
            const uint8_t seq[] = { 0x1B, '[', '1', '9', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_F9: {
            const uint8_t seq[] = { 0x1B, '[', '2', '0', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_F10: {
            const uint8_t seq[] = { 0x1B, '[', '2', '1', '~' };
            kbd_ring_put_seq(seq, sizeof(seq));
            continue;
        }
        case KEY_UP:
            if (ctrlheld)
                kbd_ring_put(PICOCALC_KEY_VIEW_UP);
            else
                kbd_ring_put_esc_csi('A');
            continue;
        case KEY_DOWN:
            if (ctrlheld)
                kbd_ring_put(PICOCALC_KEY_VIEW_DOWN);
            else
                kbd_ring_put_esc_csi('B');
            continue;
        case KEY_RIGHT:
            kbd_ring_put_esc_csi('C');
            continue;
        case KEY_LEFT:
            kbd_ring_put_esc_csi('D');
            continue;
        default:
            break;
        }

        /* ASCII and control translation. */
        if (c >= 'a' && c <= 'z' && ctrlheld)
            c = (uint8_t)(c - 'a' + 1);

        kbd_ring_put(c);
    }
    spin_unlock(i2c_kbd_lock, spin);
}

int read_i2c_kbd(void)
{
    int c = kbd_ring_get();
    if (c >= 0)
        return c;
    /* Opportunistic burst-poll on demand to reduce input latency. */
    picocalc_kbd_poll();
    return kbd_ring_get();
}

void picocalc_status_snapshot(struct picocalc_status *out)
{
    if (i2c_inited == 0 || i2c_kbd_lock == NULL) {
        memset(out, 0xFF, sizeof(*out));
        out->battery_flags = 0;
        return;
    }
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    *out = status_cache;
    spin_unlock(i2c_kbd_lock, spin);
}

int picocalc_set_lcd_backlight(uint8_t level)
{
    int r = I2C_Send_RegData(I2C_KBD_ADDR, 0x05, (char)level);
    if (r == 0) {
        uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
        status_cache.lcd_backlight = level;
        spin_unlock(i2c_kbd_lock, spin);
    }
    return r;
}

int picocalc_set_kbd_backlight(uint8_t level)
{
    int r = I2C_Send_RegData(I2C_KBD_ADDR, 0x0A, (char)level);
    if (r == 0) {
        uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
        status_cache.kbd_backlight = level;
        spin_unlock(i2c_kbd_lock, spin);
    }
    return r;
}

int picocalc_poweroff(uint8_t seconds)
{
    if (seconds == 0)
        seconds = 6;
    if (seconds < 6)
        seconds = 6;
    return I2C_Send_RegData(I2C_KBD_ADDR, 0x0E, (char)seconds);
}

int picocalc_reset_kbd(uint8_t seconds)
{
    if (seconds == 0)
        seconds = 1;
    return I2C_Send_RegData(I2C_KBD_ADDR, 0x08, (char)seconds);
}

int I2C_Send_RegData(int i2caddr,int reg,char command){
    int retval;
    unsigned char I2C_Send_Buffer[2];
    I2C_Send_Buffer[0]=reg | 0x80;
    I2C_Send_Buffer[1]=command;
    uint8_t I2C_Sendlen=2;

    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    i2c_stats.writes++;
    retval=i2c_write_timeout_us(I2C_KBD_MOD, (uint8_t)i2caddr, (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,false, I2C_KBD_TIMEOUT_US);
    if (retval != I2C_Sendlen) {
        i2c_stats.write_errors++;
        i2c_record_error();
    } else {
        i2c_record_ok();
    }
    spin_unlock(i2c_kbd_lock, spin);

    if (retval != I2C_Sendlen)
        return -1;
    return 0;
}

uint32_t picocalc_i2c_backoff_us(void)
{
    if (!i2c_inited || i2c_kbd_lock == NULL)
        return PICOCALC_I2C_BACKOFF_MAX_US;
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    uint32_t v = i2c_backoff;
    spin_unlock(i2c_kbd_lock, spin);
    return v;
}

void picocalc_i2c_stats_snapshot(struct picocalc_i2c_stats *out)
{
    if (!i2c_inited || i2c_kbd_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->backoff_us = PICOCALC_I2C_BACKOFF_MAX_US;
        return;
    }
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    *out = i2c_stats;
    spin_unlock(i2c_kbd_lock, spin);
}

void picocalc_poll_config_snapshot(struct picocalc_poll_config *out)
{
    if (!i2c_inited || i2c_kbd_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->kbd_poll_us = PICOCALC_KBD_POLL_US;
        out->status_poll_us = PICOCALC_STATUS_POLL_US;
        out->backoff_min_us = PICOCALC_I2C_BACKOFF_MIN_US;
        out->backoff_max_us = PICOCALC_I2C_BACKOFF_MAX_US;
        out->fifo_max_per_poll = 4;
        return;
    }
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    *out = poll_cfg;
    spin_unlock(i2c_kbd_lock, spin);
}

int picocalc_poll_config_set(const struct picocalc_poll_config *in)
{
    if (!i2c_inited || i2c_kbd_lock == NULL)
        return -1;

    struct picocalc_poll_config cfg = *in;
    if (cfg.kbd_poll_us < 1000)
        return -1;
    if (cfg.status_poll_us < 10000)
        return -1;
    if (cfg.backoff_min_us < 1000)
        return -1;
    if (cfg.backoff_max_us < cfg.backoff_min_us)
        return -1;
    if (cfg.backoff_max_us > 5000000)
        return -1;
    if (cfg.fifo_max_per_poll == 0)
        cfg.fifo_max_per_poll = 4;
    if (cfg.fifo_max_per_poll > 32)
        cfg.fifo_max_per_poll = 32;

    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    poll_cfg = cfg;
    if (i2c_backoff < poll_cfg.backoff_min_us)
        i2c_backoff = poll_cfg.backoff_min_us;
    if (i2c_backoff > poll_cfg.backoff_max_us)
        i2c_backoff = poll_cfg.backoff_max_us;
    i2c_stats.backoff_us = i2c_backoff;
    spin_unlock(i2c_kbd_lock, spin);
    return 0;
}

uint32_t picocalc_kbd_poll_us(void)
{
    if (!i2c_inited || i2c_kbd_lock == NULL)
        return PICOCALC_KBD_POLL_US;
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    uint32_t v = poll_cfg.kbd_poll_us ? poll_cfg.kbd_poll_us : PICOCALC_KBD_POLL_US;
    spin_unlock(i2c_kbd_lock, spin);
    return v;
}

uint32_t picocalc_status_poll_us(void)
{
    if (!i2c_inited || i2c_kbd_lock == NULL)
        return PICOCALC_STATUS_POLL_US;
    uint32_t spin = spin_lock_blocking(i2c_kbd_lock);
    uint32_t v = poll_cfg.status_poll_us ? poll_cfg.status_poll_us : PICOCALC_STATUS_POLL_US;
    spin_unlock(i2c_kbd_lock, spin);
    return v;
}
