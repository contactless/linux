// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      sunxi Watchdog Driver
 *
 *      Copyright (c) 2013 Carlo Caione
 *                    2012 Henrik Nordstrom
 *
 *      Based on xen_wdt.c
 *      (c) Copyright 2010 Novell, Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/watchdog.h>

#define WDT_MAX_TIMEOUT         16
#define WDT_MIN_TIMEOUT         1
#define WDT_TIMEOUT_MASK        0x0F

#define WDT_CTRL_RELOAD         ((1 << 0) | (0x0a57 << 1))

#define WDT_MODE_EN             (1 << 0)

#define DRV_NAME		"sunxi-wdt"
#define DRV_VERSION		"1.0"

static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;
static unsigned int panic_wdt_timeout = CONFIG_SUNXI_WATCHDOG_PANIC_TIMEOUT;

/*
 * This structure stores the register offsets for different variants
 * of Allwinner's watchdog hardware.
 */
struct sunxi_wdt_reg {
	u8 wdt_ctrl;
	u8 wdt_cfg;
	u8 wdt_mode;
	u8 wdt_timeout_shift;
	u8 wdt_reset_mask;
	u8 wdt_reset_val;
	u32 wdt_key_val;
};

struct sunxi_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	const struct sunxi_wdt_reg *wdt_regs;
};

/*
 * wdt_timeout_map maps the watchdog timer interval value in seconds to
 * the value of the register WDT_MODE at bits .wdt_timeout_shift ~ +3
 *
 * [timeout seconds] = register value
 *
 */

static const int wdt_timeout_map[] = {
	[1] = 0x1,  /* 1s  */
	[2] = 0x2,  /* 2s  */
	[3] = 0x3,  /* 3s  */
	[4] = 0x4,  /* 4s  */
	[5] = 0x5,  /* 5s  */
	[6] = 0x6,  /* 6s  */
	[8] = 0x7,  /* 8s  */
	[10] = 0x8, /* 10s */
	[12] = 0x9, /* 12s */
	[14] = 0xA, /* 14s */
	[16] = 0xB, /* 16s */
};


static int sunxi_wdt_restart(struct watchdog_device *wdt_dev,
			     unsigned long action, void *data)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	u32 val;

	/* Set system reset function */
	val = readl(wdt_base + regs->wdt_cfg);
	val &= ~(regs->wdt_reset_mask);
	val |= regs->wdt_reset_val;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_cfg);

	/* Set lowest timeout and enable watchdog */
	val = readl(wdt_base + regs->wdt_mode);
	val &= ~(WDT_TIMEOUT_MASK << regs->wdt_timeout_shift);
	val |= WDT_MODE_EN;
	val |= regs->wdt_key_val;
	writel(val, wdt_base + regs->wdt_mode);

	/*
	 * Restart the watchdog. The default (and lowest) interval
	 * value for the watchdog is 0.5s.
	 */
	writel(WDT_CTRL_RELOAD, wdt_base + regs->wdt_ctrl);

	while (1) {
		mdelay(5);
		val = readl(wdt_base + regs->wdt_mode);
		val |= WDT_MODE_EN;
		val |= regs->wdt_key_val;
		writel(val, wdt_base + regs->wdt_mode);
	}
	return 0;
}

static int sunxi_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;

	writel(WDT_CTRL_RELOAD, wdt_base + regs->wdt_ctrl);

	return 0;
}

static int sunxi_wdt_set_timeout(struct watchdog_device *wdt_dev,
		unsigned int timeout)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	u32 reg;

	if (wdt_timeout_map[timeout] == 0)
		timeout++;

	sunxi_wdt->wdt_dev.timeout = timeout;

	reg = readl(wdt_base + regs->wdt_mode);
	reg &= ~(WDT_TIMEOUT_MASK << regs->wdt_timeout_shift);
	reg |= wdt_timeout_map[timeout] << regs->wdt_timeout_shift;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_mode);

	sunxi_wdt_ping(wdt_dev);

	return 0;
}

static int sunxi_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;

	writel(regs->wdt_key_val, wdt_base + regs->wdt_mode);

	return 0;
}

static int sunxi_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct sunxi_wdt_dev *sunxi_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = sunxi_wdt->wdt_base;
	const struct sunxi_wdt_reg *regs = sunxi_wdt->wdt_regs;
	int ret;

	ret = sunxi_wdt_set_timeout(&sunxi_wdt->wdt_dev,
			sunxi_wdt->wdt_dev.timeout);
	if (ret < 0)
		return ret;

	/* Set system reset function */
	reg = readl(wdt_base + regs->wdt_cfg);
	reg &= ~(regs->wdt_reset_mask);
	reg |= regs->wdt_reset_val;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_cfg);

	/* Enable watchdog */
	reg = readl(wdt_base + regs->wdt_mode);
	reg |= WDT_MODE_EN;
	reg |= regs->wdt_key_val;
	writel(reg, wdt_base + regs->wdt_mode);

	return 0;
}

static const struct watchdog_info sunxi_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops sunxi_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= sunxi_wdt_start,
	.stop		= sunxi_wdt_stop,
	.ping		= sunxi_wdt_ping,
	.set_timeout	= sunxi_wdt_set_timeout,
	.restart	= sunxi_wdt_restart,
};

static const struct sunxi_wdt_reg sun4i_wdt_reg = {
	.wdt_ctrl = 0x00,
	.wdt_cfg = 0x04,
	.wdt_mode = 0x04,
	.wdt_timeout_shift = 3,
	.wdt_reset_mask = 0x02,
	.wdt_reset_val = 0x02,
};

static const struct sunxi_wdt_reg sun6i_wdt_reg = {
	.wdt_ctrl = 0x10,
	.wdt_cfg = 0x14,
	.wdt_mode = 0x18,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
};

