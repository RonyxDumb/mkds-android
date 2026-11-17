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
- [x] The whole decomp compiles with clang (ARMv7 Android, 32-bit Linux for tests)
- [x] Platform layer: memory map, OS threads/interrupts/alarms, DMA, card + save, firmware NVRAM, services
- [x] Boots to the menus and runs races (host test build, software renderer)
- [ ] OpenGL ES 3 renderer (high resolution)
- [ ] Android app: input, ROM import, audio output, saves
- [ ] Sound driver (sequence player, mixer)
- [ ] Widescreen, 120/240 fps

## Testing on Linux
```shell
CC=clang CFLAGS=-m32 LDFLAGS="-m32 -fuse-ld=lld" cmake -S port -B build/host -G Ninja
ninja -C build/host
build/host/mkds rom.nds --frames 1800 --dump-every 100 --out frames --press 330:A:4
```
Runs headless on a virtual clock and writes both screens as PNG.
