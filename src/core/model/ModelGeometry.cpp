#include "model/ModelGeometry.hpp"
/*!
 * \file ModelGeometry.cpp
 * \brief Satellite/receiver geometry and correction computations for PPP.
 *
 * The implementation is organized as:
 *  - Construction / product loading
 *  - Private helpers:  extractPseudorange, computeEmissionState, ensureSunMoonCached
 *  - Core geometry:    computeSatelliteGeometry
 *  - Orbit interpolation: getSatellitePV
 *  - Topocentric / attitude / wind-up utilities
 */

#include "model/ModelGeometry.hpp"
#include "model/Antenna.hpp"
#include "products/ProductParser.hpp"
#include "products/StationInfoFile.hpp"
#include "corrections/OceanTideModel.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "math/TimeUtils.hpp"
#include "math/Interpolate.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <unordered_map>

namespace GnssProcessor {


    namespace Model {

        // All interpolation goes through the Interpolate:: class methods.

        // =================================================================
        // Construction / product loading
        // =================================================================

        Geometry::Geometry(const GnssConfig& config, ProductCache& cache,
            const RinexParser& rinex, LogManager& log_mgr)
            : corrections_(log_mgr, config, cache.getClocksShared(), cache.getErpsShared(), cache.getBiasesShared(), rinex),
            config_(config),
            planetary_(log_mgr, config.enableCioFrame(), config.enableSunFrameCorrection()),
            troposphere_(config, log_mgr),
            product_cache_(cache),
            log_manager_(log_mgr),
            rinex_parser_(rinex),
            elevation_threshold_(config.getElevationMaskDeg()),
            signal_priorities_(config.getSignalPriorities()),
            // Share the single cached product copy (no per-instance duplication).
            orbits_(cache.getOrbitsShared()),
            clocks_(cache.getClocksShared()),
            antennas_(cache.getAntennasShared()),
            biases_(cache.getBiasesShared()),
            erps_(cache.getErpsShared()),
            klobuchar_coeffs_(cache.getKlobucharCoeffs())
        {
            // orbits_/clocks_/antennas_/biases_/erps_ reference the shared
            // ProductCache copy through shared_ptr<const>; nothing is duplicated.

            log_manager_.getLogger("geometry")->info(
                "Geometry model initialized: {} orbits, {} clocks, elev_mask={:.1f} deg", orbits_->size(), clocks_->size(), elevation_threshold_
            );
            // Build indices from the cached product vectors so lookups are O(1)
            loadData();
        }

        const SsrCorrectionStore& Geometry::ssrStore() const {
            return ssr_store_override_ ? *ssr_store_override_
                                       : SsrCorrectionStore::getInstance();
        }

        void Geometry::loadData() {
            log_manager_.getLogger("geometry")->debug("Begin satellite orbit index");

            orbit_index_.clear();

            // Build orbit index: satellite -> vector of pointers to OrbitData.
            // The index stores raw pointers into the shared orbits_ vector, which
            // stays alive because this Geometry holds a shared_ptr<const> to it.
            for (const OrbitData& od : *orbits_) {
                if (od.satellite.empty()) continue;
                orbit_index_[od.satellite].push_back(&od);
            }

            // Sort per-satellite entries by time (ascending) to make interpolation/search predictable
            for (auto& kv : orbit_index_) {
                auto& vec = kv.second;
                std::sort(vec.begin(), vec.end(), [](const OrbitData* a, const OrbitData* b) {
                    return TimeUtils::continuousGpsSeconds(a->time) < TimeUtils::continuousGpsSeconds(b->time);
                    });
            }

            log_manager_.getLogger("geometry")->info("Built orbit index for {} satellites", orbit_index_.size());

            // Ocean tidal loading source precedence: SIF → BLQ → EOT20 grid → zero.
            //   1. If station info is enabled (or model_corrections.otl_from_sif is
            //      on, which loads the SIF registry for this alone) and the SIF entry
            //      for the marker carries OTL, use its 3-component FES2014b
            //      coefficients.
            //   2. Else, if a BLQ file is configured and the station name is
            //      found in it, use the station-specific Onsala coefficients
            //      (includes horizontal components).
            //   3. Else, if EOT20_load.nc is available, synthesize per-station
            //      radial coefficients from the global load grid by bilinear
            //      interpolation at the receiver's a priori ECEF position.
            //   4. Else OceanTideModel applies zero displacement.  A
            //      latitude-scaled approximation is not used: it is phase-blind
            //      and can inject a biased oscillation into the up component.
            // With OTL disabled the coefficient load is skipped, so no
            // "loaded coefficients" line is logged for a correction that is not applied.
            auto log = log_manager_.getLogger("geometry");

            // Atmospheric pressure loading (S1/S2 thermal tide) is loaded
            // independently of OTL into the same Geometry-owned corrections_
            // instance that applies it per epoch.  The grid is a date-independent
            // static asset (like the opole grid): an explicit
            // files.atm_tide_grid_path wins, else <internal_data_root>/static/apl/.
            // The block is scoped so its station ECEF does not collide with the
            // OTL station_ecef below.  Off by default; the output stays
            // byte-identical until the flag is on and a grid is supplied.
            if (config_.getModelCorrections().enable_atmospheric_loading) {
                const Eigen::Vector3d apl_ecef = rinex_parser_.getApproxPositionXyz();
                std::filesystem::path apl_grid = config_.getAtmTideGridPath();
                if (apl_grid.empty()) {
                    apl_grid = config_.getInternalDataRoot() / "static" / "apl"
                             / "s1_s2_load_cm.dat";   // provisional filename; pin to the supplied product
                }
                if (apl_ecef.allFinite() && apl_ecef.norm() > 1e6
                    && std::filesystem::exists(apl_grid)) {
                    if (corrections_.loadAtmosphericTideFromGrid(apl_grid.string(), apl_ecef))
                        log->info("APL: loaded S1/S2 coefficients from grid '{}'", apl_grid.string());
                    else
                        log->warn("APL: failed to load S1/S2 grid '{}'; APL displacement will be zero",
                                  apl_grid.string());
                } else {
                    log->info("APL: enabled but no usable grid at '{}'; APL displacement will be zero "
                              "(place the S1/S2 load grid under <data>/static/apl/)",
                              apl_grid.string());
                }
            }

            if (!config_.getModelCorrections().enable_ocean_tide_loading) {
                log->info("OTL: disabled via enable_ocean_tide_loading=false; skipping coefficient load");
                return;
            }
            const auto otl_blq_path = config_.getOtlBlqPath();
            const auto eot20_path   = config_.getDownloadConfig().nc_path;
            const std::string station_name = rinex_parser_.getHeader().marker_name;
            const Eigen::Vector3d station_ecef = rinex_parser_.getApproxPositionXyz();

            // Nodal factors and the UTC Greenwich argument (default off: the original
            // evaluation, bit for bit).
            // The RINEX marker field is fixed width; the run-start lines of otl_from_sif
            // print it without the padding.
            const std::size_t mk_b = station_name.find_first_not_of(' ');
            const std::string marker_trim = (mk_b == std::string::npos)
                ? std::string() : station_name.substr(mk_b, station_name.find_last_not_of(' ') - mk_b + 1);
            const bool nodal = config_.getModelCorrections().otl_nodal_corrections;
            corrections_.setOceanTideNodalCorrections(nodal);
            if (nodal)
                log->info("OTL: nodal corrections and UTC Greenwich argument on");

            bool loaded = false;
            const char* source = "SIF";   // names the source of the run-start line under otl_from_sif

            // Highest fidelity: per-station FES2014b coefficients from a SIF.
            // Unlike the EOT20 grid (radial only), the SIF carries all three
            // components.  Convert the SIF Up/East/North amplitude+phase set to
            // the model's Up/West/South lag convention: Up passes through, and
            // since East = -West and North = -South the horizontals keep their
            // amplitude and shift phase by 180 deg.  The RINEX APPROX position
            // confirms a 4-char prefix match (find() ignores it when unusable), so
            // a marker missing from the SIF never borrows a distant station's
            // coefficients.
            //
            // Horizontal mapping: otl_from_sif reads the SIF east and north columns as
            // the BLQ west and south rows (the NGS M-PAGES reading), which is what the
            // file holds.  The station-info subsystem alone keeps its original reading
            // (true east and north, phases + 180 degrees), which reverses the
            // horizontal vector, so that runs that armed it stay unchanged.
            const bool otl_from_sif = config_.getModelCorrections().otl_from_sif;
            if (config_.enableStationInfo() || otl_from_sif) {
                if (const auto sif = config_.getStationInfo()) {
                    const StationInfo* si = sif->find(station_name, &station_ecef);
                    if (si && si->has_otl) {
                        const OtlStationCoefficients coeffs = otlCoefficientsFromSif(
                            *si, otl_from_sif ? SifOtlHorizontal::BlqRows
                                              : SifOtlHorizontal::LegacyShifted);
                        corrections_.setOceanTideCoefficients(coeffs);
                        log->info("OTL: loaded 3-component FES2014b coefficients from SIF for "
                                  "marker '{}' (matched {}); M2 up-amp={:.4f} m",
                                  station_name, si->id, coeffs.constituents[0].amp_up);
                        log_manager_.getLogger("main_log")->info(
                            "Ocean tide loading source: SIF 3-component FES2014b{} (marker '{}' -> {})",
                            otl_from_sif ? ", BLQ rows" : "",
                            otl_from_sif ? marker_trim : station_name, si->id);
                        loaded = true;
                    } else if (otl_from_sif) {
                        log->warn("OTL: SIF has no ocean tide loading block for marker '{}'{}; "
                                  "trying BLQ, then EOT20", station_name,
                                  si ? " (matched a record without one)" : " (no record matched)");
                    }
                } else if (otl_from_sif) {
                    log->warn("OTL: otl_from_sif is on but no SIF registry is loaded "
                              "(files.station_info_path(s) or <data>/static/sif/); trying BLQ, then EOT20");
                }
            }

            if (!loaded && !otl_blq_path.empty() && !station_name.empty()) {
                if (corrections_.loadOceanTideFromBLQ(otl_blq_path.string(), station_name)) {
                    log->info("OTL: loaded BLQ coefficients for station '{}'", station_name);
                    source = "BLQ";
                    loaded = true;
                } else {
                    log->warn("OTL: BLQ '{}' had no entry for station '{}'; trying EOT20 fallback",
                              otl_blq_path.string(), station_name);
                }
            } else if (!otl_blq_path.empty() && station_name.empty()) {
                log->warn("OTL: BLQ path configured but RINEX marker name is empty; trying EOT20 fallback");
            }

            if (!loaded && !eot20_path.empty() && std::filesystem::exists(eot20_path)
                && station_ecef.allFinite() && station_ecef.norm() > 1e6) {
                if (corrections_.loadOceanTideFromEot20(eot20_path.string(), station_ecef)) {
                    log->info("OTL: synthesized coefficients from EOT20 grid '{}'", eot20_path.string());
                    source = "EOT20 grid (radial only)";
                    loaded = true;
                } else {
                    log->warn("OTL: EOT20 synthesis failed for '{}'; OTL displacement will be zero",
                              eot20_path.string());
                }
            }

            if (!loaded) {
                log->info("OTL: no BLQ or EOT20 source available; ocean tidal loading disabled (zero displacement)");
            }

            // Under otl_from_sif the run start lists every station's source, so a station
            // the SIF does not serve is visible in the main log as well.
            if (otl_from_sif && !(loaded && std::string(source) == "SIF")) {
                log_manager_.getLogger("main_log")->info(
                    "Ocean tide loading source: {} (marker '{}')",
                    loaded ? source : "none, zero displacement", marker_trim);
            }
        }

