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
- Online multiplayer: the same Multi-Card Play over the internet, through a small relay server you can host yourself
- Game language follows the phone's language (English, French, German, Italian, Spanish)

## Requirements
- Android 8.0 or newer, ARM (arm64-v8a or armeabi-v7a)
- OpenGL ES 3.0
- A ROM of **Mario Kart DS (Europe)**, game code `AMCP`
  (SHA-1 `ce97d9b43f0d3ca0d48b781983e8a16f6393378f`)

## Installation
1. Download the APK from the [Releases](../../releases) page and install it.
2. On first launch, select your ROM file. It is copied into the app's private storage; the original is not modified.
3. Play. Saves are kept in the app's storage and survive updates (from 0.29 every build is signed with the same key:
   an update installs over the app; earlier builds had a different key each, and an uninstall deletes the save).
   **⚙ → Export save file** keeps a copy anywhere you like; **Import save file** loads one (also a save of the European
   game from a DS or an emulator).

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
| **MIC** (with voice chat) | Microphone on / off |
| **NET** (outside races) | Network settings: local or online play |
| **⚙** | Options (also the gamepad's home button) |
| Touch screen (inset) | Stylus input |

The Android back gesture pauses the game during a race and does nothing elsewhere.

With a gamepad connected, the on-screen controls are hidden (the screen swap button stays). If the gamepad
disconnects during a race, the game pauses and the touch controls come back. L2/R2 also work as L/R.

## Options
**⚙** opens the options over the paused game, in three columns:
- **Graphics**: frame rate (60 fps, or the display's maximum with interpolated frames), 3D resolution (auto or 1× to
  4×), widescreen on or off, one or two screens.
- **Controls**: on-screen controls hidden with a gamepad or always shown, auto-accelerate (A held in races, B still
  brakes: one thumb steers, the other drifts with R), the drift button above A or large and apart, button size and
  opacity, vibration.
- **Game**: unlock everything (every Grand Prix gets a gold trophy with three stars in the save file, which unlocks
  every cup, class, character and kart; applied when the game starts, it restarts for it), export and import of the
  save file, and a home screen shortcut with the game's own icon (read from your ROM).

The app's icon is an adaptive icon with a monochrome layer: with themed icons on (Android 13+), it takes the colours of
the wallpaper (Material You).

## Local multiplayer
1. Connect both phones to the same Wi-Fi network (a hotspot from one of them works too).
2. On the first phone: **Multiplayer → Multi-Card Play → Create Group**.
3. On the second phone: **Multiplayer → Multi-Card Play**, then select the group.
4. On the first phone, touch **Cut Off** and choose the race settings.

Races share every frame's input in lockstep, as on the DS: a stable Wi-Fi connection gives the smoothest play.
DS Download Play is not supported (every player needs the app and the game).

## Online multiplayer
Online play is Multi-Card Play with the phones in different places: up to eight players, every mode of the
multiplayer menu, each with the app and the game. It is not Nintendo Wi-Fi Connection (no friend codes, no
matchmaking with strangers): players meet in a room they agree on.

1. Someone runs the relay server on a machine with a public IPv4 address and an open UDP port (23536 by default).
   It needs only Python 3:
   ```shell
   python3 tools/relay_server.py
   ```
2. Every player touches **NET**, ticks **Play online**, and enters the server's address and the same room code.
   **Test** tells whether the server answers and who is in the room.
3. Then as in local play: one player creates the group in **Multiplayer → Multi-Card Play**, the others select it.

Next to **NET**, the game shows how many players are online in your room right now (everyone who has the game open
with the same server and room code, you included) and their names, or "no server" when the relay does not answer.
Enter your own name in the network settings (**Your name**). It is the name on this list, and its first ten
characters are your name in multiplayer and in races, in place of the nickname of your save file (which is kept:
clear the field to use it again). The game's font has the Latin letters; other characters show as "?".

**NET** turns green while online play is set up; untick the box to play on the local Wi-Fi again. A change applies
the next time the multiplayer menu is entered.

**Voice chat**: tick **Voice chat** in the same settings and allow the microphone. Everyone in the room who has it
on hears each other, in the menus and in races; the **Game volume** slider there turns the game down so that the
voices stand out; **MIC** (green: on, red: off) mutes your microphone. Only speech is
sent, through the relay server. Headphones are best: with the loudspeaker the others may hear the game and
themselves.

The server only introduces the phones and passes their packets on. Phones that can reach each other directly (most
home routers allow it) leave it out after the first second, so the delay is that of the players' own connections.

Every player plays the same frames from the same inputs, as on the DS, so races stay exact. The DS waits for every
player's input before each frame, which only works when the players are a millisecond apart; online, the host keeps
the race at full speed and the other players' inputs take effect a little later instead (by about the time a packet
needs to go to the host and back, so their steering feels delayed on slow connections). A late packet shows as a
single skipped frame. The host plays without delay: the player with the best connection should create the group.
Every player needs the same version of the app.

