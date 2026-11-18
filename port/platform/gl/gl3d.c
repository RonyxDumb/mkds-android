// The DS 3D rasteriser on OpenGL ES 3, at any internal resolution.
//
// The polygon list from the geometry engine is already in clip space; OpenGL clips and rasterises it. Per frame:
// clear (colour, alpha, depth), opaque polygons, then translucent ones in list order (the game sorts them), with
// shadow volumes through the stencil buffer. Textures are decoded from the frame's VRAM into a cache that is
// dropped whenever the texture VRAM changes.
#include "port.h"
#include "gpu2d.h"
#include "tex.h"
#include "gl3d.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vertex {
    float x, y, z, w;
    float r, g, b, a;
    float s, t;
    float mode; // 0 modulate, 1 decal, 2 toon/highlight, 3 shadow; +4 when textured
} Vertex;

static const char *VS = "#version 300 es\n"
                        "layout(location = 0) in vec4 pos;\n"
                        "layout(location = 1) in vec4 color;\n"
                        "layout(location = 2) in vec2 uv;\n"
                        "layout(location = 3) in float mode;\n"
                        "uniform vec2 tex_size;\n"
                        "uniform float wide;\n"
                        "out vec4 v_color;\n"
                        "out vec2 v_uv;\n"
                        "flat out float v_mode;\n"
                        "void main() {\n"
                        "  v_color = color; v_uv = uv / (16.0 * tex_size); v_mode = mode;\n"
                        "  gl_Position = vec4(pos.x * wide, pos.y, pos.z, pos.w);\n"
                        "}\n";
static const char *FS = "#version 300 es\n"
                        "precision highp float;\n"
                        "uniform sampler2D tex;\n"
                        "uniform float alpha_ref;\n"
                        "uniform bool highlight;\n"
                        "uniform vec3 toon[32];\n"
                        "in vec4 v_color;\n"
                        "in vec2 v_uv;\n"
                        "flat in float v_mode;\n"
                        "out vec4 color;\n"
                        "void main() {\n"
                        "  vec4 c = v_color;\n"
                        "  float m = v_mode;\n"
                        "  bool textured = m >= 4.0;\n"
                        "  if (textured) m -= 4.0;\n"
                        "  vec3 add = vec3(0.0);\n"
                        "  if (m == 2.0) {\n" // toon / highlight: the vertex red picks a toon table colour
                        "    vec3 tc = toon[int(clamp(floor(c.r * 31.0 + 0.5), 0.0, 31.0))];\n"
                        "    if (highlight) { c.rgb = vec3(c.r); add = tc; } else c.rgb = tc;\n"
                        "  }\n"
                        "  if (textured) {\n"
                        "    vec4 t = texture(tex, v_uv);\n"
                        "    if (m == 1.0) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
                        "    else c *= t;\n"
                        "  }\n"
                        "  c.rgb = min(c.rgb + add, vec3(1.0));\n"
                        // The DS never draws pixels with alpha 0; the alpha test drops those not above the reference.
                        "  if (c.a < 0.5 / 31.0 || c.a <= alpha_ref) discard;\n"
                        "  color = c;\n"
                        "}\n";

static GLuint s_prog, s_vao, s_vbo, s_fbo, s_color, s_depth;
static GLint s_u_tex_size, s_u_alpha_ref, s_u_wide, s_u_highlight, s_u_toon;
static int s_w, s_h;
static Vertex *s_verts;
static size_t s_vert_cap;

// ------------------------------------------------------------------------------------------------ textures
typedef struct TexEntry {
    uint32_t key, pltt;
    GLuint tex;
    uint16_t w, h;
} TexEntry;
#define TEX_CACHE 1024
static TexEntry s_cache[TEX_CACHE];
static int s_cache_count;
static uint64_t s_vram_hash;

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

static void cache_flush(void) {
    for (int i = 0; i < s_cache_count; i++) glDeleteTextures(1, &s_cache[i].tex);
    s_cache_count = 0;
}

