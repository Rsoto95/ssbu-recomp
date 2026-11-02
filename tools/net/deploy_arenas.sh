#!/usr/bin/env bash
# Installs the server of the game's own Online menu (Battle Arenas: nex_server.py) AND the lobby it needs for rollback
# (ssbu_lobby.py 1.7: arena keys and groups) as two systemd services on a Linux server, e.g. an Oracle Cloud "Always
# Free" Ubuntu VM. Run it ON the server, from the folder that holds nex_server.py, ssbu_lobby.py and deploy_lobby.sh:
#     sudo bash deploy_arenas.sh                    (the public address is looked up by itself)
#     sudo PUBLIC_IP=203.0.113.7 bash deploy_arenas.sh
# Then each player's game: SSBU_ONLINE_MENU=1, SSBU_ONLINE_MENU_SERVER=<public IP>, SSBU_ONLINE=<public IP>:39500.
#
# Ports (Oracle Cloud: Networking > Virtual cloud networks > your VCN > Security Lists > Default Security List > Add
# Ingress Rules, source CIDR 0.0.0.0/0, one rule per line; this script opens the VM's own firewall):
#   TCP 443      the game's NEX connection (a fixed port of the game: nothing else may use 443 on this server)
#   UDP 10025, 10125, 33334, 33335   Pia's NAT check and reachability ports
#   UDP 39599    the arena query (which arena a player is in: the rollback opponent and spectators)
#   UDP 39500    the lobby (rollback messages, spectators' pads)
# Resources: two small Python processes (standard library only), ~100 MB of RAM, almost no CPU; traffic: the spectators'
# pads (a 1-on-1 with 4 watching: under 1 Mbit/s); the fights themselves go straight between the players' PCs.
# Logs: journalctl -u ssbu-arenas -f   /   journalctl -u ssbu-lobby -f
# Remove: systemctl disable --now ssbu-arenas; rm -r /opt/ssbu-arenas; userdel ssbu-arenas (the lobby: see deploy_lobby.sh)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
for f in nex_server.py ssbu_lobby.py deploy_lobby.sh; do [ -f "$HERE/$f" ] || { echo "$f not found next to this script"; exit 1; }; done
command -v python3 >/dev/null || { apt-get update -q && apt-get install -y -q python3; }
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || { echo "python 3.8 or newer needed"; exit 1; }

PUBLIC_IP="${PUBLIC_IP:-}"
if [ -z "$PUBLIC_IP" ]; then
    PUBLIC_IP="$(curl -s --max-time 5 https://ifconfig.me || curl -s --max-time 5 https://api.ipify.org || true)"
fi
[[ "$PUBLIC_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "the public address was not found: run with PUBLIC_IP=<address>"; exit 1; }
echo "public address: $PUBLIC_IP"

# 1. the lobby (rollback messages; 1.7 has the arena keys and groups)
LADDER="${LADDER:-off}" bash "$HERE/deploy_lobby.sh" 39500

# 2. the Online menu's server
install -d -m 755 /opt/ssbu-arenas
install -m 644 "$HERE/nex_server.py" /opt/ssbu-arenas/nex_server.py
id ssbu-arenas >/dev/null 2>&1 || useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin ssbu-arenas
install -d -m 755 -o ssbu-arenas /var/lib/ssbu-arenas
cat > /etc/systemd/system/ssbu-arenas.service <<EOF
[Unit]
Description=SSBU recomp: the game's Online menu (Battle Arenas) server
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/var/lib/ssbu-arenas
ExecStart=/usr/bin/python3 /opt/ssbu-arenas/nex_server.py --bind 0.0.0.0 --port 443 --public-ip $PUBLIC_IP
Restart=always
RestartSec=3
User=ssbu-arenas
AmbientCapabilities=CAP_NET_BIND_SERVICE
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
EOF

# the VM's own firewall (Oracle's Ubuntu images use iptables rules; ufw if it is active)
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow 443/tcp
    for p in 10025 10125 33334 33335 39599; do ufw allow "$p/udp"; done
else
    iptables -C INPUT -p tcp --dport 443 -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport 443 -j ACCEPT
    for p in 10025 10125 33334 33335 39599; do
        iptables -C INPUT -p udp --dport "$p" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$p" -j ACCEPT
    done
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; else
        echo "note: install iptables-persistent to keep the firewall rules after a reboot"; fi
fi
systemctl daemon-reload
systemctl enable ssbu-arenas
systemctl restart ssbu-arenas
sleep 1
systemctl --no-pager status ssbu-arenas | head -5
echo
echo "done. Each player's game:"
echo "    SSBU_ONLINE_MENU=1  SSBU_ONLINE_MENU_SERVER=$PUBLIC_IP  SSBU_ONLINE=$PUBLIC_IP:39500"
echo "Oracle Cloud: add the ingress rules listed at the top of this script (TCP 443; UDP 10025 10125 33334 33335 39599 39500)."
