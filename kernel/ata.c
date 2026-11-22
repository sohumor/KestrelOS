#include "kernel.h"
#include "ata.h"
#include "blockdev.h"
#include "device.h"
#include "io.h"
#include "string.h"
#include "random.h"
#include "spinlock.h"

/* ATA PIO driver: two compatibility channels, 28-bit LBA, polling. */

#define ATA_SECTOR_SIZE 512
#define ATA_LBA28_SECTORS (1u << 28)
#define ATA_REG_DATA     0
#define ATA_REG_COUNT    2
#define ATA_REG_LBA_LO   3
#define ATA_REG_LBA_MID  4
#define ATA_REG_LBA_HI   5
#define ATA_REG_DRIVE    6
#define ATA_REG_STATUS   7
#define ATA_REG_CMD      7
#define ATA_ALTSTAT_OFFSET 0x206

#define ATA_SR_BSY  0x80
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_TIMEOUT 1000000

struct ata_disk {
    struct blockdev bd;
    struct ata_channel *channel;
    uint8_t select;
};

struct ata_channel {
    spinlock_t lock;
    uint16_t io;
    struct ata_disk disks[2];
};

static struct ata_channel channels[2];

/* >= 400ns settle time after a drive select. */
static void ata_delay_400ns(const struct ata_channel *ch)
{
    for (int i = 0; i < 4; i++)
        inb(ch->io + ATA_ALTSTAT_OFFSET);
}

static int ata_wait_not_busy(const struct ata_channel *ch)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(ch->io + ATA_REG_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

/* Wait for DRQ; fail on ERR/DF or timeout. */
static int ata_wait_drq(const struct ata_channel *ch)
{
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ch->io + ATA_REG_STATUS);
        if (st & ATA_SR_BSY)
            continue;
        if (st & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (st & ATA_SR_DRQ)
            return 0;
    }
    return -1;
}

/* Select the target with LBA mode and the top 4 LBA bits, program the
 * count/LBA registers and issue a command. count 256 is sent as 0. */
static int ata_setup_cmd(const struct ata_disk *disk, uint32_t lba,
                         uint32_t count, uint8_t cmd)
{
    const struct ata_channel *ch = disk->channel;

    if (inb(ch->io + ATA_REG_STATUS) != 0xff && ata_wait_not_busy(ch) < 0)
        return -1;
    outb(ch->io + ATA_REG_DRIVE, disk->select | ((lba >> 24) & 0x0F));
    ata_delay_400ns(ch);
    if (ata_wait_not_busy(ch) < 0)
        return -1;
    outb(ch->io + ATA_REG_COUNT, (uint8_t)count);
    outb(ch->io + ATA_REG_LBA_LO, (uint8_t)lba);
    outb(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ch->io + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));
    outb(ch->io + ATA_REG_CMD, cmd);
    return 0;
}

static void ata_insw(const struct ata_channel *ch, uint16_t *buf, int words)
{
    for (int i = 0; i < words; i++)
        buf[i] = inw(ch->io + ATA_REG_DATA);
}

static void ata_outsw(const struct ata_channel *ch, const uint16_t *buf, int words)
{
    for (int i = 0; i < words; i++)
        outw(ch->io + ATA_REG_DATA, buf[i]);
}

/* One READ/WRITE SECTORS command, count <= 256. */
static int ata_rw_chunk(const struct ata_disk *disk, uint32_t lba,
                        uint32_t count, void *buf, int write)
{
    const struct ata_channel *ch = disk->channel;
    uint16_t *p = buf;

    if (ata_setup_cmd(disk, lba, count, write ? ATA_CMD_WRITE : ATA_CMD_READ) < 0)
        return -1;

    for (uint32_t s = 0; s < count; s++) {
        if (ata_wait_drq(ch) < 0)
            return -1;
        if (write)
            ata_outsw(ch, p, 256);
        else
            ata_insw(ch, p, 256);
        p += 256;
        ata_delay_400ns(ch);
    }

    if (write) {
        if (ata_wait_not_busy(ch) < 0)
            return -1;
        outb(ch->io + ATA_REG_CMD, ATA_CMD_FLUSH);
        ata_delay_400ns(ch);
        if (ata_wait_not_busy(ch) < 0 ||
            (inb(ch->io + ATA_REG_STATUS) & (ATA_SR_ERR | ATA_SR_DF)))
            return -1;
    }
    entropy_pool_add_interrupt(ENTROPY_DISK,
                               (lba << 9) ^ count ^ (write ? 0x80000000u : 0));
    return 0;
}

