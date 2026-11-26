/*
 * Copyright (c) 2026, homeiot. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * 音频许可查询（SiP 0x82000050），协议照厂商 g12a BL31（v1.3:079690f10）的处理函数 0x511d0ec（平台操作表 +0xb8）。
 * 调用者：现役 Android 9 的内核 audio_data 驱动（audio_data_read → meson_efuse_fn_smc_query_audioinfo，fid 取自 DT 的 query_licence_cmd）把用户态写来的请求拷进非安全输入共享区再发这个调用，回 0 才把共享区拷回；用户态是 vendor 分区的 Dolby 与 DRA 软解库（libHwAudio_dcvdec.so、libdra.so 的 aml_check_audio_licence），回非 0 时它们当作没有许可。
 *
 * 共享区（小端）：+0 "AMQR"，+4 fid 0x82000050，+8 总长（16 位），+0xc "RSA1"（总长 0x90，钥 0 解请求、钥 1 加应答）、"RSA2"（0x110，钥 2、3）或 "RSA4"（0x210，钥 4、5：厂商只有 4 把钥，这一种也回 0x1dc）；+0x10 起是 RSA 块（长度等于模数的字节数），用私钥原地解密后：+0x13 项数（不超过 0x6f），+0x14、+0x18 各一个 "AMQR"，+0x1c 起 4 字节原样带回（用户态放的随机数），+0x20 起每项两字节：类型、结果。
 * 每项的结果按 eFuse 控制器的许可寄存器填：类型 1 是 EFUSE_LIC1 bit1、类型 2 是 LIC1 bit0、类型 3 是 EFUSE_LIC2 bit15、类型 4 恒为 1（厂商 0x511a1e8、0x511a200、0x511a220、0x511a218，分派表 0x51287f0）；类型不在 1–4 时这一项的结果写 2、回 0xa64。全部填完用另一把私钥原地加密回去，回 0。
 * 出错的返回值与留在共享区里的内容都照厂商（见 aml_audio_licence.c）：解密之后的错误不再加密，明文留在共享区里。厂商不写硬件、不碰输出共享区，这里也一样。
 *
 * 只依赖 C 标准头与 aml_rsa.h：许可寄存器的值与 RSA 工作区由调用者给，同一份源码在主机上与映射在原址的厂商处理函数差分（M12 设备目录 tools/tf-a/sstore-harness.c 的 audio 子命令）。
 */

#ifndef AML_AUDIO_LICENCE_H
#define AML_AUDIO_LICENCE_H

#include <stdint.h>

#include "aml_rsa.h"

#define AML_AUDIO_LICENCE_FID		0x82000050U
#define AML_AUDIO_LICENCE_KEYS		4U	/* 钥 0、1 给 RSA1，2、3 给 RSA2 */
#define AML_AUDIO_LICENCE_SHM_SPAN	0x110U	/* 用到的共享区：最长的 RSA2 请求（项最多到 +0xfd） */

/* shm 是非安全输入共享区，lic1、lic2 是 EFUSE_LIC1、EFUSE_LIC2 的值；回交给调用者的 x0 */
uint64_t aml_audio_licence_query(uint8_t *shm, const struct aml_rsa_key keys[AML_AUDIO_LICENCE_KEYS],
				 uint32_t lic1, uint32_t lic2, struct aml_rsa_work *w);

#endif /* AML_AUDIO_LICENCE_H */
