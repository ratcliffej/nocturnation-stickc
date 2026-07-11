#include "dal/drivers/wifi_scanner.h"

#include <cstring>

#ifdef ARDUINO
#include <WiFi.h>
#endif

namespace nocturnation {
namespace dal {

namespace {

constexpr uint8_t kNonOverlappingChannels[3] = {1, 6, 11};
constexpr int8_t  kUnseenRssiFloor           = -128;

int8_t effective_rssi(int8_t max_rssi) {
    return max_rssi == 0 ? kUnseenRssiFloor : max_rssi;
}

}  // namespace

WifiScanner::WifiScanner() { reset(); }

void WifiScanner::reset() {
    std::memset(stats_, 0, sizeof(stats_));
}

void WifiScanner::ingest_sample(uint8_t channel_1_to_13, int8_t rssi_dbm) {
    if (channel_1_to_13 < 1 || channel_1_to_13 > kWifiChannelCount) return;
    WifiChannelStats& s = stats_[channel_1_to_13 - 1];
    if (s.ap_count < 0xFF) ++s.ap_count;
    if (s.max_rssi == 0 || rssi_dbm > s.max_rssi) s.max_rssi = rssi_dbm;
}

WifiChannelStats WifiScanner::channel(uint8_t ch) const {
    if (ch < 1 || ch > kWifiChannelCount) return WifiChannelStats{0, 0};
    return stats_[ch - 1];
}

uint8_t WifiScanner::recommend_channel() const {
    uint8_t best = kNonOverlappingChannels[0];
    for (int i = 1; i < 3; ++i) {
        const WifiChannelStats& cur  = stats_[best - 1];
        const WifiChannelStats& cand = stats_[kNonOverlappingChannels[i] - 1];
        if (cand.ap_count < cur.ap_count) { best = kNonOverlappingChannels[i]; continue; }
        if (cand.ap_count > cur.ap_count) continue;
        if (effective_rssi(cand.max_rssi) < effective_rssi(cur.max_rssi)) {
            best = kNonOverlappingChannels[i];
        }
    }
    return best;
}

bool WifiScanner::scan(uint16_t dwell_ms) {
    reset();
#ifdef ARDUINO
    const int n = WiFi.scanNetworks(
        /*async=*/false,
        /*show_hidden=*/true,
        /*passive=*/true,
        /*max_ms_per_chan=*/dwell_ms
    );
    if (n < 0) return false;
    for (int i = 0; i < n; ++i) {
        ingest_sample(
            static_cast<uint8_t>(WiFi.channel(i)),
            static_cast<int8_t>(WiFi.RSSI(i))
        );
    }
    WiFi.scanDelete();
    return true;
#else
    (void)dwell_ms;
    return true;
#endif
}

}  // namespace dal
}  // namespace nocturnation
