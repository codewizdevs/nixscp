#!/bin/bash
# Builds OpenSSL and libssh for Android (arm64-v8a and x86_64) with the NDK
# into android-sdk/deps/<abi>, for NixSCP and for Qt's TLS backend (which
# loads libssl.so / libcrypto.so from the app when the _3 names are absent).
# Needs packaging/android/setup-sdk.sh first.
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${ANDROID_SDK_DIR:-$SOURCE_DIR/android-sdk}"
# shellcheck disable=SC1091
source "$SDK/env.sh"
OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
LIBSSH_VERSION=0.11.5
LIBSSH_SHA256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c
API=28                       # minimum Android 9
ABIS="${ABIS:-arm64-v8a x86_64}"

SRC="$SDK/deps-src"
mkdir -p "$SRC" && cd "$SRC"
[ -f openssl.tar.gz ] || curl -fsSL -o openssl.tar.gz "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
[ -f libssh.tar.xz ] || curl -fsSL -o libssh.tar.xz "https://www.libssh.org/files/${LIBSSH_VERSION%.*}/libssh-$LIBSSH_VERSION.tar.xz"
echo "$OPENSSL_SHA256  openssl.tar.gz" | sha256sum -c - >/dev/null
echo "$LIBSSH_SHA256  libssh.tar.xz" | sha256sum -c - >/dev/null

TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
for ABI in $ABIS; do
  case "$ABI" in
    arm64-v8a) TARGET=android-arm64 ;;
    x86_64) TARGET=android-x86_64 ;;
    *) echo "unknown ABI $ABI"; exit 1 ;;
  esac
  PREFIX="$SDK/deps/$ABI"
  if [ ! -f "$PREFIX/lib/libcrypto.so" ]; then
    echo "build-deps: OpenSSL $OPENSSL_VERSION for $ABI"
    rm -rf "openssl-$ABI" && mkdir "openssl-$ABI" && tar xf openssl.tar.gz -C "openssl-$ABI" --strip-components=1
    # Android has no versioned sonames: the libraries are libcrypto.so / libssl.so
    (cd "openssl-$ABI" && PATH="$TOOLCHAIN/bin:$PATH" ./Configure "$TARGET" -U__ANDROID_API__ -D__ANDROID_API__=$API \
        --prefix="$PREFIX" --libdir=lib shared no-tests no-docs no-ui-console >/dev/null \
      && PATH="$TOOLCHAIN/bin:$PATH" make -j"$(nproc)" build_libs >/dev/null 2>&1 \
      && PATH="$TOOLCHAIN/bin:$PATH" make install_dev >/dev/null 2>&1)
    rm -f "$PREFIX"/lib/*.a
    ls "$PREFIX/lib/"
  fi
  if [ ! -f "$PREFIX/lib/libssh.so" ]; then
    echo "build-deps: libssh $LIBSSH_VERSION for $ABI"
    rm -rf "libssh-$LIBSSH_VERSION" "libssh-$ABI" && tar xf libssh.tar.xz
    cmake -S "libssh-$LIBSSH_VERSION" -B "libssh-$ABI" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-$API \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=FALSE \
      -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.so" -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.so" \
      -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
      -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF -DWITH_GSSAPI=OFF -DWITH_SERVER=OFF \
      "-DCMAKE_C_FLAGS=-DGLOB_TILDE=0 -DS_IWRITE=S_IWUSR" >/dev/null 2>&1
    # bionic has no GLOB_TILDE (~ in config Include) nor the old S_IWRITE name
    cmake --build "libssh-$ABI" >/dev/null
    cmake --install "libssh-$ABI" >/dev/null
  fi
done
echo "build-deps: done"
for ABI in $ABIS; do ls "$SDK/deps/$ABI/lib" | tr '\n' ' '; echo; done
