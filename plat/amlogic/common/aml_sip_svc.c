/*
 * Copyright (c) 2018-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <common/debug.h>
#include <common/runtime_svc.h>
#include <context.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/mmio.h>
#include <platform_def.h>
#if PLAT_SMC_TRACE
#include <plat/common/platform.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aml_private.h"

#ifndef AML_SSTORE
#define AML_SSTORE	0
#endif

#ifndef AML_HDCP
#define AML_HDCP	0
#endif

#ifndef AML_TRIAL_EFUSE_DUMP
#define AML_TRIAL_EFUSE_DUMP	0
#endif

#if AML_TRIAL_EFUSE_DUMP
#include <lib/cassert.h>
/* 导出写进输出共享区，不越过非安全共享区 */
CASSERT(AML_SHARE_MEM_OUTPUT_BASE + AML_EFUSE_DUMP_SIZE <= AML_NS_SHARE_MEM_BASE + AML_NS_SHARE_MEM_SIZE,
	assert_efuse_dump_in_ns_share_mem);
#endif

struct aml_cpu_info {
	uint32_t version;
	uint8_t chip_id[16];
};

static int aml_sip_get_chip_id(uint64_t version)
{
	struct aml_cpu_info *info = (void *)AML_SHARE_MEM_OUTPUT_BASE;
	uint32_t size;

	if (version > 2)
		return -1;

	memset(info, 0, sizeof(struct aml_cpu_info));

	if (version == 2) {
		info->version = 2;
		size = 16;
	} else {
		info->version = 1;
		size = 12;
	}

	if (aml_scpi_get_chip_id(info->chip_id, size) == 0)
		return -1;

	return 0;
}

#if defined(AML_SIP_VENDOR_SMALL) && AML_SIP_VENDOR_SMALL
/*
 * 调用者模式下的返回地址（DMC 代理的越界记录用来分辨调用者；ELR_EL3 只是发 smc 那条指令的下一条，内核都经同一个 __arm_smccc_smc 发）：
 * AArch64 取 X30；AArch32 按模式取分组的 LR 在 AArch64 寄存器里的位置（Arm ARM 的 AArch32 寄存器映射）：usr、sys、hyp → X14，irq → X16，svc → X18，abt → X20，und → X22，fiq → X30。
 */
static u_register_t aml_caller_lr(cpu_context_t *ctx, u_register_t spsr)
{
	gp_regs_t *gp = get_gpregs_ctx(ctx);

	if (((spsr >> MODE_RW_SHIFT) & MODE_RW_MASK) != MODE_RW_32)
		return read_ctx_reg(gp, CTX_GPREG_LR);
	switch (spsr & MODE32_MASK) {
	case MODE32_irq:	return read_ctx_reg(gp, CTX_GPREG_X16);
	case MODE32_svc:	return read_ctx_reg(gp, CTX_GPREG_X18);
	case MODE32_abt:	return read_ctx_reg(gp, CTX_GPREG_X20);
	case MODE32_und:	return read_ctx_reg(gp, CTX_GPREG_X22);
	case MODE32_fiq:	return read_ctx_reg(gp, CTX_GPREG_LR);
	default:		return read_ctx_reg(gp, CTX_GPREG_X14);
	}
}

/* AO_RTI_STATUS_REG3 的一个字段换成 val（读改写） */
static void aml_ao_reg3_set(uint32_t mask, uint32_t val)
{
	mmio_clrsetbits_32(AML_AO_RTI_STATUS_REG3, mask, val & mask);
}

/* SET_USB_BOOT 的模式 1–4（CLEAR、FORCE、RUN_COMD、PANIC_DUMP）到 AO_RTI_STATUS_REG3[15:12] 的值，与厂商跳转表 0x5128048 相同 */
static void aml_sip_set_usb_boot(u_register_t mode)
{
	static const uint8_t flag[4] = { 0U, 1U, 2U, AML_USB_BOOT_FLAG_PANIC_DUMP };

	if ((mode < 1U) || (mode > 4U))
		return;
	aml_ao_reg3_set(AML_USB_BOOT_FLAG_MASK, (uint32_t)flag[mode - 1U] << AML_USB_BOOT_FLAG_SHIFT);
}
#endif

/*******************************************************************************
 * This function is responsible for handling all SiP calls
 ******************************************************************************/
