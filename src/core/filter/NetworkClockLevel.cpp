/**
 * @file NetworkClockLevel.cpp
 * @brief Implementation of card CLV, the code-only re-base of the satellite clock LEVEL
 *        (docs/refound/CLV_clock_level_2026-09-22.md).  The direction, why the carry
 *        freezes it and what the re-base does are described in the header banner.
 *
 * New code, not a relocation.  Seam contract: free functions over
 * @c NetworkRunContext that own NO state; every persistent value is a reference into
 * @c ctx.carry.clk_level.
 */

#include "filter/NetworkClockLevel.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/Constants.hpp"
#include "common/SignalBand.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkExtraBandScope.hpp"   // networkPinBandsFor (the pin pair's bands)
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "products/NetworkProductEmitter.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace {

/// Carrier frequency (Hz) of a RINEX code on a satellite, from its band's nominal
/// wavelength; 0 when the band is unknown.
double carrierHz(const std::string& sat, const std::string& code) {
    const SignalClassification sc = classifySignalForSat(sat, code);
    const double lam = bandNominalWavelengthMeters(sc.band);
    return (lam > 0.0) ? Constants::SPEED_OF_LIGHT / lam : 0.0;
}

/// Median of a copy.
double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Split "SAT_SIGNAL" (the pb_level_prev / pin_bridge key) at its first underscore.
bool splitSatSignal(const std::string& key, std::string& sat, std::string& sig) {
    const std::size_t u = key.find('_');
    if (u == std::string::npos) return false;
    sat = key.substr(0, u);
    sig = key.substr(u + 1);
    return !sat.empty() && !sig.empty();
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

double clockLevelDecayedInfo(double info, double t_info, double now_s, double horizon_s) {
    if (!(t_info > 0.0) || !(info > 0.0) || !std::isfinite(info)) return 0.0;
    const double dt = now_s - t_info;
    if (!(dt > 0.0) || !(horizon_s > 0.0)) return info;
    return info * std::exp(-dt / horizon_s);
}

double clockLevelVariance(double info_now, double floor_m, double ignition_var,
                          bool* at_floor) {
    if (at_floor) *at_floor = false;
    if (!(info_now > 0.0) || !std::isfinite(info_now)) return ignition_var;
    const double v  = 1.0 / info_now;
    const double f2 = floor_m * floor_m;
    if (v <= f2) {
        if (at_floor) *at_floor = true;
        return f2;
    }
    return v;
}

double clockLevelIfInformation(double w1, double f1_hz, double w2, double f2_hz) {
    if (!(w1 > 0.0) || !(w2 > 0.0) || !(f1_hz > 0.0) || !(f2_hz > 0.0)) return 0.0;
    const double d = f1_hz * f1_hz - f2_hz * f2_hz;
    if (!(std::abs(d) > 0.0)) return 0.0;
    const double a = f1_hz * f1_hz / d;
    const double b = f2_hz * f2_hz / d;
    const double var = a * a / w1 + b * b / w2;
    return (var > 0.0 && std::isfinite(var)) ? 1.0 / var : 0.0;
}

double clockLevelIfResidual(double v1, double f1_hz, double v2, double f2_hz) {
    const double d = f1_hz * f1_hz - f2_hz * f2_hz;
    if (!(std::abs(d) > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return (f1_hz * f1_hz * v1 - f2_hz * f2_hz * v2) / d;
}

ClockLevelTwoWay clockLevelTwoWay(const std::vector<ClockLevelPair>& pairs, int min_pairs,
                                  double clip_k, double clip_floor_m) {
    ClockLevelTwoWay out;
    // Per-satellite robust screen against the satellite's own median.
    std::map<std::string, std::vector<std::size_t>> by_sat;
    for (std::size_t k = 0; k < pairs.size(); ++k) {
        const ClockLevelPair& p = pairs[k];
        if (p.sat.empty() || !(p.w_if > 0.0) || !std::isfinite(p.v_if)) continue;
        by_sat[p.sat].push_back(k);
    }
    std::vector<std::size_t> use;
    std::map<std::string, int> sidx;
    for (const auto& [sat, ks] : by_sat) {
        std::vector<double> v;
        v.reserve(ks.size());
        for (std::size_t k : ks) v.push_back(pairs[k].v_if);
        const double med = medianOf(v);
        std::vector<double> ad;
        ad.reserve(v.size());
        for (double x : v) ad.push_back(std::abs(x - med));
        const double bar = std::max(clip_floor_m, clip_k * 1.4826 * medianOf(ad));
        std::vector<std::size_t> kept;
        for (std::size_t k : ks) {
            if (std::abs(pairs[k].v_if - med) <= bar) kept.push_back(k);
            else ++out.clipped;
        }
        if (static_cast<int>(kept.size()) < min_pairs) {
            ++out.few;
            continue;
        }
        const int next = static_cast<int>(sidx.size());
        sidx[sat] = next;
        use.insert(use.end(), kept.begin(), kept.end());
    }
    // One offset per (station, constellation): a receiver's inter-system bias is a
    // per-constellation station offset and must not leak into the satellite effects.
    std::map<std::pair<int, char>, int> ridx;
    for (std::size_t k : use) {
        const std::pair<int, char> key{pairs[k].station, pairs[k].sat[0]};
        if (ridx.count(key)) continue;
        const int next = static_cast<int>(ridx.size());
        ridx[key] = next;
    }
    const int S = static_cast<int>(sidx.size());
    const int R = static_cast<int>(ridx.size());
    if (S < 2 || R < 1) return out;

    // Normal equations of v = e_s - c_r, weight w_if.
    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(S + R, S + R);
    Eigen::VectorXd b = Eigen::VectorXd::Zero(S + R);
    for (std::size_t k : use) {
        const ClockLevelPair& p = pairs[k];
        const int i = sidx.at(p.sat);
        const int j = S + ridx.at({p.station, p.sat[0]});
        N(i, i) += p.w_if;
        N(j, j) += p.w_if;
        N(i, j) -= p.w_if;
        N(j, i) -= p.w_if;
        b(i) += p.w_if * p.v_if;
        b(j) -= p.w_if * p.v_if;
        out.info[p.sat] += p.w_if;
        ++out.n_pairs[p.sat];
    }
    out.used = static_cast<long long>(use.size());
    // Sum-to-zero on the satellite effects of each constellation, as a stiff
    // pseudo-observation (ten times the largest satellite diagonal): it fixes the one
    // direction the station offsets cannot separate, the common level (the datum).
    double big = 0.0;
    for (int i = 0; i < S; ++i) big = std::max(big, N(i, i));
    big = 10.0 * std::max(1.0, big);
    std::map<char, std::vector<int>> by_const;
    for (const auto& [sat, i] : sidx) by_const[sat[0]].push_back(i);
    for (const auto& [c, idx] : by_const) {
        (void)c;
        for (int a : idx)
            for (int bb : idx) N(a, bb) += big;
    }
    N.diagonal().array() += 1e-12 * big;   // a station seen by one satellite only
    const Eigen::LDLT<Eigen::MatrixXd> f(N);
    if (f.info() != Eigen::Success) return out;
    const Eigen::VectorXd x = f.solve(b);
    if (!x.allFinite()) return out;
    for (const auto& [sat, i] : sidx) out.effect[sat] = x(i);
    out.ok = true;
    return out;
}

ClockLevelUpdate clockLevelFilterUpdate(double info_prior, bool armed, double i_ep, double e,
                                        double break_k, double floor_m) {
    ClockLevelUpdate u;
    const double cap = (floor_m > 0.0) ? 1.0 / (floor_m * floor_m)
                                       : std::numeric_limits<double>::infinity();
    if (!(i_ep > 0.0) || !std::isfinite(e)) return u;
    bool restart = !armed || !(info_prior > 0.0);
    if (!restart) {
        const double spred = std::sqrt(1.0 / info_prior + 1.0 / i_ep);
        if (std::abs(e) > break_k * spred) {
            restart = true;
            u.broke = true;
        }
    }
    double info = restart ? i_ep : info_prior + i_ep;
    if (info >= cap) {
        info = cap;
        u.at_floor = true;
    }
    u.restarted = restart;
    u.info      = info;
    u.gain      = restart ? 1.0 : std::min(1.0, i_ep / info);
    return u;
}

long long clockLevelShiftCarry(NetworkCarryState& carry,
                               const std::map<std::string, double>& shift) {
    long long n = 0;
    if (shift.empty()) return n;
    auto shiftOf = [&](SharedParamKind kind, const std::string& sat) -> double {
        if (kind != SharedParamKind::Clock && kind != SharedParamKind::PhaseBias) return 0.0;
        const auto it = shift.find(sat);
        return (it == shift.end()) ? 0.0 : it->second;
    };
    // The carried border values (parked entries included: a re-admitted satellite's
    // seed must be in the same frame as the rest of its carry).
    std::map<std::string, std::set<std::string>> sigs;   // sat -> its phase-bias signals
    for (CarryPrior& cp : carry.prev_carry) {
        const double d = shiftOf(cp.kind, cp.sat);
        if (d == 0.0) continue;
        cp.value += d;
        ++n;
        if (cp.kind == SharedParamKind::PhaseBias) sigs[cp.sat].insert(cp.signal);
    }
    // Every carried row keeps its residual: u = r + M^T s with the right-hand side
    // built from s_prev, so the s_prev of each term on a moved column moves with it.
    auto shiftCoupling = [&](LocalCarry& lc) {
        for (CoupledShared& cs : lc.coupling) {
            const double d = shiftOf(cs.kind, cs.sat);
            if (d == 0.0) continue;
            cs.s_prev += d;
            ++n;
        }
    };
    for (auto& kv : carry.local_carry)      shiftCoupling(kv.second);
    for (auto& kv : carry.zwd_carry)        shiftCoupling(kv.second);
    for (auto& kv : carry.iono_carry)       shiftCoupling(kv.second);
    for (auto& kv : carry.iono_blk_carry)   shiftCoupling(kv.second);
    for (auto& kv : carry.gl_hold_amb)      shiftCoupling(kv.second);
    for (auto& kv : carry.gl_hold_iono)     shiftCoupling(kv.second);
    for (auto& kv : carry.gl_hold_iono_blk) shiftCoupling(kv.second);
    for (auto& kv : carry.rx_clk_carry)     shiftCoupling(kv.second);
    for (auto& kv : carry.rpb_carry)        shiftCoupling(kv.second);
    for (auto& kv : carry.rx_cb_carry)      shiftCoupling(kv.second);
    // The clock-jump baselines: a re-base is not a clock step.
    for (const auto& [sat, d] : shift) {
        if (d == 0.0) continue;
        if (auto it = carry.prev_clk.find(sat); it != carry.prev_clk.end()) {
            it->second.first += d;
            ++n;
        }
        if (auto it = carry.cb_exc_prev_clk.find(sat); it != carry.cb_exc_prev_clk.end()) {
            it->second += d;
            ++n;
        }
        if (auto it = carry.pb_hold_prev_clock_m.find(sat);
            it != carry.pb_hold_prev_clock_m.end()) {
            it->second += d;
            ++n;
        }
    }
    // The last solved phase bias and the open pin bridges ("SAT_SIGNAL").
    for (auto& [key, v] : carry.pb_level_prev) {
        std::string sat, sig;
        if (!splitSatSignal(key, sat, sig)) continue;
        const double d = shiftOf(SharedParamKind::PhaseBias, sat);
        if (d == 0.0) continue;
        v += d;
        ++n;
    }
    for (auto& [key, br] : carry.pin_bridge) {
        std::string sat, sig;
        if (!splitSatSignal(key, sat, sig)) continue;
        const double d = shiftOf(SharedParamKind::PhaseBias, sat);
        if (d == 0.0) continue;
        br.value += d;
        ++n;
    }
    // The emit-side transfer: every phase-bias signal the satellite has in the carry
    // or in the hold ("SAT|SIGNAL") follows the shift.
    for (const auto& [key, st] : carry.pb_hold) {
        (void)st;
        const std::size_t bar = key.find('|');
        if (bar == std::string::npos) continue;
        const std::string sat = key.substr(0, bar);
        if (shift.count(sat)) sigs[sat].insert(key.substr(bar + 1));
    }
    for (const auto& [sat, d] : shift) {
        if (d == 0.0) continue;
        ClockLevelSat& ls = carry.clk_level.sat[sat];
        for (const std::string& sig : sigs[sat]) ls.xfer[sig] += d;
        ls.pending_step += d;   // declared on the satellite's next C record
    }
    return n;
}

void networkClockLevelDeclare(NetworkRunContext& ctx, NetworkSolutionEpoch& sol) {
    if (!ctx.knobs.clv_on) return;
    for (NetworkSatSolution& s : sol.sats) {
        if (s.sat.empty() || !s.has_clock) continue;
        const auto it = ctx.carry.clk_level.sat.find(s.sat);
        if (it == ctx.carry.clk_level.sat.end()) continue;
        s.level_step_m = it->second.pending_step;
        it->second.pending_step = 0.0;
    }
}

// ---------------------------------------------------------------------------
// Seams.
// ---------------------------------------------------------------------------

void networkClockLevelRebase(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, const NetworkEstimator& est,
                             const Eigen::VectorXd& x_s,
                             const std::vector<StationAssembly>& asm_res,
                             const std::vector<int>& est_index_of, bool datum_changed) {
    if (!ctx.knobs.clv_on) return;
    ++ctx.counters.clv_epochs;
    if (datum_changed) {
        // The post-fit residuals then straddle two frames; the level waits one epoch.
        ++ctx.counters.clv_skip_datum;
        return;
    }
    const Eigen::Index ns = est.numShared();
    if (x_s.size() != ns) return;

    // This epoch's satellite code-bias columns, each with its satellite's clock column.
    struct CbCol { Eigen::Index col{-1}; Eigen::Index clk{-1}; std::string sat, sig; };
    std::vector<CbCol> cbs;
    for (Eigen::Index c = 0; c < ns; ++c) {
        std::string lbl;
        try { lbl = est.sharedColumnLabel(c); } catch (...) { continue; }
        if (lbl.rfind("CB:", 0) != 0) continue;
        const std::size_t p1 = lbl.find(':');
        const std::size_t p2 = lbl.find(':', p1 + 1);
        if (p2 == std::string::npos) continue;
        CbCol cb;
        cb.col = c;
        cb.sat = lbl.substr(p1 + 1, p2 - p1 - 1);
        cb.sig = lbl.substr(p2 + 1);
        // GLONASS is FDMA (no common pin-pair frequency): out of the card's scope.
        if (cb.sat.empty() || cb.sat[0] == 'R' || cb.sig.size() < 2 || cb.sig[0] != 'C')
            continue;
        try { cb.clk = est.clockColumn(cb.sat); } catch (...) { continue; }
        if (cb.clk < 0 || cb.clk >= ns) continue;
        cbs.push_back(std::move(cb));
    }
    if (cbs.empty()) return;

    // (1) One ionosphere-free pair per (station, satellite): the heaviest full-weight
    //     DATA code row on each of the constellation's two pin bands.
    std::vector<ClockLevelPair> pairs;
    std::map<char, NetworkPinBands> pin_of;
    for (std::size_t i = 0; i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != ns || R.w.size() != R.A_s.rows()
            || R.y.size() != R.A_s.rows())
            continue;
        Eigen::VectorXd x_r;
        try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
        catch (...) { continue; }
        if (x_r.size() != R.A_r.cols()) continue;
        // The coupling rows (and anything after them) are statements about the carry,
        // not observations.
        const Eigen::Index row_end = (R.cpl_row_start >= 0)
                                         ? std::min<Eigen::Index>(R.cpl_row_start, R.A_s.rows())
                                         : R.A_s.rows();
        // sat -> band digit -> (weight, residual, code)
        std::map<std::string, std::map<char, std::tuple<double, double, std::string>>> best;
        for (const CbCol& cb : cbs) {
            for (Eigen::Index r = 0; r < row_end; ++r) {
                if (R.A_s(r, cb.col) == 0.0 || R.A_s(r, cb.clk) == 0.0) continue;
                const double w = R.w(r);
                if (!(w >= kClvFullWeight) || !std::isfinite(w)) continue;
                auto& slot = best[cb.sat][cb.sig[1]];
                if (!(w > std::get<0>(slot))) continue;
                const double v = R.y(r) - R.A_s.row(r).dot(x_s) - R.A_r.row(r).dot(x_r);
                if (std::isfinite(v)) slot = {w, v, cb.sig};
            }
        }
        for (const auto& [sat, bands] : best) {
            auto pit = pin_of.find(sat[0]);
            if (pit == pin_of.end()) pit = pin_of.emplace(sat[0], networkPinBandsFor(sat[0])).first;
            if (!pit->second.valid()) continue;
            const auto i1 = bands.find(pit->second.band1);
            const auto i2 = bands.find(pit->second.band2);
            if (i1 == bands.end() || i2 == bands.end()) continue;
            const double f1 = carrierHz(sat, std::get<2>(i1->second));
            const double f2 = carrierHz(sat, std::get<2>(i2->second));
            ClockLevelPair p;
            p.sat     = sat;
            p.station = static_cast<int>(i);
            p.v_if    = clockLevelIfResidual(std::get<1>(i1->second), f1,
                                             std::get<1>(i2->second), f2);
            p.w_if    = clockLevelIfInformation(std::get<0>(i1->second), f1,
                                                std::get<0>(i2->second), f2);
            if (p.w_if > 0.0 && std::isfinite(p.v_if)) pairs.push_back(std::move(p));
        }
    }

    // (2) The two-way fit.
    const ClockLevelTwoWay tw = clockLevelTwoWay(pairs, kClvMinPairs, kClvClipK, kClvClipFloorM);
    ctx.counters.clv_pairs         += tw.used;
    ctx.counters.clv_pairs_clipped += tw.clipped;
    ctx.counters.clv_sat_few       += tw.few;
    if (!tw.ok) {
        ++ctx.counters.clv_twoway_failed;
        return;
    }

    // (3) The per-satellite filters.
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    struct Step { double d{0.0}; double i_ep{0.0}; ClockLevelUpdate u; };
    std::map<std::string, Step> steps;
    for (const auto& [sat, e] : tw.effect) {
        ClockLevelSat& ls = ctx.carry.clk_level.sat[sat];
        if (ctx.carry.clk_anchor.tripped.count(sat)) {
            ++ctx.counters.clv_sat_tripped;
            ls.armed = false;
            continue;
        }
        if (ctx.knobs.cta_on) {
            const auto ait = ctx.carry.clk_anchor.sat.find(sat);
            if (ait == ctx.carry.clk_anchor.sat.end() || !ait->second.anchored) {
                ++ctx.counters.clv_sat_unarmed;
                ls.armed = false;
                continue;
            }
        }
        const double i_ep = tw.info.at(sat);
        const double prior = ls.armed ? clockLevelDecayedInfo(ls.info, ls.t_info, now_s,
                                                              ctx.knobs.clv_horizon_s)
                                      : 0.0;
        Step st;
        st.i_ep = i_ep;
        st.u    = clockLevelFilterUpdate(prior, ls.armed, i_ep, e, kClvBreakK,
                                         ctx.knobs.clv_floor_m);
        if (!(st.u.info > 0.0)) continue;
        st.d = -st.u.gain * e;
        steps[sat] = st;
    }
    // The shifts never move a constellation's common level (the clock datum): their
    // information-weighted mean is removed.
    std::map<char, std::pair<double, double>> cmean;
    for (const auto& [sat, st] : steps) {
        auto& m = cmean[sat[0]];
        m.first  += st.i_ep * st.d;
        m.second += st.i_ep;
    }
    std::map<std::string, double> shift;
    for (auto& [sat, st] : steps) {
        const auto& m = cmean[sat[0]];
        if (m.second > 0.0) st.d -= m.first / m.second;
        shift[sat] = st.d;
    }

    // (4) Apply, then book-keep and trace.
    ctx.counters.clv_refs_shifted += clockLevelShiftCarry(ctx.carry, shift);
    for (const auto& [sat, st] : steps) {
        ClockLevelSat& ls = ctx.carry.clk_level.sat[sat];
        ls.info    = st.u.info;
        ls.t_info  = now_s;
        ls.armed   = true;
        ls.n_pairs = tw.n_pairs.count(sat) ? tw.n_pairs.at(sat) : 0;
        ls.e_last  = tw.effect.at(sat);
        ls.d_last  = st.d;
        ls.d_sum  += st.d;
        ++ctx.counters.clv_updates;
        if (st.u.restarted) {
            if (st.u.broke) ++ctx.counters.clv_breaks;
            else            ++ctx.counters.clv_arms;
        }
        if (st.u.at_floor) ++ctx.counters.clv_at_floor;
        ctx.counters.clv_shift_sum_abs += std::abs(st.d);
        if (std::abs(st.d) > ctx.counters.clv_shift_max_abs)
            ctx.counters.clv_shift_max_abs = std::abs(st.d);
        if (traces.clock_level_trace.is_open()) {
            const double v = clockLevelVariance(ls.info, ctx.knobs.clv_floor_m,
                                                ctx.knobs.clv_ignition_var);
            traces.clock_level_trace
                << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << sat << ','
                << fmt::format("{:.6e},{:.6e}", v, ls.info) << ',' << ls.n_pairs << ','
                << fmt::format("{:.6f},{:.6f}", ls.e_last, st.d) << ','
                << (st.u.restarted ? (st.u.broke ? "break" : "arm") : "update") << '\n';
        }
    }
}

void networkClockLevelTeardown(const NetworkRunContext& ctx, const NetworkProductEmitter* emitter,
                               const std::shared_ptr<spdlog::logger>& log) {
    if (!ctx.knobs.clv_on || !log) return;
    const auto& c = ctx.counters;
    // R6: per constellation, V_L of each armed satellite at its last update.
    std::map<char, std::vector<double>> v_by;
    std::map<char, int> floor_by, unarmed_by;
    for (const auto& [sat, ls] : ctx.carry.clk_level.sat) {
        if (sat.empty()) continue;
        if (!ls.armed || !(ls.info > 0.0)) {
            ++unarmed_by[sat[0]];
            continue;
        }
        bool at_floor = false;
        v_by[sat[0]].push_back(clockLevelVariance(ls.info, ctx.knobs.clv_floor_m,
                                                  ctx.knobs.clv_ignition_var, &at_floor));
        if (at_floor) ++floor_by[sat[0]];
    }
    for (auto& [cons, v] : v_by) {
        std::sort(v.begin(), v.end());
        const double med = medianOf(v);
        log->info("Network (card CLV / clock level) {}: V_L over {} armed satellite(s) at their "
                  "last update min {:.3e} / median {:.3e} / max {:.3e} m^2 (sigma {:.4f} / "
                  "{:.4f} / {:.4f} m); {} at the floor ({:.3f} m); {} satellite(s) not armed at "
                  "run end (V_L = the ignition variance {:.3e} m^2).",
                  cons, v.size(), v.front(), med, v.back(), std::sqrt(v.front()),
                  std::sqrt(med), std::sqrt(v.back()), floor_by[cons], ctx.knobs.clv_floor_m,
                  unarmed_by[cons], ctx.knobs.clv_ignition_var);
    }
    log->info("Network (card CLV): re-base: {} kept epoch(s) ({} skipped at a datum change, {} "
              "with a failed two-way fit); {} ionosphere-free pair(s) fitted, {} screened out; "
              "(satellite, epoch) {} update(s) ({} arm(s), {} innovation break(s), {} at the "
              "floor), {} with too few pairs, {} skipped at an anchor decouple, {} not armed "
              "(the anchor does not hold the clock); mean |shift| {:.4f} m, max {:.4f} m; {} "
              "carried reference(s) moved.",
              c.clv_epochs, c.clv_skip_datum, c.clv_twoway_failed, c.clv_pairs,
              c.clv_pairs_clipped, c.clv_updates, c.clv_arms, c.clv_breaks, c.clv_at_floor,
              c.clv_sat_few, c.clv_sat_tripped, c.clv_sat_unarmed,
              c.clv_updates > 0 ? c.clv_shift_sum_abs / static_cast<double>(c.clv_updates) : 0.0,
              c.clv_shift_max_abs, c.clv_refs_shifted);
    log->info("Network (card CLV): wire: {} held phase-bias emission(s) moved by the level "
              "transfer; {} phase-bias value-jump test(s) over the bar only because of the "
              "transfer and therefore not declared{}.",
              c.clv_held_moved, emitter ? emitter->clvJumpExcluded() : 0LL,
              ctx.knobs.emit_pb_hold ? "" : " (network.emit_pb_hold is off: the phase-bias "
                                            "records follow the estimate, which the re-base "
                                            "moves with the clock)");
    if (c.clv_updates == 0)
        log->warn("Network (card CLV): the card is armed but NO level update ran: the re-base is "
                  "INERT for this run (no satellite was armed, or no full-weight code pairs).");
}

}  // namespace GnssProcessor
