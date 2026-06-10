/**
 * @file RtcmSignalMap.cpp
 * @brief Static signal-ID tables per RTCM 3.x Annex 1.
 *
 * Tables transcribed from RTCM 3.3 (Amendments 1-2 included).  Where the
 * standard reserves an ID, the corresponding entry is empty.  Adding new
 * signal IDs from later amendments (e.g. additional BeiDou-3 signals) is
 * a matter of extending the appropriate per-system array.
 */

#include "rtcm/RtcmSignalMap.hpp"
#include <array>

namespace GnssProcessor::RtcmSignalMap {

namespace {

// 32-entry tables; index = RTCM signal ID (5 bits = 0..31).
// Empty string means the ID is reserved/unassigned for that system.

constexpr std::size_t kTableSize = 32;
using SignalTable = std::array<const char*, kTableSize>;

constexpr SignalTable kGps = {
//   0     1     2     3     4     5     6     7
    "",   "",   "1C", "1P", "1W", "",   "",   "",
//   8     9    10    11    12    13    14    15
    "2C", "2P", "2W", "",   "",   "",   "",   "2S",
//  16    17    18    19    20    21    22    23
    "2L", "2X", "",   "",   "",   "",   "5I", "5Q",
//  24    25    26    27    28    29    30    31
    "5X", "",   "",   "",   "",   "",   "1S", "1L"
};
// Note: ID 32 doesn't fit in 5 bits; "1X" (id 32 in some old refs) would
// alias 0.  The 5-bit encoding caps GPS signals at the 32 IDs above.

constexpr SignalTable kGlonass = {
    "",   "",   "1C", "1P", "",   "",   "",   "",
    "2C", "2P", "3I", "3Q", "3X", "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kGalileo = {
    "",   "",   "1C", "1A", "1B", "1X", "1Z", "",
    "6C", "6A", "6B", "6X", "6Z", "",   "7I", "7Q",
    "7X", "",   "8I", "8Q", "8X", "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kBeidou = {
    "",   "",   "2I", "2Q", "2X", "",   "",   "",
    "6I", "6Q", "6X", "",   "",   "",   "7I", "7Q",
    "7X", "",   "",   "",   "",   "",   "5D", "5P",
    "5X", "7D", "7P", "7Z", "8D", "8P", "8X", "1D"
};
// IDs 31..33 (1D / 1P / 1X for B1C) cluster at the top; ID 31 is "1D".
// IDs 32-33 are out of 5-bit range and only addressable via IGS-SSR
// extended fields; not represented here.

constexpr SignalTable kQzss = {
    "",   "",   "1C", "",   "",   "",   "",   "",
    "",   "6S", "6L", "6X", "",   "",   "",   "2S",
    "2L", "2X", "",   "",   "",   "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "1S", "1L"
};

constexpr SignalTable kSbas = {
    "",   "",   "1C", "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "",   ""
};

const SignalTable* tableFor(System sys) {
    switch (sys) {
    case System::GPS:     return &kGps;
    case System::GLONASS: return &kGlonass;
    case System::Galileo: return &kGalileo;
    case System::BeiDou:  return &kBeidou;
    case System::QZSS:    return &kQzss;
    case System::SBAS:    return &kSbas;
    default:              return nullptr;
    }
}

} // namespace

std::string toRinexSuffix(System sys, int signal_id) {
    const auto* table = tableFor(sys);
    if (!table) return {};
    if (signal_id < 0 || static_cast<std::size_t>(signal_id) >= kTableSize) return {};
    return std::string((*table)[static_cast<std::size_t>(signal_id)]);
}

int fromRinexSuffix(System sys, const std::string& rinex_suffix) {
    const auto* table = tableFor(sys);
    if (!table || rinex_suffix.size() != 2) return -1;
    for (std::size_t i = 0; i < kTableSize; ++i) {
        if (rinex_suffix == (*table)[i]) return static_cast<int>(i);
    }
    return -1;
}

} // namespace GnssProcessor::RtcmSignalMap