        // =================================================================
        // Simple accessors
        // =================================================================

        double Geometry::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) {
            return (sat_pos - rx_pos).norm(); // meters
        }


        // =================================================================
        // Private helpers
        // =================================================================

        void Geometry::ensureSunMoonCached(const GnssTime& time) const {
            const double cached_t = TimeUtils::continuousGpsSeconds(cached_sun_moon_epoch_);
            const double query_t  = TimeUtils::continuousGpsSeconds(time);
            if (std::abs(query_t - cached_t) > 1e-3) {
                planetary_.computeSunMoonPosition(time, *erps_,
                    cached_sun_pos_, cached_moon_pos_);
                cached_sun_moon_epoch_ = time;
            }
        }

        bool Geometry::extractPseudorange(const RinexObservation& obs, double& pseudorange,
                                          std::string& sig1_out, std::string& sig2_out) const {
            auto log = log_manager_.getLogger("geometry");

            // Reset out-params so early-return paths don't leave stale strings
            // visible to callers.  Each priority branch sets them on success.
            sig1_out.clear();
            sig2_out.clear();

            // Collect available code observations (keys starting with 'C').
            // Used for the empty-observation early-return guard and as the
            // single-frequency fallback path at the end.
            std::vector<std::pair<std::string, double>> code_obs;
            for (const auto& [key, val] : obs.measurements)
                if (!key.empty() && key[0] == 'C' && std::isfinite(val) && val > 1e6)
                    code_obs.emplace_back(key, val);

            if (code_obs.empty()) {
                log->warn(
                    "No code pseudorange available for {} at TOW {:.3f} — skipping observation",
                    obs.satellite, obs.time.tow);
                return false;
            }

            // RAWOBS diagnostic (trace, "geometry"): every raw decoded code
            // pseudorange per satellite before any IF combination, with its
            // implied rough range in light-milliseconds (pr / (c·1e-3)).  A
            // plausible MEO GPS pseudorange is about 1.9e7–2.7e7 m, i.e. about
            // 63–90 light-ms, and the codes of one satellite agree to within the
            // ionospheric delay (tens of metres).  Comparing these lines with
            // the geometric "range=" line below (computed at the a priori
            // position) identifies a corrupt MSM-decoded pseudorange, which
            // shows up as a per-satellite closure error of hundreds of km.
            if (log->should_log(spdlog::level::trace)) {
                for (const auto& [k, v] : code_obs)
                    log->trace("RAWOBS {} {} tow={:.3f}: pr={:.3f} m ({:.2f} light-ms)",
                               obs.satellite, k, obs.time.tow, v, v / 299792.458);
            }

            // Build an ionosphere-free combination from (sig1, sig2) on
            // different bands.  Optionally applies BIA code-bias corrections
            // before combining so the result aligns with the clock-reference
            // scale.  Returns false unless both signals are present, finite,
            // distinct-band, and the combination is finite.
            //
            //   P_IF = (f1²·P1 − f2²·P2) / (f1² − f2²)
            //
            // The IF combination cancels the first-order ionospheric delay,
            // leaving troposphere, code noise and residual bias as the
            // non-geometric content of the pseudorange.  The emission-time
            // seed uses IF to reduce the (I+ε)/c error in t_em from about
            // 30–90 ns to the ns level (sub-μm satellite displacement).
            PppUtils utils(log_manager_);
            auto buildIf = [&](const std::string& sig1, const std::string& sig2,
                               bool apply_bias, double& if_pr) -> bool {
                auto it1 = obs.measurements.find(sig1);
                auto it2 = obs.measurements.find(sig2);
                if (it1 == obs.measurements.end() || it2 == obs.measurements.end()) return false;
                const double p1_raw = it1->second;
                const double p2_raw = it2->second;
                if (!std::isfinite(p1_raw) || !std::isfinite(p2_raw)) return false;
                if (p1_raw < 1e6 || p2_raw < 1e6) return false;

                const double f1 = utils.getFrequency(sig1, obs.system, obs.satellite);
                const double f2 = utils.getFrequency(sig2, obs.system, obs.satellite);
                if (!(f1 > 0.0) || !(f2 > 0.0) || std::abs(f1 - f2) < 1e6) return false;

                double p1 = p1_raw, p2 = p2_raw;
                if (apply_bias) {
                    if (corrections_.hasBiasEntry(obs.satellite, sig1))
                        p1 -= corrections_.getBiasCorrection(obs.satellite, sig1, obs.time);
                    if (corrections_.hasBiasEntry(obs.satellite, sig2))
                        p2 -= corrections_.getBiasCorrection(obs.satellite, sig2, obs.time);
                }

                // Geometry-free plausibility gate.  On code, the lower-frequency
                // signal carries more ionospheric delay, so P(low-freq) exceeds
                // P(high-freq) up to a small DCB margin: (P_hi − P_lo) is near zero
                // or negative, never strongly positive.  A decoded pair that
                // inverts this (for example a corrupt MSM frame with C2W 124 m
                // below C1W) is physically impossible.  The IF combination
                // amplifies the error about 2.5×, and it then corrupts the
                // emission-time seed, the WLS and the filter (one such satellite
                // can move the standalone WLS by hundreds of km).  The pair is
                // rejected so extractPseudorange falls through to the next pair
                // or to single frequency.
                constexpr double kMaxGeomFreeMeters = 30.0;  // DCB+noise headroom; iono only makes GF more negative
                const double p_hi = (f1 > f2) ? p1 : p2;     // higher-frequency code
                const double p_lo = (f1 > f2) ? p2 : p1;     // lower-frequency code
                if (p_hi - p_lo > kMaxGeomFreeMeters) {
                    log->warn("Rejecting IF pair {}/{} for {}: geometry-free {:+.1f} m is "
                              "ionospherically impossible (high-freq code above low-freq) "
                              "— corrupt MSM decode/signal for this satellite",
                              sig1, sig2, obs.satellite, p_hi - p_lo);
                    return false;
                }

                const double f1sq = f1 * f1, f2sq = f2 * f2;
                const double pr = (f1sq * p1 - f2sq * p2) / (f1sq - f2sq);
                if (!std::isfinite(pr) || pr < 1e6) return false;

                if_pr = pr;
                return true;
            };

            // ===== Priority 1: clock-reference-aligned IF pair =====
            // The precise clock product δt_s is calibrated against a specific
            // signal pair per system (e.g. C1W+C2W for IGS Final GPS, parsed
            // from the BIA DESCRIPTION block into ProductCache::clock_ref_
            // signals_).  When the emission-time seed uses the IF combination
            // of those signals, the (c·δt_s) term cancels exactly against
            // sat_clk in Step 3 of computeEmissionState: no DCB adjustment is
            // needed and no per-signal bias enters.  This path is preferred.
            {
                const auto& clock_refs = product_cache_.getClockRefSignals();
                const char sys_char = obs.satellite.empty() ? '\0' : obs.satellite[0];
                auto it_ref = clock_refs.find(sys_char);
                if (it_ref != clock_refs.end()) {
                    const std::string& s1 = it_ref->second.sig1;
                    const std::string& s2 = it_ref->second.sig2;
                    double pr_if;
                    if (buildIf(s1, s2, /*apply_bias=*/false, pr_if)) {
                        pseudorange = pr_if;
                        sig1_out = s1;
                        sig2_out = s2;
                        log->trace(
                            "Emission seed (IF clock-ref) {} {}/{} = {:.3f} m",
                            obs.satellite, s1, s2, pseudorange);
                        return true;
                    }
                }
            }

            // ===== Priority 2: bias-aligned IF pair (DCB applied) =====
            // Walk the configured signal priority list and pick the first
            // pair on different bands where both signals have BIA coverage.
            // Applying DCBs brings each raw pseudorange onto the clock-
            // reference scale before combining, so any per-signal bias
            // collapses into combination noise instead of an uncancelled
            // offset in t_em.
            if (code_obs.size() >= 2) {
                const std::string sys_str = PppUtils::getSystemString(obs.system);
                auto prio_it = signal_priorities_.find(sys_str);
                if (prio_it != signal_priorities_.end()) {
                    std::string sig1, sig2;
                    char band1 = '\0';
                    for (const auto& sig : prio_it->second) {
                        if (sig.empty() || sig[0] != 'C') continue;
                        if (obs.measurements.count(sig) == 0) continue;
                        if (!corrections_.hasBiasEntry(obs.satellite, sig)) continue;
                        const double v = obs.measurements.at(sig);
                        if (!std::isfinite(v) || v < 1e6) continue;
                        const char band = sig.size() > 1 ? sig[1] : '0';
                        if (sig1.empty()) {
                            sig1 = sig;
                            band1 = band;
                        } else if (band != band1) {
                            sig2 = sig;
                            break;
                        }
                    }
                    if (!sig1.empty() && !sig2.empty()) {
                        double pr_if;
                        if (buildIf(sig1, sig2, /*apply_bias=*/true, pr_if)) {
                            pseudorange = pr_if;
                            sig1_out = sig1;
                            sig2_out = sig2;
                            log->trace(
                                "Emission seed (IF bias-aligned) {} {}/{} = {:.3f} m",
                                obs.satellite, sig1, sig2, pseudorange);
                            return true;
                        }
                    }
                }
            }

            // ===== Priority 3: best-effort IF pair (no BIA filter) =====
            // Same priority walk as Priority 2 without requiring BIA coverage.
            // Used when the satellite has no usable BIA entries (typical for
            // some MGEX satellites with sparse bias coverage).  The uncancelled
            // per-signal bias puts up to a few metres of offset in the IF
            // (about 10 ns of t_em error, about 40 μm of satellite
            // displacement), which is below the KF noise floor.
            if (code_obs.size() >= 2) {
                const std::string sys_str = PppUtils::getSystemString(obs.system);
                auto prio_it = signal_priorities_.find(sys_str);
                if (prio_it != signal_priorities_.end()) {
                    std::string sig1, sig2;
                    char band1 = '\0';
                    for (const auto& sig : prio_it->second) {
                        if (sig.empty() || sig[0] != 'C') continue;
                        if (obs.measurements.count(sig) == 0) continue;
                        const double v = obs.measurements.at(sig);
                        if (!std::isfinite(v) || v < 1e6) continue;
                        const char band = sig.size() > 1 ? sig[1] : '0';
                        if (sig1.empty()) {
                            sig1 = sig;
                            band1 = band;
                        } else if (band != band1) {
                            sig2 = sig;
                            break;
                        }
                    }
                    if (!sig1.empty() && !sig2.empty()) {
                        double pr_if;
                        if (buildIf(sig1, sig2, /*apply_bias=*/false, pr_if)) {
                            pseudorange = pr_if;
                            sig1_out = sig1;
                            sig2_out = sig2;
                            log->trace(
                                "Emission seed (IF best-effort) {} {}/{} = {:.3f} m",
                                obs.satellite, sig1, sig2, pseudorange);
                            return true;
                        }
                    }
                }
            }

            // ===== Fallback: first valid single-frequency pseudorange =====
            // Carries the full ionospheric delay: about 30–90 ns of t_em error,
            // about 100–400 μm of satellite displacement at the seed.  Used
            // when no IF pair can be formed (for example GEO satellites with
            // single-band coverage in the obs set, or constellations where
            // signal_mode excludes both bands).  Steps 3 and 5 of
            // computeEmissionState still cancel the receiver and satellite
            // clocks; only the ionospheric residual remains.
            pseudorange = code_obs.front().second;
            sig1_out = code_obs.front().first;
            sig2_out.clear();
            log->trace(
                "Emission seed (single-freq fallback) {} {} = {:.3f} m",
                obs.satellite, code_obs.front().first, pseudorange);
            return true;
        }

