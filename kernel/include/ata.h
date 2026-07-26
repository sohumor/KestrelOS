#pragma once

#include <stdint.h>

/* ATA PIO, primary master. 28-bit LBA. Sector = 512 bytes. */

#define FS_START_LBA 2048   /* KFS partition offset on the boot disk */

void ata_init(void);
int  ata_read(uint32_t lba, uint32_t count, void *buf);
int  ata_write(uint32_t lba, uint32_t count, const void *buf);
