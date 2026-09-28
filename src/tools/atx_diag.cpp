/**
 * @file atx_diag.cpp
 * @brief Dump parsed ATX antenna entries (PCO/PCV coverage) for a type pattern.
 *
 * Parses an ANTEX file through the production @c ProductParser and prints, for
 * every entry whose type contains the given substring, the PCO and PCV
 * frequency keys that are populated.  The output shows receiver and satellite
 * ATX coverage as the parser loaded it, for example when a receiver PCV is
 * empty or a signal is dropped under require_atx_coverage.
 *
 * Usage:  gnss_atx_diag <igs20.atx> [type-substring=LEIAR25]
 */

#include "products/ProductParser.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace GnssProcessor;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: gnss_atx_diag <igs20.atx> [type-substring]\n"); return 1; }
    const std::string atx = argv[1];
    const std::string want = (argc >= 3) ? argv[2] : "LEIAR25";

    GnssConfig config;
    LogManager  lm;
    ProductParser pp(config, lm);

    std::vector<AntennaData> ants;
    if (!pp.parseAtx(atx, ants)) { std::printf("FAILED to parse %s\n", atx.c_str()); return 1; }
    std::printf("parsed %zu antenna entries from %s\n\n", ants.size(), atx.c_str());

    int hits = 0;
    for (const auto& a : ants) {
        if (a.type.find(want) == std::string::npos) continue;
        ++hits;
        std::printf("type='%s'  dazi=%.1f zen1=%.1f dzen=%.1f valid_from(wk=%d)\n",
                    a.type.c_str(), a.dazi, a.zen1, a.dzen, a.valid_from.week);
        std::printf("    PCO keys:");
        for (const auto& [k, v] : a.pco)
            std::printf(" %s(%.2f,%.2f,%.2f)", k.c_str(), v.x(), v.y(), v.z());
        std::printf("\n    PCV keys:");
        for (const auto& [k, grid] : a.pcv) {
            std::size_t zen = grid.size();
            std::size_t az  = grid.empty() ? 0 : grid.front().size();
            std::printf(" %s[%zux%zu%s]", k.c_str(), zen, az, grid.empty() ? " EMPTY" : "");
        }
        std::printf("\n");
        // Report each frequency key with a PCO but a missing or empty PCV grid: one of
        // the conditions under which require_atx_coverage rejects a signal (the strict
        // check also needs a finite PCO on both the satellite and receiver entries).
        for (const auto& [k, v] : a.pco) {
            auto it = a.pcv.find(k);
            if (it == a.pcv.end() || it->second.empty())
                std::printf("    >>> %s has PCO but NO/EMPTY PCV  (would FAIL strict ATX coverage)\n", k.c_str());
        }
        std::printf("\n");
    }
    std::printf("%d entries matched '%s'\n", hits, want.c_str());
    return 0;
}
