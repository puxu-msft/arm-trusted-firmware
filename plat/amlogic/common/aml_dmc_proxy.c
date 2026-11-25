/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * DMC 安全寄存器的读写代理（SiP 0x8200004A，g12a_def.h）与它拒绝的请求的记录。
 * 代理只服务 AML_DMC_SEC_BASE 那一页（内核 dmc_monitor 用的范围）；厂商不查地址，页外与不对齐的地址这里不读不写、回 0。是否放宽这个范围要看实际有没有调用者要页外的地址，所以每次因为地址拒绝的读或写都记下来源：
 * - 来源是调用者的 LR（调用者模式下的返回地址，由调用方传进来：AArch32 按模式取分组的 LR，AArch64 取 X30）。ELR_EL3 只是发 smc 的那条指令之后的地址，内核的 SMC 都经同一个 __arm_smccc_smc 发出，分不出调用者，只进日志。
 * - 环形记录 AML_DMC_PROXY_LOG_SLOTS 条，按（地址、读写、调用者的 SPSR_EL3[4:0]、LR）去重，同一来源只加次数（饱和）；满了覆盖最早进来的一条；另记拒绝总次数与新建记录的次数（被覆盖的来源再来算新建，都饱和）。
 * - 经本分支私有的 SiP 0x820000F0（AML_SM_DMC_PROXY_LOG）读出，返回值按 SMC32 只用低 32 位：x1 = 0xFFFFFFFF 时回 {0, 总次数, 记录里的条数, 新建记录的次数}；x1 = i（0 是最早的一条）时回 {0, 地址低 32 位, LR 低 32 位, 次数（24 位饱和）<< 8 | 地址高 32 位非 0 << 7 | 写 << 5 | SPSR[4:0]}；别的 x1 回 x0 = 0xFFFFFFFF。厂商 BL31 对这个 fid 回 0xFFFFFFFF。
 * - 控制台一行（NOTICE：定型构建也打；试验构建另进内存日志）：新建记录的次数不超过槽数或到 2 的幂时，以及同一来源的次数到 2 的幂时。
 * rw 不是 0、1 的请求厂商同样不读不写、回 0，与我们一致，不算越界、不记。
 */

#include <common/debug.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <platform_def.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "aml_private.h"

struct aml_dmc_refused {
	u_register_t addr;
	u_register_t lr;	/* 调用者的返回地址：去重与读出用 */
	u_register_t elr;	/* 只进日志 */
	uint32_t rw;
	uint32_t mode;		/* 调用者保存的 SPSR_EL3[4:0]：bit4 = AArch32，AArch32 的 [3:0] 是模式，AArch64 的 [3:2] 是 EL */
	uint32_t count;
};

static spinlock_t refused_lock;
static struct aml_dmc_refused refused[AML_DMC_PROXY_LOG_SLOTS];
static uint32_t refused_next;		/* 下一条写进的槽 */
static uint32_t refused_held;		/* 记录里的条数 */
static uint32_t refused_records;	/* 新建记录的次数 */
static uint32_t refused_total;		/* 拒绝总次数 */

static void sat_inc(uint32_t *v)
{
	if (*v != UINT32_MAX)
		(*v)++;
}

static void caller_name(uint32_t mode, char *buf, size_t len)
{
	static const char *const a32_mode[16] = {
		"usr", "fiq", "irq", "svc", "m4", "m5", "mon", "abt",
		"m8", "m9", "hyp", "und", "m12", "m13", "m14", "sys",
	};

	if ((mode & 0x10U) == 0U)
		(void)snprintf(buf, len, "el%u", (mode >> 2) & 3U);
	else
		(void)snprintf(buf, len, "a32-%s", a32_mode[mode & 0xFU]);
}

static bool pow2(uint32_t v)
{
	return (v != 0U) && ((v & (v - 1U)) == 0U);
}

