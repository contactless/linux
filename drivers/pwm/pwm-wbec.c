// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec-power.c - Wiren Board Embedded Controller PWM driver
 *
 * Copyright (c) 2024 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/math64.h>
#include <linux/mfd/wbec.h>

struct wbec_pwm {
	struct regmap *regmap;
	struct pwm_chip chip;
};

static int wbec_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	struct wbec_pwm *wbec_pwm = container_of(chip, struct wbec_pwm, chip);
	u16 regs[3] = {};
	u32 duty_percent;
	int ret;

	if (state->period > 1000000000) {
		// Minimum frequency is 1 Hz
		dev_err(chip->dev, "Period %llu ns is not supported: bigger than 1 second\n", state->period);
		return -EINVAL;
	}
	if (state->period < 100000) {
		// Maximum frequency is 10 kHz
		dev_err(chip->dev, "Period %llu ns is not supported: less than 100 us\n", state->period);
		return -EINVAL;
	}
	if (state->polarity == PWM_POLARITY_INVERSED) {
		dev_err(chip->dev, "Polarity inverted is not supported\n");
		return -EINVAL;
	}

	regs[0] = div_u64(1000000000, state->period);
	duty_percent = div_u64(state->duty_cycle * 100, state->period);
	if (duty_percent > 100)
		duty_percent = 100;
	regs[1] = duty_percent;

	if (state->enabled)
		regs[2] |= WBEC_REG_BUZZER_CTRL_ENABLED_MSK;

	ret = regmap_bulk_write(wbec_pwm->regmap, WBEC_REG_BUZZER_FREQ,
			 regs, ARRAY_SIZE(regs));

	if (ret < 0) {
		dev_err(chip->dev, "Failed to write PWM regs: %d\n", ret);
		return ret;
	}

	return 0;
}

static int wbec_pwm_get_state(struct pwm_chip *chip,
				struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct wbec_pwm *wbec_pwm = container_of(chip, struct wbec_pwm, chip);
	u16 regs[3];
	int ret;

	ret = regmap_bulk_read(wbec_pwm->regmap, WBEC_REG_BUZZER_FREQ,
			 regs, ARRAY_SIZE(regs));

	if (ret < 0) {
		dev_err(chip->dev, "Failed to read PWM regs: %d\n", ret);
		return ret;
	}

	if (regs[2] & WBEC_REG_BUZZER_CTRL_ENABLED_MSK)
		state->enabled = true;
	else
		state->enabled = false;

	if (regs[0] == 0)
		state->period = 0;
	else
		// Period in ns (1/freq_hz * 1e9)
		state->period = 1000000000 / regs[0];

	state->duty_cycle = div_u64(state->period * regs[1], 100);
	state->polarity = PWM_POLARITY_NORMAL;

	return 0;
}


static const struct pwm_ops wbec_pwm_ops = {
	.apply = wbec_pwm_apply,
	.get_state = wbec_pwm_get_state,
};


static int wbec_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wbec *wbec;
	struct wbec_pwm *wbec_pwm;
	int ret;

	if (!dev->parent)
		return -ENODEV;

	wbec_pwm = devm_kzalloc(dev, sizeof(*wbec_pwm), GFP_KERNEL);
	if (!wbec_pwm)
		return -ENOMEM;

	wbec = dev_get_drvdata(dev->parent);
	wbec_pwm->regmap = wbec->regmap;

	wbec_pwm->chip.dev = dev;
	wbec_pwm->chip.ops = &wbec_pwm_ops;
	wbec_pwm->chip.npwm = 1;

	ret = pwmchip_add(&wbec_pwm->chip);
	if (ret < 0) {
		dev_err(dev, "Failed to add PWM chip: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, wbec_pwm);

	return 0;
}

static int wbec_pwm_remove(struct platform_device *pdev)
{
	struct wbec_pwm *wbec_pwm = platform_get_drvdata(pdev);

	pwmchip_remove(&wbec_pwm->chip);

	return 0;
}

static const struct of_device_id wbec_pwm_of_match[] = {
	{ .compatible = "wirenboard,wbec-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, wbec_pwm_of_match);

static struct platform_driver wbec_pwm_driver = {
	.probe = wbec_pwm_probe,
	.remove = wbec_pwm_remove,
	.driver = {
		.name = "wbec-pwm",
		.of_match_table = wbec_pwm_of_match,
	},
};

module_platform_driver(wbec_pwm_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board Embedded Controller PWM driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-pwm");
