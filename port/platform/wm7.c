// The ARM7 side of the DS wireless, over UDP on the local network.
//
// The game's wireless library (libs/NitroSDK/wm) runs as on the DS: it sends commands through the PXI FIFO (tag 0xa,
// a pointer to a command buffer) and receives messages from the ARM7 in its FIFO callback. This file plays the ARM7:
// it keeps the status area the library reads, answers the commands and sends the messages the library expects
// (command results, a child connecting or leaving, port data arriving, a port send done).
//
// Instead of radio frames, stations exchange UDP datagrams:
//  - a parent broadcasts a beacon (its game info) ten times a second; scanning children list the parents they hear;
//  - a child joins a parent with its 24 bytes of user data; the parent gives it an AID;
//  - port data goes over one ordered, reliable stream per link (sequence numbers, acknowledgements, resending), which
//    is what the MP protocol guarantees on the DS; a send is done when every destination acknowledged it;
//  - acknowledgements double as keep-alives; a station not heard for a few seconds is disconnected.
// Messages are delivered from the interrupt points of the OS (port_wm7_poll), like the ARM7's FIFO interrupts.
#include "port.h"
#include "hostapi.h"

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "arm_fifo_messaging.h"
#include "wireless_link_control.h"
#include "wireless_key_data_sharing.h"
#include "wireless_system_commands.h"

// ------------------------------------------------------------------------------------------------ wire format
#define NET_MAGIC 0x57444b4du // "MKDW"
#define NET_VERSION 1

enum {
    PKT_BEACON = 1, // parent -> everyone: BeaconBody
    PKT_JOIN,       // child -> parent: JoinBody
    PKT_JOIN_OK,    // parent -> child: aid in the header
    PKT_JOIN_NO,    // parent -> child: result code in the header's aid
    PKT_LEAVE,      // either way
    PKT_DATA,       // reliable stream: DataBody
    PKT_ACK,        // AckBody; also the keep-alive
};

typedef struct NetHeader {
    u32 magic;
    u8 version, type;
    u16 aid;     // the sender's AID on the link (0 = parent), or a value of the packet type
    u32 session; // the parent's session (a new one each time a parent starts)
    u8 mac[6];   // the sender
    u16 length;  // bytes after the header
} NetHeader;

typedef struct BeaconBody {
    u16 channel, beacon_period;
    u16 game_info_length, reserved;
    u8 game_info[0x80]; // WMGameInfo
} BeaconBody;

typedef struct JoinBody {
    u8 user[24];
} JoinBody;

enum { DATA_PORT, DATA_MP_READY, DATA_MP_STOP };

typedef struct DataBody {
    u32 seq;
    u8 kind, port;
    u16 mp_seq; // the MP sequence number the receiver sees (WMPortRecvCallback.seqNo)
    u16 size, reserved;
    u8 data[0x200];
} DataBody;

typedef struct AckBody {
    u32 next; // every packet before this one arrived
} AckBody;

#define DATA_HEADER_SIZE (sizeof(DataBody) - 0x200)

// ------------------------------------------------------------------------------------------------ state
#define MAX_LINKS 16      // by AID: 0 = the parent (on a child), 1..15 = children (on a parent)
#define OUT_SLOTS 64      // unacknowledged packets per link
#define MAX_PARENTS 8     // parents heard while scanning
#define MAX_SENDS 64      // port sends waiting for their acknowledgements
#define MAX_HELD 64       // received port data waiting for its port callback
#define RESEND_NS 30000000ull
#define KEEPALIVE_NS 100000000ull
#define LINK_TIMEOUT_NS 5000000000ull
#define BEACON_NS 100000000ull
#define PARENT_TIMEOUT_NS 2500000000ull
#define JOIN_RETRY_NS 200000000ull
#define JOIN_TIMEOUT_NS 3000000000ull
#define HELD_TIMEOUT_NS 3000000000ull

typedef struct OutPacket {
    u16 size;
    u8 bytes[sizeof(NetHeader) + sizeof(DataBody)];
} OutPacket;

typedef struct Link {
    bool used;
    HostNetAddr addr;
    u8 mac[6];
    u16 aid;
    u64 heard_ns, sent_ns, resend_ns;
    u32 send_next, send_acked; // packets [send_acked, send_next) are in flight
    OutPacket *out;            // OUT_SLOTS
    u32 recv_next;
    bool mp_ready; // a child that started MP
} Link;

typedef struct Parent { // a parent heard while scanning
    bool used;
    HostNetAddr addr;
    u8 mac[6];
    u32 session;
    u64 heard_ns;
    BeaconBody beacon;
} Parent;

typedef struct Send { // a port send waiting for acknowledgements
    bool used;
    u16 remaining; // destinations that have not acknowledged it
    u32 link_seq[MAX_LINKS];
    u16 dest, sent, port, mp_seq, size;
    u32 data, callback, arg;
} Send;

typedef struct Held { // port data that arrived before its port callback was set
    u64 time_ns;
    u16 port, aid, mp_seq, size;
    u8 mac[6];
    u8 data[0x200];
} Held;

static bool s_net;
static u8 s_mac[6];
static WMStatus *s_status;
static bool s_parent, s_child;
static u32 s_session;
static ParentParameter s_pparam;
static u8 s_user_game_info[0x80];
static u8 s_attr;
static u16 s_mp_seq;
static u64 s_beacon_ns;
static Link s_links[MAX_LINKS];
static Parent s_parents[MAX_PARENTS];
static int s_next_parent;
static Send s_sends[MAX_SENDS];
static Held *s_held; // MAX_HELD, a ring
static int s_held_head, s_held_count;
static u64 s_last_poll_ns;

