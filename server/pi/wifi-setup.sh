#!/bin/bash
# wifi-setup.sh - connect wlan0 using plain wpa_supplicant + systemd-networkd,
# bypassing NetworkManager.
#
# Fallback for routers that refuse NetworkManager's connection attempts even
# though plain wpa_supplicant connects fine (seen with a Pi Zero 2W on a
# Vodafone WPA2/WPA3 hub). See server/PI-ZERO-2W-SETUP.md -> "WiFi won't connect".
#
# Run on the Pi:   sudo bash /boot/firmware/wifi-setup.sh
#
# Safe to re-run to change network. If the new settings don't produce an IP
# address within 60s, the previous working setup is restored automatically
# (on a first run: NetworkManager is re-enabled). Over SSH the session drops
# while WiFi reconnects; the script keeps running and logs to $LOG.
#
# To undo (go back to NetworkManager):
#   sudo systemctl disable --now wpa_supplicant@wlan0 systemd-networkd
#   sudo rm /etc/systemd/network/25-wlan0.network /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
#   sudo rm /etc/resolv.conf     # NetworkManager regenerates it
#   sudo systemctl enable --now wpa_supplicant NetworkManager
set -euo pipefail

WPA_CONF=/etc/wpa_supplicant/wpa_supplicant-wlan0.conf
WPA_PREV="$WPA_CONF.prev"
NET_CONF=/etc/systemd/network/25-wlan0.network
LOG=/var/log/wifi-setup.log
# Services that would fight systemd-networkd/wpa_supplicant@wlan0 over wlan0.
# wpa_supplicant.service is the D-Bus instance NetworkManager drives.
COMPETING="NetworkManager.service dhcpcd.service wpa_supplicant.service"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo:  sudo bash $0"
    exit 1
fi
if [ ! -x /lib/systemd/systemd-networkd ] && [ ! -x /usr/lib/systemd/systemd-networkd ]; then
    echo "Error: systemd-networkd is not installed on this OS."
    exit 1
fi
if ! systemctl cat wpa_supplicant@wlan0 >/dev/null 2>&1; then
    echo "Error: wpa_supplicant@.service not found (is wpasupplicant installed?)."
    exit 1
fi

# --- Credentials --------------------------------------------------------------
# Default the country to the regulatory domain Raspberry Pi Imager put on the
# kernel command line (cfg80211.ieee80211_regdom=XX), if present.
COUNTRY_DEFAULT="$(grep -o 'ieee80211_regdom=[A-Z][A-Z]' /proc/cmdline 2>/dev/null | cut -d= -f2 || true)"
COUNTRY_DEFAULT="${COUNTRY_DEFAULT:-GB}"

# IFS= keeps leading/trailing spaces, which are significant in SSIDs and passwords.
SSID=""
while [ -z "$SSID" ]; do
    IFS= read -r -p "WiFi name (SSID, case-sensitive): " SSID
done
read -r -p "WiFi country code [$COUNTRY_DEFAULT]: " COUNTRY
COUNTRY="${COUNTRY:-$COUNTRY_DEFAULT}"
COUNTRY="${COUNTRY^^}"
if ! [[ "$COUNTRY" =~ ^[A-Z]{2}$ ]]; then
    echo "Error: country must be a two-letter code such as GB or US."
    exit 1
fi
while true; do
    IFS= read -r -s -p "WiFi password: " PASS; echo
    IFS= read -r -s -p "Password again: " PASS2; echo
    [ "$PASS" = "$PASS2" ] && break
    echo "Passwords didn't match - try again."