static uintptr_t aml_sip_handler(uint32_t smc_fid,
				  u_register_t x1, u_register_t x2,
				  u_register_t x3, u_register_t x4,
				  void *cookie, void *handle,
				  u_register_t flags)
{
	switch (smc_fid) {

	case AML_SM_GET_SHARE_MEM_INPUT_BASE:
		SMC_RET1(handle, AML_SHARE_MEM_INPUT_BASE);

	case AML_SM_GET_SHARE_MEM_OUTPUT_BASE:
		SMC_RET1(handle, AML_SHARE_MEM_OUTPUT_BASE);

	case AML_SM_EFUSE_READ:
	{
		void *dst = (void *)AML_SHARE_MEM_OUTPUT_BASE;
		uint64_t ret = aml_efuse_read(dst, (uint32_t)x1, x2);

		SMC_RET1(handle, ret);
	}
	case AML_SM_EFUSE_USER_MAX:
		SMC_RET1(handle,  aml_efuse_user_max());

	case AML_SM_JTAG_ON:
		aml_scpi_jtag_set_state(AML_JTAG_STATE_ON, x1);
		SMC_RET1(handle, 0);

	case AML_SM_JTAG_OFF:
		aml_scpi_jtag_set_state(AML_JTAG_STATE_OFF, x1);
		SMC_RET1(handle, 0);

	case AML_SM_GET_CHIP_ID:
		SMC_RET1(handle, aml_sip_get_chip_id(x1));

#if AML_TRIAL_EFUSE_DUMP
	case AML_SM_TRIAL_EFUSE_DUMP:
	{
		/* 试验构建的整片 eFuse 只读导出（aml_efuse_dump.c）：参数不用；x0 读出的字节数，x1–x3 读出、字节数不足、两遍不同的块 */
		uint32_t m[3];
		uint32_t n = aml_efuse_dump((uint8_t *)AML_SHARE_MEM_OUTPUT_BASE, m);

		SMC_RET4(handle, n, m[0], m[1], m[2]);
	}
#endif

#if defined(AML_SIP_VENDOR_SMALL) && AML_SIP_VENDOR_SMALL
	case AML_SM_HDMITX_SEC_READ:
		SMC_RET1(handle, aml_hdmitx_sec_read((uint32_t)x1));

	case AML_SM_HDMITX_SEC_WRITE:
		aml_hdmitx_sec_write((uint32_t)x1, (uint32_t)x2);
		SMC_RET1(handle, 0);

	case AML_SM_SET_SUSPEND_REASON:
		aml_ao_reg3_set(AML_AO_RTI_STATUS_REG3_SUSPEND_MASK,
				(x1 == 1U) ? AML_AO_RTI_STATUS_REG3_SUSPEND : 0U);
		SMC_RET1(handle, 0);

	case AML_SM_SET_USB_BOOT:
		aml_sip_set_usb_boot(x1);
		SMC_RET1(handle, 0);

	case AML_SM_EFUSE_READ_CALI:
		SMC_RET1(handle, aml_efuse_read_cali((void *)AML_SHARE_MEM_OUTPUT_BASE,
						     (uint32_t)x1, (uint32_t)x2));

	case AML_SM_EFUSE_WRITE:
		SMC_RET1(handle, 0);

	case AML_SM_TEST_N_OUTPUT:
		/* 厂商按 w1 比（0x5119574 cmp w0, #1）：SMC32 的高 32 位不算 */
		if ((uint32_t)x1 == 1U)
			mmio_setbits_32(AML_AO_GPIO_TEST_N, 1U);
		else if ((uint32_t)x1 == 0U)
			mmio_clrbits_32(AML_AO_GPIO_TEST_N, 1U);
		SMC_RET1(handle, 0);

	case AML_SM_DMC_RW:
	{
		cpu_context_t *ctx = (cpu_context_t *)handle;
		u_register_t spsr = read_ctx_reg(get_el3state_ctx(ctx), CTX_SPSR_EL3);

		SMC_RET1(handle, aml_dmc_proxy_rw(x1, x2, x3, spsr, aml_caller_lr(ctx, spsr),
						  read_ctx_reg(get_el3state_ctx(ctx), CTX_ELR_EL3)));
	}

	case AML_SM_DMC_PROXY_LOG:
	{
		uint32_t r[4];

		aml_dmc_proxy_log_read(x1, r);
		SMC_RET4(handle, r[0], r[1], r[2], r[3]);
	}

	case AML_SM_SET_REBOOT_REASON:
		/* 厂商把 x1 整个或进 REG3（0x5119ea4）；这里只取 [3:0]，越界的高位不写进别的字段 */
		aml_ao_reg3_set(AML_REBOOT_REASON_MASK, (uint32_t)x1);
		SMC_RET1(handle, 0);

	case AML_SM_DATA_PROCESS:
		if ((x1 == AML_DATA_PROCESS_BOOT_CHECK) ||
		    (x1 == AML_DATA_PROCESS_NOOP_0x100))
			SMC_RET1(handle, 0);
#if !PLAT_SMC_TRACE
		/* 跟踪打开时由调用跟踪记（按 fid 限次），不在这里每次都写，免得卷掉内存日志 */
		WARN("BL31: AML_DATA_PROCESS type 0x%lx refused\n", (unsigned long)x1);
#endif
		SMC_RET1(handle, SMC_UNK);
#endif

	default:
#if AML_HDCP
	{
		u_register_t ret;

		if (aml_hdcp_sip(smc_fid, x1, &ret))
			SMC_RET1(handle, ret);
	}
#endif
#if AML_SSTORE
	{
		u_register_t ret;

		if (aml_sstore_sip(smc_fid, x1, flags, &ret))
			SMC_RET1(handle, ret);
	}
#endif
#if !PLAT_SMC_TRACE
		ERROR("BL31: Unhandled SIP SMC: 0x%08x\n", smc_fid);
#endif
		break;
	}

	SMC_RET1(handle, SMC_UNK);
}

#if PLAT_SMC_TRACE
/* 调用跟踪（试验构建）：进出各记一次，返回值取交回调用者的 x0 */
static uintptr_t aml_sip_traced(uint32_t smc_fid,
				u_register_t x1, u_register_t x2,
				u_register_t x3, u_register_t x4,
				void *cookie, void *handle,
				u_register_t flags)
{
	plat_smc_trace(smc_fid, x1, x2, x3, handle, false, 0U);
	(void)aml_sip_handler(smc_fid, x1, x2, x3, x4, cookie, handle, flags);
	plat_smc_trace(smc_fid, x1, x2, x3, handle, true,
		       read_ctx_reg(get_gpregs_ctx(handle), CTX_GPREG_X0));
	return (uintptr_t)handle;
}
#endif

static int32_t aml_sip_setup(void)
{
#if AML_HDCP
	aml_hdcp_setup();
#endif
#if AML_SSTORE
	aml_sstore_setup();
#endif
	return 0;
}

DECLARE_RT_SVC(
	aml_sip_handler,

	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	aml_sip_setup,
#if PLAT_SMC_TRACE
	aml_sip_traced
#else
	aml_sip_handler
#endif
);
