#!/usr/bin/env bash
# Build CatOfficial for Android (arm64-v8a): native .so + WebUI assets + Gradle APK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing $1" >&2; exit 1; }; }
need cmake
need ninja
need npm
need curl

: "${ANDROID_HOME:=${ANDROID_SDK_ROOT:-}}"
: "${ANDROID_SDK_ROOT:=${ANDROID_HOME:-}}"
: "${VCPKG_ROOT:?Set VCPKG_ROOT}"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
: "${ANDROID_HOME:?Set ANDROID_HOME or ANDROID_SDK_ROOT}"
export ANDROID_HOME ANDROID_SDK_ROOT ANDROID_NDK_HOME
export VCPKG_KEEP_ENV_VARS="ANDROID_NDK_HOME;ANDROID_SDK_ROOT;ANDROID_HOME;JAVA_HOME"

TRIPLET=arm64-android
ABI=arm64-v8a
API=26
BUILD_DIR="$ROOT/build-android"
JNI_DIR="$ROOT/android/app/src/main/jniLibs/$ABI"
WEB_DIR="$ROOT/android/app/src/main/assets/web"
OVERLAY="$ROOT/vcpkg-overlays/triplets"
VCPKG="$VCPKG_ROOT/vcpkg"

echo "JAVA_HOME: ${JAVA_HOME:-}"
echo "SDK: $ANDROID_HOME"
echo "NDK: $ANDROID_NDK_HOME"
echo "vcpkg: $VCPKG_ROOT"

printf 'sdk.dir=%s\n' "${ANDROID_HOME//\\/\\\\}" > "$ROOT/android/local.properties"

"$VCPKG" install --triplet "$TRIPLET" --overlay-triplets "$OVERLAY"

BUILD_NUMBER="${CATOFFICIAL_BUILD:-0}"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCATOFFICIAL_BUILD="$BUILD_NUMBER" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DVCPKG_TARGET_TRIPLET="$TRIPLET" \
    -DVCPKG_OVERLAY_TRIPLETS="$OVERLAY" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_shared \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCATOFFICIAL_BUILD_ANDROID=ON \
    -DCATOFFICIAL_BUILD_APP=OFF \
    -DCATOFFICIAL_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --config Release

mkdir -p "$JNI_DIR"
SO="$(find "$BUILD_DIR" -name 'libcatofficial.so' | head -n1)"
[[ -n "$SO" ]] || { echo "libcatofficial.so not found" >&2; exit 1; }
cp -f "$SO" "$JNI_DIR/libcatofficial.so"
STL="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'libc++_shared.so' | grep aarch64-linux-android | head -n1 || true)"
[[ -n "$STL" ]] && cp -f "$STL" "$JNI_DIR/libc++_shared.so"
STRIP="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'llvm-strip' | head -n1 || true)"
if [[ -n "$STRIP" ]]; then
    find "$JNI_DIR" -name '*.so' -exec "$STRIP" --strip-unneeded {} \;
fi

(
    cd "$ROOT/webui"
    if [[ -d node_modules ]]; then npm ci --prefer-offline; else npm ci; fi
    npm run build
)
rm -rf "$WEB_DIR"
mkdir -p "$WEB_DIR"
cp -a "$ROOT/webui/dist/." "$WEB_DIR/"

CERT="$ROOT/android/app/src/main/assets/cacert.pem"
if [[ ! -f "$CERT" ]]; then
    echo "Downloading CA bundle for native TLS..."
    curl -fsSL "https://curl.se/ca/cacert.pem" -o "$CERT"
fi

chmod +x "$ROOT/android/gradlew"
(
    cd "$ROOT/android"
    ./gradlew --no-daemon assembleRelease
)

APK="$ROOT/android/app/build/outputs/apk/release/app-release.apk"
[[ -f "$APK" ]] || { echo "APK not found: $APK" >&2; exit 1; }
mkdir -p "$ROOT/release"
OUT="$ROOT/release/CatOfficial-android.apk"
cp -f "$APK" "$OUT"
echo "APK: $OUT"
ls -l "$OUT"
