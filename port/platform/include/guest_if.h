// How the host side (Android activity, renderer, audio) drives the game. Plain calls in the single-image builds
// (hostside/direct.c); calls into the guest image on a stack below 4 GiB in the arm64 build (hostside/bridge.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "port.h"
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

bool guest_load(void);                        // arm64: map the guest image; false when that is impossible
void guest_start_game(void);                  // starts the game thread (returns at once)
const FrameSnapshot *guest_frame_acquire(void);
void guest_snd_render(int16_t *out, int frames, int rate);
void guest_input_set(const PortInput *in);
void guest_save_flush(void);
void guest_set_paused(bool paused);
void guest_set_vblank_rate(uint32_t mhz);
uint64_t guest_frame_count(void);
// Tests: the game stops after each frame until `done` is posted; `ready` is posted when a frame is captured.
void guest_set_lockstep(void *ready, void *done);

#ifdef __cplusplus
}
#endif
