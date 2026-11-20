#include "xhci.h"
#include "kernel.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "proc.h"
#include "timer.h"
#include "initcall.h"
#include "keyboard.h"
#include "mouse.h"
#include "usb_hid_core.h"
#include "string.h"

#define MMIO_VA 0xFFFFFFFFC0100000ULL
#define MMIO_SIZE 65536
#define SLOTS 32
#define TRBS 256
#define TYPE(n) ((uint32_t)(n) << 10)
#define IOC (1u << 5)
#define TIMEOUT 200

struct trb {
    uint64_t parameter;
    uint32_t status, control;
};

struct ring {
    struct trb *trb;
    uint64_t phys;
    unsigned next, cycle;
};

struct usb_device {
    unsigned slot, port, speed, ep, protocol, packet;
    uint64_t input, output, buffer;
    struct ring control, interrupt;
    uint8_t previous[8];
    bool active, pending, logged;
};

static volatile uint8_t *mmio;
static unsigned op, runtime, doorbells, context_size, slots, ports;
static struct ring commands;
static volatile struct trb *events;
static uint64_t event_phys;
static unsigned event_next, event_cycle;
static uint64_t *dcbaa;
static struct usb_device devices[SLOTS + 1];
static uint64_t awaited;
static struct trb completion;
static bool completed, running;

static uint32_t rd(unsigned off)
{
    return *(volatile uint32_t *)(mmio + off);
}

static void wr(unsigned off, uint32_t value)
{
    *(volatile uint32_t *)(mmio + off) = value;
}

static void wr64(unsigned off, uint64_t value)
{
    wr(off, (uint32_t)value);
    wr(off + 4, value >> 32);
}

static void fence(void)
{
    __asm__ volatile("mfence" : : : "memory");
}

static bool wait_reg(unsigned off, uint32_t mask, uint32_t value)
{
    uint64_t until = timer_ticks() + TIMEOUT;
    do {
        if ((rd(off) & mask) == value)
            return true;
        __asm__ volatile("pause");
    } while (timer_ticks() < until);
    kprintf("xhci: timeout reg %x = %x mask %x wanted %x\n",
            off, rd(off), mask, value);
    return false;
}

static bool ring_init(struct ring *r)
{
    r->phys = pmm_alloc();
    if (!r->phys)
        return false;
    r->trb = P2V(r->phys);
    r->cycle = 1;
    r->next = 0;
    return true;
}

static uint64_t enqueue(struct ring *r, uint64_t parameter,
                         uint32_t status, uint32_t control)
{
    unsigned i = r->next++;
    r->trb[i].parameter = parameter;
    r->trb[i].status = status;
    fence();
    r->trb[i].control = control | r->cycle;
    if (r->next == TRBS - 1) {
        r->trb[TRBS - 1].parameter = r->phys;
        r->trb[TRBS - 1].status = 0;
        fence();
        r->trb[TRBS - 1].control = TYPE(6) | 2 | r->cycle;
        r->next = 0;
        r->cycle ^= 1;
    }
    return r->phys + i * sizeof(struct trb);
}

static void queue_report(struct usb_device *d)
{
    enqueue(&d->interrupt, d->buffer, d->packet, TYPE(1) | IOC | (1u << 2));
    d->pending = true;
    fence();
    wr(doorbells + d->slot * 4, d->ep);
}

static void emit_scancode(uint8_t sc, void *ctx)
{
    (void)ctx;
    keyboard_handle_scancode(sc);
}

static void report_event(struct trb e)
{
    unsigned slot = e.control >> 24;
    unsigned ep = (e.control >> 16) & 31;
    unsigned code = e.status >> 24;
    if (!slot || slot > slots)
        return;
    struct usb_device *d = &devices[slot];
    if (!d->active || ep != d->ep || !d->pending)
        return;
    d->pending = false;
    if (code != 1 && code != 13) {
        kprintf("usb: slot %u transfer error %u; endpoint stopped\n", slot, code);
        d->active = false;
        return;
    }
    unsigned residual = e.status & 0xFFFFFF;
    unsigned len = residual <= d->packet ? d->packet - residual : 0;
    uint8_t *data = P2V(d->buffer);
    if (d->protocol == 1 && len >= 8) {
        /* Keep each E0 sequence atomic against the PS/2 IRQ on this CPU. */
        uint64_t flags = irq_save();
        bool accepted = usb_hid_keyboard(d->previous, data, emit_scancode, NULL);
        irq_restore(flags);
        if (accepted)
            memcpy(d->previous, data, 8);
    } else if (d->protocol == 2) {
        struct usb_hid_mouse m;
        if (usb_hid_mouse(data, len, &m)) {
            mouse_report(m.dx, m.dy, m.dz, m.buttons);
            if (!d->logged && (m.dx || m.dy || m.dz || m.buttons)) {
                kprintf("usb: mouse input dx %d dy %d buttons %u\n",
                        m.dx, m.dy, m.buttons);
                d->logged = true;
            }
        }
    }
    queue_report(d);
}