        bool Geometry::computeEmissionState(const RinexObservation& obs,
            const Eigen::Vector3d& rx_pos, double pseudorange,
            Eigen::Vector3d& sat_pos_em, Eigen::Vector3d& sat_vel_em,
            double& sat_clock_out, GnssTime& emission_time,
            bool& broadcast_grade,
            DropReason& drop_reason) const {

            broadcast_grade = false;
            const double c = Constants::SPEED_OF_LIGHT;
            const double tof = pseudorange / c;

            // Step 1: Approximate emission time from pseudorange
            emission_time = obs.time;
            emission_time.tow -= tof;
            if (emission_time.tow < 0) {
                emission_time.tow += TimeUtils::SECONDS_IN_WEEK;
                emission_time.week -= 1;
            }

            // Step 2: Satellite clock at approximate emission time
            double sat_clk = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time);
            if (std::abs(sat_clk) < 1e-12) {
                // Warn once per satellite per session.  A missing clock
                // product is static within a daily run (typical for BeiDou
                // GEOs absent from MGEX clock products), so a per-epoch
                // warning would repeat the same line.
                if (sats_warned_no_clock_.insert(obs.satellite).second) {
                    log_manager_.getLogger("geometry")->warn(
                        "Satellite {} has zero/invalid clock product; observations from this satellite will be skipped this session",
                        obs.satellite);
                }
                drop_reason = DropReason::NoProduct;
                return false;
            }

