/*
 * Copyright (c) 2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <assert.h>
#include <common/debug.h>
#include <drivers/arm/gicv2.h>
#include <drivers/console.h>
#include <errno.h>
#include <stdbool.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <plat/common/platform.h>
#include <platform_def.h>

#include "aml_private.h"

#if defined(AML_TRIAL) && AML_TRIAL
#include <lib/el3_runtime/cpu_data.h>
#include <lib/el3_runtime/pubsub_events.h>

/* 试验构建的暖启动进度（g12a_def.h 的 AML_TRIAL_WARM_STAGE_REG）：本核走到的一步换成 stage，进入次数与入口属性不动 */
static void g12a_trial_warm_stage(unsigned int stage)
{
	mmio_clrsetbits_32(AML_TRIAL_WARM_STAGE_REG(plat_my_core_pos()), 0xFFU, stage);
}

/* PSCI 的 CPU_ON 收尾做完（psci_cpu_on_finish 事件；之后只剩按 warmboot_ep_info 建上下文与 eret）：记第 3 步与入口属性，入口地址与 SPSR 写进 REG7、REG9 */
static void *g12a_trial_on_done(const void *arg)
{
	const entry_point_info_t *ep = get_cpu_data(warmboot_ep_info);

	(void)arg;
	mmio_clrsetbits_32(AML_TRIAL_WARM_STAGE_REG(plat_my_core_pos()), 0xFF0000FFU,
			   ((ep->h.attr & 0xFFU) << 24) | AML_TRIAL_WARM_ON_DONE);
	mmio_write_32(AML_TRIAL_WARM_PC_REG, (uint32_t)ep->pc);
	mmio_write_32(AML_TRIAL_WARM_SPSR_REG, (uint32_t)ep->spsr);
	return NULL;
}
SUBSCRIBE_TO_EVENT(psci_cpu_on_finish, g12a_trial_on_done);
#endif

#define SCPI_POWER_ON		0
#define SCPI_POWER_RETENTION	1
#define SCPI_POWER_OFF		3

#define SCPI_SYSTEM_SHUTDOWN	0
#define SCPI_SYSTEM_REBOOT	1

/*
 * 核级掉电挂起（CPU_SUSPEND）与核级待机：平台在 platform.mk 里打开（AML_PSCI_CPU_SUSPEND）；整机挂起（SYSTEM_SUSPEND）另有开关 AML_PSCI_SYSTEM_SUSPEND，要求电源域树有系统级（PLATFORM_SYSTEM_COUNT）。
 */
#ifndef AML_PSCI_CPU_SUSPEND
#define AML_PSCI_CPU_SUSPEND	0
#endif
#ifndef AML_PSCI_SYSTEM_SUSPEND
#define AML_PSCI_SYSTEM_SUSPEND	0
#endif
#if AML_PSCI_SYSTEM_SUSPEND && !(AML_PSCI_CPU_SUSPEND && defined(PLATFORM_SYSTEM_COUNT))
#error "AML_PSCI_SYSTEM_SUSPEND needs AML_PSCI_CPU_SUSPEND and a system power level"
#endif
/*
 * CPU0 的 CPU_OFF：上游 g12a 用 WFE 循环模拟（CPU0 不真下电，CPU_ON 时关 MMU 跳回 warm 入口），前提是 A53 的 core_pwr_dwn 已刷过 L1 并退出一致性。HW_ASSISTED_COHERENCY 的平台（SM1 的 A55）不做这份缓存维护，关 MMU 后在同一块 per-cpu 栈上跑 C 代码会读到旧缓存行，所以这类平台打开 AML_CPU0_POWER_DOWN，让 CPU0 与其它核一样经 SCP 真下电、经邮箱入口回来，与厂商 g12a BL31 相同（CPU_ON 0x511974c、CPU_OFF 0x5119bdc 都不特判 CPU0）。
 */
#ifndef AML_CPU0_POWER_DOWN
#define AML_CPU0_POWER_DOWN	0
#endif

/* 唤醒整机挂起后重新设置 AO UART（厂商系统级恢复同样重设控制台，0x5119d14 → 0x5117d58）；console_meson_init 只写 UART 寄存器、不登记控制台 */
int console_meson_init(uintptr_t base_addr, uint32_t uart_clk, uint32_t baud_rate);

