#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDK_PATH="${GDK:-$ROOT/.toolchain/sgdk}"
NO_CLEAN="${NO_CLEAN:-0}"
DEBUG_PERF="${DEBUG_PERF:-0}"

if [[ ! -f "$GDK_PATH/makefile.gen" ]]; then
  echo "SGDK was not found."
  echo "Run: ./tools/setup-sgdk-linux.sh"
  echo "Or set: export GDK=/opt/sgdk"
  exit 1
fi

export GDK="$GDK_PATH"

# SGDK's makefile.gen folds $EXTRA_FLAGS into every compile's CFLAGS but never
# assigns it itself, so it falls through to this environment variable. Object
# files carry no record of which flags built them, so a define toggle must force
# a full rebuild or stale .o files silently keep the old behavior.
if [[ "$DEBUG_PERF" == "1" ]]; then
  export EXTRA_FLAGS="-DDEBUG_PERF=1"
  if [[ "$NO_CLEAN" == "1" ]]; then
    echo "DEBUG_PERF=1 forces a clean rebuild (object files don't track compiler flags); ignoring NO_CLEAN."
    NO_CLEAN=0
  fi
elif [[ "${EXTRA_FLAGS:-}" == "-DDEBUG_PERF=1" ]]; then
  unset EXTRA_FLAGS
  if [[ "$NO_CLEAN" == "1" ]]; then
    echo "Clearing a stale DEBUG_PERF build forces a clean rebuild; ignoring NO_CLEAN."
    NO_CLEAN=0
  fi
fi

cd "$ROOT"
if [[ "$NO_CLEAN" != "1" ]]; then
  make -f Makefile clean
fi
make -f Makefile
