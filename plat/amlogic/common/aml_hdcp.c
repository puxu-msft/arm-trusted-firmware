/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * HDCP 操作类 SiP 调用（接口与范围见 aml_hdcp.h）。每个调用的寄存器读写序列照厂商 g12a BL31 v1.3:079690f10 的处理函数逐条照抄（地址在各函数前），读改写与轮询的写法也照厂商（bits、poll），主机侧差分逐条比对。
 *
 * 与厂商不同之处：
 * - HDCP 1.4 的 key 不在库时，厂商 0x511e0c0 把栈上未初始化的 0x120 字节当 key（读不到时既不写缓冲也不写属性），这里当作全 0（M12 没有 hdcp key，走的就是这一支）；读到的 key 不足 0x120 字节时，余下的字节同样按 0 算。
 * - 拷进栈上的 key 与加扰后的私钥用完清零（厂商不清）。
 * - 厂商的 0x82000081/82（安全世界把 key 交进来）不提供：本平台没有安全世界的调用者，厂商对非安全调用者也回未知，“key 已交进来”的标志恒为假。
 * - RESULT22 的 0x513b6c4（最近一次实时结果）厂商只写不读，这里不留。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aml_hdcp.h"
#include "aml_sstore.h"

/* 寄存器：内核 hdmitx 的编码（hdmi_tx_reg.h），按厂商每处传给访问器的原值写（同一个寄存器有时带 bit24、有时不带，访问器不看 bit24） */
#define FC_INVIDCONF		0x10001000U
#define FC_AVIVID		0x1000101CU
#define MC_CLKDIS		0x10004001U
#define A_HDCPCFG0		0x11005000U
#define A_HDCPCFG1		0x11005001U
#define A_HDCPOBS0		0x10005002U
#define A_APIINTCLR		0x10005006U
#define A_APIINTMSK		0x10005008U
#define A_VIDPOLCFG		0x10005009U
#define A_OESSWCFG		0x1000500AU
#define A_KSVMEMCTRL		0x10005016U
#define HDCPREG_ANCONF		0x10007805U
#define HDCPREG_AN0		0x10007806U
#define HDCPREG_RMLCTL		0x1000780EU
#define HDCPREG_RMLSTS		0x1000780FU
#define HDCPREG_SEED0		0x11007810U
#define HDCPREG_SEED1		0x11007811U
#define HDCPREG_DPK0		0x11007812U	/* DPK0–DPK6：0x11007812–0x11007818 */
#define HDCP22REG_CTRL		0x11007904U
#define HDCP22REG_STS		0x10007908U
#define HDCP_REVOC_SIZE_0	0x00002299U	/* TOP 的按字节区 */
#define HDCP_REVOC_SIZE_1	0x0000229AU
#define TOP_HDCP22_BSOD		0x0000002AU	/* 内核的定义带 bit24（安全）；厂商 0x511e440 传 0x2a */
#define TOP_PKF_0		0x01000015U	/* PKF_0–3：0x01000015–0x01000018；DUK_0–3：0x01000019–0x0100001C */
#define TOP_DUK_0		0x01000019U

/* 认证成功后的结果保持与唤醒后的宽限，ISA_TIMERE 的 µs 数（厂商 add #0x1e8, lsl #12; add #0x480） */
#define HDCP22_HOLD_US		2000000U

/* HDCP 1.4 key 加密存放（attr bit8）时的 AES-256-CBC 解密钥与 IV（厂商 .data 0x512d278、0x512d268，0x511e040 用） */
static const uint8_t hdcp14_wrap_key[32] = {
	0xad, 0x93, 0x00, 0xc4, 0x8e, 0x50, 0x20, 0xc5, 0x3f, 0xbf, 0x23, 0x32, 0x80, 0x5a, 0xc6, 0xdf,
	0x2f, 0x7d, 0x49, 0xd9, 0x15, 0x8b, 0x7f, 0x04, 0x2c, 0x80, 0xb0, 0x62, 0x78, 0x25, 0x8d, 0x9c,
};
static const uint8_t hdcp14_wrap_iv[16] = {
	0x78, 0x25, 0x8d, 0x9c, 0xad, 0x93, 0x00, 0xc4, 0x2f, 0x7d, 0x49, 0xd9, 0x15, 0x8b, 0x7f, 0x04,
};

static void wipe(void *p, size_t n)
{
	volatile uint8_t *q = p;

	while (n-- != 0U)
		*q++ = 0U;
}

static uint32_t rd(const struct aml_hdcp *h, uint32_t reg)
{
	return h->ops->read(h->ops->ctx, reg);
}

