/**
 * @file ClockAnchor.cpp
 * @brief Implementation of the guarded temporal clock anchor (
 *        docs/SPEC_clock_temporal_anchor.md).  The design rationale, the two exact
 *        identities the guard rests on, and the invariant #1 and #7 contract are in
 *        the header's file comment.
 */

#include "network/solve/ClockAnchor.hpp"

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/carry/ClockClone.hpp"      // networkClockCloneRefloat
#include "network/solve/PivotHandover.hpp"   // datumChangeAffectsKey
#include "network/assembly/Responses.hpp"       // prmRespond / ResponseSite
#include "common/GnssIdentity.hpp"           // systemOfSat; phase-space statistic: band wavelengths

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

// The run-end reports at the end of this file:
#include "network/solve/Estimator.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunGroups.hpp"
#include <cstddef>
#include <memory>
#include <string>

#include "network/driver/Epoch.hpp"        // NetworkEpoch (the solve stages)
#include "network/driver/Run.hpp"          // NetworkRun
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum, NetworkPostFitHandoff
#include "network/diag/RunTraces.hpp"

namespace GnssProcessor {

namespace {

/// Maximum in-epoch withdraw→re-solve passes.  A pass can expose a second
/// satellite (removing one trip moves the per-constellation median), but the
/// target population is a handful of genuine events per day, so an epoch that
/// needs more than three passes points at a mis-specified anchor rather than three
/// data faults.  The cap is a backstop, and reaching it is logged.
constexpr int kCtaMaxPasses = 3;

/// Minimum fraction of the folded prior variance that the data must have removed
/// (`(P⁻ − P⁺)/P⁻`, the redundancy of the carried row) before a refloat is
/// admissible.  Below it the satellite's clock column is prior-dominated:
/// withdrawing its prior would leave a nearly unobservable column, which the
/// re-solve's relative-pivot guard would reject.  Such a satellite still decouples
/// and is still marked; only the same-epoch value correction is skipped, and the
/// skip is logged.
constexpr double kCtaMinCarryRedundancy = 0.05;

/// Comma-join a satellite set for a log line (fmt/ranges is not pulled in here).
std::string joinSats(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) { if (!out.empty()) out += ','; out += v; }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The gauge projection, shared by the guard and the census (§2.1).
// ---------------------------------------------------------------------------

std::vector<double> clockAnchorGaugeProject(const Eigen::MatrixXd& cov,
                                            const std::vector<Eigen::Index>& cols) {
    const std::size_t n = cols.size();
    if (n < 2) return {};                      // a lone clock is the gauge
    std::vector<double> rowmean(n, 0.0);
    double grand = 0.0;
    for (std::size_t a = 0; a < n; ++a) {
        double sum = 0.0;
        for (std::size_t b = 0; b < n; ++b) sum += cov(cols[a], cols[b]);
        rowmean[a] = sum / static_cast<double>(n);
        grand += sum;
    }
    grand /= static_cast<double>(n) * static_cast<double>(n);
    std::vector<double> out(n, 0.0);
    for (std::size_t a = 0; a < n; ++a)
        out[a] = cov(cols[a], cols[a]) - 2.0 * rowmean[a] + grand;
    return out;
}

std::vector<double> clockAnchorPhaseClockWeights(const std::vector<double>& alpha) {
    const std::size_t m = alpha.size();
    if (m < 2) return {};
    // w = A'(AA')^-1 (1, 0)' with A = [1'; alpha'] (the minimum-norm weights with
    // sum w = 1 and sum w alpha = 0).
    double s1 = 0.0, sa = 0.0, saa = 0.0;
    for (double a : alpha) { s1 += 1.0; sa += a; saa += a * a; }
    const double det = s1 * saa - sa * sa;
    if (!(std::abs(det) > 1e-12 * std::max(1.0, saa * s1))) return {};   // one band only
    // (AA')^-1 (1,0)' = (saa, -sa)' / det
    const double c1 = saa / det, c2 = -sa / det;
    std::vector<double> w(m);
    for (std::size_t j = 0; j < m; ++j) w[j] = c1 + c2 * alpha[j];
    return w;
}

std::map<std::string, ClockAnchorPhiRow> clockAnchorPhaseStatistic(
        const NetworkEstimator& est, const Eigen::VectorXd& x_s,
        const std::vector<NetworkEstimator::SharedPrior>& keys, const Eigen::MatrixXd& cov,
        double floor_m) {
    std::map<std::string, ClockAnchorPhiRow> out;
    if (keys.empty() || cov.rows() != static_cast<Eigen::Index>(keys.size())) return out;
    // Per satellite: its clock and phase-bias keys in the folded dense block.
    struct Sat { int ic{-1}; std::vector<int> ip; std::vector<std::string> sig; };
    std::map<std::string, Sat> by;
    for (std::size_t a = 0; a < keys.size(); ++a) {
        const auto& p = keys[a];
        if (p.sat.empty()) continue;
        if (p.kind == SharedParamKind::Clock && !p.clone) by[p.sat].ic = static_cast<int>(a);   // a clone is not the clock
        else if (p.kind == SharedParamKind::PhaseBias) {
            by[p.sat].ip.push_back(static_cast<int>(a));
            by[p.sat].sig.push_back(p.signal);
        }
    }
    const Eigen::MatrixXd& post = est.sharedCovariance();
    // The functional per satellite: coefficients over the folded keys and over the
    // shared columns.
    struct Fn { std::vector<std::pair<int, double>> ka; std::vector<std::pair<Eigen::Index, double>> sc; };
    std::map<System, std::vector<std::pair<std::string, Fn>>> by_const;
    for (auto& [sat, s0] : by) {
        if (s0.ic < 0 || s0.ip.size() < 2) continue;
        const System sys = systemOfSat(sat);
        // The physical band of one of the satellite's phase-bias signals (Unknown for a
        // signal the identity layer does not parse).
        const auto bandOf = [sys](const std::string& sig) {
            const auto signal = SignalId::fromObsCode(sys, sig);
            return signal ? signal->band() : FrequencyBand::Unknown;
        };
        // One phase bias per band: the best-determined one (smallest folded variance).  A
        // band with a second signal (GPS L1C and L1W) keeps its primary column; the
        // sparse family's column wanders with its own anchor, and any one signal per band
        // gives a functional blind to the level and to the ionosphere direction.  (A
        // signal choice.)
        Sat s;
        s.ic = s0.ic;
        {
            std::map<FrequencyBand, std::pair<int, std::string>> best;   // band -> (key index, signal)
            bool ok_band = true;
            for (std::size_t j = 0; j < s0.ip.size(); ++j) {
                const FrequencyBand band = bandOf(s0.sig[j]);
                const auto it = best.find(band);
                if (it == best.end()
                    || cov(s0.ip[j], s0.ip[j]) < cov(it->second.first, it->second.first))
                    best[band] = {s0.ip[j], s0.sig[j]};
                if (!(nominalWavelengthM(band) > 0.0))
                    ok_band = false;
            }
            if (!ok_band) continue;
            for (const auto& [band, kv] : best) {
                (void)band;
                s.ip.push_back(kv.first);
                s.sig.push_back(kv.second);
            }
        }
        if (s.ip.size() < 2) continue;
        std::vector<double> alpha;
        bool ok = true;
        for (const auto& sg : s.sig) {
            const double lam = nominalWavelengthM(bandOf(sg));
            if (!(lam > 0.0)) { ok = false; break; }
            alpha.push_back(lam * lam);
        }
        if (!ok) continue;
        const std::vector<double> w = clockAnchorPhaseClockWeights(alpha);
        if (w.empty()) continue;
        Fn fn;
        ClockAnchorPhiRow row;
        try {
            const Eigen::Index cc = est.clockColumn(sat);
            if (cc < 0 || cc >= x_s.size()) continue;
            double e = x_s(cc) - keys[static_cast<std::size_t>(s.ic)].value;
            double d = x_s(cc);                  // the posterior value
            fn.ka.emplace_back(s.ic, 1.0);
            fn.sc.emplace_back(cc, 1.0);
            for (std::size_t j = 0; j < s.ip.size(); ++j) {
                const Eigen::Index pc = est.phaseBiasColumn(sat, s.sig[j]);
                if (pc < 0 || pc >= x_s.size()) { ok = false; break; }
                e -= w[j] * (x_s(pc) - keys[static_cast<std::size_t>(s.ip[j])].value);
                d -= w[j] * x_s(pc);
                fn.ka.emplace_back(s.ip[j], -w[j]);
                fn.sc.emplace_back(pc, -w[j]);
            }
            if (!ok || !std::isfinite(e)) continue;
            row.e = e;
            row.d_post = d;
        } catch (...) { continue; }
        row.n_pb = static_cast<int>(s.ip.size());
        for (std::size_t j = 0; j < s.sig.size(); ++j)
            row.sigs += (j ? "+" : "") + s.sig[j];    // the functional's signals
        out[sat] = row;
        by_const[sys].emplace_back(sat, std::move(fn));
    }
    const double floor2 = floor_m * floor_m;
    for (auto& [c, v] : by_const) {
        (void)c;
        const std::size_t n = v.size();
        if (n < 2) { for (auto& [sat, fn] : v) out.erase(sat); continue; }   // no contrast
        Eigen::MatrixXd Cm(n, n), Cp(n, n);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = a; b < n; ++b) {
                double m = 0.0, q = 0.0;
                for (const auto& [ia, wa] : v[a].second.ka)
                    for (const auto& [ib, wb] : v[b].second.ka) m += wa * wb * cov(ia, ib);
                for (const auto& [ca, wa] : v[a].second.sc)
                    for (const auto& [cb, wb] : v[b].second.sc)
                        if (ca < post.rows() && cb < post.cols()) q += wa * wb * post(ca, cb);
                Cm(a, b) = Cm(b, a) = m;
                Cp(a, b) = Cp(b, a) = q;
            }
        std::vector<Eigen::Index> idx(n);
        for (std::size_t a = 0; a < n; ++a) idx[a] = static_cast<Eigen::Index>(a);
        const std::vector<double> pm = clockAnchorGaugeProject(Cm, idx);
        const std::vector<double> pp = clockAnchorGaugeProject(Cp, idx);
        std::vector<double> ev;
        ev.reserve(n);
        for (const auto& [sat, fn] : v) { (void)fn; ev.push_back(out[sat].e); }
        std::vector<double> tmp = ev;
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n / 2), tmp.end());
        const double med = tmp[n / 2];
        // The posterior value's constellation median (the guard reads e_hat).
        std::vector<double> dv;
        dv.reserve(n);
        for (const auto& [sat, fn] : v) { (void)fn; dv.push_back(out[sat].d_post); }
        std::nth_element(dv.begin(), dv.begin() + static_cast<std::ptrdiff_t>(n / 2), dv.end());
        const double dmed = dv[n / 2];
        for (std::size_t a = 0; a < n; ++a) {
            ClockAnchorPhiRow& r = out[v[a].first];
            const double s = pm[a] - pp[a];
            r.Sm = pm[a];
            r.Sp = pp[a];
            r.d_post_hat = r.d_post - dmed;
            r.floored = !(s > floor2);
            r.S = std::max(floor2, s);
            r.e_hat = r.e - med;
            r.ok = std::isfinite(r.S) && std::isfinite(r.e_hat);
        }
    }
    return out;
}

