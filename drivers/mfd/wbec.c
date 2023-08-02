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

static const struct regmap_config wbec_regmap_config = {
	.reg_bits = 16,
	.read_flag_mask = BIT(7),
	.val_bits = 16,
	.max_register = 0xFF,
};

static const struct mfd_cell wbec_cells[] = {
	{
		.name = "wbec-adc",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-adc"
	},
	{
		.name = "wbec-gpio",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-gpio"
	},
	{
		.name = "wbec-watchdog",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-watchdog"
	},
	{
		.name = "wbec-rtc",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-rtc"
	},
	{
		.name = "wbec-pwrkey",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-pwrkey"
	},
	{
		.name = "wbec-power",
		.id = PLATFORM_DEVID_NONE,
		.of_compatible = "wirenboard,wbec-power"
	},
};

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


static DEVICE_ATTR_RO(fwrev);
static DEVICE_ATTR_RO(hwrev);
static DEVICE_ATTR_RO(poweron_reason);
static DEVICE_ATTR_RO(poweron_reason_str);

static struct attribute *wbec_sysfs_entries[] = {
	&dev_attr_fwrev.attr,
	&dev_attr_hwrev.attr,
	&dev_attr_poweron_reason.attr,
	&dev_attr_poweron_reason_str.attr,
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

static struct wbec *wbec_pm;

static void wbec_pm_power_off(void)
{
	int ret;
	struct wbec *wbec = wbec_pm;

	if (!wbec)
		return;

	ret = regmap_write(wbec->regmap, WBEC_REG_POWER_CTRL, WBEC_REG_POWER_CTRL_OFF_MSK);
	if (ret)
		dev_err(wbec->dev, "Failed to shutdown device!\n");

	/* Give capacitors etc. time to drain to avoid kernel panic msg. */
	msleep(WBEC_POWER_RESET_DELAY_MS);
	dev_err(wbec->dev, "Device not actually shutdown. Check EC and FETs!\n");
}

static int wbec_restart_notify(struct notifier_block *this, unsigned long mode, void *cmd)
{
	struct wbec *wbec = wbec_pm;
	int ret;

	if (!wbec)
		return NOTIFY_STOP;

	ret = regmap_write(wbec->regmap, WBEC_REG_POWER_CTRL, WBEC_REG_POWER_CTRL_REBOOT_MSK);
	if (ret)
		dev_err(wbec->dev, "Failed to reboot device!\n");

	/* Give capacitors etc. time to drain to avoid kernel panic msg. */
	msleep(WBEC_POWER_RESET_DELAY_MS);
	dev_err(wbec->dev, "Device not actually rebooted. Check EC and FETs!\n");

	return NOTIFY_DONE;
}

static struct notifier_block wbec_restart_handler = {
	.notifier_call = wbec_restart_notify,
	.priority = 255,
};

static int wbec_probe(struct spi_device *spi)
{
	struct wbec *wbec;
	int ret;
	int wbec_id;

	wbec = devm_kzalloc(&spi->dev, sizeof(*wbec), GFP_KERNEL);
	if (!wbec)
		return -ENOMEM;

	wbec->dev = &spi->dev;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi_setup(spi);

	spi_set_drvdata(spi, wbec);

	wbec->regmap = devm_regmap_init_spi(spi, &wbec_regmap_config);
	if (IS_ERR(wbec->regmap)) {
		dev_err(&spi->dev, "regmap initialization failed\n");
		return PTR_ERR(wbec->regmap);
	}

	ret = regmap_read(wbec->regmap, WBEC_REG_INFO_WBEC_ID, &wbec_id);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to read the wbec id at 0x%X\n",
			WBEC_REG_INFO_WBEC_ID);
		return ret;
	}
	if (wbec_id != WBEC_ID) {
		dev_err(&spi->dev, "wrong wbec ID at 0x%X. Get 0x%X istead of 0x%X\n",
			WBEC_REG_INFO_WBEC_ID, wbec_id, WBEC_ID);
		return -ENOTSUPP;
	}

	ret = sysfs_create_group(&wbec->dev->kobj, &wbec_attr_group);
	if (ret) {
		dev_err(&spi->dev, "failed to create sysfs group\n");
		return ret;
	}

	ret = devm_mfd_add_devices(&spi->dev, PLATFORM_DEVID_NONE,
			      wbec_cells, ARRAY_SIZE(wbec_cells), NULL, 0, NULL);
	if (ret) {
		dev_err(&spi->dev, "failed to add MFD devices %d\n", ret);
		return ret;
	}

	wbec_pm = wbec;
	pm_power_off = wbec_pm_power_off;

	ret = register_restart_handler(&wbec_restart_handler);
	if (ret)
		dev_warn(&spi->dev, "failed to register restart handler\n");

	wbec_setup_debugfs(wbec);

	dev_info(&spi->dev, "WBEC device added\n");

	return ret;
}

static int wbec_remove(struct spi_device *spi)
{
	struct wbec *wbec = spi_get_drvdata(spi);
	/**
	 * pm_power_off may point to a function from another module.
	 * Check if the pointer is set by us and only then overwrite it.
	 */
	if (pm_power_off == wbec_pm_power_off) {
		pm_power_off = NULL;
		unregister_restart_handler(&wbec_restart_handler);
	}

	sysfs_remove_group(&wbec->dev->kobj, &wbec_attr_group);
	wbec_clean_debugfs(wbec);

	return 0;
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
MODULE_DESCRIPTION("Wiren Board 7 Embedded Controller MFD driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:wbec");
