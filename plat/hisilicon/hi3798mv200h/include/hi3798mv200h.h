/*
 * Copyright (c) 2026, David Yang <mmyangfl@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HI3798MV200H_H
#define HI3798MV200H_H

#include <arch.h>
#include <lib/utils_def.h>

/*
 * Every address below is either board-proven on the Hi3798MV200H (Skyworth STB,
 * 4x Cortex-A53) via the 2026-10 trampoline rounds, or read off the vendor DT
 * decompile (mv200h-work/extracted/mv200h-vendor.dts) -- noted per constant.
 * The CRG/GIC/WDG/UART set is identical to the upstream Poplar (Hi3798CV200)
 * port, which is the structural template for this platform.
 */

/*
 * PL011. Base board-proven (vendor DT alias uart0; the trampoline talks to it).
 * Clock is 75 MHz, statically proven from the flash fastboot image: its serial
 * init writes IBRD=40, FBRD=44 (75e6/(16*115200) ~= 40.69); the 24 MHz divisor
 * pair (13/1) exists only in an unused trampoline helper and reprogramming with
 * it killed the console (trampoline attempt 3).
 */
#define PL011_UART0_BASE		U(0xF8B00000)
#define PL011_BAUDRATE			U(115200)
#define PL011_UART0_CLK_IN_HZ		UL(75000000)

/*
 * SC (sysctrl) clock-gate and soft-reset word.  uart_cken gates the PL011 and
 * uart_sel picks its clock: 0 = 3 MHz from the MCU, 1 = 75 MHz, so the 75 MHz
 * divisor pair above needs uart_sel set.  Bit numbers agree between the
 * vendor header (hi3798mv200/hi_reg_sys.h) and the part datasheet, and the
 * healthy fastboot state reads this word as 0x60681100.
 */
#define SC_CLKGATE_SRST_CTRL		U(0xF8000048)
#define SC_UART_CKEN			(U(1) << 12)
#define SC_UART_SEL			(U(1) << 29)

/*
 * System counter at 0xF8BB0000 (CNTCR, CNTFID0 at +0x20), fed by a 24 MHz
 * xtal.  BL31 enables it because no stage before it is known to leave it
 * enabled.  The measured state of the block at this board's bootloader prompt,
 * after a boot in which BL31 wrote both registers, is CNTCR = 0x103 (what BL31
 * wrote), CNTFID0 = 1 (what BL31 did *not* write) and the count at +0x08
 * advancing at 24.07 MHz -- so the enable takes effect, the frequency field
 * does not take a write while CNTCR.EN is set, and the block's rate comes from
 * the clock that feeds it, not from CNTFID0.
 *
 * The two bits beyond CNTCR_EN are load-bearing on this part, measured: the
 * Arm reference platforms write CNTCR_FCREQ(0U) | CNTCR_EN
 * (plat/arm/common/arm_bl31_setup.c:403), and writing that here -- CNTCR = 0x1
 * -- leaves the kernel stuck at "smp: Bringing up secondary CPUs ...", with the
 * secondaries never coming up and nothing on the board recovering it, while
 * every round with 0x103 reports "Brought up 1 node, 4 CPUs" with a tick on
 * each core.  Where 0x103 comes from is the vendor tree, which writes it in two
 * places that both sit behind build guards (fastboot common/cmd_bootm.c:
 * 644-648 for the FPGA and hi3796mv2x configs, and
 * resume_entry_32bit/main.c:19-39 inside an #if 0), and no manual in this
 * workspace describes the block; which of HDBGREN and CNTCR_FCREQ(1U) actually
 * matters is unmeasured, because each answer costs a board round that can
 * wedge the board the same way.  Nothing in this
 * chain consumes CNTFID0: psci_arch_setup() programs CNTFRQ_EL0 from
 * SYS_COUNTER_FREQ_IN_TICKS, which is the frequency the kernel's arch_timer
 * uses.  See the platform doc, §Notes.
 */
#define MV200H_SYS_CNT_BASE	U(0xF8BB0000)
#define MV200H_CNTCR_VALUE	(CNTCR_EN | CNTCR_HDBG | CNTCR_FCREQ(1U))
#define SYS_COUNTER_FREQ_IN_TICKS	UL(24000000)
#define SYS_COUNTER_FREQ_IN_MHZ		U(24)

/*
 * CRG.  Base and the two CPU control offsets are board-proven: REG_CPU_RST
 * (0xf8a22050) is the word that holds and releases the secondary cores, and
 * REG_CPU_LP (0xf8a22048) is the frequency switch the vendor brackets a
 * release with -- measured on this board to change nothing, so this port does
 * not use it (board log, R112).
 */