std::vector<double> clockAnchorGaugeProjectDiag(const std::vector<double>& var) {
    const std::size_t n = var.size();
    if (n < 2) return {};
    const double dn = static_cast<double>(n);
    double sum = 0.0;
    for (double v : var) sum += v;
    const double grand = sum / (dn * dn);
    std::vector<double> out(n, 0.0);
    for (std::size_t a = 0; a < n; ++a)
        out[a] = var[a] - 2.0 * var[a] / dn + grand;   // rowmean_a = var[a]/n on a diagonal
    return out;
}

// ---------------------------------------------------------------------------
// Fold side: the variance decision and the per-satellite bookkeeping.
// ---------------------------------------------------------------------------

void clockAnchorBeginEpoch(NetworkRunContext& ctx) {
    if (!ctx.knobs.cta_on) return;
    for (auto& kv : ctx.carry.clk_anchor.sat) {
        kv.second.folded          = false;
        kv.second.folded_anchored = false;
        kv.second.stale_pending   = false;
    }
    ctx.carry.clk_anchor.tripped.clear();
}

ClockAnchorFold clockAnchorFoldVariance(NetworkRunContext& ctx,
                                        const std::string& sat,
                                        double prev_var, double dt_epoch,
                                        double pl_now_s, double value,
                                        double seed_horizon_s) {
    // The decision and the bookkeeping, in one call (split so the
    // decision can be made before PASS B and the bookkeeping at the fold).  Off: the legacy
    // expression prev_var + q_clock * dt, and nothing is written.
    const ClockAnchorFold out =
        clockAnchorFoldDecide(ctx, sat, prev_var, dt_epoch, pl_now_s, seed_horizon_s);
    clockAnchorFoldCommit(ctx, sat, out, pl_now_s, value);
    return out;
}

ClockAnchorFold clockAnchorFoldDecide(const NetworkRunContext& ctx, const std::string& sat,
                                      double prev_var, double dt_epoch, double pl_now_s,
                                      double seed_horizon_s) {
    // The anchor's rule, on a copy of the state.  The per-satellite elapsed time, not the
    // driver's dt: every skip path advances prev_tow while this carry survives untouched, so
    // dt understates a stale carry's age without bound (a first sighting has no elapsed time
    // and falls back to dt; it is LOOSE in any case).  A re-admission seed is stale only
    // beyond the seed horizon: the gap it spans is the evidence gap the park holds, and the
    // guard (not the staleness bound) re-validates it; a LOOSE seed inflates over the true
    // gap.  A stale anchored carry decouples: detected here because it decides the variance,
    // while the state transition, the event and the wire mark belong to the guard, so every
    // decoupling edge goes through one path with one identity.  LOOSE folds at the legacy
    // rate, which bounds a decoupled satellite (standing invariant #7: it is on the carry the
    // clock_runaway_check monitor already bounds).  The increment is the product the fold
    // adds to the carried variance, so clockAnchorFoldAt re-forms the variance on any carried
    // variance bit for bit.  A satellite with no entry yet is a default-constructed one (born
    // LOOSE), which is what the commit's operator[] creates.
    ClockAnchorFold out;
    const double dt_leg = std::max(0.0, dt_epoch);
    out.inc_m2    = ctx.params.q(SharedParamKind::Clock) * dt_leg;
    out.dt_s      = dt_leg;
    out.rate_m2ps = ctx.params.q(SharedParamKind::Clock);
    if (!ctx.knobs.cta_on) {
        out.var_eff = prev_var + out.inc_m2;
        return out;
    }
    ClockAnchorSat st;
    const auto it = ctx.carry.clk_anchor.sat.find(sat);
    if (it != ctx.carry.clk_anchor.sat.end()) st = it->second;
    const double dt_sat = (st.t_last > 0.0) ? (pl_now_s - st.t_last) : dt_leg;
    const double gap_bound = (seed_horizon_s > ctx.knobs.cta_max_gap_s)
                                 ? seed_horizon_s : ctx.knobs.cta_max_gap_s;
    const bool stale = !(dt_sat > 0.0)
                    || (ctx.knobs.cta_max_gap_s > 0.0 && dt_sat > gap_bound);
    if (seed_horizon_s > 0.0 && dt_sat > 0.0) {
        out.inc_m2 = ctx.params.q(SharedParamKind::Clock) * dt_sat;
        out.dt_s   = dt_sat;
    }
    if (stale && st.anchored) out.decoupled = true;   // stale_pending, cleared by BeginEpoch
    if (st.anchored && !out.decoupled) {
        out.inc_m2    = ctx.knobs.cta_q * dt_sat;
        out.dt_s      = dt_sat;
        out.rate_m2ps = ctx.knobs.cta_q;
        out.anchored  = true;
    }
    return clockAnchorFoldAt(ctx, out, prev_var);
}

