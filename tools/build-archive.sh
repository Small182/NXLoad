#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")/.."

version=3.8.11
checksum=a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2
deps="$PWD/.nxdeps"
prefix="$deps/install"
source_dir="$deps/libarchive-$version"
build_dir="$deps/libarchive-build"
tarball="$deps/libarchive-$version.tar.xz"
mkdir -p "$deps" "$prefix"

if ! printf '%s  %s\n' "$checksum" "$tarball" | sha256sum --check --status; then
    curl --fail --silent --show-error --location --retry 3 \
        "https://libarchive.org/downloads/libarchive-$version.tar.xz" -o "$tarball"
    printf '%s  %s\n' "$checksum" "$tarball" | sha256sum --check
fi
tar -xJf "$tarball" -C "$deps"

# Newlib exposes _timezone on Switch.
sed -i '/^#elif defined(__CYGWIN__) || defined(__MINGW32__) || defined(__BORLANDC__)$/s/$/ || defined(__SWITCH__)/' \
    "$source_dir/libarchive/archive_write_set_format_iso9660.c"

source "$DEVKITPRO/switchvars.sh"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$PORTLIBS_PREFIX/lib/pkgconfig"

aarch64-none-elf-cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
    -DENABLE_TEST=OFF -DBUILD_TESTING=OFF -DENABLE_OPENSSL=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF \
    -DENABLE_LIBXML2=OFF -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DHAVE_WAITPID=OFF \
    -DLIBLZMA_INCLUDE_DIR="$PORTLIBS_PREFIX/include" -DLIBLZMA_LIBRARY="$PORTLIBS_PREFIX/lib/liblzma.a" \
    -DZSTD_INCLUDE_DIR="$PORTLIBS_PREFIX/include" -DZSTD_LIBRARY="$PORTLIBS_PREFIX/lib/libzstd.a"

grep -q '^#define HAVE_LZMA_H 1$' "$build_dir/config.h"
grep -q '^#define HAVE_ZSTD_H 1$' "$build_dir/config.h"
cmake --build "$build_dir" --parallel 4
cmake --install "$build_dir"

mkdir -p romfs/licenses
cp "$source_dir/COPYING" romfs/licenses/libarchive.txt