static void drain_events(void)
{
    /* A bounded batch also gives the scheduler time during an event storm. */
    for (unsigned n = 0; n < TRBS; n++) {
        volatile struct trb *p = &events[event_next];
        uint32_t control = p->control;
        if ((control & 1) != event_cycle)
            break;
        fence();
        struct trb e = {p->parameter, p->status, control};
        unsigned type = (control >> 10) & 63;
        if ((type == 33 || type == 32) && awaited &&
            (e.parameter & ~15ULL) == awaited) {
            completion = e;
            completed = true;
        } else if (type == 32) {
            report_event(e);
        }
        if (++event_next == TRBS) {
            event_next = 0;
            event_cycle ^= 1;
        }
    }
    wr64(runtime + 0x38, (event_phys + event_next * 16) | 8);
}

static bool wait_completion(void)
{
    uint64_t until = timer_ticks() + TIMEOUT;
    do {
        drain_events();
        if (completed) {
            awaited = 0;
            unsigned code = completion.status >> 24;
            if (code == 1)
                return true;
            kprintf("xhci: completion code %u control %x\n", code, completion.control);
            return false;
        }
        __asm__ volatile("pause");
    } while (timer_ticks() < until);
    kprintf("xhci: event timeout ptr %lx status %x\n", awaited, rd(op + 4));
    awaited = 0;
    return false;
}

static bool command(uint64_t parameter, uint32_t control)
{
    completed = false;
    awaited = enqueue(&commands, parameter, 0, control);
    fence();
    wr(doorbells, 0);
    return wait_completion();
}

static bool control_transfer(struct usb_device *d, uint8_t request_type,
                             uint8_t request, uint16_t value, uint16_t index,
                             uint16_t length)
{
    bool in = request_type & 0x80;
    uint64_t setup = request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    enqueue(&d->control, setup, 8, TYPE(2) | (1u << 6) |
            ((length ? (in ? 3u : 2u) : 0u) << 16));
    if (length)
        enqueue(&d->control, d->buffer, length, TYPE(3) | (in ? (1u << 16) : 0));
    completed = false;
    awaited = enqueue(&d->control, 0, 0, TYPE(4) | IOC |
                       ((!length || !in) ? (1u << 16) : 0));
    fence();
    wr(doorbells + d->slot * 4, 1);
    return wait_completion();
}

static uint32_t *input_context(struct usb_device *d, unsigned n)
{
    return (uint32_t *)((uint8_t *)P2V(d->input) + context_size * n);
}

static void ep_context(uint32_t *ctx, struct ring *r, unsigned type,
                       unsigned packet, unsigned interval)
{
    ctx[0] = interval << 16;
    ctx[1] = (3u << 1) | (type << 3) | (packet << 16);
    ctx[2] = (uint32_t)r->phys | 1;
    ctx[3] = r->phys >> 32;
    ctx[4] = packet;
    if (type == 7)
        ctx[4] |= packet << 16;
}

