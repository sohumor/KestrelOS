#include "kernel.h"
#include "string.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "interrupts.h"
#include "proc.h"
#include "net.h"
#include "netdev.h"
#include "e1000.h"

/* Intel 8254x-family "e1000" gigabit NIC (MMIO register file).
 *
 * RX and TX are classic descriptor rings in physically contiguous RAM,
 * each descriptor pointing at its own 2 KiB DMA buffer. The 128 KiB
 * register window (a memory BAR) sits above RAM, outside the direct map,
 * so it is mapped explicitly at E1000_MMIO_VA with caching disabled.
 */

#define E1000_VENDOR 0x8086

/* 82540EM (QEMU/VirtualBox/VMware), 82545EM, 82574L, I217. */
static const uint16_t e1000_devices[] = { 0x100E, 0x100F, 0x10D3, 0x153A };

#define E1000_MMIO_VA   0xFFFFFFFFC0000000ULL
#define E1000_MMIO_SIZE (128 * 1024)

#ifndef PTE_PCD
#define PTE_PCD 0x010ULL     /* page-level cache disable */
#endif

/* Register offsets (bytes). */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200    /* 128 x u32 multicast table array */
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1u << 6)      /* set link up */
#define CTRL_RST   (1u << 26)     /* device reset */
#define STATUS_LU  (1u << 1)      /* link up status */

#define ICR_LSC    (1u << 2)
#define ICR_RXDMT0 (1u << 4)
#define ICR_RXO    (1u << 6)
#define ICR_RXT0   (1u << 7)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)     /* strip CRC; BSIZE 00 = 2048 bytes */

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define TCTL_CT    (0x10u << 4)   /* collision threshold */
#define TCTL_COLD  (0x40u << 12)  /* collision distance (full duplex) */

#define RAH_AV     (1u << 31)     /* address valid */

#define RXD_STAT_DD  0x01
#define RXD_STAT_EOP 0x02
#define TXD_STAT_DD  0x01
#define TXD_CMD_EOP  (1u << 0)
#define TXD_CMD_IFCS (1u << 1)
#define TXD_CMD_RS   (1u << 3)

#define RX_NDESC 32
#define TX_NDESC 16
#define BUF_SIZE 2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t len;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static bool present;
static volatile uint32_t *mmio;                 /* uncached register window */
static uint8_t mac[6];
static volatile struct e1000_rx_desc *rx_ring;  /* direct-map virtual */
static volatile struct e1000_tx_desc *tx_ring;
static uint8_t *rx_buf;                         /* RX_NDESC x BUF_SIZE */
static uint8_t *tx_buf;                         /* TX_NDESC x BUF_SIZE */
static int rx_next;                             /* next descriptor to check */
static int tx_next;                             /* next descriptor to fill */
static bool tx_used[TX_NDESC];

static uint32_t reg_read(uint32_t off)
{
    return mmio[off / 4];
}

static void reg_write(uint32_t off, uint32_t val)
{
    mmio[off / 4] = val;
}

/* ---- EEPROM ---- */

/* EERD layout differs across 8254x variants: the original 82540-class
 * parts use DONE = bit 4 with the address at bits 15:8, newer ones use
 * DONE = bit 1 with the address at bits 15:2. Probe by starting a read
 * of word 0 and watching which DONE bit ever sets. */
static bool eeprom_read(int addr, uint16_t *out)
{
    static int done_bit, addr_shift;

    if (done_bit == 0) {
        reg_write(REG_EERD, 1);              /* START, address 0 */
        done_bit = 0x02;
        addr_shift = 2;
        for (int i = 0; i < 1000; i++) {
            if (reg_read(REG_EERD) & 0x10) {
                done_bit = 0x10;
                addr_shift = 8;
                break;
            }
        }
    }

    reg_write(REG_EERD, ((uint32_t)addr << addr_shift) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = reg_read(REG_EERD);
        if (v & (uint32_t)done_bit) {
            *out = (uint16_t)(v >> 16);
            return true;
        }
    }
    return false;
}

