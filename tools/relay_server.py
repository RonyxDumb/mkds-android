#!/usr/bin/env python3
"""Relay server for online play (see port/platform/hostside/online.h).

Run it on a machine with a public IPv4 address and an open UDP port:

    python3 tools/relay_server.py [--port 23536] [--bind 0.0.0.0]

Players enter the machine's host name (and port, when it is not 23536) and a common room code in the app's network
settings. The server only passes datagrams between the stations of one room; it keeps nothing and needs no accounts.
A race is a few kilobytes per second and per player; stations that can reach each other directly leave the server
out after the first packets.

Wire format (little endian), before each datagram:
    u32 magic "MKDR", u8 version, u8 type, u16 port, u8 ip[4], u32 room
type 0 hello (answered with the sender's public address and the number of other stations in the room),
type 1 data for the station ip:port, type 2 broadcast to the room. Forwarded datagrams carry the sender's address.
Type 5 asks like a hello without joining the room (the app's connection test). Types 3 and 4 go between stations.
Type 6 says goodbye: the station is forgotten at once.
"""
import argparse
import socket
import struct
import time

MAGIC = 0x52444B4D
VERSION = 1
HEADER = struct.Struct("<IBBH4sI")
HELLO, DATA, BROADCAST, QUERY, BYE = 0, 1, 2, 5, 6

CLIENT_TIMEOUT = 30.0     # seconds without a datagram from a station
MAX_CLIENTS = 4096
MAX_ROOM = 64             # stations per room
MAX_DATAGRAM = 1400
# Per station: datagrams per second, averaged over a short burst (a race sends about 250).
RATE = 1500.0
BURST = 3000.0


class Client:
    __slots__ = ("room", "seen", "tokens", "refilled")

    def __init__(self, room, now):
        self.room = room
        self.seen = now
        self.tokens = BURST
        self.refilled = now


class Relay:
    def __init__(self, sock, log=print):
        self.sock = sock
        self.log = log
        self.clients = {}  # (ip, port) -> Client
        self.rooms = {}    # room -> set of (ip, port)
        self.swept = time.monotonic()

    def drop(self, addr):
        client = self.clients.pop(addr, None)
        if client is None:
            return
        members = self.rooms.get(client.room)
        if members is not None:
            members.discard(addr)
            if not members:
                del self.rooms[client.room]

    def sweep(self, now):
        self.swept = now
        for addr in [a for a, c in self.clients.items() if now - c.seen > CLIENT_TIMEOUT]:
            self.drop(addr)

    def touch(self, addr, room, now):
        """The station at addr, registered in room; None when the server or the room is full."""
        client = self.clients.get(addr)
        if client is not None and client.room != room:
            self.drop(addr)
            client = None
        if client is None:
            members = self.rooms.get(room, ())
            if len(self.clients) >= MAX_CLIENTS or len(members) >= MAX_ROOM:
                return None
            client = self.clients[addr] = Client(room, now)
            self.rooms.setdefault(room, set()).add(addr)
            self.log("%s:%d joined room %08x (%d stations, %d rooms)"
                     % (addr[0], addr[1], room, len(self.clients), len(self.rooms)))
        client.seen = now
        client.tokens = min(BURST, client.tokens + (now - client.refilled) * RATE)
        client.refilled = now
        if client.tokens < 1.0:
            return None
        client.tokens -= 1.0
        return client

    def send(self, kind, source, room, payload, dest):
        header = HEADER.pack(MAGIC, VERSION, kind, source[1], socket.inet_aton(source[0]), room)
        try:
            self.sock.sendto(header + payload, dest)
        except OSError:
            pass

    def handle(self, data, addr):
        now = time.monotonic()
        if now - self.swept > 5.0:
            self.sweep(now)
        if len(data) < HEADER.size or len(data) > HEADER.size + MAX_DATAGRAM:
            return
        magic, version, kind, port, ip, room = HEADER.unpack_from(data)
        if magic != MAGIC or version != VERSION:
            return
        if kind == QUERY:
            if len(self.clients) < MAX_CLIENTS or addr in self.clients:
                self.send(HELLO, addr, room, struct.pack("<H", len(self.rooms.get(room, ()))), addr)
            return
        if kind == BYE:
            self.drop(addr)
            return
        client = self.touch(addr, room, now)
        if client is None:
            return
        payload = data[HEADER.size:]
        if kind == HELLO:
            others = len(self.rooms[room]) - 1
            self.send(HELLO, addr, room, struct.pack("<H", others), addr)
        elif kind == DATA:
            dest = (socket.inet_ntoa(ip), port)
            # Only to a station of the same room: the server cannot be made to send to anyone else.
            other = self.clients.get(dest)
            if other is not None and other.room == room and dest != addr:
                self.send(DATA, addr, room, payload, dest)
        elif kind == BROADCAST:
            for dest in self.rooms[room]:
                if dest != addr:
                    self.send(BROADCAST, addr, room, payload, dest)

    def serve(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
            except OSError:
                continue
            self.handle(data, addr)


def main():
    parser = argparse.ArgumentParser(description="Relay server for online play")
    parser.add_argument("--port", type=int, default=23536)
    parser.add_argument("--bind", default="0.0.0.0")
    args = parser.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print("relay server on %s:%d (UDP)" % (args.bind, args.port), flush=True)
    try:
        Relay(sock, lambda line: print(line, flush=True)).serve()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
