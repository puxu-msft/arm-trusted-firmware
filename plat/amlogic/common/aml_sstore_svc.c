/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 安全存储（unifykey）的 SiP 调用：fid、共享缓冲地址与返回约定照厂商 g12a BL31（M12 docs/16 §4.1–§4.3），表与块的处理在 aml_sstore.c。
 * - 只服务非安全调用者（本平台没有 BL32）：安全调用者的取址与存储类调用回 0xFFFFFFFF（厂商的未知调用返回值）。
 * - 存储类调用、块大小与 SET_STORAGE_INFO 在同一把锁下串行（取址只回常量，不加锁）；锁在安全内存里（厂商把锁放在非安全的消息区 +0，非安全世界能占住它）。
 * - 密钥材料：E（eFuse 字节 [4,16)）经 SCP 读一次后存在安全内存里，之后不再读；它与 eFuse[0x40,0x48)、AES 轮密钥都不出 EL3，日志里只记名字、长度、结局与新块块头的 SHA-256 前 8 字节，不记 key 的值。
 * - AML_SSTORE_WRITE_BACK=0（RAM 试验用）：WRITE/REMOVE 只改表、在安全侧写块并自检，块缓冲不动、回 1，内核因此不把块写回 eMMC。
 * - AML_SSTORE_SKIP_UNCHANGED=1：值与属性都没变的 WRITE 什么都不改、回 AML_SSTORE_UNCHANGED（4），厂商内核因此不落盘（M12 docs/19 §6.4 末）。
 */

#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/cassert.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <lib/utils_def.h>
#include <platform_def.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aml_hdcp.h"
#include "aml_private.h"
#include "aml_sstore.h"

#ifndef AML_SSTORE_WRITE_BACK
#define AML_SSTORE_WRITE_BACK	1
#endif

#ifndef AML_SSTORE_SKIP_UNCHANGED
#define AML_SSTORE_SKIP_UNCHANGED	0
#endif

#ifndef AML_TRIAL
#define AML_TRIAL		0
#endif

/* 四个缓冲各 0x40000、首尾相接；消息区实际只用前 0x1C 字节，其后的 0x050FE000/0x050FF000 是通用 SiP 输入输出（厂商同样如此） */
CASSERT((AML_SSTORE_OUT_BASE == AML_SSTORE_IN_BASE + AML_SSTORE_BUF_SIZE) &&
	(AML_SSTORE_BLOCK_BASE == AML_SSTORE_OUT_BASE + AML_SSTORE_BUF_SIZE) &&
	(AML_SSTORE_MSG_BASE == AML_SSTORE_BLOCK_BASE + AML_SSTORE_BUF_SIZE) &&
	(AML_SSTORE_IN_BASE >= AML_NS_SHARE_MEM_BASE) &&
	(AML_SSTORE_MSG_BASE + AML_SSTORE_MSG_CLEAR <= AML_SHARE_MEM_INPUT_BASE),
	assert_aml_sstore_buffers_in_ns_share_mem);
#if AML_TRIAL
/* 试验构建的内存日志落在消息区的空闲部分，不能压住消息区实际用到的字节 */
CASSERT(AML_TRIAL_MEMLOG_BASE >= AML_SSTORE_MSG_BASE + AML_SSTORE_MSG_CLEAR,
	assert_aml_trial_memlog_above_sstore_msg);
#endif

/* 厂商 SiP 对未知调用与非法调用者回 x0 = 0x00000000FFFFFFFF（不是 SMCCC 的 64 位 -1） */
#define AML_SSTORE_UNK		U(0xFFFFFFFF)

static struct aml_sstore sstore;
static spinlock_t sstore_lock;
static uint8_t sstore_e[12];
static bool sstore_e_cached;

