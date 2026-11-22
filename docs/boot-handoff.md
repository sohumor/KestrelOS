# Firmware-neutral boot resources

The BIOS stage-2 loader still writes its packed E820 and VBE block at
physical `0x6000` and passes its address to the kernel. The memory
entries begin at byte 32. `bios_boot_decode()` validates the entry count
and range arithmetic, then copies the resources the kernel needs into
`struct boot_resources`.

- The physical page allocator receives usable byte ranges with exclusive
  ends. It aligns them to pages, applies its physical-address limit, and
  separately reserves the kernel and low memory.
- Graphics receives an optional linear framebuffer descriptor. It
  validates the mode and can keep using the text console if the
  framebuffer is absent or unusable.
- Early random initialization mixes the loader's timing seed and
  normalized resources without claiming entropy credit for them.
- SMP startup discovers processors independently; it does not consume the
  boot block.

These are kernel-internal types, not a loader ABI. A future loader can
provide the same resources through a versioned handoff without fabricating
E820 or VBE records. This change does not add a second loader or move
platform discovery out of the kernel.
