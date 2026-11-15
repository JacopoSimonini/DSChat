# shellcheck shell=bash
# DSChat: resolve WIFI_IFACE / UPSTREAM_IFACE from net/ap.conf ("auto" = detect).
# Sourced by net/ap-up.sh, net/ap-down.sh and ./dschat after net/ap.conf.
#
#   WIFI_IFACE=auto      the Wi-Fi interface (type managed) whose radio will also
#                        host the AP. If several exist, the one connected to a
#                        network wins; if still ambiguous, set it in ap.conf.
#   UPSTREAM_IFACE=auto  the interface of the IPv4 default route (your
#                        internet); without one, the Wi-Fi interface.
# Set either to an interface name in net/ap.conf to override the detection.

# Wi-Fi interfaces of type managed, one per line (never the AP interface).
dschat_wifi_ifaces() {
    iw dev 2>/dev/null | awk -v ap="${AP_IFACE:-dsap0}" '
        $1 == "Interface" { name = $2 }
        $1 == "type" && $2 == "managed" && name != ap { print name }'
}

# 0 if the interface is associated to a network (iw reports a channel).
dschat_iface_connected() {
    iw dev "$1" info 2>/dev/null | grep -q 'channel'
}

dschat_resolve_ifaces() {   # sets WIFI_IFACE, UPSTREAM_IFACE; returns 1 + message on failure
    local list n c
    if [[ -z ${WIFI_IFACE:-} || $WIFI_IFACE == auto ]]; then
        mapfile -t list < <(dschat_wifi_ifaces)
        n=${#list[@]}
        if (( n == 0 )); then
            echo "no Wi-Fi interface found (iw dev); set WIFI_IFACE in net/ap.conf" >&2
            return 1
        elif (( n == 1 )); then
            WIFI_IFACE=${list[0]}
        else
            WIFI_IFACE=""
            for c in "${list[@]}"; do
                if dschat_iface_connected "$c"; then
                    [[ -n $WIFI_IFACE ]] && { WIFI_IFACE=""; break; }
                    WIFI_IFACE=$c
                fi
            done
            if [[ -z $WIFI_IFACE ]]; then
                echo "several Wi-Fi interfaces (${list[*]}); set WIFI_IFACE in net/ap.conf" >&2
                return 1
            fi
        fi
    fi
    if [[ -z ${UPSTREAM_IFACE:-} || $UPSTREAM_IFACE == auto ]]; then
        UPSTREAM_IFACE=$(ip -4 route show default 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')
        [[ -n $UPSTREAM_IFACE ]] || UPSTREAM_IFACE=$WIFI_IFACE
    fi
    return 0
}
