/*
 * Copyright (c) 2018-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/mmio.h>
#include <platform_def.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aml_private.h"

#ifndef AML_SSTORE
#define AML_SSTORE	0
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
 * HDMITX 安全寄存器的地址编码，与厂商 g12a BL31 相同（0x511ed20–0x511eda0）：bit28 置位时低 24 位是 0xFF600000 起的字节偏移（≤ 0x7E31，按字节访问）；bit28 清零时低 24 位 ≤ 0x365E，落在 [0x2000, 0x365E] 的按字节访问 0xFF608000 + 偏移，其余按 32 位访问 0xFF608000 + 偏移 × 4。其余位不看。越界返回 false。
 */
static bool aml_hdmitx_sec_addr(uint32_t reg, uintptr_t *addr, bool *byte)
{
	uint32_t off = reg & AML_HDMITX_SEC_OFF_MASK;

	if ((reg & AML_HDMITX_SEC_DWC_SEL) != 0U) {
		if (off > AML_HDMITX_DWC_MAX)
			return false;
		*addr = AML_HDMITX_DWC_BASE + off;
		*byte = true;
		return true;
	}
	if (off > AML_HDMITX_TOP_MAX)
		return false;
	if (off >= AML_HDMITX_TOP_BYTE_START) {
		*addr = AML_HDMITX_TOP_BASE + off;
		*byte = true;
	} else {
		*addr = AML_HDMITX_TOP_BASE + ((uintptr_t)off << 2);
		*byte = false;
	}
	return true;
}

static uint32_t aml_hdmitx_sec_read(uint32_t reg)
{
	uintptr_t addr;
	bool byte;

	if (!aml_hdmitx_sec_addr(reg, &addr, &byte))
		return 0xFFFFFFFFU;
	return byte ? (uint32_t)mmio_read_8(addr) : mmio_read_32(addr);
}

static void aml_hdmitx_sec_write(uint32_t reg, uint32_t val)
{
	uintptr_t addr;
	bool byte;

	if (!aml_hdmitx_sec_addr(reg, &addr, &byte))
		return;
	if (byte)
		mmio_write_8(addr, (uint8_t)val);
	else
		mmio_write_32(addr, val);
}

/* AO_RTI_STATUS_REG3 的一个字段换成 val（读改写） */
static void aml_ao_reg3_set(uint32_t mask, uint32_t val)
{
	mmio_clrsetbits_32(AML_AO_RTI_STATUS_REG3, mask, val & mask);
}

/* DMC 安全寄存器的读写代理（g12a_def.h 的 0x8200004A）：只代理 AML_DMC_SEC_BASE 那一页，页外与 rw 不是 0、1 时回 0 */
static u_register_t aml_dmc_rw(u_register_t addr, u_register_t val, u_register_t rw)
{
	if ((addr < AML_DMC_SEC_BASE) || (addr >= AML_DMC_SEC_BASE + AML_DMC_SEC_SIZE) ||
	    ((addr & 3U) != 0U) || (rw > 1U))
		return 0;
	if (rw == 0U)
		return mmio_read_32(addr);
	mmio_write_32(addr, (uint32_t)val);
	return 0;
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
		SMC_RET1(handle, aml_dmc_rw(x1, x2, x3));

	case AML_SM_SET_REBOOT_REASON:
		/* 厂商把 x1 整个或进 REG3（0x5119ea4）；这里只取 [3:0]，越界的高位不写进别的字段 */
		aml_ao_reg3_set(AML_REBOOT_REASON_MASK, (uint32_t)x1);
		SMC_RET1(handle, 0);

	case AML_SM_DATA_PROCESS:
		if ((x1 == AML_DATA_PROCESS_BOOT_CHECK) ||
		    (x1 == AML_DATA_PROCESS_NOOP_0x100))
			SMC_RET1(handle, 0);
		WARN("BL31: AML_DATA_PROCESS type 0x%lx refused\n", (unsigned long)x1);
		SMC_RET1(handle, SMC_UNK);
#endif

	default:
#if AML_SSTORE
	{
		u_register_t ret;

		if (aml_sstore_sip(smc_fid, x1, flags, &ret))
			SMC_RET1(handle, ret);
	}
#endif
		ERROR("BL31: Unhandled SIP SMC: 0x%08x\n", smc_fid);
		break;
	}

	SMC_RET1(handle, SMC_UNK);
}

static int32_t aml_sip_setup(void)
{
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
	aml_sip_handler
);
