#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "kestrel_abi.h"

/* Kernel message ring.
 *
 * A fixed static ring of struct k_logent (no allocation, no blocking) so
 * that klog_write() is safe to call from IRQ context. Entries survive the
 * console being cleared or taken over by a graphical session, which is
 * what a service-supervising init needs. */

#define KLOG_RING_ENTRIES 256

void klog_init(void);

/* Append one entry. `tag` and `msg` are truncated to fit; NULL is fine. */
void klog_write(int level, const char *tag, const char *msg);

void klog_printf(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* index 0 = oldest retained entry. Returns 0, or -1 past the end. */
int klog_read(uint32_t index, struct k_logent *out);

/* Total entries ever written (not the number retained). */
uint32_t klog_count(void);

/* Number currently retained in the ring (<= KLOG_RING_ENTRIES). */
uint32_t klog_retained(void);

/* Sequence number of the oldest retained entry. */
uint32_t klog_oldest_seq(void);

/* Mirror newly written entries to the console/serial. Off by default. */
void klog_set_console(bool on);

/* --- kprintf mirroring ------------------------------------------------
 * kprintf.c is not owned by this module, so the wiring is a function
 * pointer it calls for every character it emits. klog_hook_kprintf()
 * installs the line-assembling sink; the pointer is NULL until then, so
 * the one-line change in kprintf.c is inert on an unhooked kernel. */
extern void (*klog_kprintf_sink)(char c);

void klog_hook_kprintf(void);
void klog_unhook_kprintf(void);

/* --- formatting helpers ----------------------------------------------
 * Freestanding, allocation-free, always NUL-terminate when size > 0.
 * Return the number of characters written, excluding the terminator. */
int klog_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);

int klog_snprintf(char *buf, unsigned long size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* "debug" / "info" / "warn" / "error" / "?" */
const char *klog_level_name(int level);

/* Renders one entry as a single line (with trailing '\n') into buf.
 * Returns the number of bytes written. */
int klog_format_entry(const struct k_logent *e, char *buf, unsigned long size);
