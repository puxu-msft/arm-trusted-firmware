#
# Copyright (c) 2019-2024, Arm Limited and Contributors. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

# SM1（S905X3 一族，4×Cortex-A55）：复用 plat/amlogic/g12a 的平台代码——厂商只出一份 g12a 构建的 BL31，同时服务 G12A 与 SM1（M12 厂商 BL31 的构建串 "g12a gv1.3-133-g079690f"，里面 A53 与 A55 的 cpu_ops 都有），差别只在 CPU 核与由它决定的构建配置：
# - Cortex-A55 是 DynamIQ 核，lib/cpus/aarch64/cortex_a55.S 要求 HW_ASSISTED_COHERENCY=1，后者又不许与 USE_COHERENT_MEM=1 同开（make_helpers/constraints.mk）；只编 A55 的 cpu_ops（g12a 只编 A53 的，在 SM1 上复位处理找不到 cpu_ops，BL31 一进入就跳到空指针）。
# - A55 置 MPIDR.MT、核号在 Aff1（实机 0x81000N00），而内核 DT 的 cpu reg 写 0–3（Aff0 形式），核号换算两种都认（AML_MPIDR_AFF1_CORE，plat/amlogic/common 的 aml_helpers.S 与 aml_topology.c）。

include lib/xlat_tables_v2/xlat_tables.mk

AML_PLAT		:=	plat/amlogic
AML_PLAT_SOC		:=	${AML_PLAT}/g12a
AML_PLAT_COMMON		:=	${AML_PLAT}/common

PLAT_INCLUDES		:=	-Iinclude/drivers/amlogic/			\
				-I${AML_PLAT}/${PLAT}/include			\
				-I${AML_PLAT_COMMON}/include

GIC_SOURCES		:=	drivers/arm/gic/common/gic_common.c		\
				drivers/arm/gic/v2/gicv2_main.c			\
				drivers/arm/gic/v2/gicv2_helpers.c		\
				plat/common/plat_gicv2.c

BL31_SOURCES		+=	lib/cpus/aarch64/cortex_a55.S			\
				plat/common/plat_psci_common.c			\
				drivers/amlogic/console/aarch64/meson_console.S	\
				${AML_PLAT_SOC}/g12a_bl31_setup.c		\
				${AML_PLAT_SOC}/g12a_pm.c			\
				${AML_PLAT_SOC}/g12a_common.c			\
				${AML_PLAT_COMMON}/aarch64/aml_helpers.S	\
				${AML_PLAT_COMMON}/aml_efuse.c			\
				${AML_PLAT_COMMON}/aml_mhu.c			\
				${AML_PLAT_COMMON}/aml_scpi.c			\
				${AML_PLAT_COMMON}/aml_sip_svc.c		\
				${AML_PLAT_COMMON}/aml_thermal.c		\
				${AML_PLAT_COMMON}/aml_topology.c		\
				${AML_PLAT_COMMON}/aml_console.c		\
				drivers/amlogic/crypto/sha_dma.c		\
				${XLAT_TABLES_LIB_SRCS}				\
				${GIC_SOURCES}

# Tune compiler for Cortex-A55
ifeq ($($(ARCH)-cc-id),arm-clang)
    TF_CFLAGS_aarch64	+=	-mcpu=cortex-a55
else ifneq ($(filter %-clang,$($(ARCH)-cc-id)),)
    TF_CFLAGS_aarch64	+=	-mcpu=cortex-a55
else
    TF_CFLAGS_aarch64	+=	-mtune=cortex-a55
endif

# Build config flags
# ------------------

# 本机是 r1p0（MIDR 0x411FD050，docs/11）：按 docs/design/cpu-specific-build-macros.rst 适用于 r1p0 的两条
ERRATA_A55_1221012		:= 1
ERRATA_A55_1530923		:= 1

WORKAROUND_CVE_2017_5715	:= 0

# Have different sections for code and rodata
SEPARATE_CODE_AND_RODATA	:= 1

# DynamIQ：缓存一致性由硬件维护
HW_ASSISTED_COHERENCY		:= 1
USE_COHERENT_MEM		:= 0

# 32 位内核（Android 9 的厂商 4.9）在 SVC 下发 CPU_ON 与 CPU_SUSPEND：副核与恢复的核按调用者的执行态进入，EL2 按“已实现但不用”初始化
PSCI_NS_EP_FOLLOWS_CALLER	:= 1
INIT_UNUSED_NS_EL2		:= 1

