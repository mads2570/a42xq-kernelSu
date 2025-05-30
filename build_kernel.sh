#!/bin/bash

# Set the correct Clang binary location (your provided path)
export CLANG="$HOME/linux-x86-refs_heads_android14-release/clang-r487747c/bin"

# Set the GCC binary location
export GCC64="$HOME/aosp-clang/aarch64-linux-android-4.9/bin"

# Ensure Clang and GCC directories exist before adding them to PATH
if [ -d "$CLANG" ]; then
    export PATH="$CLANG:$PATH"
else
    echo "Clang directory $CLANG not found. Please check your path."
    exit 1
fi

if [ -d "$GCC64" ]; then
    export PATH="$GCC64:$PATH"
else
    echo "GCC directory $GCC64 not found. Please check your path."
   exit 1
fi

# Set architecture to ARM64 (or modify for your platform)
export ARCH=arm64
export CLANG_TRIPLE="aarch64-linux-gnu-"
export CROSS_COMPILE="aarch64-linux-android-"
export KERNEL_MAKE_ENV="DTC_EXT=$(pwd)/tools/dtc CONFIG_BUILD_ARM64_DT_OVERLAY=y"

# Set up ccache to speed up compilation
export PATH="/usr/lib/ccache:$PATH"

# Set the output directory for kernel build
output_dir="out"

# Set the current date (for versioning or logs)
date=$(date +"%Y%m%d%H%M")
echo "date=$date"  # Log the date to the console instead of $GITHUB_ENV

# Print a message about the build process
echo "Building kernel with Clang for architecture $ARCH"

# Choose the compiler: set the CC variable dynamically
# Default to Clang, but allow override from environment or arguments
if [ "$1" == "gcc" ]; then
    export CC="gcc"
    echo "Using GCC as the compiler."
else
    export CC="clang"  # Default to Clang if not explicitly set to GCC
    echo "Using Clang as the compiler."
fi

# Clean the kernel source directory before starting the build
echo "Cleaning the kernel source with make mrproper..."
make -C "$(pwd)" mrproper

# Run the make command to configure the kernel
echo "Running kernel make process..."
make -C "$(pwd)" O="$output_dir" $KERNEL_MAKE_ENV LLVM=1 LLVM_IAS=1 CC="ccache $CC" CLANG_TRIPLE="$CLANG_TRIPLE" vendor/m23xq_eur_open_defconfig

# Uncomment this if you need to adjust the kernel configuration manually
# make -C "$(pwd)" O="$output_dir" $KERNEL_MAKE_ENV LLVM=1 LLVM_IAS=1 CC="ccache $CC" CLANG_TRIPLE="$CLANG_TRIPLE" menuconfig

# Compile the kernel using the selected compiler (Clang or GCC)
echo "Compiling the kernel..."
make -C "$(pwd)" O="$output_dir" $KERNEL_MAKE_ENV LLVM=1 LLVM_IAS=1 CC="ccache $CC" -j"$(nproc)" CONFIG_DEBUG_SECTION_MISMATCH=y

# Check if device tree overlays (DTBO) are available, then create the DTBO image
DTBO_DIR="$(pwd)/out/arch/arm64/boot/dts/samsung/m23/m23xq/"
DTBO_FILES=$(find "$DTBO_DIR" -name "*.dtbo")

if [ -n "$DTBO_FILES" ]; then
    echo "Creating DTBO image..."
    "$(pwd)/tools/mkdtimg" create "$(pwd)/out/arch/arm64/boot/dtbo.img" --page_size=4096 $DTBO_FILES
else
    echo "No .dtbo files found. Skipping DTBO image creation."
fi

echo "Kernel build process completed."
