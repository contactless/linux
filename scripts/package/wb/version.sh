#!/bin/sh

setup_kernel_vars() {
	case "$KERNEL_FLAVOUR" in
		wb2)
			DEBARCH=armel
			KERNEL_DEFCONFIG=mxs_wirenboard_defconfig
            KDEB_WBDESC="Wiren Board 2-5"
			;;
		wb2_initramfs)
			DEBARCH=armel
			KERNEL_DEFCONFIG=mxs_wirenboard_initramfs_defconfig
			KDEB_WBDESC="Wiren Board 2-5 (initramfs)"
			;;
		wb2_usbgadget)
			DEBARCH=armel
			KERNEL_DEFCONFIG=mxs_wirenboard_initramfs_defconfig
			KDEB_WBDESC="Wiren Board 2-5 (USB gadget bootlet)"
			APPEND_DT=imx28-wirenboard5x-usbfw
			;;
		wb6)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=imx6_wirenboard_defconfig
			KDEB_WBDESC="Wiren Board 6"
			;;
		wb6_initramfs)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=imx6_wirenboard_initramfs_defconfig
			KDEB_WBDESC="Wiren Board 6 (initramfs)"
			;;
		*)
			echo "Unsupported KERNEL_FLAVOUR, please specify one of: wb2, wb6"
			return 1
	esac

	case "$DEBARCH" in
		armel)
			CROSS_COMPILE=arm-linux-gnueabi-
			;;
		armhf)
			CROSS_COMPILE=arm-linux-gnueabihf-
			;;
		*)
			echo "Unsupported DEBARCH, please specify one of: armel, armhf"
			return 1
			;;
	esac
	export DEBARCH KERNEL_DEFCONFIG KDEB_WBFLAVOUR_DESC CROSS_COMPILE
}
