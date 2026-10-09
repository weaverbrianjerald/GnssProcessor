/**
 * @file NetworkFastClock.cpp
 * @brief Split-cadence clocks-only fast pass, implementation (FC-1).
 *        NetworkFastClock.hpp states the observable, the frame contract and the
 *        isolation contract; docs/SPEC_fast_clock_split_cadence.md holds the full card.
 *
 * @section fc1_dev Deviation from SPEC section 2.1
 *
 * The spec defines the anchor lump on the fitted phase,
 * @f$\hat\Phi_j(t_0)=\Phi^{obs}_j(t_0)-v_j(t_0)@f$, which needs the post-fit residual and
 * therefore a @c reconstructStation sweep.  The spec (section 5) forbids adding one, and
 * no existing sweep can supply it: all three (@c networkPostfitSigma0,
 * @c networkPostFitRaim, @c networkShHealth) run after the emit site and are gated on
 * trace or RAIM knobs.
 *
 * This implementation defines the lump on the observed phase instead, corrected by the
 * model's a-priori satellite clock (see @ref fc1_apriori below), which makes the fast
 * observable exact time-differenced carrier phase:
 * @f[ y_j(t) = [\tilde\Phi_j(t)-\tilde\Phi_j(t_0)] - [\rho(t)-\rho(t_0)] - \dot M_j\Delta t,
 *     \qquad \tilde\Phi_j = \Phi^{obs}_j + c\,\delta t^s_{ap} @f]
 * On a live arc the ambiguity and both hardware phase biases (satellite and receiver)
 * cancel identically, since they are constants of the arc rather than estimates.  What the
 * fitted value would have removed and this definition keeps is one epoch of phase noise at
 * @f$t_0@f$, which enters as a per-LOS constant: about 2 mm per band, about 6 mm after the
 * GPS L1/L2 ionosphere-free combination.  Averaged over the default
 * @c min_stations_per_sat = 3 lines of sight of a satellite, that is about 3.4 mm on
 * @f$\delta c^s@f$, against a ZOH floor of 0.157 m per 30 s.  The definition is exact in
 * the terms that matter and removes the coupling to a sweep.
 *
 * One consequence: an outlier in @f$\Phi^{obs}(t_0)@f$ affects that LOS for the whole fast
 * window rather than being excluded by the fit.  It does not corrupt the estimate, because
 * the per-tick k-sigma screen rejects that LOS on every tick and counts it, but it appears
 * as a persistent screen rather than as a clean anchor.
 *
 * @section fc1_apriori Lump on a-priori-corrected phase (emitted frame)
 *
 * Raw observed phase contains the full physical satellite clock
 * @f$-c\,\delta t^s = -c\,(\delta t^s_{ap} + \delta t^s_{res})@f$, where
 * @f$\delta t^s_{ap}@f$ is the a-priori clock the full model applies per LOS (the
 * product/broadcast clock polynomial at emission plus the periodic relativistic term,
 * ModelGeometry's dtr; by IGS convention the clock product excludes it) and
 * @f$\delta t^s_{res}@f$ is the residual the producer estimates and emits.  A lump built
 * on raw phase carries the full physical rate into @f$\dot M_j@f$, and the median-polished
 * @f$\dot c^s@f$ then follows the full physical clock rate (correlation +0.93 with the
 * broadcast c·af1, +0.07 with the slope of the emitted c0), plus the relativistic rate on
 * eccentric satellites (E14/E18, ±10–15 mm/s).  A consumer's own a-priori evaluation
 * already advances both across a fast window, so
 * @f$K = c_0(t_0) + \dot c\,\Delta t + \delta c@f$ built on the raw-phase rate counts them
 * twice: 15.3 cm RMS of spurious drift per 30 s tick, up to about 62 cm on E19
 * (af1 = 20.7 mm/s), against a physical signal of about 1.8 cm.  Correcting the phase by
 * @f$+c\,\delta t^s_{ap}@f$ at both the anchor and the tick leaves @f$\dot M_j@f$, and with
 * it the emitted @f$\dot c^s@f$, carrying only the residual clock rate, the rate of the
 * quantity the producer emits.  Only in that frame does "anchor + rate·dt" mean something
 * to a consumer.  The tick's beyond-trend @f$\delta c@f$ (the shadow instrument) is
 * unchanged at reported precision: the correction's own second difference over a 30–60 s
 * window is sub-mm even at the Galileo e = 0.16 extreme.
 */

#include "filter/NetworkFastClock.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRobustStats.hpp"   // medianPolish2Way (robust two-way decomposition)
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "products/NetworkProductIO.hpp"   // NetworkProductWriter::writeFastClock
#include "products/SsrCorrectionStore.hpp"

#include <Eigen/Dense>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace GnssProcessor {

namespace {

/// Lowest-band-first ordering key for a phase observable, so the two cached bands are
/// deterministic across epochs (the anchor and the tick must use the same bands).
int bandOrderKey(FrequencyBand b) { return static_cast<int>(b); }

}  // namespace

