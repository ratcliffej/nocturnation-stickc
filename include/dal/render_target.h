// render_target.h - parser for the structured "<class>:<group>" target
// strings passed to DAL::render_fx (Epic 4.65 Block 4).
//
// The parser is declared in its own header so native tests can hit it
// directly. The structured target format gives operators a unified
// addressing scheme across device classes; see architecture spec §4.3
// and §7.6 for the wire shape and OutputBinding contract.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace dal {

// Parses a "<hex_class>:<hex_group>" target string into class + group
// bytes. Returns true on a valid structured target; false on any legacy
// name ("local", "all-pixmobs", "esp-now-broadcast", "group-N") or
// malformed string. The strict format is: hex digits, colon, hex
// digits, end-of-string. Empty either side, embedded whitespace, "0x"
// prefixes, trailing garbage, or any field outside 0..0xFF reject.
bool parse_target_class_group(const char* target,
                               uint8_t&    out_class,
                               uint8_t&    out_group);

}  // namespace dal
}  // namespace nocturnation
