// EthernetDmxAdapter implementation.
//
// Compiled ONLY for the m5stack-atoms3-poe env (NOCT_DMX_ETHERNET). On
// every other env this file is empty so Ethernet.h never reaches the Stick
// or native builds despite build_src_filter globbing all of src/dal/.

#include "ethernet_dmx_adapter.h"

#if defined(NOCT_DMX_ETHERNET)

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <esp_mac.h>
#include <cstring>

#include "net_config.h"

namespace nocturnation {
namespace dal {

namespace {

// =============================================================================
// W5500 wiring on the M5 Atomic PoE Base + AtomS3 (per M5Stack docs).
// =============================================================================
constexpr int kPinSCK  = 5;
constexpr int kPinMISO = 7;
constexpr int kPinMOSI = 8;
constexpr int kPinCS   = 6;
constexpr int kPinRST  = -1;   // Atomic PoE base does NOT break out W5500 RST/INT

// UDP ports per protocol spec.
constexpr uint16_t kArtnetPort = 6454;
constexpr uint16_t kSacnPort   = 5568;

// Art-Net
constexpr uint8_t  kArtnetId[8]  = { 'A','r','t','-','N','e','t', 0 };
constexpr uint16_t kArtnetOpDmx  = 0x5000;   // little-endian on the wire

// sACN E1.31 fixed offsets (root + framing + DMP headers).
constexpr uint8_t  kAcnId[12] = {
    0x41,0x53,0x43,0x2d,0x45,0x31,0x2e,0x31,0x37,0x00,0x00,0x00  // "ASC-E1.17"
};
constexpr size_t kSacnUniverseHi   = 113;   // big-endian universe
constexpr size_t kSacnUniverseLo   = 114;
constexpr size_t kSacnPropCountHi  = 123;   // big-endian property value count
constexpr size_t kSacnPropCountLo  = 124;
constexpr size_t kSacnStartCode    = 125;   // DMX start code
constexpr size_t kSacnChannels     = 126;   // first channel value

constexpr uint16_t kMaxChannels = 512;

// Enttec "Output Only Send DMX" framing the parser expects.
constexpr uint8_t kEnttecStart = 0x7E;
constexpr uint8_t kEnttecLabel = 0x06;
constexpr uint8_t kEnttecEnd   = 0xE7;
constexpr uint8_t kDmxStartCode = 0x00;

// One adapter instance exists (DmxBridge's local-static), so the sockets +
// rx buffer live at file scope rather than bloating the header with
// Ethernet types.
EthernetUDP s_udp_artnet;
EthernetUDP s_udp_sacn;
uint8_t     s_rx[700];   // >= largest sACN packet (638) and Art-Net (530)

}  // namespace

EthernetDmxAdapter::EthernetDmxAdapter() = default;

void EthernetDmxAdapter::begin(uint32_t /*baud ignored*/) {
    if (active_) {
        parser_.reset();
        return;
    }

    // Pull the persisted config (DHCP/static, universes) from NVS - set
    // over the USB-serial console, survives reboots.
    const NetConfig cfg = net_config_load();
    artnet_universe_ = cfg.artnet_universe;
    sacn_universe_   = cfg.sacn_universe;

    // Dedicated Ethernet MAC from efuse (distinct from the WiFi STA MAC
    // ESP-NOW uses, so the two interfaces never collide).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);

    SPI.begin(kPinSCK, kPinMISO, kPinMOSI, /*ss=*/-1);  // CS owned by Ethernet.init
    Ethernet.init(kPinCS);
    if (kPinRST >= 0) {
        pinMode(kPinRST, OUTPUT);
        digitalWrite(kPinRST, LOW);  delay(2);
        digitalWrite(kPinRST, HIGH); delay(10);
    }

    const IPAddress ip  (cfg.ip[0],   cfg.ip[1],   cfg.ip[2],   cfg.ip[3]);
    const IPAddress gw  (cfg.gw[0],   cfg.gw[1],   cfg.gw[2],   cfg.gw[3]);
    const IPAddress mask(cfg.mask[0], cfg.mask[1], cfg.mask[2], cfg.mask[3]);
    if (cfg.use_dhcp) {
        // DHCP first; on failure fall back to the configured static address
        // so the device is still reachable on a switch with no DHCP server.
        if (Ethernet.begin(mac, /*timeout=*/5000, /*responseTimeout=*/1000) == 0) {
            Ethernet.begin(mac, ip, /*dns=*/gw, /*gw=*/gw, mask);
        }
    } else {
        Ethernet.begin(mac, ip, /*dns=*/gw, /*gw=*/gw, mask);
    }

    s_udp_artnet.begin(kArtnetPort);
    // sACN multicast group for the configured universe: 239.255.<hi>.<lo>.
    IPAddress sacn_group(239, 255, (sacn_universe_ >> 8) & 0xFF,
                                    sacn_universe_ & 0xFF);
    s_udp_sacn.beginMulticast(sacn_group, kSacnPort);

    parser_.reset();
    bytes_read_ = 0;
    active_     = true;
}

void EthernetDmxAdapter::end() {
    if (!active_) return;
    s_udp_artnet.stop();
    s_udp_sacn.stop();
    active_ = false;
}

bool EthernetDmxAdapter::hardware_present() const {
    return Ethernet.hardwareStatus() != EthernetNoHardware;
}

bool EthernetDmxAdapter::link_up() const {
    // W5500 reports real PHY link state. Treat only an explicit LinkOFF as
    // down (a quirky Unknown reads as up) so we don't false-flag a fault.
    return Ethernet.linkStatus() != LinkOFF;
}

void EthernetDmxAdapter::local_ip(uint8_t out[4]) const {
    const IPAddress ip = Ethernet.localIP();
    out[0] = ip[0];
    out[1] = ip[1];
    out[2] = ip[2];
    out[3] = ip[3];
}

EthernetDmxAdapter& ethernet_dmx_adapter_instance() {
    static EthernetDmxAdapter s_instance;
    return s_instance;
}

void EthernetDmxAdapter::feed_universe(const uint8_t* channels, uint16_t n) {
    if (n > kMaxChannels) n = kMaxChannels;
    const uint16_t len = static_cast<uint16_t>(n) + 1;  // start code + channels
    parser_.feed_byte(kEnttecStart);
    parser_.feed_byte(kEnttecLabel);
    parser_.feed_byte(len & 0xFF);
    parser_.feed_byte((len >> 8) & 0xFF);
    parser_.feed_byte(kDmxStartCode);
    for (uint16_t i = 0; i < n; ++i) {
        parser_.feed_byte(channels[i]);
    }
    parser_.feed_byte(kEnttecEnd);
    bytes_read_ += static_cast<uint32_t>(len) + 5;  // 4 header + end byte
}

size_t EthernetDmxAdapter::poll() {
    if (!active_) return 0;
    Ethernet.maintain();   // renew DHCP lease if needed (no-op on static)
    size_t frames = 0;

    // --- Art-Net (UDP 6454) ---
    for (;;) {
        const int sz = s_udp_artnet.parsePacket();
        if (sz <= 0) break;
        const int got = s_udp_artnet.read(s_rx, sizeof(s_rx));
        if (got < 18) continue;
        if (std::memcmp(s_rx, kArtnetId, sizeof(kArtnetId)) != 0) continue;
        const uint16_t op = s_rx[8] | (uint16_t(s_rx[9]) << 8);
        if (op != kArtnetOpDmx) continue;
        const uint16_t universe = (uint16_t(s_rx[15]) << 8) | s_rx[14];
        if (universe != artnet_universe_) continue;
        uint16_t dlen = (uint16_t(s_rx[16]) << 8) | s_rx[17];  // big-endian
        if (18 + dlen > got) dlen = (got > 18) ? (got - 18) : 0;
        if (dlen == 0) continue;
        feed_universe(s_rx + 18, dlen);
        ++frames;
    }

    // --- sACN / E1.31 (UDP 5568 multicast) ---
    for (;;) {
        const int sz = s_udp_sacn.parsePacket();
        if (sz <= 0) break;
        const int got = s_udp_sacn.read(s_rx, sizeof(s_rx));
        if (got < int(kSacnChannels)) continue;
        if (std::memcmp(s_rx + 4, kAcnId, sizeof(kAcnId)) != 0) continue;
        const uint16_t universe =
            (uint16_t(s_rx[kSacnUniverseHi]) << 8) | s_rx[kSacnUniverseLo];
        if (universe != sacn_universe_) continue;
        if (s_rx[kSacnStartCode] != 0x00) continue;   // only standard DMX data
        const uint16_t prop_count =
            (uint16_t(s_rx[kSacnPropCountHi]) << 8) | s_rx[kSacnPropCountLo];
        uint16_t n = (prop_count > 0) ? (prop_count - 1) : 0;  // minus start code
        if (kSacnChannels + n > size_t(got)) {
            n = (got > int(kSacnChannels)) ? (got - kSacnChannels) : 0;
        }
        if (n == 0) continue;
        feed_universe(s_rx + kSacnChannels, n);
        ++frames;
    }

    return frames;
}

}  // namespace dal
}  // namespace nocturnation

#endif  // NOCT_DMX_ETHERNET
