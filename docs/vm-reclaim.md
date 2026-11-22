# Tiered virtual memory reclaim

Under physical-memory pressure, a user fault can reclaim a cold page
from its own address space. The victim policy is unchanged: the VM
subsystem does not move a page from a running task on another CPU.

The selected page can leave RAM in three forms:

| Contents | Backing |
| --- | --- |
| Exactly zero | A page-table token with no payload or slot |
| Packs into 512 bytes | One of 128 preallocated compressed RAM slots |
| Does not fit in RAM backing | An ordinary slot in the disk swap extent |

If the RAM pool fills, pages that need a payload fall back to disk.
Zero pages still need no slot. The RAM tiers work without a swap
device. If the chosen victim needs a disk slot and none is available,
reclaim fails rather than losing the page. The disk layout and its
usage counters are unchanged. RAM slots use the same lossless word-run
codec as Hangar without changing the Hangar image format.

Faulting a page back in restores its original bytes and write
permission, then releases its backing. Task teardown also releases
both disk and RAM slots. Flight records distinguish zero-page and
compressed RAM activity from disk `swap_out` and `swap_in`.
