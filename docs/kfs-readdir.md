# Indexed directory enumeration regression

Run from the repository root (Python 3 and a native C compiler required):

```sh
python3 tools/test_kfs_readdir.py
python3 tools/test_kfs_readdir.py --source-ref 7e75333b4c5a8f96a5b35e347e1681a2e498fd6c --baseline
```

The harness compiles the actual KFS driver with a RAM block device and host
scheduler/flight-recorder stubs. Only the indexed readdir loop is instrumented
to count `readi` calls. Directory records use the real inode/direct/indirect
block format, with one free slot preceding each live entry. Full enumeration
includes the final EOF request. Setup and compilation are outside the timer;
milliseconds are host CPU time, not guest ticks or disk latency.

Measured on Ubuntu with `cc -O2`, against main at the revision above:

| Entries | Before (ms / scans) | After (ms / scans) | CPU-time speedup |
| ---: | ---: | ---: | ---: |
| 50 | 0.184 / 2,650 | 0.017 / 100 | 10.8x |
| 500 | 18.926 / 251,500 | 0.128 / 1,000 | 147.9x |
| 2000 | 378.521 / 4,006,000 | 0.552 / 4,000 | 685.7x |

Time varies across runs; scan counts are deterministic. Without `--baseline`,
the test asserts exactly twice as many scans as live entries (including holes),
and fails on main. It also checks every inode/name in order, EOF, rewind,
nonsequential access, same-size deletion/reinsertion with unchanged mtime,
transaction abort, an empty directory, invalid arguments, and a read failure.

The VFS interface supplies a path and ordinal, not an open directory handle.
KFS therefore keeps a bounded, lock-protected set of resume hints keyed by
filesystem and inode. A sequential request resumes at the next physical slot;
other requests rescan. Beginning any mutation transaction invalidates that
filesystem's hints; active transactions do not use or populate hints. Cache
invalidation on unmount/I/O failure also discards hints. This deliberately
conservative policy covers same-size modifications and inode/mount reuse
without depending on timestamps or changing the on-disk/VFS ABI. Interleaved
enumerations sharing a slot or writes between calls can lose the optimization,
but retain the original indexed semantics.
