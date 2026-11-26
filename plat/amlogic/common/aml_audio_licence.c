/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 音频许可查询（SiP 0x82000050）的协议，照厂商 0x511d0ec，协议格式见 include/aml_audio_licence.h。返回值与厂商相同：
 *   0xa0b  +0 不是 "AMQR" 或 +4 不是 0x82000050（查的是共享区里的 fid，调用时的 x1、x2 不用）
 *   0xa11  +0xc 不是 RSA1/RSA2/RSA4，或 +8 的 16 位总长与之不符（+0xa 起两字节不看）
 *   0x1dc  RSA4：选钥函数 0x511db94 只认索引 0–3
 *   0xa40  解密失败：块按大端读出的数大于模数（等于模数照收，解出 0）
 *   0xa45  解出的 +0x14 或 +0x18 不是 "AMQR"，或项数大于 0x6f
 *   0xa64  某一项的类型不在 1–4：它的结果写 2，此前的项已填
 *   0xa77  加密失败：填好结果的块大于加密钥的模数
 *   0      成功
 * 厂商在 0xa40 之前不动共享区；0xa45、0xa64、0xa77 时共享区里留着解出的明文（0xa64、0xa77 时已填了结果），这里一样。
 */

#include <string.h>

#include "aml_audio_licence.h"

#define Q_MAGIC		"AMQR"
#define Q_BLOCK		0x10U	/* RSA 块 */
#define Q_COUNT		0x13U	/* 以下是解出的明文里的位置 */
#define Q_MAGIC2	0x14U
#define Q_MAGIC3	0x18U
#define Q_ITEMS		0x20U
#define Q_MAX_ITEMS	0x6fU

#define E_HEADER	0xa0bU
#define E_KIND		0xa11U
#define E_KEY_INDEX	0x1dcU
#define E_DECRYPT	0xa40U
#define E_PLAIN		0xa45U
#define E_ITEM_TYPE	0xa64U
#define E_ENCRYPT	0xa77U

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * 用钥 idx 把块原地做一次私钥运算（厂商 0x511db94 选钥 + 0x511d7bc）：idx 大于 3 回 E_KEY_INDEX；模数不是 0x80 或 0x100 字节（0x511d7bc 回 3）或运算出错时回 fail；成功回 0。块长等于模数的字节数。
 */
static uint64_t rsa_step(struct aml_rsa_work *w, const struct aml_rsa_key *keys, uint32_t idx, uint8_t *blk, uint64_t fail)
{
	size_t size, outlen;

	if (idx >= AML_AUDIO_LICENCE_KEYS)
		return E_KEY_INDEX;
	size = aml_rsa_load(w, &keys[idx]);
	if ((size != 0x80U) && (size != 0x100U))
		return fail;
	outlen = size;
	return (aml_rsa_private(w, blk, size, blk, &outlen) == AML_RSA_OK) ? 0U : fail;
}

/* 一项的结果（厂商分派表 0x51287f0 的四个函数各回“位取反”，0x511d2a8 再按是否为 0 写 1 或 0，等于位本身）；类型不认识时回 2 */
static uint8_t item_result(uint8_t type, uint32_t lic1, uint32_t lic2)
{
	switch (type) {
	case 1U:
		return (uint8_t)((lic1 >> 1) & 1U);	/* 0x511a1e8：0xff630034 bit1 */
	case 2U:
		return (uint8_t)(lic1 & 1U);		/* 0x511a200：0xff630034 bit0 */
	case 3U:
		return (uint8_t)((lic2 >> 15) & 1U);	/* 0x511a220：0xff630038 bit15 */
	case 4U:
		return 1U;				/* 0x511a218：恒回 0 */
	default:
		return 2U;
	}
}

uint64_t aml_audio_licence_query(uint8_t *shm, const struct aml_rsa_key keys[AML_AUDIO_LICENCE_KEYS],
				 uint32_t lic1, uint32_t lic2, struct aml_rsa_work *w)
{
	static const struct {
		char tag[4];
		uint16_t len;
		uint8_t dec, enc;
	} kinds[] = {
		{ { 'R', 'S', 'A', '1' }, 0x90U, 0U, 1U },
		{ { 'R', 'S', 'A', '2' }, 0x110U, 2U, 3U },
		{ { 'R', 'S', 'A', '4' }, 0x210U, 4U, 5U },
	};
	uint32_t k, i, count;
	uint64_t r;

	if ((memcmp(shm, Q_MAGIC, 4U) != 0) || (rd32(shm + 4U) != AML_AUDIO_LICENCE_FID))
		return E_HEADER;
	for (k = 0U; k < (uint32_t)(sizeof(kinds) / sizeof(kinds[0])); k++) {
		if (memcmp(shm + 0xcU, kinds[k].tag, 4U) == 0)
			break;
	}
	if ((k == (uint32_t)(sizeof(kinds) / sizeof(kinds[0]))) ||
	    (((uint32_t)shm[8] | ((uint32_t)shm[9] << 8)) != kinds[k].len))
		return E_KIND;

	r = rsa_step(w, keys, kinds[k].dec, shm + Q_BLOCK, E_DECRYPT);
	if (r != 0U)
		return r;
	/* 项数只读一次：共享区在非安全世界手里，判上限与用它的必须是同一个值 */
	count = shm[Q_COUNT];
	if ((memcmp(shm + Q_MAGIC2, Q_MAGIC, 4U) != 0) || (memcmp(shm + Q_MAGIC3, Q_MAGIC, 4U) != 0) ||
	    (count > Q_MAX_ITEMS))
		return E_PLAIN;
	for (i = 0U; i < count; i++) {
		uint8_t v = item_result(shm[Q_ITEMS + (2U * i)], lic1, lic2);

		shm[Q_ITEMS + (2U * i) + 1U] = v;
		if (v == 2U)
			return E_ITEM_TYPE;
	}
	return rsa_step(w, keys, kinds[k].enc, shm + Q_BLOCK, E_ENCRYPT);
}
