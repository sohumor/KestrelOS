# Kernel entropy and random numbers

The random subsystem follows this pipeline:

```text
RDSEED / RDRAND
timer interrupt jitter
keyboard timing
mouse timing
disk/network/serial interrupt timing
bootloader seed
        |
        v
 entropy_pool_add()
        |
        v
 SHA-256 mixing pool
        |
        v
 ChaCha20 kernel CSPRNG
        |
        +-- getrandom() syscall
        +-- /dev/urandom
        `-- /dev/random
```

Every source has a separate domain identifier in the SHA-256 transcript.
RDSEED is preferred; RDRAND is the hardware fallback. Timing inputs mix the
TSC sample, first and second differences, PIT ticks, source ID, and event
details. Stage 2 supplies an early boot seed before the kernel has drivers.

The pool tracks a conservative initialization credit capped at 256 bits.
All public random interfaces wait for 128 credited bits; a nonblocking
`getrandom()` request fails until then. `/dev/random`, `/dev/urandom`, and
ordinary `getrandom()` use the same initialized CSPRNG. “Strong” reads do not
deplete a counter after initialization.

Reseeding derives a new key and nonce from the old key, the pool, and its
generation counter. Output is generated with ChaCha20. After each request an
unseen block replaces the live key and nonce (fast key erasure), so recovering
the current state does not directly reveal earlier output.

`tools/run-random-tests.sh` checks SHA-256 and ChaCha20 known-answer vectors,
chunking, deterministic replay, output separation, and state evolution.

This is security-oriented code, but it is still a small educational kernel and
has not had an external cryptographic audit. Machines without RDSEED/RDRAND
depend on the unpredictability of their collected timing events.
