# KestrelOS network stack

From-scratch network stack: PCI enumeration, RTL8139 and Intel e1000 NIC
drivers, and an Ethernet / ARP / IPv4 / ICMP / UDP / TCP stack with a small
DNS resolver.

## Files

| File | Purpose |
|------|---------|
| `kernel/pci.c`, `kernel/include/pci.h` | PCI config space (ports 0xCF8/0xCFC), full bus scan, device lookup, bus-master enable |
| `kernel/include/netdev.h` | Tiny NIC abstraction: name, MAC, `send`, `poll`; one registered device |
| `kernel/rtl8139.c`, `kernel/include/rtl8139.h` | RTL8139 driver: reset, RX ring, 4 TX descriptors, IRQ + polling |
| `kernel/e1000.c`, `kernel/include/e1000.h` | Intel 8254x ("e1000") driver: MMIO registers, RX/TX descriptor rings, IRQ + polling |
| `kernel/net.c`, `kernel/include/net.h` | Ethernet demux, ARP (cache + resolve + reply), IPv4 build/parse, ICMP echo both directions |
| `kernel/dhcp.c`, `kernel/include/dhcp.h` | One-shot DHCP client: DISCOVER/OFFER/REQUEST/ACK auto-configuration |
| `kernel/udp.c` | UDP send/recv, 16 port bindings with 8-packet queues |
| `kernel/tcp.c`, `kernel/include/tcp.h` | TCP state machine, retransmission, sliding window, HTTP client foundation |
| `kernel/dns.c` | A and AAAA resolver (compression-pointer aware) + dotted-quad / IPv6 literal parsing |
| `kernel/net6.c` | IPv6 parse/build, ICMPv6 echo, Neighbor Discovery (NS/NA/RS/RA, neighbor cache), SLAAC + RDNSS |
| `abi/ip6_text.h` | Header-only IPv6 text parse/format (RFC 4291 input, RFC 5952 output) shared by kernel, libc and host tests |

## Architecture

```
                 task context                     IRQ context
  udp_send/dns/ping ──> net_ip_send ──┐    NIC IRQ ──> rx_drain ──> net_rx
                          │           │                               │
                    arp_resolve       │                 ┌── ARP ──────┤
                   (wait + retry)     v                 │  (reply,    v
                          └────> netdev->send <── ICMP echo reply   IPv4 demux
                                  (TX descr.)               ICMP / UDP / TCP
```

## NIC drivers

Drivers sit behind the `struct netdev` abstraction (`kernel/include/netdev.h`):
a driver probes its hardware in its `*_init()` and, on success, registers a
`netdev` carrying its name, MAC and `send`/`poll` entry points. `net_init()`
tries the RTL8139 first, then the e1000; the rest of the stack only ever
talks to `netdev_current()`. If neither probe succeeds, networking is
disabled ("net: no NIC found").

* **RTL8139** (`kernel/rtl8139.c`) — I/O-port register file; QEMU emulates
  it (`-device rtl8139`) but VirtualBox/VMware do not. Single contiguous
  RX ring, 4 round-robin TX descriptors.
* **Intel e1000** (`kernel/e1000.c`) — the 8254x family: 82540EM (0x100E,
  what QEMU's `-device e1000`, VirtualBox and VMware emulate), 82545EM
  (0x100F), 82574L (0x10D3) and I217 (0x153A). MMIO register file: BAR0 is
  a memory BAR whose 128 KiB window lives above RAM, so it is mapped
  page-by-page at `0xFFFFFFFFC0000000` with caching disabled (PCD). The
  MAC comes from RAL/RAH when the hypervisor preloads them, else from the
  EEPROM via EERD (both DONE-bit/address-shift variants are auto-detected).
  32 RX + 16 TX legacy descriptors, one 2 KiB buffer each, allocated
  physically contiguous with `pmm_alloc_contig`. RCTL strips the CRC
  (SECRC) and accepts unicast-match + broadcast only (no promiscuous
  mode). The IRQ handler reads ICR (which acks it), tolerates spurious
  entry (shared level-triggered PCI lines), and drains the RX ring.

* **RX path runs inside the IRQ handler.** The ISR acks by writing the ISR
  bits back, drains the RX ring, and hands each frame to `net_rx()`. This
  path never sleeps: it may transmit short replies (ARP reply, ICMP echo
  reply) synchronously, and queues UDP payloads for tasks to pick up.
  Queue slot buffers are preallocated at bind time in task context, so the
  IRQ path never calls `kmalloc`.
* **TX is synchronous** from task context (and for the small IRQ-context
  replies): copy into the driver's next TX buffer (4 fixed DMA pages on
  the RTL8139, 16 ring descriptors on the e1000), kick the doorbell
  register, busy-wait only if the descriptor is still owned by hardware.
* **RTL8139 RX ring** is `8192 + 16 + 1500` bytes with the RCR WRAP bit set, so a
  frame crossing the 8 KiB boundary continues linearly into the slack
  area — no wraparound copying. `CAPR` is advanced to `rxpos - 16` with
  4-byte alignment after each packet.
