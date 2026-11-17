#pragma once

#include "frame.h"

void gl3d_init(void);
// Renders the polygon list at 256*scale x 192*scale; `wide` scales clip-space x (1 = 4:3). Returns the colour
// texture (RGBA, alpha 0..1 as the DS 3D alpha).
uint32_t gl3d_render(const Gx3dFrame *frame, const FrameSnapshot *snap, int scale, float wide);
