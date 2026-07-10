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
#include <linux/io.h>
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
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/time64.h>
#include <linux/delay.h>
#include <linux/sched/debug.h>

/* For power off WBEC activates PWON pin on PMIC for 6s */
#define WBEC_POWER_RESET_DELAY_MS			10000

/*
 * Suspend-to-off coordinator constants.
 *
 * On H616/T507 a PSCI SYSTEM_SUSPEND can drop VDD-SYS and every peripheral
 * rail (hibernate-grade power loss). The platform firmware (BL31) only takes
 * that deepest path when it finds a magic value in the sun6i RTC data0
 * register; otherwise it does the SoC-retained sleep. The magic has two
 * variants that both enter suspend-to-off: FAST skips the DRAM CRC check,
 * VERIFY brackets the sleep with a full-DRAM CRC (slower, opt-in).
 */
#define WBEC_SUSPEND_MAGIC_FAST				0x0ff51eeaU
#define WBEC_SUSPEND_MAGIC_VERIFY			0x0ff51eebU

/* sun6i RTC general-purpose data0 offset from the RTC register base. */
#define WBEC_SUSPEND_MAGIC_RTC_OFFSET			0x100

/* SUSPEND_CTRL.timeout_s is a 16-bit EC register. */
#define WBEC_SUSPEND_TIMEOUT_MIN			10U
#define WBEC_SUSPEND_TIMEOUT_MAX			0xffffU

/*
 * The EC recovery deadline (the EC warm-resets timeout_s + ~10 s after the
 * announce if it never sees a resume) is a hang backstop, not the wake source.
 * The board is woken by the RTC alarm the user armed (rtcwake / wakealarm), so
 * the deadline must land safely AFTER that alarm or it pre-empts the intended
 * wake. Size timeout_s as the seconds-to-alarm plus a margin, mirroring the
 * proven wb-suspend-off wrapper. The CRC-verify variant brackets the sleep with
 * a full-DRAM CRC (~75 s) that runs after the wake, so its deadline needs a much
 * larger margin to clear it before the backstop can fire.
 */
#define WBEC_SUSPEND_MARGIN_FAST_S			20U
#define WBEC_SUSPEND_MARGIN_VERIFY_S			90U

static bool wbec_suspend_crc_verify;
module_param_named(suspend_crc_verify, wbec_suspend_crc_verify, bool, 0644);
MODULE_PARM_DESC(suspend_crc_verify,
		 "Use the CRC-verified (slower) suspend-to-off firmware magic");

static unsigned int wbec_suspend_default_timeout_s = WBEC_SUSPEND_TIMEOUT_MAX;
module_param_named(suspend_default_timeout_s, wbec_suspend_default_timeout_s,
		   uint, 0644);
MODULE_PARM_DESC(suspend_default_timeout_s,
		 "EC recovery deadline (s) when no RTC wakealarm is armed (long backstop; wake is then the power button)");

/*
 * Mirrors LINUX_POWERON_REASON in wb-embedded-controller src/wbec.c;
 * append-only ABI.
 */
static const char * const wbec_poweron_reason[] = {
	[0] = "Power supply on",
	[1] = "Power button",
	[2] = "RTC alarm",
	[3] = "Reboot",
	[4] = "Reboot instead of poweroff",
	[5] = "Watchdog",
	[6] = "PMIC is unexpectedly off",
	[7] = "Unknown",
	[8] = "Watchdog (warm reset)",
	[9] = "Full power cycle request",
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
	void (*handler)(struct wbec *wbec);

	handler = READ_ONCE(wbec->irq_handler);
	if (handler)
		handler(wbec);
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
	if (ret) {
		dev_err(wbec->dev, "Failed to reboot device!\n");
		return NOTIFY_DONE;
	}

	/* Give capacitors etc. time to drain to avoid kernel panic msg. */
	msleep(WBEC_POWER_RESET_DELAY_MS);
	dev_warn(wbec->dev, "WBEC reboot timeout, trying next restart handler\n");

	return NOTIFY_DONE;
}

/* ----------------------------------------------------------------------- */
/* Suspend-to-off coordinator */

/*
 * There is exactly one EC per board. syscore_ops carry no private pointer, so
 * the syscore callbacks reach the armed instance through this file-static,
 * which is set only while the coordinator is armed (DT-gated, WB8/H616).
 */
static struct wbec *wbec_pm_ctx;