static uintptr_t g12a_sec_entrypoint;
static volatile uint32_t g12a_cpu0_go;
#if AML_PSCI_CPU_SUSPEND
/* 正在核级掉电挂起的核：pwr_domain_pwr_down 据此区分挂起与 CPU_OFF（CPU0 的 CPU_OFF 是 WFE 模拟，挂起是真掉电） */
static volatile uint8_t g12a_cpu_suspending[PLATFORM_CORE_COUNT];
#endif

static void g12a_pm_set_reset_addr(u_register_t mpidr, uint64_t value)
{
	unsigned int core = plat_calc_core_pos(mpidr);
	uintptr_t cpu_mailbox_addr = AML_PSCI_MAILBOX_BASE + (core << 4);

	mmio_write_64(cpu_mailbox_addr, value);
}

static void g12a_pm_reset(u_register_t mpidr)
{
	unsigned int core = plat_calc_core_pos(mpidr);
	uintptr_t cpu_mailbox_addr = AML_PSCI_MAILBOX_BASE + (core << 4) + 8;

	mmio_write_32(cpu_mailbox_addr, 0);
}

/*
 * 每核邮箱 +8 的字：厂商 g12a BL31 在发下电类的 SCPI 命令之前写全 1、命令之后写 0（0x5119794 的各调用点），语义未查，照厂商的次序。
 */
static void g12a_pm_mailbox_arm(u_register_t mpidr)
{
	unsigned int core = plat_calc_core_pos(mpidr);

	mmio_write_32(AML_PSCI_MAILBOX_BASE + (core << 4) + 8, ~0U);
}

/*
 * 下一次启动看到的 AO_RTI_STATUS_REG3：[3:0] 换成 reason；USB 启动标志 [15:12] 只在等于 8（内核 panic 后转 USB）且这次不是 panic 重启时清掉，其余值（如线刷工具设的 1、2）留给下次启动。与厂商 g12a BL31 的复位（0x5119ab0–0x5119b0c）、关机（0x51199a4–0x5119a00）相同。
 */
static void g12a_pm_write_reboot_reason(uint32_t reason)
{
	uint32_t status = mmio_read_32(AML_AO_RTI_STATUS_REG3);
	uint32_t next = (status & ~AML_REBOOT_REASON_MASK) | reason;

	if ((((status & AML_USB_BOOT_FLAG_MASK) >> AML_USB_BOOT_FLAG_SHIFT) ==
	     AML_USB_BOOT_FLAG_PANIC_DUMP) &&
	    (reason != AML_REBOOT_REASON_KERNEL_PANIC))
		next &= ~AML_USB_BOOT_FLAG_MASK;

	NOTICE("BL31: Reboot reason: 0x%x -> 0x%x\n", status, next);
	console_flush();

	mmio_write_32(AML_AO_RTI_STATUS_REG3, next);
}

static void __dead2 g12a_system_reset(void)
{
	INFO("BL31: PSCI_SYSTEM_RESET\n");

	u_register_t mpidr = read_mpidr_el1();
	/*
	 * Amlogic 的内核与引导程序把重启原因放在 SYSTEM_RESET 的 x1（AArch32 调用者是 r1）：标准 PSCI 的 SYSTEM_RESET 不带参数，从这次 SMC 保存的非安全上下文里取，只用低 4 位（厂商 0x5119a98）。
	 */
	u_register_t x1 = read_ctx_reg(get_gpregs_ctx(cm_get_context(NON_SECURE)),
				       CTX_GPREG_X1);
	int ret;

	mmio_clrbits_32(AML_AO_SEC_GP_CFG7, AML_AO_SEC_GP_CFG7_CPU_MASK);
	g12a_pm_write_reboot_reason((uint32_t)x1 & AML_REBOOT_REASON_MASK);
	g12a_pm_mailbox_arm(mpidr);

	ret = aml_scpi_sys_power_state(SCPI_SYSTEM_REBOOT);

	if (ret != 0) {
		ERROR("BL31: PSCI_SYSTEM_RESET: SCP error: %i\n", ret);
		panic();
	}

	g12a_pm_reset(mpidr);

	wfi();

	ERROR("BL31: PSCI_SYSTEM_RESET: Operation not handled\n");
	panic();
}

