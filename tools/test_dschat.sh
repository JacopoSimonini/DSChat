#!/usr/bin/env bash
# Offline test of ./dschat's hotspot watcher: sources the script and replaces
# everything that touches the system (iw, systemd, ap-up/down) with fakes that
# replay a scripted sequence of hotspot states. No root, nothing is changed.
#   tools/test_dschat.sh
# (stubs override the script's functions; checks are eval'd strings)
# shellcheck disable=SC2016,SC2034,SC2317
set -uo pipefail
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=../dschat
source "$REPO_DIR/dschat"
set +e
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
CTL=$TMP/ctl; STATE=$TMP/state; mkdir -p "$CTL" "$STATE"
CONF=$TMP/dschat.conf; printf 'SECURITY=wep\nWEP_KEY="abc#defghij k"\n' > "$CONF"; chmod 600 "$CONF"
SUDO_USER=$(id -un); POLL=0; HOTSPOT_GRACE=2
UPSTREAM_IFACE=wlan0; WIFI_IFACE=wlan0
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

# --- fakes ---------------------------------------------------------------
STEPS=(); step=0; EVENTS=$TMP/events; : > "$EVENTS"
hotspot_channel() { local s=${STEPS[step]:-}; [[ $s == none ]] || echo "$s"; }
ap_up_fake() { printf 'SSID=DSCHAT\nSECURITY=wep\nCHANNEL=%s\n' "${1%% *}" > "$STATE/info"; touch "$STATE/up"; ALIVE=1; }
start_ap() { ap_args; echo "start ${AP_ARGS[*]} ch=$(hotspot_channel | cut -d' ' -f1)" >> "$EVENTS"; ap_up_fake "$(hotspot_channel)"; }
stop_ap() { echo "stop" >> "$EVENTS"; rm -f "$STATE/up" "$STATE/info"; ALIVE=0; }
hostapd_alive() { (( ALIVE )); }
wait_relay() { return 0; }
date() { if [[ ${1:-} == +%s ]]; then echo "$((1000 + step))"; else command date "$@"; fi; }
sleep() { step=$((step + 1)); (( step < ${#STEPS[@]} )); }
run() { STEPS=("$@"); step=0; : > "$EVENTS"; : > "$CTL/last_error"; ap_up_fake "${STEPS[0]}"; cmd_watch > "$TMP/out" 2>&1; }

# --- scenarios -----------------------------------------------------------
load_security; check "WEP key with # and space read from the conf file" '[[ $SECURITY == wep && $WEP_LEN == 13 ]]'
run "11 2462" "11 2462" "11 2462"
check "steady hotspot: nothing happens" '[[ ! -s $EVENTS ]]'
run "11 2462" "11 2462" "6 2437" "6 2437" "6 2437"
check "channel 11 -> 6: AP stopped and restarted on 6" '[[ $(paste -sd, "$EVENTS") == "stop,start --no-watch --wep-file $CONF ch=6" ]]'
check "  ... and reported" 'grep -q "moved from channel 11 to 6" "$CTL/last_error" || grep -q "moved from channel 11 to 6" "$TMP/out"'
run "11 2462" "36 5180" "36 5180" "36 5180" "1 2412" "1 2412"
check "5 GHz: AP stopped, waits, back on 2.4 GHz ch 1" '[[ $(paste -sd, "$EVENTS") == "stop,start --no-watch --wep-file $CONF ch=1" ]]'
check "  ... 5 GHz reported" 'grep -q "5 GHz" "$TMP/out"'
run "11 2462" none none none none none "11 2462" "11 2462"
check "hotspot gone > grace: AP steps aside, back when it returns" '[[ $(paste -sd, "$EVENTS") == "stop,start --no-watch --wep-file $CONF ch=11" ]]'
run "11 2462" none "11 2462" "11 2462"
check "short hotspot blip (< grace): AP left alone" '[[ ! -s $EVENTS ]]'
run "11 2462" "11 2462" "11 2462"; 
ALIVE=0; STEPS=("11 2462" "11 2462" "11 2462"); step=0; : > "$EVENTS"; ap_up_fake "11 2462"; ALIVE=0; cmd_watch > "$TMP/out" 2>&1
check "hostapd died: AP restarted" '[[ $(paste -sd, "$EVENTS") == stop,start* ]] && grep -q "hostapd stopped" "$TMP/out"'
STEPS=("11 2462" "11 2462" "11 2462"); step=0; : > "$EVENTS"; rm -f "$STATE/up"; ALIVE=0; cmd_watch > "$TMP/out" 2>&1
check "AP removed by hand (ap-down.sh): reported once, not restarted" '[[ ! -s $EVENTS ]] && [[ $(grep -c "not stopped by" "$TMP/out") == 1 ]]'
printf 'SECURITY=open\n' > "$CONF"; run "11 2462" "6 2437" "6 2437"
check "open mode: no --wep-file" '[[ $(tail -1 "$EVENTS") == "start --no-watch ch=6" ]]'
chmod 644 "$CONF"; ( load_security ) 2>"$TMP/err"; rc=$?
check "group/world-readable conf refused" '(( rc != 0 )) && grep -q "chmod 600" "$TMP/err"'
chmod 600 "$CONF"; printf 'SECURITY=wep\nWEP_KEY=\n' > "$CONF"; ( load_security ) 2>"$TMP/err"; rc=$?
check "WEP without key refused" '(( rc != 0 )) && grep -q "WEP_KEY is empty" "$TMP/err"'
printf 'SECURITY=wpa\n' > "$CONF"; ( load_security ) 2>"$TMP/err"; rc=$?
check "unknown mode refused" '(( rc != 0 ))'
# --- interface auto-detection (net/iface.sh) with a fake iw / ip ----------
IW_DEV=""; IW_CONNECTED=""; DEFROUTE=""
iw() {
    if [[ $1 == dev && $# == 1 ]]; then printf '%b' "$IW_DEV"; return; fi
    if [[ $1 == dev && $3 == info ]]; then [[ " $IW_CONNECTED " == *" $2 "* ]] && echo "	channel 6 (2437 MHz)"; return 0; fi
}
ip() { [[ -n $DEFROUTE ]] && echo "default via 192.0.2.1 dev $DEFROUTE proto dhcp metric 600"; return 0; }
resolve() { WIFI_IFACE=$1; UPSTREAM_IFACE=$2; dschat_resolve_ifaces 2>"$TMP/err"; }
IW_DEV="phy#0\n\tInterface wlan0\n\t\ttype managed\n"; DEFROUTE=wlan0
resolve auto auto; check "one Wi-Fi interface, default route on it" '[[ $WIFI_IFACE == wlan0 && $UPSTREAM_IFACE == wlan0 ]]'
IW_DEV="phy#0\n\tInterface wlan0\n\t\ttype managed\n\tInterface dsap0\n\t\ttype AP\n"
resolve auto auto; check "the AP interface is never chosen" '[[ $WIFI_IFACE == wlan0 ]]'
IW_DEV="phy#1\n\tInterface wlx1\n\t\ttype managed\nphy#0\n\tInterface wlan0\n\t\ttype managed\n"; IW_CONNECTED=wlan0
resolve auto auto; check "two interfaces: the connected one wins" '[[ $WIFI_IFACE == wlan0 ]]'
IW_CONNECTED="wlan0 wlx1"
resolve auto auto; rc=$?; check "two connected: asks for WIFI_IFACE" '(( rc != 0 )) && grep -q "set WIFI_IFACE" "$TMP/err"'
resolve wlx1 auto; check "explicit WIFI_IFACE overrides" '[[ $WIFI_IFACE == wlx1 ]]'
IW_DEV=""
resolve auto auto; rc=$?; check "no Wi-Fi interface: clear error" '(( rc != 0 )) && grep -q "no Wi-Fi interface" "$TMP/err"'
IW_DEV="phy#0\n\tInterface wlan0\n\t\ttype managed\n"; DEFROUTE=usb0
resolve auto auto; check "USB tethering: upstream = default route" '[[ $WIFI_IFACE == wlan0 && $UPSTREAM_IFACE == usb0 ]]'
DEFROUTE=""
resolve auto auto; check "no default route: upstream = the Wi-Fi interface" '[[ $UPSTREAM_IFACE == wlan0 ]]'
unset -f iw ip

echo "$fails failed"
exit $(( fails > 0 ))
