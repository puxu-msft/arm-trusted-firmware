/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 安全存储（unifykey）的对象表、块的装载与写块：语义逐条照 M12 厂商 g12a BL31（v1.3:079690f10，基址 0x05100000）反汇编，括号里是厂商的函数地址。各调用的返回值、应答与表的变化与厂商相同；写出的块与厂商格式相同、互相读得懂，块里的字节只在下面第三条上可能不同。与厂商不同的地方（M12 docs/19 §6.6 有对照表）：
 * - 所有长度先按缓冲边界核对：请求、应答、块各 AML_SSTORE_BUF_SIZE；TLV 的内外层长度对不上、名字或数据超出自己的 TLV、装载出的对象 namelen > 79 或 datalen 超过它的数据区、对象超过 64 个、块大小不在 [0x400, AML_SSTORE_BUF_SIZE] 时整表作废（厂商在这些输入上会无符号回绕、越界拷贝或写出对象表）；
 * - 装载先把块拷进安全侧再验 SHA-256、解密，写块先在安全侧序列化、按装载的规则读回自检、加密、算 SHA-256，全部成功后一次拷进块缓冲：非安全世界改共享缓冲影响不到判定，写块失败时块缓冲不动（厂商这时块头已被清掉一半，但 WRITE 回 1，内核不落盘）；
 * - 写块时对象区补齐字节与对象区之后到块尾一律写 0（厂商留着工作缓冲与块缓冲里的旧字节，读的一方都不看），同一张表总是写出同一个块：对象区没有变短、厂商工作区里没有旧明文时（本机开机写 mount_check 就是这样）与厂商写出的块逐字节相同，值变短或删 key 之后块尾与补齐字节不同、两边读出的表相同。
 * 密钥材料（E、eFuse[0x40,0x48)、AES 轮密钥）与明文只在安全侧，用完清零；块缓冲里只写密文，应答缓冲里只写调用约定要交出的内容（READ 拒绝非安全世界读安全属性的 key）。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aml_sstore.h"

#define HDR_MAGIC_LEN		12U
#define HDR_VERSION		0x10U
#define HDR_ENCTYPE		0x14U
#define HDR_SHA			0x18U
#define INNER_OFF		0x200U
#define INNER_LEN		0x200U
#define OBJ_OFF			0x400U
#define BLOCK_MIN		(OBJ_OFF)

static const uint8_t hdr_magic[HDR_MAGIC_LEN] = "AMLSECURITY";

/* 取钥常量（厂商 BL31 0x512d2c0–0x512d30b、0x512c000–0x512c01b） */
static const uint8_t key_tail_default[20] = {
	0xad, 0x93, 0x00, 0xc4, 0x2f, 0x7d, 0x49, 0xd9, 0x15, 0x8b,
	0x7f, 0x04, 0x2c, 0x80, 0xb0, 0x62, 0x78, 0x25, 0x8d, 0x9c,
};
static const uint8_t iv_tail_default[4] = { 0x78, 0x25, 0x8d, 0x9c };
static const uint8_t key_tail_efuse[12] = {
	0xad, 0x93, 0x00, 0xc4, 0x8e, 0x50, 0x20, 0xc5, 0x3f, 0xbf, 0x23, 0x32,
};
static const uint8_t iv_tail_efuse[4] = { 0x80, 0x5a, 0xc6, 0xdf };
static const uint8_t key_fixed[32] = {
	0x13, 0x22, 0x02, 0x4a, 0x55, 0x23, 0xbb, 0xcb, 0xf1, 0xfb, 0x2a, 0xcc, 0xbb, 0x95, 0xf4, 0x50,
	0xae, 0x08, 0xd7, 0xfb, 0x80, 0xf2, 0x64, 0x72, 0xe3, 0x3c, 0xc4, 0xb4, 0xa3, 0x50, 0xd9, 0xf1,
};
static const uint8_t iv_fixed[16] = {
	0x2a, 0xde, 0xfc, 0xd7, 0x67, 0xc8, 0xde, 0xd0, 0xf0, 0x1e, 0xe8, 0x12, 0xf9, 0x57, 0x25, 0x36,
};

#define E_OFF			4U
#define E_LEN			12U
#define EFUSE40_OFF		0x40U
#define EFUSE40_LEN		8U