// Scan in progress.
static bool s_scanning;
static u64 s_scan_due_ns;
static u32 s_scan_buf;

// Join in progress.
static bool s_joining;
static int s_join_parent;
static u64 s_join_start_ns, s_join_sent_ns;
static JoinBody s_join;
static u16 s_join_power_save;

// Messages to the ARM9: a ring of buffers and a queue of the ones to deliver.
#define MSG_SLOTS 64
static u8 s_msg[MSG_SLOTS][0x100] __attribute__((aligned(32)));
static int s_msg_next;
static WirelessMessage *s_queue[MSG_SLOTS];
static int s_queue_head, s_queue_count;
static u8 s_payload[0x200] __attribute__((aligned(32)));

#define U16(m, off) (*(u16 *) ((u8 *) (m) + (off)))
#define U32(m, off) (*(u32 *) ((u8 *) (m) + (off)))
#define PTR(a) ((void *) (uintptr_t) (a))
#define ADDR(p) ((u32) (uintptr_t) (p))

static u64 now_ns(void) { return port_now_ns(); }
// Time from `then` to `now`; 0 when `then` is later (set during this poll).
static u64 since(u64 now, u64 then) { return now > then ? now - then : 0; }

static WirelessMessage *new_message(u16 command, u16 result) {
    u8 *m = s_msg[s_msg_next];
    s_msg_next = (s_msg_next + 1) % MSG_SLOTS;
    memset(m, 0, 0x100);
    U16(m, 0) = command;
    U16(m, 2) = result;
    return (WirelessMessage *) m;
}

#define WM7_TRACE 0
static void queue(WirelessMessage *m) {
    if (s_queue_count == MSG_SLOTS) {
        port_log("wm7: message queue full");
        return;
    }
    s_queue[(s_queue_head + s_queue_count++) % MSG_SLOTS] = m;
    if (WM7_TRACE && U16(m, 0) < 0x80) port_log("wm7: queued %02x (%d) at %d ms", U16(m, 0), s_queue_count, (int) (port_now_ns() / 1000000 % 100000));
}

static void reply(u16 command, u16 result) { queue(new_message(command, result)); }

static void deliver(WirelessMessage *m) {
    if (WM7_TRACE && U16(m, 0) != WIRELESS_REQUEST_PORT_DATA && U16(m, 0) != WIRELESS_REQUEST_PORT_SENT) {
        port_log("wm7: -> cmd %02x result %d state4 %d state8 %d at %d ms", U16(m, 0), U16(m, 2), U16(m, 4), U16(m, 8), (int) (port_now_ns() / 1000000 % 100000));
    }
    void (*fifo)(u32, u32, s32) = (void (*)(u32, u32, s32)) FifoRecvCallbackTable[0xa];
    if (fifo) fifo(0xa, ADDR(m), 0);
}

static void set_state(u16 state) {
    if (s_status) s_status->mState = state;
}

// ------------------------------------------------------------------------------------------------ sending
static void send_packet(const HostNetAddr *to, u8 type, u16 aid, const void *body, u16 size) {
    u8 buf[sizeof(NetHeader) + sizeof(DataBody)];
    NetHeader *h = (NetHeader *) buf;
    h->magic = NET_MAGIC;
    h->version = NET_VERSION;
    h->type = type;
    h->aid = aid;
    h->session = s_session;
    memcpy(h->mac, s_mac, 6);
    h->length = size;
    if (size) memcpy(buf + sizeof(NetHeader), body, size);
    hapi_net_send(to, buf, (u32) (sizeof(NetHeader) + size));
}

static u16 own_aid(void) { return s_status ? s_status->mOwnMemberIndex : 0; }

static void link_transmit(Link *l, u32 seq, u64 now) {
    OutPacket *p = &l->out[seq % OUT_SLOTS];
    hapi_net_send(&l->addr, p->bytes, p->size);
    l->sent_ns = now;
}

// Queues a reliable packet on the link; false when too many are in flight.
static bool link_push(Link *l, u8 kind, u16 port, u16 mp_seq, const void *data, u16 size, u32 *seq_out) {
    if (l->send_next - l->send_acked >= OUT_SLOTS) return false;
    u32 seq = l->send_next++;
    OutPacket *p = &l->out[seq % OUT_SLOTS];
    NetHeader *h = (NetHeader *) p->bytes;
    DataBody *d = (DataBody *) (p->bytes + sizeof(NetHeader));
    h->magic = NET_MAGIC;
    h->version = NET_VERSION;
    h->type = PKT_DATA;
    h->aid = own_aid();
    h->session = s_session;
    memcpy(h->mac, s_mac, 6);
    h->length = (u16) (DATA_HEADER_SIZE + size);
    d->seq = seq;
    d->kind = kind;
    d->port = (u8) port;
    d->mp_seq = mp_seq;
    d->size = size;
    d->reserved = 0;
    if (size) memcpy(d->data, data, size);
    p->size = (u16) (sizeof(NetHeader) + h->length);
    u64 now = now_ns();
    if (l->send_next - l->send_acked == 1) l->resend_ns = now + RESEND_NS;
    link_transmit(l, seq, now);
    if (seq_out) *seq_out = seq;
    return true;
}

static void send_ack(Link *l) {
    AckBody a = {l->recv_next};
    send_packet(&l->addr, PKT_ACK, own_aid(), &a, sizeof(a));
    l->sent_ns = now_ns();
}

