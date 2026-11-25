/*
 * Copyright (c) 2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef G12A_DEF_H
#define G12A_DEF_H

#include <lib/utils_def.h>

/*******************************************************************************
 * System oscillator
 ******************************************************************************/
#define AML_OSC24M_CLK_IN_HZ			ULL(24000000) /* 24 MHz */

/*******************************************************************************
 * Memory regions
 ******************************************************************************/
#define AML_HDCP_RX_BASE			UL(0xFFE0D000)
#define AML_HDCP_RX_SIZE			UL(0x00002000)

#define AML_HDCP_TX_BASE			UL(0xFFE01000)
#define AML_HDCP_TX_SIZE			UL(0x00001000)

#define AML_NS_SHARE_MEM_BASE			UL(0x05000000)
#define AML_NS_SHARE_MEM_SIZE			UL(0x00100000)

#define AML_SEC_SHARE_MEM_BASE			UL(0x05200000)
#define AML_SEC_SHARE_MEM_SIZE			UL(0x00100000)

#define AML_GIC_DEVICE_BASE			UL(0xFFC00000)
#define AML_GIC_DEVICE_SIZE			UL(0x00008000)

#define AML_NSDRAM0_BASE			UL(0x01000000)
#define AML_NSDRAM0_SIZE			UL(0x0F000000)

#define BL31_BASE				UL(0x05100000)
#define BL31_SIZE				UL(0x00100000)
#define BL31_LIMIT				(BL31_BASE + BL31_SIZE)

/* Shared memory used for SMC services */
#define AML_SHARE_MEM_INPUT_BASE		UL(0x050FE000)
#define AML_SHARE_MEM_OUTPUT_BASE		UL(0x050FF000)

/*
 * 试验构建（AML_TRIAL=1）的内存日志：非安全共享区里的一段 cbmem 格式环形缓冲（头 8 字节：u32 正文长、u32 游标，游标 bit31 = 已回绕；见 drivers/coreboot/cbmem_console）。厂商 g12a BL31 把 0x05000000–0x05100000 分作存储输入、输出、块缓冲、消息区（各 0x40000，消息区只用前 0x1C 字节）与 0x050FE000/0x050FF000 的通用输入输出（M12 厂商 BL31 0x5120958–0x5120984、0x5119de4–0x5119df8）；0x050E0000–0x050F0000 落在消息区的空闲部分，厂商 BL2/BL31/BL33 的二进制里没有指向 0x050C0000–0x05100000 的数据字，指令里只有消息区基址与通用输入输出两处。主线 U-Boot 与内核的 DT 把 0x05000000 起 3 MiB 保留给安全监控（secmon@5000000），不会占用它。
 */
#define AML_TRIAL_MEMLOG_BASE			UL(0x050E0000)
#define AML_TRIAL_MEMLOG_SIZE			UL(0x00010000)

/*
 * EE 看门狗（试验构建用来兜住 BL31 与 BL33 早期的挂死）：寄存器同主线 U-Boot 与 Linux 的 meson_gxbb_wdt；1 ms 时基的 CTRL 值（24 MHz 晶振 / 0x5DC0，CLKDIV_EN、CLK_EN、EE_RESET）与武装顺序同 M12 主线 U-Boot 环境的硬截止（patches/u-boot/m12.env 的 m12_console_deadline：先写不带 EN 的 CTRL、TCNT、清计数，最后置 EN），TCNT 以毫秒计、最大 0xFFFF。
 */
#define AML_WDT_CTRL				UL(0xFFD0F0D0)
#define AML_WDT_TCNT				UL(0xFFD0F0D8)
#define AML_WDT_RSET				UL(0xFFD0F0DC)
#define AML_WDT_CTRL_1MS			U(0x03205DC0)
#define AML_WDT_CTRL_EN				U(0x00040000)
#define AML_WDT_TCNT_MAX			U(0xFFFF)

#define AML_SEC_DEVICE0_BASE			UL(0xFFD00000)
#define AML_SEC_DEVICE0_SIZE			UL(0x00026000)

#define AML_SEC_DEVICE1_BASE			UL(0xFF800000)
#define AML_SEC_DEVICE1_SIZE			UL(0x0000A000)

#define AML_TZRAM_BASE				UL(0xFFFA0000)
#define AML_TZRAM_SIZE				UL(0x00048000)

/* Mailboxes */
#define AML_MHU_SECURE_SCP_TO_AP_PAYLOAD	UL(0xFFFE7800)
#define AML_MHU_SECURE_AP_TO_SCP_PAYLOAD	UL(0xFFFE7A00)
#define AML_PSCI_MAILBOX_BASE			UL(0xFFFE7F00)