# 核级掉电挂起与待机（厂商 4.9 内核的 cpuidle 用 CPU_SUSPEND 0x10000）；整机挂起待上机验证，缺省关
AML_PSCI_CPU_SUSPEND		:= 1
# A55 不做 CPU0 下电的缓存维护：CPU0 的 CPU_OFF 也经 SCP 真下电（g12a_pm.c 的 AML_CPU0_POWER_DOWN）
AML_CPU0_POWER_DOWN		:= 1
AML_PSCI_SYSTEM_SUSPEND		:= 0
$(eval $(call assert_booleans, AML_PSCI_CPU_SUSPEND AML_PSCI_SYSTEM_SUSPEND AML_CPU0_POWER_DOWN))
$(eval $(call add_defines, AML_PSCI_CPU_SUSPEND AML_PSCI_SYSTEM_SUSPEND AML_CPU0_POWER_DOWN))

# 厂商 SiP 的小项（HDMITX 安全寄存器、挂起与重启原因、AML_DATA_PROCESS 的 0x40/0x100），见 plat/amlogic/g12a/g12a_def.h
AML_SIP_VENDOR_SMALL		:= 1
$(eval $(call assert_boolean,AML_SIP_VENDOR_SMALL))
$(eval $(call add_define,AML_SIP_VENDOR_SMALL))
ifeq (${AML_SIP_VENDOR_SMALL},1)
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_dmc_proxy.c		\
				${AML_PLAT_COMMON}/aml_hdmitx.c
endif

# 安全存储（unifykey）：现役 Android 9 的内核在运行期直接发存储类 SMC，表由引导程序装好（plat/amlogic/common/aml_sstore*.c，M12 docs/19 §6）
# AML_SSTORE_WRITE_BACK=0 只给 RAM 试验：WRITE/REMOVE 不把新块交回块缓冲、回 1，内核不落盘
# AML_SSTORE_SKIP_UNCHANGED=1（缺省）：值与属性都没变的 WRITE 什么都不改、回 4，厂商内核因此不把整块写进 eMMC（init 每次开机把三个挂载计数写回原值，M12 S5 上机验过）；0 照厂商每次都写块
AML_SSTORE			:= 1
AML_SSTORE_WRITE_BACK		:= 1
AML_SSTORE_SKIP_UNCHANGED	:= 1
$(eval $(call assert_booleans, AML_SSTORE AML_SSTORE_WRITE_BACK AML_SSTORE_SKIP_UNCHANGED))
$(eval $(call add_defines, AML_SSTORE AML_SSTORE_WRITE_BACK AML_SSTORE_SKIP_UNCHANGED))
ifeq (${AML_SSTORE},1)
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_sstore.c			\
				${AML_PLAT_COMMON}/aml_sstore_crypto.c		\
				${AML_PLAT_COMMON}/aml_sstore_svc.c
endif

# HDCP 操作类调用（HDCP14 初始化与开关、HDCP 2.2 结果与 DUK，plat/amlogic/common/aml_hdcp*.c）：寄存器经 HDMITX 安全访问器（AML_SIP_VENDOR_SMALL）；key 从安全存储读、解密用它的 AES，“key 在不在库”的 0x8200001A/1B 也在安全存储里（AML_SSTORE）
AML_HDCP			:= 1
$(eval $(call assert_boolean,AML_HDCP))
$(eval $(call add_define,AML_HDCP))
ifeq (${AML_HDCP},1)
ifneq (${AML_SIP_VENDOR_SMALL}${AML_SSTORE},11)
  $(error AML_HDCP=1 needs AML_SIP_VENDOR_SMALL=1 and AML_SSTORE=1 on ${PLAT}: the HDCP calls write HDMITX secure registers through aml_hdmitx.c and take the keys from the secure storage, which also answers 0x8200001A/1B)
endif
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_hdcp.c			\
				${AML_PLAT_COMMON}/aml_hdcp_svc.c
endif

# 音频许可查询（SiP 0x82000050，plat/amlogic/common/aml_audio_licence*.c 与 aml_rsa.c）：现役 Android 9 的 Dolby 与 DRA 软解库经内核 audio_data 查许可，查不到就当没有许可（DRA 静音，M12 docs/19 §9）
# 要用厂商 BL31 里内置的四把 RSA 私钥：钥材料不进源码树，由构建入口从钥匙真源生成 aml_audio_licence_keys.h（定义 AML_AUDIO_LICENCE_KEYS_INIT），AML_AUDIO_LICENCE_KEYS_DIR 是它所在的目录（相对 TF-A 源码树或绝对路径）
AML_AUDIO_LICENCE		:= 1
AML_AUDIO_LICENCE_KEYS_DIR	:=
$(eval $(call assert_boolean,AML_AUDIO_LICENCE))
$(eval $(call add_define,AML_AUDIO_LICENCE))
ifeq (${AML_AUDIO_LICENCE},1)
ifeq ($(wildcard ${AML_AUDIO_LICENCE_KEYS_DIR}/aml_audio_licence_keys.h),)
  $(error AML_AUDIO_LICENCE=1 needs AML_AUDIO_LICENCE_KEYS_DIR=<directory holding aml_audio_licence_keys.h> on ${PLAT}: the RSA private keys of the audio licence query are kept out of the source tree, the build entry generates that header from the key file (M12 tools/tf-a/build-bl31.sh); or build with AML_AUDIO_LICENCE=0)