// Drops every texture when the texture image or palette VRAM changed since the last frame.
static void cache_validate(const VramMap *map, const uint8_t *vram) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int region = VR_TEX; region <= VR_TEXPAL; region++) {
        for (int page = 0; page < 48; page++) {
            int32_t off = map->page[region][page];
            h = (h ^ (uint64_t) (off + 1)) * 0x100000001b3ull;
            if (off >= 0) h = hash_bytes(vram + off, 0x4000, h);
        }
    }
    if (h != s_vram_hash) {
        s_vram_hash = h;
        cache_flush();
    }
}

static GLuint texture(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt) {
    if (((teximage >> 26) & 7) == 0) return 0;
    uint32_t key = teximage & 0x3ff0ffff; // address, size, format, colour 0 transparent (not wrap/flip/transform)
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 0) return 0;
    if (fmt == 7) pltt = 0;
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].key == key && s_cache[i].pltt == pltt) return s_cache[i].tex;
    }
    if (s_cache_count == TEX_CACHE) cache_flush();
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t *texels = malloc(w * h * 4);
    tex_decode(map, vram, teximage, pltt, texels);
    for (uint32_t i = 0; i < w * h; i++) { // alpha 0..31 to 0..255
        uint32_t a = texels[i] >> 24;
        texels[i] = (texels[i] & 0xffffff) | ((a * 255 + 15) / 31) << 24;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) w, (GLsizei) h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    free(texels);
    s_cache[s_cache_count].key = key;
    s_cache[s_cache_count].pltt = pltt;
    s_cache[s_cache_count].tex = tex;
    s_cache[s_cache_count].w = (uint16_t) w;
    s_cache[s_cache_count].h = (uint16_t) h;
    s_cache_count++;
    return tex;
}

static void set_wrap(uint32_t teximage) {
    GLint ws = !(teximage & (1u << 16)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    GLint wt = !(teximage & (1u << 17)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
}

// ------------------------------------------------------------------------------------------------ setup
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        port_log("gl3d shader: %s", log);
    }
    return s;
}

static void ensure_target(int w, int h) {
    if (w == s_w && h == s_h) return;
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_color);
        glDeleteRenderbuffers(1, &s_depth);
    }
    s_w = w;
    s_h = h;
    glGenTextures(1, &s_color);
    glBindTexture(GL_TEXTURE_2D, s_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &s_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        port_log("gl3d: framebuffer incomplete");
    }
}

void gl3d_init(void) {
    s_prog = glCreateProgram();
    glAttachShader(s_prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(s_prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(s_prog);
    s_u_tex_size = glGetUniformLocation(s_prog, "tex_size");
    s_u_alpha_ref = glGetUniformLocation(s_prog, "alpha_ref");
    s_u_wide = glGetUniformLocation(s_prog, "wide");
    s_u_highlight = glGetUniformLocation(s_prog, "highlight");
    s_u_toon = glGetUniformLocation(s_prog, "toon");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "tex"), 0);
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 16);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 32);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 40);
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray((GLuint) i);
    glBindVertexArray(0);
}

// ------------------------------------------------------------------------------------------------ frame
typedef struct Batch {
    uint32_t first, count; // in s_verts (triangles)
    GLuint tex;
    uint32_t teximage;
    uint32_t size_bits; // TEXIMAGE_PARAM, for the texture size
    uint32_t attr;
    uint8_t viewport[4];
    uint8_t id; // polygon ID
    int32_t aspect; // of the projection
    bool translucent, shadow, shadow_mask;
} Batch;

static Batch *s_batches;
static size_t s_batch_cap;

// Frame interpolation pairs the models of two lists. A model piece is a vertex block (BEGIN_VTXS): its polygons,
// culled ones included (GxPolygon.hidden), are the same every frame, so a block is found in the previous list by a
// signature of its structure (polygon shapes, texture formats and sizes, attributes); among blocks with the same
// structure (wheels, item boxes, trees) the one at the same place in the list, or a much nearer one (see match_lists).
// A block not found, or found too far away (a cut), is drawn as it is, whole: the parts of a model never separate.
typedef struct Block {
    uint32_t first, count, sig;
    float cx, cy, cw; // centre in clip space (sums over the vertices / their number)
} Block;

