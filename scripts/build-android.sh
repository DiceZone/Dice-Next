#!/usr/bin/env bash
# Build Dice!Next for Android (arm64-v8a): native .so + WebUI assets + Gradle APK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GITHUB="$(cd "$ROOT/.." && pwd)"
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

for dep in onedice-cpp-lib dicescript-c-lib CatOfficial; do
    [[ -d "$GITHUB/$dep" ]] || { echo "Missing sibling repository: $GITHUB/$dep" >&2; exit 1; }
done

TRIPLET=arm64-android
ABI=arm64-v8a
API=26
BUILD_DIR="$ROOT/server/build-android"
JNI_DIR="$ROOT/android/app/src/main/jniLibs/$ABI"
ASSETS="$ROOT/android/app/src/main/assets"
OVERLAY="$ROOT/server/vcpkg-overlays/triplets"
SERVER="$ROOT/server"
VCPKG="$VCPKG_ROOT/vcpkg"

echo "JAVA_HOME: ${JAVA_HOME:-}"
echo "SDK: $ANDROID_HOME"
echo "NDK: $ANDROID_NDK_HOME"
echo "vcpkg: $VCPKG_ROOT"

printf 'sdk.dir=%s\n' "${ANDROID_HOME//\\/\\\\}" > "$ROOT/android/local.properties"

"$VCPKG" install --x-manifest-root="$SERVER" --triplet "$TRIPLET" --overlay-triplets "$OVERLAY"

cmake -S "$SERVER" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DVCPKG_TARGET_TRIPLET="$TRIPLET" \
    -DVCPKG_OVERLAY_TRIPLETS="$OVERLAY" \
    -DVCPKG_MANIFEST_MODE=ON \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_shared \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
cmake --build "$BUILD_DIR" --config Release --target dice-next

mkdir -p "$JNI_DIR"
SO="$(find "$BUILD_DIR" -name 'libdice-next.so' | head -n1)"
[[ -n "$SO" ]] || { echo "libdice-next.so not found" >&2; exit 1; }
cp -f "$SO" "$JNI_DIR/libdice-next.so"
WRAP="$(find "$BUILD_DIR" -name 'libdnexecwrap.so' | head -n1)"
[[ -n "$WRAP" ]] || { echo "libdnexecwrap.so not found" >&2; exit 1; }
cp -f "$WRAP" "$JNI_DIR/libdnexecwrap.so"
STL="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'libc++_shared.so' | grep aarch64-linux-android | head -n1 || true)"
[[ -n "$STL" ]] && cp -f "$STL" "$JNI_DIR/libc++_shared.so"
STRIP="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'llvm-strip' | head -n1 || true)"
if [[ -n "$STRIP" ]]; then
    find "$JNI_DIR" -name '*.so' -exec "$STRIP" --strip-unneeded {} \;
fi

WEB_ROOT="${DICENEXT_WEB_ROOT:-$GITHUB/Dice-Next-WebUI}"
[[ -f "$WEB_ROOT/package.json" ]] || WEB_ROOT="$ROOT/web"
[[ -f "$WEB_ROOT/package.json" ]] || { echo "Dice-Next-WebUI not found" >&2; exit 1; }
(
    cd "$WEB_ROOT"
    if [[ -d node_modules ]]; then npm ci --prefer-offline; else npm ci; fi
    npm run build
)

rm -rf "$ASSETS"
mkdir -p "$ASSETS/web/dist"
cp -a "$WEB_ROOT/dist/." "$ASSETS/web/dist/"
cp -a "$SERVER/i18n" "$ASSETS/i18n"
DEFAULT_DATA="$SERVER/resources/default-data"
cp -a "$DEFAULT_DATA/decks" "$ASSETS/decks"
cp -a "$SERVER/card-templates" "$ASSETS/card-templates"
mkdir -p "$ASSETS/data"
cp -a "$DEFAULT_DATA/helpdoc" "$ASSETS/data/helpdoc"
cp -a "$DEFAULT_DATA/plugins" "$ASSETS/data/plugins"
if [[ -d "$DEFAULT_DATA/rules" ]]; then
    mkdir -p "$ASSETS/i18n/rules"
    cp -a "$DEFAULT_DATA/rules/." "$ASSETS/i18n/rules/"
fi
cp -a "$SERVER/resources/update-mirrors.json" "$ASSETS/update-mirrors.json"

DOCS_ROOT="${DICENEXT_DOC_ROOT:-$GITHUB/Dice-Next-Doc}"
[[ -d "$DOCS_ROOT" ]] || DOCS_ROOT="$ROOT/docs"
if [[ -d "$DOCS_ROOT" ]]; then
    mkdir -p "$ASSETS/docs"
    for doc in roadmap.md commands.json; do
        [[ -f "$DOCS_ROOT/$doc" ]] && cp -a "$DOCS_ROOT/$doc" "$ASSETS/docs/$doc"
    done
fi

if [[ ! -f "$ASSETS/cacert.pem" ]]; then
    echo "Downloading CA bundle for native TLS..."
    curl -fsSL "https://curl.se/ca/cacert.pem" -o "$ASSETS/cacert.pem"
fi

chmod +x "$ROOT/android/gradlew"
(
    cd "$ROOT/android"
    ./gradlew --no-daemon assembleRelease
)

APK="$ROOT/android/app/build/outputs/apk/release/app-release.apk"
[[ -f "$APK" ]] || { echo "APK not found: $APK" >&2; exit 1; }
mkdir -p "$ROOT/release"
OUT="$ROOT/release/DiceNext-android.apk"
cp -f "$APK" "$OUT"
echo "APK: $OUT"
ls -l "$OUT"
