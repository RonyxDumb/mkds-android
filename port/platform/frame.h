// The state of one displayed frame, captured at VBlank for the renderer thread.
#pragma once

#include "gx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameSnapshot {
    uint8_t io[0x1070];          // 0x04000000..: both 2D engines' registers (engine B at 0x1000)
    uint8_t palette[0x800];      // 0x05000000
    uint8_t oam[0x800];          // 0x07000000
    uint8_t vram[0xa4000];       // banks A..I (see g_vram_bank_offset)
    uint8_t vramcnt[10];         // 0x04000240..0x04000249
    const Gx3dFrame *frame3d;    // displayed 3D list (owned by the renderer side, see frame.c)
    uint64_t number;
} FrameSnapshot;

// Renderer side: called with each new snapshot, on the game thread. The snapshot stays valid until the next call
// returns, so a renderer running on another thread copies what it needs or uses port_frame_acquire/release.
typedef void (*FrameSink)(const FrameSnapshot *snapshot);
void port_frame_set_sink(FrameSink sink);

// Triple buffering for a renderer thread: the latest complete snapshot.
const FrameSnapshot *port_frame_acquire(void); // NULL when nothing new since the last acquire
void port_frame_release(const FrameSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
