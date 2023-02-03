#!/bin/bash

START=$(date +"%s")
VERSION_NAME="v1.4"
ZIPNAME="Classified-${VERSION_NAME}-$(date '+%Y%m%d-%H%M')"

export LOCALVERSION="-${VERSION_NAME}"

# DEFCONFIG NAME
DEFCONFIG="vendor/sm8150-perf_defconfig"

# HOME PATH
export HOME=/home/rajanpalaniya

# ARCH
export ARCH=arm64
export SUBARCH=arm64

# Compiler environment
export CLANG_PATH=$HOME/android/pe/prebuilts/clang/host/linux-x86/clang-r450784d/bin
export GCC_PATH=$HOME/android/pe/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin
export ARM_PATH=$HOME/android/pe/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin
export PATH="$CLANG_PATH:$PATH:$GCC_PATH:$ARM_PATH"
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-android-
export CROSS_COMPILE_ARM32=arm-linux-androideabi-

# USER
export KBUILD_BUILD_USER=Rajan
export KBUILD_BUILD_HOST=Classified

echo
echo "Perpareing environment"
echo

make mrproper
rm -rf out
rm -rf kernelzip
rm -rf *.zip
rm -rf scripts/AnyKernel3

if ! git clone -q https://github.com/classified/AnyKernel3 -b seven scripts/AnyKernel3; then
	echo -e "\nAnyKernel3 repo not found locally and cloning failed! Aborting..."
	exit 1
fi

if [[ $1 = "-r" || $1 = "--regen" ]]; then
	make O=out ARCH=arm64 $DEFCONFIG savedefconfig
	cp out/defconfig arch/arm64/configs/$DEFCONFIG
	echo -e "\nSuccessfully regenerated defconfig at $DEFCONFIG"
	exit
fi

echo
echo "Setting defconfig"
echo

make O=out CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip BRAND_SHOW_FLAG=oneplus TARGET_PRODUCT=msmnile $DEFCONFIG

echo
echo "Compiling kernel"
echo

make O=out CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip BRAND_SHOW_FLAG=oneplus TARGET_PRODUCT=msmnile -j$(nproc --all) || exit 1

kernel="out/arch/arm64/boot/Image.gz"
dtb="out/arch/arm64/boot/dtb.img"
dtbo="out/arch/arm64/boot/dtbo.img"

# Kernel Output
if [ -f "$kernel" ] && [ -f "$dtb" ] && [ -f "$dtbo" ]; then
	echo
	echo "Building Kernel Package"
	echo
	rm -rf scripts/AnyKernel3/modules
	rm $ZIPNAME.zip 2>/dev/null
	rm -rf kernelzip 2>/dev/null
	# Import Anykernel3 folder
	mkdir kernelzip
	cp -rp scripts/AnyKernel3/* kernelzip/
	cp $kernel $dtbo kernelzip/
	cp $dtb kernelzip/dtb
	cd kernelzip/
	7z a -mx9 $ZIPNAME-tmp.zip *
	zipalign -v 4 $ZIPNAME-tmp.zip ../$ZIPNAME.zip
	rm $ZIPNAME-tmp.zip
	cd ..
	ls -al $ZIPNAME.zip
else
	echo "\nCompilation failed!"
	exit 1
fi

# Show compilation time
END=$(date +"%s")
DIFF=$((END - START))
echo "Kernel compiled successfully in $((DIFF / 60)) minute(s) and $((DIFF % 60)) second(s)"