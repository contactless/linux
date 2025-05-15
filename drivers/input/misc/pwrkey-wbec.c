// SPDX-License-Identifier: GPL-2.0-only
/*
 * pwrkey-wbec.c - Wiren Board Embedded Controller power key driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/workqueue.h>
#include <linux/mfd/wbec.h>

#define WBEC_PWRKEY_POLL_PERIOD_MS		500

struct wbec_pwrkey {
	struct input_dev *pwr;
	struct delayed_work wq;
	struct regmap *regmap;
};

void pwrkey_poll_wq(struct work_struct *work)
{
	struct wbec_pwrkey *wbec_pwrkey =
		container_of(work, struct wbec_pwrkey, wq.work);
	struct input_dev *pwr = wbec_pwrkey->pwr;
	int val, wbec_id;

	// Check that WBEC presented before read flag
	val = regmap_read(wbec_pwrkey->regmap, WBEC_REG_INFO_WBEC_ID, &wbec_id);
	if (val < 0) {
		dev_err(&pwr->dev, "failed to read the wbec id at 0x%X\n",
			WBEC_REG_INFO_WBEC_ID);
	} else if (wbec_id != WBEC_ID) {
		dev_err_once(&pwr->dev, "wrong wbec ID at 0x%X. Get 0x%X istead of 0x%X\n",
			WBEC_REG_INFO_WBEC_ID, wbec_id, WBEC_ID);
	} else {
		val = regmap_test_bits(wbec_pwrkey->regmap, WBEC_REG_IRQ_FLAGS,
			WBEC_REG_IRQ_PWROFF_REQ_MSK);

		if (val > 0) {
			dev_info_once(&pwr->dev, "power key press detected\n");
			regmap_write(wbec_pwrkey->regmap, WBEC_REG_IRQ_CLEAR,
				WBEC_REG_IRQ_PWROFF_REQ_MSK);
			input_report_key(pwr, KEY_POWER, 1);
			input_sync(pwr);
			input_report_key(pwr, KEY_POWER, 0);
			input_sync(pwr);
		} else if (val < 0) {
			dev_err(&pwr->dev, "Error reading power off request from EC");
		}
	}

	schedule_delayed_work(&wbec_pwrkey->wq, msecs_to_jiffies(WBEC_PWRKEY_POLL_PERIOD_MS));
}

static int wbec_pwrkey_probe(struct platform_device *pdev)
{
	struct wbec *wbec = dev_get_drvdata(pdev->dev.parent);
	struct wbec_pwrkey *wbec_pwrkey;
	int err;

	if (!pdev->dev.parent)
		return -ENODEV;

	wbec_pwrkey = devm_kzalloc(&pdev->dev, sizeof(struct wbec_pwrkey),
				GFP_KERNEL);
	if (!wbec_pwrkey)
		return -ENOMEM;

	wbec_pwrkey->regmap = wbec->regmap;

	wbec_pwrkey->pwr = devm_input_allocate_device(&pdev->dev);
	if (!wbec_pwrkey->pwr) {
		dev_err(&pdev->dev, "Can't allocate power button\n");
		return -ENOMEM;
	}

	wbec_pwrkey->pwr->name = "wbec pwrkey";
	wbec_pwrkey->pwr->phys = "wbec_pwrkey/input0";
	wbec_pwrkey->pwr->id.bustype = BUS_HOST;
	input_set_capability(wbec_pwrkey->pwr, EV_KEY, KEY_POWER);

	err = input_register_device(wbec_pwrkey->pwr);
	if (err) {
		dev_err(&pdev->dev, "Can't register power button: %d\n", err);
		return err;
	}

	platform_set_drvdata(pdev, wbec_pwrkey);
	device_init_wakeup(&pdev->dev, true);

	INIT_DELAYED_WORK(&wbec_pwrkey->wq, pwrkey_poll_wq);
	schedule_delayed_work(&wbec_pwrkey->wq, msecs_to_jiffies(WBEC_PWRKEY_POLL_PERIOD_MS));

	return 0;
}

static int wbec_pwrkey_remove(struct platform_device *pdev)
{
	struct wbec_pwrkey *wbec_pwrkey = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "%s function\n", __func__);

	cancel_delayed_work_sync(&wbec_pwrkey->wq);
	input_unregister_device(wbec_pwrkey->pwr);

	return 0;
}

static const struct of_device_id wbec_pwrkey_of_match[] = {
	{ .compatible = "wirenboard,wbec-pwrkey" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_pwrkey_of_match);

static struct platform_driver wbec_pwrkey_driver = {
	.driver	= {
		.name = "wbec-pwrkey",
		.of_match_table = wbec_pwrkey_of_match,
	},
	.probe	= wbec_pwrkey_probe,
	.remove = wbec_pwrkey_remove,
};
module_platform_driver(wbec_pwrkey_driver);

MODULE_ALIAS("platform:wbec-pwrkey");
MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board 7 Embedded Controller Power Key driver");
MODULE_LICENSE("GPL");