static void __dead2 g12a_system_off(void)
{
	INFO("BL31: PSCI_SYSTEM_OFF\n");

	u_register_t mpidr = read_mpidr_el1();
	unsigned int i;
	int ret;

	/*
	 * 与厂商 g12a BL31 的关机相同（0x5119950–0x5119a50）：各核复位向量指回 BootROM，清每核挂起位，重启原因写 8（下次开机即“关机后唤醒”），邮箱入口写全 1，再请 SCP 关机。
	 */
	for (i = 0U; i < PLATFORM_CORE_COUNT; i++)
		mmio_write_32(AML_SYS_CPU_RVBAR0 + (i << 2), AML_SYS_CPU_RVBAR_ROM);
	mmio_clrbits_32(AML_AO_SEC_GP_CFG7, AML_AO_SEC_GP_CFG7_CPU_MASK);
	g12a_pm_write_reboot_reason(AML_REBOOT_REASON_POWEROFF);
	g12a_pm_set_reset_addr(mpidr, ~0ULL);
	g12a_pm_mailbox_arm(mpidr);

	ret = aml_scpi_sys_power_state(SCPI_SYSTEM_SHUTDOWN);

	if (ret != 0) {
		ERROR("BL31: PSCI_SYSTEM_OFF: SCP error %i\n", ret);
		panic();
	}

	g12a_pm_set_reset_addr(mpidr, 0);
	g12a_pm_reset(mpidr);

	dsbsy();
	wfi();

	ERROR("BL31: PSCI_SYSTEM_OFF: Operation not handled\n");
	panic();
}

/* CPU0 的 CPU_OFF 是否用 WFE 模拟（见 AML_CPU0_POWER_DOWN） */
static bool g12a_cpu0_emulated(unsigned int core)
{
	return (core == AML_PRIMARY_CPU) && (AML_CPU0_POWER_DOWN == 0);
}

static int32_t g12a_pwr_domain_on(u_register_t mpidr)
{
	unsigned int core = plat_calc_core_pos(mpidr);

	/* CPU0 can't be turned OFF */
	if (g12a_cpu0_emulated(core)) {
		VERBOSE("BL31: Releasing CPU0 from wait loop...\n");

		g12a_cpu0_go = 1;
		flush_dcache_range((uintptr_t)&g12a_cpu0_go,
				sizeof(g12a_cpu0_go));
		dsb();
		isb();

		sev();

		return PSCI_E_SUCCESS;
	}

	g12a_pm_set_reset_addr(mpidr, g12a_sec_entrypoint);
	aml_scpi_set_css_power_state(core,
				     SCPI_POWER_ON, SCPI_POWER_ON, SCPI_POWER_ON);
	dmbsy();
	sev();

	return PSCI_E_SUCCESS;
}

static void g12a_pwr_domain_on_finish(const psci_power_state_t *target_state)
{
	unsigned int core = plat_calc_core_pos(read_mpidr_el1());

	assert(target_state->pwr_domain_state[MPIDR_AFFLVL0] ==
					PLAT_LOCAL_STATE_OFF);
#if defined(AML_TRIAL) && AML_TRIAL
	g12a_trial_warm_stage(AML_TRIAL_WARM_ON_FINISH);
#endif

	if (core == AML_PRIMARY_CPU) {
		g12a_cpu0_go = 0;
		flush_dcache_range((uintptr_t)&g12a_cpu0_go,
				sizeof(g12a_cpu0_go));
		dsb();
		isb();
	}

	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();

	/* 起来以后撤掉邮箱入口，与厂商相同（0x5119ca4） */
	g12a_pm_set_reset_addr(read_mpidr_el1(), 0);
}

static void g12a_pwr_domain_off(const psci_power_state_t *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);

	gicv2_cpuif_disable();

	/* CPU0 can't be turned OFF */
	if (g12a_cpu0_emulated(core))
		return;

	/* 下电前邮箱 +8 写全 1，与厂商的 CPU_OFF 相同（0x5119c24–0x5119c2c） */
	g12a_pm_mailbox_arm(mpidr);
	aml_scpi_set_css_power_state(core,
				     SCPI_POWER_OFF, SCPI_POWER_ON,
				     SCPI_POWER_ON);
}

#if AML_PSCI_CPU_SUSPEND
/*
 * 原始格式的 power_state（PSCI_EXTENDED_STATE_ID=0）：待机只认核级；掉电把 0..PowerLevel 级都置 OFF；StateID 必须为 0。与厂商 g12a BL31 的 validate_power_state（0x51197c0）相同；厂商 4.9 内核的 DT 空闲态 CPU_SLEEP_0 用 0x10000（核级掉电）。
 */
