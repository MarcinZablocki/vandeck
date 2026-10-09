#!/usr/bin/env bash
# Publishes the current build as a GitHub release that panels update from: vandeck.bin plus version.json
# (version, download URL, size, SHA-256). Needs the GitHub CLI (gh) logged in.
#   idf.py build && tools/release.sh ["release notes"]
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^set(PROJECT_VER "\(.*\)").*/\1/p' CMakeLists.txt)
REPO=$(sed -n 's/^CONFIG_VD_UPDATE_REPO="\(.*\)"/\1/p' sdkconfig)
BIN=build/vandeck.bin
[ -n "$REPO" ] || { echo "Set VanDeck > GitHub repository for updates (menuconfig) first"; exit 1; }
[ -f "$BIN" ] || { echo "No $BIN: run idf.py build first"; exit 1; }
# The image's own app description (offset 48 = image + segment header + 16) must carry this version.
IMG_VER=$(dd if="$BIN" bs=1 skip=48 count=32 2>/dev/null | tr -d '\0')
[ "$IMG_VER" = "$VER" ] || { echo "$BIN is version '$IMG_VER', CMakeLists.txt says '$VER': rebuild"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "Uncommitted changes: commit first so the release matches its tag"; exit 1; }

TAG="v$VER"
SIZE=$(wc -c < "$BIN" | tr -d ' ')
SHA=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
URL="https://github.com/$REPO/releases/download/$TAG/vandeck.bin"
printf '{"project":"vandeck","version":"%s","url":"%s","size":%s,"sha256":"%s"}\n' "$VER" "$URL" "$SIZE" "$SHA" > build/version.json
cat build/version.json
gh release create "$TAG" "$BIN" build/version.json --repo "$REPO" --target "$(git rev-parse HEAD)" \
  --title "VanDeck $VER" --notes "${1:-VanDeck $VER}"
echo "Released $TAG: panels set to Notify or Automatic pick it up within 12 hours (or Settings > About > Check now)"
