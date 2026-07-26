#include "input.h"
#include "interrupts.h"

#define BUFSZ 512

static volatile uint8_t buf[BUFSZ];
static volatile unsigned head, tail;

void input_push(uint8_t c)
{
    unsigned next = (head + 1) % BUFSZ;
    if (next == tail)
        return;                   /* full: drop */
    buf[head] = c;
    head = next;
}

bool input_available(void)
{
    return head != tail;
}

int input_trygetc(void)
{
    if (head == tail)
        return -1;
    uint8_t c = buf[tail];
    tail = (tail + 1) % BUFSZ;
    return c;
}

int input_getc(void)
{
    for (;;) {
        int c = input_trygetc();
        if (c >= 0)
            return c;
        __asm__ volatile("sti; hlt");
    }
}
