/**
 * @file RtcmSignalMap.cpp
 * @brief Static signal-ID tables: the MSM numbering and the SSR bias numbering.
 *
 * The MSM tables are transcribed from RTCM 3.3 (Amendments 1-2 included).  The SSR
 * tables follow the cssrlib reference decoder (`rtcm.py`, `ssig2rsig`), which uses
 * one table for the legacy and the IGS-SSR bias messages.  Where an ID is reserved
 * or unassigned, the entry is empty.  Signal IDs from later amendments are added by
 * extending the per-system array.
 */

#include "rtcm/RtcmSignalMap.hpp"
#include <array>

namespace GnssProcessor::RtcmSignalMap {

namespace {

// 32-entry tables; index = RTCM signal ID (5 bits = 0..31).
// Empty string means the ID is reserved/unassigned for that system.

constexpr std::size_t kTableSize = 32;
using SignalTable = std::array<const char*, kTableSize>;

// ---- MSM: the cell signal-mask position ---------------------------------------

constexpr SignalTable kMsmGps = {
//   0     1     2     3     4     5     6     7
    "",   "",   "1C", "1P", "1W", "",   "",   "",
//   8     9    10    11    12    13    14    15
    "2C", "2P", "2W", "",   "",   "",   "",   "2S",
//  16    17    18    19    20    21    22    23
    "2L", "2X", "",   "",   "",   "",   "5I", "5Q",
//  24    25    26    27    28    29    30    31
    "5X", "",   "",   "",   "",   "",   "1S", "1L"
};
// ID 32 does not fit in 5 bits: "1X" (ID 32 in some older references) would
// alias ID 0, so it is not represented.  The 5-bit index limits GPS to the 32
// IDs above.

constexpr SignalTable kMsmGlonass = {
    "",   "",   "1C", "1P", "",   "",   "",   "",
    "2C", "2P", "3I", "3Q", "3X", "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kMsmGalileo = {
    "",   "",   "1C", "1A", "1B", "1X", "1Z", "",
    "6C", "6A", "6B", "6X", "6Z", "",   "7I", "7Q",
    "7X", "",   "8I", "8Q", "8X", "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kMsmBeidou = {
    "",   "",   "2I", "2Q", "2X", "",   "",   "",
    "6I", "6Q", "6X", "",   "",   "",   "7I", "7Q",
    "7X", "",   "",   "",   "",   "",   "5D", "5P",
    "5X", "7D", "7P", "7Z", "8D", "8P", "8X", "1D"
};
// B1C uses IDs 31..33 (1D, 1P, 1X); the table holds ID 31 as "1D".  IDs 32 and 33
// are outside the 5-bit range, are addressable only through IGS-SSR extended
// fields, and are not represented here.

constexpr SignalTable kMsmQzss = {
    "",   "",   "1C", "",   "",   "",   "",   "",
    "",   "6S", "6L", "6X", "",   "",   "",   "2S",
    "2L", "2X", "",   "",   "",   "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "1S", "1L"
};

constexpr SignalTable kMsmSbas = {
    "",   "",   "1C", "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "5I", "5Q",
    "5X", "",   "",   "",   "",   "",   "",   ""
};

// ---- SSR: the bias messages' signal and tracking mode identifier ------------

constexpr SignalTable kSsrGps = {
//   0     1     2     3     4     5     6     7
    "1C", "1P", "1W", "",   "",   "2C", "2D", "2S",
//   8     9    10    11    12    13    14    15
    "2L", "2X", "2P", "2W", "",   "",   "5I", "5Q",
//  16    17    18    19    20    21    22    23
    "5X", "1S", "1L", "1X", "",   "",   "",   "",
//  24    25    26    27    28    29    30    31
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kSsrGlonass = {
    "1C", "1P", "2C", "2P", "",   "",   "",   "",
    "",   "",   "3I", "3Q", "3X", "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kSsrGalileo = {
    "1A", "1B", "1C", "1X", "1Z", "5I", "5Q", "5X",
    "7I", "7Q", "7X", "8I", "8Q", "8X", "6A", "6B",
    "6C", "6X", "6Z", "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kSsrBeidou = {
    "2I", "2Q", "2X", "6I", "6Q", "6X", "7I", "7Q",
    "7X", "1D", "1P", "1X", "5D", "5P", "5X", "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

// cssrlib maps QZSS ID 2 to L1C, a duplicate of ID 0; the ID is left unassigned
// here rather than given a guessed signal.
constexpr SignalTable kSsrQzss = {
    "1C", "1S", "",   "2S", "2L", "2X", "5I", "5Q",
    "5X", "6S", "6L", "6X", "1X", "1Z", "5D", "5P",
    "5Z", "6E", "6Z", "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

constexpr SignalTable kSsrSbas = {
    "1C", "5I", "5Q", "5X", "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   "",
    "",   "",   "",   "",   "",   "",   "",   ""
};

const SignalTable* msmTableFor(System sys) {
    switch (sys) {
    case System::GPS:     return &kMsmGps;
    case System::GLONASS: return &kMsmGlonass;
    case System::Galileo: return &kMsmGalileo;
    case System::BeiDou:  return &kMsmBeidou;
    case System::QZSS:    return &kMsmQzss;
    case System::SBAS:    return &kMsmSbas;
    default:              return nullptr;
    }
}

const SignalTable* ssrTableFor(System sys) {
    switch (sys) {
    case System::GPS:     return &kSsrGps;
    case System::GLONASS: return &kSsrGlonass;
    case System::Galileo: return &kSsrGalileo;
    case System::BeiDou:  return &kSsrBeidou;
    case System::QZSS:    return &kSsrQzss;
    case System::SBAS:    return &kSsrSbas;
    default:              return nullptr;
    }
}

std::string suffixOf(const SignalTable* table, int signal_id) {
    if (!table) return {};
    if (signal_id < 0 || static_cast<std::size_t>(signal_id) >= kTableSize) return {};
    return std::string((*table)[static_cast<std::size_t>(signal_id)]);
}

int idOf(const SignalTable* table, const std::string& rinex_suffix) {
    if (!table || rinex_suffix.size() != 2) return -1;
    for (std::size_t i = 0; i < kTableSize; ++i) {
        if (rinex_suffix == (*table)[i]) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

std::string msmToRinexSuffix(System sys, int signal_id) {
    return suffixOf(msmTableFor(sys), signal_id);
}

int msmFromRinexSuffix(System sys, const std::string& rinex_suffix) {
    return idOf(msmTableFor(sys), rinex_suffix);
}

std::string ssrToRinexSuffix(System sys, int signal_id) {
    return suffixOf(ssrTableFor(sys), signal_id);
}

int ssrFromRinexSuffix(System sys, const std::string& rinex_suffix) {
    return idOf(ssrTableFor(sys), rinex_suffix);
}

} // namespace GnssProcessor::RtcmSignalMap
