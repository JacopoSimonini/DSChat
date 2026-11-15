#!/usr/bin/env bash
# DSChat: bring up an open 2.4 GHz access point for the DS Lite.
#
#   sudo net/ap-up.sh [UPSTREAM_IFACE]        (default: net/ap.conf, "auto" = detect)
#   sudo DEBUG=1 net/ap-up.sh                 (hostapd -dd full debug log)
#   sudo net/ap-up.sh --no-watch              (don't follow the logs at the end)
#   sudo net/ap-up.sh --wep KEY               (WEP instead of open; KEY = 5 or 13
#                                              ASCII characters, or 10 or 26 hex digits)
#   sudo net/ap-up.sh --wep-file FILE         (WEP, key read from the WEP_KEY=... line
#                                              of FILE, e.g. net/dschat.conf: the key
#                                              never appears on a command line)
#
# ./dschat start (repo root) runs this for you, with the security mode from
# net/dschat.conf; running it by hand still works.
#
# Everything it creates is runtime-only and is undone by net/ap-down.sh:
#   * virtual interface $AP_IFACE on the radio of $WIFI_IFACE
#   * /run/NetworkManager/conf.d/zz-dschat.conf (NM ignores $AP_IFACE)
#   * nftables table "inet dschat" (input: only DHCP + relay port from the AP;
#     forward: nothing to/from the AP)
#   * two "dschat" ACCEPT rules at the top of iptables INPUT (so ufw's
#     default-deny lets DHCP + relay port through; ufw's saved config is
#     not modified)
#   * hostapd + a dedicated dnsmasq (DHCP only, no DNS), state in /run/dschat
# It never touches $UPSTREAM_IFACE, global sysctls, NAT, or Docker's chains.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$HERE")
CONF=${DSCHAT_AP_CONF:-$HERE/ap.conf}
STATE=/run/dschat
NM_CONF=/run/NetworkManager/conf.d/zz-dschat.conf

die()  { echo "ap-up: ERROR: $*" >&2; exit 1; }
info() { echo "ap-up: $*"; }

