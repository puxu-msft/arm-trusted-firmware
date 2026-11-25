/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * HDMITX 安全寄存器的读写（SiP 0x82000018/19 与 HDCP 族共用）。地址按内核 hdmitx 驱动的编码（hdmi_tx_reg.h：bit28 选 DWC，bit24 标“安全”），译法与厂商 g12a BL31 的 0x511ed20、0x511eda4 相同：
 * - bit28 置位：低 24 位是 0xFF600000 起的字节偏移（≤ 0x7E31），按字节访问；
 * - bit28 清零：低 24 位 ≤ 0x365E，落在 [0x2000, 0x365E] 的按字节访问 0xFF608000 + 偏移，其余按 32 位访问 0xFF608000 + 偏移 × 4（例如 TOP 0x2A，HDMITX_TOP_HDCP22_BSOD，在 0xFF6080A8）；
 * - 其余位（含 bit24）不看；越界时读回 0xFFFFFFFF、写忽略。
 * 单独成文件：主机侧的差分（M12 tools/tf-a/sstore-harness.c 的 hdcp 子命令）把它与厂商的两个访问器在同一组地址上比对。
 */

#include <lib/mmio.h>
#include <platform_def.h>
#include <stdbool.h>
#include <stdint.h>

#include "aml_private.h"

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

uint32_t aml_hdmitx_sec_read(uint32_t reg)
{
	uintptr_t addr;
	bool byte;

	if (!aml_hdmitx_sec_addr(reg, &addr, &byte))
		return 0xFFFFFFFFU;
	return byte ? (uint32_t)mmio_read_8(addr) : mmio_read_32(addr);
}

void aml_hdmitx_sec_write(uint32_t reg, uint32_t val)
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