static void wr(const struct aml_hdcp *h, uint32_t reg, uint32_t val)
{
	h->ops->write(h->ops->ctx, reg, val);
}

/* 厂商 0x511eebc：字段 [start, start + len) 换成 val 的低 len 位 */
static void bits(const struct aml_hdcp *h, uint32_t reg, uint32_t val, uint32_t start, uint32_t len)
{
	uint32_t mask = (1U << len) - 1U;
	uint32_t v = rd(h, reg);

	v = (v & ~(mask << start)) | ((val & mask) << start);
	wr(h, reg, v);
}

/* 厂商 0x511ee1c：读到 reg & mask 不为 0 就停；先连读两次（各后跟 0 单位的等待），再按 1、2、4…个等待单位退避，到 timeout × 1000 单位为止，最后再读一次。返回值调用者都不用 */
static void poll(const struct aml_hdcp *h, uint32_t reg, uint32_t mask, uint32_t timeout)
{
	uint64_t limit = (uint64_t)timeout * 1000U;
	uint32_t i;

	if ((rd(h, reg) & mask) != 0U)
		return;
	h->ops->delay(h->ops->ctx, 0U);
	if ((rd(h, reg) & mask) != 0U)
		return;
	h->ops->delay(h->ops->ctx, 0U);
	for (i = 1U; (uint64_t)i < limit; i <<= 1) {
		h->ops->delay(h->ops->ctx, i);
		if ((rd(h, reg) & mask) != 0U)
			return;
	}
	(void)rd(h, reg);
}

/* 厂商 0x511e858：HDCP22REG_STS[6:4] == 2 */
static uint32_t hdcp22_ok(const struct aml_hdcp *h)
{
	return ((rd(h, HDCP22REG_STS) & 0x70U) == 0x20U) ? 1U : 0U;
}

/*
 * 厂商 0x511e1bc–0x511e25c：私钥的加扰。两个 LFSR（25 位的 a、28 位的 b）每步出一个字节与私钥字节异或，每 7 字节一组、组内从高往低。
 * 字节取 a 的低 8 位重排：bit0–7 依次取 a 的 bit7、5、3、1、6、4、2、0。
 */
static void hdcp14_scramble(uint8_t *k, uint32_t n)
{
	uint32_t a = 0x1978F5EU, b = 0xCACA938U;
	uint32_t base, j;

	for (base = 6U; base < n; base += 7U) {
		for (j = 0U; j < 7U; j++) {
			uint32_t m = ((a >> 7) & 1U) | (((a >> 5) & 1U) << 1) | (((a >> 3) & 1U) << 2) |
				     (((a >> 1) & 1U) << 3) | (((a >> 6) & 1U) << 4) | (((a >> 4) & 1U) << 5) |
				     (((a >> 2) & 1U) << 6) | ((a & 1U) << 7);
			uint32_t na = ((a >> 1) | ((a & 1U) << 24)) ^ (b & 0x1FFFFFFU);
			uint32_t nb = ((b & 0x7FFFFFFU) << 1) + (((b >> 24) ^ (b >> 27)) & 1U);

			k[base - j] ^= (uint8_t)m;
			a = na;
			b = nb;
		}
	}
}

/*
 * 厂商 0x511e0c0：从安全存储取 HDCP 1.4 的 key（0x120 字节：KSV 5 字节、3 字节空、私钥 0x118 字节），attr bit8 时先解密；私钥加扰后经 HDCPREG_DPK 按 7 字节一组写进去，每组后轮询 HDCPREG_RMLSTS。
 */
static void hdcp14_load_key(const struct aml_hdcp *h)
{
	uint8_t key[AML_HDCP14_KEY_LEN];
	uint8_t priv[AML_HDCP14_KEY_LEN - 8U];
	uint32_t attr = 0U, g, i;

	memset(key, 0, sizeof(key));
	(void)h->ops->key_read(h->ops->ctx, AML_HDCP14_KEY_NAME, key, sizeof(key), &attr);
	if ((attr & 0x100U) != 0U) {
		struct aml_sstore_aes aes;

		aml_sstore_aes256_init(&aes, hdcp14_wrap_key);
		aml_sstore_cbc_decrypt(&aes, hdcp14_wrap_iv, key, sizeof(key));
		wipe(&aes, sizeof(aes));
	}
	memcpy(priv, key + 8, sizeof(priv));
	hdcp14_scramble(priv, sizeof(priv));

	wr(h, HDCPREG_RMLCTL, 0U);
	poll(h, HDCPREG_RMLSTS, 0x40U, 1U);
	wr(h, HDCPREG_DPK0 + 6U, 0U);
	wr(h, HDCPREG_DPK0 + 5U, 0U);
	for (i = 0U; i < 5U; i++)
		wr(h, HDCPREG_DPK0 + 4U - i, key[4U - i]);
	poll(h, HDCPREG_RMLSTS, 0x41U, 1U);
	wr(h, HDCPREG_RMLCTL, 1U);
	wr(h, HDCPREG_SEED1, 0xA9U);
	wr(h, HDCPREG_SEED0, 0x38U);
	for (g = 0U; g < 40U; g++) {
		for (i = 0U; i < 7U; i++)
			wr(h, HDCPREG_DPK0 + 6U - i, priv[7U * g + 6U - i]);
		poll(h, HDCPREG_RMLSTS, (g == 39U) ? 0x68U : ((g + 2U) | 0x40U), 1U);
	}
	wipe(key, sizeof(key));
	wipe(priv, sizeof(priv));
}

