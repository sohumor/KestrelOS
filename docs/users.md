# Accounts, passwords and permissions

KestrelOS has real user accounts: every task carries a uid and a gid, the
VFS checks them against each inode's `mode`/`uid`/`gid`, and the account
database lives in three flat files under `/etc`. This document describes
those files, the password hash, what the hash does and does not protect
against, and how to add or remove an account.

Ground truth for the syscalls involved is `abi/kestrel_abi.h`:
`SYS_GETUID`, `SYS_GETGID`, `SYS_SETUID`, `SYS_CHMOD`, `SYS_CHOWN`.

## Shipped accounts

| user | uid | gid | password | home | shell |
|---|---|---|---|---|---|
| `root` | 0 | 0 | `root` | `/root` | `/bin/sh` |
| `kestrel` | 1000 | 1000 | `kestrel` | `/home/kestrel` | `/bin/sh` |

**The passwords are published on purpose.** This is a demo operating
system that people boot in QEMU to look at; a secret default password
would only get in the way. `login` prints both of them in its banner.

Home directories are created on first login if they do not exist yet:
`login` runs `mkdir` and `chown` while it is still root, before it drops
to the account's uid.

## The files

### `/etc/passwd` — mode 0644, root:root

```
username:uid:gid:realname:home:shell
```

```
root:0:0:System Administrator:/root:/bin/sh
kestrel:1000:1000:Kestrel Demo User:/home/kestrel:/bin/sh
```

World-readable, because every tool that turns a uid into a name has to
read it. It holds no secrets.

### `/etc/shadow` — mode 0600, root:root

```
username:salt:iterations:hash
```

```
root:9f2c4a1b7d3e6058:4096:0fac828111526c0c7547e435a0c74b813b6d4ed67144cff6d754a804cdcf0473
```

Readable only by root. This is the whole of the access control on the
hashes: there is no setuid bit on KestrelOS (see the note below), so an
unprivileged process simply cannot open the file.

### `/etc/group` — mode 0644, root:root

```
groupname:gid:members
```

```
root:0:root
wheel:10:root
users:100:kestrel
kestrel:1000:kestrel
```

`members` is a comma-separated list of usernames and may be empty. An
account belongs to its primary group (the gid in `/etc/passwd`) whether
or not it is named in that group's member list.

### Parsing rules

All three files are parsed the same way by every tool:

* one record per line, fields separated by `:`;
* blank lines and lines starting with `#` are ignored;
* a trailing `\r` is stripped, so a file edited on a host is still read
  correctly;
* a line with too few fields is skipped rather than half-interpreted;
* a username may not contain `:`, and neither may a password.

A record whose `home` or `shell` is not an absolute path is repaired at
login time (to `/` and `/bin/sh`), and a record with a negative uid is
rejected. A missing or unreadable file is treated as "no accounts",
never as an error worth crashing over.

## The password hash

Implemented in `libc/sha256.c` (`sha256_password()`) and mirrored exactly
by `tools/mkpasswd.py`:

```
d = SHA256(salt || password)                  # round 1
repeat iterations-1 times: d = SHA256(salt || d)
hash = lowercase hex of d
```

so `iterations` SHA-256 invocations in total. `salt` is the ASCII hex
string from the record, hashed as text; `password` is the raw bytes of
the password. `iterations` is 4096 everywhere we generate entries: slow
enough to be a visible cost per guess, fast enough that logging in on a
486-era emulated machine is not annoying.

SHA-256 itself is written from FIPS 180-4 in `libc/sha256.c` — the eight
initial hash values (fractional parts of the square roots of the first
eight primes), the 64 round constants (cube roots of the first 64
primes), the message schedule, `Ch`/`Maj`/the four sigma functions, and
the 0x80 / zero / 64-bit-length padding. It is checked against the three
published test vectors (the empty string, `"abc"`, and the 448-bit
`"abcdbcde…nopq"` message).

The salt is 16 hex digits (8 bytes) drawn from `/dev/random`, backed by the
kernel SHA-256 entropy pool and ChaCha20 CSPRNG. If the device cannot be
opened, the tools retain a last-resort time/uptime/pid fallback so account
management still reports a usable error path on a damaged `/dev` mount.

## What this protects against, and what it does not

Be clear-eyed about this. The scheme protects against **casual reading**:
someone who dumps the disk image, or who gets a look at `/etc/shadow`,
does not immediately learn everybody's password, and two accounts that
share a password do not have the same hash. That is all it is for.

It is **not** a real password-hashing construction:

