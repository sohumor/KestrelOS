#include "output.h"
#include "console.h"
#include "serial.h"
#include "spinlock.h"

/*
 * console_putc() and serial_putc() each protect their own device state, but
 * that alone permits an asynchronous kernel log to splice itself into a
 * userspace prompt between two characters.  This outer lock makes the VGA
 * console and COM1 behave as one record-oriented output stream.
 */
static spinlock_t output_lock = SPINLOCK_INIT;

uint64_t output_begin(void)
{
    return spin_lock_irqsave(&output_lock);
}

void output_end(uint64_t flags)
{
    spin_unlock_irqrestore(&output_lock, flags);
}

void output_putc_locked(char c)
{
    console_putc(c);
    serial_putc(c);
}

void output_write(const char *buf, unsigned long len)
{
    uint64_t flags = output_begin();

    for (unsigned long i = 0; i < len; i++)
        output_putc_locked(buf[i]);
    output_end(flags);
}
