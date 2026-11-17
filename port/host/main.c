// Headless test runner (32-bit Linux): runs the game as fast as possible, feeds scripted input and writes frames as
// images. Usage: mkds <rom.nds> [--frames N] [--dump-every K] [--out DIR] [--press FRAME:BUTTONS[:LEN]]...
// BUTTONS is a string of A B s(elect) S(tart) R L u d l r X Y, or "t<x>,<y>" for a touch.
#include "port.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void host_write_frame(const FrameSnapshot *f, const char *path); // host/render.c

typedef struct Press {
    uint64_t frame, length;
    uint16_t buttons;
    bool touch;
    uint16_t x, y;
} Press;

static Press s_presses[256];
static int s_press_count;
static uint64_t s_frames = 600, s_dump_every = 60;
static const char *s_out = "frames";

#include "guest_if.h"
#ifdef PORT_IPC
#include "ipc/ipc.h"
#include <time.h>
#endif
#include "hostapi.h"

void *host_low_alloc(size_t size);
static FILE *s_wav;
static uint32_t s_wav_frames;
static double s_audio_accum;

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, rate = 48000, byte_rate = rate * 4;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    uint32_t riff = 36 + data;
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_len = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite(&fmt_len, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

// One captured frame: scripted input for the next one, audio, image.
static bool s_bglog;
static bool on_frame(const FrameSnapshot *f, uint64_t n) {
    if (s_bglog) {
        const uint16_t *io = (const uint16_t *) f->io;
        port_log("bg %llu cnt %04x %04x %04x %04x ofs %d %d %d %d %d %d %d %d", (unsigned long long) n, io[4], io[5], io[6],
                 io[7], io[8], io[9], io[10], io[11], io[12], io[13], io[14], io[15]);
    }
    static PortInput *in;
    if (!in) in = host_low_alloc(sizeof(*in));
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < s_press_count; i++) {
        if (n >= s_presses[i].frame && n < s_presses[i].frame + s_presses[i].length) {
            in->buttons |= s_presses[i].buttons;
            if (s_presses[i].touch) {
                in->touching = true;
                in->touch_x = s_presses[i].x;
                in->touch_y = s_presses[i].y;
            }
        }
    }
    guest_input_set(in);
    s_audio_accum += 48000.0 / 59.8261;
    int samples = (int) s_audio_accum;
    s_audio_accum -= samples;
    static int16_t audio[2048 * 2];
    guest_snd_render(audio, samples, 48000);
    if (s_wav) {
        fwrite(audio, 4, (size_t) samples, s_wav);
        s_wav_frames += (uint32_t) samples;
    }
    if (s_dump_every && n % s_dump_every == 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05llu.png", s_out, (unsigned long long) n);
        host_write_frame(f, path);
        port_log("frame %llu: DISPCNT A %08x B %08x, 3D %u polygons, POWCNT %04x", (unsigned long long) n,
                 *(uint32_t *) f->io, *(uint32_t *) (f->io + 0x1000), f->frame3d.polygon_count,
                 *(uint16_t *) (f->io + 0x304));
    }
    return n < s_frames;
}

static void parse_press(const char *spec) {
    Press p = {0};
    char buttons[64] = "";
    unsigned long long frame = 0, length = 4;
    if (sscanf(spec, "%llu:%63[^:]:%llu", &frame, buttons, &length) < 2) {
        port_fatal("bad --press %s", spec);
    }
    p.frame = frame;
    p.length = length;
    if (buttons[0] == 't') {
        unsigned x, y;
        sscanf(buttons + 1, "%u,%u", &x, &y);
        p.touch = true;
        p.x = (uint16_t) x;
        p.y = (uint16_t) y;
    } else {
        for (const char *c = buttons; *c; c++) {
            switch (*c) {
            case 'A': p.buttons |= PAD_A; break;
            case 'B': p.buttons |= PAD_B; break;
            case 's': p.buttons |= PAD_SELECT; break;
            case 'S': p.buttons |= PAD_START; break;
            case 'R': p.buttons |= PAD_R; break;
            case 'L': p.buttons |= PAD_L; break;
            case 'u': p.buttons |= PAD_UP; break;
            case 'd': p.buttons |= PAD_DOWN; break;
            case 'l': p.buttons |= PAD_LEFT; break;
            case 'r': p.buttons |= PAD_RIGHT; break;
            case 'X': p.buttons |= PAD_X; break;
            case 'Y': p.buttons |= PAD_Y; break;
            }
        }
    }
    if (s_press_count < 256) {
        s_presses[s_press_count++] = p;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.nds [--frames N] [--dump-every K] [--out DIR] [--realtime] [--press F:BTN[:LEN]]\n",
                argv[0]);
        return 1;
    }
    uint32_t rate = 0;
    const char *save = "mkds.sav";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) s_frames = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) s_dump_every = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) s_out = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) parse_press(argv[++i]);
        else if (!strcmp(argv[i], "--realtime")) rate = 59826;
        else if (!strcmp(argv[i], "--bglog")) s_bglog = true;
        else if (!strcmp(argv[i], "--keyon")) {
            extern bool g_snd_keyon_log;
            g_snd_keyon_log = true;
        }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            s_wav = fopen(argv[++i], "wb");
            wav_header(s_wav, 0);
        }
    }
    mkdir(s_out, 0755);
#ifdef PORT_IPC
    // Two processes, as on arm64 Android: mkds_server (next to this program) runs the game in real time.
    (void) rate;
    char exe[600];
    snprintf(exe, sizeof(exe), "%s", argv[0]);
    char *slash = strrchr(exe, '/');
    snprintf(slash ? slash + 1 : exe, sizeof(exe) - (size_t) (slash ? slash + 1 - exe : 0), "mkds_server");
    if (!guest_load()) port_fatal("cannot create the shared memory");
    const char *err = ipc_start(exe, argv[1], save, ".", 59826);
    if (err) port_fatal("%s", err);
    uint64_t last = 0;
    for (;;) {
        const FrameSnapshot *f = guest_frame_acquire();
        if (!f || f->number <= last) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
            continue;
        }
        if (f->number > last + 1 && last) port_log("ipc: skipped %llu frames", (unsigned long long) (f->number - last - 1));
        last = f->number;
        if (!on_frame(f, f->number - 1)) {
            port_log("done after %llu frames", (unsigned long long) s_frames);
            guest_save_flush();
            if (s_wav) {
                wav_header(s_wav, s_wav_frames);
                fclose(s_wav);
            }
            exit(0);
        }
    }
#endif
    if (!port_mem_init() || !guest_load()) {
        port_fatal("cannot reserve the DS address space / load the game");
    }
    if (!port_rom_open(argv[1])) {
        return 1;
    }
    port_save_set_path(save);
    guest_set_vblank_rate(rate);
    void *ready = hapi_sem_new(), *done = hapi_sem_new();
    guest_set_lockstep(ready, done);
    guest_start_game();
    for (;;) {
        hapi_sem_wait(ready);
        const FrameSnapshot *f = guest_frame_acquire();
        bool more = !f || on_frame(f, f->number - 1);
        if (!more) {
            port_log("done after %llu frames", (unsigned long long) s_frames);
            guest_save_flush();
            if (s_wav) {
                wav_header(s_wav, s_wav_frames);
                fclose(s_wav);
            }
            exit(0);
        }
        hapi_sem_post(done);
    }
    return 0;
}
