/**
 * @file rtcm_diag.cpp
 * @brief Standalone RTCM3 file inspector.
 *
 * Drains a binary RTCM3 dump through @c RtcmStreamReader + @c RtcmParser
 * and prints a per-message-type histogram plus a per-satellite summary
 * of the latest SSR records (orbit / clock / combined) and MSM7
 * observation counts.  Intended for sanity-checking that frames captured
 * from a Septentrio receiver or an IGS-RTS NTRIP feed decode to plausible
 * values before plumbing them through the live realtime pipeline.
 *
 * Usage:  gnss_rtcm_diag <path-to-rtcm-dump>
 */

#include "rtcm/RtcmParser.hpp"
#include "rtcm/RtcmTypes.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "common/GnssTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

const char* msgTypeLabel(int mt) {
    switch (mt) {
    case 1005: return "Station ARP (no height)";
    case 1006: return "Station ARP + height";
    case 1057: return "GPS Orbit SSR";
    case 1058: return "GPS Clock SSR";
    case 1059: return "GPS Code-Bias SSR";
    case 1060: return "GPS Combined SSR";
    case 1061: return "GPS URA SSR";
    case 1062: return "GPS HR-Clock SSR";
    case 1063: return "GLO Orbit SSR";
    case 1064: return "GLO Clock SSR";
    case 1065: return "GLO Code-Bias SSR";
    case 1066: return "GLO Combined SSR";
    case 1067: return "GLO URA SSR";
    case 1068: return "GLO HR-Clock SSR";
    case 1077: return "GPS MSM7";
    case 1087: return "GLO MSM7";
    case 1097: return "Galileo MSM7";
    case 1107: return "SBAS MSM7";
    case 1117: return "QZSS MSM7";
    case 1127: return "BeiDou MSM7";
    case 1240: return "GAL Orbit SSR";
    case 1241: return "GAL Clock SSR";
    case 1242: return "GAL Code-Bias SSR";
    case 1243: return "GAL Combined SSR";
    case 1244: return "GAL URA SSR";
    case 1245: return "GAL HR-Clock SSR";
    case 1265: return "GPS Phase-Bias SSR";
    case 1266: return "GLO Phase-Bias SSR";
    case 1267: return "GAL Phase-Bias SSR";
    case 1270: return "BDS Phase-Bias SSR";
    case 4076: return "IGS-SSR";
    default:   return "(other / unmapped)";
    }
}