static Block *s_blocks[2];
static size_t s_block_cap[2];
static int32_t *s_match; // per polygon of the current list: index in the previous list, or -1
static size_t s_match_cap;
static uint8_t *s_used;
static size_t s_used_cap;

static size_t find_blocks(const Gx3dFrame *f, int which) {
    size_t n = 0;
    for (uint32_t i = 0; i < f->polygon_count;) {
        uint32_t j = i + 1;
        while (j < f->polygon_count && f->polygons[j].tag != 0) j++;
        if (n + 1 > s_block_cap[which]) {
            s_block_cap[which] = (n + 1) * 2;
            s_blocks[which] = realloc(s_blocks[which], s_block_cap[which] * sizeof(Block));
        }
        Block *b = &s_blocks[which][n++];
        b->first = i;
        b->count = j - i;
        uint32_t h = 0x811c9dc5u ^ (j - i);
        double sx = 0, sy = 0, sw = 0;
        int nv = 0;
        for (uint32_t k = i; k < j; k++) {
            const GxPolygon *p = &f->polygons[k];
            // The structure only: animated textures (texture coordinates scrolling, frames of a texture animation,
            // palette changes) must not make a piece unrecognisable from one frame to the next.
            h = (h ^ p->count) * 0x01000193u;
            h = (h ^ (p->teximage & 0x3ff00000u)) * 0x01000193u; // format, size, repeat, flip, colour 0
            h = (h ^ (p->attr & ~0x001f0000u)) * 0x01000193u;
            for (int v = 0; v < p->count; v++) {
                const GxVertex *g = &f->vertices[p->first + v];
                sx += g->x;
                sy += g->y;
                sw += g->w;
                nv++;
            }
        }
        b->sig = h;
        b->cx = (float) (sx / nv);
        b->cy = (float) (sy / nv);
        b->cw = (float) (sw / nv);
        i = j;
    }
    return n;
}

static const Block *s_sort_blocks;
static int cmp_block_sig(const void *a, const void *b) {
    uint32_t x = s_sort_blocks[*(const uint32_t *) a].sig, y = s_sort_blocks[*(const uint32_t *) b].sig;
    return x < y ? -1 : x > y;
}

static uint32_t *s_order[2];
static size_t s_order_cap[2];

static uint32_t *sorted_by_sig(const Block *blocks, size_t n, int which) {
    if (n + 1 > s_order_cap[which]) {
        s_order_cap[which] = (n + 1) * 2;
        s_order[which] = realloc(s_order[which], s_order_cap[which] * sizeof(uint32_t));
    }
    for (size_t i = 0; i < n; i++) s_order[which][i] = (uint32_t) i;
    s_sort_blocks = blocks;
    qsort(s_order[which], n, sizeof(uint32_t), cmp_block_sig);
    return s_order[which];
}

static float block_distance(const Block *a, const Block *b) {
    return fabsf(a->cx - b->cx) + fabsf(a->cy - b->cy) + fabsf(a->cw - b->cw);
}

