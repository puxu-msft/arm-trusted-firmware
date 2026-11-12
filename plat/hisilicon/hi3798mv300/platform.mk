#
# Copyright (c) 2026, xp <a.xp@live.com>.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Hi3798MV300 复用 hi3798mv200h 的 BL31 移植，不同的是镜像布局、启动保活看门狗（这颗芯片上不跑 stage1）和厂商的 CPU_ON 前后写入。未改动的厂商 fastboot 自己就能启动 TF-A FIP：对 FIP 执行 `bootm` 时，它把 BL31 条目复制到 0x04400000（代码里的常量），在关闭 cache 的状态下按 set/way 对数据 cache 做 clean 与 invalidate，把簇复位向量写成 0x04400000 并置 AARCH_MODE |= 0xf，再经 RMR 热复位启动核。BL33 留在 FIP 被加载的位置，所以它的地址由加载器的 FIP 基址（0x01FFFFC0）加 BL33 条目的文件偏移得出：
#
#   0x04400000  BL31（本镜像；厂商 fastboot 的固定目标地址）
#   0x02020000  DTB 槽（FIP 文件偏移 0x20040），经 x0 交给 BL33
#   0x02200000  BL33（FIP 文件偏移 0x200040：uImage 头在 0x200000）
#
# 构建：
#
#   make PLAT=hi3798mv300 RESET_TO_BL31=1 CROSS_COMPILE=aarch64-linux-gnu- bl31
#

MV200H_BL31_BASE		?= 0x04400000
MV200H_DTB_BASE			?= 0x02020000
PRELOADED_BL33_BASE		?= 0x02200000
HISI_BOOT_WDG			?= 1
# 厂商 hi3798mv300 BL31 在每次 CPU_ON 释放从核前后都写 REG_CPU_LP；在这颗芯片上保留它的序列，直到有一轮板测证明可以去掉，就像在 hi3798mv200h 上那样。
HISI_CPU_ON_LP_BRACKET		?= 1
# CM201-2 无法被远程重新上电（没有 PMIC 控制，也没有唤醒源），所以 PSCI SYSTEM_OFF（poweroff、halt、温控的 critical trip）改为复位板子而不是让各核停驻：盒子经正常启动链重新起来，而不是一直死着，直到有人拔插电源。
HISI_SYSTEM_OFF_RESETS		?= 1

$(eval $(call add_defines, MV200H_BL31_BASE MV200H_DTB_BASE))

include plat/hisilicon/hi3798mv200h/platform.mk
