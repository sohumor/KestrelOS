# Welcome to KestrelOS

KestrelOS is a from-scratch x86-64 hobby operating system: bootloader,
kernel, drivers, filesystem, network stack, libc and every userspace
program were written for this project -- no external code anywhere.

## A quick tour

Try these at the `kestrel:/$ ` prompt:

    help                    every command, one line each
    ls                      list the current directory
    cd /doc                 change directory (cd .. goes up)
    cat welcome.md          print this file
    ps                      what is running right now
    free                    memory usage
    uptime                  time since boot
    sysinfo                 full system overview

## Files

    mkdir notes             make a directory
    writefile notes/a.txt   type text, finish with ctrl-D
    hexdump notes/a.txt     see the raw bytes
    edit notes/a.txt        full-screen editor
    rm notes/a.txt          remove it

## Network

    nslookup example.com    DNS lookup
    ping 1.1.1.1            ICMP round trip
    udp                     raw UDP send/receive

## Fun

    snake                   you know what this is

The shell resolves commands from /bin; paths can be absolute or
relative to the current directory. See /etc/version for the release.
