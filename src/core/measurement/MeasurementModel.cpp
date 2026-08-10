/**
/**
 * @file MeasurementModel.cpp
 * @brief Implements measurement vector, design matrix and covariance assembly for PPP Kalman updates.
 *
 * The implementation converts a set of PPP observations and the current Kalman state
 * into an EpochMeasurement suitable for Kalman filter processing.
 *
 * Heavy sub-tasks are delegated to focused components:
 *  - StateInitializer:  first-epoch ISB, phase clock, and code bias seeding.
 *  - BssdPivotManager:  pivot selection, handover, and state re-referencing.
 *  - BssdTransform:     between-satellite single-difference S-transformation.
 *
 * This file retains:
 *  - ensureRequiredStates()   � per-epoch state creation/pruning pre-pass.
 *  - isUsableMeasurement()    � per-measurement filtering predicate.
 *  - buildJacobianRow()       � single-row Jacobian and predicted-observable assembly.
 *  - computeMeasurementUpdate � top-level orchestrator.
 *  - validateMeasurementUpdate � post-assembly validation.
 */

#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "measurement/MeasurementModel.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "math/TimeUtils.hpp"
#include "common/SignalBand.hpp"
#include "measurement/BrsdTransform.hpp"
#include "filter/StateKeySchema.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

namespace GnssProcessor {

    // ===================================================================
    // Helpers (file-scope)
    // ===================================================================

    namespace {

    /// Human-readable mode label used in log messages.
    const char* modeLabel(const GnssConfig& cfg) {
        return cfg.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK";
    }

    /// Compute the initial ambiguity standard deviation override (m) for a
    /// newly-seeded ambiguity that depends on one or more signals.  When
    /// @c loosen_amb_q_for_substituted_atx is enabled and any of the listed
    /// signals had its ATX correction frequency-fallback-substituted,
    /// returns @c ambiguity_std_substituted_atx.  Otherwise returns -1.0
    /// (sentinel for "use the global default in StateManager::addAmbiguity").
    double substitutedAmbStdOverride(const GnssConfig& config,
                                       const PppModel& ppp_model,
                                       const PppObservation& obs,
                                       std::initializer_list<std::string> sigs) {
        const auto& mc = config.getModelCorrections();
        if (!mc.loosen_amb_q_for_substituted_atx) return -1.0;
        for (const auto& sig : sigs) {
            if (sig.empty()) continue;
            if (ppp_model.wasAtxSubstituted(obs.satellite, sig, obs.system)) {
                return mc.ambiguity_std_substituted_atx;
            }
        }
        return -1.0;
    }

    } // anonymous namespace

    // ===================================================================
    // Construction
    // ===================================================================

    MeasurementModel::MeasurementModel(StateManager& state_manager, const PppModel& ppp_model,
        const GnssConfig& config, LogManager& log_manager)
        : state_manager_(state_manager), ppp_model_(ppp_model), config_(config), log_manager_(log_manager),
          state_initializer_(config, ppp_model, state_manager, log_manager),
          pivot_manager_(config, log_manager),
          bssd_transform_(config, state_manager, pivot_manager_, log_manager),
          dd_pivot_manager_(config, log_manager),
          ppp_utils_(log_manager) {
        // Select the measurement-differencing strategy once from config.
        differencing_ = MeasurementBuilderFactory::create(
            differencingModeFor(config_), log_manager_);
        log_manager_.getLogger("measurement_model")->info("MeasurementModel initialized with {} signal priority sets, mode={}",
            config_.getSignalPriorities().size(), modeLabel(config_));
    }

    void MeasurementModel::setGlonassSlots(const std::map<std::string, int>& slots) {
        // Seed this station's frequency helper with its RINEX-header FCN slots so
        // GLONASS-frequency-dependent Jacobian terms — the iono coefficient
        // alpha=(f_L1/f)^2 and the per-slot carrier wavelength — use the actual
        // channel rather than the compiled-in default table.  Mirrors PppModel's
        // own one-shot seed; the network driver calls this per station.
        ppp_utils_.setGlonassSlots(slots);
    }

    double MeasurementModel::carrierFrequency(const std::string& obs_code,
                                              const std::string& satellite) const {
        System system = System::Unknown;
        if (!satellite.empty()) {
            switch (satellite.front()) {
                case 'G': system = System::GPS;     break;
                case 'E': system = System::Galileo; break;
                case 'R': system = System::GLONASS; break;
                case 'C': system = System::BeiDou;  break;
                case 'J': system = System::QZSS;    break;
                case 'S': system = System::SBAS;    break;
                default:  system = System::Unknown; break;
            }
        }
        return ppp_utils_.getFrequency(obs_code, system, satellite);
    }

    const SsrCorrectionStore& MeasurementModel::ssrStore() const {
        return ssr_store_override_ ? *ssr_store_override_
                                   : SsrCorrectionStore::getInstance();
    }

    // ===================================================================
    // IMU state setter
    // ===================================================================

    void MeasurementModel::setImuState(const Eigen::Matrix3d& C_b_e,
                                        const Eigen::Vector3d& f_b_corrected) {
        tc_C_b_e_ = C_b_e;
        tc_f_b_ = f_b_corrected;
    }

    void MeasurementModel::appendAidingRow(EpochMeasurement& update,
                                           const AidingMeasurementRow& row) {
        // Guard: a diagonal append after BSSD/DD populated R_full would silently
        // drop the SD cross-covariance (the KF falls back to R_v.asDiagonal()
        // only when R_full's dims don't match innovations).  Fail loudly instead.
        if (update.R_full.size() != 0) {
            throw std::runtime_error(
                "appendAidingRow: R_full is populated (BSSD/DD active); a diagonal "
                "aiding row would drop SD cross-covariance. Row-contributing aiding "
                "sources are restricted to undifferenced PPP / loosely-coupled GNSS.");
        }

        const Eigen::Index n    = update.innovations.size();
        const Eigen::Index ncol = (update.H.cols() > 0)
                                ? update.H.cols()
                                : static_cast<Eigen::Index>(row.jacobian.size());
        if (row.jacobian.size() != ncol) {
            throw std::invalid_argument(
                "appendAidingRow: jacobian size does not match the state dimension "
                "(H columns).");
        }

        update.innovations.conservativeResize(n + 1);
        update.innovations(n) = row.innovation;

        update.R_v.conservativeResize(n + 1);
        update.R_v(n) = row.variance;

        update.H.conservativeResize(n + 1, ncol);
        update.H.row(n) = row.jacobian.transpose();

        update.satellite_ids.push_back(row.label);
        update.signal_types.push_back(row.signal);
    }

    // ===================================================================
    // Product coverage checks
    // ===================================================================

