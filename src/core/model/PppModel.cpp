/*!
 * \file PppModel.cpp
 * \brief Implementation of PppModel: PPP/PPP-RTK observation modelling and corrections.
 *
 * Initializes PPP state, computes satellite/receiver geometry, applies corrections
 * (orbits, clocks, tides, troposphere, ionosphere, antenna) and forms the
 * observations the estimator consumes: iono-free combinations, or per-signal
 * rows on the uncombined path (processing.uncombined_enabled, forced on for the
 * PPP-RTK family and the baseline modes).
 *
 * Decomposed into focused helpers:
 *  - Signal selection:   buildFrequencyBandMap, selectUncombinedSignals,
 *                        selectIonoFreePairs, selectSingleSignalFallback.
 *  - Shared corrections: computeBaseModeledValue, computePoleTideProjection.
 *  - Observation paths:  processIonoFreeObservation / processIonoFreePair,
 *                        processUncombinedObservation.
 *
 * External data come from ProductParser and RinexParser.
 */

#include "config/GnssConfig.hpp"
#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "rinex/RinexParser.hpp"
#include "products/ProductParser.hpp"
#include "products/StationInfoFile.hpp"
#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include "measurement/MeasurementModel.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "network/assembly/SecondCodeScope.hpp"   // secondCodeCompanions
#include "network/assembly/SecondPhaseScope.hpp"  // secondPhaseCompanions
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace GnssProcessor {

// ===========================================================================
// File-scope frequency / band mapping tables
// ===========================================================================

namespace {

/// Map system display name to the single-char key used in ClockRefMap.
char systemNameToClockRefChar(const std::string& sys) {
    if (sys == "GPS")     return 'G';
    if (sys == "Galileo") return 'E';
    if (sys == "GLONASS") return 'R';
    if (sys == "BeiDou")  return 'C';
    if (sys == "QZSS")    return 'J';
    if (sys == "SBAS")    return 'S';
    return '\0';
}

/// Maps RINEX observation code prefixes to frequency band names per system.
const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& getSystemToBands() {
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> table = {
        {"GPS",     {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}}},
        {"Galileo", {{"C1", "E1"}, {"L1", "E1"}, {"C5", "E5a"}, {"L5", "E5a"}, {"C7", "E5b"}, {"L7", "E5b"}, {"C6", "E6"}, {"L6", "E6"}, {"C8", "E5"}, {"L8", "E5"}}},
        {"GLONASS", {{"C1", "G1"}, {"L1", "G1"}, {"C2", "G2"}, {"L2", "G2"}, {"C3", "G3"}, {"L3", "G3"}, {"C4", "G1a"}, {"L4", "G1a"}, {"C6", "G2a"}, {"L6", "G2a"}}},
        {"BeiDou",  {{"C1", "B1C"}, {"L1", "B1C"}, {"C2", "B1I"}, {"L2", "B1I"}, {"C5", "B2a"}, {"L5", "B2a"}, {"C7", "B2b"}, {"L7", "B2b"}, {"C8", "B2ab"}, {"L8", "B2ab"}, {"C6", "B3"}, {"L6", "B3"}}},
        {"QZSS",    {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}, {"C6", "L6"}, {"L6", "L6"}}},
        {"SBAS",    {{"C1", "L1"}, {"L1", "L1"}, {"C5", "L5"}, {"L5", "L5"}}}
    };
    return table;
}

/// Maps system name + FrequencyMode to the target frequency bands for pair formation.
const std::map<std::string, std::map<FrequencyMode, std::vector<std::string>>>& getBandMap() {
    // FrequencyMode::Independent uses All's candidate band list; the two differ
    // only in the per-satellite sideband pruning that activeFrequencyBands()
    // applies downstream.  Independent is copied from All below instead of listed
    // separately, so the two lists cannot diverge, and the static coarse filter
    // (getAllowedBandDigits) reads that list as the band-digit superset.
    static const std::map<std::string, std::map<FrequencyMode, std::vector<std::string>>> table = [] {
        std::map<std::string, std::map<FrequencyMode, std::vector<std::string>>> t = {
            {"GPS",     {{FrequencyMode::Dual, {"L1", "L2"}}, {FrequencyMode::Triple, {"L1", "L2", "L5"}}, {FrequencyMode::All, {"L1", "L2", "L5"}}}},
            {"Galileo", {{FrequencyMode::Dual, {"E1", "E5a"}}, {FrequencyMode::Triple, {"E1", "E5a", "E5b"}}, {FrequencyMode::All, {"E1", "E5a", "E5b", "E6", "E5"}}}},
            {"GLONASS", {{FrequencyMode::Dual, {"G1", "G2"}}, {FrequencyMode::Triple, {"G1", "G2", "G3"}}, {FrequencyMode::All, {"G1", "G2", "G3", "G1a", "G2a"}}}},
            {"BeiDou",  {{FrequencyMode::Dual, {"B1I", "B3"}}, {FrequencyMode::Triple, {"B1I", "B3", "B2a"}}, {FrequencyMode::All, {"B1C", "B1I", "B2a", "B2b", "B2ab", "B3"}}}},
            {"QZSS",    {{FrequencyMode::Dual, {"L1", "L2"}}, {FrequencyMode::Triple, {"L1", "L2", "L5"}}, {FrequencyMode::All, {"L1", "L2", "L5", "L6"}}}},
            {"SBAS",    {{FrequencyMode::Dual, {"L1", "L5"}}, {FrequencyMode::Triple, {"L1", "L5"}}, {FrequencyMode::All, {"L1", "L5"}}}}
        };
        for (auto& kv : t) kv.second[FrequencyMode::Independent] = kv.second[FrequencyMode::All];
        return t;
    }();
    return table;
}

/// Human-readable label for a FrequencyMode value.
const char* frequencyModeLabel(FrequencyMode m) {
    switch (m) {
    case FrequencyMode::Dual:        return "Dual";
    case FrequencyMode::Triple:      return "Triple";
    case FrequencyMode::All:         return "All";
    case FrequencyMode::Independent: return "Independent";
    default:                         return "Unknown";
    }
}

/// Drop the E5/B2 sidebands from @p bands when this satellite also reports the
/// coherent wideband (RINEX band 8: Galileo E5 AltBOC, BeiDou B2(a+b)).
/// Implements FrequencyMode::Independent: the wideband is the joint coherent
/// processing of the same sidebands, so keeping all three would feed correlated
/// rows to the diagonal-R filter as independent ones and over-count the band.
/// When the wideband is not observed for this satellite, the sidebands are kept
/// and no band is lost.  No-op for systems without a band-8 wideband (GPS,
/// GLONASS, QZSS, SBAS).
void pruneRedundantSidebands(const std::string& system_str,
                             std::vector<std::string>& bands,
                             const ObsValueMap<double>& measurements) {
    // Logical sideband band-names superseded by the band-8 wideband, per system.
    static const std::map<std::string, std::vector<std::string>> kSidebands = {
        {"Galileo", {"E5a", "E5b"}},   // superseded by "E5"   (alt-BOC, band 8)
        {"BeiDou",  {"B2a", "B2b"}},   // superseded by "B2ab" (band 8)
    };
    const auto sit = kSidebands.find(system_str);
    if (sit == kSidebands.end()) return;

    // Require the wideband phase (a band-8 L observable).  Phase is the
    // precision- and ambiguity-critical observable this redundancy concerns,
    // and the wideband can replace the sidebands only when its carrier is
    // tracked.  With band-8 code (C8) alone, the sidebands are kept so the E5/B2
    // carrier is not lost.
    bool wideband_observed = false;
    for (const auto& kv : measurements) {
        const std::string& sig = kv.first;            // e.g. "L8Q", "L8X"
        if (sig.size() >= 2 && sig[0] == 'L' && sig[1] == '8') {
            wideband_observed = true;
            break;
        }
    }
    if (!wideband_observed) return;                    // fallback: retain sidebands

    for (const auto& sb : sit->second)
        bands.erase(std::remove(bands.begin(), bands.end(), sb), bands.end());
}

