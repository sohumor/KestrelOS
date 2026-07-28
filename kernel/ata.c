#include "kernel.h"
#include "ata.h"
#include "blockdev.h"
#include "io.h"
#include "string.h"
#include "random.h"
#include "spinlock.h"

/* ATA PIO driver: primary bus, master drive, 28-bit LBA, polling.
 *
 * The transfer path below is unchanged; the only new thing is that
 * ata_init() publishes the drive as block device "hda" instead of
 * leaving KFS to call ata_read()/ata_write() by name. */

#define ATA_IO_BASE   0x1F0
#define ATA_REG_DATA     (ATA_IO_BASE + 0)
#define ATA_REG_ERROR    (ATA_IO_BASE + 1)
#define ATA_REG_COUNT    (ATA_IO_BASE + 2)
#define ATA_REG_LBA_LO   (ATA_IO_BASE + 3)
#define ATA_REG_LBA_MID  (ATA_IO_BASE + 4)
#define ATA_REG_LBA_HI   (ATA_IO_BASE + 5)
#define ATA_REG_DRIVE    (ATA_IO_BASE + 6)
#define ATA_REG_STATUS   (ATA_IO_BASE + 7)
#define ATA_REG_CMD      (ATA_IO_BASE + 7)
#define ATA_REG_ALTSTAT  0x3F6

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_TIMEOUT 1000000

static int ata_present;
static struct blockdev ata_bdev;
static spinlock_t ata_lock = SPINLOCK_INIT;

/* >= 400ns settle time after a drive select. */
static void ata_delay_400ns(void)
{
    inb(ATA_REG_ALTSTAT);
    inb(ATA_REG_ALTSTAT);
    inb(ATA_REG_ALTSTAT);
    inb(ATA_REG_ALTSTAT);
}

static int ata_wait_not_busy(void)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(ATA_REG_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

/* Wait for DRQ; fail on ERR/DF or timeout. */
static int ata_wait_drq(void)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_REG_STATUS);
        if (st & ATA_SR_BSY)
            continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (st & ATA_SR_DRQ)
            return 0;
    }
    return -1;
}

/* Select master with LBA mode and the top 4 LBA bits, program the
 * count/LBA registers and issue a command. count 256 is sent as 0. */
static int ata_setup_cmd(uint32_t lba, uint32_t count, uint8_t cmd)
{
    if (ata_wait_not_busy() < 0)
        return -1;
    outb(ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();
    outb(ATA_REG_COUNT, (uint8_t)(count & 0xFF));
    outb(ATA_REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_CMD, cmd);
    return 0;
}

static void ata_insw(uint16_t *buf, int words)
{
    for (int i = 0; i < words; i++)
        buf[i] = inw(ATA_REG_DATA);
}

static void ata_outsw(const uint16_t *buf, int words)
{
    for (int i = 0; i < words; i++)
        outw(ATA_REG_DATA, buf[i]);
}

/* One READ/WRITE SECTORS command, count <= 256. */
static int ata_rw_chunk(uint32_t lba, uint32_t count, void *buf, int write)
{
    uint16_t *p = buf;

    if (ata_setup_cmd(lba, count == 256 ? 0 : count,
                      write ? ATA_CMD_WRITE : ATA_CMD_READ) < 0)
        return -1;

    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq() < 0)
            return -1;
        if (write)
            ata_outsw(p, 256);
        else
            ata_insw(p, 256);
        p += 256;
        ata_delay_400ns();
    }

    if (write) {
        outb(ATA_REG_CMD, ATA_CMD_FLUSH);
        if (ata_wait_not_busy() < 0)
            return -1;
        if (inb(ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
    }
    entropy_pool_add_interrupt(ENTROPY_DISK,
                               (lba << 9) ^ count ^ (write ? 0x80000000u : 0));
    return 0;
}

static int ata_rw(uint32_t lba, uint32_t count, void *buf, int write)
{
    if (!ata_present)
        return -1;
    uint64_t flags = spin_lock_irqsave(&ata_lock);
    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (ata_rw_chunk(lba, chunk, buf, write) < 0) {
            spin_unlock_irqrestore(&ata_lock, flags);
            return -1;
        }
        lba += chunk;
        count -= chunk;
        buf = (uint8_t *)buf + chunk * 512;
    }
    spin_unlock_irqrestore(&ata_lock, flags);
    return 0;
}