## Meta Quest (VR)
A second app, built from the same sources, shows the game on a large screen in VR through OpenXR (Quest 2, Pro, 3
and 3S). It installs next to the phone app, with its own save file.

- The app opens in VR: the top screen straight ahead in widescreen, the touch screen to its right, and under it a
  line that tells who is online and whether your microphone is on.
- Point a controller at that line and pull its trigger for the settings panel: choose the ROM (the panel opens by
  itself the first time) and set up the network (online play, voice chat, your name, the game's volume: the same
  settings as on a phone).
- Controllers: left stick steers; **A** accelerate, **B** brake; right trigger or grip **R** (drift), left trigger or
  grip **L** (item); **X**, **Y** as on the DS; the menu button is START, a click of the right stick SELECT. Point a
  controller at the touch screen and pull its trigger to touch it. A click of the left stick turns the microphone on
  and off.

Build it with `./gradlew assembleQuestRelease` (the phone app: `assemblePhoneRelease`).

## Building
Clone the repository (the decompilation is included in `decomp/`):
```shell
git clone https://github.com/RonyxDumb/mkds-android
```

Requirements:
- Android SDK (platform 35), NDK `28.2.13676358`, CMake `3.22.1`, JDK 17
- For the arm64 build: `gcc-aarch64-linux-gnu` (GCC 13; newer versions fail on `-mabi=ilp32`), Python 3, Ninja

```shell
./gradlew assembleRelease
```
The APKs are written to `app/build/outputs/apk/phone/release/` and `app/build/outputs/apk/quest/release/`. The GitHub Actions workflow
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
| `decomp/` | The decompilation, with the changes the port needs made directly in its sources (the original [mkds](https://github.com/RonyxDumb/mkds) is not modified) |
| `tools/portify.py` | Collects the sources the port compiles and checks them (register writes) |
| `port/platform/` | DS platform layer: memory map, OS, 2D and 3D graphics, sound driver, wireless, file system from the ROM |
| `tools/relay_server.py` | The relay server for online play |
| `port/guest/` | 64-bit ARM: the game runs as a 32-bit-pointer image in its own process |
| `app/` | Android application (`src/quest`: the Meta Quest panel and VR activity; `port/platform/android/xr.c`: OpenXR) |

- **CPU**: the game assumes 32-bit pointers. On 32-bit ARM it runs as is; on 64-bit ARM it is compiled with GCC's
  ILP32 AArch64 target and runs in a separate process, where the DS address space is free.
- **Graphics**: the DS geometry engine is reimplemented in C; its polygon lists are drawn with OpenGL ES 3 at high
  resolution. The 2D engines are rendered in software, wider than the DS screen for widescreen.
- **Sound**: the DS sound driver (the ARM7 side) is reimplemented natively and mixes into Android's audio output.
- **Wireless**: the game's own wireless library runs unchanged; its ARM7 side is reimplemented over UDP on the local
  network. Online, the same datagrams go through the relay server or, once the phones have found each other,
  directly between them (`port/platform/hostside/online.h`).

## Legal
This project contains no Nintendo code or assets. *Mario Kart DS* and Nintendo DS are trademarks of Nintendo.
This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Only use it with a game you own.
