// NetConfig - persistent (NVS) network configuration for the AtomS3-PoE
// DMX Director, plus a USB-serial config console to edit it.
//
// The board takes DMX over Ethernet, so its USB-CDC is free to host a
// config console: plug the Atom into a laptop, open a serial terminal
// (115200), and you get `show` / `dhcp` / `static` / `sacn` / `artnet` /
// `save` / `reboot`. This works regardless of network state - unlike a web
// UI you'd never reach on the wrong VLAN - which is what a multi-VLAN
// venue like EMF needs.
//
// Compiled (NVS + console body) only under NOCT_DMX_ETHERNET; on other
// builds the functions are no-op stubs so the shared src tree still links.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace dal {

struct NetConfig {
    bool     use_dhcp        = true;            // DHCP first; static below is the fallback / manual address
    uint8_t  ip[4]           = {2, 0, 0, 1};    // Art-Net 2.0.0.0/8 convention by default
    uint8_t  mask[4]         = {255, 0, 0, 0};
    uint8_t  gw[4]           = {2, 0, 0, 1};
    uint16_t sacn_universe   = 1;               // sACN universes are 1-based
    uint16_t artnet_universe = 0;               // matches QLC+/MagicQ "Universe 1 -> Art-Net 0"
    uint8_t  wifi_channel    = 1;               // ESP-NOW fleet channel (1 / 6 / 11)
};

// Load from NVS (namespace "noctnet"); returns the struct defaults for any
// key not yet stored. Save persists every field.
NetConfig net_config_load();
void      net_config_save(const NetConfig& cfg);

// Poll the USB-serial config console once. Call every loop. Reads a line
// of input if available, runs the command, prints status. No-op stub on
// non-Ethernet builds.
void net_console_poll();

}  // namespace dal
}  // namespace nocturnation
