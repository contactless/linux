// SPDX-License-Identifier: GPL-2.0
/*
 * Wiren Board 8 (Allwinner T507/H616): warm-reset the SoC shortly after
 * a panic so the panic log persists in the ramoops region.
 *
 * The ramoops region at the top of DRAM survives a SoC watchdog reset
 * (DRAM stays powered), after which U-Boot parks the records on eMMC
 * and performs a full recovery power cycle (the "pstore shuttle").
 * Without this hook a panicked kernel just parks until the EC watchdog
 * power-cycles the board, destroying DRAM and the records with it.
 *
 * The panic notifier arms the SoC watchdog with the shortest interval
 * (~0.5 s) using two raw MMIO writes - nothing here can sleep, lock or
 * allocate, so it is safe in panic context. The sunxi watchdog device
 * (watchdog0) is idle on Wiren Board systems: userspace feeds the EC
 * watchdog (watchdog1) instead.
 *
 * The hook arms itself only if a ramoops region actually registered:
 * on systems booted by an older U-Boot (no injected ramoops node)
 * panic behaviour is intentionally left unchanged, so updating the
 * kernel alone changes nothing (no-lockstep updates).
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/panic_notifier.h>

/* sun6i-compatible watchdog in the H616/T507 timer block */
#define WB8_WDT_BASE		0x030090a0
#define WB8_WDT_SIZE		0x20
#define WB8_WDT_CFG		0x14	/* 1 = whole-system reset */
#define WB8_WDT_MODE		0x18	/* bit0 = enable, INTV[7:4] = 0 -> 0.5 s */

static void __iomem *wb8_wdt_regs;

static int wb8_ramoops_panic_reset(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	writel(1, wb8_wdt_regs + WB8_WDT_CFG);
	writel(1, wb8_wdt_regs + WB8_WDT_MODE);
	pr_emerg("wb8-ramoops-panic-reset: SoC watchdog armed, warm reset in ~0.5 s\n");

	return NOTIFY_DONE;
}

static struct notifier_block wb8_ramoops_panic_nb = {
	.notifier_call = wb8_ramoops_panic_reset,
	/* run late: let other notifiers log first, the dump is on panic anyway */
	.priority = INT_MIN + 1,
};

static int __init wb8_ramoops_panic_reset_init(void)
{
	struct device_node *np;

	if (!of_machine_is_compatible("allwinner,sun50i-h616"))
		return 0;

	/*
	 * Arm only when a ramoops region exists (injected into
	 * /reserved-memory by the Wiren Board U-Boot DT fixup).
	 */
	np = of_find_compatible_node(NULL, NULL, "ramoops");
	if (!np)
		return 0;
	of_node_put(np);

	wb8_wdt_regs = ioremap(WB8_WDT_BASE, WB8_WDT_SIZE);
	if (!wb8_wdt_regs)
		return -ENOMEM;

	atomic_notifier_chain_register(&panic_notifier_list,
				       &wb8_ramoops_panic_nb);
	pr_info("wb8-ramoops-panic-reset: armed (ramoops present)\n");

	return 0;
}
late_initcall(wb8_ramoops_panic_reset_init);
