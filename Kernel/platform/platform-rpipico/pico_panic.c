#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include <kernel.h>
#include <printf.h>

extern int vsnprintf(char *str, size_t size, const char *format, va_list ap);

/*
 * Pico SDK may call panic() internally (asserts, invalid peripheral usage).
 * The stock implementation executes a BKPT, which becomes a HardFault when no
 * debugger is attached. Override it to print the message on the Fuzix console
 * and then halt, so we can see the real root cause.
 */
void __attribute__((noreturn)) fuzix_pico_panic(const char *fmt, ...)
{
	static char buf[160];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt ? fmt : "(null)", ap);
	va_end(ap);

	kputs("\r\nPICO-SDK panic: ");
	kputs(buf);
	kputs("\r\n");

	for (;;)
		;
}
