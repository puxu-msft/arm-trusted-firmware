/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RSA 私钥运算（CRT）与它用到的大数运算，语义与约束见 include/aml_rsa.h。
 * 各数的长度由 aml_rsa_load 限住：n 不超过 64 字，p、q 不超过 32 字，qP 先模 p；运算里的积都是两个小于 p 或 q 的数相乘，不超过 64 字，再加一个小于 q 的数不超过 65 字，都在 AML_BN_LIMBS 之内。
 */

#include <string.h>

#include "aml_rsa.h"

static void bn_norm(struct aml_bn *a)
{
	while ((a->n != 0U) && (a->d[a->n - 1U] == 0U))
		a->n--;
}

static void bn_set_word(struct aml_bn *r, uint32_t v)
{
	r->d[0] = v;
	r->n = (v != 0U) ? 1U : 0U;
}

static void bn_copy(struct aml_bn *r, const struct aml_bn *a)
{
	if (r != a) {
		r->n = a->n;
		memcpy(r->d, a->d, a->n * sizeof(a->d[0]));
	}
}

static uint32_t bn_bits(const struct aml_bn *a)
{
	if (a->n == 0U)
		return 0U;
	return (a->n * 32U) - (uint32_t)__builtin_clz(a->d[a->n - 1U]);
}

static int hex_digit(char c)
{
	if ((c >= '0') && (c <= '9'))
		return c - '0';
	if ((c >= 'a') && (c <= 'f'))
		return c - 'a' + 10;
	if ((c >= 'A') && (c <= 'F'))
		return c - 'A' + 10;
	return -1;
}

int aml_bn_from_hex(struct aml_bn *r, const char *s)
{
	size_t len = 0U, i;

	while (s[len] != '\0') {
		if ((hex_digit(s[len]) < 0) || (len >= (size_t)AML_RSA_MAX_BYTES * 2U))
			return -1;
		len++;
	}
	if (len == 0U)
		return -1;
	memset(r->d, 0, sizeof(r->d));
	for (i = 0U; i < len; i++)
		r->d[i / 8U] |= (uint32_t)hex_digit(s[len - 1U - i]) << (4U * (i % 8U));
	r->n = (uint32_t)((len + 7U) / 8U);
	bn_norm(r);
	return 0;
}

/* TomsFastMath 的 fp_read_unsigned_bin：大端字节读成整数（len 不超过 AML_BN_LIMBS 字，调用者保证） */
void aml_bn_from_be(struct aml_bn *r, const uint8_t *b, size_t len)
{
	size_t i;

	memset(r->d, 0, sizeof(r->d));
	for (i = 0U; i < len; i++)
		r->d[i / 4U] |= (uint32_t)b[len - 1U - i] << (8U * (i % 4U));
	r->n = (uint32_t)((len + 3U) / 4U);
	bn_norm(r);
}

/* fp_unsigned_bin_size */
size_t aml_bn_bytes(const struct aml_bn *a)
{
	return ((size_t)bn_bits(a) + 7U) / 8U;
}

/* 大端写满 len 字节（高位补 0；len 不短于 aml_bn_bytes(a)，调用者保证） */
void aml_bn_to_be(const struct aml_bn *a, uint8_t *b, size_t len)
{
	size_t i;

	for (i = 0U; i < len; i++) {
		size_t w = i / 4U;

		b[len - 1U - i] = (uint8_t)((w < a->n) ? (a->d[w] >> (8U * (i % 4U))) : 0U);
	}
}

int aml_bn_cmp(const struct aml_bn *a, const struct aml_bn *b)
{
	uint32_t i;

	if (a->n != b->n)
		return (a->n > b->n) ? 1 : -1;
	for (i = a->n; i-- > 0U;) {
		if (a->d[i] != b->d[i])
			return (a->d[i] > b->d[i]) ? 1 : -1;
	}
	return 0;
}

/* r = a + b（r 可以是 a 或 b） */
static void bn_add(struct aml_bn *r, const struct aml_bn *a, const struct aml_bn *b)
{
	uint32_t n = (a->n > b->n) ? a->n : b->n, i;
	uint64_t c = 0U;

	for (i = 0U; i < n; i++) {
		c += (uint64_t)((i < a->n) ? a->d[i] : 0U) + ((i < b->n) ? b->d[i] : 0U);
		r->d[i] = (uint32_t)c;
		c >>= 32;
	}
	r->n = n;
	if (c != 0U)
		r->d[r->n++] = (uint32_t)c;
}

