# Firmware-neutral boot resources

The loader enters the x86-64 kernel in long mode with the handoff's
physical address in RDI and its byte length in RSI. The whole handoff
must fit in the initial direct map of the first 1 GiB. The BIOS stage-2
loader writes it at physical `0x6000`, translating E820 and VBE data
before entering the kernel.

Version 1 is little-endian, with a 48-byte header followed by 1–128
usable memory ranges. The layout is fixed:

| Offset | Format | Meaning |
| ---: | --- | --- |
| 0 | u32 | Magic `0x544F4F42` (bytes `BOOT`) |
| 4 | u16 | Version (1) |
| 6 | u16 | Header size (48) |
| 8 | u32 | Number of usable ranges |
| 12 | u32 | Flags: bit 0 means a framebuffer is present; other bits must be zero |
| 16 | u32 | Uncredited timing seed |
| 20 | u32 | Reserved, zero |
| 24 | u64 | Framebuffer physical address |
| 32, 36, 40, 44 | u32 each | Width, height, byte pitch, bits per pixel |
| 48 onward | u64 pair per entry | Physical base, exclusive end |

With the framebuffer flag set, the loader describes a linear
`0x00RRGGBB` framebuffer. Without it, the kernel ignores those fields.
The byte length is exactly 48 plus 16 times the count. The kernel
rejects unsupported versions, invalid lengths and invalid ranges before
initializing memory or graphics. The old unversioned BIOS block is not
accepted: the kernel and stage 2 are built and shipped together.

`boot_handoff_decode()` copies validated fields to the kernel-internal
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

A future loader can provide these resources without fabricating E820
or VBE records. This change does not add a second loader or move
platform discovery out of the kernel.
