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
    # Arrays the game indexes past their declared size into the variables the DS links right after them: give them
    # one storage block so that the overflow lands where it did.
    ("src/Main/Sound/sound_sources.c",
     r"NNSFndList gSoundSourceSecondUpdateList;\nNNSFndList gSoundSourceUpdateLists\[1\];",
     "NNSFndList gSoundSourceUpdateListsStorage_[2] = {{0}};\n"
     "__asm__(\".globl gSoundSourceUpdateLists\\n.set gSoundSourceUpdateLists, gSoundSourceUpdateListsStorage_\\n"
     ".globl gSoundSourceSecondUpdateList\\n.set gSoundSourceSecondUpdateList, gSoundSourceUpdateListsStorage_ + 12\");", 1),
    # The member screen's sub-screen labels: the game reads nine rows (the decomp declares the first five); the other
    # four are the ones the DS has right after them.
    ("src/Main/Menu/multi_select_member_label_rows.c",
     r"const MultiSelectMemberSubLabelRows gMultiSelectMemberSubLabelRows = \{\n.*\n    5,\n    0x82\};",
     "const LabelMessageRow gMultiSelectMemberSubLabelRowsStorage_[9] = {\n"
     "    {0, 0x82, 0xd, 0, -1}, {1, 0x82, 0xd, 2, -1}, {2, 0x82, 0xd, 4, -1}, {3, 0x82, 0xd, 6, -1}, {4, 0x82, 0xd, 8, -1},\n"
     "    {5, 0x82, 0xd, 10, -1}, {6, 0x82, 0xd, 12, -1}, {7, 0x82, 0xd, 14, -1}, {8, 0x78, 0xd, 17, 6}};\n"
     "__asm__(\".globl gMultiSelectMemberSubLabelRows\\n.set gMultiSelectMemberSubLabelRows, gMultiSelectMemberSubLabelRowsStorage_\");", 1),
    # A frame finished without swapping the 3D buffers (multiplayer: the other players' data was late) keeps the
    # previous picture; its geometry is dropped instead of piling up under the next frame's (port/platform/gx3d.c).
    ("src/Main/Scene/display_settings.c",
     r"    if \(settings->mBg0As == GX_BG0_AS_3D && doSwap != 0\) \{\n"
     r"        reg_G3_SWAP_BUFFERS = \(settings->mG3xParams->mBufferMode << 1\) \| settings->mG3xParams->mSortMode;\n    \}",
     "    if (settings->mBg0As == GX_BG0_AS_3D && doSwap != 0) {\n"
     "        reg_G3_SWAP_BUFFERS = (settings->mG3xParams->mBufferMode << 1) | settings->mG3xParams->mSortMode;\n"
     "    } else if (settings->mBg0As == GX_BG0_AS_3D) {\n"
     "        extern void gx3d_discard(void);\n"
     "        gx3d_discard();\n"
     "    }", 1),
    # A scale animation whose middle part lasts to the end divides by zero (the numerator is 0 then too): the DS's
    # division gives 0, an x86 traps.
    ("libs/spl/src/particle_system.c",
     r"ptcl->mScale = scale \+ \(\(lifeRate - 0xff\) \* \(scale - scaleAnim->mMidScale\)\) / \(0xff - hi\);",
     "ptcl->mScale = scale + (hi == 0xff ? 0 : ((lifeRate - 0xff) * (scale - scaleAnim->mMidScale)) / (0xff - hi));", 1),
    # The player's name as the others see it and as the race shows it: the name of the app's network settings when
    # one is set, else the save file's nickname (port_player_name, port/platform/os.c). The save file keeps its own.
    ("src/Main/Net/connection.c", r"&gBackupBuffers->mFirstBlock->mPlayerName, &gConnectionWork->mPlayerRecords\[0\]",
     "port_player_name(gBackupBuffers->mFirstBlock->mPlayerName), &gConnectionWork->mPlayerRecords[0]", 1),
    ("src/Main/Net/connection_sharing.c", r"MI_CpuCopy8\(gBackupBuffers->mFirstBlock->mPlayerName, gConnectionWork->mChildren\.mGameInfo",
     "MI_CpuCopy8(port_player_name(gBackupBuffers->mFirstBlock->mPlayerName), gConnectionWork->mChildren.mGameInfo", 1),
    ("src/Main/Net/scan.c", r"MI_CpuCopy8\(&gBackupBuffers->mFirstBlock->mPlayerName, userData",
     "MI_CpuCopy8(port_player_name(gBackupBuffers->mFirstBlock->mPlayerName), userData", 1),
    ("src/Main/Race/race_setup_input.c", r"\*nameSlot = &gBackupBuffers->mFirstBlock->mPlayerName;",
     "*nameSlot = port_player_name(gBackupBuffers->mFirstBlock->mPlayerName);", 1),
    # The game's entry point; `main` is the host program's.
    ("src/Main/System/main.c", r"^void main\(void\) \{", "void GameMain(void) {", 1),
    # VIEWPORT geometry command written by address.
    ("src/Main/Kart/kart_model_menu.c", r"\*\(u32 \*\) 0x4000580 = 0xbfff0000;", "PORT_IO_WRITE(reg_G3_VIEWPORT, 0xbfff0000);", 1),
    # The wireless library reads its variable arguments from the stack after the last named one (CodeWarrior ABI):
    # read them with va_arg instead (they are all 32-bit words).
    ("libs/NitroSDK/wm/src/wireless_system_commands.c", r"args = \(u32 \*\) \(\(\(u32\) &paramNum & ~3\) \+ 4\);",
     "args = 0; __builtin_va_list ap; __builtin_va_start(ap, paramNum);", 2),
    ("libs/NitroSDK/wm/src/wireless_system_commands.c", r"= args\[-1\];", "= __builtin_va_arg(ap, u32);", 1),
    ("libs/NitroSDK/wm/src/wireless_system_commands.c", r"if \(args\[-1\] == current\)",
     "if (__builtin_va_arg(ap, u32) == current)", 1),
    # The member screen of a new group asks a label for its priority and blending before the label has a canvas: on
    # the DS the null pointer reads the memory at address 0 (instruction memory) and the answer is noise.
    ("src/Main/Graphics/layout_entry_angle.c",
     r"    return \(labelTableEntry->mLabelCanvas->mCellData->mPOamAttrArray->mAttr2 & LAYOUT_OAM_PRIORITY_MASK\) >> 10;",
     "    if (labelTableEntry->mLabelCanvas == NULL) return 0;\n"
     "    return (labelTableEntry->mLabelCanvas->mCellData->mPOamAttrArray->mAttr2 & LAYOUT_OAM_PRIORITY_MASK) >> 10;", 1),
    ("src/Main/Graphics/layout_entry_angle.c",
     r"    if \(\(labelTableEntry->mLabelCanvas->mCellData->mPOamAttrArray->mAttr0 & LAYOUT_OAM_MODE_MASK\) >> 10 == 1\) \{",
     "    if (labelTableEntry->mLabelCanvas != NULL &&\n"
     "        (labelTableEntry->mLabelCanvas->mCellData->mPOamAttrArray->mAttr0 & LAYOUT_OAM_MODE_MASK) >> 10 == 1) {", 1),
    # An empty label text given as an 8-bit "" to code that reads 16-bit text: on the DS the alignment padding after it
    # ends the text; here the next string followed (runaway text, stack overflow in the multiplayer menu).
    ("src/Main/Menu/multi_select_member_group.c", r'text = "";', 'text = (void *) L"";', 1),
    # Widescreen: the race and award ceremony cameras take the aspect ratio set by the platform (4:3 by default); the
    # renderer gives each polygon the shape of its own projection (GxPolygon.aspect), so other views stay 4:3.
    ("src/Main/Kart/kart_model_menu.c", r"camera->mAspectRatio = FIXED12_RATIO\(4, 3\);", "camera->mAspectRatio = g_port_aspect;", 1),
    # Name tags over the karts: placed where the (widened) race projection puts the kart, in the 4:3 coordinates of
    # their own projection; scaled so that they stay over the kart on the wide screen.
    ("src/Main/Kart/kart_name_tags.c", r"screenX = \(\(\(s64\) screenX << 19\) \+ 0x800\) >> 12;",
     "screenX = (s32) (((((s64) screenX << 19) + 0x800) >> 12) * g_port_aspect / 0x1555);", 1),
    ("src/Main/MapObjects/trophy.c", r"frustumRight = MUL_FIXED12_NO_ROUND\(-frustumTop, 0x1555\);",
     "frustumRight = MUL_FIXED12_NO_ROUND(-frustumTop, g_port_aspect);", 1),
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
    # Find their variable arguments from the address of the last fixed one (CodeWarrior spills them next to it).
    "libs/NitroSDK/os/src/formatted_text_to_buffer.c": ["OS_SPrintf", "OS_SNPrintf"],
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
            text, count = re.subn(rx, lambda m: rep, text, flags=re.M)
            if count != n:
                sys.exit(f"portify: fixup {rx!r} matched {count} times in {rel}, expected {n}")
        text, names = strip_asm(text)
        text = strip_funcs(text, STRIP_FUNCS.get(rel, []), rel)
        text, _ = rewrite_reg_writes(text, rel)
        stripped += [f"{rel}: {n}" for n in names]
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

    if fixups_by_file:
        sys.exit(f"portify: fixups for files not compiled: {sorted(fixups_by_file)}")
    write_if_changed(out / "sources.txt", "\n".join(sources) + "\n")
    write_if_changed(out / "stripped_asm.txt", "\n".join(stripped) + "\n")
    write_if_changed(out / "overlays.txt", "\n".join(sorted(used_overlays)) + "\n")
    write_if_changed(out / "overlay_units.txt",
                     "".join(f"{overlays[r]} {r}\n" for r in sorted(overlays) if (out / "src" / r).exists()))


if __name__ == "__main__":
    main()