static uint32_t sstore_efuse_read(void *ctx, uint8_t *buf, uint32_t off, uint32_t len)
{
	uint32_t n;

	(void)ctx;
	if ((off == 4U) && (len == sizeof(sstore_e)) && sstore_e_cached) {
		memcpy(buf, sstore_e, sizeof(sstore_e));
		return len;
	}
	n = aml_scpi_efuse_read(buf, off, len);
	if ((off == 4U) && (len == sizeof(sstore_e)) && (n == len)) {
		memcpy(sstore_e, buf, sizeof(sstore_e));
		sstore_e_cached = true;
#if AML_TRIAL
		{
			/* E 应等于芯片号（SCP 0xC6）[4,16) 的逆序，即主线 `sm serial` 交出的 12 字节（M12 docs/14 §9.4、docs/19 §6.5）；只记一致与否 */
			uint8_t chip[16];

			if (aml_scpi_get_chip_id(chip, sizeof(chip)) != sizeof(chip))
				NOTICE("sstore: chip id unavailable for the E cross-check\n");
			else
				NOTICE("sstore: E %s the chip id serial (reversed, as sm serial)\n",
				       aml_sstore_e_matches_chip_id(sstore_e, chip) ? "matches" : "DIFFERS from");
			aml_sstore_wipe(chip, sizeof(chip));
		}
#endif
	}
	return n;
}

static uint32_t sstore_chip_family(void *ctx)
{
	(void)ctx;
	return mmio_read_32(AML_ASSIST_HW_REV) & 0xFFU;
}

static const struct aml_sstore_ops sstore_ops = {
	.efuse_read = sstore_efuse_read,
	.chip_family = sstore_chip_family,
	.ctx = NULL,
};

#if !AML_SSTORE_WRITE_BACK
/* 不写回构建（只给 RAM 试验）的标记：build-bl31.sh 据此核对产物 */
static const char aml_sstore_hold_marker[] = "homeiot-sstore-hold";
#endif
#if AML_SSTORE_SKIP_UNCHANGED
/* 不写没变的值的构建的标记：build-bl31.sh 据此核对产物 */
static const char aml_sstore_skip_marker[] = "homeiot-sstore-skip-unchanged";
#endif

void aml_sstore_setup(void)
{
	aml_sstore_init(&sstore, (uint8_t *)AML_SSTORE_IN_BASE, (uint8_t *)AML_SSTORE_OUT_BASE,
			(uint8_t *)AML_SSTORE_BLOCK_BASE, (uint8_t *)AML_SSTORE_MSG_BASE,
			&sstore_ops, AML_SSTORE_WRITE_BACK != 0);
	sstore.skip_unchanged = (AML_SSTORE_SKIP_UNCHANGED != 0);
#if AML_SSTORE_SKIP_UNCHANGED
	NOTICE("sstore: unchanged writes are not sealed (%s)\n", aml_sstore_skip_marker);
#endif
#if !AML_SSTORE_WRITE_BACK
	NOTICE("sstore: write-back off, writes stay in secure memory (%s)\n", aml_sstore_hold_marker);
#endif
}

static const char *load_result_name(enum aml_sstore_load_result r)
{
	switch (r) {
	case AML_SSTORE_LOADED:			return "loaded";
	case AML_SSTORE_LOAD_TOO_LARGE:		return "flash size is too large";
	case AML_SSTORE_LOAD_SHORT:		return "block size below 0x400";
	case AML_SSTORE_LOAD_NO_MAGIC:		return "no AMLSECURITY magic";
	case AML_SSTORE_LOAD_BAD_SHA:		return "storage data is corrupt (SHA-256)";
	case AML_SSTORE_LOAD_KEY_FAIL:		return "key derivation failed (eFuse read)";
	case AML_SSTORE_LOAD_BAD_HEADER:	return "inner header rejected";
	case AML_SSTORE_LOAD_SIZE_MISMATCH:	return "block size differs from the inner header";
	case AML_SSTORE_LOAD_BAD_OBJECTS:	return "object records rejected";
	default:				return "?";
	}
}

static const char *seal_result_name(enum aml_sstore_seal_result r)
{
	switch (r) {
	case AML_SSTORE_SEALED:			return "sealed";
	case AML_SSTORE_SEAL_TOO_LARGE:		return "storage size is larger than flash";
	case AML_SSTORE_SEAL_SELFCHECK:		return "self-check of the new block failed";
	case AML_SSTORE_SEAL_KEY_FAIL:		return "key derivation failed (eFuse read)";
	case AML_SSTORE_SEAL_HELD:		return "held in secure memory (write-back off)";
	case AML_SSTORE_SEAL_SKIPPED:		return "unchanged, not sealed";
	default:				return "?";
	}
}

