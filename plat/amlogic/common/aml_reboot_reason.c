/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 重启原因 9（rpmbp）不写进 AO_RTI_STATUS_REG3[3:0]，改按 0 写（AML_REBOOT_REASON_NO_RPMBP，用户 2026-11-26 定，定型构建也带）：厂商 BL2 下一次从 eMMC 启动时见到原因 9、且读 RPMB 写计数器失败（密钥没编程），就把由 eFuse [0x40, 0x50) 与 eMMC CID 派生的认证密钥永久写进 RPMB（M12 厂商 BL2 0xfffa5b18–0xfffa5c88，M12 docs/16 §2.5 的“RPMB”）。原因 0 在厂商 BL33 里是 cold_boot：照常引导 Android。
 * 经过这里的有 PSCI SYSTEM_RESET 的 x1 与 SiP 0x82000049（两处都只写 [3:0]）。挡不住的：EFUSE_LIC0 bit6 = 1 时 BL2 不看原因、读计数器失败就写密钥；厂商 BL31 在役时（eMMC 链）不经过这里；非安全世界直接写 0xFF80000C（主线 mw.l、Android 的 aml_reg）能不能写进去未实测——同组的 AO_RTI_STATUS_REG2 实测非安全写无效（M12 docs/16 §3.5 的 E4），推断 REG3 一样。
 * 打完日志就刷控制台：SiP 0x82000049 之后可能紧跟看门狗复位，不刷的话 UART 里这一行会丢（SYSTEM_RESET 一路在写寄存器前另刷一次）。
 */

#include <common/debug.h>
#include <drivers/console.h>
#include <platform_def.h>
#include <stdint.h>

#include "aml_private.h"

/* 打开这项的构建的标记：build-bl31.sh 据此核对产物 */
#define NO_RPMBP_MARKER		"homeiot-no-rpmbp-reason"

uint32_t aml_reboot_reason_filter(uint32_t reason, const char *via)
{
	if (reason != AML_REBOOT_REASON_RPMBP)
		return reason;
	NOTICE("BL31: " NO_RPMBP_MARKER ": %s asked for reboot reason 9 (rpmbp), written as 0 so that BL2 does not program the RPMB key\n", via);
	console_flush();
	return AML_REBOOT_REASON_COLD;
}
