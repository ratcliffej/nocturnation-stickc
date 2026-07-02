# AtomS3-PoE Ethernet DMX Director

A dedicated, headless NocturNation **Director** that takes DMX from a lighting
console over **wired Ethernet** (sACN / Art-Net) and broadcasts to the fleet
over **ESP-NOW**. Plug it into the same switch as the desk and go.

```
MagicQ / QLC+ ──┐
                switch ──PoE cable── AtomS3 + Atomic PoE (W5500)
                                       │  sACN/Art-Net over Ethernet
                                       │  → DmxChannelMapper (shared)
                                       └─ ESP-NOW broadcast → Lumes / badges
```

## Why this board

Receiving network DMX **and** running ESP-NOW would normally fight over the one
2.4 GHz radio (they'd have to be channel-coordinated). Here the console link is
**W5500 Ethernet over SPI** — a separate peripheral — so the WiFi radio stays
dedicated to ESP-NOW, exactly as on the Sticks. No coexistence problem, and the
DMX→mapper→broadcast back-half is the same tested code the Sticks use.

## Hardware

| Item | Notes |
|---|---|
| **M5 AtomS3 Lite** | ESP32-S3. One RGB status LED, one button (button unused), no screen. |
| **M5 Atomic PoE Base** | W5500 SPI Ethernet + PoE. One cable for power + data. |

### Pins (confirmed against M5Stack docs)

| Signal | Where | Value |
|---|---|---|
| W5500 SCK / MISO / MOSI / CS | `src/dal/drivers/ethernet_dmx_adapter.cpp` | `5 / 7 / 8 / 6` — RST/INT not broken out on the base |
| Status-LED GPIO | `src/modes/dmx_bridge_mode.cpp` (`kStatusLedPin`) | `35` — AtomS3 Lite onboard WS2812 |
| External IR TX | `src/hal_atoms3poe/ir_tx_atoms3poe.h` (`kIRPin`) | `2` — Grove G2; the PoE base covers the built-in IR LED |

## Build & flash

```bash
pio run -e m5stack-atoms3-poe -t upload
```

The env already sets `upload_speed = 115200` + `upload_flags = --no-stub`:
the S3's native USB-Serial-JTAG can't complete a normal stub upload (it
desyncs with "No serial data received"), so we flash via the ROM loader.
If a fresh board won't enter the bootloader, hold the button while plugging in
USB.

Key build flags (in `platformio.ini`):
- `NOCT_DMX_ETHERNET` — swaps DmxBridge's USB-CDC DMX source for the Ethernet
  sACN/Art-Net adapter, and lights up the config console.
- `NOCT_HEADLESS_DMX_BRIDGE` — boot straight into DMX Bridge (no menu).
- `ARDUINO_USB_MODE=0` — **TinyUSB** CDC, not the hardware USB-Serial-JTAG, so
  connecting a laptop to the serial console can't hardware-reset the chip.

## Status LED

Onboard RGB, updated continuously:

| Colour | Meaning | Check |
|---|---|---|
| 🔴 red | Completely broken | W5500 not detected, or ESP-NOW radio didn't start |
| 🟣 purple | Networking fault | W5500 OK but no Ethernet link (cable / switch) |
| 🟠 amber | Up, but no data | Link + radio fine; no DMX (console stopped, wrong universe, nothing patched) |
| 🟢 green | All good | Link up and DMX streaming |

## Serial config console (USB-C)

DMX comes over Ethernet, so the USB-C port is free for config. Plug into any
laptop, open a serial terminal at **115200**. It's safe to connect mid-show:
TinyUSB means the open can't reset the chip, and the console does nothing unless
a host is connected and never blocks the broadcast.

| Command | Effect |
|---|---|
| `show` | One-shot status + the 27 broadcast-block DMX channels (role + bar) |
| `mon` | Live status (~1 Hz, in-place dashboard); press a key to stop |
| `channel <1\|6\|11>` | Set ESP-NOW fleet channel — **applies live and saves now** |
| `dhcp` | Use DHCP *(save + reboot to apply)* |
| `static <ip> <mask> <gw>` | Static address *(save + reboot)* |
| `sacn <n>` | sACN universe *(save + reboot)* |
| `artnet <n>` | Art-Net universe *(save + reboot)* |
| `netpass <pw\|off>` | Set / disable the **network console** password |
| `save` | Persist config to flash (NVS) |
| `reboot` | Restart |

The broadcast block shows **ch1–27**: the 23 FX channels plus the raw-RGB
direct-control set (**ch24 R / ch25 G / ch26 B / ch27 Raw Enable** — enable high
emits a static wash straight from ch24–26, bypassing the FX engine).

Config lives in NVS (namespace `noctnet`) and survives reboots and reflashes.

## Network console (TCP 2323)

The same console is also reachable over the wire, so you can monitor and
reconfigure the Director from anywhere on the network without walking a laptop to
it. It is **off until you set a password** from the USB console:

```
netpass swordfish          # set it (stored in NVS)
netpass off                # disable the network console again
```

Then connect from any machine on the segment:

```
nc 10.42.1.234 2323        # type the password at the prompt
```

You get the identical `show` / `mon` / `channel` / config console.

**Security:** it's a plaintext password over **unencrypted** TCP — it stops
scanners and casual/accidental access, but anyone actively sniffing that segment
could capture it. **Run the Director on a trusted / control VLAN.** One session at
a time; a wrong password disconnects and briefly locks out; an idle session drops
after 5 minutes. Connecting/disconnecting never blips the broadcast.

### Defaults

- **Network:** DHCP (falls back to static `2.0.0.x / 255.0.0.0` if no DHCP).
- **sACN:** universe 1 (multicast `239.255.0.1`, UDP 5568).
- **Art-Net:** universe 0 (UDP 6454) — matches QLC+/MagicQ "Universe 1 → Art-Net 0".
- **ESP-NOW channel:** 1.

The device listens for **both** sACN and Art-Net at once; drive it with whichever
the console is set to.

## VLANs (EMF / multi-VLAN venues)

The device does **not** 802.1Q-tag its own traffic — the switch port handles the
VLAN (set it to your lighting VLAN as access/untagged). The device just needs a
valid IP on that VLAN: DHCP if the VLAN serves it, otherwise `static …`.

## Bring-up checklist

1. Flash: `pio run -e m5stack-atoms3-poe -t upload`. The onboard LED should
   light on boot (red/purple while there's no link) — if it stays dark, the
   LED GPIO is wrong (try 38).
2. USB into a laptop, serial @ 115200 → `show` → confirm `w5500: OK`.
3. Set `channel`, `sacn`/`artnet` universe, network (`dhcp` or `static …`) →
   `save` → `reboot`.
4. Patch into the switch. LED → green when DMX flows.

## Layout

- `src/hal_atoms3poe/` — HAL backend (caps `{ESPNow, DmxInput, IRTx}`; everything
  else is `nullptr`, so only DMX Bridge mode is reachable). IRTx drives an
  external IR blaster on Grove G2 so the Director's local PixMob loopback still
  works with the PoE base covering the built-in emitter.
- `src/dal/drivers/ethernet_dmx_adapter.{h,cpp}` — W5500 + sACN/Art-Net → the
  shared `DmxInputParser` (re-framed as Enttec Pro, so downstream is identical).
- `src/dal/drivers/net_config.{h,cpp}` — NVS config + the serial console.
- DMX source + status LED + channel selection are wired into the shared
  `src/modes/dmx_bridge_mode.cpp` behind `NOCT_DMX_ETHERNET` (inert for Sticks).