/* WRITE/REMOVE 的日志：名字与长度取核心存下的副本（不再读共享缓冲），可打印 ASCII 原样记，否则只记 <binary> */
static void log_change(bool is_write, uint32_t ret)
{
	const char *what = is_write ? "write" : "remove";
	char name[AML_SSTORE_NAME_MAX + 1U];
	uint32_t namelen = sstore.last_namelen;
	uint32_t i;
	bool printable = true;

	if (namelen > AML_SSTORE_NAME_MAX) {
		namelen = 0U;
		printable = false;
	}
	for (i = 0U; i < namelen; i++) {
		name[i] = (char)sstore.last_name[i];
		if ((sstore.last_name[i] < 0x20U) || (sstore.last_name[i] > 0x7eU))
			printable = false;
	}
	name[namelen] = '\0';
	if ((sstore.last_seal == AML_SSTORE_SEALED) || (sstore.last_seal == AML_SSTORE_SEAL_HELD)) {
		const uint8_t *h = sstore.seal_sha;

		NOTICE("sstore: %s '%s' (%u bytes) -> %u, %s, block sha256 %02x%02x%02x%02x%02x%02x%02x%02x\n",
		       what, printable ? name : "<binary>", sstore.last_datalen, ret,
		       seal_result_name(sstore.last_seal), h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
	} else if (sstore.last_seal == AML_SSTORE_SEAL_SKIPPED) {
		NOTICE("sstore: %s '%s' (%u bytes) -> %u, %s\n", what, printable ? name : "<binary>",
		       sstore.last_datalen, ret, seal_result_name(sstore.last_seal));
	} else if (sstore.last_seal != AML_SSTORE_SEAL_NONE) {
		ERROR("sstore: %s '%s' -> %u, %s\n", what, printable ? name : "<binary>", ret,
		      seal_result_name(sstore.last_seal));
	} else {
		/* 写块之前就回了：2 参数错或查无此名（REMOVE）、3 表满或堆不够 */
		NOTICE("sstore: %s '%s' (namelen %u, %u bytes) -> %u, refused before sealing\n", what,
		       printable ? name : "<binary>", sstore.last_namelen, sstore.last_datalen, ret);
	}
}

uint32_t aml_sstore_key_get(const char *name, uint8_t *buf, uint32_t len, uint32_t *attr)
{
	uint32_t n;

	spin_lock(&sstore_lock);
	n = aml_sstore_key_copy(&sstore, name, buf, len, attr);
	spin_unlock(&sstore_lock);
	return n;
}

/* 处理了返回 true，结果在 *ret；不是安全存储的 fid 返回 false */
bool aml_sstore_sip(uint32_t fid, u_register_t x1, u_register_t flags, u_register_t *ret)
{
	bool ns = is_caller_non_secure(flags);

	switch (fid) {
	case AML_SM_STORAGE_IN_BASE:
		*ret = ns ? AML_SSTORE_IN_BASE : AML_SSTORE_UNK;
		return true;
	case AML_SM_STORAGE_OUT_BASE:
		*ret = ns ? AML_SSTORE_OUT_BASE : AML_SSTORE_UNK;
		return true;
	case AML_SM_STORAGE_BLOCK_BASE:
		*ret = AML_SSTORE_BLOCK_BASE;
		return true;
	case AML_SM_STORAGE_MSG_BASE:
		*ret = AML_SSTORE_MSG_BASE;
		return true;
	case AML_SM_STORAGE_BLOCK_SIZE:
	case AML_SM_SET_STORAGE_INFO:
		/* 安全调用者同样可用（厂商如此），在锁下做 */
		spin_lock(&sstore_lock);
		if (fid == AML_SM_STORAGE_BLOCK_SIZE) {
			*ret = sstore.block_size;
		} else {
			/* 厂商 0x5119f44：AO_SEC_SD_CFG18（0xFF800248）[30:28] = x1 & 7 */
			mmio_clrsetbits_32(AML_SSTORE_INFO_REG, U(0x7) << 28, ((uint32_t)x1 & U(0x7)) << 28);
			*ret = 0U;
		}
		spin_unlock(&sstore_lock);
		return true;
	case AML_SM_HDCP14_KEY_LSTORE:
	case AML_SM_HDCP22_KEY_LSTORE:
		/* 厂商对安全与非安全调用者一样回（g12a_def.h） */
		spin_lock(&sstore_lock);
		*ret = (fid == AML_SM_HDCP14_KEY_LSTORE) ?
			(aml_sstore_key_copy(&sstore, AML_HDCP14_KEY_NAME, NULL, AML_HDCP14_KEY_LEN, NULL) == AML_HDCP14_KEY_LEN) :
			(aml_sstore_key_copy(&sstore, AML_HDCP22_KEY_NAME, NULL, AML_HDCP22_KEY_LEN, NULL) == AML_HDCP22_KEY_LEN);
		spin_unlock(&sstore_lock);
		return true;
	case AML_SM_STORAGE_QUERY:
	case AML_SM_STORAGE_READ:
	case AML_SM_STORAGE_WRITE:
	case AML_SM_STORAGE_TELL:
	case AML_SM_STORAGE_VERIFY:
	case AML_SM_STORAGE_STATUS:
	case AML_SM_STORAGE_NOTIFY:
	case AML_SM_STORAGE_LIST:
	case AML_SM_STORAGE_REMOVE:
	case AML_SM_STORAGE_NOTIFY_EX:
	case AML_SM_STORAGE_SET_ENCTYPE:
	case AML_SM_STORAGE_GET_ENCTYPE:
	case AML_SM_STORAGE_VERSION:
		break;
	default:
		return false;
	}
	if (!ns) {
		*ret = AML_SSTORE_UNK;
		return true;
	}

	spin_lock(&sstore_lock);
	switch (fid) {
	case AML_SM_STORAGE_QUERY:
		*ret = aml_sstore_query(&sstore);
		break;
	case AML_SM_STORAGE_READ:
		*ret = aml_sstore_read(&sstore, ns);
		break;
	case AML_SM_STORAGE_WRITE:
		*ret = aml_sstore_write(&sstore);
		log_change(true, (uint32_t)*ret);
		break;
	case AML_SM_STORAGE_TELL:
		*ret = aml_sstore_tell(&sstore);
		break;
	case AML_SM_STORAGE_VERIFY:
		*ret = aml_sstore_verify(&sstore);
		break;
	case AML_SM_STORAGE_STATUS:
		*ret = aml_sstore_status(&sstore);
		break;
	case AML_SM_STORAGE_NOTIFY:
	case AML_SM_STORAGE_NOTIFY_EX:
	{
		uint32_t size = (fid == AML_SM_STORAGE_NOTIFY) ? AML_SSTORE_BUF_SIZE : (uint32_t)x1;
		enum aml_sstore_load_result r = aml_sstore_load(&sstore, size);

		if (r == AML_SSTORE_LOADED)
			NOTICE("sstore: %u keys loaded (block 0x%x, version %d, type %d)\n",
			       sstore.count, sstore.block_size, (int)sstore.version, (int)sstore.enc_loaded);
		else
			WARN("sstore: load of 0x%x bytes: %s, no keys\n", size, load_result_name(r));
		*ret = 0U;
		break;
	}
	case AML_SM_STORAGE_LIST:
		*ret = aml_sstore_list(&sstore);
		break;
	case AML_SM_STORAGE_REMOVE:
		*ret = aml_sstore_remove(&sstore);
		log_change(false, (uint32_t)*ret);
		break;
	case AML_SM_STORAGE_SET_ENCTYPE:
		aml_sstore_set_enctype(&sstore, (uint32_t)x1);
		*ret = 0U;
		break;
	case AML_SM_STORAGE_GET_ENCTYPE:
		*ret = (u_register_t)aml_sstore_get_enctype(&sstore);
		break;
	case AML_SM_STORAGE_VERSION:
		*ret = (u_register_t)aml_sstore_version(&sstore);
		break;
	default:
		break;
	}
	spin_unlock(&sstore_lock);
	return true;
}
