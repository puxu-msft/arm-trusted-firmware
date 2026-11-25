/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 试验构建的整片 eFuse 只读导出（AML_TRIAL_EFUSE_DUMP，SiP AML_SM_TRIAL_EFUSE_DUMP）：为把原厂 eFuse 作永久备份而设（M12 docs/19 §8、§9）。
 *
 * 读法与厂商 g12a BL31 的 0x5118db0 是同一条 SCPI 命令 0xC2（aml_scpi_efuse_read）。SCP 侧（M12 的 BL30）这条命令只读：读函数清零栈上 0x200 字节、查范围、按块号写 eFuse 控制器的命令寄存器（不带写入位 bit16，写入路径 0x10001244 才带）、等忙位清零、读出 16 字节，再拷出请求的那段。
 * 这里按 16 字节一块、每块读两遍：两遍都回满 16 字节且相同的块算读出；SCP 回的字节数不足（BL30 只在请求越界时回 0 字节）与两遍不同的块不算读出，数据位置填 0、在掩码里标出。BL30 等控制器忙位没有超时：控制器不回时 SCP 不应答，这里跟着在邮箱上等，试验构建由看门狗复位。
 * 另把 eFuse 控制器的许可寄存器 EFUSE_LIC0–3（AML_EFUSE_LIC_BASE 起 4 个字，厂商 BL2、BL30、BL31 读它们判安全启动、防回滚、音频许可）原样带上，判读时不必从 eFuse 字节偏移去猜这些位。
 *
 * 输出（dst 起 AML_EFUSE_DUMP_SIZE 字节，多字节字段小端）：
 *   [0x000, 0x200) 整片 eFuse，没读出的块是 0
 *   +0x200 "AMLEFUSE"（8 字节）
 *   +0x208 版本 1
 *   +0x20C 整片字节数 0x200
 *   +0x210 读出的块（bit i = 块 i，字节 [16i, 16i + 16)）
 *   +0x214 SCP 回的字节数不足的块
 *   +0x218 两遍都读满、内容不同的块
 *   +0x21C 这是本次冷启动以来第几次调用（从 1 起）：两次取回件来自两次调用，而不是同一次结果被读了两遍
 *   +0x220 EFUSE_LIC0–3
 *   +0x230 sha256([0x000, 0x230))：在 EL3 写完之后对 dst 算，主机取回后据此核对传输
 * 返回读出的字节数（读出的块数 × 16），masks[0..2] 依次是上面三个掩码。
 *
 * 这条调用把整片 eFuse（含安全存储的取钥材料 E，字节 [4, 16)）交给非安全世界，与安全存储“E 不出 EL3”相抵：只编进试验构建，且要另行打开（plat/amlogic/sm1/platform.mk）；试验构建只作 RAM 试验件，写进闪存的方案第 0 步 check-bootloader.py --for-emmc 拒收它（M12 docs/19 §7、§9）。
 */

#include <common/debug.h>
#include <lib/cassert.h>
#include <lib/mmio.h>
#include <platform_def.h>
#include <stdint.h>
#include <string.h>

#include "aml_private.h"
#include "aml_sstore.h"

#define DUMP_BLOCKS		(AML_EFUSE_SIZE / AML_EFUSE_BLOCK)
#define DUMP_MAGIC		"AMLEFUSE"
/* 打开这项的构建的标记：build-bl31.sh 据此核对产物 */
#define DUMP_MARKER		"homeiot-efuse-dump"
#define DUMP_VERSION		1U
#define DUMP_LIC_WORDS		4U
#define TRAILER_MAGIC		0x00U
#define TRAILER_VERSION		0x08U
#define TRAILER_SIZE		0x0CU
#define TRAILER_OK		0x10U
#define TRAILER_SHORT		0x14U
#define TRAILER_DIFFER		0x18U
#define TRAILER_SEQ		0x1CU
#define TRAILER_LIC		0x20U
#define TRAILER_SHA256		(TRAILER_LIC + 4U * DUMP_LIC_WORDS)

CASSERT((AML_EFUSE_SIZE % AML_EFUSE_BLOCK) == 0U, assert_efuse_whole_blocks);
CASSERT(DUMP_BLOCKS <= 32U, assert_efuse_blocks_fit_mask);
CASSERT(AML_EFUSE_DUMP_SIZE == AML_EFUSE_SIZE + TRAILER_SHA256 + 32U, assert_efuse_dump_trailer_fits);

static uint32_t dump_seq;

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint32_t aml_efuse_dump(uint8_t *dst, uint32_t masks[3])
{
	uint8_t a[AML_EFUSE_BLOCK], b[AML_EFUSE_BLOCK];
	uint8_t *t = dst + AML_EFUSE_SIZE;
	uint32_t ok = 0U, short_ = 0U, differ = 0U;
	uint32_t i, na, nb;

	for (i = 0U; i < DUMP_BLOCKS; i++) {
		uint8_t *d = dst + i * AML_EFUSE_BLOCK;

		na = aml_scpi_efuse_read(a, i * AML_EFUSE_BLOCK, AML_EFUSE_BLOCK);
		nb = aml_scpi_efuse_read(b, i * AML_EFUSE_BLOCK, AML_EFUSE_BLOCK);
		if ((na != AML_EFUSE_BLOCK) || (nb != AML_EFUSE_BLOCK)) {
			short_ |= 1U << i;
			memset(d, 0, AML_EFUSE_BLOCK);
		} else if (memcmp(a, b, AML_EFUSE_BLOCK) != 0) {
			differ |= 1U << i;
			memset(d, 0, AML_EFUSE_BLOCK);
		} else {
			ok |= 1U << i;
			memcpy(d, a, AML_EFUSE_BLOCK);
		}
	}
	aml_sstore_wipe(a, sizeof(a));
	aml_sstore_wipe(b, sizeof(b));

	/* 序号只在试验里用：几个核同时调时可能重号，主机侧按取回件的 sha256 另行分辨 */
	dump_seq++;
	memset(t, 0, AML_EFUSE_DUMP_SIZE - AML_EFUSE_SIZE);
	memcpy(t + TRAILER_MAGIC, DUMP_MAGIC, 8U);
	put_le32(t + TRAILER_VERSION, DUMP_VERSION);
	put_le32(t + TRAILER_SIZE, AML_EFUSE_SIZE);
	put_le32(t + TRAILER_OK, ok);
	put_le32(t + TRAILER_SHORT, short_);
	put_le32(t + TRAILER_DIFFER, differ);
	put_le32(t + TRAILER_SEQ, dump_seq);
	for (i = 0U; i < DUMP_LIC_WORDS; i++)
		put_le32(t + TRAILER_LIC + 4U * i, mmio_read_32(AML_EFUSE_LIC_BASE + 4U * i));
	aml_sstore_sha256(dst, AML_EFUSE_SIZE + TRAILER_SHA256, t + TRAILER_SHA256);

	masks[0] = ok;
	masks[1] = short_;
	masks[2] = differ;
	NOTICE("BL31: " DUMP_MARKER " #%u: blocks read 0x%08x, short 0x%08x, differ 0x%08x\n", dump_seq, ok, short_, differ);
	return (uint32_t)__builtin_popcount(ok) * AML_EFUSE_BLOCK;
}
