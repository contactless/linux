// SPDX-License-Identifier: GPL-2.0
/*
 * Wiren Board 7 (Allwinner R40/A40i) and Wiren Board 8 (Allwinner
 * T507/H616): warm-reset the SoC shortly after a panic so the panic
 * log persists in the ramoops region.
 *
 * The ramoops region at the top of DRAM survives a SoC watchdog reset
 * (DRAM stays powered), after which U-Boot parks the records on eMMC
 * and performs a full recovery power cycle (the "pstore shuttle").
 * Without this hook a panicked kernel just parks until the EC watchdog
 * acts: on pre-2.4.0 EC firmware the EC watchdog action is a full
 * power cycle, destroying DRAM and the records; 2.4.0+ warm-resets
 * first - this hook makes the reset immediate and firmware-independent.
 *
 * The panic notifier arms the SoC watchdog with a ~6 s interval using
 * a few raw MMIO writes - nothing here can sleep, lock or allocate, so
 * it is safe in panic context. The sunxi watchdog device (watchdog0)
 * is idle on Wiren Board systems: userspace feeds the EC watchdog
 * (watchdog1) instead.
 *
 * Why ~6 s instead of the 0.5 s hardware minimum: the panic notifiers
 * run before kmsg_dump writes the ramoops records and before the
 * serial console drains the backtrace, so a 0.5 s fuse could fire
 * before the dump completes. An operator-configured panic_timeout
 * shorter than 6 s intentionally wins over this hook.
 *
 * The hook arms itself only if a ramoops region actually registered:
 * on systems booted by an older U-Boot (no injected ramoops node)
 * panic behaviour is intentionally left unchanged, so updating the
 * kernel alone changes nothing (no-lockstep updates).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/panic_notifier.h>

/*
 * Register layouts and arming sequence mirror the restart handler in
 * drivers/watchdog/sunxi_wdt.c (sun4i_wdt_reg / sun6i_wdt_reg), except
 * for the interval: the restart handler uses the minimal 0.5 s, we set
 * 6 s, which has an exact hardware encoding (wdt_timeout_map[6] = 0x6).
 * Neither WB variant uses a key value (that is sun20i/sun55i only).
 */
#define WDT_CTRL_RELOAD		((1 << 0) | (0x0a57 << 1))
#define WDT_MODE_EN		(1 << 0)
#define WDT_TIMEOUT_MASK	0x0F
#define WDT_INTV_6S		0x6	/* wdt_timeout_map[6] in sunxi_wdt.c */

struct wb_wdt_variant {
	u8 ctrl;		/* counter restart register */
	u8 cfg;			/* reset configuration register */
	u8 mode;		/* enable + interval register */
	u8 timeout_shift;
	u8 reset_mask;
	u8 reset_val;		/* whole-system reset */
};

/* H616/T507: watchdog in the timer block, dedicated CFG register */
static const struct wb_wdt_variant sun6i_wdt_variant = {
	.ctrl = 0x10,
	.cfg = 0x14,
	.mode = 0x18,
	.timeout_shift = 4,
	.reset_mask = 0x03,
	.reset_val = 0x01,
};

/* R40/A40i: CFG and MODE share one register (offset 0x04) */
static const struct wb_wdt_variant sun4i_wdt_variant = {
	.ctrl = 0x00,
	.cfg = 0x04,
	.mode = 0x04,
	.timeout_shift = 3,
	.reset_mask = 0x02,
	.reset_val = 0x02,
};

static const struct of_device_id wb_wdt_matches[] = {
	{ .compatible = "allwinner,sun6i-a31-wdt", .data = &sun6i_wdt_variant },
	{ .compatible = "allwinner,sun4i-a10-wdt", .data = &sun4i_wdt_variant },
	{ /* sentinel */ }
};

static void __iomem *wb_wdt_regs;
static const struct wb_wdt_variant *wb_wdt_variant;

static int wb_ramoops_panic_reset(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	const struct wb_wdt_variant *v = wb_wdt_variant;
	u32 val;

	/* Set whole-system reset function */
	val = readl(wb_wdt_regs + v->cfg);
	val &= ~(u32)v->reset_mask;
	val |= v->reset_val;
	writel(val, wb_wdt_regs + v->cfg);

	/*
	 * Set the ~6 s interval and enable. On sun4i CFG and MODE are
	 * the same register, so this read-modify-write must (and does)
	 * preserve the reset bit written above.
	 */
	val = readl(wb_wdt_regs + v->mode);
	val &= ~((u32)WDT_TIMEOUT_MASK << v->timeout_shift);
	val |= WDT_INTV_6S << v->timeout_shift;
	val |= WDT_MODE_EN;
	writel(val, wb_wdt_regs + v->mode);

	/* Restart the counter so the full interval runs from now */
	writel(WDT_CTRL_RELOAD, wb_wdt_regs + v->ctrl);

	pr_emerg("SoC watchdog armed, warm reset in ~6 s\n");

	return NOTIFY_DONE;
}

static struct notifier_block wb_ramoops_panic_nb = {
	.notifier_call = wb_ramoops_panic_reset,
	/* run late: let other notifiers log first, the dump is on panic anyway */
	.priority = INT_MIN + 1,
};

static int __init wb_ramoops_panic_reset_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;

	if (!of_machine_is_compatible("allwinner,sun50i-h616") &&
	    !of_machine_is_compatible("allwinner,sun8i-r40"))
		return 0;

	/*
	 * Arm only when a ramoops region exists (injected into
	 * /reserved-memory by the Wiren Board U-Boot DT fixup).
	 */
	np = of_find_compatible_node(NULL, NULL, "ramoops");
	if (!np)
		return 0;
	of_node_put(np);

	np = of_find_matching_node_and_match(NULL, wb_wdt_matches, &match);
	if (!np) {
		pr_warn("no SoC watchdog node found, not armed\n");
		return 0;
	}

	wb_wdt_regs = of_iomap(np, 0);
	of_node_put(np);
	if (!wb_wdt_regs)
		return -ENOMEM;

	wb_wdt_variant = match->data;

	atomic_notifier_chain_register(&panic_notifier_list,
				       &wb_ramoops_panic_nb);
	pr_info("armed (ramoops present)\n");

	return 0;
}
late_initcall(wb_ramoops_panic_reset_init);
