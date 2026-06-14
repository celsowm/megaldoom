#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${1:-$ROOT/.toolchain/sgdk}"
BUILD_LIBRARY="${BUILD_LIBRARY:-0}"
FORCE="${FORCE:-0}"
API_URL="https://api.github.com/repos/Stephane-D/SGDK/releases/latest"
TMP_DIR="$ROOT/.toolchain/sgdk-extract"
ARCHIVE="$ROOT/.toolchain/sgdk-latest.zip"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
fail() { printf '\033[31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

if ! command -v curl >/dev/null 2>&1; then
  fail "curl is required. Install curl first."
fi
if ! command -v python3 >/dev/null 2>&1; then
  fail "python3 is required so this script can parse the GitHub release JSON without jq."
fi
if ! command -v unzip >/dev/null 2>&1; then
  fail "unzip is required. Install unzip first."
fi

if [[ -f "$INSTALL_DIR/makefile.gen" && "$FORCE" != "1" ]]; then
  step "SGDK already present at $INSTALL_DIR"
else
  [[ "$FORCE" == "1" ]] && rm -rf "$INSTALL_DIR"
  mkdir -p "$ROOT/.toolchain"

  step "Finding latest SGDK release asset"
  ASSET_URL="$(python3 - <<'PY'
import json, sys, urllib.request
url = "https://api.github.com/repos/Stephane-D/SGDK/releases/latest"
req = urllib.request.Request(url, headers={"User-Agent": "megaldoom-setup"})
with urllib.request.urlopen(req) as r:
    data = json.load(r)
assets = data.get("assets", [])
for a in assets:
    name = a.get("name", "").lower()
    if name.endswith(".zip"):
        print(a["browser_download_url"])
        sys.exit(0)
sys.exit(1)
PY
)" || fail "Could not find a .zip asset in the latest SGDK release. Download SGDK manually from GitHub releases."

  step "Downloading SGDK"
  curl -L --fail -o "$ARCHIVE" "$ASSET_URL"

  rm -rf "$TMP_DIR"
  mkdir -p "$TMP_DIR"
  step "Extracting SGDK"
  unzip -q "$ARCHIVE" -d "$TMP_DIR"

  SGDK_ROOT="$(find "$TMP_DIR" -name makefile.gen -type f -print -quit | xargs -r dirname)"
  [[ -n "$SGDK_ROOT" ]] || fail "Extracted archive does not look like SGDK; makefile.gen was not found."

  rm -rf "$INSTALL_DIR"
  mkdir -p "$INSTALL_DIR"
  step "Installing SGDK to $INSTALL_DIR"
  cp -a "$SGDK_ROOT"/. "$INSTALL_DIR"/

  rm -rf "$TMP_DIR" "$ARCHIVE"
fi

export GDK="$INSTALL_DIR"
step "GDK set for this shell process: $GDK"

if [[ "$BUILD_LIBRARY" == "1" ]]; then
  if ! command -v m68k-elf-gcc >/dev/null 2>&1; then
    cat <<'MSG'
WARNING: m68k-elf-gcc was not found in PATH.
SGDK ships Windows compiler binaries, but on Linux you usually need to install/build the m68k-elf toolchain separately.
After installing it, rerun:
  BUILD_LIBRARY=1 ./tools/setup-sgdk-linux.sh
MSG
  else
    step "Building SGDK library"
    make -f "$GDK/makelib.gen"
  fi
fi

cat <<MSG

Done. For this terminal session run:
  export GDK="$INSTALL_DIR"

Then build:
  ./tools/build-linux.sh
MSG
