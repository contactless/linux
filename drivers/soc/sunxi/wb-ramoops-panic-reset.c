// SPDX-License-Identifier: GPL-2.0
/*
 * Wiren Board 7 (Allwinner R40/A40i) and Wiren Board 8 (Allwinner
 * T507/H616): warm-reset the SoC shortly after a panic so the panic
 * log persists in the ramoops region.
 *
 * The ramoops region (a fixed low DRAM address, 0x43800000, carried by
 * the reserved-memory node in the board DT) survives a SoC watchdog
 * reset (DRAM stays powered), after which U-Boot parks the records on
 * eMMC and performs a full recovery power cycle (the "pstore shuttle").
 * A fixed address also lets a kernel-only update capture logs: the
 * region survives the warm reset and this same kernel harvests it on
 * the next boot even under an old U-Boot with no shuttle.
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
 * kernel alone changes nothing (no-lockstep updates). It also declines
 * to re-arm when it detects it just warm-reset from a panic (see the
 * loop brake below), leaving a repeat panic to the EC watchdog.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/panic_notifier.h>
#include <linux/workqueue.h>

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

/*
 * Loop brake. A kernel that panics on every boot would otherwise
 * warm-reset forever. There is no hardware "reset cause" latch on these
 * SoCs (the watchdog status register is interrupt-mode only and clears
 * on reset), but the RTC block has general-purpose registers documented
 * "for storing power-off information": one of them survives a warm reset
 * and is wiped by the EC's hard 5 V cycle, which is exactly the reset-
 * reason signal we need. The panic handler stamps it before arming;
 * if a boot comes up still stamped and we have not yet cleared it after
 * a healthy uptime, we decline to arm again and let the EC watchdog
 * escalate to a hard power cycle (which wipes the stamp and breaks the
 * loop). H616 only for now; the R40 RTC's wipe-on-hard-cycle behaviour
 * is not yet verified, so R40 keeps arming unconditionally.
 */
#define WB_RTC_BREADCRUMB_PHYS	0x0700010c	/* RTC GP data reg 3 */
#define WB_RTC_BREADCRUMB_MAGIC	0x50414e31	/* "PAN1" */

static void __iomem *wb_rtc_breadcrumb;
static bool wb_reset_from_panic;

static void wb_breadcrumb_clear_fn(struct work_struct *work)
{
	/* Healthy uptime reached: allow the next panic to warm-reset again. */
	bool was_held = wb_reset_from_panic;

	if (wb_rtc_breadcrumb)
		writel(0, wb_rtc_breadcrumb);
	wb_reset_from_panic = false;
	if (was_held)
		pr_info("healthy, panic warm-reset re-armed\n");
}
static DECLARE_DELAYED_WORK(wb_breadcrumb_clear, wb_breadcrumb_clear_fn);

static int wb_ramoops_panic_reset(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	const struct wb_wdt_variant *v = wb_wdt_variant;
	u32 val;

	/*
	 * If this boot itself came back from a panic-armed warm reset and
	 * has not yet proven healthy, do not arm again: let the EC watchdog
	 * hard-cycle the board so a crash loop cannot spin on warm resets.
	 */
	if (wb_reset_from_panic) {
		pr_emerg("panic again after a warm reset; not re-arming, leaving it to the EC watchdog\n");
		return NOTIFY_DONE;
	}

	/* Stamp the reset-reason breadcrumb before triggering the reset. */
	if (wb_rtc_breadcrumb)
		writel(WB_RTC_BREADCRUMB_MAGIC, wb_rtc_breadcrumb);

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

	/*
	 * Reset-reason breadcrumb (H616 only for now). If this boot came up
	 * with the panic stamp still set, we just warm-reset from a panic:
	 * hold off arming until a healthy uptime clears it.
	 */
	if (of_machine_is_compatible("allwinner,sun50i-h616")) {
		wb_rtc_breadcrumb = ioremap(WB_RTC_BREADCRUMB_PHYS, 4);
		if (wb_rtc_breadcrumb &&
		    readl(wb_rtc_breadcrumb) == WB_RTC_BREADCRUMB_MAGIC) {
			wb_reset_from_panic = true;
			pr_info("came back from a panic warm reset; holding off re-arm\n");
		}
	}
	/*
	 * Clear the stamp once we have stayed up long enough to count as a
	 * healthy session (runs whether or not the stamp was set, so a plain
	 * boot re-arms immediately and a post-panic boot re-arms after the
	 * delay). 60 s comfortably exceeds pstore harvest.
	 */
	schedule_delayed_work(&wb_breadcrumb_clear, msecs_to_jiffies(60000));

	atomic_notifier_chain_register(&panic_notifier_list,
				       &wb_ramoops_panic_nb);
	pr_info("armed (ramoops present)\n");

	return 0;
}
late_initcall(wb_ramoops_panic_reset_init);
