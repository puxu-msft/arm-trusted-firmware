/*
 * Copyright (c) 2018-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AML_PRIVATE_H
#define AML_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Utility functions */
unsigned int plat_calc_core_pos(u_register_t mpidr);
void aml_console_init(void);
void aml_setup_page_tables(void);
void aml_reset_pen(void);

/* MHU functions */
void aml_mhu_secure_message_start(void);
void aml_mhu_secure_message_send(uint32_t msg);
uint32_t aml_mhu_secure_message_wait(void);
void aml_mhu_secure_message_end(void);
void aml_mhu_secure_init(void);

/* SCPI functions */
void aml_scpi_set_css_power_state(u_register_t mpidr, uint32_t cpu_state,
				  uint32_t cluster_state, uint32_t css_state);
uint32_t aml_scpi_sys_power_state(uint64_t system_state);
void aml_scpi_jtag_set_state(uint32_t state, uint8_t select);
uint32_t aml_scpi_efuse_read(void *dst, uint32_t base, uint32_t size);
void aml_scpi_unknown_thermal(uint32_t arg0, uint32_t arg1,
			      uint32_t arg2, uint32_t arg3);
void aml_scpi_upload_scp_fw(uintptr_t addr, size_t size, int send);
uint32_t aml_scpi_get_chip_id(uint8_t *obuff, uint32_t osize);

/* Peripherals */
void aml_thermal_unknown(void);
uint64_t aml_efuse_read(void *dst, uint32_t offset, uint32_t size);
uint64_t aml_efuse_read_cali(void *dst, uint32_t offset, uint32_t size);
uint64_t aml_efuse_user_max(void);
/* 试验构建的整片 eFuse 只读导出（AML_TRIAL_EFUSE_DUMP，aml_efuse_dump.c）：往 dst 写 AML_EFUSE_DUMP_SIZE 字节（格式见该文件头），回读出的字节数；masks 是读出、字节数不足、两遍不同的块 */
uint32_t aml_efuse_dump(uint8_t *dst, uint32_t masks[3]);

/* 重启原因的过滤（AML_REBOOT_REASON_NO_RPMBP，aml_reboot_reason.c）：原因 9 换成 0，via 是来路（日志用） */
uint32_t aml_reboot_reason_filter(uint32_t reason, const char *via);

/* 试验构建（AML_TRIAL）：只写进内存日志的字符串 */
void aml_trial_memlog_puts(const char *s);

/* DMC 安全寄存器的读写代理与它拒绝的请求的记录（aml_dmc_proxy.c）；spsr、elr 是调用者保存的 SPSR_EL3、ELR_EL3，lr 是调用者模式下的返回地址 */
u_register_t aml_dmc_proxy_rw(u_register_t addr, u_register_t val, u_register_t rw,
			      u_register_t spsr, u_register_t lr, u_register_t elr);
void aml_dmc_proxy_log_read(u_register_t index, uint32_t out[4]);

/* HDMITX 安全寄存器（aml_hdmitx.c；地址按内核 hdmitx 的编码） */
uint32_t aml_hdmitx_sec_read(uint32_t reg);
void aml_hdmitx_sec_write(uint32_t reg, uint32_t val);

/* HDCP 操作类调用（AML_HDCP） */
void aml_hdcp_setup(void);
bool aml_hdcp_sip(uint32_t fid, u_register_t x1, u_register_t *ret);

/* 音频许可查询（AML_AUDIO_LICENCE，aml_audio_licence_svc.c）：处理非安全输入共享区里的请求，回交给调用者的 x0 */
uint64_t aml_audio_licence_sip(void);

/* 安全存储（AML_SSTORE） */
void aml_sstore_setup(void);
bool aml_sstore_sip(uint32_t fid, u_register_t x1, u_register_t flags, u_register_t *ret);
/* BL31 内部读一个 key（语义同 aml_sstore_key_copy，在存储锁下） */
uint32_t aml_sstore_key_get(const char *name, uint8_t *buf, uint32_t len, uint32_t *attr);

#endif /* AML_PRIVATE_H */