/* r = a - b，a 不小于 b（r 可以是 a 或 b） */
static void bn_sub(struct aml_bn *r, const struct aml_bn *a, const struct aml_bn *b)
{
	uint32_t i;
	uint64_t borrow = 0U;

	for (i = 0U; i < a->n; i++) {
		uint64_t t = (uint64_t)a->d[i] - ((i < b->n) ? b->d[i] : 0U) - borrow;

		r->d[i] = (uint32_t)t;
		borrow = (t >> 32) & 1U;
	}
	r->n = a->n;
	bn_norm(r);
}

/* r = a · b（r 不能是 a 或 b）；两数字数之和超过 AML_BN_LIMBS 时（按装钥的限长到不了）结果记 0，不越界 */
static void bn_mul(struct aml_bn *r, const struct aml_bn *a, const struct aml_bn *b)
{
	uint32_t i, j;

	if ((a->n == 0U) || (b->n == 0U) || ((a->n + b->n) > AML_BN_LIMBS)) {
		r->n = 0U;
		return;
	}
	memset(r->d, 0, (a->n + b->n) * sizeof(r->d[0]));
	for (i = 0U; i < a->n; i++) {
		uint64_t c = 0U;

		for (j = 0U; j < b->n; j++) {
			c += ((uint64_t)a->d[i] * b->d[j]) + r->d[i + j];
			r->d[i + j] = (uint32_t)c;
			c >>= 32;
		}
		r->d[i + b->n] = (uint32_t)c;
	}
	r->n = a->n + b->n;
	bn_norm(r);
}

/*
 * r = u mod v（v 非 0；r 可以是 u 或 v）：Knuth 算法 D 只取余数。除数左移到最高位为 1，商的每一字先按被除数的头两字与除数的头两字估、至多修两次，乘减后为负再加回一次（这一步按随机数据几乎碰不到，主机测试另造了专门触发它的数）。
 */
void aml_bn_mod(struct aml_rsa_work *w, struct aml_bn *r, const struct aml_bn *u, const struct aml_bn *v)
{
	uint32_t m = u->n, n = v->n, s, i, j;
	uint32_t *un = w->un, *vn = w->vn;

	if (n == 0U) {
		r->n = 0U;
		return;
	}
	if (aml_bn_cmp(u, v) < 0) {
		bn_copy(r, u);
		return;
	}
	if (n == 1U) {
		uint64_t k = 0U;

		for (i = m; i-- > 0U;)
			k = ((k << 32) | u->d[i]) % v->d[0];
		bn_set_word(r, (uint32_t)k);
		return;
	}
	s = (uint32_t)__builtin_clz(v->d[n - 1U]);
	for (i = n - 1U; i > 0U; i--)
		vn[i] = (v->d[i] << s) | ((s != 0U) ? (v->d[i - 1U] >> (32U - s)) : 0U);
	vn[0] = v->d[0] << s;
	un[m] = (s != 0U) ? (u->d[m - 1U] >> (32U - s)) : 0U;
	for (i = m - 1U; i > 0U; i--)
		un[i] = (u->d[i] << s) | ((s != 0U) ? (u->d[i - 1U] >> (32U - s)) : 0U);
	un[0] = u->d[0] << s;

	for (j = m - n + 1U; j-- > 0U;) {
		uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1U];
		uint64_t qhat = num / vn[n - 1U], rhat = num % vn[n - 1U];
		uint64_t c = 0U, borrow = 0U, t;

		while (((qhat >> 32) != 0U) || ((qhat * vn[n - 2U]) > ((rhat << 32) | un[j + n - 2U]))) {
			qhat--;
			rhat += vn[n - 1U];
			if ((rhat >> 32) != 0U)
				break;
		}
		for (i = 0U; i < n; i++) {
			uint64_t p = (qhat * vn[i]) + c;

			c = p >> 32;
			t = (uint64_t)un[i + j] - (uint32_t)p - borrow;
			un[i + j] = (uint32_t)t;
			borrow = (t >> 32) & 1U;
		}
		t = (uint64_t)un[j + n] - c - borrow;
		un[j + n] = (uint32_t)t;
		if ((t >> 32) != 0U) {
			/* 估大了一：加回一个除数（最高字的进位与借位相抵，丢掉） */
			c = 0U;
			for (i = 0U; i < n; i++) {
				c += (uint64_t)un[i + j] + vn[i];
				un[i + j] = (uint32_t)c;
				c >>= 32;
			}
			un[j + n] += (uint32_t)c;
		}
	}
	for (i = 0U; i < n; i++)
		r->d[i] = (un[i] >> s) | ((s != 0U) ? (un[i + 1U] << (32U - s)) : 0U);
	r->n = n;
	bn_norm(r);
}

