/* Host test driver for rsa_raw.c (run by tests/test_rsa.py): read a BCRYPT_RSAPRIVATE_BLOB and a ciphertext (hex), print raw m = c^d mod n (hex). */
#include <stdio.h>
#include "rsa_raw.c"

static unsigned unhex(const char *s, unsigned char *out)
{
    unsigned n = 0;
    for (; s[0] && s[1]; s += 2) sscanf(s, "%2hhx", &out[n++]);
    return n;
}

int main(int argc, char **argv)
{
    unsigned char blob[2048], in[512], out[512];
    unsigned blen, ilen;
    if (argc != 3) return 2;
    blen = unhex(argv[1], blob);
    ilen = unhex(argv[2], in);
    if (!rsa_blob_needed(blob, blen)) { puts("BADBLOB"); return 1; }
    if (!rsa_raw_decrypt(blob, in, ilen, out)) { puts("FAIL"); return 1; }
    for (unsigned i = 0; i < rsa_blob_modulus_bytes(blob); i++) printf("%02x", out[i]);
    putchar('\n');
    return 0;
}