static int g12a_validate_power_state(unsigned int power_state,
				     psci_power_state_t *req_state)
{
	unsigned int pwr_lvl = psci_get_pstate_pwrlvl(power_state);
	unsigned int i;

	assert(req_state != NULL);

	if (pwr_lvl > PLAT_MAX_PWR_LVL)
		return PSCI_E_INVALID_PARAMS;

	if (psci_get_pstate_type(power_state) == PSTATE_TYPE_STANDBY) {
		if (pwr_lvl != MPIDR_AFFLVL0)
			return PSCI_E_INVALID_PARAMS;
		req_state->pwr_domain_state[MPIDR_AFFLVL0] =
					PLAT_LOCAL_STATE_RET;
	} else {
		for (i = MPIDR_AFFLVL0; i <= pwr_lvl; i++)
			req_state->pwr_domain_state[i] = PLAT_LOCAL_STATE_OFF;
	}

	if (psci_get_pstate_id(power_state) != 0U)
		return PSCI_E_INVALID_PARAMS;

	return PSCI_E_SUCCESS;
}

/* 核级待机：WFI 期间让 IRQ/FIQ 路由到 EL3 也能唤醒，与厂商相同（0x5119b80–0x5119bd8） */
static void g12a_cpu_standby(plat_local_state_t cpu_state)
{
	u_register_t scr = read_scr_el3();

	assert(cpu_state == PLAT_LOCAL_STATE_RET);

	write_scr_el3(scr | SCR_IRQ_BIT | SCR_FIQ_BIT);
	isb();
	dsb();
	wfi();
	write_scr_el3(scr);
}

static uint32_t g12a_scpi_level_state(const psci_power_state_t *target_state,
				      unsigned int lvl)
{
	return (target_state->pwr_domain_state[lvl] == PLAT_LOCAL_STATE_OFF) ?
		SCPI_POWER_OFF : SCPI_POWER_ON;
}

/*
 * 掉电挂起，照厂商 g12a BL31（0x5119890–0x511990c、0x5119840–0x511988c）：邮箱入口写 warm boot 入口（CPU0 也一样：厂商对 CPU0 同样经邮箱掉电、经 SCP 唤醒），AO_SEC_GP_CFG7 置本核的挂起位，邮箱 +8 写全 1，关本核的 GIC CPU 接口，再按目标状态请 SCP 关核、关簇（簇级 OFF 时）、关整机（系统级 OFF 时）。
 */
static void g12a_pwr_domain_suspend(const psci_power_state_t *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);
	uint32_t css = SCPI_POWER_ON;

	if (target_state->pwr_domain_state[MPIDR_AFFLVL0] !=
	    PLAT_LOCAL_STATE_OFF)
		return;

	g12a_cpu_suspending[core] = 1U;
	g12a_pm_set_reset_addr(mpidr, g12a_sec_entrypoint);
	mmio_setbits_32(AML_AO_SEC_GP_CFG7,
			U(1) << (AML_AO_SEC_GP_CFG7_CPU_SHIFT + core));
	g12a_pm_mailbox_arm(mpidr);
	gicv2_cpuif_disable();
#ifdef PLATFORM_SYSTEM_COUNT
	css = g12a_scpi_level_state(target_state, MPIDR_AFFLVL2);
#endif
	aml_scpi_set_css_power_state(core, SCPI_POWER_OFF,
				     g12a_scpi_level_state(target_state,
							   MPIDR_AFFLVL1),
				     css);
}

/*
 * 掉电挂起之后由 SCP 唤醒、经邮箱入口 warm boot 回来（retention 不经这里）：清本核的挂起位，系统级挂起后先重建 GIC 分发器，再开本核的 GIC 接口、撤掉邮箱入口。与厂商相同（0x5119cac–0x5119d50）。
 */
static void g12a_pwr_domain_suspend_finish(const psci_power_state_t *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);

	if (target_state->pwr_domain_state[MPIDR_AFFLVL0] !=
	    PLAT_LOCAL_STATE_OFF)
		return;

	mmio_clrbits_32(AML_AO_SEC_GP_CFG7,
			U(1) << (AML_AO_SEC_GP_CFG7_CPU_SHIFT + core));
	g12a_cpu_suspending[core] = 0U;
