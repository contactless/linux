// SPDX-License-Identifier: GPL-2.0-only
/*
 * wdt-wbec.c - Wiren Board Embedded Controller watchdog driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/mfd/wbec.h>


#define WBEC_WDT_MIN_TIMEOUT		1
#define WBEC_WDT_MAX_TIMEOUT		600
#define WBEC_WDG_DEFAULT_TIMEOUT	60
#define WBEC_WDG_STOP_TIMEOUT		300
#define WBEC_RESET_PROTECTION_MS	300

struct wbec_watchdog {
	struct device *dev;
	struct regmap *regmap;
	struct watchdog_device wdtdev;
};

static int wbec_wdt_start(struct watchdog_device *wdd)
{
	// Nothing to do here, watchdog always running
	return 0;
}

static int wbec_wdt_ping(struct watchdog_device *wdd)
{
	struct wbec_watchdog *wdt = watchdog_get_drvdata(wdd);
	int ret;

	ret = regmap_update_bits(wdt->regmap,
				 WBEC_REG_WDT_STATUS,
				 WBEC_REG_WDT_STATUS_RESET_MSK,
				 WBEC_REG_WDT_STATUS_RESET_MSK);
	if (ret)
		dev_err(wdt->dev, "Failed to ping the watchdog (err = %d)\n",
			ret);

	return ret;
}

static int wbec_wdt_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct wbec_watchdog *wdt = watchdog_get_drvdata(wdd);
	int ret;

	if (timeout > WBEC_WDT_MAX_TIMEOUT) {
		timeout = WBEC_WDT_MAX_TIMEOUT;
		wdd->timeout = WBEC_WDT_MAX_TIMEOUT;
	}

	ret = regmap_write(wdt->regmap, WBEC_REG_WDT_TIMEOUT, timeout);
	if (ret)
		dev_err(wdt->dev, "Failed to set watchdog timeout (err = %d)\n",
			ret);

	return ret;
}

static int wbec_wdt_stop(struct watchdog_device *wdd)
{
	struct wbec_watchdog *wdt = watchdog_get_drvdata(wdd);
	int ret;

	dev_info(wdt->dev,
		"stop watchdog, but actually EC watchdog is always running, timeout set to %d seconds\n",
		WBEC_WDG_STOP_TIMEOUT);

	ret = wbec_wdt_set_timeout(wdd, WBEC_WDG_STOP_TIMEOUT);
	if (ret)
		return ret;

	// Reset watchdog after timeout updated
	return wbec_wdt_ping(wdd);
}

static const struct watchdog_info wbec_watchdog_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity = "WBEC WDT",
};

static const struct watchdog_ops wbec_watchdog_ops = {
	.owner = THIS_MODULE,
	.start = wbec_wdt_start,
	.stop = wbec_wdt_stop,
	.ping = wbec_wdt_ping,
	.set_timeout = wbec_wdt_set_timeout,
};

static int wbec_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int timeout;
	struct wbec *wbec;
	struct wbec_watchdog *wdt;
	int ret;

	if (!pdev->dev.parent)
		return -ENODEV;

	wbec = dev_get_drvdata(dev->parent);
	if (!wbec)
		return -EINVAL;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->dev = dev;
	wdt->regmap = wbec->regmap;

	wdt->wdtdev.info = &wbec_watchdog_info;
	wdt->wdtdev.ops = &wbec_watchdog_ops;
	wdt->wdtdev.min_timeout = WBEC_WDT_MIN_TIMEOUT;
	wdt->wdtdev.max_timeout = WBEC_WDT_MAX_TIMEOUT;
	wdt->wdtdev.min_hw_heartbeat_ms = WBEC_RESET_PROTECTION_MS;
	wdt->wdtdev.timeout = WBEC_WDG_DEFAULT_TIMEOUT;
	wdt->wdtdev.status = WATCHDOG_NOWAYOUT_INIT_STATUS;
	wdt->wdtdev.parent = dev;

	watchdog_set_restart_priority(&wdt->wdtdev, 128);

	watchdog_set_drvdata(&wdt->wdtdev, wdt);
	dev_set_drvdata(dev, &wdt->wdtdev);

	ret = regmap_read(wdt->regmap, WBEC_REG_WDT_TIMEOUT, &timeout);
	if (ret < 0) {
		dev_err(wdt->dev, "Failed to read watchdog timeot from wbec");
		return -ECOMM;
	}
	wdt->wdtdev.timeout = timeout;

	/* Set timeout from DT value if available */
	watchdog_init_timeout(&wdt->wdtdev, 0, dev);

	return devm_watchdog_register_device(dev, &wdt->wdtdev);
}

static const struct of_device_id wbec_wdt_of_match[] = {
	{ .compatible = "wirenboard,wbec-watchdog" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_wdt_of_match);

static struct platform_driver wbec_wdt_driver = {
	.probe = wbec_wdt_probe,
	.driver = {
		.name = "wbec-watchdog",
		.of_match_table = wbec_wdt_of_match,
	},
};
module_platform_driver(wbec_wdt_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board 7 Embedded Controller Watchdog driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-watchdog");
