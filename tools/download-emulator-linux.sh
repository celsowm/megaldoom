#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${1:-$ROOT/.tools/blastem}"
STABLE="${STABLE:-0}"
FORCE="${FORCE:-0}"
ARCHIVE="$ROOT/.tools/blastem.tar.gz"
TMP_DIR="$ROOT/.tools/blastem-extract"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
fail() { printf '\033[31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || fail "curl is required."
command -v tar >/dev/null 2>&1 || fail "tar is required."
command -v python3 >/dev/null 2>&1 || fail "python3 is required so this script can parse the nightly index."

if [[ -x "$INSTALL_DIR/blastem" && "$FORCE" != "1" ]]; then
  step "BlastEm already present at $INSTALL_DIR"
else
  [[ "$FORCE" == "1" ]] && rm -rf "$INSTALL_DIR"
  mkdir -p "$ROOT/.tools"

  if [[ "$STABLE" == "1" ]]; then
    URL="https://www.retrodev.com/blastem/blastem64-0.6.2.tar.gz"
  else
    step "Finding latest BlastEm linux64 nightly"
    FILE="$(python3 - <<'PY'
import re, urllib.request
url = "https://www.retrodev.com/blastem/nightlies/"
req = urllib.request.Request(url, headers={"User-Agent": "megaldoom-setup"})
html = urllib.request.urlopen(req).read().decode("utf-8", "replace")
m = re.search(r'href="([^"]*blastem64-[^"]*\.tar\.gz)"', html)
if not m:
    raise SystemExit(1)
print(m.group(1))
PY
)" || fail "Could not find a BlastEm linux64 nightly in the index."
    URL="https://www.retrodev.com/blastem/nightlies/$FILE"
  fi

  step "Downloading BlastEm"
  curl -L --fail -o "$ARCHIVE" "$URL"

  rm -rf "$TMP_DIR"
  mkdir -p "$TMP_DIR"
  step "Extracting BlastEm"
  tar -xzf "$ARCHIVE" -C "$TMP_DIR"

  BLASTEM_BIN="$(find "$TMP_DIR" -type f -name blastem -perm -111 -print -quit)"
  [[ -n "$BLASTEM_BIN" ]] || fail "blastem executable was not found after extraction."

  rm -rf "$INSTALL_DIR"
  mkdir -p "$INSTALL_DIR"
  cp -a "$(dirname "$BLASTEM_BIN")"/. "$INSTALL_DIR"/
  chmod +x "$INSTALL_DIR/blastem"

  rm -rf "$TMP_DIR" "$ARCHIVE"
fi

cat <<MSG

Done. Emulator path:
  $INSTALL_DIR/blastem

Run game with:
  ./tools/run-linux.sh
MSG
