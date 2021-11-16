// SPDX-License-Identifier: GPL-2.0
/*
 * Generic EDLC battery driver.
 * Copyright (c) 2021 Evgeny Boger <boger@wirenboard.com>
 *
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/math64.h>

/* EDLC mostly behaves as an ideal capacitor, at least when
 * it comes to the scope of this driver.
 * Energy, capacity percentage (i.e. SoC) and current all can
 * be derived from cell voltage.
 *
 * The energy stored in the capacitor (W) is calculated by equation
 * W = (1/2) C V^2, where C is the capacitance (in Farads), and
 * V is the capacitor voltage.
 *
 * As the maximum design energy (in W*h) is somewhat widely adopted
 * parameter of the battery pack, this driver uses it to calculate
 * current_now and energy_now.
 * The energy stored at the given moment, or W_{now}, can be
 * calculated as follows:
 * W_{now} = (1/2) C V_{now}^2, W_{max} = (1/2) C V_{max}^2
 * therefore, W_{now} = W_{max} (V_{now} / V_{max})^2
*
* Capacity percent is basically reported as the ratio of the current available
* energy and the maximum available energy. Available energy can be slightly
* smaller then the full stored energy because some energy is unrecoverable
* due cell voltage becoming too small.
* So capacity (%) is (W_{now} - W_{min}) / (W_{max} - W_{min}), which become
* (V - V_{max}) (V + V_{max}) / (V_{max} - V_{min}) / (V_{max} + V_{min})
*
* Finally, the charging or discharging current can also be derived from
* voltage measurement. The charging/discharging current i is proportional
* to the rate of voltage change, and is given by the equation i = C dV / dt.
* As before, the capacitance value is calcualted from maximum stored energy
* and maximum cell voltage.
* W_{max} = (1/2) C V_{max}^2
* C = 2 W_{max} / V_{max}^2
* i = 2 (W_{max} / V_{max}^2)  (dV / dt).
*
*
* Depending of the battery information provided, this driver reports different
* set of properties:
*  * VOLTAGE_NOW, CAPACITY and STATUS  are always reported.
*  * ENERGY_NOW, CURRENT_NOW, POWER_NOW are only reported if
*    energy_full_design_uwh is provided to the driver.
*
* If voltage_max_design_uv is provided, the properies will be always correctly
* reported as maximum cell voltage is immediately known to the driver.
* Otherwise, the driver will return 100% capacity until the EDLC is fully
* charged for the first time.
*/

#define MAX_CURRENT_UPDATE_INTERVAL_MS 10000
#define MAX_CURRENT_UPDATE_V_DELTA_UV 5000
#define VOLTAGE_JITTER_THRESHOLD_UV 2000

struct edlc_battery {
	struct device *dev;
	struct iio_channel *channel;
	struct power_supply_desc desc;
	struct power_supply *battery;
	struct power_supply_battery_info info;
	struct delayed_work bat_work;

	int voltage_max_uv;
	int voltage_min_uv;

	/* the following fields are updated every several seconds or on rapid change */
	int voltage_uv;
	bool current_valid; // current and power require two voltage measurements
	int current_ua;
	int power_uw;
	bool voltage_valid;
	int status;
	ktime_t last_updated;
};


static int edlc_battery_read_voltage_uv(struct edlc_battery *bat, int* voltage_res_uv)
{
	int ret;
	int voltage_uv;

	ret = iio_read_channel_processed(bat->channel, &voltage_uv);
	if (ret) return ret;

	voltage_uv *= 1000;
	if (voltage_uv > bat->voltage_max_uv) {
		bat->voltage_max_uv = voltage_uv;
	}
	*voltage_res_uv = voltage_uv;
	return 0;
}

