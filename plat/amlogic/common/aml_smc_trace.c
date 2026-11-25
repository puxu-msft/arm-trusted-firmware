/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 试验构建（AML_TRIAL=1，打开 PLAT_SMC_TRACE）的调用跟踪：每次 SiP 与 PSCI 调用进入时写一行
 * `smc#<序号> c<核> <调用者> <fid> #<该 fid 的第几次> <x1> <x2> <x3>`，返回时写 `smc#<序号> -> <x0>`，只写进内存日志（不经 UART，免得在 cpuidle 一类的路径上忙等）。
 * 调用者是保存的 SPSR_EL3：AArch64 记 `el<n>`，AArch32 记 `a32-<模式>`。只记寄存器，不读共享缓冲：安全存储的请求与应答（名字、数据）不进日志，长度只在 x1 里出现时（NOTIFY_EX 的块大小）才出现。
 * 每次调用进入与返回时还把序号、核号与 fid 写进 PREG_STICKY_REG1（格式见 g12a_def.h 的 AML_TRIAL_SMC_MARK；内存日志过不了复位，这个寄存器过得了）；CPU_SUSPEND 与 CPU_OFF 不改它（cpuidle 与下线核的调用不返回、又极频繁，会盖掉最后一次有意义的调用）。
 * 每个 fid 只写前 AML_SMC_TRACE_PER_FID 次、总共只写前 AML_SMC_TRACE_LINES 次调用（内核的 cpuidle 每秒几十次 CPU_SUSPEND），之后只计数；到上限时各写一行说明，总上限时、以及 SYSTEM_RESET/SYSTEM_OFF 进入时另写一张各 fid 的次数表（表外的 fid 合计在 other 里）。
 * 盲区：没有注册服务的 OEN（如 Trusted OS 的 0xB2xxxxxx，由 runtime_svc.c 的 smc_unknown() 回 SMC_UNK）与 Arm 架构服务（0x8000xxxx）的调用不经这里。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include <arch.h>
#include <context.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <plat/common/platform.h>
#include <platform_def.h>

#include "aml_private.h"

#define AML_SMC_TRACE_FIDS	48U

/* 不改复位记号的 PSCI 调用（值同 lib/psci/psci.h） */
#define FID_CPU_SUSPEND_32	0x84000001U
#define FID_CPU_SUSPEND_64	0xC4000001U
#define FID_CPU_OFF		0x84000002U
/* 进入时补写一张次数表的 PSCI 调用 */
#define FID_SYSTEM_OFF		0x84000008U
#define FID_SYSTEM_RESET	0x84000009U
#define FID_SYSTEM_RESET2_32	0x84000012U
#define FID_SYSTEM_RESET2_64	0xC4000012U
#define AML_SMC_TRACE_PER_FID	24U
#define AML_SMC_TRACE_LINES	400U

static spinlock_t trace_lock;
static uint32_t trace_seq;
static uint32_t trace_printed;
static bool trace_capped;
static uint32_t trace_fid[AML_SMC_TRACE_FIDS];
static uint32_t trace_count[AML_SMC_TRACE_FIDS];
static uint32_t trace_fids;
static uint32_t trace_other;	/* fid 表满之后进来的调用次数 */

static void trace_line(const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		aml_trial_memlog_puts(buf);
}

/* 调用者的执行态与异常级 / 模式，取自保存的 SPSR_EL3 */
static const char *caller_state(void *handle, char *buf, size_t len)
{
	u_register_t spsr = read_ctx_reg(get_el3state_ctx(handle), CTX_SPSR_EL3);
	static const char *const a32_mode[16] = {
		"usr", "fiq", "irq", "svc", "m4", "m5", "mon", "abt",
		"m8", "m9", "hyp", "und", "m12", "m13", "m14", "sys",
	};

	if (((spsr >> MODE_RW_SHIFT) & MODE_RW_MASK) == MODE_RW_32)
		(void)snprintf(buf, len, "a32-%s", a32_mode[spsr & 0xFU]);
	else
		(void)snprintf(buf, len, "el%u", (unsigned int)((spsr >> MODE_EL_SHIFT) & MODE_EL_MASK));
	return buf;
}

