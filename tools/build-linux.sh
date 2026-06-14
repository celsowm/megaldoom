#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDK_PATH="${GDK:-$ROOT/.toolchain/sgdk}"
NO_CLEAN="${NO_CLEAN:-0}"

if [[ ! -f "$GDK_PATH/makefile.gen" ]]; then
  echo "SGDK was not found."
  echo "Run: ./tools/setup-sgdk-linux.sh"
  echo "Or set: export GDK=/opt/sgdk"
  exit 1
fi

export GDK="$GDK_PATH"
cd "$ROOT"
if [[ "$NO_CLEAN" != "1" ]]; then
  make -f Makefile clean
fi
make -f Makefile
