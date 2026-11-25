/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * HDMITX 的 HDCP 操作类 SiP 调用：fid、寄存器读写序列与返回约定照厂商 g12a BL31（v1.3:079690f10，平台操作表 +0x120–+0x158、+0x180、+0x1a0–+0x1b0）。
 * HDCP 的控制寄存器（A_HDCPCFG0/1、HDCP22REG_CTRL、HDCPREG_SEED/DPK、TOP 的 PKF/DUK 与 HDCP22_BSOD）在 HDMITX 的安全区，非安全世界写不进，由 BL31 按这些调用代写；内核 hdmitx 驱动的 hdmitx_hdcp_opr 与厂商 BL33 的 hdmitx 初始化发这些调用。
 *
 * 本头文件与 aml_hdcp.c 只依赖 C 标准头与 aml_sstore.h 的 AES（hdcp key 加密存放时解密用），寄存器、定时器与安全存储经 struct aml_hdcp_ops 注入：同一份源码既编进 BL31（aml_hdcp_svc.c），也在主机上编成测试程序，与映射在原址的厂商处理函数逐 fid 比对寄存器读写序列（M12 设备目录 tools/tf-a/sstore-harness.c 的 hdcp 子命令）。
 */

#ifndef AML_HDCP_H
#define AML_HDCP_H

#include <stdbool.h>
#include <stdint.h>

/* fid；括号里是内核 hdmitx_hdcp_opr 的操作号 */
#define AML_SM_HDCP14_ENABLE		0x82000010U	/* 1 */
#define AML_SM_HDCP14_RESULT		0x82000011U	/* 2 */
#define AML_SM_HDCP14_INIT		0x82000012U	/* 0 */
#define AML_SM_HDCP14_EN_ENCRYPT	0x82000013U	/* 3 */
#define AML_SM_HDCP14_OFF		0x82000014U	/* 4 */
#define AML_SM_HDCP_MUX_22		0x82000015U	/* 5 */
#define AML_SM_HDCP_MUX_14		0x82000016U	/* 6 */
#define AML_SM_HDCP22_RESULT		0x82000017U	/* 7 */
#define AML_SM_HDCP22_SET_DUK		0x8200001CU	/* 0xc */
#define AML_SM_HDCP22_SET_TOPO		0x82000083U	/* 0xd */
#define AML_SM_HDCP22_GET_TOPO		0x82000084U	/* 0xe */
#define AML_SM_HDCP22_SUSPEND		0x8200008AU

/* 安全存储里的 key 名与读取长度（厂商 0x511e0c0、0x511eaf4） */
#define AML_HDCP14_KEY_NAME		"hdcp"
#define AML_HDCP14_KEY_LEN		0x120U
#define AML_HDCP22_KEY_NAME		"hdcp22_fw_private"
#define AML_HDCP22_KEY_LEN		0x20U

struct aml_hdcp_ops {
	/* HDMITX 安全寄存器，地址按内核 hdmitx 的编码（aml_hdmitx.c） */
	uint32_t (*read)(void *ctx, uint32_t reg);
	void (*write)(void *ctx, uint32_t reg, uint32_t val);
	/* 厂商 0x511ece0：每个单位等 ISA_TIMERE（1 µs 一个计数）走过 2 个计数以上，约 1–2 µs；0 个单位不等 */
	void (*delay)(void *ctx, uint32_t units);
	/* ISA_TIMERE（1 µs）的低 32 位 */
	uint32_t (*now)(void *ctx);
	/* 读安全存储里的 key，语义同 aml_sstore_key_copy：拷 min(长度, len) 字节、回拷了多少，没有这个 key 回 0；找得到时写 *attr（attr & 0x101） */
	uint32_t (*key_read)(void *ctx, const char *name, uint8_t *buf, uint32_t len, uint32_t *attr);
	void *ctx;
};

/* 跨调用的状态：HDCP 2.2 结果的去抖（0x82000017、0x8200008A）与拓扑标志（0x82000083/84），厂商放在 0x512d298 与 0x513b698–0x513b6d8 */
struct aml_hdcp {
	const struct aml_hdcp_ops *ops;
	bool started;		/* 0x512d298 的反面：第一次 RESULT22 时起算两个时刻 */
	bool suspended;		/* 0x513b698：SUSPEND(非 0) 之后 RESULT22 回 frozen */
	bool grace;		/* 0x513b6d4：SUSPEND(0) 之后 2 s 的宽限期 */
	uint32_t grace_end;	/* 0x513b6cc */
	uint32_t hold_end;	/* 0x513b6d0：认证成功后结果保持 2 s */
	uint32_t held;		/* 0x513b6c8 */
	uint32_t frozen;	/* 0x513b6d8 */
	uint32_t topo;		/* 0x513b6c0 */
};

void aml_hdcp_init(struct aml_hdcp *h, const struct aml_hdcp_ops *ops);
/* 认得 fid 时回 true，交回调用者的 x0 写进 *ret；x1 是调用者的 x1 */
bool aml_hdcp_smc(struct aml_hdcp *h, uint32_t fid, uint64_t x1, uint64_t *ret);

#endif /* AML_HDCP_H */
