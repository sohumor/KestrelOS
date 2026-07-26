/* PS/2 auxiliary device (mouse) driver.
 *
 * Shares the 8042 controller with kernel/keyboard.c. The keyboard reads
 * port 0x60 from its IRQ1 handler without looking at the status register,
 * so this driver must never leave a keyboard byte sitting in the output
 * buffer, and must never consume one that IRQ1 is entitled to: the IRQ12
 * handler checks status bit 5 (AUX) and bails out if the pending byte
 * came from the keyboard.
 *
 * The IRQ dispatcher in kernel/idt.c already sends the EOI (to both PICs
 * for IRQ >= 8), so mouse_irq() must NOT call pic_send_eoi() itself.
 * pic_init() leaves the cascade line (IRQ2) unmasked, so unmasking IRQ12
 * on the slave is all that is needed to let the line through.
 */

#include "mouse.h"
#include "kernel.h"
#include "interrupts.h"
#include "io.h"

#define PS2_DATA   0x60
#define PS2_STAT   0x64
#define PS2_CMD    0x64

#define ST_OUTFULL 0x01     /* output buffer holds a byte for us */
#define ST_INFULL  0x02     /* input buffer still busy, do not write */
#define ST_AUX     0x20     /* the pending byte came from the mouse */

#define CTL_READ_CFG   0x20
#define CTL_WRITE_CFG  0x60
#define CTL_AUX_ENABLE 0xA8
#define CTL_TO_AUX     0xD4

#define CFG_AUX_IRQ    0x02 /* enable IRQ12 */
#define CFG_AUX_CLK    0x20 /* set = mouse clock disabled */

#define MOUSE_ACK      0xFA

#define EVQ_SIZE       64

/* Bounded spins: the 8042 is slow but not that slow. */
#define IO_SPINS       100000

struct mouse_state {
    volatile int32_t  x, y;
    volatile uint32_t buttons;
    int  bound_w, bound_h;
    bool present;
    int  packet_size;       /* 3, or 4 with a working scroll wheel */
    int  index;
    uint8_t pkt[4];
};

static struct mouse_state ms;

static struct k_event evq[EVQ_SIZE];
static volatile unsigned ev_head, ev_tail;

/* ---------------------------------------------------------------- 8042 */

static int wait_write(void)
{
    for (int i = 0; i < IO_SPINS; i++) {
        if (!(inb(PS2_STAT) & ST_INFULL))
            return 0;
        io_wait();
    }
    return -1;
}

static int wait_read(void)
{
    for (int i = 0; i < IO_SPINS; i++) {
        if (inb(PS2_STAT) & ST_OUTFULL)
            return 0;
        io_wait();
    }
    return -1;
}

/* Read a byte that came from the auxiliary device, discarding any keyboard
 * bytes that turn up first. Only used during init, with IRQs off. */
static int aux_read(void)
{
    for (int tries = 0; tries < 16; tries++) {
        if (wait_read() < 0)
            return -1;
        uint8_t st = inb(PS2_STAT);
        uint8_t data = inb(PS2_DATA);
        if (st & ST_AUX)
            return data;
        /* keyboard byte arrived while we were probing: drop it */
    }
    return -1;
}

static int ctl_write(uint8_t cmd)
{
    if (wait_write() < 0)
        return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

/* Send a byte to the mouse and collect its 0xFA acknowledgement. */
static int aux_command(uint8_t byte)
{
    if (ctl_write(CTL_TO_AUX) < 0)
        return -1;
    if (wait_write() < 0)
        return -1;
    outb(PS2_DATA, byte);

    int ack = aux_read();
    if (ack == 0xFE)                 /* resend requested: try once more */
        return -1;
    return ack == MOUSE_ACK ? 0 : -1;
}

static int aux_set_rate(uint8_t rate)
{
    if (aux_command(0xF3) < 0)
        return -1;
    return aux_command(rate);
}

/* ------------------------------------------------------------- events */

static void ev_push(uint32_t type, int32_t x, int32_t y, uint32_t buttons)
{
    /* Coalesce a run of moves so fast motion cannot flood the queue. */
    if (type == KEV_MOUSE_MOVE && ev_head != ev_tail) {
        unsigned last = (ev_head + EVQ_SIZE - 1) % EVQ_SIZE;
        if (evq[last].type == KEV_MOUSE_MOVE) {
            evq[last].x = x;
            evq[last].y = y;
            evq[last].buttons = buttons;
            return;
        }
    }

    unsigned next = (ev_head + 1) % EVQ_SIZE;
    if (next == ev_tail)
        ev_tail = (ev_tail + 1) % EVQ_SIZE;   /* full: drop the oldest */

    evq[ev_head].type = type;
    evq[ev_head].x = x;
    evq[ev_head].y = y;
    evq[ev_head].key = 0;
    evq[ev_head].buttons = buttons;
    ev_head = next;
}

/* ------------------------------------------------------------ packets */

static void handle_packet(void)
{
    uint8_t flags = ms.pkt[0];

    /* Discard packets whose delta counters overflowed: the deltas are
     * meaningless and would teleport the cursor. */
    if (flags & 0xC0)
        return;

    int dx = (flags & 0x10) ? (int)ms.pkt[1] - 256 : (int)ms.pkt[1];
    int dy = (flags & 0x20) ? (int)ms.pkt[2] - 256 : (int)ms.pkt[2];

    int nx = ms.x + dx;
    int ny = ms.y - dy;                 /* device Y grows upward */

    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > ms.bound_w - 1) nx = ms.bound_w - 1;
    if (ny > ms.bound_h - 1) ny = ms.bound_h - 1;

    bool moved = (nx != ms.x || ny != ms.y);
    ms.x = nx;
    ms.y = ny;

    uint32_t old = ms.buttons;
    uint32_t now = flags & (K_MOUSE_LEFT | K_MOUSE_RIGHT | K_MOUSE_MIDDLE);
    ms.buttons = now;

    if (moved)
        ev_push(KEV_MOUSE_MOVE, nx, ny, now);

    uint32_t changed = old ^ now;
    for (uint32_t bit = 1; bit <= K_MOUSE_MIDDLE; bit <<= 1) {
        if (!(changed & bit))
            continue;
        ev_push((now & bit) ? KEV_MOUSE_DOWN : KEV_MOUSE_UP, nx, ny, now);
    }

    /* pkt[3] carries the wheel delta on IntelliMouse devices; the event
     * ABI has no scroll type yet, so it is parsed and dropped. */
}

