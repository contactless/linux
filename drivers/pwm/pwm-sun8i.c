// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Hao Zhang <hao5781286@gmail.com>

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/regmap.h>

#define PWM_IRQ_ENABLE_REG	0x0000
#define PCIE(ch)		BIT(ch)

#define PWM_IRQ_STATUS_REG	0x0004
#define PIS(ch)			BIT(ch)

#define CAPTURE_IRQ_ENABLE_REG	0x0010
#define CRIE(ch)		BIT((ch) * 2)
#define CFIE(ch)		BIT((ch) * 2 + 1)

#define CAPTURE_IRQ_STATUS_REG	0x0014
#define CRIS(ch)		BIT((ch) * 2)
#define CFIS(ch)		BIT((ch) * 2 + 1)

#define CLK_CFG_REG(ch)		(0x0020 + ((ch) >> 1) * 4)
#define CLK_SRC_SEL		GENMASK(8, 7)
#define CLK_SRC_BYPASS_SEC	BIT(6)
#define CLK_SRC_BYPASS_FIR	BIT(5)
#define CLK_GATING		BIT(4)
#define CLK_DIV_M		GENMASK(3, 0)

#define PWM_DZ_CTR_REG(ch)	(0x0030 + ((ch) >> 1) * 4)
#define PWM_DZ_INTV		GENMASK(15, 8)
#define PWM_DZ_EN		BIT(0)

#define PWM_ENABLE_REG		0x0040
#define PWM_EN(ch)		BIT(ch)

#define CAPTURE_ENABLE_REG	0x0044
#define CAP_EN(ch)		BIT(ch)

#define PWM_CTR_REG(ch)		(0x0060 + (ch) * 0x20)
#define PWM_PERIOD_RDY		BIT(11)
#define PWM_PUL_START		BIT(10)
#define PWM_MODE		BIT(9)
#define PWM_ACT_STA		BIT(8)
#define PWM_PRESCAL_K		GENMASK(7, 0)

#define PWM_PERIOD_REG(ch)	(0x0064 + (ch) * 0x20)
#define PWM_ENTIRE_CYCLE	GENMASK(31, 16)
#define PWM_ACT_CYCLE		GENMASK(15, 0)

#define PWM_CNT_REG(ch)		(0x0068 + (ch) * 0x20)
#define PWM_CNT_VAL		GENMASK(15, 0)

#define CAPTURE_CTR_REG(ch)	(0x006c + (ch) * 0x20)
#define CAPTURE_CRLF		BIT(2)
#define CAPTURE_CFLF		BIT(1)
#define CAPINV			BIT(0)

#define CAPTURE_RISE_REG(ch)	(0x0070 + (ch) * 0x20)
#define CAPTURE_CRLR		GENMASK(15, 0)

#define CAPTURE_FALL_REG(ch)	(0x0074 + (ch) * 0x20)
#define CAPTURE_CFLR		GENMASK(15, 0)


struct sun8i_pwm_data {
	bool has_prescaler_bypass;
	bool has_direct_mod_clk_output;
	unsigned int npwm;
};


struct sun8i_pwm_chip {
	struct pwm_chip chip;
	struct clk *clk;
	void __iomem *base;
	const struct sun8i_pwm_data *data;
	struct regmap *regmap;
};

static struct sun8i_pwm_chip *to_sun8i_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct sun8i_pwm_chip, chip);
}

static u32 sun8i_pwm_read(struct sun8i_pwm_chip *sun8i_pwm,
			  unsigned long offset)
{
	u32 val;

	regmap_read(sun8i_pwm->regmap, offset, &val);
	return val;
}

static void sun8i_pwm_set_bit(struct sun8i_pwm_chip *sun8i_pwm,
			      unsigned long reg, u32 bit)
{
	regmap_update_bits(sun8i_pwm->regmap, reg, bit, bit);
}

static void sun8i_pwm_clear_bit(struct sun8i_pwm_chip *sun8i_pwm,
				unsigned long reg, u32 bit)
{
	regmap_update_bits(sun8i_pwm->regmap, reg, bit, 0);
}

