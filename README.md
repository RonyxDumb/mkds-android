# Mario Kart DS for Android

A native Android port of *Mario Kart DS*, built from the [mkds](https://github.com/RonyxDumb/mkds) decompilation.

This is **not an emulator**: the game's C code is compiled for ARM and runs directly on your phone, with the Nintendo
DS hardware replaced by a native platform layer (OpenGL ES 3 graphics, native sound driver, Android input and audio).

**No game content is included.** You need your own dump of the game.

## Features
- Native performance on 32-bit and 64-bit ARM phones
- High-resolution 3D rendering
- Full widescreen: races, menus and award ceremonies fill the whole display
- 120 fps (and higher) on high refresh rate displays, through frame interpolation
- Touch controls designed for phones, with a floating stick
- Gamepad support (USB and Bluetooth); on-screen controls hide automatically
- Local multiplayer over Wi-Fi between phones running the app (Multi-Card Play)
- Game language follows the phone's language (English, French, German, Italian, Spanish)

## Requirements
- Android 8.0 or newer, ARM (arm64-v8a or armeabi-v7a)
- OpenGL ES 3.0
- A ROM of **Mario Kart DS (Europe)**, game code `AMCP`
  (SHA-1 `ce97d9b43f0d3ca0d48b781983e8a16f6393378f`)

## Installation
1. Download the APK from the [Releases](../../releases) page and install it.
2. On first launch, select your ROM file. It is copied into the app's private storage; the original is not modified.
3. Play. Saves are kept in the app's storage and survive updates.

## Controls
| Touch | Action |
| --- | --- |
| Left half of the screen | Floating stick (appears where your thumb lands) |
| **A** (large) / **R** (above it) | Accelerate / drift — one thumb between them holds both |
| **B** | Brake / reverse |
| **L** | Use item |
| **X**, **Y** | As on the DS |
| **SELECT**, **START** | As on the DS |
| ⇄ | Swap the screens: the touch screen becomes large, for menus |
| Touch screen (inset) | Stylus input |

The Android back gesture pauses the game during a race and does nothing elsewhere.

With a gamepad connected, the on-screen controls are hidden (the screen swap button stays). If the gamepad
disconnects during a race, the game pauses and the touch controls come back. L2/R2 also work as L/R.

## Local multiplayer
1. Connect both phones to the same Wi-Fi network (a hotspot from one of them works too).
2. On the first phone: **Multiplayer → Multi-Card Play → Create Group**.
3. On the second phone: **Multiplayer → Multi-Card Play**, then select the group.
4. On the first phone, touch **Cut Off** and choose the race settings.

Races share every frame's input in lockstep, as on the DS: a stable Wi-Fi connection gives the smoothest play.
DS Download Play is not supported (every player needs the app and the game).

## Building
Clone with the decompilation submodule:
```shell
git clone --recursive https://github.com/RonyxDumb/mkds-android
```

Requirements:
- Android SDK (platform 35), NDK `28.2.13676358`, CMake `3.22.1`, JDK 17
- For the arm64 build: `gcc-aarch64-linux-gnu` (GCC 13; newer versions fail on `-mabi=ilp32`), Python 3, Ninja

```shell
./gradlew assembleRelease
```
The APK is written to `app/build/outputs/apk/release/`. The GitHub Actions workflow
(`.github/workflows/android.yml`) builds it on every push.

### Linux test build
A headless build runs the game on a PC and writes both screens as PNG images, for debugging:
```shell
CC=clang CFLAGS=-m32 cmake -S port -B build/host -G Ninja
ninja -C build/host
build/host/mkds rom.nds --frames 1800 --dump-every 100 --out frames --press 330:A:4
```

## How it works
| Path | Contents |
| --- | --- |
| `decomp/` | The decompilation (submodule) |
| `tools/portify.py` | Prepares the decompiled sources for the port (a few documented fixups) |
| `port/platform/` | DS platform layer: memory map, OS, 2D and 3D graphics, sound driver, wireless, file system from the ROM |
| `port/guest/` | 64-bit ARM: the game runs as a 32-bit-pointer image in its own process |
| `app/` | Android application |

- **CPU**: the game assumes 32-bit pointers. On 32-bit ARM it runs as is; on 64-bit ARM it is compiled with GCC's
  ILP32 AArch64 target and runs in a separate process, where the DS address space is free.
- **Graphics**: the DS geometry engine is reimplemented in C; its polygon lists are drawn with OpenGL ES 3 at high
  resolution. The 2D engines are rendered in software, wider than the DS screen for widescreen.
- **Sound**: the DS sound driver (the ARM7 side) is reimplemented natively and mixes into Android's audio output.
- **Wireless**: the game's own wireless library runs unchanged; its ARM7 side is reimplemented over UDP on the local
  network.

## Legal
This project contains no Nintendo code or assets. *Mario Kart DS* and Nintendo DS are trademarks of Nintendo.
This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Only use it with a game you own.
