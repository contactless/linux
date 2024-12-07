// SPDX-License-Identifier: GPL-2.0-only
/*
 * wbec.c - Wiren Board Embedded Controller MFD driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/rtc.h>
#include <linux/mfd/core.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/mfd/wbec.h>
#include <linux/mod_devicetable.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/of_platform.h>

/* For power off WBEC activates PWON pin on PMIC for 6s */
#define WBEC_POWER_RESET_DELAY_MS			10000

static const char * const wbec_poweron_reason[] = {
	"Power supply on",
	"Power button",
	"RTC alarm",
	"Reboot",
	"Reboot instead of poweroff",
	"Watchdog",
	"PMIC is unexpectedly off",
};

static const struct regmap_config wbec_regmap_config_v1 = {
	.name = "wbec_regmap_v1",
	.reg_bits = 16,
	.read_flag_mask = BIT(7),
	.val_bits = 16,
	/* v1 protocol does'n use pad bits */
	.pad_bits = 0,
	.max_register = 0xFF,
	.can_sleep = true,
};

static const struct regmap_config wbec_regmap_config_v2 = {
	.name = "wbec_regmap",
	.reg_bits = 16,
	.read_flag_mask = BIT(7),
	.val_bits = 16,
	/* v2 protocol needs 5 pad words */
	.pad_bits = 16 * WBEC_REGMAP_PAD_WORDS_COUNT,
	.max_register = 0x130,
	.can_sleep = true,
};

static int wbec_check_present(struct wbec *wbec)
{
	int wbec_id, ret;

	ret = regmap_read(wbec->regmap, WBEC_REG_INFO_WBEC_ID, &wbec_id);
	if (ret)
		return ret;

	if (wbec_id != WBEC_ID)
		return -ENODEV;

	return 0;
}

/* ----------------------------------------------------------------------- */
/* SysFS interface */

static ssize_t
fwrev_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	union wbec_version {
		struct {
			u16 major;
			u16 minor;
			u16 patch;
			s16 suffix;
		};
		u16 raw[4];
	} version;
	struct wbec *wbec = dev_get_drvdata(dev);
	int ret;
	char suffix_str[32] = "";

	ret = regmap_bulk_read(wbec->regmap, WBEC_REG_INFO_FW_VER_MAJOR,
			version.raw, sizeof(version.raw));
	if (ret)
		return ret;

	if (version.suffix > 0)
		sprintf(suffix_str, "+wb%d", version.suffix);
	else if (version.suffix < 0)
		sprintf(suffix_str, "-rc%d", -version.suffix);

	return sprintf(buf, "%d.%d.%d%s\n",
			version.major, version.minor, version.patch, suffix_str);
}

static ssize_t
hwrev_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wbec *wbec = dev_get_drvdata(dev);
	int ret, hwrev;

	ret = regmap_read(wbec->regmap, WBEC_REG_INFO_BOARD_REV, &hwrev);
	if (ret)
		return ret;

	return sprintf(buf, "%d\n", (u16)hwrev);
}

static ssize_t
poweron_reason_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wbec *wbec = dev_get_drvdata(dev);
	int ret, reason;

	ret = regmap_read(wbec->regmap, WBEC_REG_INFO_POWERON_REASON, &reason);
	if (ret)
		return ret;

	return sprintf(buf, "%d\n", reason);
}

static ssize_t
poweron_reason_str_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct wbec *wbec = dev_get_drvdata(dev);
	int ret, reason;

	ret = regmap_read(wbec->regmap, WBEC_REG_INFO_POWERON_REASON, &reason);
	if (ret)
		return ret;

	if (reason >= ARRAY_SIZE(wbec_poweron_reason))
		return sprintf(buf, "Unknown\n");

	return sprintf(buf, "%s\n", wbec_poweron_reason[reason]);
}

static ssize_t
uid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct wbec *wbec = dev_get_drvdata(dev);
	int i, ret;
	char *buf_start = buf;
	u8 uid[12];

	ret = regmap_bulk_read(wbec->regmap, WBEC_REG_INFO_UID, uid, 6);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(uid); i++)
		buf += sprintf(buf, "%02x", uid[i]);

	buf += sprintf(buf, "\n");

	return buf - buf_start;
}

static DEVICE_ATTR_RO(fwrev);
static DEVICE_ATTR_RO(hwrev);
static DEVICE_ATTR_RO(poweron_reason);
static DEVICE_ATTR_RO(poweron_reason_str);
static DEVICE_ATTR_RO(uid);

