// Shared property-bag apply - see config_apply.h for the rationale.
// Body hoisted verbatim from ble_service.cpp at Epic 21 B3d.

#include "ble/config_apply.h"
#include "modes/persistence.h"

#include <cstring>

namespace nocturnation {
namespace ble {

bool apply_config_entry(const TlvEntry& e, void* raw_ctx) {
    auto* ctx = static_cast<ApplyCtx*>(raw_ctx);
    // Copy the key to a small NUL-terminated buffer for strcmp.
    char keybuf[32] = {};
    const size_t klen = e.key_len < sizeof(keybuf) - 1 ? e.key_len : sizeof(keybuf) - 1;
    std::memcpy(keybuf, e.key, klen);
    keybuf[klen] = '\0';

    auto read_u8 = [&](uint8_t& out) -> bool {
        if (e.type != ValueType::U8 || e.value_len != 1 || !e.value) return false;
        out = e.value[0];
        return true;
    };

    if (ctx->role == Role::Director) {
        if (std::strcmp(keybuf, key::kDirSidPerf) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            if (v < 0x40 || v > 0xFE) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_director_perf_source_id(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kRetxCount) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_retx_count(v);
            return true;
        }
    } else {
        if (std::strcmp(keybuf, key::kGroup) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_lume_group(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kLedPower) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_strip_brightness(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kChannelPref) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_lume_channel(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kStripChain) == 0) {
            if (e.type != ValueType::U16 || e.value_len != 2 || !e.value) {
                ctx->value_out_of_range = true; return true;
            }
            const uint16_t v = static_cast<uint16_t>(e.value[0])
                             | (static_cast<uint16_t>(e.value[1]) << 8);
            modes::persistence::save_strip_chain_size(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kStripGroupSize) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_strip_group_size(v);
            return true;
        }
    }

    // Keys common to both roles.
    if (std::strcmp(keybuf, key::kPairWinS) == 0) {
        uint8_t v;
        if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
        modes::persistence::save_pair_win_s(v);
        return true;
    }
    if (std::strcmp(keybuf, key::kFriendlyName) == 0) {
        if (e.type != ValueType::Utf8) { ctx->value_out_of_range = true; return true; }
        char clipped[21] = {};
        const size_t n = e.value_len < 20 ? e.value_len : 20;
        for (size_t i = 0; i < n; ++i) clipped[i] = static_cast<char>(e.value[i]);
        clipped[n] = '\0';
        modes::persistence::save_friendly_name(clipped);
        return true;
    }

    // Unknown key - silently accept (forward-compat, Epic 20 §4).
    return true;
}

}  // namespace ble
}  // namespace nocturnation