static bool enumerate(unsigned port)
{
    unsigned portsc = op + 0x400 + (port - 1) * 16;
    uint32_t ps = rd(portsc);
    if (!(ps & 1))
        return true;
    if (!(ps & 2)) {
        wr(portsc, (ps & ~(0xFE0000u | 2u)) | (1u << 4));
        if (!wait_reg(portsc, 1u << 4, 0) || !wait_reg(portsc, 2, 2))
            return false;
    }
    unsigned speed = (rd(portsc) >> 10) & 15;
    if (!command(0, TYPE(9)))
        return false;
    unsigned slot = completion.control >> 24;
    if (!slot || slot > slots)
        return false;
    struct usb_device *d = &devices[slot];
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->input = pmm_alloc();
    d->output = pmm_alloc();
    d->buffer = pmm_alloc();
    if (!d->input || !d->output || !d->buffer || !ring_init(&d->control))
        return false;
    dcbaa[slot] = d->output;
    uint32_t *icc = input_context(d, 0);
    uint32_t *sc = input_context(d, 1);
    icc[1] = 3;
    sc[0] = (speed << 20) | (1u << 27);
    sc[1] = port << 16;
    unsigned packet = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    ep_context(input_context(d, 2), &d->control, 4, packet, 0);
    fence();
    if (!command(d->input, TYPE(11) | (slot << 24)))
        return false;
    if (!control_transfer(d, 0x80, 6, 0x100, 0, 8))
        return false;
    uint8_t *buf = P2V(d->buffer);
    if (buf[0] < 18 || buf[1] != 1)
        return false;
    if (buf[4] == 9) {
        kprintf("usb: port %u hub skipped\n", port);
        return true;
    }
    if (speed == 1 && buf[7] != packet) {
        if (buf[7] != 8 && buf[7] != 16 && buf[7] != 32 && buf[7] != 64)
            return false;
        packet = buf[7];
        memset(P2V(d->input), 0, PAGE_SIZE);
        icc[1] = 2;
        memcpy(input_context(d, 2), (uint8_t *)P2V(d->output) + context_size,
               context_size);
        input_context(d, 2)[1] = (input_context(d, 2)[1] & 0xFFFF) | (packet << 16);
        if (!command(d->input, TYPE(13) | (slot << 24)))
            return false;
    }
    if (!control_transfer(d, 0x80, 6, 0x100, 0, 18))
        return false;
    if (!control_transfer(d, 0x80, 6, 0x200, 0, 9))
        return false;
    unsigned total = buf[2] | ((unsigned)buf[3] << 8);
    if (buf[1] != 2 || total < 9 || total > PAGE_SIZE)
        return false;
    if (!control_transfer(d, 0x80, 6, 0x200, 0, total))
        return false;
    unsigned config = buf[5], interface = 0, protocol = 0;
    unsigned endpoint = 0, maxpacket = 0, interval = 0;
    for (unsigned off = 0; off + 2 <= total;) {
        unsigned len = buf[off], type = buf[off + 1];
        if (len < 2 || off + len > total)
            return false;
        if (type == 4 && len >= 9) {
            protocol = buf[off + 3] == 0 && buf[off + 5] == 3 &&
                       buf[off + 6] == 1 ? buf[off + 7] : 0;
            interface = buf[off + 2];
        }
        if (type == 5 && len >= 7 && (protocol == 1 || protocol == 2) &&
            (buf[off + 2] & 0x80) && (buf[off + 3] & 3) == 3) {
            endpoint = buf[off + 2] & 15;
            maxpacket = (buf[off + 4] | ((unsigned)buf[off + 5] << 8)) & 0x7FF;
            interval = buf[off + 6];
            break;
        }
        off += len;
    }
    if (!endpoint) {
        kprintf("usb: port %u no boot HID interface, skipped\n", port);
        return true;
    }
    if (!maxpacket || maxpacket > 1024 || !interval ||
        maxpacket < (protocol == 1 ? 8u : 3u))
        return false;
    d->protocol = protocol;
    d->packet = maxpacket;
    d->ep = endpoint * 2 + 1;
    if (!control_transfer(d, 0, 9, config, 0, 0) ||
        !control_transfer(d, 0x21, 11, 0, interface, 0) ||
        !control_transfer(d, 0x21, 10, 0, interface, 0))
        return false;
    if (!ring_init(&d->interrupt))
        return false;
    memset(P2V(d->input), 0, PAGE_SIZE);
    icc[1] = 1u | (1u << d->ep);
    memcpy(input_context(d, 1), P2V(d->output), context_size);
    sc[0] = (sc[0] & ~(31u << 27)) | (d->ep << 27);
    if (speed >= 3) {
        if (interval > 16)
            return false;
        interval--;
    } else {
        unsigned ticks = interval * 8, exponent = 0;
        while (ticks > 1) {
            ticks >>= 1;
            exponent++;
        }
        interval = exponent;
    }
    ep_context(input_context(d, d->ep + 1), &d->interrupt, 7, maxpacket, interval);
    fence();
    if (!command(d->input, TYPE(12) | (slot << 24)))
        return false;
    d->active = true;
    if (protocol == 2)
        mouse_set_present();
    kprintf("usb: port %u slot %u %s (boot)\n", port, slot,
            protocol == 1 ? "keyboard" : "mouse");
    queue_report(d);
    return true;
}

static void poll_thread(void *arg)
{
    (void)arg;
    while (running) {
        drain_events();
        task_sleep_ticks(1);
    }
}