static void refused_record(u_register_t addr, uint32_t rw, u_register_t spsr,
			   u_register_t lr, u_register_t elr)
{
	uint32_t mode = (uint32_t)spsr & 0x1FU;
	struct aml_dmc_refused *e = NULL;
	uint32_t oldest, i, count, records;
	bool fresh = false;
	char who[16];

	spin_lock(&refused_lock);
	oldest = (refused_next + AML_DMC_PROXY_LOG_SLOTS - refused_held) % AML_DMC_PROXY_LOG_SLOTS;
	sat_inc(&refused_total);
	for (i = 0U; i < refused_held; i++) {
		struct aml_dmc_refused *c = &refused[(oldest + i) % AML_DMC_PROXY_LOG_SLOTS];

		if ((c->addr == addr) && (c->rw == rw) && (c->mode == mode) && (c->lr == lr)) {
			e = c;
			break;
		}
	}
	if (e == NULL) {
		e = &refused[refused_next];
		e->addr = addr;
		e->lr = lr;
		e->rw = rw;
		e->mode = mode;
		e->count = 0U;
		refused_next = (refused_next + 1U) % AML_DMC_PROXY_LOG_SLOTS;
		if (refused_held < AML_DMC_PROXY_LOG_SLOTS)
			refused_held++;
		sat_inc(&refused_records);
		fresh = true;
	}
	e->elr = elr;
	sat_inc(&e->count);
	count = e->count;
	records = refused_records;
	spin_unlock(&refused_lock);

	/* 新建记录（前 AML_DMC_PROXY_LOG_SLOTS 次与之后到 2 的幂时），或同一来源的次数到 2 的幂：控制台一行 */
	if (fresh ? ((records <= AML_DMC_PROXY_LOG_SLOTS) || pow2(records)) : pow2(count)) {
		caller_name(mode, who, sizeof(who));
		NOTICE("BL31: DMC proxy refused %s 0x%lx from %s LR 0x%lx ELR 0x%lx (x%u, record %u)\n",
		       (rw == 0U) ? "read" : "write", (unsigned long)addr, who,
		       (unsigned long)lr, (unsigned long)elr, count, records);
	}
}

u_register_t aml_dmc_proxy_rw(u_register_t addr, u_register_t val, u_register_t rw,
			      u_register_t spsr, u_register_t lr, u_register_t elr)
{
	if (rw > 1U)
		return 0;
	if ((addr < AML_DMC_SEC_BASE) || (addr >= AML_DMC_SEC_BASE + AML_DMC_SEC_SIZE) ||
	    ((addr & 3U) != 0U)) {
		refused_record(addr, (uint32_t)rw, spsr, lr, elr);
		return 0;
	}
	if (rw == 0U)
		return mmio_read_32(addr);
	mmio_write_32(addr, (uint32_t)val);
	return 0;
}

void aml_dmc_proxy_log_read(u_register_t index, uint32_t out[4])
{
	uint32_t i = (uint32_t)index;

	out[0] = 0U;
	spin_lock(&refused_lock);
	if (i == UINT32_MAX) {
		out[1] = refused_total;
		out[2] = refused_held;
		out[3] = refused_records;
	} else if (i < refused_held) {
		const struct aml_dmc_refused *e =
			&refused[(refused_next + AML_DMC_PROXY_LOG_SLOTS - refused_held + i) % AML_DMC_PROXY_LOG_SLOTS];
		uint32_t count = (e->count > 0xFFFFFFU) ? 0xFFFFFFU : e->count;

		out[1] = (uint32_t)e->addr;
		out[2] = (uint32_t)e->lr;
		out[3] = (count << 8) | (((((uint64_t)e->addr) >> 32) != 0U) ? 0x80U : 0U) |
			 (e->rw << 5) | e->mode;
	} else {
		out[0] = UINT32_MAX;
		out[1] = 0U;
		out[2] = 0U;
		out[3] = 0U;
	}
	spin_unlock(&refused_lock);
}
