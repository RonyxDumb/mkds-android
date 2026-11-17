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

void host_write_frame(const FrameSnapshot *f, const char *path) {
    static uint32_t screens[2][256 * 192];
    static uint32_t layer3d[256 * 192];
    soft3d_render(&f->frame3d, f, layer3d);
    gpu2d_render(f, layer3d, screens[0], screens[1]);
    static uint32_t out[256 * 384];
    memcpy(out, screens[0], sizeof(screens[0]));
    memcpy(out + 256 * 192, screens[1], sizeof(screens[1]));
    write_png(path, out, 256, 384);
}
