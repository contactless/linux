// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec-power.c - Wiren Board Embedded Controller Power Supply Battery driver
 *
 * Copyright (c) 2024 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/mfd/wbec.h>

struct wbec_battery {
	struct regmap *regmap;
	struct power_supply *batt;
	struct device *dev;
};

union wbec_pwr_status_reg {
	struct {
		/* Status bits */
		uint16_t powered_from_wbmz : 1;		/* 0xC0 */
		uint16_t wbmz_stepup_enabled : 1;	/* 0xC0 */
		uint16_t wbmz_charging_enabled : 1;	/* 0xC0 */
		uint16_t wbmz_battery_present : 1;	/* 0xC0 */
		uint16_t wbmz_supercap_present : 1;	/* 0xC0 */
		uint16_t wbmz_is_charging : 1;		/* 0xC0 */
		uint16_t wbmz_is_dead : 1;		/* 0xC0 */
		uint16_t wbmz_is_inserted : 1;		/* 0xC0 */

		/* Params: constant values depending on battery/supercap */
		uint16_t wbmz_full_design_capacity;	/* 0xC1 */
		uint16_t wbmz_voltage_min_design;	/* 0xC2 */
		uint16_t wbmz_voltage_max_design;	/* 0xC3 */
		uint16_t wbmz_constant_charge_current;	/* 0xC4 */

		/* Status */
		uint16_t wbmz_battery_voltage;		/* 0xC5 */
		uint16_t wbmz_charging_current;		/* 0xC6 */
		uint16_t wbmz_discharging_current;	/* 0xC7 */
		uint16_t wbmz_capacity_percent;		/* 0xC8 */
		int16_t wbmz_temperature;		/* 0xC9 */
	} __packed;
	uint16_t regs[10];
};

static int wbec_battery_get_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct wbec_battery *wbec_battery = power_supply_get_drvdata(psy);
	union wbec_pwr_status_reg regs;
	int ret = 0;


	/* read all battery status reg in one transaction */
	ret = regmap_bulk_read(wbec_battery->regmap, WBEC_REG_PWR_STATUS, regs.regs,
			 ARRAY_SIZE(regs.regs));

	if (ret) {
		dev_err(wbec_battery->dev, "failed to read battery status: %d\n", ret);
		return ret;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!(regs.wbmz_battery_present || regs.wbmz_supercap_present);
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!((regs.wbmz_battery_present || regs.wbmz_supercap_present) &&
				  regs.wbmz_is_inserted && regs.wbmz_stepup_enabled);
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (regs.wbmz_is_charging) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}

		/* 10 mA threshold for discharging detection */
		if (regs.wbmz_discharging_current > 10) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		} else {
			if (regs.wbmz_capacity_percent < 100)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_FULL;
		}
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		if (regs.wbmz_is_inserted == 0)
			val->intval = POWER_SUPPLY_HEALTH_NO_BATTERY;
		else if (regs.wbmz_is_dead)
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		/* Charge in mA*h ony available for WBMZ6-BATTERY */
		if (regs.wbmz_battery_present)
			val->intval = regs.wbmz_full_design_capacity * 1000;
		else
			val->intval = 0;
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		/* Energy in mWh only available for WBMZ6-SUPERCAP */
		if (regs.wbmz_supercap_present)
			val->intval = regs.wbmz_full_design_capacity * 1000;
		else
			val->intval = 0;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = regs.wbmz_constant_charge_current;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (regs.wbmz_is_charging)
			val->intval = regs.wbmz_charging_current;
		else
			val->intval = -regs.wbmz_discharging_current;

		/* WBEC gives mA but Power Supply framework gives uA */
		val->intval *= 1000;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = regs.wbmz_capacity_percent;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = regs.wbmz_voltage_max_design * 1000;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = regs.wbmz_voltage_min_design * 1000;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* WBEC gives mV but Power Supply framework gives uV */
		val->intval = regs.wbmz_battery_voltage * 1000;
		break;

	case POWER_SUPPLY_PROP_TEMP:
		/* WBEC gives 0.1 degree Celsius and Power Supply framework too */
		val->intval = regs.wbmz_temperature;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int wbec_battery_set_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	return -EINVAL;
}

static enum power_supply_property wbec_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, /* for WBMZ6-BATTERY */
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN, /* for WBMZ6-SUPERCAP */
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static int wbec_battery_prop_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	return 0;
}

static const struct power_supply_desc wbec_battery_desc = {
	.name = "wbec-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = wbec_battery_props,
	.num_properties = ARRAY_SIZE(wbec_battery_props),
	.property_is_writeable = wbec_battery_prop_writeable,
	.get_property = wbec_battery_get_prop,
	.set_property = wbec_battery_set_prop,
};

static int wbec_battery_probe(struct platform_device *pdev)
{
	struct wbec_battery *wbec_battery;
	struct power_supply_config psy_cfg = {};
	struct device *dev = &pdev->dev;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	wbec_battery = devm_kzalloc(dev, sizeof(*wbec_battery), GFP_KERNEL);
	if (!wbec_battery)
		return -ENOMEM;

	wbec_battery->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	platform_set_drvdata(pdev, wbec_battery);

	psy_cfg.drv_data = wbec_battery;
	psy_cfg.of_node = pdev->dev.of_node;

	wbec_battery->batt = devm_power_supply_register(&pdev->dev,
						       &wbec_battery_desc,
						       &psy_cfg);
	if (IS_ERR(wbec_battery->batt)) {
		dev_err(&pdev->dev, "failed to register power supply: %ld\n",
			PTR_ERR(wbec_battery->batt));
		return PTR_ERR(wbec_battery->batt);
	}

	dev_info(&pdev->dev, "registered battery\n");

	return 0;
}

static const struct of_device_id wbec_battery_of_match[] = {
	{ .compatible = "wirenboard,wbec-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, wbec_battery_of_match);

static struct platform_driver wbec_battery_driver = {
	.probe    = wbec_battery_probe,
	.driver   = {
		.name  = "wbec-battery",
		.of_match_table = wbec_battery_of_match,
	},
};

module_platform_driver(wbec_battery_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board Embedded Controller Battery driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-battery");
