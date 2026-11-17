// The host API (include/hostapi.h) on POSIX / Android: threads, clocks, the ROM file and the save file.
#include "port.h"
#include "hostapi.h"

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

void hapi_log(const char *line) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "mkds", line);
    extern void android_log_line(const char *line);
    android_log_line(line);
#else
    fprintf(stderr, "%s\n", line);
#endif
}

void hapi_abort(void) { abort(); }

uint64_t hapi_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

void hapi_sleep_ns(uint64_t ns) {
    struct timespec ts = {(time_t) (ns / 1000000000ull), (long) (ns % 1000000000ull)};
    nanosleep(&ts, NULL);
}

void hapi_local_time(HostTime *out) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    out->year = tm.tm_year + 1900;
    out->month = tm.tm_mon + 1;
    out->day = tm.tm_mday;
    out->weekday = tm.tm_wday;
    out->hour = tm.tm_hour;
    out->minute = tm.tm_min;
    out->second = tm.tm_sec;
}

// Objects handed to the guest live in memory it can address (below 4 GiB in the arm64 build).
void *host_low_alloc(size_t size);

void *hapi_mutex_new(void) {
    pthread_mutex_t *m = host_low_alloc(sizeof(*m));
    pthread_mutex_init(m, NULL);
    return m;
}
void hapi_mutex_lock(void *m) { pthread_mutex_lock(m); }
void hapi_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *hapi_sem_new(void) {
    sem_t *s = host_low_alloc(sizeof(*s));
    sem_init(s, 0, 0);
    return s;
}
void hapi_sem_free(void *s) { sem_destroy(s); }
void hapi_sem_post(void *s) { sem_post(s); }
void hapi_sem_wait(void *s) {
    while (sem_wait(s) != 0) {
    }
}

static pthread_key_t s_tls_key;
static pthread_once_t s_tls_once = PTHREAD_ONCE_INIT;
static void tls_init(void) { pthread_key_create(&s_tls_key, NULL); }
void *hapi_tls_get(void) {
    pthread_once(&s_tls_once, tls_init);
    return pthread_getspecific(s_tls_key);
}
void hapi_tls_set(void *value) {
    pthread_once(&s_tls_once, tls_init);
    pthread_setspecific(s_tls_key, value);
}

// Threads that run guest code: their stacks must be addressable by it.
void host_start_guest_thread(void (*fn)(void *), void *arg); // bridge or direct
void hapi_thread_start(void (*fn)(void *), void *arg) { host_start_guest_thread(fn, arg); }
void hapi_thread_exit(void) {
    pthread_exit(NULL);
}

uint8_t *hapi_vram_banks(void) { return g_vram_banks; }
void hapi_vram_remap(int bank) { port_vram_remap(bank); }

// ------------------------------------------------------------------------------------------------ ROM and save
static const uint8_t *s_rom;
static uint32_t s_rom_size;
static char s_save_path[512];

bool port_rom_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        port_log("rom: cannot open %s", path);
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    void *p = host_low_alloc((size_t) st.st_size);
    size_t done = 0;
    while (p && done < (size_t) st.st_size) {
        ssize_t n = read(fd, (uint8_t *) p + done, (size_t) st.st_size - done);
        if (n <= 0) break;
        done += (size_t) n;
    }
    close(fd);
    if (!p || done != (size_t) st.st_size || st.st_size < 0x4000) {
        port_log("rom: cannot read %s", path);
        return false;
    }
    s_rom = p;
    s_rom_size = (uint32_t) st.st_size;
    if (memcmp(s_rom + 0x0c, "AMCP", 4) != 0) {
        port_log("rom: game code %.4s, expected AMCP (Mario Kart DS, Europe)", (const char *) s_rom + 0x0c);
        return false;
    }
    return true;
}

const uint8_t *hapi_rom(uint32_t *size) {
    *size = s_rom_size;
    return s_rom;
}

void port_save_set_path(const char *path) { snprintf(s_save_path, sizeof(s_save_path), "%s", path); }

uint32_t hapi_save_read(void *data, uint32_t size) {
    FILE *f = fopen(s_save_path, "rb");
    if (!f) return 0;
    size_t n = fread(data, 1, size, f);
    fclose(f);
    port_log("save: loaded %zu bytes from %s", n, s_save_path);
    return (uint32_t) n;
}

bool hapi_save_write(const void *data, uint32_t size) {
    if (!s_save_path[0]) return false;
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        port_log("save: cannot write %s", tmp);
        return false;
    }
    fwrite(data, 1, size, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, s_save_path) == 0;
}
