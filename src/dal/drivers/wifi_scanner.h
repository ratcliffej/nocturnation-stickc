// WifiScanner - passive 2.4 GHz Wi-Fi scan for pre-show ESP-NOW channel
// selection. Runs a blocking scan across channels 1..13, aggregates
// per-channel AP counts + max RSSI, and recommends the least-congested
// non-overlapping channel from {1, 6, 11}.
//
// Non-overlapping channel policy: ESP-NOW uses 20 MHz Wi-Fi channels.
// Adjacent-channel interference from channel 1 and channel 6 pollutes
// channels 2-5, so recommending "quiet channel 3" is misleading. The
// picker restricts recommendations to 1/6/11 by design; the bars for
// the other channels still render for operator context.
//
// Design split: aggregation + picker are pure host-testable logic; the
// scan() method is Arduino-only (wraps WiFi.scanNetworks). Native tests
// drive the aggregator via ingest_sample() without ever calling scan().

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace dal {

// 2.4 GHz Wi-Fi channels 1..13 inclusive. Regulatory domain doesn't
// affect the enumeration - the aggregator accepts samples on any of
// these; scan() results are filtered by the underlying driver's
// country policy.
constexpr size_t kWifiChannelCount = 13;

// Per-channel scan result. ap_count = distinct APs observed on this
// channel during the scan; max_rssi = strongest AP's RSSI in dBm.
// Sentinel: max_rssi == 0 means "no AP seen". Real RSSI values are
// always negative.
struct WifiChannelStats {
    uint8_t  ap_count;
    int8_t   max_rssi;
};

class WifiScanner {
public:
    WifiScanner();

    // Fold a single (channel, rssi) sample into the aggregator. Silently
    // ignores channels outside 1..13 (e.g. 5 GHz results from dual-band
    // drivers). Public so native tests can inject fixtures directly.
    void ingest_sample(uint8_t channel_1_to_13, int8_t rssi_dbm);

    // Zero all channel stats.
    void reset();

    // Per-channel stats. ch is 1..13. Out-of-range channels return
    // zero-initialised stats - no aborts.
    WifiChannelStats channel(uint8_t ch) const;

    // Recommend the best ESP-NOW channel from the non-overlapping set
    // {1, 6, 11}. Fewest APs wins; ties broken by weakest max_rssi
    // (channel with the quietest strongest-AP wins the tie). On an
    // empty aggregator all three tie at ap_count=0, returns 1.
    uint8_t recommend_channel() const;

    // Passive blocking scan across all reachable 2.4 GHz channels
    // (Arduino builds only). dwell_ms is the per-channel listen window;
    // 300 ms is the bench-tuned default (total wall time ~4-5 s across
    // 13 channels + driver overhead). ESP-NOW packets on the pinned
    // channel are NOT received while this runs - the caller must warn
    // the operator. Returns false if the underlying scan API failed
    // to start; on native builds this no-ops and returns true.
    bool scan(uint16_t dwell_ms = 300);

private:
    WifiChannelStats stats_[kWifiChannelCount] = {};
};

}  // namespace dal
}  // namespace nocturnation
