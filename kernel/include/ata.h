#pragma once

#include <stdint.h>

/* ATA PIO, primary master. 28-bit LBA. Sector = 512 bytes.
 *
 * ata_init() probes the drive and, if it is there, registers it with the
 * block device layer as "hda" (512-byte blocks). Filesystems reach the
 * disk through that registration, not through the entry points below;
 * ata_read()/ata_write() remain for code that genuinely wants the raw
 * boot disk and for the driver's own use. */

#define ATA_SECTOR_SIZE 512

void ata_init(void);
int  ata_read(uint32_t lba, uint32_t count, void *buf);
int  ata_write(uint32_t lba, uint32_t count, const void *buf);
