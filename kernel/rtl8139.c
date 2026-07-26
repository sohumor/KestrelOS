#include "kernel.h"
#include "io.h"
#include "string.h"
#include "pci.h"
#include "pmm.h"
#include "interrupts.h"
#include "proc.h"
#include "net.h"
#include "rtl8139.h"

/* Realtek RTL8139 (QEMU: -device rtl8139). I/O-port register file.
 *
 * RX is a single contiguous ring (8192 + 16 + 1500 bytes with the WRAP bit
 * set, so a frame crossing the 8K boundary continues linearly and never
 * needs to be reassembled). TX uses the 4 round-robin descriptors.
 */

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

#define REG_IDR0     0x00   /* MAC address, 6 bytes */
#define REG_TSD0     0x10   /* TX status/command, 4 x u32 */
#define REG_TSAD0    0x20   /* TX buffer phys addr, 4 x u32 */
#define REG_RBSTART  0x30   /* RX ring phys addr */
#define REG_CR       0x37   /* command */
#define REG_CAPR     0x38   /* current address of packet read */
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_RCR      0x44
#define REG_CONFIG1  0x52

#define CR_BUFE      0x01   /* RX buffer empty */
#define CR_TE        0x04
#define CR_RE        0x08
#define CR_RST       0x10

#define ISR_ROK      0x0001
#define ISR_RER      0x0002
#define ISR_TOK      0x0004
#define ISR_TER      0x0008

#define TSD_OWN      0x00002000u  /* set by hw when DMA to FIFO is done */

/* AAP|APM|AM|AB + WRAP: accept our unicast/broadcast/multicast, no ring
 * wraparound (frames run past the 8K mark into the slack area). */
#define RCR_VAL      0x8F

#define RX_RING_LEN  8192
#define RX_BUF_TOTAL (8192 + 16 + 1500)
#define TX_NDESC     4
#define TX_MAX_LEN   1792

static bool present;
static uint16_t io_base;
static uint8_t mac[6];
static uint8_t *rx_buf;          /* virtual (direct map) */
static uint32_t rx_pos;          /* read offset within the 8K ring */
static uint64_t tx_phys;         /* 4 pages, one per descriptor */
static int tx_cur;
static bool tx_used[TX_NDESC];

static void rx_reset(void)
{
    uint8_t cr = inb(io_base + REG_CR);
    outb(io_base + REG_CR, cr & (uint8_t)~CR_RE);
    outb(io_base + REG_CR, cr | CR_RE);
    outl(io_base + REG_RCR, RCR_VAL);
    rx_pos = 0;
}

/* Drain complete packets out of the RX ring. IRQ context: no sleeping. */
static void rx_drain(void)
{
    while (!(inb(io_base + REG_CR) & CR_BUFE)) {
        uint8_t *p = rx_buf + rx_pos;
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));

        /* len includes the 4-byte CRC appended by the NIC. */
        if (!(status & 0x0001) || len < 8 || len > ETH_MTU + 18) {
            kprintf("rtl8139: bad rx (status %04x len %u), resetting rx\n",
                    status, len);
            rx_reset();
            return;
        }

        net_rx(p + 4, len - 4);

        rx_pos = (rx_pos + 4 + len + 3) & ~3u;
        if (rx_pos >= RX_RING_LEN)
            rx_pos -= RX_RING_LEN;
        outw(io_base + REG_CAPR, (uint16_t)(rx_pos - 16));
    }
}

static void rtl_irq(struct regs *r)
{
    (void)r;
    uint16_t isr = inw(io_base + REG_ISR);
    if (!isr)
        return;
    outw(io_base + REG_ISR, isr);   /* ack by writing the bits back */
    if (isr & (ISR_ROK | ISR_RER))
        rx_drain();
    /* TOK/TER: nothing to do, the send path polls descriptor status. */
}

