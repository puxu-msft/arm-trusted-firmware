/*
 * Copyright (c) 2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <common/bl_common.h>
#include <common/interrupt_props.h>
#include <drivers/arm/gicv2.h>
#include <lib/mmio.h>
#include <lib/xlat_tables/xlat_mmu_helpers.h>
#include <plat/common/platform.h>
#include <platform_def.h>

#include "aml_private.h"

#ifndef AML_TRIAL
#define AML_TRIAL	0
#endif

#if AML_TRIAL
#include <drivers/coreboot/cbmem_console.h>
#include <lib/cassert.h>

/*
 * 试验辅助（AML_TRIAL=1，只给 RAM 试验件；写进闪存的构建不开）。本机没有可用的串口，BL31 自己看不见：
 * - 冷启动一进来就武装 EE 看门狗（AML_TRIAL_WDT_MS 毫秒），不在 BL31 里撤：BL31 或 BL33 早期挂死时到点复位，设备回到闪存上的启动链；主线 U-Boot 的看门狗驱动 probe 时重写 CTRL（不带 EN）、再按 60 s 自启并喂狗（CONFIG_WATCHDOG_AUTOSTART），所以它起来以后由它接管。厂商 BL2 跳进 BL31 之前关掉了它自己的看门狗（M12 厂商 BL2 0xfffa18d8 在 0xfffab208 处调用），不武装时 BL31 挂死只能断电。
 * - 日志另写一份到非安全内存里的 cbmem 环形缓冲（AML_TRIAL_MEMLOG_BASE），BL31 按非缓存映射它（g12a_common.c）：每行写下去就在 DRAM 里，未处理异常、死等之后被看门狗复位也不丢已写的行（缓存里的脏行复位时不写回）；主线 U-Boot 里用 md 读出（读法见 M12 的 docs/19）。未处理异常的寄存器转储只走 UART 的崩溃控制台（plat_crash_console_*），不进这里。
 * aml_trial_marker 是试验构建的标记，打包与验收工具据此认出试验件。
 */
CASSERT((AML_TRIAL_WDT_MS > 0) && (AML_TRIAL_WDT_MS <= AML_WDT_TCNT_MAX),
	assert_aml_trial_wdt_ms_range);
CASSERT((AML_TRIAL_MEMLOG_BASE >= AML_NS_SHARE_MEM_BASE) &&
	(AML_TRIAL_MEMLOG_BASE + AML_TRIAL_MEMLOG_SIZE <= AML_SHARE_MEM_INPUT_BASE),
	assert_aml_trial_memlog_in_ns_share_mem);

static const char aml_trial_marker[] = "homeiot-tfa-trial";
static console_cbmc_t aml_trial_memlog;

static void aml_trial_wdt_arm(void)
{
	mmio_write_32(AML_WDT_CTRL, AML_WDT_CTRL_1MS);
	mmio_write_32(AML_WDT_TCNT, AML_TRIAL_WDT_MS);
	mmio_write_32(AML_WDT_RSET, 0U);
	mmio_write_32(AML_WDT_CTRL, AML_WDT_CTRL_1MS | AML_WDT_CTRL_EN);
}

static void aml_trial_memlog_init(void)
{
	/* MMU 还没开：直接写进 DRAM。正文长写进头里，驱动注册时读一次、此后只信安全侧的副本 */
	mmio_write_32(AML_TRIAL_MEMLOG_BASE, AML_TRIAL_MEMLOG_SIZE - 8U);
	mmio_write_32(AML_TRIAL_MEMLOG_BASE + 4U, 0U);
	(void)console_cbmc_register(AML_TRIAL_MEMLOG_BASE, &aml_trial_memlog);
	console_set_scope(&aml_trial_memlog.console, CONSOLE_FLAG_BOOT |
			  CONSOLE_FLAG_RUNTIME);
}
#endif

/*
 * Placeholder variables for copying the arguments that have been passed to
 * BL31 from BL2.
 */