void printUsage(const char* prog) {
    std::fprintf(stderr, "usage: %s <rtcm-file>\n", prog);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(argv[0]); return 1; }
    const std::string path = argv[1];

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "cannot open: %s\n", path.c_str());
        return 1;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));

    std::printf("=== RTCM 3.x diagnostic ===\n");
    std::printf("file: %s\n", path.c_str());
    std::printf("size: %.2f MB\n", static_cast<double>(size) / (1024.0 * 1024.0));

    GnssProcessor::RtcmStreamReader reader;
    reader.feed(buf.data(), buf.size());

    GnssProcessor::RtcmParser parser;
    std::map<int, std::size_t> msg_counts;
    std::size_t frames_decoded = 0;

    // Cumulative MSM7 observation counters (per satellite-system char).
    std::map<char, std::size_t> msm_obs_per_sys;
    std::map<std::string, std::size_t> msm_obs_per_sat;

    while (auto frame = reader.nextFrame()) {
        const auto pre_obs = parser.getObservations().size();
        const bool ok = parser.parseFrame(*frame);
        const int mt = parser.lastMessageType();
        ++msg_counts[mt];
        if (ok) ++frames_decoded;

        // Tally MSM7 observations from the latest decode.
        for (auto i = pre_obs; i < parser.getObservations().size(); ++i) {
            const auto& obs = parser.getObservations()[i];
            if (!obs.satellite.empty()) {
                ++msm_obs_per_sys[obs.satellite.front()];
                ++msm_obs_per_sat[obs.satellite];
            }
        }
    }

    std::printf("frames extracted: %zu  decoded: %zu  bytes discarded: %zu\n",
                reader.framesExtracted(), frames_decoded, reader.bytesDiscarded());

    // Histogram by message type, sorted ascending.
    std::printf("\nmessage-type counts:\n");
    for (const auto& [mt, n] : msg_counts) {
        std::printf("  %4d  %-26s  %zu\n", mt, msgTypeLabel(mt), n);
    }

    // Stations.
    if (!parser.getStationCoords().empty()) {
        std::printf("\nstation coordinates (1005/1006):\n");
        for (const auto& s : parser.getStationCoords()) {
            std::printf("  station %d  ARP=(%.4f, %.4f, %.4f) m  height=%.4f m\n",
                        s.reference_station_id, s.arp_ecef.x(), s.arp_ecef.y(),
                        s.arp_ecef.z(), s.antenna_height);
        }
    }

    // Latest SSR orbit correction (RAC) per satellite, from orbit messages, then from
    // combined messages for satellites without an orbit message.
    if (!parser.getSsrOrbits().empty() || !parser.getSsrCombined().empty()) {
        std::map<std::string, GnssProcessor::SsrOrbitCorrection> latest_orbit;
        for (const auto& o : parser.getSsrOrbits())   latest_orbit[o.sat] = o;
        std::map<std::string, GnssProcessor::SsrCombinedCorrection> latest_comb;
        for (const auto& c : parser.getSsrCombined()) latest_comb[c.sat] = c;

        std::printf("\nlatest SSR orbit per satellite (RAC, m):\n");
        for (const auto& [sat, o] : latest_orbit) {
            std::printf("  %-4s  IODE=%-4d  IOD-SSR=%-2d  RAC=(%+.4f, %+.4f, %+.4f)\n",
                        sat.c_str(), o.iode, o.iod_ssr,
                        o.delta_rac.x(), o.delta_rac.y(), o.delta_rac.z());
        }
        for (const auto& [sat, c] : latest_comb) {
            if (latest_orbit.count(sat)) continue; // already shown above
            std::printf("  %-4s  IODE=%-4d  IOD-SSR=%-2d  RAC=(%+.4f, %+.4f, %+.4f)  C0=%+.4f m  [combined]\n",
                        sat.c_str(), c.iode, c.iod_ssr,
                        c.delta_rac.x(), c.delta_rac.y(), c.delta_rac.z(), c.c0);
        }
    }

    // Latest SSR clock per satellite.
    if (!parser.getSsrClocks().empty()) {
        std::map<std::string, GnssProcessor::SsrClockCorrection> latest_clock;
        for (const auto& c : parser.getSsrClocks()) latest_clock[c.sat] = c;
        std::printf("\nlatest SSR clock per satellite (m, polynomial):\n");
        for (const auto& [sat, c] : latest_clock) {
            std::printf("  %-4s  IOD-SSR=%-2d  C0=%+.4f m  C1=%+.6e m/s  C2=%+.6e m/s^2\n",
                        sat.c_str(), c.iod_ssr, c.c0, c.c1, c.c2);
        }
    }

    // MSM7 observation summary.
    if (!msm_obs_per_sat.empty()) {
        std::printf("\nMSM7 observation summary:\n");
        for (const auto& [sys_char, n] : msm_obs_per_sys) {
            std::printf("  '%c' system: %zu satellite-epochs across all signals\n", sys_char, n);
        }
        std::printf("  unique satellites seen: %zu\n", msm_obs_per_sat.size());

        // Show the epoch counts of the 16 satellites with the most epochs.
        std::vector<std::pair<std::string, std::size_t>> sats_sorted(
            msm_obs_per_sat.begin(), msm_obs_per_sat.end());
        std::sort(sats_sorted.begin(), sats_sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        const std::size_t to_show = std::min<std::size_t>(sats_sorted.size(), 16);
        for (std::size_t i = 0; i < to_show; ++i) {
            std::printf("    %-4s  %zu epochs\n",
                        sats_sorted[i].first.c_str(), sats_sorted[i].second);
        }
        if (sats_sorted.size() > to_show) {
            std::printf("    (... and %zu more)\n", sats_sorted.size() - to_show);
        }
    }

    return 0;
}
