// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec-power.c - Wiren Board Embedded Controller Power Supply driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <linux/mfd/wbec.h>

struct wbec_power {
	struct regmap *regmap;

	struct power_supply *charger;
};

static int wbec_power_get_property(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val)
{
	struct wbec_power *wbec_power = power_supply_get_drvdata(psy);
	int ret;

	if (psp == POWER_SUPPLY_PROP_ONLINE) {
		ret = regmap_test_bits(wbec_power->regmap,
			WBEC_REG_PWR_STATUS,
			WBEC_REG_PWR_STATUS_POWERED_FROM_WBMZ_MSK);
		if (ret < 0)
			return ret;
		// If bit is set, it means that WB is powered
		// from battery and main power is off
		val->intval = ret ? 0 : 1;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int wbec_power_set_property(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val)
{
	return -EINVAL;
}

static int wbec_power_property_is_writeable(struct power_supply *psy,
					      enum power_supply_property psp)
{
	return 0;
}

static const enum power_supply_property wbec_power_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc wbec_power_desc = {
	.name = "wbec-power",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = wbec_power_properties,
	.num_properties = ARRAY_SIZE(wbec_power_properties),
	.property_is_writeable = wbec_power_property_is_writeable,
	.get_property = wbec_power_get_property,
	.set_property = wbec_power_set_property,
};

static int wbec_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wbec *wbec;
	struct power_supply_config psy_cfg = {};
	struct wbec_power *wbec_power;
	int ret;

	if (!dev->parent)
		return -ENODEV;

	wbec_power = devm_kzalloc(dev, sizeof(*wbec_power), GFP_KERNEL);
	if (!wbec_power)
		return -ENOMEM;

	wbec = dev_get_drvdata(dev->parent);
	wbec_power->regmap = wbec->regmap;

	psy_cfg.of_node = dev->of_node;
	psy_cfg.drv_data = wbec_power;

	wbec_power->charger = devm_power_supply_register(dev, &wbec_power_desc,
							   &psy_cfg);
	if (IS_ERR(wbec_power->charger)) {
		ret = PTR_ERR(wbec_power->charger);
		dev_err(dev, "Failed to register power supply: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, wbec_power);

	return 0;
}

static const struct of_device_id wbec_power_of_match[] = {
	{ .compatible = "wirenboard,wbec-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, wbec_power_of_match);

static struct platform_driver wbec_power_driver = {
	.probe = wbec_power_probe,
	.driver = {
		.name = "wbec-power",
		.of_match_table = wbec_power_of_match,
	},
};

module_platform_driver(wbec_power_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board Embedded Controller PWR driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-power");
