// EthernetDmxAdapter - receives sACN (E1.31) and Art-Net DMX over wired
// Ethernet (W5500 on the M5 Atomic PoE base) and presents it to DmxBridge
// mode through the SAME surface as DmxUsbCdcAdapter.
//
// Epic 7 (AtomS3-PoE Director). The Sticks take DMX as Enttec Pro framing
// over USB-CDC; this board takes it as sACN/Art-Net UDP over Ethernet.
// Rather than fork DmxBridge mode, this adapter re-frames each received
// universe as a single Enttec "Output Only Send DMX" packet and feeds it
// into the very same DmxInputParser the USB adapter uses. Everything
// downstream (parser -> DmxChannelMapper -> DAL render -> ESP-NOW) is then
// byte-for-byte identical to the Stick path.
//
// Listens for BOTH protocols at once (Art-Net on UDP 6454, sACN on UDP
// 5568 multicast) so the operator can drive it with whichever MagicQ is
// configured for - "we'll see what works". Latest frame on either wins.
//
// Arduino + Ethernet-only: the implementation is compiled only when
// NOCT_DMX_ETHERNET is defined (the m5stack-atoms3-poe env). On every
// other env the .cpp collapses to nothing, so it never drags Ethernet.h
// into the Stick or native builds even though build_src_filter globs all
// of src/dal/.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dmx_input_parser.h"

namespace nocturnation {
namespace dal {

class EthernetDmxAdapter {
public:
    // Kept for signature parity with DmxUsbCdcAdapter (DmxBridge passes a
    // baud); meaningless over Ethernet and ignored.
    static constexpr uint32_t kDefaultBaud = 921600;

    // Network DMX defaults. Art-Net's universe field is a 15-bit
    // Net<<8|SubUni; QLC+/MagicQ's "Universe 1 -> Art-Net 0" maps to 0.
    // sACN universes are 1-based (0 is reserved), so default 1; its
    // multicast group is 239.255.<hi>.<lo> of the universe number.
    static constexpr uint16_t kDefaultArtnetUniverse = 0;
    static constexpr uint16_t kDefaultSacnUniverse   = 1;

    EthernetDmxAdapter();

    // Bring up the W5500 link + open the Art-Net and sACN sockets, and
    // reset the parser. `baud` is ignored. Idempotent.
    void begin(uint32_t baud = kDefaultBaud);

    // Close the sockets. Idempotent. (The link itself is left up.)
    void end();

    // Drain pending UDP on both sockets, re-frame any DMX universes into
    // the parser. Returns the number of complete DMX frames observed this
    // call. Call from the mode's loop_tick.
    size_t poll();

    bool active() const { return active_; }

    // Same accessors DmxBridge reads on the USB adapter.
    DmxInputParser&       parser()       { return parser_; }
    const DmxInputParser& parser() const { return parser_; }
    uint32_t bytes_read() const { return bytes_read_; }

    // Optional universe overrides (call before begin()).
    void set_artnet_universe(uint16_t u) { artnet_universe_ = u; }
    void set_sacn_universe(uint16_t u)   { sacn_universe_   = u; }

    // Link/hardware health for the status LED. Valid after begin().
    bool hardware_present() const;   // W5500 detected on the SPI bus
    bool link_up() const;            // Ethernet PHY link is up (cable in)

    // Current IPv4 address (DHCP lease or static fallback) for the AtomS3R
    // dashboard. All-zeros before begin() / with no address. Kept here so
    // DmxBridge doesn't have to include Ethernet.h itself.
    void local_ip(uint8_t out[4]) const;

private:
    // Feed a freshly received DMX-512 channel slice through the parser as
    // one Enttec "Output Only Send DMX" frame. `n` is the channel count
    // (<= 512). Bumps bytes_read_ and the parser's frame_count.
    void feed_universe(const uint8_t* channels, uint16_t n);

    DmxInputParser  parser_;
    bool            active_          = false;
    uint32_t        bytes_read_      = 0;
    uint16_t        artnet_universe_ = kDefaultArtnetUniverse;
    uint16_t        sacn_universe_   = kDefaultSacnUniverse;
};

// The one adapter instance (DmxBridge owns its lifecycle). Exposed so the
// serial config console can read live state - frame counts, channel values
// - without threading a pointer through the mode.
EthernetDmxAdapter& ethernet_dmx_adapter_instance();

}  // namespace dal
}  // namespace nocturnation
