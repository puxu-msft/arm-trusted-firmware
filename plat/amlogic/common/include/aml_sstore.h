/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Amlogic 安全存储（unifykey）：与厂商 g12a BL31（v1.3:079690f10）兼容的块格式与存储类 SiP 调用（写出的块厂商读得懂、厂商写的块我们读得懂；同一张表、厂商工作区里没有旧字节时写出的块逐字节相同，差异见 aml_sstore.c 文件头）。
 *
 * 本头文件与 aml_sstore.c、aml_sstore_crypto.c 只依赖 C 标准头，同一份源码既编进 BL31（aml_sstore_svc.c 接 SiP），也在主机上编成测试程序：装载真机的块、与厂商 BL31 的代码逐调用对照（M12 设备目录 tools/tf-a/check-sstore.py、tools/tf-a/sstore-harness.c）。
 *
 * 块（AML_SSTORE_BUF_SIZE 字节，非安全共享缓冲 0x05080000，内核与引导程序把它与 eMMC reserved 分区的两份副本互相拷贝）：
 *   +0x000 "AMLSECURITY\0"，+0x010 版本，+0x014 加密类型，+0x018 sha256(块[0x200, 块大小))，其余到 0x200 为 0；
 *   +0x200 内层头 0x200 字节明文 {1,0x18}{2,4,total}{0xb,4,块大小}，一条 AES-256-CBC 链；
 *   +0x400 对象区 pad16(total) 字节，另一条从同一 IV 起的 AES-256-CBC 链；
 *   对象 {3,len}{4,4,namelen}{5,a4(namelen),name}{6,4,datalen}{7,a4(datalen),data}{0xa,0x20,sha256(data)}{8,4,类型字}{9,4,attr}。
 * 取钥：类型 2 用固定 key/IV；类型 1 用 eFuse[0x40,0x48) ‖ E ‖ 常量；其余用 E ‖ 常量；E = eFuse 字节 [4,16)。
 * 依据与各项的厂商地址见 M12 docs/16 §4、docs/19 §6。
 */

#ifndef AML_SSTORE_H
#define AML_SSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 输入、输出、块、消息区四个共享缓冲各自的大小；块大小（NOTIFY_EX 的 x1、内层头的块大小）不超过它 */
#define AML_SSTORE_BUF_SIZE		0x40000U
#define AML_SSTORE_KEYS			64U
#define AML_SSTORE_NAME_MAX		79U
#define AML_SSTORE_HEAP_SIZE		0x40000U
/* 装载时清零的消息区长度；+0 锁字、+4 写后置 1、+8 写计数 */
#define AML_SSTORE_MSG_CLEAR		0x1CU
#define AML_SSTORE_MSG_DIRTY		0x4U
#define AML_SSTORE_MSG_COUNT		0x8U
/* WRITE 写进对象的类型字 */
#define AML_SSTORE_OBJ_TYPE		0xA00000BFU
/* ASSIST_HW_REV 的低字节等于它（Amlogic 的 TXL 族号）时 GET_ENCTYPE 只看块头（厂商 0x5120ecc）；G12A/SM1 上推断不等 */
#define AML_SSTORE_FAMILY_ENC_FROM_BLOCK	0x23U

/* 存储类调用的返回值（厂商约定） */
#define AML_SSTORE_OK			0U
#define AML_SSTORE_ERR_NOKEY		1U
#define AML_SSTORE_ERR_PARAM		2U
#define AML_SSTORE_ERR_FULL		3U

struct aml_sstore_key {
	uint8_t name[80];
	uint32_t namelen;
	uint32_t attr;		/* bit0 安全（非安全世界不可读），bit8 加密 */
	uint32_t type;		/* 对象里的类型字 */
	uint32_t datalen;
	uint8_t *data;		/* 指向 heap */
	uint32_t datacap;	/* data 处可用的字节数（分配时的长度）；装载时 datalen 不得超过它 */
	uint8_t sha[32];
	uint32_t state;		/* bit0 有效；2 = 哈希不符或已删 */
};