            // Step 3: Corrected GPS transmission time.
            //
            // The pseudorange (not the geometric range) is used here so that
            // the receiver clock bias δt_r cancels.  obs.time.tow is the
            // receiver's local clock time t_rcv^GPS + δt_r, and the
            // pseudorange contains +c·δt_r, so subtracting P/c removes δt_r
            // without knowing its value.  Subtracting sat_clk then removes the
            // satellite clock bias δt_s.  The residual error is
            // (I+T+ε)/c ≈ 30–60 ns, a sub-mm satellite displacement, below
            // any precision threshold relevant to PPP.
            //
            // Do not replace this with a light-time iteration on the geometric
            // range: that reintroduces an uncancelled δt_r, which forms a
            // positive-feedback loop through the Kalman filter's receiver
            // clock estimate and produces severe outliers and instability.
            emission_time = obs.time;
            emission_time.tow -= (tof + sat_clk);
            if (emission_time.tow < 0) {
                emission_time.tow += TimeUtils::SECONDS_IN_WEEK;
                emission_time.week -= 1;
            }
            if (emission_time.tow >= TimeUtils::SECONDS_IN_WEEK) {
                emission_time.tow -= TimeUtils::SECONDS_IN_WEEK;
                emission_time.week += 1;
            }

            // Step 4: Interpolate satellite orbit at corrected emission time
            auto [pos_em, vel_em] = getSatellitePV(obs.satellite, emission_time);
            if (pos_em.norm() < 1.9e7 || !pos_em.allFinite()) {
                log_manager_.getLogger("geometry")->warn(
                    "Invalid satellite position at emission time for {} at TOW {:.3f}: norm={:.3e} m",
                    obs.satellite, emission_time.tow, pos_em.norm());
                drop_reason = DropReason::NoProduct;
                return false;
            }
            sat_pos_em = pos_em;
            sat_vel_em = vel_em;

