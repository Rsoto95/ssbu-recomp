# The relay (Miami)

A rollback match that no direct path carries goes through the lobby, on the Oracle server in Querétaro, Mexico. From
Honduras that is about 90 ms each way, so two players there play at about 200 ms. A relay closer to them carries those
matches instead.

`tools/net/ssbu_relay_v2.py` is that relay. Each game measures its round trip to the relay and to the lobby, and tells the
opponent its own (the NAT message, see `runtime/net_relay.c`, `T_RLY_PING`). The relay is used only when the two round
trips through it add up to at least 15 ms less than through the lobby. Two players near Querétaro keep using the lobby;
two players in Central America use the relay.

Measured from Honduras (TCP connect, 2026-11-27): Miami 26–34 ms, Atlanta 38–46, Dallas 53–61, Ashburn 59–68,
Bogotá 79–90, Querétaro 87–93, Monterrey 87–97, Mexico City 81–95.

## Setting it up

1. A small Linux VPS in Miami (for example Vultr, Miami, the smallest plan): 1 CPU and 512 MB are plenty. A match
   through the relay is about 120 small datagrams a second each way.
2. Open UDP 39600 in the provider's firewall and in the machine's own (`ufw allow 39600/udp`).
3. Copy `tools/net/ssbu_relay_v2.py` to `/opt/ssbu-relay/` and install the service:

```
[Unit]
Description=SSBU relay
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 /opt/ssbu-relay/ssbu_relay_v2.py --port 39600
Restart=always
User=nobody

[Install]
WantedBy=multi-user.target
```

   (`/etc/systemd/system/ssbu-relay.service`, then `systemctl enable --now ssbu-relay`). Every minute the log
   (`journalctl -u ssbu-relay`) shows what was passed.
4. Set the relay in the game: `NET_RELAY_DEFAULT` in `runtime/net_relay.c` (every player, with the next runtime release),
   or `online_relay = <host>:39600` in `build/config/graphics.ini` (one PC, to try it first). `online_relay = off` turns it off.

## What the log says

- `relay: <host> = <address>`: the relay was found.
- `relay: the match goes through the relay ... (round trips: relay A + B ms, lobby C + D ms)`: it carries this match.
- `match: NAT check of the second port ...` and `match: the opponent's NAT check ...`: what each router does. EASY keeps
  one outside port per socket; HARD gives every destination its own (in turn, or at random). One HARD side is helped by
  the spread (`match: NAT spread: ...`); two HARD sides need the lobby or the relay.
- `lobby: NAT check: ...`: the same check once per session, for the statistics.

An older design, `tools/net/ssbu_relay.py` (2026-11-05: the lobby hands out a relay and signs each match), was taken out of
the game the same day; its lobby side is still in `ssbu_lobby.py` (`--relay-secret`). This relay is separate from it.
