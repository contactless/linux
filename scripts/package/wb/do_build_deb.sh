#!/bin/bash -e
#
#

[[ -e .git ]] || {
    echo "This script must be started in kernel root directory" >&2
    exit 2
}

CORES_DEFAULT=$(nproc || echo 1)
CORES=${CORES:-$CORES_DEFAULT}

case $1 in
*help)
    echo "Usage: KERNEL_FLAVOUR=<flavour> $0" >&2
    echo -e "\nOptional envvars:"
    echo -e "\tCORES\tNumber of threads to build kernel (default $CORES)"
    echo -e "\tFORCE_DEFAULT\tDefault answer about using exising defconfig (y/n)"
    echo -e "\tVERSION_SUFFIX\tCustom version suffix for non-dev branches"
    exit
    ;;
esac


source scripts/package/wb/common.sh
init_build_dir

export DEBEMAIL="info@wirenboard.com"
export DEBFULLNAME="Wiren Board robot"

make_bootlet() {
    # Replace initramfs path in kernel config
    sed -i "s|CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE=\"initramfs.cpio\"|" "$KBUILD_OUTPUT/.config"
    # Prepare initramfs
    zcat "$INITRAMFS_DIR/initramfs.cpio.gz" > "$KBUILD_OUTPUT/initramfs.cpio"


    set -x
    INSTALL_MOD_PATH="initramfs-modules"
    mkdir -p "$KBUILD_OUTPUT/$INSTALL_MOD_PATH"

    make -j"$CORES" LOCALVERSION="$(get_kernel_revision)" ARCH=arm KBUILD_DEBARCH="${DEBARCH}" \
        INSTALL_MOD_PATH="$INSTALL_MOD_PATH" all modules

    make -j"$CORES" LOCALVERSION="$(get_kernel_revision)" ARCH=arm KBUILD_DEBARCH="${DEBARCH}" \
        INSTALL_MOD_PATH="$INSTALL_MOD_PATH" modules_install

    local CPIO_PATH
    CPIO_PATH="$(realpath --relative-to="$KBUILD_OUTPUT/$INSTALL_MOD_PATH" "$KBUILD_OUTPUT/initramfs.cpio")"
    { pushd "$KBUILD_OUTPUT/$INSTALL_MOD_PATH" >/dev/null; find . -mindepth 1 | fakeroot cpio -oA -H newc -F "$CPIO_PATH"; popd >/dev/null; }

    make -j"$CORES" LOCALVERSION="$(get_kernel_revision)" ARCH=arm KBUILD_DEBARCH="${DEBARCH}" zImage dtbs
    set +x
}

make_deb () {
    fakeroot make -j"$CORES" ARCH=arm KBUILD_DEBARCH="$DEBARCH" \
        LOCALVERSION="$(get_kernel_revision)" WB_VERSION_SUFFIX="$VERSION_SUFFIX" \
        KDEB_WBTARGET="$KERNEL_FLAVOUR" KDEB_WBDESC="$KDEB_WBDESC" \
        BOOTLET_DTB="$BOOTLET_DTB" INITRAMFS_VERSION="$INITRAMFS_VERSION" binwbdeb-pkg
}

make_bootlet_deb() {
    local DEBDIR="$KBUILD_OUTPUT/bootlet-deb"
    local BOOTLET_DIR="$DEBDIR/var/lib/wb-image-update"
    local TARGET_NAME="${KERNEL_FLAVOUR%-bootlet}"
    local PACKAGE_NAME="wb-bootlet-$TARGET_NAME"
    mkdir -p "$BOOTLET_DIR"

    cp "$KBUILD_OUTPUT/arch/arm/boot/zImage" "$BOOTLET_DIR/zImage"
    cp "$KBUILD_OUTPUT/arch/arm/boot/dts/$BOOTLET_DTB" "$BOOTLET_DIR/boot.dtb"

    rm -f "$PACKAGE_NAME"*.deb || true
    fpm -s dir -t deb -n "$PACKAGE_NAME" -v "$(get_kernel_full_version)${VERSION_SUFFIX}-fs${INITRAMFS_VERSION}" \
        --architecture "$DEBARCH" \
        --description "FIT bootlet images, $KDEB_WBDESC" \
        --maintainer "$DEBFULLNAME <$DEBEMAIL>" \
        --url "https://github.com/wirenboard/linux" \
        --deb-no-default-config-files \
        --deb-priority optional \
        --provides "wb-bootlet" \
        --replaces "wb-bootlet" \
        --conflicts "wb-bootlet" \
        --depends "$BOOTLET_DEPS" \
        -C "$DEBDIR" .
}

echo "Building kernel packages for $KERNEL_FLAVOUR ($KDEB_WBDESC)"
echo "Revision: $(get_kernel_revision)"
echo "Architecture: ${DEBARCH}"
echo "Config: ${KERNEL_DEFCONFIG}"
if [ -n "$BOOTLET_DTB" ]; then
    echo "Bootlet DTB: ${BOOTLET_DTB}"
    echo "Initramfs version: ${INITRAMFS_VERSION}"
fi

make_config

if [ -n "$BOOTLET_DTB" ]; then
    make_bootlet
    make_bootlet_deb
else
    make_image
    make_deb
fi
