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
# To undo (go back to NetworkManager):
#   sudo systemctl disable --now wpa_supplicant@wlan0 systemd-networkd
#   sudo rm /etc/systemd/network/25-wlan0.network /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
#   sudo rm /etc/resolv.conf     # NetworkManager regenerates it
#   sudo systemctl enable --now wpa_supplicant NetworkManager
set -euo pipefail

WPA_CONF=/etc/wpa_supplicant/wpa_supplicant-wlan0.conf
NET_CONF=/etc/systemd/network/25-wlan0.network

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

SSID=""
while [ -z "$SSID" ]; do
    read -r -p "WiFi name (SSID, case-sensitive): " SSID
done
read -r -p "WiFi country code [$COUNTRY_DEFAULT]: " COUNTRY
COUNTRY="${COUNTRY:-$COUNTRY_DEFAULT}"
COUNTRY="${COUNTRY^^}"
if ! [[ "$COUNTRY" =~ ^[A-Z]{2}$ ]]; then
    echo "Error: country must be a two-letter code such as GB or US."
    exit 1
fi
while true; do
    read -r -s -p "WiFi password: " PASS; echo
    read -r -s -p "Password again: " PASS2; echo
    [ "$PASS" = "$PASS2" ] && break
    echo "Passwords didn't match - try again."
done
if [ ${#PASS} -lt 8 ] || [ ${#PASS} -gt 63 ]; then
    echo "Error: WPA passwords are 8-63 characters (got ${#PASS})."
    exit 1
fi

# --- Config files -------------------------------------------------------------
echo "Writing $WPA_CONF"
{
    echo "ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev"
    echo "country=$COUNTRY"
    echo
    # Pass the password on stdin (not argv); drop comment lines, which include
    # wpa_passphrase's plaintext "#psk=" line. Only the hashed psk is stored.
    printf '%s\n' "$PASS" | wpa_passphrase "$SSID" | grep -v '^[[:space:]]*#'
} > "$WPA_CONF"
chmod 600 "$WPA_CONF"
unset PASS PASS2

echo "Writing $NET_CONF"
mkdir -p /etc/systemd/network
cat > "$NET_CONF" <<'EOF'
[Match]
Name=wlan0

[Network]
DHCP=yes
EOF

# --- Hand wlan0 over from NetworkManager ----------------------------------------
echo "Disabling NetworkManager"
nmcli dev set wlan0 managed no 2>/dev/null || true
systemctl disable --now NetworkManager
systemctl disable --now wpa_supplicant.service 2>/dev/null || true
ip addr flush dev wlan0 || true

echo "Starting wpa_supplicant@wlan0 + systemd-networkd"
systemctl enable systemd-networkd wpa_supplicant@wlan0
systemctl restart wpa_supplicant@wlan0
systemctl restart systemd-networkd

# --- Wait for an address --------------------------------------------------------
IP=""
echo -n "Waiting for an IP address"
for _ in $(seq 1 30); do
    IP="$(ip -4 -o addr show wlan0 | awk '{print $4}' | cut -d/ -f1 | head -n 1)"
    [ -n "$IP" ] && break
    echo -n "."
    sleep 2
done
echo

if [ -z "$IP" ]; then
    echo "No IP address after 60s. Diagnose with:"
    echo "  journalctl -b -u wpa_supplicant@wlan0 -u systemd-networkd | tail -30"
    exit 1
fi

# --- DNS (NetworkManager no longer writes resolv.conf) ---------------------------
GW="$(ip -4 route show default dev wlan0 | awk '{print $3; exit}')"
rm -f /etc/resolv.conf
{
    [ -n "$GW" ] && echo "nameserver $GW"
    echo "nameserver 1.1.1.1"
} > /etc/resolv.conf

echo
echo "Connected: wlan0 = $IP (gateway ${GW:-unknown})"
echo "From your PC:  ssh $(logname 2>/dev/null || echo pls)@$IP"
echo "Reboot once to confirm it reconnects on its own:  sudo reboot"