static entry_point_info_t bl32_image_ep_info;
static entry_point_info_t bl33_image_ep_info;
static image_info_t bl30_image_info;
static image_info_t bl301_image_info;

/*******************************************************************************
 * Return a pointer to the 'entry_point_info' structure of the next image for
 * the security state specified. BL33 corresponds to the non-secure image type
 * while BL32 corresponds to the secure image type. A NULL pointer is returned
 * if the image does not exist.
 ******************************************************************************/
entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t type)
{
	entry_point_info_t *next_image_info;

	next_image_info = (type == NON_SECURE) ?
			  &bl33_image_ep_info : &bl32_image_ep_info;

	/* None of the images can have 0x0 as the entrypoint. */
	if (next_image_info->pc != 0U)
		return next_image_info;

	return NULL;
}

/*******************************************************************************
 * Perform any BL31 early platform setup. Here is an opportunity to copy
 * parameters passed by the calling EL (S-EL1 in BL2 & S-EL3 in BL1) before
 * they are lost (potentially). This needs to be done before the MMU is
 * initialized so that the memory layout can be used while creating page
 * tables. BL2 has flushed this information to memory, so we are guaranteed
 * to pick up good data.
 ******************************************************************************/
struct g12a_bl31_param {
	param_header_t h;
	image_info_t *bl31_image_info;
	entry_point_info_t *bl32_ep_info;
	image_info_t *bl32_image_info;
	entry_point_info_t *bl33_ep_info;
	image_info_t *bl33_image_info;
	image_info_t *scp_image_info[];
};

void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	struct g12a_bl31_param *from_bl2;

#if AML_TRIAL
	aml_trial_wdt_arm();
	aml_trial_memlog_init();
#endif

	/* Initialize the console to provide early debug support */
	aml_console_init();

#if AML_TRIAL
	NOTICE("BL31: %s: EE watchdog %u ms (left armed for BL33), memory log 0x%lx+0x%lx\n",
	       aml_trial_marker, (unsigned int)AML_TRIAL_WDT_MS,
	       (unsigned long)AML_TRIAL_MEMLOG_BASE,
	       (unsigned long)AML_TRIAL_MEMLOG_SIZE);
#endif

	from_bl2 = (struct g12a_bl31_param *)arg0;

	/* Check params passed from BL2 are not NULL. */
	assert(from_bl2 != NULL);
	assert(from_bl2->h.type == PARAM_BL31);
	assert(from_bl2->h.version >= VERSION_1);

	/*
	 * Copy BL32 and BL33 entry point information. It is stored in Secure
	 * RAM, in BL2's address space.
	 */
	bl32_image_ep_info = *from_bl2->bl32_ep_info;
	bl33_image_ep_info = *from_bl2->bl33_ep_info;

	if (bl33_image_ep_info.pc == 0U) {
		ERROR("BL31: BL33 entrypoint not obtained from BL2\n");
		panic();
	}

	bl30_image_info = *from_bl2->scp_image_info[0];
	bl301_image_info = *from_bl2->scp_image_info[1];
}

void bl31_plat_arch_setup(void)
{
	aml_setup_page_tables();

	enable_mmu_el3(0);
}

/*******************************************************************************
 * GICv2 driver setup information
 ******************************************************************************/
static const interrupt_prop_t g12a_interrupt_props[] = {
	INTR_PROP_DESC(IRQ_SEC_PHY_TIMER, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_0, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_1, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_2, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_3, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_4, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_5, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_6, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(IRQ_SEC_SGI_7, GIC_HIGHEST_SEC_PRIORITY,
		       GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL)
};

static const gicv2_driver_data_t g12a_gic_data = {
	.gicd_base = AML_GICD_BASE,
	.gicc_base = AML_GICC_BASE,
	.interrupt_props = g12a_interrupt_props,
	.interrupt_props_num = ARRAY_SIZE(g12a_interrupt_props)
};

void bl31_platform_setup(void)
{
	aml_mhu_secure_init();

	gicv2_driver_init(&g12a_gic_data);
	gicv2_distif_init();
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();
}