/*
 * Size the EC recovery deadline from the pending RTC wakealarm. On WB8 the EC
 * itself is rtc0 and holds the alarm, so the wake time is read straight from
 * the EC over the regmap we already own. The returned timeout must place the
 * EC's recovery deadline safely AFTER that alarm (deadline = timeout_s + ~10 s
 * from the announce), so seconds-to-alarm gets a margin added on top -- a plain
 * seconds-to-alarm would put the deadline right on the alarm and could pre-empt
 * it (fatally so for the CRC-verify variant, whose ~75 s post-wake CRC would
 * still be running). When no alarm is armed a long default backstop is used and
 * the wake is the power button.
 */
static int wbec_suspend_deadline(struct wbec *wbec, u32 *timeout_s)
{
	struct rtc_time now = {0};
	struct rtc_time alarm;
	time64_t now_s, alarm_s;
	u32 margin;
	u16 t[4], a[4];
	int ret;

	ret = regmap_bulk_read(wbec->regmap, WBEC_REG_RTC_ALARM_SECS_MINS,
			       a, ARRAY_SIZE(a));
	if (ret)
		return ret;

	if (!(a[2] & WBEC_REG_RTC_ALARM_STATUS_EN_MSK)) {
		*timeout_s = min(wbec_suspend_default_timeout_s,
				 WBEC_SUSPEND_TIMEOUT_MAX);
		return 0;
	}

	ret = regmap_bulk_read(wbec->regmap, WBEC_REG_RTC_TIME_SECS_MINS,
			       t, ARRAY_SIZE(t));
	if (ret)
		return ret;

	/* Decode the EC time (same layout as rtc-wbec.c). */
	now.tm_sec = t[0] & 0x00ff;
	now.tm_min = t[0] >> 8;
	now.tm_hour = t[1] & 0x00ff;
	now.tm_mday = t[1] >> 8;
	now.tm_mon = (t[2] >> 8) - 1;
	now.tm_year = t[3] + 100;
	now_s = rtc_tm_to_time64(&now);

	/*
	 * The EC alarm carries only mday + h:m:s, so complete it with the
	 * current month/year and roll to the next month if that instant has
	 * already passed. This only sizes the recovery deadline, so the small
	 * error at month boundaries is harmless.
	 */
	alarm = now;
	alarm.tm_sec = a[0] & 0x00ff;
	alarm.tm_min = a[0] >> 8;
	alarm.tm_hour = a[1] & 0x00ff;
	alarm.tm_mday = a[1] >> 8;
	alarm_s = rtc_tm_to_time64(&alarm);
	if (alarm_s <= now_s) {
		if (++alarm.tm_mon > 11) {
			alarm.tm_mon = 0;
			alarm.tm_year++;
		}
		alarm_s = rtc_tm_to_time64(&alarm);
	}

	margin = wbec_suspend_crc_verify ? WBEC_SUSPEND_MARGIN_VERIFY_S
					 : WBEC_SUSPEND_MARGIN_FAST_S;

	if (alarm_s <= now_s)
		*timeout_s = min(wbec_suspend_default_timeout_s,
				 WBEC_SUSPEND_TIMEOUT_MAX);
	else
		*timeout_s = clamp_t(u64, (alarm_s - now_s) + margin,
				     WBEC_SUSPEND_TIMEOUT_MIN,
				     WBEC_SUSPEND_TIMEOUT_MAX);

	return 0;
}

