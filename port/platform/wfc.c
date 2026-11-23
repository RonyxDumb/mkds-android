// Nintendo WFC through Wiimmfi: the access point and the internet router the DS connects to.
//
// The game's network code runs unchanged: its Wi-Fi connection manager scans, connects to an access point and sends
// 802.11 data frames (LLC/SNAP and an IP packet) through the wireless library; its own TCP/IP stack (CPS) takes an
// address by DHCP, resolves names by DNS and opens TCP and UDP connections. Here, in place of the radio and of a home
// router:
//  - one access point is always in range (wm7.c answers the scans with it, connects and carries the frames);
//  - the router answers ARP, DHCP (the DS gets 10.77.0.2, the router is 10.77.0.1 and its DNS server) and DNS: names
//    under nintendowifi.net are looked up under wiimmfi.de, the server that took over Nintendo WFC; Nintendo's
//    connection test (conntest.nintendowifi.net) is answered by the router itself;
//  - UDP and TCP go to the internet through the phone's own sockets: each UDP port of the DS has a socket; each TCP
//    connection of the DS ends here (its segments are acknowledged here) and goes on as a connection of the phone.
// The game's Nintendo WFC settings get a connection to this access point (port_wfc_firmware), so that nothing has to
// be set up in the game's own settings menu.
#include "port.h"
#include "hostapi.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

#define WFC_LOG 1

static const u8 k_router_mac[6] = {0x02, 0x4d, 0x4b, 0x44, 0x53, 0x01};
#define ROUTER_IP 0x0a4d0001u // 10.77.0.1
#define DS_IP 0x0a4d0002u     // 10.77.0.2
static const char k_ssid[] = "MKDS-WIIMMFI";

const u8 *port_wfc_ap_mac(void) { return k_router_mac; }
const char *port_wfc_ssid(void) { return k_ssid; }

// ------------------------------------------------------------------------------------------------ helpers
static inline u16 rd16be(const u8 *p) { return (u16) (p[0] << 8 | p[1]); }
static inline u32 rd32be(const u8 *p) { return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]; }
static inline void wr16be(u8 *p, u32 v) { p[0] = (u8) (v >> 8), p[1] = (u8) v; }
static inline void wr32be(u8 *p, u32 v) { p[0] = (u8) (v >> 24), p[1] = (u8) (v >> 16), p[2] = (u8) (v >> 8), p[3] = (u8) v; }
// Host byte order <-> the network order the host API takes (an IPv4 address as stored in memory).
static inline u32 to_net(u32 ip) { u8 b[4]; wr32be(b, ip); u32 v; memcpy(&v, b, 4); return v; }
static inline u32 from_net(u32 v) { u8 b[4]; memcpy(b, &v, 4); return rd32be(b); }

static u32 sum16(const u8 *p, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2) sum += (u32) (p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (u32) p[n - 1] << 8;
    return sum;
}

static u16 fold(u32 sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16) ~sum;
}

static u64 now_ms(void) { return port_now_ns() / 1000000ull; }
static void trace_frame(const char *dir, const u8 *data, u32 len);

// ------------------------------------------------------------------------------------------------ frames to the DS
// Frames waiting for the wireless library (wm7.c takes them one by one): LLC/SNAP header and packet.
#define OUT_FRAMES 64
#define FRAME_MAX 1536
typedef struct Frame {
    u16 len;
    u8 data[FRAME_MAX];
} Frame;
static Frame *s_out;
static int s_out_head, s_out_count;
static u8 s_ds_mac[6];

