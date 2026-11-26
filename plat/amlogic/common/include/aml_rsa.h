/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RSA 私钥运算（CRT），语义照厂商 g12a BL31（v1.3:079690f10）0x511d524 起的私钥运算（返回码 22、6、1 与“72 个 64 位字加 used、sign”的大数结构同 LibTomCrypt 的 rsa_exptmod(PK_PRIVATE) 加 TomsFastMath，推断就是这两个库）：输入按大端读成整数 c，c 大于模数 n 时拒收（等于 n 照收），否则 m1 = c^dP mod p、m2 = c^dQ mod q、h = (m1 - m2)·qP mod p（有符号差、结果取非负），结果 m2 + h·q 按模数的字节数左补 0 写成大端。不加不去填充（裸 RSA）。
 * 只给音频许可查询（aml_audio_licence.c）用：钥的各分量是大端十六进制串（构建时由钥匙真源生成，不进源码树），模数最长 2048 位、p 与 q 各最长 1024 位。
 * 大数用 32 位一字、积与余数按 Knuth 算法 D（TAOCP 4.3.1）；工作区由调用者给（BL31 每核的栈只有 4 KiB），一个工作区同一时刻只给一个调用者用。
 * 耗时：qemu 逐条指令计数（独立评审 2026-11-27），BL31 里那份 -Os 目标码做一次 2048 位私钥运算约 85.7M 条指令、1024 位约 12.4M 条；一次 RSA2 查询（先解后加）约为厂商 0x511d0ec 的 3 倍，按 A55 1.9 GHz、IPC 0.7–1.2 估约 75–140 ms（厂商约 30–50 ms，未实测），这段时间这颗核在 EL3 里不响应中断。查询只在音频解码器初始化时发一次，内核的看门狗与软锁死门限是秒级；上机先量一次，嫌慢再把大数改成 64 位一字。
 * 只依赖 C 标准头：同一份源码编进 BL31，也在主机上编成测试程序，与 Python 的整数运算逐个比对（M12 设备目录 tools/tf-a/rsa-test.c）、随音频许可查询与厂商处理函数差分（tools/tf-a/sstore-harness.c 的 audio 子命令）。
 */

#ifndef AML_RSA_H
#define AML_RSA_H

#include <stddef.h>
#include <stdint.h>

#define AML_RSA_MAX_BYTES	256U	/* 模数最长 2048 位 */
#define AML_RSA_MAX_HALF_LIMBS	32U	/* p、q 最长 1024 位 */
/* 一个大数的字数：最长的是两个 1024 位数的积再加一个不超过 q 的数（m2 + h·q），64 字加进位一字；除法规范化时被除数另多一字（单列在工作区里） */
#define AML_BN_LIMBS		66U

/* LibTomCrypt 的错误码（厂商的返回值照传） */
#define AML_RSA_OK			0
#define AML_RSA_ERROR			1	/* CRYPT_ERROR：结果比模数长 */
#define AML_RSA_BUFFER_OVERFLOW		6	/* CRYPT_BUFFER_OVERFLOW：输出区比模数短 */
#define AML_RSA_INVALID_ARG		16	/* CRYPT_INVALID_ARG：本实现另用于钥不合格、输入超长 */
#define AML_RSA_PK_INVALID_SIZE		22	/* CRYPT_PK_INVALID_SIZE：输入大于模数 */

/* 大数：小端的 32 位字，n 是用到的字数（最高字非 0；数 0 时 n = 0） */
struct aml_bn {
	uint32_t n;
	uint32_t d[AML_BN_LIMBS];
};

/* 私钥（CRT 形式）：各分量是大端十六进制串（不带 0x，大小写都认） */
struct aml_rsa_key {
	const char *n, *p, *q, *dp, *dq, *qp;
};

/* 工作区：装进来的钥与运算中间量 */
struct aml_rsa_work {
	struct aml_bn n, p, q, dp, dq, qp;
	struct aml_bn t, m1, m2, x, base, prod;
	uint32_t un[AML_BN_LIMBS + 1U], vn[AML_BN_LIMBS];
};

/* 大数的基本运算（导出给主机侧测试；结果不得与 w 里的暂存区重叠） */
int aml_bn_from_hex(struct aml_bn *r, const char *s);
void aml_bn_from_be(struct aml_bn *r, const uint8_t *b, size_t len);
size_t aml_bn_bytes(const struct aml_bn *a);
void aml_bn_to_be(const struct aml_bn *a, uint8_t *b, size_t len);
int aml_bn_cmp(const struct aml_bn *a, const struct aml_bn *b);
void aml_bn_mod(struct aml_rsa_work *w, struct aml_bn *r, const struct aml_bn *u, const struct aml_bn *v);
void aml_bn_expmod(struct aml_rsa_work *w, struct aml_bn *r, const struct aml_bn *g, const struct aml_bn *e, const struct aml_bn *m);

/* 装钥（厂商 0x511db94 的取数部分）：回模数的字节数（fp_unsigned_bin_size），钥不合格（串不是十六进制、超长、p 或 q 为 0）回 0 */
size_t aml_rsa_load(struct aml_rsa_work *w, const struct aml_rsa_key *key);

/* 私钥运算（厂商 0x511d524 的 PK_PRIVATE 一支）：in 与 out 可以是同一块；*outlen 进来是输出区长度，成功时是写出的字节数 */
int aml_rsa_private(struct aml_rsa_work *w, const uint8_t *in, size_t inlen, uint8_t *out, size_t *outlen);

#endif /* AML_RSA_H */