static void sun8i_pwm_set_value(struct sun8i_pwm_chip *sun8i_pwm,
				unsigned long reg, u32 mask, u32 val)
{
	regmap_update_bits(sun8i_pwm->regmap, reg, mask, val);
}

static void sun8i_pwm_set_polarity(struct sun8i_pwm_chip *chip, u32 ch,
				   enum pwm_polarity polarity)
{
	if (polarity == PWM_POLARITY_NORMAL)
		sun8i_pwm_set_bit(chip, PWM_CTR_REG(ch), PWM_ACT_STA);
	else
		sun8i_pwm_clear_bit(chip, PWM_CTR_REG(ch), PWM_ACT_STA);
}

static int sun8i_pwm_config(struct sun8i_pwm_chip *sun8i_pwm, u8 ch,
			    const struct pwm_state *state)
{
	u64 clk_rate, clk_div, val;
	u16 prescaler = 0;
	u16 div_id = 0;

	clk_rate = clk_get_rate(sun8i_pwm->clk);
	val = state->period * clk_rate;
	do_div(val, NSEC_PER_SEC);

	// dev_dbg(pwmchip_parent(sun8i_pwm->chip), "clock source freq:%lldHz\n", clk_rate);

	/* calculate and set prescaler, div table, PWM entire cycle */
	clk_div = val;
	while (clk_div > 65535) {
		prescaler++;
		clk_div = val;
		do_div(clk_div, 1U << div_id);
		do_div(clk_div, prescaler + 1);

		if (prescaler == 255) {
			prescaler = 0;
			div_id++;
			if (div_id == 9) {
				// dev_err(pwmchip_parent(sun8i_pwm->chip),
				// 	"unsupport period value\n");
				return -EINVAL;
			}
		}
	}

	sun8i_pwm_set_value(sun8i_pwm, PWM_PERIOD_REG(ch),
			    PWM_ENTIRE_CYCLE, clk_div << 16);
	sun8i_pwm_set_value(sun8i_pwm, PWM_CTR_REG(ch),
			    PWM_PRESCAL_K, prescaler << 0);
	sun8i_pwm_set_value(sun8i_pwm, CLK_CFG_REG(ch),
			    CLK_DIV_M, div_id << 0);

	/* set duty cycle */
	val = state->period;
	do_div(val, clk_div);
	clk_div = state->duty_cycle;
	do_div(clk_div, val);
	if (clk_div > 65535)
		clk_div = 65535;

	sun8i_pwm_set_value(sun8i_pwm, PWM_PERIOD_REG(ch),
			    PWM_ACT_CYCLE, clk_div << 0);

	return 0;
}

static int sun8i_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	int ret;
	struct sun8i_pwm_chip *sun8i_pwm = to_sun8i_pwm_chip(chip);

	ret = sun8i_pwm_config(sun8i_pwm, pwm->hwpwm, state);
	if (ret) {
		// dev_err(pwmchip_parent(chip), "Failed to config PWM\n");
		return ret;
	}

	sun8i_pwm_set_polarity(sun8i_pwm, pwm->hwpwm, state->polarity);

	if (state->enabled) {
		sun8i_pwm_set_bit(sun8i_pwm,
				  PWM_ENABLE_REG, PWM_EN(pwm->hwpwm));
	} else {
		sun8i_pwm_clear_bit(sun8i_pwm,
				    PWM_ENABLE_REG, PWM_EN(pwm->hwpwm));
	}

	return 0;
}

