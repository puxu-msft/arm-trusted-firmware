/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 音频许可查询（SiP 0x82000050，AML_AUDIO_LICENCE）接进 BL31：共享区是非安全输入共享区，许可位读 eFuse 控制器的 EFUSE_LIC1、EFUSE_LIC2（只读），RSA 工作区放在 .bss（每核的栈只有 4 KiB），调用在一把锁下串行（厂商不加锁，用的是栈）。
 * 四把私钥是厂商 BL31 里内置的钥材料，不进源码树：构建时由 build-bl31.sh 从钥匙真源生成 aml_audio_licence_keys.h 放在构建目录，经 make 变量 AML_AUDIO_LICENCE_KEYS_DIR 加进头文件路径；它定义 AML_AUDIO_LICENCE_KEYS_INIT（四个 struct aml_rsa_key 的初值，各分量是大端十六进制串）。
 */

#include <lib/cassert.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <platform_def.h>
#include <stdint.h>

#include "aml_audio_licence.h"
#include "aml_audio_licence_keys.h"
#include "aml_private.h"

/* 用到的共享区不越过输入共享区（后面紧挨着输出共享区） */
CASSERT(AML_SHARE_MEM_INPUT_BASE + AML_AUDIO_LICENCE_SHM_SPAN <= AML_SHARE_MEM_OUTPUT_BASE,
	assert_audio_licence_in_input_shm);

static const struct aml_rsa_key keys[AML_AUDIO_LICENCE_KEYS] = AML_AUDIO_LICENCE_KEYS_INIT;
static struct aml_rsa_work work;
static spinlock_t lock;

uint64_t aml_audio_licence_sip(void)
{
	uint64_t r;

	spin_lock(&lock);
	r = aml_audio_licence_query((uint8_t *)AML_SHARE_MEM_INPUT_BASE, keys,
				    mmio_read_32(AML_EFUSE_LIC_BASE + 4U), mmio_read_32(AML_EFUSE_LIC_BASE + 8U), &work);
	spin_unlock(&lock);
	return r;
}