/* 厂商 0x511e47c 里按 VIC 选 A_VIDPOLCFG：这些 VIC 写 0x10，其余写 0x5A */
static uint32_t hdcp14_vidpolcfg(uint32_t vic)
{
	if (((vic - 1U) <= 2U) || ((vic - 6U) <= 9U) || ((vic - 17U) <= 1U) || ((vic - 21U) <= 9U) ||
	    ((vic - 35U) <= 3U) || ((vic - 42U) <= 3U) || ((vic - 48U) < 12U))
		return 0x10U;
	return 0x5AU;
}

/* 0x82000012，厂商 0x511e3f4 */
static void hdcp14_init(const struct aml_hdcp *h)
{
	bits(h, MC_CLKDIS, 0U, 6U, 1U);
	wr(h, A_HDCPCFG0, 0x13U);
	wr(h, A_HDCPCFG1, 0x67U);
	bits(h, TOP_HDCP22_BSOD, 1U, 25U, 1U);
	bits(h, HDCP22REG_CTRL, 3U, 6U, 2U);
	bits(h, HDCP22REG_CTRL, 1U, 1U, 2U);
}

/* 0x82000010，厂商 0x511e47c */
static void hdcp14_enable(const struct aml_hdcp *h)
{
	static const uint8_t an[8] = { 0x16U, 0x74U, 0x31U, 0xB4U, 0xA8U, 0x63U, 0xA6U, 0x88U };
	uint32_t i;

	bits(h, HDCP22REG_CTRL, 1U, 1U, 2U);
	bits(h, MC_CLKDIS, 0U, 6U, 1U);
	wr(h, A_HDCPCFG0, ((rd(h, FC_INVIDCONF) & 0x8U) != 0U) ? 0x13U : 0x12U);
	wr(h, A_VIDPOLCFG, hdcp14_vidpolcfg(rd(h, FC_AVIVID)));
	wr(h, A_OESSWCFG, 0x40U);
	bits(h, A_HDCPCFG0, 0U, 5U, 1U);
	wr(h, A_HDCPCFG1, 0x67U);
	bits(h, A_HDCPCFG1, 0U, 0U, 1U);
	wr(h, A_APIINTMSK, 1U);
	wr(h, A_APIINTCLR, 0xFFU);
	for (i = 0U; i < sizeof(an); i++)
		wr(h, HDCPREG_AN0 + i, an[i]);
	wr(h, HDCPREG_ANCONF, 0U);
	wr(h, A_KSVMEMCTRL, 1U);
	poll(h, A_KSVMEMCTRL, 2U, 1U);
	wr(h, HDCP_REVOC_SIZE_0, 0U);
	wr(h, HDCP_REVOC_SIZE_1, 0U);
	wr(h, A_KSVMEMCTRL, 0U);
	hdcp14_load_key(h);
	bits(h, HDCP22REG_CTRL, 1U, 1U, 2U);
	bits(h, A_HDCPCFG0, 1U, 2U, 1U);
}

