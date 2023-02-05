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
export DEBFULLNAME="Wirenboard robot"

make_deb () {
    fakeroot make -j${CORES} ARCH=arm KBUILD_DEBARCH=${DEBARCH} \
        LOCALVERSION=$(get_kernel_revision) WB_VERSION_SUFFIX=${VERSION_SUFFIX} \
        KDEB_WBTARGET=${KERNEL_FLAVOUR} binwbdeb-pkg
}

echo "Building kernel packages for $KERNEL_FLAVOUR ($KDEB_WBDESC)"
echo "Revision: $(get_kernel_revision)"
echo "Architecture: ${DEBARCH}"
echo "Config: ${KERNEL_DEFCONFIG}"

make_config
make_image
make_deb