done
if [ ${#PASS} -lt 8 ] || [ ${#PASS} -gt 63 ]; then
    echo "Error: WPA passwords are 8-63 characters (got ${#PASS})."
    exit 1
fi

# Hash the password (passed on stdin, not argv) and hex-encode the SSID, so
# quotes or odd bytes in either can't break the config file's syntax.
PSK="$(printf '%s\n' "$PASS" | wpa_passphrase "$SSID" | sed -n 's/^[[:space:]]*psk=//p')"
unset PASS PASS2
if ! [[ "$PSK" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Error: wpa_passphrase didn't produce a valid key."
    exit 1
fi
SSID_HEX="$(printf '%s' "$SSID" | od -An -v -tx1 | tr -d ' \n')"

# --- From here on, survive an SSH disconnect --------------------------------------
# Restarting WiFi drops an SSH session. Ignore the hangup and send command output
# to the log, so a vanished terminal can't make a write fail and abort the script.
trap '' HUP
: > "$LOG"
say() {
    printf '%s\n' "$*" >> "$LOG"
    printf '%s\n' "$*" 2>/dev/null || true
}
if [ -n "${SSH_CONNECTION:-}" ]; then
    say "Running over SSH: the session will drop while WiFi restarts. Progress is"
    say "logged to $LOG. Reconnect to the new IP (or the old one if it rolls back)."
fi

# --- Config files -----------------------------------------------------------------
FIRST_RUN=1
if [ -f "$WPA_CONF" ]; then
    FIRST_RUN=0
    cp -p "$WPA_CONF" "$WPA_PREV"
fi

say "Writing $WPA_CONF"
# umask 077: the file holds the hashed key (enough to join the network), so it
# must never exist world-readable, even briefly.
(
    umask 077
    cat > "$WPA_CONF" <<EOF
ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
country=$COUNTRY

network={
	ssid=$SSID_HEX
	psk=$PSK
}
EOF
)
chmod 600 "$WPA_CONF"

say "Writing $NET_CONF"
mkdir -p /etc/systemd/network
cat > "$NET_CONF" <<'EOF'
[Match]
Name=wlan0

[Network]
DHCP=yes
EOF

# --- Hand wlan0 over to wpa_supplicant@wlan0 + systemd-networkd ------------------
DISABLED=""
if [ "$FIRST_RUN" = 1 ]; then
    nmcli dev set wlan0 managed no >>"$LOG" 2>&1 || true
    for unit in $COMPETING; do
        if systemctl cat "$unit" >/dev/null 2>&1 && systemctl is-enabled --quiet "$unit" 2>/dev/null; then
            say "Disabling $unit"
            systemctl disable --now "$unit" >>"$LOG" 2>&1 || true
            DISABLED="$DISABLED $unit"
        fi
    done
    ip addr flush dev wlan0 >>"$LOG" 2>&1 || true
fi

say "Starting wpa_supplicant@wlan0 + systemd-networkd"
systemctl enable systemd-networkd wpa_supplicant@wlan0 >>"$LOG" 2>&1
systemctl restart wpa_supplicant@wlan0 >>"$LOG" 2>&1
systemctl restart systemd-networkd >>"$LOG" 2>&1

wait_for_ip() {
    local ip=""
    for _ in $(seq 1 30); do
        ip="$(ip -4 -o addr show wlan0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1)"
        [ -n "$ip" ] && break
        sleep 2
    done
    printf '%s' "$ip"
}

say "Waiting up to 60s for an IP address..."
IP="$(wait_for_ip)"

if [ -z "$IP" ]; then
    say "No IP address after 60s with the new settings."
    if [ "$FIRST_RUN" = 0 ]; then
        say "Restoring the previous WiFi config..."
        cp -p "$WPA_PREV" "$WPA_CONF"
        systemctl restart wpa_supplicant@wlan0 >>"$LOG" 2>&1 || true
        IP="$(wait_for_ip)"
        say "Previous config restored${IP:+ - reconnected as $IP}."
    else
        say "Handing WiFi back to the services that managed it before..."
        systemctl disable --now wpa_supplicant@wlan0 systemd-networkd >>"$LOG" 2>&1 || true
        for unit in $DISABLED; do
            systemctl enable --now "$unit" >>"$LOG" 2>&1 || true
        done
        say "Restored. The config files are left in place for diagnosis."
    fi
    say "Diagnose with:"
    say "  journalctl -b -u wpa_supplicant@wlan0 -u systemd-networkd | tail -30"
    exit 1
fi
rm -f "$WPA_PREV"

# --- DNS (NetworkManager no longer writes resolv.conf) ---------------------------
# Prefer the DNS servers DHCP handed out (from networkd's lease file), then the
# gateway, then a public resolver as a last resort.
IFINDEX="$(cat /sys/class/net/wlan0/ifindex)"
DHCP_DNS="$(sed -n 's/^DNS=//p' "/run/systemd/netif/leases/$IFINDEX" 2>/dev/null | head -n 1 || true)"
GW="$(ip -4 route show default dev wlan0 | awk '{print $3; exit}')"
rm -f /etc/resolv.conf
{
    for ns in $DHCP_DNS; do echo "nameserver $ns"; done
    if [ -z "$DHCP_DNS" ] && [ -n "$GW" ]; then echo "nameserver $GW"; fi
    echo "nameserver 1.1.1.1"
} > /etc/resolv.conf
say "DNS: ${DHCP_DNS:-${GW:-none from DHCP}} (+ 1.1.1.1 fallback)"

say ""
say "Connected: wlan0 = $IP (gateway ${GW:-unknown})"
say "From your PC:  ssh ${SUDO_USER:-<user>}@$IP"
say "Reboot once to confirm it reconnects on its own:  sudo reboot"
