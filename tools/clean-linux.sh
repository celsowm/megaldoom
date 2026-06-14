#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDK_PATH="${GDK:-$ROOT/.toolchain/sgdk}"
cd "$ROOT"
if [[ -f "$GDK_PATH/makefile.gen" ]]; then
  export GDK="$GDK_PATH"
  make -f Makefile clean || true
fi
rm -rf out build obj