static struct attribute *wbec_sysfs_entries[] = {
	&dev_attr_fwrev.attr,
	&dev_attr_hwrev.attr,
	&dev_attr_poweron_reason.attr,
	&dev_attr_poweron_reason_str.attr,
	&dev_attr_uid.attr,
	NULL,
};

static const struct attribute_group wbec_attr_group = {
	.attrs	= wbec_sysfs_entries,
};


#ifdef CONFIG_DEBUG_FS
/*
 * debugfs entries only exposed if we're using debug
 */

static ssize_t wbec_write_reg(struct file *file,
				  const char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct wbec *wbec = file->private_data;
	char buf[32];
	ssize_t buf_size;
	int regp;
	u16 reg_addr, reg_value;
	int err;
	int i = 0;

	/* Get userspace string and assure termination */
	buf_size = min((ssize_t)count, (ssize_t)(sizeof(buf)-1));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;
	buf[buf_size] = 0;

	/* The string format is "wbec: 0xnn 0xnn" for writing a register. */

	/* Check prefix */
	if (strncmp(buf, "wbec: ", 6) != 0)
		return -EINVAL;

	/* Get register address: skip spaces */
	i = 6;
	while ((i < buf_size) && (buf[i] == ' '))
		i++;
	regp = i;
	/* And replace first space after value with null-termination char */
	while ((i < buf_size) && (buf[i] != ' '))
		i++;
	buf[i] = '\0';

	err = kstrtou16(&buf[regp], 16, &reg_addr);
	if (err)
		return err;

	/* Get register value: the same method */
	i++;
	while ((i < buf_size) && (buf[i] == ' '))
		i++;
	err = kstrtou16(&buf[i], 16, &reg_value);
	if (err)
		return err;

	/* Write register */
	dev_info(wbec->dev, "debugfs writing 0x%04X to 0x%04X\n", reg_value, reg_addr);
	err = regmap_write(wbec->regmap, reg_addr, reg_value);
	if (err)
		return err;

	return buf_size;
}

static const struct file_operations wbec_write_reg_fops = {
	.open = simple_open,
	.write = wbec_write_reg,
	.llseek = noop_llseek,
};

static void wbec_setup_debugfs(struct wbec *wbec)
{
	wbec->wbec_dir = debugfs_create_dir("wbec", NULL);

	debugfs_create_file("write_reg", 0200, wbec->wbec_dir, wbec,
			    &wbec_write_reg_fops);
}

static void wbec_clean_debugfs(struct wbec *wbec)
{
	debugfs_remove_recursive(wbec->wbec_dir);
}
#else
static inline void wbec_setup_debugfs(struct wbec *wbec)
{
}
static inline void wbec_clean_debugfs(struct wbec *wbec)
{
}
#endif

static irqreturn_t wbec_irq_handler(int irq, void *dev_id)
{
	struct wbec *wbec = dev_id;

	if (wbec->irq_handler)
		wbec->irq_handler(wbec);
	else {
		dev_warn_ratelimited(wbec->dev, "Unhandled IRQ\n");
		// dummy uart exchange
	}

	return IRQ_HANDLED;
}

static int wbec_power_off(struct sys_off_data *data)
{
	int ret;
	struct wbec *wbec = data->cb_data;

	ret = regmap_write(wbec->regmap, WBEC_REG_POWER_CTRL, WBEC_REG_POWER_CTRL_OFF_MSK);
	if (ret)
		dev_err(wbec->dev, "Failed to shutdown device!\n");

	/* Give capacitors etc. time to drain to avoid kernel panic msg. */
	msleep(WBEC_POWER_RESET_DELAY_MS);
	dev_err(wbec->dev, "Device not actually shutdown. Check EC and FETs!\n");

	return NOTIFY_DONE;
}

static int wbec_restart(struct sys_off_data *data)
{
	int ret;
	struct wbec *wbec = data->cb_data;

	ret = regmap_write(wbec->regmap, WBEC_REG_POWER_CTRL, WBEC_REG_POWER_CTRL_REBOOT_MSK);
	if (ret)
		dev_err(wbec->dev, "Failed to reboot device!\n");

	/* Give capacitors etc. time to drain to avoid kernel panic msg. */
	msleep(WBEC_POWER_RESET_DELAY_MS);
	dev_err(wbec->dev, "Device not actually rebooted. Check EC and FETs!\n");

	return NOTIFY_DONE;
}