bool fastClockGeometricRange(const Model::Geometry& geom,
                             const Eigen::Vector3d& rx_ecef,
                             const RinexObservation& ro,
                             bool include_relativistic,
                             double& rho_m,
                             double& elevation_deg,
                             double& apriori_clk_m) {
    constexpr double c = Constants::SPEED_OF_LIGHT;

    // Seed the light time with a code pseudorange from this record.  The full model also
    // seeds with the pseudorange and not a geometric range (step 3 of
    // Geometry::computeEmissionState, ModelGeometry.cpp): the receiver clock offset
    // cancels in a pseudorange seed, while a light-time iteration on the geometric range
    // reintroduces it and produces severe outliers through the filter's receiver clock.
    double pr = 0.0;
    for (const auto& kv : ro.measurements) {
        const std::string& code = kv.first;
        if (obsTypeOf(code) != ObsType::Code) continue;
        const double v = kv.second;
        if (std::isfinite(v) && v > 1.0e6) { pr = v; break; }
    }
    if (pr <= 0.0) return false;

    GnssTime t_em = ro.time;
    t_em.tow -= pr / c;
    const double sat_clk = geom.getCorrections().getSatelliteClockCorrection(ro.satellite, t_em);
    t_em.tow -= sat_clk;   // seconds, matching ModelGeometry's convention
    while (t_em.tow < 0.0)                        { t_em.tow += TimeUtils::SECONDS_IN_WEEK; --t_em.week; }
    while (t_em.tow >= TimeUtils::SECONDS_IN_WEEK) { t_em.tow -= TimeUtils::SECONDS_IN_WEEK; ++t_em.week; }

    const auto pv = geom.getSatellitePV(ro.satellite, t_em);
    const Eigen::Vector3d& p_em = pv.first;
    if (!p_em.allFinite() || p_em.norm() < 1.0) return false;   // {0,0,0} is the failure sentinel

    // A-priori satellite clock in the model's convention, projected to metres.  The
    // polynomial/product part was already evaluated above for the light time.  The
    // periodic relativistic term is the one the full model applies downstream
    // (ModelGeometry's dtr; by IGS convention the clock product carries only the
    // constant and drift terms), gated by the same knob via @p include_relativistic and
    // computed on the emission-frame state as the full model does.  Evaluating the
    // polynomial at the seed emission time rather than the clock-refined one changes it
    // by the clock drift over about 1 ms of light time: micrometres.
    double dtr_s = 0.0;
    if (include_relativistic)
        dtr_s = geom.getCorrections().computeRelativisticCorrection(ro.satellite, p_em, pv.second);
    apriori_clk_m = c * (sat_clk + dtr_s);

    // Sagnac: rotate the emission-frame satellite position into the reception-time ECEF
    // frame.  GnssCorrections::computeSagnacCorrection is not added on top: it is a
    // diagnostic scalar, and this rotation already contains the effect.
    const double geom_tof = (p_em - rx_ecef).norm() / c;
    const double theta = Constants::EARTH_ANGULAR_VELOCITY * geom_tof;
    const double ct = std::cos(theta), st = std::sin(theta);
    Eigen::Matrix3d R_earth;
    R_earth <<  ct,  st, 0.0,
               -st,  ct, 0.0,
               0.0, 0.0, 1.0;
    const Eigen::Vector3d sat_pos = R_earth * p_em;

    rho_m = (sat_pos - rx_ecef).norm();
    elevation_deg = Model::Geometry::computeTopocentric(rx_ecef, sat_pos).elevation_deg;
    return std::isfinite(rho_m) && rho_m > 1.0e6 && std::isfinite(apriori_clk_m);
}

