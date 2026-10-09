/**
 * @file NetworkClockEmitRebase.cpp
 * @brief Implementation of the clock emission re-base (card CLK-R,
 *        docs/SPEC_clock_emit_rebase.md).
 */

#include "products/NetworkClockEmitRebase.hpp"

#include "common/GnssIdentity.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

void NetworkClockEmitRebase::rebaseEpoch(std::vector<Entry>& entries,
                                         double epoch_time_s,
                                         std::set<char>& reanchored) {
    if (entries.empty()) return;
    ++counters_.epochs;

    // Bucket this epoch's entries by constellation letter: the frame
    // accumulator, the μ vote and the leash are all per constellation (the
    // clock-ensemble gauge is per system, because system time offsets differ).
    std::map<char, std::vector<Entry*>> by_const;
    for (auto& e : entries) {
        if (e.sat.empty()) continue;
        by_const[letterOfSat(e.sat)].push_back(&e);
    }

    for (auto& [cst, group] : by_const) {
        double& acc = acc_[cst];   // value-inits to 0 on first sight
        ConstCounters& cc = const_counters_[cst];   // per-constellation tallies (card GAL)

        // --- μ vote: median innovation of fresh, unforced keys against the
        //     previous emitted values (after removing each key's declared level
        //     step).  Computed whichever stages are armed, because stage 2 alone
        //     needs the same common-removed statistic the census uses
        //     (dev = step − median step).  With fewer than min_common votes μ = 0.
        std::vector<double> innos;
        innos.reserve(group.size());
        for (const Entry* e : group) {
            if (e->forced) continue;
            const auto it = prev_.find(e->sat);
            if (it == prev_.end()) continue;
            if (cfg_.max_gap_s > 0.0
                && (epoch_time_s - it->second.last_seen_s) > cfg_.max_gap_s)
                continue;
            innos.push_back((e->c0_m - acc) - it->second.last_held_m - e->level_step_m);
        }
        double mu = 0.0;
        if (static_cast<int>(innos.size()) >= cfg_.min_common) {
            const std::size_t mid = innos.size() / 2;
            std::nth_element(innos.begin(), innos.begin() + mid, innos.end());
            mu = innos[mid];
        }

        // --- Stage 1: advance the frame accumulator; leash re-anchor.  When the
        //     hold is off the accumulator stays 0 (μ is a statistic only) and the
        //     emitted values pass through in the solve frame.
        bool reanchor = false;
        if (cfg_.frame_hold) {
            acc += mu;
            counters_.sum_abs_mu += std::abs(mu);
            if (std::abs(acc) > counters_.max_abs_acc) counters_.max_abs_acc = std::abs(acc);
            cc.sum_abs_mu += std::abs(mu);
            if (std::abs(acc) > cc.max_abs_acc) cc.max_abs_acc = std::abs(acc);
            if (cfg_.leash_m > 0.0 && std::abs(acc) > cfg_.leash_m) {
                // Re-anchor to the solve frame: raw passthrough for the whole
                // constellation this epoch, accumulator cleared.  The caller
                // pairs this with markDatumChange, which marks the re-coupling edge.
                reanchor = true;
                acc = 0.0;
                ++counters_.reanchors;
                ++cc.reanchors;
                reanchored.insert(cst);
            }
        }

        // --- Emit in the held frame; stage 2 step test on the common-removed
        //     step.  On a re-anchor epoch every key re-seeds (the datum mark
        //     covers the consumer) and the step test is skipped wholesale.
        for (Entry* e : group) {
            ++counters_.rows;
            ++cc.rows;
            const auto   it       = prev_.find(e->sat);
            const bool   have     = (it != prev_.end());
            const bool   stale    = have && cfg_.max_gap_s > 0.0
                                    && (epoch_time_s - it->second.last_seen_s) > cfg_.max_gap_s;
            const double held     = e->c0_m - acc;   // acc == 0 unless frame_hold

            if (!reanchor && have && !stale && !e->forced && cfg_.jump_test) {
                // dev = (held step) − μ: with frame_hold on, the accumulator has
                // already advanced by μ, so the held innovation is the
                // common-removed step; with it off, μ is subtracted explicitly.
                const double innov = held - it->second.last_held_m - e->level_step_m;
                const double dev   = cfg_.frame_hold ? innov : (innov - mu);
                if (std::abs(dev) > cfg_.jump_m) {
                    e->marked = true;
                    e->step_m = dev;
                    ++counters_.marks;
                    ++cc.marks;
                }
            } else if (!reanchor && have && stale && !e->forced && cfg_.jump_test
                       && cfg_.gap_jump_m > 0.0) {
                // Stage 2b (E1): a return after more than max_gap_s is tested
                // against the retained pre-gap baseline instead of being treated
                // as a first sighting.  With the frame hold on, the held frame is
                // consistent across the gap (the accumulator carried through the
                // absence), so the statistic is the common-removed step across the
                // gap: the eclipse-readmission re-realization (6.3–6.7 m measured
                // on the quiet bed), which stage 2 cannot see because it tests
                // fresh keys only.  The threshold is gap_jump_m, not jump_m:
                // legitimate motion over a long gap exceeds a one-epoch step, and
                // the caller sets the band.  These marks count in gap_marks only,
                // not in marks or the per-constellation tallies.
                const double innov = held - it->second.last_held_m - e->level_step_m;
                const double dev   = cfg_.frame_hold ? innov : (innov - mu);
                if (std::abs(dev) > cfg_.gap_jump_m) {
                    e->marked = true;
                    e->step_m = dev;
                    ++counters_.gap_marks;
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