ClockAnchorFold clockAnchorFoldAt(const NetworkRunContext& ctx, const ClockAnchorFold& f,
                                  double prev_var) {
    ClockAnchorFold out = f;
    out.var_eff   = prev_var + f.inc_m2;
    out.floor_hit = false;
    if (f.anchored && ctx.knobs.cta_var_floor > 0.0 && out.var_eff < ctx.knobs.cta_var_floor) {
        out.var_eff   = ctx.knobs.cta_var_floor;
        out.floor_hit = true;
    }
    return out;
}

void clockAnchorFoldCommit(NetworkRunContext& ctx, const std::string& sat,
                           const ClockAnchorFold& f, double pl_now_s, double value) {
    if (!ctx.knobs.cta_on) return;
    ClockAnchorSat& st = ctx.carry.clk_anchor.sat[sat];
    // The decoupling edge is detected by the decision because it decides the variance; the
    // state transition, the event and the wire mark belong to the guard.
    if (f.decoupled) st.stale_pending = true;
    if (f.anchored) {
        if (f.floor_hit) ++ctx.counters.cta_floor_hits;
        ++ctx.counters.cta_folded_anchored;
        ++ctx.counters.mgnss.anchor_folds_anchored[constellationKey(systemOfSat(sat))];   // per constellation
    } else {
        ++ctx.counters.cta_folded_loose;
        ++ctx.counters.mgnss.anchor_folds_loose[constellationKey(systemOfSat(sat))];      // per constellation
    }
    st.t_last          = pl_now_s;
    st.last_value      = value;
    st.last_var_eff    = f.var_eff;
    st.folded          = true;
    st.folded_anchored = f.anchored;
}

// ---------------------------------------------------------------------------
// Guard side.
// ---------------------------------------------------------------------------

namespace {

/// The decoupling edge: state transition, event and wire mark, in one path.
/// Returns true when an edge fired (the satellite was ANCHORED).
bool decouple(NetworkRunContext& ctx, NetworkRunTraces& traces, const GnssTime& t,
              const std::string& sat, const char* reason,
              std::optional<double> statistic, std::optional<double> threshold,
              const std::string& detail) {
    ClockAnchorSat& st = ctx.carry.clk_anchor.sat[sat];
    const bool was_anchored = st.anchored;
    st.anchored = false;
    st.in_band  = 0;
    if (!was_anchored) return false;   // already LOOSE: no edge, nothing to mark
    // One ANCHORED -> LOOSE edge of one satellite, whatever the cause
    // (gap, datum, ext, statistic): every decoupling edge goes through this path.
    prmRespond(ctx.counters.prm_resp, ResponseSite::ClkAnchorDecouple);
    ctx.carry.clk_anchor.tripped.insert(sat);
    ++ctx.counters.mgnss.anchor_trips[constellationKey(systemOfSat(sat))];   // per constellation
    // Re-admission continuity: a trip within the verify window of a clock seed is the
    // data rejecting the seeded pre-gap realization (invariant #7's mark-if-moved),
    // attributed in the event detail and counted.
    std::string seed_tag;
    if (net::present(ctx.entities.sats, &net::SatelliteState::readmit_seeded_s) > 0) {
        const double* rs = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::readmit_seeded_s);
        if (rs && ctx.carry.readmit_now_s - *rs <= kReadmitVerifyWindowS) {
            ++ctx.counters.n_readmit_seed_moved;
            seed_tag = ";readmit_seed_moved";
        }
    }
    traces.emitEvent(t, "CLK_ANCHOR", "cta_decouple", sat, "", statistic, threshold,
                     "refloat", fmt::format("reason={};{}{}", reason, detail, seed_tag));
    if (ctx.knobs.cta_mark) {
        net::markSat(ctx.entities, sat, net::Disc::ClockAnchor);
        ++ctx.counters.cta_marks;
    }
    return true;
}

}  // namespace

