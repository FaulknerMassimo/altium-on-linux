/*
 * Raw RSA private-key operation (m = c^d mod n) from a BCRYPT_RSAPRIVATE_BLOB.
 *
 * Wine 11.0's GnuTLS-based bcrypt always applies PKCS#1 unpadding in
 * BCryptDecrypt and ignores BCRYPT_PAD_NONE, so raw decryption fails with
 * "GnuTLS error: Decryption has failed". Freestanding C (no libc) so it builds
 * into the Windows proxy DLL and, unchanged, into a host test.
 *
 * Montgomery multiplication over 32-bit limbs; moduli up to 2048 bits.
 */

#define RSA_MAX_LIMBS 64 /* 2048-bit modulus */
#define RSA_MAX_BYTES (RSA_MAX_LIMBS * 4)

typedef unsigned int rsa_u32;
typedef unsigned long long rsa_u64;

/* BCRYPT_RSAKEY_BLOB header: Magic, BitLength, cbPublicExp, cbModulus, cbPrime1, cbPrime2 */
#define RSA_BLOB_HEADER 24
#define RSA_PRIVATE_MAGIC 0x32415352u /* "RSA2" */
#define RSA_FULLPRIVATE_MAGIC 0x33415352u /* "RSA3" */

static void bn_zero(rsa_u32 *a, int s)
{
    for (int i = 0; i < s; i++) a[i] = 0;
}

/* Big-endian bytes -> little-endian limbs; caller ensures len <= 4 * s. */
static void bn_from_be(rsa_u32 *out, int s, const unsigned char *in, unsigned len)
{
    bn_zero(out, s);
    for (unsigned i = 0; i < len; i++)
        out[i / 4] |= (rsa_u32)in[len - 1 - i] << (8 * (i % 4));
}

static void bn_to_be(unsigned char *out, unsigned len, const rsa_u32 *in, int s)
{
    for (unsigned i = 0; i < len; i++)
        out[len - 1 - i] = i / 4 < (unsigned)s ? (unsigned char)(in[i / 4] >> (8 * (i % 4))) : 0;
}

