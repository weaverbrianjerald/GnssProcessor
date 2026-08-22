/*!
 * \file GnssCorrections.cpp
 * \brief GNSS measurement and geodetic corrections.
 *
 * Organization:
 *  - Construction / product management
 *  - Clock interpolation
 *  - Relativistic & propagation corrections
 *  - Geophysical site displacements (pole tide, solid Earth tide, ocean/atm loading)
 *  - IERS 2010 Step 2 helpers (fundamental arguments, constituent tables)
 *  - Bias retrieval
 *  - Phase wind-up
 */

#include "corrections/GnssCorrections.hpp"
#include "math/Coordinates.hpp"
#include "common/Constants.hpp"
#include "config/GnssConfig.hpp"
#include "rinex/RinexParser.hpp"
#include "math/Interpolate.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <vector>
#include <fmt/format.h>

namespace GnssProcessor {

    // Shared validation constants for satellite geometry
    namespace {
        constexpr double MIN_SAT_NORM  = 2.0e7;  // ~20 000 km (lowest MEO orbit)
        constexpr double MAX_SAT_NORM  = 4.5e7;  // ~45 000 km (above BeiDou GEO)
        constexpr double MIN_RX_NORM   = 6.1e6;  // Earth surface min with margin
        constexpr double MAX_RX_NORM   = 6.6e6;  // Earth surface max + moderate altitude
        constexpr double MAX_VEL_NORM  = 5.0e3;  // ~5 km/s (GNSS satellites)
    }

    // =================================================================
    // Construction / product management
    // =================================================================