void networkClockAnchorGuard(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const GnssTime& se_time,
                             NetworkEstimator& est,
                             const NetworkSbasis::Plan& plan,
                             Eigen::VectorXd& x_s, EpochWorkspace& ws,
                             const std::vector<std::string>& registered_clock_sats,
                             const std::set<std::string>& clock_refloat_sats,
                             bool datum_changed,
                             const std::set<char>& changed_consts, const EpochEvents& events) {
    if (!ctx.knobs.cta_on) return;
    (void)config_;   // the seam signature is uniform across the run loop
    auto& anchor = ctx.carry.clk_anchor;
    ++ctx.counters.cta_epochs;

    // --- Non-statistical decoupling causes, applied before the test ----------
    // In each case the carry this satellite would be tested against is not the
    // anchored carry, so testing it would only produce a misattributed statistic.
    for (const std::string& sat : registered_clock_sats) {
        const auto it = anchor.sat.find(sat);
        if (it == anchor.sat.end() || !it->second.stale_pending) continue;
        ++ctx.counters.cta_trip_gap;
        decouple(ctx, traces, se_time, sat, "gap", std::nullopt,
                 ctx.knobs.cta_max_gap_s,
                 "carried clock older than the staleness bound");
    }
    if (datum_changed) {
        for (const std::string& sat : registered_clock_sats) {
            const auto it = anchor.sat.find(sat);
            if (it == anchor.sat.end() || !it->second.anchored) continue;
            if (!datumChangeAffectsKey(sat, changed_consts, ctx.knobs.scoped_handover))
                continue;
            // Datum keep (network.clock_anchor_datum_keep): the constellation's carry was
            // re-expressed exactly this epoch (the dense kernel, its self-check passed), this
            // clock has a row in that block, and its fold was the anchored one on the
            // re-expressed entry: the carry is in the incoming frame, so the frame did not
            // retire and the anchor stays engaged.  The statistic below still tests it.
            if (ctx.knobs.cta_datum_keep) {
                const char cons = letterOfSat(sat);   // the dense kernel's key
                const auto rec = events.handover.sts_dense.find(cons);
                const bool keep = rec != events.handover.sts_dense.end()
                               && events.handover.sts_consts.count(cons) > 0
                               && rec->second.selfcheck_ok
                               && rec->second.row_sats.count(sat) > 0
                               && it->second.folded && it->second.folded_anchored;
                if (keep) {
                    ++ctx.counters.cta_datum_kept;
                    traces.emitEvent(se_time, "CLK_ANCHOR", "cta_datum_kept", sat, "",
                                     std::nullopt, std::nullopt, "keep",
                                     fmt::format("reason=datum;q={};p={};dense=1",
                                                 rec->second.q,
                                                 rec->second.p.empty() ? std::string("none")
                                                                       : rec->second.p));
                    continue;
                }
                ++ctx.counters.cta_datum_kept_refused;
            }
            ++ctx.counters.cta_trip_datum;
            decouple(ctx, traces, se_time, sat, "datum", std::nullopt, std::nullopt,
                     "retired clock frame");
        }
    }
    for (const std::string& sat : clock_refloat_sats) {
        const auto it = anchor.sat.find(sat);
        if (it == anchor.sat.end() || !it->second.anchored) continue;
        ++ctx.counters.cta_trip_ext;
        decouple(ctx, traces, se_time, sat, "ext", std::nullopt, std::nullopt,
                 "external QC tier refloated this satellite");
    }

    // --- The test -----------------------------------------------------------
    const double k       = ctx.knobs.cta_k;
    const double abs_m   = ctx.knobs.cta_abs_m;
    const double s_floor = std::max(1e-12, ctx.knobs.cta_sigma_floor_m
                                         * ctx.knobs.cta_sigma_floor_m);
    // The resolved predicate, not the Q3 knob.  A withdraw must un-fold exactly what
    // was folded; reading a different flag than the fold used leaves a
    // negative-semidefinite residue in the shared normals (see RunContext.hpp).
    const bool gf         = ctx.knobs.carry_gauge_free;     // the fold (what a withdraw must un-fold)
    const bool proj_post  = ctx.knobs.cta_stat_proj_post;   // the denominator's posterior half
    const bool proj_prior = ctx.knobs.cta_stat_proj_prior;  // …and its prior half
    int pass = 0;
    for (; pass < kCtaMaxPasses; ++pass) {
        const Eigen::MatrixXd& cov = est.sharedCovariance();
        struct Row { std::string sat; double e{}, S{}, post_var{}, var_eff{}; bool anchored{}; };
        std::vector<Row> rows;
        std::map<System, std::vector<double>> e_by_const;
        // The statistic is on the common-removed residual, so both halves of
        // S = P⁻ − P⁺ have to be the variance of that contrast rather than of the
        // raw state (@ref clockAnchorGaugeProject documents why, and
        // why the raw marginal is wrong for either fold).  Which halves are
        // projected is set by `network.clock_anchor_gauge_free_stat`, resolved into
        // the two predicates above.  It is a bisection knob: the gauge-free arm
        // changed the fold and this denominator in one step, and the knob separates
        // the two.
        std::map<std::string, double> post_gf, prior_gf;
        if (proj_post || proj_prior) {
            std::map<System, std::vector<std::pair<std::string, Eigen::Index>>> by_c;
            for (const std::string& sat : registered_clock_sats) {
                if (sat.empty()) continue;
                const auto it = anchor.sat.find(sat);
                if (it == anchor.sat.end() || !it->second.folded) continue;
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                if (col >= 0 && col < cov.rows() && col < cov.cols())
                    by_c[systemOfSat(sat)].emplace_back(sat, col);
            }
            for (const auto& [c, v] : by_c) {
                (void)c;
                if (v.size() < 2) continue;   // no estimable contrast: keep the raw marginals
                if (proj_post) {
                    std::vector<Eigen::Index> cols;
                    cols.reserve(v.size());
                    for (const auto& m : v) cols.push_back(m.second);
                    const std::vector<double> pv = clockAnchorGaugeProject(cov, cols);
                    for (std::size_t a = 0; a < pv.size(); ++a) post_gf[v[a].first] = pv[a];
                }
                if (proj_prior) {
                    // The prior side uses the variance the fold applied, read back
                    // per satellite and not re-derived: a re-derived variance can
                    // differ from the one the mechanism applied.  The census makes
                    // the same choice.
                    std::vector<double> vv;
                    vv.reserve(v.size());
                    for (const auto& m : v) vv.push_back(anchor.sat[m.first].last_var_eff);
                    const std::vector<double> pv = clockAnchorGaugeProjectDiag(vv);
                    for (std::size_t a = 0; a < pv.size(); ++a) prior_gf[v[a].first] = pv[a];
                }
            }
        }
        rows.reserve(registered_clock_sats.size());
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            const auto it = anchor.sat.find(sat);
            if (it == anchor.sat.end() || !it->second.folded) continue;  // no standing prior ⇒ no predicted residual
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (col < 0 || col >= x_s.size()) continue;
            const double z = x_s(col);
            if (!std::isfinite(z)) continue;
            Row r;
            r.sat      = sat;
            r.e        = z - it->second.last_value;
            r.var_eff  = it->second.last_var_eff;
            r.post_var = (col < cov.rows() && col < cov.cols()) ? cov(col, col) : 0.0;
            if (proj_post) {
                const auto pit = post_gf.find(sat);
                if (pit != post_gf.end() && std::isfinite(pit->second) && pit->second >= 0.0)
                    r.post_var = pit->second;
            }
            if (proj_prior) {
                const auto qit = prior_gf.find(sat);
                if (qit != prior_gf.end() && std::isfinite(qit->second) && qit->second > 0.0)
                    r.var_eff = qit->second;
            }
            // Var(x̂ − μ) = P⁻ − P⁺ exactly.  The floor is a lower bound on the
            // denominator, not an addition to it (adding it is the pre-B3 data-incest
            // form).  A weak-data epoch removes no information, so S → 0 and the
            // unfloored statistic would divide 0 by 0 on the lowest-confidence
            // satellites.
            r.S        = std::max(s_floor, r.var_eff - r.post_var);
            r.anchored = it->second.folded_anchored;
            if (!std::isfinite(r.e) || !std::isfinite(r.S)) continue;
            rows.push_back(r);
            e_by_const[systemOfSat(sat)].push_back(r.e);
        }
        if (rows.empty()) break;
        // The per-constellation common mode is the free per-epoch clock gauge (rx
        // clocks are white, q = 1000).  Removing it is standing invariant #5:
        // without it every satellite of a re-realizing constellation reports the
        // gauge shift and the guard trips on all of them.
        std::map<System, double> med;
        for (auto& [c, v] : e_by_const) {
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                             v.end());
            med[c] = v[v.size() / 2];
        }

        // The statistic in the phase-visible complement.  Empty
        // (and nothing computed) with network.clock_anchor_guard_space = clock.
        const std::map<std::string, ClockAnchorPhiRow> phi =
            (ctx.knobs.cta_space != 0)
                ? clockAnchorPhaseStatistic(est, x_s, ws.cap_dense_keys, ws.cap_dense_cov,
                                            ctx.knobs.cta_phi_floor_m)
                : std::map<std::string, ClockAnchorPhiRow>{};
        std::set<std::string> trip;          // anchored satellites that failed
        std::set<std::string> trip_resolve;  // …whose prior may also be withdrawn
        for (const Row& r : rows) {
            const double eh = r.e - med[systemOfSat(r.sat)];
            const double w  = eh / std::sqrt(r.S);
            const double aw = std::abs(w);
            const auto pit = phi.find(r.sat);
            const bool phi_ok = pit != phi.end() && pit->second.ok;
            const double w_phi = phi_ok ? pit->second.e_hat / std::sqrt(pit->second.S) : 0.0;
            if (aw > ctx.counters.cta_max_w) {
                ctx.counters.cta_max_w     = aw;
                ctx.counters.cta_max_w_tow = se_time.tow;
                ctx.counters.cta_max_w_sat = r.sat;
            }
            const bool bad_clk = aw > k;
            // Phase mode: the phase-visible statistic decides the NIS trip (the level
            // cannot trip it); a row without one is tested on the absolute backstop only.
            // Shadow and clock modes: the clock statistic decides, as before.
            const bool bad_nis = (ctx.knobs.cta_space == 2) ? (phi_ok && std::abs(w_phi) > k)
                                                            : bad_clk;
            const bool bad_abs = abs_m > 0.0 && std::abs(eh) > abs_m;
            if (ctx.knobs.cta_space != 0 && pass == 0 && r.anchored) {
                auto& C = ctx.counters;
                if (phi_ok) {
                    ++C.cta_phi_rows;
                    C.cta_phi_sum_w2 += w_phi * w_phi;
                    if (pit->second.floored) ++C.cta_phi_floor_rows;
                    for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
                        if (std::abs(w_phi) >= kCtaWLadder[b]) ++C.cta_phi_w_ge[b];
                    for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
                        if (std::abs(pit->second.e_hat) >= kCtaELadder[b]) ++C.cta_phi_e_ge[b];
                    const bool bad_phi = std::abs(w_phi) > k;
                    if (bad_clk && bad_phi) ++C.cta_x_both;
                    else if (bad_clk)       ++C.cta_x_clk_only;
                    else if (bad_phi)       ++C.cta_x_phi_only;
                } else {
                    ++C.cta_phi_unavail;
                }
            }
            if (ctx.knobs.cta_space != 0 && pass == 0 && traces.anchor_guard_trace.is_open())
                traces.anchor_guard_trace
                    << se_time.week << ',' << fmt::format("{:.3f}", se_time.tow) << ','
                    << r.sat << ',' << (r.anchored ? 1 : 0) << ','
                    << fmt::format("{:.5f},{:.5f},{:.3f},", eh, std::sqrt(r.S), w)
                    << (phi_ok ? fmt::format("{:.5f},{:.5f},{:.3f},{}", pit->second.e_hat,
                                             std::sqrt(pit->second.S), w_phi, pit->second.n_pb)
                               : std::string(",,,0"))
                    << ',' << ((bad_nis || bad_abs) ? (r.anchored ? "trip" : "out") : "ok")
                    << '\n';
            // The k derivation of §3 needs the realized distribution, not only its
            // max: the denominator √S shrinks as q tightens, so a fixed k means a
            // different number of metres at every rung of the ladder.  Accumulated
            // over anchored rows on the first pass only; a later pass re-tests the
            // survivors against a moved median and would count them twice.  The
            // population is still truncated across epochs by the guard's own action;
            // the untruncated null needs a k-disarmed arm (see the counter docs).
            if (pass == 0 && r.anchored) {
                const double sqrt_s = std::sqrt(r.S);
                ++ctx.counters.cta_w_rows;
                ctx.counters.cta_sum_sqrt_s += sqrt_s;
                ctx.counters.cta_sum_abs_e  += std::abs(eh);
                ctx.counters.cta_sum_w2     += w * w;
                for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
                    if (aw >= kCtaWLadder[b]) ++ctx.counters.cta_w_ge[b];
                for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
                    if (std::abs(eh) >= kCtaELadder[b]) ++ctx.counters.cta_e_ge[b];
            }
            if (r.anchored) {
                if (!bad_nis && !bad_abs) continue;
                if (bad_nis) ++ctx.counters.cta_trip_nis;
                else         ++ctx.counters.cta_trip_abs;
                if (bad_nis && ctx.knobs.cta_space == 2) ++ctx.counters.cta_trip_phi;
                decouple(ctx, traces, se_time, r.sat,
                         bad_nis ? (ctx.knobs.cta_space == 2 ? "phi" : "nis") : "abs",
                         (ctx.knobs.cta_space == 2 && bad_nis) ? std::abs(w_phi) : aw,
                         bad_nis ? k : abs_m,
                         phi_ok ? fmt::format("e={:.4f};med={:.4f};sqrtS={:.4f};pass={};"
                                              "e_phi={:.4f};sqrtS_phi={:.4f}",
                                              r.e, med[systemOfSat(r.sat)], std::sqrt(r.S), pass,
                                              pit->second.e_hat, std::sqrt(pit->second.S))
                                : fmt::format("e={:.4f};med={:.4f};sqrtS={:.4f};pass={}",
                                              r.e, med[systemOfSat(r.sat)], std::sqrt(r.S), pass));
                trip.insert(r.sat);
                // Admission screen for the same-epoch value correction only; the
                // decouple and the mark are unconditional.
                const double redun = (r.var_eff > 0.0)
                                         ? (r.var_eff - r.post_var) / r.var_eff : 0.0;
                if (redun >= kCtaMinCarryRedundancy) trip_resolve.insert(r.sat);
            } else {
                // LOOSE: the invariant #7 re-validation.  The same statistic,
                // against the loose fold's own null: the question at this edge is
                // whether the event is over, not whether the clock is quiet at 2 cm
                // (the re-seed and the next anchored pass answer that).
                ClockAnchorSat& st = anchor.sat[r.sat];
                if (bad_nis || bad_abs) { st.in_band = 0; continue; }
                ++st.in_band;
                if (st.in_band >= std::max(1, ctx.knobs.cta_recouple_ep)) {
                    // A re-coupling is a LOOSE -> ANCHORED edge of a
                    // satellite the guard anchored before.  A satellite is born LOOSE, so
                    // its first engagement takes this path too (cta_recouples counts it)
                    // and is not a response; ever_anchored separates the two and is read
                    // by nothing else.
                    const bool prm_recouple = st.ever_anchored && !st.anchored;
                    st.ever_anchored = true;
                    st.anchored = true;
                    st.in_band  = 0;
                    ++ctx.counters.cta_recouples;
                    if (prm_recouple)
                        prmRespond(ctx.counters.prm_resp, ResponseSite::ClkAnchorRecouple);
                    // Not marked on the wire: re-engaging a prior whose mean is the
                    // last posterior adds information without stepping the emitted
                    // value, so a consumer has nothing to reset.  It is an event with
                    // an identity and its evidence, as invariant #7 asks of a
                    // re-coupling edge, and the next fold re-seeds from the LOOSE
                    // posterior variance instead of resuming a steady-state tight one.
                    traces.emitEvent(se_time, "CLK_ANCHOR", "cta_recouple", r.sat, "",
                                     aw, k, "reengage",
                                     fmt::format("in_band={};reseed_var={:.3e}",
                                                 std::max(1, ctx.knobs.cta_recouple_ep),
                                                 r.post_var));
                }
            }
        }
        // A tripped satellite is out of the test for the rest of the epoch: its
        // prior is either withdrawn below or knowingly left standing, and either
        // way re-testing it would re-count the same edge.
        for (const std::string& s : trip) anchor.sat[s].folded = false;
        if (trip.empty()) break;
        if (!ctx.knobs.cta_resolve) break;
        if (trip_resolve.empty()) {
            log->warn("Network (clock anchor): TOW {:.1f} — {} clock-anchor trip(s) [{}] decoupled "
                      "and marked, but their carry redundancy (P⁻−P⁺)/P⁻ is below {:.2f}: the "
                      "column is prior-dominated, so withdrawing the prior would leave it "
                      "near-unobservable.  The epoch emits the anchored value for those "
                      "satellites; they fold LOOSE from the next epoch.",
                      se_time.tow, trip.size(), joinSats(trip), kCtaMinCarryRedundancy);
            break;
        }

        // --- The value correction: withdraw and re-solve (adapt and emit) -----
        // Split the withdrawal by fold path.  A key in the C1 dense block is
        // marginalized out of the block (rows/cols dropped, block re-inverted); a
        // diagonal key is subtracted outright.  Both are exact inverses of what
        // was folded, which is why the capture records the folded (value, info)
        // instead of recomputing them.
        std::set<std::string> dense_drop;
        for (const auto& kk : ws.cap_dense_keys)
            if (kk.kind == SharedParamKind::Clock && !kk.clone && trip_resolve.count(kk.sat))
                dense_drop.insert(kk.sat);
        std::vector<NetworkEstimator::SharedPrior> diag_drop;
        for (const auto& p : ws.cap_priors)
            if (p.kind == SharedParamKind::Clock && trip_resolve.count(p.sat)
                && !dense_drop.count(p.sat))
                diag_drop.push_back(p);
        if (dense_drop.empty() && diag_drop.empty()) {
            // The capture channel is the only record of what was folded; with it
            // unarmed there is nothing to withdraw exactly, and a guess would leave
            // residual prior information behind.  Log it and stop.
            log->warn("Network (clock anchor): TOW {:.1f} — {} clock-anchor trip(s) cannot be "
                      "value-corrected in-epoch: no captured clock prior to withdraw (the "
                      "RAIM-replay capture channel is unarmed for this fold).  Decoupled and "
                      "marked; the correction lands next epoch.",
                      se_time.tow, trip_resolve.size());
            break;
        }

        bool folded_dense = false;
        Eigen::VectorXd x_try;
        try {
            if (!dense_drop.empty()) {
                // On a clone epoch the fold was the joint one, so its walk factors
                // are withdrawn (the clones stay; filter/NetworkClockClone).  Off, the clone
                // refloat does nothing and returns false.
                // The gauge-free flag has to match the fold, or the withdrawn block
                // is not the block that was applied.
                // Each withdrawal below is named Q3Withdraw just before its
                // call, each restore in the catch Q3Restore (the clone refloat names its own).
                if (!networkClockCloneRefloat(ctx, ws, est, dense_drop, /*restore=*/false)) {
                    est.contributionKind(ContributionKind::Q3Withdraw);
                    est.refoldSharedPriorDenseDroppingClocks(ws.cap_dense_keys,
                                                             ws.cap_dense_cov, dense_drop,
                                                             /*restore=*/false, gf);
                }
                folded_dense = true;
            }
            if (!diag_drop.empty()) {
                // A gauge-free "diagonal" block is not diagonal: removing one key
                // moves the group's information-weighted mean, so every survivor's
                // anchor target changes.  Re-fold the group instead of subtracting
                // one entry.
                est.contributionKind(ContributionKind::Q3Withdraw);
                if (gf) {
                    std::set<std::string> ds;
                    for (const auto& p : diag_drop) ds.insert(p.sat);
                    est.refoldSharedPriorClockGaugeFreeDropping(ws.cap_priors, ds,
                                                                /*restore=*/false);
                } else {
                    est.withdrawSharedPrior(diag_drop);
                }
            }
            x_try = est.solve(plan.shared_datum);
        } catch (const std::exception& ex) {
            ++ctx.counters.cta_resolve_failed;
            log->warn("Network (clock anchor): TOW {:.1f} in-epoch clock refloat re-solve failed "
                      "({}); restoring the anchored solve.  The {} tripped satellite(s) stay "
                      "decoupled and marked — only the same-epoch VALUE correction is lost.",
                      se_time.tow, ex.what(), trip_resolve.size());
            try {
                if (folded_dense
                    && !networkClockCloneRefloat(ctx, ws, est, dense_drop, /*restore=*/true)) {
                    est.contributionKind(ContributionKind::Q3Restore);
                    est.refoldSharedPriorDenseDroppingClocks(ws.cap_dense_keys,
                                                             ws.cap_dense_cov, dense_drop,
                                                             /*restore=*/true, gf);
                }
                if (!diag_drop.empty()) {
                    est.contributionKind(ContributionKind::Q3Restore);
                    if (gf) {
                        std::set<std::string> ds;
                        for (const auto& p : diag_drop) ds.insert(p.sat);
                        est.refoldSharedPriorClockGaugeFreeDropping(ws.cap_priors, ds,
                                                                    /*restore=*/true);
                    } else {
                        est.addSharedPrior(diag_drop);
                    }
                }
                x_s = est.solve(plan.shared_datum);   // refresh shared_cov_ consistently
            } catch (const std::exception& ex2) {
                log->error("Network (clock anchor): TOW {:.1f} the restore after a failed refloat "
                           "re-solve ALSO failed ({}) — this epoch's shared covariance no "
                           "longer matches its solution.",
                           se_time.tow, ex2.what());
            }
            break;
        }
        x_s = x_try;
        ++ctx.counters.cta_resolves;
        // Keep the RAIM replay channel in step: the within-epoch re-solve replays
        // cap_priors / cap_dense_* verbatim onto a fresh arrowhead, so a withdrawn
        // prior left in the capture would be re-injected.
        if (!diag_drop.empty()) {
            // The capture's kinds are erased in step with the priors,
            // when the two are in step; a capture without kinds keeps none.
            auto& cp = ws.cap_priors;
            auto& ck = ws.cap_prior_kinds;
            const bool kinds_in_step = (ck.size() == cp.size());
            std::size_t kept = 0;
            for (std::size_t r = 0; r < cp.size(); ++r) {
                if (cp[r].kind == SharedParamKind::Clock && trip_resolve.count(cp[r].sat) > 0)
                    continue;
                if (kept != r) {
                    cp[kept] = std::move(cp[r]);
                    if (kinds_in_step) ck[kept] = ck[r];
                }
                ++kept;
            }
            cp.resize(kept);
            if (kinds_in_step) ck.resize(kept);
        }
        if (!dense_drop.empty()) {
            // Marginalize the dropped keys out of the captured block too, so a
            // replay re-folds the same reduced block this solve used.
            std::vector<Eigen::Index> keep;
            keep.reserve(ws.cap_dense_keys.size());
            for (std::size_t i = 0; i < ws.cap_dense_keys.size(); ++i) {
                const auto& kk = ws.cap_dense_keys[i];
                if (kk.kind == SharedParamKind::Clock && !kk.clone && dense_drop.count(kk.sat)) continue;
                keep.push_back(static_cast<Eigen::Index>(i));
            }
            std::vector<NetworkEstimator::SharedPrior> nk;
            Eigen::MatrixXd nc(static_cast<Eigen::Index>(keep.size()),
                               static_cast<Eigen::Index>(keep.size()));
            nk.reserve(keep.size());
            for (std::size_t a = 0; a < keep.size(); ++a) {
                nk.push_back(ws.cap_dense_keys[static_cast<std::size_t>(keep[a])]);
                for (std::size_t b = 0; b < keep.size(); ++b)
                    nc(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                        ws.cap_dense_cov(keep[a], keep[b]);
            }
            ws.cap_dense_keys = std::move(nk);
            ws.cap_dense_cov  = std::move(nc);
            networkClockCloneRefloatTrim(ws, dense_drop);   // no-op off
        }
        log->info("Network (clock anchor): TOW {:.1f} clock-anchor refloat — {} satellite(s) [{}] "
                  "withdrawn and the epoch RE-SOLVED before emission (pass {}).",
                  se_time.tow, trip_resolve.size(), joinSats(trip_resolve), pass + 1);
    }
    if (pass >= kCtaMaxPasses)
        log->warn("Network (clock anchor): TOW {:.1f} hit the {}-pass in-epoch refloat cap — any "
                  "further clock-anchor trip this epoch is decoupled and marked but not "
                  "value-corrected.  A run that reaches this cap regularly is telling you the "
                  "anchor q or k is mis-specified, not that the data has that many faults.",
                  se_time.tow, kCtaMaxPasses);
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the Q3 guarded temporal clock anchor.
int networkClockAnchorRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (!ctx.knobs.cta_on) return 0;
    // Guarded temporal clock anchor run-end telemetry; the ON-path counters
    // are the evidence (invariant #4).  `folded_anchored = 0` means the anchor never
    // bound a single row: the mechanism is inert and every downstream number in the
    // run is the legacy carry, however clean the log reads.  Trips are split by
    // reason because they are not interchangeable evidence: a run that only shows
    // `datum`/`ext` trips has tested neither the detector (nis, abs) nor the
    // staleness guard (gap), and an untested guard is not a guard (invariant #1; the
    // DOY-129 event-window arm tests them).
    const auto& c = ctx.counters;
    const long long folds = c.cta_folded_anchored + c.cta_folded_loose;
    const long long trips = c.cta_trip_nis + c.cta_trip_abs + c.cta_trip_gap
                          + c.cta_trip_datum + c.cta_trip_ext;
    log->info("Network (guarded temporal clock anchor): {} epoch(s); {} clock "
              "carry fold(s) — {} ANCHORED at q = {:.3e} m²/s (targets √(q·Δt) = {:.2f} cm "
              "at 30 s), {} LOOSE, {} at the variance floor.  Trips {} = {} nis + {} abs + "
              "{} gap + {} datum + {} ext; {} in-epoch re-solve(s) ({} failed), {} "
              "re-coupling edge(s), {} wire mark(s) queued.  Largest |w| {:.2f} (k = {:.2f}) "
              "on {} at TOW {:.1f}.",
              c.cta_epochs, folds, c.cta_folded_anchored, ctx.knobs.cta_q,
              100.0 * std::sqrt(std::max(0.0, ctx.knobs.cta_q) * 30.0),
              c.cta_folded_loose, c.cta_floor_hits, trips, c.cta_trip_nis,
              c.cta_trip_abs, c.cta_trip_gap, c.cta_trip_datum, c.cta_trip_ext,
              c.cta_resolves, c.cta_resolve_failed, c.cta_recouples, c.cta_marks,
              c.cta_max_w, ctx.knobs.cta_k,
              c.cta_max_w_sat.empty() ? std::string("(none)") : c.cta_max_w_sat,
              c.cta_max_w_tow);
    if (ctx.knobs.cta_space != 0) {
        // The phase-space statistic beside the clock statistic.
        // In shadow mode the clock statistic decided; in phase mode the phase one did.
        std::string wl, el;
        for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
            wl += fmt::format("{}>={:g}:{}", b ? " " : "", kCtaWLadder[b], c.cta_phi_w_ge[b]);
        for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
            el += fmt::format("{}>={:g}:{}", b ? " " : "", kCtaELadder[b], c.cta_phi_e_ge[b]);
        log->info("Network (phase-space guard, {}): {} anchored row(s) with the "
                  "ionosphere-free phase clock statistic, {} without one (abs only in phase "
                  "mode), {} at the {:.3f} m floor; sigma0_phi {:.3f}; over k on both {}, on "
                  "the CLOCK statistic only {} (the level's motion), on the PHASE statistic "
                  "only {}; {} decouple(s) decided by the phase statistic.  |w_phi| ladder "
                  "[{}]; |e_phi| ladder [{}].",
                  ctx.knobs.cta_space == 2 ? "DECIDING" : "shadow", c.cta_phi_rows,
                  c.cta_phi_unavail, c.cta_phi_floor_rows, ctx.knobs.cta_phi_floor_m,
                  c.cta_phi_rows > 0 ? std::sqrt(c.cta_phi_sum_w2
                                                 / static_cast<double>(c.cta_phi_rows)) : 0.0,
                  c.cta_x_both, c.cta_x_clk_only, c.cta_x_phi_only, c.cta_trip_phi, wl, el);
    }
    if (ctx.knobs.carry_gauge_free) {
        // Denominator of the gauge-free step (§2.1).  `gf_keys = 0` on an armed run
        // means the downdate never reached a group of ≥ 2 carried clocks, so the
        // anchor still constrains the gauge: the failure the step exists to prevent,
        // which only this line shows.
        log->info("Network (gauge-free fold): {} clock prior key(s) "
                  "gauge-downdated over {} epoch(s); {} epoch(s) INERT (no constellation "
                  "contributed 2+ carried clocks, or UᵀΛU was singular).  Expect the "
                  "CLK-R drained common mode Σ|μ| to RETURN toward the control's level — "
                  "the anchor no longer does the datum's work.",
                  c.cta_gf_keys, c.cta_epochs, c.cta_gf_inert);
        if (c.cta_gf_keys == 0)
            log->warn("Network: the gauge-free fold is ARMED but downdated "
                      "ZERO keys — it is INERT for this run and the anchor is folding on "
                      "the RAW state exactly as if the knob were off.  Any A/B against a "
                      "raw-state arm is measuring nothing.");
    }
    if (c.cta_folded_anchored == 0)
        log->warn("Network (clock anchor): the anchor folded ZERO anchored rows — it is INERT for "
                  "this run.  Every satellite stayed LOOSE, so the emitted clock is the "
                  "legacy carry's.  Check that satellites are surviving the re-coupling "
                  "re-validation ({} consecutive in-band epochs) and that the staleness "
                  "bound ({:.0f} s) is above this run's epoch spacing.",
                  ctx.knobs.cta_recouple_ep, ctx.knobs.cta_max_gap_s);
    // Only the fault-class trips (nis, abs) are resolve candidates: a gap, datum or
    // ext decouple withdraws the prior because the frame or the visibility moved,
    // emits a fresh solve, and has no anchored value to correct.  Counting those here
    // would report warped epochs on a quiet run with zero fault trips: a composite
    // denominator makes a working run read as a broken one.
    if (const long long fault_trips = c.cta_trip_nis + c.cta_trip_abs;
        ctx.knobs.cta_resolve && c.cta_resolves == 0 && fault_trips > 0)
        log->warn("Network (clock anchor): {} FAULT trip(s) (nis+abs) but ZERO in-epoch "
                  "re-solves — every one was decoupled and marked WITHOUT the same-epoch "
                  "value correction, so each shipped an anchor-warped epoch.  See the "
                  "per-trip warnings for the cause (prior-dominated column, or no "
                  "captured prior to withdraw).", fault_trips);
    // The denominator this run's guard and census standardized by (§2.1).  Two arms
    // of the bisection differ only here, so without this line the pair cannot be told
    // apart in the archived console.
    log->info("Network: guard/census denominator — P⁻ {} , P⁺ {} (fold: {}).",
              ctx.knobs.cta_stat_proj_prior ? "GAUGE-PROJECTED" : "raw per-satellite var_eff",
              ctx.knobs.cta_stat_proj_post  ? "GAUGE-PROJECTED" : "raw marginal cov(c,c)",
              ctx.knobs.carry_gauge_free ? "gauge-free" : "raw state");
    // The k derivation of §3, per rung.  The denominator √S shrinks as q tightens, so
    // a fixed k is a different number of metres at every rung; the ladder below shows
    // that without a second disarmed run per rung, and the metre ladder is what the
    // absolute backstop's rail is read from (on the 3 h arm, none of the 63 nis trips
    // would have been caught by the shipped 2.0 m rail).
    if (c.cta_w_rows > 0) {
        const double n     = static_cast<double>(c.cta_w_rows);
        const double msq   = c.cta_sum_sqrt_s / n;
        const double s0    = std::sqrt(c.cta_sum_w2 / n);
        std::string wl, el;
        for (std::size_t b = 0; b < kCtaWLadder.size(); ++b)
            wl += fmt::format("{}>={:g}:{} ({:.3g}%)", b ? "  " : "", kCtaWLadder[b],
                              c.cta_w_ge[b], 100.0 * static_cast<double>(c.cta_w_ge[b]) / n);
        for (std::size_t b = 0; b < kCtaELadder.size(); ++b)
            el += fmt::format("{}>={:g}m:{} ({:.3g}%)", b ? "  " : "", kCtaELadder[b],
                              c.cta_e_ge[b], 100.0 * static_cast<double>(c.cta_e_ge[b]) / n);
        log->info("Network (guard ladder over {} ANCHORED row(s)): mean √S = "
                  "{:.4f} m ⇒ the armed k = {:.2f} is a {:.3f} m bar at this rung; mean |ê| = "
                  "{:.4f} m; guard σ̂₀ = √(Σw²/n) = {:.3f}.  |w| ladder: {}.  |ê| ladder: {}.",
                  c.cta_w_rows, msq, ctx.knobs.cta_k, ctx.knobs.cta_k * msq,
                  c.cta_sum_abs_e / n, s0, wl, el);
        log->info("Network: read k off the |w| ladder ABOVE, not off a trip "
                  "count — but note this population is TRUNCATED across epochs (a tripped "
                  "satellite goes LOOSE and stops contributing until it re-couples), so the "
                  "upper bins UNDER-report.  The un-truncated null needs an arm with "
                  "network.clock_anchor_k beyond the top rung and network.clock_anchor_abs_m "
                  "= 0 — that is what §3 means by 'with the guard disarmed'.");
    }
    if (ctx.knobs.cta_var_floor > 0.0 && c.cta_floor_hits == 0 && c.cta_folded_anchored > 0)
        log->info("Network (clock anchor): the variance floor never bound ({:.3e} m² vs the armed "
                  "q·Δt) — the consistency cap is armed but inactive at this q, which is the "
                  "expected reading while q·Δt stays above it.", ctx.knobs.cta_var_floor);
    else if (ctx.knobs.cta_var_floor > 0.0 && c.cta_folded_anchored > 0)
        // The floor binding at the bottom rungs is the design, not a warning
        // condition (§7.4): below q·Δt ≈ (3 mm)² the anchor would otherwise claim
        // precision that no realistic correlated-error floor supports, and deviation
        // #1's consistency cap stops it.  The line reports how often it bound,
        // because "the floor governs" and "the floor is a rounding error" are
        // different postures and only the fraction separates them.
        log->info("Network (deviation-#1 consistency floor): the floor BOUND on {} "
                  "of {} anchored fold(s) ({:.1f} %) at {:.3e} m² (σ = {:.2f} mm) against the "
                  "armed q·Δt = {:.3e} m² at 30 s.  A binding floor is the DESIGN at these "
                  "rungs, not a fault: it is the cap that keeps an independence-assuming "
                  "carry from accumulating information against a time-correlated error "
                  "floor.  Grade it on the census's clk family and the wire, never on the "
                  "fact that it bound.",
                  c.cta_floor_hits, c.cta_folded_anchored,
                  100.0 * static_cast<double>(c.cta_floor_hits)
                        / static_cast<double>(c.cta_folded_anchored),
                  ctx.knobs.cta_var_floor, 1000.0 * std::sqrt(ctx.knobs.cta_var_floor),
                  std::max(0.0, ctx.knobs.cta_q) * 30.0);
    return 0;
}

