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

#define AML_JTAG_STATE_ON			U(0)
#define AML_JTAG_STATE_OFF			U(1)

#define AML_JTAG_M3_AO				U(0)
#define AML_JTAG_M3_EE				U(1)
#define AML_JTAG_A53_AO				U(2)
#define AML_JTAG_A53_EE				U(3)

#endif /* G12A_DEF_H */