static bool read_mac(void)
{
    uint32_t ral = reg_read(REG_RAL);

    if (ral != 0) {
        /* VirtualBox/QEMU preload the station address registers. */
        uint32_t rah = reg_read(REG_RAH);
        mac[0] = (uint8_t)ral;
        mac[1] = (uint8_t)(ral >> 8);
        mac[2] = (uint8_t)(ral >> 16);
        mac[3] = (uint8_t)(ral >> 24);
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
        return true;
    }

    for (int w = 0; w < 3; w++) {
        uint16_t v;
        if (!eeprom_read(w, &v))
            return false;
        mac[w * 2] = (uint8_t)v;
        mac[w * 2 + 1] = (uint8_t)(v >> 8);
    }
    return true;
}

/* ---- RX ---- */

/* Drain complete frames out of the RX ring. IRQ context: no sleeping,
 * no allocation. SECRC is set, so len already excludes the CRC. */
static void rx_drain(void)
{
    while (rx_ring[rx_next].status & RXD_STAT_DD) {
        volatile struct e1000_rx_desc *d = &rx_ring[rx_next];
        int len = d->len;

        if ((d->status & RXD_STAT_EOP) && d->errors == 0 &&
            len > 0 && len <= BUF_SIZE)
            net_rx(rx_buf + rx_next * BUF_SIZE, len);

        d->status = 0;
        reg_write(REG_RDT, (uint32_t)rx_next);   /* hand it back to hw */
        rx_next = (rx_next + 1) % RX_NDESC;
    }
}

static void e1000_irq(struct regs *r)
{
    (void)r;
    /* Reading ICR acks/clears it. PCI IRQs are level-triggered and can
     * be shared: ICR == 0 means the interrupt was not ours. */
    uint32_t icr = reg_read(REG_ICR);
    if (!icr)
        return;
    rx_drain();
}

static void e1000_poll(void)
{
    if (!present)
        return;
    uint64_t f = irq_save();
    (void)reg_read(REG_ICR);   /* ack anything pending */
    rx_drain();
    irq_restore(f);
}

/* ---- TX ---- */

static int e1000_send(const void *frame, int len)
{
    if (!present || len <= 0 || len > BUF_SIZE)
        return -1;

    uint64_t f = irq_save();

    int i = tx_next;
    volatile struct e1000_tx_desc *d = &tx_ring[i];

    /* Wait for the previous transmit on this descriptor to complete. */
    if (tx_used[i]) {
        int spin = 1000000;
        while (!(d->status & TXD_STAT_DD) && --spin > 0)
            ;
        if (spin == 0) {
            irq_restore(f);
            kprintf("e1000: tx descriptor %d stuck\n", i);
            return -1;
        }
    }

    uint8_t *buf = tx_buf + i * BUF_SIZE;
    memcpy(buf, frame, len);
    if (len < 60) {   /* minimum ethernet frame (without CRC), zero pad */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }

    d->len = (uint16_t)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_used[i] = true;

    tx_next = (tx_next + 1) % TX_NDESC;
    reg_write(REG_TDT, (uint32_t)tx_next);   /* doorbell starts the DMA */

    irq_restore(f);
    return len;
}

/* ---- init ---- */

static struct netdev e1000_netdev = {
    .name = "e1000",
    .send = e1000_send,
    .poll = e1000_poll,
};

