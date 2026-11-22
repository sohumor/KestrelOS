#include "kernel.h"
#include "../kernel/input.c"
#include "initcall.h"
#include "keyboard_seat.h"
#include "timer.h"
#include "usb_hid_core.h"

#define PENDING (-2)

static int reader_result;

static void console_reader(void *arg)
{
    (void)arg;
    __atomic_store_n(&reader_result, input_getc(), __ATOMIC_RELEASE);
}

static bool check_reader(bool cancel)
{
    int pid;
    __atomic_store_n(&reader_result, PENDING, __ATOMIC_RELAXED);
    struct task *reader =
        kthread_create_with_pid(console_reader, NULL, "console-probe", &pid);
    if (!reader)
        return false;

    bool parked = false;
    uint64_t deadline = timer_ticks() + TIMER_HZ;
    do {
        uint64_t flags = spin_lock_irqsave(&input_lock);
        for (int i = 0; input_ready.waiters != NULL; i++) {
            struct k_psinfo info;
            if (task_psinfo(i, &info) != 0)
                break;
            if (info.pid == pid) {
                parked = info.state == K_STATE_SLEEPING;
                break;
            }
        }
        spin_unlock_irqrestore(&input_lock, flags);
        if (!parked)
            task_sleep_ticks(1);
    } while (!parked && timer_ticks() < deadline);

    if (!parked)
        return false;
    if (cancel)
        task_kill(reader);
    else
        input_push('W');

    deadline = timer_ticks() + TIMER_HZ;
    int result;
    while ((result = __atomic_load_n(&reader_result, __ATOMIC_ACQUIRE)) == PENDING &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);
    return result == (cancel ? -1 : 'W');
}

static void seat_key(uint8_t usage, bool down, void *ctx)
{
    keyboard_source_key(ctx, usage, down);
}

static bool check_keyboard_seat(void)
{
    const uint8_t idle[8] = {0}, usb_shift[8] = {2};
    struct keyboard_source ps2 = {0}, usb = {0};
    bool ok = true;

    keyboard_source_key(&ps2, KKEY_LSHIFT, true);
    ok &= usb_hid_keyboard(idle, usb_shift, seat_key, &usb);
    keyboard_source_key(&ps2, KKEY_LSHIFT, false);
    keyboard_source_key(&ps2, KKEY_A, true);
    ok &= input_trygetc() == 'A';
    keyboard_source_key(&ps2, KKEY_RSHIFT, true);
    keyboard_source_clear(&usb);
    keyboard_source_key(&ps2, KKEY_A, true);
    ok &= input_trygetc() == 'A';
    keyboard_source_key(&ps2, KKEY_RSHIFT, false);
    keyboard_source_key(&ps2, KKEY_A, true);
    ok &= input_trygetc() == 'a';

    keyboard_source_key(&usb, KKEY_RALT, true);
    keyboard_source_key(&ps2, KKEY_F4, true);
    ok &= input_trygetc_wm_hotkey() == KEY_WM_CLOSE;
    keyboard_source_clear(&usb);
    keyboard_source_key(&ps2, KKEY_TAB, true);
    ok &= input_trygetc() == '\t';
    return ok;
}

static int console_input_probe(void)
{
    if (!check_keyboard_seat()) {
        kprintf("FAIL keyboard seat\n");
        return -1;
    }
    kprintf("PASS keyboard seat: shared chords, independent modifier release\n");

    input_push('Q');
    if (input_getc() != 'Q' || !check_reader(false) || !check_reader(true)) {
        kprintf("FAIL console input events\n");
        return -1;
    }
    kprintf("PASS console input events: queued input, parked reader wake, and cancellation\n");
    return 0;
}

late_initcall(console_input_probe);