/* Announce the power-off suspend window to the EC over the (sleeping) regmap. */
static int wbec_suspend_announce(struct wbec *wbec)
{
	u32 timeout_s;
	int ret;

	/*
	 * Ack the host-visible RTC-alarm IRQ latch (IRQ_FLAGS.RTC_ALARM) before
	 * the power-off cycle. In suspend-to-off the alarm wakes the board by
	 * pulsing the PMIC, not through a Linux IRQ, so nothing else acks this
	 * bit; clearing it keeps the resumed kernel's regmap-irq from seeing a
	 * stale pending alarm. It touches only the host-visible flag; the alarm
	 * arm bit and time in ALARM_STATUS are left intact so the pending
	 * wakealarm still fires.
	 *
	 * NB: this does NOT gate the EC's off-mode wake. That wake reads a
	 * separate EC-internal fired latch (the firmware alarm_fired_latch,
	 * sourced from the STM32G0 RTC ALRAF flag), which the host cannot reach
	 * over the regmap. A stale value there was what woke the board ~220 ms
	 * into off-mode; it is drained by the EC firmware on off-mode entry
	 * (wb-embedded-controller feature/ec-suspend-mode, "drain the stale RTC
	 * alarm latch on off-mode entry"). The timed rtcwake -m mem wake depends
	 * on that EC firmware, not on this write.
	 */
	ret = regmap_write(wbec->regmap, WBEC_REG_IRQ_CLEAR,
			   WBEC_REG_IRQ_RTC_ALARM_MSK);
	if (ret)
		return ret;

	ret = wbec_suspend_deadline(wbec, &timeout_s);
	if (ret) {
		dev_err(wbec->dev, "suspend: cannot read wake deadline: %d\n",
			ret);
		return ret;
	}

	ret = regmap_write(wbec->regmap, WBEC_REG_SUSPEND_CTRL_TIMEOUT_S,
			   timeout_s);
	if (ret)
		return ret;

	ret = regmap_write(wbec->regmap, WBEC_REG_SUSPEND_CTRL_OFF_MODE,
			   WBEC_REG_SUSPEND_CTRL_OFF_MODE_EN_MSK);
	if (ret) {
		/* Do not leave a half-announced window behind. */
		regmap_write(wbec->regmap, WBEC_REG_SUSPEND_CTRL_TIMEOUT_S, 0);
		return ret;
	}

	dev_dbg(wbec->dev, "suspend-to-off announced, EC deadline %u s\n",
		timeout_s);
	return 0;
}

static void wbec_suspend_deannounce(struct wbec *wbec)
{
	regmap_write(wbec->regmap, WBEC_REG_SUSPEND_CTRL_OFF_MODE, 0);
	regmap_write(wbec->regmap, WBEC_REG_SUSPEND_CTRL_TIMEOUT_S, 0);
}

/*
 * Device suspend/resume (normal phase): the EC handshake talks over SPI, which
 * may sleep, so it cannot live in a _noirq/syscore callback. The wbec SPI
 * device suspends before its SPI controller (child before parent), so the bus
 * is still live here. Only the deep, firmware-assisted path (PSCI
 * SYSTEM_SUSPEND, i.e. PM_SUSPEND_MEM) drops the rails and needs the EC
 * coordination; s2idle and hibernation leave pm_suspend_target_state alone and
 * are skipped.
 */
static int wbec_suspend(struct device *dev)
{
	struct wbec *wbec = dev_get_drvdata(dev);

	if (!wbec->suspend_magic_reg)
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;

	return wbec_suspend_announce(wbec);
}

static int wbec_resume(struct device *dev)
{
	struct wbec *wbec = dev_get_drvdata(dev);

	if (!wbec->suspend_magic_reg)
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;

	wbec_suspend_deannounce(wbec);
	return 0;
}

static const struct dev_pm_ops wbec_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(wbec_suspend, wbec_resume)
};

/*
 * The BL31 magic is a bare RTC-data0 mmio poke that must land right before the
 * PSCI SYSTEM_SUSPEND SMC and be gone on resume. syscore is the right phase:
 * it runs with interrupts off on the boot CPU after every device has
 * suspended, and crucially only for a genuine deep suspend (s2idle never
 * reaches syscore_suspend), so writing the magic here cannot arm the firmware
 * for a sleep that does not drop the rails. The explicit target-state check
 * additionally rejects the hibernation syscore path.
 */
static int wbec_pm_syscore_suspend(void)
{
	struct wbec *wbec = wbec_pm_ctx;

	if (!wbec || !wbec->suspend_magic_reg)
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;

	writel(wbec_suspend_crc_verify ? WBEC_SUSPEND_MAGIC_VERIFY
				       : WBEC_SUSPEND_MAGIC_FAST,
	       wbec->suspend_magic_reg);
	return 0;
}

static void wbec_pm_syscore_resume(void)
{
	struct wbec *wbec = wbec_pm_ctx;

	if (!wbec || !wbec->suspend_magic_reg)
		return;

	/* BL31 clears the magic during suspend; clear again to cover an abort. */
	writel(0, wbec->suspend_magic_reg);
}

static struct syscore_ops wbec_pm_syscore_ops = {
	.suspend = wbec_pm_syscore_suspend,
	.resume = wbec_pm_syscore_resume,
};