bool e1000_init(void)
{
    struct pci_dev d;
    bool found = false;

    for (unsigned i = 0;
         i < sizeof(e1000_devices) / sizeof(e1000_devices[0]); i++) {
        if (pci_find(E1000_VENDOR, e1000_devices[i], &d)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    if (d.bar0 & 1) {
        kprintf("e1000: BAR0 is not a memory BAR, skipping\n");
        return false;
    }

    /* Enable memory space decode (bit 1) + bus mastering (bit 2). */
    uint32_t cmd = pci_read32(d.bus, d.dev, d.fn, 0x04);
    cmd = (cmd & 0xFFFF0000u) | (uint16_t)(cmd | 0x0006);
    pci_write32(d.bus, d.dev, d.fn, 0x04, cmd);

    /* Map the 128 KiB register window uncached; it lives above RAM and
     * is not covered by the direct map. */
    uint64_t mmio_phys = (uint64_t)d.bar0 & ~0xFULL;
    for (uint64_t off = 0; off < E1000_MMIO_SIZE; off += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), E1000_MMIO_VA + off,
                     mmio_phys + off, PTE_W | PTE_PCD);
    mmio = (volatile uint32_t *)E1000_MMIO_VA;

    /* Device reset: set CTRL.RST and wait for hardware auto-clear. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (int spin = 0; spin < 10000; spin++) {
        if (!(reg_read(REG_CTRL) & CTRL_RST))
            break;
    }

    reg_write(REG_IMC, 0xFFFFFFFFu);   /* mask + ack everything */
    (void)reg_read(REG_ICR);

    if (!read_mac()) {
        kprintf("e1000: could not read MAC address\n");
        return false;
    }
    reg_write(REG_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                       ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    reg_write(REG_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | RAH_AV);

    for (int i = 0; i < 128; i++)
        reg_write(REG_MTA + i * 4, 0);

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);

    /* Wait briefly for link-up status */
    for (int spin = 0; spin < 10000; spin++) {
        if (reg_read(REG_STATUS) & STATUS_LU)
            break;
    }

    /* Rings + buffers: physically contiguous, zeroed, < 4 GiB. */
    uint64_t rx_ring_phys = pmm_alloc_contig(1);
    uint64_t tx_ring_phys = pmm_alloc_contig(1);
    uint64_t rx_buf_phys = pmm_alloc_contig(RX_NDESC * BUF_SIZE / PAGE_SIZE);
    uint64_t tx_buf_phys = pmm_alloc_contig(TX_NDESC * BUF_SIZE / PAGE_SIZE);
    if (!rx_ring_phys || !tx_ring_phys || !rx_buf_phys || !tx_buf_phys) {
        kprintf("e1000: out of contiguous memory for DMA buffers\n");
        return false;
    }
    rx_ring = P2V(rx_ring_phys);
    tx_ring = P2V(tx_ring_phys);
    rx_buf = P2V(rx_buf_phys);
    tx_buf = P2V(tx_buf_phys);

    for (int i = 0; i < RX_NDESC; i++)
        rx_ring[i].addr = rx_buf_phys + (uint64_t)i * BUF_SIZE;
    for (int i = 0; i < TX_NDESC; i++)
        tx_ring[i].addr = tx_buf_phys + (uint64_t)i * BUF_SIZE;

    reg_write(REG_RDBAL, (uint32_t)rx_ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, RX_NDESC * 16);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_NDESC - 1);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    reg_write(REG_TDBAL, (uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, TX_NDESC * 16);
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    reg_write(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    int irq = d.irq_line;
    if (irq > 0 && irq < 16) {   /* 0 and 0xFF mean "not routed" */
        irq_install_handler(irq, e1000_irq);
        pic_clear_mask(irq);
        if (irq >= 8)
            pic_clear_mask(2);   /* cascade */
        (void)reg_read(REG_ICR); /* ack pending before enabling IMS */
        reg_write(REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);
        (void)reg_read(REG_ICR); /* flush ICR again */
    } else {
        kprintf("e1000: no usable irq line (%d), polling only\n", irq);
    }

    present = true;
    memcpy(e1000_netdev.mac, mac, 6);
    netdev_register(&e1000_netdev);
    kprintf("e1000: mmio 0x%x, irq %d, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            (uint32_t)mmio_phys, irq,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}
