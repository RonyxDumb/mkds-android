// Shared presentation on OpenGL ES 3 (Android and the host test build): turns frame snapshots into the two DS
// screens and draws them into rectangles of the current framebuffer.
//
// For each new frame snapshot: the 3D layer is rasterised on the GPU at a resolution that matches the screen
// (gl3d.c), the 2D engines are drawn in software at the native 256x192 and split into what is above and below the
// 3D layer, and a shader composites them with the colour effects and the master brightness. Frames with a display
// capture (dual-screen 3D, menu blur) are drawn entirely in software, because the capture writes into VRAM that the
// next frames display.
#include "port.h"
#include "frame.h"
#include "gpu2d.h"
#include "gl3d.h"
#include "present.h"

#include <GLES3/gl3.h>
#include <string.h>

static const char *VS = "#version 300 es\n"
                        "layout(location = 0) in vec2 pos;\n"
                        "layout(location = 1) in vec2 uv;\n"
                        "out vec2 v_uv;\n"
                        "void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
// A plain 2D screen.
static const char *FS_PLAIN = "#version 300 es\n"
                              "precision mediump float;\n"
                              "uniform sampler2D tex;\n"
                              "in vec2 v_uv;\n"
                              "out vec4 color;\n"
                              "void main() { color = vec4(texture(tex, v_uv).rgb, 1.0); }\n";
// Engine A with the 3D layer: below / 3D / above, then the master brightness.
static const char *FS_COMPOSE =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D above;\n"
    "uniform sampler2D below;\n"
    "uniform sampler2D layer3d;\n"
    "uniform int bright_mode;\n"
    "uniform float bright;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  vec4 up = texture(above, v_uv);\n"
    "  vec4 dn = texture(below, v_uv);\n"
    "  vec4 d3 = texture(layer3d, vec2(v_uv.x, 1.0 - v_uv.y));\n"
    "  vec3 c;\n"
    "  if (d3.a <= 0.0) c = dn.rgb;\n"
    "  else if (dn.a <= 0.0) c = d3.rgb;\n"
    "  else if (d3.a < 0.99) c = mix(dn.rgb, d3.rgb, d3.a);\n"
    "  else c = mix(dn.rgb, d3.rgb, dn.a);\n"
    "  c = mix(c, up.rgb, up.a);\n"
    "  if (bright_mode == 1) c += (1.0 - c) * bright;\n"
    "  else if (bright_mode == 2) c -= c * bright;\n"
    "  color = vec4(c, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        port_log("shader: %s", log);
    }
    return s;
}

static GLuint program(const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

static GLuint make_texture(void) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void upload(GLuint tex, const uint32_t *pixels) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void draw_quad(int x, int y, int w, int h, int surface_w, int surface_h) {
    float x0 = (float) x / surface_w * 2 - 1, x1 = (float) (x + w) / surface_w * 2 - 1;
    float y0 = 1 - (float) y / surface_h * 2, y1 = 1 - (float) (y + h) / surface_h * 2;
    const float v[] = {x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1};
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); // client-side arrays
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

typedef struct Presented {
    bool software; // both screens complete in s_tex_plain[0..1]
    bool a_on_top;
    uint16_t bright; // engine A MASTER_BRIGHT
    GLuint tex3d;
} Presented;

static GLuint s_prog_plain, s_prog_compose, s_tex_plain[2], s_tex_above, s_tex_below;
static GLint s_u_bright_mode, s_u_bright;
static Presented s_shown = {true, true, 0, 0};

void present_init(void) {
    s_prog_plain = program(FS_PLAIN);
    s_prog_compose = program(FS_COMPOSE);
    glUseProgram(s_prog_compose);
    glUniform1i(glGetUniformLocation(s_prog_compose, "above"), 0);
    glUniform1i(glGetUniformLocation(s_prog_compose, "below"), 1);
    glUniform1i(glGetUniformLocation(s_prog_compose, "layer3d"), 2);
    s_u_bright_mode = glGetUniformLocation(s_prog_compose, "bright_mode");
    s_u_bright = glGetUniformLocation(s_prog_compose, "bright");
    s_tex_plain[0] = make_texture();
    s_tex_plain[1] = make_texture();
    s_tex_above = make_texture();
    s_tex_below = make_texture();
    gl3d_init();
}

void present_update(const FrameSnapshot *f, int scale, float wide) {
    static uint32_t buf_a[256 * 192], buf_b[256 * 192], buf_below[256 * 192], layer3d[256 * 192];
    s_shown.a_on_top = gpu2d_engine_a_on_top(f);
    bool capture = f->io[0x67] & 0x80;
    uint32_t dispcnt_a = (uint32_t) f->io[0] | f->io[1] << 8 | f->io[2] << 16 | (uint32_t) f->io[3] << 24;
    bool has_3d = (dispcnt_a & 8) && ((dispcnt_a >> 16) & 3) == 1;
    if (capture || !has_3d) {
        s_shown.software = true;
        if (capture) soft3d_render(&f->frame3d, f, layer3d);
        gpu2d_render(f, capture ? layer3d : NULL, buf_a, buf_b);
        upload(s_tex_plain[0], buf_a);
        upload(s_tex_plain[1], buf_b);
    } else {
        s_shown.software = false;
        gpu2d_render_split(f, buf_a, buf_below, buf_b);
        upload(s_tex_above, buf_a);
        upload(s_tex_below, buf_below);
        upload(s_tex_plain[1], buf_b); // engine B
        s_shown.tex3d = gl3d_render(&f->frame3d, f, scale, wide);
        s_shown.bright = (uint16_t) (f->io[0x6c] | f->io[0x6d] << 8);
    }
}

bool present_engine_a_on_top(void) { return s_shown.a_on_top; }

void present_draw(const int layout[8], int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int screen = 0; screen < 2; screen++) { // 0 = top rectangle, 1 = bottom rectangle
        bool engine_a = (screen == 0) == s_shown.a_on_top;
        const int *r = &layout[screen * 4];
        if (s_shown.software) {
            glUseProgram(s_prog_plain);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_plain[screen]);
        } else if (!engine_a) {
            glUseProgram(s_prog_plain);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_plain[1]);
        } else {
            glUseProgram(s_prog_compose);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_above);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_tex_below);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_shown.tex3d);
            uint32_t mode = s_shown.bright >> 14, factor = s_shown.bright & 31;
            if (factor > 16) factor = 16;
            glUniform1i(s_u_bright_mode, (mode == 1 || mode == 2) ? (int) mode : 0);
            glUniform1f(s_u_bright, factor / 16.0f);
            glActiveTexture(GL_TEXTURE0);
        }
        draw_quad(r[0], r[1], r[2], r[3], w, h);
    }
}