// ---- End of the run-end reports ----

// ---- Solve stages (generated by scripts/nom_sg_cut.py) ----
/// The cold-start a-priori satellite-clock Tikhonov anchor (Doxygen at the declaration).
void networkFoldColdClockAnchor(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    NetworkEstimator& est = run.engines.est;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const bool   within_epoch_armed = ctx.knobs.within_epoch_armed;   // network.raim_within_epoch
    const double clk_anchor_sigma   = ctx.knobs.clk_anchor_sigma;     // D6-resolved anchor sigma (m)
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored; // brdc_rts: the per-satellite broadcast anchors are the clock gauge (pre-gate); no pivot machinery and no sum-to-zero membership wander
    auto& cap_priors = ws.cap_priors;
    // ---- End of the bindings ----
    // --- A-priori satellite-clock anchor (cold-start conditioning) ------
    // The shared clock column is Δδtˢ relative to the a-priori precise product clock,
    // so it is small (sub-metre for precise post-processing).  With no absolute anchor
    // the per-epoch joint solve floats it by hundreds of metres at cold start to absorb
    // the clock↔iono↔bias confusion (no carry-forward exists at epoch 1, and from
    // epoch 2 on the carry anchors to that floated value).  A Tikhonov prior pulling
    // each Δδtˢ toward 0 with σ = initial_std.sat_clock (the documented "Δδtˢ vs
    // a-priori product clock" quality: sub-metre for precise products, looser for RT
    // broadcast) regularizes only the ill-conditioned cold-start direction.  The
    // per-constellation clock datum stays fixed by the explicit Datum (a large-weight
    // pin that dominates this soft prior), so the few-metre datum offset that the data
    // determine is kept.  The prior is applied after the gate (like the carry-forward)
    // so the null-space rank gate still sees the clock datum as a gauge, and it
    // combines in information space with the carry-forward (anchor toward 0 plus random
    // walk toward the previous value).  It applies only when σ binds (< 100 m); the
    // loose default (1000 m) leaves the anchor off and the run byte-identical.
    //
    // Measured scope (305 stations, GPS+Galileo; see [[project_global_sh_iono]]):
    // σ = 0.05 m reduces the cold-start clock-Δ rms from 56 to 2.9 m for about 6
    // epochs, after which the W6 clock↔bias↔ambiguity systematic returns (the data want
    // about 80 m, with information far above the anchor's).  The anchor improves
    // cold-start clock-product quality only.  The divergence of the co-estimated
    // SH-VTEC field is a separate, decoupled cold-start estimability defect (its
    // large-scale level is unobservable under the common-mode-free iono R), and
    // pinning the clock does not move the field.
    {
        const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
        // Broadcast-anchored datum: the same per-satellite anchors were folded before
        // the gate as the gauge realization (buildSharedDatum constraint rows), and
        // folding them again here would double the anchor information (effective
        // σ/√2).  Skip them in that mode; every other mode keeps the post-gate
        // regularizer.
        if (!clock_brdc_anchored
            && std::isfinite(clk_sigma) && clk_sigma > 0.0 && clk_sigma < 100.0) {
            const double clk_info = 1.0 / (clk_sigma * clk_sigma);
            std::vector<NetworkEstimator::SharedPrior> clk_anchor;
            clk_anchor.reserve(registered_clock_sats.size());
            for (const std::string& sat : registered_clock_sats) {
                NetworkEstimator::SharedPrior p;
                p.kind  = SharedParamKind::Clock;
                p.sat   = sat;
                p.value = 0.0;          // anchor Δδtˢ toward the a-priori clock
                p.info  = clk_info;
                clk_anchor.push_back(p);
            }
            est.contributionKind(ContributionKind::ColdClockAnchor);
            est.addSharedPrior(clk_anchor);
            if (within_epoch_armed)
                networkCapturePriors(cap_priors, ws.cap_prior_kinds, clk_anchor,
                                     ContributionKind::ColdClockAnchor);
        }
    }
}

