#!/usr/bin/env bash
# Installs the online lobby (ssbu_lobby.py) as a systemd service on a Linux server, e.g. an Oracle Cloud "Always Free"
# Ubuntu VM. Run it ON the server, from the folder that holds ssbu_lobby.py:
#     sudo bash deploy_lobby.sh [port]            (default UDP port 39500)
# Then players set online_server = <server address>:<port> (or SSBU_ONLINE=<server address>:<port>).
# Oracle Cloud also needs the port opened in the VCN: Networking > Virtual cloud networks > your VCN > Security List >
# Add Ingress Rule: source 0.0.0.0/0, protocol UDP, destination port <port>. This script opens the VM's own firewall.
# The lobby itself is standard library Python only.
#
# Status endpoint for the project site (players online, rooms, up/down; counts only, never addresses or names):
#   the lobby always serves GET /status on 127.0.0.1:$STATUS_PORT (default 8080, TCP, reachable from this machine only).
#   With a domain name that points at this server (a DNS A/AAAA record you made), the script also installs Caddy from
#   its official apt repository and publishes it as https://<domain>/status with an automatic TLS certificate:
#       sudo DOMAIN=lobby.example.org bash deploy_lobby.sh [port]
#       (optional: ACME_EMAIL=<an address of yours> for the certificate authority's expiry notices, STATUS_PORT=8080;
#        Caddy works without ACME_EMAIL)
#   That needs TCP 80 and 443 open in the cloud firewall too (Oracle: two more ingress rules, protocol TCP).
#   Without DOMAIN, Caddy is skipped; run the script again later with DOMAIN=... to add it.
#   Then set STATUS_URL = "https://<domain>/status" in the site's config.js.
# Ranked ladder (ladder.py, next to ssbu_lobby.py): on when ladder.py is there (LADDER=on: required), by invitation, its sqlite file in
#   /var/lib/ssbu-lobby/ladder.db; with DOMAIN the table is published as https://<domain>/ladder.
#       sudo LADDER=off bash deploy_lobby.sh          no ladder
#       sudo LADDER_OPEN=1 bash deploy_lobby.sh       any new player is ranked (no invitation needed)
#       sudo ssbu-ladder-invite <name>                a one-use invitation code for a player (it goes in the game's
#                                                     graphics.ini as ladder_invite = <code>)
#       sudo ssbu-account list | rename <old> <new> | ban <name> | unban <name> | delete <name>    the players' accounts
# Tournaments (tournament.py, next to ssbu_lobby.py; with the ladder): made and played in the game (F1 > Tournament), the
#   brackets on https://<domain>/tournaments. By default any account may make one; to allow only some:
#       sudo LADDER_OPEN=1 TOURNAMENT_ADMINS="Rober,napky" bash deploy_lobby.sh
# Remove: systemctl disable --now ssbu-lobby; rm -r /opt/ssbu-lobby /usr/local/bin/ssbu-ladder-invite /usr/local/bin/ssbu-account; userdel ssbu-lobby
#   (the ladder's data: rm -r /var/lib/ssbu-lobby) (and, if it was added: apt-get remove caddy)
set -euo pipefail
PORT="${1:-39500}"
STATUS_PORT="${STATUS_PORT:-8080}"
STATUS_BIND="${STATUS_BIND:-127.0.0.1}"   # (0.0.0.0: the pages straight on http://<ip>:$STATUS_PORT, no domain: deploy_arenas.sh)
DOMAIN="${DOMAIN:-}"
ACME_EMAIL="${ACME_EMAIL:-}"
LADDER="${LADDER:-auto}"
LADDER_OPEN="${LADDER_OPEN:-}"
TOURNAMENT_ADMINS="${TOURNAMENT_ADMINS:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$HERE/ssbu_lobby.py" ] || { echo "ssbu_lobby.py not found next to this script"; exit 1; }
[[ "$PORT" =~ ^[0-9]+$ && "$STATUS_PORT" =~ ^[0-9]+$ ]] || { echo "ports must be numbers"; exit 1; }
[[ "$STATUS_BIND" =~ ^[0-9.]+$ ]] || { echo "STATUS_BIND must be an IPv4 address (127.0.0.1 or 0.0.0.0)"; exit 1; }
DOMAIN_RE='^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?\.[A-Za-z]{2,}$'
if [ -n "$DOMAIN" ] && ! [[ "$DOMAIN" =~ $DOMAIN_RE ]]; then
    echo "DOMAIN must be a plain host name such as lobby.example.org (no https://, no path)"; exit 1
