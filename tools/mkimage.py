#!/usr/bin/env python3
"""Assemble the KestrelOS bootable disk image.

Layout (512-byte sectors):
  LBA 0        stage 1 (MBR)
  LBA 1-63     stage 2
  LBA 64-2047  kernel flat binary (max 992 KiB)
  LBA 2048+    KFS filesystem (or zeros if no fs image given)
  after KFS    16 MiB KSWAP01 extent (one header page + data pages)
"""
import sys
import os

SECTOR = 512
STAGE2_SECTORS = 63
KERNEL_LBA = 64
FS_LBA = 2048
DEFAULT_FS_SIZE = 32 * 1024 * 1024
SWAP_SIZE = 16 * 1024 * 1024
SWAP_HEADER_SIZE = 4096


def main():
    if len(sys.argv) < 5:
        print("usage: mkimage.py stage1.bin stage2.bin kernel.bin out.img [fs.img]")
        return 1
    stage1_path, stage2_path, kernel_path, out_path = sys.argv[1:5]
    fs_path = sys.argv[5] if len(sys.argv) > 5 else None

    with open(stage1_path, "rb") as f:
        stage1 = f.read()
    with open(stage2_path, "rb") as f:
        stage2 = f.read()
    with open(kernel_path, "rb") as f:
        kernel = f.read()

    if len(stage1) != SECTOR:
        print(f"error: stage1 must be exactly {SECTOR} bytes, got {len(stage1)}")
        return 1
    if len(stage2) > STAGE2_SECTORS * SECTOR:
        print(f"error: stage2 too big ({len(stage2)} > {STAGE2_SECTORS * SECTOR})")
        return 1
    max_kernel = (FS_LBA - KERNEL_LBA) * SECTOR
    if len(kernel) > max_kernel:
        print(f"error: kernel too big ({len(kernel)} > {max_kernel})")
        return 1

    img = bytearray()
    img += stage1
    img += stage2.ljust(STAGE2_SECTORS * SECTOR, b"\0")
    img += kernel
    img += b"\0" * (FS_LBA * SECTOR - len(img))

    if fs_path and os.path.exists(fs_path):
        with open(fs_path, "rb") as f:
            img += f.read()
    else:
        img += b"\0" * DEFAULT_FS_SIZE

    swap_pages = (SWAP_SIZE - SWAP_HEADER_SIZE) // 4096
    header = bytearray(SWAP_HEADER_SIZE)
    header[0:8] = b"KSWAP01\0"
    header[8:12] = (1).to_bytes(4, "little")
    header[12:16] = swap_pages.to_bytes(4, "little")
    img += header
    img += b"\0" * (swap_pages * 4096)

    with open(out_path, "wb") as f:
        f.write(img)

    ksec = (len(kernel) + SECTOR - 1) // SECTOR
    print(f"mkimage: {out_path}: {len(img)} bytes "
          f"(kernel {len(kernel)} bytes = {ksec} sectors)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
