#!/usr/bin/env python3
"""Prepare the decomp's C sources for the native port.

For every source file of the decomp that the port compiles, write a copy under OUT/ that clang can build. The sources
themselves are written for the port (CodeWarrior assembly and functions the platform layer provides are inside
`#ifndef PLATFORM_PORT`, register writes with side effects use PORT_IO_WRITE); here:
  - writes to side-effect registers that do not go through PORT_IO_WRITE stop the build;
  - files of an overlay get `#pragma clang section` so that the overlay's .data/.bss can be restored to their initial
    state whenever the game loads the overlay again, as the DS does by reading it from the ROM.

Also writes OUT/sources.txt (one generated path per line) and OUT/overlays.txt.
Files are only rewritten when their content changes, to keep incremental builds incremental.

Usage: portify.py <decomp> <out> <exclude-list> <version>
"""
import os
import re
import sys
from pathlib import Path

# Registers whose writes have side effects must go through the platform layer: PORT_IO_WRITE(reg_X, v), which
# forwards them to the geometry engine, the VRAM mapper or the interrupt controller. A plain `reg_X = v;` (or |=,
# &=, ^=) in a compiled file stops the build: write it as PORT_IO_WRITE in the source.
SIDE_EFFECT_REG = re.compile(r"\b(reg_(?:G3_[A-Z0-9_x]+|G3X_GXFIFO|GX_VRAMCNT_[A-I]|OS_IF))\s*([|&^]?)=(?!=)")


def check_reg_writes(text, rel):
    m = SIDE_EFFECT_REG.search(text)
    if m:
        line = text.count("\n", 0, m.start()) + 1
        sys.exit(f"portify: {rel}:{line}: write to {m.group(1)} must be PORT_IO_WRITE({m.group(1)}, ...)")


def module_units(decomp, version):
    """Map source path -> overlay name ('ov000' ...) for overlay units."""
    units = {}
    ovdir = Path(decomp) / "config" / version / "arm9" / "overlays"
    for ov in sorted(ovdir.iterdir()):
        for line in open(ov / "delinks.txt"):
            line = line.rstrip()
            if line and not line.startswith((" ", "\t", "/")) and line.endswith(":"):
                units[line[:-1]] = ov.name
    return units


def build_units(decomp, version):
    """Every source the real build links: the units of delinks.txt (main, ITCM, DTCM, overlays)."""
    units = []
    cfg = Path(decomp) / "config" / version / "arm9"
    for d in [cfg / "delinks.txt", cfg / "itcm" / "delinks.txt", cfg / "dtcm" / "delinks.txt"] + sorted(
            (cfg / "overlays").glob("*/delinks.txt")):
        for line in open(d):
            line = line.rstrip()
            if line and not line.startswith((" ", "\t", "/")) and line.endswith(":"):
                units.append(line[:-1])
    return units


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_text() == text:
            return
    except OSError:
        pass
    path.write_text(text)


def main():
    decomp, out, exclude_file, version = sys.argv[1:5]
    decomp = Path(decomp)
    out = Path(out)
    excludes = []
    for line in open(exclude_file):
        line = line.split("#", 1)[0].strip()
        if line:
            excludes.append(line)

    def excluded(rel):
        return any(rel == e or (e.endswith("/") and rel.startswith(e)) for e in excludes)

    overlays = module_units(decomp, version)

    sources, used_overlays = [], set()
    for rel in sorted(set(build_units(decomp, version))):
        src = decomp / rel
        if not rel.endswith(".c") or excluded(rel):
            continue
        text = src.read_text(encoding="utf-8", errors="surrogateescape")
        check_reg_writes(text, rel)
        ov = overlays.get(rel)
        header = f'#line 1 "{rel}"\n'
        dst = out / "src" / rel
        write_if_changed(dst, header + text)
        sources.append(dst.as_posix())

    # Headers: one tree with the decomp's include/ and the port's shadow headers on top, so that a quoted include
    # resolves to the same file from everywhere (CodeWarrior's -gccinc semantics).
    inc = out / "include"
    compat = Path(__file__).resolve().parent.parent / "port" / "compat" / "include"
    shadowed = {p.relative_to(compat).as_posix() for p in compat.rglob("*.h")}
    headers = [(h, h.relative_to(decomp / "include").as_posix()) for h in sorted((decomp / "include").rglob("*.h"))]
    for d in sorted(decomp.glob("libs/**/include")):
        # The C library's headers (stdio.h, string.h ...) go to their own folder, used only for the game's code, so
        # that the platform layer gets the host's.
        prefix = "../msl_include/" if d.parts[-2] == "msl" else ""
        headers += [(h, prefix + h.relative_to(d).as_posix()) for h in sorted(d.rglob("*.h"))]
    for h, rel in headers:
        text = h.read_text(encoding="utf-8", errors="surrogateescape")
        if rel == "types.h":
            # mwcc aligns 64-bit integers to 4 bytes (u64 members sit at offsets like 0x2c); keep that layout.
            for t in ("u64", "s64", "vu64", "vs64"):
                text, n = re.subn(rf"^typedef (.*) {t};$", rf"typedef \1 __attribute__((aligned(4))) {t};", text, flags=re.M)
                if n != 1:
                    sys.exit(f"portify: types.h: typedef of {t} not found")
        if rel in shadowed:
            rel += ".decomp"
        write_if_changed(inc / rel, text)
    for h in sorted(compat.rglob("*.h")):
        rel = h.relative_to(compat).as_posix()
        text = re.sub(r'#include_next "([^"]+)"', r'#include "\1.decomp"', h.read_text())
        write_if_changed(inc / rel, text)

    write_if_changed(out / "sources.txt", "\n".join(sources) + "\n")
    write_if_changed(out / "overlays.txt", "\n".join(sorted(used_overlays)) + "\n")
    write_if_changed(out / "overlay_units.txt",
                     "".join(f"{overlays[r]} {r}\n" for r in sorted(overlays) if (out / "src" / r).exists()))


if __name__ == "__main__":
    main()