* **ARP**: 16-entry cache. `arp_resolve()` (task context) sends requests
  and waits ~1 s with 3 retries; off-subnet destinations resolve the
  gateway MAC instead. The stack also learns the source MAC of every
  received ARP and IPv4 packet, which is why ICMP echo replies from IRQ
  context never block on ARP.
* **IPv4**: ihl=5 on send, ttl 64, header checksum generated and
  verified. Fragments are dropped (no reassembly). ICMP (1), TCP (6), and
  UDP (17) are demultiplexed.
* **ICMP**: answers inbound echo requests (the host can ping the VM).
  `icmp_ping()` sends a 32-byte-payload echo request and reports RTT with
  10 ms granularity (PIT tick).
* **UDP**: 16 port bindings, 8 queued packets per port, drop-on-full.
  `udp_recv()` binds the port on the fly if needed. Transmit checksums include
  the IPv4 pseudo-header; receive drops corrupt checksummed datagrams while
  retaining IPv4 compatibility with peers that explicitly send checksum 0.
* **DNS**: single A query with RD=1 to the configured server, source port
  `0xC000 + counter`, 2 tries with 1 s timeout each, handles name
  compression pointers, takes the first A answer. Dotted-quad strings are
  parsed directly without a query.

## Configuration

Dynamic DHCP configuration on boot with fallback to QEMU / VirtualBox NAT static defaults (`10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`).

IPv4 addresses in the API are big-endian (network order) `uint32_t`.

IPv6 configures itself after IPv4: a link-local `fe80::/64` address from the
modified EUI-64 of the MAC, then up to three Router Solicitations. The first
autonomous `/64` prefix in a Router Advertisement gives a SLAAC global address,
the advertising router becomes the default route and an RDNSS option gives an
IPv6 DNS server. On QEMU user networking that is `fec0::5054:ff:fe12:3456/64`,
router `fe80::2`, DNS `fec0::3`; the host is reachable at `fec0::2` (slirp
connects that to the host's `::1`). IPv6 addresses cross the syscall ABI as
16 network-order bytes (`SYS_NETINFO6`, `SYS_DNS6`, `SYS_PING6`,
`SYS_TCP_CONNECT6`, `SYS_UDP_SEND6`). In userspace, `net_resolve()` tries an
IPv4 literal, an IPv6 literal, DNS A, then AAAA, and `net_tcp_connect()`
connects to either family, which is what ping, telnet, curl, the HTTP and TLS
clients and the browser use; URLs take bracketed literals
(`http://[fec0::2]:8080/`).

## QEMU flags

```
-device rtl8139,netdev=n0 -netdev user,id=n0
```

or, with the Intel NIC (also what VirtualBox/VMware provide by default):

```
-device e1000,netdev=n0 -netdev user,id=n0
```

Slirp enables IPv4 and IPv6 by default. Naming only one of them turns the
other off, so `-netdev user,id=n0,ipv6=on` alone disables IPv4; use
`ipv4=on,ipv6=on` (as `tools/e2e.py` does) or neither.

To reach a UDP service in the guest from the host, add a hostfwd, e.g.
`-netdev user,id=n0,hostfwd=udp::7777-:7777`. To let the host ping the
guest, note that slirp does not forward inbound ICMP; inbound echo works
from other guests on a shared socket/tap network. Outbound
ping/UDP/DNS works out of the box.

## What works

* PCI bus scan with device log at boot
* RTL8139 and Intel e1000 (82540EM) NIC support (QEMU & VirtualBox)
* Automatic IP configuration via DHCP with static fallback
* Outbound ICMP ping (`icmp_ping`) to the gateway/DNS or beyond
* Answering inbound ICMP echo requests and ARP requests
* UDP send/recv with port queues
* DNS A and AAAA resolution, over IPv4 or IPv6 DNS servers
* IPv6: link-local + SLAAC addressing, Neighbor Discovery, ICMPv6 echo both
  directions (`ping fec0::2`, `ping -6 <name>`), UDP and TCP over IPv6
* Client TCP with bounded out-of-order receive reassembly and negotiated SACK
* HTTP/1.1 plus verified TLS 1.3 HTTPS clients

## Limits

* No IP fragmentation/reassembly; frames limited to a 1500-byte MTU.
* One NIC, one IP; RTL8139 or e1000 (RTL8139 wins if both are present).
* TCP is client-side only. It advertises SACK-Permitted, emits up to four
  receive SACK blocks, prunes acknowledged transmit blocks, and selectively
  retransmits holes; it does not implement congestion-control algorithms
  beyond the bounded window/retransmission logic in `tcp.c`.
* ARP and neighbor cache entries never expire (fine for slirp's static world).
* IPv6: one SLAAC address (first autonomous /64 prefix), optimistic DAD,
  no fragments, no MLD reports (NICs accept all multicast instead), no
  privacy addresses, no DHCPv6; RA lifetimes other than router lifetime 0
  are ignored.
* On an RX ring error the receiver is reset and any queued frames in the
  ring are lost (never observed under QEMU in practice).
