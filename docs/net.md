# KestrelOS network stack

From-scratch network stack: PCI enumeration, RTL8139 NIC driver, and an
Ethernet / ARP / IPv4 / ICMP / UDP stack with a small DNS resolver.

## Files

| File | Purpose |
|------|---------|
| `kernel/pci.c`, `kernel/include/pci.h` | PCI config space (ports 0xCF8/0xCFC), full bus scan, device lookup, bus-master enable |
| `kernel/rtl8139.c`, `kernel/include/rtl8139.h` | RTL8139 driver: reset, RX ring, 4 TX descriptors, IRQ + polling |
| `kernel/net.c`, `kernel/include/net.h` | Ethernet demux, ARP (cache + resolve + reply), IPv4 build/parse, ICMP echo both directions |
| `kernel/udp.c` | UDP send/recv, 16 port bindings with 8-packet queues |
| `kernel/dns.c` | A-record resolver (compression-pointer aware) + dotted-quad parsing |

## Architecture

```
                 task context                     IRQ context
  udp_send/dns/ping ──> net_ip_send ──┐    NIC IRQ ──> rx_drain ──> net_rx
                          │           │                               │
                    arp_resolve       │                 ┌── ARP ──────┤
                   (wait + retry)     v                 │  (reply,    v
                          └────> rtl8139_send <── ICMP echo reply   IPv4 demux
                                  (TX descr.)                    ICMP / udp_input
```

* **RX path runs inside the IRQ handler.** The ISR acks by writing the ISR
  bits back, drains the RX ring, and hands each frame to `net_rx()`. This
  path never sleeps: it may transmit short replies (ARP reply, ICMP echo
  reply) synchronously, and queues UDP payloads for tasks to pick up.
  Queue slot buffers are preallocated at bind time in task context, so the
  IRQ path never calls `kmalloc`.
* **TX is synchronous** from task context (and for the small IRQ-context
  replies): copy into one of 4 fixed DMA pages, kick the TSD register,
  busy-wait only if the descriptor is still owned by hardware.
* **RX ring** is `8192 + 16 + 1500` bytes with the RCR WRAP bit set, so a
  frame crossing the 8 KiB boundary continues linearly into the slack
  area — no wraparound copying. `CAPR` is advanced to `rxpos - 16` with
  4-byte alignment after each packet.
* **ARP**: 16-entry cache. `arp_resolve()` (task context) sends requests
  and waits ~1 s with 3 retries; off-subnet destinations resolve the
  gateway MAC instead. The stack also learns the source MAC of every
  received ARP and IPv4 packet, which is why ICMP echo replies from IRQ
  context never block on ARP.
* **IPv4**: ihl=5 on send, ttl 64, header checksum generated and
  verified. Fragments are dropped (no reassembly). Only ICMP (1) and
  UDP (17) are demuxed.
* **ICMP**: answers inbound echo requests (the host can ping the VM).
  `icmp_ping()` sends a 32-byte-payload echo request and reports RTT with
  10 ms granularity (PIT tick).
* **UDP**: 16 port bindings, 8 queued packets per port, drop-on-full.
  `udp_recv()` binds the port on the fly if needed. TX checksum is 0
  (legal for UDP over IPv4).
* **DNS**: single A query with RD=1 to the configured server, source port
  `0xC000 + counter`, 2 tries with 1 s timeout each, handles name
  compression pointers, takes the first A answer. Dotted-quad strings are
  parsed directly without a query.

## Configuration

Static, set in `net_init()` to the QEMU user-mode ("slirp") defaults:

* IP `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`

All addresses in the API are big-endian (network order) `uint32_t`.

## QEMU flags

```
-device rtl8139,netdev=n0 -netdev user,id=n0
```

To reach a UDP service in the guest from the host, add a hostfwd, e.g.
`-netdev user,id=n0,hostfwd=udp::7777-:7777`. To let the host ping the
guest, note that slirp does not forward inbound ICMP; inbound echo works
from other guests on a shared socket/tap network. Outbound
ping/UDP/DNS works out of the box.

## What works

* PCI bus scan with device log at boot
* Outbound ICMP ping (`icmp_ping`) to the gateway/DNS or beyond
* Answering inbound ICMP echo requests and ARP requests
* UDP send/recv with port queues
* DNS A-record resolution against 10.0.2.3

## Limits

* No TCP.
* No DHCP — addressing is hardcoded to QEMU slirp defaults.
* No IP fragmentation/reassembly; frames limited to a 1500-byte MTU.
* No checksum on transmitted UDP (checksum 0 — allowed for IPv4).
* One NIC, one IP; RTL8139 only.
* ARP cache entries never expire (fine for slirp's static world).
* On an RX ring error the receiver is reset and any queued frames in the
  ring are lost (never observed under QEMU in practice).
