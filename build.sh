#!/bin/bash

# HOME path
export HOME=/home/rajan

# Compiler environment
export CLANG_PATH=$HOME/android/pe/prebuilts/clang/host/linux-x86/clang-r416183b1/bin
export PATH="$CLANG_PATH:/home/rajan/android/pe/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin:$PATH"
export CROSS_COMPILE=aarch64-linux-android-
export CROSS_COMPILE_ARM32=arm-linux-androidkernel-

echo
echo "Setting defconfig"
echo

make mrproper
make ARCH=arm64 vendor/sm8150-perf_defconfig
make ARCH=arm64 savedefconfig
