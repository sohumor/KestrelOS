#include "kernel.h"
#include "power.h"
#include "interrupts.h"
#include "serial.h"
#include "io.h"

/* Reboot and shutdown. There is no ACPI implementation here, so we go
 * through the small set of pokes that real chipsets and the emulators we
 * test on actually honour, cheapest and least destructive first. */

#define KBC_STATUS      0x64    /* read: status, write: command */
#define KBC_DATA        0x60
#define KBC_OUT_FULL    0x01    /* a byte is waiting to be read */
#define KBC_IN_FULL     0x02    /* the controller has not consumed our last */
#define KBC_PULSE_RESET 0xFE    /* pulse line 0 low == assert RESET */

#define KBC_SPINS       100000

/* PIC data ports; writing 0xFF masks every line. */
#define PIC1_DATA       0x21
#define PIC2_DATA       0xA1

/* Virtual-machine shutdown registers. On real hardware these are either
 * unclaimed (the write is discarded) or a harmless ACPI PM1a write. */
#define QEMU_NEW_PORT   0x604
#define QEMU_OLD_PORT   0xB004
#define VBOX_PORT       0x4004
#define ACPI_SLP_EN_S5  0x2000
#define VBOX_SHUTDOWN   0x3400

/* 64-bit lidt operand. Limit 0 makes every vector invalid. */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static void kbc_drain(void)
{
    for (uint32_t i = 0; i < KBC_SPINS; i++) {
        uint8_t s = inb(KBC_STATUS);
        if (s & KBC_OUT_FULL) {
            inb(KBC_DATA);           /* discard stale scancode */
            continue;
        }
        if (!(s & KBC_IN_FULL))
            return;                  /* input buffer clear, safe to command */
    }
}

/* Load a zero-length IDT and take an exception: the CPU cannot find a
 * handler, cannot deliver #GP, cannot deliver #DF, and triple-faults into
 * a reset. This is the last resort and it always works. */
__attribute__((noreturn))
static void triple_fault(void)
{
    static const struct idt_ptr null_idtr = { 0, 0 };

    cli();
    __asm__ volatile("lidt %0" : : "m"(null_idtr));
    for (;;)
        __asm__ volatile("int3");
    __builtin_unreachable();
}

void power_reboot(void)
{
    kprintf("power: rebooting\n");

    cli();

    /* 8042 keyboard controller: pulsing output line 0 drives the CPU RESET
     * pin on anything PC-compatible, including QEMU. */
    kbc_drain();
    outb(KBC_STATUS, KBC_PULSE_RESET);

    /* The pulse is not instant; give the board a moment before escalating. */
    for (uint32_t i = 0; i < KBC_SPINS; i++)
        io_wait();

    triple_fault();
}

void power_halt(void)
{
    kprintf("power: system halted, it is now safe to turn off the machine\n");
    serial_write("power: system halted\n");

    cli();

    /* Ask the hypervisor to power off. Harmless on bare metal, and it makes
     * headless QEMU runs exit cleanly instead of spinning a host core. */
    outw(QEMU_NEW_PORT, ACPI_SLP_EN_S5);
    outw(QEMU_OLD_PORT, ACPI_SLP_EN_S5);
    outw(VBOX_PORT, VBOX_SHUTDOWN);

    /* Still alive: mask every IRQ line so nothing can wake us, then park.
     * The cli is repeated inside the loop in case an NMI ever unwinds it. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    hang();
}