// ------------------------------------------------------------------------------------------------ port sends
static void send_done(Send *s, u16 result) {
    WirelessMessage *m = new_message(WIRELESS_REQUEST_PORT_SENT, result);
    U16(m, 4) = 0;
    U16(m, 8) = 20; // WM_STATECODE_PORT_SEND
    U16(m, 0x0a) = s->port;
    U16(m, 0x0c) = s->dest;
    U16(m, 0x0e) = s->remaining;
    U16(m, 0x10) = s->sent;
    U32(m, 0x14) = s->data;
    U16(m, 0x18) = s->size;
    U16(m, 0x1a) = s->mp_seq;
    U32(m, 0x1c) = s->callback; // WmReceiveFifo calls it
    U32(m, 0x20) = s->arg;
    if (s_status) {
        U16(m, 0x24) = s_status->mMpMaxSendSize;
        U16(m, 0x26) = s_status->mMpMaxRecvSize;
    }
    s->used = false;
    queue(m);
}

static void check_sends(void) {
    for (int i = 0; i < MAX_SENDS; i++) {
        Send *s = &s_sends[i];
        if (!s->used) continue;
        for (int a = 0; a < MAX_LINKS; a++) {
            if (!(s->remaining & (1 << a))) continue;
            Link *l = &s_links[a];
            if (!l->used) {
                s->remaining &= (u16) ~(1 << a);
            } else if (l->send_acked > s->link_seq[a]) {
                s->remaining &= (u16) ~(1 << a);
                s->sent |= (u16) (1 << a);
            }
        }
        if (!s->remaining) send_done(s, WIRELESS_RESULT_SUCCEEDED);
    }
}

static void drop_sends(void) {
    for (int i = 0; i < MAX_SENDS; i++) s_sends[i].used = false;
}

// ------------------------------------------------------------------------------------------------ links
static Link *link_open(u16 aid, const HostNetAddr *addr, const u8 *mac) {
    Link *l = &s_links[aid];
    OutPacket *out = l->out;
    memset(l, 0, sizeof(*l));
    l->out = out ? out : malloc(sizeof(OutPacket) * OUT_SLOTS);
    l->used = true;
    l->aid = aid;
    l->addr = *addr;
    memcpy(l->mac, mac, 6);
    l->heard_ns = l->sent_ns = now_ns();
    return l;
}

static void update_children(void) {
    if (!s_status || !s_parent) return;
    u16 connected = 0, ready = 0;
    for (int a = 1; a < MAX_LINKS; a++) {
        if (!s_links[a].used) continue;
        connected |= (u16) (1 << a);
        if (s_links[a].mp_ready) ready |= (u16) (1 << a);
    }
    s_status->mConnectedChildren = connected;
    s_status->mMpReadyBitmap = ready;
    s_status->mLinkLevel = connected ? 3 : 0;
}

static void child_reset_status(void) {
    if (!s_status) return;
    s_status->mOwnMemberIndex = 0;
    s_status->mMpFlag = 0;
    s_status->mLinkLevel = 0;
    s_status->mConnectedChildren = 0;
    s_status->mMpReadyBitmap = 0;
}

// The link to `aid` ended (left, timed out, or this station disconnected it): tell the ARM9 like the DS does.
static void link_lost(u16 aid, bool notify, u16 state) {
    Link *l = &s_links[aid];
    if (!l->used) return;
    l->used = false;
    check_sends();
    if (s_parent) {
        update_children();
        if (notify) {
            WirelessMessage *m = new_message(WIRELESS_REQUEST_OPEN_HOST, 0);
            U16(m, 8) = state;
            memcpy((u8 *) m + 0x0a, l->mac, 6);
            U16(m, 0x10) = aid;
            queue(m);
        }
        port_log("wm7: child %d left", aid);
    } else if (s_child) {
        s_child = false;
        drop_sends();
        child_reset_status();
        set_state(WIRELESS_UNIT_STATE_POWERED_ON);
        if (notify) {
            WirelessMessage *m = new_message(WIRELESS_REQUEST_ENTER_HOST, 0);
            U16(m, 8) = state;
            memcpy((u8 *) m + 0x0a, l->mac, 6);
            queue(m);
        }
        port_log("wm7: left the parent");
    }
}

static void leave_all(bool notify) {
    for (int a = 0; a < MAX_LINKS; a++) {
        if (!s_links[a].used) continue;
        for (int i = 0; i < 3; i++) send_packet(&s_links[a].addr, PKT_LEAVE, own_aid(), NULL, 0);
        link_lost((u16) a, notify, WIRELESS_STATE_UNLINKED);
    }
}

// ------------------------------------------------------------------------------------------------ the beacon
static u16 game_info(u8 *out) { // WMGameInfo
    WMGameInfo *g = (WMGameInfo *) out;
    memset(out, 0, 0x80);
    g->mMagicNumber = 1;
    g->mVer = 1;
    g->mPlatform = 0;
    g->mGgid = s_pparam.mGgid;
    g->mTgid = s_pparam.mTgid;
    u16 len = s_pparam.mUserGameInfoLength > 0x70 ? 0x70 : s_pparam.mUserGameInfoLength;
    g->mUserGameInfoLength = (u8) len;
    g->mAttribute = s_attr;
    g->mParentMaxSize = s_pparam.mParentMaxSize;
    g->mChildMaxSize = s_pparam.mChildMaxSize;
    memcpy(&g->mUserGameInfo, s_user_game_info, len);
    return (u16) (0x10 + len);
}

