# Networking on KestrelOS

The kernel drives an RTL8139 NIC and speaks ARP, IPv4, ICMP, UDP, DNS
and TCP. Addresses come from DHCP at boot; `sysinfo` and `nslookup`
show what was learned.

    ping <host|ip>        ICMP echo, round-trip time in milliseconds
    nslookup <name>       resolve a name through the configured DNS server
    udp <host> <port>     send a datagram
    wget <url>            download over HTTP

There is no TLS: `https://` URLs are refused with a clear message rather
than a confusing connection error. Plain `http://` works, including
redirects, chunked transfer-encoding and non-default ports.

Programs reach the stack through the syscalls listed in `docs/ABI.md`:
`SYS_DNS`, `SYS_PING`, `SYS_UDP_SEND`, `SYS_UDP_RECV`, `SYS_NETINFO` and
`SYS_TCP_CONNECT` / `SEND` / `RECV` / `CLOSE`. The libc header
`<http.h>` wraps the TCP calls in a one-shot `http_get()`.
