// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec_adc.c - Wiren Board Embedded Controller IIO driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/err.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/iio/sysfs.h>
#include <linux/mfd/wbec.h>

enum wbec_adc_channel {
	WBEC_ADC_CH_V_IN,
	WBEC_ADC_CH_V_3_3,
	WBEC_ADC_CH_V_5_0,
	WBEC_ADC_CH_VBUS_CONSOLE,
	WBEC_ADC_CH_VBUS_NETWORK,
	WBEC_ADC_CH_TEMP,
	WBEC_ADC_CH_ADC1,
	WBEC_ADC_CH_ADC2,
	WBEC_ADC_CH_ADC3,
	WBEC_ADC_CH_ADC4,
	WBEC_ADC_CH_ADC5,
	WBEC_ADC_CH_ADC6,
};


struct wbec_adc {
	struct regmap *regmap;
};

static int read_voltage(struct regmap *map, unsigned int reg, int *voltage)
{
	int ret, val;

	ret = regmap_read(map, reg, &val);
	if (ret)
		return ret;

	/* Voltage in mV, u16 type */
	*voltage = (u16)val;
	return 0;
}

static int read_temperature(struct regmap *map, unsigned int reg, int *temperature)
{
	int ret, val;

	ret = regmap_read(map, reg, &val);
	if (ret)
		return ret;

	/* Temperature in deg C x100, s16 type */
	*temperature = (s16)val;
	return 0;
}

static int read_raw_value(struct regmap *map, struct iio_chan_spec const *channel, int *val)
{
	int ret;

	switch (channel->type) {
	case IIO_VOLTAGE:
		ret = read_voltage(map, channel->address, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	case IIO_TEMP:
		ret = read_temperature(map, channel->address, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	default:
		break;
	}
	return -EINVAL;
}

static int read_scale(struct iio_chan_spec const *channel, int *val, int *val2)
{
	switch (channel->type) {
	case IIO_VOLTAGE:
		/* WBEC units is mV, Linux units is mV */
		*val = 1;
		*val2 = 0;
		return IIO_VAL_INT;

	case IIO_TEMP:
		/* WBEC units is deg C x100, Linux units is deg C x1000 */
		*val = 10;
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		break;
	}
	return -EINVAL;
}

static int wbec_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *channel, int *val,
				int *val2, long mask)
{
	struct wbec_adc *wbec_adc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return read_raw_value(wbec_adc->regmap, channel, val);

	case IIO_CHAN_INFO_SCALE:
		return read_scale(channel, val, val2);

	default:
		break;
	}

	return -EINVAL;
}


#define WBEC_ADC_CHANNEL(_id, _type, _ext_name) { \
	.type = _type, \
	.indexed = 1, \
	.channel = WBEC_ADC_CH_##_id, \
	.address = WBEC_REG_ADC_DATA_##_id, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE), \
	.extend_name = _ext_name, \
	.datasheet_name = #_id, \
}


static const struct iio_chan_spec wbec_adc_channels[] = {
	WBEC_ADC_CHANNEL(V_IN, IIO_VOLTAGE, "IN"),
	WBEC_ADC_CHANNEL(V_3_3, IIO_VOLTAGE, "3V3"),
	WBEC_ADC_CHANNEL(V_5_0, IIO_VOLTAGE, "5V"),
	WBEC_ADC_CHANNEL(VBUS_CONSOLE, IIO_VOLTAGE, "VBUS_CONSOLE"),
	WBEC_ADC_CHANNEL(VBUS_NETWORK, IIO_VOLTAGE, "VBUS_NETWORK"),
	WBEC_ADC_CHANNEL(TEMP, IIO_TEMP, "BOARD"),
	WBEC_ADC_CHANNEL(ADC1, IIO_VOLTAGE, "ADC1"),
	WBEC_ADC_CHANNEL(ADC2, IIO_VOLTAGE, "ADC2"),
	WBEC_ADC_CHANNEL(ADC3, IIO_VOLTAGE, "ADC3"),
	WBEC_ADC_CHANNEL(ADC4, IIO_VOLTAGE, "ADC4"),
	WBEC_ADC_CHANNEL(ADC5, IIO_VOLTAGE, "ADC5"),
	WBEC_ADC_CHANNEL(ADC6, IIO_VOLTAGE, "ADC6"),
};

static const struct iio_info wbec_adc_info = {
	.read_raw = wbec_read_raw,
};

static int wbec_adc_probe(struct platform_device *pdev)
{
	struct wbec *wbec = dev_get_drvdata(pdev->dev.parent);
	struct iio_dev *indio_dev;
	struct wbec_adc *wbec_adc;

	if (!pdev->dev.parent)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*wbec_adc));
	if (!indio_dev)
		return -ENOMEM;

	wbec_adc = iio_priv(indio_dev);

	platform_set_drvdata(pdev, indio_dev);
	wbec_adc->regmap = wbec->regmap;

	indio_dev->name = "wbec";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &wbec_adc_info;

	indio_dev->channels = wbec_adc_channels;
	indio_dev->num_channels = ARRAY_SIZE(wbec_adc_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static const struct of_device_id wbec_adc_of_match[] = {
	{ .compatible = "wirenboard,wbec-adc" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_adc_of_match);

static struct platform_driver wbec_adc_driver = {
	.driver = {
		.name	= "wbec-adc",
		.of_match_table = wbec_adc_of_match,
	},
	.probe = wbec_adc_probe,
};

module_platform_driver(wbec_adc_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board 7 Embedded Controller IIO driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wbec-adc");