static void send_beacon(void) {
    BeaconBody b;
    memset(&b, 0, sizeof(b));
    b.channel = s_pparam.mChannel;
    b.beacon_period = s_pparam.mBeaconPeriod;
    b.game_info_length = game_info(b.game_info);
    send_packet(NULL, PKT_BEACON, 0, &b, sizeof(b));
}

// The WMBssDesc a scan reports for a parent.
static void bss_desc(const Parent *p, WMBssDesc *d) {
    memset(d, 0, 0xc0);
    u16 gl = p->beacon.game_info_length > 0x80 ? 0x80 : p->beacon.game_info_length;
    d->mLength = (u16) ((0x40 + gl + 1) / 2);
    d->mRssi = 40;
    memcpy(d->mBssid, p->mac, 6);
    d->mSsidLength = 0;
    d->mCapaInfo = 0x21;
    d->mRateSet.mBasic = 3;
    d->mRateSet.mSupport = 3;
    d->mBeaconPeriod = p->beacon.beacon_period;
    d->mDtimPeriod = 1;
    d->mChannel = p->beacon.channel;
    d->mGameInfoLength = gl;
    d->mOtherElementCount = 0;
    memcpy(&d->mGameInfo, p->beacon.game_info, gl);
}

// ------------------------------------------------------------------------------------------------ commands
static void net_start(void) {
    if (s_net) return;
    s_net = hapi_net_open();
    if (!s_held) s_held = malloc(sizeof(Held) * MAX_HELD);
}

static void stop_everything(void) {
    leave_all(false);
    drop_sends();
    s_parent = s_child = false;
    s_scanning = s_joining = false;
    s_held_count = 0;
    if (s_status) {
        child_reset_status();
        s_status->mPwrMgtMode = 0;
    }
}

static void start_scan(u32 scan_buf, u16 time_ms) {
    s_scanning = true;
    s_scan_buf = scan_buf;
    if (time_ms < 10) time_ms = 10;
    if (time_ms > 200) time_ms = 200;
    s_scan_due_ns = now_ns() + (u64) time_ms * 1000000ull;
    set_state(WIRELESS_UNIT_STATE_SCANNING);
}

static void scan_result(void) {
    s_scanning = false;
    u64 now = now_ns();
    for (int i = 0; i < MAX_PARENTS; i++) {
        Parent *p = &s_parents[(s_next_parent + i) % MAX_PARENTS];
        if (!p->used || since(now, p->heard_ns) > PARENT_TIMEOUT_NS) continue;
        s_next_parent = (s_next_parent + i + 1) % MAX_PARENTS;
        WMBssDesc *d = PTR(s_scan_buf);
        if (d) bss_desc(p, d);
        WirelessMessage *m = new_message(WIRELESS_REQUEST_BEGIN_SEARCH, 0);
        U16(m, 8) = WIRELESS_STATE_HOST_SIGHTED;
        memcpy((u8 *) m + 0x0a, p->mac, 6);
        U16(m, 0x10) = p->beacon.channel;
        U16(m, 0x12) = 3; // link level
        U16(m, 0x14) = 0; // SSID length
        u16 gl = p->beacon.game_info_length > 0x80 ? 0x80 : p->beacon.game_info_length;
        U16(m, 0x36) = gl;
        memcpy((u8 *) m + 0x38, p->beacon.game_info, gl > 0xc8 ? 0xc8 : gl);
        queue(m);
        return;
    }
    WirelessMessage *m = new_message(WIRELESS_REQUEST_BEGIN_SEARCH, 0);
    U16(m, 8) = WIRELESS_STATE_SEARCH_EMPTY;
    queue(m);
}

static void join_failed(u16 result) {
    s_joining = false;
    set_state(WIRELESS_UNIT_STATE_POWERED_ON);
    WirelessMessage *m = new_message(WIRELESS_REQUEST_ENTER_HOST, result);
    queue(m);
    port_log("wm7: joining failed (%d)", result);
}

static void port_send(const u32 *p) {
    u32 data = p[0], size = p[1] & 0xffff, dest = p[2] & 0xffff, port = p[3] & 0xffff;
    u32 callback = p[5], arg = p[6];
    Send *s = NULL;
    for (int i = 0; i < MAX_SENDS; i++) {
        if (!s_sends[i].used) {
            s = &s_sends[i];
            break;
        }
    }
    s_mp_seq = (u16) (s_mp_seq + 2);
    Send tmp;
    if (!s) s = &tmp;
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->dest = (u16) dest;
    s->port = (u16) port;
    s->mp_seq = s_mp_seq;
    s->size = (u16) size;
    s->data = data;
    s->callback = callback;
    s->arg = arg;
    if (s == &tmp || size > 0x200) {
        send_done(s, s == &tmp ? WIRELESS_RESULT_SEND_BACKLOG : WIRELESS_RESULT_BAD_INPUT);
        return;
    }
    u16 targets = s_parent ? (u16) (dest & 0xfffe) : (u16) (dest & 1);
    for (int a = 0; a < MAX_LINKS; a++) {
        if (!(targets & (1 << a)) || !s_links[a].used) continue;
        if (!link_push(&s_links[a], DATA_PORT, (u16) port, s_mp_seq, PTR(data), (u16) size, &s->link_seq[a])) {
            // Too much in flight on this link: the sender retries (data sharing does on SEND_BACKLOG).
            s->remaining = 0;
            send_done(s, WIRELESS_RESULT_SEND_BACKLOG);
            return;
        }
        s->remaining |= (u16) (1 << a);
    }
    if (!s->remaining) send_done(s, WIRELESS_RESULT_SUCCEEDED);
}

