/**
 * @file NetworkClockEmitRebase.cpp
 * @brief Implementation of the clock emission re-base (Card CLK-R,
 *        docs/SPEC_clock_emit_rebase.md).
 */

#include "products/NetworkClockEmitRebase.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

void NetworkClockEmitRebase::rebaseEpoch(std::vector<Entry>& entries,
                                         double epoch_time_s,
                                         std::set<char>& reanchored) {
    if (entries.empty()) return;
    ++counters_.epochs;

    // Bucket this epoch's entries per constellation letter — the frame
    // accumulator, the μ vote and the leash are all per-constellation (the
    // clock-ensemble gauge is per-system; system time offsets differ).
    std::map<char, std::vector<Entry*>> by_const;
    for (auto& e : entries) {
        if (e.sat.empty()) continue;
        by_const[e.sat.front()].push_back(&e);
    }

    for (auto& [cst, group] : by_const) {
        double& acc = acc_[cst];   // value-inits to 0 on first sight

        // --- μ vote: median innovation of fresh, unforced keys against the
        //     previous EMITTED values.  Computed regardless of which stages are
        //     armed — stage 2 alone needs the same common-removed statistic the
        //     census uses (dev = step − median step).
        std::vector<double> innos;
        innos.reserve(group.size());
        for (const Entry* e : group) {
            if (e->forced) continue;
            const auto it = prev_.find(e->sat);
            if (it == prev_.end()) continue;
            if (cfg_.max_gap_s > 0.0
                && (epoch_time_s - it->second.last_seen_s) > cfg_.max_gap_s)
                continue;
            innos.push_back((e->c0_m - acc) - it->second.last_held_m);
        }
        double mu = 0.0;
        if (static_cast<int>(innos.size()) >= cfg_.min_common) {
            const std::size_t mid = innos.size() / 2;
            std::nth_element(innos.begin(), innos.begin() + mid, innos.end());
            mu = innos[mid];
        }

        // --- Stage 1: advance the frame accumulator; leash re-anchor.  When the
        //     hold is OFF the accumulator stays 0 (μ is a statistic only) and the
        //     emitted values pass through in the solve frame.
        bool reanchor = false;
        if (cfg_.frame_hold) {
            acc += mu;
            counters_.sum_abs_mu += std::abs(mu);
            if (std::abs(acc) > counters_.max_abs_acc) counters_.max_abs_acc = std::abs(acc);
            if (cfg_.leash_m > 0.0 && std::abs(acc) > cfg_.leash_m) {
                // Re-anchor to the solve frame: raw passthrough for the whole
                // constellation this epoch, accumulator cleared.  The caller
                // pairs this with markDatumChange — the marked re-coupling edge.
                reanchor = true;
                acc = 0.0;
                ++counters_.reanchors;
                reanchored.insert(cst);
            }
        }

        // --- Emit in the held frame; stage 2 step test on the common-removed
        //     step.  On a re-anchor epoch every key re-seeds (the datum mark
        //     covers the consumer) and the step test is skipped wholesale.
        for (Entry* e : group) {
            ++counters_.rows;
            const auto   it       = prev_.find(e->sat);
            const bool   have     = (it != prev_.end());
            const bool   stale    = have && cfg_.max_gap_s > 0.0
                                    && (epoch_time_s - it->second.last_seen_s) > cfg_.max_gap_s;
            const double held     = e->c0_m - acc;   // acc == 0 unless frame_hold

            if (!reanchor && have && !stale && !e->forced && cfg_.jump_test) {
                // dev = (held step) − μ residue: with the accumulator already
                // advanced by μ, the held innovation IS the common-removed step
                // when frame_hold is on; with it off, subtract μ explicitly.
                const double innov = held - it->second.last_held_m;
                const double dev   = cfg_.frame_hold ? innov : (innov - mu);
                if (std::abs(dev) > cfg_.jump_m) {
                    e->marked = true;
                    e->step_m = dev;
                    ++counters_.marks;
                }
            }
            if (!have) ++counters_.inits;
            else if (stale) ++counters_.gaps;

            e->c0_m = held;
            auto& st = prev_[e->sat];
            st.last_held_m = held;
            st.last_seen_s = epoch_time_s;
        }
    }
}

}  // namespace GnssProcessor
