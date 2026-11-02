#!/usr/bin/env bash
# Opens the ports of the game's Online menu server (deploy_arenas.sh) in the default security list of an Oracle Cloud VCN,
# from Oracle Cloud Shell (the ">_" terminal of the console, already signed in). Paste it there, or:
#     bash oci_arenas_ports.sh [VCN name]          (default: the only / first VCN of the tenancy's root compartment)
# Ports: TCP 443; UDP 10025 10125 33334 33335 (Pia's NAT check), 39599 (arena query), 39500 (lobby). Rules already there
# are kept; nothing else is changed.
set -euo pipefail
C="${OCI_TENANCY:?run this in Oracle Cloud Shell (OCI_TENANCY is not set)}"
if [ -n "${1:-}" ]; then VCN=$(oci network vcn list -c "$C" --display-name "$1" --query 'data[0].id' --raw-output)
else VCN=$(oci network vcn list -c "$C" --query 'data[0].id' --raw-output); fi
[ -n "$VCN" ] && [ "$VCN" != null ] || { echo "no VCN found"; exit 1; }
SL=$(oci network vcn get --vcn-id "$VCN" --query 'data."default-security-list-id"' --raw-output)
oci network security-list get --security-list-id "$SL" --output json > /tmp/ssbu_sl.json
python3 > /tmp/ssbu_in.json <<'PY'
import json
rules = json.load(open("/tmp/ssbu_sl.json"))["data"]["ingress-security-rules"]
def has(proto, p):
    for r in rules:
        o = r.get("udp-options" if proto == "17" else "tcp-options") or {}
        g = o.get("destination-port-range") or {}
        if r.get("protocol") == proto and r.get("source") == "0.0.0.0/0" and g.get("min", 0) <= p <= g.get("max", -1):
            return True
    return False
add = []
for proto, p, what in [("6", 443, "SSBU Online menu (NEX)")] + [("17", p, "SSBU Online menu") for p in (10025, 10125, 33334, 33335, 39599, 39500)]:
    if not has(proto, p):
        key = "udp-options" if proto == "17" else "tcp-options"
        add.append({"protocol": proto, "source": "0.0.0.0/0", "source-type": "CIDR_BLOCK", "is-stateless": False,
                    "description": what, key: {"destination-port-range": {"min": p, "max": p}}})
print(json.dumps(rules + add if add else None))
PY
if [ "$(cat /tmp/ssbu_in.json)" != null ]; then
    oci network security-list update --security-list-id "$SL" --ingress-security-rules file:///tmp/ssbu_in.json --force >/dev/null
    echo "ports opened: TCP 443; UDP 10025 10125 33334 33335 39599 39500"
else echo "the ports were already open"; fi
