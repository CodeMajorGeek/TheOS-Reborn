#!/bin/bash

TOOLCHAIN_PATH=$(pwd)

GCC_TAR="gcc-11.2.0.tar.xz"
BIN_TAR="binutils-2.37.tar.xz"

GCC_TAR_URL="https://ftp.gnu.org/gnu/gcc/gcc-11.2.0/$GCC_TAR"
BIN_TAR_URL="https://ftp.gnu.org/gnu/binutils/$BIN_TAR"

TARBALLS_DIR="Tarballs"
GCC_DIR="$TARBALLS_DIR/gcc-11.2.0"
BIN_DIR="$TARBALLS_DIR/binutils-2.37"

ARCH="x86_64"
TARGET="$ARCH-pc-theos"
BUILD="$TOOLCHAIN_PATH/../Build/$ARCH"
SYSROOT="$BUILD/Root"
PREFIX="$TOOLCHAIN_PATH/Local/$ARCH"

TMP_TARGET="$TOOLCHAIN_PATH/tmp/"

if [ -f "$GCC_TAR" ]; then
	echo "GCC sources already downloaded, skipped !"
else

	echo "Let's download the GCC sources..."
	wget $GCC_TAR_URL
	echo "Done !"
fi

if [ -f "$BIN_TAR" ]; then
	echo "Binutils sources already downloaded, skipped !"
else
	echo "Let's download the binutils sources..."
	wget $BIN_TAR_URL
	echo "Done !"
fi

mkdir -p $TARBALLS_DIR
echo "Let's extract the downloaded sources..."
tar -xf $GCC_TAR -C $TARBALLS_DIR
tar -xf $BIN_TAR -C $TARBALLS_DIR
echo "Done !"

echo "Let's apply the patches to gcc and binutils..."

pushd $BIN_DIR
patch -p1 < "$TOOLCHAIN_PATH/Patches/binutils.patch" > /dev/null
popd

pushd $GCC_DIR
patch -p1 < "$TOOLCHAIN_PATH/Patches/gcc.patch" > /dev/null
popd

echo "Done !"

echo "Let's create the prefix dirextory..."
mkdir -p $PREFIX

echo "Let's compile binutils for amd64 cross compiling..."
mkdir -p $BIN_DIR/build
pushd $BIN_DIR/build
../configure --prefix="$PREFIX" \
       	--target="$TARGET" \
       	--with-sysroot="$SYSROOT" \
	--enable-shared \
	--disable-nls || exit 1
make -j "$(nproc)" || exit 1
make install || exit 1
popd
echo "Done !"

echo "Let's compile gcc for amd64 cross compiling..."
mkdir -p $GCC_DIR/build
pushd $GCC_DIR/build
../configure --prefix="$PREFIX" \
	--target="$TARGET" \
	--with-sysroot="$SYSROOT" \
	--disable-nls \
	--with-newlib \
	--enable-shared \
	--enable-languages=c \
	--without-headers \
	--enable-default-pie \
	--enable-lto || exit 1
make all-gcc || exit 1
make -j $(nproc) all-target-libgcc || exit 1
mkdir $TMP_TARGET
make -j $(nproc) DESTDIR="$TMP_TARGET" install-gcc install-target-libgcc || exit 1
cp -a "$TMP_TARGET$PREFIX"/* $PREFIX
rm -rf "$TMP_TARGET"
popd
echo "Done !"

echo "Let's remove all the unecessary files and folders..."
rm $BIN_TAR
rm $GCC_TAR
rm -rf "$TARBALLS_DIR"
echo "Done !"

echo "Finished !"
exit 0
