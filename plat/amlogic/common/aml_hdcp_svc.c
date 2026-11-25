/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * HDCP 操作类 SiP 调用接进 BL31（AML_HDCP，要 AML_SIP_VENDOR_SMALL 与 AML_SSTORE）：寄存器经 aml_hdmitx.c 的安全访问器，时基是 ISA_TIMERE，key 经 aml_sstore_key_get 从安全存储读。
 * 调用在一把锁下串行：寄存器序列与去抖状态都不可重入（厂商不加锁）。安全与非安全调用者一样处理（厂商的分派不看调用者）。
 */

#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <platform_def.h>
#include <stdbool.h>
#include <stdint.h>

#include "aml_hdcp.h"
#include "aml_private.h"

static struct aml_hdcp hdcp;
static spinlock_t hdcp_lock;

static uint32_t hdcp_read(void *ctx, uint32_t reg)
{
	(void)ctx;
	return aml_hdmitx_sec_read(reg);
}

static void hdcp_write(void *ctx, uint32_t reg, uint32_t val)
{
	(void)ctx;
	aml_hdmitx_sec_write(reg, val);
}

/* 厂商 0x511ece0：每个单位等 64 位的 ISA_TIMERE 走过 2 个计数以上（约 1–2 µs） */
static void hdcp_delay(void *ctx, uint32_t units)
{
	(void)ctx;
	while (units-- != 0U) {
		uint64_t t0 = mmio_read_64(AML_ISA_TIMERE);

		while ((mmio_read_64(AML_ISA_TIMERE) - t0) <= 1U)
			;
	}
}

static uint32_t hdcp_now(void *ctx)
{
	(void)ctx;
	return mmio_read_32(AML_ISA_TIMERE);
}

static uint32_t hdcp_key_read(void *ctx, const char *name, uint8_t *buf, uint32_t len, uint32_t *attr)
{
	(void)ctx;
	return aml_sstore_key_get(name, buf, len, attr);
}

static const struct aml_hdcp_ops hdcp_ops = {
	.read = hdcp_read,
	.write = hdcp_write,
	.delay = hdcp_delay,
	.now = hdcp_now,
	.key_read = hdcp_key_read,
};

void aml_hdcp_setup(void)
{
	aml_hdcp_init(&hdcp, &hdcp_ops);
}

bool aml_hdcp_sip(uint32_t fid, u_register_t x1, u_register_t *ret)
{
	uint64_t r;
	bool known;

	spin_lock(&hdcp_lock);
	known = aml_hdcp_smc(&hdcp, fid, x1, &r);
	spin_unlock(&hdcp_lock);
	*ret = (u_register_t)r;
	return known;
}