static int bn_cmp(const rsa_u32 *a, const rsa_u32 *b, int s)
{
    for (int i = s - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* a -= b over s limbs; wraps modulo 2^(32 s). */
static void bn_sub(rsa_u32 *a, const rsa_u32 *b, int s)
{
    rsa_u64 borrow = 0;
    for (int i = 0; i < s; i++) {
        rsa_u64 d = (rsa_u64)a[i] - b[i] - borrow;
        a[i] = (rsa_u32)d;
        borrow = d >> 63;
    }
}

/* -n0^-1 mod 2^32 by Newton iteration (n0 odd). */
static rsa_u32 mont_ninv(rsa_u32 n0)
{
    rsa_u32 x = 1;
    for (int i = 0; i < 5; i++) x *= 2 - n0 * x;
    return 0u - x;
}

/* r = a * b * R^-1 mod n (CIOS). r may alias a or b. */
static void mont_mul(rsa_u32 *r, const rsa_u32 *a, const rsa_u32 *b, const rsa_u32 *n, rsa_u32 ninv, int s)
{
    rsa_u32 t[RSA_MAX_LIMBS + 2];
    bn_zero(t, s + 2);
    for (int i = 0; i < s; i++) {
        rsa_u64 c = 0;
        rsa_u32 m;
        for (int j = 0; j < s; j++) {
            c += (rsa_u64)t[j] + (rsa_u64)a[j] * b[i];
            t[j] = (rsa_u32)c;
            c >>= 32;
        }
        c += t[s];
        t[s] = (rsa_u32)c;
        t[s + 1] = (rsa_u32)(c >> 32);

        m = t[0] * ninv;
        c = ((rsa_u64)t[0] + (rsa_u64)m * n[0]) >> 32;
        for (int j = 1; j < s; j++) {
            c += (rsa_u64)t[j] + (rsa_u64)m * n[j];
            t[j - 1] = (rsa_u32)c;
            c >>= 32;
        }
        c += t[s];
        t[s - 1] = (rsa_u32)c;
        t[s] = t[s + 1] + (rsa_u32)(c >> 32);
    }
    if (t[s] || bn_cmp(t, n, s) >= 0) bn_sub(t, n, s);
    for (int i = 0; i < s; i++) r[i] = t[i];
}

/* out = base^exp mod n; base < n, n odd. */
static void mont_pow(rsa_u32 *out, const rsa_u32 *base, const rsa_u32 *exp, const rsa_u32 *n, int s)
{
    rsa_u32 ninv = mont_ninv(n[0]);
    rsa_u32 r2[RSA_MAX_LIMBS + 1], one[RSA_MAX_LIMBS], x[RSA_MAX_LIMBS], acc[RSA_MAX_LIMBS];

    /* r2 = R^2 mod n, by doubling 1 a total of 64 s times. */
    bn_zero(r2, s + 1);
    r2[0] = 1;
    for (int i = 0; i < 64 * s; i++) {
        rsa_u32 carry = 0;
        for (int j = 0; j < s; j++) {
            rsa_u32 v = r2[j];
            r2[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry || bn_cmp(r2, n, s) >= 0) bn_sub(r2, n, s);
    }

    bn_zero(one, s);
    one[0] = 1;
    mont_mul(x, base, r2, n, ninv, s);
    mont_mul(acc, one, r2, n, ninv, s);
    for (int i = 32 * s - 1; i >= 0; i--) {
        mont_mul(acc, acc, acc, n, ninv, s);
        if ((exp[i / 32] >> (i % 32)) & 1) mont_mul(acc, acc, x, n, ninv, s);
    }
    mont_mul(out, acc, one, n, ninv, s);
}

/* d = e^-1 mod (p-1)(q-1), with e < 2^32. Returns 0 if no inverse exists. */
static int rsa_private_exponent(rsa_u32 *d, const rsa_u32 *p, int sp, const rsa_u32 *q, int sq, rsa_u32 e, int s)
{
    rsa_u32 phi[2 * RSA_MAX_LIMBS + 2], t[2 * RSA_MAX_LIMBS + 2], pm1[RSA_MAX_LIMBS], qm1[RSA_MAX_LIMBS];
    int sphi = sp + sq;
    rsa_u64 r = 0, c;
    rsa_u32 k;

    if (e < 3 || !(p[0] & 1) || !(q[0] & 1)) return 0;
    for (int i = 0; i < sp; i++) pm1[i] = p[i];
    for (int i = 0; i < sq; i++) qm1[i] = q[i];
    pm1[0] -= 1; /* p, q odd: no borrow */
    qm1[0] -= 1;

    bn_zero(phi, sphi + 1);
    for (int i = 0; i < sq; i++) {
        c = 0;
        for (int j = 0; j < sp; j++) {
            c += (rsa_u64)phi[i + j] + (rsa_u64)pm1[j] * qm1[i];
            phi[i + j] = (rsa_u32)c;
            c >>= 32;
        }
        phi[i + sp] = (rsa_u32)c;
    }

    /* (1 + k phi) = 0 mod e for some k in [1, e); then d = (1 + k phi) / e < phi. */
    for (int i = sphi - 1; i >= 0; i--) r = ((r << 32) | phi[i]) % e;
    for (k = 1; k < e; k++)
        if ((1 + (rsa_u64)k * r) % e == 0) break;
    if (k == e) return 0;

    c = 1;
    for (int i = 0; i < sphi; i++) {
        c += (rsa_u64)phi[i] * k;
        t[i] = (rsa_u32)c;
        c >>= 32;
    }
    t[sphi] = (rsa_u32)c;

    r = 0;
    for (int i = sphi; i >= 0; i--) {
        rsa_u64 cur = (r << 32) | t[i];
        t[i] = (rsa_u32)(cur / e);
        r = cur % e;
    }
    if (r) return 0;
    for (int i = s; i <= sphi; i++)
        if (t[i]) return 0; /* d must fit in the modulus size */
    for (int i = 0; i < s; i++) d[i] = i <= sphi ? t[i] : 0;
    return 1;
}

static rsa_u32 rd32(const unsigned char *p)
{
    return p[0] | (rsa_u32)p[1] << 8 | (rsa_u32)p[2] << 16 | (rsa_u32)p[3] << 24;
}

/*
 * Bytes of blob needed by rsa_raw_decrypt (header, exponent, modulus, primes),
 * or 0 if the blob is not a usable RSA private key.
 */
static unsigned rsa_blob_needed(const unsigned char *blob, unsigned len)
{
    rsa_u32 magic, cb_exp, cb_mod, cb_p, cb_q;
    if (len < RSA_BLOB_HEADER) return 0;
    magic = rd32(blob);
    cb_exp = rd32(blob + 8);
    cb_mod = rd32(blob + 12);
    cb_p = rd32(blob + 16);
    cb_q = rd32(blob + 20);
    if (magic != RSA_PRIVATE_MAGIC && magic != RSA_FULLPRIVATE_MAGIC) return 0;
    if (!cb_exp || cb_exp > 4 || !cb_mod || cb_mod > RSA_MAX_BYTES) return 0;
    if (!cb_p || !cb_q || cb_p > cb_mod || cb_q > cb_mod) return 0;
    if ((rsa_u64)RSA_BLOB_HEADER + cb_exp + cb_mod + cb_p + cb_q > len) return 0;
    return RSA_BLOB_HEADER + cb_exp + cb_mod + cb_p + cb_q;
}

/* Modulus size in bytes (the raw output size), for a blob accepted by rsa_blob_needed. */
static unsigned rsa_blob_modulus_bytes(const unsigned char *blob)
{
    return rd32(blob + 12);
}

/*
 * out[0..cbModulus) = in^d mod n, big-endian, zero-padded on the left.
 * Returns 1 on success, 0 if the input or key is unusable.
 */
static int rsa_raw_decrypt(const unsigned char *blob, const unsigned char *in, unsigned in_len, unsigned char *out)
{
    rsa_u32 cb_exp = rd32(blob + 8), cb_mod = rd32(blob + 12), cb_p = rd32(blob + 16), cb_q = rd32(blob + 20);
    const unsigned char *pe = blob + RSA_BLOB_HEADER, *pn = pe + cb_exp, *pp = pn + cb_mod, *pq = pp + cb_p;
    int s = (int)((cb_mod + 3) / 4), sp = (int)((cb_p + 3) / 4), sq = (int)((cb_q + 3) / 4);
    rsa_u32 n[RSA_MAX_LIMBS], p[RSA_MAX_LIMBS], q[RSA_MAX_LIMBS], c[RSA_MAX_LIMBS], d[RSA_MAX_LIMBS], m[RSA_MAX_LIMBS];
    rsa_u32 e = 0;

    if (in_len > cb_mod) return 0;
    for (rsa_u32 i = 0; i < cb_exp; i++) e = (e << 8) | pe[i];
    bn_from_be(n, s, pn, cb_mod);
    bn_from_be(p, sp, pp, cb_p);
    bn_from_be(q, sq, pq, cb_q);
    bn_from_be(c, s, in, in_len);
    if (!(n[0] & 1) || bn_cmp(c, n, s) >= 0) return 0;
    if (!rsa_private_exponent(d, p, sp, q, sq, e, s)) return 0;
    mont_pow(m, c, d, n, s);
    bn_to_be(out, cb_mod, m, s);
    return 1;
}