    GnssCorrections::GnssCorrections(LogManager& log_mgr)
        : log_manager_(log_mgr),
        // clocks_/erps_/biases_/orbits_ keep their in-class empty-shared default.
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (minimal)");
        buildClockIndex();
    }

    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const std::vector<ClockData>& clocks,
        const std::vector<ErpData>& erps)
        : log_manager_(log_mgr),
        // Unused legacy ctor: take a private copy (not a per-station hot path).
        clocks_(std::make_shared<const std::vector<ClockData>>(clocks)),
        erps_(std::make_shared<const std::vector<ErpData>>(erps)),
        config_(nullptr), rinex_parser_(nullptr),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed with {} clocks, {} erps",
            clocks_->size(), erps_->size());
        buildClockIndex();
    }

    GnssCorrections::GnssCorrections(LogManager& log_mgr,
        const GnssConfig& config,
        std::shared_ptr<const std::vector<ClockData>> clocks,
        std::shared_ptr<const std::vector<ErpData>> erps,
        std::shared_ptr<const std::vector<BiasData>> biases,
        const RinexParser& rinex)
        : log_manager_(log_mgr),
        // Share the cached copies directly (no per-station duplication).  Fall back
        // to an empty-shared vector if a caller passes null.
        clocks_(clocks ? std::move(clocks) : std::make_shared<const std::vector<ClockData>>()),
        erps_(erps ? std::move(erps) : std::make_shared<const std::vector<ErpData>>()),
        biases_(biases ? std::move(biases) : std::make_shared<const std::vector<BiasData>>()),
        config_(&config), rinex_parser_(&rinex),
        ocean_tide_model_(log_mgr) {
        log_manager_.getLogger("corrections")->trace("GnssCorrections constructed (full) with {} clocks, {} erps, {} biases",
            clocks_->size(), erps_->size(), biases_->size());
        buildClockIndex();
        buildBiasIndex();
    }

    void GnssCorrections::setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps) {
        // Override with caller-owned data (e.g. an SSR clock stream): take a private
        // copy behind a fresh shared_ptr, releasing the previously-shared cache copy.
        clocks_ = std::make_shared<const std::vector<ClockData>>(clocks);
        erps_ = std::make_shared<const std::vector<ErpData>>(erps);
        buildClockIndex();
        log_manager_.getLogger("corrections")->trace("GnssCorrections updated: {} clocks, {} erps", clocks_->size(), erps_->size());
    }

    void GnssCorrections::buildClockIndex() {
        clock_index_.clear();
        clock_index_.reserve(300);

        for (const auto& c : *clocks_)
            clock_index_[c.satellite].push_back(&c);

        for (auto& [sat, vec] : clock_index_) {
            std::sort(vec.begin(), vec.end(),
                [](const ClockData* a, const ClockData* b) {
                    return (a->time.week * TimeUtils::SECONDS_IN_WEEK + a->time.tow) <
                        (b->time.week * TimeUtils::SECONDS_IN_WEEK + b->time.tow);
                });
        }

        log_manager_.getLogger("corrections")->debug(
            "Built clock index: {} satellites indexed", clock_index_.size());
    }

    const SsrCorrectionStore& GnssCorrections::ssrStore() const {
        return ssr_store_override_ ? *ssr_store_override_
                                   : SsrCorrectionStore::getInstance();
    }

    const SsrCorrectionStore& GnssCorrections::orbitEchoStore() const {
        return orbit_echo_store_override_ ? *orbit_echo_store_override_
                                          : SsrCorrectionStore::getInstance();
    }

    // =================================================================
    // Clock interpolation
    // =================================================================

    double GnssCorrections::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time,
        bool* broadcast_grade) const {
        auto log = log_manager_.getLogger("ppp_model");

        // Provenance: "broadcast grade" means the returned clock came from the
        // broadcast polynomial with NO precise CLK sample and NO SSR clock delta
        // — i.e. metre-level.  Callers use this to drop carrier phase, which is
        // meaningless against a metre-level clock.  Default to false so error
        // paths (sat dropped anyway) never spuriously gate phase.
        if (broadcast_grade) *broadcast_grade = false;

        if (satellite.empty()) {
            log->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for clock correction for {}: week {}, TOW {:.3f}", satellite, time.week, time.tow);
            return 0.0;
        }

        // Precise CLK product first.  interpolateClock returns 0.0 when no clock
        // sample covers this epoch — an absent product, or every node outside
        // its MAX_WINDOW (e.g. a stale fixture shadowing a live rooftop run).
        // (It is safe to call with clocks_ empty: the clock index is then empty
        // and it returns 0.0, so the broadcast fallback below can still fire.)
        double clock_corr = interpolateClock(satellite, time, *clocks_);

        // SSR record up front so the broadcast baseline + IODE gate can select
        // the ephemeris VERSION the SSR clock delta was computed for (iode_ssr).
        // When the receiver has cut over to a newer ephemeris but the SSR still
        // references the prior one, this RE-REFERENCES to the retained previous
        // record (LiveEphemerisStore keeps current+previous across a cutover)
        // instead of dropping the satellite to broadcast-grade.
        // Reject an SSR record desynced from the obs epoch (live corrections vs a
        // lagging obs stream): getOrbitClock's freshness gate now also drops a
        // record far in the FUTURE relative to the obs (the −1.5 h desync), so a
        // stale/mis-timed SSR no longer detonates applyClockToBias.  Surface the
        // drop loudly (rate-limited by spdlog level) so it never hides again.
        //
        // RTA-9(b): the CLOCK apply window is widened by the stale-hold rung —
        // with processing.ssr_clock_stale_hold_factor > 0 a clock aged in
        // (ssr_max_age, factor × ssr_max_age] is still accepted here (the
        // C0+C1·dt polynomial extrapolates over the true dt below) instead of
        // dropping the satellite to broadcast-grade; MeasurementModel de-weights
        // the consuming rows through the URA-staleness q²·dt³/3 machinery using
        // the SAME clockHoldWindow.  Factor 0 (default) keeps the window at
        // ssr_max_age exactly — byte-identical.
        const double ssr_max_age = config_ ? config_->getSsrMaxAge() : 60.0;
        const double clk_hold    = config_ ? config_->getSsrClockStaleHoldFactor() : 0.0;
        const double apply_window =
            SsrCorrectionStore::clockHoldWindow(ssr_max_age, clk_hold);
        const auto ssr = ssrStore().getOrbitClock(satellite, time, apply_window);
        if (!ssr) {
            if (auto se = ssrStore().getSsrEpoch(satellite)) {
                const double age = TimeUtils::continuousGpsSeconds(time)
                                 - TimeUtils::continuousGpsSeconds(*se);
                if (std::abs(age) > apply_window)
                    log->warn("SSR for {} ({}) REJECTED stale: |age|={:.1f}s > {:.1f}s "
                              "(obs↔SSR desync) — falling back to broadcast-grade",
                              satellite, PppUtils::getSystemString(satellite), age, apply_window);
            }
        }
        // Card 7 (RTF-2 / plan D7): the broadcast BASELINE is pinned to the
        // version the CLOCK was referenced to.  A stamped clock IODE (brdc_rts
        // network clock: C0 = delta vs broadcast(clock_iode)) WINS over the
        // orbit anchor, so across a producer/consumer ephemeris-cutover skew
        // the store's current+previous retention re-references the baseline to
        // the producer's version instead of dropping the satellite.  Every
        // unstamped path has clock_iode == iode (combined / inherited, Card 3)
        // or clock_iode == -1 (falls through to iode) — byte-identical.
        const int  ssr_iode = (ssr && ssr->has_clock)
            ? (ssr->clock_iode >= 0 ? ssr->clock_iode : ssr->iode)
            : -1;

        // Live broadcast SV clock, version-matched to the SSR.  Its IODE ALWAYS
        // drives the gate — so a precise-CLK baseline no longer forces the gate
        // permissive (the CLK product carries no usable IODE).  It supplies the
        // clock BASELINE only when no precise CLK sample covered this epoch.  In
        // post-processing the live store is empty (bc == nullopt), so brdc_iode
        // stays -1 (permissive) and the prior SP3/CLK behaviour stands.
        const auto bc = LiveEphemerisStore::getInstance()
                            .computeSatelliteClock(satellite, time, ssr_iode);
        int  brdc_iode      = bc ? bc->iode : -1;
        bool used_broadcast = false;
        if (std::abs(clock_corr) < 1e-12 && bc) {
            clock_corr     = bc->bias;
            used_broadcast = true;
        }

        // SSR clock overlay (IGS-SSR): a delta to ADD to the broadcast/CLK
        // baseline, applied only when fresh, finite, and IODE-matched.  A
        // satellite carrying a fresh SSR clock is cm-grade, so it is NOT
        // broadcast-grade even when the baseline came from the broadcast
        // polynomial.
        bool ssr_clock_applied = false;
        if (ssr && ssr->has_clock) {
            // SSR diagnostic (trace, "corrections" logger) — one line per
            // (sat, epoch).  Exposes (1) the orbit/clock RATE terms the
            // between-update propagation depends on (delta_rac + delta_rac_dot*dt,
            // c0 + c1*dt + c2*dt^2) and the age dt; (2) the clock BASELINE the SSR
            // delta is stacked on (baseline_clk is SECONDS — the broadcast SV
            // clock — plus used_broadcast); (3) the IODE bookkeeping:
            //   eph_iode  = CURRENT live broadcast ephemeris IODE (steps at a cutover)
            //   iode_ssr  = the ephemeris version the SSR delta was computed for
            //   brdc_iode = the version the gate USED (re-referenced to match
            //               iode_ssr — current or retained previous — when possible)
            // eph_iode != brdc_iode == iode_ssr with iode_match=true means a
            // cutover desync was re-referenced (not dropped); iode_match=false
            // means no held version matched and the satellite is dropped.  Inert
            // unless "corrections" is at trace.
            if (auto clog = log_manager_.getLogger("corrections");
                clog->should_log(spdlog::level::trace)) {
                int eph_iode = -2;   // CURRENT version (no SSR target); -2 = no live clock
                if (auto cur = LiveEphemerisStore::getInstance()
                                  .computeSatelliteClock(satellite, time))
                    eph_iode = cur->iode;
                clog->trace(
                    "SSR-RATE {} sys={} has_orbit={} has_clock={} dt={:.1f}s "
                    "rac(r,a,c)=[{:+.4f},{:+.4f},{:+.4f}]m "
                    "racdot(r,a,c)=[{:+.3e},{:+.3e},{:+.3e}]m/s "
                    "c0={:+.4f}m c1={:+.3e}m/s c2={:+.3e}m/s2 hr={:+.4f}m "
                    "baseline_clk={:+.6f}s used_broadcast={} "
                    "eph_iode={} iode_ssr={} brdc_iode={} iode_match={}",
                    satellite, PppUtils::getSystemString(satellite),
                    ssr->has_orbit, ssr->has_clock,
                    TimeUtils::continuousGpsSeconds(time)
                        - TimeUtils::continuousGpsSeconds(ssr->epoch_time),
                    ssr->delta_rac.x(), ssr->delta_rac.y(), ssr->delta_rac.z(),
                    ssr->delta_rac_dot.x(), ssr->delta_rac_dot.y(), ssr->delta_rac_dot.z(),
                    ssr->c0, ssr->c1, ssr->c2, ssr->hr_clock,
                    clock_corr, used_broadcast,
                    eph_iode, ssr->clock_iode, brdc_iode,
                    SsrCorrectionStore::iodeMatches(brdc_iode, ssr->clock_iode));
            }
            // Card 7 (RTF-2 / conventions §2): orbit-coherence echo check.  A
            // brdc_rts clock echoes the RTS orbit it was estimated against;
            // broadcast radial-orbit error absorbs 1:1 into that clock, so
            // applying it against a DIFFERENT orbit update is a silent dm-m
            // range bias no residual QC can see.  Compare the echo against the
            // orbit record the consumer's overlay applies (orbitEchoStore —
            // under RTA-8 split routing a different store than the clock's).
            // Self-coherent merges (a combined record: the clock's own orbit
            // part IS the echoed orbit) short-circuit without a second lookup.
            // Mismatches are COUNTED whenever an echo is present (RTA-13
            // count-only convention); application is WITHHELD only when the
            // skew budget (processing.network_consumer.orbit_clock_max_skew_s,
            // armed on the clock store) is set > 0.  No pre-Card-7 stream
            // stamps an echo on a separate clock record, so the default path
            // never reaches the lookup — byte-identical.
            bool echo_ok = true;
            if (ssr->orbit_iod_ssr >= 0
                && !(ssr->has_orbit
                     && ssr->applied_orbit_iod_ssr == ssr->orbit_iod_ssr)) {
                auto applied =
                    orbitEchoStore().getOrbitClock(satellite, time, ssr_max_age);
                // The echo contract is about the orbit the overlay ACTUALLY
                // APPLIES, not merely a record's presence: Geometry's apply
                // sites gate the RAC delta on iodeMatches(brdc_iode, iode)
                // against the pinned broadcast pick.  Replicate that gate
                // through the SAME pinned pick (pickForSsr serves both the PV
                // and clock evaluations of one SatRecord) — a fresh orbit
                // record whose referenced broadcast version is no longer held
                // is NOT applied by the overlay, so for coherence purposes it
                // counts as "no applied orbit" (fail-closed under the budget).
                // Post-processing (empty live store) stays permissive, exactly
                // like the overlay gate itself.
                if (applied && applied->has_orbit) {
                    const auto obc = LiveEphemerisStore::getInstance()
                                         .computeSatelliteClock(satellite, time,
                                                                applied->iode);
                    const int orbit_brdc_iode = obc ? obc->iode : -1;
                    if (!SsrCorrectionStore::iodeMatches(orbit_brdc_iode,
                                                         applied->iode))
                        applied.reset();
                }
                const auto verdict =
                    ssrStore().checkOrbitEcho(satellite, *ssr, applied);
                const double skew_budget = ssrStore().orbitEchoMaxSkew();
                if (verdict != SsrCorrectionStore::OrbitEchoVerdict::Match
                    && verdict != SsrCorrectionStore::OrbitEchoVerdict::NotStamped
                    && skew_budget > 0.0) {
                    echo_ok = false;
                    log->debug(
                        "SSR clock for {} ({}) WITHHELD: orbit echo incoherent "
                        "({}) — echo oiod={} oepoch_tow={:.1f} vs applied "
                        "oiod={} oepoch_tow={:.1f} (budget {:.1f}s)",
                        satellite, PppUtils::getSystemString(satellite),
                        verdict == SsrCorrectionStore::OrbitEchoVerdict::NoAppliedOrbit
                            ? "no applied SSR orbit"
                            : (verdict == SsrCorrectionStore::OrbitEchoVerdict::IodSsrMismatch
                                   ? "IOD-SSR mismatch" : "content-epoch skew"),
                        ssr->orbit_iod_ssr, ssr->orbit_epoch.tow,
                        applied && applied->has_orbit ? applied->applied_orbit_iod_ssr : -1,
                        applied && applied->has_orbit ? applied->orbit_epoch_time.tow : 0.0,
                        skew_budget);
                }
            }

            // RTF-2 (Card 3): the CLOCK application gates on the CLOCK IODE
            // (clock_iode), not the orbit anchor (ssr->iode).  For every current
            // path clock_iode == ssr->iode (standard clocks inherit the orbit
            // anchor in getOrbitClock; standalone network clocks stay -1), so this
            // is byte-identical; only a brdc_rts producer's stamped clock IODE
            // makes it differ.  The ORBIT overlay gate (ModelGeometry) stays on iode.
            if (std::isfinite(clock_corr) && echo_ok
                && SsrCorrectionStore::iodeMatches(brdc_iode, ssr->clock_iode)) {
                const double dt = TimeUtils::continuousGpsSeconds(time)
                                - TimeUtils::continuousGpsSeconds(ssr->epoch_time);
                clock_corr += SsrCorrectionStore::applyClockToBias(
                    ssr->c0, ssr->c1, ssr->c2, ssr->hr_clock, dt);
                ssr_clock_applied = true;
                // RTA-9(b): surface a stale-HELD application (middle rung).  The
                // classification uses the content-epoch age — the same dt the
                // polynomial just extrapolated over and the same dt the
                // MeasurementModel R inflation grows with.
                if (clk_hold > 1.0
                    && SsrCorrectionStore::classifyClockAge(dt, ssr_max_age, clk_hold)
                           == SsrCorrectionStore::ClockAgeRung::StaleHeld) {
                    log->debug("SSR clock for {} ({}) STALE-HELD: age {:.1f}s in "
                               "({:.1f}, {:.1f}]s — applied (C0+C1·dt) with URA-staleness "
                               "R inflation on consuming rows",
                               satellite, PppUtils::getSystemString(satellite), dt,
                               ssr_max_age, apply_window);
                }
            } else {
                // Diagnostic: an SSR clock record IS present but was NOT applied —
                // almost always an IODE-convention mismatch (brdc vs ssr).  Logging
                // both IODs lets a live run distinguish a decode problem (garbage
                // ssr_iode) from a convention problem (sane-but-unequal) per
                // constellation (GAL low-8-bit IODnav, BDS mod(toe/720,240)).
                // echo_ok=false = the Card-7 orbit-coherence withhold (see the
                // per-event debug line above for the echo detail).
                log->trace("SSR clock present for {} ({}) but NOT applied: "
                           "brdc_iode={} clock_iode={} clock_finite={} echo_ok={}",
                           satellite, PppUtils::getSystemString(satellite),
                           brdc_iode, ssr->clock_iode, std::isfinite(clock_corr),
                           echo_ok);
            }
        }

        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-1) {
            // Phase is usable only when the clock is precise (CLK) or SSR-
            // corrected; a bare broadcast polynomial is metre-level.
            if (broadcast_grade) *broadcast_grade = used_broadcast && !ssr_clock_applied;
            log->trace("Satellite clock for {} ({}) at TOW {:.3f}: {:.3e} s{}",
                satellite, PppUtils::getSystemString(satellite), time.tow, clock_corr,
                used_broadcast ? (ssr_clock_applied ? " (broadcast+SSR)" : " (broadcast fallback)") : "");
            return clock_corr;
        }

        log->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, PppUtils::getSystemString(satellite), time.tow, clock_corr);
        return 0.0;
    }

    double GnssCorrections::interpolateClock(const std::string& satellite,
        const GnssTime& time,
        const std::vector<ClockData>& clocks) const
    {
        const SatelliteClockInterpolationMode mode = config_
            ? config_->getSatelliteClockInterpolationMode()
            : SatelliteClockInterpolationMode::Linear;

        constexpr double MAX_WINDOW     = 3600.0;
        constexpr double MAX_CLOCK_BIAS = 1e-3;
        constexpr size_t LAGRANGE_POINTS = 9;

        if (satellite.empty()) {
            log_manager_.getLogger("corrections")->debug("Clock interp: empty satellite ID");
            return 0.0;
        }

        auto idx_it = clock_index_.find(satellite);
        if (idx_it == clock_index_.end() || idx_it->second.empty()) {
            // Only worth flagging when a precise CLK product IS loaded but this
            // satellite is missing from it.  In live broadcast+SSR mode the whole
            // clock index is empty by design (no SP3/CLK), so this would fire for
            // EVERY satellite EVERY epoch — pure noise that buries the log while
            // the broadcast+SSR fallback in getSatelliteClockCorrection does the
            // real work.  Suppress it when there is no precise product at all.
            if (!clock_index_.empty())
                log_manager_.getLogger("corrections")->debug("Clock interp for {}: no data in index", satellite);
            return 0.0;
        }

        const auto& sat_clocks = idx_it->second;
        const double t_query = TimeUtils::continuousGpsSeconds(time);
        auto log = log_manager_.getLogger("corrections");

        auto valid_node = [&](const ClockData* c) -> bool {
            return std::isfinite(c->clock_bias)
                && std::abs(c->clock_bias) < MAX_CLOCK_BIAS
                && std::abs(TimeUtils::continuousGpsSeconds(c->time) - t_query) < MAX_WINDOW;
        };

        // Binary search helper used by both modes
        auto find_upper = [&]() {
            return std::lower_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
                [](const ClockData* c, double t) {
                    return TimeUtils::continuousGpsSeconds(c->time) < t;
                });
        };

        // ---------------------------------------------------------------
        // LINEAR MODE
        // ---------------------------------------------------------------
        if (mode == SatelliteClockInterpolationMode::Linear) {
            auto upper_it = find_upper();

            // Exact epoch match
            if (upper_it != sat_clocks.end()) {
                double t_node = TimeUtils::continuousGpsSeconds((*upper_it)->time);
                if (std::abs(t_node - t_query) < 1e-6 && valid_node(*upper_it))
                    return (*upper_it)->clock_bias;
            }

            // Boundary: fall back to nearest single node
            if (upper_it == sat_clocks.begin() || upper_it == sat_clocks.end()) {
                const ClockData* nearest = (upper_it == sat_clocks.end())
                    ? sat_clocks.back() : sat_clocks.front();
                if (valid_node(nearest)) {
                    log->debug("Clock interp (linear nearest) for {} at tow={:.3f}: boundary, result={:.3e}s",
                        satellite, time.tow, nearest->clock_bias);
                    return nearest->clock_bias;
                }
                return 0.0;
            }

            const ClockData* c_lo = *(upper_it - 1);
            const ClockData* c_hi = *upper_it;

            if (!valid_node(c_lo) || !valid_node(c_hi)) {
                log->warn("Clock interp (linear): invalid bracketing nodes for {} at tow={:.3f}", satellite, time.tow);
                return 0.0;
            }

            const double x0 = TimeUtils::continuousGpsSeconds(c_lo->time);
            const double x1 = TimeUtils::continuousGpsSeconds(c_hi->time);
            const double dt = x1 - x0;

            if (dt < 1e-9) {
                log->warn("Clock interp (linear): degenerate bracket dt={:.3e}s for {} at tow={:.3f}",
                    dt, satellite, time.tow);
                return c_lo->clock_bias;
            }

            // Reject interpolation across clock gaps
            constexpr double MAX_BRACKET_GAP = 65.0;
            if (dt > MAX_BRACKET_GAP) {
                log->warn("Clock interp (linear): bracket gap {:.1f}s exceeds {:.1f}s for {} at tow={:.3f} — rejecting",
                    dt, MAX_BRACKET_GAP, satellite, time.tow);
                return 0.0;
            }

            const double result = c_lo->clock_bias
                + (c_hi->clock_bias - c_lo->clock_bias) * (t_query - x0) / dt;

            log->debug("Clock interp (linear) for {} at tow={:.3f}: bracket=[{:.3f},{:.3f}]s span={:.1f}s "
                "result={:.3e}s ({:.3f}m)",
                satellite, time.tow, x0, x1, dt, result, result * Constants::SPEED_OF_LIGHT);
            return result;
        }

        // ---------------------------------------------------------------
        // LSQ SAT CLOCK MODE — weighted linear LSQ over multi-node window
        // with explicit rate-observation anchoring at coarse-cadence epochs.
        //
        // Fits c(t) ≈ a + b·(t − t_query) locally, where:
        //   - Value equations:  y_k = a + b·s_k + ν_k       (every node)
        //   - Rate equations:   m_j = b + μ_j                (rate-bearing nodes)
        //   - Weights from per-sample sigmas (CLK file) when present, otherwise
        //     defaults LSQ_DEFAULT_SIG_VAL / LSQ_DEFAULT_SIG_RAT below.
        //
        // Window strategy: bracket-centered, sized to span the previous and
        // next coarse-cadence rate-bearing nodes that contain t_query (~11
        // nodes for a 30 s file with rates every 5 min).  When t_query is
        // within LSQ_BOUNDARY_TOL_S of one of those rate nodes, the window
        // is extended by one rate window on that side — yielding 3 rate
        // constraints with a window roughly centered on the query.  Capped
        // at LSQ_MAX_NODES as a runaway guard against data gaps.
        //
        // Degrades gracefully to standard 2-node linear interpolation when
        // fewer than LSQ_MIN_NODES valid nodes are in the window or the
        // normal equations are singular.  Boundary queries (before first
        // or after last clock sample) hand off to single-node nearest, same
        // as the Linear path.
        //
        // Trade-off: gives up exact-node-passage (LSQ smooths instead of
        // interpolating), introducing ~mm-level discontinuities at clock
        // epochs when the window slides.  In exchange the observation-noise
        // component of the value estimate at the bracket midpoint drops by
        // ~√(N/2) versus plain linear — material when σ_observation ≫ σ_
        // process_per_sample (the typical regime for 30 s IGS Final clocks
        // in the seconds-to-minutes range).
        // ---------------------------------------------------------------
        if (mode == SatelliteClockInterpolationMode::LsqSatClock) {
            constexpr size_t LSQ_NOMINAL_NODES   = 11;        // 5-min span at 30 s cadence
            constexpr size_t LSQ_MAX_NODES       = 25;
            constexpr size_t LSQ_MIN_NODES       = 3;
            constexpr double LSQ_BOUNDARY_TOL_S  = 60.0;
            constexpr double LSQ_DEFAULT_SIG_VAL = 30e-12;    // s (IGS Final residual ~30 ps)
            constexpr double LSQ_DEFAULT_SIG_RAT = 0.5e-12;   // s/s (typical AC rate sigma)
            constexpr double MAX_BRACKET_GAP_LSQ = 65.0;      // matches Linear-mode guard

            const size_t n_total = sat_clocks.size();

            // Standard 2-node linear at the bracket, no logging here — the
            // LSQ branch logs the fallback once with full context above.
            auto linearFallback = [&](size_t lo, size_t hi) -> double {
                if (lo >= n_total || hi >= n_total) return 0.0;
                const ClockData* c_lo = sat_clocks[lo];
                const ClockData* c_hi = sat_clocks[hi];
                if (!valid_node(c_lo) || !valid_node(c_hi)) return 0.0;
                const double x0 = TimeUtils::continuousGpsSeconds(c_lo->time);
                const double x1 = TimeUtils::continuousGpsSeconds(c_hi->time);
                const double dt = x1 - x0;
                if (dt < 1e-9) return c_lo->clock_bias;
                if (dt > MAX_BRACKET_GAP_LSQ) return 0.0;
                return c_lo->clock_bias + (c_hi->clock_bias - c_lo->clock_bias)
                                        * (t_query - x0) / dt;
            };

            // --- Bracket location ---
            auto upper_bracket_it = std::upper_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
                [](double t, const ClockData* c) {
                    return t < TimeUtils::continuousGpsSeconds(c->time);
                });

            size_t bracket_lo;
            if (upper_bracket_it == sat_clocks.begin()) {
                bracket_lo = 0;
            } else if (upper_bracket_it == sat_clocks.end()) {
                bracket_lo = n_total - 1;
            } else {
                bracket_lo = std::distance(sat_clocks.begin(), upper_bracket_it) - 1;
            }

            // Exact-node shortcut (same as Linear) — preserves passage
            // through the exact sample value when t_query coincides.
            if (upper_bracket_it != sat_clocks.end()) {
                const double t_node = TimeUtils::continuousGpsSeconds((*upper_bracket_it)->time);
                if (std::abs(t_node - t_query) < 1e-6 && valid_node(*upper_bracket_it))
                    return (*upper_bracket_it)->clock_bias;
            }

            // --- Locate rate-bearing nodes bracketing t_query ---
            // r_prev: largest index ≤ bracket_lo where has_rate is true
            // r_next: smallest index > bracket_lo where has_rate is true
            // Both set to n_total if no such node exists in that direction.
            auto findRateLeft = [&](size_t from_inclusive) -> size_t {
                for (size_t i = from_inclusive + 1; i-- > 0; ) {
                    if (sat_clocks[i]->has_rate) return i;
                }
                return n_total;
            };
            auto findRateRight = [&](size_t from_inclusive) -> size_t {
                for (size_t i = from_inclusive; i < n_total; ++i) {
                    if (sat_clocks[i]->has_rate) return i;
                }
                return n_total;
            };

            const size_t r_prev = findRateLeft(bracket_lo);
            const size_t r_next = findRateRight(bracket_lo + 1);

            // --- Window selection ---
            size_t win_start, win_end;
            const size_t half = LSQ_NOMINAL_NODES / 2;

            if (r_prev == n_total && r_next == n_total) {
                // No rate-bearing nodes anywhere → value-only LSQ on a
                // bracket-centered nominal-width window.
                win_start = (bracket_lo >= half) ? bracket_lo - half : 0;
                win_end   = std::min(win_start + LSQ_NOMINAL_NODES, n_total);
            } else {
                size_t lo_idx = (r_prev != n_total) ? r_prev : 0;
                size_t hi_idx = (r_next != n_total) ? r_next : (n_total - 1);

                // Boundary proximity: when t_query is within
                // LSQ_BOUNDARY_TOL_S of a rate node, extend that side
                // to include the next rate node beyond — gives 3 rate
                // constraints with a roughly query-centered window.
                if (r_prev != n_total) {
                    const double t_prev = TimeUtils::continuousGpsSeconds(sat_clocks[r_prev]->time);
                    if (std::abs(t_query - t_prev) < LSQ_BOUNDARY_TOL_S && r_prev > 0) {
                        const size_t r_prev2 = findRateLeft(r_prev - 1);
                        if (r_prev2 != n_total && r_prev2 < lo_idx) lo_idx = r_prev2;
                    }
                }
                if (r_next != n_total) {
                    const double t_next = TimeUtils::continuousGpsSeconds(sat_clocks[r_next]->time);
                    if (std::abs(t_next - t_query) < LSQ_BOUNDARY_TOL_S && r_next + 1 < n_total) {
                        const size_t r_next2 = findRateRight(r_next + 1);
                        if (r_next2 != n_total && r_next2 > hi_idx) hi_idx = r_next2;
                    }
                }

                win_start = lo_idx;
                win_end   = std::min(hi_idx + 1, n_total);  // half-open
            }

            // Runaway cap (data gaps / sparse coverage) — re-center on
            // bracket and clip to LSQ_MAX_NODES width.
            if (win_end > win_start && (win_end - win_start) > LSQ_MAX_NODES) {
                const size_t max_half = LSQ_MAX_NODES / 2;
                win_start = (bracket_lo >= max_half) ? bracket_lo - max_half : 0;
                win_end   = std::min(win_start + LSQ_MAX_NODES, n_total);
            }

            // --- Collect valid nodes within the window ---
            std::vector<const ClockData*> nodes;
            nodes.reserve(win_end - win_start);
            for (size_t i = win_start; i < win_end; ++i) {
                if (valid_node(sat_clocks[i])) nodes.push_back(sat_clocks[i]);
            }

            // --- Graceful fallback when too few valid nodes for LSQ ---
            if (nodes.size() < LSQ_MIN_NODES) {
                // Boundary cases (before-first or after-last) → nearest
                // single-node value, same convention as Linear mode.
                if (upper_bracket_it == sat_clocks.begin() || upper_bracket_it == sat_clocks.end()) {
                    const ClockData* nearest = (upper_bracket_it == sat_clocks.end())
                        ? sat_clocks.back() : sat_clocks.front();
                    if (valid_node(nearest)) {
                        log->debug(
                            "Clock interp (LSQ→nearest-fallback) for {} at tow={:.3f}: only {} valid nodes — using nearest",
                            satellite, time.tow, nodes.size());
                        return nearest->clock_bias;
                    }
                    return 0.0;
                }
                const double result = linearFallback(bracket_lo, bracket_lo + 1);
                log->debug(
                    "Clock interp (LSQ→linear-fallback) for {} at tow={:.3f}: only {} valid nodes in window, "
                    "result={:.3e}s ({:.3f}m)",
                    satellite, time.tow, nodes.size(), result, result * Constants::SPEED_OF_LIGHT);
                return result;
            }

            // --- Weighted normal equations (centered at t_query) ---
            //
            //   [ Sw      Swx          ] [ a ]   [ Swy           ]
            //   [ Swx     Swxx + Sm    ] [ b ] = [ Swxy + Smy    ]
            //
            // s_k = (t_k − t_query) shift makes the fit value at t_query
            // equal to `a` directly (no separate evaluation step).  Rate
            // observations contribute to the slope row only (∂(a+b·t)/∂a = 0).
            double Sw = 0.0, Swx = 0.0, Swxx = 0.0, Swy = 0.0, Swxy = 0.0;
            double Sm = 0.0, Smy = 0.0;
            size_t n_rates = 0;

            for (const ClockData* c : nodes) {
                const double s = TimeUtils::continuousGpsSeconds(c->time) - t_query;
                const double y = c->clock_bias;
                const double sig_y = (c->clock_bias_sigma > 0.0)
                                     ? c->clock_bias_sigma : LSQ_DEFAULT_SIG_VAL;
                const double w_y = 1.0 / (sig_y * sig_y);
                Sw   += w_y;
                Swx  += w_y * s;
                Swxx += w_y * s * s;
                Swy  += w_y * y;
                Swxy += w_y * s * y;

                if (c->has_rate) {
                    const double m = c->clock_rate;
                    const double sig_m = (c->clock_rate_sigma > 0.0)
                                         ? c->clock_rate_sigma : LSQ_DEFAULT_SIG_RAT;
                    const double w_m = 1.0 / (sig_m * sig_m);
                    Sm  += w_m;
                    Smy += w_m * m;
                    ++n_rates;
                }
            }

            const double det = Sw * (Swxx + Sm) - Swx * Swx;
            if (!std::isfinite(det) || std::abs(det) < 1e-30) {
                // Singular normal equations — extremely rare (would imply
                // all sample times coincide).  Fall back to plain linear.
                const double fallback_hi = std::min<size_t>(bracket_lo + 1, n_total - 1);
                const double result = linearFallback(bracket_lo, fallback_hi);
                log->warn(
                    "Clock interp (LSQ→linear-fallback) for {} at tow={:.3f}: singular normal eqs (det={:.3e})",
                    satellite, time.tow, det);
                return result;
            }

            const double a = ((Swxx + Sm) * Swy - Swx * (Swxy + Smy)) / det;
            // const double b_slope = (-Swx * Swy + Sw * (Swxy + Smy)) / det;
            // ^ slope estimate available if needed downstream (currently unused)

            log->debug(
                "Clock interp (LSQ) for {} at tow={:.3f}: window=[{}..{}] ({} valid, {} with rates), "
                "result={:.3e}s ({:.3f}m)",
                satellite, time.tow, win_start, win_end - 1,
                nodes.size(), n_rates, a, a * Constants::SPEED_OF_LIGHT);

            return a;
        }

        // ---------------------------------------------------------------
        // LAGRANGE MODE — bracket-centered window of LAGRANGE_POINTS nodes
        //
        // Not recommended for standard 30 s IGS CLK files — use Linear instead.
        // Retained for high-rate (1 Hz) products.
        //
        // The window is anchored on the bracket [x_a, x_{a+1}] containing
        // t_query and slides by one node when t_query crosses x_{a+1}.  At
        // that instant x_{a+1} is a node of both windows so the polynomial
        // value is identical for both — clock is continuous across the
        // swap.  Selecting nodes by nearest-to-query instead shifts the
        // window at the midpoint between epochs, where the two polynomials
        // disagree, producing a per-epoch sawtooth.
        // ---------------------------------------------------------------
        const size_t n_total = sat_clocks.size();

        auto upper_bracket_it = std::upper_bound(sat_clocks.begin(), sat_clocks.end(), t_query,
            [](double t, const ClockData* c) {
                return t < TimeUtils::continuousGpsSeconds(c->time);
            });

        size_t bracket_lo;
        if (upper_bracket_it == sat_clocks.begin()) {
            bracket_lo = 0;
        } else if (upper_bracket_it == sat_clocks.end()) {
            bracket_lo = n_total - 1;
        } else {
            bracket_lo = std::distance(sat_clocks.begin(), upper_bracket_it) - 1;
        }

        const size_t n_win = std::min(LAGRANGE_POINTS, n_total);
        const size_t half_before = (n_win - 1) / 2;
        size_t start_idx;
        if (bracket_lo < half_before) {
            start_idx = 0;
        } else if (bracket_lo + (n_win - half_before) > n_total) {
            start_idx = n_total - n_win;
        } else {
            start_idx = bracket_lo - half_before;
        }
        const size_t end_idx = start_idx + n_win;

        std::vector<const ClockData*> nodes;
        nodes.reserve(LAGRANGE_POINTS);
        for (size_t i = start_idx; i < end_idx; ++i)
            if (valid_node(sat_clocks[i])) nodes.push_back(sat_clocks[i]);

        if (nodes.empty()) {
            log->warn("Clock interp (lagrange) for {} at tow={:.3f}: no valid nodes", satellite, time.tow);
            return 0.0;
        }
        if (nodes.size() == 1) return nodes[0]->clock_bias;

        // Extrapolation guard: fall back to linear to avoid oscillation
        const double t_first = TimeUtils::continuousGpsSeconds(nodes.front()->time);
        const double t_last  = TimeUtils::continuousGpsSeconds(nodes.back()->time);
        if (t_query < t_first || t_query > t_last) {
            std::sort(nodes.begin(), nodes.end(), [t_query](const ClockData* a, const ClockData* b) {
                return std::abs(TimeUtils::continuousGpsSeconds(a->time) - t_query) <
                       std::abs(TimeUtils::continuousGpsSeconds(b->time) - t_query);
            });
            const double x0 = TimeUtils::continuousGpsSeconds(nodes[0]->time);
            const double x1 = TimeUtils::continuousGpsSeconds(nodes[1]->time);
            const double dt = x1 - x0;
            const double result = (std::abs(dt) > 1e-9)
                ? nodes[0]->clock_bias + (nodes[1]->clock_bias - nodes[0]->clock_bias) * (t_query - x0) / dt
                : nodes[0]->clock_bias;
            log->debug("Clock interp (lagrange->linear extrap) for {} at tow={:.3f}: result={:.3e}s ({:.3f}m)",
                satellite, time.tow, result, result * Constants::SPEED_OF_LIGHT);
            return result;
        }

        const size_t n = nodes.size();
        std::vector<double> x(n), y(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = TimeUtils::continuousGpsSeconds(nodes[i]->time);
            y[i] = nodes[i]->clock_bias;
        }

        std::vector<double> w = Interpolate::barycentricWeights(x);
        const double result   = Interpolate::barycentricEval(x, w, y, t_query);

        log->debug("Clock interp (lagrange) for {} at tow={:.3f}: {} pts, span={:.1f}s, result={:.3e}s ({:.3f}m)",
            satellite, time.tow, n, x.back() - x.front(), result, result * Constants::SPEED_OF_LIGHT);
        return result;
    }

    // =================================================================
    // Relativistic & propagation corrections
    // =================================================================

    double GnssCorrections::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        // dtr = -2 * (r . v) / c^2  [seconds]
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MAX_CORRECTION = 1e-6;

        auto log = log_manager_.getLogger("ppp_model");
        std::string sys = PppUtils::getSystemString(satellite);

        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log->warn("Invalid relativistic inputs for {} ({}): pos=({:.3f},{:.3f},{:.3f}), vel=({:.3f},{:.3f},{:.3f})",
                satellite, sys, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }

        const double pos_norm = sat_pos.norm();
        const double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_SAT_NORM || pos_norm > MAX_SAT_NORM || vel_norm > MAX_VEL_NORM) {
            log->warn("Unrealistic relativistic norms for {} ({}): pos={:.3e} m, vel={:.3e} m/s",
                satellite, sys, pos_norm, vel_norm);
            return 0.0;
        }

        const double r_dot_v = sat_pos.dot(sat_vel);
        const double correction = -2.0 * r_dot_v / C_SQUARED;

        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log->warn("Excessive relativistic correction for {} ({}): {:.3e} s, r.v={:.3e}", satellite, sys, correction, r_dot_v);
            return 0.0;
        }

        log->trace("Relativistic correction for {} ({}): {:.3e} s, r.v={:.3e}", satellite, sys, correction, r_dot_v);
        return correction;
    }

    double GnssCorrections::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // rpath = (2*GM/c^2) * ln((r_sat + r_rec + r_sr) / (r_sat + r_rec - r_sr))  [meters]
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT;
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MAX_DELAY = 0.03;

        auto log = log_manager_.getLogger("ppp_model");
        std::string sys = PppUtils::getSystemString(satellite);

        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log->warn("Invalid Shapiro inputs for {} ({}): rx=({:.3f},{:.3f},{:.3f}), sat=({:.3f},{:.3f},{:.3f})",
                satellite, sys, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        const double rsat = sat_pos.norm();
        const double rrec = rx_pos.norm();
        const double rrs  = (sat_pos - rx_pos).norm();

        if (rrs < 1.5e7 || rsat < MIN_SAT_NORM || rsat > MAX_SAT_NORM) {
            log->warn("Unrealistic Shapiro norms for {} ({}): range={:.3e} m, sat={:.3e} m", satellite, sys, rrs, rsat);
            return 0.0;
        }

        const double delay = (2.0 * GM / C_SQUARED) * std::log((rsat + rrec + rrs) / (rsat + rrec - rrs));

        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log->warn("Invalid Shapiro delay for {} ({}): {:.3e} m", satellite, sys, delay);
            return 0.0;
        }

        log->trace("Shapiro delay for {} ({}): {:.6f} m, range={:.3e} m", satellite, sys, delay, rrs);
        return delay;
    }

    double GnssCorrections::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // correction = (omega_E / c) * (x_s * y_r - y_s * x_r)  [meters]
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5;
        constexpr double C = Constants::SPEED_OF_LIGHT;
        constexpr double MAX_SAGNAC = 40.0;

        auto log = log_manager_.getLogger("ppp_model");

        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log->warn("Invalid Sagnac inputs: rx=({},{},{}), sat=({},{},{})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log->warn("Unrealistic Sagnac norms: rx={:.3e} m, sat={:.3e} m", rx_norm, sat_norm);
            return 0.0;
        }

        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC) {
            log->warn("Excessive Sagnac correction: {:.3e} m", correction);
            return 0.0;
        }

        log->trace("Sagnac correction: {:.3f} m", correction);
        return correction;
    }

    // =================================================================
    // Geophysical site displacements
    // =================================================================

    // -----------------------------------------------------------------------
    // Ocean pole tide (IERS 2010 Sec. 7.1.5, Desai 2002) physical constants.
    //
    // Verbatim NGS M-PAGES values (Lib/Gnss/{Grs80,Iers}.hpp) so this port
    // reproduces M-PAGES Lib/Station/PoleTide.cpp::computeOceanPoleTide — and
    // the independent Python oracle scripts/ocean_pole_tide_reference.py — to
    // ~1e-12.  The Kappa scale bakes in AS2RAD so the wobble m1,m2 are passed in
    // arcseconds (consistent with computePoleTideCorrection above).
    // -----------------------------------------------------------------------
    namespace {
        constexpr double kOptOmega     = 7.292115e-5;       // grs80::OMEGA   [rad/s]
        constexpr double kOptAEarth    = 6378136.6;         // iers::A_EARTH  [m]
        constexpr double kOptGmEarth   = 3.986004418e14;    // iers::GM_EARTH [m^3/s^2]
        constexpr double kOptGNewton   = 6.67428e-11;       // iers::G        [m^3/kg/s^2]
        constexpr double kOptGE        = 9.7803278;         // iers::G_E      [m/s^2] mean equatorial gravity
        constexpr double kOptRhoSeawat = 1025.0;            // sea-water density [kg/m^3]
        constexpr double kOptGammaReal = 0.6870;            // Desai gamma_2^R
        constexpr double kOptGammaImag = 0.0036;            // Desai gamma_2^I

        // Kappa = 4*pi*G*a*rho_sw*Hp/(3*g_E) * AS2RAD, Hp = sqrt(8pi/15)*Omega^2*a^4/GM.
        // Evaluated once; constexpr so there is zero per-epoch cost.
        inline double oceanPoleTideKappa() {
            const double Hp = std::sqrt(8.0 * Constants::PI / 15.0)
                            * kOptOmega * kOptOmega
                            * kOptAEarth * kOptAEarth * kOptAEarth * kOptAEarth
                            / kOptGmEarth;
            const double K = 4.0 * Constants::PI * kOptGNewton * kOptAEarth
                           * kOptRhoSeawat * Hp / (3.0 * kOptGE);
            const double AS2RAD = Constants::PI / 648000.0;   // arcsec -> rad
            return K * AS2RAD;                                 // [m / arcsec]
        }
    }

    Eigen::Vector3d GnssCorrections::computeOceanPoleTideDisplacement(
        double m1_arcsec, double m2_arcsec, const std::array<double, 6>& opt_coeffs) {
        // opt_coeffs = [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I]
        const double kappa = oceanPoleTideKappa();
        const double cR = m1_arcsec * kOptGammaReal + m2_arcsec * kOptGammaImag;
        const double cI = m2_arcsec * kOptGammaReal - m1_arcsec * kOptGammaImag;
        const double east  = kappa * (cR * opt_coeffs[2] + cI * opt_coeffs[3]);
        const double north = kappa * (cR * opt_coeffs[0] + cI * opt_coeffs[1]);
        const double up    = kappa * (cR * opt_coeffs[4] + cI * opt_coeffs[5]);
        return Eigen::Vector3d(east, north, up);  // (East, North, Up) metres
    }

    Eigen::Vector3d GnssCorrections::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        auto log = log_manager_.getLogger("ppp_model");

        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log->warn("Invalid receiver position for pole tide at TOW {:.3f}: norm={:.3e}", time.tow, rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for pole tide: week {}, TOW {:.3f}", time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_->empty()) {
            log->warn("No ERP data for pole tide at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Linear interpolation of ERP (xp, yp) bracketing the query time.
        //
        // Previously this used std::min_element to pick the single nearest
        // ERP entry, which produced a discrete step in xp/yp at the midpoint
        // between consecutive ERP samples.  For typical daily ERP this is a
        // sub-mm step every 12 h at noon (well below noise), but for any
        // sub-daily ERP file the artifact grows linearly with the per-sample
        // polar motion delta.  Linear interpolation matches the Sun/Moon
        // path in Planetary::interpolateErp and eliminates the step.
        const double t_query = TimeUtils::continuousGpsSeconds(time);
        auto upper_it = std::upper_bound(erps_->begin(), erps_->end(), t_query,
            [](double t, const ErpData& e) {
                return t < TimeUtils::continuousGpsSeconds(e.time);
            });

        double xp_arcsec = 0.0, yp_arcsec = 0.0;
        if (upper_it == erps_->begin()) {
            // Query before first ERP sample — use first entry (clamped extrap).
            if (!std::isfinite(upper_it->xp) || !std::isfinite(upper_it->yp)) {
                log->trace("No valid ERP data for pole tide at TOW {:.3f}: first entry invalid", time.tow);
                return Eigen::Vector3d::Zero();
            }
            xp_arcsec = upper_it->xp;
            yp_arcsec = upper_it->yp;
        } else if (upper_it == erps_->end()) {
            // Query past last ERP sample — use last entry (clamped extrap).
            auto last = std::prev(erps_->end());
            if (!std::isfinite(last->xp) || !std::isfinite(last->yp)) {
                log->trace("No valid ERP data for pole tide at TOW {:.3f}: last entry invalid", time.tow);
                return Eigen::Vector3d::Zero();
            }
            xp_arcsec = last->xp;
            yp_arcsec = last->yp;
        } else {
            // Interior — linear interpolate between the bracketing pair.
            auto lo_it = std::prev(upper_it);
            if (!std::isfinite(lo_it->xp) || !std::isfinite(lo_it->yp) ||
                !std::isfinite(upper_it->xp) || !std::isfinite(upper_it->yp)) {
                log->trace("ERP bracket invalid for pole tide at TOW {:.3f}", time.tow);
                return Eigen::Vector3d::Zero();
            }
            const double t_lo = TimeUtils::continuousGpsSeconds(lo_it->time);
            const double t_hi = TimeUtils::continuousGpsSeconds(upper_it->time);
            const double dt   = t_hi - t_lo;
            if (dt < 1e-9) {
                xp_arcsec = lo_it->xp;
                yp_arcsec = lo_it->yp;
            } else {
                const double w = (t_query - t_lo) / dt;
                xp_arcsec = lo_it->xp + w * (upper_it->xp - lo_it->xp);
                yp_arcsec = lo_it->yp + w * (upper_it->yp - lo_it->yp);
            }
        }

        const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
        const double phi = llh.lat_rad;
        const double lambda = llh.lon_rad;

        // Mean pole: IERS 2018 secular linear model (IERS Conventions 2010
        // update, adopted by the IGS in August 2018).  This is the convention
        // to which the post-2018 IGS SP3/CLK/ERP products consumed by this
        // engine are referenced.  The older IERS2010 cubic mean pole diverges
        // by >0.1 arcsec by 2024; the previous linear truncation of that cubic
        // was wrong by ~0.2 arcsec, a large secular pole-tide bias.
        // Coefficients in mas; (decimal_year - 2000) in years.
        const double mjd = TimeUtils::gpsTimeToMjd(time);
        const double decimal_year = TimeUtils::mjdToDecimalYear(mjd);
        const double dt_years = decimal_year - 2000.0;
        const double xp_mean_mas = 55.0  + 1.677 * dt_years;
        const double yp_mean_mas = 320.5 + 3.460 * dt_years;

        // Wobble relative to the secular pole, in ARCSECONDS.  IERS 2010
        // Eq. 7.24 takes m1,m2 in arcsec and the -33/-9/+9 mm Love/Shida
        // coefficients yield the displacement directly in millimetres (scaled
        // to metres here).
        //
        // FIXED: the previous code formed m1/m2 in RADIANS (via AS2RAD) yet
        // applied the metre-valued coefficients, underscaling the entire
        // correction by ~AS2RAD (4.85e-6) so the pole tide was silently ~0
        // despite enable_pole_tide.  Keeping m1/m2 in arcsec restores the
        // correct mm-cm displacement, matching RTKLIB tide_pole and POINT
        // site/PoleTide.cpp.
        const double m1 =  xp_arcsec - xp_mean_mas * 1.0e-3;
        const double m2 = -(yp_arcsec - yp_mean_mas * 1.0e-3);

        // IERS 2010 Eq. 7.24 rotational-deformation pole tide (radial/north/east,
        // metres), written in geodetic latitude phi.  IERS gives the components in
        // colatitude theta = 90deg - phi: S_r = -33 sin(2 theta)(m1 cosL + m2 sinL),
        // S_theta = -9 cos(2 theta)(m1 cosL + m2 sinL), S_lambda = +9 cos(theta)
        // (m1 sinL - m2 cosL), with north = -S_theta.  Substituting theta = 90-phi
        // (sin 2theta = sin 2phi, cos 2theta = -cos 2phi, cos theta = sin phi):
        //   radial = -33 sin(2 phi)(m1 cosL + m2 sinL)
        //   north  = -9  cos(2 phi)(m1 cosL + m2 sinL)
        //   east   = +9  sin(phi)  (m1 sinL - m2 cosL)
        // FIXED 2026-06-24: the east term previously used cos(phi); the IERS
        // (and RTKLIB tide_pole) factor is sin(phi) = cos(colatitude).  The
        // radial/north terms were already in the geodetic-phi convention, so
        // cos(phi) on east was internally inconsistent (~1.48x scale at mid-lat
        // and the wrong latitude dependence — vanishing at the equator instead of
        // the pole), a sub-mm-to-mm east bias.
        double radial = -0.033 * std::sin(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        double north  = -0.009 * std::cos(2.0 * phi) * (m1 * std::cos(lambda) + m2 * std::sin(lambda));
        double east   =  0.009 * std::sin(phi)        * (m1 * std::sin(lambda) - m2 * std::cos(lambda));

        // Ocean pole tide (IERS 2010 Sec. 7.1.5, Desai 2002): add the load
        // displacement on top of the Sec. 7.1.4 rotational-deformation term when
        // the station's grid coefficients are available.  Default runs (no
        // coefficients loaded -> enable_ocean_pole_tide off) are byte-identical;
        // only the rot-def term above contributes.  Magnitude ~0.1-2 mm, well
        // inside the 5 cm guard below.
        if (has_opt_coeffs_) {
            const Eigen::Vector3d opt = computeOceanPoleTideDisplacement(m1, m2, opt_coeffs_);  // (E,N,U)
            east   += opt.x();
            north  += opt.y();
            radial += opt.z();
            log->trace("Ocean pole tide at TOW {:.3f}: E={:.3e} N={:.3e} U={:.3e} m",
                       time.tow, opt.x(), opt.y(), opt.z());
        }

        // Local (r,n,e) -> ECEF
        const double cp = std::cos(phi), sp = std::sin(phi);
        const double cl = std::cos(lambda), sl = std::sin(lambda);
        Eigen::Vector3d correction(
            radial * cp * cl - north * sp * cl - east * sl,
            radial * cp * sl - north * sp * sl + east * cl,
            radial * sp + north * cp);

        if (!correction.allFinite() || correction.norm() > 0.05) {
            log->warn("Invalid pole tide at TOW {:.3f}: norm={:.3e}", time.tow, correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log->trace("Pole tide at TOW {:.3f}: ({:.3e},{:.3e},{:.3e}) m, m1={:.3e}, m2={:.3e}",
            time.tow, correction.x(), correction.y(), correction.z(), m1, m2);
        return correction;
    }

    Eigen::Vector3d GnssCorrections::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        return ocean_tide_model_.computeDisplacement(rx_pos, time);
    }

    void GnssCorrections::setOceanTideCoefficients(const OtlStationCoefficients& coeffs) {
        ocean_tide_model_.setStationCoefficients(coeffs);
    }

    bool GnssCorrections::loadOceanTideFromEot20(const std::string& nc_path,
                                                 const Eigen::Vector3d& station_ecef) {
        OtlStationCoefficients coeffs;
        if (ocean_tide_model_.parseEot20Load(nc_path, station_ecef, coeffs)) {
            ocean_tide_model_.setStationCoefficients(coeffs);
            return true;
        }
        return false;
    }

    bool GnssCorrections::loadOceanTideFromBLQ(const std::string& blq_path, const std::string& station_name) {
        OtlStationCoefficients coeffs;
        if (ocean_tide_model_.parseBLQ(blq_path, station_name, coeffs)) {
            ocean_tide_model_.setStationCoefficients(coeffs);
            return true;
        }
        return false;
    }

    bool GnssCorrections::hasOceanTideCoefficients() const {
        return ocean_tide_model_.hasStationCoefficients();
    }

    void GnssCorrections::setOceanPoleTideCoefficients(const std::array<double, 6>& coeffs) {
        opt_coeffs_ = coeffs;
        has_opt_coeffs_ = true;
    }

    bool GnssCorrections::loadOceanPoleTideFromGrid(const std::string& grid_path,
                                                    const Eigen::Vector3d& station_ecef) {
        auto log = log_manager_.getLogger("ppp_model");

        if (!station_ecef.allFinite() || station_ecef.norm() < 1e6) {
            log->warn("OPT: invalid station ECEF for grid interpolation (norm={:.3e})",
                      station_ecef.norm());
            return false;
        }

        std::ifstream file(grid_path);
        if (!file.is_open()) {
            log->error("OPT: cannot open opole grid '{}'", grid_path);
            return false;
        }

        // Grid geometry (IERS Conventions 2010 chapter 7, opoleloadcoefcmcor.txt):
        //   720 longitudes:  lon[i] = 0.25  + 0.5*i, i = 0..719  (0.25 .. 359.75)
        //   360 latitudes:   lat[j] = -89.75 + 0.5*j, j = 0..359 (-89.75 .. 89.75)
        //   14 header lines, then nlat*nlon data rows, lat-major / lon-minor.
        //   Each row: lon lat u_r^R u_r^I u_n^R u_n^I u_e^R u_e^I.
        constexpr int kNlon = 720, kNlat = 360;
        constexpr double kLon0 = 0.25, kLat0 = -89.75, kStep = 0.5;

        std::string line;
        for (int i = 0; i < 14; ++i) {
            if (!std::getline(file, line)) {
                log->error("OPT: grid '{}' truncated in header", grid_path);
                return false;
            }
        }

        // Six coefficient planes (radial/north/east real & imag) flattened as
        // [j*nlon + i].  ~12 MB transient; freed when this method returns.
        std::vector<float> urR(static_cast<size_t>(kNlon) * kNlat), urI(urR.size()),
                           unR(urR.size()), unI(urR.size()),
                           ueR(urR.size()), ueI(urR.size());
        for (int j = 0; j < kNlat; ++j) {
            for (int i = 0; i < kNlon; ++i) {
                double lon_d, lat_d, rR, rI, nR, nI, eR, eI;
                if (!(file >> lon_d >> lat_d >> rR >> rI >> nR >> nI >> eR >> eI)) {
                    log->error("OPT: grid '{}' truncated at lat row {} lon col {}",
                               grid_path, j, i);
                    return false;
                }
                const size_t idx = static_cast<size_t>(j) * kNlon + i;
                urR[idx] = static_cast<float>(rR); urI[idx] = static_cast<float>(rI);
                unR[idx] = static_cast<float>(nR); unI[idx] = static_cast<float>(nI);
                ueR[idx] = static_cast<float>(eR); ueI[idx] = static_cast<float>(eI);
            }
        }

        // Station geodetic position; longitude wrapped to [0, 360).
        const Coordinates::Llh llh = Coordinates::ecefToLlh(station_ecef);
        const double lat_deg = llh.lat_rad * 180.0 / Constants::PI;
        double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
        if (lon_deg < 0.0) lon_deg += 360.0;

        // Bilinear interpolation indices.  Longitude wraps across the 360/0 seam;
        // latitude clamps at the polar grid edges (weights computed after the
        // clamp so the boundary rows interpolate correctly).
        const double fi = (lon_deg - kLon0) / kStep;
        double fj = (lat_deg - kLat0) / kStep;
        if (fj < 0.0) fj = 0.0;
        if (fj > kNlat - 1) fj = kNlat - 1;
        int j0 = static_cast<int>(std::floor(fj));
        if (j0 < 0) j0 = 0;
        if (j0 > kNlat - 2) j0 = kNlat - 2;      // keep j0+1 in range
        const int j1 = j0 + 1;
        const double wj = fj - j0;
        const int i0 = static_cast<int>(std::floor(fi));
        const double wi = fi - std::floor(fi);
        const int i0w = ((i0 % kNlon) + kNlon) % kNlon;
        const int i1w = (i0w + 1) % kNlon;

        auto bilerp = [&](const std::vector<float>& g) -> double {
            const double v00 = g[static_cast<size_t>(j0) * kNlon + i0w];
            const double v10 = g[static_cast<size_t>(j0) * kNlon + i1w];
            const double v01 = g[static_cast<size_t>(j1) * kNlon + i0w];
            const double v11 = g[static_cast<size_t>(j1) * kNlon + i1w];
            const double v0 = v00 * (1.0 - wi) + v10 * wi;
            const double v1 = v01 * (1.0 - wi) + v11 * wi;
            return v0 * (1.0 - wj) + v1 * wj;
        };

        // Store in optCoeffs order [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I].
        opt_coeffs_[0] = bilerp(unR);
        opt_coeffs_[1] = bilerp(unI);
        opt_coeffs_[2] = bilerp(ueR);
        opt_coeffs_[3] = bilerp(ueI);
        opt_coeffs_[4] = bilerp(urR);
        opt_coeffs_[5] = bilerp(urI);
        has_opt_coeffs_ = true;

        log->info("OPT: interpolated opole coeffs at ({:.4f},{:.4f}) deg lon/lat: "
                  "uN=({:.5f},{:.5f}) uE=({:.5f},{:.5f}) uR=({:.5f},{:.5f})",
                  lon_deg, lat_deg,
                  opt_coeffs_[0], opt_coeffs_[1], opt_coeffs_[2],
                  opt_coeffs_[3], opt_coeffs_[4], opt_coeffs_[5]);
        return true;
    }

    void GnssCorrections::setAtmosphericTideCoefficients(const std::array<double, 12>& coeffs) {
        atm_tide_coeffs_ = coeffs;
        has_atm_tide_coeffs_ = true;
    }

    Eigen::Vector3d GnssCorrections::computeAtmosphericTideDisplacement(
        const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        if (!has_atm_tide_coeffs_) {
            return Eigen::Vector3d::Zero();
        }
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn(
                "APL: invalid receiver position at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Local-solar-time arguments (sun-synchronous tide).  UT fraction of day
        // from the MJD; UT1-UTC (< 1 s) neglected, consistent with the OTL/solid
        // tide models.  lambda = station longitude (the local-solar-time offset).
        const Coordinates::Llh llh = Coordinates::ecefToLlh(rx_pos);
        const double ut_frac  = std::fmod(TimeUtils::gpsTimeToMjd(time), 1.0);
        const double lambda   = llh.lon_rad;
        const double theta_s1 = 2.0 * Constants::PI * ut_frac + lambda;
        const double theta_s2 = 2.0 * (2.0 * Constants::PI * ut_frac + lambda);

        const double c1 = std::cos(theta_s1), s1 = std::sin(theta_s1);
        const double c2 = std::cos(theta_s2), s2 = std::sin(theta_s2);

        // Per component d_c = aS1^cos*cos(S1) + aS1^sin*sin(S1)
        //                   + aS2^cos*cos(S2) + aS2^sin*sin(S2).
        // Layout per harmonic: [aE^cos, aE^sin, aN^cos, aN^sin, aU^cos, aU^sin];
        // index 0..5 = S1, 6..11 = S2.
        auto comp = [&](int base) {
            return atm_tide_coeffs_[base + 0] * c1 + atm_tide_coeffs_[base + 1] * s1
                 + atm_tide_coeffs_[base + 6] * c2 + atm_tide_coeffs_[base + 7] * s2;
        };
        const double d_e = comp(0);   // East
        const double d_n = comp(2);   // North
        const double d_u = comp(4);   // Up

        const Eigen::Vector3d enu(d_e, d_n, d_u);
        const Eigen::Vector3d ecef = Coordinates::enuToEcef(enu, llh);

        // Physical sanity guard: the S1+S2 site displacement is sub-cm (radial
        // typically <= ~1.5 mm).  Mirrors the OTL (>0.5 m) / pole-tide guards.
        if (!ecef.allFinite() || ecef.norm() > 0.01) {
            log_manager_.getLogger("ppp_model")->warn(
                "APL: S1/S2 displacement out of range at TOW {:.3f}: "
                "ENU=({:.5f},{:.5f},{:.5f}) m, norm={:.5f}",
                time.tow, d_e, d_n, d_u, ecef.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace(
            "APL S1/S2 displacement at TOW {:.3f}: ENU=({:.5f},{:.5f},{:.5f}) m, "
            "ECEF=({:.5f},{:.5f},{:.5f}) m",
            time.tow, d_e, d_n, d_u, ecef.x(), ecef.y(), ecef.z());

        return ecef;
    }

    bool GnssCorrections::loadAtmosphericTideFromGrid(const std::string& grid_path,
                                                      const Eigen::Vector3d& station_ecef) {
        auto log = log_manager_.getLogger("ppp_model");

        if (!station_ecef.allFinite() || station_ecef.norm() < 1e6) {
            log->warn("APL: invalid station ECEF for grid interpolation (norm={:.3e})",
                      station_ecef.norm());
            return false;
        }

        std::ifstream file(grid_path);
        if (!file.is_open()) {
            log->error("APL: cannot open atmospheric-tide grid '{}'", grid_path);
            return false;
        }

        // PROVISIONAL grid geometry — mirrors the IERS opole grid layout for two
        // harmonics.  The exact filename, header-line count, column order, and
        // frame (CM to match OTL) of the atmospheric S1/S2 load grid (Ray & Ponte
        // 2003 / IERS Conventions 2010 Sec. 7.1) MUST be pinned to the supplied
        // file before enabling in production.  Assumed: 0.5-deg global grid,
        // lat-major/lon-minor, each row:
        //   lon lat  S1[uE^R uE^I uN^R uN^I uU^R uU^I]  S2[uE^R uE^I uN^R uN^I uU^R uU^I]
        // where ^R/^I are in-phase(cos)/out-of-phase(sin) coefficients in metres.
        constexpr int kNlon = 720, kNlat = 360, kHeaderLines = 0;  // TODO: confirm header count
        constexpr double kLon0 = 0.25, kLat0 = -89.75, kStep = 0.5;
        constexpr int kNc = 12;

        std::string line;
        for (int i = 0; i < kHeaderLines; ++i) {
            if (!std::getline(file, line)) {
                log->error("APL: grid '{}' truncated in header", grid_path);
                return false;
            }
        }

        // 12 coefficient planes (S1 then S2, each E/N/U cos & sin) flattened [j*nlon+i].
        std::vector<std::vector<float>> plane(
            kNc, std::vector<float>(static_cast<size_t>(kNlon) * kNlat));
        for (int j = 0; j < kNlat; ++j) {
            for (int i = 0; i < kNlon; ++i) {
                double lon_d, lat_d;
                if (!(file >> lon_d >> lat_d)) {
                    log->error("APL: grid '{}' truncated at lat row {} lon col {}",
                               grid_path, j, i);
                    return false;
                }
                const size_t idx = static_cast<size_t>(j) * kNlon + i;
                for (int k = 0; k < kNc; ++k) {
                    double v;
                    if (!(file >> v)) {
                        log->error("APL: grid '{}' truncated reading coeff {} at ({},{})",
                                   grid_path, k, j, i);
                        return false;
                    }
                    plane[k][idx] = static_cast<float>(v);
                }
            }
        }

        // Station geodetic position; longitude wrapped to [0, 360).
        const Coordinates::Llh llh = Coordinates::ecefToLlh(station_ecef);
        const double lat_deg = llh.lat_rad * 180.0 / Constants::PI;
        double lon_deg = llh.lon_rad * 180.0 / Constants::PI;
        if (lon_deg < 0.0) lon_deg += 360.0;

        // Bilinear interpolation; longitude wraps the 360/0 seam, latitude clamps.
        const double fi = (lon_deg - kLon0) / kStep;
        double fj = (lat_deg - kLat0) / kStep;
        if (fj < 0.0) fj = 0.0;
        if (fj > kNlat - 1) fj = kNlat - 1;
        int j0 = static_cast<int>(std::floor(fj));
        if (j0 < 0) j0 = 0;
        if (j0 > kNlat - 2) j0 = kNlat - 2;
        const int j1 = j0 + 1;
        const double wj = fj - j0;
        const int i0 = static_cast<int>(std::floor(fi));
        const double wi = fi - std::floor(fi);
        const int i0w = ((i0 % kNlon) + kNlon) % kNlon;
        const int i1w = (i0w + 1) % kNlon;

        auto bilerp = [&](const std::vector<float>& g) -> double {
            const double v00 = g[static_cast<size_t>(j0) * kNlon + i0w];
            const double v10 = g[static_cast<size_t>(j0) * kNlon + i1w];
            const double v01 = g[static_cast<size_t>(j1) * kNlon + i0w];
            const double v11 = g[static_cast<size_t>(j1) * kNlon + i1w];
            const double v0 = v00 * (1.0 - wi) + v10 * wi;
            const double v1 = v01 * (1.0 - wi) + v11 * wi;
            return v0 * (1.0 - wj) + v1 * wj;
        };

        // Grid plane order [S1: uE^R uE^I uN^R uN^I uU^R uU^I, S2: ...] maps 1:1 to
        // the member layout [aE^cos, aE^sin, aN^cos, aN^sin, aU^cos, aU^sin] per
        // harmonic (cos==real==in-phase, sin==imag==out-of-phase).
        for (int k = 0; k < kNc; ++k) atm_tide_coeffs_[k] = bilerp(plane[k]);
        has_atm_tide_coeffs_ = true;

        log->info("APL: interpolated S1/S2 coeffs at ({:.4f},{:.4f}) deg lon/lat: "
                  "S1 U=({:.5f},{:.5f}) m, S2 U=({:.5f},{:.5f}) m",
                  lon_deg, lat_deg,
                  atm_tide_coeffs_[4], atm_tide_coeffs_[5],
                  atm_tide_coeffs_[10], atm_tide_coeffs_[11]);
        return true;
    }

    // =================================================================
    // Solid Earth tide (IERS 2010)
    // =================================================================

    double GnssCorrections::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos,
        const GnssTime& time) const {
        if (!receiver_pos.allFinite() || !sat_pos.allFinite() || !sun_pos.allFinite() || !moon_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Solid Earth tide: non-finite inputs");
            return 0.0;
        }

        // LOS unit vector (sat -> rx); the site displacement projects onto it.
        const Eigen::Vector3d rho = receiver_pos - sat_pos;
        const double rho_norm = rho.norm();
        if (!(rho_norm > 0.0)) return 0.0;
        const Eigen::Vector3d los = rho / rho_norm;

        // Step-2 time arguments for the frequency-dependent corrections:
        //   jc  = Julian centuries from J2000 (TT ~ UT at this precision),
        //         including the intra-day fraction;
        //   fhr = UT fractional hour of day, supplying the 15 deg/hr Earth-
        //         rotation term in the Doodson tau.
        // UT1-UTC and the GPS-UTC leap offset (~18 s) are neglected (<<0.1 mm
        // in the diurnal tide phase), consistent with the rest of the engine.
        const double mjd = TimeUtils::gpsTimeToMjd(time);
        const double jc  = (mjd - 51544.5) / TimeUtils::DAYS_PER_JULIAN_CENTURY;
        const double fhr = (mjd - std::floor(mjd)) * 24.0;

        const Eigen::Vector3d disp =
            computeSolidEarthTideDisplacement(receiver_pos, sun_pos, moon_pos, jc, fhr);

        const double stide = disp.dot(los);
        if (!std::isfinite(stide)) return 0.0;
        log_manager_.getLogger("ppp_model")->trace("Solid Earth tide: {:.4f} m", stide);
        return stide;
    }

    // =================================================================
    // IERS 2010 solid Earth tide — Step 1 (in-phase + out-of-phase) and
    // Step 2 (frequency-dependent) station-displacement helpers.
    //
    // Faithful port of the IERS Conventions dehanttideinel routine
    // (Dehant / Mathews / Gipson), cross-checked line-by-line against the
    // in-tree POINT C translation (reference_software/point/.../SolidEarth.cpp)
    // and validated against the official IERS dehanttideinel test vector
    // (see SolidEarthTideTest).  The permanent (zero-frequency) tide is
    // retained -> conventional tide-free coordinates, matching IGS/ITRF.
    //
    // Every helper returns geocentric ECEF displacement (meters) and takes
    // geocentric direction cosines: cPhi/sPhi (latitude), cLa/sLa (longitude).
    // =================================================================
    namespace {

        constexpr double kTideDeg2Rad = Constants::PI / 180.0;
        constexpr double kTideH20 = 0.6078, kTideL20 = 0.0847;  // nominal degree-2 Love/Shida
        constexpr double kTideH3  = 0.292,  kTideL3   = 0.015;  // nominal degree-3 Love/Shida
        constexpr double kTideMassRatioSun  = 332945.943062;    // M_sun  / M_earth
        constexpr double kTideMassRatioMoon = 0.012300034;      // M_moon / M_earth
        constexpr double kTideRe = 6378136.55;                  // IERS equatorial radius (m)

        // ST1IDIU: out-of-phase diurnal correction (mantle anelasticity).
        Eigen::Vector3d st1Diurnal(const Eigen::Vector3d& xSun, const Eigen::Vector3d& xMon,
                                   double fac2Sun, double fac2Mon,
                                   double cPhi, double sPhi, double cLa, double sLa) {
            constexpr double dhi = -0.0025, dli = -0.0007;
            const double c2Phi = cPhi * cPhi - sPhi * sPhi;
            double dr = 0.0, dn = 0.0, de = 0.0;
            const auto add = [&](const Eigen::Vector3d& b, double fac) {
                const double r2 = b.squaredNorm();
                const double g = b.z() * (b.x() * sLa - b.y() * cLa) / r2;
                dr += -3.0 * dhi * sPhi * cPhi * fac * g;
                dn += -3.0 * dli * c2Phi * fac * g;
                de += -3.0 * dli * sPhi * fac * b.z() * (b.x() * cLa + b.y() * sLa) / r2;
            };
            add(xSun, fac2Sun);
            add(xMon, fac2Mon);
            return Eigen::Vector3d(dr * cLa * cPhi - de * sLa - dn * sPhi * cLa,
                                   dr * sLa * cPhi + de * cLa - dn * sPhi * sLa,
                                   dr * sPhi + dn * cPhi);
        }

        // ST1ISEM: out-of-phase semidiurnal correction (mantle anelasticity).
        Eigen::Vector3d st1SemiD(const Eigen::Vector3d& xSun, const Eigen::Vector3d& xMon,
                                 double fac2Sun, double fac2Mon,
                                 double cPhi, double sPhi, double cLa, double sLa) {
            constexpr double dhi = -0.0022, dli = -0.0007;
            const double c2La = cLa * cLa - sLa * sLa;
            const double s2La = 2.0 * cLa * sLa;
            double dr = 0.0, dn = 0.0, de = 0.0;
            const auto add = [&](const Eigen::Vector3d& b, double fac) {
                const double r2 = b.squaredNorm();
                const double bx = b.x(), by = b.y();
                const double tA = (bx * bx - by * by) * s2La - 2.0 * bx * by * c2La;
                const double tB = (bx * bx - by * by) * c2La + 2.0 * bx * by * s2La;
                dr += (-3.0 / 4.0) * dhi * cPhi * cPhi * fac * tA / r2;
                dn += ( 3.0 / 2.0) * dli * sPhi * cPhi * fac * tA / r2;
                de += (-3.0 / 2.0) * dli * cPhi * fac * tB / r2;
            };
            add(xSun, fac2Sun);
            add(xMon, fac2Mon);
            return Eigen::Vector3d(dr * cLa * cPhi - de * sLa - dn * sPhi * cLa,
                                   dr * sLa * cPhi + de * cLa - dn * sPhi * sLa,
                                   dr * sPhi + dn * cPhi);
        }

        // ST1L1: latitude (L^1) dependence of the degree-2 Love numbers.
        // Horizontal only (no radial term); separate diurnal + semidiurnal bands.
        Eigen::Vector3d st1L1(const Eigen::Vector3d& xSun, const Eigen::Vector3d& xMon,
                              double fac2Sun, double fac2Mon,
                              double cPhi, double sPhi, double cLa, double sLa) {
            constexpr double l1D = 0.0012, l1SD = 0.0024;
            Eigen::Vector3d corr = Eigen::Vector3d::Zero();
            {   // diurnal band
                double dn = 0.0, de = 0.0;
                const auto add = [&](const Eigen::Vector3d& b, double fac) {
                    const double r2 = b.squaredNorm();
                    dn += -l1D * sPhi * sPhi * fac * b.z() * (b.x() * cLa + b.y() * sLa) / r2;
                    de +=  l1D * sPhi * (cPhi * cPhi - sPhi * sPhi) * fac * b.z()
                           * (b.x() * sLa - b.y() * cLa) / r2;
                };
                add(xSun, fac2Sun);
                add(xMon, fac2Mon);
                dn *= 3.0; de *= 3.0;
                corr.x() += -de * sLa - dn * sPhi * cLa;
                corr.y() +=  de * cLa - dn * sPhi * sLa;
                corr.z() +=  dn * cPhi;
            }
            {   // semidiurnal band
                const double c2La = cLa * cLa - sLa * sLa;
                const double s2La = 2.0 * cLa * sLa;
                double dn = 0.0, de = 0.0;
                const auto add = [&](const Eigen::Vector3d& b, double fac) {
                    const double r2 = b.squaredNorm();
                    const double bx = b.x(), by = b.y();
                    dn += -l1SD / 2.0 * sPhi * cPhi * fac
                          * ((bx * bx - by * by) * c2La + 2.0 * bx * by * s2La) / r2;
                    de += -l1SD / 2.0 * sPhi * sPhi * cPhi * fac
                          * ((bx * bx - by * by) * s2La - 2.0 * bx * by * c2La) / r2;
                };
                add(xSun, fac2Sun);
                add(xMon, fac2Mon);
                dn *= 3.0; de *= 3.0;
                corr.x() += -de * sLa - dn * sPhi * cLa;
                corr.y() +=  de * cLa - dn * sPhi * sLa;
                corr.z() +=  dn * cPhi;
            }
            return corr;
        }

        // ANGLES: Doodson fundamental arguments (degrees) from UT hour + Julian century.
        struct DoodsonAngles { double s, tau, h, p, zns, ps; };
        DoodsonAngles computeAngles(double fhr, double jc) {
            const double jc2 = jc * jc, jc3 = jc2 * jc, jc4 = jc2 * jc2;
            double s = 218.31664563 + 481267.88194 * jc - 0.0014663889 * jc2 + 0.00000185139 * jc3;
            const double tau = fhr * 15.0 + 280.4606184 + 36000.7700536 * jc
                             + 0.00038793 * jc2 - 0.0000000258 * jc3 - s;
            const double pr = 1.396971278 * jc + 0.000308889 * jc2
                            + 0.000000021 * jc3 + 0.000000007 * jc4;
            s += pr;
            const double h = 280.46645 + 36000.7697489 * jc + 0.00030322222 * jc2
                           + 0.000000020 * jc3 - 0.00000000654 * jc4;
            const double p = 83.35324312 + 4069.01363525 * jc - 0.01032172222 * jc2
                           - 0.0000124991 * jc3 + 0.00000005263 * jc4;
            const double zns = 234.95544499 + 1934.13626197 * jc - 0.00207561111 * jc2
                             - 0.00000213944 * jc3 + 0.00000001650 * jc4;
            const double ps = 282.93734098 + 1.71945766667 * jc + 0.00045688889 * jc2
                            - 0.00000001778 * jc3 - 0.00000000334 * jc4;
            return { std::fmod(s, 360.0), std::fmod(tau, 360.0), std::fmod(h, 360.0),
                     std::fmod(p, 360.0), std::fmod(zns, 360.0), std::fmod(ps, 360.0) };
        }

        // STEP2DIU: Step-2 diurnal frequency correction (IERS 2010 Table 7.3a, 31 lines).
        // Columns: Doodson multipliers [s,h,p,N',ps], then dR_in, dR_out, dT_in, dT_out (mm).
        Eigen::Vector3d st2Diurnal(double fhr, double jc, double lon,
                                   double cPhi, double sPhi, double cLa, double sLa) {
            static constexpr double T[31][9] = {
                {-3., 0., 2., 0., 0., -0.01,  0.00,  0.00,  0.00},
                {-3., 2., 0., 0., 0., -0.01,  0.00,  0.00,  0.00},
                {-2., 0., 1.,-1., 0., -0.02,  0.00,  0.00,  0.00},
                {-2., 0., 1., 0., 0., -0.08,  0.00, -0.01,  0.01},
                {-2., 2.,-1., 0., 0., -0.02,  0.00,  0.00,  0.00},
                {-1., 0., 0.,-1., 0., -0.10,  0.00,  0.00,  0.00},
                {-1., 0., 0., 0., 0., -0.51,  0.00, -0.02,  0.03},
                {-1., 2., 0., 0., 0.,  0.01,  0.00,  0.00,  0.00},
                { 0.,-2., 1., 0., 0.,  0.01,  0.00,  0.00,  0.00},
                { 0., 0.,-1., 0., 0.,  0.02,  0.00,  0.00,  0.00},
                { 0., 0., 1., 0., 0.,  0.06,  0.00,  0.00,  0.00},
                { 0., 0., 1., 1., 0.,  0.01,  0.00,  0.00,  0.00},
                { 0., 2.,-1., 0., 0.,  0.01,  0.00,  0.00,  0.00},
                { 1.,-3., 0., 0., 1., -0.06,  0.00,  0.00,  0.00},
                { 1.,-2., 0.,-1., 0.,  0.01,  0.00,  0.00,  0.00},
                { 1.,-2., 0., 0., 0., -1.23, -0.07,  0.06,  0.01},  // P1
                { 1.,-1., 0., 0.,-1.,  0.02,  0.00,  0.00,  0.00},
                { 1.,-1., 0., 0., 1.,  0.04,  0.00,  0.00,  0.00},
                { 1., 0., 0.,-1., 0., -0.22,  0.01,  0.01,  0.00},
                { 1., 0., 0., 0., 0., 12.00, -0.80, -0.67, -0.03},  // K1
                { 1., 0., 0., 1., 0.,  1.73, -0.12, -0.10,  0.00},
                { 1., 0., 0., 2., 0., -0.04,  0.00,  0.00,  0.00},
                { 1., 1., 0., 0.,-1., -0.50, -0.01,  0.03,  0.00},
                { 1., 1., 0., 0., 1.,  0.01,  0.00,  0.00,  0.00},
                { 0., 1., 0., 1.,-1., -0.01,  0.00,  0.00,  0.00},
                { 1., 2.,-2., 0., 0., -0.01,  0.00,  0.00,  0.00},
                { 1., 2., 0., 0., 0., -0.11,  0.01,  0.01,  0.00},
                { 2.,-2., 1., 0., 0., -0.01,  0.00,  0.00,  0.00},
                { 2., 0.,-1., 0., 0., -0.02,  0.00,  0.00,  0.00},
                { 3., 0., 0., 0., 0.,  0.00,  0.00,  0.00,  0.00},
                { 3., 0., 0., 1., 0.,  0.00,  0.00,  0.00,  0.00},
            };
            const DoodsonAngles a = computeAngles(fhr, jc);
            const double c2Phi = cPhi * cPhi - sPhi * sPhi;
            double dr = 0.0, dn = 0.0, de = 0.0;
            for (const auto& r : T) {
                const double theta = (a.tau + r[0]*a.s + r[1]*a.h + r[2]*a.p
                                      + r[3]*a.zns + r[4]*a.ps) * kTideDeg2Rad + lon;
                const double st = std::sin(theta), ct = std::cos(theta);
                dr += (r[5]*st + r[6]*ct) * 2.0 * sPhi * cPhi;
                dn += (r[7]*st + r[8]*ct) * c2Phi;
                de += (r[7]*ct + r[8]*st) * sPhi;
            }
            dr *= 1e-3; dn *= 1e-3; de *= 1e-3;
            return Eigen::Vector3d(dr * cLa * cPhi - de * sLa - dn * sPhi * cLa,
                                   dr * sLa * cPhi + de * cLa - dn * sPhi * sLa,
                                   dr * sPhi + dn * cPhi);
        }

        // STEP2LON: Step-2 long-period frequency correction (IERS 2010 Table 7.3b, 5 lines).
        // Zonal (axially symmetric: no longitude / tau dependence, no east term).
        Eigen::Vector3d st2Lon(double fhr, double jc,
                               double cPhi, double sPhi, double cLa, double sLa) {
            static constexpr double T[5][9] = {
                {0., 0., 0., 1., 0.,  0.47,  0.16,  0.23,  0.07},
                {0., 2., 0., 0., 0., -0.20, -0.11, -0.12, -0.05},
                {1., 0.,-1., 0., 0., -0.11, -0.09, -0.08, -0.04},
                {2., 0., 0., 0., 0., -0.13, -0.15, -0.11, -0.07},
                {2., 0., 0., 1., 0., -0.05, -0.06, -0.05, -0.03},
            };
            const DoodsonAngles a = computeAngles(fhr, jc);
            const double sin2lat = 2.0 * sPhi * cPhi;
            const double radLat  = (3.0 * sPhi * sPhi - 1.0) / 2.0;
            double dr = 0.0, dn = 0.0;
            for (const auto& r : T) {
                const double theta = (r[0]*a.s + r[1]*a.h + r[2]*a.p
                                      + r[3]*a.zns + r[4]*a.ps) * kTideDeg2Rad;
                const double st = std::sin(theta), ct = std::cos(theta);
                dr += (r[5]*ct + r[6]*st) * radLat;
                dn += (r[7]*ct + r[8]*st) * sin2lat;
            }
            dr *= 1e-3; dn *= 1e-3;
            return Eigen::Vector3d(dr * cLa * cPhi - dn * sPhi * cLa,
                                   dr * sLa * cPhi - dn * sPhi * sLa,
                                   dr * sPhi + dn * cPhi);
        }

    } // namespace

    Eigen::Vector3d GnssCorrections::computeSolidEarthTideDisplacement(
        const Eigen::Vector3d& xSta, const Eigen::Vector3d& xSun,
        const Eigen::Vector3d& xMon, double jc, double fhr) {
        const double rSta = xSta.norm(), rSun = xSun.norm(), rMon = xMon.norm();
        if (!(rSta > 0.0 && rSun > 0.0 && rMon > 0.0)) return Eigen::Vector3d::Zero();

        // Geocentric station direction cosines (lat: cPhi/sPhi, lon: cLa/sLa).
        const double cPhi = std::sqrt(xSta.x() * xSta.x() + xSta.y() * xSta.y()) / rSta;
        const double sPhi = xSta.z() / rSta;
        double cLa = 1.0, sLa = 0.0;
        if (cPhi != 0.0) { cLa = xSta.x() / (cPhi * rSta); sLa = xSta.y() / (cPhi * rSta); }
        const double lon = std::atan2(xSta.y(), xSta.x());

        // Geocentric-latitude-dependent degree-2 Love/Shida numbers (IERS Eq. 7.2).
        const double h2 = kTideH20 - 0.0006 * (1.0 - 1.5 * cPhi * cPhi);
        const double l2 = kTideL20 + 0.0002 * (1.0 - 1.5 * cPhi * cPhi);

        const double cpS = xSta.dot(xSun) / (rSta * rSun);  // cos(zenith) to Sun
        const double cpM = xSta.dot(xMon) / (rSta * rMon);  // cos(zenith) to Moon

        // Step-1 in-phase: radial (p2/p3) along the station, tangential (x2/x3) along the body.
        const double p2S = 3.0 * (h2 / 2.0 - l2) * cpS * cpS - h2 / 2.0;
        const double p2M = 3.0 * (h2 / 2.0 - l2) * cpM * cpM - h2 / 2.0;
        const double p3S = 2.5 * (kTideH3 - 3.0 * kTideL3) * cpS * cpS * cpS + 1.5 * (kTideL3 - kTideH3) * cpS;
        const double p3M = 2.5 * (kTideH3 - 3.0 * kTideL3) * cpM * cpM * cpM + 1.5 * (kTideL3 - kTideH3) * cpM;
        const double x2S = 3.0 * l2 * cpS;
        const double x2M = 3.0 * l2 * cpM;
        const double x3S = 1.5 * kTideL3 * (5.0 * cpS * cpS - 1.0);
        const double x3M = 1.5 * kTideL3 * (5.0 * cpM * cpM - 1.0);

        double rr = kTideRe / rSun; double rr3 = rr * rr * rr;
        const double fac2Sun = kTideMassRatioSun * kTideRe * rr3;
        const double fac3Sun = fac2Sun * rr;
        rr = kTideRe / rMon; rr3 = rr * rr * rr;
        const double fac2Mon = kTideMassRatioMoon * kTideRe * rr3;
        const double fac3Mon = fac2Mon * rr;

        Eigen::Vector3d disp;
        for (int i = 0; i < 3; ++i) {
            disp[i] = fac2Sun * (x2S * xSun[i] / rSun + p2S * xSta[i] / rSta)
                    + fac2Mon * (x2M * xMon[i] / rMon + p2M * xSta[i] / rSta)
                    + fac3Sun * (x3S * xSun[i] / rSun + p3S * xSta[i] / rSta)
                    + fac3Mon * (x3M * xMon[i] / rMon + p3M * xSta[i] / rSta);
        }

        // Step-1 out-of-phase (anelasticity) + L1 latitude dependence.
        disp += st1Diurnal(xSun, xMon, fac2Sun, fac2Mon, cPhi, sPhi, cLa, sLa);
        disp += st1SemiD (xSun, xMon, fac2Sun, fac2Mon, cPhi, sPhi, cLa, sLa);
        disp += st1L1    (xSun, xMon, fac2Sun, fac2Mon, cPhi, sPhi, cLa, sLa);

        // Step-2 frequency dependence: diurnal (Table 7.3a) + long-period (Table 7.3b).
        disp += st2Diurnal(fhr, jc, lon, cPhi, sPhi, cLa, sLa);
        disp += st2Lon   (fhr, jc, cPhi, sPhi, cLa, sLa);
        return disp;
    }

    // =================================================================
    // Bias retrieval
    // =================================================================

    double GnssCorrections::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        auto log = log_manager_.getLogger("ppp_model");

        if (satellite.empty() || freq_type.empty()) {
            log->warn("Invalid bias inputs: sat='{}', freq='{}' at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log->warn("Invalid time for bias for {}: week {}, TOW {:.3f}", satellite, time.week, time.tow);
            return 0.0;
        }

        const double t_query = TimeUtils::continuousGpsSeconds(time);

        auto it = std::find_if(biases_->begin(), biases_->end(),
            [&satellite, &freq_type, t_query](const BiasData& b) {
                if (b.satellite != satellite || b.signal != freq_type) return false;
                double t_start = TimeUtils::continuousGpsSeconds(b.start_time);
                double t_end = TimeUtils::continuousGpsSeconds(b.end_time);
                return (t_start <= t_query && t_query <= t_end);
            });

        if (it != biases_->end()) {
            log->trace("Bias for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, PppUtils::getSystemString(satellite), freq_type, time.tow, it->bias);
            return it->bias;
        }

        if (!biases_->empty())
            log->debug("No bias entry for {} {} at t={:.3f}, {} entries total", satellite, freq_type, t_query, biases_->size());

        log->trace("No bias for {} ({}) {} at TOW {:.3f}",
            satellite, PppUtils::getSystemString(satellite), freq_type, time.tow);
        return 0.0;
    }

    void GnssCorrections::buildBiasIndex() {
        bias_signals_.clear();
        size_t skipped_zero_code = 0;
        size_t skipped_glonass_code = 0;
        // Skip CODE bias entries that should not be applied:
        //
        // 1. Zero-valued entries.  Some MGEX products ship placeholder
        //    C2I/C6I entries for BeiDou with bias=0.0 (every satellite,
        //    every code signal); treating those as "covered" causes the
        //    model to apply zero correction AND suppress the per-(sat,
        //    signal) sat-bias state path, leaving the real ~tens-of-
        //    metres hardware delay unmodelled and the row pinned as a
        //    permanent outlier.
        //
        // 2. GLONASS code entries, regardless of value.  GLONASS is
        //    FDMA — each satellite has a different carrier — so the
        //    *receiver*-side hardware delay differs per channel and
        //    can't be cancelled by a single-value-per-(sat, signal)
        //    bias correction.  Even when a product publishes a GLONASS
        //    DCB it's a network-aggregate that won't reflect this
        //    receiver's per-channel delays.  Treating the entries as
        //    missing forces the per-(sat, signal) sat-bias state path
        //    to absorb whatever the actual delay is.
        //
        // Both paths require @c estimate_sat_biases=true to actually
        // create the absorbing state.  Phase biases are left as-is
        // (zero is a meaningful value in some product schemes; GLONASS
        // phase biases, when published, are usable because the float
        // ambiguity absorbs whatever the receiver-side ICB is anyway).
        for (const auto& b : *biases_) {
            const bool is_code = !b.signal.empty() && b.signal[0] == 'C';
            if (is_code && b.bias == 0.0) {
                ++skipped_zero_code;
                continue;
            }
            // GLONASS PRNs in IGS conventions are "R01"…"R26".
            const bool is_glonass_sat = !b.satellite.empty() && b.satellite[0] == 'R';
            if (is_code && is_glonass_sat) {
                ++skipped_glonass_code;
                continue;
            }
            bias_signals_.insert(b.satellite + "|" + b.signal);
        }
        auto log = log_manager_.getLogger("corrections");
        log->debug(
            "Built bias index: {} unique satellite|signal pairs from {} entries "
            "({} zero code entries skipped, {} GLONASS code entries skipped)",
            bias_signals_.size(), biases_->size(),
            skipped_zero_code, skipped_glonass_code);
        if (skipped_zero_code > 0) {
            log->info(
                "Skipped {} zero-valued code bias entries; treated as missing "
                "so per-sat bias states will be created (requires "
                "estimate_sat_biases=true to take effect).",
                skipped_zero_code);
        }
        if (skipped_glonass_code > 0) {
            log->info(
                "Skipped {} GLONASS code bias entries; FDMA per-channel delay "
                "is absorbed by per-(sat, signal) sat-bias states regardless "
                "of product values (requires estimate_sat_biases=true).",
                skipped_glonass_code);
        }
    }

    bool GnssCorrections::hasBiasEntry(const std::string& satellite,
                                        const std::string& signal) const {
        return bias_signals_.count(satellite + "|" + signal) > 0;
    }

    // =================================================================
    // Phase wind-up
    // =================================================================

    double GnssCorrections::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time,
        const Eigen::Vector3d& sun_pos,
        const Eigen::Vector3d& sat_vel,
        bool accumulate) const {

        // Wu et al. (1993), stated in THIS function's variables.  k = sat->rx.
        //
        // The quadrature term's sign is -sgn((x cross y) . k): MINUS when the
        // antenna's own third axis points along +k, PLUS when it points along -k.
        // All three satellite branches below build x_s cross y_s == ez (the
        // geocentric nadir, ~ +k), and the receiver pair is x_r = NORTH,
        // y_r = EAST with x_r cross y_r == DOWN and (DOWN).k == +cos(zenith) > 0
        // over the whole observable hemisphere.  Hence MINUS on BOTH dipoles:
        //
        //     D = x - k(k.x) - k x y      =>  |D| = 1 + cos(eps)
        //
        // (The wrong sign gives |D| = 1 - cos(eps), a dipole that VANISHES on
        //  boresight, and mirrors the phase about the LOS-vertical plane.)
        //
        // WARNING: RTKLIB's familiar (-, +) pairing is stated with eyr = -east
        // (WEST).  Transcribing its '+' onto this file's y_r = +east flips only
        // one of the two lines, and a HALF FIX matches neither the model nor its
        // negative -- a uniformly distributed band-common error spanning a full
        // cycle.  See docs/SPEC_phase_windup_sign.md section 3.
        Eigen::Vector3d k_raw = rx_pos - sat_pos;
        double k_raw_norm = k_raw.norm();
        if (k_raw_norm < 1e-3) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate LOS", satellite);
            return 0.0;
        }
        Eigen::Vector3d k = k_raw / k_raw_norm;

        // --- Satellite body frame ---
        Eigen::Vector3d ez = -sat_pos.normalized();
        Eigen::Vector3d x_s, y_s;

        // BeiDou-2 GEO (C01-C05, C59, C60): orbit-normal mode
        bool use_orbit_normal = false;
        if (system == System::BeiDou && satellite.size() >= 2) {
            int prn = 0;
            try { prn = std::stoi(satellite.substr(1)); } catch (...) {}
            if ((prn >= 1 && prn <= 5) || prn == 59 || prn == 60)
                use_orbit_normal = true;
        }

        if (use_orbit_normal) {
            Eigen::Vector3d orbit_normal = sat_pos.cross(sat_vel);
            if (orbit_normal.norm() < 1e-8) orbit_normal = Eigen::Vector3d::UnitY();
            y_s = orbit_normal.normalized();
            x_s = y_s.cross(ez);
            if (x_s.norm() < 1e-8) x_s = Eigen::Vector3d::UnitX();
            x_s.normalize();
            y_s = ez.cross(x_s);
            y_s.normalize();
        }
        else if (sun_pos.norm() > 1e6) {
            // Standard yaw-steering
            Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();
            Eigen::Vector3d ey = ez.cross(e_sun);
            if (ey.norm() < 1e-8) {
                Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized() : Eigen::Vector3d::UnitY();
                ey = ez.cross(vhat);
            }
            ey.normalize();
            Eigen::Vector3d ex = ey.cross(ez);
            if (ex.norm() < 1e-8) ex = Eigen::Vector3d::UnitX();
            ex.normalize();
            x_s = ex;
            y_s = ey;
        } else {
            // Velocity-based fallback.  Built about the NADIR axis (ez), exactly
            // as the orbit-normal and yaw-steering branches above, so that
            // x_s cross y_s == ez in ALL THREE branches and the single
            // -k.cross(y_s) quadrature sign below is valid unconditionally.
            //
            // This branch previously framed about z_s = -ez (radially OUTWARD),
            // giving x_s cross y_s == -ez -- the OPPOSITE handedness relative to
            // k from its two sibling branches.  Reframing negates y_s and leaves
            // x_s bit-identical, so the effective dipole built below with the
            // corrected MINUS is numerically identical to what the old frame
            // produced with the old PLUS.  That identity is the review gate: if
            // this branch's Ds moves, the reframe is wrong.
            Eigen::Vector3d vel = sat_vel;
            if (vel.norm() < 1e-8 && !orbits_->empty()) {
                auto orbit_it = std::find_if(orbits_->begin(), orbits_->end(),
                    [&satellite](const OrbitData& o) { return o.satellite == satellite; });
                if (orbit_it != orbits_->end() && orbit_it->velocity_valid)
                    vel = orbit_it->velocity;
            }
            y_s = vel.norm() > 1e-8 ? vel.normalized().cross(ez) : Eigen::Vector3d::Zero();
            if (y_s.norm() < 1e-8) y_s = Eigen::Vector3d(0, 1, 0);
            y_s.normalize();
            x_s = y_s.cross(ez);
            if (x_s.norm() < 1e-8) x_s = Eigen::Vector3d::UnitX();
            x_s.normalize();
            // Re-derive y_s from the settled x_s so that x_s cross y_s == ez
            // EXACTLY, mirroring the orbit-normal branch.  A no-op whenever y_s
            // is already perpendicular to ez (every non-degenerate path); it
            // matters only in the doubly-degenerate sub-case where y_s fell
            // through to the hard-coded (0, 1, 0).
            y_s = ez.cross(x_s);
            y_s.normalize();
        }

        // --- Receiver local frame (North / East) ---
        if (rx_pos.norm() < 1e3) return 0.0;
        Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);
        double phi = rx_llh.lat_rad, lam = rx_llh.lon_rad;
        Eigen::Vector3d x_r(-std::sin(phi)*std::cos(lam), -std::sin(phi)*std::sin(lam), std::cos(phi));
        Eigen::Vector3d y_r(-std::sin(lam), std::cos(lam), 0.0);

        // Effective dipoles.  MINUS on both -- see the sign rule at the top of
        // this function.  BOTH LINES MUST CARRY THE SAME SIGN: flipping one is
        // strictly worse than flipping neither.
        Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) - k.cross(y_s);   // satellite
        Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) - k.cross(y_r);   // receiver, y_r = +EAST
        double Ds_norm = Ds.norm(), Dr_norm = Dr.norm();
        if (Ds_norm < 1e-10 || Dr_norm < 1e-10) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: degenerate dipole", satellite);
            return 0.0;
        }

        // Compute signed angle between effective dipoles using atan2 for
        // full (-π, π] range.  This avoids the acos + sign approach which
        // loses information near 0 and π.
        Eigen::Vector3d Ds_hat = Ds / Ds_norm, Dr_hat = Dr / Dr_norm;
        double windUp_c = std::atan2(k.dot(Ds_hat.cross(Dr_hat)), Ds_hat.dot(Dr_hat));
        const double wrapped_c = windUp_c;  // raw atan2 angle, pre-accumulation

        // Accumulated mode: unwrap relative to previous epoch for continuity.
        // Required for IAR where ambiguities must remain constant integers.
        // Fractional mode (default): raw atan2 angle; float ambiguity absorbs
        // integer-cycle component. See header for full design discussion.
        const double epoch_s = static_cast<double>(time.week) * 604800.0 + time.tow;
        double gap_s     = std::numeric_limits<double>::quiet_NaN();
        bool   restarted = false;
        if (accumulate) {
            constexpr double TWO_PI = 2.0 * Constants::PI;
            constexpr double kMaxGapSeconds = 300.0; // 5 minutes
            auto pw_it = prev_windup_.find(satellite);
            if (pw_it == prev_windup_.end()) {
                // First epoch this satellite is seen: seed the accumulator
                // with the raw atan2 angle (nothing to unwrap against yet).
                prev_windup_[satellite] = { windUp_c, epoch_s };
            } else if (epoch_s == pw_it->second.epoch_s) {
                // Same epoch.  computePhaseWindUp() is called once per signal
                // and once per measurement-rebuild / relinearization pass; the
                // wind-up angle is a per-epoch geometric quantity, so reuse the
                // accumulated value produced by this epoch's first call.  Re-
                // storing here would overwrite prev_windup_ with the still-
                // wrapped atan2 angle, resetting the accumulator reference and
                // silently collapsing accumulated mode back to fractional.
                gap_s = 0.0;
                windUp_c = pw_it->second.angle;
            } else {
                const double gap = epoch_s - pw_it->second.epoch_s;
                gap_s = gap;
                if (gap > 0.0 && gap < kMaxGapSeconds) {
                    double diff = windUp_c - pw_it->second.angle;
                    diff -= TWO_PI * std::round(diff / TWO_PI);
                    windUp_c = pw_it->second.angle + diff;
                } else {
                    // Gap exceeds the continuity threshold (or epochs ran
                    // backwards): the unwrap reference is no longer reliable,
                    // so restart accumulation from the raw atan2 angle.
                    restarted = true;
                    log_manager_.getLogger("ppp_model")->debug(
                        "Phase wind-up gap {:.0f}s for {} exceeds {:.0f}s "
                        "continuity threshold: restarting accumulation",
                        gap, satellite, kMaxGapSeconds);
                }
                prev_windup_[satellite] = { windUp_c, epoch_s };
            }
        }

        double phi_m = windUp_c * wavelength / (2.0 * Constants::PI);
        log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: {:.6f} m ({:.3f} rad, {})",
            satellite, phi_m, windUp_c, accumulate ? "accumulated" : "fractional");

        // Diagnostic CSV: one row per (satellite, epoch).  computePhaseWindUp
        // is called once per signal and once per measurement-rebuild pass, so
        // dedupe on epoch_s to keep the trace at per-epoch granularity.
        if (windup_trace_.is_open()) {
            double& last_traced = windup_trace_epoch_[satellite];
            if (last_traced != epoch_s) {
                last_traced = epoch_s;
                const Eigen::Vector3d los = -k;  // receiver -> satellite
                const Eigen::Vector3d up(std::cos(phi) * std::cos(lam),
                                         std::cos(phi) * std::sin(lam),
                                         std::sin(phi));
                const double el_deg = std::asin(std::clamp(los.dot(up), -1.0, 1.0))
                                      * 180.0 / Constants::PI;
                double az_deg = std::atan2(los.dot(y_r), los.dot(x_r))
                                * 180.0 / Constants::PI;
                if (az_deg < 0.0) az_deg += 360.0;
                windup_trace_ << std::fixed << std::setprecision(3)
                              << time.week << ',' << time.tow << ','
                              << satellite << ','
                              << std::setprecision(6)
                              << el_deg << ',' << az_deg << ','
                              << (accumulate ? "accumulated" : "fractional") << ','
                              << wavelength << ','
                              << wrapped_c << ',' << windUp_c << ','
                              << windUp_c / (2.0 * Constants::PI) << ','
                              << phi_m << ',';
                if (std::isfinite(gap_s)) windup_trace_ << gap_s;
                windup_trace_ << ',' << (restarted ? 1 : 0) << '\n';
            }
        }
        return phi_m;
    }

    bool GnssCorrections::enableWindUpTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("corrections");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec && logger) {
                logger->warn("Failed to create log directory '{}': {}",
                              log_dir.string(), ec.message());
            }
        }

        const auto trace_path = log_dir / "windup_trace.csv";
        windup_trace_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!windup_trace_.is_open()) {
            if (logger) logger->warn("Could not open wind-up trace file '{}'", trace_path.string());
            return false;
        }

        windup_trace_ << "week,tow_s,satellite,elevation_deg,azimuth_deg,mode,"
                      << "wavelength_m,wrapped_rad,accumulated_rad,cycles,windup_m,gap_s,restart\n";
        windup_trace_epoch_.clear();
        if (logger) logger->info("Phase wind-up CSV trace enabled: {}", trace_path.string());
        return true;
    }

} // namespace GnssProcessor