#define AML_SEC_DEVICE2_BASE			UL(0xFF620000)
#define AML_SEC_DEVICE2_SIZE			UL(0x00028000)

/*******************************************************************************
 * GIC-400 and interrupt handling related constants
 ******************************************************************************/
#define AML_GICD_BASE				UL(0xFFC01000)
#define AML_GICC_BASE				UL(0xFFC02000)

#define IRQ_SEC_PHY_TIMER			29

#define IRQ_SEC_SGI_0				8
#define IRQ_SEC_SGI_1				9
#define IRQ_SEC_SGI_2				10
#define IRQ_SEC_SGI_3				11
#define IRQ_SEC_SGI_4				12
#define IRQ_SEC_SGI_5				13
#define IRQ_SEC_SGI_6				14
#define IRQ_SEC_SGI_7				15
#define IRQ_SEC_SGI_8				16

/*******************************************************************************
 * UART definitions
 ******************************************************************************/
#define AML_UART0_AO_BASE			UL(0xFF803000)
#define AML_UART0_AO_CLK_IN_HZ			AML_OSC24M_CLK_IN_HZ
#define AML_UART_BAUDRATE			U(115200)

/*******************************************************************************
 * Memory-mapped I/O Registers
 ******************************************************************************/
#define AML_AO_TIMESTAMP_CNTL			UL(0xFF8000B4)

#define AML_SYS_CPU_CFG7			UL(0xFF634664)

/*
 * SYS_CPU_CFG2–5（Khadas u-boot arch-g12a/secure_apb.h:769–778，每核一个）：厂商 BL31 关机前写成 0xFFFF0000（BootROM 入口），按用法推断是各核的复位向量（RVBAR）；关机之后靠遥控器等经 SCP 唤醒时，核从这里起步。
 */
#define AML_SYS_CPU_RVBAR0			UL(0xFF634650)
#define AML_SYS_CPU_RVBAR_ROM			U(0xFFFF0000)

/*
 * AO_RTI_STATUS_REG3：G12 家族在 AO 寄存器区偏移 0xC（Khadas u-boot arch-g12a/secure_apb.h:3144），不是 AXG 的 0x1C——G12 的 0xFF80001C 是 AO_PAD_DS_A（同文件 3158 行，AO 引脚驱动强度）。[3:0] 是重启原因，[15:12] 是给下次启动的 USB 启动标志；厂商 g12a BL31 的 PSCI 复位与关机写的就是这里（M12 厂商 BL31 反汇编 0x51199a4、0x5119ab0）。
 */
#define AML_AO_RTI_STATUS_REG3			UL(0xFF80000C)
#define AML_AO_RTI_SCP_STAT			UL(0xFF80023C)

/*
 * AO_SEC_GP_CFG7（arch-g12a/secure_apb.h:3560）：[15:8] 每核一位，厂商 BL31 在核级挂起时置位、恢复时清掉（0x5119890、0x5119cac），复位与关机时整段清零（0x5119a8c、0x5119988）；BL30 怎样用它未查。
 */
#define AML_AO_SEC_GP_CFG7			UL(0xFF80025C)
#define AML_AO_SEC_GP_CFG7_CPU_SHIFT		U(8)
#define AML_AO_SEC_GP_CFG7_CPU_MASK		U(0xFF00)

/*
 * AO_RTI_STATUS_REG3 的字段（厂商 g12a BL31 与 BL2 的用法）：[3:0] 重启原因（PSCI 复位的 x1 低 4 位；下次启动 BL30 把它搬到 AO_RTI_SCP_STAT[15:12] 再把 [3:0] 置 0xD），[15:12] USB 启动标志（8 = 内核 panic 后转 USB）。
 */
#define AML_REBOOT_REASON_MASK			U(0xF)
#define AML_REBOOT_REASON_KERNEL_PANIC		U(0xC)
#define AML_REBOOT_REASON_POWEROFF		U(0x8)
#define AML_USB_BOOT_FLAG_SHIFT			U(12)
#define AML_USB_BOOT_FLAG_MASK			U(0xF000)
#define AML_USB_BOOT_FLAG_PANIC_DUMP		U(0x8)
#define AML_AO_RTI_SCP_READY_OFF		U(0x14)
#define AML_A0_RTI_SCP_READY_MASK		U(3)
#define AML_AO_RTI_SCP_IS_READY(v)					\
	((((v) >> AML_AO_RTI_SCP_READY_OFF) &				\
	AML_A0_RTI_SCP_READY_MASK) == AML_A0_RTI_SCP_READY_MASK)