/* r = a · b mod m（r 可以是 a 或 b，不能是 m） */
static void bn_mulmod(struct aml_rsa_work *w, struct aml_bn *r, const struct aml_bn *a, const struct aml_bn *b, const struct aml_bn *m)
{
	bn_mul(&w->prod, a, b);
	aml_bn_mod(w, r, &w->prod, m);
}

/* r = g^e mod m，从高位起逐位平方再乘（r 不能是 e 或 m；g 先模 m，与 fp_exptmod 在 g 不小于 m 时先约化同值） */
void aml_bn_expmod(struct aml_rsa_work *w, struct aml_bn *r, const struct aml_bn *g, const struct aml_bn *e, const struct aml_bn *m)
{
	uint32_t i;

	aml_bn_mod(w, &w->base, g, m);
	bn_set_word(&w->prod, 1U);
	aml_bn_mod(w, r, &w->prod, m);
	for (i = bn_bits(e); i-- > 0U;) {
		bn_mulmod(w, r, r, r, m);
		if (((e->d[i / 32U] >> (i % 32U)) & 1U) != 0U)
			bn_mulmod(w, r, r, &w->base, m);
	}
}

size_t aml_rsa_load(struct aml_rsa_work *w, const struct aml_rsa_key *key)
{
	if ((aml_bn_from_hex(&w->n, key->n) != 0) || (aml_bn_from_hex(&w->p, key->p) != 0) ||
	    (aml_bn_from_hex(&w->q, key->q) != 0) || (aml_bn_from_hex(&w->dp, key->dp) != 0) ||
	    (aml_bn_from_hex(&w->dq, key->dq) != 0) || (aml_bn_from_hex(&w->x, key->qp) != 0))
		return 0U;
	if ((w->p.n == 0U) || (w->p.n > AML_RSA_MAX_HALF_LIMBS) || (w->q.n == 0U) || (w->q.n > AML_RSA_MAX_HALF_LIMBS))
		return 0U;
	/* 只用到 h = (m1 - m2)·qP mod p：qP 先模 p 不改变 h，积就不会超长 */
	aml_bn_mod(w, &w->qp, &w->x, &w->p);
	return aml_bn_bytes(&w->n);
}

int aml_rsa_private(struct aml_rsa_work *w, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen)
{
	size_t x, r;

	if (inlen > AML_RSA_MAX_BYTES)
		return AML_RSA_INVALID_ARG;
	aml_bn_from_be(&w->t, in, inlen);
	if (aml_bn_cmp(&w->n, &w->t) < 0)
		return AML_RSA_PK_INVALID_SIZE;
	aml_bn_expmod(w, &w->m1, &w->t, &w->dp, &w->p);
	aml_bn_expmod(w, &w->m2, &w->t, &w->dq, &w->q);
	/* h = (m1 - m2)·qP mod p：LibTomCrypt 用有符号的 fp_sub 与取非负余数的 fp_mod，这里先把 m2 模 p 再在 [0, p) 里相减，余数相同 */
	aml_bn_mod(w, &w->x, &w->m2, &w->p);
	if (aml_bn_cmp(&w->m1, &w->x) >= 0) {
		bn_sub(&w->t, &w->m1, &w->x);
	} else {
		bn_add(&w->t, &w->m1, &w->p);
		bn_sub(&w->t, &w->t, &w->x);
	}
	bn_mulmod(w, &w->t, &w->t, &w->qp, &w->p);
	/* m = m2 + h·q */
	bn_mul(&w->x, &w->t, &w->q);
	bn_add(&w->t, &w->x, &w->m2);

	x = aml_bn_bytes(&w->n);
	if (x > *outlen) {
		*outlen = x;
		return AML_RSA_BUFFER_OVERFLOW;
	}
	r = aml_bn_bytes(&w->t);
	if (r > x)
		return AML_RSA_ERROR;
	*outlen = x;
	aml_bn_to_be(&w->t, out, x);
	return AML_RSA_OK;
}
