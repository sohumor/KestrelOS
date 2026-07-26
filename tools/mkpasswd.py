#!/usr/bin/env python3
"""Generate KestrelOS /etc/shadow entries.

Usage:
  mkpasswd.py <username> <password> [--salt HEX] [--iterations N]
  mkpasswd.py --ship        emit the shipped rootfs/etc/shadow verbatim

A shadow line is

    username:salt:iterations:hash

and the hash is defined exactly as libc/sha256.c's sha256_password():

    d = SHA256(salt || password)            # salt is the ASCII hex string
    repeat iterations-1 times: d = SHA256(salt || d)
    hash = lowercase hex of d

so `iterations` SHA-256 invocations in total. Nothing here is a real
password-hashing construction: there is no per-invocation memory cost and
the salt on a live system comes from a non-cryptographic RNG. See
docs/users.md for the security limitations that follow from that.

Without --salt a fresh 16-hex-digit salt is drawn from os.urandom, so the
output changes every run; the shipped entries below pin their salts so the
committed rootfs/etc/shadow is byte-for-byte reproducible.
"""
import hashlib
import os
import sys

DEFAULT_ITERATIONS = 4096
SALT_HEX_DIGITS = 16

# Shipped demo accounts: (username, password, fixed salt).
# The passwords are deliberately public - this is a demo OS and they are
# printed in the login banner and in docs/users.md.
SHIPPED = [
    ("root", "root", "9f2c4a1b7d3e6058"),
    ("kestrel", "kestrel", "3ac7e18b56d02f94"),
]


def password_hash(salt, password, iterations):
    """Mirror of sha256_password() in libc/sha256.c."""
    if iterations < 1:
        iterations = 1
    s = salt.encode("ascii")
    d = hashlib.sha256(s + password.encode("ascii")).digest()
    for _ in range(iterations - 1):
        d = hashlib.sha256(s + d).digest()
    return d.hex()


def make_salt():
    return os.urandom(SALT_HEX_DIGITS // 2).hex()


def shadow_line(username, password, salt, iterations):
    return "%s:%s:%d:%s" % (username, salt, iterations,
                            password_hash(salt, password, iterations))


def emit_shipped():
    for username, password, salt in SHIPPED:
        print(shadow_line(username, password, salt, DEFAULT_ITERATIONS))
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        return 1
    if args[0] == "--ship":
        if len(args) != 1:
            print("mkpasswd: --ship takes no other arguments", file=sys.stderr)
            return 1
        return emit_shipped()

    salt = None
    iterations = DEFAULT_ITERATIONS
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--salt", "--iterations"):
            if i + 1 >= len(args):
                print("mkpasswd: %s needs an argument" % a, file=sys.stderr)
                return 1
            value = args[i + 1]
            i += 2
        elif a.startswith("--salt=") or a.startswith("--iterations="):
            a, value = a.split("=", 1)
            i += 1
        elif a.startswith("-"):
            print("mkpasswd: unknown option %s" % a, file=sys.stderr)
            return 1
        else:
            positional.append(a)
            i += 1
            continue
        if a == "--salt":
            salt = value
        else:
            try:
                iterations = int(value, 0)
            except ValueError:
                print("mkpasswd: --iterations must be a number", file=sys.stderr)
                return 1

    if len(positional) != 2:
        print("usage: mkpasswd.py <username> <password> "
              "[--salt HEX] [--iterations N]", file=sys.stderr)
        return 1
    username, password = positional
    if ":" in username or ":" in password:
        print("mkpasswd: ':' separates shadow fields and cannot appear in "
              "a username or password", file=sys.stderr)
        return 1
    if iterations < 1:
        print("mkpasswd: --iterations must be at least 1", file=sys.stderr)
        return 1
    if salt is None:
        salt = make_salt()
    salt = salt.lower()
    if not salt or any(c not in "0123456789abcdef" for c in salt):
        print("mkpasswd: --salt must be hex digits", file=sys.stderr)
        return 1

    print(shadow_line(username, password, salt, iterations))
    return 0


if __name__ == "__main__":
    sys.exit(main())
