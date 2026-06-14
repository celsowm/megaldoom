#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROM_PATH="${ROM_PATH:-}"
EMULATOR_PATH="${EMULATOR:-$ROOT/.tools/blastem/blastem}"
NO_BUILD="${NO_BUILD:-0}"
NO_CLEAN="${NO_CLEAN:-0}"

find_rom() {
  if [[ -f "$ROOT/out/rom.bin" ]]; then
    printf '%s\n' "$ROOT/out/rom.bin"
    return 0
  fi
  if [[ -f "$ROOT/out/megaldoom.bin" ]]; then
    printf '%s\n' "$ROOT/out/megaldoom.bin"
    return 0
  fi
  find "$ROOT" \
    -path "$ROOT/.toolchain" -prune -o \
    -path "$ROOT/.tools" -prune -o \
    -type f -name '*.bin' -printf '%T@\t%p\n' 2>/dev/null | sort -nr | head -n 1 | cut -f2-
}

if [[ "$NO_BUILD" != "1" ]]; then
  if [[ "$NO_CLEAN" == "1" ]]; then
    NO_CLEAN=1 "$ROOT/tools/build-linux.sh"
  else
    "$ROOT/tools/build-linux.sh"
  fi
fi

if [[ -z "$ROM_PATH" ]]; then
  ROM_PATH="$(find_rom || true)"
fi

if [[ -z "$ROM_PATH" || ! -f "$ROM_PATH" ]]; then
  echo "ROM was not found. Build first with ./tools/build-linux.sh"
  exit 1
fi

if [[ ! -x "$EMULATOR_PATH" ]]; then
  echo "BlastEm was not found."
  echo "Run: ./tools/download-emulator-linux.sh"
  echo "Or set: export EMULATOR=/path/to/blastem"
  exit 1
fi

echo "Running: $ROM_PATH"
exec "$EMULATOR_PATH" "$ROM_PATH"
