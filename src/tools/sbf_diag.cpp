/**
 * @file sbf_diag.cpp
 * @brief Standalone Septentrio SBF file inspector.
 *
 * Drains a binary SBF capture through @c SbfStreamReader + @c SbfParser and
 * prints a per-block-number histogram, the decoded observation/ephemeris
 * counts, the raw navigation message counts with the words of the first GPS
 * message, the ephemerides assembled from raw subframes, and a per-satellite
 * dump of the first epoch with observations (MeasEpoch or Meas3Ranges,
 * whichever block comes first).  Intended for
 * sanity-checking a real receiver capture against the decoder before
 * plumbing it through the live realtime pipeline.
 *
 * Usage:  gnss_sbf_diag <path-to-sbf-dump>
 */

#include "sbf/SbfStreamReader.hpp"
#include "sbf/SbfParser.hpp"
#include "sbf/SubframeAssembler.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "rinex/RinexParser.hpp"
#include "common/GnssTypes.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
const char* blockLabel(int bn) {
    switch (bn) {
    case 4027: return "MeasEpoch (decoded)";
    case 4109: return "Meas3Ranges (decoded)";
    case 4110: return "Meas3CN0HiRes (skipped)";
    case 4111: return "Meas3Doppler (skipped)";
    case 4007: return "PVTGeodetic (skipped)";
    case 5891: return "GPSNav (decoded)";
    case 4002: return "GALNav (decoded)";
    case 4081: return "BDSNav (decoded)";
    case 4004: return "GLONav (decoded)";
    case 4095: return "QZSNav (decoded)";
    default:   return "";
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: gnss_sbf_diag <path-to-sbf-dump>\n");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::printf("cannot open %s\n", argv[1]);
        return 1;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    std::printf("read %zu bytes from %s\n", data.size(), argv[1]);

    using namespace GnssProcessor;
    SbfStreamReader reader;
    reader.feed(data);

    std::map<int, int> block_hist;
    std::map<std::string, int> sat_epochs;
    std::map<std::string, int> rawnav_per_sat;
    int n_meas_epochs = 0, n_eph = 0, n_rawnav = 0, n_rawnav_crc = 0;
    std::vector<RinexObservation> first_epoch;
    std::vector<uint32_t> first_gps_rawnav;
    std::string first_gps_rawnav_sat;
    SubframeAssembler assembler;
    std::vector<BroadcastEphemeris> assembled;

    while (auto frame = reader.nextFrame()) {
        SbfParser parser;
        parser.parseFrame(*frame);
        block_hist[parser.lastBlockNumber()]++;
        const auto& obs = parser.getObservations();
        if (!obs.empty()) {
            ++n_meas_epochs;
            if (first_epoch.empty()) first_epoch = obs;
            for (const auto& o : obs) sat_epochs[o.satellite]++;
        }
        n_eph += static_cast<int>(parser.getBroadcastEphemeris().size());
        for (const auto& m : parser.getRawNavMessages()) {
            ++n_rawnav;
            if (m.crc_passed) ++n_rawnav_crc;
            rawnav_per_sat[m.sat]++;
            if (first_gps_rawnav.empty() && m.system == System::GPS) {
                first_gps_rawnav = m.words;
                first_gps_rawnav_sat = m.sat;
            }
            if (auto eph = assembler.consume(m)) assembled.push_back(*eph);
        }
    }

    std::printf("\nframes: extracted=%llu  discarded=%llu\n",
                static_cast<unsigned long long>(reader.framesExtracted()),
                static_cast<unsigned long long>(reader.bytesDiscarded()));
    std::printf("\nblock-number histogram:\n");
    for (const auto& [bn, c] : block_hist)
        std::printf("  %5d  x%-4d %s\n", bn, c, blockLabel(bn));

    std::printf("\nMeasEpoch epochs: %d   broadcast ephemeris: %d   distinct sats: %zu\n",
                n_meas_epochs, n_eph, sat_epochs.size());
    std::printf("raw-nav messages: %d (crc-passed %d)   distinct sats: %zu\n",
                n_rawnav, n_rawnav_crc, rawnav_per_sat.size());
    if (!first_gps_rawnav.empty()) {
        std::printf("first GPS raw-nav (%s) words:", first_gps_rawnav_sat.c_str());
        for (uint32_t w : first_gps_rawnav) std::printf(" %08x", w);
        std::printf("\n");
    }

    std::printf("\nassembled ephemerides (from raw subframes): %zu\n", assembled.size());
    auto& store = LiveEphemerisStore::getInstance();
    store.clear();
    for (const auto& e : assembled) {
        store.insert(e);
        const auto& k = e.kepler;
        const auto pv = store.computeSatellitePV(e.satellite, k.toe);
        const double posnorm_km = pv ? pv->position.norm() / 1000.0 : 0.0;
        std::printf("  %-4s iode=%-4d sqrtA=%.3f e=%.6f i0=%.4frad toe=%.0f |pos|=%.1fkm\n",
                    e.satellite.c_str(), e.iode, k.sqrt_a, k.e, k.i0, k.toe.tow, posnorm_km);
    }

    std::printf("\nfirst MeasEpoch — %zu satellites:\n", first_epoch.size());
    for (const auto& o : first_epoch) {
        std::printf("  %-4s", o.satellite.c_str());
        for (const auto& [code, val] : o.measurements) {
            if (code[0] == 'C')      std::printf(" %s=%.2fm", code.c_str(), val);
            else if (code[0] == 'S') std::printf(" %s=%.1fdB", code.c_str(), val);
        }
        std::printf("\n");
    }
    return 0;
}