void networkFastClockHarvest(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    const std::vector<bool>& present,
    const std::vector<std::vector<PppObservation>>& modeled,
    const NetworkSolutionEpoch& sol,
    const std::unique_ptr<PppUtils>& fc_ppp_utils) {
    // --- alias preamble (seam contract: the body reads the driver's original names) ---
    const auto& nc = config_.getNetworkConfig();
    const auto& fc = nc.fast_clock;
    auto& carry = ctx.carry.fast_clock;
    auto& stations = setup.stations;
    // The a-priori clock correction follows the full model's relativistic knob, so the
    // corrected phase matches the model under any configuration.
    const bool apriori_rel = config_.getModelCorrections().enable_relativistic_correction;

    if (!fc.enable || !fc_ppp_utils) return;

    const double now_s = TimeUtils::continuousGpsSeconds(se.time);

    // The anchor advances only on an emitted epoch (the caller ensures this by observing
    // n_emitted across networkEmitStore).  fastClockAdvanceArm is the pure decision; its
    // contract explains why the streak break is inferred from the gap.
    const FastClockArmDecision arm =
        fastClockAdvanceArm(carry.t0_s, carry.consecutive_emitted, now_s, fc.emit_max_gap_s);

    // Keep the outgoing anchor aside: it is the finite-difference base for the lump rate.
    // A broken chain drops it, because the cached LOS then refer to an anchor on the far
    // side of a break in the consumer's continuity, and differencing into it would measure
    // against a frame the consumer does not hold.
    std::map<std::string, std::vector<FastClockLos>> prev_los;
    const double prev_t0_s = carry.t0_s;
    if (!arm.chain_broken) prev_los.swap(carry.los);
    carry.los.clear();
    carry.c0_emitted.clear();

    const double dt_rate = arm.chain_broken ? 0.0 : (now_s - prev_t0_s);

    // Fast lookup into the outgoing anchor: (sat, station index) -> its cached LOS.
    auto findPrev = [&](const std::string& sat, int sta_idx) -> const FastClockLos* {
        const auto it = prev_los.find(sat);
        if (it == prev_los.end()) return nullptr;
        for (const FastClockLos& l : it->second)
            if (l.sta_idx == sta_idx) return &l;
        return nullptr;
    };

    std::size_t n_los = 0, n_dual = 0, n_single_skipped = 0, n_rho_fail = 0;

    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (i < present.size() && !present[i]) continue;
        if (i >= modeled.size()) continue;
        const auto sit = se.by_station.find(stations[i].name);
        if (sit == se.by_station.end()) continue;

        // Raw records for this station, indexed by satellite.  The harvest reads the raw
        // cycles (not PppObservation::observed) so that the anchor and the tick use the
        // same source through the same cached wavelength; a mismatch there would scale a
        // count of about 1e8 cycles into metres of error.
        std::unordered_map<std::string, const RinexObservation*> raw_of;
        raw_of.reserve(sit->second.size());
        for (const RinexObservation& ro : sit->second) raw_of.emplace(ro.satellite, &ro);

        const Model::Geometry& geom = stations[i].model->getGeometry();
        const Eigen::Vector3d& rx = stations[i].ecef;

        for (const PppObservation& po : modeled[i]) {
            if (po.broadcast_only) continue;   // phase dropped for this satellite
            const auto rit = raw_of.find(po.satellite);
            if (rit == raw_of.end()) continue;
            const RinexObservation& ro = *rit->second;

            // The admitted set is whatever the full solve modeled: signal_priorities,
            // elevation mask, eclipsing exclusion and slip handling have already been
            // applied to `modeled`.  The fast tier never widens it.
            struct Cand { FrequencyBand band; std::string code; };
            std::vector<Cand> cands;
            for (const auto& kv : po.observed) {
                const std::string& code = kv.first;
                if (obsTypeOf(code) != ObsType::Phase) continue;
                if (!ro.measurements.count(code)) continue;
                const double cyc = ro.measurements.at(code);
                if (!std::isfinite(cyc) || cyc == 0.0) continue;
                const auto sig = SignalId::fromObsCode(po.system, code);
                cands.push_back({sig ? sig->band() : FrequencyBand::Unknown, code});
            }
            if (cands.empty()) continue;
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                const int ka = bandOrderKey(a.band), kb = bandOrderKey(b.band);
                return ka != kb ? ka < kb : a.code < b.code;   // deterministic tie-break
            });
            // Two lowest distinct bands.  Same-band attribute variants (L1C vs L1W) carry
            // the same ionospheric coefficient, so a pair drawn from one band cannot form
            // an ionosphere-free combination at all.
            std::vector<Cand> picked;
            picked.push_back(cands.front());
            for (std::size_t k = 1; k < cands.size() && picked.size() < 2; ++k)
                if (cands[k].band != picked.front().band) picked.push_back(cands[k]);

            if (picked.size() < 2 && !fc.allow_single_band) { ++n_single_skipped; continue; }

            double rho = 0.0, el_deg = 0.0, apr_clk_m = 0.0;
            if (!fastClockGeometricRange(geom, rx, ro, apriori_rel, rho, el_deg, apr_clk_m)) {
                ++n_rho_fail;
                continue;
            }

            FastClockLos l;
            l.sta_idx = static_cast<int>(i);
            l.rho0 = rho;
            l.elevation_rad = el_deg * Constants::PI / 180.0;
            l.n_band = static_cast<int>(picked.size());

            const FastClockLos* prev = (dt_rate > 0.0) ? findPrev(po.satellite, l.sta_idx) : nullptr;
            bool ok = true;
            for (int b = 0; b < l.n_band; ++b) {
                FastClockLosBand& fb = l.band[static_cast<std::size_t>(b)];
                fb.signal = picked[static_cast<std::size_t>(b)].code;
                const double lambda =
                    fc_ppp_utils->getWavelength(po.system, fb.signal, po.satellite);
                if (!(lambda > 0.0)) { ok = false; break; }
                fb.freq_hz = Constants::SPEED_OF_LIGHT / lambda;
                fb.wavelength_m = lambda;
                // Corrected phase minus geometry: +c·δt_ap removes the a-priori satellite
                // clock (polynomial plus relativistic) from the lump, so the lump rate,
                // and with it the emitted ċ, is the residual clock rate in the frame the
                // producer emits, not the full physical rate the consumer's own a-priori
                // already advances (see @ref fc1_apriori).
                fb.lump = ro.measurements.at(fb.signal) * lambda - rho + apr_clk_m;
                if (!std::isfinite(fb.lump)) { ok = false; break; }

                // Rate: finite difference against the same (station, satellite, signal)
                // in the outgoing anchor.  A band that changed code (an attribute flip)
                // is treated as fresh: it is a different observable, and differencing
                // across it would fold the inter-channel bias into a clock rate.
                if (prev) {
                    for (int pb = 0; pb < prev->n_band; ++pb) {
                        const FastClockLosBand& pfb = prev->band[static_cast<std::size_t>(pb)];
                        if (pfb.signal != fb.signal) continue;
                        fb.lump_rate = (fb.lump - pfb.lump) / dt_rate;
                        fb.has_rate = std::isfinite(fb.lump_rate);
                        break;
                    }
                }
            }
            if (!ok) continue;

            carry.los[po.satellite].push_back(std::move(l));
            ++n_los;
            if (picked.size() == 2) ++n_dual;
        }
    }

    // --- Recover the residual satellite clock rate from the same lump rates ----------
    // With the a-priori clock removed from the lump (@ref fc1_apriori),
    // Mdot(r,s) = tau_dot_r - cdot^s + atmospheric rate, where cdot^s is the rate of the
    // residual (emitted-frame) clock.  The satellite (column) effect of a two-way median
    // polish over Mdot is therefore -cdot^s, once the datum station's row effect is
    // pinned to zero to match the frame of the tick's delta.
    //
    // The tick's observable subtracts Mdot*dt, so it estimates the clock change beyond
    // the linear trend.  Emitting anchor + delta alone, as SPEC section 8 states it, would
    // drop the drift, which over 30-300 s is the dominant part of the true clock motion.
    // The emitted value is anchor + cdot*dt + delta.
    //
    // A median polish is used instead of a second weighted solve because a slipped or
    // re-floated arc produces a wild Mdot, which a robust decomposition rejects.
    carry.c1_rate.clear();
    if (!arm.chain_broken && dt_rate > 0.0) {
        std::vector<std::tuple<int, std::string, double>> rate_cells;
        rate_cells.reserve(n_los);
        for (const auto& kv : carry.los) {
            for (const FastClockLos& l : kv.second) {
                if (l.n_band < 2 || !l.band[0].has_rate || !l.band[1].has_rate) continue;
                // IF-combine the two band rates so the ionospheric rate cancels in the
                // same way the ionospheric change does in the observable itself.
                const double f1 = l.band[0].freq_hz, f2 = l.band[1].freq_hz;
                const double d = f1 * f1 - f2 * f2;
                if (std::abs(d) < 1.0) continue;
                const double a1 = f1 * f1 / d, a2 = -f2 * f2 / d;
                const double r = a1 * l.band[0].lump_rate + a2 * l.band[1].lump_rate;
                if (std::isfinite(r)) rate_cells.emplace_back(l.sta_idx, kv.first, r);
            }
        }
        if (rate_cells.size() >= 4) {
            const MedianPolish rp = medianPolish2Way(rate_cells);
            int datum_row = -1;
            for (std::size_t i = 0; i < stations.size(); ++i)
                if (stations[i].name == nc.clock_datum_station) { datum_row = static_cast<int>(i); break; }
            const auto dit = rp.row_eff.find(datum_row);
            const double datum_row_eff = (dit != rp.row_eff.end()) ? dit->second : 0.0;
            // value = overall + row_eff[r] + col_eff[s] ~= tau_dot_r - cdot^s.  Pinning
            // tau_dot_datum := 0 fixes the split: cdot^s = -(overall + row_eff[datum] + col_eff[s]).
            for (const auto& ce : rp.col_eff) {
                const double cdot = -(rp.overall + datum_row_eff + ce.second);
                if (std::isfinite(cdot)) carry.c1_rate[ce.first] = cdot;
            }
        }
    }

    // The emitted clock per satellite, read after networkEmitStore: these are the values
    // the consumer received, which makes a fast row a delta in the emitted frame.  Any
    // per-era frame constant (re-base, datum, pivot, IF-pin realization) therefore
    // cancels, because re-bases happen only inside the full pipeline at era boundaries.
    for (const NetworkSatSolution& s : sol.sats)
        if (s.has_clock) carry.c0_emitted[s.sat] = s.clock_c0_m;

    carry.t_prev_s = arm.chain_broken ? 0.0 : prev_t0_s;
    carry.t0_s     = now_s;
    carry.t0_week  = se.time.week;
    carry.t0_tow   = se.time.tow;
    carry.datum_station = nc.clock_datum_station;
    carry.tick_seq = 0;
    carry.consecutive_emitted = arm.consecutive_emitted;
    // Armed requires two consecutive emitted epochs and an anchor that holds a
    // line-of-sight cache.  Reporting "armed" with an empty cache would make the tick's
    // zero-estimate outcome look like a data problem instead of an anchor problem.
    carry.armed = arm.armed && !carry.los.empty();

    ctx.counters.fc_anchor_los = n_los;
    ctx.counters.fc_anchor_sats = carry.los.size();
    if (n_rho_fail > 0) ctx.counters.fc_anchor_rho_fail += n_rho_fail;

    // Median |Mdot| is the one INC-1 quantity no other output of the run exposes, and it
    // checks that the lump rate is a rate: the lump's time variation is dominated by the
    // receiver clock, so Mdot should sit at the receiver's drift scale (sub-m/s for a
    // steered receiver, up to about 1e2 m/s for a free-running one) and not at the
    // ambiguity scale (about 1e7 m), which a mis-paired signal or a wavelength mismatch
    // would produce.
    double med_rate = 0.0;
    std::size_t n_rate = 0;
    {
        std::vector<double> rates;
        rates.reserve(n_los);
        for (const auto& kv : carry.los)
            for (const FastClockLos& l : kv.second)
                for (int b = 0; b < l.n_band; ++b)
                    if (l.band[static_cast<std::size_t>(b)].has_rate)
                        rates.push_back(std::abs(l.band[static_cast<std::size_t>(b)].lump_rate));
        n_rate = rates.size();
        if (!rates.empty()) {
            std::nth_element(rates.begin(), rates.begin() + rates.size() / 2, rates.end());
            med_rate = rates[rates.size() / 2];
        }
    }

    if (log && (carry.consecutive_emitted <= 2 || (carry.consecutive_emitted % 60) == 0))
        log->info("Network (FC-1 harvest): anchor at TOW {:.1f} — {} LOS ({} dual-band, "
                  "{} single-band skipped, {} without a usable range) over {} satellite(s); "
                  "{} clock(s) cached; {} band(s) with a rate, median |Mdot| = {:.4f} m/s "
                  "over dt {:.1f} s; streak {}{}.",
                  se.time.tow, n_los, n_dual, n_single_skipped, n_rho_fail, carry.los.size(),
                  carry.c0_emitted.size(), n_rate, med_rate, dt_rate,
                  carry.consecutive_emitted, carry.armed ? ", ARMED" : " (not yet armed)");
}

