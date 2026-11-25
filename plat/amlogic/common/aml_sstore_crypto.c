/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 安全存储用的 AES-256（FIPS-197）、CBC 与 SHA-256（FIPS 180-4）：只依赖 C 标准头，BL31 与主机测试共用。
 * 按字节查表的直白实现，不追求速度：装载解密约 2 KiB、写块加密同量，SHA-256 每次走一遍块（256 KiB），在 A55 上是毫秒级，与厂商 BL31 的软件实现（LibTomCrypt）同一量级。
 * 主机测试用 FIPS-197 C.3、SP 800-38A F.2.5/F.2.6 与 FIPS 180-2 的向量核对（M12 tools/tf-a/sstore-harness.c 的 kat）。
 * 栈上的中间状态（轮状态、CBC 链值、SHA-256 的消息字）用完清零：最后一轮的状态与 CBC 的 IV 都含密钥材料。
 */

#include <stdint.h>
#include <string.h>

#include "aml_sstore.h"

static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t aes_inv_sbox[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

void aml_sstore_wipe(void *p, size_t n)
{
	volatile uint8_t *q = p;

	while (n-- != 0U)
		*q++ = 0U;
}

static uint32_t aes_sub_word(uint32_t w)
{
	return ((uint32_t)aes_sbox[(w >> 24) & 0xffU] << 24) |
	       ((uint32_t)aes_sbox[(w >> 16) & 0xffU] << 16) |
	       ((uint32_t)aes_sbox[(w >> 8) & 0xffU] << 8) |
	       (uint32_t)aes_sbox[w & 0xffU];
}

/* 轮密钥按大端字存（FIPS-197 §5.2，Nk = 8、Nr = 14） */
void aml_sstore_aes256_init(struct aml_sstore_aes *ctx, const uint8_t key[32])
{
	uint32_t rcon = 0x01U;
	unsigned int i;

	for (i = 0U; i < 8U; i++) {
		ctx->rk[i] = ((uint32_t)key[4U * i] << 24) |
			     ((uint32_t)key[4U * i + 1U] << 16) |
			     ((uint32_t)key[4U * i + 2U] << 8) |
			     (uint32_t)key[4U * i + 3U];
	}
	for (i = 8U; i < 60U; i++) {
		uint32_t t = ctx->rk[i - 1U];

		if ((i % 8U) == 0U) {
			t = aes_sub_word((t << 8) | (t >> 24)) ^ (rcon << 24);
			rcon = (rcon << 1) ^ (((rcon >> 7) & 1U) * 0x1bU);
		} else if ((i % 8U) == 4U) {
			t = aes_sub_word(t);
		}
		ctx->rk[i] = ctx->rk[i - 8U] ^ t;
	}
}

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ (((x >> 7) & 1U) * 0x1bU));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0U;

	while (b != 0U) {
		if ((b & 1U) != 0U)
			r ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

/* 状态按列存：s[4c + r] */
static void add_round_key(uint8_t s[16], const uint32_t *rk)
{
	unsigned int c;

	for (c = 0U; c < 4U; c++) {
		s[4U * c] ^= (uint8_t)(rk[c] >> 24);
		s[4U * c + 1U] ^= (uint8_t)(rk[c] >> 16);
		s[4U * c + 2U] ^= (uint8_t)(rk[c] >> 8);
		s[4U * c + 3U] ^= (uint8_t)rk[c];
	}
}

static void sub_shift(uint8_t s[16], const uint8_t *box, bool inverse)
{
	uint8_t t[16];
	unsigned int c, r;

	for (c = 0U; c < 4U; c++) {
		for (r = 0U; r < 4U; r++) {
			/* ShiftRows：第 r 行左移 r 位；逆向右移 */
			unsigned int src = inverse ? ((c + 4U - r) % 4U) : ((c + r) % 4U);

			t[4U * c + r] = box[s[4U * src + r]];
		}
	}
	memcpy(s, t, sizeof(t));
	aml_sstore_wipe(t, sizeof(t));
}

void aml_sstore_aes_encrypt(const struct aml_sstore_aes *ctx, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	unsigned int round, c;

	memcpy(s, in, sizeof(s));
	add_round_key(s, &ctx->rk[0]);
	for (round = 1U; round <= 14U; round++) {
		sub_shift(s, aes_sbox, false);
		if (round != 14U) {
			for (c = 0U; c < 4U; c++) {
				uint8_t *col = &s[4U * c];
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

				col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
				col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
				col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
				col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
			}
		}
		add_round_key(s, &ctx->rk[4U * round]);
	}
	memcpy(out, s, sizeof(s));
	aml_sstore_wipe(s, sizeof(s));
}

void aml_sstore_aes_decrypt(const struct aml_sstore_aes *ctx, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	unsigned int round, c;

	memcpy(s, in, sizeof(s));
	add_round_key(s, &ctx->rk[56]);
	for (round = 13U; ; round--) {
		sub_shift(s, aes_inv_sbox, true);
		add_round_key(s, &ctx->rk[4U * round]);
		if (round == 0U)
			break;
		for (c = 0U; c < 4U; c++) {
			uint8_t *col = &s[4U * c];
			uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];

			col[0] = (uint8_t)(gmul(a0, 14U) ^ gmul(a1, 11U) ^ gmul(a2, 13U) ^ gmul(a3, 9U));
			col[1] = (uint8_t)(gmul(a0, 9U) ^ gmul(a1, 14U) ^ gmul(a2, 11U) ^ gmul(a3, 13U));
			col[2] = (uint8_t)(gmul(a0, 13U) ^ gmul(a1, 9U) ^ gmul(a2, 14U) ^ gmul(a3, 11U));
			col[3] = (uint8_t)(gmul(a0, 11U) ^ gmul(a1, 13U) ^ gmul(a2, 9U) ^ gmul(a3, 14U));
		}
	}
	memcpy(out, s, sizeof(s));
	aml_sstore_wipe(s, sizeof(s));
}