static int wbec_probe(struct spi_device *spi)
{
	struct wbec *wbec;
	int ret;

	wbec = devm_kzalloc(&spi->dev, sizeof(*wbec), GFP_KERNEL);
	if (!wbec)
		return -ENOMEM;

	wbec->dev = &spi->dev;
	wbec->spi = spi;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi_setup(spi);

	spi_set_drvdata(spi, wbec);

	wbec->regmap = devm_regmap_init_spi(spi, &wbec_regmap_config_v2);

	if (IS_ERR(wbec->regmap)) {
		dev_err(&spi->dev, "regmap initialization failed\n");
		return PTR_ERR(wbec->regmap);
	}

	ret = wbec_check_present(wbec);
	if (ret == -ENODEV) {
		dev_info(wbec->dev, "WBEC not found with v2 protocol, trying v1\n");
		/* don't worry about memory leak, previous regmap will be freed by devm */
		wbec->regmap = devm_regmap_init_spi(spi, &wbec_regmap_config_v1);

		if (IS_ERR(wbec->regmap)) {
			dev_err(&spi->dev, "regmap initialization failed\n");
			return PTR_ERR(wbec->regmap);
		}

		ret = wbec_check_present(wbec);
		if (ret) {
			dev_err(wbec->dev, "WBEC not found\n");
			return ret;
		}
		dev_info(wbec->dev, "WBEC found with v1 protocol\n");
		wbec->support_v2_protocol = false;
	} else {
		if (ret)
			return ret;
		dev_info(wbec->dev, "WBEC found with v2 protocol\n");
		wbec->support_v2_protocol = true;
	}

	if (wbec->support_v2_protocol) {
		/* IRQ for UARTs needs v2 protocol */
		if (spi->irq) {
			ret = devm_request_threaded_irq(wbec->dev, spi->irq, NULL, wbec_irq_handler,
							IRQF_TRIGGER_RISING | IRQF_ONESHOT,
							dev_name(wbec->dev), wbec);
			if (ret)
				return dev_err_probe(wbec->dev, ret, "failed to request IRQ\n");
		} else {
			dev_warn(wbec->dev, "no IRQ defined, wbec-uart not supported\n");
		}
	}

	ret = sysfs_create_group(&wbec->dev->kobj, &wbec_attr_group);
	if (ret) {
		dev_err(&spi->dev, "failed to create sysfs group\n");
		return ret;
	}

	ret = devm_of_platform_populate(&spi->dev);
	if (ret) {
		dev_err(&spi->dev, "failed to add MFD devices %d\n", ret);
		return ret;
	}

	devm_register_sys_off_handler(wbec->dev,
		SYS_OFF_MODE_POWER_OFF,
		SYS_OFF_PRIO_FIRMWARE,
		wbec_power_off, wbec);

	devm_register_sys_off_handler(wbec->dev,
		SYS_OFF_MODE_RESTART,
		SYS_OFF_PRIO_FIRMWARE,
		wbec_restart, wbec);

	wbec_setup_debugfs(wbec);

	dev_info(&spi->dev, "WBEC device added\n");

	return ret;
}

static void wbec_remove(struct spi_device *spi)
{
	struct wbec *wbec = spi_get_drvdata(spi);

	sysfs_remove_group(&wbec->dev->kobj, &wbec_attr_group);
	wbec_clean_debugfs(wbec);
}

static const struct spi_device_id wbec_spi_id[] = {
	{"wbec", 0},
	{}
};
MODULE_DEVICE_TABLE(spi, wbec_spi_id);

static const struct of_device_id wbec_of_match[] = {
	{ .compatible = "wirenboard,wbec" },
	{}
};
MODULE_DEVICE_TABLE(of, wbec_of_match);

static struct spi_driver wbec_driver = {
	.driver = {
		.name = "wbec",
		.of_match_table = wbec_of_match,
	},
	.probe = wbec_probe,
	.remove = wbec_remove,
	.id_table = wbec_spi_id,
};


module_spi_driver(wbec_driver);

MODULE_AUTHOR("Pavel Gasheev <pavel.gasheev@wirenboard.com>");
MODULE_DESCRIPTION("Wiren Board Embedded Controller MFD driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:wbec");
