/**
 * @file NetworkProductEmitter.cpp
 * @brief Implementation of the PPP-RTK network product emitter (Sink A).
 */

#include "products/NetworkProductEmitter.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/GnssTypes.hpp"

#include <cmath>

namespace GnssProcessor {
namespace {

/// Map a RINEX satellite-ID prefix to its constellation (for the SSR header).
System systemFromSat(const std::string& sat) {
    if (sat.empty()) return System::Unknown;
    switch (sat[0]) {
        case 'G': return System::GPS;
        case 'R': return System::GLONASS;
        case 'E': return System::Galileo;
        case 'C': return System::BeiDou;
        case 'J': return System::QZSS;
        case 'S': return System::SBAS;
        default:  return System::Unknown;
    }
}

} // namespace

NetworkProductEmitter::NetworkProductEmitter(double disc_threshold_m)
    : disc_threshold_m_(disc_threshold_m) {}

void NetworkProductEmitter::markDatumChange() { force_datum_bump_ = true; }

int NetworkProductEmitter::discontinuityCounter(const std::string& sat,
                                                const std::string& signal) const {
    const auto it = disc_counter_.find(sat + "|" + signal);
    return it != disc_counter_.end() ? it->second : 0;
}

void NetworkProductEmitter::emitToStore(const NetworkSolutionEpoch& sol,
                                        SsrCorrectionStore& store) {
    for (const auto& s : sol.sats) {
        SsrHeader header;
        header.epoch_time  = sol.epoch_time;
        header.iod_ssr     = sol.iod_ssr;
        header.provider_id = sol.provider_id;
        header.solution_id = sol.solution_id;
        header.system      = systemFromSat(s.sat);

        if (s.has_clock) {
            SsrClockCorrection rec;
            rec.sat           = s.sat;
            rec.c0            = s.clock_c0_m;
            rec.c1            = 0.0;   // ZOH (C0-only) for v1, matching the user extrapolation.
            rec.c2            = 0.0;
            rec.received_time = sol.epoch_time;
            rec.epoch_time    = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;
            store.pushClock(rec, header);
        }

        if (!s.code_bias_m.empty()) {
            SsrCodeBias rec;
            rec.sat             = s.sat;
            rec.bias_per_signal = s.code_bias_m;
            rec.received_time   = sol.epoch_time;
            rec.iod_ssr         = sol.iod_ssr;
            store.pushCodeBias(rec, header);
        }

        if (!s.phase_bias.empty()) {
            SsrPhaseBias rec;
            rec.sat           = s.sat;
            rec.received_time = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;
            for (const auto& [signal, npb] : s.phase_bias) {
                const std::string key = s.sat + "|" + signal;

                // Discontinuity bookkeeping (stream state the estimator can't
                // know): bump on the first jump beyond threshold, or on a forced
                // datum change.  First sighting records the bias and leaves the
                // counter at 0 — the consumer's noteSsrDiscontinuity only reacts
                // to CHANGES, so a steady counter never triggers a reset.
                int& ctr = disc_counter_[key];   // value-inits to 0 on first sight
                const auto last_it = last_phase_bias_.find(key);
                if (last_it != last_phase_bias_.end()) {
                    if (force_datum_bump_
                        || std::abs(npb.bias_m - last_it->second) > disc_threshold_m_) {
                        ++ctr;
                    }
                }
                last_phase_bias_[key] = npb.bias_m;

                SsrPhaseBiasEntry entry;
                entry.bias_m                = npb.bias_m;
                entry.signal_integer        = npb.signal_integer;
                entry.wide_lane_integer     = npb.wide_lane_integer;
                entry.discontinuity_counter = ctr;
                rec.bias_per_signal[signal] = entry;
            }
            store.pushPhaseBias(rec, header);
        }
    }

    // The datum-change bump applies to exactly one emit.
    force_datum_bump_ = false;
}

} // namespace GnssProcessor
