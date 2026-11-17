// Android entry points (com.mkds.port.Native).
#include "port.h"
#include "frame.h"
#include "guest_if.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>

void android_render_set_window(ANativeWindow *window);
void android_render_set_layout(const int *rects);

// The DS address space must be reserved before anything else claims the low addresses: do it at load time.
static bool s_memory_ok;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) vm, (void) reserved;
    s_memory_ok = port_mem_init() && guest_load();
    return JNI_VERSION_1_6;
}

void android_crash_init(const char *dir);

JNIEXPORT void JNICALL Java_com_mkds_port_Native_init(JNIEnv *env, jclass cls, jstring dir) {
    (void) cls;
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    android_crash_init(d);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    port_log("memory map %s", s_memory_ok ? "reserved" : "NOT reserved");
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
    guest_set_vblank_rate(59826);
    guest_start_game();
    extern void android_audio_start(void);
    android_audio_start();
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
    static PortInput *in; // in memory the guest can address
    if (!in) {
        extern void *host_low_alloc(size_t size);
        in = host_low_alloc(sizeof(*in));
    }
    *in = (PortInput) {(uint16_t) buttons, touching, (uint16_t) x, (uint16_t) y, false};
    guest_input_set(in);
}

JNIEXPORT void JNICALL Java_com_mkds_port_Native_setPaused(JNIEnv *env, jclass cls, jboolean paused) {
    (void) env, (void) cls;
    guest_set_paused(paused);
    extern void android_audio_pause(bool paused);
    android_audio_pause(paused);
    if (paused) {
        guest_save_flush();
    }
}
