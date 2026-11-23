// Host frame output: both screens (top above bottom) as a PNG, drawn by the platform's 2D renderer with the software
// 3D rasteriser for the 3D layer.
#include "port.h"
#include "frame.h"
#include "gpu2d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    }
    c ^= 0xffffffffu;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc(0, (const uint8_t *) type, 4);
    c = crc(c, data, len);
    uint8_t cb[4];
    be32(cb, c);
    fwrite(cb, 1, 4, f);
}

// RGB PNG with stored (uncompressed) deflate blocks.
static void write_png(const char *path, const uint32_t *rgba, int w, int h) {
    size_t raw_len = (size_t) h * (1 + w * 3);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t) y * (1 + w * 3);
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = rgba[y * w + x];
            row[1 + x * 3] = (uint8_t) c;
            row[2 + x * 3] = (uint8_t) (c >> 8);
            row[3 + x * 3] = (uint8_t) (c >> 16);
        }
    }
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *o++ = off + n == raw_len;
        *o++ = (uint8_t) n;
        *o++ = (uint8_t) (n >> 8);
        *o++ = (uint8_t) ~n;
        *o++ = (uint8_t) (~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    be32(o, (b << 16) | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) {
        port_log("cannot write %s", path);
        free(raw);
        free(z);
        return;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t) w);
    be32(ihdr + 4, (uint32_t) h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t) (o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

extern float g_host_aspect;
#ifdef HOST_GL
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "present.h"

// OpenGL ES output: the same presentation code as on Android, into a pbuffer, read back as PNG.
#define GL_SCALE 3
static bool gl_frame(const FrameSnapshot *f, const char *path) {
    static bool ready, failed;
    static int W, H;
    if (!W) {
        W = (int) (192 * GL_SCALE * g_host_aspect + 0.5f) & ~3;
        H = 384 * GL_SCALE;
    }
    if (failed) return false;
    if (!ready) {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(dpy, NULL, NULL)) {
            failed = true;
            return false;
        }
        const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
        if (n < 1) {
            port_log("host: no EGL config");
            failed = true;
            return false;
        }
        const EGLint pb_attr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
        EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        eglMakeCurrent(dpy, surf, surf, ctx);
        port_log("host: GL %s", (const char *) glGetString(GL_RENDERER));
        present_init();
        ready = true;
    }
    if (getenv("MKDS_DROPCAP") && f->number % 3 == 0) ((FrameSnapshot *) f)->io[0x67] &= 0x7f; // tests: as on phones
    present_set_wide(g_host_aspect);
    present_update(f, GL_SCALE);
    if (!path) return true; // the frame taken, not drawn
    int layout[8] = {0, 0, W, H / 2, 0, H / 2, W, H / 2};
    static int one = -1;
    if (one < 0) one = getenv("MKDS_ONE") != NULL; // tests: the phone's one-screen picture (in the upper half)
    if (one) {
        int same[8] = {0, 0, W, H / 2, 0, 0, W, H / 2};
        memcpy(layout, same, sizeof(layout));
    }
    const char *interp = getenv("MKDS_INTERP"); // tests: draw the 3D between the last two frames
    present_draw(layout, W, H, interp ? (float) atof(interp) : 1.0f);
    static uint32_t pixels[768 * GL_SCALE * 384 * GL_SCALE], flipped[768 * GL_SCALE * 384 * GL_SCALE];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int y = 0; y < H; y++) memcpy(flipped + y * W, pixels + (H - 1 - y) * W, W * 4);
    write_png(path, flipped, W, H);
    return true;
}
#endif

float g_host_aspect = 4.0f / 3.0f; // --wide: the top screen's aspect ratio (OpenGL output only)

