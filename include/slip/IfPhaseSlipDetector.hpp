#pragma once

/**
* @file IfPhaseSlipDetector.hpp
* @brief Between-satellite, between-epoch ionosphere-free phase slip detector.
*
* @note Built and wired on the network path; staged on the user and kinematic
*       paths.  The network estimator pins station positions, which meets the
*       static-site precondition of the position-uncertainty gating below.  The
*       detector is compiled (see CMakeLists.txt), and when
*       `network.cycle_slip.if_phase_td` is set, network setup
*       (ApplicationNetworkSetup.cpp, called from Application::runNetwork)
*       builds one per non-pivot station, or one per station under the
*       prioritized phase datum.  On that path it runs with deferred flagging:
*       the driver pools the stations' double differences by signal pair, flags
*       against the pooled MAD, and feeds the flags to the invariant-carry slip
*       funnel (card 4, C3c) and, when the phase-leveling carry is the active
*       one, to its refloat.  In the general user pipeline (kinematic, or before
*       convergence) the detector is inert: the position gating below is unsafe
*       there, and the `enable_if_slip_detector` user toggle has no
*       instantiation path.  User-side slip detection is in
*       slip/MwSlipDetector and slip/GraphicSlipDetector.
*
* The detector double-differences the IF phase OMC (observed minus computed).
* The model subtraction is essential: it removes the satellite-specific
* geometric range (about 20,000 km) and the satellite clock, which change by
* meters per epoch and do not cancel when raw observables are differenced
* between satellites.  After the subtraction the OMC holds the receiver
* clock, ZWD, ISB, ambiguity and noise, and the double difference cancels all
* of them except the noise.
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
*   5. Applies an adaptive MAD-based outlier threshold and fixed thresholds in
*      units of the IF wavelength (deferred to the pooled test on the network
*      path; see setDeferFlagging).
*
* The MW detector is limited by code noise (about 0.3 WL cycles).  This
* detector works at carrier-phase precision (millimeters) and detects slips
* of 1–2 IF cycles that the MW and post-fit residual checks miss.
*
* Position-uncertainty gating:
*   The between-satellite single difference does not cancel the receiver
*   position contribution: a position error Δpos leaves a differential
*   geometric residual of about (e_sat - e_ref) · Δpos, which can reach
*   meters before convergence.  To avoid false slip flags the detector
*   accepts an optional 3D position sigma (from the filter covariance) and
*   suppresses outlier flagging while the position is uncertain.  Gradients
*   are still stored, so detection resumes directly once the position
*   converges.
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
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @class IfPhaseSlipDetector
     * @brief Detects cycle slips via between-satellite, between-epoch IF phase OMC differences.
     *
     * The detector works per constellation (per gradient chain under
     * setAllPairs): it computes the IF phase OMC (observed - modeled) to
     * remove the large satellite-specific geometric range and satellite
     * clock, then forms between-satellite single differences (removing
     * common receiver clock, ISB, phase clock), then between-epoch double
     * differences (removing constant ambiguities and slowly-varying ZWD).
     * A clean double difference should be near zero; a slip of N cycles on
     * both bands appears as a jump of N * lambda_IF (a slip on one band
     * gives a larger jump that is not a multiple of lambda_IF).
     *
     * When the estimated position is uncertain (pre-convergence), the
     * differential geometric error (e_sat - e_ref) · Δpos can exceed the
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
         * Processes observations grouped by constellation (by gradient chain
         * under @ref setAllPairs).  Returns a vector of ambiguity keys that
         * should be reset (format: "G01_L1C_L2W").
         *
         * The IF phase OMC (observed_IF - modeled_IF) is used.  The modeled
         * value removes the large satellite-specific geometric range and
         * satellite clock that change by meters per epoch.  After model
         * subtraction, the OMC contains only receiver clock, ZWD*mf_w,
         * ISB, phase clock, ambiguity and noise:
         *   - Between-satellite diff cancels rx_clk, ISB, phase_clk.
         *   - Between-epoch diff cancels ZWD*mf_w and ambiguity.
         * The statistic therefore does not depend on the filter's clock, ISB,
         * phase-clock or ambiguity states.  Two filter-driven terms survive:
         * the epoch-to-epoch change of the modelled ZWD (@p model_nuisance_m)
         * and of the receiver position (removed by the geometry correction
         * from @c receiver_position, and gated by @p position_sigma_3d).
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @param position_sigma_3d Optional 3D position uncertainty (meters).
         *        When > kPositionSigmaThresholdM the detector stores gradients
         *        but suppresses slip flagging to avoid false detections from
         *        differential geometric error during pre-convergence.
         *        Pass 0.0 or omit it to turn this gate off (the original behaviour).
         * @param model_nuisance_m Magnitude (m) of the epoch-to-epoch change in
         *        the modelled part of the OMC that does not cancel in the
         *        between-satellite and between-epoch differencing: in practice
         *        |ΔZWD|, the one filter state that survives both (§6.14.6 item 3).
         *        Compared against the threshold set by @ref setNuisanceGate.  The
         *        gate is off while that threshold is 0 (the default), and a caller
         *        that omits this argument passes 0, so existing callers are
         *        unchanged.
         * @return std::vector<std::string> Vector of ambiguity keys to reset.
         */
        std::vector<std::string> detect(const std::vector<PppObservation>& observations,
            double position_sigma_3d = 0.0,
            const Eigen::Vector3d& receiver_position = Eigen::Vector3d::Zero(),
            double model_nuisance_m = 0.0);

        /**
         * @brief Arm the model-nuisance gate (§6.14.6 item 3).
         *
         * Counterpart of the position-uncertainty gate for the nuisance that
         * survives on the network path.  Station positions are pinned there, so
         * `position_sigma_3d` is passed as 0.0 and the position gate never
         * fires.  The term that survives the differencing is
         * Δ(ZWD)·(mf_sat − mf_ref), and its effect is measurable: over the first
         * epochs of a cold run the reference gate's fires fall together with
         * the median |ΔZWD|, and after the solution settles the top |ΔZWD|
         * decile still fires at about twice the baseline rate.
         *
         * A gated epoch suppresses this detector's own flagging and its
         * reference arm, and publishes its jumps with IfJumpInfo::valid false,
         * so the network pool skips them as well.
         *
         * @param max_m Suppress flagging when @c model_nuisance_m exceeds this.
         *        0 (default) disables the gate; the output is then byte-identical.
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
         * @brief Clear IF phase history for a satellite in one gradient chain.
         *
         * Per-pair counterpart of @ref resetSatellite (§14.10).  Each band pair
         * has its own @c constellation_state_ entry, so a flag on the (L1C,L5Q)
         * pair restarts that chain only.  Resetting the whole satellite would
         * also discard the (L1C,L2W) chain that did not flag, which loses a
         * clean arc and makes one pair's decision depend on another's.
         *
         * A @p state_key with no entry is a silent no-op (the pair had no chain
         * to restart).
         *
         * @param state_key Chain key, i.e. @ref IfJumpInfo::state_key of the
         *                  flagged sample ("GPS" for a first pair, "GPS|L1C_L5Q"
         *                  for an additional one).
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatelliteInState(const std::string& state_key,
                                   const std::string& satellite);

        /**
         * @brief Keep a flagged satellite's gradient as the origin of the next
         *        double difference instead of clearing it (card ARL, slip-ladder
         *        defect 2; @c network.cycle_slip.if_phase_td_reseed_on_flag).
         *
         * The alternative to @ref resetSatellite for a driver that flags from the
         * pooled test (@ref setDeferFlagging).  It is called after the flag epoch's
         * detect() and before the next one.  At that point every chain's stored
         * gradient for @p satellite is the flag epoch's own between-satellite
         * single difference, which already contains the step.  Keeping it means
         * the next epoch's double difference measures only what happened between
         * the two epochs: about 0 on an arc that stays clean, and the size of a
         * second step if the slip repeats in that epoch.  A reset leaves that
         * epoch with no double difference, so a slip repeated there passes the
         * detector unseen (repetition 2 of slip-ladder rung S7).
         *
         * The reference handling is that of a reset: a chain whose reference is
         * @p satellite clears it, and the next epoch elects one again.  The
         * satellite's next sample on each re-seeded chain is published with
         * IfJumpInfo::after_reseed set.
         *
         * A one-epoch outlier (a spike, not a step) reads with the opposite sign in
         * the next epoch, so under re-seeding it is flagged twice where a reset
         * flags it once.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @return true when at least one chain held a flag-epoch gradient for the
         *         satellite, so the next epoch has a double difference; false when
         *         there was none to keep (the call then acts as a reset).
         */
        bool reseedSatellite(const std::string& satellite);

        /**
         * @brief Keep a flagged satellite's gradient in one gradient chain.
         *
         * Per-pair counterpart of @ref reseedSatellite, as
         * @ref resetSatelliteInState is of @ref resetSatellite (§14.10).  A
         * @p state_key with no entry is a silent no-op and returns false.
         *
         * @param state_key Chain key, i.e. @ref IfJumpInfo::state_key of the
         *                  flagged sample.
         * @param satellite Satellite identifier (e.g. "G01").
         * @return true when the chain held a flag-epoch gradient for the
         *         satellite; false otherwise.
         */
        bool reseedSatelliteInState(const std::string& state_key,
                                    const std::string& satellite);

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
            double lambda_if_m{ 0.0 };   ///< IF wavelength c/(f1+f2) (m): the IF jump of a (1,1) slip, and the unit of the physical floor.
            double elevation_deg{ 0.0 };  ///< Satellite elevation (degrees) at detection epoch.
            std::string sig1;             ///< First signal of the IF pair in key order (e.g. "L1C"); the higher band on GPS pairs.
            std::string sig2;             ///< Second signal of the IF pair in key order (e.g. "L2W").
            bool   valid{ false };        ///< True if a finite jump was computed on an epoch that was not gated (position or model nuisance).
            /// True for the chain's reference satellite, whose double difference
            /// is identically 0 by construction (grad(ref) = OMC(ref) − OMC(ref)
            /// = 0, so dd(ref) = 0 every epoch).  The network pool arm excludes
            /// these structural zeros from its robust scale: they sit at the
            /// centre of the distribution and deflate the MAD
            /// (docs/SPEC_amb_iono_split_overcount.md §6.14.5, defect 2).  They
            /// carry no information about the noise the threshold tracks, and
            /// the pool arm cannot flag the reference in any case (its deviation
            /// is about 0), so a separate per-station reference gate covers it
            /// (flag-rate tier T1r).
            bool   is_reference{ false };
            /// Key of the @c constellation_state_ entry (gradient chain) this
            /// sample came from (§14.10).  Under @ref setAllPairs the first pair
            /// of a satellite keeps the plain system string ("GPS"), so its chain
            /// and its decisions are byte-identical to those with the knob off;
            /// every additional pair gets a key such as "GPS|L1C_L5Q" and its own
            /// prev_grad, and takes the first chain's reference where that
            /// satellite is present (§14.10.4).  A driver that flags one pair
            /// resets only that chain (@ref resetSatelliteInState): resetting the
            /// satellite would discard the other pairs' clean chains and make the
            /// per-pair decisions depend on each other, which per-pair evaluation
            /// must avoid.
            std::string state_key;
            /// True when a driver re-seeded this chain's gradient for the satellite
            /// after the previous epoch's flag (@ref reseedSatellite): this sample is
            /// the post-flag evaluation that a reset leaves out.  Always false
            /// while no driver re-seeds.
            bool after_reseed{ false };
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
         * First band pair of each satellite only; @ref getLastIfJumpsAllPairs
         * holds every pair.  Used by the network producer's deferred-flagging
         * path: the driver pools these between-epoch double differences across
         * all stations by signal pair to form a robust per-pair MAD, then
         * thresholds (max(k·MAD, floor)) and refloats, instead of each station
         * thresholding its own handful of satellites.  Keyed by satellite id.
         */
        const std::map<std::string, IfJumpInfo>& getLastIfJumps() const { return last_if_jumps_; }

        /// Key of @ref getLastIfJumpsAllPairs: (satellite, "&lt;sig1&gt;_&lt;sig2&gt;").
        using SatPairKey = std::pair<std::string, std::string>;

        /**
         * @brief Every (satellite, band pair) IF jump from the last detect() call.
         *
         * Always populated, whichever pair mode is set, so it is a superset of
         * @ref getLastIfJumps (§14.10).  With @ref setAllPairs off it holds the
         * same samples under two-part keys; with it on it also holds the
         * additional pairs, so a triple-band satellite contributes three pairs.
         * The network driver reads this map under
         * `network.cycle_slip.if_phase_td_pairs = all` and @ref getLastIfJumps
         * otherwise, which keeps the default path byte-identical.
         *
         * @return (satellite, pair label) → jump; ordered, so pooling is
         *         deterministic.
         */
        const std::map<SatPairKey, IfJumpInfo>& getLastIfJumpsAllPairs() const {
            return last_if_jumps_all_;
        }

        /**
         * @brief Evaluate every distinct pair of a satellite's phase bands (§14.10).
         *
         * Off (default), the detector forms one pair per satellite from its
         * first two phase signals in key order (L1C+L2W on GPS, L1C+L5Q on
         * Galileo); if those two share a frequency, the satellite forms no pair.
         * On, it also forms every other pair with distinct frequencies, each in
         * its own @c constellation_state_ entry with its own between-epoch
         * gradient chain and its own reset key; an additional chain takes the
         * first chain's reference where that satellite is present and elects
         * its own otherwise (§14.10.4).  The first pair keeps the plain
         * system-string entry, so its population, reference, double differences
         * and log lines are byte-identical to those with the knob off, and the
         * extra pairs are a pure addition.
         *
         * @param all True to evaluate every pair.
         */
        void setAllPairs(bool all) { all_pairs_ = all; }

        /**
         * @brief Hardened reference-satellite slip gate (card R,
         *        docs/SPEC_slip_qc_hardening.md §2).
         *
         * The legacy gate is the last non-empirical decision in the IF-TD chain:
         * it fires whenever |median(non-ref ΔΔ)| exceeds a fixed 0.5·λ_IF with
         * only n ≥ 2 contributors, with no elevation normalization, no
         * empirical scale and no test that tells a step from a drift.  On a thin
         * signal family a persistent sub-slip drift therefore re-fires it every
         * epoch, while the raw wide-lane and geometry-free series stay flat to
         * ±0.1 cycles and no LLI is set.  Once the carry is warm, every fire
         * refloats both band ambiguities and marks the invariant-carry chords
         * slipped (card 4).
         *
         * When @ref RefGateParams::enable is set the decision becomes:
         *   1. Scale: |median| must exceed max(k·s_eff, floor_frac·λ_IF), where
         *      s_eff is an empirical-Bayes blend of this chain's EWMA of the
         *      non-reference DD dispersion (before this epoch's fold) with a
         *      cold prior, the physical floor divided by k (see
         *      @ref updateRefDispersion).  Same shape as the network pool arm;
         *      no stochastic-model σ enters.
         *   2. Coherence: a genuine reference slip is a common step, so every
         *      non-reference double difference moves together and the family's
         *      dispersion is small relative to |median|; a drift scatters.  The
         *      legacy gate has no such step-versus-drift test.  It rebases by
         *      the median after each fire, so a re-fire is evidence of persistent
         *      drift, not of stale state.  An incoherent family in which at
         *      least max(2, ⌈n/2⌉) members individually exceed λ_IF is a
         *      multi-satellite station event: the reference is reset without
         *      the median rebase (RefGateVerdict::FireNoRebase).
         *   3. Thin family: below @ref RefGateParams::min_n contributors the
         *      median is not a trustworthy common-mode estimate; the physical
         *      floor is widened by @ref RefGateParams::thin_widen and each
         *      deferral is counted, so the behaviour change is never silent.
         *   4. Persistence and escalation: after
         *      @ref RefGateParams::max_consecutive consecutive fires on the same
         *      reference, the gate stops re-firing and forces a reference
         *      re-selection that excludes that satellite.  The drifting
         *      satellite then enters the network pool arm, where an
         *      elevation-normalized, empirically scaled residual judges it.  The
         *      spec's further step (a new reference that slips at once is a
         *      station-local common mode and goes to the pool/RAIM layer) is not
         *      implemented: a receiver-level common mode cancels in the
         *      between-satellite difference, and on the re-election epoch the
         *      median-offset rebase removes it before the gate runs.
         *
         * Default-constructed params leave @c enable false, so the legacy gate
         * runs unchanged and the output is byte-identical.  The user/kinematic
         * path never calls this setter, so it is unaffected in both modes.
         */
        struct RefGateParams {
            bool   enable{false};          ///< Master switch (default off: legacy gate).
            double k{6.0};                 ///< Empirical multiplier on the effective dispersion scale.
            int    min_n{4};               ///< Minimum non-reference DDs for a trustworthy median.
            double thin_widen{3.0};        ///< Physical-floor multiplier below @ref min_n.
            double coherence{0.5};         ///< Largest dispersion/|median| accepted as a common step (dispersion = 1.4826·MAD of the non-ref DDs).
            /// Consecutive fires on one reference before escalating to a forced
            /// re-selection.  The default 1 matches the config default of the only
            /// path that arms the gate
            /// (`network.cycle_slip.if_phase_td_ref_max_consecutive`, GnssConfigTypes
            /// SlipQcConfig).  Spec rule R2 says a genuine reference slip fires
            /// once, so a real one-epoch common step never reaches the cap, while a
            /// persistent drift escalates on its second epoch (on its third with a
            /// cap of 2).  On the audit configuration a family of 14 false fires
            /// drops to 6 with a cap of 2 and to 4 with a cap of 1, and a
            /// genuine station event is unaffected at either cap.  Keep this
            /// default equal to the config default: if the two differ, unit tests
            /// certify a gate that field runs do not use (spec-followups N2).
            int    max_consecutive{1};
            double ewma_alpha{0.15};       ///< EWMA weight for the dispersion scale.
            double shrink_kappa{20.0};     ///< Empirical-Bayes shrinkage constant (samples).
            double floor_frac{0.5};        ///< Physical floor in units of λ_IF (0.5 = the legacy value).

            /// Abstain while the empirical arm has no samples (§6.14.6 item 1).
            ///
            /// With no empirical information the cold prior `floor_frac·λ_IF/k`
            /// makes `k·s_eff` equal `phys`, and while samples are few it stays
            /// close to it, so for its first epochs the hardened gate reduces to
            /// the legacy fixed floor.  That is deliberate: it keeps the scale
            /// from being contaminated by the epoch's own dispersion (see
            /// @ref updateRefDispersion).  The consequence is that every cold run
            /// starts on the 3σ bar (0.5·λ_IF) that the hardened gate is meant to
            /// replace.  In those epochs `carry_warm` is false (n_emitted below
            /// `network.phase_leveling_warmup_epochs`, default 6), so a detection
            /// is logged as `action=diagnostic` and cannot act; firing there only
            /// adds events that change nothing.
            ///
            /// When set, the gate returns @ref RefGateVerdict::Suppress while
            /// @c ConstellationState::ref_scale_count is below @ref shrink_kappa,
            /// the sample count at which the shrinkage weight reaches ½ and the
            /// empirical arm starts to carry information.  This differs from the
            /// rejected "shrink toward this epoch's dispersion": abstention
            /// asserts nothing about the scale, it declines to decide.  Cost:
            /// this arm does not catch a genuine reference slip inside the
            /// warm-up; such a detection could not act there anyway, and MW/LLI
            /// still screen the window.  Default false; with it off, the output
            /// is byte-identical.
            bool   warmup_abstain{false};

            /// Elevation-normalize the empirical scale (§6.14.6 item 2).
            ///
            /// The gate tests the median of the non-reference DDs, and 88 % of its
            /// variance is the reference satellite's own noise divided by
            /// sin(el_ref) (measured in §6.14.2): a term common to every member
            /// that no family size can average away (σ(median) measured flat at
            /// 17.3–18.4 mm for n = 3…10).  The scale it is judged against,
            /// @c disp, is the family's scatter about that median, i.e. the
            /// per-satellite noise at the family's own elevations.  Unlike the
            /// pool arm the gate applies no elevation map, so `k·s_eff` is a raw
            /// mixture over whatever elevations happened to be up, held per
            /// (station, chain) in state that outlives the reference it describes.
            ///
            /// When set, the EWMA stores the zenith-equivalent scale
            /// `disp·⟨sin el⟩_family` (a stationary station property that
            /// survives a reference handover) and the test rebuilds the
            /// statistic's own sigma from it:
            /// @code
            ///   σ_ref = s_z / sin(el_ref)                  // the common term
            ///   σ_fam = 1.2533 · (s_z / ⟨sin el⟩) / √n     // median of n iid
            ///   thr   = max(phys, k · hypot(σ_ref, σ_fam)) // phys × thin_widen below min_n
            /// @endcode
            /// On the 24 h bed, s_z ≈ 12.8 mm and el_ref ≈ 62° give σ_ref 14.5 mm
            /// (measured station-local common term 14.83 mm) and σ_fam 10.1 mm,
            /// hypot 17.7 mm, against a measured σ(median) of 17.70 mm.  The
            /// threshold follows el_ref and n, which the flat `k·disp` form
            /// cannot.  The physical floor stays raw, since a slip is a slip at
            /// any elevation (slip-QC lesson 7).  Default false; with it off, the
            /// output is byte-identical.
            bool   elev_norm{false};

            /// Elevation clamp for @ref elev_norm, degrees.  Matches the pool
            /// arm's own clamp so a horizon arc cannot explode 1/sin(el).
            double elev_clamp_deg{5.0};
        };

        /**
         * @brief Reference-gate run telemetry (card R; standing invariant #4).
         *
         * @c legacy_candidates counts the epochs on which the legacy condition
         * held while the hardened gate was armed, so `legacy_candidates − fires`
         * is exactly what the hardening removed, and the four `suppressed_*`
         * counters say why.
         */
        struct RefGateCounters {
            /// @name Tier T1r: the gate's own flag rate, counted unconditionally.
            /// @{
            /// These two are incremented whether or not @ref RefGateParams::enable
            /// is set, because the reference arm runs and acts in both modes (with
            /// the gate off, the legacy condition decides).  Every other counter
            /// here is hardened-gate telemetry only.  Without these two the
            /// reference arm's flags do not appear in `network_flag_rates.csv`,
            /// whose T1 tier covers only the pool arm (standing invariant #4;
            /// docs/SPEC_amb_iono_split_overcount.md §6.14.5, defect 1).
            long long gate_opportunities{0};     ///< Station-epoch-chains (one chain per constellation by default) on which the arm ran with a family of n ≥ 2 finite non-reference DDs; gated epochs (position or model nuisance) are excluded because flagging is suppressed there.
            long long gate_flags{0};             ///< …of those, the ones that fired (legacy condition, or the hardened verdict when armed).
            /// @}
            long long legacy_candidates{0};      ///< |median| > floor_frac·λ_IF with n ≥ 2 (the legacy fire condition).
            long long fires{0};                  ///< Reference slips actually flagged (both kinds).
            long long fires_station_event{0};    ///< …of those, the incoherent multi-satellite kind (fired without the median rebase).
            long long suppressed_scale{0};       ///< Rejected by the empirical scale or the coherence test.
            long long suppressed_thin{0};        ///< Deferred: family below @ref RefGateParams::min_n.
            long long suppressed_persistence{0}; ///< Rejected by the consecutive-fire cap (escalated instead).
            long long reselects{0};              ///< Forced reference re-selections.
            /// Declined while the empirical arm was still cold
            /// (@ref RefGateParams::warmup_abstain).  Stays 0 when that knob is
            /// off: the gate then does not abstain but falls back to the legacy
            /// floor, and those fires are counted by @ref fires.
            long long suppressed_warmup{0};
        };

        /// Verdict of the hardened reference gate on a candidate (card R).
        enum class RefGateVerdict {
            Suppress,       ///< Not a reference event: no candidate, warm-up abstention, drift, thin family, or a capped run.
            FireRebase,     ///< A coherent common step: the legacy action (reset key + median rebase of prev_grad).
            FireNoRebase    ///< A multi-satellite station event: reset the reference, but do not claim the median as its slip.
        };

        /// Install the reference-gate parameters (card R); only network setup calls this.
        void setRefGate(const RefGateParams& p) { ref_gate_ = p; }
        /// Reference-gate run telemetry for the run-end summary (card R).
        const RefGateCounters& refGateCounters() const { return ref_gate_counters_; }

        /**
         * @brief Outcome of one chain's reference arm this epoch (§22.6).
         *
         * Recorded by the first chain so that the additional chains that adopted
         * its reference can inherit the verdict, instead of passing the
         * reference's own step into their pooled outlier test, where it would
         * appear on every other satellite.  Lives for one epoch only.
         */
        struct RefArmOutcome {
            std::string    satellite;                            ///< The chain's reference satellite ("" = no verdict: the arm did not run or had no usable family).
            RefGateVerdict verdict{ RefGateVerdict::Suppress };   ///< What the arm decided.
            bool           fired{ false };                        ///< verdict != Suppress.
        };

        /**
         * @brief Per-gradient-chain reference-arm telemetry (§22.6).
         *
         * Keyed by chain key ("GPS", "GPS|L2W_L5X").  The aggregate
         * @ref RefGateCounters sums every chain into one number, so it cannot
         * show whether the additional chains' arm is a flood or a backstop
         * (standing invariant #4: the run-end counters are the on-path
         * telemetry).
         */
        struct RefArmChainCounters {
            long long opportunities{ 0 };      ///< Epochs with a usable family (n ≥ 2 finite non-reference DDs) on which the arm ran on this chain or a fired first-chain verdict was inherited.
            long long candidates{ 0 };         ///< …of those, |median| > floor_frac·λ_IF (the legacy candidate condition: the decision with the hardened gate off, a pre-filter with it on, telemetry only on an inherited fire).
            long long fires_own{ 0 };          ///< Fires decided by this chain's own arm (on an additional chain, only through the hardened gate).
            long long fires_inherited{ 0 };    ///< Fires inherited from the first chain's verdict on an adopted reference.
            long long fanout_suppressed{ 0 };  ///< Non-reference DDs above the physical floor before the rebase and below it after: the rows the reference step would otherwise have placed on other satellites' arcs (a floor-crossing count, not what the pool would have flagged).
        };

        /// Reference-arm run telemetry per gradient chain (§22.6).
        const std::map<std::string, RefArmChainCounters>& refArmChainCounters() const {
            return ref_arm_chain_counters_;
        }

        /**
         * @brief Run the reference arm on the additional chains too (§22.6).
         *
         * Off by default.  With it off, the reference arm runs on the first
         * chain only (§14.10.4): an additional chain adopts the first chain's
         * reference where it can and elects its own otherwise, and in neither
         * case is a slip on that reference attributed.  The effect measured on
         * the field seed (§22.6): an L5 slip on the satellite that is the
         * reference of both L5 chains reads `abs_dev = 0` on its own rows by
         * construction and spreads onto the other satellites of those chains
         * as false refloats, plus a network-wide phase-bias re-realization.
         *
         * On, an additional chain with its own elected reference runs the arm
         * with that chain's own dispersion state and its own λ_IF, and one with
         * an adopted reference inherits the first chain's verdict for that
         * satellite this epoch.  Neither path may use the legacy candidate
         * condition as the decision: |median| > 0.5·λ_IF is met by noise on the
         * wide combinations ((L2W,L5Q): noise amplification 16.6, λ_IF 0.125 m;
         * 5 351 candidates and 64 fires on a quiet null, §14.10.3).  Both paths
         * are therefore refused on an additional chain unless the hardened
         * (empirical) gate is armed.  This is the second lock: the loader
         * already disarms `network.cycle_slip.if_phase_td_ref_arm_all` unless
         * `network.cycle_slip.if_phase_td_pairs = all` and
         * `network.cycle_slip.if_phase_td_ref_gate` are both set.
         *
         * Nothing changes on the first chain, with @ref all_pairs_ off, or with
         * this off.
         *
         * @param all True to run the arm on every chain.
         */
        void setRefArmAllChains(bool all) { ref_arm_all_ = all; }

        /**
         * @brief Defer per-satellite outlier flagging to an external (network) pool.
         *
         * When enabled, detect() still groups, single-differences and
         * double-differences, applies the reference-change and reference-slip
         * corrections, caches the jumps (see @ref getLastIfJumps) and carries
         * the gradients forward, but it does not run its own per-station MAD
         * outlier flag.  It therefore does not exclude per-satellite-flagged
         * arcs from the next epoch's gradients: the caller calls
         * @ref resetSatellite (or @ref resetSatelliteInState for one chain) on
         * the arcs it flags from the pool, or @ref reseedSatellite
         * (@ref reseedSatelliteInState) to keep the flag epoch's gradient as the
         * next epoch's origin.  The reference-satellite slip check
         * still runs, since it is a per-station common-mode test the network
         * pool cannot see.  Default false (self-contained per-station flagging,
         * the original behaviour).
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

        /** Evaluate every distinct band pair, not only the first two signals of
         *  each satellite (§14.10); see @ref setAllPairs. */
        bool all_pairs_{ false };

        /** Run the reference arm on the additional chains too (§22.6): through
         *  the hardened gate on an own-elected reference, by inheritance on an
         *  adopted one; see @ref setRefArmAllChains.  Inert unless
         *  @ref all_pairs_ is on and the hardened gate is enabled. */
        bool ref_arm_all_{ false };

        /** Minimum elevation (degrees) at which the current reference is kept.
         *  Below it, or when the reference is missing, the highest-elevation
         *  satellite is elected, with no minimum. */
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
         * Rationale: for two satellites 60° apart on the sky,
         * |e_sat - e_ref| = 2·sin(30°) = 1, so a position error Δpos produces a
         * differential OMC of up to |Δpos|.  For GPS L1/L2 the IF wavelength is
         * about 10.7 cm, so even a 0.5 m position uncertainty can produce up to
         * 0.5 m of differential error, about 4.7 IF cycles.  A threshold of
         * 0.5 m provides a conservative margin.
         */
        static constexpr double kPositionSigmaThresholdM = 0.5;

        /**
         * @brief Per-constellation persistent state across epochs.
         */
        struct ConstellationState {
            /** Previous epoch between-satellite IF OMC gradients keyed by satellite ID. */
            std::map<std::string, double> prev_grad;

            /** Previous epoch LOS unit vectors keyed by satellite ID.  Stored and
             *  cleared alongside prev_grad but not read: the geometry correction
             *  uses the current-epoch LOS vectors. */
            std::map<std::string, Eigen::Vector3d> prev_los;

            /** Satellites whose prev_grad entry a driver kept after a flag
             *  (@ref reseedSatellite).  Read by the next published sample
             *  (IfJumpInfo::after_reseed) and cleared with prev_grad when the chain
             *  stores that epoch's gradients, so a mark lives exactly as long as
             *  the gradient it describes.  Empty while no driver re-seeds. */
            std::set<std::string> reseeded;

            /** Receiver position at the previous epoch (ECEF meters). */
            Eigen::Vector3d prev_position{ Eigen::Vector3d::Zero() };

            /** True if prev_position has been set at least once. */
            bool has_prev_position{ false };

            /** Current reference satellite ID for this constellation. */
            std::string ref_satellite;

            /** Current reference satellite elevation (degrees). */
            double ref_elevation{ 0.0 };

            // Hardened reference-gate state (card R).  With the gate disabled these
            // fields keep their initial values: only the gate sets them, although
            // selectReference reads and clears ref_force_reselect and ref_exclude.
            /** EWMA of the non-reference DD dispersion, 1.4826·MAD (raw m; the
             *  zenith-equivalent disp·⟨sin el⟩ under @ref RefGateParams::elev_norm). */
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

        /**
         * @brief Keep @p satellite's stored gradient in one chain: the body of
         *        @ref reseedSatellite and @ref reseedSatelliteInState.
         *
         * Marks the entry in ConstellationState::reseeded when the chain holds one,
         * and clears the chain's reference when it is @p satellite, as a reset
         * does.
         *
         * @param state_key Chain key (for the debug log line only).
         * @param cs        The chain's state.
         * @param satellite Satellite identifier (e.g. "G01").
         * @return true when the chain held a gradient for @p satellite.
         */
        bool reseedInChain(const std::string& state_key, ConstellationState& cs,
                           const std::string& satellite);

        /** Per-gradient-chain state.  Keyed by system string ("GPS") for a
         *  satellite's first band pair (the key used with the knob off, so the
         *  default path is unchanged) and by "&lt;system&gt;|&lt;sig1&gt;_&lt;sig2&gt;" for
         *  every additional pair under @ref setAllPairs (§14.10).  Each entry
         *  holds its own between-epoch gradient chain and reference; an
         *  additional chain takes the first chain's reference where that
         *  satellite is present and elects its own otherwise (§14.10.4). */
        std::map<std::string, ConstellationState> constellation_state_;

        /** Cached per-satellite IF jump from the most recent detect() call, first
         *  pair only (the chains whose key is the bare system string).
         *  Populated at the end of detect() so that getLastIfJump() can
         *  return values without re-computing. */
        std::map<std::string, IfJumpInfo> last_if_jumps_;

        /** The same cache keyed by (satellite, pair label), holding every
         *  evaluated pair (§14.10): a superset of @ref last_if_jumps_ that holds
         *  the same samples while @ref all_pairs_ is off. */
        std::map<SatPairKey, IfJumpInfo> last_if_jumps_all_;

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

        /** @brief Median of a vector via RobustStats::median (@p values is not modified). */
        static double computeMedian(std::vector<double>& values);

        /**
         * @brief Group observations into gradient chains and extract IF phase OMC data.
         *
         * With @ref all_pairs_ off there is one chain per system, and each
         * satellite contributes its first band pair.  With it on, each satellite
         * also contributes every other pair of its phase bands with distinct
         * frequencies, each to a chain of its own keyed
         * "&lt;system&gt;|&lt;sig1&gt;_&lt;sig2&gt;", while the first such pair stays in the
         * bare-system chain so its population and decisions are unchanged
         * (§14.10).
         *
         * @param observations Epoch observations.
         * @return Map of chain key -> vector of SatData.
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
         * @brief Process double differences for one gradient chain and flag slips.
         *
         * Applies the reference-change correction and the geometry correction,
         * then, unless the epoch is gated, the reference-slip check and (unless
         * flagging is deferred) the non-reference outlier detection.  Appends
         * flagged keys to reset_keys.
         *
         * @param sat_vec         Satellite data for this constellation.
         * @param sys_str         Chain key ("GPS", or "GPS|L1C_L5Q" for an
         *                        additional chain): names the chain in log lines
         *                        and per-chain telemetry, and its '|' marks an
         *                        additional chain.
         * @param cs              Persistent constellation state.
         * @param grad_phi_if     Current-epoch between-satellite gradients.
         * @param lambda_if       Median IF wavelength for this constellation.
         * @param ref_changed     Whether the reference satellite changed.
         * @param position_sigma_3d 3D position sigma (meters), written to the
         *                        trace; the gate itself is read from
         *                        @ref epoch_gated_.
         * @param[out] reset_keys Ambiguity keys to reset are appended here.
         * @param run_ref_arm     Run the per-station reference-satellite slip test
         *                        (tier T1r) on this chain.  True on every chain
         *                        under the default pair width, so the default path
         *                        is unchanged.  False on the additional chains of
         *                        @ref setAllPairs, which take the first chain's
         *                        reference where present and do not re-run its
         *                        test (§14.10.4 gives the cost of re-running it);
         *                        @ref setRefArmAllChains (§22.6) turns it back on
         *                        through the hardened (empirical) gate.
         * @param[out] out_outcome Where to record this chain's reference-arm
         *                        verdict so that the additional chains that
         *                        adopted the reference can inherit it (§22.6).
         *                        nullptr: not recorded.
         * @param inherit         A first-chain verdict to apply on this chain (the
         *                        adopted-reference path, §22.6).  nullptr: no
         *                        inheritance.  Ignored unless the hardened gate is
         *                        enabled, the verdict fired, and it names this
         *                        chain's current reference.
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
            std::vector<std::string>& reset_keys,
            bool run_ref_arm = true,
            RefArmOutcome* out_outcome = nullptr,
            const RefArmOutcome* inherit = nullptr);

        /**
         * @brief Advance this chain's non-reference DD dispersion scale and
         *        return the effective scale from before the update (card R).
         *
         * Called on every epoch that has a usable family (≥ 2 finite non-ref
         * DDs) while the gate is armed, not only on the epochs where the legacy
         * condition fired.  A scale measured only on candidate epochs would be
         * conditioned on a large median, which lets a persistent drift raise
         * its own acceptance threshold until a genuine slip no longer clears it.
         *
         * @param cs            Constellation state (mutated: EWMA + sample count).
         * @param non_ref_dds   Finite non-reference double differences (raw m).
         * @param median_dd     Their median (raw m).
         * @param cold_prior    Scale used while no empirical samples exist: the
         *                      physical floor expressed as a scale
         *                      (floor_frac·λ_IF / k), so a cold gate reduces to
         *                      exactly the legacy physical threshold.
         * @param sin_bar       Mean sin(elevation) over the non-reference family,
         *                      elevations clamped at @ref RefGateParams::elev_clamp_deg.
         *                      Used only when @ref RefGateParams::elev_norm is set,
         *                      to store the zenith-equivalent dispersion in the
         *                      EWMA; ignored otherwise.
         * @param[out] disp_out This epoch's dispersion, 1.4826·MAD (raw m).
         * @return The empirical-Bayes effective scale s_eff before this fold (m).
         */
        double updateRefDispersion(ConstellationState& cs,
            const std::vector<double>& non_ref_dds,
            double median_dd, double cold_prior, double sin_bar, double& disp_out);

        /**
         * @brief Adjudicate a candidate reference-satellite slip (card R).
         *
         * Called only when the legacy condition (|median| > floor_frac·λ_IF,
         * n ≥ 2) already holds and @ref RefGateParams::enable is set.  Updates
         * the persistence and escalation state, increments
         * @ref ref_gate_counters_, and returns whether and how the reference
         * slip fires.
         *
         * @param cs            Constellation state (mutated: fire run, escalation flags).
         * @param non_ref_dds   Finite non-reference double differences (raw m).
         * @param disp          This epoch's dispersion, 1.4826·MAD (raw m).
         * @param s_eff         Pre-update effective empirical scale (raw m).
         * @param median_dd     Median of the non-reference DDs (raw m).
         * @param lambda_if     Constellation median IF wavelength (m).
         * @param sys_str       System string (logging).
         * @param sin_bar       Mean sin(el) over the family (clamped); the factor
         *                      the EWMA was normalized by.
         * @param sin_ref       sin(el) of the reference satellite (clamped); the
         *                      common term is divided by it.
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

        /// This epoch's combined gating verdict (position or model nuisance),
        /// computed once in @ref detect.  @ref processDoubleDifferences reads
        /// this flag instead of re-deriving the predicate from
        /// `position_sigma_3d`, so the reference gate and the flagging share
        /// one definition; a check on `position_sigma_3d` alone would miss the
        /// nuisance gate and let the reference gate fire through a gate the
        /// caller has closed.
        bool epoch_gated_{ false };

        /** Reference-gate parameters (card R); default-constructed means the legacy behaviour. */
        RefGateParams ref_gate_{};

        /** Reference-gate run telemetry (card R). */
        RefGateCounters ref_gate_counters_{};

        /** Reference-arm run telemetry per gradient chain (§22.6).  Populated on
         *  every chain the arm runs on (the first chain included) and on
         *  inherited fires, so a comparison with the knob on and off compares
         *  like with like. */
        std::map<std::string, RefArmChainCounters> ref_arm_chain_counters_;

        /** Monotonic detect() call index: the unit of the reference gate's
         *  persistence run length (card R), an epoch identity that needs no
         *  time model. */
        long long epoch_counter_{ 0 };
    };

} // namespace GnssProcessor