fi
command -v python3 >/dev/null || { apt-get update -q && apt-get install -y -q python3; }
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || { echo "python 3.8 or newer needed"; exit 1; }

install -d -m 755 /opt/ssbu-lobby
install -m 644 "$HERE/ssbu_lobby.py" /opt/ssbu-lobby/ssbu_lobby.py
LADDER_ARGS=""
if [ "$LADDER" = auto ] && [ ! -f "$HERE/ladder.py" ]; then
    echo "note: ladder.py is not next to this script: no ranked ladder"; LADDER=off
fi
if [ "$LADDER" != off ] && [ "$LADDER" != 0 ]; then
    [ -f "$HERE/ladder.py" ] || { echo "ladder.py not found next to this script (LADDER=off: no ladder)"; exit 1; }
    install -m 644 "$HERE/ladder.py" /opt/ssbu-lobby/ladder.py
    if [ -f "$HERE/tournament.py" ]; then install -m 644 "$HERE/tournament.py" /opt/ssbu-lobby/tournament.py   # (tournaments: with the ladder)
    else echo "note: tournament.py is not next to this script: no tournaments"; fi
    LADDER_ARGS=" --ladder-db /var/lib/ssbu-lobby/ladder.db"
    [ -n "$LADDER_OPEN" ] && [ "$LADDER_OPEN" != 0 ] && LADDER_ARGS="$LADDER_ARGS --ladder-open"
    if [ -n "$TOURNAMENT_ADMINS" ]; then
        [[ "$TOURNAMENT_ADMINS" =~ ^[^\"]+$ ]] || { echo "TOURNAMENT_ADMINS: account names, comma-separated, without quotes"; exit 1; }
        LADDER_ARGS="$LADDER_ARGS --tournament-admins \"$TOURNAMENT_ADMINS\""
    fi
fi
# a fixed system user (not DynamicUser): the ladder's database is written both by the service and by ssbu-ladder-invite
id ssbu-lobby >/dev/null 2>&1 || useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin ssbu-lobby
cat > /usr/local/bin/ssbu-ladder-invite <<'EOF'
#!/bin/sh
# a one-use invitation code for the lobby's ranked ladder: ssbu-ladder-invite <player name>
[ -n "$1" ] || { echo "usage: ssbu-ladder-invite <player name>"; exit 1; }
exec sudo -u ssbu-lobby /usr/bin/python3 /opt/ssbu-lobby/ssbu_lobby.py --ladder-db /var/lib/ssbu-lobby/ladder.db --ladder-invite "$1"
EOF
chmod 755 /usr/local/bin/ssbu-ladder-invite
cat > /usr/local/bin/ssbu-account <<'EOF'
#!/bin/sh
# the players' accounts: ssbu-account list | rename <old name> <new name> | ban <name> | unban <name> | delete <name>
L="sudo -u ssbu-lobby /usr/bin/python3 /opt/ssbu-lobby/ssbu_lobby.py --ladder-db /var/lib/ssbu-lobby/ladder.db"
case "$1" in
  list)   exec $L --account-list ;;
  rename) [ -n "$2" ] && [ -n "$3" ] || { echo "usage: ssbu-account rename <old name> <new name>"; exit 1; }; exec $L --account-rename "$2" "$3" ;;
  ban|unban|delete) [ -n "$2" ] || { echo "usage: ssbu-account $1 <name>"; exit 1; }; exec $L --account-$1 "$2" ;;
  *) echo "usage: ssbu-account list | rename <old> <new> | ban <name> | unban <name> | delete <name>"; exit 1 ;;
esac
EOF
chmod 755 /usr/local/bin/ssbu-account