static u8 *frame_begin(u16 ethertype) {
    if (!s_out) s_out = calloc(OUT_FRAMES, sizeof(Frame));
    if (!s_out || s_out_count == OUT_FRAMES) return NULL;
    Frame *f = &s_out[(s_out_head + s_out_count) % OUT_FRAMES];
    static const u8 llc[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
    memcpy(f->data, llc, 6);
    wr16be(f->data + 6, ethertype);
    return f->data + 8;
}

static void frame_end(u32 payload) {
    Frame *f = &s_out[(s_out_head + s_out_count) % OUT_FRAMES];
    f->len = (u16) (8 + payload);
    s_out_count++;
}

// The next frame for the DS: copied into `out` (FRAME_MAX), its length returned; 0 when none.
static u64 s_from_ds_ns; // the DS's last frame
static u32 s_frames_in, s_frames_out; // the log

int port_wfc_next_frame(u8 *out, u8 *src_mac, u8 *dst_mac) {
    if (!s_out_count) return 0;
    // Not in the same moment as the DS's own frame: on a network an answer takes at least a millisecond, and the
    // DS's stack counts on it (it updates a connection after its frame is sent; an answer before that is lost).
    if (port_now_ns() - s_from_ds_ns < 3000000ull) return 0;
    Frame *f = &s_out[s_out_head];
    s_out_head = (s_out_head + 1) % OUT_FRAMES;
    s_out_count--;
    memcpy(out, f->data, f->len);
    trace_frame("->", f->data, f->len);
    s_frames_out++;
    memcpy(src_mac, k_router_mac, 6);
    memcpy(dst_mac, s_ds_mac, 6);
    return f->len;
}

// An IPv4 packet to the DS: header here, `payload` bytes already written after it (at ip + 20).
static void send_ip(u8 *ip, u8 proto, u32 src, u32 dst, u32 payload) {
    ip[0] = 0x45;
    ip[1] = 0;
    wr16be(ip + 2, 20 + payload);
    static u16 s_id;
    wr16be(ip + 4, ++s_id);
    wr16be(ip + 6, 0x4000); // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    wr16be(ip + 10, 0);
    wr32be(ip + 12, src);
    wr32be(ip + 16, dst);
    wr16be(ip + 10, fold(sum16(ip, 20, 0)));
    frame_end(20 + payload);
}

static u16 l4_checksum(const u8 *seg, u32 len, u8 proto, u32 src, u32 dst) {
    u8 pseudo[12];
    wr32be(pseudo, src);
    wr32be(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    wr16be(pseudo + 10, len);
    return fold(sum16(seg, len, sum16(pseudo, 12, 0)));
}

static void send_udp(u32 src, u16 sport, u32 dst, u16 dport, const u8 *data, u32 len) {
    if (len > FRAME_MAX - 8 - 28) return;
    u8 *ip = frame_begin(0x0800);
    if (!ip) return;
    u8 *udp = ip + 20;
    wr16be(udp, sport);
    wr16be(udp + 2, dport);
    wr16be(udp + 4, 8 + len);
    wr16be(udp + 6, 0);
    memcpy(udp + 8, data, len);
    u16 c = l4_checksum(udp, 8 + len, 17, src, dst);
    wr16be(udp + 6, c ? c : 0xffff);
    send_ip(ip, 17, src, dst, 8 + len);
}

// ------------------------------------------------------------------------------------------------ ARP, DHCP
static void on_arp(const u8 *p, u32 len) {
    if (len < 28 || rd16be(p + 6) != 1) return; // requests only
    u8 *o = frame_begin(0x0806);
    if (!o) return;
    memcpy(o, p, 6);       // hardware and protocol types and sizes
    wr16be(o + 6, 2);      // reply
    memcpy(o + 8, k_router_mac, 6);
    memcpy(o + 14, p + 24, 4); // the asked address is ours (every address is behind the router)
    memcpy(o + 18, p + 8, 10); // to the asker
    frame_end(28);
}

static void on_dhcp(const u8 *b, u32 len) {
    if (len < 240 || b[0] != 1 || rd32be(b + 236) != 0x63825363) return;
    int type = 0;
    for (u32 i = 240; i + 1 < len && b[i] != 0xff;) {
        if (b[i] == 0) { i++; continue; }
        if (b[i] == 53 && b[i + 1] >= 1) type = b[i + 2];
        i += 2 + b[i + 1];
    }
    if (type != 1 && type != 3) return; // DISCOVER, REQUEST
    static u8 r[300];
    memset(r, 0, sizeof(r));
    r[0] = 2; // reply
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, b + 4, 4); // transaction
    wr32be(r + 16, DS_IP);
    wr32be(r + 20, ROUTER_IP);
    memcpy(r + 28, b + 28, 16); // client hardware address
    wr32be(r + 236, 0x63825363);
    u8 *o = r + 240;
    *o++ = 53, *o++ = 1, *o++ = type == 1 ? 2 : 5; // OFFER, ACK
    *o++ = 54, *o++ = 4, wr32be(o, ROUTER_IP), o += 4;
    *o++ = 51, *o++ = 4, wr32be(o, 86400), o += 4;
    *o++ = 1, *o++ = 4, wr32be(o, 0xffffff00), o += 4;
    *o++ = 3, *o++ = 4, wr32be(o, ROUTER_IP), o += 4;
    *o++ = 6, *o++ = 4, wr32be(o, ROUTER_IP), o += 4;
    *o++ = 0xff;
    memcpy(s_ds_mac, b + 28, 6);
    send_udp(ROUTER_IP, 67, 0xffffffffu, 68, r, (u32) (o - r));
    if (WFC_LOG) {
        port_log("wfc: DHCP %s (transaction %08x; %u frames from the DS, %u to it)", type == 1 ? "offer" : "ack",
                 rd32be(b + 4), s_frames_in, s_frames_out);
    }
}

// ------------------------------------------------------------------------------------------------ DNS
// The test build names local servers itself (MKDS_HOSTS, hostapi): then the names are looked up there only.
static bool getenv_hosts(void) {
    static int s = -1;
    if (s < 0) s = hapi_inet_resolve("mkds-hosts-test") != 0;
    return s;
}
// The name looked up for a name the DS asks: Nintendo WFC's servers are Wiimmfi's now.
static void wiimmfi_name(const char *name, char *out, u32 size) {
    const char *suffix = "nintendowifi.net";
    u32 n = (u32) strlen(name), s = (u32) strlen(suffix);
    if (n >= s && !strcmp(name + n - s, suffix)) {
        snprintf(out, size, "%.*swiimmfi.de", (int) (n - s), name);
    } else {
        snprintf(out, size, "%s", name);
    }
}

// The question's name of a DNS message, and the first address of its answer (0: none).
static void dns_describe(const u8 *m, u32 len, char *name, u32 size, u32 *addr) {
    u32 i = 12, n = 0;
    name[0] = 0;
    *addr = 0;
    while (i < len && m[i] && !(m[i] & 0xc0)) {
        u32 l = m[i++];
        if (i + l > len || n + l + 1 >= size) return;
        if (n) name[n++] = '.';
        memcpy(name + n, m + i, l);
        n += l;
        i += l;
    }
    name[n] = 0;
    i += 5; // end, type, class
    u32 answers = len >= 12 ? rd16be(m + 6) : 0;
    for (u32 a = 0; a < answers && i < len; a++) {
        if (m[i] & 0xc0) i += 2;
        else {
            while (i < len && m[i]) i += 1 + m[i];
            i++;
        }
        if (i + 10 > len) return;
        u16 type = rd16be(m + i), rdlen = rd16be(m + i + 8);
        i += 10;
        if (type == 1 && rdlen == 4 && i + 4 <= len) {
            *addr = rd32be(m + i);
            return;
        }
        i += rdlen;
    }
}

// Wiimmfi's DNS server: what a real DS is set to for Wiimmfi; it answers Nintendo's names with Wiimmfi's servers.
#define WIIMMFI_DNS 0xa7ebe524u // 167.235.229.36
#define DNS_PENDING 8
typedef struct DnsQuery {
    bool used;
    u16 ds_port, id;
    u64 sent_ms;
    u32 len;
    u8 q[512];
} DnsQuery;
static DnsQuery s_dns[DNS_PENDING];
static int32_t s_dns_sock = -1;

static void answer_dns(u16 sport, const u8 *q, u32 len, bool forwarded_failed);

static void on_dns(u16 sport, const u8 *q, u32 len) {
    if (len < 12 || len > 512) return;
    // Asked of Wiimmfi's DNS server; answered here when it does not answer (answer_dns).
    if (s_dns_sock < 0) s_dns_sock = hapi_inet_open(0);
    DnsQuery *d = NULL;
    for (int i = 0; i < DNS_PENDING; i++) {
        if (!s_dns[i].used || now_ms() - s_dns[i].sent_ms > 5000) d = &s_dns[i];
    }
    if (s_dns_sock >= 0 && d && !getenv_hosts()) {
        d->used = true;
        d->ds_port = sport;
        d->id = rd16be(q);
        d->sent_ms = now_ms();
        d->len = len;
        memcpy(d->q, q, len);
        hapi_inet_send(s_dns_sock, q, len, to_net(WIIMMFI_DNS), 53);
        return;
    }
    answer_dns(sport, q, len, true);
}

static void poll_dns(void) {
    if (s_dns_sock >= 0) {
        static u8 buf[600];
        for (int k = 0; k < 8; k++) {
            HostNetAddr from;
            int32_t n = hapi_inet_recv(s_dns_sock, buf, sizeof(buf), &from);
            if (n < 12) break;
            for (int i = 0; i < DNS_PENDING; i++) {
                DnsQuery *d = &s_dns[i];
                if (!d->used || d->id != rd16be(buf)) continue;
                d->used = false;
                // conntest is the router's own (answer_dns); everything else as Wiimmfi says.
                if (strstr((const char *) d->q + 12, "conntest")) answer_dns(d->ds_port, d->q, d->len, true);
                else send_udp(ROUTER_IP, 53, DS_IP, d->ds_port, buf, (u32) n);
                if (WFC_LOG) {
                    char name[256];
                    u32 a;
                    dns_describe(buf, (u32) n, name, sizeof(name), &a);
                    port_log("wfc: DNS %s = %d.%d.%d.%d (Wiimmfi's DNS)", name, a >> 24, (a >> 16) & 255,
                             (a >> 8) & 255, a & 255);
                }
                break;
            }
        }
    }
    for (int i = 0; i < DNS_PENDING; i++) {
        DnsQuery *d = &s_dns[i];
        if (d->used && now_ms() - d->sent_ms > 1500) { // no answer: the phone's own DNS
            d->used = false;
            answer_dns(d->ds_port, d->q, d->len, true);
        }
    }
}

static void answer_dns(u16 sport, const u8 *q, u32 len, bool forwarded_failed) {
    (void) forwarded_failed;
    if (len < 12 || len > 512) return;
    char name[256];
    u32 i = 12, n = 0;
    while (i < len && q[i]) {
        u32 l = q[i++];
        if (i + l > len || n + l + 1 >= sizeof(name)) return;
        if (n) name[n++] = '.';
        memcpy(name + n, q + i, l);
        n += l;
        i += l;
    }
    name[n] = 0;
    if (i + 5 > len) return;
    u32 qend = i + 5;
    u16 qtype = rd16be(q + i + 1);
    for (u32 k = 0; k < n; k++) {
        if (name[k] >= 'A' && name[k] <= 'Z') name[k] = (char) (name[k] + 32);
    }
    u32 addr = 0;
    if (qtype == 1) {
        if (!strcmp(name, "conntest.nintendowifi.net")) {
            addr = ROUTER_IP; // answered here (tcp: the router's port 80)
        } else {
            char lookup[300];
            wiimmfi_name(name, lookup, sizeof(lookup));
            addr = from_net(hapi_inet_resolve(lookup));
            if (WFC_LOG) port_log("wfc: DNS %s -> %s = %d.%d.%d.%d", name, lookup, addr >> 24, (addr >> 16) & 255,
                                  (addr >> 8) & 255, addr & 255);
        }
    }
    static u8 r[600];
    memcpy(r, q, qend);
    r[2] = 0x81;
    r[3] = addr ? 0x80 : 0x83; // recursion available; no error / name error
    wr16be(r + 6, addr ? 1 : 0);
    wr16be(r + 8, 0);
    wr16be(r + 10, 0);
    u32 o = qend;
    if (addr) {
        wr16be(r + o, 0xc00c); // the name of the question
        wr16be(r + o + 2, 1);
        wr16be(r + o + 4, 1);
        wr32be(r + o + 6, 300);
        wr16be(r + o + 10, 4);
        wr32be(r + o + 12, addr);
        o += 16;
    }
    send_udp(ROUTER_IP, 53, DS_IP, sport, r, o);
}

// ------------------------------------------------------------------------------------------------ UDP
#define UDP_PORTS 16
typedef struct UdpPort {
    bool used;
    u16 port; // the DS's
    int32_t sock;
    u64 used_ms;
    int logged, replies_logged;
} UdpPort;
static UdpPort s_udp[UDP_PORTS];

static void on_udp(const u8 *ip, const u8 *udp, u32 len) {
    if (len < 8) return;
    u16 sport = rd16be(udp), dport = rd16be(udp + 2);
    u32 dst = rd32be(ip + 16), ulen = rd16be(udp + 4);
    if (ulen < 8 || ulen > len) return;
    const u8 *data = udp + 8;
    u32 n = ulen - 8;
    if (dport == 67) {
        on_dhcp(data, n);
        return;
    }
    if (dst == ROUTER_IP && dport == 53) {
        on_dns(sport, data, n);
        return;
    }
    if (dst == ROUTER_IP || (dst >> 28) == 0xe || dst == 0xffffffffu) return; // the router, multicast, broadcast
    UdpPort *u = NULL, *free_slot = NULL, *oldest = &s_udp[0];
    for (int i = 0; i < UDP_PORTS; i++) {
        if (s_udp[i].used && s_udp[i].port == sport) u = &s_udp[i];
        if (!s_udp[i].used && !free_slot) free_slot = &s_udp[i];
        if (s_udp[i].used_ms < oldest->used_ms) oldest = &s_udp[i];
    }
    if (!u) {
        u = free_slot ? free_slot : oldest;
        if (u->used) hapi_inet_close(u->sock);
        u->sock = hapi_inet_open(0);
        u->used = u->sock >= 0;
        u->port = sport;
        u->logged = u->replies_logged = 0;
        if (!u->used) return;
    }
    u->used_ms = now_ms();
    if (WFC_LOG && u->logged < 4) {
        u->logged++;
        port_log("wfc: UDP %d -> %d.%d.%d.%d:%d, %d bytes", sport, dst >> 24, (dst >> 16) & 255, (dst >> 8) & 255,
                 dst & 255, dport, (int) n);
    }
    if (hapi_inet_send(u->sock, data, n, to_net(dst), dport) < 0 && WFC_LOG) port_log("wfc: UDP send failed");
}

static void poll_udp(void) {
    poll_dns();
    static u8 buf[1472];
    for (int i = 0; i < UDP_PORTS; i++) {
        UdpPort *u = &s_udp[i];
        if (!u->used) continue;
        for (int k = 0; k < 8 && s_out_count < OUT_FRAMES - 4; k++) {
            HostNetAddr from;
            int32_t n = hapi_inet_recv(u->sock, buf, sizeof(buf), &from);
            if (n <= 0) break;
            u->used_ms = now_ms();
            u32 fi = from_net(from.ip);
            if (WFC_LOG && u->replies_logged < 4) {
                u->replies_logged++;
                port_log("wfc: UDP %d <- %d.%d.%d.%d:%d, %d bytes", u->port, fi >> 24, (fi >> 16) & 255,
                         (fi >> 8) & 255, fi & 255, (int) from.port, (int) n);
            }
            send_udp(fi, (u16) from.port, DS_IP, u->port, buf, (u32) n);
        }
    }
}

// ------------------------------------------------------------------------------------------------ TCP
// Each connection of the DS ends here. Segments to the DS are sent while its window allows and sent again from the
// oldest unacknowledged byte when no acknowledgement comes (its receive ring can drop a frame); segments from the DS
// are taken in order only, so that a missing one is simply sent again by the DS.
#define TCP_CONNS 16
#define TCP_OUT 32768 // from the internet, waiting for the DS's acknowledgement
#define TCP_IN 8192   // from the DS, waiting to be sent on
enum { T_FREE, T_CONNECTING, T_OPEN, T_CLOSING };
typedef struct Tcp {
    u8 state;
    bool local;        // the router itself (the connection test)
    bool remote_eof;   // the internet side has closed: a FIN follows the data
    bool fin_sent, ds_fin;
    u16 ds_port, port;
    u32 ip;
    int32_t sock;
    u32 rcv_nxt;       // next byte expected from the DS
    u32 snd_una, snd_nxt, iss;
    u16 ds_wnd, mss;
    u64 sent_ms, last_ms;
    u8 out[TCP_OUT];   // bytes snd_una.. (out_len of them)
    u32 out_len;
    u8 in[TCP_IN];
    u32 in_len;
    u32 bytes_up, bytes_down; // the log
} Tcp;
static Tcp *s_tcp;

static void tcp_segment(Tcp *t, u8 flags, u32 seq, const u8 *data, u32 len) {
    u8 *ip = frame_begin(0x0800);
    if (!ip) return;
    u8 *th = ip + 20;
    u32 hl = flags & 0x02 ? 24 : 20; // the SYN carries the MSS
    wr16be(th, t->port);
    wr16be(th + 2, t->ds_port);
    wr32be(th + 4, seq);
    wr32be(th + 8, t->rcv_nxt);
    th[12] = (u8) ((hl / 4) << 4);
    th[13] = flags;
    wr16be(th + 14, TCP_IN - t->in_len);
    wr16be(th + 16, 0);
    wr16be(th + 18, 0);
    if (hl == 24) {
        th[20] = 2, th[21] = 4;
        wr16be(th + 22, 1400);
    }
    if (len) memcpy(th + hl, data, len);
    wr16be(th + 16, l4_checksum(th, hl + len, 6, t->ip, DS_IP));
    send_ip(ip, 6, t->ip, DS_IP, hl + len);
}

static void tcp_reset(u32 ip, u16 port, u16 ds_port, u32 seq, u32 ack) {
    Tcp t = {0};
    t.ip = ip, t.port = port, t.ds_port = ds_port, t.rcv_nxt = ack;
    tcp_segment(&t, 0x14, seq, NULL, 0); // RST ACK
}

static void tcp_free(Tcp *t) {
    if (WFC_LOG && !t->local) {
        port_log("wfc: TCP to %d.%d.%d.%d:%d closed (sent %u, received %u bytes; %s%s)", t->ip >> 24,
                 (t->ip >> 16) & 255, (t->ip >> 8) & 255, t->ip & 255, t->port, t->bytes_up, t->bytes_down,
                 t->remote_eof ? "server closed" : "server open", t->ds_fin ? ", DS closed" : "");
    }
    if (t->sock >= 0 && !t->local) hapi_inet_close(t->sock);
    t->state = T_FREE;
}

// The connection test: Nintendo's server answered with this header; the router answers the same.
static void local_request(Tcp *t) {
    bool line = false;
    for (u32 i = 0; i < t->in_len; i++) line |= t->in[i] == '\n';
    if (t->out_len || !line) return;
    static const char reply[] = "HTTP/1.0 200 OK\r\nX-Organization: Nintendo\r\nContent-Type: text/html\r\n"
                                "Content-Length: 2\r\nConnection: close\r\n\r\nok";
    memcpy(t->out, reply, sizeof(reply) - 1);
    t->out_len = sizeof(reply) - 1;
    t->in_len = 0;
    t->remote_eof = true;
}

static void on_tcp(const u8 *ip, const u8 *th, u32 len) {
    if (len < 20) return;
    u32 src = rd32be(ip + 12), dst = rd32be(ip + 16);
    (void) src;
    u16 sport = rd16be(th), dport = rd16be(th + 2);
    u32 seq = rd32be(th + 4), ack = rd32be(th + 8);
    u32 hl = (u32) (th[12] >> 4) * 4;
    u8 flags = th[13];
    if (hl < 20 || hl > len) return;
    const u8 *data = th + hl;
    u32 n = len - hl;
    if (!s_tcp) s_tcp = calloc(TCP_CONNS, sizeof(Tcp));
    if (!s_tcp) return;
    Tcp *t = NULL;
    for (int i = 0; i < TCP_CONNS; i++) {
        Tcp *c = &s_tcp[i];
        if (c->state != T_FREE && c->ds_port == sport && c->ip == dst && c->port == dport) t = c;
    }
    if (flags & 0x04) { // RST
        if (t) {
            if (WFC_LOG) port_log("wfc: the DS reset a connection");
            tcp_free(t);
        }
        return;
    }
    if (flags & 0x02) { // SYN
        if (t) {
            if (t->state == T_OPEN && seq + 1 == t->rcv_nxt) tcp_segment(t, 0x12, t->iss, NULL, 0); // again
            return;
        }
        for (int i = 0; i < TCP_CONNS && !t; i++) {
            if (s_tcp[i].state == T_FREE) t = &s_tcp[i];
        }
        if (!t) { // all in use: the one idle the longest makes room
            for (int i = 0; i < TCP_CONNS; i++) {
                if (!t || s_tcp[i].last_ms < t->last_ms) t = &s_tcp[i];
            }
            if (WFC_LOG) port_log("wfc: too many TCP connections, the oldest is closed");
            tcp_reset(t->ip, t->port, t->ds_port, t->snd_nxt, t->rcv_nxt);
            tcp_free(t);
        }
        if (!t) {
            tcp_reset(dst, dport, sport, 0, seq + 1);
            return;
        }
        memset(t, 0, offsetof(Tcp, out));
        t->out_len = t->in_len = 0;
        t->bytes_up = t->bytes_down = 0;
        t->ds_port = sport, t->ip = dst, t->port = dport;
        t->rcv_nxt = seq + 1;
        t->iss = ((u32) rand() ^ (u32) port_now_ns()) * 2654435761u; // differs between runs
        t->snd_una = t->snd_nxt = t->iss + 1;
        t->ds_wnd = rd16be(th + 14);
        t->mss = 536;
        for (u32 i = 20; i + 1 < hl;) { // the DS's MSS
            if (th[i] == 0) break;
            if (th[i] == 1) { i++; continue; }
            if (th[i] == 2 && th[i + 1] == 4 && i + 4 <= hl) t->mss = rd16be(th + i + 2);
            i += th[i + 1] ? th[i + 1] : 1;
        }
        if (t->mss > 1400) t->mss = 1400;
        t->last_ms = now_ms();
        t->local = dst == ROUTER_IP;
        if (t->local) {
            if (dport != 80) {
                tcp_reset(dst, dport, sport, 0, seq + 1);
                t->state = T_FREE;
                return;
            }
            t->sock = -1;
            t->state = T_OPEN;
            tcp_segment(t, 0x12, t->iss, NULL, 0); // SYN ACK
            return;
        }
        t->sock = hapi_inet_open(1);
        if (t->sock < 0 || hapi_inet_connect(t->sock, to_net(dst), dport) != 0) {
            if (t->sock >= 0) hapi_inet_close(t->sock);
            tcp_reset(dst, dport, sport, 0, seq + 1);
            return;
        }
        t->state = T_CONNECTING;
        if (WFC_LOG) port_log("wfc: TCP to %d.%d.%d.%d:%d", dst >> 24, (dst >> 16) & 255, (dst >> 8) & 255, dst & 255,
                              dport);
        return;
    }
    if (!t) {
        if (!(flags & 0x01)) tcp_reset(dst, dport, sport, ack, seq + n); // not a connection we know
        return;
    }
    if (t->state == T_CONNECTING) return;
    t->last_ms = now_ms();
    t->ds_wnd = rd16be(th + 14);
    if (flags & 0x10) { // ACK: what the DS has
        u32 acked = ack - t->snd_una;
        u32 in_flight = t->snd_nxt - t->snd_una + (t->fin_sent ? 1 : 0);
        if (acked && acked <= in_flight) {
            u32 bytes = acked > t->out_len ? t->out_len : acked;
            memmove(t->out, t->out + bytes, t->out_len - bytes);
            t->out_len -= bytes;
            t->snd_una = ack;
            if ((int32_t) (t->snd_nxt - t->snd_una) < 0) t->snd_nxt = t->snd_una;
            t->sent_ms = now_ms();
        }
    }
    bool ack_now = false;
    if (n) {
        if (seq == t->rcv_nxt && t->in_len + n <= TCP_IN) {
            memcpy(t->in + t->in_len, data, n);
            t->in_len += n;
            t->rcv_nxt += n;
        }
        ack_now = true; // also for a repeat, or what did not fit (the DS sends it again)
    }
    if ((flags & 0x01) && seq + n == t->rcv_nxt) { // FIN
        t->rcv_nxt++;
        t->ds_fin = true;
        ack_now = true;
    }
    if (ack_now) tcp_segment(t, 0x10, t->snd_nxt, NULL, 0);
    if (t->local) local_request(t);
    if (t->fin_sent && t->ds_fin && t->snd_una == t->snd_nxt + 0) tcp_free(t);
}

static void poll_tcp(void) {
    if (!s_tcp) return;
    u64 now = now_ms();
    for (int i = 0; i < TCP_CONNS; i++) {
        Tcp *t = &s_tcp[i];
        if (t->state == T_FREE) continue;
        if (t->state == T_CONNECTING) {
            int32_t st = hapi_inet_state(t->sock);
            if (st > 0) {
                t->state = T_OPEN;
                tcp_segment(t, 0x12, t->iss, NULL, 0); // SYN ACK
                t->sent_ms = now;
            } else if (st < 0 || now - t->last_ms > 15000) {
                tcp_reset(t->ip, t->port, t->ds_port, 0, t->rcv_nxt);
                tcp_free(t);
                if (WFC_LOG) port_log("wfc: TCP connection refused");
            }
            continue;
        }
        // On to the internet.
        if (t->in_len && !t->local) {
            int32_t k = hapi_inet_send(t->sock, t->in, t->in_len, 0, 0);
            if (k > 0) {
                t->bytes_up += (u32) k;
                memmove(t->in, t->in + k, t->in_len - (u32) k);
                t->in_len -= (u32) k;
                tcp_segment(t, 0x10, t->snd_nxt, NULL, 0); // the window opened again
            } else if (k < 0) {
                t->remote_eof = true;
            }
        }
        // The DS closed its side (and all it sent went on): the connection ends here too, also when the server
        // would keep it open (GameSpy's servers do); what the DS has not taken yet is dropped.
        if (t->ds_fin && t->in_len == 0 && !t->remote_eof) {
            t->remote_eof = true;
            u32 sent = t->snd_nxt - t->snd_una;
            if (t->out_len > sent) t->out_len = sent;
        }
        // From the internet.
        while (!t->local && !t->remote_eof && t->out_len < TCP_OUT) {
            int32_t k = hapi_inet_recv(t->sock, t->out + t->out_len, TCP_OUT - t->out_len, NULL);
            if (k > 0) {
                t->out_len += (u32) k;
                t->bytes_down += (u32) k;
            }
            else {
                if (k < 0) t->remote_eof = true;
                break;
            }
        }
        // To the DS: what its window takes; again from the oldest when nothing was acknowledged for a while.
        if (t->snd_nxt != t->snd_una && now - t->sent_ms > 400) {
            t->snd_nxt = t->snd_una;
            t->fin_sent = false;
        }
        u32 window = t->ds_wnd ? t->ds_wnd : 1;
        while (s_out_count < OUT_FRAMES - 4) {
            u32 offset = t->snd_nxt - t->snd_una;
            if (offset >= t->out_len || offset >= window) break;
            u32 n = t->out_len - offset;
            if (n > t->mss) n = t->mss;
            if (n > window - offset) n = window - offset;
            tcp_segment(t, n == t->out_len - offset ? 0x18 : 0x10, t->snd_nxt, t->out + offset, n);
            t->snd_nxt += n;
            t->sent_ms = now;
        }
        if (t->remote_eof && !t->fin_sent && t->snd_nxt - t->snd_una == t->out_len && t->in_len == 0) {
            tcp_segment(t, 0x11, t->snd_nxt, NULL, 0); // FIN ACK
            t->fin_sent = true;
            t->sent_ms = now;
        }
        if ((t->fin_sent && t->ds_fin && t->snd_una == t->snd_nxt + 1) || now - t->last_ms > 120000) tcp_free(t);
    }
}

// ------------------------------------------------------------------------------------------------ from the DS
// A frame the DS sends (WM_SetDCFData): LLC/SNAP header and packet.
extern int g_wm7_trace;
static void trace_frame(const char *dir, const u8 *data, u32 len) {
    // Always the first frames after connecting (the log shows where a connection stops), every frame in tests.
    if (len < 8 || (!g_wm7_trace && s_frames_in + s_frames_out > 160)) return;
    u16 type = rd16be(data + 6);
    const u8 *p = data + 8;
    if (type == 0x0800 && len >= 28) {
        u32 ihl = (u32) (p[0] & 15) * 4;
        const u8 *l4 = p + ihl;
        u32 s = rd32be(p + 12), d = rd32be(p + 16);
        if (p[9] == 6) {
            port_log("wfc %s TCP %x:%d > %x:%d flags %02x seq %u ack %u len %d", dir, s, rd16be(l4), d, rd16be(l4 + 2),
                     l4[13], rd32be(l4 + 4), rd32be(l4 + 8), (int) (rd16be(p + 2) - ihl - (l4[12] >> 4) * 4));
        } else {
            port_log("wfc %s IP proto %d %x:%d > %x:%d len %d", dir, p[9], s, rd16be(l4), d, rd16be(l4 + 2),
                     rd16be(p + 2));
        }
    } else {
        port_log("wfc %s type %04x len %d", dir, type, (int) len);
    }
}

void port_wfc_frame_from_ds(const u8 *data, u32 len) {
    trace_frame("<-", data, len);
    s_frames_in++;
    s_from_ds_ns = port_now_ns();
    if (len < 8 || data[0] != 0xaa || data[1] != 0xaa) return;
    u16 type = rd16be(data + 6);
    const u8 *p = data + 8;
    len -= 8;
    if (type == 0x0806) {
        on_arp(p, len);
        return;
    }
    if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4) return;
    u32 ihl = (u32) (p[0] & 15) * 4, total = rd16be(p + 2);
    if (ihl < 20 || total > len || total < ihl) return;
    if (rd16be(p + 6) & 0x3fff) return; // fragments: the DS sends none
    if (p[9] == 17) on_udp(p, p + ihl, total - ihl);
    else if (p[9] == 6) on_tcp(p, p + ihl, total - ihl);
}

