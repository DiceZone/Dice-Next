#!/usr/bin/env bash
# Complete macOS ARM64 Beta package. Usage: ./package-macos.sh

set -euo pipefail
trap 'status=$?; echo "Packaging failed at line ${LINENO} (exit ${status})." >&2; exit "${status}"' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SERVER_DIR="$PROJECT_ROOT/server"
WEB_ROOT="${DICENEXT_WEB_ROOT:-$PROJECT_ROOT/../Dice-Next-WebUI}"
DOCS_ROOT="${DICENEXT_DOC_ROOT:-$PROJECT_ROOT/../Dice-Next-Doc}"
[[ -d "$WEB_ROOT" ]] || WEB_ROOT="$PROJECT_ROOT/web"
[[ -d "$DOCS_ROOT" ]] || DOCS_ROOT="$PROJECT_ROOT/docs"
WEB_DIST="$WEB_ROOT/dist"
DEFAULT_DATA="$SERVER_DIR/resources/default-data"
RELEASE_DIR="$PROJECT_ROOT/release"
SERVER_BIN="$SERVER_DIR/build/dice-next-server"
VERSION_SOURCE="$SERVER_DIR/build/generated/version_build.cpp"
TRIPLET="arm64-osx"

[[ -f "$SERVER_BIN" ]] || { echo "Missing $SERVER_BIN" >&2; exit 1; }
[[ -d "$WEB_DIST" ]] || { echo "Missing $WEB_DIST; build Dice-Next-WebUI first or set DICENEXT_WEB_ROOT" >&2; exit 1; }
[[ -d "$DOCS_ROOT" ]] || { echo "Missing documentation project; set DICENEXT_DOC_ROOT" >&2; exit 1; }
for required in decks helpdoc plugins/js; do
    [[ -d "$DEFAULT_DATA/$required" ]] || { echo "Missing bundled resource: server/resources/default-data/$required" >&2; exit 1; }
done
for demo in seal_demo.js checkin.js cfg_deck_demo.js; do
    [[ -f "$DEFAULT_DATA/plugins/js/$demo" ]] || { echo "Missing bundled example plugin: $demo" >&2; exit 1; }
done
[[ -f "$VERSION_SOURCE" ]] || { echo "Missing $VERSION_SOURCE" >&2; exit 1; }
[[ "$SERVER_BIN" -nt "$VERSION_SOURCE" ]] || { echo "Server binary is older than generated build number" >&2; exit 1; }

BUILD=$(sed -nE 's/.*buildNumber\(\) \{ return ([0-9]+); \}.*/\1/p' "$VERSION_SOURCE" | head -n1)
COUNTER=$(sed -nE 's/.*:[[:space:]]*([0-9]+).*/\1/p' "$SERVER_DIR/build_counter.txt" | head -n1)
[[ -n "$BUILD" && "$COUNTER" == "$BUILD" ]] || { echo "Build counter and compiled build differ" >&2; exit 1; }
VERSION=$(sed -nE 's/project\(dice-next-server VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$SERVER_DIR/CMakeLists.txt" | head -n1)
VERSION=${VERSION:-3.0.0}
BUILD_PADDED=$(printf '%03d' "$BUILD")
TIMESTAMP=$(date +%Y-%m-%d-%H%M%S)
PACKAGE_NAME="DiceNext-beta-${VERSION}(${BUILD_PADDED})-macos-arm64-${TIMESTAMP}"
STAGE_NAME="DiceNext-beta"
STAGING_DIR="$RELEASE_DIR/$STAGE_NAME"
ARCHIVE="$RELEASE_DIR/$PACKAGE_NAME.tar.gz"

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR" "$RELEASE_DIR"
install -m 0755 "$SERVER_BIN" "$STAGING_DIR/dice-next-server"
cp -a "$SERVER_DIR/i18n" "$STAGING_DIR/i18n"

# Bundled defaults come only from the tracked resources tree. Never package
# server/data: it is ignored runtime state and may contain private user data.
cp -a "$DEFAULT_DATA/decks" "$STAGING_DIR/decks"
DATA_STAGE="$STAGING_DIR/data"
mkdir -p "$DATA_STAGE"
cp -a "$DEFAULT_DATA/helpdoc" "$DATA_STAGE/helpdoc"
cp -a "$DEFAULT_DATA/plugins" "$DATA_STAGE/plugins"
# Built-in card templates live outside data so user overrides remain separate.
cp -a "$SERVER_DIR/card-templates" "$STAGING_DIR/card-templates"
mkdir -p "$STAGING_DIR/docs" "$STAGING_DIR/web"
for doc in roadmap.md commands.json; do
    [[ -f "$DOCS_ROOT/$doc" ]] && cp -a "$DOCS_ROOT/$doc" "$STAGING_DIR/docs/$doc"
done
cp -a "$WEB_DIST" "$STAGING_DIR/web/dist"

# macOS test packages use the static vcpkg triplet.  Keep the distribution to a
# single application executable: Gatekeeper should only need to assess that one
# file, not every bundled third-party dylib.  System frameworks remain dynamic
# and are trusted by macOS.  Refuse to create a misleading "single file" package
# if a vcpkg dylib was linked by accident.
if command -v otool >/dev/null 2>&1; then
    external_libs="$(otool -L "$SERVER_BIN" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/Library/)' || true)"
    if [[ -n "$external_libs" ]]; then
        echo "Unexpected non-system dynamic dependencies in static macOS build:" >&2
        echo "$external_libs" >&2
        exit 1
    fi
fi

cat > "$STAGING_DIR/start.sh" << 'EOF'
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$ROOT/dice-next-server" "$@"
EOF
chmod +x "$STAGING_DIR/start.sh"

cat > "$STAGING_DIR/README.txt" << EOF
DiceNext ${VERSION}(${BUILD_PADDED}) — macOS ARM64 Beta

Run:
  1. Extract this archive.
  2. In a terminal, run: ./start.sh
  3. Open the WebUI: http://localhost:18088

Notes:
  - First launch creates the config/ directory automatically.
  - If macOS blocks the binary, allow it in Privacy & Security.
  - This package statically links third-party dependencies. macOS may ask to
    allow dice-next-server once because this beta build is not Apple-signed.
  - Upgrade by replacing the program, i18n, web/dist and data files;
    keep your config and database files.
  - Feedback: QQ group 933145116.
EOF

rm -f "$ARCHIVE"
tar -C "$RELEASE_DIR" -czf "$ARCHIVE" "$STAGE_NAME"
rm -rf "$STAGING_DIR"
echo "OK: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
