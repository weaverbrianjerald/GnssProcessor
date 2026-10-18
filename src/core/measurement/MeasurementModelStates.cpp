/**
 * @file MeasurementModelStates.cpp
 * @brief MeasurementModel's state pre-pass: ensureRequiredStates and the member
 *        it calls for each state category.
 *
 * Owns the births, seeds and prunes of the dynamic states a single-receiver
 * epoch requires (slant ionosphere, phase colour, per-band or NL/WL
 * ambiguities with the BSSD pivot-relative seeds, receiver and satellite code
 * biases), the marks that keep an ambiguity out of AR, and the band-count
 * warning; the network cold receiver-clock pre-seed it calls is in
 * MeasurementModelSeeds.cpp with the other seeds.  ensureRequiredStates calls
 * one member per category in a fixed order; the order is a dependency chain
 * (the ionosphere is seeded before the clock pre-seed and the ambiguities, the
 * pivot seeds before the non-pivot seeds, the satellite code-bias seed reads
 * the state after the ambiguity births).  The members share one working value,
 * RequiredStates, and each opens with an alias preamble so its body reads the
 * names the single function used.  The table "the state pre-pass, in call
 * order" in MeasurementModel.hpp lists each category's birth function, gate,
 * seed and variance.
 */

#include "measurement/MeasurementModel.hpp"
#include "measurement/Seeds.hpp"
#include "filter/StateKeySchema.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace GnssProcessor {

    // ===================================================================
    // The pre-pass's working value
    // ===================================================================

    /**
     * @brief What one epoch requires of the dynamic state: the pre-pass's
     *        configuration flags, and the keep sets and seeds its members
     *        collect for the members that follow.
     *
     * Built by @ref MeasurementModel::ensureRequiredStates for one call and
     * passed by reference to each member in order; nothing in it outlives the
     * call.  The field names are the names the members' bodies use.
     */
    struct MeasurementModel::RequiredStates {
        /// Uncombined (raw-signal) processing: slant ionosphere states and raw
        /// phase ambiguities.
        bool is_uncombined{ false };
        /// The ambiguity basis is NL/WL (one NL and one WL per non-reference band)
        /// instead of one ambiguity per band.
        bool nlwl{ false };
        /// Phase-colour states (network.phase_colour_state).
        bool pc_on{ false };
        /// Between-satellite single differences: ambiguities are seeded relative
        /// to the constellation's pivot.
        bool bssd_active{ false };

        /// The ambiguity keys this epoch observes, plus the hold-protected keys:
        /// the ambiguity prune's keep set.
        std::set<std::string> required_ambiguities;
        /// The satellites with a code or phase row in uncombined mode, plus the
        /// satellites of the hold-protected keys: the ionosphere keep set.
        std::set<std::string> required_iono_sats;
        /// Phase-colour states (network.phase_colour_state): the
        /// satellites with an uncombined phase observation this epoch, with their
        /// elevation.
        std::map<std::string, double> required_pc_sats;
        /// NlWl path: the phase signals seen per (satellite, band).  They determine
        /// a deterministic reference band per satellite, and states are seeded from
        /// the reference-band phase observation.  The inner key is the RINEX band
        /// digit (signal[1]); the value is the first signal seen on that band
        /// (obs.observed is an ordered map), so seeding is reproducible across
        /// rebuilds.
        std::map<std::string, std::map<char, std::string>> nlwl_phase_signals;
        /// BSSD: the pivot's absolute ambiguity seed per "<cons>|<signal>" in
        /// metres (per-band basis), or per "<cons>|NL" and "<cons>|WL|<a>_<b>" in
        /// cycles (NL/WL basis).
        std::map<std::string, double> pivot_abs_seeds;
    };

    namespace {
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
    // State pre-pass: the driver
    // ===================================================================

    void MeasurementModel::ensureRequiredStates(const std::vector<PppObservation>& observations,
        const std::string& epoch_prefix) {
        RequiredStates need;
        need.is_uncombined = config_.isUncombinedEnabled();
        need.nlwl = (config_.getStateAmbiguityBasis() == StateAmbiguityBasis::NlWl);
        need.pc_on = config_.getNetworkConfig().phase_colour_state;
        need.bssd_active = config_.enableBetweenSatelliteSd();

        collectRequiredStates(observations, need);
        addHoldProtectedKeys(need);
        collectNlWlKeys(need);
        ensureIonosphereStates(observations, epoch_prefix, need);
        ensurePhaseColourStates(need);
        preseedColdReceiverClock(observations, epoch_prefix);
        computePivotSeeds(observations, need);
        ensurePerBandAmbiguities(observations, epoch_prefix, need);
        ensureNlWlAmbiguities(observations, epoch_prefix, need);

        // The prune passes the ambiguity prune sink, which records the removed
        // columns so the arc ledger can state that the column a later re-entry
        // lands on is a fresh one.  The sink is null unless the network producer
        // armed the recorder, so every other path is unaffected.
        state_manager_.pruneAmbiguitiesExcept(need.required_ambiguities,
                                              state_manager_.ambiguityPruneSink());

        warnAmbiguityBandCount(epoch_prefix, need);
        markUnresolvableAmbiguities(observations, epoch_prefix, need);
        ensureReceiverCodeBiasStates(observations, epoch_prefix);
        ensureSatCodeBiasStates(observations, epoch_prefix, need);
    }

    // ===================================================================
    // The members, in the driver's order
    // ===================================================================

    void MeasurementModel::collectRequiredStates(const std::vector<PppObservation>& observations,
                                                 RequiredStates& need) const {
        const bool is_uncombined = need.is_uncombined;
        const bool nlwl = need.nlwl;
        const bool pc_on = need.pc_on;
        auto& required_ambiguities = need.required_ambiguities;
        auto& required_iono_sats = need.required_iono_sats;
        auto& required_pc_sats = need.required_pc_sats;
        auto& nlwl_phase_signals = need.nlwl_phase_signals;

        // --- First pass: collect required satellites and ambiguity keys ---
        for (const auto& obs : observations) {
            // Under BSSD, GLONASS is skipped by default: its FDMA per-channel
            // receiver delay does not cancel in a between-satellite SD.  With
            // enable_glonass_bssd=true it is kept; the per-(sat, signal)
            // sat-bias states (estimate_sat_biases) then absorb the ICB on code
            // rows and the float ambiguities absorb it on phase rows.
            if (obs.system == System::GLONASS && config_.enableBetweenSatelliteSd()
                && !config_.enableGlonassBssd())
                continue;

            for (const auto& [ref_type, observed] : obs.observed) {
                if (ref_type.empty()) continue;
                if (!obs.observed.count(ref_type) || !obs.variances.count(ref_type) || !obs.modeled.count(ref_type)) continue;

                // When BSSD is active, skip signals that the constellation's
                // pivot does not track: they cannot form same-signal SDs and
                // the ambiguity state would be unobservable.  The pivot must
                // track the same full signal key (the raw signal in uncombined
                // mode, the iono-free combination in IF mode), because
                // BssdTransform groups SD rows by that key.
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
                    if (pc_on && is_uncombined) required_pc_sats[obs.satellite] = obs.elevation;
                }
            }
        }
    }

    void MeasurementModel::addHoldProtectedKeys(RequiredStates& need) const {
        const bool is_uncombined = need.is_uncombined;
        auto& required_ambiguities = need.required_ambiguities;
        auto& required_iono_sats = need.required_iono_sats;

        // --- Hold-protected keys (soft-LLI / gap bridge) ---
        // An ambiguity held across a short tracking outage, pending a TurboEdit
        // snapshot + dN·λ repair, has no observation this epoch, so it is not in
        // required_ambiguities and the driver's prune would delete the pre-gap state
        // that the repair restores.  The held keys join the keep-set, and in
        // uncombined mode their satellites join the iono keep-set, so that a
        // whole-satellite outage does not leave the repaired ambiguity against a
        // reseeded iono state.  The seeding loops are driven by observations and
        // never touch these keys, so the held state passes through unchanged.
        for (const auto& key : protected_ambiguity_keys_) {
            required_ambiguities.insert(key);
            if (is_uncombined) {
                const auto p = key.find('_');
                if (p != std::string::npos)
                    required_iono_sats.insert(key.substr(0, p));
            }
        }
    }

    void MeasurementModel::collectNlWlKeys(RequiredStates& need) {
        const bool nlwl = need.nlwl;
        const auto& nlwl_phase_signals = need.nlwl_phase_signals;
        auto& required_ambiguities = need.required_ambiguities;

        // NlWl basis: assemble NL/WL ambiguity keys from the per-(sat, band)
        // phase signal map.  The reference band is the lowest-numbered band
        // observed this epoch (std::map keeps band digits sorted ascending).  It
        // is recomputed every epoch, so it stays fixed while that band remains
        // observed; the NL/WL keys carry the reference band, so a change of
        // reference band yields new keys and the driver's prune removes the old ones.
        nlwl_ref_band_.clear();
        if (!nlwl) return;
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

    void MeasurementModel::ensureIonosphereStates(const std::vector<PppObservation>& observations,
                                                  const std::string& epoch_prefix,
                                                  const RequiredStates& need) {
        const bool is_uncombined = need.is_uncombined;
        const auto& required_iono_sats = need.required_iono_sats;

        // --- Ionosphere states (created and seeded before the ambiguities) ---
        if (!is_uncombined) return;

        for (const auto& obs : observations) {
            if (!required_iono_sats.count(obs.satellite)) continue;
            if (state_manager_.getIonosphereIndex(obs.satellite) < 0) {
                if (obs.elevation > 0.0) {
                    state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
                    state_manager_.setIonosphereAzimuth(obs.satellite, obs.azimuth);
                }

                const auto iono_seed_opt = seedIonosphere(residualSeedInput(), obs);
                if (iono_seed_opt.has_value())
                    log_manager_.getLogger("state_initializer")->debug(
                        "Iono seed for {} from {}/{}: P1-mod1={:.3f}, P2-mod2={:.3f}, "
                        "alpha1={:.4f}, alpha2={:.4f}, seed={:.3f} m",
                        obs.satellite, iono_seed_opt->code_a, iono_seed_opt->code_b,
                        iono_seed_opt->range_a_m, iono_seed_opt->range_b_m,
                        iono_seed_opt->alpha_a, iono_seed_opt->alpha_b, iono_seed_opt->value_m);
                // When no BIA-covered code pair is available, the seed
                // would otherwise inherit raw differential code biases
                // (e.g. BeiDou with placeholder zero-valued BIA entries).
                // Fall back to seed=0 with an inflated initial std so the
                // filter freely converges to the true iono via subsequent
                // measurements rather than trusting the zero too tightly.
                constexpr double kUnseededIonoStdScale = 10.0;
                const double iono_seed = iono_seed_opt ? iono_seed_opt->value_m : 0.0;
                const double std_scale = iono_seed_opt.has_value() ? 1.0 : kUnseededIonoStdScale;
                state_manager_.addIonosphereState(obs.satellite, iono_seed, std_scale);
                log_manager_.getLogger("measurement_model")->debug(
                    "{}Added ionosphere state for satellite {} with seed={:.3f} m{}",
                    epoch_prefix, obs.satellite, iono_seed,
                    iono_seed_opt.has_value() ? "" : " (no BIA-covered pair, std×10)");
            }
        }
        state_manager_.pruneIonosphereStatesExcept(required_iono_sats);

        for (const auto& obs : observations) {
            if (required_iono_sats.count(obs.satellite) && obs.elevation > 0.0) {
                state_manager_.setIonosphereElevation(obs.satellite, obs.elevation);
                state_manager_.setIonosphereAzimuth(obs.satellite, obs.azimuth);
            }
        }
    }

    void MeasurementModel::ensurePhaseColourStates(const RequiredStates& need) {
        const bool pc_on = need.pc_on;
        const auto& required_pc_sats = need.required_pc_sats;

        // --- Phase-colour states (network.phase_colour_state) ---
        // One column per satellite with an uncombined phase observation, seeded 0 (the
        // network never writes the solution back, so the linearization point stays 0);
        // the satellite's elevation is recorded for the network's prior law.  Its
        // StateManager variance is not read by the network solve.
        if (!pc_on) return;
        const double v0 = config_.getNetworkConfig().phase_colour_sigma_zen_m
                        * config_.getNetworkConfig().phase_colour_sigma_zen_m;
        std::set<std::string> keep;
        for (const auto& [sat, elev] : required_pc_sats) {
            keep.insert(sat);
            if (state_manager_.getPhaseColourIndex(sat) < 0)
                state_manager_.addPhaseColourState(sat, v0);
            state_manager_.setPhaseColourElevation(sat, elev);
        }
        state_manager_.prunePhaseColourStatesExcept(keep);
    }

    void MeasurementModel::computePivotSeeds(const std::vector<PppObservation>& observations,
                                             RequiredStates& need) const {
        const bool nlwl = need.nlwl;
        const bool bssd_active = need.bssd_active;
        const auto& nlwl_phase_signals = need.nlwl_phase_signals;
        auto& pivot_abs_seeds = need.pivot_abs_seeds;

        // Pivot absolute seeds, keyed by constellation and full signal type
        // (e.g. "GPS|L1W", "GPS|L1C"): one anchor per (sat, signal) pair.  Under
        // channel_mode=all the pivot tracks several signals per band and each
        // has its own SD frame (non-pivot L1W is differenced against pivot L1W,
        // L1C against L1C).  BssdTransform's group key uses the same full-signal
        // granularity, so both sides refer to the same pivot row for a given
        // ambiguity state.
        //
        // On the NlWl basis the seeds are converted to cycles here (NL: the
        // pivot's cycle ambiguity on L_ref_band; WL: its N_a − N_b difference for
        // each band pair beyond ref_band).  The keys "<cons>|NL" and
        // "<cons>|WL|<a>_<b>" keep the non-pivot seeding lookup a flat
        // string→double map.
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
                        ambiguitySeed(obs, ref_type, x_seed);
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
                    ambiguitySeed(obs, ref_sig, x_seed)
                    / lambda_ref;
                pivot_abs_seeds[cons + "|NL"] = nl_seed_cyc;

                for (const auto& [b, b_sig] : sit->second) {
                    if (b == ref_band) continue;
                    const double lambda_b = ppp_utils_.getWavelength(
                        obs.system, b_sig, obs.satellite);
                    if (!std::isfinite(lambda_b) || lambda_b <= 0.0) continue;
                    const double n_b_cyc =
                        ambiguitySeed(obs, b_sig, x_seed)
                        / lambda_b;
                    // WL canonical = N_low_band - N_high_band.  ref_band is the
                    // lowest observed band, so it is the lower of the pair and
                    // WL = NL - N_b (e.g. L1 ref + L2/L5 WL).  The ref_band > b
                    // branch (canonical key with b first, WL = N_b - NL) keeps
                    // the sign right if ref_band is ever above b; the current
                    // reference-band choice never takes it.
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
    }

    void MeasurementModel::ensurePerBandAmbiguities(const std::vector<PppObservation>& observations,
                                                    const std::string& epoch_prefix,
                                                    const RequiredStates& need) {
        const bool nlwl = need.nlwl;
        const bool bssd_active = need.bssd_active;
        const auto& required_ambiguities = need.required_ambiguities;
        const auto& pivot_abs_seeds = need.pivot_abs_seeds;

        // Per-band seed/add loop (skipped under the NlWl basis; see ensureNlWlAmbiguities).
        if (nlwl) return;
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
                    double seed = ambiguitySeed(obs, ref_type, x_now);

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

            // ---- No seed-site lane snap -----------------------------------
            // A seed-site lane snap was removed from this point and is not to
            // be rebuilt: an edit to the seeds here cannot act on the network
            // solve.
            //
            //   1. Its only write was setStateVector on a local ambiguity
            //      column.  On the network producer that vector is the
            //      linearization point x_lin and the solve is never written
            //      back into it, so the write is a pre-solve frame edit.  The
            //      reduced solve absorbs an x_lin shift δ on a local column
            //      exactly: data rows move as y → y − A_r e_c δ, the carried
            //      prior's RHS absorbs the same shift through
            //      η = Λ·(value_abs − x_lin), b_red is invariant and the
            //      absolute estimate x_lin + x_r is unchanged.
            //      The exceptions are the two prefit screens that gate on
            //      H·x_lin rather than on the estimate: the SD-EWL
            //      correction-frame cut (`network.sd_ewl.max_abs_z_m`, which
            //      changes the row set) and the prefit fuse
            //      (`network.prefit_fuse_sigma`, which changes row weights).
            //      Both are off by default and the fuse's default classes
            //      exclude phase, so the removal leaves default output
            //      unchanged; with either armed, a seed edit does change it.
            //   2. The DD-fraction trace is x_lin-invariant as well
            //      (NetworkDdFrac forms value_m = x_lin + x_r), so the snap was
            //      also invisible to it.
            //   3. The snap offset `d` was not a lane error.  With a kept
            //      anchor, the anchor term cancels identically and the integer
            //      cancels against s·ΔMW, leaving d = −Δe_b/λ_b: one band's
            //      frozen-frame model error (frozen receiver clock, ZWD, iono
            //      seed) over a single-band wavelength.  1/λ_L2 = 4.095 cyc/m
            //      is 3.53× the lane coefficient 1/λ_WL = 1.160 cyc/m.
            //
            // The lane is measured in the carry instead (local_carry.value_abs
            // = x_lin + back-substituted Δx, an absolute metre level in the
            // epoch's realized gauge).  See docs/SPEC_lane_slip_response.md
            // §7.2.

        }
    }

    void MeasurementModel::ensureNlWlAmbiguities(const std::vector<PppObservation>& observations,
                                                 const std::string& epoch_prefix,
                                                 const RequiredStates& need) {
        const bool nlwl = need.nlwl;
        const bool bssd_active = need.bssd_active;
        const auto& nlwl_phase_signals = need.nlwl_phase_signals;
        const auto& required_ambiguities = need.required_ambiguities;
        const auto& pivot_abs_seeds = need.pivot_abs_seeds;

        // NlWl seed/add loop.  Iterates the per-(sat, band) phase signal map
        // of collectRequiredStates rather than the raw observation list; this keeps
        // seeding deterministic across measurement-rebuild passes within
        // the same epoch, and naturally enforces "one NL + one WL per
        // non-ref band" without requiring the caller-side dedup tracking
        // that the per-band loop avoids by virtue of its key uniqueness.
        if (!nlwl) return;
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
                        ambiguitySeed(obs, ref_sig, x_now)
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
                    ambiguitySeed(obs, ref_sig, x_now)
                    / lambda_ref;
                const double n_b_cyc =
                    ambiguitySeed(obs, b_sig, x_now)
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

    void MeasurementModel::warnAmbiguityBandCount(const std::string& epoch_prefix,
                                                  const RequiredStates& need) const {
        const bool nlwl = need.nlwl;
        const auto& required_ambiguities = need.required_ambiguities;

        // Defensive band-count check.  The band filter admits at most N
        // distinct phase bands per satellite, where N follows the configured
        // frequency_mode (Dual = 2, Triple = 3; All and Independent are uncapped;
        // the warning text below still names it by its deprecated key, signal_mode).
        // A satellite with more bands than that means some upstream path has
        // bypassed the filter (e.g. a slip detector emitting a reset key for a
        // band-filtered signal, or a PPP_RTK override that skips the filter).
        // The check reads the keep-set, so the states it flags survive the
        // driver's prune, which runs first; the warning names the satellite and
        // bands so the path
        // can be traced to its source.
        if (nlwl) return;
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

    void MeasurementModel::markUnresolvableAmbiguities(const std::vector<PppObservation>& observations,
                                                       const std::string& epoch_prefix,
                                                       const RequiredStates& need) {
        const bool is_uncombined = need.is_uncombined;
        const bool nlwl = need.nlwl;
        const auto& required_ambiguities = need.required_ambiguities;

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
        // The receiver-bias and sat-bias members called after this one apply
        // to code rows only, so they are unaffected by the basis switch.
        if (nlwl) return;
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
                        !phaseBiasCoveredForAr(rowProducts(), obs.satellite, ref_type, obs.time);
                    const bool missing_atx = !ifCombHasFullRxAtx(rowProducts(), ref_type, obs.system);
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
                        !phaseBiasCoveredForAr(rowProducts(), obs.satellite, ref_type, obs.time);
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

    void MeasurementModel::ensureReceiverCodeBiasStates(const std::vector<PppObservation>& observations,
                                                        const std::string& epoch_prefix) {
        // --- Receiver bias states ---
        std::set<std::string> required_code_biases;
        if (config_.estimateIfb() && config_.isUncombinedEnabled()) {
            for (const auto& obs : observations) {
                for (const auto& [ref_type, observed] : obs.observed) {
                    if (ref_type.empty() || ref_type[0] != 'C') continue;
                    const auto bias = state_initializer_.resolveReceiverBias(ref_type, obs.system, obs.satellite);
                    if (!std::isfinite(bias.coefficient) || bias.state_key.empty()) continue;
                    // GLONASS network producer: allocate only the GLONASS per-FCN-slot
                    // IFB.  GPS/Galileo receiver DCB/IFB states are intentionally absent
                    // here, as in a GPS+Galileo network that does not estimate the
                    // satellite code biases: a non-reference-band IFB has no IFB-free
                    // reference code to anchor the slant iono, so creating one adds a
                    // per-satellite iono<->IFB<->ambiguity rank deficiency to the
                    // absolute per-epoch local block.  Such a network absorbs the
                    // receiver code bias into the clock/iono (the L1-referenced
                    // convention).  (Under network.estimate_sat_code_bias, which the
                    // GLONASS producer refuses, estimate_ifb is forced and these states
                    // are allocated; the station split keeps the DCB and holds the
                    // extra-band IFB at its seed unless
                    // network.estimate_rx_extra_band_code_delay is armed.)  The
                    // GLONASS per-slot IFB is the one receiver bias the network needs
                    // (its FDMA group delay cannot be modelled otherwise); its residual
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
    }

    void MeasurementModel::ensureSatCodeBiasStates(const std::vector<PppObservation>& observations,
                                                   const std::string& epoch_prefix,
                                                   const RequiredStates& need) {
        const bool is_uncombined = need.is_uncombined;

        // --- Per-satellite signal bias states (code-only in uncombined) ---
        // Phase ambiguities absorb satellite phase biases, so only code rows
        // without BIA coverage need a per-satellite bias state.
        //
        // A new state is seeded from the row's innovation (observed − predicted
        // without the bias), not from 0.  Hardware delays on uncovered signals (e.g.
        // BeiDou C7I, where the MGEX BIA carries only C2I/C6I) can exceed 100 m once
        // differenced against a same-band pivot whose bias state is also unset.  A
        // new state at 0 with initial_std.sat_bias = 10 m then gives sres ≫ threshold
        // on its first epoch, and the row is excluded before the state receives any
        // update, so the signal stays an outlier permanently.  The seed brings that
        // first innovation to about 0.  It also absorbs whatever else is unmodeled
        // this epoch (clock/iono/ISB residuals); BSSD cancels the common-mode part
        // across SD pairs, and the small initial std lets later epochs refine the
        // rest.
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

            // BSSD frame consistency.  The raw seed zeroes this satellite's
            // undifferenced innovation, which absorbs the full receiver clock at
            // this epoch because BSSD allocates no receiver-clock state.  A
            // satellite that appears later than the incumbents therefore carries a
            // different clock epoch in its bias than the pivot it is differenced
            // against, and that gap (the receiver-clock drift between the two
            // seeding epochs) is a permanent SD offset: the row is flagged as an
            // outlier and, once excluded, receives no update to recover.
            //
            // Re-referencing the seed to the pivot's current undifferenced
            // innovation places the new bias in the same frame as the established
            // states (seed → clock(t0)+dcb, SD residual about 0), the code-bias
            // analogue of pivot-relative ambiguity seeding.  It applies only when
            // the pivot already carries a same-band bias state, which shows that a
            // frame exists to reference; otherwise the raw seed defines the frame
            // and is correct.
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

        // Band gate: under BSSD, only signals the current pivot tracks are
        // observable in SD.  A sat-bias state created for an untracked signal would
        // be seeded against the current state vector, including an unconverged iono
        // state, and would keep that contaminated value, since sat-bias process
        // noise is essentially zero.  After a later pivot rotation the stale seed
        // produces a large innovation, the row is excluded as an outlier, and the
        // state never recovers.  Gating on bssd_pivot_signals_ defers the seed to
        // the epoch the signal first becomes usable, against the converged state.
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
        // innovation seed (seedSatBias).  When a (sat, signal) has been pruned
        // recently (visibility gap, brief masking, geometry hole) the cache
        // holds its last-known value + diagonal variance.  Restoring it
        // skips a full re-convergence cycle and keeps the new state from
        // re-seeding via the first innovation against a not-yet-converged
        // neighbour state vector.
        //
        // The helper logs which case applied (restored from the cache, warm or
        // cold-value, or newly seeded).  A restore consumes the cache entry,
        // so a key is in either the active state or the cache, not both.
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

        // In the PPP-RTK network producer, ensureNetworkSharedStates owns the
        // satellite code-bias states (a shared border column, allocated and pruned
        // together with the satellite clock and phase bias), as it owns the
        // satellite clock and phase bias.  This single-receiver fallback path skips
        // them, so the two paths never manage the same per-(sat,signal) states.  The
        // network admits every GPS/Galileo raw code signal regardless of .BIA
        // coverage; the .BIA/SSR gate below is a user-side concern that does not
        // apply when the network estimates its own OSB.
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
                    // "Already corrected?" consults both bias sources, not only the
                    // static .BIA product.  Real-time has no .BIA, so hasBiasEntry
                    // is false everywhere and the live SSR stream is the actual
                    // correction source (applied as ssr_bias_term in
                    // buildJacobianRow, gated only by the staleness gate).  Without
                    // the SSR check, every SSR-covered signal gets a redundant
                    // near-frozen state.  The test is per-(sat,signal) because IGS
                    // streams are partial (E1 covered, E5a/E5b not), so a signal
                    // that SSR does not carry still gets its state as its only code
                    // correction.  The strict prune below drops the state on the
                    // epoch SSR coverage arrives mid-arc; ensureRequiredStates
                    // re-creates it if coverage disappears.
                    if (biaCoversSignal(rowProducts(), obs.satellite, ref_type)
                        || ssrCoversCodeSignal(rowProducts(), obs.satellite, ref_type, obs.time)) continue;
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
                    if (ifCombHasFullBia(rowProducts(), obs.satellite, ref_type)) continue;
                    if (!signalCoveredByPivot(obs.satellite, ref_type)) continue;
                    const std::string key = obs.satellite + "|" + ref_type;
                    required_sat_biases.insert(key);
                    if (state_manager_.getSatBiasIndex(key) < 0) {
                        addOrRestoreSatBias(key, obs, ref_type, observed, "IF");
                    }
                }
            }
        }

        // Strict prune: drop every sat-bias state whose (sat, signal) is not
        // required this epoch, the same policy as the ambiguity prune.  No
        // state for an invisible satellite or for a signal that left pivot
        // coverage survives in the active filter.  The pruned states are
        // stashed in the StateManager's sat-bias cache, so a near-term
        // re-acquisition can warm-restart instead of taking a cold re-seed
        // (which is fragile against unconverged neighbour states; see
        // seedSatBias and the band gate above).
        if (!network_owns_sat_bias && config_.estimateSatBiases()) {
            state_manager_.pruneSatBiasStatesExcept(required_sat_biases);
        }
    }

} // namespace GnssProcessor
