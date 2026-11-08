# Setting up a Raspberry Pi Zero 2W from scratch

This guide takes a blank microSD card to a Pi Zero 2W running the photo-frame
server in Docker. Steps 1–2 happen on your PC; everything after that happens
on the Pi (over SSH, or on a monitor + keyboard if WiFi needs fixing).

> Already have a Linux host with Docker? Skip to the Docker steps in the
> [README](../README.md#server-raspberry-pi-or-any-linux-machine). Moving an
> old tarball install? See [MIGRATION.md](MIGRATION.md).

## What you need

- Raspberry Pi Zero 2W and a microSD card (16 GB+, A1/A2 rated).
- A **5 V / 2.5 A** power supply plugged into the port labelled **PWR**, not
  the one labelled USB. A weak supply causes WiFi drop-outs and random resets.
- A **2.4 GHz** WiFi network. The Zero 2W has no 5 GHz radio, and it cannot
  join a WPA3-only network. WPA2 works; WPA2/WPA3 mixed should work, but one
  such router needed the [WiFi fallback](#when-the-router-refuses-networkmanager).
- [Raspberry Pi Imager](https://www.raspberrypi.com/software/) **2.0 or newer**.
  Older Imager versions can't apply settings to current Raspberry Pi OS images,
  so the WiFi, SSH and hostname you enter are silently ignored.
- Optional but strongly recommended for the first install: a mini-HDMI adapter
  plus monitor, and a USB OTG adapter plus keyboard. If WiFi doesn't come up,
  they're the only way in.

## 1. Flash the SD card

1. In Imager, choose **Raspberry Pi Zero 2 W**, then **Raspberry Pi OS Lite (64-bit)**.
2. Open the OS customisation settings and set:
   - **Hostname**, e.g. `photoframe`
   - **Username and password**
   - **WiFi** SSID and password, plus your **WiFi country**. The network name
     and password are case-sensitive.
   - **Locale / timezone**
   - **Services → Enable SSH**. Adding your PC's public key is better than a password.
3. Write the card. When Imager asks whether to apply the customisation, answer **Yes**.

**Why 64-bit:** Docker has said v28 is the last major version for 32-bit
Raspberry Pi OS, so a 32-bit install is stuck on an old Docker release. The
server image is still published for `linux/arm/v7`, but the arm64 build is the
main tested path. 64-bit uses somewhat more RAM, and the server fits
comfortably in the Zero 2W's 512 MB.

## 2. Before first boot (card still in your PC)

The card's boot partition shows up as a drive called **`bootfs`**. On the Pi
it is mounted at `/boot/firmware`, so anything you copy there now can be run
on the Pi before it has a network connection.

1. **Copy the helper scripts** from this repo's [`server/pi/`](pi/) folder to
   the `bootfs` drive:
   - `wifi-setup.sh`: the WiFi fallback (see [WiFi won't connect](#wifi-wont-connect))
   - `set-resolution.sh`: forces a readable console resolution
2. **If you'll use a monitor above 1080p**, such as a 4K screen: the Zero 2W
   can only output up to 1080p, and the console comes out garbled or cut off.
   Open `cmdline.txt` on `bootfs` in a text editor and add this to the **end of
   the existing line**, with a space before it and **no new line**:
   ```
   video=HDMI-A-1:1280x720@60
   ```
   Use `1920x1080@60` for 1080p. Once the Pi is running, change it with
   `set-resolution.sh` instead of editing the file by hand.

Eject the drive safely before removing the card.

## 3. First boot

Put the card in the Pi and power it on. The first boot applies your settings,
expands the filesystem and reboots by itself. **Allow about 5 minutes.**

Then find the Pi from your PC:

```bash
ping photoframe.local          # mDNS; often fails on Windows even when the Pi is fine
ping photoframe                # or photoframe.<router-domain>, via your router's DNS
```

Your router's connected-devices page also lists it by hostname. Then connect:

```bash
ssh <user>@<pi-ip>
```

If the Pi never appears on your network, connect the monitor and keyboard,
log in, and work through [WiFi won't connect](#wifi-wont-connect).

## 4. Reserve its IP address

On your router, create a **DHCP reservation** for the Pi. The frame firmware
has no mDNS support, so its `server_url` must be a fixed IP address.

## 5. Optional: SSH key login

From your PC (PowerShell shown; on Linux or macOS use `ssh-copy-id <user>@<pi-ip>`):

```powershell
type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh <user>@<pi-ip> "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
```

## 6. Update the OS

```bash
sudo apt update && sudo apt full-upgrade -y     # 10–20 minutes on a Zero 2W
sudo apt install -y git
sudo reboot
```

## 7. Install Docker

```bash
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER
sudo systemctl enable docker      # the container only restarts on boot if Docker does
exit                              # log back in so the docker group applies
```

After logging back in:

```bash
docker compose version            # must be v2.24+
```

## 8. Install and start the server

```bash
git clone https://github.com/lukesmithuk/esp32-simple-picture-frame.git
cd esp32-simple-picture-frame/server
cp .env.example .env && chmod 600 .env
openssl rand -hex 24              # use this as PHOTOFRAME_API_KEY
nano .env
mkdir -p data
docker pull ghcr.io/lukesmithuk/esp32-simple-picture-frame:latest
docker compose up -d
docker compose ps                 # wait for "healthy" (~20 s; the first pull takes minutes)
```

Create `data/` **yourself** before the first `up`. Otherwise Docker creates it
owned by root, and `update.sh` can't write its database backups there. If
that has already happened, run `sudo chown $USER: data`.

Pull the image **before** running `up`. `compose.yaml` also has `build: .`, so
if Compose's own pull fails it quietly falls back to building the image on the
Pi, which is very slow on a Zero 2W.

Open `http://<pi-ip>:8080` to upload photos. For updates, battery-alert email
and backups, see the [README](../README.md#server-raspberry-pi-or-any-linux-machine).

## 9. Point the frame at it

In the frame's `/sdcard/config.txt`:

```
server_url=http://<pi-ip>:8080
server_api_key=<PHOTOFRAME_API_KEY from .env>
```

---

## Troubleshooting

### Console unreadable on a high-resolution monitor

Symptoms include only part of each screen of output showing, or text that is
cut off or garbled. Force a lower resolution:

```bash
sudo bash /boot/firmware/set-resolution.sh 720     # or 1080, or auto to remove the override
sudo reboot
```

The script keeps `cmdline.txt` on one line (even if it was edited on Windows),
replaces any earlier `video=` setting, and saves a backup as `cmdline.txt.bak`.
Until it's fixed, adding `| tail -15` to the end of commands keeps their output
on the visible part of the screen.

### WiFi won't connect

On the Pi's console:

```bash
nmcli dev status                  # wlan0 state
rfkill list                       # "Soft blocked: yes" under Wireless LAN?
vcgencmd get_throttled            # 0x0 = power OK
journalctl -b -u NetworkManager | grep -iE "wlan0|reason" | tail -25
```

| You see | Cause and fix |
|---|---|
| `Soft blocked: yes` | WiFi country not set: run `sudo raspi-config` → Localisation Options → WLAN Country. |
| `get_throttled` isn't `0x0` | Under-voltage: use a better supply and cable in the **PWR** port. |
| `4-Way Handshake failed` | Wrong password. A keyboard-layout mismatch often changes `@ " # \|`; check with `echo 'password'`. |
| `ssid-not-found` | Name typo (case-sensitive), a 5 GHz-only network, or a weak signal. |
| `associating -> disconnected` within milliseconds, then `association took too long`, `asking for new secrets`, `no-secrets` | **The router is refusing NetworkManager's connection attempt.** The "password is required" messages are misleading: NetworkManager assumes a password problem after any failure. See below. |

#### When the router refuses NetworkManager

Two tests confirm it:

1. **Phone hotspot.** Set up a 2.4 GHz WPA2 hotspot with a simple password, then
   run `sudo nmcli --ask dev wifi connect "HotspotName"`. If this connects, the
   Pi's hardware, power and OS are fine.
2. **Plain wpa_supplicant.** This bypasses NetworkManager:
   ```bash
   sudo nmcli dev set wlan0 managed no
   wpa_passphrase "YourSSID" | sudo tee /tmp/w.conf > /dev/null     # type the password, then Enter
   sudo wpa_supplicant -i wlan0 -c /tmp/w.conf | grep -iE "assoc|auth|reason|status|CONNECTED"
   # wait ~30 s, then Ctrl+C (it keeps running to hold the connection)
   sudo rm /tmp/w.conf
   sudo nmcli dev set wlan0 managed yes
   ```

If this connects (`CTRL-EVENT-CONNECTED`) where NetworkManager doesn't, use
**`wifi-setup.sh`**. It switches the Pi to plain `wpa_supplicant` with
`systemd-networkd` handling the IP address, and disables NetworkManager:

```bash
sudo bash /boot/firmware/wifi-setup.sh
```

It asks for the network name, country and password. Only a hashed form of the
password is stored, and it is never written to your shell history. It waits
for an IP address, sets up DNS, and prints the IP and the `ssh` command to use.
Reboot once to confirm it reconnects by itself. Instructions to undo it are at
the top of the script.

Once it's in place, `nmcli` no longer manages WiFi. To change network, run the
script again. If the new settings don't produce an IP address within 60
seconds, it restores the previous working setup automatically. On a first run,
that means handing WiFi back to NetworkManager. Running it over SSH is safe:
the session drops while WiFi restarts, but the script keeps going and logs
progress to `/var/log/wifi-setup.log`.

These NetworkManager changes were tried on a Vodafone WPA2/WPA3 hub and did
**not** help, so they're probably not worth repeating: forcing WPA2
(`key-mgmt wpa-psk`), setting PMF to `optional` or `disable`, locking to a
BSSID, a random MAC address, the `brcmfmac feature_disable=0x82000` driver
option, rebooting the router, and switching OS version (Trixie 64-bit and
Bookworm 32-bit behaved the same).
