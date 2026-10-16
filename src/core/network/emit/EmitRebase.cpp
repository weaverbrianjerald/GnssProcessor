/**
 * @file EmitRebase.cpp
 * @brief Implementation of the emit-layer frame re-base (Card 4 (M1) phase D).
 */

#include "network/emit/EmitRebase.hpp"

#include "network/ar/ArcGraph.hpp"   // arcgraph::Dsu (card AGT)

#include <algorithm>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunGroups.hpp"
#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

void NetworkEmitRebase::reset() {
    last_emit_.clear();
    // Counters accumulate over the whole run (like the carry driver's).  A mid-run
    // reset() is a carry cold start, not a counter reset, so counters_ is left intact.
}

bool NetworkEmitRebase::lastEmittedForTest(const std::string& sat,
                                           const std::string& signal,
                                           double& value) const {
    const auto it = last_emit_.find(sat + "|" + signal);
    if (it == last_emit_.end()) return false;
    value = it->second;
    return true;
}

void NetworkEmitRebase::rebaseEpoch(std::vector<Entry>& entries) {
    if (entries.empty()) return;
    ++counters_.epochs;

    // Bucket entry indices by connected-component id.  The common fractional δ is
    // per component: the datum receiver r1 is elected per connected component of
    // the bipartite (rx,sat) graph, so its fractional lump is shared only within it.
    std::map<int, std::vector<std::size_t>> by_comp;
    for (std::size_t i = 0; i < entries.size(); ++i)
        by_comp[entries[i].component].push_back(i);

    for (const auto& [comp, idxs] : by_comp) {
        (void)comp;
        ++counters_.components;

        // Wavelength for the component: the first positive lambda_m (all its
        // entries share a signal ⇒ one λ; GLONASS FDMA is excluded upstream, so a
        // per-sat λ never arises here).  λ = 0 leaves the component un-re-based.
        double lambda = 0.0;
        for (const std::size_t i : idxs)
            if (entries[i].lambda_m > 0.0) { lambda = entries[i].lambda_m; break; }

        // The clean per-key steps Δ_s = raw_s − last_emit_s, over non-slipped,
        // baselined keys only, estimate the common fractional δ.  A slipped or
        // first-sighting key is excluded so it cannot pull δ (its own integer step
        // is preserved below).
        struct Clean { std::size_t i; double delta_step; };
        std::vector<Clean> clean;
        clean.reserve(idxs.size());
        for (const std::size_t i : idxs) {
            const Entry& e = entries[i];
            if (e.slipped) continue;
            const auto it = last_emit_.find(e.sat + "|" + e.signal);
            if (it == last_emit_.end()) continue;   // first sighting: not a δ witness
            clean.push_back({i, e.bias_m - it->second});
        }

        // Common fractional δ by robust coordinate descent: alternate integer
        // rounding (per key) with the median of the residuals (robust to the
        // minority of clean keys whose Δ carries a genuine per-sat integer, e.g.
        // an undetected reattach).  Two sweeps suffice: between 30 s epochs a
        // carried DD drifts ≪ ½ cycle, so the integers are unambiguous.
        double delta = 0.0;
        if (lambda > 0.0 && !clean.empty()) {
            std::vector<double> resid(clean.size());
            for (int sweep = 0; sweep < 2; ++sweep) {
                for (std::size_t j = 0; j < clean.size(); ++j) {
                    const double k = std::round((clean[j].delta_step - delta) / lambda);
                    resid[j] = clean[j].delta_step - k * lambda;
                }
                std::nth_element(resid.begin(), resid.begin() + resid.size() / 2,
                                 resid.end());
                delta = resid[resid.size() / 2];   // median residual = the shared fractional
            }
        }

        // Apply.  δ is common to the whole component, which keeps every satellite
        // (a fresh birth included) in one re-based frame; the per-sat integer is
        // removed only for a clean, baselined key.  A slipped key keeps k_s=0, so
        // its integer step survives and the emitter jump test fires.
        for (const std::size_t i : idxs) {
            Entry& e = entries[i];
            const std::string key = e.sat + "|" + e.signal;
            const auto it = last_emit_.find(key);
            const bool baselined = (it != last_emit_.end());

            long long k = 0;
            if (!e.slipped && baselined && lambda > 0.0)
                k = std::llround((e.bias_m - it->second - delta) / lambda);

            e.bias_m = e.bias_m - static_cast<double>(k) * lambda - delta;
            last_emit_[key] = e.bias_m;   // advance the emission frame (== emitter's last_phase_bias_)

            counters_.integer_cycles += std::llabs(k);
            if (k != 0) ++counters_.keys_rebased;
            if (e.slipped && baselined) ++counters_.slips_preserved;
        }

        counters_.frac_shift_abs_sum += std::abs(delta);
        ++counters_.frac_shift_n;
    }
}

std::map<std::string, int> networkEmitRebaseComponents(const EmitRebaseArcs& arcs) {
    std::map<std::string, int> comp_of;
    int next_comp = 0;
    for (const auto& [group, group_arcs] : arcs) {
        // Vertices: the stations by index, then the satellites by lexical rank.
        std::map<std::string, int> sat_vertex;
        int n_station_vertices = 0;
        for (const auto& [sta, sat] : group_arcs) {
            sat_vertex.emplace(sat, 0);
            n_station_vertices = std::max(n_station_vertices, sta + 1);
        }
        int rank = 0;
        for (auto& [sat, v] : sat_vertex) v = n_station_vertices + rank++;

        arcgraph::Dsu dsu;
        for (const auto& [sta, sat] : group_arcs)
            arcgraph::dsuUnite(dsu, sta, sat_vertex.at(sat));   // station, then satellite

        // One id per component, in the order of its lexically first satellite.
        std::map<int, int> id_of_root;
        for (const auto& [sat, v] : sat_vertex) {
            int& id = id_of_root[arcgraph::dsuFind(dsu, v)];
            if (id == 0) id = ++next_comp;
            comp_of[sat + "|" + group.second] = id;
        }
    }
    return comp_of;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the emit-layer frame re-base.
int networkEmitRebaseRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkEmitRebase& emit_rebase_engine = v.run.emit.rebase;
    const bool emit_rebase_on = v.ctx.knobs.emit_rebase_on;

    if (!emit_rebase_on) return 0;
    // Emit-layer re-base telemetry (Card 4 (M1) phase D, spec §3): the integer cycles
    // and common-fractional shift removed onto the previous emission frame (an
    // S-selection on outputs only; user DDs are invariant to it).  For the consumer
    // A/B: high integer/fractional churn with a silent disc counter is the datum
    // freedom the carry buys; a disc step that survives is a genuine event.
    const auto& rb = emit_rebase_engine.counters();
    log->info("Network (Card 4/M1 phase D emit re-base): {} emit epoch(s), {} component "
              "re-base(s), {} integer cycle(s) removed across {} key(s), {} reference-path "
              "slip step(s) preserved; mean |common-fractional δ| = {:.4f} m over the run "
              "(OUTPUTS-only S-selection; DDs invariant).",
              rb.epochs, rb.components, rb.integer_cycles, rb.keys_rebased,
              rb.slips_preserved,
              rb.frac_shift_n ? rb.frac_shift_abs_sum / static_cast<double>(rb.frac_shift_n)
                              : 0.0);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