WATCH=1
WEP_KEY=""
WEP_FILE=""
# KEY=value reader (the file is never executed): last KEY= line wins; the
# value is trimmed and may be wrapped in "..." or '...'.
conf_value() {   # file key
    local line val=""
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line =~ ^[[:space:]]*# ]] && continue
        if [[ $line =~ ^[[:space:]]*$2[[:space:]]*=(.*)$ ]]; then
            val=${BASH_REMATCH[1]}
            val=${val#"${val%%[![:space:]]*}"}; val=${val%"${val##*[![:space:]]}"}
            if [[ $val =~ ^\"(.*)\"$ || $val =~ ^\'(.*)\'$ ]]; then val=${BASH_REMATCH[1]}; fi
        fi
    done < "$1"
    printf '%s' "$val"
}
ARGS=()
while (( $# )); do
    case $1 in
        --no-watch) WATCH=0 ;;
        --wep) [[ $# -ge 2 ]] || die "--wep needs a key"; WEP_KEY=$2; shift ;;
        --wep=*) WEP_KEY=${1#--wep=} ;;
        --wep-file) [[ $# -ge 2 ]] || die "--wep-file needs a file"; WEP_FILE=$2; shift ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done
if [[ -n $WEP_FILE ]]; then
    [[ -r $WEP_FILE ]] || die "cannot read $WEP_FILE"
    WEP_KEY=$(conf_value "$WEP_FILE" WEP_KEY)
    [[ -n $WEP_KEY ]] || die "no WEP_KEY=... in $WEP_FILE"
fi
# WEP key: 5/13 printable ASCII (no quotes/backslash) or 10/26 hex digits.
WEP_LINES=""
if [[ -n $WEP_KEY ]]; then
    if [[ $WEP_KEY =~ ^[0-9A-Fa-f]{10}$ || $WEP_KEY =~ ^[0-9A-Fa-f]{26}$ ]]; then
        WEP_LINES="wep_key0=$WEP_KEY"
    # (byte-wise check in the C locale: exactly 5 or 13 printable ASCII bytes)
    elif printf '%s' "$WEP_KEY" | LC_ALL=C grep -qxE '[ -~]{5}|[ -~]{13}' \
         && [[ $WEP_KEY != *\"* && $WEP_KEY != *\\* ]]; then
        WEP_LINES="wep_key0=\"$WEP_KEY\""
    else
        die "--wep key must be 5 or 13 ASCII characters, or 10 or 26 hex digits"
    fi
    WEP_LINES="wep_default_key=0
$WEP_LINES"
fi

[[ $EUID -eq 0 ]] || die "must run as root: sudo $0"
[[ -r $CONF ]] || die "config $CONF not found"
# shellcheck source=ap.conf
source "$CONF"
[[ ${#ARGS[@]} -ge 1 ]] && UPSTREAM_IFACE=${ARGS[0]}
# shellcheck source=iface.sh
source "$HERE/iface.sh"
command -v iw >/dev/null || die "'iw' not found"
dschat_resolve_ifaces || die "cannot choose the Wi-Fi interface (see above)"

for cmd in iw ip nft nmcli iptables dnsmasq python3; do
    command -v "$cmd" >/dev/null || die "'$cmd' not found"
done
command -v hostapd >/dev/null || die "hostapd not installed (sudo apt install hostapd)"

[[ -e $STATE/up ]] && die "AP already up (sudo net/ap-down.sh first)"
ip link show "$AP_IFACE" &>/dev/null && die "interface $AP_IFACE already exists (sudo net/ap-down.sh)"
[[ $AP_SSID =~ ^[A-Za-z0-9_-]{1,16}$ ]] || die "AP_SSID must be 1-16 chars [A-Za-z0-9_-]"
[[ -d /sys/class/net/$WIFI_IFACE/wireless || -d /sys/class/net/$WIFI_IFACE/phy80211 ]] \
    || die "$WIFI_IFACE is not a Wi-Fi interface"

# --- radio and channel ------------------------------------------------------
PHY=phy$(iw dev "$WIFI_IFACE" info | awk '/wiphy/ {print $2}')
[[ $PHY != phy ]] || die "cannot find the phy of $WIFI_IFACE"

if [[ $UPSTREAM_IFACE == "$WIFI_IFACE" ]]; then
    # Concurrent managed + AP: the card allows only one channel, so the AP
    # must sit on whatever channel the hotspot uses.
    read -r CHANNEL FREQ < <(iw dev "$WIFI_IFACE" info |
        awk '/channel/ {gsub(/[(]/, "", $3); print $2, $3; exit}') || true
    [[ -n ${FREQ:-} ]] || die "$WIFI_IFACE is not connected. Connect it to your (2.4 GHz) hotspot first."
    if (( FREQ > 2500 )); then
        die "$WIFI_IFACE is on channel $CHANNEL ($FREQ MHz, 5 GHz).
       The AP must share the hotspot's channel and the DS Lite only does 2.4 GHz.
       Switch the phone hotspot to 2.4 GHz, let $WIFI_IFACE reconnect, then retry."
    fi
    (( CHANNEL >= 1 && CHANNEL <= 13 )) || die "unsupported 2.4 GHz channel $CHANNEL"
    MODE="concurrent with $UPSTREAM_IFACE"
else
    CHANNEL=$AP_CHANNEL
    ip link show "$UPSTREAM_IFACE" &>/dev/null || die "upstream interface $UPSTREAM_IFACE not found"
    if iw dev "$WIFI_IFACE" info | grep -q 'channel'; then
        die "$WIFI_IFACE is still connected to a network but UPSTREAM_IFACE=$UPSTREAM_IFACE.
       Disconnect it first (nmcli device disconnect $WIFI_IFACE) or use it as upstream."
    fi
    MODE="AP only, upstream is $UPSTREAM_IFACE"
fi

# --- subnet must not collide with anything routed already ----------------------
python3 - "$AP_ADDR/$AP_PREFIX" <<'EOF' || die "subnet $AP_ADDR/$AP_PREFIX overlaps an existing network, change it in ap.conf"
import ipaddress, json, subprocess, sys
ours = ipaddress.ip_interface(sys.argv[1]).network
nets = []
for r in json.loads(subprocess.check_output(["ip", "-j", "-4", "route", "show"])):
    if r.get("dst") not in (None, "default"):
        nets.append((ipaddress.ip_network(r["dst"], strict=False), r.get("dev")))
for i in json.loads(subprocess.check_output(["ip", "-j", "-4", "addr", "show"])):
    for a in i.get("addr_info", []):
        nets.append((ipaddress.ip_interface(f"{a['local']}/{a['prefixlen']}").network, i["ifname"]))
bad = [(n, d) for n, d in nets if n.overlaps(ours) and not n.is_loopback]
for n, d in bad:
    print(f"  overlaps {n} on {d}", file=sys.stderr)
sys.exit(1 if bad else 0)
EOF
NETMASK=$(python3 -c "import ipaddress,sys; print(ipaddress.ip_interface(sys.argv[1]).netmask)" "$AP_ADDR/$AP_PREFIX")

# --- MAC allowlist -----------------------------------------------------------
ALLOW_FILE=$REPO/$MAC_ALLOWLIST
ALLOWED=()
if [[ -f $ALLOW_FILE ]]; then
    while read -r line; do
        line=${line%%#*}; line=${line//[[:space:]]/}
        [[ -z $line ]] && continue
        [[ $line =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || die "bad MAC '$line' in $ALLOW_FILE"
        ALLOWED+=("${line,,}")
    done < "$ALLOW_FILE"
fi

# --- from here on, undo everything if a step fails ------------------------------
mkdir -p "$STATE"
chmod 755 "$STATE"
echo "$CONF" > "$STATE/conf"
touch "$STATE/up"
rollback() {
    echo "ap-up: failed, rolling back" >&2
    [[ -s $STATE/hostapd.log ]] && { echo "--- hostapd.log (last 30 lines) ---" >&2; tail -n 30 "$STATE/hostapd.log" >&2; }
    "$HERE/ap-down.sh" --quiet || true
}
trap rollback ERR

info "radio $PHY, channel $CHANNEL ($MODE)"

# 1. Tell NetworkManager to ignore the AP interface before it exists.
mkdir -p "$(dirname "$NM_CONF")"
printf '# DSChat runtime config, removed by net/ap-down.sh\n[keyfile]\nunmanaged-devices+=interface-name:%s\n' \
    "$AP_IFACE" > "$NM_CONF"
nmcli general reload conf

# 2. Virtual AP interface with its own locally administered MAC.
BASE_MAC=$(cat "/sys/class/net/$WIFI_IFACE/address")
AP_MAC=$(python3 -c "
import sys
b = [int(x, 16) for x in sys.argv[1].split(':')]
b[0] |= 0x02
if sys.argv[1].lower() == ':'.join(f'{x:02x}' for x in b): b[5] ^= 0x01
print(':'.join(f'{x:02x}' for x in b))" "$BASE_MAC")
iw phy "$PHY" interface add "$AP_IFACE" type __ap
ip link set dev "$AP_IFACE" address "$AP_MAC"
nmcli device set "$AP_IFACE" managed no 2>/dev/null || true
sysctl -qw "net.ipv6.conf.$AP_IFACE.disable_ipv6=1"
ip addr add "$AP_ADDR/$AP_PREFIX" dev "$AP_IFACE"

# 3. Firewall: own nftables table (drops are final in nftables, so this
#    restricts the AP regardless of other tables) ...
nft -f - <<EOF
table inet dschat {
    chain input {
        type filter hook input priority filter - 10; policy accept;
        iifname "$AP_IFACE" ct state established,related accept
        iifname "$AP_IFACE" udp dport 67 accept
        iifname "$AP_IFACE" tcp dport $RELAY_PORT accept
        iifname "$AP_IFACE" counter drop
        # the AP address (relay, DHCP) only from the AP itself and this laptop:
        # never from the hotspot, Docker or any other interface
        ip daddr $AP_ADDR iifname != { "$AP_IFACE", "lo" } counter drop
    }
    chain forward {
        type filter hook forward priority filter - 10; policy accept;
        iifname "$AP_IFACE" counter drop
        oifname "$AP_IFACE" counter drop
    }
    # Keep the laptop's own multicast chatter (SSDP, mDNS, ...) off the DS's
    # 1-2 Mbps air; the DS needs none of it.
    chain output {
        type filter hook output priority filter - 10; policy accept;
        oifname "$AP_IFACE" ip daddr 224.0.0.0/4 counter drop
    }
}
EOF
#    ... plus ACCEPTs in iptables INPUT, because an accept in our table does
#    not override ufw's default-deny in the iptables table.
iptables -I INPUT 1 -i "$AP_IFACE" -p udp --dport 67 -m comment --comment dschat -j ACCEPT
iptables -I INPUT 1 -i "$AP_IFACE" -p tcp --dport "$RELAY_PORT" -m comment --comment dschat -j ACCEPT

# 4. hostapd. DS Lite: 802.11b only, basic rates 1+2 Mbps (it cannot receive
#    5.5/11 Mbps broadcasts), long preamble, no 11n/WMM, open, visible SSID.
ACL_LINES="macaddr_acl=0"
if (( ${#ALLOWED[@]} )); then
    printf '%s\n' "${ALLOWED[@]}" > "$STATE/accept_macs"
    ACL_LINES="macaddr_acl=1
accept_mac_file=$STATE/accept_macs"
fi
# 0600: it can hold the WEP key
( umask 077; : > "$STATE/hostapd.conf" )
cat > "$STATE/hostapd.conf" <<EOF
interface=$AP_IFACE
driver=nl80211
ctrl_interface=$STATE/hostapd-ctrl
ssid=$AP_SSID
utf8_ssid=0
ignore_broadcast_ssid=0
hw_mode=b
channel=$CHANNEL
ieee80211n=0
wmm_enabled=0
supported_rates=10 20 55 110
basic_rates=10 20
preamble=0
auth_algs=$([[ -n $WEP_KEY ]] && echo 3 || echo 1)
wpa=0
$WEP_LINES
beacon_int=100
dtim_period=1
max_num_sta=4
disassoc_low_ack=0
ap_isolate=1
$ACL_LINES
logger_stdout=-1
logger_stdout_level=1
EOF
HOSTAPD_ARGS=(-B -t -P "$STATE/hostapd.pid" -f "$STATE/hostapd.log")
[[ ${DEBUG:-0} == 1 ]] && HOSTAPD_ARGS+=(-dd)
: > "$STATE/hostapd.log"
hostapd "${HOSTAPD_ARGS[@]}" "$STATE/hostapd.conf"
for _ in $(seq 50); do
    grep -q 'AP-ENABLED' "$STATE/hostapd.log" && break
    grep -qE 'AP-DISABLED|Interface initialization failed|Could not' "$STATE/hostapd.log" && break
    sleep 0.2
done
grep -q 'AP-ENABLED' "$STATE/hostapd.log" || { echo "ap-up: hostapd did not enable the AP" >&2; false; }

# 5. dnsmasq: DHCP only (port=0 disables DNS), bound to the AP interface only.
#    The DNS-server option is REQUIRED although no DNS is served: DSWiFi only
#    leaves "Acquiring DHCP" once lwIP has an IPv4 address AND a DNS server
#    (dswifi source/arm9/access_point.c). Without option 6 the DS gets the
#    lease, answers ARP, and still times out. DNS queries to it are dropped.
DNSMASQ_USER=dnsmasq; getent passwd dnsmasq >/dev/null || DNSMASQ_USER=nobody
: > "$STATE/dnsmasq.log"; : > "$STATE/dnsmasq.leases"
chown "$DNSMASQ_USER" "$STATE/dnsmasq.log" "$STATE/dnsmasq.leases"
dnsmasq --conf-file=/dev/null \
    --user="$DNSMASQ_USER" \
    --pid-file="$STATE/dnsmasq.pid" \
    --interface="$AP_IFACE" --bind-interfaces --except-interface=lo \
    --port=0 --no-resolv --no-hosts \
    --dhcp-range="$DHCP_START,$DHCP_END,$NETMASK,$DHCP_LEASE" \
    --dhcp-option=option:router,"$AP_ADDR" \
    --dhcp-option=option:dns-server,"$AP_ADDR" \
    --dhcp-authoritative \
    --dhcp-leasefile="$STATE/dnsmasq.leases" \
    --log-dhcp --log-facility="$STATE/dnsmasq.log"
# dnsmasq makes its log 0660; make it readable for ap-watch / debugging
# without sudo (it holds only MACs, IPs and hostnames).
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ $(stat -c %a "$STATE/dnsmasq.log") == 644 ]] && break
    chmod 0644 "$STATE/dnsmasq.log"; sleep 0.2
done

trap - ERR

# What ./dschat status shows (no secrets in here).
cat > "$STATE/info" <<EOF
SSID=$AP_SSID
SECURITY=$([[ -n $WEP_KEY ]] && echo wep || echo open)
CHANNEL=$CHANNEL
AP_IFACE=$AP_IFACE
WIFI_IFACE=$WIFI_IFACE
UPSTREAM_IFACE=$UPSTREAM_IFACE
AP_ADDR=$AP_ADDR
RELAY_PORT=$RELAY_PORT
STARTED=$(date +%s)
EOF
chmod 644 "$STATE/info"

# --- summary -----------------------------------------------------------------
UP_STATE=$(nmcli -g GENERAL.STATE device show "$UPSTREAM_IFACE" 2>/dev/null || echo "?")
AP_NM=$(nmcli -g GENERAL.STATE device show "$AP_IFACE" 2>/dev/null || echo "?")
echo
info "AP is up"
echo "    SSID        $AP_SSID ($([[ -n $WEP_KEY ]] && echo "WEP, ${#WEP_KEY}-character key (not shown)" || echo open), 802.11b, channel $CHANNEL)"
echo "    interface   $AP_IFACE  MAC $AP_MAC  NM: $AP_NM"
echo "    address     $AP_ADDR/$AP_PREFIX, DHCP $DHCP_START-$DHCP_END"
echo "    relay port  $RELAY_PORT (only DHCP and this port are reachable from the AP)"
echo "    allowlist   ${#ALLOWED[@]} MAC(s)$( (( ${#ALLOWED[@]} )) || echo ' (empty: everyone allowed)')"
echo "    upstream    $UPSTREAM_IFACE: $UP_STATE"
echo "    logs        $STATE/hostapd.log  $STATE/dnsmasq.log"
echo
echo "    Start the relay:   relay/.venv/bin/python relay/relay.py"
echo "    Stop everything:   sudo net/ap-down.sh"
echo

if (( WATCH )); then
    exec "$HERE/ap-watch.sh"
fi
