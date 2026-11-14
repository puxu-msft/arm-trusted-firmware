/*
 * Copyright (c) 2026, David Yang <mmyangfl@gmail.com>.
 * Based on plat/hisilicon/poplar.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <platform_def.h>

#include <arch.h>
#include <arch_helpers.h>
#include <bl31/bl31.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <cortex_a53.h>
#include <drivers/arm/pl011.h>
#include <context.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <drivers/generic_delay_timer.h>
#include <lib/mmio.h>
#include <lib/utils.h>
#include <plat/common/platform.h>

#include "hi3798mv200h.h"
#include "plat_private.h"

static entry_point_info_t bl33_image_ep_info;
static console_t console;

#if HISI_BOOT_WDG
#include <drivers/arm/sp805.h>

/*
 * 给自己不启动看门狗的加载器准备的启动保活网（hi3798mv300 上厂商 fastboot 的 FIP 路径直接热复位进本镜像）。冷启动路径上第一件事就启动它，这样从这里到内核 sp805_wdt probe 之间任何一处卡死（BL31、BL33、内核早期启动）都会让板子复位；之后由内核驱动接管该模块，BL31 除 SYSTEM_RESET 外不再碰它。序列与 hi3798mv200h 上的 stage1 相同：打开门控并解除复位、解锁、写 LOAD、再写 CONTROL，不重新上锁（在这个系列上，写完 LOAD 再写锁会冻结计数器）。CRG 写入后的稳定空转照搬厂商 fastboot（它在解锁该模块前于 VA 0xc08800 空转 1000 次）；回读不符就重试启动，因为后面没有任何环节会发现看门狗没启动。
 */
#define HISI_BOOT_WDG_TRIES	3U

static bool hisi_boot_wdg_arm(void)
{
	unsigned int try, i;

	for (try = 0U; try < HISI_BOOT_WDG_TRIES; try++) {
		mmio_clrsetbits_32(REG_BASE_CRG + REG_PERI_CRG94,
				   CRG94_WDG0_SRST, CRG94_WDG0_CKEN);
		(void)mmio_read_32(REG_BASE_CRG + REG_PERI_CRG94);
		dsb();
		for (i = 0U; i < 1000U; i++) {
			__asm__ volatile("nop");
		}
		mmio_write_32(HISI_WDG0_BASE + SP805_WDOG_LOCK_OFF,
			      WDOG_UNLOCK_KEY);
		mmio_write_32(HISI_WDG0_BASE + SP805_WDOG_LOAD_OFF,
			      HISI_WDG0_BOOT_LOAD);
		mmio_write_32(HISI_WDG0_BASE + SP805_WDOG_CTR_OFF,
			      SP805_CTR_INTEN | SP805_CTR_RESEN);
		if ((mmio_read_32(HISI_WDG0_BASE + SP805_WDOG_LOAD_OFF) ==
		     HISI_WDG0_BOOT_LOAD) &&
		    ((mmio_read_32(HISI_WDG0_BASE + SP805_WDOG_CTR_OFF) &
		      (SP805_CTR_INTEN | SP805_CTR_RESEN)) ==
		     (SP805_CTR_INTEN | SP805_CTR_RESEN))) {
			return true;
		}
	}
	return false;
}
#endif

entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t type)
{
	entry_point_info_t *next_image_info;

	assert(sec_state_is_valid(type));
	next_image_info = (type == NON_SECURE)
			? &bl33_image_ep_info : NULL;
	/*
	 * None of the images on the ARM development platforms can have 0x0
	 * as the entrypoint
	*/
	if ((next_image_info != NULL) && (next_image_info->pc != 0))
		return next_image_info;
	else
		return NULL;
}

/*
 * There is no BL2 on this platform: stage1 (out of tree, ARM32) arms the boot
 * watchdog, writes the cluster reset vector and warm-resets the boot core into
 * this image.  So the BL33 entry point is fixed at build time, using the
 * upstream PRELOADED_BL33_BASE build option (its default is in platform.mk):
 * the kernel Image tftp'd to that address, entered at EL2h
 * (the level the trampoline rounds proved the kernel boots cleanly at, and the
 * level booting.rst recommends), with the DTB address as its only argument.
 */
void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	unsigned int val;

#if HISI_BOOT_WDG
	bool wdg_armed = hisi_boot_wdg_arm();
