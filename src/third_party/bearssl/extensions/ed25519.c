/* Ed25519 on the pinned BearSSL i15 Montgomery arithmetic, RFC 8032.
 * Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No separate Ed25519 backend or precomputed mutable tables. Field values have
 * 17 15-bit limbs plus the BearSSL encoded-length word. Scalar products use
 * 35 words before reduction. All scratch is owned by the call. */
#include "newmota_crypto.h"
#include "inner.h"

typedef uint16_t fe[18];
typedef struct { fe x, y, z, t; } point;
static const fe prime = {272,32749,32767,32767,32767,32767,32767,32767,32767,
    32767,32767,32767,32767,32767,32767,32767,32767,32767};
static const fe order = {269,21485,14827,3177,16531,19813,24307,30632,28540,
    20,0,0,0,0,0,0,0,4096};
static const fe d_normal = {272,30883,9906,14120,12122,2743,10299,4944,14341,
    6144,29649,26077,14851,14540,32718,2779,13943,20995};
static const fe sqrtm1_normal = {272,8368,5149,27805,10096,18316,9724,427,8588,
    10031,30639,25847,26628,12980,15329,5104,4672,11139};
static const unsigned char order_le[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
static const unsigned char base_encoding[32] = {0x58,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66};

static void reverse(unsigned char *dst, const unsigned char *src, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[n - 1 - i];
}
static void select_fe(fe out, const fe a, const fe b, unsigned bit) {
    uint16_t mask = (uint16_t)-(int)bit;
    for (unsigned i = 0; i < 18; ++i) out[i] = a[i] ^ (mask & (a[i] ^ b[i]));
}
static void add(fe out, const fe a, const fe b) {
    fe tmp;
    uint32_t carry, borrow;
    memcpy(tmp, a, sizeof tmp);
    carry = br_i15_add(tmp, b, 1);
    borrow = br_i15_sub(tmp, prime, 0);
    br_i15_sub(tmp, prime, carry | (borrow ^ 1));
    memcpy(out, tmp, sizeof tmp);
    nm_crypto_zero(tmp, sizeof tmp);
}
static void sub(fe out, const fe a, const fe b) {
    fe tmp;
    memcpy(tmp, a, sizeof tmp);
    br_i15_add(tmp, prime, br_i15_sub(tmp, b, 1));
    memcpy(out, tmp, sizeof tmp);
    nm_crypto_zero(tmp, sizeof tmp);
}
static void mul(fe out, const fe a, const fe b) {
    fe tmp;
    br_i15_montymul(tmp, a, b, prime, br_i15_ninv15(prime[1]));
    memcpy(out, tmp, sizeof tmp);
    nm_crypto_zero(tmp, sizeof tmp);
}
static void one(fe out) {
    memset(out, 0, sizeof(fe)); out[0] = 272; out[1] = 1;
    br_i15_to_monty(out, prime);
}
static void power(fe out, const fe a, const unsigned char exponent[32]) {
    fe acc, square, product;
    one(acc);
    for (int i = 255; i >= 0; --i) {
        mul(square, acc, acc);
        mul(product, square, a);
        select_fe(acc, square, product, (exponent[i / 8] >> (i % 8)) & 1);
    }
    memcpy(out, acc, sizeof acc);
    nm_crypto_zero(acc, sizeof acc); nm_crypto_zero(square, sizeof square);
    nm_crypto_zero(product, sizeof product);
}
static void inverse(fe out, const fe a) {
    unsigned char exponent[32];
    memset(exponent, 0xff, sizeof exponent); exponent[0] = 0xeb; exponent[31] = 0x7f;
    power(out, a, exponent);
}
static void encode_fe(unsigned char out[32], const fe a) {
    fe tmp;
    unsigned char big[32];
    memcpy(tmp, a, sizeof tmp);
    br_i15_from_monty(tmp, prime, br_i15_ninv15(prime[1]));
    br_i15_encode(big, sizeof big, tmp);
    reverse(out, big, 32);
    nm_crypto_zero(tmp, sizeof tmp); nm_crypto_zero(big, sizeof big);
}
static void identity(point *p) {
    memset(p, 0, sizeof *p); p->x[0] = p->t[0] = 272;
    one(p->y); one(p->z);
}
static void point_add(point *out, const point *p, const point *q) {
    fe a,b,c,d,e,f,g,h,u,v,curve;
    point result;
    sub(u,p->y,p->x); sub(v,q->y,q->x); mul(a,u,v);
    add(u,p->y,p->x); add(v,q->y,q->x); mul(b,u,v);
    memcpy(curve,d_normal,sizeof curve); br_i15_to_monty(curve,prime);
    add(curve,curve,curve); mul(u,p->t,q->t); mul(c,u,curve);
    mul(u,p->z,q->z); add(d,u,u);
    sub(e,b,a); sub(f,d,c); add(g,d,c); add(h,b,a);
    mul(result.x,e,f); mul(result.y,g,h); mul(result.t,e,h); mul(result.z,f,g);
    *out = result;
    nm_crypto_zero(&result,sizeof result);
    nm_crypto_zero(a,sizeof a); nm_crypto_zero(b,sizeof b); nm_crypto_zero(c,sizeof c);
    nm_crypto_zero(d,sizeof d); nm_crypto_zero(e,sizeof e); nm_crypto_zero(f,sizeof f);
    nm_crypto_zero(g,sizeof g); nm_crypto_zero(h,sizeof h); nm_crypto_zero(u,sizeof u);
    nm_crypto_zero(v,sizeof v); nm_crypto_zero(curve,sizeof curve);
}
static void point_mul(point *out, const point *p, const unsigned char scalar[32]) {
    point acc, doubled, added;
    identity(&acc);
    for (int i = 255; i >= 0; --i) {
        unsigned bit = (scalar[i / 8] >> (i % 8)) & 1;
        point_add(&doubled,&acc,&acc); point_add(&added,&doubled,p);
        select_fe(acc.x,doubled.x,added.x,bit); select_fe(acc.y,doubled.y,added.y,bit);
        select_fe(acc.z,doubled.z,added.z,bit); select_fe(acc.t,doubled.t,added.t,bit);
    }
    *out = acc;
    nm_crypto_zero(&acc,sizeof acc); nm_crypto_zero(&doubled,sizeof doubled);
    nm_crypto_zero(&added,sizeof added);
}
static int point_decode(point *p, const unsigned char encoding[32]) {
    unsigned char y[32], big[32], exponent[32], xbytes[32];
    fe yy,u,v,vi,xx,check,root,zero;
    unsigned sign = encoding[31] >> 7;
    int ok = 0;
    memcpy(y,encoding,32); y[31] &= 0x7f; reverse(big,y,32);
    if (!br_i15_decode_mod(p->y,big,32,prime)) goto cleanup;
    br_i15_to_monty(p->y,prime); one(p->z);
    mul(yy,p->y,p->y); sub(u,yy,p->z);
    memcpy(v,d_normal,sizeof v); br_i15_to_monty(v,prime); mul(v,v,yy); add(v,v,p->z);
    inverse(vi,v); mul(xx,u,vi);
    memset(exponent,0xff,32); exponent[0]=0xfe; exponent[31]=0x0f;
    power(p->x,xx,exponent); mul(check,p->x,p->x);
    if (!nm_crypto_equal(check,xx,sizeof xx)) {
        memcpy(root,sqrtm1_normal,sizeof root); br_i15_to_monty(root,prime);
        mul(p->x,p->x,root); mul(check,p->x,p->x);
        if (!nm_crypto_equal(check,xx,sizeof xx)) goto cleanup;
    }
    encode_fe(xbytes,p->x);
    memset(zero,0,sizeof zero); zero[0]=272;
    if (br_i15_iszero(p->x) && sign) goto cleanup;
    sub(root,zero,p->x);
    select_fe(p->x,p->x,root,(xbytes[0] & 1) ^ sign);
    mul(p->t,p->x,p->y); ok=1;
cleanup:
    nm_crypto_zero(y,sizeof y); nm_crypto_zero(big,sizeof big); nm_crypto_zero(xbytes,sizeof xbytes);
    nm_crypto_zero(yy,sizeof yy); nm_crypto_zero(u,sizeof u); nm_crypto_zero(v,sizeof v);
    nm_crypto_zero(vi,sizeof vi); nm_crypto_zero(xx,sizeof xx); nm_crypto_zero(check,sizeof check);
    nm_crypto_zero(root,sizeof root); nm_crypto_zero(zero,sizeof zero);
    if (!ok) nm_crypto_zero(p,sizeof *p);
    return ok;
}
static void point_encode(unsigned char out[32], const point *p) {
    fe zi,x,y;
    unsigned char xb[32];
    inverse(zi,p->z); mul(x,p->x,zi); mul(y,p->y,zi);
    encode_fe(out,y); encode_fe(xb,x); out[31] |= (xb[0] & 1) << 7;
    nm_crypto_zero(zi,sizeof zi); nm_crypto_zero(x,sizeof x); nm_crypto_zero(y,sizeof y);
    nm_crypto_zero(xb,sizeof xb);
}
static int point_identity(const point *p) {
    fe delta;
    sub(delta,p->y,p->z);
    int result = br_i15_iszero(p->x) && br_i15_iszero(delta);
    nm_crypto_zero(delta,sizeof delta);
    return result;
}
static void reduce(unsigned char out[32], const unsigned char *input, size_t length) {
    unsigned char big[64]; fe value;
    reverse(big,input,length); br_i15_decode_reduce(value,big,length,order);
    br_i15_encode(big,32,value); reverse(out,big,32);
    nm_crypto_zero(big,sizeof big); nm_crypto_zero(value,sizeof value);
}
static void scalar_muladd(unsigned char out[32], const unsigned char a[32],
    const unsigned char b[32], const unsigned char c[32])
{
    fe aa,bb,cc,value;
    uint16_t product[35]={0}; unsigned char big[64];
    reverse(big,a,32); br_i15_decode_reduce(aa,big,32,order);
    reverse(big,b,32); br_i15_decode_reduce(bb,big,32,order);
    reverse(big,c,32); br_i15_decode_reduce(cc,big,32,order);
    br_i15_mulacc(product,aa,bb);
    br_i15_encode(big,64,product); br_i15_decode_reduce(value,big,64,order);
    br_i15_add(value,cc,1); br_i15_sub(value,order,br_i15_sub(value,order,0)^1);
    br_i15_encode(big,32,value); reverse(out,big,32);
    nm_crypto_zero(aa,sizeof aa); nm_crypto_zero(bb,sizeof bb); nm_crypto_zero(cc,sizeof cc);
    nm_crypto_zero(value,sizeof value); nm_crypto_zero(product,sizeof product); nm_crypto_zero(big,sizeof big);
}
static void hash(unsigned char out[64], const void *a, size_t an,
    const void *b, size_t bn, const void *c, size_t cn)
{
    br_sha512_context ctx;
    br_sha512_init(&ctx); br_sha512_update(&ctx,a,an); br_sha512_update(&ctx,b,bn);
    br_sha512_update(&ctx,c,cn); br_sha512_out(&ctx,out); nm_crypto_zero(&ctx,sizeof ctx);
}
int nm_ed25519_secret_key(const void *seed, void *secret_key) {
    point base,p; unsigned char expanded[64], result[64];
    int ok = 0;
    if (!seed || !secret_key) return 0;
    memcpy(result,seed,32); hash(expanded,result,32,NULL,0,NULL,0);
    expanded[0]&=248; expanded[31]&=63; expanded[31]|=64;
    if (!point_decode(&base,base_encoding)) goto cleanup;
    point_mul(&p,&base,expanded); point_encode(result+32,&p); memcpy(secret_key,result,64);
    ok = 1;
cleanup:
    nm_crypto_zero(expanded,sizeof expanded); nm_crypto_zero(result,sizeof result);
    nm_crypto_zero(&p,sizeof p); nm_crypto_zero(&base,sizeof base); return ok;
}
int nm_ed25519_sign(const void *secret_key, const void *message, size_t message_len, void *signature) {
    const unsigned char *key=secret_key;
    point base,p; unsigned char expanded[64], r[64], h[64], result[64], checked[64];
    int ok=0;
    if (!key || !signature || (!message && message_len)) return 0;
    if (!nm_ed25519_secret_key(key,checked) || !nm_crypto_equal(checked,key,64)) goto cleanup;
    hash(expanded,key,32,NULL,0,NULL,0); expanded[0]&=248; expanded[31]&=63; expanded[31]|=64;
    hash(r,expanded+32,32,message,message_len,NULL,0); reduce(r,r,64);
    if (!point_decode(&base,base_encoding)) goto cleanup;
    point_mul(&p,&base,r); point_encode(result,&p);
    hash(h,result,32,key+32,32,message,message_len); reduce(h,h,64);
    scalar_muladd(result+32,h,expanded,r); memcpy(signature,result,64); ok=1;
cleanup:
    nm_crypto_zero(expanded,sizeof expanded); nm_crypto_zero(r,sizeof r); nm_crypto_zero(h,sizeof h);
    nm_crypto_zero(result,sizeof result); nm_crypto_zero(checked,sizeof checked);
    nm_crypto_zero(&p,sizeof p); nm_crypto_zero(&base,sizeof base); return ok;
}
int nm_ed25519_verify(const void *public_key, const void *message, size_t message_len, const void *signature) {
    const unsigned char *key=public_key, *sig=signature;
    point a,r,base,p,q,check; unsigned char h[64], encoded[32], big[32]; fe s;
    int ok=0;
    if (!key || !sig || (!message && message_len)) return 0;
    reverse(big,sig+32,32); if (!br_i15_decode_mod(s,big,32,order)) goto cleanup;
    if (!point_decode(&a,key) || !point_decode(&r,sig) || point_identity(&a)) goto cleanup;
    point_mul(&check,&a,order_le); if (!point_identity(&check)) goto cleanup;
    point_mul(&check,&r,order_le); if (!point_identity(&check)) goto cleanup;
    if (!point_decode(&base,base_encoding)) goto cleanup;
    hash(h,sig,32,key,32,message,message_len); reduce(h,h,64);
    point_mul(&p,&base,sig+32); point_mul(&q,&a,h); point_add(&q,&q,&r);
    point_encode(encoded,&p); point_encode(big,&q); ok=nm_crypto_equal(encoded,big,32);
cleanup:
    nm_crypto_zero(h,sizeof h); nm_crypto_zero(encoded,sizeof encoded); nm_crypto_zero(big,sizeof big);
    nm_crypto_zero(s,sizeof s); nm_crypto_zero(&a,sizeof a); nm_crypto_zero(&r,sizeof r);
    nm_crypto_zero(&base,sizeof base); nm_crypto_zero(&p,sizeof p); nm_crypto_zero(&q,sizeof q);
    nm_crypto_zero(&check,sizeof check); return ok;
}