/*
 * Ack a stale EC RTC-alarm IRQ at the very start of every suspend.
 *
 * The EC drives its INT line (a rising-edge wakeup GPIO, PE7) while
 * IRQ_FLAGS.RTC_ALARM is set. When an off-mode alarm wakes the board that bit
 * is left set, and nothing in the kernel ever acks it: the wbec IRQ is only
 * wired for the UART, and the RTC alarm has no Linux IRQ (the off-mode wake is
 * a PMIC pulse, not an IRQ). So after the first cycle the EC INT stays asserted
 * and the kernel keeps counting it as a pending wakeup. On the *next* deep
 * suspend that makes pm_wakeup_pending() abort suspend_enter() before the
 * coordinator's off-cycle runs -- every rtcwake after the first fails, while a
 * fresh boot (no pending EC IRQ) works.
 *
 * The ack has to land before the wakeup IRQ is armed and sampled. PM_SUSPEND_-
 * PREPARE runs at the very start of a suspend (suspend_prepare(), before tasks
 * freeze, before dpm_suspend and before suspend_device_irqs()/pm_wakeup_-
 * pending()), so clearing here deasserts the line in time. The equivalent ack
 * in the .suspend callback (dpm_suspend_start) runs too late relative to that
 * check. Runs in process context with the SPI bus fully alive, so the regmap
 * write is safe here.
 */
static int wbec_pm_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct wbec *wbec = wbec_pm_ctx;

	if (!wbec || !wbec->suspend_magic_reg)
		return NOTIFY_DONE;

	if (action == PM_SUSPEND_PREPARE) {
		regmap_write(wbec->regmap, WBEC_REG_IRQ_CLEAR,
			     WBEC_REG_IRQ_RTC_ALARM_MSK);
		/*
		 * Drop any wakeup-abort state the stale EC IRQ already recorded,
		 * so a leftover from before this suspend request cannot pre-empt
		 * the enter path once the line itself is quiet.
		 */
		pm_wakeup_clear(0);
	}

	return NOTIFY_DONE;
}

static struct notifier_block wbec_pm_nb = {
	.notifier_call = wbec_pm_notify,
};

/*
 * Arm the coordinator when (and only when) the DT wires a suspend-magic RTC
 * phandle, which is present only on WB8/H616. Everything downstream keys off
 * wbec->suspend_magic_reg staying NULL on other boards.
 */
static int wbec_suspend_setup(struct wbec *wbec)
{
	struct device_node *rtc_np;
	struct resource res;
	int ret;

	rtc_np = of_parse_phandle(wbec->dev->of_node,
				  "wirenboard,suspend-magic-rtc", 0);
	if (!rtc_np)
		return 0;

	ret = of_address_to_resource(rtc_np, 0, &res);
	of_node_put(rtc_np);
	if (ret)
		return dev_err_probe(wbec->dev, ret,
				     "bad wirenboard,suspend-magic-rtc\n");

	wbec->suspend_magic_reg = devm_ioremap(wbec->dev,
					       res.start + WBEC_SUSPEND_MAGIC_RTC_OFFSET,
					       sizeof(u32));
	if (!wbec->suspend_magic_reg)
		return -ENOMEM;

	wbec_pm_ctx = wbec;
	register_syscore_ops(&wbec_pm_syscore_ops);
	register_pm_notifier(&wbec_pm_nb);

	dev_info(wbec->dev, "suspend-to-off coordinator armed\n");
	return 0;
}

static void wbec_suspend_teardown(struct wbec *wbec)
{
	if (!wbec->suspend_magic_reg)
		return;

	unregister_pm_notifier(&wbec_pm_nb);
	unregister_syscore_ops(&wbec_pm_syscore_ops);
	wbec_pm_ctx = NULL;
}

/* ----------------------------------------------------------------------- */

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

	ret = devm_device_add_group(wbec->dev, &wbec_attr_group);
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

	ret = wbec_suspend_setup(wbec);
	if (ret)
		return ret;

	wbec_setup_debugfs(wbec);

	dev_info(&spi->dev, "WBEC device added\n");

	return 0;
}

static void wbec_remove(struct spi_device *spi)
{
	struct wbec *wbec = spi_get_drvdata(spi);

	wbec_suspend_teardown(wbec);
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
		.pm = pm_sleep_ptr(&wbec_pm_ops),
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