// Pairs the blocks of the two lists, in list order. A model draws its pieces in the same order every frame, so the
// block at the place where the lists line up so far is the first choice, even when another identical piece is a
// little nearer: an item box is made of identical pieces close together, which "the nearest one" swaps as soon as the
// camera moves faster than they are apart. Another identical block is taken only when it is much nearer (the expected
// one belongs to an object that has gone: an item box taken, a tree out of view), and if no other current piece is
// much nearer to it.
static void match_lists(const Gx3dFrame *prev, const Gx3dFrame *f) {
    size_t np = find_blocks(prev, 0), nc = find_blocks(f, 1);
    if (f->polygon_count + 1 > s_match_cap) {
        s_match_cap = (f->polygon_count + 1) * 2;
        s_match = realloc(s_match, s_match_cap * sizeof(*s_match));
    }
    if (np + 1 > s_used_cap) {
        s_used_cap = (np + 1) * 2;
        s_used = realloc(s_used, s_used_cap);
    }
    memset(s_used, 0, np);
    for (uint32_t i = 0; i < f->polygon_count; i++) s_match[i] = -1;
    const Block *pb = s_blocks[0], *cb = s_blocks[1];
    const uint32_t *po = sorted_by_sig(pb, np, 0), *co = sorted_by_sig(cb, nc, 1);
    long offset = 0; // where the lists line up so far
    for (size_t c = 0; c < nc; c++) {
        const Block *b = &cb[c];
        // The previous blocks with this structure: po[lo..hi).
        size_t lo = 0, hi = np;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (pb[po[mid]].sig < b->sig) lo = mid + 1;
            else hi = mid;
        }
        hi = lo;
        while (hi < np && pb[po[hi]].sig == b->sig) hi++;
        if (lo == hi) continue;
        long want = (long) c + offset, near = -1;
        float want_d = -1, near_d = 0;
        if (want >= 0 && want < (long) np && !s_used[want] && pb[want].sig == b->sig) want_d = block_distance(&pb[want], b);
        for (size_t k = lo; k < hi; k++) {
            if (s_used[po[k]]) continue;
            float d = block_distance(&pb[po[k]], b);
            if (near < 0 || d < near_d) {
                near = (long) po[k];
                near_d = d;
            }
        }
        long best = -1;
        float best_d = 0;
        if (want_d >= 0 && (want_d <= near_d * 3.0f + 64.0f || near == want)) {
            best = want;
            best_d = want_d;
        } else if (near >= 0) {
            // Mutual: no other current piece with this structure is clearly nearer to it.
            bool mutual = true;
            size_t clo = 0, chi = nc;
            while (clo < chi) {
                size_t mid = (clo + chi) / 2;
                if (cb[co[mid]].sig < b->sig) clo = mid + 1;
                else chi = mid;
            }
            for (size_t o = clo; o < nc && cb[co[o]].sig == b->sig && mutual; o++) {
                if (co[o] != c && block_distance(&pb[near], &cb[co[o]]) < near_d * 0.6f) mutual = false;
            }
            if (mutual) {
                best = near;
                best_d = near_d;
            }
        }
        if (best < 0) continue;
        // A cut or a jump: further than the depth of the piece (clip space works behind the camera too).
        float scale = fmaxf(fabsf(pb[best].cw), fabsf(b->cw)) + 4096.0f;
        if (best_d > scale) continue;
        s_used[best] = 1;
        offset = best - (long) c;
        for (uint32_t k = 0; k < b->count; k++) s_match[b->first + k] = (int32_t) (pb[best].first + k);
    }
}

static float s_target_aspect = 4.0f / 3.0f; // of the render target

static void push_polygon(const Gx3dFrame *f, const GxPolygon *p, Vertex **out, GLuint tex, const Gx3dFrame *prev,
                         const GxPolygon *pp, float t) {
    // A polygon keeps the shape its projection gives it: one projected for 4:3 drawn over the whole wide screen is
    // narrowed about the centre (it may lie beyond the 4:3 part: name tags over karts). Partial viewports: below.
    float xs = 1.0f;
    if (p->viewport[0] == 0 && p->viewport[2] == 255) {
        xs = (p->aspect > 0 ? p->aspect / 4096.0f : 4.0f / 3.0f) / s_target_aspect;
        if (xs > 1.0f) xs = 1.0f;
    }

    uint32_t alpha = (p->attr >> 16) & 31;
    if (alpha == 0) alpha = 31; // wireframe: drawn filled
    uint32_t mode = (p->attr >> 4) & 3;
    float m = (float) mode + (tex ? 4.0f : 0.0f);
    Vertex v[4];
    for (int k = 0; k < p->count; k++) {
        const GxVertex *g = &f->vertices[p->first + k];
        float x = (float) g->x, y = (float) g->y, z = (float) g->z, w = (float) g->w;
        if (pp) {
            const GxVertex *o = &prev->vertices[pp->first + k];
            x = o->x + (x - o->x) * t;
            y = o->y + (y - o->y) * t;
            z = o->z + (z - o->z) * t;
            w = o->w + (w - o->w) * t;
        }
        v[k] = (Vertex) {x * xs / 4096.0f, y / 4096.0f, z / 4096.0f, w / 4096.0f,
                         g->r / 31.0f, g->g / 31.0f, g->b / 31.0f, alpha / 31.0f,
                         g->s, g->t, m};
    }
    // Triangle fan.
    for (int k = 1; k + 1 < p->count; k++) {
        *(*out)++ = v[0];
        *(*out)++ = v[k];
        *(*out)++ = v[k + 1];
    }
}

