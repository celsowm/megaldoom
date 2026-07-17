#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDK_PATH="${GDK:-$ROOT/.toolchain/sgdk}"
NO_CLEAN="${NO_CLEAN:-0}"
CLEAN="${CLEAN:-0}"
DEBUG_PERF="${DEBUG_PERF:-0}"
FORCE_ASSETS="${FORCE_ASSETS:-0}"

if [[ ! -f "$GDK_PATH/makefile.gen" ]]; then
  echo "SGDK was not found."
  echo "Run: ./tools/setup-sgdk-linux.sh"
  echo "Or set: export GDK=/opt/sgdk"
  exit 1
fi

export GDK="$GDK_PATH"

ASSET_ARGS=("$ROOT/tools/generate-frontend-assets.py")
if [[ "$FORCE_ASSETS" == "1" ]]; then
  ASSET_ARGS+=(--force)
fi
python3 "${ASSET_ARGS[@]}"

# Preserve caller flags, but own DEBUG_PERF so it cannot leak from a previous
# invocation. SGDK dependencies do not record flags, so build-state.py requests
# a clean only when the effective configuration changed.
read -r -a INHERITED_FLAGS <<< "${EXTRA_FLAGS:-}"
EFFECTIVE_FLAG_PARTS=()
for flag in "${INHERITED_FLAGS[@]}"; do
  if [[ "$flag" != "-DDEBUG_PERF" && "$flag" != -DDEBUG_PERF=* ]]; then
    EFFECTIVE_FLAG_PARTS+=("$flag")
  fi
done
if [[ "$DEBUG_PERF" == "1" ]]; then
  EFFECTIVE_FLAG_PARTS+=("-DDEBUG_PERF=1")
fi
EFFECTIVE_FLAGS="${EFFECTIVE_FLAG_PARTS[*]}"
if [[ -n "$EFFECTIVE_FLAGS" ]]; then
  export EXTRA_FLAGS="$EFFECTIVE_FLAGS"
else
  unset EXTRA_FLAGS
fi

STATE_PATH="$ROOT/build/build-config.json"
ROM_STATE_PATH="$ROOT/build/rom-bin.json"
STATE_DECISION="$(python3 "$ROOT/tools/build-state.py" check \
  --state "$STATE_PATH" --output "$ROOT/out" "--flags=$EFFECTIVE_FLAGS")"
MUST_CLEAN="$CLEAN"
if [[ "$STATE_DECISION" == "clean" ]]; then
  MUST_CLEAN=1
  if [[ "$CLEAN" != "1" ]]; then
    echo "Compiler flags changed; cleaning stale objects."
  fi
fi
if [[ "$NO_CLEAN" == "1" ]]; then
  echo "NO_CLEAN is retained for compatibility; builds are incremental by default."
fi

cd "$ROOT"
if [[ "$MUST_CLEAN" == "1" ]]; then
  make -f Makefile clean
fi
# Keep SGDK's target-specific release flags while replacing only its phony
# Java padding recipe. The real padding runs below when the ROM hash changed.
make -f Makefile release SIZEBND=echo
python3 "$ROOT/tools/build-state.py" seal-link --output "$ROOT/out" --root "$ROOT"
ROM_DECISION="$(python3 "$ROOT/tools/build-state.py" check-rom \
  --state "$ROM_STATE_PATH" --rom "$ROOT/out/rom.bin")"
if [[ "$ROM_DECISION" == "pad" ]]; then
  make -f Makefile release
  python3 "$ROOT/tools/build-state.py" record-rom \
    --state "$ROM_STATE_PATH" --rom "$ROOT/out/rom.bin"
fi
python3 "$ROOT/tools/build-state.py" seal-link --output "$ROOT/out" --root "$ROOT"
python3 "$ROOT/tools/build-state.py" record --state "$STATE_PATH" "--flags=$EFFECTIVE_FLAGS"