/* 平台接口：在 BL31 里经 SCP 读 eFuse、读 ASSIST_HW_REV；主机测试换成桩 */
struct aml_sstore_ops {
	/* eFuse 字节 [off, off + len) 读进 buf，返回读到的字节数 */
	uint32_t (*efuse_read)(void *ctx, uint8_t *buf, uint32_t off, uint32_t len);
	/* ASSIST_HW_REV（0xFFD0814C）的低字节 */
	uint32_t (*chip_family)(void *ctx);
	void *ctx;
};

/* 装载（NOTIFY/NOTIFY_EX）的结局：只给 BL31 打日志用，SMC 一律回 0 */
enum aml_sstore_load_result {
	AML_SSTORE_LOADED,		/* 解开并建表（count 可以是 0） */
	AML_SSTORE_LOAD_TOO_LARGE,	/* x1 > AML_SSTORE_BUF_SIZE：不动任何状态 */
	AML_SSTORE_LOAD_SHORT,		/* x1 < 0x400，放不下两层头 */
	AML_SSTORE_LOAD_NO_MAGIC,
	AML_SSTORE_LOAD_BAD_SHA,
	AML_SSTORE_LOAD_KEY_FAIL,	/* 取钥失败（读 eFuse 失败） */
	AML_SSTORE_LOAD_BAD_HEADER,	/* 内层头解析失败、total 为 0，或块大小、total 越界 */
	AML_SSTORE_LOAD_SIZE_MISMATCH,	/* 内层头的块大小与 x1 不同 */
	AML_SSTORE_LOAD_BAD_OBJECTS,	/* 对象 TLV 解析失败或超过 64 个 */
};

/* 写块（WRITE/REMOVE 之后重新序列化）的结局 */
enum aml_sstore_seal_result {
	AML_SSTORE_SEAL_NONE,		/* 这次 WRITE/REMOVE 在写块之前就回了（参数错、查无此名、表满） */
	AML_SSTORE_SEALED,		/* 新块已写进块缓冲、消息区已置位 */
	AML_SSTORE_SEAL_TOO_LARGE,	/* 放不进块大小 */
	AML_SSTORE_SEAL_SELFCHECK,	/* 自检：按装载的解析规则读回明文，与对象表不符 */
	AML_SSTORE_SEAL_KEY_FAIL,
	AML_SSTORE_SEAL_HELD,		/* 不写回（write_back = false）：已在安全侧序列化并自检，块缓冲不动 */
};

struct aml_sstore {
	/* 非安全共享缓冲，各 AML_SSTORE_BUF_SIZE 字节 */
	uint8_t *in;
	uint8_t *out;
	uint8_t *block;
	uint8_t *msg;
	const struct aml_sstore_ops *ops;
	/* false：WRITE/REMOVE 只改对象表与安全侧的序列化结果，不碰块缓冲、回 1（内核因此不落盘），供 RAM 试验 */
	bool write_back;

	struct aml_sstore_key keys[AML_SSTORE_KEYS];
	uint32_t count;		/* 厂商的表项计数：新 key 放在 keys[count]；装载失败时不清 */
	uint32_t block_size;	/* GET_SHARE_STORAGE_BLOCK_SIZE；冷启动 0x40000 */
	int32_t enc_req;	/* SET_ENCTYPE 的请求值；冷启动 -1 */
	int32_t enc_loaded;	/* 装载（或上次写块）用的加密类型；冷启动 -1 */
	int32_t version;	/* VERSION；冷启动 0，装载先置 -1 */
	enum aml_sstore_load_result last_load;
	enum aml_sstore_seal_result last_seal;
	/* 上一次 WRITE/REMOVE 请求里的名字与数据长度（从共享缓冲取一次后的副本），只供日志 */
	uint8_t last_name[AML_SSTORE_NAME_MAX + 1U];
	uint32_t last_namelen;
	uint32_t last_datalen;
	uint8_t seal_sha[32];	/* 上次写出（或扣在安全侧）的新块的块头 SHA-256，供日志核对；不是密钥材料 */
	uint32_t heap_used;
	uint8_t heap[AML_SSTORE_HEAP_SIZE];
	/* 装载与写块的安全侧工作区：装载时是块的快照（就地解密），写块时是新块（就地加密），用完清零 */
	uint8_t work[AML_SSTORE_BUF_SIZE];
};

