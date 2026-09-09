#!/usr/bin/env python3
"""Rewrite the runner half of tools/blastem-runner.patch from the working copy.

`.externals/` is gitignored, so the deterministic runner only exists in this
repository as a patch. Nothing kept the two in step: tools/build-blastem-windows.ps1
applies the patch only when megaldoom_runner.c is absent, so once the file
exists every later edit lives purely in an untracked directory. A verified fix
made that way is invisible to a fresh clone and is lost the first time the
external tree is recreated -- which is exactly what happened to the waypoint
progress-detection fix.

Only the two new-file sections (megaldoom_runner.c/.h) are regenerated here;
the Makefile/blastem.c/genesis.c hunks are ordinary context diffs against
upstream sources this repository does not vendor, so they stay hand-maintained.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "tools" / "blastem-runner.patch"
SOURCE = ROOT / ".externals" / "blastem"
NEW_FILES = ("megaldoom_runner.c", "megaldoom_runner.h")


def new_file_section(name):
    body = (SOURCE / name).read_bytes().decode("utf-8").replace("\r\n", "\n")
    lines = body.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    out = [
        "diff --git a/%s b/%s" % (name, name),
        "new file mode 100644",
        "index 0000000..0000000",
        "--- /dev/null",
        "+++ b/%s" % name,
        "@@ -0,0 +1,%d @@" % len(lines),
    ]
    out.extend("+" + line for line in lines)
    return "\n".join(out) + "\n"


def split_sections(text):
    sections, current = [], []
    for line in text.split("\n"):
        if line.startswith("diff --git ") and current:
            sections.append("\n".join(current) + "\n")
            current = []
        current.append(line)
    tail = "\n".join(current)
    if tail.strip():
        sections.append(tail if tail.endswith("\n") else tail + "\n")
    return sections


def renumber_hunks(section):
    """Recompute @@ counts for a hand-maintained context hunk.

    The Makefile/blastem.c/genesis.c hunks are edited by hand whenever the
    runner grows a new command-line option, and a hunk whose header still
    claims the old line counts makes `git apply` reject the whole patch as
    corrupt -- a confusing way to learn that two integers are stale. Counts
    come from the body; each hunk's new-side start is its old-side start
    shifted by everything the earlier hunks in the same file added.
    """
    lines = section.split("\n")
    out, delta, index = [], 0, 0
    while index < len(lines):
        line = lines[index]
        if not line.startswith("@@"):
            out.append(line)
            index += 1
            continue
        tail = line.partition("@@ ")[2].partition(" @@")
        old_start = int(tail[0].split(" ")[0].split(",")[0].lstrip("-"))
        suffix = tail[2]
        body, cursor = [], index + 1
        while cursor < len(lines) and not lines[cursor].startswith(("@@", "diff --git ")):
            body.append(lines[cursor])
            cursor += 1
        while body and body[-1] == "":
            body.pop()
        old_count = sum(1 for b in body if b[:1] in (" ", "-"))
        new_count = sum(1 for b in body if b[:1] in (" ", "+"))
        out.append("@@ -%d,%d +%d,%d @@%s"
                   % (old_start, old_count, old_start + delta, new_count, suffix))
        out.extend(body)
        delta += new_count - old_count
        index = cursor
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"


def rebuild():
    sections = split_sections(PATCH.read_bytes().decode("utf-8").replace("\r\n", "\n"))
    rebuilt = []
    for section in sections:
        header = section.split("\n", 1)[0]
        name = header.split(" b/")[-1].strip() if " b/" in header else ""
        if name in NEW_FILES:
            rebuilt.append(new_file_section(name))
        else:
            rebuilt.append(renumber_hunks(section))
    return "".join(rebuilt)


def main():
    if not (SOURCE / NEW_FILES[0]).exists():
        raise SystemExit("no working copy at %s; nothing to capture" % SOURCE)
    updated = rebuild()
    if "--check" in sys.argv:
        if PATCH.read_bytes().decode("utf-8").replace("\r\n", "\n") != updated:
            raise SystemExit(
                "tools/blastem-runner.patch is stale: it does not match "
                ".externals/blastem. Run: python tools/refresh-blastem-patch.py")
        print("ok    blastem-runner.patch matches the working copy")
        return
    PATCH.write_text(updated, newline="\n")
    print("wrote %s" % PATCH)


if __name__ == "__main__":
    main()
