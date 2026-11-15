#!/usr/bin/env bash
# DSChat: tear down everything net/ap-up.sh created. Safe to run repeatedly,
# and after a partial/failed ap-up.
#
#   sudo net/ap-down.sh
#
# The last logs are saved to net/logs/last/ (gitignored) before /run/dschat
# is removed. The upstream connection is never touched.

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STATE=/run/dschat
NM_CONF=/run/NetworkManager/conf.d/zz-dschat.conf
QUIET=0
[[ ${1:-} == --quiet ]] && QUIET=1

say() { (( QUIET )) || echo "ap-down: $*"; }

[[ $EUID -eq 0 ]] || { echo "ap-down: must run as root: sudo $0" >&2; exit 1; }

CONF=$HERE/ap.conf
[[ -r $STATE/conf ]] && CONF=$(cat "$STATE/conf")
# shellcheck source=ap.conf
source "$CONF"
# shellcheck source=iface.sh
source "$HERE/iface.sh"
[[ -r $STATE/info ]] && UPSTREAM_IFACE=$(sed -n 's/^UPSTREAM_IFACE=//p' "$STATE/info")
dschat_resolve_ifaces 2>/dev/null || true

stop_pid() {   # name pidfile
    local pid
    [[ -r $2 ]] || return 0
    pid=$(cat "$2")
    if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid"
        for _ in $(seq 25); do kill -0 "$pid" 2>/dev/null || break; sleep 0.2; done
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid"
        say "stopped $1 ($pid)"
    fi
}

# 1. Daemons.
stop_pid dnsmasq "$STATE/dnsmasq.pid"
stop_pid hostapd "$STATE/hostapd.pid"

# 2. Firewall: our iptables rules (by exact spec) and our nft table.
while iptables -C INPUT -i "$AP_IFACE" -p tcp --dport "$RELAY_PORT" -m comment --comment dschat -j ACCEPT 2>/dev/null; do
    iptables -D INPUT -i "$AP_IFACE" -p tcp --dport "$RELAY_PORT" -m comment --comment dschat -j ACCEPT
    say "removed iptables INPUT rule tcp/$RELAY_PORT"
done
while iptables -C INPUT -i "$AP_IFACE" -p udp --dport 67 -m comment --comment dschat -j ACCEPT 2>/dev/null; do
    iptables -D INPUT -i "$AP_IFACE" -p udp --dport 67 -m comment --comment dschat -j ACCEPT
    say "removed iptables INPUT rule udp/67"
done
if nft list table inet dschat &>/dev/null; then
    nft delete table inet dschat && say "removed nft table inet dschat"
fi

# 3. Interface (its address and per-interface sysctl go with it).
if ip link show "$AP_IFACE" &>/dev/null; then
    iw dev "$AP_IFACE" del && say "removed interface $AP_IFACE"
fi

# 4. NetworkManager runtime config.
if [[ -e $NM_CONF ]]; then
    rm -f "$NM_CONF"
    nmcli general reload conf && say "removed $NM_CONF"
fi

# 5. Keep the last logs, then remove the runtime state.
if [[ -d $STATE ]]; then
    LOGDIR=$HERE/logs/last
    mkdir -p "$LOGDIR"
    cp -f "$STATE"/*.log "$STATE"/*.conf "$STATE"/dnsmasq.leases "$LOGDIR"/ 2>/dev/null
    # never keep the WEP key outside /run
    [[ -f $LOGDIR/hostapd.conf ]] && sed -i 's/^\(wep_key[0-9]=\).*/\1<redacted>/' "$LOGDIR/hostapd.conf"
    [[ -n ${SUDO_UID:-} ]] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$HERE/logs"
    rm -rf "$STATE"
    say "logs saved to net/logs/last/, removed $STATE"
fi

# 6. Report on the upstream link (never modified).
if [[ -n ${UPSTREAM_IFACE:-} ]]; then
    say "upstream $UPSTREAM_IFACE: $(nmcli -g GENERAL.STATE,GENERAL.CONNECTION device show "$UPSTREAM_IFACE" 2>/dev/null | paste -sd' ')"
fi
say "done"
exit 0
