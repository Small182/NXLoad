#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")/.."
repo="$PWD"
deps="$repo/.nxdeps"
prefix="$deps/install"
mkdir -p "$deps" "$prefix"

checkout() {
    local name="$1" url="$2" ref="$3" target="$deps/$1"
    if [ ! -d "$target/.git" ]; then
        git init -q "$target"
    fi
    if [ "$(git -C "$target" rev-parse HEAD 2>/dev/null || true)" != "$ref" ]; then
        git -C "$target" fetch --depth 1 "$url" "$ref"
        git -C "$target" checkout --detach FETCH_HEAD
    fi
    if [ "$name" = mbedtls ]; then
        git -C "$target" submodule update --init --recursive --depth 1
    fi
}

checkout mbedtls https://github.com/Mbed-TLS/mbedtls.git 068ff080b369adfac81509f9b57b2afabaf82dc5
checkout curl https://github.com/curl/curl.git 3163ec065643ba20709f190111f42c3e1fe95273

source "$DEVKITPRO/switchvars.sh"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$PORTLIBS_PREFIX/lib/pkgconfig"

config="$deps/mbedtls/include/mbedtls/mbedtls_config.h"
config_py="$deps/mbedtls/scripts/config.py"
python3 "$config_py" -f "$config" set MBEDTLS_ENTROPY_HARDWARE_ALT
python3 "$config_py" -f "$config" set MBEDTLS_NO_PLATFORM_ENTROPY
python3 "$config_py" -f "$config" set MBEDTLS_PLATFORM_MS_TIME_ALT
python3 "$config_py" -f "$config" set MBEDTLS_THREADING_C
python3 "$config_py" -f "$config" set MBEDTLS_THREADING_ALT
cp "$repo/tools/threading_alt.h" "$deps/mbedtls/include/mbedtls/threading_alt.h"
python3 "$config_py" -f "$config" unset MBEDTLS_NET_C
python3 "$config_py" -f "$config" unset MBEDTLS_TIMING_C
for option in MBEDTLS_SSL_PROTO_DTLS MBEDTLS_SSL_DTLS_CONNECTION_ID \
    MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT MBEDTLS_SSL_DTLS_ANTI_REPLAY \
    MBEDTLS_SSL_DTLS_HELLO_VERIFY MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE; do
    python3 "$config_py" -f "$config" unset "$option"
done

aarch64-none-elf-cmake -S "$deps/mbedtls" -B "$deps/mbedtls-build" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build "$deps/mbedtls-build" --parallel 4
cmake --install "$deps/mbedtls-build"

cp "$PORTLIBS_PREFIX/include/zlib.h" "$PORTLIBS_PREFIX/include/zconf.h" "$prefix/include/"
CFLAGS="-I$prefix/include $CFLAGS" \
aarch64-none-elf-cmake -S "$deps/curl" -B "$deps/curl-build" \
    -DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DHTTP_ONLY=ON -DCURL_DISABLE_MIME=ON -DENABLE_IPV6=OFF -DENABLE_UNIX_SOCKETS=OFF -DCURL_DISABLE_SOCKETPAIR=ON \
    -DENABLE_THREADED_RESOLVER=OFF -DENABLE_ARES=OFF -DUSE_NGHTTP2=OFF \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=ON \
    -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF \
    -DZLIB_INCLUDE_DIR="$prefix/include" -DZLIB_LIBRARY="$PORTLIBS_PREFIX/lib/libz.a" \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=ON \
    -DCURL_CA_BUNDLE=romfs:/cacert.pem -DCURL_CA_PATH=none \
    -DMBEDTLS_INCLUDE_DIR="$prefix/include" \
    -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.a" \
    -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.a"
cmake --build "$deps/curl-build" --parallel 4
cmake --install "$deps/curl-build"

mkdir -p "$repo/romfs/licenses"
cp "$deps/curl/COPYING" "$repo/romfs/licenses/curl.txt"
cp "$deps/mbedtls/LICENSE" "$repo/romfs/licenses/mbedtls.txt"
