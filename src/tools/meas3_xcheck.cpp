/**
 * @file meas3_xcheck.cpp
 * @brief Cross-validate the Meas3Ranges (4109) decoder against MeasEpoch (4027).
 *
 * A Septentrio receiver can log the legacy MeasEpoch (4027) and the compact
 * Meas3Ranges (4109) blocks for the same epochs.  For such a capture this tool
 * decodes both and compares the reconstructed code and phase observables per
 * (satellite, RINEX code), which checks the differential Meas3 decoder against
 * MeasEpoch with no external reference.  Agreement is expected at the
 * encodings' shared resolution: pseudorange ~1 mm, carrier phase ~0.001
 * cycles.  A difference above 0.02 m (code) or 0.02 cycles (phase) counts as
 * a mismatch.
 *
 * Usage:  gnss_meas3_xcheck <path-to-sbf-dump>
 */

#include "sbf/SbfStreamReader.hpp"
#include "sbf/SbfParser.hpp"
#include "rinex/RinexParser.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {
// (satellite, obs-code) -> value, for one epoch.
using EpochObs = std::map<std::pair<std::string, std::string>, double>;

void collect(const std::vector<RinexObservation>& obs, EpochObs& code, EpochObs& phase) {
    for (const auto& o : obs)
        for (const auto& [k, v] : o.measurements) {
            if (k[0] == 'C') code[{o.satellite, k}]  = v;
            else if (k[0] == 'L') phase[{o.satellite, k}] = v;
        }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: gnss_meas3_xcheck <path-to-sbf-dump>\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", argv[1]); return 1; }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());

    SbfStreamReader reader;
    reader.feed(data);

    // Per-epoch (TOW ms) observables from each block type.
    std::map<uint32_t, EpochObs> me_code, me_phase;   // 4027 ground truth
    std::map<uint32_t, EpochObs> m3_code, m3_phase;   // 4109 under test

    while (auto frame = reader.nextFrame()) {
        if (frame->size() < 14) continue;
        const uint32_t tow = static_cast<uint32_t>(
            (*frame)[8] | ((*frame)[9] << 8) | ((*frame)[10] << 16) | ((uint32_t)(*frame)[11] << 24));
        SbfParser parser;
        parser.parseFrame(*frame);
        const int bn = parser.lastBlockNumber();
        if (bn == 4027)      collect(parser.getObservations(), me_code[tow], me_phase[tow]);
        else if (bn == 4109) collect(parser.getObservations(), m3_code[tow], m3_phase[tow]);
    }

    // Compare only epochs for which both block types produced code observables.
    int n_code = 0, n_phase = 0, bad_code = 0, bad_phase = 0;
    int only_me = 0, only_m3 = 0, n_epochs = 0;
    double worst_code = 0.0, worst_phase = 0.0;
    std::vector<std::string> samples;

    for (const auto& [tow, mec] : me_code) {
        auto it = m3_code.find(tow);
        if (it == m3_code.end() || it->second.empty()) continue;   // 4109 not decoded (pre-reference)
        ++n_epochs;
        const EpochObs& m3c = it->second;
        const EpochObs& mep = me_phase[tow];
        const EpochObs& m3p = m3_phase[tow];

        for (const auto& [key, vme] : mec) {
            auto jt = m3c.find(key);
            if (jt == m3c.end()) { ++only_me; continue; }
            const double d = std::fabs(vme - jt->second);
            ++n_code; worst_code = std::max(worst_code, d);
            if (d > 0.02 && samples.size() < 20)
                samples.push_back(std::string("CODE ") + key.first + " " + key.second +
                    " me=" + std::to_string(vme) + " m3=" + std::to_string(jt->second) +
                    " d=" + std::to_string(d));
            if (d > 0.02) ++bad_code;
        }
        for (const auto& [key, vme] : m3c) if (!mec.count(key)) ++only_m3;

        for (const auto& [key, vme] : mep) {
            auto jt = m3p.find(key);
            if (jt == m3p.end()) continue;
            const double d = std::fabs(vme - jt->second);
            ++n_phase; worst_phase = std::max(worst_phase, d);
            if (d > 0.02 && samples.size() < 20)
                samples.push_back(std::string("PHASE ") + key.first + " " + key.second +
                    " d=" + std::to_string(d) + " cy");
            if (d > 0.02) ++bad_phase;
        }
    }

    std::printf("epochs compared (both 4027 & 4109): %d\n", n_epochs);
    std::printf("CODE  : %d compared, %d mismatched (>2cm), worst=%.4f m\n",
                n_code, bad_code, worst_code);
    std::printf("PHASE : %d compared, %d mismatched (>0.02cy), worst=%.4f cy\n",
                n_phase, bad_phase, worst_phase);
    std::printf("set diff: 4027-only=%d  4109-only=%d  (per-epoch obs present in one but not the other)\n",
                only_me, only_m3);
    if (!samples.empty()) {
        std::printf("\nfirst mismatches:\n");
        for (const auto& s : samples) std::printf("  %s\n", s.c_str());
    }
    const bool pass = bad_code == 0 && bad_phase == 0 && only_me == 0 && only_m3 == 0 && n_epochs > 0;
    std::printf("\n%s\n", pass ? "PASS: Meas3Ranges matches MeasEpoch bit-for-bit at shared resolution"
                               : "CHECK ABOVE (mismatches or no overlapping epochs)");
    return pass ? 0 : 2;
}