/// The Q3 guard between the solve and the within-epoch tier (Doxygen at the declaration).
void networkClockAnchorGuardEpoch(NetworkRun& run, const NetworkEpoch& ep, const SolveDatum& datum,
                                  const NetworkPostFitHandoff& handoff, Eigen::VectorXd& x_s,
                                  EpochWorkspace& ws) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const SyncedEpoch& se = ep.se;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const NetworkSbasis::Plan& plan = datum.plan;
    const std::set<char>& changed_consts = datum.changed_consts;
    const bool& datum_changed = datum.datum_changed;
    const std::set<std::string>& clock_refloat_sats = handoff.clock_refloat_sats;
    const EpochEvents& events = ep.events;
    // ---- End of the bindings ----
    // --- Guarded temporal clock anchor (docs/SPEC_clock_temporal_anchor.md) ---
    // Must run here: after the primary solve and before the within-epoch adaptation
    // below.  A stiff clock prior fighting a real step pushes the tension into the
    // residuals and raises σ̂0, and the adaptation would then attribute a prior fault to
    // a satellite's or a station's data and drop it.  A prior fault is resolved first,
    // by refloating the prior (never by de-weighting a measurement) and re-solving the
    // epoch in place, so the assessment below sees the corrected solve and the epoch
    // emits (adapt-and-emit; a withhold here would be an adaptation failure).
    // clock_refloat_sats is always empty at this call (nothing is inserted before the
    // within-epoch adaptation below), so the guard's `ext` coupling reads no satellites
    // on this path.  No-op, with no state allocation, when off.
    networkClockAnchorGuard(ctx, config_, log, traces, se.time, est, plan, x_s, ws,
                            registered_clock_sats, clock_refloat_sats, datum_changed,
                            changed_consts, events);
}
// ---- End of the solve stages ----

}  // namespace GnssProcessor
