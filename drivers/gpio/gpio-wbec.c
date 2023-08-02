// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpio-wbec.c - Wiren Board Embedded Controller GPIO driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/gpio/driver.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/gpio/regmap.h>
#include <linux/mfd/wbec.h>

#define WBEC_GPIO_PER_REG		16

static int wbec_gpio_probe(struct platform_device *pdev)
{
	struct gpio_regmap_config config = {};
	struct wbec *wbec;
	struct device *dev = &pdev->dev;
	int ret, ngpios;

	if (!pdev->dev.parent)
		return -ENODEV;

	wbec = dev_get_drvdata(pdev->dev.parent);

	ret = device_property_read_u32(dev, "ngpios", &ngpios);
	if (ret) {
		dev_err(dev, "No ngpios property in device tree");
		return -EINVAL;
	}

	config.ngpio = ngpios;
	config.regmap = wbec->regmap;
	config.parent = dev;
	config.reg_dat_base = WBEC_REG_GPIO;
	config.reg_set_base = WBEC_REG_GPIO;
	config.ngpio_per_reg = WBEC_GPIO_PER_REG;
	config.reg_stride = 1;

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(&pdev->dev, &config));
}

static const struct of_device_id wbec_gpio_of_match[] = {
	{ .compatible = "wirenboard,wbec-gpio" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_gpio_of_match);

static struct platform_driver wbec_gpio_driver = {
	.driver = {
		.name = "wbec-gpio",
		.of_match_table = wbec_gpio_of_match,
	},
	.probe = wbec_gpio_probe,
};
module_platform_driver(wbec_gpio_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board 7 Embedded Controller GPIO driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-gpio");
