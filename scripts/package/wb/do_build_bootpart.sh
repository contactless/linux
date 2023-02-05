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

make_bootpart() {
    local tmpdir_rel="bootpart-tmp"
    local fstmpdir_rel="$tmpdir_rel/fs"
    local fstmpdir="$KBUILD_OUTPUT/$fstmpdir_rel"
    local tmpdir="$KBUILD_OUTPUT/$tmpdir_rel"

    mkdir -p "$tmpdir"

    mkdir -p $fstmpdir
    mkdir -p $fstmpdir/boot
    mkdir -p $fstmpdir/boot/dtbs
    echo "$fstmpdir"
    cp $KBUILD_OUTPUT/arch/arm/boot/zImage "$fstmpdir/boot/zImage"
    cp $KBUILD_OUTPUT/.config "$fstmpdir/boot/config"
    cp $KBUILD_OUTPUT/arch/arm/boot/dts/*wirenboard7*-38071cb.dtb "$fstmpdir/boot/dtbs/"

    echo "$(get_kernel_full_version)" > $fstmpdir/version.txt
    cp debian/changelog $fstmpdir/changelog.txt

    local imgfile="$tmpdir/bootpart.img"
    dd if=/dev/zero bs=1M count=16 of="$imgfile"
    /sbin/mkfs.ext2 $imgfile -L kernel+fdt
    local loopdev=$(sudo losetup -f --show "$imgfile")
    echo "loopdev: $loopdev"
    mkdir -p $tmpdir/mnt
    sudo mount -t auto $loopdev $tmpdir/mnt
    sudo rsync -a $tmpdir/fs/ $tmpdir/mnt/
    sudo umount $tmpdir/mnt
    sudo losetup -d $loopdev
    /sbin/resize2fs -M $imgfile
    ls -lh $imgfile


}

echo "Building kernel packages for $KERNEL_FLAVOUR ($KDEB_WBDESC)"
echo "Revision: $(get_kernel_revision)"
echo "Architecture: ${DEBARCH}"
echo "Config: ${KERNEL_DEFCONFIG}"

make_config
make_image
make_bootpart
