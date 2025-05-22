#!/bin/sh

INITRAMFS_DIR="/usr/src/wb-initramfs/$KERNEL_FLAVOUR"

setup_kernel_vars() {
	case "$KERNEL_FLAVOUR" in
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
			BOOTLET_DTB=allwinner/imx6ul-wirenboard6x-init.dtb
			BOOTLET_DEPS=linux-image-wb6
			PROVIDES_BOOTLET_FOR_FITS=y
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
			BOOTLET_DTB=allwinner/sun8i-r40-wirenboard72x-initram.dtb
			BOOTLET_DEPS=linux-image-wb7
			PROVIDES_BOOTLET_FOR_FITS=y
			KDEB_WBDESC="Wiren Board 7 (bootlet)"
			;;
		wb7x-factory-bootlet)
			# overriding INITRAMFS_DIR, initramfs is the same as for wb7x-bootlet
			INITRAMFS_DIR="/usr/src/wb-initramfs/wb7x-bootlet"
			DEBARCH=armhf
			KERNEL_DEFCONFIG=wirenboard7_initramfs_defconfig
			BOOTLET_DTB=allwinner/sun8i-r40-wirenboard72x-factory.dtb
			PROVIDES_BOOTLET_FOR_FITS=  # used in production, no need to install it on boards
			BOOTLET_DEPS=
			KDEB_WBDESC="Wiren Board 7 (factory bootlet)"
			;;
		wb8)
			DEBARCH=arm64
			KERNEL_DEFCONFIG="defconfig wb8.config"
			KDEB_WBDESC="Wiren Board 8"
			;;
		wb8x-bootlet)
			INITRAMFS_DIR="/usr/src/wb-initramfs/wb8x-bootlet"
			DEBARCH=arm64
			KERNEL_DEFCONFIG="defconfig wb8-bootlet.config"
			BOOTLET_DTB=allwinner/sun50i-h616-wirenboard8-bootlet.dtb
			PROVIDES_BOOTLET_FOR_FITS=y
			BOOTLET_DEPS=linux-image-wb8
			KDEB_WBDESC="Wiren Board 8 (bootlet)"
			;;
		*)
			echo "Unsupported KERNEL_FLAVOUR, please specify one of: wb6, wb7, wb8, wb8x-bootlet"
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
			KERNEL_ARCH=arm
			KERNEL_IMAGE=zImage
			;;
		armhf)
			CROSS_COMPILE=arm-linux-gnueabihf-
			KERNEL_ARCH=arm
			KERNEL_IMAGE=zImage
			;;
		arm64)
			CROSS_COMPILE=aarch64-linux-gnu-
			KERNEL_ARCH=arm64
			KERNEL_IMAGE=Image.gz
			;;
		*)
			echo "Unsupported DEBARCH, please specify one of: armel, armhf, arm64"
			return 1
			;;
	esac
	export DEBARCH KERNEL_ARCH KERNEL_IMAGE KERNEL_DEFCONFIG KDEB_WBFLAVOUR_DESC CROSS_COMPILE BOOTLET_DTB INITRAMFS_VERSION KDEB_WBDESC BOOTLET_DEPS PROVIDES_BOOTLET_FOR_FITS
}
