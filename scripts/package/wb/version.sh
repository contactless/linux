#!/bin/sh

INITRAMFS_DIR="/usr/src/wb-initramfs/$KERNEL_FLAVOUR"

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
		wb67-bootlet|wb6x-bootlet)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=imx6_wirenboard_initramfs_defconfig
			BOOTLET_DTB=imx6ul-wirenboard6x-init.dtb
			BOOTLET_DEPS=linux-image-wb6
			KDEB_WBDESC="Wiren Board 6 (bootlet)"
			;;
		wb7)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=wirenboard7_defconfig
			KDEB_WBDESC="Wiren Board 7"
			;;
		wb7-38071cb)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=wirenboard7_38071cb_defconfig
			KDEB_WBDESC="Wiren Board 7 38071cb no modules, no wireless"
			;;
		wb7x-bootlet)
			DEBARCH=armhf
			KERNEL_DEFCONFIG=wirenboard7_initramfs_defconfig
			BOOTLET_DTB=sun8i-r40-wirenboard72x-initram.dtb
			BOOTLET_DEPS=linux-image-wb7
			KDEB_WBDESC="Wiren Board 7 (bootlet)"
			;;
		wb7x-factory-bootlet)
			# overriding INITRAMFS_DIR, initramfs is the same as for wb7x-bootlet
			INITRAMFS_DIR="/usr/src/wb-initramfs/wb7x-bootlet"
			DEBARCH=armhf
			KERNEL_DEFCONFIG=wirenboard7_initramfs_defconfig
			BOOTLET_DTB=sun8i-r40-wirenboard72x-factory.dtb
			BOOTLET_DEPS=
			KDEB_WBDESC="Wiren Board 7 (factory bootlet)"
			;;
		*)
			echo "Unsupported KERNEL_FLAVOUR, please specify one of: wb2, wb6, wb7"
			return 1
	esac

	case "$KERNEL_FLAVOUR" in
		*-bootlet)
			if [ ! -d "$INITRAMFS_DIR" ]; then
				echo "Please install proper wb-bootlet-initramfs-X package"
				return 1
			fi

			INITRAMFS_VERSION="$(cat "$INITRAMFS_DIR/version")"
			;;
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
	export DEBARCH KERNEL_DEFCONFIG KDEB_WBFLAVOUR_DESC CROSS_COMPILE BOOTLET_DTB INITRAMFS_VERSION KDEB_WBDESC BOOTLET_DEPS
}
