#!/usr/bin/env python3
"""Deterministic tests for frontend caching and compiler-flag invalidation."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate-frontend-assets.py"
STATE_TOOL = ROOT / "tools" / "build-state.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


frontend = load_module("frontend_assets", GENERATOR)
build_state = load_module("build_state", STATE_TOOL)


def run_generator(source: Path, output: Path, *args: str, script: Path = GENERATOR):
    return subprocess.run(
        [sys.executable, str(script), "--source-dir", str(source), "--output-dir", str(output), *args],
        text=True,
        capture_output=True,
        check=False,
    )


with tempfile.TemporaryDirectory(prefix="megaldoom-build-test-") as folder:
    temp = Path(folder)
    source = temp / "source"
    output = temp / "frontend"
    source.mkdir()
    for index, name in enumerate((*frontend.PATCHES, *frontend.GLYPHS), start=1):
        (source / f"{name}.png").write_bytes(f"source-{index}".encode())

    current, reason = frontend.cache_status(source, output)
    assert not current and "missing" in reason

    generation = 0
    real_generate = frontend.generate

    def fake_generate(_source: Path, destination: Path) -> None:
        nonlocal_generation[0] += 1
        destination.mkdir(parents=True, exist_ok=True)
        for name in frontend.EXPECTED_OUTPUTS:
            (destination / name).write_bytes(f"generated-{nonlocal_generation[0]}-{name}".encode())

    nonlocal_generation = [generation]
    frontend.generate = fake_generate
    try:
        frontend.publish(source, output)
    finally:
        frontend.generate = real_generate
    assert frontend.complete_outputs(output)
    assert (output / frontend.MANIFEST_NAME).is_file()
    assert len(frontend.EXPECTED_OUTPUTS) == 30

    mtimes = {name: (output / name).stat().st_mtime_ns for name in frontend.EXPECTED_OUTPUTS}
    cached = run_generator(source, output)
    assert cached.returncode == 0 and "frontend assets: cached" in cached.stdout
    assert mtimes == {name: (output / name).stat().st_mtime_ns for name in frontend.EXPECTED_OUTPUTS}
    assert run_generator(source, output, "--check").returncode == 0

    missing = output / frontend.EXPECTED_OUTPUTS[0]
    missing.unlink()
    assert run_generator(source, output, "--check").returncode == 1
    frontend.generate = fake_generate
    try:
        frontend.publish(source, output)
    finally:
        frontend.generate = real_generate
    assert missing.is_file()

    changed_source = source / f"{frontend.PATCHES[0]}.png"
    changed_source.write_bytes(b"changed-source")
    assert run_generator(source, output, "--check").returncode == 1
    frontend.generate = fake_generate
    try:
        frontend.publish(source, output)
    finally:
        frontend.generate = real_generate

    forced_check = run_generator(source, output, "--force", "--check")
    assert forced_check.returncode == 1, "--force must bypass a current cache"

    copied_generator = temp / "changed-generator.py"
    shutil.copyfile(GENERATOR, copied_generator)
    copied_generator.write_text(copied_generator.read_text(encoding="utf-8") + "\n# changed\n", encoding="utf-8")
    changed = run_generator(source, output, "--check", script=copied_generator)
    assert changed.returncode == 1 and "generator changed" in changed.stderr

    shutil.rmtree(source)
    no_sources_cached = run_generator(source, output)
    assert no_sources_cached.returncode == 0 and "using the complete recorded cache" in no_sources_cached.stdout
    (output / frontend.EXPECTED_OUTPUTS[-1]).unlink()
    no_sources_stale = run_generator(source, output)
    assert no_sources_stale.returncode == 1
    assert "Extract your local DOOM1.WAD" in no_sources_stale.stderr

    state = temp / "build" / "build-config.json"
    objects = temp / "out"
    assert not build_state.needs_clean(state, objects, "")
    (objects / "src").mkdir(parents=True)
    (objects / "src" / "main.o").write_bytes(b"object")
    assert build_state.needs_clean(state, objects, "")

    (objects / "res").mkdir()
    (objects / "res" / "resources.o").write_bytes(b"resource")
    (objects / "sega.o").write_bytes(b"boot")
    (objects / "rom.out").write_bytes(b"rom")
    (temp / "src").mkdir()
    (temp / "src" / "main.c").write_text("int main(void);", encoding="utf-8")
    (temp / "res").mkdir()
    (temp / "res" / "resources.res").write_text("", encoding="utf-8")
    build_state.seal_link(objects, temp)
    link_inputs = (objects / "cmd_").read_text(encoding="utf-8").split()
    assert link_inputs == ["out/res/resources.o", "out/src/main.o"]
    assert (objects / "cmd_").stat().st_mtime_ns == (objects / "rom.out").stat().st_mtime_ns
    rom = objects / "rom.bin"
    rom.write_bytes(b"unpadded")
    rom_state = temp / "build" / "rom-bin.json"
    assert not build_state.rom_is_recorded(rom_state, rom)
    build_state.record_rom(rom_state, rom)
    assert build_state.rom_is_recorded(rom_state, rom)
    rom.write_bytes(b"changed")
    assert not build_state.rom_is_recorded(rom_state, rom)
    build_state.record(state, "")
    assert not build_state.needs_clean(state, objects, "")
    assert build_state.needs_clean(state, objects, "-DDEBUG_PERF=1")
    cli_check = subprocess.run(
        [sys.executable, str(STATE_TOOL), "check", "--state", str(state),
         "--output", str(objects), "--flags=-DDEBUG_PERF=1"],
        text=True, capture_output=True, check=False,
    )
    assert cli_check.returncode == 0 and cli_check.stdout.strip() == "clean"
    build_state.record(state, "-DDEBUG_PERF=1")
    assert not build_state.needs_clean(state, objects, "-DDEBUG_PERF=1")
    assert build_state.needs_clean(state, objects, "")

windows = (ROOT / "tools" / "build-windows.ps1").read_text(encoding="utf-8")
runner = (ROOT / "tools" / "run-windows.ps1").read_text(encoding="utf-8")
linux = (ROOT / "tools" / "build-linux.sh").read_text(encoding="utf-8")
package = (ROOT / "package.json").read_text(encoding="utf-8")
for token in ("[switch]$Clean", "[switch]$ForceAssets", "build-state.py", "generate-frontend-assets.py"):
    assert token in windows
for token in ('CLEAN="${CLEAN:-0}"', 'FORCE_ASSETS="${FORCE_ASSETS:-0}"', "build-state.py", "generate-frontend-assets.py"):
    assert token in linux
for token in ("[switch]$Restart", "[switch]$Detach", "Stop-Process -Force", "Start-Process"):
    assert token in runner
assert '"debug": "pwsh -NoProfile -File tools/run-windows.ps1 -DebugPerf -Restart -Detach"' in package

print("ok    build: frontend cache and compiler-flag invalidation")
