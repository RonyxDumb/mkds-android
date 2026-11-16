# Mario Kart DS — native Android port

Native port of *Mario Kart DS* (EUR, `AMCP`) built from the [RonyxDumb/mkds](https://github.com/RonyxDumb/mkds)
decompilation (`decomp/`, git submodule). No emulation: the game's C code is compiled with the NDK clang and runs
directly on the phone's CPU; the Nintendo DS hardware the game talks to is replaced by a native platform layer.

Goals: 16:9 widescreen, 120/240 fps presentation, high internal resolution.

**No game content in this repository.** On first launch the app asks for your own ROM dump
(SHA-1 `ce97d9b43f0d3ca0d48b781983e8a16f6393378f`) and reads the game files from it.

## Layout
| Path | Contents |
| --- | --- |
| `decomp/` | The decompilation (submodule), compiled unmodified |
| `port/compat/` | Clang compatibility: force-included prefix header and shadow headers (same relative path as in `decomp/include`, searched first) |
| `port/overrides/` | Native replacements for decomp files that drive the DS hardware directly (SDK `OS`, `MI`, `GX`, `SND`, `FS` ...) |
| `port/platform/` | Platform layer: DS memory map, 2D/3D graphics on OpenGL ES, sound, file system from the ROM, input |
| `app/` | Android application (Gradle) |
| `tools/` | Build helpers |

## Architecture
- **ABI**: `armeabi-v7a` first. The game, its file formats and the SDK assume 32-bit pointers; a 32-bit process keeps
  every structure and every file loaded in place exactly as on the DS.
- **Hardware**: register writes with side effects (3D geometry commands, divider/sqrt, DMA, IPC to the ARM7) go
  through the platform layer; the rest of the I/O space, VRAM, OAM and palettes are plain memory the renderers read.
- **ARM7**: not part of the decomp. Its services (sound sequence player, touch screen, RTC, saves) are reimplemented
  natively behind the same IPC interface.
- **Graphics**: the DS geometry engine is reimplemented in C (matrix stacks, lighting, clipping) and its polygon
  lists are rasterised with OpenGL ES 3 at high resolution. Widescreen widens the 3D projection and the viewport;
  high frame rates interpolate between consecutive game frames.

## Status
- [x] Decomp compiles with clang (syntax level) apart from SDK hardware files
- [ ] Platform layer: memory map, OS, file system
- [ ] 2D/3D renderer (GLES 3)
- [ ] Sound
- [ ] Android app, input, saves
- [ ] Widescreen, 120/240 fps