            // Step 5: Re-evaluate satellite clock at corrected emission time.
            // Capture clock provenance here (the final, authoritative clock) so
            // the caller can drop carrier phase when only a broadcast clock backs
            // this satellite.
            sat_clk = corrections_.getSatelliteClockCorrection(obs.satellite, emission_time, &broadcast_grade);
            if (std::abs(sat_clk) < 1e-12) {
                log_manager_.getLogger("geometry")->warn(
                    "Satellite clock is zero/invalid for {} at corrected emission TOW {:.3f} - skipping",
                    obs.satellite, emission_time.tow);
                drop_reason = DropReason::NoProduct;
                return false;
            }
            sat_clock_out = sat_clk;
            drop_reason = DropReason::None;
            return true;
        }

        // =================================================================
        // Core geometry computation
        // =================================================================

        bool Geometry::computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos,
            Eigen::Vector3d& rx_pos,
            double& elevation,
            double& azimuth,
            Eigen::Vector3d& los,
            double& range,
            double& tropo_delay,
            double& sat_clock,
            double& shapiro_delay,
            double& sagnac_corr,
            double& otl_corr,
            double& apl_corr,
            double& relativistic_corr,
            double& solid_tide,
            double& mf_w,
            double& mf_d,
            Eigen::Vector3d& sat_vel,
            bool& broadcast_only,
            DropReason& drop_reason) const
        {
            drop_reason = DropReason::None;
            broadcast_only = false;

            // Tracks whether the satellite's clock is broadcast-grade (metre
            // level: no precise CLK, no SSR clock).  Set by computeEmissionState
            // from the final clock evaluation; surfaced via broadcast_only so the
            // measurement model can fall back to code-only for this satellite.
            bool broadcast_grade = false;

            // Reject user-configured excluded satellites (e.g. E14/E18 eccentric orbit).
            // Categorised as NoModel: the configuration excludes the satellite
            // from modelling.
            if (config_.isSatelliteExcluded(obs.satellite)) {
                drop_reason = DropReason::NoModel;
                return false;
            }

            const double c = Constants::SPEED_OF_LIGHT;
            const double omegaE = Constants::EARTH_ANGULAR_VELOCITY;

            // --- Validate receiver position ---
            rx_pos = state.position;
            if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
                log_manager_.getLogger("geometry")->warn("Invalid receiver position for {} at TOW {:.3f}",
                    obs.satellite, obs.time.tow);
                // The receiver position is not initialised.  Charged to
                // NoModel: the cause is not per-satellite, but no observation
                // can be modelled (every satellite is rejected this epoch).
                drop_reason = DropReason::NoModel;
                return false;
            }

            // --- Extract pseudorange + identify IF signals used ---
            // The chosen sig1/sig2 are needed by the refinement step below
            // so that matching ATX corrections can be applied on the same
            // pair and IF-combined to align with the pseudorange.
            double pseudorange = 0.0;
            std::string seed_sig1, seed_sig2;
            if (!extractPseudorange(obs, pseudorange, seed_sig1, seed_sig2)) {
                // No usable code pseudorange in this epoch's signal set.
                drop_reason = DropReason::NoModel;
                return false;
            }

            // --- Emission-time computation (first pass) ---
            Eigen::Vector3d sat_pos_em = Eigen::Vector3d::Zero();
            Eigen::Vector3d sat_vel_em = Eigen::Vector3d::Zero();
            double sat_clock_em = 0.0;
            GnssTime emission_time{};

            // computeEmissionState owns its own NoProduct annotations.
            if (!computeEmissionState(obs, rx_pos, pseudorange, sat_pos_em, sat_vel_em,
                                      sat_clock_em, emission_time, broadcast_grade, drop_reason))
                return false;

            // --- Earth rotation + receiver-frame geometry (lambda reused
            //     for seed and refined passes) ---
            // Rotates sat_pos_em about the z axis by ωE·τ (τ = geometric
            // time of flight) into the reception-time ECEF frame (the
            // Sagnac-equivalent rotation), then computes range, the
            // receiver→satellite LOS unit vector (sat_pos − rx_pos)/range,
            // elevation and azimuth: the receiver-frame quantities the
            // refinement and downstream corrections need.
            auto rotateAndComputeTopo = [&](const Eigen::Vector3d& s_em) {
                const double geom_tof = (s_em - rx_pos).norm() / c;
                const double theta = omegaE * geom_tof;
                Eigen::Matrix3d R_earth;
                R_earth <<  std::cos(theta), std::sin(theta), 0.0,
                           -std::sin(theta), std::cos(theta), 0.0,
                            0.0,             0.0,             1.0;
                sat_pos = R_earth * s_em;
                range   = (sat_pos - rx_pos).norm();
                los     = (sat_pos - rx_pos) / range;
                const Topocentric t = computeTopocentric(rx_pos, sat_pos);
                elevation = t.elevation_deg;
                azimuth   = t.azimuth_deg;
            };

            rotateAndComputeTopo(sat_pos_em);

            // --- Elevation mask check (seed) ---
            // The refinement below shifts elevation by far less than a degree
            // (refined t_em differs from seed t_em by tens of ns, sat_pos by
            // about 100 μm), so the gate is essentially invariant to the
            // refinement.  The check after refinement only catches
            // satellites at the mask boundary.
            if (elevation < elevation_threshold_) {
                log_manager_.getLogger("geometry")->debug(
                    "Satellite {} below elevation threshold (seed): {:.3f} deg < {:.3f} deg",
                    obs.satellite, elevation, elevation_threshold_);
                drop_reason = DropReason::BelowMask;
                return false;
            }

            // --- Eclipse exclusion (seed) ---
            // Drop satellites in Earth's umbral shadow when
            // processing.exclude_eclipsing_satellites is on.  Yaw attitude
            // during eclipse and post-shadow recovery is block-specific and
            // not modelled here; without yaw input the antenna PCO/PCV and
            // phase wind-up are unreliable for these satellites.  The shadow
            // test is sensitive to sat_pos only at the sub-metre level, so
            // the seed-pass position suffices: the refinement shifts sat_pos
            // by about 100 μm, which cannot change the eclipse state.
            if (config_.excludeEclipsingSatellites()) {
                ensureSunMoonCached(obs.time);
                if (Planetary::isSatelliteEclipsing(sat_pos, cached_sun_pos_,
                                                    config_.getEclipseShadowScale())) {
                    log_manager_.getLogger("geometry")->debug(
                        "Satellite {} excluded: in Earth's umbral shadow (shadow_scale={:.3f})",
                        obs.satellite, config_.getEclipseShadowScale());
                    drop_reason = DropReason::Eclipsing;
                    return false;
                }
            }

            // --- Emission-time refinement: slant tropo + IF-combined ATX ---
            //
            // Gated by @c model_corrections.refine_emission_time (default
            // true).  When disabled, the seed-pass emission state is kept and
            // the geometry is single-pass, which serves A/B comparison and the
            // isolation of effects that depend on the second pass.
            //
            // From the seed pseudorange, subtract:
            //   (a) the slant hydrostatic tropospheric delay (ZHD × mf_d),
            //       about 2–13 m from zenith to 10° elevation;
            //   (b) the IF-combined satellite + receiver ATX LOS projection
            //       (PCO + PCV; no wind-up, since these are code signals),
            //       about 1–7 m (satellite PCO 0.5–2.5 m, IF amplification
            //       about 3×);
            // and add the periodic relativistic term (c) described below.
            //
            // computeEmissionState then runs again with the refined
            // pseudorange, reducing the t_em error from (T + ATX + ε)/c, tens
            // of ns, to about ε/c, a few ns (sub-μm satellite displacement).
            //
            // The ATX call uses (sat_pos, sat_vel_em, elevation, azimuth)
            // from the seed pass.  The refinement changes these by less than
            // a percent, below the scale on which ATX and troposphere vary,
            // so one refinement pass suffices (no iteration).
            if (config_.getModelCorrections().refine_emission_time) {
                double mf_d_seed = 1.0;
                double mf_w_seed = 1.0;
                troposphere_.getTroposphereMapping(elevation, rx_pos, obs.time, mf_d_seed, mf_w_seed);

                double slant_zhd_seed = 0.0;
                if (config_.getModelCorrections().enable_troposphere) {
                    slant_zhd_seed = troposphere_.computeSaastamoinenZHD(rx_pos, obs.time) * mf_d_seed;
                }

                double pco_los_seed = 0.0;
                if (antenna_ != nullptr && !seed_sig1.empty()) {
                    ensureSunMoonCached(obs.time);
                    const std::string rx_ant_type = rinex_parser_.getReceiverAntennaType();
                    const double atx_1 = antenna_->getAntennaCorrection(
                        obs.satellite, seed_sig1, obs.system,
                        rx_pos, sat_pos, sat_vel_em, elevation, azimuth,
                        rx_ant_type, obs.time, cached_sun_pos_);

                    if (!seed_sig2.empty()) {
                        const double atx_2 = antenna_->getAntennaCorrection(
                            obs.satellite, seed_sig2, obs.system,
                            rx_pos, sat_pos, sat_vel_em, elevation, azimuth,
                            rx_ant_type, obs.time, cached_sun_pos_);
                        PppUtils utils(log_manager_);
                        const double f1 = utils.getFrequency(seed_sig1, obs.system, obs.satellite);
                        const double f2 = utils.getFrequency(seed_sig2, obs.system, obs.satellite);
                        if (f1 > 0.0 && f2 > 0.0 && std::abs(f1 - f2) > 1e6) {
                            const double f1sq = f1 * f1, f2sq = f2 * f2;
                            pco_los_seed = (f1sq * atx_1 - f2sq * atx_2) / (f1sq - f2sq);
                        } else {
                            // Frequency lookup failed or bands coincide; fall
                            // back to single-band ATX (still removes most of
                            // the per-band offset, no IF amplification).
                            pco_los_seed = atx_1;
                        }
                    } else {
                        // Single-frequency fallback in extractPseudorange:
                        // apply that signal's ATX directly.
                        pco_los_seed = atx_1;
                    }
                }

                // (c) Periodic relativistic clock correction `dtr = −2(r·v)/c²`.
                //
                // By IGS convention the CLK product carries only the constant
                // and drift terms of δt_s; the periodic relativistic term is
                // computed separately and applied downstream in the observation
                // model.  The seed pseudorange therefore contains an
                // uncancelled `−c·dtr` contribution (dtr is zero at perigee and
                // apogee, negative from perigee to apogee where r·v > 0, and
                // positive from apogee to perigee).  Left in, it biases the seed
                // t_em by `+dtr`: up to about 14 m / 47 ns for GPS at the
                // largest eccentricity, typically 3–7 m / 10–20 ns for IIR/IIF.
                //
                // Removing `−c·dtr` from the pseudorange means adding `+c·dtr`.
                // The sign is opposite to that of the tropo/ATX terms because
                // those enter the pseudorange with positive sign, whereas the
                // relativistic clock effect enters with negative sign through
                // the `−c·δt_s` term.
                //
                // dtr is recomputed downstream from the refined satellite state
                // for the observation-model contribution; the value computed
                // here serves only the seed refinement.
                double dtr_los_seed = 0.0;
                if (config_.getModelCorrections().enable_relativistic_correction) {
                    const double dtr_seed_s = corrections_.computeRelativisticCorrection(
                        obs.satellite, sat_pos_em, sat_vel_em);
                    dtr_los_seed = dtr_seed_s * c;
                }

                if (std::abs(slant_zhd_seed) > 1e-12 ||
                    std::abs(pco_los_seed)   > 1e-12 ||
                    std::abs(dtr_los_seed)   > 1e-12) {
                    const double pseudorange_refined =
                        pseudorange - slant_zhd_seed - pco_los_seed + dtr_los_seed;
                    if (!computeEmissionState(obs, rx_pos, pseudorange_refined,
                                              sat_pos_em, sat_vel_em,
                                              sat_clock_em, emission_time, broadcast_grade, drop_reason))
                        return false;

                    // Re-rotate + re-topocentric with refined sat state.
                    rotateAndComputeTopo(sat_pos_em);

                    // Safety re-check on elevation in case the refinement
                    // pushed a borderline sat across the mask threshold.
                    if (elevation < elevation_threshold_) {
                        log_manager_.getLogger("geometry")->debug(
                            "Satellite {} below elevation threshold (refined): {:.3f} deg < {:.3f} deg",
                            obs.satellite, elevation, elevation_threshold_);
                        drop_reason = DropReason::BelowMask;
                        return false;
                    }

                    log_manager_.getLogger("geometry")->trace(
                        "Refined seed for {}: -slant_zhd={:.3f} m, -pco_los_if={:.3f} m, +dtr_los={:.3f} m",
                        obs.satellite, slant_zhd_seed, pco_los_seed, dtr_los_seed);
                }
            }

            // --- Relativistic periodic correction (uses refined sat state) ---
            relativistic_corr = config_.getModelCorrections().enable_relativistic_correction
                ? corrections_.computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em)
                : 0.0;

            sat_clock = sat_clock_em;

            log_manager_.getLogger("geometry")->debug(
                "Geometry {} at rx_tow={:.3f}: emission_tow={:.3f}, tof={:.9f}s ({:.3f}m), "
                "sat_clk={:.3e}s, rel_corr={:.3e}s, r_dot_v={:.3e}, vel_norm={:.1f}m/s",
                obs.satellite, obs.time.tow, emission_time.tow,
                pseudorange / c, pseudorange,
                sat_clock_em, relativistic_corr,
                sat_pos_em.dot(sat_vel_em), sat_vel_em.norm());

            // --- Propagation corrections ---
            troposphere_.getTroposphereMapping(elevation, rx_pos, obs.time, mf_d, mf_w);
            tropo_delay = config_.getModelCorrections().enable_troposphere
                ? troposphere_.computeSaastamoinenZHD(rx_pos, obs.time)
                : 0.0;
            shapiro_delay = config_.getModelCorrections().enable_shapiro_delay
                ? corrections_.computeShapiroDelay(obs.satellite, rx_pos, sat_pos)
                : 0.0;

            // Sagnac (diagnostic only: the effect is already in sat_pos through the Earth rotation)
            sagnac_corr = (omegaE / c) * (sat_pos_em.x() * rx_pos.y() - sat_pos_em.y() * rx_pos.x());

            // Ocean tidal loading (site displacement projected onto LOS)
            Eigen::Vector3d otl_ecef = config_.getModelCorrections().enable_ocean_tide_loading
                ? corrections_.computeOceanTideCorrection(rx_pos, obs.time)
                : Eigen::Vector3d::Zero();
            otl_corr = otl_ecef.dot(-los);

            // Atmospheric pressure loading (S1/S2 thermal tide; site displacement
            // projected onto LOS).  Same sign convention as OTL/solid-tide: a
            // station displacement toward the satellite shortens the range, and
            // .dot(-los) (sat->rx) gives that sign.  Zero unless the correction
            // is enabled and per-station S1/S2 coefficients are loaded.
            Eigen::Vector3d apl_ecef = config_.getModelCorrections().enable_atmospheric_loading
                ? corrections_.computeAtmosphericTideDisplacement(rx_pos, obs.time)
                : Eigen::Vector3d::Zero();
            apl_corr = apl_ecef.dot(-los);

            // Solid Earth tide (uses cached Sun/Moon positions)
            ensureSunMoonCached(obs.time);
            solid_tide = config_.getModelCorrections().enable_solid_earth_tide
                ? corrections_.computeSolidEarthTide(rx_pos, sat_pos, cached_sun_pos_, cached_moon_pos_, obs.time)
                : 0.0;

            sat_vel = sat_vel_em;

            // Surface the clock provenance captured during emission-time solve:
            // when the clock is broadcast-grade the caller drops carrier phase.
            broadcast_only = broadcast_grade;

            log_manager_.getLogger("geometry")->trace(
                "Satellite geometry for {} at TOW {:.3f}: range={:.3f} m, elev={:.3f} deg, az={:.3f} deg, "
                "tropo={:.3f} m, sat_clock={:.3e} s ({:.3f} m), shapiro={:.3e} m, sagnac={:.3f} m, "
                "otl={:.4f} m, apl={:.4f} m, relativistic={:.3e} s ({:.3f} m), solid_tide={:.3f} m, broadcast_only={}",
                obs.satellite, obs.time.tow, range, elevation, azimuth,
                tropo_delay, sat_clock, sat_clock * c,
                shapiro_delay, sagnac_corr, otl_corr, apl_corr,
                relativistic_corr, relativistic_corr * c, solid_tide, broadcast_only);

            return true;
        }

        // =================================================================
        // Orbit interpolation
        // =================================================================

        /**
         * @brief Compute satellite position and velocity via Neville interpolation.
         *
         * Selects IGS_POINTS consecutive orbit nodes centred on the bracket that
         * contains the query epoch, checks for gaps and extrapolation, rotates
         * the nodes into the ECEF frame of the query epoch, then evaluates
         * position and velocity with Neville's algorithm (nodes sorted by
         * distance from the query point) or, when trig SP3 interpolation is
         * enabled, with a sidereal-trig SVD fit.
         */
        std::pair<Eigen::Vector3d, Eigen::Vector3d> Geometry::getSatellitePV(
            const std::string& satellite, const GnssTime& time) const
        {
            constexpr size_t IGS_POINTS = 11;
            constexpr size_t MIN_POINTS = 9;
            constexpr double MIN_POS_NORM = 1.9e7;   // ~19,000 km (lowest MEO orbit)
            constexpr double MAX_POS_NORM = 4.5e7;   // ~45,000 km (above BeiDou GEO ~42,164 km)
            constexpr double MAX_VEL_NORM = 5.0e3;    // ~5 km/s (GNSS satellites: ~3-4 km/s)

            const auto zero_result = std::make_pair(Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero());

            if (satellite.empty()) {
                log_manager_.getLogger("geometry")->warn("getSatellitePV: empty satellite ID");
                return zero_result;
            }

            // Broadcast (live) ephemeris fallback: decode the satellite PV from
            // the receiver's RTCM/SBF nav stream and overlay the IODE-gated SSR
            // orbit correction, as the SP3 branch below does.  The lambda
            // serves each case where the loaded SP3 cannot describe the query
            // epoch: (a) no SP3 entry, (b) too few valid nodes, and (c) the
            // query epoch outside the SP3's time coverage (for example a stale
            // fixture in real-time mode).  The live store is filled by
            // RealTimeEngine and, under network brdc_rts, by the network epoch
            // loop (ApplicationNetwork.cpp).  In other post-processing runs it is
            // empty, so this returns nullopt and the SP3 path runs unchanged.
            auto liveFallback = [&]() -> std::optional<std::pair<Eigen::Vector3d, Eigen::Vector3d>> {
                // Fetch the SSR first so the broadcast PV is computed from the
                // ephemeris version the orbit delta refers to (iode_ssr).  Across
                // an ephemeris cutover the receiver can be one version ahead of
                // the SSR stream; the store then uses the retained previous
                // record instead of dropping the satellite to broadcast grade.
                const auto ssr = ssrStore().getOrbitClock(satellite, time, config_.getSsrMaxAge());
                const int ssr_iode = (ssr && ssr->has_orbit) ? ssr->iode : -1;
                auto bc = LiveEphemerisStore::getInstance()
                              .computeSatellitePV(satellite, time, ssr_iode);
                if (!bc) return std::nullopt;
                Eigen::Vector3d pos = bc->position;
                Eigen::Vector3d vel = bc->velocity;
                bool applied_apc = false;
                if (ssr && ssr->has_orbit
                    && SsrCorrectionStore::iodeMatches(bc->iode, ssr->iode)) {
                    const double dt = TimeUtils::continuousGpsSeconds(time)
                                    - TimeUtils::continuousGpsSeconds(ssr->orbit_epoch_time);
                    pos = SsrCorrectionStore::applyOrbitToPosition(
                        pos, vel, ssr->delta_rac, ssr->delta_rac_dot, dt);
                    applied_apc =
                        (ssr->orbit_reference_point == SsrReferencePoint::APC);
                }
                // Record whether the overlay applied for this query was
                // APC-referenced, so PppModel can skip the satellite-PCO term.
                // Recorded only while honor_ssr_reference_point is on; with it
                // off the map is never touched and the output is byte-identical
                // (RTB-5(c)).
                if (config_.getRealTimeConfig().honor_ssr_reference_point) {
                    ssr_orbit_apc_applied_[satellite] = applied_apc;
                }
                return std::make_pair(pos, vel);
            };

            // O(1) index lookup
            auto it = orbit_index_.find(satellite);
            if (it == orbit_index_.end()) {
                if (auto r = liveFallback()) return *r;
                log_manager_.getLogger("geometry")->warn("getSatellitePV: no orbit data for {}", satellite);
                return zero_result;
            }

            const auto& sat_orbits = it->second;  // Already sorted chronologically in loadData()
            const double t_query = TimeUtils::continuousGpsSeconds(time);

            // Detect the SP3 epoch interval from the actual orbit data rather than
            // assuming a fixed 900 s (15 min) or 300 s (5 min) spacing. Use the
            // median of the first few consecutive-node spacings for robustness
            // against occasional gaps.
            double sp3_interval = 300.0; // fallback = 5-minutes
            if (sat_orbits.size() >= 2) {
                std::vector<double> spacings;
                spacings.reserve(std::min<size_t>(sat_orbits.size() - 1, 10));
                for (size_t i = 0; i + 1 < sat_orbits.size() && spacings.size() < 10; ++i) {
                    double dt = TimeUtils::continuousGpsSeconds(sat_orbits[i + 1]->time)
                              - TimeUtils::continuousGpsSeconds(sat_orbits[i]->time);
                    if (dt > 0.0) spacings.push_back(dt);
                }
                if (!spacings.empty()) {
                    std::sort(spacings.begin(), spacings.end());
                    sp3_interval = spacings[spacings.size() / 2]; // median
                }
            }
            const double MAX_SPAN = sp3_interval * (IGS_POINTS - 1) + 1.0;

            // === Step 1: collect all valid nodes for this satellite ===
            // Precompute continuous GPS seconds for each orbit node to avoid
            // redundant recomputation inside sort comparators.
            struct NodeEntry {
                const OrbitData* ptr;
                double t;   // precomputed continuousGpsSeconds
            };
            std::vector<NodeEntry> valid_nodes;
            valid_nodes.reserve(sat_orbits.size());

            for (const OrbitData* o : sat_orbits) {
                if (!o->position_valid || !o->position.allFinite()) continue;
                double norm = o->position.norm();
                if (norm <= MIN_POS_NORM || norm >= MAX_POS_NORM) continue;
                valid_nodes.push_back({ o, TimeUtils::continuousGpsSeconds(o->time) });
            }

            if (valid_nodes.size() < MIN_POINTS) {
                if (auto r = liveFallback()) return *r;
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: insufficient valid nodes ({}) for {} at week {} tow {:.3f}",
                    valid_nodes.size(), satellite, time.week, time.tow);
                return zero_result;
            }

            // === Coverage gate (stale-SP3 shadowing) ===
            // valid_nodes is in ascending epoch order (sat_orbits was sorted in
            // loadData()).  If the query epoch falls outside the loaded SP3's time
            // span by more than one node interval, the SP3 does not describe this
            // epoch (typically a stale fixture in real-time mode).  The current
            // broadcast ephemeris is then preferred over extrapolating a stale
            // polynomial to a position the norm gate below would reject.  The
            // one-interval tolerance is wider than the half-interval
            // extrapolation warning further down, so a query near the edge of
            // valid SP3 coverage is not diverted.  In post-processing the live
            // store is empty, liveFallback() returns nullopt, and the SP3 path
            // (with its own gap and extrapolation handling) runs unchanged.
            {
                const double t_cov_first = valid_nodes.front().t;
                const double t_cov_last  = valid_nodes.back().t;
                if (t_query < t_cov_first - sp3_interval || t_query > t_cov_last + sp3_interval) {
                    if (auto r = liveFallback()) {
                        log_manager_.getLogger("geometry")->debug(
                            "getSatellitePV: {} query tow {:.3f} outside SP3 span "
                            "[{:.1f}, {:.1f}]s (continuous); using broadcast ephemeris",
                            satellite, time.tow, t_cov_first, t_cov_last);
                        return *r;
                    }
                }
            }

            // === Step 2: bracket-centred window selection ===
            // Find the bracket [x_a, x_{a+1}] containing t_query, then pick the
            // IGS_POINTS consecutive nodes anchored on it: (width − 1)/2 nodes
            // before the bracket's lower node, that node, and the remaining
            // nodes after it, shifted inward at the data boundaries.  The window
            // shifts by one node when t_query crosses an SP3 epoch; at that
            // instant t_query coincides with a node shared by both windows, so both
            // Neville polynomials return the node value and the position is
            // continuous across the swap.
            //
            // Selecting the nearest N nodes instead swaps a node at the
            // midpoint between SP3 epochs (every 900/2 s for IGS Final), where
            // the two polynomials disagree by a few mm.  That produces a
            // 15-min sawtooth in PPP position, synchronous across the
            // constellation.
            const size_t n_total = valid_nodes.size();
            const size_t n_win = std::min(IGS_POINTS, n_total);

            auto upper_it = std::upper_bound(valid_nodes.begin(), valid_nodes.end(), t_query,
                [](double t, const NodeEntry& n) { return t < n.t; });

            size_t bracket_lo;
            if (upper_it == valid_nodes.begin()) {
                bracket_lo = 0;
            } else if (upper_it == valid_nodes.end()) {
                bracket_lo = n_total - 1;
            } else {
                bracket_lo = std::distance(valid_nodes.begin(), upper_it) - 1;
            }

            // Build a centered window of `width` consecutive indices anchored
            // on the bracket, clipped at the file boundaries.
            auto centered_window = [&](size_t width) -> std::pair<size_t, size_t> {
                const size_t half = (width - 1) / 2;
                size_t s;
                if (bracket_lo < half) {
                    s = 0;
                } else if (bracket_lo + (width - half) > n_total) {
                    s = n_total - width;
                } else {
                    s = bracket_lo - half;
                }
                return { s, s + width };
            };

            auto [win_start, win_end] = centered_window(n_win);
            std::vector<NodeEntry> nodes(valid_nodes.begin() + win_start,
                                         valid_nodes.begin() + win_end);

            // === Step 3: extrapolation and gap checks (chronological order) ===
            const double t_first = nodes.front().t;
            const double t_last  = nodes.back().t;

            if (t_query < t_first || t_query > t_last) {
                const double extrap_dist = (t_query < t_first) ? (t_first - t_query) : (t_query - t_last);
                if (extrap_dist > sp3_interval / 2.0) {
                    log_manager_.getLogger("geometry")->warn(
                        "Position extrapolation for {} at week {} tow {:.3f}: {:.1f}s outside SP3 window [{:.1f}, {:.1f}]s",
                        satellite, time.week, time.tow, extrap_dist,
                        nodes.front().ptr->time.tow, nodes.back().ptr->time.tow);
                }
            }

            // Gap check: if the window span exceeds MAX_SPAN there is a hole
            // in the data.  Re-pick a MIN_POINTS-wide centered window and
            // re-check before giving up.
            const double span = t_last - t_first;
            if (span > MAX_SPAN) {
                auto [s2, e2] = centered_window(std::min(MIN_POINTS, n_total));
                nodes.assign(valid_nodes.begin() + s2, valid_nodes.begin() + e2);

                const double trimmed_span = nodes.back().t - nodes.front().t;
                if (trimmed_span > MAX_SPAN) {
                    log_manager_.getLogger("geometry")->warn(
                        "getSatellitePV: data gap too large after trimming for {} at week {} tow {:.3f}: "
                        "span={:.1f}s exceeds MAX_SPAN={:.1f}s",
                        satellite, time.week, time.tow, trimmed_span, MAX_SPAN);
                    return zero_result;
                }
            }

            // === Step 4: build interpolation arrays sorted by distance from query ===
            // Neville's algorithm converges best when nodes are ordered nearest-first.
            const size_t n = nodes.size();

            // Sort by distance from query for Neville's optimal convergence
            std::sort(nodes.begin(), nodes.end(), [t_query](const NodeEntry& a, const NodeEntry& b) {
                return std::abs(a.t - t_query) < std::abs(b.t - t_query);
            });

            // Earth-rotation pre-correction (RTKLIB / cssrlib / IGS convention):
            // rotate each SP3 node's (X, Y) into the ECEF frame at @c t_query
            // before interpolating.  Without this step the polynomial fits a
            // function that carries the sidereal ω-frequency rotation of ECEF
            // relative to the satellite's inertial trajectory, which a degree-10
            // fit does not capture exactly: the residual is sub-mm in the window
            // interior and grows to about 1 mm near the SP3 boundaries.  With the
            // pre-correction the polynomial fits a smooth, near-inertial function.
            //
            // The rotation: ECEF(t_query) = R_z(ω·(t_node − t_query)) · ECEF(t_node),
            // with R_z the counter-clockwise rotation [cos −sin; sin cos] in (X, Y).
            // All nodes are then expressed in the ECEF frame of t_query, so the
            // value interpolated at t_query is the ECEF position at the query
            // epoch with no post-rotation.
            const double omegaE_interp = Constants::EARTH_ANGULAR_VELOCITY;
            std::vector<double> x(n);
            std::vector<double> yx(n), yy(n), yz(n);
            for (size_t i = 0; i < n; ++i) {
                x[i] = nodes[i].t;
                const double dt_node = nodes[i].t - t_query;
                const double alpha = omegaE_interp * dt_node;
                const double cs = std::cos(alpha);
                const double sn = std::sin(alpha);
                const double px = nodes[i].ptr->position.x();
                const double py = nodes[i].ptr->position.y();
                yx[i] = cs * px - sn * py;
                yy[i] = sn * px + cs * py;
                yz[i] = nodes[i].ptr->position.z();
            }

            Eigen::Vector3d pos = Eigen::Vector3d::Zero();
            Eigen::Vector3d vel = Eigen::Vector3d::Zero();

            // Neville's algorithm: simultaneous position and velocity interpolation.
            // The polynomial derivative at t_query is taken in the ECEF frame
            // frozen at t_query, which does not rotate, so the raw derivative is
            // the satellite's inertial velocity.  Downstream consumers (Doppler
            // line-of-sight rate, BeiDou orbit-normal yaw, relativistic r·v)
            // expect ECEF-frame velocity, recovered as
            // v_ecef = v_inertial − ω × pos, i.e. for ω = (0,0,ω):
            //   v_ecef.x = v_inertial.x + ω · pos.y
            //   v_ecef.y = v_inertial.y − ω · pos.x
            //   v_ecef.z = v_inertial.z
            // (Note: relativity r·v is invariant to this conversion because
            //  r · (ω×r) = 0; only Doppler / cross-product attitude paths see
            //  the difference.)
            try {
                double px, vx, py, vy, pz, vz;
                if (config_.enableTrigSp3Interp()) {
                    // Sidereal-trig SVD fit on the same derotated channels and time
                    // base as the Neville path (yx/yy/yz against continuous GPS
                    // seconds x).  The window holds up to IGS_POINTS nodes, so with
                    // a 9-term basis this is an N-point least-squares smoother that
                    // does not pass through the nodes for N > 9.  This differs on
                    // purpose from the M-PAGES 9-point exact fit, so that the trig
                    // branch reuses the derotated window unchanged (see the
                    // trigSvdInterp doc).  Per-node sigmas come from the SP3 '++'
                    // accuracy codes; those hold one value per satellite, so the
                    // sigma set is uniform and the fit is effectively unweighted,
                    // as in M-PAGES, whose per-satellite SVsig has no effect either.
                    // An empty vector selects the unweighted fast path.
                    std::vector<double> sig;
                    sig.reserve(n);
                    bool any_sigma = false;
                    for (size_t i = 0; i < n; ++i) {
                        const float a = nodes[i].ptr->accuracy_sigma_m;
                        sig.push_back(a > 0.0f ? static_cast<double>(a) : 0.0);
                        if (a > 0.0f) any_sigma = true;
                    }
                    if (!any_sigma) sig.clear();

                    Interpolate::trigSvdInterp(x, yx, sig, t_query, px, vx);
                    Interpolate::trigSvdInterp(x, yy, sig, t_query, py, vy);
                    Interpolate::trigSvdInterp(x, yz, sig, t_query, pz, vz);
                } else {
                    Interpolate::nevilleInterpDeriv(x, yx, t_query, px, vx);
                    Interpolate::nevilleInterpDeriv(x, yy, t_query, py, vy);
                    Interpolate::nevilleInterpDeriv(x, yz, t_query, pz, vz);
                }

                pos = Eigen::Vector3d(px, py, pz);
                vel = Eigen::Vector3d(vx + omegaE_interp * py,
                                      vy - omegaE_interp * px,
                                      vz);
            }
            catch (const std::exception& ex) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: exception during interpolation for {}: {}", satellite, ex.what());
                return zero_result;
            }

            // Validate position
            if (!pos.allFinite()) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: non-finite position for {} at tow {:.3f}", satellite, time.tow);
                return zero_result;
            }

            double pos_norm = pos.norm();
            if (pos_norm <= MIN_POS_NORM || pos_norm >= MAX_POS_NORM) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: position out of range for {} at tow {:.3f}: norm={:.1f} m",
                    satellite, time.tow, pos_norm);
                return zero_result;
            }

            // Validate velocity
            if (!vel.allFinite() || vel.norm() > MAX_VEL_NORM) {
                log_manager_.getLogger("geometry")->warn(
                    "getSatellitePV: invalid velocity for {} at tow {:.3f}: norm={:.1f} m/s",
                    satellite, time.tow, vel.norm());
                vel = Eigen::Vector3d::Zero();
            }

            // Apply the SSR orbit correction if a fresh record is available
            // and the SSR's IODE matches the broadcast-nav baseline of the
            // orbit node closest in time to the query.  When the baseline has
            // no IODE (SP3-derived: iode=-1) the gate passes every record, so
            // post-processing behaviour is unchanged.  When OrbitData carries
            // an iode from a broadcast-nav source, the gate rejects
            // corrections referenced to a different ephemeris.
            int brdc_iode = -1;
            {
                const auto it_idx = orbit_index_.find(satellite);
                if (it_idx != orbit_index_.end() && !it_idx->second.empty()) {
                    const double t_query = TimeUtils::continuousGpsSeconds(time);
                    const OrbitData* closest = nullptr;
                    double best_dt = std::numeric_limits<double>::max();
                    for (const OrbitData* nd : it_idx->second) {
                        const double dt_abs = std::abs(
                            TimeUtils::continuousGpsSeconds(nd->time) - t_query);
                        if (dt_abs < best_dt) { best_dt = dt_abs; closest = nd; }
                    }
                    if (closest) brdc_iode = closest->iode;
                }
            }

            {
                bool applied_apc = false;
                if (auto ssr = ssrStore().getOrbitClock(satellite, time, config_.getSsrMaxAge());
                    ssr && ssr->has_orbit
                    && SsrCorrectionStore::iodeMatches(brdc_iode, ssr->iode)) {
                    const double dt = TimeUtils::continuousGpsSeconds(time)
                                    - TimeUtils::continuousGpsSeconds(ssr->orbit_epoch_time);
                    pos = SsrCorrectionStore::applyOrbitToPosition(
                        pos, vel, ssr->delta_rac, ssr->delta_rac_dot, dt);
                    applied_apc =
                        (ssr->orbit_reference_point == SsrReferencePoint::APC);
                }
                // As in liveFallback: recorded only while
                // honor_ssr_reference_point is on; with it off the path is
                // byte-identical and allocation-free (RTB-5(c)).
                if (config_.getRealTimeConfig().honor_ssr_reference_point) {
                    ssr_orbit_apc_applied_[satellite] = applied_apc;
                }
            }

            // Find chronological window bounds for debug logging without re-sorting
            auto [t_min_it, t_max_it] = std::minmax_element(x.begin(), x.end());
            // Map back to node indices for TOW display
            size_t i_min = static_cast<size_t>(t_min_it - x.begin());
            size_t i_max = static_cast<size_t>(t_max_it - x.begin());

            log_manager_.getLogger("geometry")->debug(
                "getSatellitePV for {} at tow={:.3f}: pos=({:.3f}, {:.3f}, {:.3f}) km, "
                "vel=({:.3f}, {:.3f}, {:.3f}) m/s, |v|={:.3f} m/s, nodes={}, window=[{:.1f},{:.1f}]",
                satellite, time.tow,
                pos.x() / 1000.0, pos.y() / 1000.0, pos.z() / 1000.0,
                vel.x(), vel.y(), vel.z(), vel.norm(),
                n, nodes[i_min].ptr->time.tow, nodes[i_max].ptr->time.tow);

            return { pos, vel };
        }

        // =================================================================
        // Topocentric / attitude / wind-up utilities
        // =================================================================

        Topocentric Geometry::computeTopocentric(const Eigen::Vector3d& rx_pos_ecef,
            const Eigen::Vector3d& sat_pos_ecef) {
            // Pure geometry: static, no logger dependency.  Degenerate inputs
            // (satellite within 1 m of the receiver, or an uninitialised
            // receiver at the Earth's centre) return a zeroed Topocentric,
            // which the caller reads as elevation 0 (below any positive mask).
            Topocentric out;

            Eigen::Vector3d delta = sat_pos_ecef - rx_pos_ecef;
            double range = delta.norm();
            if (range < 1.0) return out;
            if (rx_pos_ecef.norm() < 1e-6) return out;

            Coordinates::Llh ref_llh = Coordinates::ecefToLlh(rx_pos_ecef);
            Eigen::Vector3d los_enu = Coordinates::ecefToEnu(delta, ref_llh) / range;

            double elev_deg = std::asin(std::clamp(los_enu.z(), -1.0, 1.0)) * 180.0 / Constants::PI;
            elev_deg = std::max(0.0, std::min(90.0, elev_deg));

            double az_deg = std::atan2(los_enu.x(), los_enu.y()) * 180.0 / Constants::PI;
            if (az_deg < 0.0) az_deg += 360.0;

            out.elevation_deg = elev_deg;
            out.azimuth_deg = az_deg;
            return out;
        }

    } // namespace Model

} // namespace GnssProcessor