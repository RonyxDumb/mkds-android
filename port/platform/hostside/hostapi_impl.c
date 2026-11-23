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
// fn and arg come from the guest: 32-bit values (only the low half of the registers is defined).
void hapi_thread_start(void (*fn)(void *), void *arg) {
    host_start_guest_thread((void (*)(void *)) (uintptr_t) (uint32_t) (uintptr_t) fn,
                            (void *) (uintptr_t) (uint32_t) (uintptr_t) arg);
}
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

// ------------------------------------------------------------------------------------------------ local network
// Two UDP sockets: one on an ephemeral port for everything a station sends and receives directly, one on the fixed
// beacon port that only listens for beacons (shared by every instance on the device: broadcasts reach them all).
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "online.h"

#define NET_BEACON_PORT 23535
// Pointers from the guest: 32-bit values (only the low half of the registers is defined in the arm64 build).
#define GUEST_PTR(p) ((void *) (uintptr_t) (uint32_t) (uintptr_t) (p))

static int s_sock = -1, s_beacon_sock = -1;

// The game has socket functions of its own with the same names (its Wi-Fi library's), which the 32-bit builds link
// into the same image: take the C library's directly.
#include <dlfcn.h>
static struct {
    int (*socket)(int, int, int);
    int (*bind)(int, const struct sockaddr *, socklen_t);
    int (*setsockopt)(int, int, int, const void *, socklen_t);
    ssize_t (*sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    ssize_t (*recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
    int (*getifaddrs)(struct ifaddrs **);
    void (*freeifaddrs)(struct ifaddrs *);
    int (*getaddrinfo)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    void (*freeaddrinfo)(struct addrinfo *);
} libc_net;

static bool libc_net_load(void) {
    if (libc_net.socket) return true;
#ifdef __ANDROID__
    void *libc = dlopen("libc.so", RTLD_NOW);
#else
    void *libc = dlopen("libc.so.6", RTLD_NOW);
#endif
    if (!libc) return false;
    libc_net.socket = (int (*)(int, int, int)) dlsym(libc, "socket");
    libc_net.bind = (int (*)(int, const struct sockaddr *, socklen_t)) dlsym(libc, "bind");
    libc_net.setsockopt = (int (*)(int, int, int, const void *, socklen_t)) dlsym(libc, "setsockopt");
    libc_net.sendto = (ssize_t (*)(int, const void *, size_t, int, const struct sockaddr *, socklen_t)) dlsym(libc, "sendto");
    libc_net.recvfrom = (ssize_t (*)(int, void *, size_t, int, struct sockaddr *, socklen_t *)) dlsym(libc, "recvfrom");
    libc_net.getifaddrs = (int (*)(struct ifaddrs **)) dlsym(libc, "getifaddrs");
    libc_net.freeifaddrs = (void (*)(struct ifaddrs *)) dlsym(libc, "freeifaddrs");
    libc_net.getaddrinfo =
        (int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **)) dlsym(libc, "getaddrinfo");
    libc_net.freeaddrinfo = (void (*)(struct addrinfo *)) dlsym(libc, "freeaddrinfo");
    return libc_net.socket && libc_net.bind && libc_net.setsockopt && libc_net.sendto && libc_net.recvfrom;
}

static int udp_socket(uint16_t port, bool reuse) {
    int fd = libc_net.socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    libc_net.setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    if (reuse) {
        libc_net.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (libc_net.bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        port_log("net: cannot bind port %u (%s)", port, strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

// Online play (online.h) is set up in "<save file>.online": one line, "host port room", written by the app's network
// settings; no file or an empty one is local play. The test build takes it from MKDS_ONLINE ("host port room").
// Read each time the wireless starts, so a change applies the next time the game's multiplayer menu is entered.
static void online_load_config(void) {
    char line[256] = "";
    char path[600];
    snprintf(path, sizeof(path), "%s.online", s_save_path[0] ? s_save_path : "mkds");
    FILE *f = fopen(path, "rb");
    if (f) {
        if (!fgets(line, sizeof(line), f)) line[0] = 0;
        fclose(f);
    }
#ifndef __ANDROID__
    if (getenv("MKDS_ONLINE")) snprintf(line, sizeof(line), "%s", getenv("MKDS_ONLINE"));
#endif
    line[strcspn(line, "\r\n")] = 0;
    OnlineLibc libc = {libc_net.sendto, libc_net.getaddrinfo, libc_net.freeaddrinfo};
    online_configure(&libc, line);
}

bool hapi_net_open(void) {
    if (!libc_net_load()) {
        port_log("net: no socket functions");
        return false;
    }
    online_load_config();
    if (s_sock >= 0) return true;
    s_sock = udp_socket(0, false);
    s_beacon_sock = udp_socket(NET_BEACON_PORT, true);
    if (s_sock < 0) {
        port_log("net: no socket (%s)", strerror(errno));
        return false;
    }
    port_log("net: open%s", s_beacon_sock < 0 ? " (beacons cannot be received)" : "");
    return true;
}

void hapi_net_close(void) {
    if (s_sock >= 0) close(s_sock);
    if (s_beacon_sock >= 0) close(s_beacon_sock);
    s_sock = s_beacon_sock = -1;
}

void hapi_net_mac(uint8_t *mac) {
    mac = GUEST_PTR(mac);
    static uint8_t s_mac[6];
    static bool s_have;
    if (!s_have) {
        // Kept next to the save file, so that the address stays the same from one start to the next.
        char path[600];
        snprintf(path, sizeof(path), "%s.mac", s_save_path[0] ? s_save_path : "mkds");
        FILE *f = fopen(path, "rb");
        if (!f || fread(s_mac, 1, 6, f) != 6) {
            uint64_t r = hapi_now_ns() ^ ((uint64_t) getpid() << 32);
            int rnd = open("/dev/urandom", O_RDONLY);
            if (rnd >= 0) {
                if (read(rnd, &r, sizeof(r)) != (ssize_t) sizeof(r)) r ^= (uint64_t) time(NULL);
                close(rnd);
            }
            s_mac[0] = 0x00, s_mac[1] = 0x09, s_mac[2] = 0xbf; // a DS address
            s_mac[3] = (uint8_t) r, s_mac[4] = (uint8_t) (r >> 8), s_mac[5] = (uint8_t) (r >> 16);
            if (f) fclose(f);
            f = fopen(path, "wb");
            if (f) fwrite(s_mac, 1, 6, f);
        }
        if (f) fclose(f);
        s_have = true;
    }
    memcpy(mac, s_mac, 6);
}

static void send_to(uint32_t ip, uint16_t port, const void *data, uint32_t size) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    libc_net.sendto(s_sock, data, size, 0, (struct sockaddr *) &a, sizeof(a));
}

// Tests on one computer: MKDS_NET_SIM="delay_ms,jitter_ms,loss_percent" delays, shuffles and drops the datagrams
// sent to other stations, like a busy Wi-Fi network.
#ifndef __ANDROID__
typedef struct SimPacket {
    uint64_t due;
    uint32_t ip, port, size;
    uint8_t data[1600];
} SimPacket;
static SimPacket s_sim[512];
static int s_sim_delay = -1, s_sim_jitter, s_sim_loss;
static uint32_t s_sim_rand = 12345;

static uint32_t sim_rand(void) {
    s_sim_rand = s_sim_rand * 1103515245u + 12345u;
    return s_sim_rand >> 8;
}

static void sim_flush(void) {
    uint64_t now = hapi_now_ns();
    for (int i = 0; i < 512; i++) {
        if (s_sim[i].size && s_sim[i].due <= now) {
            send_to(s_sim[i].ip, (uint16_t) s_sim[i].port, s_sim[i].data, s_sim[i].size);
            s_sim[i].size = 0;
        }
    }
}

static bool sim_send(uint32_t ip, uint32_t port, const void *data, uint32_t size) {
    if (s_sim_delay < 0) {
        const char *e = getenv("MKDS_NET_SIM");
        s_sim_delay = 0;
        if (e) sscanf(e, "%d,%d,%d", &s_sim_delay, &s_sim_jitter, &s_sim_loss);
        s_sim_rand ^= (uint32_t) getpid();
    }
    if (!s_sim_delay && !s_sim_jitter && !s_sim_loss) return false;
    if (s_sim_loss && (int) (sim_rand() % 100) < s_sim_loss) return true;
    for (int i = 0; i < 512; i++) {
        if (!s_sim[i].size && size <= sizeof(s_sim[i].data)) {
            uint64_t ms = (uint64_t) s_sim_delay * 1000 + (s_sim_jitter ? sim_rand() % ((uint32_t) s_sim_jitter * 1000) : 0);
            s_sim[i].due = hapi_now_ns() + ms * 1000;
            s_sim[i].ip = ip;
            s_sim[i].port = port;
            s_sim[i].size = size;
            memcpy(s_sim[i].data, data, size);
            return true;
        }
    }
    return false;
}
#else
static void sim_flush(void) {}
static bool sim_send(uint32_t ip, uint32_t port, const void *data, uint32_t size) {
    (void) ip, (void) port, (void) data, (void) size;
    return false;
}
#endif

void hapi_net_send(const HostNetAddr *to, const void *data, uint32_t size) {
    to = GUEST_PTR(to);
    data = GUEST_PTR(data);
    if (s_sock < 0) return;
    sim_flush();
    if (online_enabled()) {
        online_send(s_sock, to, data, size);
        return;
    }
    if (to) {
        if (!sim_send(to->ip, to->port, data, size)) send_to(to->ip, (uint16_t) to->port, data, size);
        return;
    }
    // Broadcast on every interface (a phone sharing its connection as a hotspot has no default route into it), and
    // to this device (other instances, tests).
    struct ifaddrs *list = NULL;
    if (libc_net.getifaddrs && libc_net.getifaddrs(&list) == 0) {
        for (struct ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
                send_to(((struct sockaddr_in *) i->ifa_broadaddr)->sin_addr.s_addr, NET_BEACON_PORT, data, size);
            }
        }
        if (libc_net.freeifaddrs) libc_net.freeifaddrs(list);
    }
    send_to(htonl(INADDR_BROADCAST), NET_BEACON_PORT, data, size);
    send_to(htonl(INADDR_LOOPBACK), NET_BEACON_PORT, data, size);
}

int32_t hapi_net_recv(void *data, uint32_t capacity, HostNetAddr *from) {
    data = GUEST_PTR(data);
    from = GUEST_PTR(from);
    sim_flush();
    if (online_enabled() && s_sock >= 0) {
        // Online: everything comes in on the main socket, from the server or directly from a station it introduced.
        // The local network's beacons are read and dropped.
        online_tick(s_sock);
        struct sockaddr_in a;
        socklen_t len = sizeof(a);
        for (int i = 0; i < 16 && s_beacon_sock >= 0; i++) {
            if (libc_net.recvfrom(s_beacon_sock, data, capacity, 0, (struct sockaddr *) &a, &len) < 0) break;
            len = sizeof(a);
        }
        for (int i = 0; i < 16; i++) {
            len = sizeof(a);
            ssize_t n = libc_net.recvfrom(s_sock, data, capacity, 0, (struct sockaddr *) &a, &len);
            if (n < 0) return -1;
            HostNetAddr src = {a.sin_addr.s_addr, ntohs(a.sin_port)};
            int32_t size = online_receive(s_sock, data, (int32_t) n, &src);
            if (size < 0) continue;
            if (from) *from = src;
            return size;
        }
        return -1;
    }
    int socks[2] = {s_sock, s_beacon_sock};
    for (int i = 0; i < 2; i++) {
        if (socks[i] < 0) continue;
        struct sockaddr_in a;
        socklen_t len = sizeof(a);
        ssize_t n = libc_net.recvfrom(socks[i], data, capacity, 0, (struct sockaddr *) &a, &len);
        if (n >= 0) {
            if (from) {
                from->ip = a.sin_addr.s_addr;
                from->port = ntohs(a.sin_port);
            }
            return (int32_t) n;
        }
    }
    return -1;
}

// ------------------------------------------------------------------------------------------------ internet
#include <poll.h>
// Sockets to the internet for Nintendo WFC through Wiimmfi (platform/wfc.c, the router the DS connects to). Handles
// are the file descriptors; everything is non-blocking but the name lookup.
static struct {
    int (*connect)(int, const struct sockaddr *, socklen_t);
    ssize_t (*send)(int, const void *, size_t, int);
    ssize_t (*recv)(int, void *, size_t, int);
    int (*getsockopt)(int, int, int, void *, socklen_t *);
    int (*close)(int);
} libc_inet;

static bool libc_inet_load(void) {
    if (libc_inet.connect) return true;
    if (!libc_net_load()) return false;
#ifdef __ANDROID__
    void *libc = dlopen("libc.so", RTLD_NOW);
#else
    void *libc = dlopen("libc.so.6", RTLD_NOW);
#endif
    if (!libc) return false;
    libc_inet.connect = (int (*)(int, const struct sockaddr *, socklen_t)) dlsym(libc, "connect");
    libc_inet.send = (ssize_t (*)(int, const void *, size_t, int)) dlsym(libc, "send");
    libc_inet.recv = (ssize_t (*)(int, void *, size_t, int)) dlsym(libc, "recv");
    libc_inet.getsockopt = (int (*)(int, int, int, void *, socklen_t *)) dlsym(libc, "getsockopt");
    libc_inet.close = (int (*)(int)) dlsym(libc, "close");
    return libc_inet.connect && libc_inet.send && libc_inet.recv && libc_inet.getsockopt && libc_inet.close;
}

int32_t hapi_inet_open(int32_t tcp) {
    if (!libc_inet_load()) return -1;
    int fd = libc_net.socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (tcp) {
        int one = 1;
        libc_net.setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
    } else {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        libc_net.bind(fd, (struct sockaddr *) &a, sizeof(a)); // an ephemeral port
    }
    return fd;
}

int32_t hapi_inet_connect(int32_t h, uint32_t ip, uint32_t port) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons((uint16_t) port);
    if (libc_inet.connect(h, (struct sockaddr *) &a, sizeof(a)) == 0) return 0;
    return errno == EINPROGRESS || errno == EAGAIN ? 0 : -1;
}

int32_t hapi_inet_state(int32_t h) {
    struct pollfd p = {h, POLLOUT, 0};
    if (poll(&p, 1, 0) <= 0) return 0;
    int err = 0;
    socklen_t len = sizeof(err);
    if (libc_inet.getsockopt(h, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return -1;
    return 1;
}

int32_t hapi_inet_send(int32_t h, const void *data, uint32_t size, uint32_t ip, uint32_t port) {
    ssize_t n;
    if (ip) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ip;
        a.sin_port = htons((uint16_t) port);
        n = libc_net.sendto(h, GUEST_PTR(data), size, 0, (struct sockaddr *) &a, sizeof(a));
    } else {
        n = libc_inet.send(h, GUEST_PTR(data), size, MSG_NOSIGNAL);
    }
    if (n >= 0) return (int32_t) n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

int32_t hapi_inet_recv(int32_t h, void *data, uint32_t capacity, HostNetAddr *from) {
    struct sockaddr_in a = {0};
    socklen_t len = sizeof(a);
    ssize_t n = libc_net.recvfrom(h, GUEST_PTR(data), capacity, 0, (struct sockaddr *) &a, &len);
    if (n > 0) {
        if (from) {
            HostNetAddr *f = GUEST_PTR(from);
            f->ip = a.sin_addr.s_addr;
            f->port = ntohs(a.sin_port);
        }
        return (int32_t) n;
    }
    if (n == 0) return -2; // the other side closed (TCP)
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

void hapi_inet_close(int32_t h) {
    if (h >= 0 && libc_inet_load()) libc_inet.close(h);
}

// The address of a host name (network order), 0 when unknown. Blocking. The test build can name addresses itself:
// MKDS_HOSTS="name=a.b.c.d,name=..." (a local server standing for one on the internet).
uint32_t hapi_inet_resolve(const char *name_guest) {
    const char *name = GUEST_PTR(name_guest);
    const char *hosts = getenv("MKDS_HOSTS");
    if (!hosts && !strcmp(name, "mkds-hosts-test")) return 0; // (wfc.c: are local servers named? no)
    if (hosts) {
        size_t nl = strlen(name);
        for (const char *p = hosts; *p;) {
            const char *eq = strchr(p, '='), *end = strchr(p, ',');
            if (!end) end = p + strlen(p);
            if (eq && eq < end && (size_t) (eq - p) == nl && !strncmp(p, name, nl)) {
                char ip[32];
                snprintf(ip, sizeof(ip), "%.*s", (int) (end - eq - 1), eq + 1);
                return inet_addr(ip);
            }
            p = *end ? end + 1 : end;
        }
    }
    if (!libc_inet_load() || !libc_net.getaddrinfo) return 0;
    struct addrinfo hint = {0}, *res = NULL;
    hint.ai_family = AF_INET;
    if (libc_net.getaddrinfo(name, NULL, &hint, &res) != 0 || !res) {
        port_log("wfc: cannot resolve %s", name);
        return 0;
    }
    uint32_t ip = ((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr;
    libc_net.freeaddrinfo(res);
    return ip;
}

// The DS firmware's settings (the Nintendo WFC connections and user ID), kept next to the save file: "<save>.fw".
uint32_t hapi_fw_read(void *data, uint32_t size) {
    char path[600];
    snprintf(path, sizeof(path), "%s.fw", s_save_path[0] ? s_save_path : "mkds");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(GUEST_PTR(data), 1, size, f);
    fclose(f);
    return (uint32_t) n;
}

bool hapi_fw_write(const void *data, uint32_t size) {
    char path[600], tmp[620];
    snprintf(path, sizeof(path), "%s.fw", s_save_path[0] ? s_save_path : "mkds");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    fwrite(GUEST_PTR(data), 1, size, f);
    fclose(f);
    return rename(tmp, path) == 0;
}
