#!/usr/bin/env bash
# Complete Linux Beta package. Mirrors package.ps1's content layout.
# Usage: ./package-linux.sh [amd64|arm64]

set -euo pipefail
trap 'status=$?; echo "Packaging failed at line ${LINENO} (exit ${status})." >&2; exit "${status}"' ERR

ARCH=${1:-amd64}
if [[ "$ARCH" != "amd64" && "$ARCH" != "arm64" ]]; then
    echo "Unsupported architecture: $ARCH" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SERVER_DIR="$PROJECT_ROOT/server"
WEB_ROOT="${DICENEXT_WEB_ROOT:-$PROJECT_ROOT/../Dice-Next-WebUI}"
DOCS_ROOT="${DICENEXT_DOC_ROOT:-$PROJECT_ROOT/../Dice-Next-Doc}"
[[ -d "$WEB_ROOT" ]] || WEB_ROOT="$PROJECT_ROOT/web"
[[ -d "$DOCS_ROOT" ]] || DOCS_ROOT="$PROJECT_ROOT/docs"
WEB_DIST="$WEB_ROOT/dist"
RELEASE_DIR="$PROJECT_ROOT/release"
BUILD_DIR="build"
TRIPLET="x64-linux-dynamic"
if [[ "$ARCH" == "arm64" ]]; then
    BUILD_DIR="build-arm64"
    TRIPLET="arm64-linux-dynamic"
fi
SERVER_BIN="$SERVER_DIR/$BUILD_DIR/dice-next-server"
VERSION_SOURCE="$SERVER_DIR/$BUILD_DIR/generated/version_build.cpp"

[[ -f "$SERVER_BIN" ]] || { echo "Missing $SERVER_BIN; run ./build-linux.sh $ARCH first" >&2; exit 1; }
[[ -d "$WEB_DIST" ]] || { echo "Missing $WEB_DIST; build Dice-Next-WebUI first or set DICENEXT_WEB_ROOT" >&2; exit 1; }
[[ -d "$DOCS_ROOT" ]] || { echo "Missing documentation project; set DICENEXT_DOC_ROOT" >&2; exit 1; }
[[ -f "$VERSION_SOURCE" ]] || { echo "Missing $VERSION_SOURCE; rebuild the server first" >&2; exit 1; }
[[ "$SERVER_BIN" -nt "$VERSION_SOURCE" ]] || { echo "Server binary is older than its generated build number; rebuild first" >&2; exit 1; }

BUILD=$(sed -nE 's/.*buildNumber\(\) \{ return ([0-9]+); \}.*/\1/p' "$VERSION_SOURCE" | head -n1)
[[ -n "$BUILD" ]] || { echo "Cannot read compiled build number" >&2; exit 1; }
COUNTER=$(sed -nE 's/.*:[[:space:]]*([0-9]+).*/\1/p' "$SERVER_DIR/build_counter.txt" | head -n1)
[[ "$COUNTER" == "$BUILD" ]] || { echo "Build counter ($COUNTER) does not match compiled build ($BUILD)" >&2; exit 1; }

VERSION=$(sed -nE 's/project\(dice-next-server VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$SERVER_DIR/CMakeLists.txt" | head -n1)
VERSION=${VERSION:-3.0.0}
BUILD_PADDED=$(printf '%03d' "$BUILD")
TIMESTAMP=$(date +%Y-%m-%d-%H%M%S)
PACKAGE_NAME="DiceNext-beta-${VERSION}(${BUILD_PADDED})-linux-${ARCH}-${TIMESTAMP}"
STAGE_NAME="DiceNext-beta"
STAGING_DIR="$RELEASE_DIR/$STAGE_NAME"
ARCHIVE="$RELEASE_DIR/$PACKAGE_NAME.tar.gz"

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR" "$RELEASE_DIR"
echo "Packaging $PACKAGE_NAME ..."

install -m 0755 "$SERVER_BIN" "$STAGING_DIR/dice-next-server"
cp -a "$SERVER_DIR/i18n" "$STAGING_DIR/i18n"

DATA_STAGE="$STAGING_DIR/data"
mkdir -p "$DATA_STAGE"
copy_content() {
    local name=$1 source="$SERVER_DIR/data/$1"
    [[ -d "$source" ]] || source="$SERVER_DIR/$name"
    # Optional bundled resources are not present in every source checkout.
    # Their absence must not make a complete package fail under `set -e`.
    if [[ -d "$source" ]]; then
        cp -a "$source" "$DATA_STAGE/$name"
    fi
}
copy_content decks
copy_content rules
copy_content help
copy_content helpdoc
copy_content card-templates
copy_content rulepacks
mkdir -p "$DATA_STAGE/plugins/js"
for demo in seal_demo.js checkin.js cfg_deck_demo.js; do
    source="$SERVER_DIR/data/plugins/js/$demo"
    [[ -f "$source" ]] || source="$SERVER_DIR/plugins/js/$demo"
    [[ -f "$source" ]] && cp -a "$source" "$DATA_STAGE/plugins/js/$demo"
done

mkdir -p "$STAGING_DIR/docs" "$STAGING_DIR/web"
for doc in roadmap.md commands.json; do
    [[ -f "$DOCS_ROOT/$doc" ]] && cp -a "$DOCS_ROOT/$doc" "$STAGING_DIR/docs/$doc"
done
cp -a "$WEB_DIST" "$STAGING_DIR/web/dist"

# The Linux build uses dynamic vcpkg triplets. Ship their shared libraries and
# start the server with a package-local library path so clean machines work.
LIB_STAGE="$STAGING_DIR/lib"
mkdir -p "$LIB_STAGE"
if [[ -n "${VCPKG_ROOT:-}" && -d "$VCPKG_ROOT/installed/$TRIPLET/lib" ]]; then
    shopt -s nullglob
    for library in "$VCPKG_ROOT/installed/$TRIPLET/lib/"*.so*; do cp -a "$library" "$LIB_STAGE/"; done
    shopt -u nullglob
fi

cat > "$STAGING_DIR/start.sh" << 'EOF'
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ROOT/dice-next-server" "$@"
EOF
chmod +x "$STAGING_DIR/start.sh"

cat > "$STAGING_DIR/README.txt" << EOF
DiceNext ${VERSION}(${BUILD_PADDED}) — Linux ${ARCH} Beta

Run:
  1. Extract this archive.
  2. In a terminal, run: ./start.sh
  3. Open the WebUI: http://localhost:18088

Notes:
  - First launch creates the config/ directory automatically.
  - Package-local runtime libraries are loaded by start.sh.
  - Upgrade by replacing the program, lib, i18n, web/dist and data files;
    keep your config and database files.
  - This is a Beta build. Feedback: QQ group 933145116.
EOF

rm -f "$ARCHIVE"
tar -C "$RELEASE_DIR" -czf "$ARCHIVE" "$STAGE_NAME"
rm -rf "$STAGING_DIR"
echo "OK: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
