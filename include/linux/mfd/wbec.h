/* SPDX-License-Identifier: GPL-2.0-only
 * wbec.c - Wiren Board Embedded Controller MFD driver
 *
 * Copyright (c) 2023 Wiren Board LLC
 *
 * Author: Pavel Gasheev <pavel.gasheev@wirenboard.com>
 */

#ifndef __WBEC_H
#define __WBEC_H

#include <linux/regulator/machine.h>
#include <linux/regmap.h>

#define WBEC_ID                                             0x3CD2

/* Region INFO: RO */
#define WBEC_REG_INFO_WBEC_ID                               0x00
#define WBEC_REG_INFO_BOARD_REV                             0x01
#define WBEC_REG_INFO_FW_VER_MAJOR                          0x02
#define WBEC_REG_INFO_FW_VER_MINOR                          0x03
#define WBEC_REG_INFO_FW_VER_PATCH                          0x04
#define WBEC_REG_INFO_FW_VER_SUFFIX                         0x05
#define WBEC_REG_INFO_POWERON_REASON                        0x06

/* Region RTC_TIME: RW */
#define WBEC_REG_RTC_TIME_SECS_MINS                         0x10
#define WBEC_REG_RTC_TIME_HOURS_DAYS                        0x11
#define WBEC_REG_RTC_TIME_WEEKDAYS_MONTHS                   0x12
#define WBEC_REG_RTC_TIME_YEARS                             0x13

/* Region RTC_ALARM: RW */
#define WBEC_REG_RTC_ALARM_SECS_MINS                        0x20
#define WBEC_REG_RTC_ALARM_HOURS_DAYS                       0x21
#define WBEC_REG_RTC_ALARM_STATUS                           0x22
   #define WBEC_REG_RTC_ALARM_STATUS_EN_MSK                 BIT(0)

/* Region RTC_CFG: RW */
#define WBEC_REG_RTC_CFG_OFFSET                             0x30
   #define WBEC_REG_RTC_CFG_OFFSET_CALM_MASK                GENMASK(8, 0)
   #define WBEC_REG_RTC_CFG_OFFSET_CALP_BIT                 BIT(15)
   #define WBEC_REG_RTC_CFG_OFFSET_CALW8_BIT                BIT(14)
   #define WBEC_REG_RTC_CFG_OFFSET_CALW16_BIT               BIT(13)

/* Region ADC_DATA: RO */
#define WBEC_REG_ADC_DATA_V_IN                              0x40
#define WBEC_REG_ADC_DATA_V_3_3                             0x41
#define WBEC_REG_ADC_DATA_V_5_0                             0x42
#define WBEC_REG_ADC_DATA_VBUS_CONSOLE                      0x43
#define WBEC_REG_ADC_DATA_VBUS_NETWORK                      0x44
#define WBEC_REG_ADC_DATA_TEMP                              0x45
#define WBEC_REG_ADC_DATA_ADC1                              0x46
#define WBEC_REG_ADC_DATA_ADC2                              0x47
#define WBEC_REG_ADC_DATA_ADC3                              0x48
#define WBEC_REG_ADC_DATA_ADC4                              0x49
#define WBEC_REG_ADC_DATA_ADC5                              0x4A
#define WBEC_REG_ADC_DATA_ADC6                              0x4B

/* Region GPIO: RW */
#define WBEC_REG_GPIO                                       0x80
   #define WBEC_REG_GPIO_A1_MSK                             BIT(0)
   #define WBEC_REG_GPIO_A2_MSK                             BIT(1)
   #define WBEC_REG_GPIO_A3_MSK                             BIT(2)
   #define WBEC_REG_GPIO_A4_MSK                             BIT(3)
   #define WBEC_REG_GPIO_V_OUT_MSK                          BIT(4)

/* Region WDT: RW */
#define WBEC_REG_WDT_TIMEOUT                                0x90
#define WBEC_REG_WDT_STATUS                                 0x91
   #define WBEC_REG_WDT_STATUS_RESET_MSK                    BIT(0)

/* Region POWER_CTRL: RW */
#define WBEC_REG_POWER_CTRL                                 0xA0
   #define WBEC_REG_POWER_CTRL_OFF_MSK                      BIT(0)
   #define WBEC_REG_POWER_CTRL_REBOOT_MSK                   BIT(1)

/* Region IRQ_FLAGS: RW */
#define WBEC_REG_IRQ_FLAGS                                  0xB0
#define WBEC_REG_IRQ_MSK                                    0xB2
#define WBEC_REG_IRQ_CLEAR                                  0xB4
   #define WBEC_REG_IRQ_RTC_ALARM_MSK                       BIT(0)
   #define WBEC_REG_IRQ_PWROFF_REQ_MSK                      BIT(1)

/* Region PWR_STATUS: RO */
#define WBEC_REG_PWR_STATUS                                 0xC0
   #define WBEC_REG_PWR_STATUS_POWERED_FROM_WBMZ_MSK        BIT(0)


struct wbec {
	struct device *dev;
	struct regmap *regmap;
	struct dentry *wbec_dir;
};

#endif