    bool MeasurementModel::ifCombHasFullBia(const std::string& satellite,
                                             const std::string& if_key) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true; // not an IF key � assume covered
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasBiasEntry(satellite, sig1)
            && ppp_model_.hasBiasEntry(satellite, sig2);
    }

    bool MeasurementModel::ssrCoversCodeSignal(const std::string& satellite,
                                               const std::string& signal,
                                               const GnssTime& obs_time) const {
        // Match buildJacobianRow's ssr_bias_term gate exactly: the network
        // producer ESTIMATES satellite biases (importing them would render those
        // states unobservable), so it never imports SSR code biases and must
        // keep estimating them.  Everywhere else, a presence check against the
        // live store: true iff a non-trivial ssr_bias_term will load this exact
        // (sat, raw signal) row, in which case an estimated sat-bias state for
        // the same row would be redundant.  Empty store in post-processing -> false.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        // A5: with the staleness gate on, coverage must track the aged apply gate
        // (same effective stale-hold limit) — once the gate drops a bias, the row
        // is uncorrected, so the sat-bias state and no-BIA downweight re-engage.
        if (config_.enableSsrBiasMaxAge())
            return ssrStore().hasCodeBias(satellite, signal, obs_time,
                                          config_.getSsrBiasMaxAge()
                                              * config_.getSsrBiasStaleHoldFactor());
        return ssrStore().hasCodeBias(satellite, signal);
    }

    bool MeasurementModel::biaCoversSignal(const std::string& satellite,
                                           const std::string& signal) const {
        if (!ppp_model_.hasBiasEntry(satellite, signal)) return false;
        if (!config_.getModelCorrections().bias_coverage_requires_apply) return true;
        // Producer exemption: the network's code rows are corrected by the
        // network-estimated shared code-bias states, not by the .BIA, so its
        // coverage semantics stay legacy under either flag value.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network) return true;
        return config_.getModelCorrections().enable_code_phase_bias;
    }

    bool MeasurementModel::ssrCoversPhaseSignal(const std::string& satellite,
                                                const std::string& signal,
                                                const GnssTime& obs_time) const {
        // Mirror ssrCoversCodeSignal exactly, on the phase channel: the network
        // producer estimates satellite phase biases as states and never imports
        // them; everywhere else, presence in the live store — age-gated with the
        // same effective stale-hold limit as the apply site so coverage tracks
        // what buildJacobianRow actually folds into the row.
        if (config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
            return false;
        if (config_.enableSsrBiasMaxAge())
            return ssrStore().getPhaseBias(satellite, signal, obs_time,
                                           config_.getSsrBiasMaxAge()
                                               * config_.getSsrBiasStaleHoldFactor())
                .has_value();
        return ssrStore().getPhaseBias(satellite, signal).has_value();
    }

    bool MeasurementModel::phaseBiasCoveredForAr(const std::string& satellite,
                                                 const std::string& signal,
                                                 const GnssTime& obs_time) const {
        const size_t us = signal.find('_');
        const bool is_if = (us != std::string::npos && us > 0 && us + 1 < signal.size());
        // T3a: veto AR-eligibility when the covering network phase bias explicitly declares
        // signal_integer=false (the anchor-free ensemble datum emits a non-integer bias).
        // Fresh-coverage-gated + toggle-gated, so an absent bias / .BIA-only / OFF path is
        // byte-identical.  For an IF combo BOTH component biases must be integer-grade.
        if (config_.networkRequireSignalIntegerForAr()) {
            const bool   gate    = config_.enableSsrBiasMaxAge();
            const double max_age = config_.getSsrBiasMaxAge() * config_.getSsrBiasStaleHoldFactor();
            auto nonInteger = [&](const std::string& sig) -> bool {
                auto pb = gate ? ssrStore().getPhaseBias(satellite, sig, obs_time, max_age)
                               : ssrStore().getPhaseBias(satellite, sig);
                return pb.has_value() && !pb->signal_integer;
            };
            if (is_if) {
                if (nonInteger(signal.substr(0, us)) || nonInteger(signal.substr(us + 1)))
                    return false;
            } else if (nonInteger(signal)) {
                return false;
            }
        }
        if (!config_.getModelCorrections().bias_coverage_requires_apply) {
            // Legacy: raw .BIA presence, exactly the historical AR gate.
            return is_if ? ifCombHasFullBia(satellite, signal)
                         : ppp_model_.hasBiasEntry(satellite, signal);
        }
        if (is_if) {
            const std::string sig1 = signal.substr(0, us);
            const std::string sig2 = signal.substr(us + 1);
            return (biaCoversSignal(satellite, sig1) && biaCoversSignal(satellite, sig2))
                || (ssrCoversPhaseSignal(satellite, sig1, obs_time)
                    && ssrCoversPhaseSignal(satellite, sig2, obs_time));
        }
        return biaCoversSignal(satellite, signal)
            || ssrCoversPhaseSignal(satellite, signal, obs_time);
    }

    bool MeasurementModel::ifCombHasFullRxAtx(const std::string& if_key,
                                               System system) const {
        const size_t us = if_key.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= if_key.size())
            return true;
        const std::string sig1 = if_key.substr(0, us);
        const std::string sig2 = if_key.substr(us + 1);
        return ppp_model_.hasReceiverAtxEntry(sig1, system)
            && ppp_model_.hasReceiverAtxEntry(sig2, system);
    }

    // ===================================================================
    // State pre-pass
    // ===================================================================

    void MeasurementModel::ensureRequiredStates(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        const bool is_uncombined = config_.isUncombinedEnabled();
        const bool nlwl = (config_.getStateAmbiguityBasis()
                           == StateAmbiguityBasis::NlWl);

        std::set<std::string> required_ambiguities;
        std::set<std::string> required_iono_sats;

        // For the NlWl path: collect the phase signals seen per (satellite,
        // band) so we can pick a deterministic reference band per satellite
        // and seed states from the ref-band phase observation.  Key formed
        // by RINEX band digit (signal[1]); value is the *first* signal we
        // saw on that band so seeding is reproducible across rebuilds.
        std::map<std::string, std::map<char, std::string>> nlwl_phase_signals;

        // --- First pass: collect required satellites and ambiguity keys ---
        for (const auto& obs : observations) {
            // Skip GLONASS in BSSD by default — its FDMA per-channel
            // receiver delay does not cancel in between-satellite SD.
            // The user can opt back in with enable_glonass_bssd=true,
            // in which case the per-(sat, signal) sat-bias machinery
            // (estimate_sat_biases) absorbs the ICB on code rows and
            // float ambiguities absorb it on phase rows.
            if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()
                && !config_.enableGlonassBssd())
                continue;

            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty()) continue;
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;

                // When BSSD is active, skip signals not tracked by the
                // pivot — they cannot form same-signal SDs and the
                // ambiguity state would be unobservable.  The pivot must
                // track the identical full signal key (raw signal in
                // uncombined mode, iono-free combo in IF mode), since
                // BssdTransform groups SD rows by that exact key.
                if (!bssd_pivot_signals_.empty() && ref_type.size() >= 2
                    && (ref_type[0] == 'C' || ref_type[0] == 'L')) {
                    const std::string cons = PppUtils::getSystemString(obs.satellite);
                    auto cit = bssd_pivot_signals_.find(cons);
                    if (cit != bssd_pivot_signals_.end()
                        && !cit->second.count(ref_type)) continue;
                }

                if (is_uncombined && (ref_type[0] == 'C' || ref_type[0] == 'L'))
                    required_iono_sats.insert(obs.satellite);

                if (ref_type[0] != 'L') continue;

                const bool is_iono_free = (ref_type.find('_') != std::string::npos);
                if (!is_uncombined && !is_iono_free) continue;

                if (nlwl) {
                    // NlWl basis is uncombined-only (validated at config
                    // load), so iono-free phase combos cannot appear here.
                    if (is_iono_free) continue;
                    if (ref_type.size() < 2) continue;
                    nlwl_phase_signals[obs.satellite]
                        .try_emplace(ref_type[1], ref_type);
                } else {
                    required_ambiguities.insert(obs.satellite + "_" + ref_type);
                }
            }
        }

        // --- Hold-protected keys (soft-LLI / gap bridge) ---
        // An ambiguity held across a short tracking outage pending a
        // TurboEdit snapshot+dN·λ repair has no observation this epoch by
        // definition, so it never enters required_ambiguities above and the
        // prune below would delete exactly the pre-gap state the repair
        // restores.  Union the held keys into the keep-set (and their
        // satellites into the iono keep-set, so a whole-satellite outage
        // does not strand the repaired ambiguity against a reseeded iono
        // state).  The seeding loops are observation-driven and therefore
        // never touch these keys — the held state passes through unchanged.
        for (const auto& key : protected_ambiguity_keys_) {
            required_ambiguities.insert(key);
            if (is_uncombined) {
                const auto p = key.find('_');
                if (p != std::string::npos)
                    required_iono_sats.insert(key.substr(0, p));
            }
        }

        // NlWl basis: assemble NL/WL ambiguity keys from the per-(sat, band)
        // phase signal map.  Reference band is the lowest-numbered observed
        // band per satellite (std::map keeps band digits sorted ascending),
        // frozen for the lifetime of the satellite's arc.
        nlwl_ref_band_.clear();
        if (nlwl) {
            for (const auto& [sat, bands] : nlwl_phase_signals) {
                if (bands.empty()) continue;
                const char ref_band = bands.begin()->first;
                nlwl_ref_band_[sat] = ref_band;
                required_ambiguities.insert(StateKeys::makeNlAmbiguityKey(sat, ref_band));
                for (const auto& [b, _sig] : bands) {
                    if (b == ref_band) continue;
                    required_ambiguities.insert(StateKeys::makeWlAmbiguityKey(sat, ref_band, b));
                }
            }
        }

        // --- Ionosphere states (create and seed BEFORE ambiguities) ---
        if (is_uncombined) {
            // Card LV-1 (docs/SPEC_level_information_budget.md): the network
            // producer installs this epoch's per-station seed frame (the lag-1 SH
            // field sampled at each LOS's IPP) before calling in.  The station's
            // median RAW seed — its receiver GF bias ⊕ median slant iono — is
            // computed ONCE here, LAZILY (only when something is actually being
            // allocated, so a settled epoch pays nothing), and is what the reframe
            // removes.  Consumed once: the frame is disarmed at the end of the
            // block so a stale field can never seed a later epoch.
            const bool lv1_on = state_initializer_.ionoSeedFrameArmed();
            std::optional<double> lv1_offset;
            bool lv1_offset_done = false;

            for (const auto& obs : observations) {
                if (!required_iono_sats.count(obs.satellite)) continue;
                if (state_manager_.getIonosphereIndex(obs.satellite) < 0) {
                    if (obs.elevation > 0.0) {
                        state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
                        state_manager_.setIonosphereAzimuth(obs.satellite, obs.azimuth);
                    }

                    const auto iono_seed_opt = state_initializer_.computeIonosphereSeed(obs);
                    // When no BIA-covered code pair is available, the seed
                    // would otherwise inherit raw differential code biases
                    // (e.g. BeiDou with placeholder zero-valued BIA entries).
                    // Fall back to seed=0 with an inflated initial std so the
                    // filter freely converges to the true iono via subsequent
                    // measurements rather than trusting the zero too tightly.
                    constexpr double kUnseededIonoStdScale = 10.0;
                    double iono_seed = iono_seed_opt.value_or(0.0);
                    const double std_scale = iono_seed_opt.has_value() ? 1.0 : kUnseededIonoStdScale;
                    // LV-1 reframe.  Applied ONLY to a real seed: an unseeded state
                    // (no BIA-covered pair) has no receiver-bias content to remove
                    // and keeps its 0 / std×10 fallback exactly as before.
                    if (lv1_on && iono_seed_opt.has_value()) {
                        if (!lv1_offset_done) {
                            lv1_offset = state_initializer_.computeStationSeedOffset(
                                observations, state_initializer_.ionoSeedFrame().min_sats);
                            lv1_offset_done = true;
                        }
                        iono_seed = state_initializer_.applyIonoSeedFrame(
                            obs.satellite, iono_seed, lv1_offset);
                    }
                    state_manager_.addIonosphereState(obs.satellite, iono_seed, std_scale);
                    log_manager_.getLogger("measurement_model")->debug(
                        "{}Added ionosphere state for satellite {} with seed={:.3f} m{}",
                        epoch_prefix, obs.satellite, iono_seed,
                        iono_seed_opt.has_value() ? "" : " (no BIA-covered pair, std×10)");
                }
            }
            // Consume-once: whether or not anything was allocated, this epoch's
            // frame is spent.  A producer that stops installing it (station absent,
            // knob flipped) therefore falls back to RAW seeds visibly, via the
            // fallback counter — never silently onto a stale field.
            if (lv1_on) state_initializer_.clearIonoSeedFrame();
            state_manager_.pruneIonosphereStatesExcept(required_iono_sats);

            for (const auto& obs : observations) {
                if (required_iono_sats.count(obs.satellite) && obs.elevation > 0.0) {
                    state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
                    state_manager_.setIonosphereAzimuth(obs.satellite, obs.azimuth);
                }
            }
        }

        // --- Cold-start receiver-clock pre-seed (network.cold_clock_seed) ---
        // Runs AFTER the iono seed (so computeRawResidual removes the seeded
        // slant iono from the code residual) and BEFORE the ambiguity seed
        // (below), once per station.  On the network path nothing else solves
        // the receiver clock before the ambiguities are first seeded, so a
        // free-running receiver's whole clock offset (e.g. JPLM ≈ 0.856 ms →
        // 2.6e5 m) would otherwise leak UNIFORMLY into every cold ambiguity
        // seed via computeAmbiguitySeed's clk_term=x(clock)=0.  Gated + latched
        // (mark only on success, mirroring the ISB init contract): an OFF run
        // never calls the seeder and is byte-identical.
        if (config_.networkColdClockSeed()
            && !state_initializer_.isClockSeedInitialized()) {
            if (state_initializer_.seedColdReceiverClock(observations, epoch_prefix))
                state_initializer_.markClockSeedInitialized();
        }

        // --- Ambiguity states ---
        const bool bssd_active = config_.enableBetweenSatelliteSd();

        // Pivot absolute seeds, keyed by constellation + full signal type
        // (e.g. "GPS|L1W", "GPS|L1C") — one anchor per (sat, signal) pair.
        // Under channel_mode=all the pivot tracks multiple signals per band
        // and each gets its own SD frame; non-pivots' L1W SD against pivot's
        // L1W, non-pivots' L1C SD against pivot's L1C.  BssdTransform's
        // group key now uses the same full-signal granularity, so the two
        // sides agree on what "the pivot row" is for each ambiguity state.
        //
        // For NlWl basis the seeds are pre-converted to cycles (NL: pivot's
        // L_ref_band cycle ambiguity; WL: pivot's N_a - N_b cycle difference
        // for each band-pair the pivot tracks beyond ref_band).  Keys are
        // "<cons>|NL" and "<cons>|WL|<a>_<b>" so the lookup at non-pivot
        // seeding stays a flat string→double map.
        std::map<std::string, double> pivot_abs_seeds;
        if (bssd_active && !nlwl) {
            const Eigen::VectorXd x_seed = state_manager_.getStateVector();
            const auto& pivot_sats = pivot_manager_.getPivotSatellites();
            for (const auto& obs : observations) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                auto piv_it = pivot_sats.find(cons);
                if (piv_it == pivot_sats.end() || piv_it->second != obs.satellite)
                    continue;
                for (const auto& [ref_type, val] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    const std::string pkey = cons + "|" + ref_type;
                    pivot_abs_seeds[pkey] =
                        state_manager_.computeAmbiguitySeed(obs, ref_type, x_seed);
                }
            }
        }
        if (bssd_active && nlwl) {
            const Eigen::VectorXd x_seed = state_manager_.getStateVector();
            const auto& pivot_sats = pivot_manager_.getPivotSatellites();
            for (const auto& obs : observations) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                auto piv_it = pivot_sats.find(cons);
                if (piv_it == pivot_sats.end() || piv_it->second != obs.satellite)
                    continue;
                auto sit = nlwl_phase_signals.find(obs.satellite);
                if (sit == nlwl_phase_signals.end() || sit->second.empty())
                    continue;
                const char ref_band = sit->second.begin()->first;
                const auto& ref_sig = sit->second.begin()->second;
                const double lambda_ref = ppp_utils_.getWavelength(
                    obs.system, ref_sig, obs.satellite);
                if (!std::isfinite(lambda_ref) || lambda_ref <= 0.0) continue;

                const double nl_seed_cyc =
                    state_manager_.computeAmbiguitySeed(obs, ref_sig, x_seed)
                    / lambda_ref;
                pivot_abs_seeds[cons + "|NL"] = nl_seed_cyc;

                for (const auto& [b, b_sig] : sit->second) {
                    if (b == ref_band) continue;
                    const double lambda_b = ppp_utils_.getWavelength(
                        obs.system, b_sig, obs.satellite);
                    if (!std::isfinite(lambda_b) || lambda_b <= 0.0) continue;
                    const double n_b_cyc =
                        state_manager_.computeAmbiguitySeed(obs, b_sig, x_seed)
                        / lambda_b;
                    // WL canonical = N_low_band - N_high_band.  When ref_band
                    // is the lower of the pair (the typical case, e.g. L1
                    // ref + L2/L5 WL) WL = NL - N_b.  When ref_band > b
                    // (rare: sat first observed only on L2, then L1 appears),
                    // the canonical key has b first, so WL = N_b - NL.
                    const char a = std::min(ref_band, b);
                    const char z = std::max(ref_band, b);
                    const double wl_seed_cyc = (a == ref_band)
                        ? (nl_seed_cyc - n_b_cyc)
                        : (n_b_cyc - nl_seed_cyc);
                    const std::string wl_pair = std::string{ a } + "_" + std::string{ z };
                    pivot_abs_seeds[cons + "|WL|" + wl_pair] = wl_seed_cyc;
                }
            }
        }

        // Per-band seed/add loop (skipped under NlWl basis — see below).
        if (!nlwl) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'L') continue;
                    const std::string amb_key = obs.satellite + "_" + ref_type;
                    if (!required_ambiguities.count(amb_key)) continue;

                    const int existing_idx = state_manager_.getAmbiguityIndex(amb_key);

                    bool needs_seed = (existing_idx < 0)
                                   || state_manager_.isAmbiguityPendingReseed(amb_key);

                    if (needs_seed) {
                        const Eigen::VectorXd x_now = state_manager_.getStateVector();
                        double seed = state_manager_.computeAmbiguitySeed(obs, ref_type, x_now);

                        if (bssd_active) {
                            const std::string cons = PppUtils::getSystemString(obs.satellite);
                            const auto& pivot_sats = pivot_manager_.getPivotSatellites();
                            auto piv_it = pivot_sats.find(cons);
                            const bool is_pivot = piv_it != pivot_sats.end()
                                               && piv_it->second == obs.satellite;
                            if (is_pivot) {
                                // Pivot's row of this exact signal is the
                                // anchor in BssdTransform's per-signal group;
                                // state = 0 by construction.
                                seed = 0.0;
                            } else {
                                // SD against the pivot's same-signal seed.
                                const std::string pkey = cons + "|" + ref_type;
                                auto ps_it = pivot_abs_seeds.find(pkey);
                                if (ps_it != pivot_abs_seeds.end())
                                    seed -= ps_it->second;
                            }
                        }

                        if (existing_idx < 0) {
                            const double init_std_override =
                                substitutedAmbStdOverride(config_, ppp_model_, obs, {ref_type});
                            state_manager_.addAmbiguity(amb_key, seed, init_std_override);
                            log_manager_.getLogger("measurement_model")->debug(
                                "{}Added missing ambiguity state for {} (key={}), seed={:.3f} m{}",
                                epoch_prefix, obs.satellite, amb_key, seed,
                                bssd_active ? " (SD)" : "");
                        } else {
                            Eigen::VectorXd x_reseed = x_now;
                            x_reseed(existing_idx) = seed;
                            state_manager_.setStateVector(x_reseed);
                            log_manager_.getLogger("measurement_model")->debug(
                                "{}Re-seeded reset ambiguity for {} (key={}), seed={:.3f} m{}",
                                epoch_prefix, obs.satellite, amb_key, seed,
                                bssd_active ? " (SD)" : "");
                        }
                        state_manager_.clearPendingReseed(amb_key);
                    }
                }
            }
        }

        // NlWl seed/add loop.  Iterates the per-(sat, band) phase signal map
        // built earlier rather than the raw observation list; this keeps
        // seeding deterministic across measurement-rebuild passes within
        // the same epoch, and naturally enforces "one NL + one WL per
        // non-ref band" without requiring the caller-side dedup tracking
        // that the per-band loop avoids by virtue of its key uniqueness.
        if (nlwl) {
            for (const auto& [sat, bands] : nlwl_phase_signals) {
                if (bands.empty()) continue;
                const char ref_band = bands.begin()->first;
                const auto& ref_sig = bands.begin()->second;

                // Find this sat's PppObservation (one per epoch in the
                // single-receiver path NlWl supports).
                const PppObservation* obs_ptr = nullptr;
                for (const auto& obs : observations) {
                    if (obs.satellite == sat) { obs_ptr = &obs; break; }
                }
                if (!obs_ptr) continue;
                const PppObservation& obs = *obs_ptr;

                const double lambda_ref = ppp_utils_.getWavelength(
                    obs.system, ref_sig, obs.satellite);
                if (!std::isfinite(lambda_ref) || lambda_ref <= 0.0) continue;

                const std::string cons = PppUtils::getSystemString(obs.satellite);
                const auto& pivot_sats = pivot_manager_.getPivotSatellites();
                auto piv_it = pivot_sats.find(cons);
                const bool is_pivot = bssd_active
                                   && piv_it != pivot_sats.end()
                                   && piv_it->second == obs.satellite;

                // --- NL state ---
                const std::string nl_key = StateKeys::makeNlAmbiguityKey(sat, ref_band);
                if (required_ambiguities.count(nl_key)) {
                    const int existing_idx = state_manager_.getAmbiguityIndex(nl_key);
                    const bool needs_seed = (existing_idx < 0)
                        || state_manager_.isAmbiguityPendingReseed(nl_key);
                    if (needs_seed) {
                        const Eigen::VectorXd x_now = state_manager_.getStateVector();
                        double nl_seed_cyc =
                            state_manager_.computeAmbiguitySeed(obs, ref_sig, x_now)
                            / lambda_ref;
                        if (bssd_active) {
                            if (is_pivot) {
                                nl_seed_cyc = 0.0;
                            } else {
                                auto ps_it = pivot_abs_seeds.find(cons + "|NL");
                                if (ps_it != pivot_abs_seeds.end())
                                    nl_seed_cyc -= ps_it->second;
                            }
                        }
                        if (existing_idx < 0) {
                            const double init_std_override =
                                substitutedAmbStdOverride(config_, ppp_model_, obs, {ref_sig});
                            state_manager_.addAmbiguity(nl_key, nl_seed_cyc, init_std_override);
                            log_manager_.getLogger("measurement_model")->debug(
                                "{}Added NL ambiguity state {} (band {}), seed={:.3f} cyc{}",
                                epoch_prefix, nl_key, ref_band, nl_seed_cyc,
                                bssd_active ? " (SD)" : "");
                        } else {
                            Eigen::VectorXd x_reseed = x_now;
                            x_reseed(existing_idx) = nl_seed_cyc;
                            state_manager_.setStateVector(x_reseed);
                            log_manager_.getLogger("measurement_model")->debug(
                                "{}Re-seeded NL ambiguity {}, seed={:.3f} cyc{}",
                                epoch_prefix, nl_key, nl_seed_cyc,
                                bssd_active ? " (SD)" : "");
                        }
                        state_manager_.clearPendingReseed(nl_key);
                    }
                }

                // --- WL states (one per non-ref band) ---
                for (const auto& [b, b_sig] : bands) {
                    if (b == ref_band) continue;
                    const std::string wl_key = StateKeys::makeWlAmbiguityKey(sat, ref_band, b);
                    if (!required_ambiguities.count(wl_key)) continue;

                    const int existing_idx = state_manager_.getAmbiguityIndex(wl_key);
                    const bool needs_seed = (existing_idx < 0)
                        || state_manager_.isAmbiguityPendingReseed(wl_key);
                    if (!needs_seed) continue;

                    const double lambda_b = ppp_utils_.getWavelength(
                        obs.system, b_sig, obs.satellite);
                    if (!std::isfinite(lambda_b) || lambda_b <= 0.0) continue;

                    const Eigen::VectorXd x_now = state_manager_.getStateVector();
                    const double nl_cyc =
                        state_manager_.computeAmbiguitySeed(obs, ref_sig, x_now)
                        / lambda_ref;
                    const double n_b_cyc =
                        state_manager_.computeAmbiguitySeed(obs, b_sig, x_now)
                        / lambda_b;
                    // Canonical sign: WL_<low>_<high> stores N_low - N_high.
                    const char a = std::min(ref_band, b);
                    const char z = std::max(ref_band, b);
                    double wl_seed = (a == ref_band) ? (nl_cyc - n_b_cyc)
                                                     : (n_b_cyc - nl_cyc);
                    if (bssd_active) {
                        if (is_pivot) {
                            wl_seed = 0.0;
                        } else {
                            const std::string wl_pair = std::string{ a } + "_" + std::string{ z };
                            auto ps_it = pivot_abs_seeds.find(cons + "|WL|" + wl_pair);
                            if (ps_it != pivot_abs_seeds.end())
                                wl_seed -= ps_it->second;
                        }
                    }
                    if (existing_idx < 0) {
                        const double init_std_override =
                            substitutedAmbStdOverride(config_, ppp_model_, obs, {ref_sig, b_sig});
                        state_manager_.addAmbiguity(wl_key, wl_seed, init_std_override);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added WL ambiguity state {} (bands {}-{}), seed={:.3f} cyc{}",
                            epoch_prefix, wl_key, ref_band, b, wl_seed,
                            bssd_active ? " (SD)" : "");
                    } else {
                        Eigen::VectorXd x_reseed = x_now;
                        x_reseed(existing_idx) = wl_seed;
                        state_manager_.setStateVector(x_reseed);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Re-seeded WL ambiguity {}, seed={:.3f} cyc{}",
                            epoch_prefix, wl_key, wl_seed,
                            bssd_active ? " (SD)" : "");
                    }
                    state_manager_.clearPendingReseed(wl_key);
                }
            }
        }

        state_manager_.pruneAmbiguitiesExcept(required_ambiguities);

        // Defensive band-count check.  A correctly-functioning band
        // filter should produce at most N distinct phase bands per
        // satellite, where N corresponds to the configured signal_mode
        // (Dual = 2, Triple = 3, All = unbounded).  If a satellite ends
        // up with more bands than expected, some upstream path has
        // bypassed the filter (e.g., a slip detector emitting a reset
        // key for a band-filtered signal, or a PPP_RTK overrides we
        // missed).  The pruneAmbiguitiesExcept call above clears any
        // such state immediately, but the warning surfaces the
        // mechanism so it can be tracked down at the source.
        if (!nlwl) {
            int expected_bands = 0;
            switch (config_.getFrequencyMode()) {
                case FrequencyMode::Dual:        expected_bands = 2; break;
                case FrequencyMode::Triple:      expected_bands = 3; break;
                case FrequencyMode::All:         expected_bands = 0; break;  // 0 = no cap
                case FrequencyMode::Independent: expected_bands = 0; break;  // per-sat variable → no cap
            }
            if (expected_bands > 0) {
                std::map<std::string, std::set<char>> per_sat_bands;
                for (const auto& key : required_ambiguities) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    if (parts->signal.size() < 2) continue;
                    if (parts->signal.find('_') != std::string::npos) continue;  // IF combo
                    per_sat_bands[parts->satellite].insert(parts->signal[1]);
                }
                for (const auto& [sat, bands] : per_sat_bands) {
                    if (static_cast<int>(bands.size()) > expected_bands) {
                        std::string band_list;
                        for (char b : bands) {
                            if (!band_list.empty()) band_list += ',';
                            band_list += b;
                        }
                        log_manager_.getLogger("measurement_model")->warn(
                            "{}Sat {} has {} ambiguity bands {{{}}} — exceeds "
                            "signal_mode={} cap of {}.  An upstream path bypassed "
                            "the band filter (likely a slip detector reset key on "
                            "a band-filtered signal).",
                            epoch_prefix, sat, bands.size(), band_list,
                            config_.getFrequencyMode() == FrequencyMode::Dual ? "Dual" : "Triple",
                            expected_bands);
                    }
                }
            }
        }

        // --- Mark unresolvable ambiguities ---
        // Ambiguities without full BIA phase bias coverage cannot be resolved
        // to their correct integer.  Mark them as unresolvable so they are
        // excluded from the LAMBDA search.  The float ambiguity still absorbs
        // the time-constant satellite phase bias with zero process noise.
        state_manager_.clearUnresolvableAmbiguities();

        // NlWl basis: per-band unresolvable bookkeeping does not apply to
        // NL/WL state keys.  For now the WL state is always resolvable
        // (MW provides a near-noise-free integer constraint) and the NL
        // state's resolvability depends on band-pair BIA coverage in a
        // way Phase 5 will model properly.  TODO: revisit when LAMBDA NL
        // search is wired through AmbiguityResolver in the NlWl basis.
        // The remaining receiver-bias / sat-bias bookkeeping below applies
        // to code rows only, so it is unaffected by the basis switch.
        if (!nlwl) {
            // GLONASS in BSSD: every phase ambiguity is contaminated by
            // the receiver per-channel inter-channel bias (ICB) which the
            // float ambiguity absorbs.  IAR would lock onto the wrong
            // integer; mark all GLONASS phase ambiguities unresolvable
            // so LAMBDA skips them.
            if (config_.enableBetweenSatelliteSd() && config_.enableGlonassBssd()) {
                for (const auto& obs : observations) {
                    if (obs.system != System::GLONASS) continue;
                    for (const auto& [ref_type, observed] : obs.observed) {
                        if (ref_type.empty() || ref_type[0] != 'L') continue;
                        const std::string amb_key = obs.satellite + "_" + ref_type;
                        if (!required_ambiguities.count(amb_key)) continue;
                        state_manager_.markAmbiguityUnresolvable(amb_key);
                    }
                }
            }
            if (!is_uncombined) {
                // Combined (IF) mode: check both component signals of the IF label.
                for (const auto& obs : observations) {
                    for (const auto& [ref_type, observed] : obs.observed) {
                        if (ref_type.empty() || ref_type[0] != 'L') continue;
                        if (ref_type.find('_') == std::string::npos) continue;
                        const std::string amb_key = obs.satellite + "_" + ref_type;
                        if (!required_ambiguities.count(amb_key)) continue;
                        const bool missing_bia =
                            !phaseBiasCoveredForAr(obs.satellite, ref_type, obs.time);
                        const bool missing_atx = !ifCombHasFullRxAtx(ref_type, obs.system);
                        if (missing_bia || missing_atx) {
                            state_manager_.markAmbiguityUnresolvable(amb_key);
                            log_manager_.getLogger("measurement_model")->trace(
                                "{}Unresolvable ambiguity {}: missing_bia={}, missing_atx={}",
                                epoch_prefix, amb_key, missing_bia, missing_atx);
                        }
                    }
                }
            } else {
                // Uncombined (PPP-RTK) mode: check the individual phase signal.
                for (const auto& obs : observations) {
                    for (const auto& [ref_type, observed] : obs.observed) {
                        if (ref_type.empty() || ref_type[0] != 'L') continue;
                        if (ref_type.find('_') != std::string::npos) continue;
                        const std::string amb_key = obs.satellite + "_" + ref_type;
                        if (!required_ambiguities.count(amb_key)) continue;
                        const bool missing_bia =
                            !phaseBiasCoveredForAr(obs.satellite, ref_type, obs.time);
                        if (missing_bia) {
                            state_manager_.markAmbiguityUnresolvable(amb_key);
                            log_manager_.getLogger("measurement_model")->trace(
                                "{}Unresolvable ambiguity {}: no applied phase bias "
                                "(.BIA or network/SSR)",
                                epoch_prefix, amb_key);
                        }
                    }
                }
            }
        }

        // --- Receiver bias states ---
        std::set<std::string> required_code_biases;
        if (config_.estimateIfb() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
                    if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;
                    // GLONASS network producer: allocate ONLY the GLONASS per-FCN-slot
                    // IFB.  GPS/Galileo receiver DCB/IFB states are intentionally absent
                    // (as in the GPS+Galileo network) — a non-reference-band IFB has no
                    // IFB-free reference code to anchor the slant iono, so creating one
                    // adds a per-satellite iono<->IFB<->ambiguity rank deficiency to the
                    // absolute per-epoch local block.  The GPS+Galileo network avoids it
                    // by absorbing the receiver code bias into the clock/iono (the L1-
                    // referenced convention), so we keep those bands IFB-free here.  The
                    // GLONASS per-slot IFB is the one receiver bias the network needs
                    // (its FDMA group delay is otherwise unmodellable); its residual
                    // iono/common-mode gauges are pinned by soft anchors in the driver.
                    if (config_.enableGlonassNetwork()
                        && !StateKeys::isGlonassPerSlotIfbKey(bias.state_key)) continue;
                    if (required_code_biases.count(bias.state_key)) continue;
                    required_code_biases.insert(bias.state_key);
                    if (state_manager_.getCodeBiasIndex(bias.state_key) < 0) {
                        state_manager_.addCodeBiasState(bias.state_key, 0.0);
                        log_manager_.getLogger("measurement_model")->debug(
                            "{}Added receiver bias state for {}", epoch_prefix, bias.state_key);
                    }
                }
            }
        }
        state_manager_.pruneCodeBiasStatesExcept(required_code_biases);

        // --- Per-satellite signal bias states (code-only in uncombined) ---
        // Phase ambiguities absorb satellite phase biases, so only code
        // measurements without BIA coverage need a per-satellite bias state.
        //
        // On first creation the state is seeded from the row's innovation
        // (observed - predicted-without-bias) rather than 0.  Hardware
        // delays on uncovered signals (e.g. BeiDou C7I in MGEX BIA, which
        // ships only C2I/C6I) routinely run 100+ m once differenced
        // against a same-band pivot whose bias state is also unset.  A
        // fresh state at value 0 with std initial_std.sat_bias=10 m
        // produces sres ≫ threshold on epoch 1 and the row is excluded
        // before the state ever gets a measurement update — pinning the
        // signal as a permanent outlier.  Seeding collapses the first
        // innovation to ~0 instead.  The seed absorbs whatever else is
        // unmodeled at this epoch (clock/iono/ISB residuals); BSSD
        // cancels the common-mode part across SD pairs, and the small
        // (10 m) initial std lets future epochs refine the rest.
        auto seedSatBias = [&](const PppObservation& obs,
                               const std::string& ref_type,
                               double observed) -> double {
            if (!obs.modeled.count(ref_type)) return 0.0;
            const Eigen::VectorXd x_now = state_manager_.getStateVector();
            Eigen::VectorXd jac_unused;
            double predicted = 0.0;
            if (!buildJacobianRow(obs, ref_type, x_now,
                                  static_cast<size_t>(x_now.size()),
                                  jac_unused, predicted))
                return 0.0;
            double seed = observed - predicted;

            // BSSD frame consistency.  The raw seed above zeroes this
            // satellite's UNDIFFERENCED innovation.  Because BSSD allocates no
            // receiver-clock state (it cancels in the SD measurement), that
            // innovation absorbs the FULL receiver clock at the current epoch.
            // A satellite that first appears later than the incumbents then
            // bakes a different clock epoch into its bias than the pivot it is
            // differenced against; the gap (= receiver-clock drift between the
            // two seeding epochs) is a permanent SD offset that pins the row as
            // an outlier and, once the per-row handler excludes it, never gets
            // a measurement update to recover.  Re-reference the seed to the
            // pivot's CURRENT undifferenced innovation so the new bias lands in
            // the same frame as the established states (seed -> clock(t0)+dcb,
            // SD residual ~0).  This is the code-bias analogue of pivot-relative
            // ambiguity seeding (N_sd = N_abs(i) - N_abs(pivot)).  Applied only
            // when the pivot already carries a same-band bias state — that is
            // the signal that a frame exists to reference; otherwise the raw
            // (frame-defining) seed is correct.
            if (config_.enableBetweenSatelliteSd()) {
                const std::string cons = PppUtils::getSystemString(obs.satellite);
                const auto& pivots = pivot_manager_.getPivotSatellites();
                auto piv_it = pivots.find(cons);
                if (piv_it != pivots.end() && piv_it->second != obs.satellite) {
                    const std::string& pivot_sat = piv_it->second;
                    const std::string band =
                        ref_type.size() >= 2 ? ref_type.substr(0, 2) : ref_type;
                    const PppObservation* piv_obs = nullptr;
                    for (const auto& o : observations) {
                        if (o.satellite == pivot_sat) { piv_obs = &o; break; }
                    }
                    if (piv_obs) {
                        // Band-match the pivot's code signal (it may track a
                        // different attribute on the same band, e.g. C1X vs C1C)
                        // and require it to already have a bias state.
                        std::string piv_sig;
                        for (const auto& [s, v] : piv_obs->observed) {
                            if (s.size() >= 2 && s[0] == 'C'
                                && s.find('_') == std::string::npos
                                && s.substr(0, 2) == band
                                && piv_obs->modeled.count(s)
                                && state_manager_.getSatBiasIndex(pivot_sat + "|" + s) >= 0) {
                                piv_sig = s;
                                break;
                            }
                        }
                        if (!piv_sig.empty()) {
                            Eigen::VectorXd jac_p;
                            double pred_p = 0.0;
                            if (buildJacobianRow(*piv_obs, piv_sig, x_now,
                                                 static_cast<size_t>(x_now.size()),
                                                 jac_p, pred_p)) {
                                const double innov_pivot =
                                    piv_obs->observed.at(piv_sig) - pred_p;
                                seed -= innov_pivot;
                            }
                        }
                    }
                }
            }
            return seed;
        };

        // Band-gate helper: when BSSD is active, only signals whose band(s)
        // are covered by the current pivot are observable in SD.  Creating
        // a sat-bias state when the band is *not* covered would seed
        // against the current state vector — including any iono state
        // that hasn't yet converged for this satellite — and freeze that
        // contaminated value (process noise on sat-bias is essentially
        // zero).  When a future pivot rotation finally brings the band
        // into BSSD, the row uses the stale bad seed, produces a huge
        // innovation, gets flagged as an outlier, and is excluded from
        // the update — so the state never gets a chance to recover.
        // Gating creation on @c bssd_pivot_signals_ ensures the seed is
        // computed against the converged state at the moment the signal
        // first becomes usable.
        auto signalCoveredByPivot = [&](const std::string& sat,
                                         const std::string& sig) -> bool {
            if (bssd_pivot_signals_.empty()) return true;  // BSSD off
            const std::string cons = PppUtils::getSystemString(sat);
            auto cit = bssd_pivot_signals_.find(cons);
            if (cit == bssd_pivot_signals_.end()) return true;  // no pivot for cons
            if (sig.size() < 2) return false;
            // Match the pivot's full signal key (raw signal in uncombined
            // mode, iono-free combo in IF mode); BssdTransform groups SD
            // rows by that exact key.
            return cit->second.count(sig) > 0;
        };

        // Helper: try the sat-bias cache before falling back to the
        // WLS-based seed.  When a (sat, signal) has been pruned recently
        // (visibility gap, brief masking, geometry hole) the cache
        // holds its last-known value + diagonal variance.  Restoring it
        // skips a full re-convergence cycle and prevents the new state
        // from re-seeding via the first innovation against a not-yet-
        // converged neighbor state vector.
        //
        // Reports back whether a cache restore happened so the caller's
        // log line can distinguish the "warm restored" vs "WLS seeded"
        // cases.  Always consumes the cache entry on restore — a key
        // is either in the active state or in the cache, never both.
        auto addOrRestoreSatBias = [&](const std::string& key,
                                       const PppObservation& obs,
                                       const std::string& ref_type,
                                       double observed,
                                       const char* tag) {
            auto restoration = state_manager_.tryRestoreSatBias(
                key, state_manager_.getStateEpochTime());
            if (restoration.has_value()) {
                state_manager_.addSatBiasStateWithVariance(
                    key, restoration->value, restoration->variance);
                state_manager_.consumeSatBiasCacheEntry(key);
                log_manager_.getLogger("measurement_model")->info(
                    "{}Restored {} sat bias state for {} from cache: "
                    "value={:+.3f} m, std={:.3f} m, gap={:.1f} s ({})",
                    epoch_prefix, tag, key,
                    restoration->value, std::sqrt(restoration->variance),
                    restoration->gap_s,
                    restoration->is_warm ? "warm" : "cold-value");
            } else {
                const double seed = seedSatBias(obs, ref_type, observed);
                state_manager_.addSatBiasState(key, seed);
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Added {} sat bias state for {}, seed={:+.3f} m",
                    epoch_prefix, tag, key, seed);
            }
        };

        // In the PPP-RTK NETWORK producer the satellite code-bias state lifecycle is
        // owned by ensureNetworkSharedStates (a SHARED border column, allocated and
        // pruned in lockstep with the satellite clock + phase bias), NOT by this
        // single-receiver fallback path — exactly as the satellite clock and phase
        // bias are network-owned.  Skip it here so the two paths never fight over the
        // same per-(sat,signal) states (the network admits every GPS/Galileo raw code
        // signal regardless of .BIA coverage; the .BIA/SSR gate below is a user-side
        // concern that does not apply when the network estimates its own OSB).
        const bool network_owns_sat_bias =
            config_.getProcessingMode() == ProcessingMode::PPP_RTK_Network;
        std::set<std::string> required_sat_biases;
        if (!network_owns_sat_bias
            && config_.estimateSatBiases() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type.find('_') != std::string::npos) continue;
                    if (ref_type[0] != 'C') continue;  // code-only
                    // Per-slot GLONASS IFB owns the receiver-side code delay on
                    // the FDMA band where it creates a state.  A per-(sat,signal)
                    // sat-bias on that same signal is collinear with it (both
                    // unit-coefficient and additive), so skip sat-bias creation
                    // wherever resolveReceiverBias yields a per-slot IFB key.
                    // Signals with no IFB state (k=0 datum, unknown slot, CDMA
                    // band) fall through and keep their sat-bias fallback.
                    if (config_.enableGlonassIfb() && config_.estimateIfb()
                        && obs.system == System::GLONASS
                        && !state_initializer_.resolveReceiverBias(
                                ref_type, obs.system, obs.satellite).state_key.empty())
                        continue;
                    // "Already corrected?" — consult BOTH bias sources, not just
                    // the static .BIA product.  In real-time there is no .BIA, so
                    // hasBiasEntry is false for every signal and the live SSR
                    // stream is the actual correction source, applied (ungated) as
                    // ssr_bias_term in buildJacobianRow.  Without the SSR check a
                    // redundant, near-frozen (sat_bias_q~1e-10) per-(sat,signal)
                    // state gets created for every SSR-covered signal — the bug
                    // this gate fixes.  Per-(sat,signal): IGS streams are partial
                    // (e.g. E1 covered, E5a/E5b not), so a signal SSR does NOT
                    // carry still gets its state (its only code correction).  The
                    // strict prune below drops the state the epoch SSR coverage
                    // arrives mid-arc; ensureRequiredStates re-creates it if
                    // coverage later disappears.
                    if (biaCoversSignal(obs.satellite, ref_type)
                        || ssrCoversCodeSignal(obs.satellite, ref_type, obs.time)) continue;
                    if (!signalCoveredByPivot(obs.satellite, ref_type)) continue;
                    const std::string key = obs.satellite + "|" + ref_type;
                    required_sat_biases.insert(key);
                    if (state_manager_.getSatBiasIndex(key) < 0) {
                        addOrRestoreSatBias(key, obs, ref_type, observed, "");
                    }
                }
            }
        }
        if (!network_owns_sat_bias && config_.estimateSatBiases() && !is_uncombined) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    if (ref_type.find('_') == std::string::npos) continue;
                    if (ifCombHasFullBia(obs.satellite, ref_type)) continue;
                    if (!signalCoveredByPivot(obs.satellite, ref_type)) continue;
                    const std::string key = obs.satellite + "|" + ref_type;
                    required_sat_biases.insert(key);
                    if (state_manager_.getSatBiasIndex(key) < 0) {
                        addOrRestoreSatBias(key, obs, ref_type, observed, "IF");
                    }
                }
            }
        }

        // Strict prune: drop every sat-bias state whose (sat, signal)
        // isn't required *this* epoch — matches the ambiguity prune
        // policy.  No state for an invisible satellite or for a signal
        // that fell out of pivot coverage survives in the active filter.
        // The pruned states are stashed in the StateManager's sat-bias
        // cache so a near-term re-acquisition can warm-restart instead
        // of paying a cold-start re-seed (which is fragile against
        // un-converged neighbor states — see seedSatBias comment).
        if (!network_owns_sat_bias && config_.estimateSatBiases()) {
            state_manager_.pruneSatBiasStatesExcept(required_sat_biases);
        }
    }

    // ===================================================================
    // Network producer: shared satellite-state pre-allocation
    // ===================================================================

    void MeasurementModel::ensureNetworkSharedStates(
        const std::vector<PppObservation>& observations,
        const std::set<std::string>& allowed_phase_keys,
        const std::set<std::string>* cbadm_admitted_code_keys) {
        if (config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network) return;
        if (!config_.isUncombinedEnabled()) return;  // network V1 is uncombined-only

        // Empty exclusion set: the network driver runs no KF rebuild loop, so no
        // row is carried-excluded.  Matches the excluded set computeMeasurementUpdate
        // is called with below, keeping this admission gate identical to the build.
        const std::set<std::string> no_excl;

        std::set<std::string> required_clock_keys;
        std::set<std::string> required_phase_keys;
        std::set<std::string> required_code_keys;

        for (const auto& obs : observations) {
            // GLONASS in the network product (code-only scope, enableGlonassNetwork):
            // it gets a shared satellite CLOCK column (allocated below), but its
            // FDMA per-(sat,signal) PHASE BIAS is unphysical (the receiver group-
            // delay-vs-frequency response is per-receiver/per-antenna), so the
            // phase-bias branch is skipped for GLONASS.  The receiver group delay
            // is carried by the per-FCN-slot receiver IFB as a local nuisance.
            // When the flag is off, GLONASS is excluded entirely (legacy default).
            if (obs.system == System::GLONASS && !config_.enableGlonassNetwork())
                continue;

            // Mirror the obs-level admission gates in computeMeasurementUpdate's
            // row-assembly loop, so every allocated shared column is written by at
            // least one row this epoch (no phantom all-zero columns → no spurious
            // null-space dimension for the network rank gate to trip on).
            if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) continue;
            if (obs.range < 1.5e7 || !std::isfinite(obs.range))         continue;
            if (obs.elevation < config_.getElevationMaskDeg())          continue;
            if (obs.los.norm() < 0.5 || !obs.los.allFinite())           continue;
            if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w))            continue;

            bool sat_usable = false;
            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeasurement(obs, ref_type, no_excl, "")) continue;
                if (ref_type.find('_') != std::string::npos) continue;  // raw signals only
                sat_usable = true;

                // Per-(sat,signal) phase bias on raw phase rows.  Skipped for
                // GLONASS (code-only network scope): a shared per-(sat,signal)
                // satellite phase bias is unphysical for FDMA, so GLONASS phase
                // observations contribute to the satellite CLOCK only (the float
                // ambiguity absorbs the per-receiver phase nuisance).
                if (config_.estimateSatPhaseBiases()
                    && obs.system != System::GLONASS
                    && classifyMeasurement(ref_type) == MeasurementType::Phase) {
                    // Per-station phase-bias exclusion (network.phase_bias_exclude_unmatched_antenna):
                    // an unmatched-receiver-antenna station gets NO phase-bias state for ANY
                    // signal — its uncalibrated (zero receiver PCV) carrier phase must not leak
                    // into the shared satellite phase biases.  Same withhold-not-drop semantics
                    // as the coverage gate below: the phase row is still built (feeding the sat
                    // clock and the local float ambiguity), only the shared bias column is
                    // withheld.  Default false ⇒ byte-identical.
                    if (phase_bias_emission_excluded_)
                        continue;
                    // Phase-bias coverage gate (network.phase_bias_min_station_fraction):
                    // a signal too sparsely tracked across the network gets NO phase-bias
                    // state — the float ambiguity absorbs the bias and the signal is not
                    // AR-eligible.  The observation is still used (the phase row is built);
                    // only the shared bias column is withheld.  Empty allowlist = admit all.
                    if (!phaseBiasAdmitted(PppUtils::getSystemString(obs.satellite), ref_type))
                        continue;
                    const std::string pk =
                        StateKeys::makeSatPhaseBiasKey(obs.satellite, ref_type);
                    if (!allowed_phase_keys.empty() && !allowed_phase_keys.count(pk))
                        continue;
                    // CB-ADM phase co-gate (symmetric with the code sponge): when
                    // the run-latched census QUARANTINED this signal's code pair,
                    // withhold its shared PHASE bias too — the phase folds into each
                    // station's float ambiguity exactly as the code folds into the
                    // local sponge, so a 2-station signal leaves the shared system on
                    // BOTH families and cannot orphan a pure-PB near-null gauge (the
                    // measured code-only-quarantine failure).  Gated at STATE
                    // ALLOCATION so getSatPhaseBiasStates()/shared_phase_keys stay
                    // consistent everywhere (a registration-site gate breaks the
                    // "every shared_phase_key is a column" invariant).  Companion
                    // code key = same sat + L<band><attr> -> C<band><attr>.  Null
                    // pointer (flag off / census inactive) ⇒ byte-identical.
                    if (cbadm_admitted_code_keys) {
                        std::string csig = ref_type;
                        if (!csig.empty()) csig[0] = 'C';
                        if (!cbadm_admitted_code_keys->count(obs.satellite + "|" + csig))
                            continue;
                    }
                    required_phase_keys.insert(pk);
                    if (state_manager_.getSatPhaseBiasIndex(pk) < 0)
                        state_manager_.addSatPhaseBiasState(pk, 0.0);
                }

                // Per-(sat,signal) CODE bias (OSB) on raw code rows (W6).  Promoted
                // to a shared border column for GPS + Galileo only — FDMA makes a
                // single shared per-(sat,signal) code OSB unphysical for GLONASS, so
                // its receiver group delay stays the per-FCN-slot rx IFB and it
                // contributes to the satellite CLOCK only.  Allocated at value 0:
                // like the clock and phase bias this is a SHARED state the network
                // solve treats as absolute (lin point 0, never written back), so the
                // innovation carries the full bias and the joint solve recovers it.
                // The +1 Jacobian is written by buildJacobianRow's sat_bias_term
                // (gated on estimateSatBiases(), forced on with this flag).
                if (config_.estimateNetworkSatCodeBias()
                    && obs.system != System::GLONASS
                    && classifyMeasurement(ref_type) == MeasurementType::Code) {
                    const std::string cbk = obs.satellite + "|" + ref_type;  // matches sat_bias_term key
                    required_code_keys.insert(cbk);
                    if (state_manager_.getSatBiasIndex(cbk) < 0)
                        state_manager_.addSatBiasState(cbk, 0.0);
                }
            }

            // One clock per satellite with any usable raw measurement.
            if (config_.estimateSatClocks() && sat_usable) {
                const std::string ck = StateKeys::makeSatClockKey(obs.satellite);
                required_clock_keys.insert(ck);
                if (state_manager_.getSatClockIndex(ck) < 0)
                    state_manager_.addSatClockState(ck, 0.0);
            }
        }

        // Drop stale shared states (satellite/signal no longer usable this epoch),
        // so the per-station column set is exactly this epoch's required set.
        if (config_.estimateSatClocks())
            state_manager_.pruneSatClockStatesExcept(required_clock_keys);
        if (config_.estimateSatPhaseBiases())
            state_manager_.pruneSatPhaseBiasStatesExcept(required_phase_keys);
        if (config_.estimateNetworkSatCodeBias())
            state_manager_.pruneSatBiasStatesExcept(required_code_keys);
    }

    // ===================================================================
    // Measurement filtering
    // ===================================================================

    bool MeasurementModel::isUsableMeasurement(const PppObservation& obs,
        const std::string& ref_type,
        const std::set<std::string>& excluded,
        const std::string& epoch_prefix) const {
        if (ref_type.empty()) return false;

        if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()
            && !config_.enableGlonassBssd()) {
            return false;
        }

        // When BSSD is active, exclude code/phase/Doppler measurements on
        // signals not tracked by the pivot satellite.  Such measurements
        // cannot be same-signal differenced and would be rejected by
        // BssdTransform's purity rule anyway; filtering here is an upstream
        // optimization.  Doppler is included — under BSSD it is SD-transformed
        // alongside code/phase, which is what cancels the receiver clock-rate
        // and ISBR in same-system pairs.
        if (!bssd_pivot_signals_.empty() && ref_type.size() >= 2
            && (ref_type[0] == 'C' || ref_type[0] == 'L' || ref_type[0] == 'D')) {
            const std::string cons = PppUtils::getSystemString(obs.satellite);
            auto cit = bssd_pivot_signals_.find(cons);
            if (cit != bssd_pivot_signals_.end()
                && !cit->second.count(ref_type)) return false;
        }

        const MeasurementType mt = classifyMeasurement(ref_type);

        if (mt != MeasurementType::Code && mt != MeasurementType::Phase &&
            !(mt == MeasurementType::Doppler && config_.estimateVelocity()))
            return false;

        const bool is_uncombined = config_.isUncombinedEnabled();
        const bool has_underscore = (ref_type.find('_') != std::string::npos);

        if (mt == MeasurementType::Doppler) {
            if (has_underscore) return false;
        } else if (is_uncombined) {
            if (has_underscore) return false;
        } else {
            if (!has_underscore) return false;
        }

        if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type))
            return false;

        const std::string excl_key = obs.satellite + "_" + ref_type;
        if (excluded.count(excl_key)) {
            log_manager_.getLogger("measurement_model")->debug(
                "{}Excluding previously rejected measurement: {}", epoch_prefix, excl_key);
            return false;
        }

        return true;
    }

    // ===================================================================
    // Jacobian + innovation assembly
    // ===================================================================

    bool MeasurementModel::buildJacobianRow(const PppObservation& obs,
        const std::string& ref_type,
        const Eigen::VectorXd& x,
        size_t state_size,
        Eigen::VectorXd& jacobian,
        double& predicted) const {

        const MeasurementType mtype = classifyMeasurement(ref_type);
        const bool is_phase = (mtype == MeasurementType::Phase);
        const bool is_doppler = (mtype == MeasurementType::Doppler);
        const bool is_uncombined = config_.isUncombinedEnabled();
        const double mapping_wet = obs.mf_w;

        jacobian = Eigen::VectorXd::Zero(state_size);

        // Doppler measurements
        if (is_doppler) {
            if (!state_manager_.hasVelocity()) return false;

            jacobian.segment<3>(state_manager_.getVelocityIndex()) = -obs.los;

            const Eigen::Vector3d rx_vel = x.segment<3>(state_manager_.getVelocityIndex());
            predicted = obs.modeled.at(ref_type) - obs.los.dot(rx_vel);

            // Receiver clock-rate partial — Doppler observes the GPS-referenced
            // rate (c · dδt_r/dt) with unit coefficient.  Without this term the
            // receiver oscillator drift has nowhere to go but the velocity
            // state, contaminating position via the F[pos, vel] = dt coupling.
            if (state_manager_.hasClockRate()) {
                const size_t cr_idx = state_manager_.getClockRateIndex();
                jacobian(cr_idx) = 1.0;
                predicted += x(cr_idx);
            }

            // Inter-system clock-rate offset (ISBR) for non-GPS observations.
            // Mirrors the ISB partial in the code/phase branch — total observed
            // rate on a non-GPS Doppler is (global clock-rate + ISBR_sys).
            const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
            if (!sys_name.empty()) {
                const size_t isbr_idx = state_manager_.getSystemTimeRateOffsetIndex(sys_name);
                if (isbr_idx < state_size) {
                    jacobian(isbr_idx) = 1.0;
                    predicted += x(isbr_idx);
                }
            }

            if (config_.tightlyCoupledImu() && state_manager_.hasImuBiases()) {
                const Eigen::Vector3d f_e = tc_C_b_e_ * tc_f_b_;
                Eigen::Matrix3d skew_fe;
                skew_fe <<       0.0, -f_e(2),  f_e(1),
                            f_e(2),      0.0, -f_e(0),
                           -f_e(1),  f_e(0),      0.0;

                const Eigen::Vector3d att_partial = -skew_fe.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAttitudeIndex()) = att_partial;

                const Eigen::Vector3d ba_partial = -tc_C_b_e_.transpose() * obs.los;
                jacobian.segment<3>(state_manager_.getAccelBiasIndex()) = ba_partial;
            }

            return true;
        }

        // Position partial: -LOS
        jacobian.segment<3>(state_manager_.getPositionIndex()) = -obs.los;

        // Troposphere wet delay
        jacobian(state_manager_.getTropoIndex()) = mapping_wet;

        // Receiver clock.  Under BSSD the clock cancels in SD and has no
        // state — skip the Jacobian write and treat the clock contribution
        // as zero (BssdTransform would have zeroed this column anyway).
        double rx_clk = 0.0;
        if (state_manager_.hasClock()) {
            jacobian(state_manager_.getClockIndex()) = 1.0;
            rx_clk = x(state_manager_.getClockIndex());
        }

        // Phase clock offset
        double phase_clk_value = 0.0;
        if (is_phase && state_manager_.hasPhaseClock()) {
            jacobian(state_manager_.getPhaseClockIndex()) = 1.0;
            phase_clk_value = x(state_manager_.getPhaseClockIndex());
        }

        // Tropospheric gradients
        double tropo_grad_term = 0.0;
        if (state_manager_.hasTropoGradients()) {
            const double el_rad = obs.elevation * Constants::PI / 180.0;
            const double az_rad = obs.azimuth * Constants::PI / 180.0;
            const double sin_el = std::sin(el_rad);
            const double tan_el = std::tan(el_rad);
            const double mf_grad = 1.0 / (sin_el * tan_el + 0.0032);
            const double cos_az = std::cos(az_rad);
            const double sin_az = std::sin(az_rad);

            jacobian(state_manager_.getTropoGradientNorthIndex()) = mf_grad * cos_az;
            jacobian(state_manager_.getTropoGradientEastIndex())  = mf_grad * sin_az;

            tropo_grad_term = mf_grad * (x(state_manager_.getTropoGradientNorthIndex()) * cos_az
                                        + x(state_manager_.getTropoGradientEastIndex()) * sin_az);
        }

        // Inter-system time offset
        const std::string sys_name = StateInitializer::systemToIsbName(obs.system);
        double time_offset_value = 0.0;
        if (!sys_name.empty()) {
            const size_t isb_idx = state_manager_.getSystemTimeOffsetIndex(sys_name);
            if (isb_idx < state_size) {
                jacobian(isb_idx) = 1.0;
                time_offset_value = x(isb_idx);
            }
        }

        // Per-satellite ionosphere
        double iono_term = 0.0;
        if (is_uncombined) {
            const int iono_idx = state_manager_.getIonosphereIndex(obs.satellite);
            if (iono_idx >= 0 && iono_idx < static_cast<int>(state_size)) {
                const double freq = ppp_utils_.getFrequency(ref_type, obs.system, obs.satellite);
                if (freq > 0.0) {
                    const double f_ref = Constants::GPS_L1_FREQ;
                    const double alpha = (f_ref * f_ref) / (freq * freq);
                    const double iono_coeff = (is_phase ? -1.0 : 1.0) * alpha;
                    jacobian(iono_idx) = iono_coeff;
                    iono_term = iono_coeff * x(iono_idx);
                }
            }
        }

        // Receiver bias � DCB / IFB (code only, uncombined)
        double code_bias_term = 0.0;
        if (config_.estimateIfb() && is_uncombined && !is_phase) {
            const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
            if (std::isfinite(bias.coefficient) && !bias.state_key.empty()) {
                const int cb_idx = state_manager_.getCodeBiasIndex(bias.state_key);
                if (cb_idx >= 0 && cb_idx < static_cast<int>(state_size)) {
                    jacobian(cb_idx) = bias.coefficient;
                    code_bias_term = bias.coefficient * x(cb_idx);
                }
            }
        }

        // Per-satellite signal bias (code-only: phase ambiguity absorbs sat phase bias)
        double sat_bias_term = 0.0;
        if (config_.estimateSatBiases()) {
            const bool is_if = (ref_type.find('_') != std::string::npos);
            const bool applicable = is_uncombined
                ? (!is_if && !is_phase)
                : (is_if && !is_phase);
            if (applicable) {
                const std::string sb_key = obs.satellite + "|" + ref_type;
                const int sb_idx = state_manager_.getSatBiasIndex(sb_key);
                if (sb_idx >= 0 && sb_idx < static_cast<int>(state_size)) {
                    jacobian(sb_idx) = 1.0;
                    sat_bias_term = x(sb_idx);
                }
            }
        }

        // Per-satellite signal PHASE bias (network producer only).  On the
        // single-receiver user side the satellite phase bias is imported as a
        // known SSR correction (below) or absorbed into the float ambiguity; the
        // network ESTIMATES it as an explicit per-(sat,signal) state on
        // undifferenced phase rows (Odijk Eq. 4 — it carries the network pivot-
        // receiver ambiguity lump).  Co-linear with the per-receiver float
        // ambiguity on a single row — separable only ACROSS stations (W4).  This
        // only CONSUMES an already-present column (creation/ownership is the
        // network estimator's); gated on estimateSatPhaseBiases() (default off),
        // so it is inert in every other mode.
        double sat_phase_bias_term = 0.0;
        if (config_.estimateSatPhaseBiases() && is_phase && is_uncombined
            && ref_type.find('_') == std::string::npos) {
            const std::string spb_key = StateKeys::makeSatPhaseBiasKey(obs.satellite, ref_type);
            const int spb_idx = state_manager_.getSatPhaseBiasIndex(spb_key);
            if (spb_idx >= 0 && spb_idx < static_cast<int>(state_size)) {
                jacobian(spb_idx) = 1.0;
                sat_phase_bias_term = x(spb_idx);
                // Card 3 (network.phase_datum_eq34): the per-(receiver,system,
                // signal) receiver phase-bias split state δ_{r,σ}.  It rides
                // EXACTLY the phase rows whose (sat,σ) has an UNGATED shared
                // phase-bias column (spb_idx >= 0 here — the D2-graph vertices;
                // a gated (sat,σ) keeps legacy per-station lumping, no RPB).  It
                // enters with +1 — the SAME sign as the ambiguity column it
                // splits (Odijk §4/§6: the phase row is …+δ_{r,σ}−δ^s_σ+z^s_r) —
                // so the receiver fractional phase bias gets its own estimable
                // state instead of lumping into the emitted satellite phase bias.
                // Folded into sat_phase_bias_term (both are +1 phase-bias-family
                // predicted terms) so the predicted assembly below is byte-
                // identical when the toggle is off.
                if (config_.networkPhaseDatumEq34()) {
                    const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(
                        PppUtils::getSystemString(obs.satellite), ref_type);
                    const int rpb_idx = state_manager_.getReceiverPhaseBiasIndex(rpb_key);
                    if (rpb_idx >= 0 && rpb_idx < static_cast<int>(state_size)) {
                        jacobian(rpb_idx) = 1.0;
                        sat_phase_bias_term += x(rpb_idx);
                    }
                }
            }
        }

        // Per-satellite CLOCK (network producer only).  The satellite orbit is
        // held FIXED from the precise/real-time product; the network ESTIMATES
        // an explicit per-satellite clock correction Δδtˢ (metres) to stay
        // consistent with the chosen S-system datum.  The observable carries the
        // satellite clock as −c·δtˢ and obs.modeled already removed the a-priori
        // product clock, so the residual-clock state enters with a −1 Jacobian on
        // BOTH code and phase rows (one clock per satellite, all signals).  Like
        // the bias states this CONSUMES an already-present column (the network
        // estimator creates/owns it); gated on estimateSatClocks() (default off),
        // inert in every other mode.  Single-station PPP would be rank-deficient
        // here (rx-clock ↔ common sat-clock), which is why it is network-only.
        double sat_clock_term = 0.0;
        if (config_.estimateSatClocks() && is_uncombined) {
            const std::string sc_key = StateKeys::makeSatClockKey(obs.satellite);
            const int sc_idx = state_manager_.getSatClockIndex(sc_key);
            if (sc_idx >= 0 && sc_idx < static_cast<int>(state_size)) {
                jacobian(sc_idx) = -1.0;
                sat_clock_term = -x(sc_idx);
            }
        }

        // SSR satellite biases (PPP-RTK): a KNOWN correction added to the
        // predicted observable (no state, no Jacobian).  getCodeBias /
        // getPhaseBias return the per-(sat, signal) value in metres; combined
        // iono-free keys like "C1W_C5X" return 0/nullopt and are skipped — SSR
        // biases are per raw signal.  No-op in post-processing (empty store).
        //
        // SIGN CONVENTION: the decoded bias is the satellite signal bias
        // PRESENT in the observation, so it is added to the clean model.  This
        // is the one convention that can only be confirmed empirically against
        // the provider's stream — if the float ambiguity fractions cluster near
        // 0.5 instead of 0, flip the corresponding sign constant and rebuild.
        //
        // NETWORK PRODUCER: gated OFF — the network estimates the satellite
        // code/phase bias as states above, so importing the same quantity here
        // would double-count and render those bias states unobservable.
        double ssr_bias_term = 0.0;
        if (config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network) {
            constexpr double kSsrCodeBiasSign  = +1.0;
            constexpr double kSsrPhaseBiasSign = +1.0;
            const auto& ssr_store = ssrStore();
            const bool   gate    = config_.enableSsrBiasMaxAge();  // A5: reject stale biases by obs-vs-record age
            // A5 stale-hold: the APPLY limit is hold_factor × max_age — the last
            // record keeps being applied over sub-limit staleness gaps so a provider
            // interval hovering near max_age produces no model step (and no
            // freshness-flip ambiguity reset; RealTimeEngine keys its flip detector
            // to this SAME limit).  A held bias is applied at full weight — the
            // network URA/staleness channel (consume_ura, ssr_max_age_s) is a
            // separate knob and does not cover this hold zone.
            const double max_age = config_.getSsrBiasMaxAge()
                                 * config_.getSsrBiasStaleHoldFactor();
            if (is_phase) {
                auto pb = gate ? ssr_store.getPhaseBias(obs.satellite, ref_type, obs.time, max_age)
                               : ssr_store.getPhaseBias(obs.satellite, ref_type);
                if (pb) ssr_bias_term = kSsrPhaseBiasSign * pb->bias_m;
            } else {
                ssr_bias_term = kSsrCodeBiasSign
                    * (gate ? ssr_store.getCodeBias(obs.satellite, ref_type, obs.time, max_age)
                            : ssr_store.getCodeBias(obs.satellite, ref_type));
            }
        }

        // Assemble predicted observable
        const double modeled = obs.modeled.at(ref_type);
        const double zwd = x(state_manager_.getTropoIndex());
        const double tropo_wet_term = mapping_wet * zwd;

        const Eigen::Vector3d pos_current = x.segment<3>(state_manager_.getPositionIndex());
        const double range_correction = -obs.los.dot(pos_current - obs.rx_pos_linearization);

        predicted = modeled + range_correction + rx_clk + phase_clk_value
            + tropo_wet_term + tropo_grad_term + time_offset_value
            + iono_term + code_bias_term + sat_bias_term + sat_phase_bias_term
            + sat_clock_term + ssr_bias_term;

        // Ambiguity (phase only)
        double amb_term = 0.0;
        if (is_phase) {
            const bool nlwl = (config_.getStateAmbiguityBasis()
                               == StateAmbiguityBasis::NlWl);
            if (!nlwl) {
                const std::string amb_key = obs.satellite + "_" + ref_type;
                const int amb_idx = state_manager_.getAmbiguityIndex(amb_key);
                if (amb_idx < 0) return false;
                jacobian(amb_idx) = 1.0;
                amb_term = x(amb_idx);
                predicted += amb_term;
            } else {
                // NlWl basis: state holds NL_ref (cycles, the L_ref carrier
                // ambiguity) and WL_(ref, b) = N_a - N_b in canonical
                // ascending-band form for each non-ref band b.  The phase
                // observation φ_b in meters is
                //     φ_b = ρ + λ_b · N_b
                // where N_b is reconstructed from state as
                //     N_b = NL_ref                                  (b == ref)
                //     N_b = NL_ref - sign · WL_<a>_<z>              (b != ref)
                // with sign = +1 when ref_band is the lower of the pair
                // and sign = -1 when ref_band is the higher one.
                if (ref_type.size() < 2) return false;
                auto rb_it = nlwl_ref_band_.find(obs.satellite);
                if (rb_it == nlwl_ref_band_.end()) return false;
                const char ref_band = rb_it->second;
                const char band = ref_type[1];
                const double lambda_b = ppp_utils_.getWavelength(
                    obs.system, ref_type, obs.satellite);
                if (!std::isfinite(lambda_b) || lambda_b <= 0.0) return false;

                const std::string nl_key = StateKeys::makeNlAmbiguityKey(
                    obs.satellite, ref_band);
                const int nl_idx = state_manager_.getAmbiguityIndex(nl_key);
                if (nl_idx < 0) return false;

                jacobian(nl_idx) = lambda_b;
                if (band == ref_band) {
                    amb_term = lambda_b * x(nl_idx);
                } else {
                    const std::string wl_key = StateKeys::makeWlAmbiguityKey(
                        obs.satellite, ref_band, band);
                    const int wl_idx = state_manager_.getAmbiguityIndex(wl_key);
                    if (wl_idx < 0) return false;
                    const double wl_sign = (ref_band < band) ? +1.0 : -1.0;
                    jacobian(wl_idx) = -lambda_b * wl_sign;
                    amb_term = lambda_b * (x(nl_idx) - wl_sign * x(wl_idx));
                }
                predicted += amb_term;
            }
        }

        // PHASE-DECOMP diagnostic (trace, "measurement_model"): the phase-row
        // mirror of CODE-DECOMP below — break the zero-difference predicted
        // phase into its term-level parts so a wandering float or a product
        // datum step can be attributed to the exact channel.  ssr_pbias is the
        // APPLIED network/SSR satellite phase-bias term (0 when the store has
        // no entry for this exact (sat, signal) — the probe term the
        // closed-loop datum attribution needed); sat_pbias is the network
        // producer's estimated per-(sat,signal) phase-bias STATE (inert
        // elsewhere); sat_clk the producer's residual satellite-clock state;
        // amb the float-ambiguity contribution in metres (NL/WL basis included).
        if (is_phase && !obs.satellite.empty()) {
            if (auto lg = log_manager_.getLogger("measurement_model");
                lg->should_log(spdlog::level::trace)) {
                const double observed_val =
                    obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
                lg->trace("PHASE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                          "zd_innov={:+.3f}m | rx_clk={:+.3f} phase_clk={:+.3f} "
                          "iono={:+.3f} ssr_pbias={:+.3f} sat_pbias={:+.3f} "
                          "sat_clk={:+.3f} amb={:+.3f}",
                          obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                          observed_val - predicted, rx_clk, phase_clk_value, iono_term,
                          ssr_bias_term, sat_phase_bias_term, sat_clock_term, amb_term);
            }
        }

        // CODE-DECOMP diagnostic (trace, "measurement_model"): for ANY code
        // signal, break the zero-difference predicted observable into its terms
        // so a hundreds-of-metre / kilometre residual can be attributed (GPS
        // IF-code km residuals on the live IGS-station path motivated lifting
        // this off the original Galileo-only gate).  rx_clk is the
        // receiver-clock STATE (0 in BSSD, where the between-sat SD removes the
        // physical clock — so zd_innov here still CARRIES it, equally on every
        // signal, and it cancels only after differencing); iono is the per-sat
        // slant-iono term (+alpha*I on code); ssr_cbias is the SSR code bias
        // matched by EXACT RINEX obs code (0 if the provider's set lacks this
        // code); sat_bias is the estimated per-(sat,signal) state; bia_cbias is
        // the external .BIA term (0 unless enable_code_phase_bias).
        if (!is_phase && !obs.satellite.empty()) {
            if (auto lg = log_manager_.getLogger("measurement_model");
                lg->should_log(spdlog::level::trace)) {
                const double observed_val =
                    obs.observed.count(ref_type) ? obs.observed.at(ref_type) : 0.0;
                lg->trace("CODE-DECOMP {} {} tow={:.1f} observed={:.3f} predicted={:.3f} "
                          "zd_innov={:+.3f}m | rx_clk={:+.3f} iono={:+.3f} ssr_cbias={:+.3f} "
                          "sat_bias={:+.3f} bia_cbias={:+.3f}",
                          obs.satellite, ref_type, obs.time.tow, observed_val, predicted,
                          observed_val - predicted, rx_clk, iono_term, ssr_bias_term,
                          sat_bias_term, code_bias_term);
            }
        }

        return true;
    }

    // ===================================================================
    // computeMeasurementUpdate � top-level orchestrator
    // ===================================================================

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(const std::vector<PppObservation>& observations,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {
        EpochMeasurement update;
        update.time = observations.empty() ? GnssTime{ 0, 0.0 } : observations[0].time;
        update.observations = observations;
        update.excluded_measurements = excluded_measurements;
        const std::string epoch_tag = TimeUtils::formatGnssTime(update.time);
        const std::string epoch_prefix;  // %E in spdlog pattern carries the timestamp

        if (observations.empty()) {
            log_manager_.getLogger("measurement_model")->error("{}No observations provided for measurement update, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- First-epoch initialization (delegated to StateInitializer) ---
        // Latch "ISB initialized" ONLY when the seed actually succeeded.  If GPS
        // (the ISB reference) is absent this epoch the call bails and returns
        // false; marking it done anyway would freeze every non-GPS ISB at 0 for
        // the whole session when GPS joins later.  Retry until GPS is present.
        if (!state_initializer_.isIsbInitialized()) {
            if (state_initializer_.initializeInterSystemBiases(observations, epoch_prefix))
                state_initializer_.markIsbInitialized();
        }
        if (!state_initializer_.isPhaseClockInitialized() && config_.estimatePhaseClock()) {
            state_initializer_.initializePhaseClockOffset(observations, epoch_prefix);
            state_initializer_.markPhaseClockInitialized();
        }

        // --- Differencing pre-pass: pivot handover + signal-coverage map ---
        // Delegated to the differencing strategy's preparePivots hook (no-op
        // for undifferenced PPP; BssdMeasurementBuilder runs the pivot handover
        // and builds the pivot signal-coverage map).  Invoked after the
        // bssd_pivot_signals_ reset and before ensureRequiredStates so that
        // ambiguity states for pivot-uncovered signals are never created.  The
        // handover's first-pass-vs-rebuild and forced-only semantics now live
        // in BssdMeasurementBuilder::preparePivots.
        bssd_pivot_signals_.clear();
        differencing_->preparePivots(*this, observations, epoch_prefix, update.time);

        // --- State pre-pass ---
        ensureRequiredStates(observations, epoch_prefix);

        // --- First-epoch code bias seeding ---
        if (!state_initializer_.isCodeBiasInitialized() && config_.estimateIfb()) {
            state_initializer_.initializeCodeBiases(observations, epoch_prefix);
            state_initializer_.markCodeBiasInitialized();
        }

        // --- Freeze pivot covariances (differencing strategy) ---
        differencing_->freezePivotCovariances(*this);

        // --- Validate state vector ---
        const Eigen::VectorXd x = state_manager_.getStateVector();
        if (x.size() < 5) {
            log_manager_.getLogger("measurement_model")->error("{}Invalid state vector size: {}, expected at least 5, mode={}",
                epoch_prefix, x.size(), modeLabel(config_));
            return update;
        }

        const size_t state_size = state_manager_.getStateSize();
        if (static_cast<size_t>(x.size()) != state_size) {
            log_manager_.getLogger("measurement_model")->error("{}State vector size {} does not match StateManager size {}, mode={}",
                epoch_prefix, x.size(), state_size, modeLabel(config_));
            return update;
        }

        // --- Count usable measurements ---
        size_t num_obs = 0;
        for (const auto& obs : observations)
            for (const auto& [ref_type, observed] : obs.observed)
                if (isUsableMeasurement(obs, ref_type, excluded_measurements, epoch_prefix))
                    ++num_obs;

        if (num_obs == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid observations found in residuals, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }

        // --- Allocate output ---
        update.innovations.resize(num_obs);
        update.R_v.resize(num_obs);
        update.H.resize(num_obs, state_size);
        update.H.setZero();
        update.satellite_ids.reserve(num_obs);
        update.signal_types.reserve(num_obs);

        // --- Assemble rows ---
        size_t row = 0;
        for (const auto& obs : observations) {
            // Validate stored geometry
            if (obs.sat_pos.norm() < 1.9e7 || !obs.sat_pos.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored satellite position for {} at TOW {:.3f}: norm={:.3e} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.sat_pos.norm(), modeLabel(config_));
                continue;
            }
            if (obs.range < 1.5e7 || !std::isfinite(obs.range)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored range for {} at TOW {:.3f}: {:.3f} m, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.range, modeLabel(config_));
                continue;
            }
            if (obs.elevation < config_.getElevationMaskDeg()) {
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Satellite {} below elevation mask at TOW {:.3f}, elevation={:.3f} deg, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.elevation, modeLabel(config_));
                continue;
            }
            if (obs.los.norm() < 0.5 || !obs.los.allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid stored LOS for {} at TOW {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, modeLabel(config_));
                continue;
            }
            if (obs.mf_w <= 0.0 || !std::isfinite(obs.mf_w)) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid wet mapping function for {} at TOW {:.3f}: {:.3f}, mode={}",
                    epoch_prefix, obs.satellite, obs.time.tow, obs.mf_w, modeLabel(config_));
                continue;
            }

            for (const auto& [ref_type, observed] : obs.observed) {
                if (!isUsableMeasurement(obs, ref_type, excluded_measurements, epoch_prefix)) continue;

                try {
                    Eigen::VectorXd jacobian;
                    double predicted = 0.0;
                    if (!buildJacobianRow(obs, ref_type, x, state_size, jacobian, predicted))
                        continue;

                    update.innovations(row) = observed - predicted;
                    update.R_v(row) = obs.variances.at(ref_type);

                    // M4 (contract v2): when the network product carries a per-key
                    // COMBINED-correction σ and closed_loop.consume_combined_sigma is on,
                    // fold THAT σ (age-inflated) into this row's variance INSTEAD OF the
                    // individual URA + phase-bias-σ RSS below.  The combined σ is mm-level
                    // for a fresh (reconverging) satellite whose individual clock/bias σ
                    // are cm-level, so the row keeps its weight instead of being deweighted
                    // 10–100× off (Zhang 2021 / Psychas 2024b).  Using BOTH the combined σ
                    // AND the URA/B1b terms would DOUBLE-COUNT the shared clock variance and
                    // REINTRODUCE that very cliff, so an active combined row SUPPRESSES the
                    // two blocks below (guarded by !combined_active).  Per-key fallback to
                    // v1 when the token is absent — a too-stale correction fails the
                    // freshness gate → nullopt → combined_active false → the v1 URA path
                    // (which owns staleness) handles it — so mixed streams stay safe.  The
                    // product's rho is NOT applied here: the measurement R is diagonal (no
                    // off-diagonal code/phase slot); a correlated-measurement consumer is
                    // future work.  Off / non-network / producer rows ⇒ combined_active
                    // false ⇒ R byte-identical.
                    bool   combined_active = false;
                    double combined_var    = 0.0;
                    if (config_.networkConsumeCombinedSigma()
                        && config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network
                        && ref_type.size() >= 2) {
                        const auto&  ssr      = ssrStore();
                        const double clk_hold = config_.getSsrClockStaleHoldFactor();
                        const double window   = SsrCorrectionStore::clockHoldWindow(
                            config_.getSsrMaxAge(), clk_hold);
                        // Apply the combined σ only to a FRESH network correction.  A
                        // stale-HELD clock (RTA-9(b): content age in (max_age, hold_window])
                        // keeps the v1 URA path below, so its ZOH clock-staleness growth
                        // (q²·dt³/3) is NOT lost — combined_var carries no staleness term
                        // when the producer advertised no agek, and a held clock must never
                        // be consumed at full weight.  agek covers latency WITHIN the fresh
                        // window; beyond it the conservative v1 staleness model owns the
                        // weighting.  With clk_hold ≤ 1 (default) every accepted clock is
                        // Fresh, so this never changes the common case.
                        bool clock_fresh = true;
                        if (clk_hold > 1.0) {
                            if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                                const double age = TimeUtils::continuousGpsSeconds(obs.time)
                                                 - TimeUtils::continuousGpsSeconds(*ep);
                                clock_fresh = SsrCorrectionStore::classifyClockAge(
                                                  age, config_.getSsrMaxAge(), clk_hold)
                                              == SsrCorrectionStore::ClockAgeRung::Fresh;
                            }
                        }
                        double sig_c = 0.0;
                        if (ref_type[0] == 'L') {          // PHASE row: combined phase σ (sigc)
                            const bool   pb_gate    = config_.enableSsrBiasMaxAge();
                            const double pb_max_age = config_.getSsrBiasMaxAge()
                                                    * config_.getSsrBiasStaleHoldFactor();
                            auto pb = pb_gate
                                ? ssr.getPhaseBias(obs.satellite, ref_type, obs.time, pb_max_age)
                                : ssr.getPhaseBias(obs.satellite, ref_type);
                            if (pb && pb->combined_sigma_m > 0.0
                                   && std::isfinite(pb->combined_sigma_m))
                                sig_c = pb->combined_sigma_m;
                        } else if (ref_type[0] == 'C') {   // CODE row: combined code σ (sigcc, per-sat)
                            if (auto oc = ssr.getOrbitClock(obs.satellite, obs.time, window);
                                oc && oc->has_clock && oc->combined_code_sigma_m > 0.0
                                   && std::isfinite(oc->combined_code_sigma_m))
                                sig_c = oc->combined_code_sigma_m;
                        }
                        if (sig_c > 0.0 && clock_fresh) {
                            // Age inflation σc²·(1 + agek·Δt_age²): Δt_age is the correction
                            // content age (obs.time − SSR content epoch), the same dt the URA
                            // ZOH term uses.  agek 0 (producer advertised none) ⇒ plain σc².
                            double       infl = 1.0;
                            const double agek = ssr.combinedSigmaAgeCoeff();
                            if (agek > 0.0) {
                                if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                                    const double dt = TimeUtils::continuousGpsSeconds(obs.time)
                                                    - TimeUtils::continuousGpsSeconds(*ep);
                                    if (dt > 0.0) infl = 1.0 + agek * dt * dt;
                                }
                            }
                            combined_var    = sig_c * sig_c * infl;
                            combined_active = true;
                        }
                    }

                    // B1: inflate R by the network URA on rows carrying this satellite's
                    // SSR corrections (an independent clock/bias error variance), plus the
                    // ZOH clock-staleness q²·dt³/3.  Gated (default off) + not on the
                    // producer's own rows ⇒ R byte-identical when off.
                    //
                    // RTA-9(b): the clock stale-hold rung EXTENDS this machinery (never a
                    // second inflation path).  With processing.ssr_clock_stale_hold_factor
                    // > 1: (1) the URA freshness window widens to the SAME clockHoldWindow
                    // the clock apply gate uses (a URA exactly as stale as the held clock
                    // still qualifies, and the q²·dt³/3 term then grows with the true dt);
                    // (2) when the URA channel does NOT deliver the staleness term for a
                    // stale-HELD clock (consume_ura off, no URA record for the sat, or
                    // ura_staleness_q2 explicitly 0), the same q²·dt³/3 form is applied
                    // with a documented fallback q² so a held clock is never consumed at
                    // full weight.  Factor 0 (default) leaves the original branch exactly.
                    {
                        const double clk_hold = config_.getSsrClockStaleHoldFactor();
                        const bool consumer_rows =
                            config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network;
                        if ((config_.networkConsumeUra() || clk_hold > 1.0) && consumer_rows
                            && !combined_active) {   // M4: combined σ supersedes URA on this row
                            const auto& ssr = ssrStore();
                            const double max_age = config_.getSsrMaxAge();
                            // P1-3: gate URA freshness on the SAME window as the clock/orbit
                            // overlay (getUra's own default is a looser 60 s — a URA up to
                            // 60 s stale would qualify a clock the store rejects at 30 s).
                            // RTA-9(b) widens BOTH windows together via clockHoldWindow.
                            const double window =
                                SsrCorrectionStore::clockHoldWindow(max_age, clk_hold);
                            // Content-epoch age — the dt the clock polynomial extrapolates
                            // over and the ZOH variance grows with.
                            double dt = 0.0;
                            bool   have_dt = false;
                            if (auto ep = ssr.getSsrEpoch(obs.satellite)) {
                                dt = TimeUtils::continuousGpsSeconds(obs.time)
                                   - TimeUtils::continuousGpsSeconds(*ep);
                                have_dt = true;
                            }
                            const double q2 = config_.networkUraStalenessQ2();
                            double add_var = 0.0;
                            bool staleness_folded = false;   // ZOH term already delivered by the URA channel
                            if (config_.networkConsumeUra()) {
                                if (auto ura = ssr.getUra(obs.satellite, obs.time, window)) {
                                    add_var += (*ura) * (*ura);
                                    if (q2 > 0.0 && have_dt && dt > 0.0) {
                                        add_var += q2 * dt * dt * dt / 3.0;  // ZOH extrapolation var
                                        staleness_folded = true;
                                    }
                                }
                            }
                            // RTA-9(b) fallback rung: a stale-HELD clock (age in
                            // (max_age, window]) whose staleness the URA channel did not
                            // fold gets the same q²·dt³/3 growth.  Fallback q² = 1.5e-7
                            // m²/s³ — the network.clock_model_q2 default (the white-FM
                            // random-walk density the producer's own clock model assumes),
                            // so the held-clock variance grows at the rate the product's
                            // clock is modelled to wander.  Only rows whose satellite
                            // actually carries an applied (held) SSR clock qualify.
                            if (clk_hold > 1.0 && !staleness_folded && have_dt
                                && SsrCorrectionStore::classifyClockAge(dt, max_age, clk_hold)
                                       == SsrCorrectionStore::ClockAgeRung::StaleHeld) {
                                if (auto oc = ssr.getOrbitClock(obs.satellite, obs.time, window);
                                    oc && oc->has_clock) {
                                    constexpr double kSsrClockHoldFallbackQ2 = 1.5e-7;  // m²/s³
                                    const double q2_eff = q2 > 0.0 ? q2 : kSsrClockHoldFallbackQ2;
                                    add_var += q2_eff * dt * dt * dt / 3.0;
                                }
                            }
                            if (add_var > 0.0) update.R_v(row) += add_var;
                        }
                    }

                    // B1b (defect-2 lever B, Tienstra §6.6): inflate a PHASE row's
                    // measurement variance by the network phase-bias correction σ² — the
                    // phase-bias block of Qŷ_B1 the clock-only URA channel omits.  The
                    // consumer otherwise applies phase-bias corrections as if EXACT; a
                    // wandering bias (the named defect-2 driver) is down-weighted instead.
                    // Per-(sat,signal) via getPhaseBias; IF rows return nullopt (per-signal
                    // biases only) and are untouched.  Producer rows are excluded.  σ is 0
                    // for RTCM/IGS-SSR biases and when emit_phase_bias_sigma is off, so
                    // OFF (or a non-network stream) ⇒ R byte-identical.
                    if (config_.networkConsumePhaseBiasSigma() && !combined_active
                        && ref_type.size() >= 2 && ref_type[0] == 'L'
                        && config_.getProcessingMode() != ProcessingMode::PPP_RTK_Network) {
                        // T3e: use the SAME freshness decision as the apply site
                        // (buildJacobianRow ~1482) so R is inflated by the σ of exactly the
                        // bias that was applied — not one the apply path already aged out (or,
                        // at the hold boundary, the reverse).  gate/max_age are recomputed here
                        // (out of scope in this function).  A5 gate OFF ⇒ plain overload ⇒
                        // byte-identical to before.
                        const bool   pb_gate    = config_.enableSsrBiasMaxAge();
                        const double pb_max_age = config_.getSsrBiasMaxAge()
                                                * config_.getSsrBiasStaleHoldFactor();
                        auto pb = pb_gate
                            ? ssrStore().getPhaseBias(obs.satellite, ref_type, obs.time, pb_max_age)
                            : ssrStore().getPhaseBias(obs.satellite, ref_type);
                        if (pb && pb->sigma_m > 0.0 && std::isfinite(pb->sigma_m)) {
                            update.R_v(row) += pb->sigma_m * pb->sigma_m;
                        }
                    }

                    // M4 (contract v2): apply the combined-correction variance for this row
                    // (the URA + B1b additions above were suppressed by !combined_active).
                    if (combined_active)
                        update.R_v(row) += combined_var;

                    // Downweight code on signals without BIA coverage
                    if (!config_.estimateSatBiases()
                        && config_.noBiaCodeDownweight() > 1.0
                        && ref_type.size() >= 2 && ref_type[0] == 'C') {
                        const bool is_if = (ref_type.find('_') != std::string::npos);
                        // A signal the live SSR stream corrects is NOT missing
                        // bias coverage — ssr_bias_term loads the row — so it must
                        // not be downweighted as if uncorrected.  (Raw signals
                        // only; IF rows get ssr_bias_term=0, so the IF branch is
                        // left on the .BIA check alone.)
                        const bool missing_bia = is_if
                            ? !ifCombHasFullBia(obs.satellite, ref_type)
                            : (config_.isUncombinedEnabled()
                               && !biaCoversSignal(obs.satellite, ref_type)
                               && !ssrCoversCodeSignal(obs.satellite, ref_type, obs.time));
                        if (missing_bia)
                            update.R_v(row) *= config_.noBiaCodeDownweight();
                    }

                    update.H.row(row) = jacobian;
                    update.satellite_ids.push_back(obs.satellite);
                    update.signal_types.push_back(ref_type);

                    if (!std::isfinite(update.innovations(row)) || !std::isfinite(update.R_v(row))) {
                        update.innovations(row) = 0.0;
                        update.R_v(row) = 1e6;
                    }

                    log_manager_.getLogger("measurement_model")->trace(
                        "{}Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}",
                        epoch_prefix, obs.satellite, ref_type,
                        observed, obs.modeled.at(ref_type), predicted, update.innovations(row));

                    row++;
                }
                catch (const std::exception&) {
                    continue;
                }
            }
        }

        // --- Finalize ---
        if (row == 0) {
            log_manager_.getLogger("measurement_model")->error("{}No valid measurement updates computed, mode={}",
                epoch_prefix, modeLabel(config_));
            return update;
        }
        if (row < num_obs) {
            log_manager_.getLogger("measurement_model")->warn("{}Processed {} observations out of {} expected, mode={}",
                epoch_prefix, row, num_obs, modeLabel(config_));
            update.innovations.conservativeResize(row);
            update.R_v.conservativeResize(row);
            update.H.conservativeResize(row, Eigen::NoChange);
            update.satellite_ids.resize(row);
            update.signal_types.resize(row);
        }
        update.bad_residual_count = 0;
        log_manager_.getLogger("measurement_model")->info("{}Computed measurement update: {} valid innovations, H ({}x{}), mode={}",
            epoch_prefix, row, update.H.rows(), update.H.cols(), modeLabel(config_));

        // --- Differencing S-transformation (strategy) ---
        differencing_->applyDifferencing(*this, update, epoch_prefix);

        return update;
    }

    // ===================================================================
    // Validation
    // ===================================================================

    bool MeasurementModel::validateMeasurementUpdate(const EpochMeasurement& measurement) const {
        const std::string epoch_tag = TimeUtils::formatGnssTime(measurement.time);
        const std::string epoch_prefix;  // %E in spdlog pattern carries the timestamp

        if (measurement.innovations.size() != measurement.R_v.size() ||
            measurement.innovations.size() != measurement.H.rows() ||
            measurement.H.cols() != state_manager_.getStateSize() ||
            measurement.innovations.size() != measurement.satellite_ids.size() ||
            measurement.innovations.size() != measurement.signal_types.size()) {
            log_manager_.getLogger("measurement_model")->error(
                "{}Measurement update dimension mismatch: innovations={}, R_v={}, H={}x{}, "
                "sat_ids={}, signal_types={}, state={}, mode={}",
                epoch_prefix,
                measurement.innovations.size(), measurement.R_v.size(),
                measurement.H.rows(), measurement.H.cols(),
                measurement.satellite_ids.size(), measurement.signal_types.size(),
                state_manager_.getStateSize(), modeLabel(config_));
            return false;
        }

        for (size_t i = 0; i < measurement.innovations.size(); ++i) {
            if (!std::isfinite(measurement.innovations(i)) || !std::isfinite(measurement.R_v(i))) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Invalid measurement values for {} {}: innovation={:.3f}, variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (measurement.R_v(i) <= 0.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-positive variance for {} {}: variance={:.3f}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.R_v(i), modeLabel(config_));
                return false;
            }
            if (std::abs(measurement.innovations(i)) > 100.0) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Large innovation for {} {}: innovation={:.3f} m, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i],
                    measurement.innovations(i), modeLabel(config_));
            }
        }

        for (int i = 0; i < measurement.H.rows(); ++i) {
            if (!measurement.H.row(i).allFinite()) {
                log_manager_.getLogger("measurement_model")->warn(
                    "{}Non-finite Jacobian row for {} {}, mode={}",
                    epoch_prefix,
                    measurement.satellite_ids[i], measurement.signal_types[i], modeLabel(config_));
                return false;
            }
        }

        log_manager_.getLogger("measurement_model")->trace(
            "{}Validated measurement update: {} measurements, mode={}",
            epoch_prefix, measurement.innovations.size(), modeLabel(config_));
        return true;
    }

    // ===================================================================
    // BSSD delegation
    // ===================================================================

    void MeasurementModel::handleBssdPivotHandover(
        const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix,
        bool forced_only)
    {
        pivot_manager_.handlePivotHandover(observations, state_manager_, epoch_prefix, forced_only);
    }

    // -------------------------------------------------------------------
    // DD pivot handover (S-transform) moved to DdPivotManager.
    // The legacy MeasurementModel::handleBaselinePivotHandover method
    // is gone; @c dd_pivot_manager_.selectPivots applies the handover
    // internally when the chosen pivot differs from the sticky one.
    // -------------------------------------------------------------------

    void MeasurementModel::freezePivotAmbiguityCovariance()
    {
        pivot_manager_.freezePivotAmbiguityCovariance(state_manager_);
    }

    void MeasurementModel::freezePivotIonoAndBiasCovariance()
    {
        pivot_manager_.freezePivotIonoAndBiasCovariance(state_manager_);
    }

    // =================================================================
    // Baseline (DD) measurement update — Slice 5e+
    //
    // Unified path: DD rows are built from two per-receiver PppModel
    // outputs rather than a separate geometry layer.  The per-receiver
    // residuals (observed - modeled) already incorporate Sagnac, emission
    // time, satellite clock, Shapiro, antenna PCO/PCV, solid tide, and
    // stochastic-model variances — DD cancels the parts that are common
    // between receivers (sat clock on short baselines, most propagation
    // terms) and leaves the position-dependent geometry + ambiguity
    // signal the filter needs.
    // =================================================================

    namespace {

        /// Build the DD ambiguity key: "DD|cons|sat_i|code|pivot".
        std::string makeDdAmbKey(const std::string& cons,
                                 const std::string& sat_i,
                                 const std::string& code,
                                 const std::string& pivot) {
            return "DD|" + cons + "|" + sat_i + "|" + code + "|" + pivot;
        }

        /// Map first char of a satellite id to constellation name.
        std::string ddConstellationOf(const std::string& sat) {
            if (sat.empty()) return {};
            switch (sat[0]) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return std::string(1, sat[0]);
            }
        }

        /// Constellation string -> System enum.  Used by IF row collapse
        /// to look up frequencies via PppUtils::getFrequency().  Returns
        /// System::Unknown for unrecognised names so the caller can skip.
        System ddSystemFromConsName(const std::string& cons) {
            if (cons == "GPS")     return System::GPS;
            if (cons == "Galileo") return System::Galileo;
            if (cons == "GLONASS") return System::GLONASS;
            if (cons == "BeiDou")  return System::BeiDou;
            if (cons == "QZSS")    return System::QZSS;
            if (cons == "SBAS")    return System::SBAS;
            return System::Unknown;
        }

    } // anonymous namespace

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {
        // Baseline DD is dispatched through the differencing strategy so the
        // build path is uniform with PPP / PPP-RTK; DdMeasurementBuilder calls
        // back into buildDdMeasurement (below) to assemble the rows.
        return differencing_->buildDoubleDifference(
            *this, rov_modeled, ref_modeled, current_state, excluded_measurements);
    }

    EpochMeasurement MeasurementModel::buildDdMeasurement(
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {

        EpochMeasurement em;
        // Seed the output exclusion set from the caller's carry set so
        // the KF outlier handlers (which use insert(), not assign) build
        // a cumulative union across rebuild passes.  Without this, the
        // caller's `carry_excluded = em.excluded_measurements` after the
        // KF returns would replace the previous pass's exclusions with
        // only this pass's additions, and a satellite with two bad
        // signals would oscillate (pass N excludes C1C; pass N+1 starts
        // fresh, excludes C2W, forgets C1C; pass N+2 sees C1C again).
        // Mirrors the PPP overload's behavior at line 595.
        em.excluded_measurements = excluded_measurements;

        auto klog = log_manager_.getLogger("measurement_model");
        // First-epoch diagnostic: reveal whether shared sats / signals exist.
        // Off after first call via a static flag so we don't spam the log.
        static bool first_dd_call = true;
        const bool diag = first_dd_call;
        if (diag) {
            klog->info("DD first-epoch diagnostic: rov_modeled={} sats, ref_modeled={} sats",
                       rov_modeled.size(), ref_modeled.size());
            if (!rov_modeled.empty()) {
                const auto& r0 = rov_modeled.front();
                std::string codes;
                for (const auto& [code, v] : r0.observed) { codes += code; codes += ' '; }
                klog->info("DD diag: rov[0]={} observed_codes=[{}]", r0.satellite, codes);
            }
            if (!ref_modeled.empty()) {
                const auto& e0 = ref_modeled.front();
                std::string codes;
                for (const auto& [code, v] : e0.observed) { codes += code; codes += ' '; }
                klog->info("DD diag: ref[0]={} observed_codes=[{}]", e0.satellite, codes);
            }
        }
        if (rov_modeled.empty() || ref_modeled.empty()) { first_dd_call = false; return em; }
        em.time = rov_modeled.front().time;

        const bool code_only = config_.getBaselineConfig().code_only;
        const double amb_init_std = config_.getInitialStd().ambiguity;
        const double amb_init_var =
            (amb_init_std > 0.0 ? amb_init_std * amb_init_std : 1e8);

        // Active-system + excluded-sat filters.
        const auto& active_vec = config_.getActiveSystems();
        const std::set<std::string> active_set(active_vec.begin(), active_vec.end());

        // Index by satellite.
        std::map<std::string, const PppObservation*> rov_by_sat, ref_by_sat;
        for (const auto& o : rov_modeled) rov_by_sat[o.satellite] = &o;
        for (const auto& o : ref_modeled) ref_by_sat[o.satellite] = &o;

        // Aliasing policy: when true, codes that share the same (System, Band)
        // but differ in tracking attribute (e.g. rov C2W vs ref C2C) are
        // paired across receivers.  When false, only exact-string code
        // matches are kept (legacy strict behavior).  Off-band cancellation
        // properties of DD do NOT depend on identical attributes — a small
        // sat-DCB residual remains on code DDs (absorbed by the ambiguity
        // float on phase DDs in non-code-only mode).  See Phase 2 plan for
        // DCB harmonization.
        const bool signal_aliasing = config_.getBaselineConfig().signal_aliasing;

        // Per-band canonical code chosen at one receiver: pick the
        // tracking attribute with the lowest @c signalAttributePriority().
        // Returns map keyed by (band, obs_type) → (rinex_code, priority).
        auto pickCanonical = [](const PppObservation& obs, const std::string& cons) {
            std::map<std::pair<FrequencyBand, char>,
                     std::pair<std::string, int>> best;
            for (const auto& [code, val] : obs.observed) {
                if (code.size() < 3) continue;
                const auto cls = classifySignal(cons, code);
                if (cls.band == FrequencyBand::Unknown) continue;
                if (cls.type != ObsType::Code && cls.type != ObsType::Phase) continue;
                const auto key = std::make_pair(cls.band, static_cast<char>(cls.type));
                const int prio = signalAttributePriority(cons, cls.band, cls.attribute);
                auto it = best.find(key);
                if (it == best.end() || prio < it->second.second) {
                    best[key] = { code, prio };
                }
            }
            return best;
        };

        // Group shared sats by constellation.  For each (sat, band, obs_type)
        // tuple shared between the two receivers, run the BRSD transform
        // (BrsdTransform::apply) to compute the between-receiver
        // residual + variance + rover-side LOS once.  Downstream BSSD
        // pivot-differencing then forms DD rows from BRSD entries.
        //
        // BRSD output replaces what used to be a thin SharedSignal
        // metadata struct: BrsdObservation already carries the same
        // (band, obs_type, rov_code, ref_code, brsd_aliased) metadata
        // plus the freshly-computed residual / variance / LOS, so the
        // canonical-code selection and BRSD math live in a single pass.
        struct SharedInfo {
            const PppObservation* rov{ nullptr };
            const PppObservation* ref{ nullptr };
            std::vector<BrsdObservation> brsd_signals;  ///< One BRSD per shared (band, obs_type).
            double avg_snr{ 0.0 };
            size_t num_sigs{ 0 };
        };
        std::map<std::string, std::map<std::string, SharedInfo>> by_cons;

        for (const auto& [sat, rov_ptr] : rov_by_sat) {
            const std::string cons = ddConstellationOf(sat);
            if (!active_set.empty() && active_set.find(cons) == active_set.end()) continue;
            if (config_.isSatelliteExcluded(sat)) continue;

            auto ref_it = ref_by_sat.find(sat);
            if (ref_it == ref_by_sat.end()) continue;
            const PppObservation* ref_ptr = ref_it->second;

            SharedInfo info;
            info.rov = rov_ptr;
            info.ref = ref_ptr;

            const auto rov_canon = pickCanonical(*rov_ptr, cons);
            const auto ref_canon = pickCanonical(*ref_ptr, cons);
            for (const auto& [k, rov_pair] : rov_canon) {
                auto rit = ref_canon.find(k);
                if (rit == ref_canon.end()) continue;
                const std::string& rov_code = rov_pair.first;
                const std::string& ref_code = rit->second.first;
                const bool aliased = (rov_code != ref_code);
                if (aliased && !signal_aliasing) continue;  // strict mode drops mismatches

                auto brsd = BrsdTransform::apply(*rov_ptr, *ref_ptr, cons,
                                                 rov_code, ref_code,
                                                 k.first, k.second);
                if (!brsd) continue;  // missing observed/modeled or NaN
                info.brsd_signals.push_back(std::move(*brsd));
            }

            // SNR averaging — any 'S*' RINEX code on the rover side.
            double snr_sum = 0.0; size_t snr_cnt = 0;
            for (const auto& [code, val] : rov_ptr->observed) {
                if (!code.empty() && code[0] == 'S') { snr_sum += val; ++snr_cnt; }
            }
            info.num_sigs = info.brsd_signals.size();
            info.avg_snr  = (snr_cnt > 0) ? (snr_sum / snr_cnt) : 0.0;
            if (info.num_sigs < 2) continue;
            by_cons[cons][sat] = std::move(info);
        }
        if (diag) {
            size_t total_shared = 0;
            size_t total_aliased = 0;
            for (const auto& [cons, sats] : by_cons) {
                klog->info("DD diag: constellation '{}' has {} shared sats with ≥2 signals", cons, sats.size());
                total_shared += sats.size();
                for (const auto& [sat, info] : sats) {
                    std::string codes;
                    for (const auto& b : info.brsd_signals) {
                        codes += b.rov_code;
                        if (b.brsd_aliased) {
                            codes += "->"; codes += b.ref_code; ++total_aliased;
                        }
                        codes += '(';
                        codes += bandLabel(b.band);
                        codes += ") ";
                    }
                    klog->info("DD diag:   {}: {} BRSD signals=[{}]", sat,
                               info.brsd_signals.size(), codes);
                }
            }
            klog->info("DD diag: {} total viable shared sats; signal_aliasing={}, aliased_signals={}",
                       total_shared, signal_aliasing, total_aliased);
        }
        if (by_cons.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — by_cons empty");
            first_dd_call = false;
            return em;
        }

        // --- Sticky pivot selection per constellation ---
        // Build the candidate map (cons → sat → ranking-info) from the
        // BRSD-populated by_cons structure and hand it to DdPivotManager.
        // The manager's selectPivots applies the sticky-or-rerank logic
        // and, on a pivot change, the S-transform handover (gated by
        // BaselineConfig::enable_pivot_handover) — preserving DD
        // ambiguity integer cycles across handovers.
        DdPivotManager::CandidateMap pivot_candidates;
        for (const auto& [cons, sats] : by_cons) {
            for (const auto& [sat, info] : sats) {
                pivot_candidates[cons][sat] = { info.avg_snr, info.num_sigs };
            }
        }
        const std::map<std::string, std::string> pivots =
            dd_pivot_manager_.selectPivots(pivot_candidates, state_manager_);

        if (diag) {
            for (const auto& [c, p] : pivots) {
                klog->info("DD diag: pivot for '{}' = '{}'", c, p);
            }
        }
        if (pivots.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — pivots empty");
            first_dd_call = false;
            return em;
        }

        // --- Collect DD rows ---
        // Each row carries both rover and reference RINEX codes.  When
        // aliasing is off they are equal (legacy behavior).  When on and
        // the receivers report different tracking attributes on the same
        // band, the row is built cross-attribute — residuals and variances
        // are looked up under each receiver's own code.
        //
        // @c integer_capable is true iff all four observations in the DD
        // (rov_i, rov_p, ref_i, ref_p) use the same RINEX code.  Only
        // those rows have receiver-bias and satellite-bias terms that
        // cancel pairwise; the float ambiguity is then a clean integer
        // multiple of the carrier wavelength and is eligible for AR.
        // Any other combination (rov/ref mismatch on this row, OR rover
        // code differs between sat_i and pivot, OR reference code does)
        // carries a non-integer DCB / IFB residual.
        // The DD row carries the BRSD-combined variance for the non-pivot
        // (var_brsd_i = var_rov_i + var_ref_i) and pivot (var_brsd_p =
        // var_rov_p + var_ref_p).  Diagonal R = var_brsd_i + var_brsd_p;
        // off-diagonal R between rows sharing a pivot = var_brsd_p alone.
        // Splitting along the BRSD seam (rather than four var_*_*
        // components) makes the variance composition trace cleanly back
        // to the BrsdObservation layer.
        struct DdRow {
            char obs_type{ 'C' };
            std::string sat_i, sat_p, cons;
            std::string rov_code, ref_code;
            FrequencyBand band{ FrequencyBand::Unknown };
            bool integer_capable{ true };
            Eigen::RowVector3d h_pos;
            double innov_pre{ 0.0 };
            double var_brsd_i{ 0.0 };  ///< BRSD variance on the non-pivot side (var_rov + var_ref).
            double var_brsd_p{ 0.0 };  ///< BRSD variance on the pivot side (var_rov + var_ref).
            ///< Rover-side wet mapping function values for the non-pivot and
            ///< pivot satellites.  Used to populate the H column for the
            ///< differential ZWD state when @c estimate_differential_tropo
            ///< is on.  Values are nearly band-independent so the IF
            ///< collapse just propagates the per-band values from r1.
            double mf_w_rov_i{ 0.0 };
            double mf_w_rov_p{ 0.0 };
            std::string group_key;
            int amb_idx{ -1 };
            double innov{ 0.0 };
        };
        std::vector<DdRow> rows;

        // ---- BSSD step: BRSD entries → DD rows ----
        // Hand the BRSD inputs and pivot map to BssdTransform::differenceBrsd,
        // which performs the BSSD differencing per (cons, band, obs_type)
        // group and returns BssdDdRow entries (residual, variance pair,
        // rover-side LOS pair, integer-capability flag, metadata).
        // MeasurementModel then layers on its private bookkeeping —
        // group_key for R off-diagonal correlation and the H_pos partial
        // derived from the LOS pair — to produce a DdRow.
        //
        // The differencing math now lives entirely inside BssdTransform;
        // the same class also serves PPP-RTK SD via apply().  See the
        // class header for the architectural notes on the two methods.
        {
            // Build a (cons, sat) → vector<BrsdObservation> view over
            // by_cons.  The static differenceBrsd signature wants this
            // shape so the structural dependency on MeasurementModel's
            // local SharedInfo struct stays decoupled.
            std::map<std::string,
                     std::map<std::string, std::vector<BrsdObservation>>> brsd_by_cons_sat;
            for (const auto& [cons, sats] : by_cons) {
                for (const auto& [sat, info] : sats) {
                    brsd_by_cons_sat[cons][sat] = info.brsd_signals;
                }
            }

            const auto bssd_rows = BssdTransform::differenceBrsd(
                brsd_by_cons_sat, pivots, excluded_measurements);

            rows.reserve(bssd_rows.size());
            for (const auto& br : bssd_rows) {
                DdRow r;
                r.obs_type        = br.obs_type;
                r.sat_i           = br.sat_i;
                r.sat_p           = br.sat_p;
                r.cons            = br.cons;
                r.rov_code        = br.rov_code;
                r.ref_code        = br.ref_code;
                r.band            = br.band;
                r.integer_capable = br.integer_capable;
                r.innov_pre       = br.innov_pre;
                r.var_brsd_i      = br.var_brsd_i;
                r.var_brsd_p      = br.var_brsd_p;
                // Geometry partial w.r.t. the rover position state.
                // Reference LOS doesn't appear because the reference is
                // not a state in baseline mode (its contribution already
                // cancelled in the BRSD layer feeding differenceBrsd).
                r.h_pos = -(br.los_rov_i - br.los_rov_p).transpose();
                // Wet mapping function values for the differential-ZWD H
                // column.  PppModel populates @c mf_w on each PppObservation
                // from the GMF/VMF wet mapping function at the rover's
                // elevation; nearly band-independent so any band's per-row
                // value is fine.  Default 0.0 if the satellite isn't in
                // rov_by_sat (shouldn't happen — both stations agreed on
                // the satellite at the BRSD layer — but be defensive).
                {
                    auto it_i = rov_by_sat.find(br.sat_i);
                    auto it_p = rov_by_sat.find(br.sat_p);
                    r.mf_w_rov_i = (it_i != rov_by_sat.end()) ? it_i->second->mf_w : 0.0;
                    r.mf_w_rov_p = (it_p != rov_by_sat.end()) ? it_p->second->mf_w : 0.0;
                }
                // Group key for R off-diagonal correlation: same pivot
                // variance contribution is shared by all non-pivot rows
                // on the same (cons, band, obs_type).
                r.group_key = br.cons + "|" + bandLabel(br.band)
                            + "|" + std::string(1, br.obs_type);
                rows.push_back(std::move(r));
            }
        }

        // --- Iono-free row collapse (long-baseline / educational mode) ---
        // When @c BaselineConfig::use_iono_free is on, replace the per-band
        // DD rows for each (sat_i, pivot, obs_type) group with a single
        // row formed from the dual-frequency ionosphere-free combination
        //
        //     y_IF = α·y_1 + β·y_2,   α = f1²/(f1²-f2²),  β = -f2²/(f1²-f2²)
        //
        // where (f1, y_1) is the higher-frequency band and (f2, y_2) is
        // the lower.  This eliminates the first-order ionospheric delay
        // — required for long baselines (>10-20 km) where the DD does
        // NOT cancel iono between receivers.  Cost: ~3x amplified code
        // noise on GPS L1+L2 (the variance scales by α²+β² ≈ 9.5), and
        // the ambiguity is no longer integer-cycle (it's a non-trivial
        // linear combination of N1 and N2 in cycles), so AR is excluded
        // for IF phase rows via @c integer_capable=false.  Sat-pairs
        // with fewer than two shared bands are dropped — IF needs two.
        // The H_pos block is unchanged: the geometry partial is the
        // same linear combination's derivative w.r.t. position, which
        // factors out of α and β (both scaled the same).
        if (config_.getBaselineConfig().use_iono_free && !rows.empty()) {
            // Group row indices by (cons, sat_i, sat_p, obs_type).
            std::map<std::string, std::vector<size_t>> if_groups;
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                const std::string gk = r.cons + "|" + r.sat_i + "|" + r.sat_p
                                     + "|" + std::string(1, r.obs_type);
                if_groups[gk].push_back(i);
            }

            std::vector<DdRow> if_rows;
            if_rows.reserve(if_groups.size());
            size_t n_dropped_single_band = 0;

            // Cap on IF noise amplification factor K = sqrt(α²+β²).  Same
            // physics and same threshold as PppModel::selectIonoFreePairs;
            // picking by K rather than by raw frequency rank avoids the
            // BeiDou trap where the two highest frequencies (B1C, B1I) sit
            // ~14 MHz apart and yield K ≈ 77.  Standard wide pairs (GPS
            // L1/L2 K≈3, BeiDou B1I/B3 K≈3.5) pass comfortably; narrow
            // pairs (B1C/B1I, B2x family) are rejected.
            constexpr double kMaxIfNoiseFactor = 10.0;
            size_t n_dropped_high_K = 0;

            for (const auto& [gk, idxs] : if_groups) {
                if (idxs.size() < 2) {
                    // Need two bands to cancel ionosphere; skip.
                    ++n_dropped_single_band;
                    continue;
                }
                // Pick the band pair within this sat-pair group with the
                // smallest noise amplification factor K, subject to K
                // staying under the cap.  For dual-frequency receivers
                // there's only one pair so this reduces to the prior
                // behavior; for triple-/all-frequency it picks the best
                // available combination.
                const System sys = ddSystemFromConsName(rows[idxs.front()].cons);
                double best_K = std::numeric_limits<double>::infinity();
                size_t best_hi = 0, best_lo = 0;
                bool found = false;
                for (size_t i = 0; i < idxs.size(); ++i) {
                    for (size_t j = i + 1; j < idxs.size(); ++j) {
                        const double fa = ppp_utils_.getFrequency(
                            rows[idxs[i]].rov_code, sys, rows[idxs[i]].sat_i);
                        const double fb = ppp_utils_.getFrequency(
                            rows[idxs[j]].rov_code, sys, rows[idxs[j]].sat_i);
                        if (!(std::isfinite(fa) && std::isfinite(fb))
                         || fa <= 0.0 || fb <= 0.0 || fa == fb) continue;
                        const double fas = fa * fa, fbs = fb * fb;
                        const double denom = fas - fbs;
                        if (std::abs(denom) < 1e-3 * fas) continue;
                        const double K = std::sqrt(fas * fas + fbs * fbs) / std::abs(denom);
                        if (K < best_K) {
                            best_K = K;
                            // hi-frequency index first
                            if (fa >= fb) { best_hi = idxs[i]; best_lo = idxs[j]; }
                            else          { best_hi = idxs[j]; best_lo = idxs[i]; }
                            found = true;
                        }
                    }
                }
                if (!found || best_K > kMaxIfNoiseFactor) {
                    ++n_dropped_high_K;
                    continue;
                }

                const DdRow& r1 = rows[best_hi];  // higher f
                const DdRow& r2 = rows[best_lo];  // lower f

                const double f1 = ppp_utils_.getFrequency(r1.rov_code, sys, r1.sat_i);
                const double f2 = ppp_utils_.getFrequency(r2.rov_code, sys, r2.sat_i);
                const double f1s   = f1 * f1;
                const double f2s   = f2 * f2;
                const double denom = f1s - f2s;
                const double alpha =  f1s / denom;
                const double beta  = -f2s / denom;
                const double a2 = alpha * alpha;
                const double b2 = beta  * beta;

                DdRow ifr;
                ifr.obs_type = r1.obs_type;
                ifr.sat_i    = r1.sat_i;
                ifr.sat_p    = r1.sat_p;
                ifr.cons     = r1.cons;
                ifr.band     = r1.band;            // documentation only
                // Concatenated codes serve as both the ambiguity-key code
                // segment and the trace signal_types token.
                ifr.rov_code = r1.rov_code + "+" + r2.rov_code;
                ifr.ref_code = r1.ref_code + "+" + r2.ref_code;
                ifr.h_pos    = r1.h_pos;           // identical for both bands
                // Wet mapping function is essentially frequency-independent;
                // the GMF/VMF wet partial depends on elevation only, so r1
                // and r2 agree to within a few PPM.  Propagate r1's values.
                ifr.mf_w_rov_i = r1.mf_w_rov_i;
                ifr.mf_w_rov_p = r1.mf_w_rov_p;
                ifr.innov_pre = alpha * r1.innov_pre + beta * r2.innov_pre;
                ifr.var_brsd_i = a2 * r1.var_brsd_i + b2 * r2.var_brsd_i;
                ifr.var_brsd_p = a2 * r1.var_brsd_p + b2 * r2.var_brsd_p;
                // Distinct group key so IF rows correlate with each other
                // (same pivot variance) but not with any per-band rows
                // that might appear in mixed scenarios (currently they
                // don't; rows is fully replaced below).
                ifr.group_key = ifr.cons + "|IF|" + std::string(1, ifr.obs_type);
                ifr.amb_idx   = -1;
                // IF ambiguity is c·(f1·N1 - f2·N2)/(f1²-f2²) — never an
                // integer cycle.  Force float-only so AR's exclusion set
                // catches it at @c getNonIntegerCapableDdAmbiguityKeys().
                ifr.integer_capable = false;
                if_rows.push_back(std::move(ifr));
            }

            if (diag) {
                klog->info("DD diag: IF row collapse: {} per-band rows -> {} IF rows "
                           "({} sat-pair groups dropped for <2 bands, "
                           "{} dropped because best-pair K > {:.1f})",
                           rows.size(), if_rows.size(), n_dropped_single_band,
                           n_dropped_high_K, kMaxIfNoiseFactor);
            }
            rows = std::move(if_rows);
        }

        if (rows.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — rows empty (no shared band/obs_type with the pivot)");
            first_dd_call = false;
            return em;
        }

        // --- Prune stale DD ambiguity states ---
        // Compute the set of DD ambiguity keys this epoch will reference
        // by reading the final post-collapse @c rows vector.  Each phase
        // row contributes one expected key built from its rov_code, which
        // is either a single RINEX code ("L1C") in per-band mode or the
        // concatenated IF label ("L1C+L2W") after iono-free collapse.
        // Any DD-keyed ambiguity in the state that's NOT in this set is
        // stale and removed.  This catches every leak path in one pass:
        //   - pivot change for a group (old pivot's keys vanish from the set)
        //   - rover or reference tracker switching codes mid-arc
        //   - satellite leaving view at either receiver
        //   - satellite added to excluded_satellites at runtime
        //   - iono-free toggle changing the code segment of every key
        //   - any other condition that produces no row this epoch
        // Must run BEFORE em.H column indices are sized below — the H
        // matrix uses state_manager.getStateSize(), and removeAmbiguity()
        // compacts the state vector and shifts dynamic-state indices.
        // Building the expected set from @c rows (rather than from the
        // pre-collapse @c by_cons / @c brsd_signals) ensures the expected
        // and added keys agree on whether the code segment is per-band or
        // an IF concatenation; otherwise every IF DD ambiguity is pruned
        // and re-added each epoch, losing covariance.
        {
            // Build the expected set as parsed (cons, sat_i, code, pivot) tuples
            // so the prune doesn't depend on string-formatting details of
            // makeDdAmbKey matching exactly what's stored.  Then walk the
            // ambiguity map, parse each DD-namespaced key, and remove any
            // tuple not in the expected set.
            using DdTuple = std::tuple<std::string, std::string, std::string, std::string>;
            std::set<DdTuple> expected;
            for (const auto& r : rows) {
                if (r.obs_type != 'L') continue;
                expected.emplace(r.cons, r.sat_i, r.rov_code, r.sat_p);
            }
            // Protect ambiguities for phase rows EXCLUDED this pass by the KF
            // rebuild loop.  An excluded row never enters @c rows, so its
            // (cons,sat_i,code,pivot) tuple is absent from @c expected above and
            // the prune below would delete the very ambiguity state the KF just
            // soft-reset — undoing the outlier-recovery reseed and forcing a
            // from-scratch reinit (innov_pre, amb_init_var) next epoch, which
            // loses the converged float + covariance.  Reconstruct each excluded
            // row's expected tuple under this epoch's pivot and keep it live.
            // The KF exclusion token is "sat_i_<rov_code>" (or "..._<rov>/<ref>"
            // when aliased); the DD ambiguity key uses rov_code.  Genuine leak
            // paths (sat dropout, code switch, pivot change) never appear in
            // @c excluded_measurements — those sats produce no row to exclude —
            // so this does not weaken the prune.
            for (const auto& ex : excluded_measurements) {
                const auto us = ex.find('_');
                if (us == std::string::npos || us + 1 >= ex.size()) continue;
                const std::string sat_i = ex.substr(0, us);
                std::string code = ex.substr(us + 1);
                const auto slash = code.find('/');
                if (slash != std::string::npos) code = code.substr(0, slash);  // rover code
                const std::string cons = ddConstellationOf(sat_i);
                auto pit = pivots.find(cons);
                if (pit == pivots.end()) continue;
                expected.emplace(cons, sat_i, code, pit->second);
            }
            std::vector<std::string> to_prune;
            for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
                const auto parts = StateKeys::parseDdAmbiguityKey(key);
                if (!parts) continue;
                if (expected.count({ parts->cons, parts->sat_i, parts->code, parts->pivot }))
                    continue;
                to_prune.push_back(key);
            }
            for (const auto& k : to_prune) state_manager_.removeAmbiguity(k);
            if (!to_prune.empty()) {
                log_manager_.getLogger("main_log")->info(
                    "DD: pruned {} stale ambiguity state(s) (sat dropout, code "
                    "switch, or pivot change)", to_prune.size());
            }
        }

        // --- Refresh the AR-exclusion-key snapshot ---
        // Any phase row whose four observations are NOT all the same
        // RINEX code carries a residual receiver IFB or satellite DCB
        // term that does not round to integer.  Those keys are exposed
        // to AmbiguityResolver::resolveDd via getNonIntegerCapableDdAmbiguityKeys()
        // so they remain float-only.  Rebuilt from scratch each epoch
        // from the current phase rows; code rows are skipped because
        // they do not create ambiguity states.
        non_integer_capable_dd_ambiguity_keys_.clear();
        for (const auto& r : rows) {
            if (r.obs_type != 'L') continue;
            if (r.integer_capable) continue;
            non_integer_capable_dd_ambiguity_keys_.insert(
                makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p));
        }

        // --- Ensure DD ambiguity states exist for phase rows ---
        if (!code_only) {
            for (auto& r : rows) {
                if (r.obs_type != 'L') continue;
                // Ambiguity key uses the rover's RINEX code.  When
                // aliasing is on and the rover and reference disagree on
                // tracking attribute, the float ambiguity will absorb the
                // residual sat-DCB; integer fixing requires DCB
                // harmonization (Phase 2 plan) to be valid.
                const std::string key = makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p);
                int aidx = state_manager_.getAmbiguityIndex(key);
                if (aidx < 0) {
                    state_manager_.addAmbiguity(key, r.innov_pre);
                    aidx = state_manager_.getAmbiguityIndex(key);
                    if (aidx >= 0) {
                        Eigen::MatrixXd P = state_manager_.getCovariance();
                        P(aidx, aidx) = amb_init_var;
                        state_manager_.setCovariance(P);
                    }
                }
                r.amb_idx = aidx;
            }
        }

        // --- Per-row innov (code = pre; phase = pre - amb_state) ---
        Eigen::VectorXd x;
        try {
            const size_t ss = state_manager_.getStateSize();
            x = (current_state.size() == static_cast<Eigen::Index>(ss))
                ? current_state
                : state_manager_.getStateVector();
        } catch (const std::exception& ex) {
            klog->error("DD state-vector fetch failed: {}", ex.what());
            first_dd_call = false;
            return em;
        } catch (...) {
            klog->error("DD state-vector fetch failed: unknown exception");
            first_dd_call = false;
            return em;
        }
        std::vector<size_t> kept;
        kept.reserve(rows.size());
        try {
            for (size_t i = 0; i < rows.size(); ++i) {
                auto& r = rows[i];
                if (code_only && r.obs_type == 'L') continue;
                if (r.obs_type == 'L') {
                    if (r.amb_idx < 0) continue;
                    if (r.amb_idx >= x.size()) {
                        klog->warn("DD row {} amb_idx={} out of range x.size={}", i, r.amb_idx, x.size());
                        continue;
                    }
                    r.innov = r.innov_pre - x(r.amb_idx);
                } else {
                    r.innov = r.innov_pre;
                }
                kept.push_back(i);
            }
        } catch (const std::exception& ex) {
            klog->error("DD innov loop failed: {}", ex.what());
            first_dd_call = false;
            return em;
        }
        const Eigen::Index m = static_cast<Eigen::Index>(kept.size());
        if (diag) {
            klog->info("DD diag: {} total DD rows built, {} kept after filter (code_only={})",
                       rows.size(), m, code_only);
        }
        if (m == 0) {
            if (diag) klog->info("DD diag: EARLY RETURN — m==0 (all rows filtered out)");
            first_dd_call = false;
            return em;
        }

        try {
            const size_t n_state = state_manager_.getStateSize();
            em.innovations = Eigen::VectorXd(m);
            em.H = Eigen::MatrixXd::Zero(m, static_cast<Eigen::Index>(n_state));
            em.R_full = Eigen::MatrixXd::Zero(m, m);
            em.R_v = Eigen::VectorXd::Zero(m);
            em.satellite_ids.reserve(static_cast<size_t>(m));
            em.signal_types.reserve(static_cast<size_t>(m));

            // Differential ZWD column index — pulled once outside the row
            // loop; max() if not present (PPP / PPP-RTK or baseline with
            // estimate_differential_tropo=false).
            const size_t diff_tropo_idx = state_manager_.getDifferentialTropoIndex();
            const bool has_diff_tropo = state_manager_.hasDifferentialTropo()
                && diff_tropo_idx < n_state;

            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& r = rows[kept[static_cast<size_t>(a)]];
                em.H.block(a, 0, 1, 3) = r.h_pos;
                if (r.obs_type == 'L' && r.amb_idx >= 0
                    && static_cast<size_t>(r.amb_idx) < n_state) {
                    em.H(a, r.amb_idx) = 1.0;
                }
                // Differential ZWD partial: phase rows only (the wet delay
                // affects code identically but adding code rows would just
                // double-up the geometry constraint with poorer SNR).  In
                // DD between non-pivot i and pivot p, ∂y/∂Δzwd ≈ mf_w_i −
                // mf_w_p (rover-side, short-baseline approximation that the
                // mapping functions match between rover and reference).
                if (has_diff_tropo && r.obs_type == 'L') {
                    em.H(a, static_cast<Eigen::Index>(diff_tropo_idx)) =
                        r.mf_w_rov_i - r.mf_w_rov_p;
                }
                em.innovations(a) = r.innov;
                em.R_v(a)         = r.var_brsd_i + r.var_brsd_p;
                em.satellite_ids.push_back(r.sat_i + "-" + r.sat_p);
                // Surface the rover code in residual traces.  When aliased
                // with a different reference attribute, append the ref
                // code so log readers can spot mixed-attribute rows.
                em.signal_types.push_back(
                    (r.rov_code == r.ref_code)
                        ? r.rov_code
                        : (r.rov_code + "/" + r.ref_code));
            }
            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& ra = rows[kept[static_cast<size_t>(a)]];
                em.R_full(a, a) = em.R_v(a);
                for (Eigen::Index b = a + 1; b < m; ++b) {
                    const auto& rb = rows[kept[static_cast<size_t>(b)]];
                    if (ra.group_key != rb.group_key) continue;
                    const double cov = ra.var_brsd_p;
                    em.R_full(a, b) = cov;
                    em.R_full(b, a) = cov;
                }
            }
        } catch (const std::exception& ex) {
            klog->error("DD H/R assembly failed: {}", ex.what());
            first_dd_call = false;
            return em;
        } catch (...) {
            klog->error("DD H/R assembly failed: unknown exception");
            first_dd_call = false;
            return em;
        }

        first_dd_call = false;
        return em;
    }

} // namespace GnssProcessor