static int edlc_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct edlc_battery *bat = power_supply_get_drvdata(psy);
	struct power_supply_battery_info *info = &bat->info;
	int ret;
	int voltage;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = edlc_battery_read_voltage_uv(bat, &val->intval);
		return ret;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		if (info->voltage_min_design_uv < 0)
			return -ENODATA;
		val->intval = info->voltage_min_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = bat->voltage_max_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		if (info->voltage_max_design_uv < 0)
			return -ENODATA;
		val->intval = info->voltage_max_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		if (info->energy_full_design_uwh < 0)
			return -ENODATA;
		val->intval = info->energy_full_design_uwh;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_EMPTY_DESIGN:
	case POWER_SUPPLY_PROP_ENERGY_EMPTY:
		if (info->energy_full_design_uwh < 0)
			return -ENODATA;

		val->intval = bat->voltage_min_uv;
		val->intval /= 1000; // uV to mV
		val->intval *= val->intval; // mV ^2
		val->intval /= (bat->voltage_max_uv / 1000);
		val->intval *= info->energy_full_design_uwh;
		val->intval /= (bat->voltage_max_uv / 1000);
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
 		val->intval = bat->current_ua;
		if (!bat->current_valid)
			return -ENODATA;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bat->status;
		return 0;
	case POWER_SUPPLY_PROP_POWER_NOW:
		if (!bat->current_valid)
			return -ENODATA;
		val->intval = bat->power_uw;
		return 0;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (info->energy_full_design_uwh < 0)
			return -ENODATA;
		ret = edlc_battery_read_voltage_uv(bat, &val->intval);
		val->intval /= 1000; // uV to mV
		val->intval *= val->intval; // mV ^2
		val->intval /= (bat->voltage_max_uv / 1000);
		val->intval *= info->energy_full_design_uwh;
		val->intval /= (bat->voltage_max_uv / 1000);
		return ret;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = edlc_battery_read_voltage_uv(bat, &voltage);
		if (voltage <= bat->voltage_min_uv) {
			val->intval = 0;
		} else if (voltage >= bat->voltage_max_uv) {
			val->intval = 100;
		} else {
			voltage /= 1000; // uV to mV
			val->intval = (voltage + (bat->voltage_min_uv / 1000));
			val->intval *= (voltage - (bat->voltage_min_uv / 1000));
			val->intval /= ((bat->voltage_max_uv - bat->voltage_min_uv) / 1000);
			val->intval *= 100; //percent
			val->intval = DIV_ROUND_UP(val->intval, (bat->voltage_max_uv + bat->voltage_min_uv) / 1000);
		}
		return ret;
	default:
		return -EINVAL;
	};
}

static enum power_supply_property edlc_battery_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_POWER_NOW
};

static void edlc_battery_clear_cache(struct edlc_battery *bat)
{
	bat->current_valid = false;
	bat->voltage_valid = false;
}

static int edlc_battery_get_status(struct edlc_battery *bat, int delta_v)
{
	if (delta_v == 0) {
		if (bat->voltage_max_uv - bat->voltage_uv <= VOLTAGE_JITTER_THRESHOLD_UV) {
			return POWER_SUPPLY_STATUS_FULL;
		} else {
			return POWER_SUPPLY_STATUS_NOT_CHARGING;
		}
	} else if (delta_v > 0) {
		return POWER_SUPPLY_STATUS_CHARGING;
	} else {
		return POWER_SUPPLY_STATUS_DISCHARGING;
	}
}

static void eldc_battery_update_current(struct edlc_battery *bat, int delta_uv, int delta_ms)
{
	if (bat->info.energy_full_design_uwh <= 0)
		return;

	if (delta_uv == 0) { // fast path
		bat->current_ua = 0;
		bat->power_uw = 0;
	} else {
		int64_t tmp_divisor;
		int64_t tmp = 2;
		tmp *= bat->info.energy_full_design_uwh;
		tmp *= delta_uv;
		tmp *= 3600; // Wh to J

		tmp_divisor = bat->voltage_max_uv / 1000;
		tmp_divisor *= tmp_divisor;
		tmp_divisor *= delta_ms;
		// printk("tmp: %lld, td: %lld, dv: %d, dt: %d, e:%d\n", tmp, tmp_divisor, delta_uv, delta_ms, bat->info.energy_full_design_uwh);

		bat->current_ua = 1000 * (int32_t) div64_s64(tmp, tmp_divisor);
		bat->power_uw = (bat->current_ua / 1000) * (bat->voltage_uv / 1000);
	}
	bat->current_valid = true;
}