void networkFastClockTick(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    bool shadow_pass) {
    // --- alias preamble ---
    const auto& nc = config_.getNetworkConfig();
    const auto& fc = nc.fast_clock;
    auto& carry = ctx.carry.fast_clock;
    auto& ctr = ctx.counters;
    auto& stations = setup.stations;
    // Same relativistic gating as the harvest: anchor and tick must correct the phase
    // identically or the a-priori clock would survive into the delta.
    const bool apriori_rel = config_.getModelCorrections().enable_relativistic_correction;

    if (!fc.enable) return;

    const auto t_tick0 = std::chrono::steady_clock::now();
    ++ctr.fc_ticks_driven;
    ++carry.tick_seq;

    // The tick timer closes on every exit path, so early returns are included in the
    // measured cost, not only the successful path.
    struct TickTimer {
        NetworkServiceCounters& c;
        std::chrono::steady_clock::time_point t0;
        ~TickTimer() {
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            c.fc_wall_ms += ms;
            // The section-7.3 budget (<= 50 ms/tick at 305 stations) bounds the maximum,
            // not the mean: an over-budget tick at a real-time cadence is a missed epoch,
            // not a slower average.
            c.fc_wall_ms_max = std::max(c.fc_wall_ms_max, ms);
        }
    } tick_timer{ctr, t_tick0};

    // An unarmed anchor (fewer than two consecutive emitted full epochs: a cold start, a
    // warm resume, or the epoch after a withhold streak) is not an error; the tier waits
    // one full-solve era.
    if (!carry.armed) return;

    const double now_s = TimeUtils::continuousGpsSeconds(se.time);
    const double dt = now_s - carry.t0_s;
    if (!(dt > 0.0)) return;

    // --- Datum contract (SPEC section 2.3): the fast datum is the clock-datum pin of the
    // slow solve, and it is never re-elected inside a fast window.  If the pin is absent
    // this tick, the whole tick is withheld.  Re-datuming would change the frame of the
    // deltas without any signal to the consumer.
    int datum_idx = -1;
    for (std::size_t i = 0; i < stations.size(); ++i)
        if (stations[i].name == carry.datum_station) { datum_idx = static_cast<int>(i); break; }
    if (datum_idx < 0 || !se.by_station.count(carry.datum_station)) {
        ++ctr.fc_datum_withheld_ticks;
        if (log && ctr.fc_datum_withheld_ticks <= 3)
            log->warn("Network (FC-1): tick at TOW {:.1f} WITHHELD — clock-datum station "
                      "'{}' is absent this epoch.  The fast tier never re-elects a datum "
                      "inside a window (the frame would move with no way to tell the "
                      "consumer); it withholds instead.", se.time.tow, carry.datum_station);
        return;
    }

    // --- Per-LOS observable: y_IF = [Phi~(t) - Phi~(t0)] - [rho(t) - rho(t0)] - Mdot*dt,
    //     IF-combined over the arc's two cached bands, where Phi~ is the a-priori-clock-
    //     corrected phase (@ref fc1_apriori).  What remains is dtau_r(t) - dc^s(t) + eps,
    //     with dc^s the residual (emitted-frame) clock change beyond its linear trend.
    struct Cell {
        int sta;            // station index (median-polish row)
        std::string sat;    // satellite id (median-polish column)
        double y;           // IF-combined delta (m)
        double w;           // weight = 1/var
    };
    std::vector<Cell> cells;
    cells.reserve(carry.los.size() * 16);

    // One raw-record index per station, built once for the whole tick.
    std::unordered_map<int, const std::vector<RinexObservation>*> raw_by_sta;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        const auto it = se.by_station.find(stations[i].name);
        if (it != se.by_station.end()) raw_by_sta[static_cast<int>(i)] = &it->second;
    }

    const double phase_sigma =
        config_.getDefaultMeasurementNoise().phase_std_m > 0.0
            ? config_.getDefaultMeasurementNoise().phase_std_m : 0.002;
    const double phase_var_zen = phase_sigma * phase_sigma;

    std::size_t n_rho_fail = 0;
    for (const auto& kv : carry.los) {
        const std::string& sat = kv.first;
        for (const FastClockLos& l : kv.second) {
            const auto rit = raw_by_sta.find(l.sta_idx);
            if (rit == raw_by_sta.end()) continue;
            const RinexObservation* ro = nullptr;
            for (const RinexObservation& r : *rit->second)
                if (r.satellite == sat) { ro = &r; break; }
            if (!ro) continue;   // satellite set since t0; its clock is not extrapolated

            const Model::Geometry& geom =
                stations[static_cast<std::size_t>(l.sta_idx)].model->getGeometry();
            const Eigen::Vector3d& rx = stations[static_cast<std::size_t>(l.sta_idx)].ecef;
            double rho_t = 0.0, el_deg = 0.0, apr_clk_m = 0.0;
            if (!fastClockGeometricRange(geom, rx, *ro, apriori_rel, rho_t, el_deg, apr_clk_m)) {
                ++n_rho_fail;
                continue;
            }

            // Per-band delta from the anchor fit.  The cached wavelength is used, never a
            // freshly computed one (see FastClockLosBand::wavelength_m).
            double yb[2] = {0.0, 0.0};
            bool band_ok = true;
            for (int b = 0; b < l.n_band; ++b) {
                const FastClockLosBand& fb = l.band[static_cast<std::size_t>(b)];
                const auto mit = ro->measurements.find(fb.signal);
                if (mit == ro->measurements.end() || !std::isfinite(mit->second)
                    || mit->second == 0.0) { band_ok = false; break; }
                // The same a-priori clock correction the anchor's lump carries: the delta
                // must difference two identically corrected phases, or the full a-priori
                // clock (km scale) would appear uncorrected in y.
                const double phi_m = mit->second * fb.wavelength_m + apr_clk_m;
                yb[b] = phi_m - (rho_t + fb.lump + (fb.has_rate ? fb.lump_rate * dt : 0.0));
                if (!std::isfinite(yb[b])) { band_ok = false; break; }
            }
            if (!band_ok) continue;   // arc lost a band since t0

            double y = 0.0, var = 0.0;
            if (l.n_band >= 2) {
                // Ionosphere-free combination of the two per-band deltas: the first-order
                // ionospheric change cancels exactly, because mu_j scales identically
                // whether it multiplies I or dI.
                const double f1 = l.band[0].freq_hz, f2 = l.band[1].freq_hz;
                const double d = f1 * f1 - f2 * f2;
                if (std::abs(d) < 1.0) continue;   // same-band pair cannot form IF
                const double a1 =  f1 * f1 / d;
                const double a2 = -f2 * f2 / d;
                y = a1 * yb[0] + a2 * yb[1];
                var = (a1 * a1 + a2 * a2) * phase_var_zen;
            } else {
                // Single band: mu_j*dI survives.  Admitted only under an explicit knob.
                y = yb[0];
                var = phase_var_zen;
            }
            // Elevation amplification var ~ 1/sin(el), i.e. a fixed phase elevation power
            // of 0.5; this does not follow filter.phase_elevation_power (loader default
            // 1.0, var ~ 1/sin²(el)).  The factor 2 is for TDCP: y differences two
            // independent phase draws.
            const double sin_el = std::sin(std::max(el_deg, 1.0) * Constants::PI / 180.0);
            var = 2.0 * var / std::max(sin_el, 1e-3);
            if (!(var > 0.0) || !std::isfinite(y)) continue;

            cells.push_back({l.sta_idx, sat, y, 1.0 / var});
        }
    }
    if (cells.empty()) return;

    // --- Robust two-way seed (SPEC section 2.2 step 1).  Tukey median polish over the
    //     (station x satellite) bipartite structure: row effects are the receiver clock
    //     changes, column effects the satellite clock changes, and the interaction is the
    //     per-LOS residual after both are removed, which is the screening statistic.  The
    //     WLS below does not start from these effects; only the interactions are used.
    std::vector<std::tuple<int, std::string, double>> polish_in;
    polish_in.reserve(cells.size());
    for (const Cell& c : cells) polish_in.emplace_back(c.sta, c.sat, c.y);
    const MedianPolish mp = medianPolish2Way(polish_in);

    // --- Per-LOS screen (SPEC section 2.2 step 3), on an empirical scale.
    //
    // Deviation from the spec: section 2.2 sizes the screen with clockOscPredict's
    // q2-driven prediction spread.  That scale does not fit this statistic, for two
    // reasons: the satellite clock motion the spread describes has already been removed
    // as the column effect, and the project rule (from the MW thresholds) is that
    // detection thresholds come from the observable stream, not from the stochastic model.
    // The scale used here is the robust spread (1.4826·median |interaction|) of the
    // interactions, floored by the RMS formal sigma so that an unusually quiet epoch
    // cannot shrink the gate onto its own noise.
    std::vector<double> absint;
    absint.reserve(mp.interaction.size());
    for (double v : mp.interaction) absint.push_back(std::abs(v));
    double mad = 0.0;
    if (!absint.empty()) {
        std::vector<double> tmp = absint;
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        mad = 1.4826 * tmp[tmp.size() / 2];
    }
    double formal_floor = 0.0;
    for (const Cell& c : cells) formal_floor += 1.0 / c.w;
    formal_floor = std::sqrt(formal_floor / static_cast<double>(cells.size()));
    const double screen_scale = std::max(mad, formal_floor);
    const double screen_thresh = fc.screen_k_sigma * screen_scale;

    // A slip appears here as a lambda*dN jump, far above the gate.  Screened cells above
    // kSlipScaleM are also counted as slips, so the run-end readout shows whether the
    // tier is rejecting noise or absorbing slips.  This screen is the tier's event
    // response to a slip under invariant #1 (a stiff frozen quantity needs a wired,
    // tested event response); repair stays with the full pipeline.  A one-cycle slip on
    // GPS L1 or L2 moves the IF delta by 0.48 m or 0.38 m, so 0.10 m sits well below
    // either and well above phase noise.
    constexpr double kSlipScaleM = 0.10;   // about half an L1 cycle (lambda1 = 0.190 m)
    std::vector<char> keep(cells.size(), 1);
    std::size_t n_screened = 0, n_slip = 0;
    for (std::size_t k = 0; k < cells.size() && k < mp.interaction.size(); ++k) {
        if (std::abs(mp.interaction[k]) > screen_thresh) {
            keep[k] = 0;
            ++n_screened;
            if (std::abs(mp.interaction[k]) > kSlipScaleM) ++n_slip;
        }
    }

    // --- Estimability floors (SPEC section 2.2 step 4).  A satellite below the floor is
    //     not refreshed: the consumer keeps the last emitted value and its age grows.
    //     The clock is not extrapolated.
    std::map<std::string, int> los_per_sat;
    std::map<int, int> sats_per_sta;
    for (std::size_t k = 0; k < cells.size(); ++k)
        if (keep[k]) { ++los_per_sat[cells[k].sat]; ++sats_per_sta[cells[k].sta]; }
    for (std::size_t k = 0; k < cells.size(); ++k) {
        if (!keep[k]) continue;
        if (los_per_sat[cells[k].sat] < fc.min_stations_per_sat) keep[k] = 0;
        else if (sats_per_sta[cells[k].sta] < fc.min_sats_per_station) keep[k] = 0;
    }
    std::set<std::string> sat_set;
    std::set<int> sta_set;
    std::size_t n_used = 0;
    for (std::size_t k = 0; k < cells.size(); ++k)
        if (keep[k]) { sat_set.insert(cells[k].sat); sta_set.insert(cells[k].sta); ++n_used; }
    // Anchored satellites with no surviving cell this tick (screened, below a floor, or
    // with no cell formed at all).
    for (const auto& kv : carry.los)
        if (!sat_set.count(kv.first)) ++ctr.fc_sat_below_floor;
    if (sat_set.empty() || n_used == 0) {
        ctr.fc_los_screened += n_screened;
        ctr.fc_screen_slips += n_slip;
        return;
    }

    // --- One weighted least-squares pass with the station clocks Schur-eliminated
    //     (SPEC section 2.2 step 2).  Model: y_k = tau_{r(k)} - c_{s(k)}, with
    //     tau_datum := 0 realizing the frame.  Eliminating tau analytically leaves a dense
    //     n_sat x n_sat SPD system (32x32 at GPS scale) that solves in microseconds.  It
    //     stays serial so that the result is deterministic.
    std::map<std::string, int> sat_ix;
    for (const std::string& s : sat_set) { const int n = static_cast<int>(sat_ix.size()); sat_ix[s] = n; }
    const int ns = static_cast<int>(sat_ix.size());

    std::map<int, double> W_r, Wy_r;          // per-station weight sum and weighted y sum
    for (std::size_t k = 0; k < cells.size(); ++k) {
        if (!keep[k]) continue;
        W_r[cells[k].sta]  += cells[k].w;
        Wy_r[cells[k].sta] += cells[k].w * cells[k].y;
    }
    // Per-station weighted satellite-incidence row: g_r[s] = sum_{j in r, s(j)=s} w_j.
    std::map<int, std::map<int, double>> g_r;
    for (std::size_t k = 0; k < cells.size(); ++k)
        if (keep[k]) g_r[cells[k].sta][sat_ix[cells[k].sat]] += cells[k].w;

    Eigen::MatrixXd N = Eigen::MatrixXd::Zero(ns, ns);
    Eigen::VectorXd b = Eigen::VectorXd::Zero(ns);
    for (std::size_t k = 0; k < cells.size(); ++k) {
        if (!keep[k]) continue;
        const int s = sat_ix[cells[k].sat];
        const int r = cells[k].sta;
        const double w = cells[k].w;
        N(s, s) += w;
        b(s)    -= w * cells[k].y;
        if (r == datum_idx) continue;         // tau_datum == 0: no elimination term
        const double Wr = W_r[r];
        if (!(Wr > 0.0)) continue;
        b(s) += w * (Wy_r[r] / Wr);
        for (const auto& gs : g_r[r]) N(s, gs.first) -= w * gs.second / Wr;
    }

    Eigen::LDLT<Eigen::MatrixXd> ldlt(N);
    if (ldlt.info() != Eigen::Success) {
        if (log)
            log->warn("Network (FC-1): tick at TOW {:.1f} — the {}x{} Schur system is not "
                      "factorable; no clocks refreshed this tick.", se.time.tow, ns, ns);
        ctr.fc_los_screened += n_screened;
        ctr.fc_screen_slips += n_slip;
        return;
    }
    const Eigen::VectorXd c_hat = ldlt.solve(b);
    if (!c_hat.allFinite()) {
        ctr.fc_los_screened += n_screened;
        ctr.fc_screen_slips += n_slip;
        return;
    }

    // Post-fit sigma0 and per-satellite formal sigma.  sigma0 scales the formal covariance
    // so the reported sigma reflects this tick's misfit, not the a-priori model.
    double sse = 0.0;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        if (!keep[k]) continue;
        const int r = cells[k].sta;
        double tau = 0.0;
        if (r != datum_idx && W_r[r] > 0.0) {
            double acc = Wy_r[r];
            for (const auto& gs : g_r[r]) acc += gs.second * c_hat(gs.first);
            tau = acc / W_r[r];
        }
        const double v = cells[k].y - tau + c_hat(sat_ix[cells[k].sat]);
        sse += cells[k].w * v * v;
    }
    const int dof = static_cast<int>(n_used) - ns - static_cast<int>(sta_set.size()) + 1;
    const double s0sq = (dof > 0) ? sse / static_cast<double>(dof) : 1.0;
    const Eigen::MatrixXd Ninv = ldlt.solve(Eigen::MatrixXd::Identity(ns, ns));

    // --- R11 emission-continuity gate (SPEC section 4.4).  Keyed on the last emitted full
    //     solve, not on solve continuity: a withheld era advances the solve clock while
    //     the consumer receives nothing, so a solve-keyed gate would publish a delta
    //     measured inside an era the consumer never received.  A failed gate emits
    //     nothing rather than an extrapolated value.  This block only classifies; the
    //     emission block below (INC-3) acts on would_emit.
    const bool gap_ok = (dt <= fc.emit_max_gap_s);
    const bool would_emit = fc.emit && gap_ok && !shadow_pass;
    if (fc.emit && !gap_ok && !shadow_pass) ++ctr.fc_gap_withheld_ticks;

    ++ctr.fc_ticks_estimated;
    ctr.fc_los_used     += n_used;
    ctr.fc_los_screened += n_screened;
    ctr.fc_screen_slips += n_slip;

    // --- Emission (INC-3).  The published value is
    //         c0(t) = c0_emitted(t0) + cdot^s * dt + delta_c^s(t)
    //     The middle term is required: the observable subtracted Mdot*dt, so delta_c is
    //     the change beyond the linear trend, and over 30-300 s that trend is the dominant
    //     part of the true clock motion.  SPEC section 8's "c0_emitted(t0) + delta_c"
    //     would publish a systematically de-trended clock, worse than plain ZOH.
    //     cdot^s is the residual (emitted-frame) rate by construction, since the lump is
    //     built on a-priori-corrected phase, so K advances only what the consumer's own
    //     a-priori evaluation does not already advance (@ref fc1_apriori describes the
    //     double count this prevents).
    std::vector<std::tuple<std::string, double, double>> emit_rows;
    if (would_emit) emit_rows.reserve(sat_ix.size());
    for (const auto& sv : sat_ix) {
        const auto c0it = carry.c0_emitted.find(sv.first);
        if (c0it == carry.c0_emitted.end()) continue;   // no anchored value: skip the satellite
        const auto rit = carry.c1_rate.find(sv.first);
        const double drift = (rit != carry.c1_rate.end()) ? rit->second * dt : 0.0;
        const double c0_fast = c0it->second + drift + c_hat(sv.second);
        if (!std::isfinite(c0_fast)) continue;
        if (would_emit)
            emit_rows.emplace_back(sv.first, c0_fast,
                                   std::sqrt(std::max(0.0, s0sq * Ninv(sv.second, sv.second))));
    }
    if (would_emit && !emit_rows.empty() && setup.product_writer) {
        GnssTime anchor{};
        anchor.week = carry.t0_week;
        anchor.tow  = carry.t0_tow;
        setup.product_writer->writeFastClock(se.time, anchor, emit_rows);
        if (nc.product_flush_per_epoch) setup.product_writer->flush();
        ++ctr.fc_ticks_emitted;
        // Refresh the producer's own store too, so a closed-loop consumer sees the fast
        // clocks on the same terms as a file consumer: same value, same age, and the
        // bias and discontinuity state left untouched.
        for (const auto& r : emit_rows) {
            SsrClockCorrection c;
            c.sat           = std::get<0>(r);
            c.c0            = std::get<1>(r);
            c.received_time = se.time;
            c.epoch_time    = se.time;
            setup.network_store.pushFastClock(c);
        }
    }

    // --- Trace.  In shadow mode this row is the grading instrument; on a real tick it is
    //     the only per-epoch record of the tier (the full pipeline skipped this epoch).
    if (traces.fast_clock_trace.is_open()) {
        traces.fast_clock_trace
            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ",T,"
            << fmt::format("{:.3f}", dt) << ',' << (shadow_pass ? 1 : 0) << ','
            << n_used << ',' << n_screened << ',' << ns << ','
            << (would_emit ? 1 : 0) << ",,,\n";
        const int stride = fc.trace_sat_stride;
        if (stride > 0 && (carry.tick_seq % static_cast<std::size_t>(stride)) == 0) {
            for (const auto& sv : sat_ix) {
                const double sig = std::sqrt(std::max(0.0, s0sq * Ninv(sv.second, sv.second)));
                traces.fast_clock_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ",S,"
                    << fmt::format("{:.3f}", dt) << ',' << (shadow_pass ? 1 : 0) << ",,,,,"
                    << sv.first << ',' << fmt::format("{:.6f}", c_hat(sv.second)) << ','
                    << fmt::format("{:.6f}", sig) << '\n';
            }
        }
        // Flushed per tick: this stream grades runs that may be killed mid-flight, and an
        // unflushed tail would be lost.
        traces.fast_clock_trace.flush();
    }

    if (log && (carry.tick_seq == 1 || (ctr.fc_ticks_estimated % 200) == 0))
        log->info("Network (FC-1 tick): TOW {:.1f} dt {:.1f} s{} — {} LOS used / {} screened "
                  "(scale {:.4f} m, k {:.1f}), {} satellite(s) refreshed, sigma0 {:.2f}.",
                  se.time.tow, dt, shadow_pass ? " [shadow]" : "", n_used, n_screened,
                  screen_scale, fc.screen_k_sigma, ns, std::sqrt(s0sq));
}

} // namespace GnssProcessor
