==============================================
Kernel driver wbec
==============================================

Supported board/chip:

  * Wiren Board Embedded Cotroller

Author:
        Pavel Gasheev <pavel.gasheev@wirenboard.com>

Description
-----------
Wiren Board Embedded Cotroller (WBEC) is a microcontroller with special
firmware. It is connected to the main CPU via SPI bus.

The wbec is a MFD (Multi-Function Device) driver. It provides a set of
sub-drivers for each device connected to WBEC:

  * gpio-wbec - GPIO driver
  * adc-wbec - ADC driver
  * wdt-wbec - Watchdog driver
  * rtc-wbec - RTC driver
  * pwrkey-wbec - Power key driver
  * wbec-power - Power driver

The wbec driver also provides a set of sysfs attributes for WBEC:

  * fwver - firmware version in text format
  * hwver - board hardware version - a number from 0 to 4095
  * poweron_reason - code of reason of last power on
  * poweron_reason_str - string representation of poweron_reason