static const struct sunxi_wdt_reg sun20i_wdt_reg = {
	.wdt_ctrl = 0x10,
	.wdt_cfg = 0x14,
	.wdt_mode = 0x18,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
	.wdt_key_val = 0x16aa0000,
};

static const struct sunxi_wdt_reg sun55i_wdt_reg = {
	.wdt_ctrl = 0x0c,
	.wdt_cfg = 0x10,
	.wdt_mode = 0x14,
	.wdt_timeout_shift = 4,
	.wdt_reset_mask = 0x03,
	.wdt_reset_val = 0x01,
	.wdt_key_val = 0x16aa0000,
};

static const struct of_device_id sunxi_wdt_dt_ids[] = {
	{ .compatible = "allwinner,sun4i-a10-wdt", .data = &sun4i_wdt_reg },
	{ .compatible = "allwinner,sun6i-a31-wdt", .data = &sun6i_wdt_reg },
	{ .compatible = "allwinner,sun20i-d1-wdt", .data = &sun20i_wdt_reg },
	{ .compatible = "allwinner,sun55i-a523-wdt", .data = &sun55i_wdt_reg },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_wdt_dt_ids);

/*
 * Panic handling: when the panic_wdt_timeout module parameter is nonzero, arm
 * the watchdog from a panic notifier so a panicked machine resets itself
 * shortly after the panic, even with the kernel panic timeout set to 0.
 *
 * The notifier only ARMS the watchdog and returns: panic notifiers run
 * before kmsg_dump(), so the panic record must still be written (e.g. to
 * pstore/ramoops) before the reset fires. The watchdog resets the SoC
 * without cutting power, leaving DRAM - and thus ramoops records - intact.
 *
 * All register accesses in the ops used here are plain MMIO with no
 * locking, which is safe in panic context (secondary CPUs are stopped).
 */
static struct sunxi_wdt_dev *sunxi_wdt_panic_instance;

static int sunxi_wdt_panic_notify(struct notifier_block *nb,
				  unsigned long code, void *unused)
{
	struct sunxi_wdt_dev *sunxi_wdt = READ_ONCE(sunxi_wdt_panic_instance);
	unsigned int t = READ_ONCE(panic_wdt_timeout);

	if (!sunxi_wdt || !t)
		return NOTIFY_DONE;

	t = clamp(t, (unsigned int)WDT_MIN_TIMEOUT,
		  (unsigned int)WDT_MAX_TIMEOUT);

	pr_emerg("%s: arming watchdog, reset in %u s\n", DRV_NAME, t);

	sunxi_wdt->wdt_dev.timeout = t;
	sunxi_wdt_start(&sunxi_wdt->wdt_dev);

	return NOTIFY_DONE;
}

static struct notifier_block sunxi_wdt_panic_nb = {
	.notifier_call = sunxi_wdt_panic_notify,
	/* run early: arming a recovery watchdog must not be skipped
	 * because another notifier crashed before us */
	.priority = INT_MAX,
};

static void sunxi_wdt_unregister_panic_notifier(void *data)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &sunxi_wdt_panic_nb);
	WRITE_ONCE(sunxi_wdt_panic_instance, NULL);
}

static int sunxi_wdt_register_panic_notifier(struct device *dev,
					     struct sunxi_wdt_dev *sunxi_wdt)
{
	if (cmpxchg(&sunxi_wdt_panic_instance, NULL, sunxi_wdt))
		return 0;	/* already owned by another instance */

	atomic_notifier_chain_register(&panic_notifier_list,
				       &sunxi_wdt_panic_nb);

	return devm_add_action_or_reset(dev,
					sunxi_wdt_unregister_panic_notifier,
					NULL);
}

static int sunxi_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sunxi_wdt_dev *sunxi_wdt;
	int err;

	sunxi_wdt = devm_kzalloc(dev, sizeof(*sunxi_wdt), GFP_KERNEL);
	if (!sunxi_wdt)
		return -ENOMEM;

	sunxi_wdt->wdt_regs = of_device_get_match_data(dev);
	if (!sunxi_wdt->wdt_regs)
		return -ENODEV;

	sunxi_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sunxi_wdt->wdt_base))
		return PTR_ERR(sunxi_wdt->wdt_base);

	sunxi_wdt->wdt_dev.info = &sunxi_wdt_info;
	sunxi_wdt->wdt_dev.ops = &sunxi_wdt_ops;
	sunxi_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	sunxi_wdt->wdt_dev.max_timeout = WDT_MAX_TIMEOUT;
	sunxi_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	sunxi_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&sunxi_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&sunxi_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&sunxi_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&sunxi_wdt->wdt_dev, sunxi_wdt);

	sunxi_wdt_stop(&sunxi_wdt->wdt_dev);

	watchdog_stop_on_reboot(&sunxi_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &sunxi_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	err = sunxi_wdt_register_panic_notifier(dev, sunxi_wdt);
	if (err)
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)",
		 sunxi_wdt->wdt_dev.timeout, nowayout);

	return 0;
}

static struct platform_driver sunxi_wdt_driver = {
	.probe		= sunxi_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= sunxi_wdt_dt_ids,
	},
};

module_platform_driver(sunxi_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(panic_wdt_timeout, uint, 0644);
MODULE_PARM_DESC(panic_wdt_timeout, "Arm watchdog on kernel panic with this timeout in seconds (default="
		 __MODULE_STRING(CONFIG_SUNXI_WATCHDOG_PANIC_TIMEOUT) ", 0 to disable)");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started "
		"(default=" __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Carlo Caione <carlo.caione@gmail.com>");
MODULE_AUTHOR("Henrik Nordstrom <henrik@henriknordstrom.net>");
MODULE_DESCRIPTION("sunxi WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
