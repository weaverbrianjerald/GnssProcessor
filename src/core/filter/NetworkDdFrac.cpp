/**
 * @file NetworkDdFrac.cpp
 * @brief Implementation of @ref networkDdFrac — the SB_DDFRAC block relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkDdFrac.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkPivotHandover.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "math/Coordinates.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkDdFrac(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    const std::unique_ptr<PppUtils>& dd_ppp_utils) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    const auto& nc = config_.getNetworkConfig();
    auto& stations = setup.stations;
    auto& dd_frac_trace        = traces.dd_frac_trace;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    // Pure gauge-pin predicate (byte-copy of Application::networkIsGaugePinned_,
    // INC-7 H1 — member-state-free, so reproduced here as a local for the seam).
    auto networkIsGaugePinned_ = [](const std::vector<NetStation>& stations,
                                    bool phase_datum_prioritized, std::size_t i,
                                    const std::string& sat, const std::string& signal) -> bool {
        if (!phase_datum_prioritized) return stations[i].is_pivot;
        return stations[i].pinned_amb_targets.count(sat + "_" + signal) != 0;
    };


    // --- S1: the (cons, signal, sat) -> anchor STATION map -----------------------
    // docs/SPEC_lane_spectrometer.md §3.3 anchor partition.  Emitted HERE because this
    // seam already reconstructs the gauge-pin predicate and already walks every
    // station's ambiguity key map, so the map is read off the election that ran rather
    // than re-derived (a second derivation is a second thing that can disagree with
    // the datum).  Deliberately OUTSIDE the DD-fractional gate below: that block is
    // stride-decimated and stream-gated, while the anchor map is small (bounded by
    // gauge freedoms = satellites x signals, NOT by station pairs) and the partition
    // wants it on every processed epoch — the sticky tree re-elects often (47 tree
    // swaps / 31 epochs measured at 50 stations) and the grader takes the per-arc
    // MODE, which needs the full sequence.
    //
    // Note the complement: the DD-fractional emit SKIPS pinned ambiguities (see the
    // networkIsGaugePinned_ continue below), so no satellite can appear both as a
    // graded DD member and as its own anchor on the same signal — the two files are
    // exactly the two halves of the gauge.
    if (traces.phase_anchor_trace.is_open()) {
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0 || !stations[i].sm) continue;
            for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                (void)sidx;
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                if (!networkIsGaugePinned_(stations, phase_datum_prioritized,
                                           i, parts->satellite, parts->signal))
                    continue;
                const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                traces.phase_anchor_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << cst << ',' << parts->signal << ',' << parts->satellite << ','
                    << stations[i].name << '\n';
            }
        }
        // Flushed per epoch: this stream exists to grade runs that may be killed
        // mid-flight, and an unflushed tail is exactly the evidence that goes missing.
        traces.phase_anchor_trace.flush();
    }

    // --- Cross-station DD fractional-residual (B5) ----------------------
    // For each present station, back-substitute its local block and read each
    // ABSOLUTE float ambiguity (x_lin + x_r(col), m) per (sat,signal).  Within a
    // (const,signal), difference each sat against a reference sat (between-sat SD),
    // then difference that across station pairs → the between-station DD.  A DD near
    // an integer #cycles is evidence the float biases support user AR.  P1-2: the
    // fractional cycle (dd_cyc = dd_m/λ, frac_cyc = dd_cyc − round) is computed HERE
    // so the CSV is a ready fractional histogram.  EXCLUDED from the pool: the pivot
    // station (its ambiguities are pinned to zero — a degenerate DD) and GLONASS
    // (FDMA: no common λ per signal, between-sat SD not integer).  Read-only; gated.
    if (dd_frac_trace.is_open()
        && ((n_loop_epochs - 1)
            % static_cast<size_t>(std::max(1, nc.trace_dd_fractional_epoch_stride)) == 0)) {  // T2e stride
        auto sysOfChar = [](char c) -> System {
            switch (c) {
                case 'G': return System::GPS;      case 'E': return System::Galileo;
                case 'C': return System::BeiDou;   case 'J': return System::QZSS;
                case 'S': return System::SBAS;     default:  return System::Unknown;
            }
        };
        struct AmbAbs { double value_m; std::size_t est_id; Eigen::Index col; };  // T2a: +est_id,col
        // (const,signal) -> sat -> station index -> absolute float ambiguity (m) plus its
        // (estimator station id, local A_r column) for the T2a joint-covariance request.
        std::map<std::pair<std::string, std::string>,
                 std::map<std::string, std::map<size_t, AmbAbs>>> pool;
        const auto t_dd_pool0 = std::chrono::steady_clock::now();   // T2b timers
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            // Legacy: skip the whole pivot (all its ambiguities are pinned to
            // zero → degenerate DD).  Prioritized: keep the station and skip only
            // its per-(sat,signal) anchored (pinned) ambiguities below.
            if (!phase_datum_prioritized && stations[i].is_pivot) continue;
            Eigen::VectorXd x_r;
            try {
                x_r = est.reconstructStation(
                    static_cast<std::size_t>(est_index_of[i]), x_s);
            } catch (...) { continue; }
            const auto& h2l = asm_res[i].h2local;
            const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
            for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                if (networkIsGaugePinned_(stations, phase_datum_prioritized,
                                          i, parts->satellite, parts->signal))
                    continue;   // anchored (pinned to 0) → degenerate DD
                const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                if (cst == "R") continue;   // GLONASS FDMA: excluded (no common λ / integer DD)
                const int amb_idx = static_cast<int>(sidx);
                if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                const auto hit = h2l.find(amb_idx);
                if (hit == h2l.end()) continue;   // ambiguity column not in A_r this epoch
                if (hit->second < 0
                    || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                const double value_m = x_lin(amb_idx) + x_r(hit->second);
                if (!std::isfinite(value_m)) continue;
                pool[{cst, parts->signal}][parts->satellite][i] =
                    AmbAbs{value_m, static_cast<std::size_t>(est_index_of[i]),
                           static_cast<Eigen::Index>(hit->second)};
            }
        }
        const auto t_dd_pool1 = std::chrono::steady_clock::now();
        // --- T2a/T2b: collect the between-station DDs, (optionally) subsample, then
        //     propagate the joint conditional covariance ONCE to attach a per-DD formal σ
        //     (cycles).  The DD variance is gᵀ Q g with g=[+1,−1,−1,+1] over the four undiff
        //     ambiguities (sta_a/sat, sta_a/ref, sta_b/sat, sta_b/ref); only the diagonal is
        //     needed, so no dense nd×nd differencing matrix is materialised.  dd_m / dd_cyc /
        //     frac_cyc are computed EXACTLY as before (benchmark-identical); dd_sigma_cyc is
        //     appended (−1 = covariance unavailable this epoch).
        struct DdOut {
            std::string cons, signal, sat, ref;
            std::size_t sta_a_i, sta_b_i;
            double dd_m, lambda, dd_cyc, frac_cyc;
            std::pair<std::size_t, Eigen::Index> amb[4];  // ia(sat), ra(ref)@a, ib(sat), rb(ref)@b
            int rq[4]{ -1, -1, -1, -1 };                  // request indices, filled post-subsample
        };
        std::vector<DdOut> dds;
        for (auto& [cs, satmap] : pool) {
            std::string ref;   // reference sat = lexically-lowest sat present at ≥2 stations
            for (const auto& [sat, stamap] : satmap)
                if (stamap.size() >= 2) { ref = sat; break; }
            if (ref.empty()) continue;
            // λ for this (const,signal).  GLONASS is already excluded, so the nominal
            // per-band wavelength is satellite-independent; 0 (unknown signal) leaves the
            // cycle columns 0 so the reader falls back to the metre column.
            double lambda = 0.0;
            if (dd_ppp_utils) {
                const System sys = sysOfChar(cs.first.empty() ? '?' : cs.first[0]);
                if (sys != System::Unknown)
                    lambda = dd_ppp_utils->getWavelength(sys, cs.second, ref);
            }
            const auto& rmap = satmap[ref];
            for (const auto& [sat, stamap] : satmap) {
                if (sat == ref) continue;
                for (auto ia = stamap.begin(); ia != stamap.end(); ++ia) {
                    const auto ra = rmap.find(ia->first);
                    if (ra == rmap.end()) continue;
                    const double sd_a = ia->second.value_m - ra->second.value_m;
                    auto ib = ia; ++ib;
                    for (; ib != stamap.end(); ++ib) {
                        const auto rb = rmap.find(ib->first);
                        if (rb == rmap.end()) continue;
                        const double sd_b = ib->second.value_m - rb->second.value_m;
                        const double dd_m = sd_a - sd_b;   // between-station DD (m)
                        const double dd_cyc = (lambda > 0.0) ? (dd_m / lambda) : 0.0;
                        const double frac_cyc =
                            (lambda > 0.0) ? (dd_cyc - std::round(dd_cyc)) : 0.0;
                        DdOut d;
                        d.cons = cs.first; d.signal = cs.second; d.sat = sat; d.ref = ref;
                        d.sta_a_i = ia->first; d.sta_b_i = ib->first;
                        d.dd_m = dd_m; d.lambda = lambda; d.dd_cyc = dd_cyc; d.frac_cyc = frac_cyc;
                        d.amb[0] = { ia->second.est_id, ia->second.col };
                        d.amb[1] = { ra->second.est_id, ra->second.col };
                        d.amb[2] = { ib->second.est_id, ib->second.col };
                        d.amb[3] = { rb->second.est_id, rb->second.col };
                        dds.push_back(std::move(d));
                    }
                }
            }
        }
        const auto t_dd_collect = std::chrono::steady_clock::now();
        const std::size_t n_all_dd = dds.size();
        // T2b: deterministic seeded subsample keyed on (week,tow) — reproducible per epoch;
        // 0 = unlimited (full-pair benchmark).  Bounds BOTH the emit and the covariance-
        // request count (the dominant 305-station tracer cost).
        const int dd_max_pairs = nc.trace_dd_fractional_max_pairs;
        if (dd_max_pairs > 0 && dds.size() > static_cast<std::size_t>(dd_max_pairs)) {
            std::mt19937 rng(static_cast<unsigned>(se.time.week) * 604800u
                             + static_cast<unsigned>(se.time.tow));
            std::vector<std::size_t> idx(dds.size());
            for (std::size_t j = 0; j < idx.size(); ++j) idx[j] = j;
            std::shuffle(idx.begin(), idx.end(), rng);
            idx.resize(static_cast<std::size_t>(dd_max_pairs));
            std::sort(idx.begin(), idx.end());   // stable emit order
            std::vector<DdOut> kept; kept.reserve(idx.size());
            for (std::size_t j : idx) kept.push_back(std::move(dds[j]));
            dds.swap(kept);
        }
        // Deduplicated (station,col) request set over the (subsampled) DDs.
        std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
        std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
        auto reqOf = [&](const std::pair<std::size_t, Eigen::Index>& k) -> int {
            auto it = req_idx.find(k);
            if (it != req_idx.end()) return it->second;
            const int r = static_cast<int>(reqs.size());
            reqs.push_back(k); req_idx[k] = r; return r;
        };
        for (auto& d : dds)
            for (int p = 0; p < 4; ++p) d.rq[p] = reqOf(d.amb[p]);
        // Carry-aware readback (docs/SPEC_carry_aware_covariance.md §6).  OFF (default) is
        // the pre-card expression, verbatim.  ON, this σ is the σ of the estimator that
        // actually ran — which is what makes `med|frac|` conditioned on it a TEST rather
        // than a restatement (a uniform fractional part is the EXPECTED reading whenever
        // σ_DD ≳ 0.3 cyc, so the campaign's six-hour null needs a σ it can trust).
        const bool dd_carry_aware = ctx.knobs.carry_aware_covariance;
        Eigen::MatrixXd cov;
        if (!reqs.empty()) {
            try { cov = est.jointConditionalCovariance(reqs, plan.shared_datum, dd_carry_aware); }
            catch (...) { cov = Eigen::MatrixXd(); }
        }
        const bool cov_ok =
            (cov.rows() == static_cast<Eigen::Index>(reqs.size()) && cov.rows() > 0);
        const auto t_dd_cov = std::chrono::steady_clock::now();
        for (const DdOut& d : dds) {
            double dd_sigma_cyc = -1.0;   // −1 = covariance unavailable this epoch
            if (cov_ok && d.lambda > 0.0) {
                static const double g[4] = { +1.0, -1.0, -1.0, +1.0 };
                double var = 0.0;
                for (int p = 0; p < 4; ++p)
                    for (int q = 0; q < 4; ++q)
                        var += g[p] * g[q] * cov(d.rq[p], d.rq[q]);
                dd_sigma_cyc = (var > 0.0) ? std::sqrt(var) / d.lambda : 0.0;
            }
            dd_frac_trace
                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                << d.cons << ',' << d.signal << ',' << d.sat << ',' << d.ref << ','
                << stations[d.sta_a_i].name << ',' << stations[d.sta_b_i].name << ','
                << fmt::format("{:.4f}", d.dd_m) << ','
                << fmt::format("{:.6f}", d.lambda) << ','
                << fmt::format("{:.4f}", d.dd_cyc) << ','
                << fmt::format("{:.4f}", d.frac_cyc) << ','
                << fmt::format("{:.4f}", dd_sigma_cyc) << ','
                // INC-0: the column says WHICH covariance produced the σ next to it.  The
                // parent card's §11 is the lesson — a number published without its state
                // is an instrument that lies, and this exact σ was read as meaningful for
                // a whole campaign while it could not see 78 % of the carried state.
                << (dd_carry_aware ? 1 : 0) << '\n';
        }
        const auto t_dd_emit = std::chrono::steady_clock::now();
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        log->info("Network (T2b) DD-frac tracer TOW {:.1f}: pool {:.0f} ms ({} sta), "
                  "collect {:.0f} ms ({} DD{}), cov {:.0f} ms ({} reqs), emit {:.0f} ms",
                  se.time.tow, ms(t_dd_pool0, t_dd_pool1), stations.size(),
                  ms(t_dd_pool1, t_dd_collect), n_all_dd,
                  (dd_max_pairs > 0 && n_all_dd > dds.size())
                      ? fmt::format(" ->{} kept", dds.size()) : std::string(),
                  ms(t_dd_collect, t_dd_cov), reqs.size(), ms(t_dd_cov, t_dd_emit));
    }
}

} // namespace GnssProcessor