static int ata_rw(struct blockdev *bd, uint64_t lba, uint32_t count,
                  void *buf, int write)
{
    const struct ata_disk *disk = bd->priv;
    struct ata_channel *ch = disk->channel;
    if (lba >= bd->blocks || count > bd->blocks - lba)
        return -1;

    uint64_t flags = spin_lock_irqsave(&ch->lock);
    while (count > 0) {
        uint32_t chunk = count > 256 ? 256 : count;
        if (ata_rw_chunk(disk, (uint32_t)lba, chunk, buf, write) < 0) {
            spin_unlock_irqrestore(&ch->lock, flags);
            return -1;
        }
        lba += chunk;
        count -= chunk;
        buf = (uint8_t *)buf + chunk * ATA_SECTOR_SIZE;
    }
    spin_unlock_irqrestore(&ch->lock, flags);
    return 0;
}

/* --- block device face ------------------------------------------------
 * One block = one 512-byte sector, so the LBA needs no scaling. The
 * command registers only carry 28 bits of LBA, so anything above that is
 * refused here rather than silently truncated into the wrong sector. */

static int ata_bd_read(struct blockdev *bd, uint64_t lba, uint32_t count,
                       void *buf)
{
    return ata_rw(bd, lba, count, buf, 0);
}

static int ata_bd_write(struct blockdev *bd, uint64_t lba, uint32_t count,
                        const void *buf)
{
    return ata_rw(bd, lba, count, (void *)buf, 1);
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

static int ata_identify(const struct ata_disk *disk, uint16_t id[256])
{
    const struct ata_channel *ch = disk->channel;

    outb(ch->io + ATA_REG_DRIVE, disk->select);
    ata_delay_400ns(ch);
    if (inb(ch->io + ATA_REG_STATUS) == 0xff || ata_wait_not_busy(ch) < 0)
        return -1;

    outb(ch->io + ATA_REG_COUNT, 0);
    outb(ch->io + ATA_REG_LBA_LO, 0);
    outb(ch->io + ATA_REG_LBA_MID, 0);
    outb(ch->io + ATA_REG_LBA_HI, 0);
    outb(ch->io + ATA_REG_CMD, ATA_CMD_IDENTIFY);
    ata_delay_400ns(ch);
    uint8_t st = inb(ch->io + ATA_REG_STATUS);
    if (st == 0 || st == 0xff || ata_wait_not_busy(ch) < 0)
        return -1;
    /* Non-zero signature here means ATAPI/SATA, not plain ATA. */
    if (inb(ch->io + ATA_REG_LBA_MID) || inb(ch->io + ATA_REG_LBA_HI) ||
        ata_wait_drq(ch) < 0)
        return -1;
    ata_insw(ch, id, 256);
    ata_delay_400ns(ch);
    return 0;
}

static int ata_probe_disk(struct ata_channel *ch, int channel, int target)
{
    struct ata_disk *disk = &ch->disks[target];
    struct blockdev *bd = &disk->bd;
    uint16_t id[256];
    char model[41];

    disk->channel = ch;
    disk->select = 0xe0 | (target << 4);
    uint64_t flags = spin_lock_irqsave(&ch->lock);
    int result = ata_identify(disk, id);
    spin_unlock_irqrestore(&ch->lock, flags);
    if (result < 0)
        return -1;

    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (!sectors || !(id[49] & (1u << 9)) ||
        ((id[106] & 0xd000) == 0x5000 && (id[117] != 256 || id[118])))
        return -1;
    if (sectors > ATA_LBA28_SECTORS)
        sectors = ATA_LBA28_SECTORS;
    ata_extract_model(id, model);

    strcpy(bd->name, "hda");
    bd->name[2] += 2 * channel + target;
    bd->block_size = ATA_SECTOR_SIZE;
    bd->blocks = sectors;
    bd->priv = disk;
    bd->read = ata_bd_read;
    bd->write = ata_bd_write;
    if (blockdev_register(bd) < 0)
        return -1;
    kprintf("ata: %s %s: %s, %u sectors (%u MiB)\n",
            channel ? "secondary" : "primary", target ? "slave" : "master",
            model, sectors, sectors / 2048);
    return 0;
}

static int ata_probe(struct device *dev)
{
    const struct platform_devinfo *info = dev->bus_data;
    int index = strcmp(dev->name, "ata-primary") != 0;
    struct ata_channel *ch = &channels[index];

    if (!info || info->io_len != 8)
        return -1;
    ch->io = info->io_base;
    int found = 0;
    for (int i = 0; i < 2; i++)
        if (ata_probe_disk(ch, index, i) == 0)
            found++;
    if (!found)
        return -1;
    dev->drv_data = ch;
    return 0;
}

void ata_init(void)
{
    static const char *const names[] = { "ata-primary", "ata-secondary", NULL };
    static struct driver ata_driver = {
        .name = "ata-pio",
        .probe = ata_probe,
        .match_table = names,
    };

    ata_driver.bus = bus_find("platform");
    if (ata_driver.bus)
        driver_register(&ata_driver);
}
