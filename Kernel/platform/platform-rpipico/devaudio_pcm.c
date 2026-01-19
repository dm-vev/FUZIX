#include <kernel.h>
#include <kdata.h>
#include <string.h>

#include "devaudio_pcm.h"
#include "picosdk.h"

#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/timer.h>

#ifndef CONFIG_AUDIO_PCM
#error "devaudio_pcm.c requires CONFIG_AUDIO_PCM"
#endif

#if AUDIO_OUTPUT_MODE != AUDIO_OUTPUT_MODE_PWM
#error "Only AUDIO_OUTPUT_MODE_PWM is supported on rpipico"
#endif

#if AUDIO_BUFFER_SIZE < 2
#error "AUDIO_BUFFER_SIZE must be >= 2"
#endif

static uint8_t audio0_buf[AUDIO_BUFFER_SIZE];
static volatile uint16_t audio0_head;
static volatile uint16_t audio0_tail;
static volatile uint16_t audio0_count;

static uint16_t audio0_owner_pid;
static uint8_t audio0_wait;

static uint8_t audio0_pwm_init_done;
static uint8_t audio0_alarm_claimed;
static uint8_t audio0_running;

static uint8_t audio0_pwm_slice;

static uint64_t audio0_period_fp;
static uint64_t audio0_next_fp;

static void audio0_pwm_init(void)
{
	uint16_t wrap = 255;

	if (audio0_pwm_init_done)
		return;

	gpio_set_function(AUDIO_PWM_PIN, GPIO_FUNC_PWM);
	audio0_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PWM_PIN);
	pwm_set_wrap(audio0_pwm_slice, wrap);
	pwm_set_clkdiv_int_frac(audio0_pwm_slice, 1, 0);
	pwm_set_gpio_level(AUDIO_PWM_PIN, 128);
	pwm_set_enabled(audio0_pwm_slice, true);

	audio0_pwm_init_done = 1;
}

static void audio0_schedule_next(void)
{
	absolute_time_t next;
	uint64_t next_us;

	audio0_next_fp += audio0_period_fp;
	next_us = audio0_next_fp >> 32;
	update_us_since_boot(&next, next_us);

	if (hardware_alarm_set_target(1, next))
	{
		/* We missed the deadline; resync from current time. */
		audio0_next_fp = (time_us_64() << 32) + audio0_period_fp;
		update_us_since_boot(&next, audio0_next_fp >> 32);
		hardware_alarm_set_target(1, next);
	}
}

static void audio0_tick_cb(unsigned alarm)
{
	irqflags_t irq = di();
	bool was_full = false;
	uint8_t sample = 128;

	used(alarm);

	udata.u_ininterrupt = 1;

	if (audio0_running && audio0_count) {
		was_full = (audio0_count == AUDIO_BUFFER_SIZE);
		sample = audio0_buf[audio0_tail];
		audio0_tail++;
		if (audio0_tail == AUDIO_BUFFER_SIZE)
			audio0_tail = 0;
		audio0_count--;
	}
	pwm_set_gpio_level(AUDIO_PWM_PIN, sample);
	if (was_full)
		wakeup(&audio0_wait);

	if (audio0_running)
		audio0_schedule_next();

	udata.u_ininterrupt = 0;
	irqrestore(irq);
}

static void audio0_start(void)
{
	if (audio0_running)
		return;
	if (!audio0_alarm_claimed) {
		hardware_alarm_claim(1);
		audio0_alarm_claimed = 1;
	}
	audio0_period_fp = (1000000ULL << 32) / AUDIO_SAMPLE_RATE;
	audio0_next_fp = (time_us_64() << 32);
	hardware_alarm_set_callback(1, audio0_tick_cb);
	audio0_running = 1;
	audio0_schedule_next();
}

static void audio0_stop(void)
{
	irqflags_t irq;

	if (!audio0_running)
		return;

	irq = di();
	audio0_running = 0;
	hardware_alarm_cancel(1);
	hardware_alarm_set_callback(1, NULL);
	if (audio0_alarm_claimed) {
		hardware_alarm_unclaim(1);
		audio0_alarm_claimed = 0;
	}
	pwm_set_gpio_level(AUDIO_PWM_PIN, 128);
	irqrestore(irq);
}

int audio0_open(uint_fast8_t minor, uint16_t flag)
{
	irqflags_t irq;

	used(flag);
	if (minor != 0) {
		udata.u_error = ENODEV;
		return -1;
	}

	irq = di();
	if (audio0_owner_pid && audio0_owner_pid != udata.u_ptab->p_pid) {
		irqrestore(irq);
		udata.u_error = EBUSY;
		return -1;
	}
	audio0_owner_pid = udata.u_ptab->p_pid;
	audio0_head = 0;
	audio0_tail = 0;
	audio0_count = 0;
	irqrestore(irq);

	audio0_pwm_init();
	audio0_start();
	return 0;
}

int audio0_close(uint_fast8_t minor)
{
	irqflags_t irq;

	used(minor);

	irq = di();
	if (audio0_owner_pid != udata.u_ptab->p_pid) {
		irqrestore(irq);
		return 0;
	}
	audio0_owner_pid = 0;
	audio0_head = 0;
	audio0_tail = 0;
	audio0_count = 0;
	irqrestore(irq);

	audio0_stop();
	return 0;
}

static void audio0_buf_write(const uint8_t *data, uint16_t len)
{
	uint16_t head = audio0_head;
	uint16_t first = min(len, (uint16_t)(AUDIO_BUFFER_SIZE - head));

	memcpy(audio0_buf + head, data, first);
	head += first;
	if (head == AUDIO_BUFFER_SIZE)
		head = 0;
	if (len != first) {
		memcpy(audio0_buf, data + first, len - first);
		head = len - first;
	}
	audio0_head = head;
	audio0_count += len;
}

int audio0_write(uint_fast8_t minor, uint_fast8_t rawflag, uint_fast8_t flag)
{
	uint16_t written = 0;
	uint8_t scratch[128];

	used(minor);
	used(rawflag);

	if (audio0_owner_pid != udata.u_ptab->p_pid) {
		udata.u_error = EBADF;
		return -1;
	}

	while (written < udata.u_count) {
		irqflags_t irq;
		uint16_t space;
		uint16_t want = udata.u_count - written;
		uint16_t chunk;

		irq = di();
		space = AUDIO_BUFFER_SIZE - audio0_count;
		irqrestore(irq);

		if (space == 0) {
			if (written)
				break;
			if (flag & O_NDELAY) {
				udata.u_error = EAGAIN;
				return -1;
			}
			if (psleep_flags(&audio0_wait, flag))
				return -1;
			continue;
		}

		chunk = min(want, (uint16_t)sizeof(scratch));
		chunk = min(chunk, space);
		if (uget(udata.u_base, scratch, chunk))
			return -1;

		irq = di();
		if (audio0_owner_pid != udata.u_ptab->p_pid) {
			irqrestore(irq);
			udata.u_error = EBADF;
			return -1;
		}
		audio0_buf_write(scratch, chunk);
		irqrestore(irq);

		udata.u_base += chunk;
		written += chunk;
	}
	return written;
}

/* vim: sw=4 ts=4 et: */