#endif

	/*
	 * Inference, not measurement: the warm reset is believed to clear the
	 * uart0 clock gate and drop uart_sel to the 3 MHz MCU clock (no round
	 * has yet returned the grab words below, so the reset state of this
	 * register has never been read).  A healthy fastboot leaves both bits
	 * set, so restoring them is idempotent if the belief is wrong and
	 * required if it is right.  It must happen before the PL011 is
	 * touched: with the gate closed the upstream console putc spins on
	 * TXFF forever, and a clockless block reads UARTFR as 0.
	*/
	val = mmio_read_32(SC_CLKGATE_SRST_CTRL);
	mmio_write_32(SC_CLKGATE_SRST_CTRL,
		      val | SC_UART_CKEN | SC_UART_SEL);
	console_pl011_register(PL011_UART0_BASE, PL011_UART0_CLK_IN_HZ,
			       PL011_BAUDRATE, &console);

	/* Init console for crash report */
	plat_crash_console_init();

#if HISI_BOOT_WDG
	if (wdg_armed) {
		NOTICE("BL31: boot watchdog armed, %lus per expiry\n",
		       HISI_WDG0_BOOT_LOAD / SYS_COUNTER_FREQ_IN_TICKS);
	} else {
		ERROR("BL31: boot watchdog did not read back armed\n");
	}
#endif

	/*
	 * On the warm-reset entry this part has EL2 disabled until HCE lands
	 * alone, so TF-A's one-shot SCR write relies on the HCE pre-write in
	 * plat_reset_handler (trampoline attempt 9).  Nothing else from the
	 * vendor fastboot's EL3 state is inherited on purpose.
	*/

	zeromem(&bl33_image_ep_info, sizeof(bl33_image_ep_info));

	bl33_image_ep_info.pc = (uintptr_t)PRELOADED_BL33_BASE;
	/*
	 * R99: EL2, not EL1. The chain this board is known good at (the display
	 * bring-up log) entered at EL2, so Linux ran el2_setup, is_hyp_mode_
	 * available() became true, and arch_timer_select_ppi() took the arm64
	 * fall-through ARCH_TIMER_PHYS_NONSECURE_PPI -- the DT's second cell,
	 * INTID 30 -- which is what that log prints as "(phys)". At EL1 that
	 * same function takes the !is_hyp_mode_available() branch and picks the
	 * virtual timer, INTID 27, and R85 measured exactly 27 enabled by the
	 * bound event: nobody owns EL2 there, so no comparator can ever fire.
	 * A53 is ARMv8.0, so VHE is not available and EL2 entry cannot land on
	 * the hyp PPI.
	*/
	bl33_image_ep_info.spsr = SPSR_64(MODE_EL2, MODE_SP_ELX,
		  DISABLE_ALL_EXCEPTIONS);
	bl33_image_ep_info.args.arg0 = (uintptr_t)MV200H_DTB_BASE;
	/*
	 * NON_SECURE, not the secure world this board's vendor boot uses:
	 * upstream wants a non-secure kernel, and the R95 secure-payload
	 * arm is a closed fork -- see the board log.
	*/
	SET_SECURITY_STATE(bl33_image_ep_info.h.attr, NON_SECURE);

	if (bl33_image_ep_info.pc == 0)
		panic();
}

/*
 * Enable the SoC's system counter before anything reads CNTPCT_EL0.  This is
 * what plat/arm/common/arm_bl31_setup.c and plat/arm/board/fvp/fvp_common.c do
 * for the Arm development platforms; here it has to be BL31's job because no
 * stage before this one is known to leave the counter enabled (platform doc,
 * §Notes).
 *
 * CNTFID0 is deliberately not written.  While CNTCR.EN is set the field does
 * not take the write -- CNTCR in the same block reads back what this function
 * wrote while CNTFID0 keeps the value it had before (board-proven, §Notes) --
 * and the in-tree code that does change this field clears CNTCR.EN around the
 * write (drivers/st/clk/stm32mp_clkfunc.c: stm32mp_stgen_config).  Nothing in
 * this chain reads the field back either: the frequency the kernel is told is
 * CNTFRQ_EL0, which psci_arch_setup() programs from
 * SYS_COUNTER_FREQ_IN_TICKS.
 */
static void hisi_sys_counter_setup(void)
{
	mmio_write_32(MV200H_SYS_CNT_BASE + CNTCR_OFF, MV200H_CNTCR_VALUE);
}

#if HISI_TZPC_NS_OPEN
/*
 * 加载器留下的属性字，以及开放后回读的属性字。TZPC 所在的 0xf8a80000 窗口从非安全侧读出全 0（2026-11-14 开放前实测），所以这份副本是从 Linux 看到它的途径：它位于 BL31 的保留 RAM 中（地址查 bl31.elf），经 /dev/mem 读取。
 */