// The DS connected to the access point.
void port_wfc_connected(const u8 *ds_mac) {
    memcpy(s_ds_mac, ds_mac, 6);
    s_frames_in = s_frames_out = 0;
    s_out_count = 0;
    port_log("wfc: connected to the access point");
}

// The DS left the access point: every connection ends.
void port_wfc_disconnected(void) {
    port_log("wfc: left the access point (%u frames from the DS, %u to it)", s_frames_in, s_frames_out);
    for (int i = 0; i < UDP_PORTS; i++) {
        if (s_udp[i].used) hapi_inet_close(s_udp[i].sock);
        s_udp[i].used = false;
    }
    for (int i = 0; s_tcp && i < TCP_CONNS; i++) {
        if (s_tcp[i].state != T_FREE) tcp_free(&s_tcp[i]);
    }
    s_out_count = 0;
}

// From the OS's interrupt points (wm7.c), while connected.
void port_wfc_poll(void) {
    poll_udp();
    poll_tcp();
}

// ------------------------------------------------------------------------------------------------ settings
// The game's Nintendo WFC settings (firmware flash, three connections of 0x100 bytes at 0x3fa00, a CRC-16 each):
// once the game has set them up (they are valid), an empty first connection becomes one to this access point.
static u16 crc16(const u8 *p, u32 n) {
    u16 crc = 0;
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (u16) (crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1);
    }
    return crc;
}

bool port_wfc_firmware(u8 *firmware) {
    u8 *page = firmware + 0x3fa00;
    if (crc16(page, 0xfe) != (u16) (page[0xfe] | page[0xff] << 8)) return false; // not set up yet
    if (page[0xe7] != 0xff) return false; // in use
    for (int i = 0; i < 3; i++) {
        u8 *p = firmware + 0x3fa00 + i * 0x100;
        if (p[0xe7] != 0xff && !memcmp(p + 0x40, k_ssid, sizeof(k_ssid) - 1)) return false; // there already
    }
    memset(page, 0, 0xe8); // ISP login, SSIDs, WEP keys, addresses (none: DHCP), types
    memcpy(page + 0x40, k_ssid, sizeof(k_ssid) - 1);
    page[0xe6] = 0; // no WEP
    page[0xe7] = 0; // a connection set up by hand
    u16 crc = crc16(page, 0xfe);
    page[0xfe] = (u8) crc;
    page[0xff] = (u8) (crc >> 8);
    port_log("wfc: Nintendo WFC connection 1 set to the port's access point");
    return true;
}
