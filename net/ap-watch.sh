#!/usr/bin/env bash
# DSChat: follow the AP logs and print station / DHCP events.
# Started automatically by ap-up.sh; Ctrl-C stops watching, the AP stays up.
# Can be run again any time (no root needed if /run/dschat is readable).

STATE=/run/dschat
[[ -e $STATE/hostapd.log ]] || { echo "ap-watch: AP is not up" >&2; exit 1; }

echo "ap-watch: watching for stations (Ctrl-C stops watching; the AP stays up)"
tail -n 0 -F "$STATE/hostapd.log" "$STATE/dnsmasq.log" 2>/dev/null |
    grep --line-buffered -E 'AP-STA-|STA [0-9a-f:]{17}|ACL|DHCP(DISCOVER|OFFER|REQUEST|ACK|NAK|DECLINE|RELEASE|INFORM)|not allowed|deauth|disassoc' |
    while IFS= read -r line; do
        case $line in
            *AP-STA-CONNECTED*)
                mac=$(grep -oE '([0-9a-f]{2}:){5}[0-9a-f]{2}' <<<"$line" | tail -1)
                echo "$(date +%T)  >>> STATION ASSOCIATED: $mac" ;;
            *AP-STA-DISCONNECTED*)
                mac=$(grep -oE '([0-9a-f]{2}:){5}[0-9a-f]{2}' <<<"$line" | tail -1)
                echo "$(date +%T)  <<< station left: $mac" ;;
            *DHCPNAK*|*DHCPDECLINE*)
                echo "$(date +%T)  !!! ${line#*dnsmasq-dhcp*: }" ;;
            *DHCP*)
                # keep the message type (DHCPACK etc.) visible; drop syslog prefix
                echo "$(date +%T)  ${line#*dnsmasq-dhcp*: }" ;;
            *)
                echo "$(date +%T)  $line" ;;
        esac
    done