static int sun8i_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct sun8i_pwm_chip *sun8i_pwm = to_sun8i_pwm_chip(chip);
	u64 clk_rate, tmp;
	u32 val;
	u16 clk_div, act_cycle;
	u8 prescal, div_id;
	u8 chn = pwm->hwpwm;

	clk_rate = clk_get_rate(sun8i_pwm->clk);

	val = sun8i_pwm_read(sun8i_pwm, PWM_CTR_REG(chn));
	if (PWM_ACT_STA & val)
		state->polarity = PWM_POLARITY_NORMAL;
	else
		state->polarity = PWM_POLARITY_INVERSED;

	prescal = PWM_PRESCAL_K & val;

	val = sun8i_pwm_read(sun8i_pwm, PWM_ENABLE_REG);
	if (PWM_EN(chn) & val)
		state->enabled = true;
	else
		state->enabled = false;

	val = sun8i_pwm_read(sun8i_pwm, PWM_PERIOD_REG(chn));
	act_cycle = PWM_ACT_CYCLE & val;
	clk_div = val >> 16;

	val = sun8i_pwm_read(sun8i_pwm, CLK_CFG_REG(chn));
	div_id = CLK_DIV_M & val;

	tmp = act_cycle * prescal * (1U << div_id) * NSEC_PER_SEC;
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);
	tmp = clk_div * prescal * (1U << div_id) * NSEC_PER_SEC;
	state->period = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);
	return 0;
}

static const struct regmap_config sun8i_pwm_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = CAPTURE_FALL_REG(7),
};

static const struct pwm_ops sun8i_pwm_ops = {
	.apply = sun8i_pwm_apply,
	.get_state = sun8i_pwm_get_state,
};

static const struct sun8i_pwm_data pwm_data = {
	.has_prescaler_bypass = false,
	.has_direct_mod_clk_output = false,
	.npwm = 8,
};

static const struct of_device_id sun8i_pwm_dt_ids[] = {
	{
		.compatible = "allwinner,sun8i-r40-pwm",
		.data = &pwm_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, sun8i_pwm_dt_ids);



static int sun8i_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	const struct sun8i_pwm_data *data;
	struct sun8i_pwm_chip *sun8ichip;
	int ret;

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return -ENODEV;

	chip = devm_pwmchip_alloc(&pdev->dev, data->npwm, sizeof(*sun8ichip));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	sun8ichip = to_sun8i_pwm_chip(chip);

	sun8ichip->data = data;
	sun8ichip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sun8ichip->base))
		return PTR_ERR(sun8ichip->base);

	sun8ichip->regmap = devm_regmap_init_mmio(&pdev->dev, sun8ichip->base,
					    &sun8i_pwm_regmap_config);
	if (IS_ERR(sun8ichip->regmap)) {
		dev_err(&pdev->dev, "Failed to create regmap\n");
		return PTR_ERR(sun8ichip->regmap);
	}

	/* we use mux-0 (24M) as the only clock source */
	sun8ichip->clk = devm_clk_get(&pdev->dev, "mux-0");

	if (IS_ERR(sun8ichip->clk)) {
		dev_err(&pdev->dev, "Failed to get PWM clock\n");
		return PTR_ERR(sun8ichip->clk);
	}

	ret = clk_prepare_enable(sun8ichip->clk);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PWM clock\n");
		return ret;
	}

	/* configure all pwm pairs */
	int i;
	for (i = 0; i < sun8ichip->chip.npwm; ++i) {
		/* use mux-0 */
		sun8i_pwm_set_value(sun8ichip, CLK_CFG_REG(i),
					CLK_SRC_SEL, 0 << 7);
		/* ungate clock */
		sun8i_pwm_set_bit(sun8ichip,
				  CLK_CFG_REG(i), CLK_GATING);
	}

	sun8ichip->chip.ops = &sun8i_pwm_ops;

	ret = pwmchip_add(&sun8ichip->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to add PWM chip: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, sun8ichip);

	return 0;
}

static void sun8i_pwm_remove(struct platform_device *pdev)
{
	struct sun8i_pwm_chip *pwm = platform_get_drvdata(pdev);

	clk_disable_unprepare(pwm->clk);
	pwmchip_remove(&pwm->chip);
}

static struct platform_driver sun8i_pwm_driver = {
	.driver = {
		.name = "sun8i-pwm",
		.of_match_table = sun8i_pwm_dt_ids,
	},
	.probe = sun8i_pwm_probe,
	.remove = sun8i_pwm_remove,
};
module_platform_driver(sun8i_pwm_driver);

MODULE_ALIAS("platform: sun8i-pwm");
MODULE_AUTHOR("Hao Zhang <hao5781286@gmail.com>");
MODULE_DESCRIPTION("Allwinner sun8i PWM driver");
MODULE_LICENSE("GPL v2");
