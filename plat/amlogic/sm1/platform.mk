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

# 核号在 MPIDR 的 Aff1（兼认 Aff0 形式的目标 MPIDR）
AML_MPIDR_AFF1_CORE		:= 1
$(eval $(call assert_boolean,AML_MPIDR_AFF1_CORE))
$(eval $(call add_define,AML_MPIDR_AFF1_CORE))

# Verify build config
# -------------------

ifneq (${RESET_TO_BL31}, 0)
  $(error Error: ${PLAT} needs RESET_TO_BL31=0)
endif

ifeq (${ARCH},aarch32)
  $(error Error: AArch32 not supported on ${PLAT})
endif