endif
PLAT_INCLUDES		+=	-I${AML_AUDIO_LICENCE_KEYS_DIR}
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_audio_licence.c		\
				${AML_PLAT_COMMON}/aml_audio_licence_svc.c	\
				${AML_PLAT_COMMON}/aml_rsa.c
endif

# 重启原因 9（rpmbp）按 0 写：厂商 BL2 见到原因 9 且 RPMB 没有密钥时会把派生的密钥永久写进 eMMC 的 RPMB（plat/amlogic/common/aml_reboot_reason.c，M12 docs/16 §2.5；用户 2026-11-26 定，定型构建也带）
AML_REBOOT_REASON_NO_RPMBP	:= 1
$(eval $(call assert_boolean,AML_REBOOT_REASON_NO_RPMBP))
$(eval $(call add_define,AML_REBOOT_REASON_NO_RPMBP))
ifeq (${AML_REBOOT_REASON_NO_RPMBP},1)
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_reboot_reason.c
endif

# 试验辅助（只给 RAM 试验件）：冷启动武装 EE 看门狗、日志另写一份到内存（plat/amlogic/g12a/g12a_bl31_setup.c）
AML_TRIAL			:= 0
AML_TRIAL_WDT_MS		:= 30000
$(eval $(call assert_boolean,AML_TRIAL))
$(eval $(call add_defines, AML_TRIAL AML_TRIAL_WDT_MS))
ifeq (${AML_TRIAL},1)
BL31_SOURCES		+=	drivers/coreboot/cbmem_console/aarch64/cbmem_console.S	\
				${AML_PLAT_COMMON}/aml_smc_trace.c
endif
# 试验构建的整片 eFuse 只读导出（SiP 0x820000F1，plat/amlogic/common/aml_efuse_dump.c）：为把原厂 eFuse 作永久备份单独打开，只能与 AML_TRIAL=1 同开；它把整片 eFuse（含安全存储的取钥材料 E）交给非安全世界（M12 docs/19 §8）
AML_TRIAL_EFUSE_DUMP		:= 0
$(eval $(call assert_boolean,AML_TRIAL_EFUSE_DUMP))
$(eval $(call add_define,AML_TRIAL_EFUSE_DUMP))
ifeq (${AML_TRIAL_EFUSE_DUMP},1)
ifneq (${AML_TRIAL}${AML_SSTORE},11)
  $(error AML_TRIAL_EFUSE_DUMP=1 needs AML_TRIAL=1 and AML_SSTORE=1 on ${PLAT}: the dump hands the whole eFuse (with the secure storage key material) to the normal world, so it is only built into RAM trial builds, and it takes SHA-256 from the secure storage code)
endif
BL31_SOURCES		+=	${AML_PLAT_COMMON}/aml_efuse_dump.c
endif
# 试验构建把每次 SiP 与 PSCI 调用记进内存日志（plat/amlogic/common/aml_smc_trace.c）
PLAT_SMC_TRACE			:= ${AML_TRIAL}
ifeq (${PLAT_SMC_TRACE},1)
ifneq (${AML_TRIAL},1)
  $(error PLAT_SMC_TRACE=1 needs AML_TRIAL=1 on ${PLAT}: plat_smc_trace() is only built into trial builds (plat/amlogic/common/aml_smc_trace.c))
endif
endif

# 核号在 MPIDR 的 Aff1（兼认 Aff0 形式的目标 MPIDR）
AML_MPIDR_AFF1_CORE		:= 1
$(eval $(call assert_boolean,AML_MPIDR_AFF1_CORE))
$(eval $(call add_define,AML_MPIDR_AFF1_CORE))

# 各核的复位向量（SYS_CPU_CFG2–5）指向 BL31 的等待入口 aml_reset_pen，核上电后按自己的邮箱跳进 warm boot 入口，与厂商 g12a BL31 相同（plat/amlogic/g12a/g12a_bl31_setup.c）。
# 核从复位状态直接进 BL31，warm boot 入口要自己初始化 SCTLR_EL3：PROGRAMMABLE_RESET_ADDRESS=1（BL31 的冷启动入口不受它影响，RESET_TO_BL31=0）
AML_RESET_PEN			:= 1
$(eval $(call assert_boolean,AML_RESET_PEN))
$(eval $(call add_define,AML_RESET_PEN))
ifeq (${AML_RESET_PEN},1)
PROGRAMMABLE_RESET_ADDRESS	:= 1
endif

# Verify build config
# -------------------

ifneq (${RESET_TO_BL31}, 0)
  $(error Error: ${PLAT} needs RESET_TO_BL31=0)
endif

ifeq (${ARCH},aarch32)
  $(error Error: AArch32 not supported on ${PLAT})
endif
