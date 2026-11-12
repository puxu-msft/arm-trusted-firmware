/*
 * Copyright (c) 2026, David Yang <mmyangfl@gmail.com>.
 * Based on plat/hisilicon/poplar.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <platform_def.h>

#include <stdbool.h>

#include <drivers/arm/gic_common.h>
#include <drivers/arm/gicv2.h>
#include <lib/mmio.h>
#include <plat/common/platform.h>

#include "plat_private.h"

/******************************************************************************
 * On a GICv2 system, the Group 1 secure interrupts are treated as Group 0
 * interrupts.
 *****************************************************************************/
static const interrupt_prop_t hisi_interrupt_props[] = {
	MV200H_G1S_IRQ_PROPS(GICV2_INTR_GROUP0),
	MV200H_G0_IRQ_PROPS(GICV2_INTR_GROUP0)
};

static unsigned int target_mask_array[PLATFORM_CORE_COUNT];

static const gicv2_driver_data_t hisi_gic_data = {
	.gicd_base = MV200H_GICD_BASE,
	.gicc_base = MV200H_GICC_BASE,
	.interrupt_props = hisi_interrupt_props,
	.interrupt_props_num = ARRAY_SIZE(hisi_interrupt_props),
	.target_masks = target_mask_array,
	.target_masks_num = ARRAY_SIZE(target_mask_array),
};

/******************************************************************************
 * Helper to initialize the GICv2 only driver.
 *
 * This is the only GIC entry point EL3 needs: the distributor belongs to the
 * non-secure payload, and every round in which EL3 programmed it here ended
 * with no interrupt delivery.  See the R99 finding in
 * docs/mv200h-tfa-bl31-boardlog-2026-10-26.md.
 *****************************************************************************/
void hisi_gic_driver_init(void)
{
	gicv2_driver_init(&hisi_gic_data);
}

#if HISI_GIC_GROUP1_ALL
/******************************************************************************
 * 把每个中断交给非安全世界：GICD_IGROUPRn = ~0，并打开 GICC_PMR。
 *
 * Hi3798MV300 的厂商 fastboot 让它的内核跑在安全世界，并把所有中断线留在 Group 0。在非安全内核看来这些中断线是 RAZ/WI，于是 gic_get_cpumask() 读到 0（"GIC CPU mask not found - kernel will fail to boot"），arch_timer PPI 也永远不会到达（板测日志 2026-11-12，从死掉的内核留在 DDR 里的 printk 缓冲区读回）。同一个 fastboot 还把每个 CPU 接口的 GICC_PMR 留在 Secure 优先级（非安全视角读出 0x00）；只要它处于 0x00-0x7f，非安全侧对 GICC_PMR 的写入就会被忽略，内核自己的 0xf0 永远写不进去，所有中断一直被屏蔽，第一个 msleep() 再也不返回（板测日志 2026-11-12：hisi_femac 与 tsensor 的 probe 卡在各自的第一次睡眠）。这里只写 group 位和屏蔽值；优先级、目标与使能仍交给内核自己的 gic_dist_init()/gic_cpu_init()，R99 发现它们必须归内核掌管。GICD_IGROUPR0 与 GICC_PMR 都按 CPU banked，所以每个核各设自己那一份：启动核以 all_lines 调用，从核从 pwr_domain_on_finish 调用。
 *****************************************************************************/
void hisi_gic_group1(bool all_lines)
{
	unsigned int lines = 32U;
	unsigned int n;

	if (all_lines) {
		lines = ((mmio_read_32(MV200H_GICD_BASE + GICD_TYPER) &
			  TYPER_IT_LINES_NO_MASK) + 1U) * 32U;
	}
	for (n = 0U; n < lines; n += 32U) {
		mmio_write_32(MV200H_GICD_BASE + GICD_IGROUPR + (n >> 3), ~0U);
	}
	mmio_write_32(MV200H_GICC_BASE + GICC_PMR, GIC_PRI_MASK);
}
#endif