/// Active frequency bands for (system, mode): the static band-map list with
/// FrequencyMode::Independent's per-satellite sideband pruning applied.  The
/// single source of truth used by every band-selection path so the uncombined
/// and iono-free flows stay consistent.
std::vector<std::string> activeFrequencyBands(
        const std::string& system_str, FrequencyMode mode,
        const ObsValueMap<double>& measurements) {
    const auto& band_map = getBandMap();
    const auto it = band_map.find(system_str);
    if (it == band_map.end()) return {};
    const auto mit = it->second.find(mode);
    if (mit == it->second.end()) return {};
    std::vector<std::string> bands = mit->second;      // copy
    if (mode == FrequencyMode::Independent)
        pruneRedundantSidebands(system_str, bands, measurements);
    return bands;
}

} // anonymous namespace

    // ===========================================================================
    //  Free function: allowed RINEX band digits for (constellation, FrequencyMode)
    // ===========================================================================
    std::set<char> getAllowedBandDigits(const std::string& constellation, FrequencyMode mode) {
        std::set<char> digits;
        const auto& band_map        = getBandMap();
        const auto& system_to_bands = getSystemToBands();

        auto bit = band_map.find(constellation);
        if (bit == band_map.end()) return digits;
        auto mit = bit->second.find(mode);
        if (mit == bit->second.end()) return digits;

        auto sit = system_to_bands.find(constellation);
        if (sit == system_to_bands.end()) return digits;

        // For each allowed band name (e.g. "L1", "E5b"), find every RINEX
        // prefix mapped to it ("C7", "L7" → digit '7' for Galileo E5b)
        // and collect those digits.  A given band name may have multiple
        // prefixes (code + phase share the same digit), so the set
        // deduplicates naturally.
        for (const auto& band_name : mit->second) {
            for (const auto& [prefix, name] : sit->second) {
                if (name == band_name && prefix.size() >= 2)
                    digits.insert(prefix[1]);
            }
        }
        return digits;
    }

    /**
     * @brief Construct a PppModel instance.
     *
     * Builds the internal geometry, ionosphere, antenna and corrections helpers.
     *
     * @param config Read-only configuration.
     * @param product_parser External products (SP3/CLK/...).
     * @param log_manager Diagnostic output.
     * @param rinex_parser Observation metadata and receiver info.
     */
    PppModel::PppModel(const GnssConfig& config, ProductParser& product_parser,
        LogManager& log_manager, const RinexParser& rinex_parser)
        : config_(config),
        product_parser_(product_parser),
        log_manager_(log_manager),
        rinex_parser_(rinex_parser),
        ppp_utils_(log_manager),
        // Pass cache reference to geometry
        geometry_(config, ProductCache::getInstance(), rinex_parser, log_manager),
        // GnssCorrections shares the single cached copy (no per-instance duplication)
        gnss_corrections_(log_manager, config,
            ProductCache::getInstance().getClocksShared(),
            ProductCache::getInstance().getErpsShared(),
            ProductCache::getInstance().getBiasesShared(),
            rinex_parser),
        // ionosphere_ is a std::optional, constructed in the body below
        // only when the Klobuchar fallback is enabled.
        antenna_(config, product_parser, log_manager, rinex_parser),
        stochastic_model_(config, log_manager),
        signal_priorities_(config.getSignalPriorities()),
        // Initialize reference from cache
        klobuchar_coeffs_(ProductCache::getInstance().getKlobucharCoeffs())
    {
        // Wire the antenna model into Geometry so its emission-time
        // refinement can subtract the IF-combined satellite + receiver
        // ATX LOS projection from the seed pseudorange.  Both antenna_
        // and geometry_ are members of this PppModel instance, so the
        // pointer Geometry stores is valid for the lifetime of *this.
        geometry_.setAntennaModel(antenna_);

        // Optional Klobuchar ionospheric-delay model.  Skipped (along with
        // its inner Geometry construction and coefficient parse) for the
        // common configs where it would be a no-op anyway: iono-free
        // combinations, DD short-baseline, and PPP-RTK uncombined (per-
        // satellite slant iono states).  Set
        // model_corrections.enable_klobuchar_ionosphere=true for single-
        // frequency PPP runs that genuinely need the fallback.
        if (config.getModelCorrections().enable_klobuchar_ionosphere) {
            ionosphere_.emplace(config, log_manager, product_parser, rinex_parser);
        }

        // Ocean pole tide (IERS 2010 Sec. 7.1.5 / Desai 2002): interpolate the
        // six per-station load coefficients from the global opole grid into
        // gnss_corrections_, the instance whose computePoleTideCorrection
        // computePoleTideProjection calls each epoch.  The Geometry-owned
        // GnssCorrections that handles OTL and solid tide is a separate object
        // and does not compute the pole tide.  Gated by enable_ocean_pole_tide;
        // with it off (the default) the grid is never read and runs are
        // byte-identical.
        if (config.getModelCorrections().enable_ocean_pole_tide) {
            auto log = log_manager_.getLogger("ppp_model");
            const Eigen::Vector3d opt_ecef = rinex_parser_.getApproxPositionXyz();

            // Prefer per-station SIF Desai coefficients when the station-info
            // subsystem is enabled and the station resolves.  These are the
            // authoritative values the opole grid is sampled from, so they
            // carry no bilinear-interpolation error.  SifOceanPoleTide is laid
            // out in exactly the [N_R, N_I, E_R, E_I, U_R, U_I] order
            // setOceanPoleTideCoefficients expects.  The RINEX APPROX position
            // confirms a 4-char prefix match (find() ignores it when unusable).
            bool from_sif = false;
            if (config.enableStationInfo()) {
                if (const auto sif = config.getStationInfo()) {
                    const std::string key = rinex_parser_.getHeader().marker_name;
                    if (const StationInfo* si = sif->find(key, &opt_ecef); si && si->opt.valid) {
                        gnss_corrections_.setOceanPoleTideCoefficients(
                            { si->opt.north_real, si->opt.north_imag,
                              si->opt.east_real,  si->opt.east_imag,
                              si->opt.up_real,    si->opt.up_imag });
                        log->info("Ocean pole tide: per-station SIF coefficients for marker '{}' "
                                  "(matched {})", key, si->id);
                        log_manager_.getLogger("main_log")->info(
                            "Ocean pole tide source: SIF per-station coefficients (marker '{}' -> {})",
                            key, si->id);
                        from_sif = true;
                    }
                }
            }

            if (!from_sif) {
                std::filesystem::path opt_grid = config.getOptGridPath();
                if (opt_grid.empty()) {
                    opt_grid = config.getInternalDataRoot() / "static" / "opole"
                             / "opoleloadcoefcmcor.txt";
                }
                if (opt_ecef.allFinite() && opt_ecef.norm() > 1e6
                    && !opt_grid.empty() && std::filesystem::exists(opt_grid)) {
                    if (gnss_corrections_.loadOceanPoleTideFromGrid(opt_grid.string(), opt_ecef))
                        log->info("Ocean pole tide: coefficients loaded from '{}'", opt_grid.string());
                    else
                        log->warn("Ocean pole tide: grid load failed for '{}'; OPT disabled",
                                  opt_grid.string());
                } else {
                    log->warn("Ocean pole tide enabled but grid not found ('{}') or station ECEF "
                              "invalid; OPT disabled. Place opoleloadcoefcmcor.txt under "
                              "<data>/static/opole/.",
                              opt_grid.empty() ? "<unset>" : opt_grid.string());
                }
            }
        }

        // Connect GLONASS frequency slots from RINEX parser to PppUtils
        ppp_utils_.setGlonassSlots(rinex_parser_.getGlonassFreqSlots());
        log_manager_.getLogger("ppp_model")->info(
            "PppModel initialized: mode={}, elev_mask={}°, klobuchar_coeffs={}",
            config.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config.getElevationMaskDeg(),
            klobuchar_coeffs_.size());
    }

    /**
     * @brief Initialize a PppState with default values from RINEX and the configuration.
     *
     * Populates position (a configured reference position, or the RINEX marker position with
     * the receiver antenna H/E/N offset applied), epoch time, clock bias and troposphere ZWD.
     * No zenith ionosphere state is set: in PPP-RTK mode, MeasurementModel creates per-satellite
     * slant ionosphere states as satellites appear.  A state whose position is nonzero, free of
     * NaN and within 1e9 m of the geocentre, with a 5-row core covariance, is left unchanged.
     *
     * @param state Output PppState to initialize (modified in-place).
     */
    void PppModel::initializeState(PppState& state) {
        size_t core_size = 5; // Position(3) + Clock(1) + Tropo(1) for both PPP and PPP-RTK
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            // If a user-supplied reference position is configured, use it directly
            // (bypasses RINEX APPROX POSITION and antenna offset). This is intended
            // for fixed-station residual analysis with zero position process noise.
            if (config_.hasReferencePosition()) {
                // Propagate the fixed a-priori to the observation epoch via the
                // configured ITRF station velocity (no-op when disabled/unset).
                const GnssTime t_obs = rinex_parser_.getTimeOfFirstObs();
                const auto rp = config_.getReferencePositionEcefAtEpoch(t_obs);
                state.position = Eigen::Vector3d(rp[0], rp[1], rp[2]);
                if (config_.enableItrfVelocity()) {
                    const auto& r0 = config_.getReferencePositionEcef();
                    log_manager_.getLogger("ppp_model")->info(
                        "Using user-supplied reference position (ITRF-velocity propagated): "
                        "({:.4f}, {:.4f}, {:.4f}) m [r0=({:.4f}, {:.4f}, {:.4f}), "
                        "shift=({:+.4f}, {:+.4f}, {:+.4f}) m]",
                        rp[0], rp[1], rp[2], r0[0], r0[1], r0[2],
                        rp[0] - r0[0], rp[1] - r0[1], rp[2] - r0[2]);
                }
                else {
                    log_manager_.getLogger("ppp_model")->info(
                        "Using user-supplied reference position: ({:.4f}, {:.4f}, {:.4f}) m",
                        state.position.x(), state.position.y(), state.position.z());
                }
            }
            else if (Eigen::Vector3d marker_pos = rinex_parser_.getApproxPositionXyz();
                     marker_pos.norm() > 1e3 && !marker_pos.hasNaN()) {
                // Apply the antenna H/E/N offset to move from the marker to the ARP.
                // RINEX APPROX POSITION XYZ is the marker position.
                // ANTENNA: DELTA H/E/N gives the offset from marker to ARP in the local frame
                // (H = Up, E = East, N = North).
                Eigen::Vector3d hen = rinex_parser_.getAntennaDeltaHen();

                if (hen.norm() > 1e-6) {
                    // Coordinates owns the (H,E,N) -> ENU reorder and the rotation.
                    const Coordinates::Llh marker_llh = Coordinates::ecefToLlh(marker_pos);
                    state.position = Coordinates::markerToArp(marker_pos, hen);

                    log_manager_.getLogger("ppp_model")->info(
                        "Applied antenna H/E/N offset ({:.4f}, {:.4f}, {:.4f}) m to marker position, "
                        "ARP ECEF: ({:.3f}, {:.3f}, {:.3f}) m, lat={:.6f}°, lon={:.6f}°",
                        hen(0), hen(1), hen(2),
                        state.position.x(), state.position.y(), state.position.z(),
                        marker_llh.lat_rad * 180.0 / Constants::PI,
                        marker_llh.lon_rad * 180.0 / Constants::PI);
                }
                else {
                    state.position = marker_pos;
                    log_manager_.getLogger("ppp_model")->info(
                        "Initialized position from RINEX marker (no antenna offset): ({:.3f}, {:.3f}, {:.3f}) m",
                        marker_pos.x(), marker_pos.y(), marker_pos.z());
                }
            }
            else if (!config_.hasReferencePosition()) {
                state.position = Eigen::Vector3d::Zero();
                log_manager_.getLogger("ppp_model")->warn("Invalid or zero RINEX approximate position, using default: (0, 0, 0) m");
            }

            GnssTime init_time = rinex_parser_.getTimeOfFirstObs();
            if (init_time.tow > 0.0 && init_time.week > 0) {
                state.epoch_time = init_time;
                log_manager_.getLogger("ppp_model")->info("Initialized state epoch time from RINEX: week {}, TOW {:.3f}",
                    init_time.week, init_time.tow);
            }
            else {
                state.epoch_time = GnssTime{ 0, 0.0 };
                log_manager_.getLogger("ppp_model")->warn("Invalid RINEX time of first observation, using default: TOW 0.0");
            }

            state.clock_bias = 0.0;
            // Initialize ZWD to a climatological a priori value.
            // Typical ZWD at sea level ranges from ~0.05 m (dry/cold) to ~0.35 m (humid/tropical).
            // A reasonable mid-latitude default is ~0.1 m. The filter will refine this estimate.
            state.troposphere_zwd = 0.1;
            log_manager_.getLogger("ppp_model")->info("Initialized troposphere ZWD to climatological default: {:.3f} m", state.troposphere_zwd);
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                // Per-satellite slant ionosphere states are created dynamically
                // in MeasurementModel as satellites become visible. No single
                // zenith iono state is needed.
                log_manager_.getLogger("ppp_model")->info("PPP-RTK mode: per-satellite ionosphere states will be created dynamically");
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->debug("State position already initialized: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                state.position.x(), state.position.y(), state.position.z(), state.position.norm());
        }
    }

    std::map<std::pair<std::string, char>, PppModel::CanonicalPick>
    PppModel::decideCanonicalPicks(const std::vector<CanonicalCandidate>& candidates,
                                   CanonicalSignalRule rule,
                                   double min_coverage,
                                   bool code_follows_phase) {
        using Key = std::pair<std::string, char>;
        // Group by (band, obs-type), preserving the caller's priority order.
        std::map<Key, std::vector<const CanonicalCandidate*>> by_key;
        for (const auto& c : candidates) {
            if (c.count <= 0) continue;
            if (c.band.empty()) continue;
            if (c.obs_type != 'C' && c.obs_type != 'L') continue;
            by_key[Key{ c.band, c.obs_type }].push_back(&c);
        }
        const double cov = std::min(1.0, std::max(0.0, min_coverage));

        // The rule itself, optionally with one signal withheld.  With the winner
        // withheld it returns the runner-up (what would have been latched instead).
        auto choose = [&](const std::vector<const CanonicalCandidate*>& v,
                          const std::string& withheld) -> const CanonicalCandidate* {
            if (rule == CanonicalSignalRule::Priority) {
                int max_count = 0;
                for (const auto* c : v)
                    if (c->signal != withheld) max_count = std::max(max_count, c->count);
                for (const auto* c : v) {
                    if (c->signal == withheld) continue;
                    if (static_cast<double>(c->count) >= cov * static_cast<double>(max_count))
                        return c;
                }
                return nullptr;
            }
            // Count: strictly-greater replacement over the priority order, so the
            // earlier (higher-priority) entry keeps a tie (the historical rule).
            const CanonicalCandidate* best = nullptr;
            for (const auto* c : v) {
                if (c->signal == withheld) continue;
                if (!best || c->count > best->count) best = c;
            }
            return best;
        };

        std::map<Key, CanonicalPick> out;
        for (const auto& [key, v] : by_key) {
            const CanonicalCandidate* win = choose(v, std::string{});
            if (!win) continue;
            const CanonicalCandidate* run = choose(v, win->signal);
            out[key] = CanonicalPick{ win->signal, win->count,
                                      run ? run->signal : std::string{},
                                      run ? run->count : 0 };
        }
        if (rule != CanonicalSignalRule::Priority) return out;
        // With the follow clause off, each code row keeps the pick the priority
        // rule made from the code's own list above: the same
        // first-in-the-list-with-coverage decision the phase got, without reference
        // to the phase's attribute.  Nothing else in the rule changes, so a station
        // whose two lists agree latches identically either way.
        if (!code_follows_phase) return out;

        // The code follows the phase's tracking attribute where that code exists
        // at this station and clears the same floor.  A station that latches L1W
        // takes C1W, so the pair the semi-codeless receiver actually tracks stays
        // together; a station with no matching code keeps its own pick.
        for (auto& [key, pick] : out) {
            if (key.second != 'C') continue;
            const auto phase_it = out.find(Key{ key.first, 'L' });
            if (phase_it == out.end() || phase_it->second.signal.empty()) continue;
            const char attr = phase_it->second.signal.back();
            if (!pick.signal.empty() && pick.signal.back() == attr) continue;  // already follows
            const auto& v = by_key[key];
            int max_count = 0;
            for (const auto* c : v) max_count = std::max(max_count, c->count);
            for (const auto* c : v) {
                if (c->signal.empty() || c->signal.back() != attr) continue;
                if (static_cast<double>(c->count) < cov * static_cast<double>(max_count)) break;
                pick.runner_up       = pick.signal;
                pick.runner_up_count = pick.count;
                pick.signal          = c->signal;
                pick.count           = c->count;
                break;
            }
        }
        return out;
    }

    CanonicalSignalRule
    PppModel::canonicalRuleForStation(CanonicalSignalRule rule,
                                      const std::vector<std::string>& priority_stations,
                                      const std::string& station) {
        if (priority_stations.empty()) return rule;   // no scope: the global rule
        if (station.empty()) return CanonicalSignalRule::Count;
        const auto ieq = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::toupper(static_cast<unsigned char>(a[i]))
                    != std::toupper(static_cast<unsigned char>(b[i]))) return false;
            return true;
        };
        for (const auto& s : priority_stations)
            if (ieq(s, station)) return CanonicalSignalRule::Priority;
        return CanonicalSignalRule::Count;
    }

    void PppModel::decideConstellationCanonicals(const std::vector<RinexObservation>& observations) {
        // Group observations by constellation so per-constellation tallies
        // only consider that constellation's satellites.
        std::map<std::string, std::vector<const RinexObservation*>> by_cons;
        for (const auto& obs : observations) {
            const std::string sys = PppUtils::getSystemString(obs.system);
            if (sys == "Unknown") continue;
            by_cons[sys].push_back(&obs);
        }

        const auto& system_to_bands = getSystemToBands();

        for (const auto& [cons, sat_obs_list] : by_cons) {
            // A constellation whose latch was restored from a snapshot runs this
            // tally once more, diagnostically: it writes nothing and only counts the
            // restored entries this tally would have decided differently.  That
            // audit is the only record that a resume would have re-decided the
            // latch.  It is unreachable without a restore, so every other run takes
            // the original guard unchanged.  See docs/SPEC_event_harness.md §21.16
            // (RS-8).
            const bool already_decided = constellation_canonical_decided_.count(cons) > 0;
            const bool audit_only = already_decided
                                    && constellation_canonical_audit_pending_.count(cons) > 0;
            if (already_decided && !audit_only) continue;
            if (sat_obs_list.empty()) continue;

            auto pit = signal_priorities_.find(cons);
            if (pit == signal_priorities_.end()) continue;
            const auto& signals = pit->second;

            auto sit = system_to_bands.find(cons);
            if (sit == system_to_bands.end()) continue;

            // Tally every admitted priority-list channel of this constellation at
            // this epoch, in priority order, and pass the ordered candidates to
            // the pure rule.  Under the default
            // CanonicalSignalRule::Count that rule is the historical one: per
            // (band, obs_type) the highest observation count wins, and because
            // the list is walked once with strictly-greater replacement, the
            // earlier (higher-priority) signal keeps a tie.
            std::vector<CanonicalCandidate> candidates;
            candidates.reserve(signals.size());

            for (const auto& sig : signals) {
                if (sig.size() < 3) continue;
                const char obs_type = sig[0];
                if (obs_type != 'C' && obs_type != 'L') continue;
                // Network coverage gate: a sparsely tracked code is not a
                // canonical candidate, so the per-band pick falls through to the
                // best admitted channel (e.g. L1C when L1W is excluded network-wide).
                if (!signalAdmitted(cons, sig)) continue;

                // Resolve which band this signal belongs to via the
                // RINEX-prefix → band-name table.
                std::string band_name;
                for (const auto& [pattern, name] : sit->second) {
                    if (sig.find(pattern) == 0) { band_name = name; break; }
                }
                if (band_name.empty()) continue;

                int count = 0;
                for (const auto* obs : sat_obs_list)
                    if (obs->measurements.count(sig)) ++count;
                if (count == 0) continue;

                candidates.push_back(CanonicalCandidate{ sig, band_name, obs_type, count });
            }

            // The rule is scoped per station.  With no station list
            // configured this is config_.canonicalSignalRule() unchanged, so the
            // output is byte-identical to the unscoped rule; with a list, the listed
            // stations decide under 'priority' and the rest under 'count'.
            const auto station_rule =
                canonicalRuleForStation(config_.canonicalSignalRule(),
                                        config_.canonicalSignalPriorityStations(),
                                        canonical_trace_label_);
            // The follow clause is the code half of the priority rule, so it is
            // passed in the same call; true is the CSL-1 behaviour.
            const auto picks = decideCanonicalPicks(candidates, station_rule,
                                                    config_.canonicalSignalMinCoverage(),
                                                    config_.canonicalSignalCodeFollowsPhase());
            struct Candidate {
                std::string signal;
                int count{ 0 };
            };
            std::map<std::pair<std::string, char>, Candidate> best;
            for (const auto& [key, pick] : picks)
                best[key] = Candidate{ pick.signal, pick.count };

            // Compare the picks with the producer's declared roster.
            // The comparison runs whenever a declaration reached
            // this model, except in the audit pass; the latch moves only under
            // network.closed_loop.roster_latch, so with the knob off the comparison
            // only counts and the output is byte-identical.  It runs before the
            // summary below so every trace line states the latch this station will
            // use, not the one its own rule would have chosen.  Unreachable without
            // a declaration.
            std::string declared_note;
            if (declared_roster_.present && !audit_only) {
                std::map<std::pair<std::string, char>, std::string> chosen;
                for (const auto& [key, cand] : best) chosen[key] = cand.signal;
                applyDeclaredLatch_(cons, sit->second, candidates, chosen);
                for (auto& [key, cand] : best) {
                    const auto dit = chosen.find(key);
                    if (dit == chosen.end() || dit->second == cand.signal) continue;
                    if (!declared_note.empty()) declared_note += ", ";
                    declared_note += key.first + std::string(1, key.second) + ": "
                                   + cand.signal + " -> " + dit->second;
                    cand.signal = dit->second;
                    cand.count  = 0;
                    for (const auto& c : candidates)
                        if (c.signal == cand.signal && c.obs_type == key.second)
                            cand.count = c.count;
                }
            }

            // Build a stable trace string for the decision log.
            std::string summary;
            for (const auto& [key, cand] : best) {
                if (!summary.empty()) summary += ", ";
                summary += cand.signal + "(" + key.first + ":"
                         + std::to_string(cand.count) + ")";
            }

            if (audit_only) {
                // Audit pass (RS-8): compare, count and log, and leave the latch
                // unchanged.  A disagreement is the case the restore prevents: the
                // kept signal is the snapshot writer's, and the tallied one is what
                // this run would have latched from the resume epoch's satellite counts.
                std::string disagreements;
                long long n_seen = 0, n_diff = 0;
                const auto cit = constellation_canonical_.find(cons);
                const std::map<std::pair<std::string, char>, std::string> kNoLatch;
                const auto& locked =
                    (cit != constellation_canonical_.end()) ? cit->second : kNoLatch;
                for (const auto& [key, cand] : best) {
                    const auto lit = locked.find(key);
                    if (lit == locked.end()) continue;   // nothing restored for this band
                    ++n_seen;
                    if (lit->second == cand.signal) continue;
                    ++n_diff;
                    if (!disagreements.empty()) disagreements += ", ";
                    disagreements += key.first + std::string(1, key.second) + " keeps "
                                   + lit->second + " vs " + cand.signal + "("
                                   + std::to_string(cand.count) + ") here";
                }
                canon_audit_entries_  += n_seen;
                canon_audit_disagree_ += n_diff;
                constellation_canonical_audit_pending_.erase(cons);
                log_manager_.getLogger("main_log")->info(
                    "Canonical signals audited for {} station '{}' ({} sats this "
                    "epoch): {} of {} restored entr{} would have been decided "
                    "DIFFERENTLY here{}{} | this epoch's tally: {}",
                    cons, canonical_trace_label_.empty() ? "-" : canonical_trace_label_,
                    sat_obs_list.size(), n_diff, n_seen, n_seen == 1 ? "y" : "ies",
                    n_diff > 0 ? " — " : "", disagreements, summary);
                continue;
            }

            for (const auto& [key, cand] : best)
                constellation_canonical_[cons][key] = cand.signal;
            constellation_canonical_decided_.insert(cons);
            // Retain the candidate list so the signal roster's census can evaluate a
            // second rule over the same first-epoch counts, such as what the count
            // rule would have latched here.  A pure store, kept on
            // every network run since the roster is built on every one,
            // and on no other: the roster is the only reader.  Nothing on the
            // latch path reads it, so no branch changes.
            if (config_.isNetworkMode()) canonical_candidates_[cons] = candidates;

            log_manager_.getLogger("ppp_model")->info(
                "Canonical signals locked for {} ({} sats this epoch): {}",
                cons, sat_obs_list.size(), summary);
            // The same decision on main_log, which every run writes; the ppp_model
            // log is empty (0 bytes) in every run, so the line above is not seen.
            // Trace-class: products are byte-identical (RS-8, §21.16).
            log_manager_.getLogger("main_log")->info(
                "Canonical signals decided for {} station '{}' ({} sats this epoch): {}",
                cons, canonical_trace_label_.empty() ? "-" : canonical_trace_label_,
                sat_obs_list.size(), summary);
            // A latch that the declaration moved is logged on the same logger as the
            // decision, so an armed run can be read from the trace as well as from
            // the counters.  Silent when the declaration changed nothing, so an
            // inert arm adds no bytes.
            if (!declared_note.empty())
                log_manager_.getLogger("main_log")->info(
                    "Canonical latch FOLLOWS the producer's declared roster for {} station "
                    "'{}' (network.closed_loop.roster_latch): {}",
                    cons, canonical_trace_label_.empty() ? "-" : canonical_trace_label_,
                    declared_note);

            // One line per station and constellation naming the latch and its
            // runner-up with both counts, under either rule, so a run states which
            // tracking attribute each band carries and by what margin the
            // alternative lost.  Trace-class: nothing downstream reads
            // it, and products and event ledgers are unaffected.
            std::string latch_detail;
            for (const auto& [key, pick] : picks) {
                if (!latch_detail.empty()) latch_detail += ", ";
                latch_detail += key.first + std::string(1, key.second) + "="
                              + pick.signal + "(" + std::to_string(pick.count) + ")";
                latch_detail += pick.runner_up.empty()
                              ? " [sole]"
                              : " over " + pick.runner_up + "("
                                + std::to_string(pick.runner_up_count) + ")";
            }
            // The rule named here is the one this station decided under, not the
            // global key, so a scoped run never reports 'priority' at a station
            // running 'count'.  With no scope the two are the same value, so an
            // unscoped run's line is unchanged.
            // The follow setting is named only when it is off: the default line
            // stays byte-identical, and a run that turned the clause off says so on
            // the line that states the latch, which is where an arm of the code
            // half is read.
            log_manager_.getLogger("main_log")->info(
                "Canonical latch for {} station '{}' (rule={}{}{}, min_coverage={:.2f}, "
                "{} sats this epoch): {}",
                cons, canonical_trace_label_.empty() ? "-" : canonical_trace_label_,
                station_rule == CanonicalSignalRule::Priority ? "priority" : "count",
                config_.canonicalSignalPriorityStations().empty()
                    ? std::string()
                    : fmt::format(" [scoped to {} station(s)]",
                                  config_.canonicalSignalPriorityStations().size()),
                config_.canonicalSignalCodeFollowsPhase()
                    ? std::string()
                    : std::string(", code_follows_phase=false"),
                config_.canonicalSignalMinCoverage(), sat_obs_list.size(),
                latch_detail.empty() ? std::string("(none)") : latch_detail);
        }
    }

    void PppModel::setDeclaredSignalRoster(const NetworkSignalDeclaration& decl, bool act) {
        declared_roster_     = decl;
        declared_roster_act_ = act && decl.present;
        if (decl.present) declared_latch_.seen = true;
        declared_latch_.armed = declared_roster_act_;
    }

    void PppModel::applyDeclaredLatch_(
            const std::string& cons,
            const std::vector<std::pair<std::string, std::string>>& band_table,
            const std::vector<CanonicalCandidate>& candidates,
            std::map<std::pair<std::string, char>, std::string>& chosen) {
        const auto cit = declared_roster_.bands.find(cons);
        if (cit == declared_roster_.bands.end()) return;   // nothing declared here

        // The band name of a signal, through the same prefix table the candidate
        // walk in decideConstellationCanonicals uses.  Resolving it from the signal
        // instead of from the declaration's band digit keeps one mapping rule in
        // the file: digit and name are 1:1 within a constellation, but only this
        // table states it.
        const auto bandNameOf = [&](const std::string& sig) -> std::string {
            for (const auto& [pattern, name] : band_table)
                if (sig.find(pattern) == 0) return name;
            return {};
        };
        const auto tracked = [&](const std::string& sig, char type) {
            for (const auto& c : candidates)
                if (c.signal == sig && c.obs_type == type && c.count > 0) return true;
            return false;
        };

        std::set<std::pair<std::string, char>> handled;
        for (const auto& [band_digit, row] : cit->second) {
            (void)band_digit;
            for (const char type : { 'L', 'C' }) {
                const std::vector<std::string>& decl_list =
                    (type == 'L') ? row.phase : row.code;
                if (decl_list.empty()) continue;
                // The declaration's own order is the priority list here: the first
                // declared signal this station tracks wins.  This is the signal-latch
                // rule keyed to the producer's declaration instead of the config list.
                std::string pick;
                for (const std::string& sig : decl_list)
                    if (tracked(sig, type)) { pick = sig; break; }

                const std::string band_name =
                    bandNameOf(pick.empty() ? decl_list.front() : pick);
                if (band_name.empty()) continue;
                const auto key = std::make_pair(band_name, type);
                const auto own = chosen.find(key);
                if (own == chosen.end()) continue;   // this station latched nothing here
                handled.insert(key);
                ++declared_latch_.compared;
                // This station latched a signal for which the network declares no
                // family, so the correction it needs is absent from the wire, not
                // late (the observed dropouts).  The count does not depend on
                // what the latch then does.  It is taken before the branch below
                // because a station that tracks none of the declared signals is
                // the worst case of this class.
                if (std::find(decl_list.begin(), decl_list.end(), own->second)
                    == decl_list.end())
                    ++declared_latch_.own_not_declared;

                if (pick.empty()) {
                    // The network carries families on this band, and this station
                    // tracks none of them: its own rule stands, and that is the
                    // reading, not a failure.
                    ++declared_latch_.none_tracked;
                    ++declared_latch_.own_rule;
                    continue;
                }
                if (own->second == pick) {
                    ++declared_latch_.agree;
                    ++declared_latch_.own_rule;
                    continue;
                }
                ++declared_latch_.differ;
                if (declared_roster_act_) {
                    chosen[key] = pick;
                    ++declared_latch_.from_roster;
                } else {
                    ++declared_latch_.own_rule;
                }
            }
        }
        // Cells the declaration said nothing about keep the station's own rule, and
        // are counted so the two populations add up to the station's latch.
        for (const auto& [key, sig] : chosen) {
            (void)sig;
            if (!handled.count(key)) ++declared_latch_.own_rule;
        }
    }

    std::vector<PppModel::CanonicalSignal> PppModel::exportCanonicalSignals() const {
        std::vector<CanonicalSignal> out;
        for (const auto& [cons, per_band] : constellation_canonical_)
            for (const auto& [key, sig] : per_band)
                out.push_back(CanonicalSignal{ cons, key.first, key.second, sig });
        return out;
    }

    std::size_t PppModel::restoreCanonicalSignals(
            const std::vector<CanonicalSignal>& entries) {
        std::size_t n = 0;
        std::set<std::string> touched;
        for (const auto& e : entries) {
            if (e.constellation.empty() || e.band.empty() || e.signal.empty()) continue;
            if (e.obs_type != 'C' && e.obs_type != 'L') continue;
            constellation_canonical_[e.constellation][{ e.band, e.obs_type }] = e.signal;
            touched.insert(e.constellation);
            ++n;
        }
        // Mark each restored constellation decided: without the mark, the guard at
        // the top of decideConstellationCanonicals re-runs the tally at the first
        // processed epoch and overwrites the restored entries.
        for (const auto& cons : touched) {
            constellation_canonical_decided_.insert(cons);
            constellation_canonical_audit_pending_.insert(cons);
            std::string summary;
            for (const auto& [key, sig] : constellation_canonical_[cons]) {
                if (!summary.empty()) summary += ", ";
                summary += sig + "(" + key.first + std::string(1, key.second) + ")";
            }
            log_manager_.getLogger("main_log")->info(
                "Canonical signals restored for {} station '{}' (from the snapshot, "
                "no satellite tally): {}",
                cons, canonical_trace_label_.empty() ? "-" : canonical_trace_label_,
                summary);
        }
        return n;
    }

    bool PppModel::computeModels(const std::vector<RinexObservation>& observations,
                                 PppState& state,
                                 std::vector<PppObservation>& modeled_obs,
                                 std::map<std::string, DropReason>* drop_reasons) {
        // Record a drop reason for `sat` if the caller supplied a map.  If the
        // same sat fails on a different signal later in the loop, the last reason
        // wins: callers read the map as the final disposition of each sat for
        // this epoch.
        auto note_drop = [&](const std::string& sat, DropReason r) {
            if (drop_reasons && !sat.empty() && r != DropReason::None) {
                (*drop_reasons)[sat] = r;
            }
        };
        if (observations.empty()) {
            log_manager_.getLogger("ppp_model")->error("No observations provided for PPP modeling");
            return false;
        }

        // Decide the canonical (band, type) → signal map for each constellation
        // at the first epoch it is seen, from how many of its satellites observe
        // each priority-list signal (under the default count rule the
        // most-observed signal wins).  Later epochs skip decided constellations.
        // See the cache member doc for the rationale.
        decideConstellationCanonicals(observations);

        modeled_obs.clear();
        modeled_obs.reserve(observations.size());

        for (const auto& obs : observations) {
            // --- Compute satellite geometry & corrections ---
            SatGeometry geom;
            DropReason drop_reason = DropReason::None;
            bool geom_ok = geometry_.computeSatelliteGeometry(
                obs, state,
                geom.sat_pos, geom.rx_pos, geom.elevation, geom.azimuth,
                geom.los, geom.range, geom.tropo_zhd,
                geom.sat_clock, geom.shapiro_delay, geom.sagnac_corr,
                geom.otl_corr, geom.apl_corr, geom.relativistic_corr, geom.solid_tide,
                geom.mf_w, geom.mf_d, geom.sat_vel,
                geom.broadcast_only,
                drop_reason);

            if (!geom_ok) {
                log_manager_.getLogger("ppp_model")->debug("Geometry failed for {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                note_drop(obs.satellite, drop_reason);
                continue;
            }

            // --- Populate base PppObservation fields ---
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements.toStdMap();
            ppp_obs.variances = obs.variances.toStdMap();
            ppp_obs.lli_flags = obs.lli_flags.toStdMap();

            ppp_obs.sat_pos = geom.sat_pos;
            ppp_obs.sat_vel = geom.sat_vel;
            ppp_obs.rx_pos_linearization = geom.rx_pos;
            ppp_obs.elevation = geom.elevation;
            ppp_obs.azimuth = geom.azimuth;
            ppp_obs.los = geom.los;
            ppp_obs.range = geom.range;
            ppp_obs.mf_w = geom.mf_w;
            ppp_obs.mf_d = geom.mf_d;

            log_manager_.getLogger("ppp_model")->trace(
                "Stored geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, azimuth={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow,
                geom.sat_pos.x(), geom.sat_pos.y(), geom.sat_pos.z(),
                geom.elevation, geom.azimuth,
                geom.los.x(), geom.los.y(), geom.los.z(), geom.range);

            // --- Validate system ---
            std::string system_str = PppUtils::getSystemString(obs.system);
            if (system_str == "Unknown") {
                log_manager_.getLogger("ppp_model")->warn("Unknown system for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                note_drop(obs.satellite, DropReason::NoModel);
                continue;
            }

            auto obs_types = selectObservationTypes(obs.measurements, obs.system, system_str);
            if (obs_types.empty()) {
                log_manager_.getLogger("ppp_model")->warn("No valid observation types for system {} for satellite {}", system_str, obs.satellite);
                note_drop(obs.satellite, DropReason::NoModel);
                continue;
            }

            // --- Signal selection ---
            // Failure here (the E11/E12 case) means the sat passed geometry (above
            // the mask, with products and observations) but no usable
            // (BIA-covered, ATX-covered, canonical-band) signal pair could be
            // assembled for positioning.  Reported as NoModel.
            std::vector<std::pair<std::string, std::string>> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            // The companion codes and second phases
            // this call adds, if any, are recorded on the observation so
            // downstream consumers can tell those rows from canonical ones (the
            // Melbourne-Wubbena guard, R4; the sigma scale, R5; the cell telemetry,
            // R7).  The sets stay empty unless network.second_code_channel,
            // network.second_phase_channel or channel_mode "all" applies on the
            // network path.
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type,
                               &ppp_obs.companion_codes, &ppp_obs.second_phases)) {
                note_drop(obs.satellite, DropReason::NoModel);
                continue;
            }

            // --- Process observation ---
            if (config_.isUncombinedEnabled()) {
                // Build the set of signal keys that were actually selected for
                // positioning so that observed only contains those signals.
                // The KF measurement model must not see unselected bands.
                std::set<std::string> selected_keys;
                for (const auto& sp : selected_signals) {
                    if (!sp.first.empty())  selected_keys.insert(sp.first);
                    if (!sp.second.empty()) selected_keys.insert(sp.second);
                }

                // Keep every raw RINEX code and phase measurement in raw_observed,
                // whatever the signal_mode.  The MW cycle-slip detector forms all
                // dual-frequency MW pairs from raw_observed, so multi-frequency
                // slip detection (e.g. L1/L2/L5) works even when signal_mode
                // restricts the KF to two frequencies.
                for (const auto& [key, val] : ppp_obs.observed) {
                    if (key.size() >= 2 && (key[0] == 'C' || key[0] == 'L') &&
                        key.find('_') == std::string::npos) {
                        ppp_obs.raw_observed[key] = val;
                    }
                }

                if (!processUncombinedObservation(obs, state, ppp_obs, system_str, selected_signals, geom)) {
                    note_drop(obs.satellite, DropReason::NoModel);
                    continue;
                }

                // Strip observed of any raw RINEX keys that were not selected,
                // so the KF measurement model only sees signals that match the
                // configured signal_mode for positioning.
                for (auto it = ppp_obs.observed.begin(); it != ppp_obs.observed.end(); ) {
                    const auto& k = it->first;
                    if (k.size() >= 2 && (k[0] == 'C' || k[0] == 'L') &&
                        k.find('_') == std::string::npos &&
                        !selected_keys.count(k)) {
                        it = ppp_obs.observed.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, geom)) {
                    note_drop(obs.satellite, DropReason::NoModel);
                    continue;
                }
            }

            // --- Model Doppler observables (range-rate in m/s) ---
            if (config_.estimateVelocity()) {
                const double range_rate_sat = geom.los.dot(geom.sat_vel);
                for (const auto& [key, val] : obs.measurements) {
                    if (key.empty() || key[0] != 'D') continue;
                    const double wavelength = ppp_utils_.getWavelength(obs.system, key, obs.satellite);
                    if (wavelength <= 0.0) continue;

                    // Convert RINEX Doppler (Hz) to range-rate (m/s).
                    // Positive Doppler in RINEX = approaching = decreasing range.
                    const double observed_rr = -val * wavelength;

                    ppp_obs.observed[key] = observed_rr;
                    ppp_obs.modeled[key] = range_rate_sat;

                    // Variance: default Doppler variance converted to (m/s)^2.
                    const auto& n = config_.getDefaultMeasurementNoise();
                    const double default_var = (n.doppler_std_m * wavelength) * (n.doppler_std_m * wavelength);
                    const double fallback_var = obs.variances.count(key) ? obs.variances.at(key) * wavelength * wavelength : default_var;
                    const double zenith_var = stochastic_model_.computeVariance(key, obs.measurements, 90.0, fallback_var);
                    const double var = stochastic_model_.computeVariance(key, obs.measurements, geom.elevation, fallback_var);
                    ppp_obs.variances[key] = var;
                    ppp_obs.zenith_variances[key] = zenith_var;

                    log_manager_.getLogger("ppp_model")->trace(
                        "Doppler {} for {}: obs_rr={:.4f} m/s, mod_rr={:.4f} m/s, var={:.6f}",
                        key, obs.satellite, observed_rr, range_rate_sat, var);
                }
            }

            // --- Broadcast-grade gate: code-only ---
            // With a metre-level range model (broadcast clock, no precise CLK, no
            // SSR clock) the float ambiguity cannot be separated from the clock and
            // orbit error, so phase cannot improve the position, and modelling it
            // at phase noise makes the filter inconsistent.  Drop every phase
            // observable (raw "L1C" and IF "L1C_L2W") so the KF sees code only and
            // creates no ambiguity states.  raw_observed stays intact for the MW
            // slip detector's bookkeeping.
            ppp_obs.broadcast_only = geom.broadcast_only;
            // Ephemeris freshness for the real-time BSSD pivot selector (inert /
            // 0 in post-processing, where the live store is empty so this returns
            // nullopt).  Signed age (obs epoch − toe); the selector takes |age| as
            // the broadcast extrapolation magnitude.
            if (auto age = LiveEphemerisStore::getInstance().toeAgeSeconds(obs.satellite, obs.time))
                ppp_obs.eph_toe_age_s = *age;
            if (geom.broadcast_only && !broadcast_phase_gate_suppressed_) {
                for (std::map<std::string, double>* m :
                         { &ppp_obs.observed, &ppp_obs.modeled,
                           &ppp_obs.variances, &ppp_obs.zenith_variances }) {
                    for (auto it = m->begin(); it != m->end(); ) {
                        if (!it->first.empty() && it->first[0] == 'L') it = m->erase(it);
                        else ++it;
                    }
                }
                if (sats_phase_gated_.insert(obs.satellite).second) {
                    log_manager_.getLogger("ppp_model")->info(
                        "Broadcast-grade clock for {} — using code-only (carrier phase "
                        "excluded; phase at mm precision is inconsistent with metre-level "
                        "broadcast products)", obs.satellite);
                }
            } else if (geom.broadcast_only && broadcast_phase_gate_suppressed_) {
                // brdc_rts network producer: the satellite's a-priori clock is
                // broadcast-grade, but the network estimates the clock (Δδtˢ
                // against that a-priori), so carrier phase is informative and is
                // kept; the code-only erase above is skipped.  Logged once per
                // satellite, so the mode is visible without a line per epoch.
                if (sats_phase_retained_.insert(obs.satellite).second) {
                    log_manager_.getLogger("ppp_model")->info(
                        "Broadcast-grade clock for {} — carrier phase RETAINED (brdc_rts "
                        "network producer estimates the satellite clock; phase gate "
                        "suppressed)", obs.satellite);
                }
            }

            if (!ppp_obs.modeled.empty()) {
                modeled_obs.push_back(ppp_obs);
            } else {
                // Reached when selectSignals succeeded but neither the uncombined
                // nor the iono-free path populated ppp_obs.modeled.  Dominant case
                // is the PPP single-signal fallback: selectIonoFreePairs fails,
                // selectSignals picks single signals, and with no
                // processSingleSignalObservation the sat falls through empty.
                // Report NoModel with context instead of dropping the sat silently,
                // so the sat-status trace carries the reason.
                note_drop(obs.satellite, DropReason::NoModel);
                log_manager_.getLogger("ppp_model")->debug(
                    "Sat {} {} dropped with empty modeled set: "
                    "is_iono_free={}, uncombined={}, comb_type={}, "
                    "n_selected={} -- check selectIonoFreePairs / "
                    "frequency_mode interaction",
                    obs.satellite, system_str,
                    is_iono_free, config_.isUncombinedEnabled(),
                    comb_type, selected_signals.size());
            }
        }

        if (modeled_obs.empty()) {
            log_manager_.getLogger("ppp_model")->error("No valid PPP observations modeled");
            return false;
        }
        log_manager_.getLogger("ppp_model")->info("Modeled {} PPP observations for mode {}", modeled_obs.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    // --- Signal selection: decomposed helpers ---

    bool PppModel::buildFrequencyBandMap(
        const ObsValueMap<double>& measurements,
        const std::string& system_str,
        const std::vector<std::string>& signals,
        std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        std::vector<std::string>& freq_bands) const {

        freq_band_to_signal.clear();
        freq_bands.clear();

        const auto& system_to_bands = getSystemToBands();
        auto bands_it = system_to_bands.find(system_str);
        if (bands_it == system_to_bands.end()) return false;

        const auto& band_map = getBandMap();
        if (band_map.find(system_str) == band_map.end()) return false;

        // Which frequency bands this system may use.
        //
        // On the uncombined path with estimation.lock_signals_to_clock_ref (off by
        // default), only the two bands matching the BIA satellite-clock reference
        // observables are admitted, which avoids extra IFB states and keeps the
        // measurement model consistent with the clock/bias products.  Without the
        // lock, or with no clock-ref info for this system, both the uncombined and
        // the combined path use the band list of the configured frequency_mode
        // (legacy key signal_mode).
        std::vector<std::string> clock_ref_bands;  // populated only when lock is active
        const std::vector<std::string>* allowed_bands_ptr = nullptr;
        // When the clock-ref lock is active, maps each RINEX band digit
        // (e.g. '1', '2') to the required tracking attribute (e.g. 'W')
        // so that only signals matching the clock-reference observables
        // are accepted.  Empty when the lock is inactive.
        std::map<char, char> clock_ref_attr;

        const bool use_clock_ref_lock = config_.isUncombinedEnabled()
                                     && config_.lockSignalsToClockRef();

        if (use_clock_ref_lock) {
            // Try to derive the two allowed bands from the BIA clock-reference signals
            const char sys_char = systemNameToClockRefChar(system_str);
            const auto& clock_refs = ProductCache::getInstance().getClockRefSignals();
            auto crit = clock_refs.find(sys_char);
            if (crit != clock_refs.end()) {
                const auto& ref = crit->second;
                // Build RINEX prefix from the clock-ref signal (e.g. "C1W" → "C1")
                // and look up the band name via the system_to_bands table.
                // Also extract the tracking attribute per band digit so that
                // only signals with a matching attribute are accepted later.
                for (const auto* sig : { &ref.sig1, &ref.sig2 }) {
                    if (sig->size() < 2) continue;
                    const std::string prefix = sig->substr(0, 2); // e.g. "C1"
                    for (const auto& [pat, band] : bands_it->second) {
                        if (pat == prefix) {
                            if (std::find(clock_ref_bands.begin(), clock_ref_bands.end(), band)
                                == clock_ref_bands.end()) {
                                clock_ref_bands.push_back(band);
                            }
                            break;
                        }
                    }
                    // Record required tracking attribute for this band digit
                    // (e.g. "C1W" → band digit '1', attribute 'W').
                    if (sig->size() >= 3) {
                        clock_ref_attr[(*sig)[1]] = (*sig)[2];
                    }
                }
            }
            if (!clock_ref_bands.empty()) {
                allowed_bands_ptr = &clock_ref_bands;
                log_manager_.getLogger("ppp_model")->trace(
                    "Clock-ref lock active for {}: allowed bands = [{}], attributes = [{}]",
                    system_str, [&]{
                        std::string s;
                        for (size_t i = 0; i < clock_ref_bands.size(); ++i) {
                            if (i) s += ", ";
                            s += clock_ref_bands[i];
                        }
                        return s;
                    }(), [&]{
                        std::string s;
                        for (auto it = clock_ref_attr.begin(); it != clock_ref_attr.end(); ++it) {
                            if (it != clock_ref_attr.begin()) s += ", ";
                            s += it->first;
                            s += ':';
                            s += it->second;
                        }
                        return s;
                    }());
            }
        }

        // Fall back to the static band map when clock-ref lock is not active
        // or when no clock-ref info was available for this system.
        // Respect the configured signal_mode so the user can control how many
        // frequency bands enter the filter (dual, triple or all).
        std::vector<std::string> resolved_bands;  // owns the Independent-pruned set when the band-map fallback is used
        if (!allowed_bands_ptr) {
            resolved_bands = activeFrequencyBands(system_str, config_.getFrequencyMode(), measurements);
            allowed_bands_ptr = &resolved_bands;
        }

        const auto& allowed_bands = *allowed_bands_ptr;

        // Populate freq_band_to_signal / freq_bands from the priority-ordered
        // signal list, for every signal passing the band and attribute filters.
        //
        // Where the constellation's canonical map was decided at the first epoch
        // (the common case) use it directly: it reflects what the receiver
        // actually tracks and stays stable, so a brief low-elevation dropout does
        // not switch the KF to another tracking attribute and seed a persistent
        // inter-channel-bias-contaminated ambiguity.  A canonical unobserved this
        // epoch simply means selectUncombinedSignals skips the band for this sat.
        auto cit = constellation_canonical_.find(system_str);
        if (cit != constellation_canonical_.end() && !cit->second.empty()) {
            auto applyCached = [&](const std::map<char, char>& attr_filter) {
                for (const auto& [key, sig] : cit->second) {
                    const std::string& band  = key.first;
                    const char         otype = key.second;
                    if (std::find(allowed_bands.begin(), allowed_bands.end(), band)
                        == allowed_bands.end()) continue;
                    // Honour the clock-ref attribute filter when active.
                    if (!attr_filter.empty() && sig.size() >= 3) {
                        auto ait = attr_filter.find(sig[1]);
                        if (ait != attr_filter.end() && sig[2] != ait->second)
                            continue;
                    }
                    if (freq_band_to_signal.find(band) == freq_band_to_signal.end()) {
                        freq_band_to_signal[band] = { "", "" };
                        freq_bands.push_back(band);
                    }
                    auto& [best_code, best_phase] = freq_band_to_signal[band];
                    if (otype == 'C') best_code  = sig;
                    if (otype == 'L') best_phase = sig;
                }
            };
            applyCached(clock_ref_attr);
            if (freq_bands.empty() && !clock_ref_attr.empty()) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Clock-ref attribute lock rejected cached canonical for {}; "
                    "relaxing to band-only lock", system_str);
                static const std::map<char, char> no_attr;
                applyCached(no_attr);
            }
            return !freq_bands.empty();
        }

        // Fallback when the cache is empty for this constellation
        // (decideConstellationCanonicals didn't run, or found no
        // observations for it).  Use the original priority-list walk
        // with per-epoch measurement filtering as a safety net.
        auto populate = [&](const std::map<char, char>& attr_filter) {
            for (const auto& sig : signals) {
                if (!measurements.count(sig)) continue;
                if (!signalAdmitted(system_str, sig)) continue;   // network coverage gate
                // When an attribute filter is active, reject signals whose
                // tracking attribute does not match the clock-reference
                // observable for the corresponding band digit.
                // Signal format is "{type}{band}{attr}" (e.g. "C1W").
                if (!attr_filter.empty() && sig.size() >= 3) {
                    auto ait = attr_filter.find(sig[1]);
                    if (ait != attr_filter.end() && sig[2] != ait->second)
                        continue;
                }
                for (const auto& [pattern, band] : bands_it->second) {
                    if (std::find(allowed_bands.begin(), allowed_bands.end(), band) == allowed_bands.end())
                        continue;
                    if (sig.find(pattern) != 0) continue;

                    if (freq_band_to_signal.find(band) == freq_band_to_signal.end()) {
                        freq_band_to_signal[band] = { "", "" };
                        freq_bands.push_back(band);
                    }
                    auto& [best_code, best_phase] = freq_band_to_signal[band];
                    if (sig[0] == 'C' && (best_code.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), best_code))) {
                        best_code = sig;
                    }
                    if (sig[0] == 'L' && (best_phase.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), best_phase))) {
                        best_phase = sig;
                    }
                }
            }
        };

        // First pass: strict attribute match against clock-ref signals.
        populate(clock_ref_attr);

        // If the strict attribute filter produced no results (the receiver
        // does not track the exact clock-ref tracking mode), retry without
        // the attribute constraint so the system is not excluded entirely.
        // The band restriction is still enforced.
        if (freq_bands.empty() && !clock_ref_attr.empty()) {
            log_manager_.getLogger("ppp_model")->debug(
                "Clock-ref attribute lock produced no signals for {}; "
                "relaxing to band-only lock", system_str);
            static const std::map<char, char> no_attr;
            populate(no_attr);
        }

        return !freq_bands.empty();
    }

    bool PppModel::selectUncombinedSignals(
        const RinexObservation& obs, const std::string& system_str,
        const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        const std::vector<std::string>& freq_bands,
        std::vector<std::pair<std::string, std::string>>& selected_signals,
        std::set<std::string>* companion_out,
        std::set<std::string>* second_phase_out) const {

        selected_signals.clear();
        for (const auto& band : freq_bands) {
            auto band_it = freq_band_to_signal.find(band);
            if (band_it == freq_band_to_signal.end()) continue;
            const auto& [best_code, best_phase] = band_it->second;
            bool found_code = false, found_phase = false;
            if (!best_code.empty() && obs.measurements.count(best_code)) {
                selected_signals.emplace_back(best_code, "");
                found_code = true;
            }
            if (!best_phase.empty() && obs.measurements.count(best_phase)) {
                selected_signals.emplace_back(best_phase, "");
                found_phase = true;
            }
            log_manager_.getLogger("ppp_model")->trace(
                "Uncombined band {} for {} {}: code={} ({}), phase={} ({})",
                band, obs.satellite, system_str,
                best_code, found_code ? "ok" : "missing",
                best_phase, found_phase ? "ok" : "missing");
        }

        // channel_mode "all": add every other tracking-mode channel on the
        // already-selected bands.  buildFrequencyBandMap picks one code and one
        // phase per band; this loop adds the remaining channels (e.g. L1W beside
        // L1C, or C1W beside C1C), each as its own KF row and each phase channel
        // with its own ambiguity state.  Channels whose band is not in
        // @p freq_bands are skipped: only the channel dimension widens, not the
        // band dimension.  No-op for channel_mode "single", which asks for the
        // canonical priority-best selection.
        const bool channel_all = (config_.getChannelMode() == ChannelMode::All);
        // On the network path the widening is the single entry for extra channels
        // and subsumes the second code and second phase channels.  Each
        // extra channel it adds is reported through the same two sets those channels
        // fill, so every downstream reader (the receiver inter-signal code state, the
        // two sigma scales, the Melbourne-Wubbena guard, the cell telemetry and both
        // edges of invariant #7) works unchanged and one predicate serves both
        // mechanisms.  A channel is non-canonical when the band map did not select it
        // for its band, the same test the second code channel's receiver side applies.
        const bool subsume =
            channel_all && config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network;
        if (channel_all) {
            std::set<std::string> already;
            for (const auto& sp : selected_signals) {
                if (!sp.first.empty())  already.insert(sp.first);
                if (!sp.second.empty()) already.insert(sp.second);
            }
            const auto& system_to_bands = getSystemToBands();
            auto stb_it = system_to_bands.find(system_str);
            if (stb_it != system_to_bands.end()) {
                for (const auto& [sig, val] : obs.measurements) {
                    if (sig.empty()) continue;
                    const char prefix = sig[0];
                    if (prefix != 'C' && prefix != 'L') continue;
                    if (already.count(sig)) continue;
                    // Network coverage gate: never augment with a non-admitted channel.
                    if (!signalAdmitted(system_str, sig)) continue;
                    for (const auto& [pattern, band] : stb_it->second) {
                        if (sig.find(pattern) != 0) continue;
                        if (std::find(freq_bands.begin(), freq_bands.end(), band)
                            == freq_bands.end()) continue;
                        selected_signals.emplace_back(sig, "");
                        already.insert(sig);
                        if (subsume) {
                            // The added row is a second channel only when the band's
                            // canonical row of the same type is present this epoch.
                            // With it absent this is the fallback case, which the
                            // per-epoch sweep counts and the receiver side routes by
                            // the same signal-against-latch test, so the two agree on
                            // which state a row belongs to.
                            auto bit = freq_band_to_signal.find(band);
                            std::string canon;
                            if (bit != freq_band_to_signal.end())
                                canon = (prefix == 'C') ? bit->second.first
                                                        : bit->second.second;
                            const bool canon_here =
                                !canon.empty() && obs.measurements.count(canon) != 0;
                            if (canon_here && prefix == 'C' && companion_out)
                                companion_out->insert(sig);
                            if (canon_here && prefix == 'L' && second_phase_out)
                                second_phase_out->insert(sig);
                        }
                        log_manager_.getLogger("ppp_model")->trace(
                            "Uncombined band {} for {} {}: extra channel {} added (channel_mode=All)",
                            band, obs.satellite, system_str, sig);
                        break;
                    }
                }
            }
        }

        // --- Second code channel ----------------------------------
        // One canonical code per station and band means a satellite's C1W code bias
        // is supported only by the stations that latched C1W, so the C1C and C1W
        // satellite families compete for the same roster instead of sharing it.
        // Add, as code-only rows, every allow-listed band-1 code this station
        // reports for this satellite besides the one the band map already selected.
        // See docs/refound/SCC_second_code_channel_2026-09-21.md (R1).
        //
        // Why not the channel_mode=All widening above: it also adds the phase
        // channels (each with its own ambiguity and phase-bias column) and every
        // admitted tracking attribute, not only the allow-listed ones, while the second code channel
        // adds one extra code row and nothing else.  With channel_mode all the
        // loader keeps this knob armed and the widening subsumes it (last paragraph).
        //
        // Why band 1 only: a band-2 companion would make the network.sat_isc_state
        // bridging roster non-empty for the first time; the loader refuses a band-2
        // allow-list entry and the spec defers it (§4).  The allow-list is validated
        // at load, so membership here already implies a 3-character code type on
        // band 1.
        //
        // The added key goes into selected_signals because the measurement model
        // needs all three of observed / modeled / variances to accept a row, and it
        // is also reported through @p companion_out: that set is what makes a
        // companion row distinguishable downstream, and R4's Melbourne-Wubbena
        // guard and R7's cell telemetry both read it.  raw_observed is filled in
        // computeModels from every raw key and is untouched by this block.
        //
        // Runs only on the network path with network.second_code_channel armed;
        // otherwise the block is skipped whole and the selection is byte-identical.
        //
        // Subsumed under `channel_mode: all`: the widening
        // above has already added every admitted code of every selected band and
        // reported it through @p companion_out, so running this block as well would
        // append a duplicate row for each allow-listed code.  There the allow-list
        // (network.second_code_signals) is ignored by design, and only the sigma
        // scale (network.second_code_sigma_scale) still applies.
        if (config_.networkSecondCodeChannel() && !subsume
            && config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) {
            for (const auto& band : freq_bands) {
                const auto band_it = freq_band_to_signal.find(band);
                if (band_it == freq_band_to_signal.end()) continue;
                const std::string& best_code = band_it->second.first;
                // Band 1 is the supported scope, tested on the signal's own RINEX band
                // digit: the band name is constellation-specific ("L1" on GPS, "E1" on
                // Galileo, "B1C" on BeiDou), while the allow-list and the receiver-side
                // latch test both use the digit.
                if (best_code.size() < 2 || best_code[1] != '1') continue;
                // The band's selected code must be present this epoch, not only
                // named: without it there is no canonical row for the companion to
                // accompany, and a lone second code is the fallback case, which the
                // receiver side handles and counts separately.
                if (!obs.measurements.count(best_code)) continue;
                // What this station reports for this satellite, filtered by the
                // network coverage gate (the same gate the band map and the
                // channel_mode widening apply), so a non-admitted channel cannot
                // enter through this path either.
                std::set<std::string> reported;
                for (const auto& [sig, val] : obs.measurements) {
                    (void)val;
                    if (sig.size() != 3 || sig[0] != 'C' || sig[1] != '1') continue;
                    if (!signalAdmitted(system_str, sig)) continue;
                    reported.insert(sig);
                }
                for (const auto& sig : secondCodeCompanions(
                         reported, best_code, config_.networkSecondCodeSignals())) {
                    selected_signals.emplace_back(sig, "");
                    if (companion_out) companion_out->insert(sig);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Uncombined band {} for {} {}: companion code {} added beside "
                        "the latched {}",
                        band, obs.satellite, system_str, sig, best_code);
                }
            }
        }

        // --- Second phase channel --------------------------
        // The phase counterpart of the block above.  One carrier phase per station
        // and band means a satellite's L1W phase bias rests only on the stations that
        // latched L1W, which leaves it thinly supported (the coverage figures,
        // including the coverage-gate drop edges, are in docs/refound/SID_signal_identity_2026-09-22.md).  Add, as
        // ordinary phase rows, every allow-listed band-1 phase this station reports
        // for this satellite besides the one the latch already selected.  See
        // docs/refound/SID_signal_identity_2026-09-22.md section 4 (R1).
        //
        // What the added key becomes: everything downstream of `selected_signals` is
        // per signal.  The measurement model allocates an ordinary float ambiguity
        // "<sat>_<signal>" and the shared per-(satellite, signal) phase-bias state of
        // that signal's existing family, so an L1W row at an L1C station joins the
        // L1W family instead of creating one.  Nothing here allocates a receiver
        // inter-signal phase state, and none may be added: a float ambiguity already
        // absorbs an arbitrary constant per arc, so such a state would sit in the
        // null space of the normal equations (the audit's risk 5).  This is the one
        // structural difference from the second code channel, whose code rows have no such absorber.
        //
        // Why not the channel_mode=All widening above: it widens every selected band
        // to every admitted tracking attribute, code side included, while phase 6a
        // adds a bounded, allow-listed band-1 phase set.  With channel_mode all the
        // loader keeps this knob armed and the widening subsumes it (last paragraph).
        //
        // Why band 1 only: band 2 is left to phase 6b, which lifts the channel_mode
        // interlock instead of working around it.  The allow-list is validated at
        // load, so membership here already implies a 3-character phase type on
        // band 1.
        //
        // The fallback case is not added.  An allow-listed phase with no latched
        // phase of that band beside it is not a second phase: the latch defines what
        // "second" means, and adding the row would silently make it the band's only
        // phase.  The serial PASS-A sweep counts those occurrences
        // (`spc_fallback_rows`) and does nothing else with them.
        //
        // Runs only on the network path with network.second_phase_channel armed;
        // otherwise the block is skipped whole and the selection is byte-identical.
        //
        // Subsumed under `channel_mode: all` for the same
        // reason as the block above: the widening has already added every admitted
        // phase of every selected band and reported it through @p second_phase_out.
        if (config_.networkSecondPhaseChannel() && !subsume
            && config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) {
            for (const auto& band : freq_bands) {
                const auto band_it = freq_band_to_signal.find(band);
                if (band_it == freq_band_to_signal.end()) continue;
                const std::string& best_phase = band_it->second.second;
                // Band 1 is this phase's scope, tested on the signal's own RINEX band
                // digit: the band name is constellation-specific ("L1" on GPS, "E1" on
                // Galileo), while the allow-list uses the digit.
                if (best_phase.size() < 2 || best_phase[1] != '1') continue;
                // The band's latched phase must be present this epoch, not only
                // named; without it this is the fallback case described above.
                if (!obs.measurements.count(best_phase)) continue;
                // What this station reports for this satellite, filtered by the network
                // coverage gate (the same gate the band map and the channel_mode
                // widening apply), so a non-admitted channel cannot enter here either.
                std::set<std::string> reported;
                for (const auto& [sig, val] : obs.measurements) {
                    (void)val;
                    if (sig.size() != 3 || sig[0] != 'L' || sig[1] != '1') continue;
                    if (!signalAdmitted(system_str, sig)) continue;
                    reported.insert(sig);
                }
                for (const auto& sig : secondPhaseCompanions(
                         reported, best_phase, config_.networkSecondPhaseSignals())) {
                    selected_signals.emplace_back(sig, "");
                    if (second_phase_out) second_phase_out->insert(sig);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Uncombined band {} for {} {}: second phase {} added beside the "
                        "latched {} (second phase channel)",
                        band, obs.satellite, system_str, sig, best_phase);
                }
            }
        }
        return !selected_signals.empty();
    }

    bool PppModel::selectIonoFreePairs(
        const RinexObservation& obs, const std::string& system_str,
        const std::vector<std::string>& signals,
        const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
        const std::vector<std::string>& freq_bands,
        std::vector<std::pair<std::string, std::string>>& selected_signals) const {

        selected_signals.clear();
        const auto& band_map = getBandMap();
        if (!band_map.count(system_str)) return false;

        auto target_bands = activeFrequencyBands(system_str, config_.getFrequencyMode(), obs.measurements);
        std::vector<std::pair<std::string, std::string>> candidate_pairs;

        // Reject IF pairs whose noise amplification factor exceeds this cap.
        // The IF combination noise is sqrt(α₁² + α₂²)·σ where α_i = f_i²/(f_i²−f_j²);
        // typical wide pairs (GPS L1/L2, BeiDou B1I/B3) sit at K ≈ 3, but pairs
        // separated by only ~15 MHz (BeiDou B1C/B1I, B2I/B2b/B2ab variants) yield
        // K ≈ 50–80, which turns ~0.3 m code noise into ~15–25 m IF noise.  The
        // cap is checked once per band pair below; code and phase share the same
        // K because the band frequency does not depend on the tracking attribute.
        constexpr double kMaxIfNoiseFactor = 10.0;
        auto if_noise_factor = [&](const std::string& sig) -> double {
            if (sig.empty()) return std::numeric_limits<double>::quiet_NaN();
            return ppp_utils_.getFrequency(sig, obs.system, obs.satellite);
        };

        for (size_t i = 0; i < target_bands.size(); ++i) {
            for (size_t j = i + 1; j < target_bands.size(); ++j) {
                const auto& b1 = target_bands[i];
                const auto& b2 = target_bands[j];

                // Anchor policy: in triple, all and independent modes, at least one
                // band must be the anchor (the first band).
                const auto sm = config_.getFrequencyMode();
                const bool use_anchor = (sm == FrequencyMode::Triple || sm == FrequencyMode::All || sm == FrequencyMode::Independent) && target_bands.size() >= 2;
                if (use_anchor && b1 != target_bands.front() && b2 != target_bands.front()) {
                    log_manager_.getLogger("ppp_model")->trace(
                        "Skipping non-anchor band pair for {} {}: {} / {} (anchor={})",
                        obs.satellite, system_str, b1, b2, target_bands.front());
                    continue;
                }

                if (!freq_band_to_signal.count(b1) || !freq_band_to_signal.count(b2)) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Missing band signals for {} {}: band {} present={}, band {} present={}, signal_mode={}",
                        obs.satellite, system_str, b1, freq_band_to_signal.count(b1),
                        b2, freq_band_to_signal.count(b2), frequencyModeLabel(sm));
                    continue;
                }

                const auto& s1 = freq_band_to_signal.at(b1);
                const auto& s2 = freq_band_to_signal.at(b2);

                // Frequency-separation check.  Use the code signal if available,
                // else the phase signal; frequency is band-level, so K is
                // identical for both.
                const std::string check_sig1 = !s1.first.empty() ? s1.first : s1.second;
                const std::string check_sig2 = !s2.first.empty() ? s2.first : s2.second;
                if (!check_sig1.empty() && !check_sig2.empty()) {
                    const double f1 = if_noise_factor(check_sig1);
                    const double f2 = if_noise_factor(check_sig2);
                    if (std::isfinite(f1) && std::isfinite(f2) && f1 > 0 && f2 > 0) {
                        const double f1_sq = f1 * f1;
                        const double f2_sq = f2 * f2;
                        const double denom = f1_sq - f2_sq;
                        const double K = (std::abs(denom) > 1e-3 * f1_sq)
                            ? std::sqrt(f1_sq * f1_sq + f2_sq * f2_sq) / std::abs(denom)
                            : std::numeric_limits<double>::infinity();
                        if (K > kMaxIfNoiseFactor) {
                            log_manager_.getLogger("ppp_model")->debug(
                                "Skipping IF pair for {} {}: bands {}/{} too close — "
                                "noise amplification K={:.1f} exceeds cap {:.1f} "
                                "(f1={:.3f} MHz, f2={:.3f} MHz)",
                                obs.satellite, system_str, b1, b2, K, kMaxIfNoiseFactor,
                                f1 * 1e-6, f2 * 1e-6);
                            continue;
                        }
                    }
                }

                // Per-satellite availability check.  freq_band_to_signal is
                // constellation-wide, so under mixed L5 capability (say 4 of 11
                // GPS sats tracking L5Q) the canonical locks in L5Q and every
                // (L1, L5) candidate names a signal the other sats never measure.
                // Guarding pair construction against the per-sat measurement set
                // gives non-L5 sats only (L1, L2) and L5-capable sats both.
                // processIonoFreeObservation also skips a pair whose measurements
                // are missing, as a second guard.
                const bool code_ok  = !s1.first.empty()  && !s2.first.empty()
                                   && obs.measurements.count(s1.first)
                                   && obs.measurements.count(s2.first);
                const bool phase_ok = !s1.second.empty() && !s2.second.empty()
                                   && obs.measurements.count(s1.second)
                                   && obs.measurements.count(s2.second);
                if (code_ok) {
                    candidate_pairs.emplace_back(s1.first, s2.first);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Selected iono-free code pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                        obs.satellite, system_str, s1.first, s2.first, b1, b2, frequencyModeLabel(sm));
                } else if (!s1.first.empty() && !s2.first.empty()) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Skipping iono-free code pair for {} {}: bands {}/{} "
                        "canonical {}/{} not all measured this epoch "
                        "(present={}/{}), signal_mode={}",
                        obs.satellite, system_str, b1, b2, s1.first, s2.first,
                        obs.measurements.count(s1.first),
                        obs.measurements.count(s2.first),
                        frequencyModeLabel(sm));
                }
                if (phase_ok) {
                    candidate_pairs.emplace_back(s1.second, s2.second);
                    log_manager_.getLogger("ppp_model")->trace(
                        "Selected iono-free phase pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                        obs.satellite, system_str, s1.second, s2.second, b1, b2, frequencyModeLabel(sm));
                } else if (!s1.second.empty() && !s2.second.empty()) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Skipping iono-free phase pair for {} {}: bands {}/{} "
                        "canonical {}/{} not all measured this epoch "
                        "(present={}/{}), signal_mode={}",
                        obs.satellite, system_str, b1, b2, s1.second, s2.second,
                        obs.measurements.count(s1.second),
                        obs.measurements.count(s2.second),
                        frequencyModeLabel(sm));
                } else if (s1.second.empty() || s2.second.empty()) {
                    log_manager_.getLogger("ppp_model")->debug(
                        "Missing phase signal canonical for {} {}: band {} phase={}, band {} phase={}, signal_mode={}",
                        obs.satellite, system_str, b1, s1.second, b2, s2.second, frequencyModeLabel(sm));
                }
            }
        }

        if (candidate_pairs.empty()) return false;

        // Score each pair by signal priority (lower is better).
        auto pair_score = [&](const std::pair<std::string, std::string>& p) -> int {
            auto it1 = std::find(signals.begin(), signals.end(), p.first);
            auto it2 = std::find(signals.begin(), signals.end(), p.second);
            int idx1 = (it1 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it1));
            int idx2 = (it2 == signals.end()) ? std::numeric_limits<int>::max() / 2 : static_cast<int>(std::distance(signals.begin(), it2));
            return idx1 + idx2;
        };

        std::vector<std::pair<std::string, std::string>> code_cands, phase_cands;
        for (const auto& p : candidate_pairs) {
            if (!p.first.empty() && p.first[0] == 'C') code_cands.push_back(p);
            else if (!p.first.empty() && p.first[0] == 'L') phase_cands.push_back(p);
        }

        // Sort by priority (best first) for deterministic ordering.
        // The first pair is the primary combination used by the MW slip
        // detector; additional pairs provide independent geometry.
        std::sort(code_cands.begin(), code_cands.end(),
            [&](const auto& a, const auto& b) { return pair_score(a) < pair_score(b); });
        std::sort(phase_cands.begin(), phase_cands.end(),
            [&](const auto& a, const auto& b) { return pair_score(a) < pair_score(b); });

        for (const auto& p : code_cands)  selected_signals.push_back(p);
        for (const auto& p : phase_cands) selected_signals.push_back(p);

        return !selected_signals.empty();
    }

    void PppModel::selectSingleSignalFallback(
        const RinexObservation& obs,
        const std::vector<std::string>& signals,
        std::vector<std::pair<std::string, std::string>>& selected_signals) const {

        selected_signals.clear();
        for (const auto& sig : signals) {
            if (obs.measurements.count(sig) && sig[0] == 'C') {
                selected_signals.emplace_back(sig, "");
                break;
            }
        }
        for (const auto& sig : signals) {
            if (obs.measurements.count(sig) && sig[0] == 'L') {
                selected_signals.emplace_back("", sig);
                break;
            }
        }
    }

    // ===================================================================
    // selectSignals: top-level orchestrator
    // ===================================================================

    /**
     * @brief Select candidate signal pairs for one observation.
     *
     * Uses the configured priority lists and band mappings to choose code/phase
     * pairs for iono-free combination or single/uncombined processing.
     *
     * @param obs Input observation record.
     * @param system_str GNSS system string (e.g. "GPS").
     * @param selected_signals Output (signal1, signal2) pairs; one member may be
     *        empty for single/uncombined selections.
     * @param is_iono_free Output; true when a dual or triple IF combination exists.
     * @param comb_type Output: "single", "dual", "triple" or "uncombined".
     * @param companion_out Optional; receives the companion codes the uncombined
     *        selection adds.  See selectUncombinedSignals.
     * @param second_phase_out Optional; receives the second phases the uncombined
     *        selection adds.  See selectUncombinedSignals.
     * @return true if useful signals were selected.
     */
    bool PppModel::selectSignals(const RinexObservation& obs, const std::string& system_str,
        std::vector<std::pair<std::string, std::string>>& selected_signals,
        bool& is_iono_free, std::string& comb_type,
        std::set<std::string>* companion_out,
        std::set<std::string>* second_phase_out) {

        auto it = signal_priorities_.find(system_str);
        const auto& signals = it != signal_priorities_.end() ? it->second : std::vector<std::string>{};
        if (signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Build frequency band map from available measurements.
        std::map<std::string, std::pair<std::string, std::string>> freq_band_to_signal;
        std::vector<std::string> freq_bands;
        if (!buildFrequencyBandMap(obs.measurements, system_str, signals, freq_band_to_signal, freq_bands)) {
            log_manager_.getLogger("ppp_model")->warn("No frequency band mappings for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Log available signals.
        {
            std::string available_signals;
            for (const auto& [band, sig_pair] : freq_band_to_signal) {
                available_signals += band + ":(" + (sig_pair.first.empty() ? "none" : sig_pair.first) + "," +
                    (sig_pair.second.empty() ? "none" : sig_pair.second) + ")";
                if (band != freq_bands.back()) available_signals += ", ";
            }
            // The bracketed list is the per-band code/phase map this
            // satellite's selection starts from.  The mode printed is the
            // configured frequency_mode (legacy signal_mode): band selection
            // follows it (or the clock-ref bands) directly, with no
            // promotion to All on the uncombined path.
            const char* clock_ref_note =
                (config_.isUncombinedEnabled() && config_.lockSignalsToClockRef())
                    ? " (clock-ref-locked)" : "";
            log_manager_.getLogger("ppp_model")->debug("Available signals for {} {}: [{}], signal_mode={}{}",
                obs.satellite, system_str,
                available_signals.empty() ? "none" : available_signals,
                frequencyModeLabel(config_.getFrequencyMode()), clock_ref_note);
        }

        selected_signals.clear();
        is_iono_free = freq_bands.size() >= 2 && !config_.isUncombinedEnabled();
        comb_type = freq_bands.size() >= 3 ? "triple" : freq_bands.size() == 2 ? "dual" : "single";

        if (config_.isUncombinedEnabled()) {
            if (!selectUncombinedSignals(obs, system_str, freq_band_to_signal, freq_bands,
                                         selected_signals, companion_out,
                                         second_phase_out)) {
                log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}",
                    obs.satellite, system_str, obs.time.tow);
                return false;
            }
            comb_type = "uncombined";
        } else if (is_iono_free) {
            if (!selectIonoFreePairs(obs, system_str, signals, freq_band_to_signal, freq_bands, selected_signals)) {
                // Silent-fallback gate.  No candidate (band1, band2) pair survived
                // the freq_band_to_signal, noise-factor and code/phase
                // availability checks, so this falls through to the single-signal
                // path.  PPP mode has no processSingleSignalObservation, so the
                // sat drops with an empty modeled set and shows as "NoModel".
                // Logged at info so the fallback is visible without ppp_model
                // debug logging; the per-pair rejection reasons (missing band,
                // noise-K cap, etc.) are logged at debug inside
                // selectIonoFreePairs.
                log_manager_.getLogger("ppp_model")->info(
                    "selectIonoFreePairs failed for {} {} -- falling back to "
                    "single-signal mode (freq_bands=[{}], mode={}).  This "
                    "sat will land in NoModel in the satellite-status trace; "
                    "set ppp_model log level to debug for per-pair rejection "
                    "reasons.",
                    obs.satellite, system_str,
                    [&]{
                        std::string s;
                        for (size_t i = 0; i < freq_bands.size(); ++i) {
                            if (i) s += ",";
                            s += freq_bands[i];
                        }
                        return s;
                    }(),
                    frequencyModeLabel(config_.getFrequencyMode()));
                comb_type = "single";
                is_iono_free = false;
            }
        }

        if (!is_iono_free && selected_signals.empty()) {
            selectSingleSignalFallback(obs, signals, selected_signals);
            comb_type = "single";
        }

        if (selected_signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}", obs.satellite, system_str, obs.time.tow);
            return false;
        }

        // Log final selection.
        std::string signal_list;
        for (size_t i = 0; i < selected_signals.size(); ++i) {
            signal_list += (selected_signals[i].first.empty() ? "" : selected_signals[i].first) +
                (selected_signals[i].second.empty() ? "" : (selected_signals[i].first.empty() ? "" : "/") + selected_signals[i].second);
            if (i < selected_signals.size() - 1) signal_list += ",";
        }
        log_manager_.getLogger("ppp_model")->trace("Selected signals for {} {}: [{}], comb_type={}, is_iono_free={}, mode={}",
            obs.satellite, system_str, signal_list.empty() ? "none" : signal_list, comb_type, is_iono_free,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        return true;
    }

    // --- Shared correction helpers ---

    double PppModel::computePoleTideProjection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time,
        const Eigen::Vector3d& los) const {
        if (!config_.getModelCorrections().enable_pole_tide)
            return 0.0;
        const Eigen::Vector3d pole_tide_ecef = gnss_corrections_.computePoleTideCorrection(rx_pos, time);
        return pole_tide_ecef.dot(-los);
    }

    double PppModel::phaseAtxSubstitutionVarInflation(const std::string& signal,
        const std::string& satellite,
        System system,
        double elevation_deg) const {
        const double s = config_.getModelCorrections().atx_substitution_phase_std_m;
        if (s <= 0.0) return 0.0;                                 // feature disabled (default)
        if (signal.empty() || signal.front() != 'L') return 0.0;  // carrier-phase rows only
        if (!antenna_.wasAtxSubstituted(satellite, signal, system)) return 0.0;
        // 1/sin² elevation amplification, matching the phase thermal term, so the
        // inflation keeps its weight against the (also 1/sin²-scaled) thermal
        // variance at low elevation, where the substituted-PCV residual and its
        // ZWD/height aliasing are largest.  sin(el) is clamped at 0.05 (about 2.9°)
        // to bound the factor; the elevation mask removes such rows upstream anyway.
        double sin_el = std::sin(elevation_deg * Constants::PI / 180.0);
        constexpr double kMinSin = 0.05;
        if (sin_el < kMinSin) sin_el = kMinSin;
        return (s * s) / (sin_el * sin_el);
    }

    double PppModel::computeBaseModeledValue(const RinexObservation& obs,
        const PppObservation& ppp_obs,
        const SatGeometry& geom,
        const std::string& signal_type) const {

        const double slant_dry_delay = geom.tropo_zhd * ppp_obs.mf_d;

        const std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();
        // Under realtime.honor_ssr_reference_point, skip the satellite-PCO range
        // term for satellites whose applied SSR orbit correction this epoch is
        // APC-referenced (legacy-SSR dialect): the corrected orbit already points
        // at the phase center, so the CoM->APC PCO would count 0.6-2.6 m along
        // the LOS twice.  Geometry records the per-satellite flag where the orbit
        // overlay is applied; with the toggle off (the default) nothing is
        // recorded and this is always false (RTB-5(c)).
        const bool skip_sat_pco =
            config_.getRealTimeConfig().honor_ssr_reference_point
            && geometry_.ssrOrbitApcApplied(obs.satellite);
        const double antenna_corr = antenna_.getAntennaCorrection(
            obs.satellite, signal_type, obs.system,
            geom.rx_pos, geom.sat_pos, geom.sat_vel,
            geom.elevation, geom.azimuth, rx_antenna_type, obs.time,
            geometry_.getCachedSunPosition(), skip_sat_pco);

        const double bias_corr = config_.getModelCorrections().enable_code_phase_bias
            ? gnss_corrections_.getBiasCorrection(obs.satellite, signal_type, obs.time)
            : 0.0;

        return geom.range
            - (geom.sat_clock * Constants::SPEED_OF_LIGHT)
            - (geom.relativistic_corr * Constants::SPEED_OF_LIGHT)
            + slant_dry_delay
            + antenna_corr
            + bias_corr
            + geom.shapiro_delay
            + geom.otl_corr
            + geom.apl_corr
            + geom.solid_tide;
    }

    // ===================================================================
    // Iono-free observation processing
    // ===================================================================

    bool PppModel::processIonoFreePair(const RinexObservation& obs, PppObservation& ppp_obs,
        const std::string& system_str,
        const std::pair<std::string, std::string>& sig_pair,
        char meas_type,
        const SatGeometry& geom,
        double pole_tide_corr,
        const std::string& comb_type) {

        const std::string ref_type = sig_pair.first + "_" + sig_pair.second;

        std::vector<double> modeled_obs(2, 0.0);
        std::vector<double> variances(2, 0.0);
        std::vector<double> frequencies(2, 0.0);
        std::vector<double> wavelengths(2, 0.0);

        if (meas_type == 'L') {
            wavelengths[0] = ppp_utils_.getWavelength(obs.system, sig_pair.first, obs.satellite);
            wavelengths[1] = ppp_utils_.getWavelength(obs.system, sig_pair.second, obs.satellite);
            if (wavelengths[0] <= 0.0 || wavelengths[1] <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid wavelength for pair {} on system {} for satellite {}: wl1={:.3f}, wl2={:.3f} m",
                    ref_type, system_str, obs.satellite, wavelengths[0], wavelengths[1]);
                return false;
            }
        }

        for (size_t j = 0; j < 2; ++j) {
            const std::string& type = (j == 0 ? sig_pair.first : sig_pair.second);

            frequencies[j] = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
            if (frequencies[j] <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn("Invalid frequency for type {} on system {} for satellite {}", type, system_str, obs.satellite);
                return false;
            }

            // Per-signal higher-order ionosphere correction (I₂ + I₃),
            // applied before the IF combination so the IF-residual term
            // c₁·I_h(f₁) + c₂·I_h(f₂) enters the modeled value instead of
            // being absorbed by the first-order iono state or the float
            // ambiguity.  No-op unless
            // model_corrections.enable_higher_order_ionosphere is on and a
            // GIM is loaded; see PppModel::setGim and
            // computeHigherOrderIonoCorrection.
            const double i_h_corr = computeHigherOrderIonoCorrection(
                obs.time, geom.rx_pos, geom.sat_pos, frequencies[j], meas_type);

            modeled_obs[j] = computeBaseModeledValue(obs, ppp_obs, geom, type)
                + pole_tide_corr + i_h_corr;

            const auto& n = config_.getDefaultMeasurementNoise();
            const double fallback_var = obs.variances.count(type) ? obs.variances.at(type)
                : (meas_type == 'C' ? n.code_std_m * n.code_std_m : n.phase_std_m * n.phase_std_m);
            variances[j] = stochastic_model_.computeVariance(type, obs.measurements, geom.elevation, fallback_var,
                obs.satellite, antenna_.getAntennas());
            // Phase-only inflation for a substituted receiver ATX, placed before
            // the IF combination so the c₁²/c₂² propagation would carry it into
            // the combined variance (no-op unless atx_substitution_phase_std_m > 0).
            // Not applied on the iono-free path: the call below is commented out.
            //variances[j] += phaseAtxSubstitutionVarInflation(type, obs.satellite, obs.system, geom.elevation); // not needed
            // Per-signal zenith variance, stored for MwSlipDetector although the
            // iono-free aggregation below stores only the combined variance under
            // ref_type.  Without it, MW slip detection in iono-free PPP mode has no
            // per-signal noise prior and falls back to the global default zenith
            // variance.
            ppp_obs.zenith_variances[type] = stochastic_model_.computeVariance(
                type, obs.measurements, 90.0, fallback_var,
                obs.satellite, antenna_.getAntennas());
        }

        const double f1_sq = frequencies[0] * frequencies[0];
        const double f2_sq = frequencies[1] * frequencies[1];
        const double denom = (f1_sq - f2_sq);
        if (!std::isfinite(denom) || std::abs(denom) < 1e-12) {
            log_manager_.getLogger("ppp_model")->warn("Invalid denominator for iono-free pair {} for {}", ref_type, obs.satellite);
            return false;
        }

        const double c1 = f1_sq / denom;
        const double c2 = -f2_sq / denom;
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for dual-frequency pair {} for {}", ref_type, obs.satellite);
            return false;
        }

        const double modeled_value = c1 * modeled_obs[0] + c2 * modeled_obs[1];
        double variance_value = c1 * c1 * variances[0] + c2 * c2 * variances[1];

        double observed_value = 0.0;
        for (size_t j = 0; j < 2; ++j) {
            const std::string& type = (j == 0 ? sig_pair.first : sig_pair.second);
            double meas = obs.measurements.at(type);
            if (meas_type == 'L') {
                meas *= wavelengths[j];
            }
            observed_value += (j == 0 ? c1 : c2) * meas;
        }

        ppp_obs.observed[ref_type] = observed_value;
        ppp_obs.modeled[ref_type] = modeled_value;
        ppp_obs.variances[ref_type] = variance_value;

        log_manager_.getLogger("ppp_model")->trace(
            "Iono-free {} for {} ({}): observed={:.3f} m, modeled={:.3f} m, variance={:.6f}, comb_type={}, mode={}",
            ref_type, obs.satellite, (meas_type == 'C' ? "CODE" : "PHASE"),
            observed_value, modeled_value, variance_value, comb_type,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        return true;
    }

    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>> selected_signals,
        const std::string& comb_type, const SatGeometry& geom) {

        if (comb_type != "dual" && comb_type != "triple") {
            log_manager_.getLogger("ppp_model")->warn("Invalid combination type {} for {}", comb_type, obs.satellite);
            return false;
        }

        if (selected_signals.size() < 2) {
            log_manager_.getLogger("ppp_model")->warn(
                "Too few signal pairs for {} combination for {} (got {})",
                comb_type, obs.satellite, selected_signals.size());
            return false;
        }

        // Validate signal pairs and split into code / phase.
        std::vector<std::pair<std::string, std::string>> code_pairs;
        std::vector<std::pair<std::string, std::string>> phase_pairs;
        code_pairs.reserve(selected_signals.size());
        phase_pairs.reserve(selected_signals.size());

        for (const auto& pair : selected_signals) {
            if (pair.first.empty() || pair.second.empty()) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Empty signal in selected pair for {} {}: first={}, second={}",
                    obs.satellite, system_str, pair.first, pair.second);
                return false;  // programming error in selectIonoFreePairs
            }
            // Skip individual pairs whose measurements are not present for
            // this sat instead of aborting the whole sat.  selectIonoFreePairs
            // already guards its candidates against the per-sat measurement
            // set; this branch is a second guard for a caller that proposes a
            // pair the canonical map names but this sat did not measure.  If
            // both code_pairs and phase_pairs end up empty, the post-loop
            // check below catches it.
            if (!obs.measurements.count(pair.first) || !obs.measurements.count(pair.second)) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Skipping pair for {} {}: {} present={}, {} present={}",
                    obs.satellite, system_str,
                    pair.first, obs.measurements.count(pair.first),
                    pair.second, obs.measurements.count(pair.second));
                continue;
            }
            if (pair.first[0] == 'C' && pair.second[0] == 'C') {
                code_pairs.push_back(pair);
            } else if (pair.first[0] == 'L' && pair.second[0] == 'L') {
                phase_pairs.push_back(pair);
            } else {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid mixed signal pair for {} {}: {} / {}",
                    obs.satellite, system_str, pair.first, pair.second);
                return false;  // programming error
            }
        }

        if (code_pairs.empty() && phase_pairs.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid iono-free signal pairs for {} {}", obs.satellite, system_str);
            return false;
        }

        const double pole_tide_corr = computePoleTideProjection(geom.rx_pos, obs.time, ppp_obs.los);

        bool success = false;
        for (const auto& p : code_pairs) {
            success = processIonoFreePair(obs, ppp_obs, system_str, p, 'C', geom, pole_tide_corr, comb_type) || success;
        }
        for (const auto& p : phase_pairs) {
            success = processIonoFreePair(obs, ppp_obs, system_str, p, 'L', geom, pole_tide_corr, comb_type) || success;
        }

        // Move raw RINEX measurements to raw_observed; only combined keys (containing '_') stay.
        for (auto it = ppp_obs.observed.begin(); it != ppp_obs.observed.end(); ) {
            if (it->first.find('_') == std::string::npos) {
                ppp_obs.raw_observed[it->first] = it->second;
                it = ppp_obs.observed.erase(it);
            } else {
                ++it;
            }
        }

        return success;
    }

    // Each signal gets the same corrections as the iono-free path (range, sat
    // clock, relativistic, hydrostatic tropo, antenna, bias, Shapiro, solid,
    // ocean and pole tide, atmospheric loading, higher-order ionosphere) but
    // forms no dual-frequency combination.  The first-order ionosphere is not
    // modelled here: the modeled value is geometry plus corrections, and
    // MeasurementModel adds the per-satellite slant iono state scaled by the
    // frequency coefficient.
    bool PppModel::processUncombinedObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>>& selected_signals,
        const SatGeometry& geom) {

        const double pole_tide_corr = computePoleTideProjection(geom.rx_pos, obs.time, ppp_obs.los);

        const auto& n = config_.getDefaultMeasurementNoise();
        const double default_code_var = n.code_std_m * n.code_std_m;
        const double default_phase_var = n.phase_std_m * n.phase_std_m;

        bool success = false;

        for (const auto& sig_pair : selected_signals) {
            const std::string& type = sig_pair.first.empty() ? sig_pair.second : sig_pair.first;
            if (type.empty()) continue;

            if (!obs.measurements.count(type)) {
                log_manager_.getLogger("ppp_model")->trace(
                    "Missing measurement for {} on {} in uncombined mode", type, obs.satellite);
                continue;
            }

            // Skip code signals that have no BIA product entry when
            // estimation.require_bia_for_signals is set and biases are applied
            // (model_corrections.enable_code_phase_bias).  Phase measurements are
            // always kept: the float ambiguity absorbs any time-constant satellite
            // phase bias left by missing BIA coverage.  With estimate_sat_biases
            // on, code signals are also kept, because a per-satellite bias state
            // absorbs the uncorrected delay.  MW slip detection is unaffected (it
            // uses raw_observed).
            const char meas_type_pre = type[0];
            if (meas_type_pre == 'C'
                && config_.requireBiaForSignals()
                && !config_.estimateSatBiases()
                && config_.getModelCorrections().enable_code_phase_bias
                && !gnss_corrections_.hasBiasEntry(obs.satellite, type)) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Skipping code {} on {} — no BIA entry (require_bia_for_signals=true)",
                    type, obs.satellite);
                continue;
            }

            // Skip signals without strict ATX coverage when
            // @c model_corrections.require_atx_coverage is set.  Both the
            // satellite and the receiver antenna entries must supply a
            // finite PCO and a populated PCV for this signal's exact
            // frequency, with no fallback chain.  This makes the ATX file
            // an authoritative allowlist when "all" signals are requested
            // but only an ATX-curated subset should be processed.
            if (config_.getModelCorrections().require_atx_coverage
                && !antenna_.hasFullAtxCoverage(obs.satellite, type, obs.system)) {
                log_manager_.getLogger("ppp_model")->debug(
                    "Skipping {} on {} — no strict ATX coverage (require_atx_coverage=true)",
                    type, obs.satellite);
                continue;
            }

            const char meas_type = type[0];
            if (meas_type != 'C' && meas_type != 'L') continue;

            const double freq = ppp_utils_.getFrequency(type, obs.system, obs.satellite);
            if (freq <= 0.0) {
                log_manager_.getLogger("ppp_model")->warn(
                    "Invalid frequency for {} on {} {}", type, obs.satellite, system_str);
                continue;
            }

            // Per-signal higher-order ionosphere correction (I₂ + I₃).
            // In uncombined / PPP-RTK mode the first-order ionosphere is
            // estimated as a per-satellite slant state; modelling I_h here
            // keeps higher-order content out of that state, so it
            // represents I₁ only.  No-op when disabled or when no GIM is
            // set; see PppModel::computeHigherOrderIonoCorrection.
            const double i_h_corr = computeHigherOrderIonoCorrection(
                obs.time, geom.rx_pos, geom.sat_pos, freq, meas_type);

            const double modeled_value = computeBaseModeledValue(obs, ppp_obs, geom, type)
                + pole_tide_corr + i_h_corr;

            double observed_value = obs.measurements.at(type);
            if (meas_type == 'L') {
                const double wavelength = ppp_utils_.getWavelength(obs.system, type, obs.satellite);
                if (wavelength <= 0.0) {
                    log_manager_.getLogger("ppp_model")->warn(
                        "Invalid wavelength for {} on {} {}", type, obs.satellite, system_str);
                    continue;
                }
                observed_value *= wavelength;
            }

            const double fallback_var = obs.variances.count(type) ? obs.variances.at(type)
                : (meas_type == 'C' ? default_code_var : default_phase_var);
            // Zenith pass (elevation_deg=90 → sin²=1 neutralizes elevation amplification,
            // preserving block/band/attribute/SNR scaling).  Consumed by MwSlipDetector
            // as a pair-aware noise floor that doesn't scale with line-of-sight geometry.
            const double zenith_variance = stochastic_model_.computeVariance(
                type, obs.measurements, 90.0, fallback_var,
                obs.satellite, antenna_.getAntennas());
            double variance = stochastic_model_.computeVariance(
                type, obs.measurements, geom.elevation, fallback_var,
                obs.satellite, antenna_.getAntennas());
            // Phase-only inflation for a substituted receiver ATX (no-op unless
            // atx_substitution_phase_std_m > 0).  Applied to the elevation-scaled
            // KF variance only; the zenith variance feeding MwSlipDetector is left
            // unchanged, because the slowly varying PCV bias cancels in epoch
            // differencing and inflating it there would desensitise slip detection.
            variance += phaseAtxSubstitutionVarInflation(type, obs.satellite, obs.system, geom.elevation);

            ppp_obs.observed[type] = observed_value;
            ppp_obs.modeled[type] = modeled_value;
            ppp_obs.variances[type] = variance;
            ppp_obs.zenith_variances[type] = zenith_variance;

            log_manager_.getLogger("ppp_model")->trace(
                "Uncombined {} for {} ({}): observed={:.3f} m, modeled={:.3f} m, variance={:.6f}, freq={:.3f} MHz",
                type, obs.satellite, (meas_type == 'C' ? "CODE" : "PHASE"),
                observed_value, modeled_value, variance, freq / 1e6);

            success = true;
        }

        return success;
    }

    // -----------------------------------------------------------------------
    // Higher-order ionosphere (I₂ + I₃)
    // -----------------------------------------------------------------------

    double PppModel::computeHigherOrderIonoCorrection(const GnssTime& time,
                                                      const Eigen::Vector3d& rx_pos,
                                                      const Eigen::Vector3d& sat_pos,
                                                      double frequency_hz,
                                                      char meas_type) const {
        if (!config_.getModelCorrections().enable_higher_order_ionosphere) return 0.0;
        if (gim_ == nullptr || !gim_->isLoaded()) return 0.0;
        if (frequency_hz <= 0.0) return 0.0;

        const GimQuery q = gim_->slantIonoDelay(time, rx_pos, sat_pos, frequency_hz);
        if (!q.valid) return 0.0;

        return higher_order_iono_.slantHigherOrderDelay(
            rx_pos, sat_pos,
            q.ipp_lat_rad, q.ipp_lon_rad, q.ipp_radius_m,
            q.stec_tecu, frequency_hz, meas_type);
    }

    // Returns 0 when the first two (or three) priority-list signals are all
    // measured, i.e. an iono-free combination is available and removes the
    // delay.  Otherwise returns the Klobuchar delay, or 0 when the Klobuchar
    // model is disabled or its eight coefficients are missing.
    double PppModel::computeIonosphericDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const std::string& freq_type,
        System system,
        const std::map<std::string, double>& measurements) const {
        double freq = ppp_utils_.getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid frequency for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        std::string system_str = ppp_utils_.getSystemString(system);
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities defined for system {}", system_str);
            return 0.0;
        }
        const auto& signals = it->second;

        if (signals.size() >= 3 && measurements.count(signals[0]) && measurements.count(signals[1]) && measurements.count(signals[2])) {
            log_manager_.getLogger("ppp_model")->trace("Triple-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (signals.size() >= 2 && measurements.count(signals[0]) && measurements.count(signals[1])) {
            log_manager_.getLogger("ppp_model")->trace("Dual-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (!ionosphere_.has_value()) {
            // Klobuchar is disabled in model_corrections
            // (enable_klobuchar_ionosphere), so a row that needs a
            // single-frequency a-priori delay gets 0.  The iono-free, DD and
            // uncombined PPP-RTK paths do not call this function.
            return 0.0;
        }
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger("ppp_model")->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = ionosphere_->computeKlobucharDelay(time, rx_pos, sat_pos, freq);
        log_manager_.getLogger("ppp_model")->trace("Computed Klobuchar ionospheric delay for {}: {:.3f} m", freq_type, delay);
        return delay;
    }

    // Forms the LC combination from two measurements with frequency-dependent
    // coefficients; returns 0.0 on error.
    double PppModel::computeIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& signal1,
        const std::string& signal2) const {
        if (!measurements.count(signal1) || !measurements.count(signal2)) {
            log_manager_.getLogger("ppp_model")->warn("Missing measurements for signals {} or {} in iono-free combination", signal1, signal2);
            return 0.0;
        }

        double freq1 = ppp_utils_.getFrequency(signal1, system, "");
        double freq2 = ppp_utils_.getFrequency(signal2, system, "");
        if (freq1 <= 0.0 || freq2 <= 0.0 || std::abs(freq1 - freq2) < 1e-6) {
            log_manager_.getLogger("ppp_model")->warn("Invalid or identical frequencies for {} ({:.3f} MHz) and {} ({:.3f} MHz)",
                signal1, freq1 / 1e6, signal2, freq2 / 1e6);
            return 0.0;
        }

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2;
        double c1 = f1_sq / (f1_sq - f2_sq);
        double c2 = -f2_sq / (f1_sq - f2_sq);
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for iono-free combination: c1={:.3f}, c2={:.3f}", c1, c2);
            return 0.0;
        }

        double combination = c1 * measurements.at(signal1) + c2 * measurements.at(signal2);
        if (!std::isfinite(combination)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite iono-free combination for signals {} and {}: {:.3f}",
                signal1, signal2, combination);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed iono-free combination for signals {} and {}: value={:.3f} m, "
            "freqs=[{:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}]",
            signal1, signal2, combination, freq1 / 1e6, freq2 / 1e6, c1, c2);
        return combination;
    }

    // GLONASS is FDMA, so a cycle count depends on the satellite's channel
    // frequency.  Returns @p observed_data × f_k / f_ref, with f_k the satellite's
    // channel frequency for @p freq_type and f_ref the channel-0 base frequency
    // (PppUtils::getReferenceFrequency).  The ratio is dimensionless, so the result
    // has the input's unit: cycles, as the trace line labels them (a range in
    // metres does not depend on the carrier frequency).  For cycles this converts a
    // count at f_ref into a count at f_k; the opposite direction is f_ref / f_k.
    // Returns the input unchanged for non-GLONASS systems, empty inputs or invalid
    // frequencies.
    double PppModel::rescaleGlonassObservation(double observed_data, const std::string& freq_type,
        System system, const std::string& satellite) const {
        if (system != System::GLONASS) {
            return observed_data;
        }

        if (freq_type.empty() || satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for GLONASS rescaling: freq_type='{}', satellite='{}'",
                freq_type, satellite);
            return observed_data;
        }

        double actual_freq = ppp_utils_.getFrequency(freq_type, system, satellite);
        double ref_freq = ppp_utils_.getReferenceFrequency(freq_type, system);
        if (actual_freq <= 0.0 || ref_freq <= 0.0 || !std::isfinite(actual_freq) || !std::isfinite(ref_freq)) {
            log_manager_.getLogger("ppp_model")->warn("Cannot rescale GLONASS observation for type {}, satellite {}: actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
                freq_type, satellite, actual_freq / 1e6, ref_freq / 1e6);
            return observed_data;
        }

        double scaled_data = observed_data * (actual_freq / ref_freq);
        log_manager_.getLogger("ppp_model")->trace("Rescaled GLONASS observation for type {}, satellite {}: from {:.3f} to {:.3f} cycles, actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
            freq_type, satellite, observed_data, scaled_data, actual_freq / 1e6, ref_freq / 1e6);
        return scaled_data;
    }

    // Returns the available measurement keys in configured-priority order,
    // including GLONASS channel-specific keys where applicable.  May be empty.
    std::vector<std::string> PppModel::selectObservationTypes(const ObsValueMap<double>& measurements,
        System system,
        const std::string& system_str) const {
        std::vector<std::string> selected_types;
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            return selected_types;
        }

        if (system == System::GLONASS) {
            for (const auto& signal : it->second) {
                if (measurements.count(signal)) {
                    selected_types.push_back(signal);
                }
                for (int channel = -7; channel <= 7; ++channel) {
                    std::string type = signal + "_k" + (channel >= 0 ? std::to_string(channel) : "m" + std::to_string(-channel));
                    if (measurements.count(type)) {
                        selected_types.push_back(type);
                    }
                }
            }
        }
        else {
            for (const auto& type : it->second) {
                if (measurements.count(type)) {
                    selected_types.push_back(type);
                }
            }
        }
        return selected_types;
    }

    /**
     * @brief Accessor for the RinexParser used by this model.
     * @return Const reference to the configured RinexParser.
     */
    const RinexParser& PppModel::getRinexParser() const {
        return rinex_parser_;
    }

} // namespace GnssProcessor
//#endif//#endif