static void mouse_irq(struct regs *r)
{
    (void)r;

    /* Drain everything the controller has for us; one IRQ can cover
     * several bytes. */
    for (int i = 0; i < 32; i++) {
        uint8_t st = inb(PS2_STAT);
        if (!(st & ST_OUTFULL))
            return;
        if (!(st & ST_AUX))
            return;                     /* keyboard byte: leave it for IRQ1 */

        uint8_t data = inb(PS2_DATA);

        if (ms.index == 0 && !(data & 0x08))
            continue;                   /* out of sync: discard until bit 3 */

        ms.pkt[ms.index++] = data;
        if (ms.index >= ms.packet_size) {
            ms.index = 0;
            handle_packet();
        }
    }
}

/* ---------------------------------------------------------------- API */

void mouse_init(int screen_w, int screen_h)
{
    uint64_t flags;
    int cfg;

    ms.present = false;
    ms.packet_size = 3;
    ms.index = 0;
    ms.buttons = 0;
    ev_head = ev_tail = 0;
    mouse_set_bounds(screen_w, screen_h);
    ms.x = ms.bound_w / 2;
    ms.y = ms.bound_h / 2;

    /* The probe is polled; keep IRQ1 from stealing our acks. */
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    cli();

    if (ctl_write(CTL_AUX_ENABLE) < 0)
        goto fail;

    if (ctl_write(CTL_READ_CFG) < 0)
        goto fail;
    if (wait_read() < 0)
        goto fail;
    cfg = inb(PS2_DATA);

    cfg |= CFG_AUX_IRQ;
    cfg &= ~CFG_AUX_CLK;

    if (ctl_write(CTL_WRITE_CFG) < 0)
        goto fail;
    if (wait_write() < 0)
        goto fail;
    outb(PS2_DATA, (uint8_t)cfg);

    if (aux_command(0xF6) < 0)          /* set defaults */
        goto fail;

    /* Optional IntelliMouse handshake: the magic rate sequence makes a
     * wheel mouse switch to device id 3 and 4-byte packets. */
    if (aux_set_rate(200) == 0 && aux_set_rate(100) == 0 &&
        aux_set_rate(80) == 0 && aux_command(0xF2) == 0) {
        int id = aux_read();
        if (id == 0x03)
            ms.packet_size = 4;
        aux_set_rate(100);              /* back to a sane report rate */
    }

    if (aux_command(0xF4) < 0)          /* enable data reporting */
        goto fail;

    ms.present = true;
    irq_install_handler(12, mouse_irq);
    pic_clear_mask(12);

    if (flags & 0x200)
        sti();
    kprintf("mouse: PS/2 %s, %dx%d\n",
            ms.packet_size == 4 ? "wheel" : "standard",
            ms.bound_w, ms.bound_h);
    return;

fail:
    ms.present = false;
    if (flags & 0x200)
        sti();
    kprintf("mouse: no PS/2 pointing device\n");
}

bool mouse_present(void)
{
    return ms.present;
}

void mouse_get(struct k_mouse *out)
{
    if (!out)
        return;
    out->x = ms.x;
    out->y = ms.y;
    out->buttons = ms.buttons;
    out->present = ms.present ? 1 : 0;
}

bool mouse_pop_event(struct k_event *ev)
{
    uint64_t flags;

    if (!ev || !ms.present)
        return false;

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    cli();

    bool got = false;
    if (ev_head != ev_tail) {
        *ev = evq[ev_tail];
        ev_tail = (ev_tail + 1) % EVQ_SIZE;
        got = true;
    }

    if (flags & 0x200)
        sti();
    return got;
}

void mouse_set_bounds(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ms.bound_w = w;
    ms.bound_h = h;
    if (ms.x > w - 1) ms.x = w - 1;
    if (ms.y > h - 1) ms.y = h - 1;
    if (ms.x < 0) ms.x = 0;
    if (ms.y < 0) ms.y = 0;
}