#if defined(AML_TRIAL) && AML_TRIAL
	g12a_trial_warm_stage(AML_TRIAL_WARM_SUSPEND_FINISH);
#endif
#ifdef PLATFORM_SYSTEM_COUNT
	if (target_state->pwr_domain_state[MPIDR_AFFLVL2] ==
	    PLAT_LOCAL_STATE_OFF) {
		(void)console_meson_init(AML_UART0_AO_BASE,
					 AML_UART0_AO_CLK_IN_HZ,
					 AML_UART_BAUDRATE);
		gicv2_distif_init();
	}
#endif
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();
	g12a_pm_set_reset_addr(mpidr, 0);
}

#if AML_PSCI_SYSTEM_SUSPEND
static void g12a_get_sys_suspend_power_state(psci_power_state_t *req_state)
{
	unsigned int i;

	for (i = MPIDR_AFFLVL0; i <= PLAT_MAX_PWR_LVL; i++)
		req_state->pwr_domain_state[i] = PLAT_LOCAL_STATE_OFF;
}
#endif
#endif /* AML_PSCI_CPU_SUSPEND */

static void __dead2 g12a_pwr_domain_pwr_down_wfi(const psci_power_state_t
						 *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);

#if AML_PSCI_CPU_SUSPEND
	/*
	 * 掉电挂起：邮箱 +8 写 0 后在 WFI 里等 SCP 断电，不回卷（厂商 0x5119910 与 0x51272e0 同样不回卷：SCP 已收到关核命令，醒来接着跑会与它断电相撞）。偶发唤醒就再 WFI。
	 */
	if (g12a_cpu_suspending[core] != 0U) {
		g12a_pm_reset(mpidr);
		dsbsy();
		for (;;)
			wfi();
	}
#endif

	/* CPU0 can't be turned OFF, emulate it with a WFE loop */
	if (g12a_cpu0_emulated(core)) {
		VERBOSE("BL31: CPU0 entering wait loop...\n");

		while (g12a_cpu0_go == 0)
			wfe();

		VERBOSE("BL31: CPU0 resumed.\n");

		/*
		 * Because setting CPU0's warm reset entrypoint through PSCI
		 * mailbox and/or mmio mapped RVBAR (0xda834650) does not seem
		 * to work, jump to it manually.
		 * In order to avoid an assert, MMU has to be disabled.
		 */
		disable_mmu_el3();
		((void(*)(void))g12a_sec_entrypoint)();
	}

	/*
	 * 邮箱入口不清：PSCI 在调到这里之前已把本核记成 OFF，这个窗口里别的核发来的 CPU_ON 会先写好入口再请 SCP 上电，清掉它，本核上电后就在复位向量的等待入口（aml_reset_pen）里一直 WFE。厂商 g12a BL31 的掉电路径同样只写 +8 的字。
	 */
	dsbsy();
	g12a_pm_reset(mpidr);

	for (;;)
		wfi();
}

/*******************************************************************************
 * Platform handlers and setup function.
 ******************************************************************************/
static const plat_psci_ops_t g12a_ops = {
	.pwr_domain_on			= g12a_pwr_domain_on,
	.pwr_domain_on_finish		= g12a_pwr_domain_on_finish,
	.pwr_domain_off			= g12a_pwr_domain_off,
	.pwr_domain_pwr_down		= g12a_pwr_domain_pwr_down_wfi,
#if AML_PSCI_CPU_SUSPEND
	.cpu_standby			= g12a_cpu_standby,
	.validate_power_state		= g12a_validate_power_state,
	.pwr_domain_suspend		= g12a_pwr_domain_suspend,
	.pwr_domain_suspend_finish	= g12a_pwr_domain_suspend_finish,
#if AML_PSCI_SYSTEM_SUSPEND
	.get_sys_suspend_power_state	= g12a_get_sys_suspend_power_state,
#endif
#endif
	.system_off			= g12a_system_off,
	.system_reset			= g12a_system_reset
};

int plat_setup_psci_ops(uintptr_t sec_entrypoint,
			const plat_psci_ops_t **psci_ops)
{
	g12a_sec_entrypoint = sec_entrypoint;
	*psci_ops = &g12a_ops;
	g12a_cpu0_go = 0;
	return 0;
}