/* 返回该 fid 这一次是第几次（从 1 起）；表满时返回 0（不再按 fid 计） */
static uint32_t count_fid(uint32_t fid)
{
	uint32_t i;

	for (i = 0U; i < trace_fids; i++) {
		if (trace_fid[i] == fid)
			return ++trace_count[i];
	}
	if (trace_fids == AML_SMC_TRACE_FIDS) {
		trace_other++;
		return 0U;
	}
	trace_fid[trace_fids] = fid;
	trace_count[trace_fids] = 1U;
	trace_fids++;
	return 1U;
}

static void trace_table(const char *why)
{
	uint32_t i;

	trace_line("smc trace: %s (seq %u), per fid:\n", why, trace_seq);
	for (i = 0U; i < trace_fids; i++)
		trace_line("smc trace:   %08x %u\n", trace_fid[i], trace_count[i]);
	trace_line("smc trace:   other %u\n", trace_other);
}

static bool marks_call(uint32_t fid)
{
	return (fid != FID_CPU_SUSPEND_32) && (fid != FID_CPU_SUSPEND_64) && (fid != FID_CPU_OFF);
}

void plat_smc_trace(uint32_t smc_fid, u_register_t x1, u_register_t x2,
		    u_register_t x3, void *handle, bool done, u_register_t ret)
{
	/* 每个核上进入与返回成对出现（中间可能被别的核插进来），按核记下这次调用的序号与是否写了 */
	static uint32_t cur_seq[PLATFORM_CORE_COUNT];
	static bool cur_printed[PLATFORM_CORE_COUNT];
	unsigned int core = plat_my_core_pos();
	char who[12];
	uint32_t nth;

	if (core >= PLATFORM_CORE_COUNT)
		return;
	spin_lock(&trace_lock);
	if (!done)
		cur_seq[core] = ++trace_seq;
	/* 最近一次调用另记进 PREG_STICKY_REG1（g12a_def.h 的 AML_TRIAL_SMC_MARK_REG） */
	if (marks_call(smc_fid))
		mmio_write_32(AML_TRIAL_SMC_MARK_REG,
			      AML_TRIAL_SMC_MARK(cur_seq[core], core, smc_fid, done));
	if (done) {
		if (cur_printed[core])
			trace_line("smc#%u -> %lx\n", cur_seq[core], (unsigned long)ret);
		cur_printed[core] = false;
		spin_unlock(&trace_lock);
		return;
	}
	nth = count_fid(smc_fid);
	cur_printed[core] = (trace_printed < AML_SMC_TRACE_LINES) &&
			    (nth != 0U) && (nth <= AML_SMC_TRACE_PER_FID);
	if (cur_printed[core]) {
		trace_printed++;
		trace_line("smc#%u c%u %s %08x #%u %lx %lx %lx\n", cur_seq[core], core,
			   caller_state(handle, who, sizeof(who)), smc_fid, nth,
			   (unsigned long)x1, (unsigned long)x2, (unsigned long)x3);
		if (nth == AML_SMC_TRACE_PER_FID)
			trace_line("smc trace: fid %08x reached %u calls, counting only\n",
				   smc_fid, AML_SMC_TRACE_PER_FID);
	}
	if ((trace_printed == AML_SMC_TRACE_LINES) && !trace_capped) {
		trace_capped = true;
		trace_table("400 calls written, counting only");
	}
	if ((smc_fid == FID_SYSTEM_RESET) || (smc_fid == FID_SYSTEM_OFF) ||
	    (smc_fid == FID_SYSTEM_RESET2_32) || (smc_fid == FID_SYSTEM_RESET2_64))
		trace_table("system reset/off");
	spin_unlock(&trace_lock);
}
