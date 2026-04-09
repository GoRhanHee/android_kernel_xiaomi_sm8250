#!/bin/bash

# Import KernelSU-Next
curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash -s legacy

# Define toolchain variables
CLANG_DIR=$PWD/toolchain/clang_14
PATH=$CLANG_DIR/bin:$PATH

# Check if toolchain exists
if [ ! -f "$CLANG_DIR/bin/clang-14" ]; then
    echo "-----------------------------------------------"
    echo "Toolchain not found! Downloading..."
    echo "-----------------------------------------------"
    rm -rf $CLANG_DIR
    mkdir -p $CLANG_DIR
    pushd $CLANG_DIR > /dev/null
    wget -q --show-progress https://github.com/GoRhanHee/android_kernel_xiaomi_sm8250/releases/download/toolchain/clang-r445002.tar.gz
    tar xf clang-r445002.tar.gz 
    rm clang-r445002.tar.gz 
    echo "Cleaning up..."
    popd > /dev/null
fi

MAKE_ARGS="
LLVM=1 \
LLVM_IAS=1 \
ARCH=arm64 \
O=out
"

make ${MAKE_ARGS} -j24 alioth_gorhanhee_defconfig || exit 1
make ${MAKE_ARGS} -j24 || exit 1

# Cooking boot.img
mkdir prebuilts/output
chmod +x ${ANDROID_BUILD_TOP}/prebuilts/*
cd ${ANDROID_BUILD_TOP}/prebuilts

unzip -jo ${ANDROID_BUILD_TOP}/prebuilts/boot.zip boot.img -d ${ANDROID_BUILD_TOP}/prebuilts/
./magiskboot unpack boot.img
cp ${ANDROID_BUILD_TOP}/out/arch/arm64/boot/Image ${ANDROID_BUILD_TOP}/prebuilts/kernel
./magiskboot repack boot.img
cp ${ANDROID_BUILD_TOP}/prebuilts/new-boot.img ${ANDROID_BUILD_TOP}/prebuilts/output/boot.img