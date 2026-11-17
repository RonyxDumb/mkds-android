// Android entry points (com.mkds.port.Native).
#include "port.h"
#include "frame.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

void GameMain(void);
void port_overlays_init(void);
void android_render_set_window(ANativeWindow *window);
void android_render_set_layout(const int *rects);
extern volatile bool g_paused;

static void *game_thread(void *arg) {
    (void) arg;
    port_os_run(GameMain);
    port_log("game thread ended");
    return NULL;
}

// The DS address space must be reserved before anything else claims the low addresses: do it at load time.
static bool s_memory_ok;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) vm, (void) reserved;
    s_memory_ok = port_mem_init();
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL Java_com_mkds_port_Native_start(JNIEnv *env, jclass cls, jstring rom, jstring save) {
    (void) cls;
    if (!s_memory_ok) {
        return (*env)->NewStringUTF(env, "This device does not let the game reserve its memory at the addresses it "
                                         "needs (0x02000000-0x08000000).");
    }
    const char *rom_path = (*env)->GetStringUTFChars(env, rom, NULL);
    const char *save_path = (*env)->GetStringUTFChars(env, save, NULL);
    bool ok = port_rom_open(rom_path);
    if (ok) {
        port_save_set_path(save_path);
    }
    (*env)->ReleaseStringUTFChars(env, rom, rom_path);
    (*env)->ReleaseStringUTFChars(env, save, save_path);
    if (!ok) {
        return (*env)->NewStringUTF(env, "The ROM could not be opened.");
    }
    port_overlays_init();
    gx3d_init();
    g_vblank_rate_mhz = 59826;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_t thread;
    pthread_create(&thread, &attr, game_thread, NULL);
    pthread_attr_destroy(&attr);
    return NULL;
}

JNIEXPORT void JNICALL Java_com_mkds_port_Native_setSurface(JNIEnv *env, jclass cls, jobject surface) {
    (void) cls;
    android_render_set_window(surface ? ANativeWindow_fromSurface(env, surface) : NULL);
}

JNIEXPORT void JNICALL Java_com_mkds_port_Native_setLayout(JNIEnv *env, jclass cls, jint tx, jint ty, jint tw, jint th,
                                                           jint bx, jint by, jint bw, jint bh) {
    (void) env, (void) cls;
    int rects[8] = {tx, ty, tw, th, bx, by, bw, bh};
    android_render_set_layout(rects);
}

JNIEXPORT void JNICALL Java_com_mkds_port_Native_setInput(JNIEnv *env, jclass cls, jint buttons, jboolean touching,
                                                          jint x, jint y) {
    (void) env, (void) cls;
    PortInput in = {(uint16_t) buttons, touching, (uint16_t) x, (uint16_t) y, false};
    port_input_set(&in);
}

JNIEXPORT void JNICALL Java_com_mkds_port_Native_setPaused(JNIEnv *env, jclass cls, jboolean paused) {
    (void) env, (void) cls;
    g_paused = paused;
    if (paused) {
        port_save_flush();
    }
}