#define REG_BASE_CRG			U(0xF8A22000)
#define REG_CPU_LP			U(0x48)
#define REG_CPU_RST			U(0x50)

/* Single reset vector for the whole A53 cluster plus the AArch64-at-reset
 * switch.  Board-proven: a released core starts at the word in 0x34 regardless
 * of what is written elsewhere in the block, and AARCH_MODE selects AArch64
 * for the cores coming out of reset.
 */
#define REG_PERI_CPU_RVBARADDR		U(0xF8A80034)
#define REG_PERI_CPU_AARCH_MODE		U(0xF8A80030)
#define REG_AARCH64_ALL_CORES		U(0xF)

/*
 * TZPC 外设安全属性：每个 IP 组一位，置位表示非安全世界可以访问；清零的组在非安全侧读出为零（非安全写入是否生效未验证）。与 Poplar 移植是同一个字（plat/hisilicon/poplar: HISI_TZPC_SEC_ATTR_CTRL）。开放掩码取 hi3798mv2x 移植写入的值（github.com/185264646/ATF-hi3798mv2x，分支 hi3798mv2x_mainline），Hi3798MV300 板上的非安全内核曾在这个值下运行，0xf8004000 处的 gpio5 也在其中。
 */
#define HISI_TZPC_SEC_ATTR_CTRL		U(0xF8A80010)
#define HISI_TZPC_NS_OPEN_MASK		U(0xFFF9FFFF)

/*
 * PMC6：CPU 供电 buck 的 PWM 调压字（[31:16] = duty + 1，[15:0] = period + 1，电压 = 1250 mV − 2.5 mV × duty，厂商 4.4 内核 device_volt_scale/mpu_get_volt）。fastboot 不写它，冷启动读到的 0x003d00dd（1.10 V）来自复位默认值或更早的阶段。
 */
#define HISI_PMC6_CPU_VDD		U(0xF8A23018)

/* GIC-400.  Same pair the vendor DT gives the kernel. */
#define GICD_BASE			U(0xF1001000)
#define GICC_BASE			U(0xF1002000)

/* FIQ platform related defines.  From the vendor ATF's identical mv200 table
 * (unverified on this board: BL31 runs these only if a secure interrupt is
 * routed, which the kernel path never does -- kept for GICv2 prop tables).
 */
#define HISI_IRQ_SEC_SGI_0		U(8)
#define HISI_IRQ_SEC_SGI_1		U(9)
#define HISI_IRQ_SEC_SGI_2		U(10)
#define HISI_IRQ_SEC_SGI_3		U(11)
#define HISI_IRQ_SEC_SGI_4		U(12)
#define HISI_IRQ_SEC_SGI_5		U(13)
#define HISI_IRQ_SEC_SGI_6		U(14)
#define HISI_IRQ_SEC_SGI_7		U(15)
#define HISI_IRQ_SEC_TIMER0		U(60)
#define HISI_IRQ_SEC_TIMER1		U(50)
#define HISI_IRQ_SEC_TIMER2		U(52)
#define HISI_IRQ_SEC_TIMER3		U(88)

/*
 * SP805 watchdog 0.  Board-proven (the kernel's sp805 probe and the CRG94
 * clock/reset bits both act on this exact block).  The register offsets and
 * control bits come from drivers/arm/sp805.h; only the load value is
 * platform's own: 0x249F00 = 100 ms at the 24 MHz counting clock measured on
 * this part, which is what the PSCI SYSTEM_RESET path waits on.
 */
#define HISI_WDG0_BASE			U(0xF8A2C000)
#define HISI_WDG0_RESET_LOAD		U(0x00249F00)

/* CRG94 的 bit 0 是看门狗计数时钟的门控；bit 4 置位时让看门狗保持复位。 */
#define REG_PERI_CRG94			U(0x178)
#define CRG94_WDG0_CKEN			(U(1) << 0)
#define CRG94_WDG0_SRST			(U(1) << 4)

/*
 * HISI_BOOT_WDG 的启动保活装载值：24 MHz 下 170 s，即 hi3798mv200h 上 stage1 启动看门狗时用的值（计数器最多能装 178 s）。中断未清除时 SP805 在第二次到期才发出复位，所以窗口约 340 s。
 */
#define HISI_WDG0_BOOT_LOAD		U(0xF32FDC00)

/*
 * 厂商 CPU_ON 在释放从核前后写入 REG_CPU_LP 的值（HISI_CPU_ON_LP_BRACKET）：先写 0x206，再写 0x606，释放，然后写清掉 bit 10 的保存值，最后写回保存值。
 */
#define CPU_LP_RELEASE_A		U(0x206)
#define CPU_LP_RELEASE_B		U(0x606)
#define CPU_LP_RESTORE_CLR		(U(1) << 10)

#endif /* HI3798MV200H_H */
