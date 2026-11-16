// What one displayed frame needs: the 3D polygon list with its rendering state, and the 2D engines' registers and
// memories. Built by gx3d.c (polygons) and frame.c (snapshot), consumed by the renderers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_MAX_POLYGONS 4096
#define GX_MAX_VERTICES 16384

typedef struct GxVertex {
    int32_t x, y, z, w; // clip space, 20.12 fixed point (before the viewport transform)
    int16_t s, t;       // texture coordinates, 12.4 texels
    uint8_t r, g, b;    // 0..31 (lit and clamped)
    uint8_t pad;
} GxVertex;

typedef struct GxPolygon {
    uint32_t first;      // index of the first vertex
    uint8_t count;       // 3 or 4
    uint8_t viewport[4]; // x1, y1, x2, y2 (DS screen coordinates, y up from the bottom as in VIEWPORT)
    uint8_t front;       // 1 when the polygon faces the viewer
    uint32_t attr;       // POLYGON_ATTR latched at BEGIN_VTXS
    uint32_t teximage;   // TEXIMAGE_PARAM
    uint32_t pltt;       // PLTT_BASE
    uint32_t tag;        // identity of the draw call for frame interpolation (matrix + command position hash)
} GxPolygon;

typedef struct Gx3dFrame {
    GxVertex vertices[GX_MAX_VERTICES];
    GxPolygon polygons[GX_MAX_POLYGONS];
    uint32_t vertex_count, polygon_count;
    uint32_t swap_param; // SWAP_BUFFERS: bit 0 manual translucent sort, bit 1 W-buffering
    // Rendering registers latched at the swap.
    uint16_t disp3dcnt;
    uint32_t clear_color;
    uint16_t clear_depth;
    uint16_t clrimage_offset;
    uint32_t fog_color;
    uint16_t fog_offset;
    uint8_t fog_table[32];
    uint16_t edge_color[8];
    uint16_t toon_table[32];
    uint8_t alpha_ref;
    uint64_t serial; // increases with every list
} Gx3dFrame;

// The 3D list being displayed (swapped in at the last VBlank).
const Gx3dFrame *gx3d_displayed(void);

#ifdef __cplusplus
}
#endif
