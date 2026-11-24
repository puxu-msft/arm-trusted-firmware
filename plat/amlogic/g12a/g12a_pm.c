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
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <plat/common/platform.h>
#include <platform_def.h>

#include "aml_private.h"

#define SCPI_POWER_ON		0
#define SCPI_POWER_RETENTION	1
#define SCPI_POWER_OFF		3

#define SCPI_SYSTEM_SHUTDOWN	0
#define SCPI_SYSTEM_REBOOT	1

static uintptr_t g12a_sec_entrypoint;
static volatile uint32_t g12a_cpu0_go;

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

static int32_t g12a_pwr_domain_on(u_register_t mpidr)
{
	unsigned int core = plat_calc_core_pos(mpidr);

	/* CPU0 can't be turned OFF */
	if (core == AML_PRIMARY_CPU) {
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

	if (core == AML_PRIMARY_CPU) {
		g12a_cpu0_go = 0;
		flush_dcache_range((uintptr_t)&g12a_cpu0_go,
				sizeof(g12a_cpu0_go));
		dsb();
		isb();
	}

	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();
}

static void g12a_pwr_domain_off(const psci_power_state_t *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);

	gicv2_cpuif_disable();

	/* CPU0 can't be turned OFF */
	if (core == AML_PRIMARY_CPU)
		return;

	aml_scpi_set_css_power_state(core,
				     SCPI_POWER_OFF, SCPI_POWER_ON,
				     SCPI_POWER_ON);
}

static void __dead2 g12a_pwr_domain_pwr_down_wfi(const psci_power_state_t
						 *target_state)
{
	u_register_t mpidr = read_mpidr_el1();
	unsigned int core = plat_calc_core_pos(mpidr);

	/* CPU0 can't be turned OFF, emulate it with a WFE loop */
	if (core == AML_PRIMARY_CPU) {
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

	dsbsy();
	g12a_pm_set_reset_addr(mpidr, 0);
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