static uint32_t ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* 共享缓冲里的字段只取一次：经 volatile 读进局部变量，之后只用局部值 */
static uint32_t ns_ld32(const uint8_t *p)
{
	const volatile uint8_t *q = p;

	return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

static uint32_t a4(uint32_t n)
{
	return (n + 3U) & ~3U;
}

static uint32_t pad16(uint32_t n)
{
	return (n + 15U) & ~15U;
}

void aml_sstore_init(struct aml_sstore *s, uint8_t *in, uint8_t *out,
		     uint8_t *block, uint8_t *msg,
		     const struct aml_sstore_ops *ops, bool write_back)
{
	memset(s, 0, sizeof(*s));
	s->in = in;
	s->out = out;
	s->block = block;
	s->msg = msg;
	s->ops = ops;
	s->write_back = write_back;
	/* 厂商载荷里的初值：块大小 0x512d310、两个加密类型 0x512d314/0x512d318；VERSION 0x513db50 在 .bss */
	s->block_size = AML_SSTORE_BUF_SIZE;
	s->enc_req = -1;
	s->enc_loaded = -1;
	s->version = 0;
}

/* 0x51214dc：4 字节对齐的递增分配，size = 0 或放不下返回 NULL */
static uint8_t *heap_alloc(struct aml_sstore *s, uint32_t size)
{
	uint32_t n = a4(size);
	uint8_t *p;

	if ((size == 0U) || (n < size) || (n > AML_SSTORE_HEAP_SIZE - s->heap_used))
		return NULL;
	p = &s->heap[s->heap_used];
	s->heap_used += n;
	return p;
}

/* 装载失败（0x5120ba0）：堆复位、对象表清零；计数不清（厂商的计数在表外） */
static void table_reset(struct aml_sstore *s)
{
	s->heap_used = 0U;
	aml_sstore_wipe(s->heap, sizeof(s->heap));
	aml_sstore_wipe(s->keys, sizeof(s->keys));
}

/* 0x5120878：在 keys[0..count] 里找有效、长度与字节都相同的名字 */
static int lookup(const struct aml_sstore *s, const uint8_t *name, uint32_t namelen)
{
	uint32_t i;

	for (i = 0U; (i <= s->count) && (i < AML_SSTORE_KEYS); i++) {
		const struct aml_sstore_key *k = &s->keys[i];

		if (((k->state & 1U) != 0U) && (k->namelen == namelen) &&
		    (memcmp(k->name, name, namelen) == 0))
			return (int)i;
	}
	return -1;
}

uint32_t aml_sstore_key_copy(const struct aml_sstore *s, const char *name, uint8_t *buf, uint32_t len, uint32_t *attr)
{
	int i = lookup(s, (const uint8_t *)name, (uint32_t)strlen(name));
	const struct aml_sstore_key *k;
	uint32_t n;

	if (i < 0)
		return 0U;
	k = &s->keys[i];
	if (attr != NULL)
		*attr = k->attr & 0x101U;
	if ((k->data == NULL) || (k->datalen == 0U))
		return 0U;
	n = (k->datalen < len) ? k->datalen : len;
	if (buf != NULL)
		memcpy(buf, k->data, n);
	return n;
}

/* 0x511a540：eFuse[0x40,0x48) 全 0 或读不出 */
static bool efuse40_blank(const struct aml_sstore *s)
{
	uint8_t buf[EFUSE40_LEN] = { 0 };
	uint32_t n = s->ops->efuse_read(s->ops->ctx, buf, EFUSE40_OFF, EFUSE40_LEN);
	uint8_t any = 0U;
	unsigned int i;

	for (i = 0U; i < EFUSE40_LEN; i++)
		any |= buf[i];
	aml_sstore_wipe(buf, sizeof(buf));
	/* 厂商比较返回值的低字节（sxtb + cmp #8） */
	return ((int8_t)(uint8_t)n != (int8_t)EFUSE40_LEN) || (any == 0U);
}

/* 0x5120ecc：GET_ENCTYPE */
int64_t aml_sstore_get_enctype(struct aml_sstore *s)
{
	if ((s->ops->chip_family(s->ops->ctx) & 0xffU) == AML_SSTORE_FAMILY_ENC_FROM_BLOCK)
		return s->enc_loaded;
	if (efuse40_blank(s))
		return 1;
	return s->enc_loaded;
}

/* 0x5120f04：写块用的加密类型，由 SET_ENCTYPE 的请求与 GET_ENCTYPE 合出 */
static int32_t seal_enctype(struct aml_sstore *s)
{
	int32_t cur = (int32_t)aml_sstore_get_enctype(s);
	int32_t req = s->enc_req;

	if (req == 0)
		return (cur == 1) ? 1 : 0;
	if ((req == 1) && (cur == 1))
		return 1;
	if ((req == 2) && ((cur == 2) || (cur == -1)))
		return 2;
	if (cur != -1)
		return cur;
	return efuse40_blank(s) ? 1 : 0;
}

/* 0x5120718 的取钥部分（类型 1 为 0x511a5b0）：成功时填好轮密钥与 IV */
static bool derive_key(struct aml_sstore *s, int32_t type, struct aml_sstore_aes *aes, uint8_t iv[16])
{
	uint8_t key[32], e[E_LEN], f[EFUSE40_LEN];
	bool ok = false;

	if (type == 2) {
		aml_sstore_aes256_init(aes, key_fixed);
		memcpy(iv, iv_fixed, 16U);
		return true;
	}
	if (type == 1) {
		if (s->ops->efuse_read(s->ops->ctx, f, EFUSE40_OFF, EFUSE40_LEN) != EFUSE40_LEN)
			goto out;
		if (s->ops->efuse_read(s->ops->ctx, e, E_OFF, E_LEN) != E_LEN)
			goto out;
		memcpy(key, f, EFUSE40_LEN);
		memcpy(&key[EFUSE40_LEN], e, E_LEN);
		memcpy(&key[EFUSE40_LEN + E_LEN], key_tail_efuse, sizeof(key_tail_efuse));
		memcpy(iv, e, E_LEN);
		memcpy(&iv[E_LEN], iv_tail_efuse, sizeof(iv_tail_efuse));
	} else {
		if (s->ops->efuse_read(s->ops->ctx, e, E_OFF, E_LEN) != E_LEN)
			goto out;
		memcpy(key, e, E_LEN);
		memcpy(&key[E_LEN], key_tail_default, sizeof(key_tail_default));
		memcpy(iv, e, E_LEN);
		memcpy(&iv[E_LEN], iv_tail_default, sizeof(iv_tail_default));
	}
	aml_sstore_aes256_init(aes, key);
	ok = true;
out:
	aml_sstore_wipe(key, sizeof(key));
	aml_sstore_wipe(e, sizeof(e));
	aml_sstore_wipe(f, sizeof(f));
	return ok;
}

/* 0x5121590：内层头 {1,len}{tag,len,值}…，取 tag 2（total）与 tag 0xb（块大小） */
static bool parse_inner(const uint8_t *p, uint32_t *total, uint32_t *flash_size)
{
	uint32_t left, off = 8U;

	if (ld32(p) != 1U)
		return false;
	left = ld32(p + 4);
	if (left > INNER_LEN - 8U)
		return false;
	while (left != 0U) {
		uint32_t tag, len;

		if (left < 8U)
			return false;
		tag = ld32(p + off);
		len = ld32(p + off + 4U);
		if (len > left - 8U)
			return false;
		if ((tag == 2U) || (tag == 0xbU)) {
			/* 厂商不看 len，直接取 +8 的 u32 */
			if (off + 12U > INNER_LEN)
				return false;
			if (tag == 2U)
				*total = ld32(p + off + 8U);
			else
				*flash_size = ld32(p + off + 8U);
		}
		off += 8U + len;
		left -= 8U + len;
	}
	return true;
}

/*
 * 0x51215f4：解析一个对象到 k（k 不先清零：厂商在哈希不符后复用同一表项，没被覆盖的字段沿用）。
 * heap 为 false 时只做自检：数据指针指向明文本身、不分配。
 * 返回 false 即整表作废；*consumed = 外层长度 + 8。
 */
static bool parse_object(struct aml_sstore *s, const uint8_t *p, uint32_t remaining,
			 struct aml_sstore_key *k, uint32_t *consumed, bool heap)
{
	uint32_t left, off;

	if ((remaining < 8U) || (ld32(p) != 3U))
		return false;
	left = ld32(p + 4);
	if (left > remaining - 8U)
		return false;
	*consumed = left + 8U;
	off = 8U;
	while (left != 0U) {
		uint32_t tag, len;
		const uint8_t *v;

		if (left < 8U)
			return false;
		tag = ld32(p + off);
		len = ld32(p + off + 4U);
		v = p + off + 8U;
		switch (tag) {
		case 4U:
		case 6U:
		case 8U:
		case 9U:
			/* 取 u32 后固定前进 12 字节，不看 len */
			if (left < 12U)
				return false;
			if (tag == 4U)
				k->namelen = ld32(v);
			else if (tag == 6U)
				k->datalen = ld32(v);
			else if (tag == 8U)
				k->type = ld32(v);
			else
				k->attr = ld32(v);
			off += 12U;
			left -= 12U;
			continue;
		case 0xaU:
			if (len != 0x20U) {
				k->state = 2U;
				return false;
			}
			if (left < 0x28U)
				return false;
			memcpy(k->sha, v, 0x20U);
			off += 0x28U;
			left -= 0x28U;
			continue;
		default:
			break;
		}
		if (len > left - 8U)
			return false;
		if (tag == 5U) {
			if ((k->namelen > AML_SSTORE_NAME_MAX) || (k->namelen > len))
				return false;
			memset(k->name, 0, sizeof(k->name));
			memcpy(k->name, v, k->namelen);
		} else if (tag == 7U) {
			if (k->datalen > len)
				return false;
			if (!heap) {
				k->data = (uint8_t *)(uintptr_t)v;
				k->datacap = k->datalen;
			} else {
				k->data = heap_alloc(s, k->datalen);
				if (k->data == NULL) {
					k->state = 2U;
					return false;
				}
				memcpy(k->data, v, k->datalen);
				k->datacap = k->datalen;
			}
		}
		off += 8U + len;
		left -= 8U + len;
	}
	return true;
}

static bool object_sha_ok(const struct aml_sstore_key *k)
{
	uint8_t sha[32];

	if ((k->data == NULL) && (k->datalen != 0U))
		return false;
	aml_sstore_sha256(k->data, k->datalen, sha);
	return memcmp(sha, k->sha, sizeof(sha)) == 0;
}

/* 0x5120994 */
enum aml_sstore_load_result aml_sstore_load(struct aml_sstore *s, uint32_t size)
{
	struct aml_sstore_aes aes;
	uint8_t iv[16], sha[32];
	uint32_t total = 0U, flash_size = 0U, remaining, cipher;
	uint8_t *w = s->work;
	enum aml_sstore_load_result r;

	if (size > AML_SSTORE_BUF_SIZE)
		return s->last_load = AML_SSTORE_LOAD_TOO_LARGE;

	memset(s->msg, 0, AML_SSTORE_MSG_CLEAR);
	table_reset(s);
	s->unsealed = false;
	s->enc_loaded = -1;
	s->version = -1;

	if (size < BLOCK_MIN) {
		r = AML_SSTORE_LOAD_SHORT;
		goto fail_size;
	}
	/* 一次拷进安全侧，之后只看这份快照 */
	memcpy(w, s->block, size);
	if (memcmp(w, hdr_magic, HDR_MAGIC_LEN) != 0) {
		r = AML_SSTORE_LOAD_NO_MAGIC;
		goto fail_size;
	}
	s->enc_loaded = (int32_t)ld32(w + HDR_ENCTYPE);
	s->version = (int32_t)ld32(w + HDR_VERSION);
	if (s->version > 0) {
		aml_sstore_sha256(w + INNER_OFF, size - INNER_OFF, sha);
		if (memcmp(sha, w + HDR_SHA, sizeof(sha)) != 0) {
			r = AML_SSTORE_LOAD_BAD_SHA;
			goto fail_size;
		}
	}
	if (!derive_key(s, s->enc_loaded, &aes, iv)) {
		r = AML_SSTORE_LOAD_KEY_FAIL;
		goto fail_size;
	}
	aml_sstore_cbc_decrypt(&aes, iv, w + INNER_OFF, INNER_LEN);
	if (!parse_inner(w + INNER_OFF, &total, &flash_size) || (total == 0U)) {
		r = AML_SSTORE_LOAD_BAD_HEADER;
		goto fail_size;
	}
	if (flash_size != 0U) {
		if ((flash_size < BLOCK_MIN) || (flash_size > AML_SSTORE_BUF_SIZE)) {
			r = AML_SSTORE_LOAD_BAD_HEADER;
			goto fail_size;
		}
		s->block_size = flash_size;
	}
	if (s->block_size != size) {
		r = AML_SSTORE_LOAD_SIZE_MISMATCH;
		goto fail;
	}
	cipher = pad16(total);
	if ((cipher < total) || (cipher > size - OBJ_OFF)) {
		r = AML_SSTORE_LOAD_BAD_HEADER;
		goto fail;
	}
	aml_sstore_cbc_decrypt(&aes, iv, w + OBJ_OFF, cipher);

	s->count = 0U;
	remaining = total;
	w += OBJ_OFF;
	while (remaining != 0U) {
		struct aml_sstore_key *k;
		uint32_t consumed;

		if (s->count >= AML_SSTORE_KEYS) {
			r = AML_SSTORE_LOAD_BAD_OBJECTS;
			goto fail;
		}
		k = &s->keys[s->count];
		if (!parse_object(s, w, remaining, k, &consumed, true)) {
			r = AML_SSTORE_LOAD_BAD_OBJECTS;
			goto fail;
		}
		/*
		 * tag 4、6 可以出现在 tag 5、7 之后再把长度改大，表项也沿用前一个哈希不符对象的数据指针：解析完再核一次，
		 * namelen 放得进名字字段、datalen 不超过它的数据区，否则整表作废（厂商在这里越界拷贝）
		 */
		if ((k->namelen > AML_SSTORE_NAME_MAX) ||
		    ((k->data != NULL) && (k->datalen > k->datacap))) {
			r = AML_SSTORE_LOAD_BAD_OBJECTS;
			goto fail;
		}
		if (object_sha_ok(k)) {
			k->state = 1U;
			s->count++;
		} else {
			k->state = 2U;
		}
		w += consumed;
		remaining -= consumed;
	}
	r = AML_SSTORE_LOADED;
	goto out;

fail_size:
	s->block_size = size;
fail:
	table_reset(s);
out:
	aml_sstore_wipe(&aes, sizeof(aes));
	aml_sstore_wipe(iv, sizeof(iv));
	aml_sstore_wipe(s->work, size);
	return s->last_load = r;
}

/* 0x512152c：{tag, a4(len), value 补零}，返回写出的字节数 */
static uint32_t put_tlv(uint8_t *p, uint32_t tag, const uint8_t *v, uint32_t len)
{
	uint32_t n = a4(len);

	st32(p, tag);
	st32(p + 4, n);
	memset(p + 8, 0, n);
	if (len != 0U)
		memcpy(p + 8, v, len);
	return n + 8U;
}

static uint32_t put_u32(uint8_t *p, uint32_t tag, uint32_t v)
{
	st32(p, tag);
	st32(p + 4, 4U);
	st32(p + 8, v);
	return 12U;
}

static uint32_t name_len(const struct aml_sstore_key *k)
{
	uint32_t n = 0U;

	/* 厂商用 strlen(名字)：名字字段 80 字节、namelen ≤ 79 处之后都是 0 */
	while ((n < sizeof(k->name)) && (k->name[n] != 0U))
		n++;
	return n;
}

static uint32_t object_size(const struct aml_sstore_key *k)
{
	uint32_t nl = name_len(k);
	uint32_t size = 8U + 0x28U + 12U + 12U;

	if (nl != 0U)
		size += 12U + 8U + a4(nl);
	if ((k->data != NULL) && (k->datalen != 0U))
		size += 12U + 8U + a4(k->datalen);
	return size;
}

/* 0x5121788：把 k 写成对象 TLV，返回字节数（等于 object_size(k)） */
static uint32_t put_object(const struct aml_sstore_key *k, uint8_t *p)
{
	uint32_t nl = name_len(k);
	uint32_t off = 8U;

	st32(p, 3U);
	if (nl != 0U) {
		off += put_u32(p + off, 4U, nl);
		off += put_tlv(p + off, 5U, k->name, nl);
	}
	if ((k->data != NULL) && (k->datalen != 0U)) {
		off += put_u32(p + off, 6U, k->datalen);
		off += put_tlv(p + off, 7U, k->data, k->datalen);
	}
	off += put_tlv(p + off, 0xaU, k->sha, 0x20U);
	off += put_u32(p + off, 8U, k->type);
	off += put_u32(p + off, 9U, k->attr);
	st32(p + 4, off - 8U);
	return off;
}

/* 自检：按装载的解析规则读回刚写的明文，逐项与对象表相同、哈希对得上 */
static bool selfcheck(struct aml_sstore *s, uint32_t total)
{
	uint32_t got_total = 0U, got_size = 0U, remaining = total, i = 0U;
	const uint8_t *p = s->work + OBJ_OFF;

	if (!parse_inner(s->work + INNER_OFF, &got_total, &got_size) ||
	    (got_total != total) || (got_size != s->block_size))
		return false;
	while (remaining != 0U) {
		struct aml_sstore_key t;
		const struct aml_sstore_key *k;
		uint32_t consumed, nl;

		while ((i < AML_SSTORE_KEYS) &&
		       (((s->keys[i].state & 1U) == 0U) || (s->keys[i].data == NULL)))
			i++;
		if (i == AML_SSTORE_KEYS)
			return false;
		k = &s->keys[i++];
		memset(&t, 0, sizeof(t));
		if (!parse_object(s, p, remaining, &t, &consumed, false))
			return false;
		nl = name_len(k);
		if ((t.namelen != nl) || (memcmp(t.name, k->name, nl) != 0) ||
		    (t.datalen != k->datalen) || (t.data == NULL) || (t.datalen > t.datacap) ||
		    (memcmp(t.data, k->data, k->datalen) != 0) ||
		    (memcmp(t.sha, k->sha, sizeof(t.sha)) != 0) ||
		    (t.attr != k->attr) || (t.type != k->type) || !object_sha_ok(&t))
			return false;
		p += consumed;
		remaining -= consumed;
	}
	while ((i < AML_SSTORE_KEYS) &&
	       (((s->keys[i].state & 1U) == 0U) || (s->keys[i].data == NULL)))
		i++;
	return i == AML_SSTORE_KEYS;
}

/* 0x5120f74：WRITE/REMOVE 之后把整张表写成新块；成功返回 true */
static bool seal(struct aml_sstore *s)
{
	struct aml_sstore_aes aes;
	uint8_t iv[16];
	uint8_t *w = s->work;
	uint32_t total = 0U, cap, cipher, i;
	int32_t type = seal_enctype(s);
	enum aml_sstore_seal_result r;

	/* 厂商：pad16(total) + 0x400 > 块大小即报 "storage size is larger than flash!" 失败 */
	if (s->block_size < BLOCK_MIN) {
		r = AML_SSTORE_SEAL_TOO_LARGE;
		goto out;
	}
	cap = s->block_size - OBJ_OFF;
	for (i = 0U; i < AML_SSTORE_KEYS; i++) {
		const struct aml_sstore_key *k = &s->keys[i];
		uint32_t n;

		if (((k->state & 1U) == 0U) || (k->data == NULL))
			continue;
		n = object_size(k);
		if (n > cap - total) {
			r = AML_SSTORE_SEAL_TOO_LARGE;
			goto out;
		}
		if (put_object(k, w + OBJ_OFF + total) != n) {
			r = AML_SSTORE_SEAL_SELFCHECK;
			goto out;
		}
		total += n;
	}
	cipher = pad16(total);
	if (cipher > cap) {
		r = AML_SSTORE_SEAL_TOO_LARGE;
		goto out;
	}
	memset(w + OBJ_OFF + total, 0, cap - total);
	memset(w + INNER_OFF, 0, INNER_LEN);
	put_u32(w + INNER_OFF + 8U, 2U, total);
	put_u32(w + INNER_OFF + 20U, 0xbU, s->block_size);
	st32(w + INNER_OFF, 1U);
	st32(w + INNER_OFF + 4U, 0x18U);
	if (!selfcheck(s, total)) {
		r = AML_SSTORE_SEAL_SELFCHECK;
		goto out;
	}
	if (!derive_key(s, type, &aes, iv)) {
		r = AML_SSTORE_SEAL_KEY_FAIL;
		goto out;
	}
	aml_sstore_cbc_encrypt(&aes, iv, w + INNER_OFF, INNER_LEN);
	aml_sstore_cbc_encrypt(&aes, iv, w + OBJ_OFF, cipher);
	memset(w, 0, INNER_OFF);
	memcpy(w, hdr_magic, HDR_MAGIC_LEN);
	st32(w + HDR_VERSION, 1U);
	st32(w + HDR_ENCTYPE, (uint32_t)type);
	aml_sstore_sha256(w + INNER_OFF, s->block_size - INNER_OFF, w + HDR_SHA);
	memcpy(s->seal_sha, w + HDR_SHA, sizeof(s->seal_sha));
	if (!s->write_back) {
		r = AML_SSTORE_SEAL_HELD;
		goto out;
	}
	memcpy(s->block, w, s->block_size);
	st32(s->msg + AML_SSTORE_MSG_DIRTY, 1U);
	st32(s->msg + AML_SSTORE_MSG_COUNT, ns_ld32(s->msg + AML_SSTORE_MSG_COUNT) + 1U);
	s->version = 1;
	s->enc_loaded = type;
	s->unsealed = false;
	r = AML_SSTORE_SEALED;
out:
	aml_sstore_wipe(&aes, sizeof(aes));
	aml_sstore_wipe(iv, sizeof(iv));
	aml_sstore_wipe(s->work, (s->block_size >= BLOCK_MIN) ? s->block_size : 0U);
	s->last_seal = r;
	return r == AML_SSTORE_SEALED;
}

/* 请求里的名字：长度 ≤ 79 时拷进 name，否则返回 false */
static bool take_name(const uint8_t *src, uint32_t namelen, uint8_t name[80])
{
	if (namelen > AML_SSTORE_NAME_MAX)
		return false;
	memset(name, 0, 80U);
	memcpy(name, src, namelen);
	return true;
}

/* 0x5120ce8 */
uint32_t aml_sstore_query(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	uint32_t exist = 0U;
	int i;

	if (!take_name(s->in + 4, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	i = lookup(s, name, namelen);
	if ((i >= 0) && (s->keys[i].datalen != 0U) && (s->keys[i].data != NULL))
		exist = 1U;
	st32(s->out, exist);
	return AML_SSTORE_OK;
}

/* 0x5120bd0：读 min(datalen, buflen) 字节，安全属性的 key 不给非安全调用者 */
uint32_t aml_sstore_read(struct aml_sstore *s, bool ns_caller)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	uint32_t buflen = ns_ld32(s->in + 4);
	const struct aml_sstore_key *k;
	uint32_t n;
	int i;

	if (!take_name(s->in + 8, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	i = lookup(s, name, namelen);
	if (i < 0)
		return AML_SSTORE_ERR_NOKEY;
	k = &s->keys[i];
	if (((k->attr & 1U) != 0U) && ns_caller)
		return AML_SSTORE_ERR_NOKEY;
	if ((k->data == NULL) || (k->datalen == 0U))
		return AML_SSTORE_ERR_NOKEY;
	n = (k->datalen < buflen) ? k->datalen : buflen;
	if (n > AML_SSTORE_BUF_SIZE - 4U)
		n = AML_SSTORE_BUF_SIZE - 4U;
	st32(s->out, n);
	memcpy(s->out + 4, k->data, n);
	return AML_SSTORE_OK;
}

/* 0x5121168：请求 {namelen, datalen, attr, name[namelen], data[datalen]} */
uint32_t aml_sstore_write(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	uint32_t datalen = ns_ld32(s->in + 4);
	uint32_t attr = ns_ld32(s->in + 8);
	struct aml_sstore_key *k;
	const uint8_t *src;
	int i;

	s->last_seal = AML_SSTORE_SEAL_NONE;
	s->last_namelen = namelen;
	s->last_datalen = datalen;
	if (!take_name(s->in + 12, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	memcpy(s->last_name, name, sizeof(s->last_name));
	if ((datalen - 1U) > (AML_SSTORE_BUF_SIZE - 1U))
		return AML_SSTORE_ERR_PARAM;
	/* 厂商不核对，数据可以越出输入缓冲读到应答缓冲 */
	if (datalen > AML_SSTORE_BUF_SIZE - 12U - namelen)
		return AML_SSTORE_ERR_PARAM;
	src = s->in + 12 + namelen;
	i = lookup(s, name, namelen);
	if (i < 0) {
		if (s->count >= AML_SSTORE_KEYS)
			return AML_SSTORE_ERR_FULL;
		s->unsealed = true;
		k = &s->keys[s->count++];
		memset(k, 0, sizeof(*k));
		memcpy(k->name, name, namelen);
		k->namelen = namelen;
		k->data = heap_alloc(s, datalen);
		if (k->data == NULL)
			return AML_SSTORE_ERR_FULL;
		k->datacap = datalen;
		k->attr = attr;
		k->state |= 1U;
		k->type = AML_SSTORE_OBJ_TYPE;
	} else {
		k = &s->keys[i];
		if (k->attr != attr)
			return AML_SSTORE_ERR_PARAM;
		if (s->skip_unchanged && !s->unsealed && (k->data != NULL) && (k->datalen == datalen) &&
		    (memcmp(k->data, src, datalen) == 0)) {
			s->last_seal = AML_SSTORE_SEAL_SKIPPED;
			return AML_SSTORE_UNCHANGED;
		}
		s->unsealed = true;
		if (k->datalen < datalen) {
			/* 厂商在分配失败时把数据指针也置成 NULL（之后写块会丢掉这个 key），这里保留旧值 */
			uint8_t *p = heap_alloc(s, datalen);

			if (p == NULL)
				return AML_SSTORE_ERR_FULL;
			k->data = p;
			k->datacap = datalen;
		}
	}
	memcpy(k->data, src, datalen);
	k->datalen = datalen;
	aml_sstore_sha256(k->data, datalen, k->sha);
	return seal(s) ? AML_SSTORE_OK : AML_SSTORE_ERR_NOKEY;
}

/* 0x5120c84 */
uint32_t aml_sstore_tell(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	const struct aml_sstore_key *k;
	int i;

	if (!take_name(s->in + 4, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	i = lookup(s, name, namelen);
	if (i < 0)
		return AML_SSTORE_ERR_NOKEY;
	k = &s->keys[i];
	if ((k->data == NULL) || (k->datalen == 0U))
		return AML_SSTORE_ERR_NOKEY;
	st32(s->out, k->datalen);
	return AML_SSTORE_OK;
}

/* 0x5120db4：交出对象里存的 sha256(明文)（厂商对安全属性的 key 同样交出） */
uint32_t aml_sstore_verify(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	const struct aml_sstore_key *k;
	int i;

	if (!take_name(s->in + 4, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	i = lookup(s, name, namelen);
	if (i < 0)
		return AML_SSTORE_ERR_NOKEY;
	k = &s->keys[i];
	if ((k->datalen == 0U) || (k->data == NULL))
		return AML_SSTORE_ERR_NOKEY;
	memcpy(s->out, k->sha, sizeof(k->sha));
	return AML_SSTORE_OK;
}

/* 0x5120d54 */
uint32_t aml_sstore_status(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	int i;

	if (!take_name(s->in + 4, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	i = lookup(s, name, namelen);
	if (i < 0)
		return AML_SSTORE_ERR_NOKEY;
	st32(s->out, s->keys[i].attr & 0x101U);
	return AML_SSTORE_OK;
}

/* 0x5120e2c：{total, name0\0name1\0…}，按表项顺序，含安全属性的 key 的名字 */
uint32_t aml_sstore_list(struct aml_sstore *s)
{
	uint32_t total = 0U, i;
	uint8_t *p = s->out + 4;

	for (i = 0U; i < AML_SSTORE_KEYS; i++) {
		const struct aml_sstore_key *k = &s->keys[i];

		if (((k->state & 1U) == 0U) || (k->data == NULL))
			continue;
		/* 64 × 80 字节放得下，不会越出应答缓冲 */
		memcpy(p, k->name, k->namelen);
		p[k->namelen] = 0U;
		p += k->namelen + 1U;
		total += k->namelen + 1U;
	}
	st32(s->out, total);
	return AML_SSTORE_OK;
}

/* 0x512130c：标为已删（bit0 清、置 2）后写块；名字过长或不存在都回 2 */
uint32_t aml_sstore_remove(struct aml_sstore *s)
{
	uint8_t name[80];
	uint32_t namelen = ns_ld32(s->in);
	int i;

	s->last_seal = AML_SSTORE_SEAL_NONE;
	s->last_namelen = namelen;
	s->last_datalen = 0U;
	if (!take_name(s->in + 4, namelen, name))
		return AML_SSTORE_ERR_PARAM;
	memcpy(s->last_name, name, sizeof(s->last_name));
	i = lookup(s, name, namelen);
	if (i < 0)
		return AML_SSTORE_ERR_PARAM;
	s->unsealed = true;
	s->keys[i].state = (s->keys[i].state & ~1U) | 2U;
	return seal(s) ? AML_SSTORE_OK : AML_SSTORE_ERR_NOKEY;
}

/* 0x512136c：只影响之后写块选的加密类型 */
void aml_sstore_set_enctype(struct aml_sstore *s, uint32_t type)
{
	s->enc_req = (int32_t)type;
}

/* 0x512137c */
int64_t aml_sstore_version(const struct aml_sstore *s)
{
	return s->version;
}

bool aml_sstore_e_matches_chip_id(const uint8_t e[12], const uint8_t chip[16])
{
	uint8_t diff = 0U;
	unsigned int i;

	for (i = 0U; i < E_LEN; i++)
		diff |= (uint8_t)(e[i] ^ chip[4U + E_LEN - 1U - i]);
	return diff == 0U;
}
