/*
 * Copyright (c) 2018-2020, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch.h>
#include <asm_macros.S>
#include <assert_macros.S>
#include <platform_def.h>

	.globl	plat_crash_console_flush
	.globl	plat_crash_console_init
	.globl	plat_crash_console_putc
	.globl	platform_mem_init
	.globl	plat_is_my_cpu_primary
	.globl	plat_my_core_pos
	.globl	plat_reset_handler
	.globl	plat_calc_core_pos
#if defined(AML_RESET_PEN) && AML_RESET_PEN
	.globl	aml_reset_pen
#endif

	/* -----------------------------------------------------
	 * unsigned int plat_my_core_pos(void);
	 * -----------------------------------------------------
	 */
func plat_my_core_pos
	mrs	x0, mpidr_el1
	b	plat_calc_core_pos
endfunc plat_my_core_pos

	/* -----------------------------------------------------
	 *  unsigned int plat_calc_core_pos(u_register_t mpidr);
	 * -----------------------------------------------------
	 */
func plat_calc_core_pos
#if defined(AML_MPIDR_AFF1_CORE) && AML_MPIDR_AFF1_CORE
	/*
	 * 核号在 Aff1（A55 置 MT 位，实机 MPIDR 0x81000N00）；内核按 DT 的 cpu reg 给的目标 MPIDR 是 Aff0 形式（0–3）。Aff1 不为 0 取 Aff1，否则取 Aff0，与厂商 g12a BL31 的换算相同（M12 厂商 BL31 0x511a190）。
	 */
	ubfx	x1, x0, #MPIDR_AFF1_SHIFT, #MPIDR_AFFINITY_BITS
	and	x0, x0, #MPIDR_CPU_MASK
	cmp	x1, #0
	csel	x0, x1, x0, ne
#else
	and	x0, x0, #MPIDR_CPU_MASK
#endif
	ret
endfunc plat_calc_core_pos

	/* -----------------------------------------------------
	 * unsigned int plat_is_my_cpu_primary(void);
	 * -----------------------------------------------------
	 */
func plat_is_my_cpu_primary
	mrs	x0, mpidr_el1
	and	x0, x0, #(MPIDR_CLUSTER_MASK | MPIDR_CPU_MASK)
	cmp	x0, #AML_PRIMARY_CPU
	cset	w0, eq
	ret
endfunc plat_is_my_cpu_primary

	/* ---------------------------------------------
	 * void platform_mem_init(void);
	 * ---------------------------------------------
	 */
func platform_mem_init
	ret
endfunc platform_mem_init

	/* ---------------------------------------------
	 * int plat_crash_console_init(void)
	 * ---------------------------------------------
	 */
func plat_crash_console_init
	mov_imm	x0, AML_UART0_AO_BASE
	mov_imm	x1, AML_UART0_AO_CLK_IN_HZ
	mov_imm	x2, AML_UART_BAUDRATE
	b	console_meson_init
endfunc plat_crash_console_init

	/* ---------------------------------------------
	 * int plat_crash_console_putc(int c)
	 * Clobber list : x1, x2
	 * ---------------------------------------------
	 */
func plat_crash_console_putc
	mov_imm	x1, AML_UART0_AO_BASE
	b	console_meson_core_putc
endfunc plat_crash_console_putc

	/* ---------------------------------------------
	 * void plat_crash_console_flush()
	 * Out : void.
	 * Clobber list : x0, x1
	 * ---------------------------------------------
	 */
func plat_crash_console_flush
	mov_imm	x0, AML_UART0_AO_BASE
	b	console_meson_core_flush
endfunc plat_crash_console_flush

	/* ---------------------------------------------
	 * void plat_reset_handler(void);
	 * ---------------------------------------------
	 */
func plat_reset_handler
	ret
endfunc plat_reset_handler

#if defined(AML_RESET_PEN) && AML_RESET_PEN
	/* ---------------------------------------------------------------------
	 * void aml_reset_pen(void)
	 * 各核的复位向量（bl31_platform_setup 写进 SYS_CPU_CFG2–5，g12a_bl31_setup.c）。核上电后从复位状态进来（MMU 与缓存关）：先把 SCTLR_EL3.EE 清成小端（复位值由实现定义）、VBAR_EL3 指向 BL31 的异常向量（复位值未定义，等待时出异常也落到已知的处理），再按核号读自己的邮箱（AML_PSCI_MAILBOX_BASE + 核号 × 16），是 0 就 WFE 等，不是 0 就跳过去。
	 * 邮箱的写入：CPU_ON 由 pwr_domain_on 在请 SCP 上电之前写成 warm boot 入口；核级掉电挂起由 pwr_domain_suspend 在请 SCP 关核之前写好，醒来时直接用。跳过去之后 SCTLR_EL3、VBAR_EL3 由 warm boot 入口按 PROGRAMMABLE_RESET_ADDRESS 整个初始化。
	 * 与厂商 g12a BL31 的入口 0x5126000 相同（M12 厂商 BL31：清 EE、设 VBAR_EL3、按核号读邮箱、等到非 0 就 br）。按 4 KiB 对齐：厂商的入口 0x05126000 在 4 KiB 边界上；这里的 0x05101000 的 bit12 是 1（厂商的是 0），S4b 实测副核照样进来，复位向量寄存器至少 bit12 起作用，更低的位没查。
	 * 不用栈；用 x0–x2 与 x30（plat_calc_core_pos 用 x0、x1）。
	 * ---------------------------------------------------------------------
	 */
func aml_reset_pen, 12
	mrs	x0, sctlr_el3
	bic	x0, x0, #SCTLR_EE_BIT
	msr	sctlr_el3, x0
	isb
	adr	x0, runtime_exceptions
	msr	vbar_el3, x0
	isb
	mrs	x0, mpidr_el1
	bl	plat_calc_core_pos
	mov_imm	x1, AML_PSCI_MAILBOX_BASE
	add	x1, x1, x0, lsl #4
1:	ldr	x2, [x1]
	cbnz	x2, 2f
	wfe
	b	1b
2:	br	x2
endfunc aml_reset_pen
#endif
