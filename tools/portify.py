#!/usr/bin/env python3
"""Prepare the decomp's C sources for the native port.

For every source file of the decomp that the port compiles, write a copy under OUT/ that clang can build:
  - CodeWarrior `asm` functions are removed (the port implements the ones it needs in C, see
    port/platform/asm_replacements.c); their names are listed in OUT/stripped_asm.txt;
  - the fixups in FIXUPS are applied (each must match, so a decomp change that breaks one fails loudly);
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

# (path, regex, replacement, expected count)
FIXUPS = [
    # The tail call relies on r0 still holding `model` (K&R declaration, no argument passed).
    ("src/Main/MapObjects/shadow_model_alpha.c", r"DrawShadowModelIfDrawFlag\(model\);", "DrawShadowModelIfDrawFlag(model, 0, 0);", 1),
    ("src/Main/MapObjects/shadow_model_alpha.c", r"DrawShadowModelIfDrawFlag\(\);", "DrawShadowModelIfDrawFlag(model, 0, 0);", 1),
    ("src/Main/MapObjects/send_obj_shadow.c", r"DrawShadowModelIfDrawFlag\(gJgShadow\);", "DrawShadowModelIfDrawFlag(gJgShadow, 0, 0);", 1),
    # VIEWPORT geometry command written by address.
    ("src/Main/Kart/kart_model_menu.c", r"\*\(u32 \*\) 0x4000580 = 0xbfff0000;", "PORT_IO_WRITE(reg_G3_VIEWPORT, 0xbfff0000);", 1),
    ("libs/NitroSystem/g2d/src/font_glyph_layout.c", r"asm \{ clz leadingZeros, value \}", "leadingZeros = __builtin_clz(value);", 1),
]

# Functions removed from files that are otherwise compiled; the platform layer defines them.
STRIP_FUNCS = {
    # Program DMA channels by writing the registers directly.
    "src/ITCM/dma_transfer.c": ["StartDmaTransfer", "StartDmaTransferAndChannel0Dummy",
                                "StartDmaTransferWithoutIrqDisable",
                                "StartDmaTransferAndChannel0DummyWithoutIrqDisable"],
    # Writes geometry commands through pointers to the command registers.
    "src/Main/Graphics/stem_sets.c": ["DrawTimedQuad"],
    # Context switching: the platform layer runs each OS thread on a host thread (port/platform/os.c).
    # Overlays are linked into the program; loading one restores its data (port/platform/overlay.c).
    "libs/NitroSDK/fs/src/overlay_rom_loader.c": ["FS_ClearOverlayImage", "FS_LoadOverlayImageAsync",
                                                  "FS_LoadOverlayImage", "FS_StartOverlay"],
    "src/Main/Net/slot_result_sync.c": ["IsInOverlayRange"],
    "libs/NitroSDK/os/src/thread_scheduler.c": ["OS_InitThread", "OS_CreateThread", "OSi_ExitThread_ArgSpecified",
                                                "OSi_ExitThread_Destroy", "OSi_RescheduleThread"],
}

# Registers whose writes have side effects: `reg_X = v;` (and |=, &=) become `PORT_IO_WRITE(reg_X, v);`, which the
# platform layer forwards to the geometry engine, the VRAM mapper or the interrupt controller.
SIDE_EFFECT_REG = re.compile(r"\b(reg_(?:G3_[A-Z0-9_x]+|G3X_GXFIFO|GX_VRAMCNT_[A-I]|OS_IF))\s*([|&^]?)=(?!=)")


def rewrite_reg_writes(text, rel):
    out, pos, count = [], 0, 0
    for m in SIDE_EFFECT_REG.finditer(text):
        if m.start() < pos:
            continue
        # The statement must start here: only whitespace or a statement boundary before it.
        before = text[:m.start()].rstrip()
        if before and before[-1] not in ";{}:)" and not before.endswith("else"):
            sys.exit(f"portify: {rel}: unsupported write to {m.group(1)}: ...{text[m.start()-40:m.end()+20]!r}")
        depth, i = 0, m.end()
        while i < len(text):
            c = text[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            i += 1
        expr = text[m.end():i].strip()
        reg, op = m.group(1), m.group(2)
        if op:
            expr = f"{reg} {op} ({expr})"
        out.append(text[pos:m.start()])
        out.append(f"PORT_IO_WRITE({reg}, {expr})")
        pos = i
        count += 1
    out.append(text[pos:])
    return "".join(out), count


C_FUNC_TEMPLATE = r"^[A-Za-z_][^;{{}}()]*?\b{name}\s*\([^;{{}}]*\)\s*\{{"


def strip_funcs(text, names, rel):
    for name in names:
        m = re.search(C_FUNC_TEMPLATE.format(name=re.escape(name)), text, re.M)
        if not m:
            sys.exit(f"portify: {rel}: function {name} to strip not found")
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        text = text[:m.start()] + f"/* PORT: {name} provided by the platform layer */" + text[i + 1:]
    return text


ASM_FUNC = re.compile(r"^(?:static\s+)?(?:inline\s+)?asm\s+[^;{]*?\b(\w+)\s*\([^;{]*\)\s*\{", re.M)


def strip_asm(text):
    """Remove top-level `asm <type> name(...) { ... }` definitions. Returns (text, names)."""
    names = []
    out = []
    pos = 0
    for m in ASM_FUNC.finditer(text):
        if m.start() < pos:
            continue
        depth = 0
        i = m.end() - 1
        while i < len(text):
            c = text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append(text[pos:m.start()])
        out.append(f"/* PORT: asm function {m.group(1)} removed (port/platform/asm_replacements.c) */")
        names.append(m.group(1))
        pos = i + 1
    out.append(text[pos:])
    return "".join(out), names


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
    fixups_by_file = {}
    for f, rx, rep, n in FIXUPS:
        fixups_by_file.setdefault(f, []).append((rx, rep, n))

    sources, stripped, used_overlays = [], [], set()
    for rel in sorted(set(build_units(decomp, version))):
        src = decomp / rel
        if not rel.endswith(".c") or excluded(rel):
            continue
        text = src.read_text(encoding="utf-8", errors="surrogateescape")
        for rx, rep, n in fixups_by_file.pop(rel, []):
            text, count = re.subn(rx, rep, text)
            if count != n:
                sys.exit(f"portify: fixup {rx!r} matched {count} times in {rel}, expected {n}")
        text, names = strip_asm(text)
        text = strip_funcs(text, STRIP_FUNCS.get(rel, []), rel)
        text, _ = rewrite_reg_writes(text, rel)
        stripped += [f"{rel}: {n}" for n in names]
        ov = overlays.get(rel)
        header = f'#line 1 "{rel}"\n'
        if ov:
            used_overlays.add(ov)
            header = (f'#pragma clang section text="{ov}_text" rodata="{ov}_rodata" data="{ov}_data" bss="{ov}_bss"\n') + header
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
        headers += [(h, h.relative_to(d).as_posix()) for h in sorted(d.rglob("*.h"))]
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

    if fixups_by_file:
        sys.exit(f"portify: fixups for files not compiled: {sorted(fixups_by_file)}")
    write_if_changed(out / "sources.txt", "\n".join(sources) + "\n")
    write_if_changed(out / "stripped_asm.txt", "\n".join(stripped) + "\n")
    write_if_changed(out / "overlays.txt", "\n".join(sorted(used_overlays)) + "\n")


if __name__ == "__main__":
    main()