/* 0x8200001C，厂商 0x511eaf4：hdcp22_fw_private 的 32 字节按小端字写进 PKF_3..0、DUK_3..0；读满 32 字节才回 1，读不到时写 0 */
static uint32_t hdcp22_set_duk(const struct aml_hdcp *h)
{
	uint8_t k[AML_HDCP22_KEY_LEN];
	uint32_t ok, i;

	memset(k, 0, sizeof(k));
	ok = (h->ops->key_read(h->ops->ctx, AML_HDCP22_KEY_NAME, k, sizeof(k), NULL) == sizeof(k)) ? 1U : 0U;
	for (i = 0U; i < 8U; i++) {
		const uint8_t *p = &k[4U * i];
		uint32_t reg = (i < 4U) ? (TOP_PKF_0 + 3U - i) : (TOP_DUK_0 + 7U - i);

		wr(h, reg, (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
	}
	wipe(k, sizeof(k));
	return ok;
}

/* 0x82000017，厂商 0x511e8f4：HDCP 2.2 认证结果，认证成功后保持 2 s；挂起期间与唤醒后 2 s 内回挂起时的结果 */
static uint32_t hdcp22_result(struct aml_hdcp *h)
{
	if (!h->started) {
		uint32_t t = h->ops->now(h->ops->ctx);

		h->started = true;
		h->hold_end = t;
		h->grace_end = t;
	}
	if (h->suspended)
		return h->frozen;
	if (h->grace) {
		if ((int32_t)(h->grace_end - h->ops->now(h->ops->ctx)) >= 0)
			return (hdcp22_ok(h) != 0U) ? 1U : h->frozen;
		h->grace = false;
	}
	if ((rd(h, HDCP22REG_CTRL) & 0x4U) == 0U) {
		h->held = 0U;
		h->hold_end = h->ops->now(h->ops->ctx);
		return 0U;
	}
	if (hdcp22_ok(h) != 0U) {
		h->held = 1U;
		h->hold_end = h->ops->now(h->ops->ctx) + HDCP22_HOLD_US;
		return 1U;
	}
	if ((int32_t)(h->hold_end - h->ops->now(h->ops->ctx)) < 0)
		h->held = 0U;
	return h->held;
}

/* 0x8200008A，厂商 0x511e898：x1 != 0 进挂起（记下此刻的结果），x1 == 0 出挂起、起 2 s 宽限 */
static void hdcp22_suspend(struct aml_hdcp *h, uint32_t on)
{
	h->suspended = (on != 0U);
	if (h->suspended) {
		h->frozen = hdcp22_ok(h);
	} else {
		h->grace_end = h->ops->now(h->ops->ctx) + HDCP22_HOLD_US;
		h->grace = true;
	}
}

void aml_hdcp_init(struct aml_hdcp *h, const struct aml_hdcp_ops *ops)
{
	memset(h, 0, sizeof(*h));
	h->ops = ops;
}

bool aml_hdcp_smc(struct aml_hdcp *h, uint32_t fid, uint64_t x1, uint64_t *ret)
{
	*ret = 0U;
	switch (fid) {
	case AML_SM_HDCP14_ENABLE:
		hdcp14_enable(h);
		return true;
	case AML_SM_HDCP14_RESULT:
		/* 厂商 0x511e72c：A_HDCPOBS0 bit0；分派处 sxtw */
		*ret = rd(h, A_HDCPOBS0) & 1U;
		return true;
	case AML_SM_HDCP14_INIT:
		hdcp14_init(h);
		return true;
	case AML_SM_HDCP14_EN_ENCRYPT:
		/* 厂商 0x511e6d8：A_HDCPCFG1 bit1（关加密）清零 */
		bits(h, A_HDCPCFG1, 0U, 1U, 1U);
		return true;
	case AML_SM_HDCP14_OFF:
		/* 厂商 0x511e6f0：关加密，A_HDCPCFG0 bit2 清零 */
		bits(h, A_HDCPCFG1, 1U, 1U, 1U);
		bits(h, A_HDCPCFG0, 0U, 2U, 1U);
		return true;
	case AML_SM_HDCP_MUX_22:
		/* 厂商 0x511e880（操作表 +0x150）：HDCP22REG_CTRL[2:1] = 3 */
		bits(h, HDCP22REG_CTRL, 3U, 1U, 2U);
		return true;
	case AML_SM_HDCP_MUX_14:
		/* 厂商 0x511e74c（操作表 +0x148）：HDCP22REG_CTRL[2:1] = 1 */
		bits(h, HDCP22REG_CTRL, 1U, 1U, 2U);
		return true;
	case AML_SM_HDCP22_RESULT:
		*ret = hdcp22_result(h);
		return true;
	case AML_SM_HDCP22_SET_DUK:
		*ret = hdcp22_set_duk(h);
		return true;
	case AML_SM_HDCP22_SET_TOPO:
		/* 厂商 0x511ecc0：按 w1 比 */
		h->topo = ((uint32_t)x1 != 0U) ? 1U : 0U;
		return true;
	case AML_SM_HDCP22_GET_TOPO:
		*ret = h->topo;
		return true;
	case AML_SM_HDCP22_SUSPEND:
		hdcp22_suspend(h, (uint32_t)x1);
		return true;
	default:
		return false;
	}
}
