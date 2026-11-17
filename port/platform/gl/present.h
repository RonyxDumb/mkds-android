#pragma once

#include "frame.h"

// Needs a current OpenGL ES 3 context.
void present_init(void);
// Renders a new snapshot (3D at 256*scale x 192*scale; `wide` scales the 3D horizontally).
void present_update(const FrameSnapshot *f, int scale, float wide);
// Draws the top and bottom screens into the rectangles (x, y, w, h each, y down) of a w x h framebuffer 0.
void present_draw(const int layout[8], int w, int h);
bool present_engine_a_on_top(void);
