#!/usr/bin/env python3
"""Check src/shfolderproxy/rsa_raw.c against Python's pow() on random RSA keys.

Builds tests/test_rsa.c for the host (with ASan/UBSan) and compares raw
private-key decryption (c^d mod n) for 512-, 1024- and 2048-bit keys with
e = 65537 and e = 3, including edge inputs (0, 1, 2, n-1) and inputs shorter
than the modulus.

    tests/test_rsa.py
"""
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "clang")


def is_prime(n):
    if n < 2:
        return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(24):
        x = pow(random.randrange(2, n - 1), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def prime(bits, e):
    while True:
        p = random.getrandbits(bits) | (1 << (bits - 1)) | (1 << (bits - 2)) | 1
        if is_prime(p) and (p - 1) % e:
            return p


def private_blob(bits, e, n, p, q):
    """BCRYPT_RSAKEY_BLOB ("RSA2") + exponent, modulus, prime1, prime2 (big-endian)."""
    k, half = (n.bit_length() + 7) // 8, bits // 16
    eb = e.to_bytes((e.bit_length() + 7) // 8, "big")
    header = b"".join(x.to_bytes(4, "little") for x in (0x32415352, bits, len(eb), k, half, half))
    return header + eb + n.to_bytes(k, "big") + p.to_bytes(half, "big") + q.to_bytes(half, "big")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rsa")
        subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-fsanitize=address,undefined",
                        "-I", os.path.join(ROOT, "src", "shfolderproxy"),
                        os.path.join(ROOT, "tests", "test_rsa.c"), "-o", exe], check=True)
        ok = fail = 0
        for bits in (512, 512, 512, 1024, 2048):
            for e in (65537, 3):
                p, q = prime(bits // 2, e), prime(bits // 2, e)
                n = p * q
                k = (n.bit_length() + 7) // 8
                d = pow(e, -1, (p - 1) * (q - 1))
                blob = private_blob(bits, e, n, p, q)
                for c in (0, 1, 2, n - 1, random.randrange(n), random.randrange(n)):
                    if random.random() < 0.3:  # shorter than the modulus
                        cin = c.to_bytes(max(1, (c.bit_length() + 7) // 8), "big")
                    else:
                        cin = c.to_bytes(k, "big")
                    got = subprocess.run([exe, blob.hex(), cin.hex()], capture_output=True,
                                         text=True, check=False).stdout.strip()
                    want = pow(c, d, n).to_bytes(k, "big").hex()
                    if got == want:
                        ok += 1
                    else:
                        fail += 1
                        print(f"MISMATCH bits={bits} e={e} c={hex(c)[:24]}", file=sys.stderr)
        print(f"ok={ok} fail={fail}")
        return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
