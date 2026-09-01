/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Microsoft OS 2.0 descriptors
 *
 * Definitions for the BOS platform capability descriptor that announces an
 * MS OS 2.0 descriptor set, per the "Microsoft OS 2.0 Descriptors
 * Specification". The descriptor set itself is opaque to the kernel and is
 * supplied by the gadget driver (see struct usb_composite_dev).
 */

#ifndef __LINUX_USB_MSOS20_H
#define __LINUX_USB_MSOS20_H

#include <linux/types.h>

/* wIndex of the vendor request that returns the descriptor set */
#define MSOS20_DESCRIPTOR_INDEX		0x07

/* dwWindowsVersion: NTDDI_WINBLUE, the minimum version that supports MS OS 2.0 */
#define MSOS20_WINDOWS_VERSION_8_1	0x06030000

/* {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} in little endian byte order */
#define MSOS20_UUID							\
	((const u8[16]) {						\
		0xdf, 0x60, 0xdd, 0xd8, 0x89, 0x45, 0xc7, 0x4c,		\
		0x9c, 0xd2, 0x65, 0x9d, 0x9e, 0x64, 0x8a, 0x9f })

#define USB_DT_USB_MSOS20_CAP_SIZE	28

/* Descriptor set header: wLength, wDescriptorType, dwWindowsVersion, wTotalLength */
#define MSOS20_SET_HEADER_SIZE		10

/* The whole set has to fit into the control endpoint buffer */
#define MSOS20_DESC_SET_MAX_LENGTH	1024

/* MS OS 2.0 platform capability descriptor, kept in the BOS descriptor */
struct usb_msos20_platform_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__u8	bDevCapabilityType;
	__u8	bReserved;
	__u8	PlatformCapabilityUUID[16];
	__le32	dwWindowsVersion;
	__le16	wMSOSDescriptorSetTotalLength;
	__u8	bMS_VendorCode;
	__u8	bAltEnumCode;
} __packed;

#endif /* __LINUX_USB_MSOS20_H */