static void edlc_battery_work(struct work_struct *work)
{
	struct edlc_battery *bat;
	struct delayed_work *delayed_work;
	int ret;
	int voltage_now = 0;

	delayed_work = to_delayed_work(work);
	bat = container_of(delayed_work, struct edlc_battery, bat_work);
	ret = edlc_battery_read_voltage_uv(bat, &voltage_now);
	if (!ret) {
		ktime_t now = ktime_get();
		if (likely(bat->voltage_valid)) {
			int delta_uv = voltage_now - bat->voltage_uv;
			int delta_ms = ktime_ms_delta(now, bat->last_updated);
			if ((delta_ms >= MAX_CURRENT_UPDATE_INTERVAL_MS) || 
			    (abs(delta_uv) >= MAX_CURRENT_UPDATE_V_DELTA_UV))
			{
				int prev_status = bat->status;
				if (abs(delta_uv) < VOLTAGE_JITTER_THRESHOLD_UV)
					delta_uv = 0;

				bat->voltage_uv = voltage_now;
				bat->last_updated = now;

				eldc_battery_update_current(bat, delta_uv, delta_ms);

				bat->status = edlc_battery_get_status(bat, delta_uv);
				if (prev_status != bat->status) {
					power_supply_changed(bat->battery);
				}
			}
		} else {
			// save voltage point if called first time after probe or resume
			bat->voltage_uv = voltage_now;
			bat->last_updated = now;
			bat->voltage_valid = true;
		}
	} else {
		edlc_battery_clear_cache(bat);
	}

	schedule_delayed_work(&bat->bat_work,
			msecs_to_jiffies(500));
}

static void edlc_battery_clear_data(void *res)
{
	cancel_delayed_work_sync(res);
}

static int edlc_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct edlc_battery *bat;
	struct power_supply_config psy_cfg = {};
	struct power_supply_desc *desc;
	int ret;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = dev;
	bat->channel = devm_iio_channel_get(dev, "voltage");
	if (IS_ERR(bat->channel))
		return PTR_ERR(bat->channel);

	desc = &bat->desc;
	desc->name = "edlc-battery";
	desc->type = POWER_SUPPLY_TYPE_BATTERY;
	desc->properties = edlc_battery_properties;
	desc->num_properties = ARRAY_SIZE(edlc_battery_properties);
	desc->get_property = edlc_battery_get_property;
	psy_cfg.drv_data = bat;
	psy_cfg.of_node = dev->of_node;

	bat->battery = devm_power_supply_register(dev, desc, &psy_cfg);
	if (IS_ERR(bat->battery))
		return dev_err_probe(dev, PTR_ERR(bat->battery),
				     "Unable to register battery\n");

	ret = power_supply_get_battery_info(bat->battery, &bat->info);
	if (ret) {
		dev_dbg(dev, "Unable to get battery info: %d\n", ret);
	}

	if (bat->info.voltage_min_design_uv >= 0) {
		bat->voltage_min_uv = bat->info.voltage_min_design_uv;
	}
	if (bat->info.voltage_max_design_uv > 0) {
		bat->voltage_max_uv = bat->info.voltage_max_design_uv;
	} else {
		dev_info(dev, "Unable to get max design voltage, will "
		"report incorrect data prior to first full charge\n");
	}
	if (bat->info.energy_full_design_uwh < 0) {
		dev_info(dev, "Unable to get full design energy, only "
		"percent capacity will be reported\n");
	}

	edlc_battery_clear_cache(bat);

	INIT_DELAYED_WORK(&bat->bat_work, edlc_battery_work);
	devm_add_action(dev, edlc_battery_clear_data, &bat->bat_work);
	schedule_delayed_work(&bat->bat_work, msecs_to_jiffies(0));

	return 0;
}

static int __maybe_unused edlc_battery_suspend(struct device *dev)
{
	struct edlc_battery *bat = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&bat->bat_work);
	return 0;
}

static int __maybe_unused edlc_battery_resume(struct device *dev)
{
	struct edlc_battery *bat = dev_get_drvdata(dev);

	edlc_battery_clear_cache(bat);
	schedule_delayed_work(&bat->bat_work, msecs_to_jiffies(0));
	return 0;
}

static SIMPLE_DEV_PM_OPS(edlc_battery_pm_ops, edlc_battery_suspend, edlc_battery_resume);

#ifdef CONFIG_OF
static const struct of_device_id edlc_battery_of_match[] = {
	{ .compatible = "edlc-battery", },
	{ },
};
MODULE_DEVICE_TABLE(of, edlc_battery_of_match);
#endif

static struct platform_driver edlc_battery_driver = {
	.driver = {
		.name = "edlc-battery",
		.of_match_table = of_match_ptr(edlc_battery_of_match),
		.pm = &edlc_battery_pm_ops,
	},
	.probe = edlc_battery_probe,
};
module_platform_driver(edlc_battery_driver);

MODULE_DESCRIPTION("Generic EDLC battery driver");
MODULE_AUTHOR("Evgeny Boger <boger@wirenboard.com>");
MODULE_LICENSE("GPL");