void port_wm7_command(u32 address) {
    u8 *buf = PTR(address);
    u16 id = U16(buf, 0) & 0x7fff;
    const u32 *p = (const u32 *) (buf + 4);
    WirelessMessage *m;
    if (WM7_TRACE && id != WIRELESS_REQUEST_QUEUE_EXCHANGE_DATA) {
        port_log("wm7: <- cmd %02x %08x %08x %08x", id, p[0], p[1], p[2]);
    }
    switch (id) {
    case WIRELESS_REQUEST_POWER_UP_STACK: // WM_Initialize: enable and power on
    case WIRELESS_REQUEST_ACTIVATE:       // WM_Enable
        s_status = PTR(p[1]);
        memset(s_status, 0, sizeof(WMStatus));
        hapi_net_mac(s_mac);
        net_start();
        set_state(id == WIRELESS_REQUEST_ACTIVATE ? WIRELESS_UNIT_STATE_ENABLED : WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_RESTART: // WM_Reset
        stop_everything();
        set_state(WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_CLOSE_STACK: // WM_End: powered off and disabled
        stop_everything();
        set_state(WIRELESS_UNIT_STATE_DISABLED);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_DEACTIVATE:
        set_state(WIRELESS_UNIT_STATE_DISABLED);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_RADIO_ON:
        set_state(WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_RADIO_OFF:
        set_state(WIRELESS_UNIT_STATE_ENABLED);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_CONFIGURE_HOST: { // WM_SetParentParameter
        const ParentParameter *pp = PTR(p[0]);
        s_pparam = *pp;
        memset(s_user_game_info, 0, sizeof(s_user_game_info));
        if (pp->mUserGameInfo && pp->mUserGameInfoLength) {
            memcpy(s_user_game_info, pp->mUserGameInfo, pp->mUserGameInfoLength > 0x70 ? 0x70 : pp->mUserGameInfoLength);
        }
        s_attr = (u8) ((pp->mEntryFlag ? WIRELESS_ATTRIBUTE_ENTRY : 0) | (pp->mMultiBootFlag ? WIRELESS_ATTRIBUTE_MULTIBOOT : 0) |
                       (pp->mKsFlag ? WIRELESS_ATTRIBUTE_KS_FLAG : 0) | (pp->mCsFlag ? WIRELESS_ATTRIBUTE_CS_FLAG : 0));
        if (s_status) s_status->mMaxEntry = pp->mMaxEntry;
        reply(id, 0);
        break;
    }
    case WIRELESS_REQUEST_OPEN_HOST: // WM_StartParent
        s_parent = true;
        s_child = false;
        s_session = (u32) (now_ns() ^ ((u32) s_mac[5] << 24) ^ ((u32) s_mac[4] << 16)) | 1;
        s_mp_seq = 0;
        if (s_status) {
            s_status->mOwnMemberIndex = 0;
            s_status->mPwrMgtMode = 0;
            s_status->mMpMaxSendSize = (u16) (s_pparam.mParentMaxSize + (s_pparam.mKsFlag ? 0x2a : 0) + 4);
            s_status->mMpMaxRecvSize = (u16) (s_pparam.mChildMaxSize + (s_pparam.mKsFlag ? 6 : 0) + 2);
            s_status->mMaxEntry = s_pparam.mMaxEntry;
        }
        update_children();
        set_state(WIRELESS_UNIT_STATE_PARENT_STARTED);
        s_beacon_ns = 0;
        m = new_message(id, 0);
        U16(m, 8) = WIRELESS_STATE_SESSION_START;
        queue(m);
        port_log("wm7: parent started (ggid %x tgid %x)", s_pparam.mGgid, s_pparam.mTgid);
        break;
    case WIRELESS_REQUEST_CLOSE_HOST: // WM_EndParent
        leave_all(false);
        drop_sends();
        s_parent = false;
        if (s_status) s_status->mMpFlag = 0;
        update_children();
        set_state(WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_BEGIN_SEARCH: // WM_StartScan
        start_scan(U32(buf, 4), U16(buf, 8));
        break;
    case WIRELESS_REQUEST_BEGIN_SEARCH_CHANNELS: // WM_StartScanEx
        start_scan(U32(buf, 4), U16(buf, 0x0a));
        break;
    case WIRELESS_REQUEST_STOP_SEARCH: // WM_EndScan
        s_scanning = false;
        set_state(WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_ENTER_HOST: { // WM_StartConnectEx
        const WMBssDesc *d = PTR(U32(buf, 4));
        int found = -1;
        for (int i = 0; d && i < MAX_PARENTS; i++) {
            if (s_parents[i].used && !memcmp(s_parents[i].mac, d->mBssid, 6)) found = i;
        }
        memcpy(s_join.user, buf + 8, 24);
        s_join_power_save = (u16) U32(buf, 0x20);
        if (found < 0) {
            join_failed(9); // timeout: no such parent
            break;
        }
        s_joining = true;
        s_join_parent = found;
        s_session = s_parents[found].session; // the join names the parent's session
        s_join_start_ns = now_ns();
        s_join_sent_ns = 0;
        port_log("wm7: joining %02x:%02x:%02x", d->mBssid[3], d->mBssid[4], d->mBssid[5]);
        break;
    }
    case WIRELESS_REQUEST_LEAVE: { // WM_Disconnect, WM_DisconnectChildren
        u16 bitmap = (u16) p[0], done = 0;
        if (s_parent) {
            for (int a = 1; a < MAX_LINKS; a++) {
                if (!(bitmap & (1 << a)) || !s_links[a].used) continue;
                for (int i = 0; i < 3; i++) send_packet(&s_links[a].addr, PKT_LEAVE, 0, NULL, 0);
                link_lost((u16) a, true, 26); // WM_STATECODE_DISCONNECTED_FROM_MYSELF
                done |= (u16) (1 << a);
            }
        } else if (s_child && s_links[0].used) {
            for (int i = 0; i < 3; i++) send_packet(&s_links[0].addr, PKT_LEAVE, own_aid(), NULL, 0);
            link_lost(0, false, 0);
            done = 1;
        }
        m = new_message(id, 0);
        U16(m, 8) = bitmap;
        U16(m, 0x0a) = done;
        queue(m);
        break;
    }
    case WIRELESS_REQUEST_BEGIN_EXCHANGE: // WM_StartMPEx
        if (s_status) {
            s_status->mMpFlag = 1;
            s_status->mMpRecvBufSize = (u16) (p[1] * 2);
            s_status->mMpSendBuf = PTR(p[2]);
        }
        set_state(s_parent ? WIRELESS_UNIT_STATE_PARENT_WITH_MP : WIRELESS_UNIT_STATE_CHILD_WITH_MP);
        if (s_child && s_links[0].used) link_push(&s_links[0], DATA_MP_READY, 0, 0, NULL, 0, NULL);
        m = new_message(id, 0);
        U16(m, 4) = WIRELESS_STATE_EXCHANGE_BEGUN;
        queue(m);
        break;
    case WIRELESS_REQUEST_QUEUE_EXCHANGE_DATA: // WM_SetMPDataToPortEx: the answer is the port-sent message
        port_send(p);
        break;
    case WIRELESS_REQUEST_STOP_EXCHANGE: // WM_EndMP
        if (s_status) s_status->mMpFlag = 0;
        if (s_child && s_links[0].used) link_push(&s_links[0], DATA_MP_STOP, 0, 0, NULL, 0, NULL);
        drop_sends();
        set_state(s_parent ? WIRELESS_UNIT_STATE_PARENT_STARTED
                           : s_child ? WIRELESS_UNIT_STATE_CHILD_CONNECTED : WIRELESS_UNIT_STATE_POWERED_ON);
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_SET_HOST_ADVERT: // WM_SetGameInfo
        memset(s_user_game_info, 0, sizeof(s_user_game_info));
        memcpy(s_user_game_info, PTR(p[0]), (p[1] & 0xffff) > 0x70 ? 0x70 : (p[1] & 0xffff));
        s_pparam.mUserGameInfoLength = (u16) ((p[1] & 0xffff) > 0x70 ? 0x70 : p[1]);
        s_pparam.mGgid = p[2];
        s_pparam.mTgid = (u16) p[3];
        s_attr = (u8) p[4];
        s_beacon_ns = 0; // at once
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_SET_JOIN_ALLOWED: // WM_SetEntry
        s_attr = (u8) ((s_attr & ~WIRELESS_ATTRIBUTE_ENTRY) | (p[0] ? WIRELESS_ATTRIBUTE_ENTRY : 0));
        s_beacon_ns = 0;
        reply(id, 0);
        break;
    case WIRELESS_REQUEST_MEASURE_CHANNEL: // WM_MeasureChannel: every channel is free
        m = new_message(id, 0);
        U16(m, 8) = U16(buf, 6); // channel
        U16(m, 0x0a) = 0;        // busy ratio
        queue(m);
        break;
    default: // life times, beacon indication, MP frequency, keys, DCF: accepted
        if (id != WIRELESS_REQUEST_SET_TIMEOUT && id != WIRELESS_REQUEST_SET_EXCHANGE_RATE &&
            id != WIRELESS_REQUEST_WATCH_BEACONS) {
            port_log("wm7: command %d accepted without effect", id);
        }
        reply(id, 0);
        break;
    }
    U16(buf, 0) |= WIRELESS_FREE_FLAG;
}

// ------------------------------------------------------------------------------------------------ receiving
static bool port_ready(u16 port) {
    WMArm9Buf *w = WMi_GetSystemWork();
    return w && s_status && s_status->mMpFlag && w->mPortCallbacks[port & 15];
}

static void deliver_port_data(u16 port, u16 aid, const u8 *mac, u16 mp_seq, const u8 *data, u16 size) {
    memcpy(s_payload, data, size);
    WirelessMessage *m = new_message(WIRELESS_REQUEST_PORT_DATA, 0);
    U16(m, 4) = WIRELESS_STATE_PORT_DATA_ARRIVED;
    U16(m, 6) = port;
    U32(m, 8) = ADDR(s_payload);
    U32(m, 0x0c) = ADDR(s_payload);
    U16(m, 0x10) = size;
    U16(m, 0x12) = aid;
    memcpy((u8 *) m + 0x14, mac, 6);
    U16(m, 0x1a) = mp_seq;
    U16(m, 0x20) = own_aid();
    U16(m, 0x22) = s_status ? (u16) (s_status->mConnectedChildren | 1) : 1;
    deliver(m);
}

// Port data that came early waits for its port callback (the receiver sets it once its MP has started).
static void deliver_held(void) {
    u64 now = now_ns();
    while (s_held_count) {
        Held *h = &s_held[s_held_head];
        if (!port_ready(h->port)) {
            if (since(now, h->time_ns) < HELD_TIMEOUT_NS) return;
        } else {
            deliver_port_data(h->port, h->aid, h->mac, h->mp_seq, h->data, h->size);
        }
        s_held_head = (s_held_head + 1) % MAX_HELD;
        s_held_count--;
    }
}

static void receive_data(Link *l, const DataBody *d) {
    switch (d->kind) {
    case DATA_MP_READY:
    case DATA_MP_STOP:
        l->mp_ready = d->kind == DATA_MP_READY;
        update_children();
        if (l->mp_ready && s_parent) {
            // Like the DS, a child that starts MP is "connected" again for the ports (data sharing sends it the sets).
        }
        break;
    case DATA_PORT: {
        u16 size = d->size > 0x200 ? 0x200 : d->size;
        if (!s_held_count && port_ready(d->port)) {
            deliver_port_data(d->port, l->aid, l->mac, d->mp_seq, d->data, size);
        } else if (s_held_count < MAX_HELD) {
            Held *h = &s_held[(s_held_head + s_held_count++) % MAX_HELD];
            h->time_ns = now_ns();
            h->port = d->port;
            h->aid = l->aid;
            h->mp_seq = d->mp_seq;
            h->size = size;
            memcpy(h->mac, l->mac, 6);
            memcpy(h->data, d->data, size);
        }
        break;
    }
    }
}

static Link *link_of(const NetHeader *h, const HostNetAddr *from) {
    if (s_parent) {
        if (h->aid < 1 || h->aid >= MAX_LINKS || h->session != s_session) return NULL;
    } else if (s_child) {
        if (h->aid != 0 || h->session != s_session) return NULL;
    } else {
        return NULL;
    }
    Link *l = &s_links[h->aid];
    if (!l->used || memcmp(l->mac, h->mac, 6)) return NULL;
    l->addr = *from; // follows an address change
    return l;
}

static void on_join(const NetHeader *h, const JoinBody *j, const HostNetAddr *from) {
    if (!s_parent || h->session != s_session) {
        send_packet(from, PKT_JOIN_NO, WIRELESS_RESULT_REFUSED, NULL, 0);
        return;
    }
    for (int a = 1; a < MAX_LINKS; a++) { // a repeated request: answer again
        if (s_links[a].used && !memcmp(s_links[a].mac, h->mac, 6)) {
            s_links[a].addr = *from;
            send_packet(from, PKT_JOIN_OK, (u16) a, NULL, 0);
            return;
        }
    }
    if (!(s_attr & WIRELESS_ATTRIBUTE_ENTRY)) {
        send_packet(from, PKT_JOIN_NO, WIRELESS_RESULT_NOT_LISTED, NULL, 0);
        return;
    }
    int aid = 0;
    u16 max = s_pparam.mMaxEntry ? s_pparam.mMaxEntry : 15;
    for (int a = 1; a <= max && a < MAX_LINKS; a++) {
        if (!s_links[a].used) {
            aid = a;
            break;
        }
    }
    if (!aid) {
        send_packet(from, PKT_JOIN_NO, WIRELESS_RESULT_LIST_FULL, NULL, 0);
        return;
    }
    link_open((u16) aid, from, h->mac);
    update_children();
    send_packet(from, PKT_JOIN_OK, (u16) aid, NULL, 0);
    WirelessMessage *m = new_message(WIRELESS_REQUEST_OPEN_HOST, 0);
    U16(m, 8) = WIRELESS_STATE_LINKED;
    memcpy((u8 *) m + 0x0a, h->mac, 6);
    U16(m, 0x10) = (u16) aid;
    memcpy((u8 *) m + 0x14, j->user, 24);
    queue(m);
    port_log("wm7: child %d joined", aid);
}

static void on_join_ok(const NetHeader *h, const HostNetAddr *from) {
    if (!s_joining) return;
    Parent *p = &s_parents[s_join_parent];
    if (h->session != p->session || memcmp(h->mac, p->mac, 6)) return;
    s_joining = false;
    u16 aid = h->aid;
    s_child = true;
    s_parent = false;
    s_session = p->session;
    s_mp_seq = 0;
    link_open(0, from, p->mac);
    const WMGameInfo *g = (const WMGameInfo *) p->beacon.game_info;
    if (s_status) {
        s_status->mOwnMemberIndex = aid;
        s_status->mPwrMgtMode = s_join_power_save;
        bool ks = (g->mAttribute & WIRELESS_ATTRIBUTE_KS_FLAG) != 0;
        s_status->mMpMaxSendSize = (u16) (g->mChildMaxSize + (ks ? 6 : 0) + 2);
        s_status->mMpMaxRecvSize = (u16) (g->mParentMaxSize + (ks ? 0x2a : 0) + 4);
        s_status->mConnectedChildren = 0;
        s_status->mLinkLevel = 3;
        s_status->mMpFlag = 0;
    }
    set_state(WIRELESS_UNIT_STATE_CHILD_CONNECTED);
    WirelessMessage *m = new_message(WIRELESS_REQUEST_ENTER_HOST, 0);
    U16(m, 8) = WIRELESS_STATE_LINKED;
    U16(m, 0x0a) = aid; // read as the own AID by the game
    U16(m, 0x10) = aid; // read as the own AID by WmReceiveFifo
    queue(m);
    port_log("wm7: joined as child %d", aid);
}

static void receive(const u8 *buf, int32_t n, const HostNetAddr *from) {
    if (n < (int32_t) sizeof(NetHeader)) return;
    const NetHeader *h = (const NetHeader *) buf;
    if (h->magic != NET_MAGIC || h->version != NET_VERSION || h->length > n - (int32_t) sizeof(NetHeader)) return;
    if (!memcmp(h->mac, s_mac, 6)) return; // our own beacon
    const u8 *body = buf + sizeof(NetHeader);
    u64 now = now_ns();
    switch (h->type) {
    case PKT_BEACON: {
        if (h->length < sizeof(BeaconBody)) return;
        int slot = -1, oldest = 0;
        for (int i = 0; i < MAX_PARENTS; i++) {
            if (s_parents[i].used && !memcmp(s_parents[i].mac, h->mac, 6)) {
                slot = i;
                break;
            }
            if (!s_parents[i].used || s_parents[i].heard_ns < s_parents[oldest].heard_ns) oldest = i;
        }
        if (slot < 0) {
            if (s_joining && s_join_parent == oldest) return;
            slot = oldest;
        }
        Parent *p = &s_parents[slot];
        p->used = true;
        p->addr = *from;
        memcpy(p->mac, h->mac, 6);
        p->session = h->session;
        p->heard_ns = now;
        memcpy(&p->beacon, body, sizeof(BeaconBody));
        break;
    }
    case PKT_JOIN:
        if (h->length >= sizeof(JoinBody)) on_join(h, (const JoinBody *) body, from);
        break;
    case PKT_JOIN_OK:
        on_join_ok(h, from);
        break;
    case PKT_JOIN_NO:
        if (s_joining && h->session == s_parents[s_join_parent].session) join_failed(h->aid);
        break;
    case PKT_LEAVE: {
        Link *l = link_of(h, from);
        if (l) link_lost(l->aid, true, WIRELESS_STATE_UNLINKED);
        break;
    }
    case PKT_ACK: {
        Link *l = link_of(h, from);
        if (!l || h->length < sizeof(AckBody)) return;
        l->heard_ns = now;
        u32 next = ((const AckBody *) body)->next;
        if (next > l->send_acked && next <= l->send_next) {
            l->send_acked = next;
            l->resend_ns = now + RESEND_NS;
            check_sends();
        }
        break;
    }
    case PKT_DATA: {
        Link *l = link_of(h, from);
        if (!l || h->length < DATA_HEADER_SIZE) return;
        l->heard_ns = now;
        const DataBody *d = (const DataBody *) body;
        if (d->seq == l->recv_next) {
            l->recv_next++;
            receive_data(l, d);
        }
        send_ack(l); // also for repeats, whose acknowledgement was lost
        break;
    }
    }
}

// ------------------------------------------------------------------------------------------------ the interrupt
bool port_wm7_active(void) { return s_net && s_status && s_status->mState != WIRELESS_UNIT_STATE_DISABLED; }

// Called at the OS's interrupt points: network input, timers, then the messages for the ARM9. True when a message
// was delivered.
bool port_wm7_poll(void) {
    if (!s_net) return false;
    u64 now = now_ns();
    bool ran = false;
    static u64 s_prev;
    if (WM7_TRACE && s_prev && now - s_prev > 100000000ull) port_log("wm7: no poll for %d ms", (int) ((now - s_prev) / 1000000));
    s_prev = now;
    if (since(now, s_last_poll_ns) >= 500000ull) {
        s_last_poll_ns = now;
        static u8 buf[sizeof(NetHeader) + sizeof(DataBody) + 64] __attribute__((aligned(8)));
        HostNetAddr from;
        int32_t n;
        for (int i = 0; i < 64 && (n = hapi_net_recv(buf, sizeof(buf), &from)) >= 0; i++) receive(buf, n, &from);

        if (s_parent && now >= s_beacon_ns) {
            send_beacon();
            s_beacon_ns = now + BEACON_NS;
        }
        if (s_scanning && now >= s_scan_due_ns) scan_result();
        if (s_joining) {
            if (since(now, s_join_start_ns) > JOIN_TIMEOUT_NS) {
                join_failed(9);
            } else if (since(now, s_join_sent_ns) > JOIN_RETRY_NS) {
                send_packet(&s_parents[s_join_parent].addr, PKT_JOIN, 0, &s_join, sizeof(s_join));
                s_join_sent_ns = now;
            }
        }
        for (int a = 0; a < MAX_LINKS; a++) {
            Link *l = &s_links[a];
            if (!l->used) continue;
            if (since(now, l->heard_ns) > LINK_TIMEOUT_NS) {
                port_log("wm7: station %d timed out", a);
                link_lost((u16) a, true, WIRELESS_STATE_UNLINKED);
                continue;
            }
            if (l->send_acked != l->send_next && now >= l->resend_ns) {
                // Resend what is in flight (the oldest first, a few at a time).
                for (u32 s = l->send_acked; s != l->send_next && s - l->send_acked < 8; s++) link_transmit(l, s, now);
                l->resend_ns = now + RESEND_NS;
            }
            if (since(now, l->sent_ns) > KEEPALIVE_NS) send_ack(l);
        }
    }
    deliver_held();
    for (int guard = 0; s_queue_count && guard < MSG_SLOTS; guard++) {
        WirelessMessage *m = s_queue[s_queue_head];
        s_queue_head = (s_queue_head + 1) % MSG_SLOTS;
        s_queue_count--;
        deliver(m);
        ran = true;
    }
    return ran;
}
