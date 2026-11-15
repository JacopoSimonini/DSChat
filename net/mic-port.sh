#!/usr/bin/env bash
# Diagnostic (mictest): let the DS reach the mictest receiver
# (tools/mic/receiver.py, TCP 4243 on the AP address). Runtime-only.
#
#   sudo net/mic-port.sh open     after ./dschat start (or net/ap-up.sh)
#   sudo net/mic-port.sh close    when done (before or after ./dschat stop)
#   sudo net/mic-port.sh status
#
# open adds exactly two rules, both tagged "dschat-mic":
#   * nft:      `iifname dsap0 tcp dport 4243 accept` at the top of chain
#               inet dschat input (the AP's own table; it vanishes with the AP)
#   * iptables: INPUT -i dsap0 -p tcp --dport 4243 ACCEPT (needed because ufw's
#               default-deny lives in the iptables table)
# Only the DS network (dsap0) gets the port; nothing on the hotspot side.
# close removes both (safe to repeat, also when the AP is already down).
# If ./dschat's watcher restarts the AP (hotspot channel change), the nft
# rule goes with the old table: run `open` again.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ap.conf
source "$HERE/ap.conf"
PORT=4243
TAG=dschat-mic
[[ $EUID -eq 0 ]] || { echo "mic-port: run with sudo" >&2; exit 1; }

ipt_rule=(INPUT -i "$AP_IFACE" -p tcp --dport "$PORT" -m comment --comment "$TAG" -j ACCEPT)
nft_handles() {
    nft -a list chain inet dschat input 2>/dev/null | awk -v t="\"$TAG\"" '$0 ~ t {print $NF}'
}

case ${1:-} in
    open)
        nft list table inet dschat &>/dev/null || { echo "mic-port: the AP is not up (./dschat start)" >&2; exit 1; }
        [[ -n $(nft_handles) ]] || nft insert rule inet dschat input iifname "$AP_IFACE" tcp dport "$PORT" accept comment "\"$TAG\""
        iptables -C "${ipt_rule[@]}" 2>/dev/null || iptables -I "${ipt_rule[0]}" 1 "${ipt_rule[@]:1}"
        echo "mic-port: tcp/$PORT open on $AP_IFACE only (close: sudo net/mic-port.sh close)"
        ;;
    close)
        for h in $(nft_handles); do nft delete rule inet dschat input handle "$h"; done
        while iptables -C "${ipt_rule[@]}" 2>/dev/null; do iptables -D "${ipt_rule[@]}"; done
        echo "mic-port: tcp/$PORT closed"
        ;;
    status)
        n=$(nft_handles | wc -l); i=0
        iptables -C "${ipt_rule[@]}" 2>/dev/null && i=1
        echo "mic-port: nft rules $n, iptables rule $i ($( (( n && i )) && echo open || echo closed))"
        ;;
    *) echo "usage: sudo $0 open|close|status" >&2; exit 2 ;;
esac
