#!/usr/bin/env python3
"""The relay of online rollback matches (runtime/net_relay.c, T_RLY_PING).

A match that no direct path carries goes through the lobby (Oracle, Queretaro): ~90 ms each way from Honduras. This relay,
run close to the players (Miami: ~30 ms from Honduras), carries the rollback messages of such a match instead, when both
games find it the shorter way. It knows nothing of the lobby: it pairs the senders of one (lobby, a, b) and passes their
messages to each other.

Wire (UDP, big-endian; every packet starts 'S' 'U' 1 <type>):
  60 RLY_PING  client: time u64                      -> 61 RLY_PONG: the same time u64
  62 RLY_REG   client: lobby ip u32, mine u32, the opponent's u32  (every second while used: notes the sender's address)
  63 RLY_DATA  both:   lobby ip u32, from u32, to u32, rollback message
                       (passed as it came to the address "to" registered for (lobby, to, from); dropped otherwise)

Run:  python3 ssbu_relay.py [--port 39600]      (a systemd unit: see docs/RELAY.md)
"""
import argparse
import asyncio
import logging
import struct
import time

T_PING, T_PONG, T_REG, T_DATA = 60, 61, 62, 63
EXPIRE_S = 30            # a registration not renewed for this long is dropped
MAX_PER_S = 1500         # datagrams per second from one address (a match sends ~120); more are dropped
MAX_LEN = 1500

log = logging.getLogger('relay')


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.reg = {}            # (lobby, mine, other) -> (address, last time)
        self.rate = {}           # address -> [window start, count]
        self.stats = dict(ping=0, reg=0, data=0, passed=0, dropped=0)
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def allowed(self, addr, now):
        r = self.rate.get(addr)
        if not r or now - r[0] >= 1.0:
            self.rate[addr] = [now, 1]
            return True
        r[1] += 1
        return r[1] <= MAX_PER_S

    def datagram_received(self, data, addr):
        now = time.monotonic()
        if len(data) < 4 or len(data) > MAX_LEN or data[0:3] != b'SU\x01' or not self.allowed(addr, now):
            return
        t = data[3]
        if t == T_PING and len(data) >= 12:
            self.stats['ping'] += 1
            self.transport.sendto(b'SU\x01' + bytes([T_PONG]) + data[4:12], addr)
        elif t == T_REG and len(data) >= 16:
            lobby, mine, other = struct.unpack('>III', data[4:16])
            if not mine or not other or mine == other:
                return
            key = (lobby, mine, other)
            old = self.reg.get(key)
            if not old or old[0] != addr:
                log.info('registered %s: lobby %s, %s -> %s', addr, ip(lobby), ip(mine), ip(other))
            self.reg[key] = (addr, now)
            self.stats['reg'] += 1
        elif t == T_DATA and len(data) > 16:
            self.stats['data'] += 1
            lobby, frm, to = struct.unpack('>III', data[4:16])
            me = self.reg.get((lobby, frm, to))
            dst = self.reg.get((lobby, to, frm))
            if not me or me[0] != addr or not dst or now - dst[1] > EXPIRE_S:
                self.stats['dropped'] += 1        # (the sender is not the registered one, or the other side is not here)
                return
            self.reg[(lobby, frm, to)] = (addr, now)
            self.transport.sendto(data, dst[0])
            self.stats['passed'] += 1

    def sweep(self):
        now = time.monotonic()
        for k in [k for k, v in self.reg.items() if now - v[1] > EXPIRE_S]:
            del self.reg[k]
        for a in [a for a, r in self.rate.items() if now - r[0] > 10]:
            del self.rate[a]


def ip(v):
    return '%d.%d.%d.%d' % (v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255)


async def main(port):
    loop = asyncio.get_running_loop()
    transport, relay = await loop.create_datagram_endpoint(Relay, local_addr=('0.0.0.0', port))
    log.info('relay on UDP %d', port)
    try:
        while True:
            await asyncio.sleep(60)
            relay.sweep()
            log.info('last minute: %s; pairs registered %d', relay.stats, len(relay.reg) // 2)
            relay.stats = dict.fromkeys(relay.stats, 0)
    finally:
        transport.close()


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--port', type=int, default=39600)
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    asyncio.run(main(a.port))
