/**
 * @file ClockLevel.cpp
 * @brief Implementation of the satellite clock level read: the level monitor, its
 *        innovation instrument and the G5 restore raise.  The level direction and the
 *        read are described in the header banner.
 *
 * Seam contract: free functions over @c NetworkRunContext that own no state; every
 * persistent value is a reference into @c ctx.carry (the @c clm_* members).
 */

#include "network/solve/ClockLevel.hpp"

#include "network/state/RunContext.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "network/solve/Estimator.hpp"
#include "network/assembly/ExtraBandScope.hpp"   // networkPinBandsFor (the pin pair's bands)
#include "network/assembly/RowClassify.hpp"     // rowTagAt, isCodeObservationKind (the code rows)
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"

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

// The run-end reports at the end of this file:
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include <cstddef>
#include <memory>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

namespace {

/// Carrier frequency (Hz) of a RINEX code on a satellite, from its band's nominal
/// wavelength (speed of light over that wavelength); 0 when the code or its band is
/// unknown.
double carrierHz(const std::string& sat, const std::string& code) {
    const auto signal = SignalId::fromObsCode(systemOfSat(sat), code);
    const double lam = signal ? nominalWavelengthM(signal->band()) : 0.0;
    return (lam > 0.0) ? Constants::SPEED_OF_LIGHT / lam : 0.0;
}

/// Median of a copy.
double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

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
    std::map<std::pair<int, System>, int> ridx;
    for (std::size_t k : use) {
        const std::pair<int, System> key{pairs[k].station, systemOfSat(pairs[k].sat)};
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
        const int j = S + ridx.at({p.station, systemOfSat(p.sat)});
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
    // pseudo-observation (ten times the largest satellite diagonal, at least 10): it
    // fixes the one direction the station offsets cannot separate, the common level
    // (the datum).
    double big = 0.0;
    for (int i = 0; i < S; ++i) big = std::max(big, N(i, i));
    big = 10.0 * std::max(1.0, big);
    std::map<System, std::vector<int>> by_const;
    for (const auto& [sat, i] : sidx) by_const[systemOfSat(sat)].push_back(i);
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
    // The effect covariance at unit weight: the satellites' block of
    // the inverse normals (one solve per satellite on the same factor), projected on each
    // constellation's contrast.  The stiff sum-to-zero leaves 1/(n^2 big) on every entry
    // along the common direction; the projection removes it exactly, which gives the
    // minimum-norm (sum-to-zero) covariance of the effects.
    Eigen::MatrixXd Z(S, S);
    for (int i = 0; i < S; ++i) {
        Eigen::VectorXd e = Eigen::VectorXd::Zero(S + R);
        e(i) = 1.0;
        const Eigen::VectorXd z = f.solve(e);
        Z.col(i) = z.head(S);
    }
    if (Z.allFinite()) {
        for (const auto& [c, idx] : by_const) {
            (void)c;
            const double n = static_cast<double>(idx.size());
            double grand = 0.0;
            std::map<int, double> rowsum;
            for (int a : idx)
                for (int bb : idx) { rowsum[a] += Z(a, bb); grand += Z(a, bb); }
            for (const auto& [sat, i] : sidx)
                if (rowsum.count(i))
                    out.var[sat] = Z(i, i) - 2.0 * rowsum[i] / n + grand / (n * n);
        }
    }
    double chi = 0.0;
    for (std::size_t k : use) {
        const ClockLevelPair& p = pairs[k];
        const double r = p.v_if - x(sidx.at(p.sat)) + x(S + ridx.at({p.station, systemOfSat(p.sat)}));
        chi += p.w_if * r * r;
    }
    const long long dof = out.used - (S - 1) - R;
    if (dof > 0) out.s0sq = chi / static_cast<double>(dof);
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Seams.
// ---------------------------------------------------------------------------

namespace {

/// Step (1) of the level read of the level monitor: this epoch's satellite
/// code-bias columns, then one ionosphere-free pair per (station, satellite) from the
/// heaviest full-weight data code row on each of the constellation's two pin bands, read
/// on the satellite's realized pin pair only.
/// @return false when there is nothing to read (a layout mismatch or no code-bias column).
bool clockLevelBuildPairs(NetworkRunContext& ctx, const NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of,
                          std::vector<ClockLevelPair>& pairs, const EpochEvents& events) {
    const Eigen::Index ns = est.numShared();
    if (x_s.size() != ns) return false;

    // This epoch's satellite code-bias columns, each with its satellite's clock column.
    struct CbCol {
        Eigen::Index col{-1};
        Eigen::Index clk{-1};
        std::string  sat, sig;
        SignalId     signal{};   ///< the code's signal; its band digit meets the pin bands
    };
    std::vector<CbCol> cbs;
    for (Eigen::Index c = 0; c < ns; ++c) {
        CbCol cb;
        SharedParamKind kind{};
        if (!est.sharedColumnIdentity(c, kind, cb.sat, cb.sig)
            || kind != SharedParamKind::CodeBias)
            continue;
        cb.col = c;
        // GLONASS is FDMA (no common pin-pair frequency): out of the card's scope.
        const System sys = systemOfSat(cb.sat);
        const auto code = ObservableId::parse(sys, cb.sig);
        if (sys == System::GLONASS || !code || code->type != ObsType::Code) continue;
        cb.signal = code->signal;
        try { cb.clk = est.clockColumn(cb.sat); } catch (...) { continue; }
        if (cb.clk < 0 || cb.clk >= ns) continue;
        cbs.push_back(std::move(cb));
    }
    if (cbs.empty()) return false;

    // (1) One ionosphere-free pair per (station, satellite): the heaviest full-weight
    //     data code row on each of the constellation's two pin bands.
    std::map<System, NetworkPinBands> pin_of;
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
        // A code-bias column's rows: the code observation rows (by their tags)
        // that carry the column's partial and the satellite clock's.  The tag
        // replaces the row's position ahead of the coupling region, which kept out the
        // coupling rows (statements about the carry, which can carry both partials).
        const Eigen::Index n_rows = R.A_s.rows();
        const auto codeRowOf = [&R](Eigen::Index r, const CbCol& cb) {
            return isCodeObservationKind(rowTagAt(R, r).kind)
                   && R.A_s(r, cb.col) != 0.0 && R.A_s(r, cb.clk) != 0.0;
        };
        // sat -> band digit -> (weight, residual, code)
        std::map<std::string, std::map<char, std::tuple<double, double, std::string>>> best;
        for (const CbCol& cb : cbs) {
            // A column outside the satellite's realized pin pair (a free same-band extra,
            // or any satellite without a pin this epoch) does not enter.
            const auto pr = events.code_datum.this_epoch_if_pin_real.find(cb.sat);
            if (pr == events.code_datum.this_epoch_if_pin_real.end()
                || (cb.sig != pr->second.first && cb.sig != pr->second.second))
                continue;
            for (Eigen::Index r = 0; r < n_rows; ++r) {
                if (!codeRowOf(r, cb)) continue;
                const double w = R.w(r);
                if (!(w >= kClvFullWeight) || !std::isfinite(w)) continue;
                auto& slot = best[cb.sat][cb.signal.digit];   // a pin band role
                if (!(w > std::get<0>(slot))) continue;
                const double v = R.y(r) - R.A_s.row(r).dot(x_s) - R.A_r.row(r).dot(x_r);
                if (std::isfinite(v)) slot = {w, v, cb.sig};
            }
        }
        for (const auto& [sat, bands] : best) {
            const System sys = systemOfSat(sat);
            auto pit = pin_of.find(sys);
            if (pit == pin_of.end())
                pit = pin_of.emplace(sys, networkPinBandsFor(rinexLetter(sys))).first;
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

    return true;
}

/// The level innovation instrument (the offline prototype is the
/// session scratchpad's wf/lvl/lvl3.py).  The ionosphere-free code is the only
/// observable of a satellite's level, and it sees the clock alone (the code bias's IF
/// combination is the datum), so the prefit IF code read against the constellation is an
/// innovation of the level: its two-way effect at the prior clocks is the post-fit
/// effect minus the constellation contrast of this epoch's clock update,
/// nu_s = (z_s - mean z) - (dc_s - mean dc).  (The slant ionosphere cannot absorb it: the
/// IF coefficients cancel any ionosphere share exactly, A(1-k') - B(1-g k') = 1 with
/// A - B g = 0, so carried ambiguities do not change the reading.)  Its predicted variance
/// is S = u'P-u + s0^2 Q: the previous stash's clock block along the contrast plus the
/// anchor walk, and the fit's own variance.  Per epoch NIS = nu^2/S; per window of W
/// epochs, the mean innovation per satellite and its chi2(1) statistic
/// T = nubar^2 / (sum S / k^2), and the truth-free level NEES from two station halves
/// (even and odd station index), NEES_tf = sum_s nubar_A nubar_B / sum_s Pbar_s: the
/// cross product of two independent reads of the same level error is unbiased for its
/// square, whatever the code noise.  Measure-only.
void clockLevelInnovation(NetworkRunContext& ctx, NetworkRunTraces& traces, const GnssTime& t,
                          const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                          const std::vector<ClockLevelPair>& pairs, const ClockLevelTwoWay& tw) {
    auto& C = ctx.carry;
    auto& K = ctx.counters;
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    std::map<std::string, double> c_now;
    for (const auto& [sat, e] : tw.effect) {
        (void)e;
        try {
            const Eigen::Index col = est.clockColumn(sat);
            if (col >= 0 && col < x_s.size() && std::isfinite(x_s(col))) c_now[sat] = x_s(col);
        } catch (...) {}
    }
    const double dt = (C.clm_prev_s > 0.0) ? now_s - C.clm_prev_s : 0.0;
    // Split-half fits (station parity), on the post-fit pairs: the clock-update contrast
    // is subtracted below exactly as for the full fit.
    std::vector<ClockLevelPair> pa, pb;
    for (const ClockLevelPair& p : pairs) (p.station % 2 == 0 ? pa : pb).push_back(p);
    const ClockLevelTwoWay ta = clockLevelTwoWay(pa, kClvMinPairs, kClvClipK, kClvClipFloorM);
    const ClockLevelTwoWay tb = clockLevelTwoWay(pb, kClvMinPairs, kClvClipK, kClvClipFloorM);
    if (tw.ok && net::present(ctx.entities.sats, &net::SatelliteState::clm_prev_clk) > 0 && dt > 0.0 && dt <= 300.0
        && std::isfinite(tw.s0sq)) {
        // The fit set F: satellites with an effect, a clock now and a clock before, not
        // decoupled by the anchor this epoch (its prior was withdrawn).
        std::vector<std::string> F;
        for (const auto& [sat, z] : tw.effect) {
            (void)z;
            if (!c_now.count(sat) || !net::getSat(ctx.entities.sats, sat, &net::SatelliteState::clm_prev_clk)) continue;
            if (C.clk_anchor.tripped.count(sat)) { ++K.clm_innov_tripped; continue; }
            F.push_back(sat);
        }
        const std::size_t n = F.size();
        if (n >= 3) {
            double zbar = 0.0, dbar = 0.0, zabar = 0.0, zbbar = 0.0;
            std::size_t nab = 0;
            for (const auto& s : F) {
                zbar += tw.effect.at(s);
                dbar += c_now.at(s) - (*net::getSat(ctx.entities.sats, s, &net::SatelliteState::clm_prev_clk));
            }
            zbar /= static_cast<double>(n); dbar /= static_cast<double>(n);
            // The halves share the fit set only where both saw the satellite.
            std::vector<std::string> Fab;
            if (ta.ok && tb.ok)
                for (const auto& s : F)
                    if (ta.effect.count(s) && tb.effect.count(s)) Fab.push_back(s);
            if (Fab.size() >= 3) {
                double dab = 0.0;
                for (const auto& s : Fab) {
                    zabar += ta.effect.at(s); zbbar += tb.effect.at(s);
                    dab += c_now.at(s) - (*net::getSat(ctx.entities.sats, s, &net::SatelliteState::clm_prev_clk));
                }
                nab = Fab.size();
                zabar /= static_cast<double>(nab); zbbar /= static_cast<double>(nab);
                dab /= static_cast<double>(nab);
                for (const auto& s : Fab) {
                    const double dc = c_now.at(s) - (*net::getSat(ctx.entities.sats, s, &net::SatelliteState::clm_prev_clk)) - dab;
                    auto& w = net::putSat(ctx.entities, s, &net::SatelliteState::clm_win);
                    w.s_nua += (ta.effect.at(s) - zabar) - dc;
                    w.s_nub += (tb.effect.at(s) - zbbar) - dc;
                    ++w.kab;
                }
            }
            // The prior contrast variance u'P-u from the previous stash's clock block
            // (the pivot, pinned outside the block, contributes 0) plus the walk the
            // fold applied (the anchor's q, or the loose clock q for a LOOSE clock).
            const double q_clk = ctx.params.q(SharedParamKind::Clock);   // the registry
            std::vector<int> pr(n, -1);
            for (std::size_t a = 0; a < n; ++a) {
                if (const int* pi = net::getSat(ctx.entities.sats, F[a], &net::SatelliteState::clm_prev_pidx)) pr[a] = *pi;
            }
            const auto Pab = [&](std::size_t a, std::size_t b) {
                if (pr[a] < 0 || pr[b] < 0) return 0.0;
                double v = C.clm_prev_pclk(pr[a], pr[b]);
                if (a == b) {
                    const auto ait = C.clk_anchor.sat.find(F[a]);
                    const bool anch = ctx.knobs.cta_on && ait != C.clk_anchor.sat.end()
                                      && ait->second.anchored;
                    v += (anch ? ctx.knobs.cta_q : q_clk) * dt;
                }
                return v;
            };
            double sumP = 0.0;
            std::vector<double> rowsum(n, 0.0);
            for (std::size_t a = 0; a < n; ++a)
                for (std::size_t b = 0; b < n; ++b) {
                    const double v = Pab(a, b);
                    rowsum[a] += v;
                    sumP += v;
                }
            const double nn = static_cast<double>(n);
            for (std::size_t a = 0; a < n; ++a) {
                const std::string& s = F[a];
                const double z  = tw.effect.at(s) - zbar;
                const double dc = c_now.at(s) - (*net::getSat(ctx.entities.sats, s, &net::SatelliteState::clm_prev_clk)) - dbar;
                const double nu = z - dc;
                const double P  = std::max(0.0, Pab(a, a) - 2.0 * rowsum[a] / nn + sumP / (nn * nn));
                const auto vit = tw.var.find(s);
                const double R  = (vit != tw.var.end() ? vit->second : 0.0) * tw.s0sq;
                const double S  = P + R;
                if (!(S > 0.0) || !std::isfinite(nu)) continue;
                const double nis = nu * nu / S;
                auto& w = net::putSat(ctx.entities, s, &net::SatelliteState::clm_win);
                ++w.k; w.s_nu += nu; w.s_var += S; w.s_r += R; w.s_p += P;
                ++K.clm_innov_rows;
                if (K.clm_epochs > 15) {
                    K.clm_innov_nis_sum += nis;
                    ++K.clm_innov_nis_n;
                    if (nis > 6.63) ++K.clm_innov_nis_gt;
                }
                if (traces.level_innov_trace.is_open())
                    traces.level_innov_trace
                        << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << s << ','
                        << tw.n_pairs.at(s) << ','
                        << fmt::format("{:.4f},{:.4f},{:.4f},{:.4e},{:.4e},{:.4e},{:.4f}",
                                       z, dc, nu, R, P, tw.s0sq, nis)
                        << '\n';
            }
        }
    }
    // Window end: the per-satellite means, T, and the split-half NEES.
    if (++K.clm_win_ep >= std::max(1, ctx.knobs.clm_innov_window_ep)) {
        double sxab = 0.0, sP = 0.0, sdd = 0.0, sRk = 0.0, sT = 0.0, snu2 = 0.0;
        int nsat = 0, nab = 0, nT = 0;
        long long ngt = 0;
        const int kmin = std::max(1, K.clm_win_ep / 2);
        net::forEach(ctx.entities.sats, &net::SatelliteState::clm_win,   // satellite order (O1)
                     [&](const SatId& id, const ClockLevelInnovWin& w) {
            const std::string s = net::satKey(id);
            if (w.k < kmin) return;
            const double kk = static_cast<double>(w.k);
            const double nub = w.s_nu / kk;
            const double T = nub * nub / (w.s_var / (kk * kk));
            ++nsat; snu2 += nub * nub; sP += w.s_p / kk; sRk += w.s_r / (kk * kk);
            if (std::isfinite(T)) { sT += T; ++nT; if (T > 6.63) ++ngt; }
            double nua = 0.0, nubb = 0.0;
            if (w.kab >= kmin) {
                nua = w.s_nua / static_cast<double>(w.kab);
                nubb = w.s_nub / static_cast<double>(w.kab);
                sxab += nua * nubb;
                sdd += 0.25 * (nua - nubb) * (nua - nubb);
                ++nab;
            }
            if (traces.level_innov_win_trace.is_open())
                traces.level_innov_win_trace
                    << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << s << ','
                    << w.k << ',' << fmt::format("{:.4f},{:.4f},{:.4f},{:.4e},{:.4e},{:.3f}",
                                                 nub, nua, nubb, w.s_var / (kk * kk),
                                                 w.s_p / kk, T)
                    << '\n';
        });
        if (nsat > 0 && nab > 0 && sP > 0.0) {
            const double nees = sxab / sP;
            const double phi = (sRk > 0.0) ? (sdd / nab) / (sRk / nsat) : 0.0;
            K.clm_win_nees.push_back(nees);
            K.clm_win_rms.push_back(std::sqrt(snu2 / nsat));
            K.clm_win_T.push_back(nT > 0 ? sT / nT : 0.0);
            K.clm_win_phi.push_back(phi);
            K.clm_win_Tgt += ngt;
            K.clm_win_Tn += nT;
        }
        net::dropAll(ctx.entities.sats, &net::SatelliteState::clm_win);
        K.clm_win_ep = 0;
    }
    // The prior of the next epoch: this epoch's clocks and stashed clock block (the raise,
    // when armed, runs after this call and moves only the level direction's variance,
    // which the next epoch then reads through this copy only if it was taken after it;
    // taking it here keeps the instrument independent of the raise).
    net::assignSat(ctx.entities, &net::SatelliteState::clm_prev_clk, c_now);
    C.clm_prev_s = now_s;
    net::dropAll(ctx.entities.sats, &net::SatelliteState::clm_prev_pidx);
    const SharedCarryCov& pcc = C.prev_carry_cov;
    std::vector<std::pair<std::string, std::size_t>> crow;
    for (const auto& [key, row] : pcc.row) {
        const std::size_t bar = key.find('|');
        if (bar == std::string::npos || key.substr(bar + 1) != "CLK") continue;
        if (row >= static_cast<std::size_t>(pcc.P.rows())) continue;
        crow.emplace_back(key.substr(0, bar), row);
    }
    C.clm_prev_pclk.resize(static_cast<Eigen::Index>(crow.size()),
                           static_cast<Eigen::Index>(crow.size()));
    for (std::size_t a = 0; a < crow.size(); ++a) {
        net::putSat(ctx.entities, crow[a].first, &net::SatelliteState::clm_prev_pidx) = static_cast<int>(a);
        for (std::size_t b = 0; b < crow.size(); ++b)
            C.clm_prev_pclk(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                pcc.P(static_cast<Eigen::Index>(crow[a].second),
                      static_cast<Eigen::Index>(crow[b].second));
    }
}

}  // namespace

void networkClockLevelRestoreRaise(NetworkRun& run) {
    // ---- From the run (generated by scripts/nom_ep1_cut.py, each line checked
    //      by scripts/nom_ep1_verify.py). The seam reads no epoch member. ----
    NetworkRunContext& ctx = run.ctx;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (!ctx.carry.resume.clm_raise) return;
    ctx.carry.resume.clm_raise = false;
    SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    const Eigen::Index np = pcc.P.rows();
    if (np == 0 || pcc.P.cols() != np) {
        log->warn("Network (card CTU G5): level raise on restore armed but no dense border "
                  "block was restored: INERT.");
        return;
    }
    std::map<std::string, std::vector<std::size_t>> u_rows;
    for (const auto& [key, row] : pcc.row) {
        const std::size_t bar = key.find('|');
        if (bar == std::string::npos || row >= static_cast<std::size_t>(np)) continue;
        const std::string rest = key.substr(bar + 1);
        if (rest == "CLK" || rest.rfind("PB|", 0) == 0) u_rows[key.substr(0, bar)].push_back(row);
    }
    long long n = 0;
    double sum = 0.0, mx = 0.0;
    for (const auto& [sat, rows] : u_rows) {
        const double* it = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::clm_info_c);
        if (!it || !(*it > 0.0)) continue;
        bool has_clk = false;
        for (const auto& [key, row] : pcc.row)
            if (key == sat + "|CLK") { has_clk = true; break; }
        if (!has_clk) continue;   // the pivot: pinned, no level to loosen
        // The seam is a process-noise event of variance v along the level
        // direction, applied by this epoch's time update (the dense fold, and the coupling
        // rows' exact update when armed), not a raise of the stashed block.  The
        // distinction matters for the restored coupling rows: a row harvested in a lineage
        // whose recursion stated the level carries a level component in its regression, and
        // only the time update's re-attribution through P + Q releases it; a raised block
        // alone leaves the rows asserting the old level (measured: 0.14 m frozen after the
        // seam with the raise).
        // The information the inherited level is credited with: all the snapshot's epochs
        // (credit 0, the default: V = 1/I_c), or only `credit` epochs' worth of the
        // writer's average per-epoch information (V = n / (credit I_c)); a lineage whose
        // level recursion was not consistent holds a level error beyond its own
        // information, and crediting it with less re-opens the level to the code.
        const int credit = ctx.knobs.clm_restore_credit_ep;
        const int* nit = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::clm_n);
        const int n_ep = nit ? *nit : 0;
        const double v = (credit > 0 && n_ep > credit)
                             ? static_cast<double>(n_ep) / (static_cast<double>(credit) * *it)
                             : 1.0 / *it;
        ctx.carry.clm_seam_q[sat] = v;
        (void)rows;
        ++n; sum += v; mx = std::max(mx, v);
    }
    ctx.counters.clm_restore_raised = n;
    ctx.counters.clm_restore_raise_sum = sum;
    ctx.counters.clm_restore_raise_max = mx;
    log->info("Network (card CTU G5): level noise on restore armed for {} satellite(s): this "
              "epoch's time update adds each satellite's code-only level variance along its level "
              "direction (sum {:.4e} m^2, largest {:.4e} m^2); phase-visible combinations get "
              "none.", n, sum, mx);
}

void networkClockLevelMonitor(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const GnssTime& t = ep.se.time;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    bool datum_changed = ep.solve.datum_changed;
    const EpochEvents& events_ro = ep.events;
    // ---- End of the bindings ----
    if (!ctx.knobs.clm_on) return;
    ++ctx.counters.clm_epochs;
    if (datum_changed) {
        ++ctx.counters.clm_skip_datum;
        // The clock frame stepped: the previous epoch's clocks and block are in the
        // retired frame, so the next innovation has no prior (the window keeps its sums).
        net::dropAll(ctx.entities.sats, &net::SatelliteState::clm_prev_clk);
        ctx.carry.clm_prev_pclk.resize(0, 0);
        net::dropAll(ctx.entities.sats, &net::SatelliteState::clm_prev_pidx);
        return;
    }
    // (1)-(2) The level read, on the realized pin pair only.
    std::vector<ClockLevelPair> pairs;
    if (!clockLevelBuildPairs(ctx, est, x_s, asm_res, est_index_of, pairs, events_ro))
        return;
    const ClockLevelTwoWay tw = clockLevelTwoWay(pairs, kClvMinPairs, kClvClipK, kClvClipFloorM);
    clockLevelInnovation(ctx, traces, t, est, x_s, pairs, tw);
    // (3) The code-only information, accumulated without decay: the level has no process
    //     noise, and a decaying bound would make the raise a per-epoch level variance.
    for (const auto& [sat, info] : tw.info) {
        if (!(info > 0.0) || !std::isfinite(info)) continue;
        net::putSat(ctx.entities, sat, &net::SatelliteState::clm_info) += info;
        ++net::putSat(ctx.entities, sat, &net::SatelliteState::clm_n);
    }
    // The same information in the constellation contrast and at the fit's own
    // unit variance, 1 / (s0^2 Q_ss): the level variance a consistent carry holds after
    // these epochs is its inverse (section 14.4: the effective code gain is IF-efficient).
    if (tw.ok && std::isfinite(tw.s0sq) && tw.s0sq > 0.0)
        for (const auto& [sat, q] : tw.var)
            if (q > 0.0 && std::isfinite(q)) net::putSat(ctx.entities, sat, &net::SatelliteState::clm_info_c) += 1.0 / (tw.s0sq * q);
    // (4) The stashed block's variance along each satellite's level direction.
    SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    const Eigen::Index np = pcc.P.rows();
    if (np == 0 || pcc.P.cols() != np) return;
    std::map<std::string, std::vector<std::size_t>> u_rows;   // sat -> clock row first, then its PBs
    std::map<std::string, bool> has_clk;
    for (const auto& [key, row] : pcc.row) {
        const std::size_t bar = key.find('|');
        if (bar == std::string::npos || row >= static_cast<std::size_t>(np)) continue;
        const std::string sat = key.substr(0, bar);
        const std::string rest = key.substr(bar + 1);
        if (rest == "CLK") {
            u_rows[sat].insert(u_rows[sat].begin(), row);
            has_clk[sat] = true;
        } else if (rest.rfind("PB|", 0) == 0) {
            u_rows[sat].push_back(row);
        }
    }
    const double now_tow = t.tow;
    for (const auto& [sat, rows] : u_rows) {
        const double* ii = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::clm_info);
        if (!ii || !(*ii > 0.0)) continue;
        if (!has_clk.count(sat)) {                       // the pinned pivot, or a clock not carried
            ++ctx.counters.clm_no_clock;
            continue;
        }
        const double uu = static_cast<double>(rows.size());
        double upu = 0.0;
        for (std::size_t a : rows)
            for (std::size_t b : rows)
                upu += pcc.P(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b));
        const double p_level = upu / (uu * uu);
        const double inv     = 1.0 / *ii;
        const double ratio   = p_level / inv;
        const int    n_ep    = net::putSat(ctx.entities, sat, &net::SatelliteState::clm_n);
        if (n_ep >= 2 && std::isfinite(ratio)) {
            ++ctx.counters.clm_rows;
            if (ratio < 1.0) ++ctx.counters.clm_below1;
            if (ctx.counters.clm_ratios.empty() || ratio < ctx.counters.clm_min_ratio)
                ctx.counters.clm_min_ratio = ratio;
            ctx.counters.clm_ratios.push_back(ratio);
        }
        double raised = 0.0;
        if (ctx.knobs.clm_raise && std::isfinite(ratio) && ratio < 1.0) {
            // P += a u u' with a = 1/I_L - P_L: the level variance becomes 1/I_L, every
            // direction orthogonal to u is untouched, and the block stays positive
            // definite (a positive rank-one addition).  The carried variances follow the
            // diagonal, because the fold rebuilds the diagonal from them.
            raised = inv - p_level;
            for (std::size_t a : rows)
                for (std::size_t b : rows)
                    pcc.P(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) += raised;
            for (CarryPrior& cp : ctx.carry.prev_carry) {
                if (cp.sat != sat || cp.t_park_s > 0.0) continue;
                if (cp.kind == SharedParamKind::Clock || cp.kind == SharedParamKind::PhaseBias) {
                    const auto rit = pcc.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                    if (rit != pcc.row.end()) cp.var += raised;
                }
            }
            ++ctx.counters.clm_raised;
            ++ctx.counters.clm_raised_by_sat[sat];
            ctx.counters.clm_raised_sum += raised;
            ctx.counters.clm_raised_max = std::max(ctx.counters.clm_raised_max, raised);
        }
        if (traces.level_bound_trace.is_open())
            traces.level_bound_trace << t.week << ',' << fmt::format("{:.3f}", now_tow) << ','
                                     << sat << ','
                                     << fmt::format("{:.6e},{:.6e},{:.6e},{:.6e}", p_level, inv,
                                                    ratio, raised)
                                     << ',' << n_ep << '\n';
    }
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the CTU level monitor.
int networkClockLevelMonitorRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (!ctx.knobs.clm_on) return 0;
    // The level monitor: the ratio of the stashed block's level
    // variance to the code-only bound, per satellite per kept epoch from its second
    // epoch of code on.  A consistent estimator keeps it at or above 1; the control
    // (the card 1d kappa) reads far below 1, the freeze's truth-free fingerprint.
    const auto& c = ctx.counters;
    std::vector<double> r = c.clm_ratios;
    double med = 0.0, p10 = 0.0;
    if (!r.empty()) {
        std::sort(r.begin(), r.end());
        med = r[r.size() / 2];
        p10 = r[r.size() / 10];
    }
    log->info("Network (card CTU / level monitor): {} epoch(s) ({} skipped at a datum "
              "change); {} satellite-epoch ratio(s) P_L/(1/I_L): min {:.3e}, p10 {:.3e}, "
              "median {:.3e}, {} below 1 ({:.1f} %); {} satellite-epoch(s) without a carried "
              "clock (the pivot).  Bound raise {}: {} raise(s) over {} satellite(s), summed "
              "{:.4e} m^2, largest {:.4e} m^2.",
              c.clm_epochs, c.clm_skip_datum, c.clm_rows, c.clm_min_ratio, p10, med,
              c.clm_below1,
              c.clm_rows > 0 ? 100.0 * static_cast<double>(c.clm_below1)
                                   / static_cast<double>(c.clm_rows) : 0.0,
              c.clm_no_clock, ctx.knobs.clm_raise ? "ON" : "off", c.clm_raised,
              c.clm_raised_by_sat.size(), c.clm_raised_sum, c.clm_raised_max);
    if (c.clm_rows == 0)
        log->warn("Network: the level monitor is ARMED but counted no ratio: "
                  "no satellite had both code information and a carried level.");
    // The level innovation instrument.  A consistent level reads
    // NIS mean ~1, window T ~1 and NEES_tf ~0.6 to 1 (the split-half cross product over
    // the mean prior variance); a frozen or overconfident one reads NEES_tf >> 1.
    {
        std::string nees, rms, tt, phi;
        for (std::size_t w = 0; w < c.clm_win_nees.size(); ++w) {
            const char* sep = w ? " " : "";
            nees += fmt::format("{}{:.2f}", sep, c.clm_win_nees[w]);
            rms  += fmt::format("{}{:.3f}", sep, c.clm_win_rms[w]);
            tt   += fmt::format("{}{:.2f}", sep, c.clm_win_T[w]);
            phi  += fmt::format("{}{:.2f}", sep, c.clm_win_phi[w]);
        }
        log->info("Network (card CTU / level innovation): {} satellite-epoch innovation(s) "
                  "({} left out, the anchor withdrew the prior); NIS mean {:.3f} over {} after "
                  "epoch 15, {:.1f} % above 6.63; per {}-epoch window: truth-free level NEES "
                  "[{}], rms mean innovation [{}] m, mean T [{}] ({:.1f} % of window "
                  "satellites above 6.63), split-half noise ratio [{}].",
                  c.clm_innov_rows, c.clm_innov_tripped,
                  c.clm_innov_nis_n > 0 ? c.clm_innov_nis_sum
                                              / static_cast<double>(c.clm_innov_nis_n) : 0.0,
                  c.clm_innov_nis_n,
                  c.clm_innov_nis_n > 0 ? 100.0 * static_cast<double>(c.clm_innov_nis_gt)
                                              / static_cast<double>(c.clm_innov_nis_n) : 0.0,
                  ctx.knobs.clm_innov_window_ep, nees, rms, tt,
                  c.clm_win_Tn > 0 ? 100.0 * static_cast<double>(c.clm_win_Tgt)
                                         / static_cast<double>(c.clm_win_Tn) : 0.0,
                  phi);
    }
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