#define AML_HIU_MAILBOX_SET_0			UL(0xFF63C404)
#define AML_HIU_MAILBOX_STAT_0			UL(0xFF63C408)
#define AML_HIU_MAILBOX_CLR_0			UL(0xFF63C40C)
#define AML_HIU_MAILBOX_SET_3			UL(0xFF63C428)
#define AML_HIU_MAILBOX_STAT_3			UL(0xFF63C42C)
#define AML_HIU_MAILBOX_CLR_3			UL(0xFF63C430)

/*
 * eFuse 用户区（厂商 g12a BL31 的做法，M12 厂商 BL31 反汇编）：用户偏移 0 对应 eFuse 字节 0x1A0（读 0x511a3dc、写 0x511a304 都加 0x1A0），用户区最多 0x60 字节，再减去 eFuse 字节 0x190 给出的 TEE OTP 区（该字节 × 16，超过 0x60 按 0x60 算，0x511a288–0x511a2dc），即 EFUSE_USER_MAX 返回 0x60 − TEE 区（0x511a2e0）。
 */
#define AML_EFUSE_USER_BASE			U(0x1A0)
#define AML_EFUSE_USER_SPAN			U(0x60)
#define AML_EFUSE_TEE_AREA_SIZE			U(0x190)

#define AML_SHA_DMA_BASE			UL(0xFF63E000)
#define AML_SHA_DMA_DESC			(AML_SHA_DMA_BASE + 0x08)
#define AML_SHA_DMA_STATUS			(AML_SHA_DMA_BASE + 0x28)

/*******************************************************************************
 * System Monitor Call IDs and arguments
 ******************************************************************************/
#define AML_SM_GET_SHARE_MEM_INPUT_BASE		U(0x82000020)
#define AML_SM_GET_SHARE_MEM_OUTPUT_BASE	U(0x82000021)

#define AML_SM_EFUSE_READ			U(0x82000030)
#define AML_SM_EFUSE_USER_MAX			U(0x82000033)

#define AML_SM_JTAG_ON				U(0x82000040)
#define AML_SM_JTAG_OFF				U(0x82000041)
#define AML_SM_GET_CHIP_ID			U(0x82000044)

/*
 * 厂商 g12a BL31 的 SiP 里，现役 Android 9（厂商 4.9 内核与厂商 BL33）要用、又不涉及安全存储的几个（调用方盘点与 M12 docs/19 §9 的复查；eFuse 写只回 0、不写；PLAT=sm1 打开 AML_SIP_VENDOR_SMALL）：
 * - 0x82000018/19：HDMITX 安全寄存器读、写（厂商 0x511ed20、0x511eda4；地址的编码见 aml_sip_svc.c）；
 * - 0x82000042：设挂起原因，AO_RTI_STATUS_REG3[11:8] = (x1 == 1)（厂商 0x5119eb8；内核关机前发 0x82000042(1)）；
 * - 0x82000043：SET_USB_BOOT，模式 1、2、3、4 把 AO_RTI_STATUS_REG3[15:12] 写成 0、1、2、8，其余模式不改，一律回 0（厂商 0x5119ee4 与跳转表 0x5128048；厂商 BL33 在 ROM USB 启动时的烧录分支 0x105857c 先发 0x82000043(1)，set_usb_boot 命令 0x1045ad0 也发它；厂商 BL2 0xfffa382c 在 [15:12] = 2 时把启动设备当作 7）；
 * - 0x8200003D：读 eFuse 的校准数据（厂商 0x5118150 → 平台操作表 +0xa8 = 0x511a50c）：x1 是偏移、x2 是长度，按 32 位比较，偏移 ≤ 6、长度 ≤ 7、1 ≤ 偏移 + 长度 ≤ 7 时把整片 eFuse 的字节 [0x11 + 偏移, 0x11 + 偏移 + 长度) 读进输出共享区、回读到的字节数，否则回 0。偏移是整片 eFuse 的字节坐标，与 0x82000030 的用户区坐标不同（那里加 0x1A0，0x511a3dc）；读法同 0x82000030（厂商 0x5118db0：SCPI eFuse 读，回 SCP 给的字节数）。调用方是厂商 BL33 的 eFuse 封装 0x1002300（类型 4）：返回值不为 0 就当字节数从输出共享区拷出（0x10023f4 sxtw 后调 memcpy），回 SMC_UNK 就拷 −1 个字节；开机路径上的 0x106e2d8 → 0x105474c 读偏移 0 起 7 字节、要求回 7；
 * - 0x82000031：eFuse 写，不提供，一律回 0（写了 0 字节）。厂商 BL33 的 eFuse 封装 0x1002300 对类型 1 只看返回值是否为 0（0x1002400：0 报失败、非 0 报成功），回 SMC_UNK 会让 efuse 写命令误报成功；内核的 eFuse 写在 M12 上不可达（DT 的 efuse 节点 disabled）；
 * - 0x82000046：TEST_N 的输出使能（厂商 0x5119570）：x1 = 1 时置 AO_GPIO_TEST_N（0xFF800140）bit0，x1 = 0 时清，其余不改，回 0；内核 meson_g12a_aobus_init 开机发一次 0x82000046(1)；
 * - 0x82000049：设重启原因，AO_RTI_STATUS_REG3[3:0]（厂商 0x5119e94）；
 * - 0x8200004A：DMC 安全寄存器的读写代理（厂商 0x5119f70）：x1 地址、x2 值、x3 = 0 读（回读到的 32 位）、1 写（回 0），x3 其余值回 0。厂商不查地址；这里只代理内核 dmc_monitor 用到的那一页（DT 的 reg_base 0xFF639000 起 0x400 字节、4 字节对齐），页外读回 0、写不做——照厂商不查地址，非安全世界就能经它读写 BL31 自己的内存（E 在那里，与“不把 E 交给非安全世界”冲突），页表外的地址还会让 EL3 取址异常。内核 dmc_monitor 每秒读、写一次 DMC_SEC_STATUS（0xFF6392E0），回 SMC_UNK 时每次都在 EL3 打一行错误日志；
 * - 0x820000FF：AML_DATA_PROCESS，只认类型 0x40 与 0x100、不做事、回 0（厂商 bootm 的 aml_sec_boot_check 发 0x40，回非 0 就中止引导），别的类型（eFuse 写、口令等）一律回 SMC_UNK。
 */
