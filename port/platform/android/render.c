// Presentation on Android: a render thread with an OpenGL ES 3 context on the activity's surface. Each new frame
// snapshot is drawn into two 256x192 textures (2D engines in software, 3D layer from the software rasteriser for now)
// and shown in the layout the Java side chose, at the display's refresh rate.
#include "port.h"
#include "frame.h"
#include "gpu2d.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *s_window;       // requested window
static bool s_window_changed;
static int s_layout[8] = {0, 0, 256, 192, 256, 0, 256, 192};
static pthread_t s_thread;
static bool s_thread_running;

static const char *VS = "#version 300 es\n"
                        "layout(location = 0) in vec2 pos;\n"
                        "layout(location = 1) in vec2 uv;\n"
                        "out vec2 v_uv;\n"
                        "void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS = "#version 300 es\n"
                        "precision mediump float;\n"
                        "uniform sampler2D tex;\n"
                        "in vec2 v_uv;\n"
                        "out vec4 color;\n"
                        "void main() { color = vec4(texture(tex, v_uv).rgb, 1.0); }\n";

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

static void draw_quad(int x, int y, int w, int h, int surface_w, int surface_h) {
    float x0 = (float) x / surface_w * 2 - 1, x1 = (float) (x + w) / surface_w * 2 - 1;
    float y0 = 1 - (float) y / surface_h * 2, y1 = 1 - (float) (y + h) / surface_h * 2;
    const float v[] = {x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void *render_thread(void *arg) {
    (void) arg;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLSurface surf = EGL_NO_SURFACE;
    ANativeWindow *window = NULL;
    GLuint prog = 0, tex[2] = {0, 0};
    static uint32_t screens[2][256 * 192], layer3d[256 * 192];

    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (!s_window && !s_window_changed) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        if (s_window_changed) {
            s_window_changed = false;
            if (surf != EGL_NO_SURFACE) {
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(dpy, surf);
                surf = EGL_NO_SURFACE;
            }
            if (window) {
                ANativeWindow_release(window);
            }
            window = s_window;
            if (window) {
                ANativeWindow_acquire(window);
                surf = eglCreateWindowSurface(dpy, cfg, window, NULL);
                eglMakeCurrent(dpy, surf, surf, ctx);
                eglSwapInterval(dpy, 1);
                if (!prog) {
                    prog = glCreateProgram();
                    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
                    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
                    glLinkProgram(prog);
                    glGenTextures(2, tex);
                    for (int i = 0; i < 2; i++) {
                        glBindTexture(GL_TEXTURE_2D, tex[i]);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }
                }
            }
        }
        bool have_window = window != NULL;
        int layout[8];
        memcpy(layout, s_layout, sizeof(layout));
        pthread_mutex_unlock(&s_lock);
        if (!have_window) {
            continue;
        }

        const FrameSnapshot *f = port_frame_acquire();
        if (f) {
            soft3d_render(&f->frame3d, f, layer3d);
            gpu2d_render(f, layer3d, screens[0], screens[1]);
            for (int i = 0; i < 2; i++) {
                glBindTexture(GL_TEXTURE_2D, tex[i]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, screens[i]);
            }
        }
        int w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            draw_quad(layout[i * 4], layout[i * 4 + 1], layout[i * 4 + 2], layout[i * 4 + 3], w, h);
        }
        eglSwapBuffers(dpy, surf);
    }
    return NULL;
}

void android_render_set_window(ANativeWindow *window) {
    pthread_mutex_lock(&s_lock);
    if (s_window) {
        ANativeWindow_release(s_window);
    }
    s_window = window;
    s_window_changed = true;
    if (!s_thread_running) {
        s_thread_running = true;
        pthread_create(&s_thread, NULL, render_thread, NULL);
    }
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_lock);
    // A destroyed surface must not be used after surfaceDestroyed returns: wait for the render thread to let go.
    while (!window) {
        pthread_mutex_lock(&s_lock);
        bool pending = s_window_changed;
        pthread_mutex_unlock(&s_lock);
        if (!pending) break;
        usleep(1000);
    }
}

void android_render_set_layout(const int *rects) {
    pthread_mutex_lock(&s_lock);
    memcpy(s_layout, rects, sizeof(s_layout));
    pthread_mutex_unlock(&s_lock);
}
