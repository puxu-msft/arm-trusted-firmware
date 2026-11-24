/*
 * Copyright (c) 2018-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

#include <platform_def.h>

#include "aml_private.h"

#ifdef AML_EFUSE_USER_BASE
/*
 * 用户区的起点与大小由平台定义（G12 家族见 g12a_def.h）：大小是 AML_EFUSE_USER_SPAN 减去 eFuse 字节 AML_EFUSE_TEE_AREA_SIZE 给出的 TEE OTP 区（该字节 × 16，不超过 SPAN）。读不出那个字节时按没有 TEE 区算，与厂商一样（它读进初值为 0 的变量）。
 */
uint64_t aml_efuse_user_max(void)
{
	uint8_t tee = 0U;
	uint32_t tee_size;

	(void)aml_scpi_efuse_read(&tee, AML_EFUSE_TEE_AREA_SIZE, 1U);
	tee_size = (uint32_t)tee * 16U;
	if (tee_size > AML_EFUSE_USER_SPAN)
		tee_size = AML_EFUSE_USER_SPAN;

	return AML_EFUSE_USER_SPAN - tee_size;
}

uint64_t aml_efuse_read(void *dst, uint32_t offset, uint32_t size)
{
	uint64_t max = aml_efuse_user_max();

	if ((size == 0U) || ((uint64_t)offset >= max) ||
	    ((uint64_t)size > max - (uint64_t)offset))
		return 0;

	return aml_scpi_efuse_read(dst, offset + AML_EFUSE_USER_BASE, size);
}

#ifdef AML_EFUSE_CALI_BASE
/*
 * eFuse 校准数据（SiP 0x8200003D，g12a_def.h）：只读整片 eFuse 字节 [AML_EFUSE_CALI_BASE, AML_EFUSE_CALI_BASE + AML_EFUSE_CALI_SPAN) 这一窗口。参数检查与厂商 0x511a50c 逐条相同，不合法回 0；长度 0 而偏移非 0 时厂商照样发 SCPI 读（回 SCP 给的字节数），这里同样发，按请求长度截断后回 0。
 */
uint64_t aml_efuse_read_cali(void *dst, uint32_t offset, uint32_t size)
{
	if ((offset > AML_EFUSE_CALI_OFFSET_MAX) || (size > AML_EFUSE_CALI_SPAN) ||
	    ((offset + size) == 0U) || ((offset + size) > AML_EFUSE_CALI_SPAN))
		return 0;

	return aml_scpi_efuse_read(dst, AML_EFUSE_CALI_BASE + offset, size);
}
#endif
#else
#define EFUSE_BASE	0x140
#define EFUSE_SIZE	0xC0

uint64_t aml_efuse_read(void *dst, uint32_t offset, uint32_t size)
{
	if ((uint64_t)(offset + size) > (uint64_t)EFUSE_SIZE)
		return 0;

	return aml_scpi_efuse_read(dst, offset + EFUSE_BASE, size);
}

uint64_t aml_efuse_user_max(void)
{
	return EFUSE_SIZE;
}
#endif