#define AML_SM_HDMITX_SEC_READ			U(0x82000018)
#define AML_SM_HDMITX_SEC_WRITE			U(0x82000019)
#define AML_SM_SET_SUSPEND_REASON		U(0x82000042)
#define AML_SM_SET_USB_BOOT			U(0x82000043)
#define AML_SM_EFUSE_READ_CALI			U(0x8200003D)
#define AML_SM_EFUSE_WRITE			U(0x82000031)
#define AML_SM_TEST_N_OUTPUT			U(0x82000046)
#define AML_SM_DMC_RW				U(0x8200004A)
#define AML_SM_SET_REBOOT_REASON		U(0x82000049)
#define AML_SM_DATA_PROCESS			U(0x820000FF)

#define AML_DATA_PROCESS_BOOT_CHECK		U(0x40)
#define AML_DATA_PROCESS_NOOP_0x100		U(0x100)

/* eFuse 校准数据的窗口（SiP 0x8200003D）：整片 eFuse 的字节坐标 */
#define AML_EFUSE_CALI_BASE			U(0x11)
#define AML_EFUSE_CALI_SPAN			U(7)
#define AML_EFUSE_CALI_OFFSET_MAX		U(6)

#define AML_AO_GPIO_TEST_N			UL(0xFF800140)

/* DMC 安全寄存器页（SiP 0x8200004A 代理的范围，厂商 DT 的 dmc_monitor reg_base） */
#define AML_DMC_SEC_BASE			UL(0xFF639000)
#define AML_DMC_SEC_SIZE			UL(0x400)

/* HDMITX：厂商 SiP 0x18/0x19 允许的两块（厂商 0x511ed20–0x511eda0） */
#define AML_HDMITX_SEC_BASE			UL(0xFF600000)
#define AML_HDMITX_SEC_SIZE			UL(0x00010000)
#define AML_HDMITX_SEC_DWC_SEL			U(0x10000000)
#define AML_HDMITX_SEC_OFF_MASK			U(0x00FFFFFF)
#define AML_HDMITX_DWC_BASE			UL(0xFF600000)
#define AML_HDMITX_DWC_MAX			U(0x7E31)
#define AML_HDMITX_TOP_BASE			UL(0xFF608000)
#define AML_HDMITX_TOP_MAX			U(0x365E)
#define AML_HDMITX_TOP_BYTE_START		U(0x2000)

#define AML_AO_RTI_STATUS_REG3_SUSPEND_MASK	U(0x0F00)
#define AML_AO_RTI_STATUS_REG3_SUSPEND		U(0x0100)

