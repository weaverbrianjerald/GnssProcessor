#pragma once

/**
* @file IfPhaseSlipDetector.hpp
* @brief Between-satellite, between-epoch ionosphere-free phase slip detector.
*
* @note STATUS: BUILT + WIRED on the NETWORK path; STAGED on the user/kinematic
*       path.  The network estimator PINS station positions, which satisfies the
*       static-site precondition of the position-uncertainty gating below, so
*       this detector IS compiled (see CMakeLists.txt) and instantiated per
*       non-pivot station when `network.cycle_slip.if_phase_td` is set
*       (Application::runNetwork): pooled per-(station, signal-pair) MAD
*       flagging, its flags feeding the Card-4 C3c invariant-carry slip funnel
*       (and the phase-leveling refloat when that carry is the active one).  In
*       the general (kinematic / pre-convergence) USER pipeline it remains INERT
*       — the position gating below is unsafe there and the
*       `enable_if_slip_detector` user toggle has no instantiation path.  For
*       user-side slip detection see slip/MwSlipDetector and
*       slip/GraphicSlipDetector.
*
* Implements the double-differenced IF phase OMC (observed-minus-computed)
* approach for sensitive cycle slip detection.  The model subtraction is
* essential: it removes the satellite-specific geometric range (~20,000 km)
* and satellite clock, which change by meters per epoch and do NOT cancel
* in between-satellite differencing of raw observables.  After model
* subtraction the OMC contains only receiver clock, ZWD, ISB, ambiguity
* and noise  all of which DO cancel in the double difference.
*
* Algorithm:
*   1. Computes per-satellite IF phase OMC = observed_IF - modeled_IF.
*   2. Selects a high-elevation reference satellite per constellation.
*   3. Forms between-satellite single differences (gradients):
*          grad(sat) = OMC(sat) - OMC(ref)
*      Cancels rx_clk, ISB, phase_clk (common-mode).
*   4. Forms between-epoch double differences:
*          dd(sat) = grad(sat, t) - grad(sat, t-1)
*      Cancels ZWD*mf_w (slowly varying) and constant ambiguity.
*   5. Applies MAD-based adaptive + fixed (IF wavelength) outlier thresholds.
*
* Unlike the MW detector (which is code-noise limited at ~0.3 WL cycles),
* this detector operates on carrier phase precision (~mm) and can reliably
* detect slips of 1-2 IF cycles that the MW and post-fit residual checks miss.
*
* Position-uncertainty gating:
*   The between-satellite single difference does NOT perfectly cancel the
*   receiver position contribution  a position error ?pos produces a
*   differential geometric residual of ~(e_sat - e_ref) · ?pos, which can
*   reach meters during pre-convergence.  To avoid false slip flags the
*   detector accepts an optional 3D position sigma (from the filter
*   covariance) and suppresses outlier flagging when the position is
*   uncertain.  Gradient storage continues so that the detector resumes
*   seamlessly once the position converges.
*
* Ported from stochastic_model_phase_v03.m / compute_if_phase_diff().
*/

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Dense>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @class IfPhaseSlipDetector
     * @brief Detects cycle slips via between-satellite, between-epoch IF phase OMC differences.
     *
     * The detector works per constellation: it computes the IF phase OMC
     * (observed - modeled) to remove the large satellite-specific geometric
     * range and satellite clock, then forms between-satellite single
     * differences (removing common receiver clock, ISB, phase clock), then
     * between-epoch double differences (removing constant ambiguities and
     * slowly-varying ZWD).  A clean double difference should be near zero;
     * a cycle slip appears as a jump of ~N * lambda_IF.
     *
     * When the estimated position is uncertain (pre-convergence), the
     * differential geometric error (e_sat - e_ref) · ?pos can exceed the
     * IF wavelength, causing false slip detections.  The detector therefore
     * accepts a 3D position sigma and gates outlier flagging accordingly.
     */
    class IfPhaseSlipDetector {
    public:
        /**
         * @brief Construct a new IfPhaseSlipDetector.
         * @param config Configuration providing thresholds and processing options.
         * @param log_manager Log manager used to emit diagnostic messages.
         */
        IfPhaseSlipDetector(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Detect IF phase cycle slips for the provided epoch observations.
         *
         * Processes observations grouped by constellation. Returns a vector of
         * ambiguity keys that should be reset (format: "G01_L1C_L2W").
         *
         * The IF phase OMC (observed_IF - modeled_IF) is used. The modeled
         * value removes the large satellite-specific geometric range and
         * satellite clock that change by meters per epoch. After model
         * subtraction, the OMC contains only receiver clock, ZWD*mf_w,
         * ISB, phase clock, ambiguity and noise:
         *   - Between-satellite diff cancels rx_clk, ISB, phase_clk.
         *   - Between-epoch diff cancels ZWD*mf_w and ambiguity.
         * This makes the detector independent of Kalman filter state.
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @param position_sigma_3d Optional 3D position uncertainty (meters).
         *        When > kPositionSigmaThresholdM the detector stores gradients
         *        but suppresses slip flagging to avoid false detections from
         *        differential geometric error during pre-convergence.
         *        Pass 0.0 or omit to always run full detection (backward compatible).
         * @return std::vector<std::string> Vector of ambiguity keys to reset.
         */
        /// @param model_nuisance_m Magnitude (m) of the epoch-to-epoch change in
        ///        the MODELLED part of the OMC that does NOT cancel in the
        ///        between-satellite / between-epoch differencing — in practice
        ///        |ΔZWD|, the one filter state that survives both (§6.14.6 item 3).
        ///        Compared against the threshold set by @ref setNuisanceGate; 0
        ///        (the default) with a 0 threshold leaves the gate disabled, so
        ///        every existing caller is unchanged.
        std::vector<std::string> detect(const std::vector<PppObservation>& observations,
            double position_sigma_3d = 0.0,
            const Eigen::Vector3d& receiver_position = Eigen::Vector3d::Zero(),
            double model_nuisance_m = 0.0);

        /**
         * @brief Arm the model-nuisance gate (§6.14.6 item 3).
         *
         * The sibling of the position-uncertainty gate, for the nuisance that
         * actually survives on the NETWORK path.  Position is pinned there, so
         * `position_sigma_3d` is passed 0.0 and that gate has never once fired;
         * the term that does survive is Δ(ZWD)·(mf_sat − mf_ref), and it is
         * measurable: median |ΔZWD| collapses 539 → 62 mm over the first twelve
         * epochs of a cold run while the reference gate's fires decay 116 → 0 in
         * lockstep, and in the settled era the top |ΔZWD| decile still carries
         * 2.05× the baseline fire rate.
         *
         * @param max_m Suppress flagging when @c model_nuisance_m exceeds this.
         *        0 (default) disables the gate — byte-identical.
         */
        void setNuisanceGate(double max_m) { nuisance_max_m_ = max_m; }

        /**
         * @brief Clear IF phase history for a specific satellite.
         *
         * Call this when an ambiguity is reset by an external mechanism so the
         * detector starts a fresh arc on the next epoch.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite);

        /**
         * @brief Clear all internal state (all constellations).
         *
         * Called after a complete filter reset.
         */
        void resetAll();

        /**
         * @brief IF phase jump information for one satellite from the last detect() call.
         */
        struct IfJumpInfo {
            double delta_grad_m{ 0.0 };  ///< Between-epoch DD IF phase jump (meters).
            double lambda_if_m{ 0.0 };   ///< IF wavelength = c/(f1+f2) = the (1,1) IF slip jump (m); the physical floor.
            double elevation_deg{ 0.0 };  ///< Satellite elevation (degrees) at detection epoch.
            std::string sig1;             ///< High-band signal of the IF pair (e.g. "L1C").
            std::string sig2;             ///< Low-band signal of the IF pair (e.g. "L2W").
            bool   valid{ false };        ///< True if a finite jump was computed.
            /// True for the constellation's REFERENCE satellite, whose double
            /// difference is identically 0 by construction (grad(ref) =
            /// OMC(ref) − OMC(ref) = 0 ⇒ dd(ref) = 0 every epoch).  The network
            /// pool arm MUST exclude these structural zeros from its robust
            /// SCALE: they sit at the centre of the distribution and deflate the
            /// MAD (measured 16.0 % on runs/nonint_losblock24h_hv, 12.5 % of all
            /// samples — docs/SPEC_amb_iono_split_overcount.md §6.14.5 defect 2).
            /// They carry no information about the noise the threshold tracks,
            /// and the reference can never be flagged by the pool arm anyway
            /// (its deviation is ~0) — which is why the separate per-station
            /// REFERENCE GATE exists (flag-rate tier T1r).
            bool   is_reference{ false };
        };

        /**
         * @brief Retrieve the IF phase jump for a satellite from the last detect() call.
         *
         * Returns the between-epoch, between-satellite double-differenced IF
         * phase jump (in meters) that was computed during the most recent
         * call to detect().  This is the quantity denoted delta_grad in the
         * internal processing.  The value is relative to the constellation's
         * reference satellite and is valid only when IfJumpInfo::valid is true.
         *
         * @param satellite  Satellite identifier (e.g. "G01").
         * @return IfJumpInfo  Phase jump info (valid=false if not available).
         */
        IfJumpInfo getLastIfJump(const std::string& satellite) const;

        /**
         * @brief All per-satellite IF jumps from the last detect() call.
         *
         * Used by the NETWORK producer's deferred-flagging path: the driver pools
         * these between-epoch double differences across ALL stations by signal pair
         * to form a robust per-pair MAD, then thresholds (max(k·MAD, floor)) and
         * refloats — instead of each station thresholding its own ~handful of
         * satellites. Keyed by satellite id.
         */
        const std::map<std::string, IfJumpInfo>& getLastIfJumps() const { return last_if_jumps_; }

        /**
         * @brief Card R (2026-07-21): hardened reference-satellite slip gate.
         *
         * The legacy gate is the last non-empirical decision in the IF-TD chain:
         * it fires whenever |median(non-ref ΔΔ)| exceeds a FIXED 0.5·λ_IF with
         * only n ≥ 2 contributors — no elevation normalization, no empirical
         * scale, no step-vs-drift semantics.  On a thin signal family a
         * persistent SUB-slip drift therefore re-fires it every epoch (the
         * 2026-07-21 audit measured 9 consecutive fires at one station, 5 at
         * another, with the raw wide-lane and geometry-free series flat to
         * ±0.1 cycles and no LLI).  Every fire refloats both band ambiguities
         * and marks the Card-4 carry chords slipped.
         *
         * When @ref RefGateParams::enable is set the decision becomes:
         *   1. **Scale** — |median| must exceed max(k·s_eff, floor_frac·λ_IF),
         *      where s_eff is an empirical-Bayes blend of this constellation's
         *      EWMA of the non-reference DD DISPERSION with the current epoch's
         *      own dispersion.  Same shape as the network pool arm; no
         *      stochastic-model σ enters.
         *   2. **Coherence** — a genuine reference slip is a COMMON step: every
         *      non-reference double difference moves together, so the family's
         *      MAD is small relative to |median|.  A drift scatters.  This is
         *      the step-vs-drift discriminator the legacy gate structurally
         *      lacked (it rebases by the median after each fire, so a re-fire
         *      is evidence of persistent drift, not of stale state).
         *   3. **Thin family** — below @ref RefGateParams::min_n contributors
         *      the median is not a trustworthy common-mode estimate; the
         *      physical floor is widened by @ref RefGateParams::thin_widen and
         *      the deferral is COUNTED (never a silent behaviour change).
         *   4. **Persistence / escalation** — after
         *      @ref RefGateParams::max_consecutive consecutive fires on the same
         *      reference the gate stops re-firing and forces a reference
         *      RE-SELECTION instead, which moves the drifting satellite into the
         *      network pool arm where an elevation-normalized, empirically
         *      scaled residual judges it.  If the brand-new reference "slips"
         *      immediately, the common mode is station-local (a receiver event)
         *      and is handed to the pool/RAIM layer rather than re-fired here.
         *
         * Default-constructed params leave @c enable false ⇒ the legacy gate
         * runs unchanged ⇒ byte-identical.  The user/kinematic path never calls
         * this setter, so it is unaffected in both modes.
         */
        struct RefGateParams {
            bool   enable{false};          ///< Master arm (default OFF ⇒ legacy gate).
            double k{6.0};                 ///< Empirical multiplier on the effective dispersion scale.
            int    min_n{4};               ///< Minimum non-reference DDs for a trustworthy median.
            double thin_widen{3.0};        ///< Physical-floor multiplier below @ref min_n.
            double coherence{0.5};         ///< Max MAD(non-ref DDs)/|median| for a COMMON step.
            /// Consecutive fires on one reference before escalating to a forced
            /// re-selection.  **1**, matching the only armed path's config default
            /// (`network.cycle_slip.if_phase_td_ref_max_consecutive`, GnssConfigTypes
            /// SlipQcConfig): spec R2 reads "a genuine reference slip fires ONCE", so a
            /// real one-epoch common step never pays the cap, while a persistent drift
            /// escalates on its second epoch instead of its third.  Measured on the
            /// 2026-07-21 audit config the false JPLM/MKEA G10 family goes 14 → 6 at 2
            /// and 14 → 4 at 1, with the genuine OWMG00NZL/G31 event untouched either
            /// way.  Kept in lockstep with the config default deliberately — the two
            /// drifting apart is how a unit test starts certifying a gate the field
            /// never runs (spec-followups N2).
            int    max_consecutive{1};
            double ewma_alpha{0.15};       ///< EWMA weight for the dispersion scale.
            double shrink_kappa{20.0};     ///< Empirical-Bayes shrinkage constant (samples).
            double floor_frac{0.5};        ///< Physical floor in units of λ_IF (0.5 = the legacy value).

            /// **ABSTAIN while the empirical arm has no samples** (§6.14.6 item 1).
            ///
            /// With zero empirical information the cold prior
            /// `floor_frac·λ_IF/k` makes `k·s_eff` collapse onto `phys`, so the
            /// hardened gate degenerates to *exactly* the legacy fixed floor for
            /// its first few epochs.  That is deliberate — it is what stops the
            /// scale from being self-contaminated by this epoch's own dispersion
            /// (see @ref updateRefDispersion) — but it means every cold run still
            /// opens on the 3σ bar the card exists to remove.  Measured on the
            /// 2 h golden A/B: all 118 surviving fires were at epochs 1 and 3,
            /// and every one of them was `action=diagnostic`, because `carry_warm`
            /// (n_emitted ≥ 6) is false there and a detection therefore CANNOT
            /// act.  Firing on a bar we cannot justify, to raise an event that
            /// cannot act, is pure event-stream pollution.
            ///
            /// When set, the gate returns @ref RefGateVerdict::Suppress until
            /// @c ConstellationState::ref_scale_count reaches @ref shrink_kappa
            /// — the same sample count at which the shrinkage weight reaches ½
            /// and the empirical arm starts to mean anything.  This is NOT the
            /// falsified "shrink toward this epoch's dispersion": abstention
            /// asserts nothing about the scale, it declines to decide.  Cost: a
            /// genuine reference slip inside the warm-up is not caught by THIS
            /// arm — it was already unactionable there, and MW/LLI still covers
            /// the window.  Default false ⇒ byte-identical.
            bool   warmup_abstain{false};

            /// **Elevation-normalize the empirical scale** (§6.14.6 item 2).
            ///
            /// The quantity this gate tests is the median of the non-reference
            /// DDs, and 88 % of its variance is the REFERENCE satellite's own
            /// noise divided by sin(el_ref) — a common term no family size can
            /// average away (measured: σ(median) is flat 17.3–18.4 mm for
            /// n = 3…10).  But the scale it is judged against, @c disp, is the
            /// family's scatter about that median, i.e. the per-SATELLITE noise
            /// at the family's own elevations.  Unlike the pool arm the gate
            /// applies no elevation map at all, so `k·s_eff` is a raw mixture
            /// over whatever elevations happened to be up, keyed to a
            /// (station, constellation) that outlives the reference it describes.
            ///
            /// When set, the EWMA stores the ZENITH-equivalent scale
            /// `disp·⟨sin el⟩_family` (a stationary station property that
            /// survives a reference handover) and the test rebuilds the
            /// statistic's own sigma from it:
            /// @code
            ///   σ_ref = s_z / sin(el_ref)                  // the common term
            ///   σ_fam = 1.2533 · (s_z / ⟨sin el⟩) / √n     // median of n iid
            ///   thr   = max(phys, k · hypot(σ_ref, σ_fam))
            /// @endcode
            /// Validated against the 24 h bed: s_z ≈ 12.8 mm and el_ref ≈ 62°
            /// give σ_ref 14.5 mm (measured station-local common 14.83) and
            /// σ_fam 10.1 mm, hypot 17.7 mm — the measured σ(median) of 17.70 mm.
            /// The threshold now tracks el_ref and n, which the flat `k·disp`
            /// form cannot.  The physical FLOOR stays raw: a slip is a slip at
            /// any elevation (slip-QC lesson 7).  Default false ⇒ byte-identical.
            bool   elev_norm{false};

            /// Elevation clamp for @ref elev_norm, degrees.  Matches the pool
            /// arm's own clamp so a horizon arc cannot explode 1/sin(el).
            double elev_clamp_deg{5.0};
        };

        /**
         * @brief Card R run telemetry (standing invariant #4).
         *
         * @c legacy_candidates counts epochs where the LEGACY condition would
         * have fired, so `legacy_candidates − fires` is exactly what the
         * hardening removed and the three `suppressed_*` counters say why.
         */
        struct RefGateCounters {
            /// @name Tier T1r — the gate's OWN flag rate, counted UNCONDITIONALLY.
            /// @{
            /// These two are incremented whether or not @ref RefGateParams::enable
            /// is armed, because the LEGACY condition always runs and always acts.
            /// Every other counter here is hardened-gate-only telemetry.  Without
            /// them the reference arm is invisible in `network_flag_rates.csv`,
            /// which reads `T1 … flags=0` for runs the tier drove thousands of
            /// refloats on (standing invariant #4 —
            /// docs/SPEC_amb_iono_split_overcount.md §6.14.5 defect 1).
            long long gate_opportunities{0};     ///< Station-epoch-constellations with a family of n ≥ 2 finite non-reference DDs (pos-gated epochs excluded — flagging is suppressed there).
            long long gate_flags{0};             ///< …of those, the ones that fired (legacy condition, or the hardened verdict when armed).
            /// @}
            long long legacy_candidates{0};      ///< |median| > floor_frac·λ_IF with n ≥ 2 (the legacy fire condition).
            long long fires{0};                  ///< Reference slips actually flagged (both kinds).
            long long fires_station_event{0};    ///< …of those, the incoherent multi-satellite kind (fired WITHOUT the median rebase).
            long long suppressed_scale{0};       ///< Rejected by the empirical scale or the coherence test.
            long long suppressed_thin{0};        ///< Deferred: family below @ref RefGateParams::min_n.
            long long suppressed_persistence{0}; ///< Rejected by the consecutive-fire cap (escalated instead).
            long long reselects{0};              ///< Forced reference re-selections.
            /// Declined while the empirical arm was still cold
            /// (@ref RefGateParams::warmup_abstain).  Permanently 0 when that
            /// knob is off — which is the honest reading, not a dead counter:
            /// with it off the gate does not abstain, it falls back to the
            /// legacy floor, and those fires are counted by @ref fires.
            long long suppressed_warmup{0};
        };

        /// Card R verdict: what the hardened gate decided about a candidate.
        enum class RefGateVerdict {
            Suppress,       ///< Not a reference event — drift, thin family, or a capped run.
            FireRebase,     ///< A COHERENT common step: the legacy action (reset key + median rebase of prev_grad).
            FireNoRebase    ///< A multi-satellite station event: reset the reference, but do NOT claim the median as its slip.
        };

        /// Install the Card-R gate parameters (network wiring only).
        void setRefGate(const RefGateParams& p) { ref_gate_ = p; }
        /// Card-R run telemetry for the run-end summary.
        const RefGateCounters& refGateCounters() const { return ref_gate_counters_; }

        /**
         * @brief Defer per-satellite outlier flagging to an external (network) pool.
         *
         * When enabled, detect() still groups, single-differences, double-differences,
         * applies the reference-change / reference-slip corrections, caches the jumps
         * (see @ref getLastIfJumps) and carries the gradients forward — but it does
         * NOT run its own per-station MAD outlier flag (so it does not exclude
         * per-satellite-flagged arcs from the next epoch's gradients; the caller is
         * responsible for calling @ref resetSatellite on arcs it flags from the pool).
         * The reference-satellite slip check still runs (it is a per-station
         * common-mode test the network pool cannot see). Default false (self-contained
         * per-station flagging, the original behaviour).
         *
         * @param defer True to defer per-satellite flagging to the caller.
         */
        void setDeferFlagging(bool defer) { defer_flagging_ = defer; }

        /**
         * @brief Enable CSV tracing for IF phase slip diagnostics.
         *
         * Opens or creates "if_slip_trace.csv" in the specified directory.
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened, false otherwise.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Check if tracing is currently enabled.
         * @return true if the trace file is open and writable.
         */
        bool isTracingEnabled() const { return trace_file_.is_open(); }

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        PppUtils ppp_utils_;

        /** When true, per-satellite outlier flagging is deferred to the caller
         *  (network-pooled MAD path); see @ref setDeferFlagging. */
        bool defer_flagging_{ false };

        /** Minimum elevation (degrees) for a satellite to be chosen as reference. */
        static constexpr double kRefElevThreshDeg = 45.0;

        /** Number of MAD-sigmas for the adaptive outlier threshold. */
        static constexpr double kOutlierSigmaMultiplier = 8.0;

        /** Maximum number of iterative outlier removal passes. */
        static constexpr size_t kMaxOutlierPasses = 10;

        /**
         * @brief 3D position sigma threshold (meters) for gating slip detection.
         *
         * When the caller-provided position_sigma_3d exceeds this value, the
         * detector still computes and stores between-satellite gradients (so
         * that between-epoch differences are available once the position
         * converges) but suppresses outlier flagging.
         *
         * Rationale: a satellite pair separated by ~60° in the sky and a
         * position error of ?pos produces a differential OMC of roughly
         * |?pos| * sin(60°) ? 0.87 * |?pos|.  For GPS L1/L2 the IF
         * wavelength is ~10.7 cm, so even a 0.5 m position uncertainty
         * can produce ~0.43 m of differential error  several IF cycles.
         * A threshold of 0.5 m provides a conservative margin.
         */
        static constexpr double kPositionSigmaThresholdM = 0.5;

        /**
         * @brief Per-constellation persistent state across epochs.
         */
        struct ConstellationState {
            /** Previous epoch between-satellite IF OMC gradients keyed by satellite ID. */
            std::map<std::string, double> prev_grad;

            /** Previous epoch LOS unit vectors keyed by satellite ID (for geometry correction). */
            std::map<std::string, Eigen::Vector3d> prev_los;

            /** Receiver position at the previous epoch (ECEF meters). */
            Eigen::Vector3d prev_position{ Eigen::Vector3d::Zero() };

            /** True if prev_position has been set at least once. */
            bool has_prev_position{ false };

            /** Current reference satellite ID for this constellation. */
            std::string ref_satellite;

            /** Current reference satellite elevation (degrees). */
            double ref_elevation{ 0.0 };

            // --- Card R: hardened reference-gate state (inert while the gate
            //     is disabled — nothing below is ever read or written then) ---
            /** EWMA of the non-reference DD dispersion, 1.4826·MAD (raw m). */
            double ref_scale_ewma{ 0.0 };
            /** Samples folded into @ref ref_scale_ewma (shrinkage weight). */
            long ref_scale_count{ 0 };
            /** Reference satellite of the most recent fire (run-length identity). */
            std::string ref_fire_sat;
            /** Detector epoch index of the most recent fire (−1 = never). */
            long long ref_last_fire_epoch{ -1 };
            /** Consecutive fires on @ref ref_fire_sat. */
            int ref_consec_fires{ 0 };
            /** Set by an escalation: force @ref selectReference to re-elect. */
            bool ref_force_reselect{ false };
            /** Satellite the escalation excludes from the next election. */
            std::string ref_exclude;
        };

        /** Per-constellation state keyed by system string (e.g. "GPS"). */
        std::map<std::string, ConstellationState> constellation_state_;

        /** Cached per-satellite IF jump from the most recent detect() call.
         *  Populated at the end of detect() so that getLastIfJump() can
         *  return values without re-computing. */
        std::map<std::string, IfJumpInfo> last_if_jumps_;

        /** @brief Per-satellite data collected during observation grouping. */
        struct SatData {
            std::string satellite;
            std::string phase_if_key;
            std::string sig1, sig2;
            double phi_if_omc;
            double elevation;
            double lambda_if;
            System system;
            Eigen::Vector3d los{ Eigen::Vector3d::Zero() }; ///< LOS unit vector for geometry correction.
        };

        bool findPhaseIfLabel(const PppObservation& obs, std::string& phase_if_key) const;
        bool splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const;
        double computeIfWavelengthMeters(const PppObservation& obs,
            const std::string& sig1, const std::string& sig2) const;

        std::map<std::string, double> checkOutliers(
            const std::map<std::string, double>& values,
            const std::string& ref_satellite,
            double lambda_if) const;

        /** @brief Compute median of a vector (sorts in-place). */
        static double computeMedian(std::vector<double>& values);

        /**
         * @brief Group observations by constellation and extract IF phase OMC data.
         * @param observations Epoch observations.
         * @return Map of system string -> vector of SatData.
         */
        std::map<std::string, std::vector<SatData>> groupByConstellation(
            const std::vector<PppObservation>& observations) const;

        /**
         * @brief Select or validate the reference satellite for a constellation.
         * @param sat_vec  Satellite data for this constellation.
         * @param cs       Persistent constellation state (ref is updated in-place).
         * @return True if the reference changed this epoch.
         */
        bool selectReference(const std::vector<SatData>& sat_vec, ConstellationState& cs) const;

        /**
         * @brief Process double differences for one constellation and flag slips.
         *
         * Handles reference-change correction, reference-slip check, and
         * non-reference outlier detection. Appends flagged keys to reset_keys.
         *
         * @param sat_vec         Satellite data for this constellation.
         * @param sys_str         System string (for logging).
         * @param cs              Persistent constellation state.
         * @param grad_phi_if     Current-epoch between-satellite gradients.
         * @param lambda_if       Median IF wavelength for this constellation.
         * @param ref_changed     Whether the reference satellite changed.
         * @param position_sigma_3d 3D position sigma (meters).
         * @param[out] reset_keys Ambiguity keys to reset are appended here.
         */
        std::map<std::string, double> processDoubleDifferences(
            const std::vector<SatData>& sat_vec,
            const std::string& sys_str,
            ConstellationState& cs,
            const std::map<std::string, double>& grad_phi_if,
            double lambda_if,
            bool ref_changed,
            double position_sigma_3d,
            const Eigen::Vector3d& delta_pos,
            std::vector<std::string>& reset_keys);

        /**
         * @brief Card R: advance this constellation's non-reference DD
         *        dispersion scale and return the PRE-update effective scale.
         *
         * Called on EVERY epoch that has a usable family (≥ 2 finite non-ref
         * DDs) while the gate is armed — deliberately not only on the epochs
         * where the legacy condition fired.  A scale measured solely on
         * candidate epochs would be conditioned on a large median, letting a
         * persistent drift raise its own acceptance threshold until a genuine
         * slip no longer clears it.
         *
         * @param cs            Constellation state (mutated: EWMA + sample count).
         * @param non_ref_dds   Finite non-reference double differences (raw m).
         * @param median_dd     Their median (raw m).
         * @param cold_prior    Scale used while no empirical samples exist —
         *                      the physical floor expressed as a scale
         *                      (floor_frac·λ_IF / k), so a cold gate degenerates
         *                      to exactly the legacy physical threshold.
         * @param[out] disp_out This epoch's dispersion, 1.4826·MAD (raw m).
         * @return The empirical-Bayes effective scale s_eff BEFORE this fold (m).
         */
        /// @param sin_bar Mean sin(elevation) over the non-reference family,
        ///        elevations clamped at @ref RefGateParams::elev_clamp_deg.
        ///        Used only when @ref RefGateParams::elev_norm is set, to store
        ///        the ZENITH-equivalent dispersion in the EWMA; ignored otherwise.
        double updateRefDispersion(ConstellationState& cs,
            const std::vector<double>& non_ref_dds,
            double median_dd, double cold_prior, double sin_bar, double& disp_out);

        /**
         * @brief Card R: adjudicate a candidate reference-satellite slip.
         *
         * Called ONLY when the legacy condition (|median| > floor_frac·λ_IF,
         * n ≥ 2) already held and @ref RefGateParams::enable is set.  Updates
         * the persistence/escalation state, increments @ref ref_gate_counters_,
         * and returns whether the reference slip should actually fire.
         *
         * @param cs            Constellation state (mutated: fire run, escalation flags).
         * @param non_ref_dds   Finite non-reference double differences (raw m).
         * @param disp          This epoch's dispersion, 1.4826·MAD (raw m).
         * @param s_eff         Pre-update effective empirical scale (raw m).
         * @param median_dd     Median of the non-reference DDs (raw m).
         * @param lambda_if     Constellation median IF wavelength (m).
         * @param sys_str       System string (logging).
         * @param sin_bar       Mean sin(el) over the family (clamped); the scale
         *                      the EWMA was normalized BY.
         * @param sin_ref       sin(el) of the REFERENCE satellite (clamped); the
         *                      elevation the common term is divided by.
         *                      Both are used only when
         *                      @ref RefGateParams::elev_norm is set.
         * @return The verdict; see @ref RefGateVerdict.
         */
        RefGateVerdict evaluateRefGate(ConstellationState& cs,
            const std::vector<double>& non_ref_dds, double disp, double s_eff,
            double median_dd, double lambda_if,
            const std::string& sys_str, double sin_bar, double sin_ref);

        /**
         * @brief Write a single trace CSV record.
         */
        void writeTraceRecord(const SatData& sd, const std::string& sys_str,
            double grad, double delta_grad,
            const std::string& ref_sat, bool ref_changed,
            double lambda_if, bool is_slip,
            const std::string& slip_reason,
            double position_sigma_3d, bool pos_gated) const;

        /** Output stream for IF slip trace CSV file. */
        mutable std::ofstream trace_file_;

        /** Current epoch time for tracing. */
        mutable GnssTime current_epoch_;

        /** Model-nuisance gate threshold, m; 0 ⇒ disabled (§6.14.6 item 3). */
        double nuisance_max_m_{ 0.0 };

        /// This epoch's combined gating verdict (position OR model nuisance),
        /// computed once in @ref detect.  @ref processDoubleDifferences reads it
        /// rather than re-deriving the predicate from `position_sigma_3d`, which
        /// is how the two definitions silently drifted apart when the nuisance
        /// arm was added — the reference gate would have kept firing through a
        /// gate the caller believed was closed.
        bool epoch_gated_{ false };

        /** Card R: gate parameters (default-constructed ⇒ legacy behaviour). */
        RefGateParams ref_gate_{};

        /** Card R: run telemetry. */
        RefGateCounters ref_gate_counters_{};

        /** Monotonic detect() call index — the unit the Card-R persistence
         *  run-length is measured in (epoch identity without a time model). */
        long long epoch_counter_{ 0 };
    };

} // namespace GnssProcessor