uint32_t gl3d_render(const Gx3dFrame *f, const Gx3dFrame *prev, float t, const FrameSnapshot *snap, int scale,
                     float wide) {
    int w = (int) (256 * scale * wide + 0.5f), h = 192 * scale;
    float sx = w / 256.0f; // horizontal pixels per DS pixel
    s_target_aspect = 4.0f / 3.0f * wide;
    if (prev && (t >= 1.0f || prev->vertex_count == 0)) prev = NULL;
    if (prev) match_lists(prev, f);
    ensure_target(w, h);
    VramMap map;
    vram_map_build(&map, snap->vramcnt);
    bool texturing = f->disp3dcnt & 1;
    if (texturing) cache_validate(&map, snap->vram);

    size_t need = (size_t) f->polygon_count * 6 + 6;
    if (need > s_vert_cap) {
        s_vert_cap = need * 2;
        s_verts = realloc(s_verts, s_vert_cap * sizeof(Vertex));
    }
    if (f->polygon_count + 1 > s_batch_cap) {
        s_batch_cap = (f->polygon_count + 1) * 2;
        s_batches = realloc(s_batches, s_batch_cap * sizeof(Batch));
    }

    // Opaque polygons first, translucent ones after, each in list order; consecutive polygons with the same state
    // form one draw call.
    size_t nb = 0;
    Vertex *out = s_verts;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < f->polygon_count; i++) {
            const GxPolygon *p = &f->polygons[i];
            if (p->hidden) continue;
            uint32_t alpha = (p->attr >> 16) & 31;
            uint32_t fmt = (p->teximage >> 26) & 7;
            bool shadow = ((p->attr >> 4) & 3) == 3;
            bool translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6 || shadow;
            if (translucent != (pass == 1)) continue;
            GLuint tex = texturing ? texture(&map, snap->vram, p->teximage, p->pltt) : 0;
            Batch *b = nb ? &s_batches[nb - 1] : NULL;
            uint32_t state_attr = p->attr & 0x0000c800; // depth equal, depth write of translucent, ...
            uint8_t id = (uint8_t) ((p->attr >> 24) & 63);
            bool shadow_mask = shadow && id == 0;
            if (!b || b->tex != tex || b->teximage != (p->teximage & 0x000f0000) || b->size_bits != p->teximage || b->attr != state_attr ||
                memcmp(b->viewport, p->viewport, 4) != 0 || b->translucent != translucent || b->shadow != shadow ||
                b->shadow_mask != shadow_mask || b->id != id || b->aspect != p->aspect) {
                b = &s_batches[nb++];
                b->first = (uint32_t) (out - s_verts);
                b->count = 0;
                b->tex = tex;
                b->teximage = p->teximage & 0x000f0000;
                b->size_bits = p->teximage;
                b->attr = state_attr;
                memcpy(b->viewport, p->viewport, 4);
                b->translucent = translucent;
                b->shadow = shadow;
                b->shadow_mask = shadow_mask;
                b->id = id;
                b->aspect = p->aspect;
            }
            Vertex *start = out;
            const GxPolygon *pp = prev && s_match[i] >= 0 ? &prev->polygons[s_match[i]] : NULL;
            push_polygon(f, p, &out, tex, prev, pp, t);
            b->count += (uint32_t) (out - start);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    uint32_t cc = f->clear_color;
    glClearColor((cc & 31) / 31.0f, ((cc >> 5) & 31) / 31.0f, ((cc >> 10) & 31) / 31.0f, ((cc >> 16) & 31) / 31.0f);
    glClearDepthf(((f->clear_depth & 0x7fff) * 0x200 + 0x1ff) / (float) 0xffffff);
    glClearStencil((GLint) ((cc >> 24) & 63)); // the stencil keeps the opaque polygon ID (bits 0-5), shadow mark bit 7
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glUseProgram(s_prog);
    glUniform1f(s_u_wide, 1.0f); // the game projects the wide view itself (g_port_aspect)
    float alpha_ref = (f->disp3dcnt & 4) ? f->alpha_ref / 31.0f : 0.0f;
    glUniform1f(s_u_alpha_ref, alpha_ref > 0 ? alpha_ref : -1.0f);
    glUniform1i(s_u_highlight, (f->disp3dcnt & 2) != 0);
    float toon[32 * 3];
    for (int i = 0; i < 32; i++) {
        uint16_t c = f->toon_table[i];
        toon[i * 3] = (c & 31) / 31.0f;
        toon[i * 3 + 1] = ((c >> 5) & 31) / 31.0f;
        toon[i * 3 + 2] = ((c >> 10) & 31) / 31.0f;
    }
    glUniform3fv(s_u_toon, 32, toon);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) ((out - s_verts) * sizeof(Vertex)), s_verts, GL_STREAM_DRAW);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    bool blending = f->disp3dcnt & 8;

    for (size_t i = 0; i < nb; i++) {
        Batch *b = &s_batches[i];
        if (!b->count) continue;
        int vx = b->viewport[0], vy = b->viewport[1];
        int vw = b->viewport[2] - vx + 1, vh = b->viewport[3] - vy + 1;
        // A polygon keeps the shape its projection gives it: a view projected for 4:3 on a wider target takes the
        // centred 4:3 part of it.
        float xs = 1.0f;
        if (!(b->viewport[0] == 0 && b->viewport[2] == 255)) { // a partial viewport: its 4:3 place, centred
            xs = (b->aspect > 0 ? b->aspect / 4096.0f : 4.0f / 3.0f) / s_target_aspect;
            if (xs > 1.0f) xs = 1.0f;
        }
        float left = (w - w * xs) / 2;
        glViewport((GLint) (left + vx * sx * xs + 0.5f), vy * scale, (GLsizei) (vw * sx * xs + 0.5f), vh * scale);
        // Depth "equal" (decals such as the karts' emblems): the DS accepts depths within a small margin; a
        // polygon offset towards the viewer with less-or-equal does the same.
        bool equal = b->attr & (1u << 14);
        glDepthFunc(equal ? GL_LEQUAL : GL_LESS);
        if (equal) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -4.0f);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (b->tex) {
            glBindTexture(GL_TEXTURE_2D, b->tex);
            set_wrap(b->teximage);
            glUniform2f(s_u_tex_size, (float) TEX_WIDTH(b->size_bits), (float) TEX_HEIGHT(b->size_bits));
        } else {
            glUniform2f(s_u_tex_size, 1, 1);
        }
        if (b->shadow) {
            // Shadow volumes: the mask (polygon ID 0) marks the stencil (bit 7) where the depth test fails; a shadow
            // polygon then draws where the mark is, except on pixels of opaque polygons with its own ID, and clears it.
            glEnable(GL_STENCIL_TEST);
            glDepthMask(GL_FALSE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glStencilMask(0x80);
            if (b->shadow_mask) {
                glStencilFunc(GL_ALWAYS, 0x80, 0xff);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
            } else {
                glStencilFunc(GL_EQUAL, 0x80 | b->id, 0xbf); // own ID: no shadow
                glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glStencilFunc(GL_EQUAL, 0x80, 0x80);
                glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilMask(0xff);
            glDisable(GL_STENCIL_TEST);
            continue;
        }
        glEnable(GL_STENCIL_TEST);
        if (!b->translucent) {
            // Opaque polygons record their ID for the shadows.
            glStencilMask(0x7f);
            glStencilFunc(GL_ALWAYS, b->id, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        } else {
            // A translucent pixel is not drawn over a translucent pixel of the same polygon ID (the DS keeps that ID
            // with bit 6 set); it records its own.
            glStencilMask(0x7f);
            glStencilFunc(GL_NOTEQUAL, 0x40 | b->id, 0x7f);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
        if (b->translucent && blending) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask((b->attr & (1u << 11)) ? GL_TRUE : GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
        glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
    }
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glStencilMask(0xff);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return s_color;
}