/*
 * 安全存储（unifykey，AML_SSTORE=1 时提供，plat/amlogic/common/aml_sstore*.c）：fid 与非安全共享缓冲照厂商 g12a BL31（M12 docs/16 §4.2–§4.3）。四个缓冲各 0x40000、首尾相接：请求、应答、整块加密镜像（内核与引导程序把它与 eMMC reserved 分区的两份副本互拷）、消息区（只用前 0x1C 字节：+4 写后置 1、+8 写计数）。
 */
#define AML_SSTORE_IN_BASE			UL(0x05000000)
#define AML_SSTORE_OUT_BASE			UL(0x05040000)
#define AML_SSTORE_BLOCK_BASE			UL(0x05080000)
#define AML_SSTORE_MSG_BASE			UL(0x050C0000)

#define AML_SM_STORAGE_IN_BASE			U(0x82000023)
#define AML_SM_STORAGE_OUT_BASE			U(0x82000024)
#define AML_SM_STORAGE_BLOCK_BASE		U(0x82000025)
#define AML_SM_STORAGE_MSG_BASE			U(0x82000026)
#define AML_SM_STORAGE_BLOCK_SIZE		U(0x82000027)
#define AML_SM_SET_STORAGE_INFO			U(0x82000028)
#define AML_SM_STORAGE_QUERY			U(0x82000060)
#define AML_SM_STORAGE_READ			U(0x82000061)
#define AML_SM_STORAGE_WRITE			U(0x82000062)
#define AML_SM_STORAGE_TELL			U(0x82000063)
#define AML_SM_STORAGE_VERIFY			U(0x82000064)
#define AML_SM_STORAGE_STATUS			U(0x82000065)
#define AML_SM_STORAGE_NOTIFY			U(0x82000066)
#define AML_SM_STORAGE_LIST			U(0x82000067)
#define AML_SM_STORAGE_REMOVE			U(0x82000068)
#define AML_SM_STORAGE_NOTIFY_EX		U(0x82000069)
#define AML_SM_STORAGE_SET_ENCTYPE		U(0x8200006A)
#define AML_SM_STORAGE_GET_ENCTYPE		U(0x8200006B)
#define AML_SM_STORAGE_VERSION			U(0x8200006C)

/*
 * HDCP 的 key 在不在库（AML_SSTORE 提供，aml_sstore_svc.c）：安全存储里有名为 hdcp、不短于 0x120 字节的 key 时 0x8200001A 回 1，有名为 hdcp22_fw_private、不短于 0x20 字节的 key 时 0x8200001B 回 1，否则回 0（厂商 0x511e764、0x511ea44：从安全存储把这个 key 读进 0x120、0x20 字节的缓冲，读满才回 1；0x5120bd0 拷 min(长度, 缓冲长)）。
 * 厂商在读之前先看一个“key 已由安全世界交进来”的标志（[0x513b574]、[0x513b69c]），只有安全世界发的 0x82000081、0x82000082 置它（非安全调用者回未知，0x51184d0、0x5118520 的 cbnz x23）；本平台没有安全世界的调用者，这个标志恒为假。内核的 show_hdcp_lstore 按两个返回值报 HDCP 能力，systemcontrol 据此决定做不做 HDCP 认证；回 SMC_UNK 会被当成两种 key 都在（M12 没烧 HDCP key，厂商在这台机器上回 0）。
 */
#define AML_SM_HDCP14_KEY_LSTORE		U(0x8200001A)
#define AML_SM_HDCP22_KEY_LSTORE		U(0x8200001B)
#define AML_HDCP14_KEY_NAME			"hdcp"
#define AML_HDCP14_KEY_LEN			U(0x120)
#define AML_HDCP22_KEY_NAME			"hdcp22_fw_private"
#define AML_HDCP22_KEY_LEN			U(0x20)

/* SET_STORAGE_INFO 写的 AO 寄存器（[30:28]）；GET_ENCTYPE 读的 ASSIST_HW_REV（厂商拿低字节与 0x23 比，0x23 是 Amlogic 的 TXL 族号） */
#define AML_SSTORE_INFO_REG			UL(0xFF800248)
#define AML_ASSIST_HW_REV			UL(0xFFD0814C)

#define AML_JTAG_STATE_ON			U(0)
#define AML_JTAG_STATE_OFF			U(1)

#define AML_JTAG_M3_AO				U(0)
#define AML_JTAG_M3_EE				U(1)
#define AML_JTAG_A53_AO				U(2)
#define AML_JTAG_A53_EE				U(3)

#endif /* G12A_DEF_H */