int xhci_init(void)
{
    struct pci_dev pci;
    if (!pci_find_class(0x0C, 3, 0x30, &pci))
        return 0;
    if (pci.bar0 & 1)
        return -1;
    uint64_t bar = pci.bar0 & ~15ULL;
    if ((pci.bar0 & 6) == 4)
        bar |= (uint64_t)pci_read32(pci.bus, pci.dev, pci.fn, 0x14) << 32;
    if (!bar || (bar & (PAGE_SIZE - 1)))
        return -1;
    uint32_t cmd = pci_read16(pci.bus, pci.dev, pci.fn, 4);
    pci_write32(pci.bus, pci.dev, pci.fn, 4, cmd | 6u | (1u << 10));
    for (unsigned off = 0; off < MMIO_SIZE; off += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), MMIO_VA + off, bar + off,
                     PTE_P | PTE_W | PTE_PCD);
    mmio = (volatile uint8_t *)MMIO_VA;
    op = rd(0) & 0xFF;
    uint32_t hcs = rd(4), hcc = rd(0x10);
    ports = hcs >> 24;
    slots = (hcs & 255) < SLOTS ? hcs & 255 : SLOTS;
    context_size = hcc & 4 ? 64 : 32;
    doorbells = rd(0x14) & ~3u;
    runtime = rd(0x18) & ~31u;
    if (!slots || op < 0x20 || op + 0x400 + ports * 16 > MMIO_SIZE ||
        doorbells + (slots + 1) * 4 > MMIO_SIZE || runtime + 0x40 > MMIO_SIZE)
        return -1;
    kprintf("xhci: controller %02x:%02x.%u ports %u contexts %u\n",
            pci.bus, pci.dev, pci.fn, ports, context_size);
    unsigned ext = (hcc >> 16) * 4;
    for (unsigned n = 0; ext && n < 256; n++) {
        if (ext > MMIO_SIZE - 8)
            return -1;
        uint32_t cap = rd(ext);
        if ((cap & 255) == 1) {
            wr(ext, cap | (1u << 24));
            if (!wait_reg(ext, 1u << 16, 0))
                return -1;
            wr(ext + 4, 0);
        }
        unsigned next = ((cap >> 8) & 255) * 4;
        ext = next ? ext + next : 0;
    }
    wr(op, rd(op) & ~1u);
    if (!wait_reg(op + 4, 1, 1))
        return -1;
    wr(op, 2);
    if (!wait_reg(op, 2, 0) || !wait_reg(op + 4, 1u << 11, 0))
        return -1;
    if (!(rd(op + 8) & 1))
        return -1;
    uint64_t dcbaa_phys = pmm_alloc();
    uint64_t erst_phys = pmm_alloc();
    event_phys = pmm_alloc();
    if (!dcbaa_phys || !erst_phys || !event_phys || !ring_init(&commands))
        return -1;
    dcbaa = P2V(dcbaa_phys);
    unsigned scratch = ((rd(8) >> 27) & 31) | (((rd(8) >> 21) & 31) << 5);
    if (scratch) {
        uint64_t array = pmm_alloc_contig((scratch * 8 + PAGE_SIZE - 1) / PAGE_SIZE);
        uint64_t buffers = pmm_alloc_contig(scratch);
        if (!array || !buffers)
            return -1;
        uint64_t *p = P2V(array);
        for (unsigned i = 0; i < scratch; i++)
            p[i] = buffers + i * PAGE_SIZE;
        dcbaa[0] = array;
    }
    events = P2V(event_phys);
    event_cycle = 1;
    uint64_t *erst = P2V(erst_phys);
    erst[0] = event_phys;
    erst[1] = TRBS;
    wr(op + 0x38, slots);
    wr64(op + 0x30, dcbaa_phys);
    wr64(op + 0x18, commands.phys | 1);
    wr(runtime + 0x28, 1);
    wr64(runtime + 0x38, event_phys);
    fence();
    wr64(runtime + 0x30, erst_phys);
    wr(runtime + 0x20, 0);
    wr(op, 1);
    if (!wait_reg(op + 4, 1, 0))
        goto fail;
    for (unsigned port = 1; port <= ports; port++) {
        unsigned off = op + 0x400 + (port - 1) * 16;
        if (hcc & 8)
            wr(off, (rd(off) & ~(0xFE0000u | 2u)) | (1u << 9));
    }
    timer_sleep(2);
    for (unsigned port = 1; port <= ports; port++) {
        if (!enumerate(port)) {
            kprintf("usb: port %u enumeration failed\n", port);
            goto fail;
        }
    }
    running = true;
    if (!kthread_create(poll_thread, NULL, "xhci"))
        goto fail;
    return 0;
fail:
    running = false;
    wr(op, 0);
    wait_reg(op + 4, 1, 1);
    return -1;
}

static int xhci_drv_init(void)
{
    return xhci_init();
}
driver_initcall(xhci_drv_init);