void aml_sstore_cbc_encrypt(const struct aml_sstore_aes *ctx, const uint8_t iv[16], uint8_t *buf, size_t len)
{
	uint8_t prev[16];
	size_t off;
	unsigned int i;

	memcpy(prev, iv, sizeof(prev));
	for (off = 0U; off + 16U <= len; off += 16U) {
		for (i = 0U; i < 16U; i++)
			buf[off + i] ^= prev[i];
		aml_sstore_aes_encrypt(ctx, &buf[off], &buf[off]);
		memcpy(prev, &buf[off], sizeof(prev));
	}
	aml_sstore_wipe(prev, sizeof(prev));
}

void aml_sstore_cbc_decrypt(const struct aml_sstore_aes *ctx, const uint8_t iv[16], uint8_t *buf, size_t len)
{
	uint8_t prev[16], ct[16];
	size_t off;
	unsigned int i;

	memcpy(prev, iv, sizeof(prev));
	for (off = 0U; off + 16U <= len; off += 16U) {
		memcpy(ct, &buf[off], sizeof(ct));
		aml_sstore_aes_decrypt(ctx, &buf[off], &buf[off]);
		for (i = 0U; i < 16U; i++)
			buf[off + i] ^= prev[i];
		memcpy(prev, ct, sizeof(prev));
	}
	aml_sstore_wipe(prev, sizeof(prev));
	aml_sstore_wipe(ct, sizeof(ct));
}

static const uint32_t sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t ror32(uint32_t x, unsigned int n)
{
	return (x >> n) | (x << (32U - n));
}

static void sha256_block(struct aml_sstore_sha256 *ctx, const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;
	unsigned int i;

	for (i = 0U; i < 16U; i++) {
		w[i] = ((uint32_t)p[4U * i] << 24) | ((uint32_t)p[4U * i + 1U] << 16) |
		       ((uint32_t)p[4U * i + 2U] << 8) | (uint32_t)p[4U * i + 3U];
	}
	for (i = 16U; i < 64U; i++) {
		uint32_t s0 = ror32(w[i - 15U], 7U) ^ ror32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
		uint32_t s1 = ror32(w[i - 2U], 17U) ^ ror32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);

		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}
	a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
	e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
	for (i = 0U; i < 64U; i++) {
		uint32_t t1 = h + (ror32(e, 6U) ^ ror32(e, 11U) ^ ror32(e, 25U)) +
			      ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
		uint32_t t2 = (ror32(a, 2U) ^ ror32(a, 13U) ^ ror32(a, 22U)) +
			      ((a & b) ^ (a & c) ^ (b & c));

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
	ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
	{
		volatile uint32_t *q = w;

		for (i = 0U; i < 64U; i++)
			q[i] = 0U;
	}
}

void aml_sstore_sha256_init(struct aml_sstore_sha256 *ctx)
{
	static const uint32_t iv[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
	};

	memcpy(ctx->h, iv, sizeof(iv));
	ctx->len = 0U;
	ctx->fill = 0U;
}

void aml_sstore_sha256_update(struct aml_sstore_sha256 *ctx, const uint8_t *p, size_t n)
{
	ctx->len += n;
	while (n != 0U) {
		if ((ctx->fill == 0U) && (n >= 64U)) {
			sha256_block(ctx, p);
			p += 64;
			n -= 64U;
			continue;
		}
		ctx->buf[ctx->fill++] = *p++;
		n--;
		if (ctx->fill == 64U) {
			sha256_block(ctx, ctx->buf);
			ctx->fill = 0U;
		}
	}
}

void aml_sstore_sha256_final(struct aml_sstore_sha256 *ctx, uint8_t out[32])
{
	uint64_t bits = ctx->len * 8U;
	unsigned int i;

	ctx->buf[ctx->fill++] = 0x80U;
	if (ctx->fill > 56U) {
		while (ctx->fill < 64U)
			ctx->buf[ctx->fill++] = 0U;
		sha256_block(ctx, ctx->buf);
		ctx->fill = 0U;
	}
	while (ctx->fill < 56U)
		ctx->buf[ctx->fill++] = 0U;
	for (i = 0U; i < 8U; i++)
		ctx->buf[56U + i] = (uint8_t)(bits >> (56U - 8U * i));
	sha256_block(ctx, ctx->buf);
	for (i = 0U; i < 8U; i++) {
		out[4U * i] = (uint8_t)(ctx->h[i] >> 24);
		out[4U * i + 1U] = (uint8_t)(ctx->h[i] >> 16);
		out[4U * i + 2U] = (uint8_t)(ctx->h[i] >> 8);
		out[4U * i + 3U] = (uint8_t)ctx->h[i];
	}
}

void aml_sstore_sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
	struct aml_sstore_sha256 ctx;

	aml_sstore_sha256_init(&ctx);
	aml_sstore_sha256_update(&ctx, p, n);
	aml_sstore_sha256_final(&ctx, out);
	aml_sstore_wipe(&ctx, sizeof(ctx));
}