// Tests (MKDS_PARTS): each layer of each engine alone, side by side (BG0..3, OBJ; transparent = grey), with the
// layer settings in the name: path-parts.png.
static void write_parts(const FrameSnapshot *f, const char *path) {
    static uint32_t part[256 * 192], sheet[6 * 256 * 2 * 192];
    for (int e = 0; e < 2; e++) {
        for (int l = 0; l < 6; l++) {
            int back = gpu2d_back_layer_2d(f, e == 0);
            unsigned keep = l < 5 ? 1u << l : GPU2D_KEEP_ALL & ~GPU2D_KEEP_BD & ~(back >= 0 ? 1u << back : 0);
            gpu2d_render_part(f, e == 0, keep, part, NULL, 0);
            for (int y = 0; y < 192; y++) {
                for (int x = 0; x < 256; x++) {
                    uint32_t c = part[y * 256 + x];
                    if (!(c >> 24)) c = ((x / 8 + y / 8) & 1) ? 0xff606060u : 0xff808080u;
                    sheet[(e * 192 + y) * 1536 + l * 256 + x] = c;
                }
            }
        }
    }
    char p[600];
    snprintf(p, sizeof(p), "%s-parts.png", path);
    write_png(p, sheet, 1536, 384);
    const uint16_t *a = (const uint16_t *) f->io, *b = (const uint16_t *) (f->io + 0x1000);
    port_log("parts %s: A dispcnt %04x%04x bgcnt %04x %04x %04x %04x | B dispcnt %04x%04x bgcnt %04x %04x %04x %04x | "
             "A on top %d", path, a[1], a[0], a[4], a[5], a[6], a[7], b[1], b[0], b[4], b[5], b[6], b[7],
             gpu2d_engine_a_on_top(f));
}

void host_write_frame(const FrameSnapshot *f, const char *path) {
    if (getenv("MKDS_PARTS")) write_parts(f, path);
    if (getenv("MKDS_POLYLOG")) { // tests: the 3D list's viewports and projection aspects
        const Gx3dFrame *g = &f->frame3d;
        int shown = 0;
        uint32_t last = 0xffffffffu;
        for (uint32_t i = 0; i < g->polygon_count && shown < 8; i++) {
            const GxPolygon *p = &g->polygons[i];
            uint32_t key = (uint32_t) p->viewport[0] | p->viewport[1] << 8 | p->viewport[2] << 16 |
                           (uint32_t) p->viewport[3] << 24;
            key ^= (uint32_t) p->aspect * 2654435761u;
            if (key == last) continue;
            last = key;
            shown++;
            port_log("poly %u: viewport %d %d %d %d aspect %04x", i, p->viewport[0], p->viewport[1], p->viewport[2],
                     p->viewport[3], (unsigned) p->aspect);
        }
    }
#ifdef HOST_GL
    if (gl_frame(f, path)) return;
#endif
    static uint32_t screens[2][256 * 192];
    static uint32_t layer3d[256 * 192];
    soft3d_render(&f->frame3d, f, layer3d);
    gpu2d_render(f, layer3d, screens[0], screens[1], 0);
    static uint32_t out[256 * 384];
    memcpy(out, screens[0], sizeof(screens[0]));
    memcpy(out + 256 * 192, screens[1], sizeof(screens[1]));
    write_png(path, out, 256, 384);
}

// A frame that is not written out but has a display capture: the capture is made as on the phones (the game shows
// the captured pictures later: the race map, menu blurs).
void host_capture_frame(const FrameSnapshot *f) {
#ifdef HOST_GL
    // As on the phones, every frame goes through the presentation (the dual-screen intros follow the frames).
    static int every = -1;
    if (every < 0) every = getenv("MKDS_EVERY") != NULL;
    if (every) {
        present_set_wide(g_host_aspect);
        if (gl_frame(f, NULL)) return;
    }
#endif
    static uint32_t screens[2][GPU2D_MAX_W * 192], layer3d[256 * 192];
    soft3d_render(&f->frame3d, f, layer3d);
    gpu2d_render(f, layer3d, screens[0], screens[1], 0);
}