* **The CSPRNG has not been independently audited.** It mixes RDSEED/RDRAND,
  the bootloader seed, and interrupt timing through SHA-256, then generates
  with ChaCha20 and fast key erasure. That is a real CSPRNG design, but its
  entropy estimates and implementation have not received the adversarial
  review expected of a production kernel. See `docs/random.md`.
* **4096 rounds of SHA-256 is cheap.** A real construction (scrypt,
  Argon2, bcrypt) is deliberately memory-hard so that attacking it on a
  GPU is not thousands of times cheaper than checking it. Iterated
  SHA-256 has none of that; it is exactly the shape of thing a GPU eats.
* **There is no login rate limit that survives a reboot.** `login` waits
  two seconds after a bad password and starts over after three, which
  slows a human down and nothing else.
* **There is no privilege boundary around the checking code.** See the
  next section.

If you want to store something that actually matters, do not store it
here.

## No setuid bit

`abi/kestrel_abi.h` defines only the nine permission bits; there is no
setuid bit, and `SYS_SETUID` succeeds only for a caller that is already
root. That has consequences worth stating plainly:

* `login` works because init runs it as root: it verifies the password,
  *then* drops to the account's uid, and can never climb back.
* `su` can drop privilege (root becoming somebody else) but can never
  raise it. Run from an unprivileged shell it fails, and says so.
* `passwd` can only be completed by root, because `/etc/shadow` is mode
  0600 and there is no way for an unprivileged program to gain the right
  to write it. An unprivileged run still checks the old password (and
  fails at the file, with an explanation).

This is a deliberate simplification, not an oversight: a setuid bit is a
privilege-escalation surface, and the kernel would need to be much more
careful about argument handling and inherited state before it earned one.

## The tools

| command | what it does |
|---|---|
| `login` | banner, username, password (no echo), `setuid`, `exec` the shell |
| `su [user]` | password check, then a nested shell as `user` (default `root`) |
| `passwd [user]` | change a password; root may change anyone's |
| `useradd [-u UID] [-g GID] [-r REALNAME] [-h HOME] [-s SHELL] name` | create an account (root only) |
| `userdel [-r] name` | remove an account, and with `-r` its home (root only) |
| `whoami` | the current account's name |
| `id [user]` | `uid=…(…) gid=…(…) groups=…` |
| `groups [user]` | the group names an account belongs to |
| `lsusers [-g]` | list `/etc/passwd`, or `/etc/group` with `-g` |
| `chmod [-v] MODE file…` | octal (`0755`) or symbolic (`u+x,go-w`) |
| `chown user[:group] file…` | names or numbers; `:group` alone changes the group |

Passwords are read with `read()` on fd 0 and echoed as nothing at all —
not even asterisks, which would leak the length to anyone watching.
Backspace and ctrl-U edit the buffer silently; ctrl-C and ctrl-D abandon
the prompt.

## Adding an account

From a root shell:

```
kestrel:/$ useradd -r "Ana Lovelace" ana
password:
retype password:
useradd: created ana (uid 1001, gid 1001, home /home/ana, shell /bin/sh)
```

`useradd` picks the next free uid at or above 1000, creates a group of
the same name, makes the home directory and hands it over with `chown`.
Nothing is written until both password prompts agree, so aborting at the
prompt leaves the account files untouched.

To remove it again, with its home directory:

```
kestrel:/$ userdel -r ana
userdel: removed ana (uid 1001)
```

`userdel` refuses uid 0, and refuses to recursively remove a "home" that
is not at least two path components deep — `/etc` is not a home
directory.

### By hand, or at build time

`tools/mkpasswd.py` generates shadow lines outside the running system:

```
$ python3 tools/mkpasswd.py ana hunter2
ana:1f0c9b3e77a25d84:4096:9c2f…

$ python3 tools/mkpasswd.py --ship
root:9f2c4a1b7d3e6058:4096:0fac…
kestrel:3ac7e18b56d02f94:4096:7c97…
```

`--ship` reprints the two entries committed in `rootfs/etc/shadow`, with
their salts pinned, so the shipped image is byte-for-byte reproducible.
Without `--salt` a fresh salt is drawn from `os.urandom`, so ordinary
runs produce a different line every time. Append the line to
`rootfs/etc/shadow`, add the matching `/etc/passwd` and `/etc/group`
records, and rebuild.

The image builder must be told about the mode of `/etc/shadow`:
`tools/mkfs.py` takes `--mode /etc/shadow:0600`. Without it the file
lands at the default 0644 and every user can read the hashes.
