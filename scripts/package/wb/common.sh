#!/bin/sh

source scripts/package/wb/version.sh

export DEBEMAIL="info@wirenboard.com"
export DEBFULLNAME="Wirenboard robot"

init_build_dir() {
    setup_kernel_vars || exit $?
    export KBUILD_OUTPUT=".build-$KERNEL_FLAVOUR"
    mkdir -p "$KBUILD_OUTPUT"
}

check_wb_changelog() {
    if [ ! -f debian/changelog ]; then
        echo "Can't find debian/changelog, aborting" >&2
        exit 2
    fi
}

get_kernel_full_version() {
    check_wb_changelog

    local changelog_top=$(head -1 debian/changelog)
    echo $changelog_top | sed 's/.*(\(.*\)).*/\1/'
}

get_kernel_revision() {
    check_wb_changelog

    local changelog_top=$(head -1 debian/changelog)
    local packageversion="$(echo $changelog_top | sed 's/.*(\(.*\)).*/\1/')${WB_VERSION_SUFFIX}"

    case $packageversion in
    *-*)
        local revision=${packageversion##*-}
        ;;
    *)
        echo "Package version must be in format '${version}-<revision>'" >&2
        exit 2
        ;;
    esac

    echo "-${revision}"
}

make_config() {
    if [[ -t 0 && -e "${KBUILD_OUTPUT}/.config" ]]; then
        echo ".config already present"
        if [[ -n "$FORCE_DEFAULT" ]]; then
            echo "Forced using $KERNEL_DEFCONFIG: $FORCE_DEFAULT"
            yn=$FORCE_DEFAULT
        else
            read -n 1 -p "Use $KERNEL_DEFCONFIG instead? (y/N) " yn
            echo
        fi
        if [[ "$yn" == "y" ]]; then
            make ARCH=arm $KERNEL_DEFCONFIG
        else
            echo "Using existing .config"
        fi
    else
        make ARCH=arm $KERNEL_DEFCONFIG
    fi
}

make_image() {
    make -j${CORES} LOCALVERSION=$(get_kernel_revision) ARCH=arm KBUILD_DEBARCH=${DEBARCH} zImage modules dtbs
}
