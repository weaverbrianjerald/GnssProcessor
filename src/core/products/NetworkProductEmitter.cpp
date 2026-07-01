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

int NetworkProductEmitter::codeBiasDiscontinuityCounter(const std::string& sat,
                                                        const std::string& signal) const {
    const auto it = code_disc_counter_.find(sat + "|" + signal);
    return it != code_disc_counter_.end() ? it->second : 0;
}

void NetworkProductEmitter::emitToStore(const NetworkSolutionEpoch& sol,
                                        SsrCorrectionStore& store) {
    // --- Code-bias discontinuity pre-pass (H1) ----------------------------
    // Build this epoch's per-(constellation, signal) satellite set and each
    // satellite's distinct-band tuple FROM THE SOLUTION ITSELF, so the datum
    // re-reference detection is reproducible on replay with nothing extra
    // serialized.  The emitted code_bias_m keys are exactly the producer's
    // registered (sat, signal) shared columns — the same set the per-(const,
    // signal) sum-to-zero and the per-sat IF pin are built over — so a set or
    // band-tuple change here is precisely a datum re-reference.  The
    // constellation key is the satellite ID's leading char, matching
    // NetworkSbasis::constellationOf used by the producer's sum-to-zero rows.
    std::map<std::string, std::set<std::string>> cur_code_satset;  // "const|signal" -> sats
    std::map<std::string, std::vector<char>>     cur_band_tuple;   // "sat" -> sorted distinct band digits
    for (const auto& s : sol.sats) {
        if (s.code_bias_m.empty() || s.sat.empty()) continue;
        const char cst = s.sat[0];
        std::set<char> bands;
        for (const auto& [signal, bias] : s.code_bias_m) {
            (void)bias;
            cur_code_satset[std::string(1, cst) + "|" + signal].insert(s.sat);
            if (signal.size() >= 2) bands.insert(signal[1]);  // RINEX band digit
        }
        cur_band_tuple[s.sat].assign(bands.begin(), bands.end());  // std::set<char> is sorted-unique
    }
    // Channels whose satellite set changed vs the previous emit.  First-sighting
    // channels (absent from prev) are intentionally excluded — no step yet.
    std::set<std::string> changed_channels;
    for (const auto& [chan, sats] : cur_code_satset) {
        const auto it = prev_code_satset_.find(chan);
        if (it != prev_code_satset_.end() && it->second != sats)
            changed_channels.insert(chan);
    }

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
            rec.sat           = s.sat;
            rec.received_time = sol.epoch_time;
            rec.iod_ssr       = sol.iod_ssr;

            // The per-satellite ionosphere-free pin re-references when this
            // satellite's distinct-band set changes (a band drops, or it falls
            // to a single distinct frequency → the all-ones gauge), even with
            // NO (const,signal) membership change → step all its code biases.
            const auto bt_it = prev_band_tuple_.find(s.sat);
            const bool sat_band_changed =
                (bt_it != prev_band_tuple_.end() && cur_band_tuple[s.sat] != bt_it->second);

            for (const auto& [signal, bias] : s.code_bias_m) {
                const std::string key  = s.sat + "|" + signal;
                const std::string chan = std::string(1, s.sat[0]) + "|" + signal;

                // Bump on a forced (clock-pivot) datum change, a per-(const,
                // signal) sum-to-zero set re-reference, or this satellite's IF-
                // pin band-set change.  First sighting records the bias and
                // leaves the counter at 0 — the consumer's noteSsrDiscontinuity
                // reacts only to CHANGES, so a steady counter never triggers.
                int&       ctr  = code_disc_counter_[key];  // value-inits to 0 on first sight
                const bool seen = last_code_bias_.count(key) != 0;
                if (seen && (force_datum_bump_
                             || changed_channels.count(chan)
                             || sat_band_changed)) {
                    ++ctr;
                }
                last_code_bias_[key]        = bias;
                rec.bias_per_signal[signal] = bias;
                rec.disc_per_signal[signal] = ctr;
            }
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

    // Replace (never merge) the previous-epoch code-bias trackers with this
    // epoch's — a channel/satellite that vanishes then returns is a clean
    // first-sighting on both live and a fresh replay process, so the counters
    // reproduce deterministically.
    prev_code_satset_ = std::move(cur_code_satset);
    prev_band_tuple_  = std::move(cur_band_tuple);

    // The datum-change bump applies to exactly one emit (both phase and code).
    force_datum_bump_ = false;
}

} // namespace GnssProcessor