static uint32_t hisi_tzpc_attr[2];

/*
 * 开放非安全内核要驱动的 IP 组。只置位、不清位：加载器已开放的组无论掩码怎么写都保持开放。
 */
static void hisi_tzpc_ns_open(void)
{
	hisi_tzpc_attr[0] = mmio_read_32(HISI_TZPC_SEC_ATTR_CTRL);
	mmio_write_32(HISI_TZPC_SEC_ATTR_CTRL,
		      hisi_tzpc_attr[0] | HISI_TZPC_NS_OPEN_MASK);
	hisi_tzpc_attr[1] = mmio_read_32(HISI_TZPC_SEC_ATTR_CTRL);
	flush_dcache_range((uintptr_t)hisi_tzpc_attr, sizeof(hisi_tzpc_attr));
	NOTICE("BL31: TZPC 0x%x -> 0x%x\n", hisi_tzpc_attr[0],
	       hisi_tzpc_attr[1]);
}
#endif

/*
 * BL31 入口时的 PMC6（CPU 调压字）。fastboot 不写 PMC6，所以这就是这次复位后留下的值：用来判断 Linux 降压后经 SysRq-B 或看门狗复位，CPU 电压会不会带着低压进入下一次启动（fastboot 以 1.2 GHz 运行）。与 hisi_tzpc_attr 一样放在 BL31 的保留 RAM 里，Linux 经 /dev/mem 读（地址查 bl31.elf）。
 */
static uint32_t hisi_pmc6_at_entry;

void bl31_platform_setup(void)
{
	hisi_pmc6_at_entry = mmio_read_32(HISI_PMC6_CPU_VDD);
	flush_dcache_range((uintptr_t)&hisi_pmc6_at_entry, sizeof(hisi_pmc6_at_entry));
	hisi_sys_counter_setup();
#if HISI_TZPC_NS_OPEN
	hisi_tzpc_ns_open();
#endif
	/* Init arch timer */
	generic_delay_timer_init();

	/* Init GIC distributor and CPU interface */
	hisi_gic_driver_init();
#if HISI_GIC_GROUP1_ALL
	hisi_gic_group1(true);
#endif
	/*
	 * R99: not here. gicv2_distif_init()/gicv2_pcpu_distif_init()/
	 * gicv2_cpuif_enable() rewrite the distributor and the interface from
	 * the secure bank on every boot, which is the one thing the two chains
	 * that are known to deliver interrupts do not have: the vendor's own
	 * build of this chip's ATF leaves both calls in the TEE-enabled branch
	 * it does not build (hisi_bl31_setup.c:167-189), and this board's
	 * display bring-up chain (docs/mv200h-display-board-139.txt, our own
	 * 7.3-rc7 build, HDMI and ethernet drivers with live IRQs) went through
	 * a trampoline that never touched the GIC either. The kernel's own
	 * gic_dist_init() programs everything it needs; driver_init stays
	 * because plat_ic_* uses the registered bases.
	*/
}

void bl31_plat_arch_setup(void)
{
	plat_configure_mmu_el3(BL31_BASE,
			       (BL31_LIMIT - BL31_BASE),
			       BL_CODE_BASE,
			       BL_CODE_END,
			       BL_COHERENT_RAM_BASE,
			       BL_COHERENT_RAM_END);

	INFO("BL33: kernel 0x%lx dtb 0x%lx\n",
	     (unsigned long)bl33_image_ep_info.pc,
	     (unsigned long)bl33_image_ep_info.args.arg0);

	/*
	 * The kernel invalidates its own Image but never the DTB, and this
	 * part's warm reset does not flush the cluster L2 (trampoline rounds):
	 * a dirty line left there by a previous run would shadow the tftp'd
	 * bytes.  DDR is identity-mapped here, so invalidate the DTB window;
	 * a no-op whenever no stale line exists, and the tftp'd content is
	 * what the kernel then reads either way.
	*/
	inv_dcache_range(MV200H_DTB_BASE, MV200H_DTB_MAX_SIZE);
}

void bl31_plat_runtime_setup(void)
{
	/*
	 * Documentation/arm64/booting.rst requires the EL3 boot code to leave
	 * CNTVOFF_EL2 at zero; nothing on this path writes it before BL31.
	*/
	write_cntvoff_el2(0);
}