bool rtl8139_init(void)
{
    struct pci_dev d;

    if (!pci_find(RTL_VENDOR, RTL_DEVICE, &d))
        return false;
    if (!(d.bar0 & 1)) {
        kprintf("rtl8139: BAR0 is not an I/O BAR, skipping\n");
        return false;
    }
    io_base = (uint16_t)(d.bar0 & 0xFFFC);
    pci_enable_bus_master(&d);

    outb(io_base + REG_CONFIG1, 0x00);   /* power on */

    outb(io_base + REG_CR, CR_RST);
    int spin = 1000000;
    while ((inb(io_base + REG_CR) & CR_RST) && --spin > 0)
        ;
    if (spin == 0) {
        kprintf("rtl8139: reset timed out\n");
        return false;
    }

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_IDR0 + i);

    /* RX ring: 3 pages cover 8192+16+1500. Phys < 4GB on our 128MB VM. */
    uint64_t rx_phys = pmm_alloc_contig((RX_BUF_TOTAL + PAGE_SIZE - 1)
                                        / PAGE_SIZE);
    tx_phys = pmm_alloc_contig(TX_NDESC);
    if (!rx_phys || !tx_phys) {
        kprintf("rtl8139: out of contiguous memory for DMA buffers\n");
        return false;
    }
    rx_buf = P2V(rx_phys);
    rx_pos = 0;

    outl(io_base + REG_RBSTART, (uint32_t)rx_phys);
    outl(io_base + REG_RCR, RCR_VAL);
    for (int i = 0; i < TX_NDESC; i++)
        outl(io_base + REG_TSAD0 + i * 4,
             (uint32_t)(tx_phys + (uint64_t)i * PAGE_SIZE));

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK | ISR_RER | ISR_TER);
    outb(io_base + REG_CR, CR_TE | CR_RE);

    int irq = d.irq_line;
    if (irq > 0 && irq < 16) {   /* 0 and 0xFF mean "not routed" */
        irq_install_handler(irq, rtl_irq);
        pic_clear_mask(irq);
        if (irq >= 8)
            pic_clear_mask(2);   /* cascade */
    } else {
        kprintf("rtl8139: no usable irq line (%d), polling only\n", irq);
    }

    present = true;
    kprintf("rtl8139: io 0x%x irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            io_base, irq, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool rtl8139_present(void)
{
    return present;
}

const uint8_t *rtl8139_mac(void)
{
    return mac;
}

int rtl8139_send(const void *frame, int len)
{
    if (!present || len <= 0 || len > TX_MAX_LEN)
        return -1;

    uint64_t f = irq_save();

    int d = tx_cur;
    tx_cur = (tx_cur + 1) % TX_NDESC;
    uint16_t tsd = io_base + REG_TSD0 + (uint16_t)(d * 4);

    /* Wait for the previous transmit on this descriptor to leave the
     * buffer (OWN set means the DMA into the FIFO completed). */
    if (tx_used[d]) {
        int spin = 1000000;
        while (!(inl(tsd) & TSD_OWN) && --spin > 0)
            ;
        if (spin == 0) {
            irq_restore(f);
            kprintf("rtl8139: tx descriptor %d stuck\n", d);
            return -1;
        }
    }

    uint8_t *buf = P2V(tx_phys + (uint64_t)d * PAGE_SIZE);
    memcpy(buf, frame, len);
    if (len < 60) {   /* minimum ethernet frame (without CRC), zero pad */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }

    outl(io_base + REG_TSAD0 + d * 4,
         (uint32_t)(tx_phys + (uint64_t)d * PAGE_SIZE));
    outl(tsd, (uint32_t)len);   /* size with OWN clear starts the DMA */
    tx_used[d] = true;

    irq_restore(f);
    return 0;
}

void rtl8139_poll(void)
{
    if (!present)
        return;
    uint64_t f = irq_save();
    uint16_t isr = inw(io_base + REG_ISR);
    if (isr)
        outw(io_base + REG_ISR, isr);
    rx_drain();
    irq_restore(f);
}