cat > /etc/systemd/system/ssbu-lobby.service <<EOF
[Unit]
Description=SSBU native port online lobby (UDP $PORT, status on 127.0.0.1:$STATUS_PORT)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/bin/python3 /opt/ssbu-lobby/ssbu_lobby.py --bind 0.0.0.0 --port $PORT --status-port $STATUS_PORT --status-bind $STATUS_BIND$LADDER_ARGS
Restart=always
RestartSec=3
User=ssbu-lobby
Group=ssbu-lobby
StateDirectory=ssbu-lobby
StateDirectoryMode=0750
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
MemoryMax=256M
CPUQuota=50%
LogRateLimitIntervalSec=60
LogRateLimitBurst=200

[Install]
WantedBy=multi-user.target
EOF

# the VM's own firewall (Oracle's Ubuntu images use iptables rules; ufw if it is active)
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow "$PORT/udp"
else
    iptables -C INPUT -p udp --dport "$PORT" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$PORT" -j ACCEPT
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; else
        echo "note: install iptables-persistent to keep the firewall rule after a reboot"; fi
fi

systemctl daemon-reload
systemctl enable ssbu-lobby
systemctl restart ssbu-lobby
sleep 1
systemctl --no-pager status ssbu-lobby | head -5
echo "lobby running on UDP $PORT. Logs: journalctl -u ssbu-lobby -f"
echo "remember the cloud firewall (Oracle: VCN Security List ingress rule, UDP $PORT)"
echo "status endpoint (local only): curl -s http://127.0.0.1:$STATUS_PORT/status"
if [ -n "$LADDER_ARGS" ]; then echo "ladder on: invite a player with  sudo ssbu-ladder-invite <name>  (table: curl -s http://127.0.0.1:$STATUS_PORT/ladder.json)"; fi

# ---- optional: https://DOMAIN/status through Caddy (automatic TLS)
if [ -z "$DOMAIN" ]; then
    echo
    echo "No DOMAIN given: the status endpoint stays local and Caddy was not installed."
    echo "To publish it for the website later: point a DNS A/AAAA record of your domain at this server, open TCP 80 and 443"
    echo "in the cloud firewall, then run:  sudo DOMAIN=<your.domain> bash deploy_lobby.sh $PORT"
    echo "and set STATUS_URL = \"https://<your.domain>/status\" in the site's config.js."
    exit 0
fi

if ! command -v caddy >/dev/null; then
    # official Caddy apt repository (https://caddyserver.com/docs/install#debian-ubuntu-raspbian)
    apt-get update -q
    apt-get install -y -q debian-keyring debian-archive-keyring apt-transport-https curl gnupg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
    chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg /etc/apt/sources.list.d/caddy-stable.list
    apt-get update -q
    apt-get install -y -q caddy
fi

if [ -f /etc/caddy/Caddyfile ] && [ ! -f /etc/caddy/Caddyfile.before-ssbu ]; then
    cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-ssbu
fi
GLOBAL=""
[ -n "$ACME_EMAIL" ] && GLOBAL="{
	email $ACME_EMAIL
}
"
cat > /etc/caddy/Caddyfile <<EOF
${GLOBAL}# written by deploy_lobby.sh: the lobby's status endpoint (counts only), the ladder's table and the tournaments' brackets (names and ratings), nothing else
$DOMAIN {
	encode gzip
	handle /status {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle /ladder* {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle /player/* {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle /tournament* {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle /api/* {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle {
		respond "not found" 404
	}
	log {
		output discard
	}
}
EOF
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile

if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow 80/tcp
    ufw allow 443/tcp
else
    for p in 80 443; do
        iptables -C INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport "$p" -j ACCEPT
    done
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; fi
fi

systemctl enable caddy >/dev/null 2>&1 || true
systemctl reload caddy 2>/dev/null || systemctl restart caddy
echo
echo "Caddy serves https://$DOMAIN/status (the certificate is fetched on first use; DNS must already point here)."
echo "Open TCP 80 and 443 in the cloud firewall too. Check: curl -s https://$DOMAIN/status"
echo "Site: set STATUS_URL = \"https://$DOMAIN/status\" in config.js."
if [ -n "$LADDER_ARGS" ]; then echo "Ladder table: https://$DOMAIN/ladder"; echo "Tournaments: https://$DOMAIN/tournaments"; fi