int ata_read(uint32_t lba, uint32_t count, void *buf)
{
    return ata_rw(lba, count, buf, 0);
}

int ata_write(uint32_t lba, uint32_t count, const void *buf)
{
    return ata_rw(lba, count, (void *)buf, 1);
}

/* --- block device face ------------------------------------------------
 * One block = one 512-byte sector, so the LBA needs no scaling. The
 * command registers only carry 28 bits of LBA, so anything above that is
 * refused here rather than silently truncated into the wrong sector. */

#define ATA_LBA28_MAX 0x0FFFFFFFu

static int ata_bd_read(struct blockdev *bd, uint64_t lba, uint32_t count,
                       void *buf)
{
    (void)bd;
    if (lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - (uint32_t)lba)
        return -1;
    return ata_read((uint32_t)lba, count, buf);
}

static int ata_bd_write(struct blockdev *bd, uint64_t lba, uint32_t count,
                        const void *buf)
{
    (void)bd;
    if (lba > ATA_LBA28_MAX || count > ATA_LBA28_MAX - (uint32_t)lba)
        return -1;
    return ata_write((uint32_t)lba, count, buf);
}

/* Model string is stored word-swapped in IDENTIFY words 27..46. */
static void ata_extract_model(const uint16_t *id, char *model)
{
    for (int i = 0; i < 20; i++) {
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';
}

void ata_init(void)
{
    uint16_t id[256];
    char model[41];

    outb(ATA_REG_DRIVE, 0xE0);
    ata_delay_400ns();

    /* Floating bus: no controller/drive at all. */
    if (inb(ATA_REG_STATUS) == 0xFF) {
        kprintf("ata: no controller on primary bus\n");
        return;
    }

    outb(ATA_REG_COUNT, 0);
    outb(ATA_REG_LBA_LO, 0);
    outb(ATA_REG_LBA_MID, 0);
    outb(ATA_REG_LBA_HI, 0);
    outb(ATA_REG_CMD, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    if (inb(ATA_REG_STATUS) == 0) {
        kprintf("ata: primary master not present\n");
        return;
    }
    if (ata_wait_not_busy() < 0) {
        kprintf("ata: primary master timed out\n");
        return;
    }
    /* Non-zero signature here means ATAPI/SATA, not plain ATA. */
    if (inb(ATA_REG_LBA_MID) != 0 || inb(ATA_REG_LBA_HI) != 0) {
        kprintf("ata: primary master is not an ATA disk\n");
        return;
    }
    if (ata_wait_drq() < 0) {
        kprintf("ata: IDENTIFY failed\n");
        return;
    }

    ata_insw(id, 256);
    ata_extract_model(id, model);

    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    ata_present = 1;
    kprintf("ata: primary master: %s, %u sectors (%u MiB)\n",
            model, sectors, sectors / 2048);

    /* IDENTIFY word 60/61 is the 28-bit sector count; a drive that
     * reports more than LBA28 can address is clamped, not trusted. */
    if (sectors > ATA_LBA28_MAX)
        sectors = ATA_LBA28_MAX;

    strncpy(ata_bdev.name, "hda", sizeof(ata_bdev.name) - 1);
    ata_bdev.block_size = ATA_SECTOR_SIZE;
    ata_bdev.blocks = sectors;
    ata_bdev.priv = NULL;
    ata_bdev.read = ata_bd_read;
    ata_bdev.write = ata_bd_write;
    blockdev_register(&ata_bdev);
}