void aml_sstore_init(struct aml_sstore *s, uint8_t *in, uint8_t *out,
		     uint8_t *block, uint8_t *msg,
		     const struct aml_sstore_ops *ops, bool write_back);

/* NOTIFY_EX(x1 = size)；NOTIFY 等于 size = 0x40000 */
enum aml_sstore_load_result aml_sstore_load(struct aml_sstore *s, uint32_t size);

/* 存储类调用：请求在 in，应答在 out，返回值按厂商约定（0 成功，1、2、3 见上） */
uint32_t aml_sstore_query(struct aml_sstore *s);
uint32_t aml_sstore_read(struct aml_sstore *s, bool ns_caller);
uint32_t aml_sstore_write(struct aml_sstore *s);
uint32_t aml_sstore_tell(struct aml_sstore *s);
uint32_t aml_sstore_verify(struct aml_sstore *s);
uint32_t aml_sstore_status(struct aml_sstore *s);
uint32_t aml_sstore_list(struct aml_sstore *s);
uint32_t aml_sstore_remove(struct aml_sstore *s);
void aml_sstore_set_enctype(struct aml_sstore *s, uint32_t type);
/* 下面两个按厂商把 32 位有符号值符号扩展成 64 位返回 */
int64_t aml_sstore_get_enctype(struct aml_sstore *s);
int64_t aml_sstore_version(const struct aml_sstore *s);
/*
 * BL31 内部读一个 key（厂商 0x5120bd0 以标志 0 调用时的语义，不看 attr bit0；HDCP 族用）：表里有这个名字（NUL 结尾）且有值时，把值的前 min(长度, len) 字节拷进 buf（buf 为 NULL 时不拷）、回这个字节数，否则回 0；
 * 找得到名字时另把 attr & 0x101 写进 *attr（厂商 0x5120d54；attr 为 NULL 时不写）。
 */
uint32_t aml_sstore_key_copy(const struct aml_sstore *s, const char *name, uint8_t *buf, uint32_t len, uint32_t *attr);

/* 加密与摘要（aml_sstore_crypto.c） */
struct aml_sstore_aes {
	uint32_t rk[60];	/* AES-256 的 15 轮轮密钥 */
};

struct aml_sstore_sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	uint32_t fill;
};

void aml_sstore_aes256_init(struct aml_sstore_aes *ctx, const uint8_t key[32]);
void aml_sstore_aes_encrypt(const struct aml_sstore_aes *ctx, const uint8_t in[16], uint8_t out[16]);
void aml_sstore_aes_decrypt(const struct aml_sstore_aes *ctx, const uint8_t in[16], uint8_t out[16]);
/* CBC，len 是 16 的倍数，就地处理 */
void aml_sstore_cbc_encrypt(const struct aml_sstore_aes *ctx, const uint8_t iv[16], uint8_t *buf, size_t len);
void aml_sstore_cbc_decrypt(const struct aml_sstore_aes *ctx, const uint8_t iv[16], uint8_t *buf, size_t len);

void aml_sstore_sha256_init(struct aml_sstore_sha256 *ctx);
void aml_sstore_sha256_update(struct aml_sstore_sha256 *ctx, const uint8_t *p, size_t n);
void aml_sstore_sha256_final(struct aml_sstore_sha256 *ctx, uint8_t out[32]);
void aml_sstore_sha256(const uint8_t *p, size_t n, uint8_t out[32]);

/* 清零密钥材料与明文，不被编译器优化掉 */
void aml_sstore_wipe(void *p, size_t n);

/*
 * E（eFuse 字节 [4,16)）与芯片号 v2 是否同源：chip 是 SCP 0xC6 的 16 字节应答（GET_CHIP_ID 0x82000044 原样交给非安全世界的那 16 字节，前 4 字节是 cpu_id），
 * E 等于它 [4,16) 的逆序，即主线 U-Boot 的 `sm serial` 交出的 12 字节（meson_sm_get_serial 把 serial 反转成大端，arch/arm/mach-meson/sm.c）。只返回是否相同，不交出任何字节。
 */
bool aml_sstore_e_matches_chip_id(const uint8_t e[12], const uint8_t chip[16]);

#endif /* AML_SSTORE_H */
