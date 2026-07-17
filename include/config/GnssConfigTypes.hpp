#pragma once

/**
 * @file GnssConfigTypes.hpp
 * @brief Configuration vocabulary for GnssConfig: the enums and POD
 *        config sub-structs consumed by GnssConfig and its callers.
 *
 * Extracted from GnssConfig.hpp so the 2.8k-line config header no longer
 * forces every includer to re-parse the full type vocabulary, and so a
 * caller that only needs a config enum/struct can depend on this lighter
 * header. GnssConfig.hpp includes this file. All types remain in namespace
 * GnssProcessor, exactly as before — this is a pure relocation.
 */

#include "logging/LogManager.hpp"   // LogLevel (used by LoggingConfig)
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Enumeration for the top-level application execution mode.
     *
     * Selects between batch post-processing from files and real-time
     * streaming operation.
     */
    enum class AppMode {
        PostProcessing, ///< Batch processing from RINEX / product files.
        RealTime        ///< Streaming / live processing from RTCM data.
    };

    /**
     * @brief Selects how input files (RINEX observations, navigation,
     *        SP3/CLK/BIA/ERP, ATX, ocean loading) are located.
     *
     * - Internal: self-sufficient mode. PathResolver discovers files by
     *   globbing the configured @c internal_data_root (defaults to
     *   @c <project_root>/data). Suitable for unit tests, demos, and
     *   any run where the user wants a turnkey experience without a
     *   network connection. Forces @c DownloadConfig::enabled = false.
     * - Production: PathResolver delegates to @c ProductResolver, which
     *   computes IGS long-name filenames from year/doy/station and
     *   downloads any missing products from CDDIS into
     *   @c DownloadConfig::data_root.  Used for real PPP / PPP-RTK runs
     *   on arbitrary stations and dates.
     *
     * The two modes share the same on-disk layout (orbit/, clock/,
     * dcb/, erp/, brdc/, rinex/, atx/, nc/) so swapping data sources is
     * a one-line config change.
     */
    enum class DataSource {
        Internal,   ///< Use files under @c internal_data_root only.
        Production  ///< Auto-download from CDDIS into @c data_root.
    };

    /**
     * @brief Configuration for one station in a baseline / network processing run.
     *
     * Each station supplies its own RINEX observation file.  Reference
     * stations additionally require a known ECEF position (fixed during
     * the solve).  Rovers may optionally supply an a-priori position to
     * seed the filter; otherwise the position is initialized from the
     * RINEX header approximate position.
     */
    struct StationConfig {
        std::string name;                         ///< Short station identifier (e.g. "REF1", "ROV1").
        std::filesystem::path rinex_path;         ///< Path to the station's observation RINEX file.
        std::array<double, 3> position_ecef{ 0.0, 0.0, 0.0 }; ///< A-priori (or fixed) ECEF position (m).
        bool has_position{ false };               ///< True if @c position_ecef was supplied in the config.
        bool fixed{ false };                      ///< True when the station position is held fixed (reference station).
        /// RTA-12 provenance: true when @c position_ecef was resolved from a loaded
        /// SIF registry (velocity-propagated occupation) under
        /// @c network.use_sif_coordinates rather than typed in the JSON.  Explicit
        /// JSON coordinates always win, so this is false whenever the user supplied
        /// them.  Logged per station at roster build.
        bool position_from_sif{ false };
    };

    /**
     * @brief Configuration for double-differenced baseline / network processing.
     *
     * Used when @c ProcessingMode is @c BaselineSingle or @c BaselineMulti.
     * A single reference station is required in both cases.  For
     * @c BaselineSingle exactly one rover is expected; for @c BaselineMulti
     * one or more rovers may be listed — each is double-differenced
     * independently against the master reference (star topology, v1).
     */
    struct BaselineConfig {
        StationConfig reference;              ///< Master reference station (fixed coordinates).
        std::vector<StationConfig> rovers;    ///< Rover stations (one for Single, >=1 for Multi).
        double max_baseline_km{ 30.0 };       ///< Soft upper bound for the short-baseline atmosphere assumption.
        double epoch_sync_tolerance_s{ 0.01 };///< Max time-tag difference when matching epochs across receivers.
        bool code_only{ false };              ///< Debug toggle: skip phase DD updates (filter uses only code DDs, no ambiguity states).
        bool use_doppler{ false };            ///< Include Doppler ('D') observables when forming DDs.  Off by default because BaselineFilter has no velocity state yet — Doppler DDs are computed but not consumed.  Set true to surface Doppler residual statistics for diagnostics or when a velocity state is added in a future slice.
        bool signal_aliasing{ true };         ///< Pair signals across receivers at the band level when tracking attributes differ (e.g. rover C2W vs reference C2C, both GPS L2).  When false, only exact RINEX-3 string matches form DD rows (legacy strict behavior).  Default ON because mixed-receiver fleets are common and the strict path silently drops viable measurements.  Sat-DCB residual on cross-attribute code rows is small (typically <1 m) and is absorbed by the float ambiguity for phase rows.  Phase 2 plan adds DCB harmonization for ambiguity fixing.
        bool use_iono_free{ false };          ///< Use the dual-frequency ionosphere-free combination instead of per-band uncombined DDs.  Long-baseline (>10-20 km) DDs do NOT cancel ionosphere between receivers; the IF combination eliminates the first-order term at the cost of ~3x amplified code noise and a non-integer ambiguity (float-only — AR is automatically excluded for IF rows).  On short baselines IF is unnecessary and noisier than uncombined; the toggle exists as a testing / educational knob.  Per sat-pair and obs-type the row builder picks the two highest-frequency bands available at both rover and reference and collapses them into a single IF row.  Sat-pairs with fewer than two shared bands are dropped in IF mode.
        bool enable_pivot_handover{ true };   ///< When the per-constellation sticky pivot changes (incumbent left the shared-sat set), apply an S-transformation to re-reference the existing DD ambiguities to the new pivot instead of erasing and re-seeding them.  Preserves integer cycle information across pivot changes — the user-visible symptom of NOT doing this is a position discontinuity at every pivot handover, since the freshly-seeded float ambiguities have to absorb the rover's geometry residual all over again.  The transform is x_new = T·x_old, P_new = T·P_old·Tᵀ, where T is the standard DD S-transform that maps (DD relative to old pivot) to (DD relative to new pivot).  Default true; set false to revert to the legacy reset-on-handover behavior for testing or comparison.
        bool estimate_differential_tropo{ false };  ///< Estimate a single differential zenith wet delay (Δzwd ≡ ZWD_rov − ZWD_ref) as a state, mapped to slant via the wet mapping function.  PppModel applies only the Saastamoinen ZHD a-priori; the wet delay is otherwise unmodeled and on short baselines (< ~10 km) it cancels in BRSD because both stations see nearly identical wet path delays.  On longer baselines (especially with altitude differences) Δzwd can reach several centimeters and the residual smears into vertical position and float ambiguities.  Set true with use_iono_free for the typical long-baseline configuration.  Per phase DD row the H column entry is (mf_w_rov_i − mf_w_rov_p); short-baseline approximation assumes mapping functions match between rover and reference.  Single state (one Δzwd, one process-noise channel); not adequate for >100 km baselines where rover and reference mapping functions diverge.
        double differential_tropo_init_std_m{ 0.1 };  ///< Initial standard deviation of the differential ZWD state (m).  Default covers ~1 cm/10 km horizontal × 30 km plus modest altitude-difference contribution.  Used only when @c estimate_differential_tropo is true.
        double differential_tropo_process_noise_m_per_sqrt_s{ 1e-4 };  ///< Random-walk process noise spectral density (m/√s) for the differential ZWD state.  Default 1e-4 ≈ 6 mm/√h, a typical published value for short-to-medium-baseline differential troposphere drift.  Squared to a per-second variance rate inside @c getProcessNoiseMatrix.  Used only when @c estimate_differential_tropo is true.
    };

    /**
     * @brief Configuration for the PPP-RTK NETWORK producer (@c PPP_RTK_Network).
     *
     * A set of PEER stations (no master/rover asymmetry) whose single-receiver
     * normal equations are jointly reduced onto a shared satellite-parameter
     * border (clock + code/phase bias) under an explicit minimum-constraint
     * S-basis.  Each station holds its coordinates fixed/tightly-constrained.  The
     * datum fields name the network time reference (a pivot receiver clock) and
     * optionally a reference satellite per constellation for the clock gauge.
     */
    /**
     * @brief Closed-loop integer-recovery harness configuration (held-out consumer).
     *
     * The closed-loop acceptance test for the network producer: a HELD-OUT
     * station — deliberately NOT one of the @c NetworkConfig::stations in the
     * joint solve — consumes the network's freshly-emitted SSR products
     * (satellite clock + phase bias) epoch-by-epoch through the existing
     * user-side PPP-RTK + ambiguity-resolution pipeline, fed from an OWNED
     * @c SsrCorrectionStore (never the process-wide singleton).  Success =
     * "our own user engine recovers integers from our own products on an
     * independent station."  The consumer holds its coordinates as TRUTH for
     * scoring the recovered position; integer recovery (WL fix rate / count)
     * is the primary metric.
     */
    struct ClosedLoopConfig {
        bool          enable{ false };   ///< Run the held-out consumer after each network solve epoch.
        StationConfig station;           ///< Held-out consumer station (NOT in the network solve); needs rinex_path + known ECEF (truth).
        int           ar_mode{ 1 };      ///< Consumer AR mode (0 float, 1 continuous, 2 instantaneous, 3 fix-and-hold).  Producer is float; the consumer enables AR.
        bool          ar_wl_only{ true };///< WL-only AR — matches the float-only v1 producer (validates wide-lane recovery; full-NL is gated on the unscheduled NL-LAMBDA validation).
        std::vector<std::string> ar_active_systems;  ///< Constellations eligible for consumer AR (empty => inherit the base config's AR active systems).
        bool          consume_network_products{ true }; ///< Inject the OWNED network store into the consumer.  Set false for the A/B BASELINE: the consumer then runs on precise SP3/CLK + .BIA only (a normal PPP-RTK user), isolating the network product's contribution to integer recovery / position from the consumer's own modeling.
        bool          consume_network_clock{ true };    ///< When consuming products, also consume the network CLOCK.  Set false to keep precise SP3/CLK for the clock while consuming only the network PHASE BIAS — the cleanest integer-recovery test, isolating the (integer-essential) phase-bias product from a marginal thin-network clock product.
        bool          consume_network_iono{ false };     ///< Consume the network's co-estimated SH-VTEC ionosphere: the producer evaluates its VTEC field at the consumer's pierce points and supplies an UNDIFFERENCED per-satellite slant-iono value (metres on L1); the consumer applies it as a tight, quality-scaled BLUP / ionosphere-weighted prior on its slant-iono states instead of floating them at ionosphere_q.  This is the strong (ionosphere-corrected) user model Odijk-2012/TK-2015 identify as the fast-AR enabler.  Default OFF => byte-identical (producer pushes nothing, consumer applies nothing).
        bool          consume_combined_sigma{ false };   ///< M4 (contract v2): fold the network product's per-key COMBINED-correction σ (age-inflated) into a network-corrected observation's measurement variance INSTEAD OF the URA + phase-bias-σ RSS, so a fresh (reconverging) satellite whose individual clock/bias σ are cm-level but whose COMBINED correction is mm-level is NOT deweighted 10–100× off.  Per-key fallback to the v1 weighting when the sigc/sigcc tokens are absent (mixed-stream safe).  Default OFF => byte-identical.
        bool          consumer_undifferenced{ false };   ///< Consumer user model.  false = BSSD (legacy: between-satellite single-difference S-transform in the filter).  true = UNDIFFERENCED uncombined PPP-RTK USER (the end-state): no observation-space differencing — apply the network satellite phase biases directly to the raw float and fix per-band ambiguities INDIVIDUALLY via the resolver's internal-SD LAMBDA (no WL/NL cascade, no pivot-handover machinery).  Dissolves the network-bias↔.BIA-frame conflict by dropping the MW WL.
    };

    /**
     * @brief Producer-side network ambiguity-resolution gates.
     *
     * Controls integer fixing of the network's own between-station,
     * between-satellite double-difference ambiguities so the emitted satellite
     * phase biases are integer-leveled (their integer part referred to a stable
     * network datum, fractional part the satellite UPD).  Disabled by default —
     * the float product is bit-identical until @c enable is set.  Gates are
     * stricter than the single-station user path because a wrong network fix
     * poisons every downstream user (see
     * @c docs/network_ambiguity_resolution_design.md §5).
     */
    /**
     * @brief MW-seeded wide-lane pseudo-observation layer (M5 card WL-1;
     *        @c network.mw_wl.*).  See docs/SPEC_mw_wl_card.md.
     *
     * Feeds the per-arc Melbourne-Wubbena Welford means, double-differenced over the
     * Card-4 fundamental-cycle chord basis, into the producer solve as rank-K
     * pseudo-observation blocks on the cross-band ambiguity columns — the per-arc
     * code-coherence accumulator the per-epoch network structurally lacks.  Default
     * OFF ⇒ byte-identical (no detectors built for the card, no blocks folded).
     * Requires @c network.carry_invariant (the chord basis is the carry's; v1
     * mutex-checked at load).  GLONASS is excluded upstream.
     */
    struct MwWlConfig {
        bool   enable{ false };               ///< Phase-A master.  OFF => byte-identical.
        int    min_arc_epochs{ 10 };          ///< Welford n per arc before its cycles are eligible.
        double sigma_floor_cyc{ 0.05 };       ///< per-row R floor (cycles): R = max(Σ var/λ_WL², floor²).
        int    max_rows_per_epoch{ 0 };       ///< 0 = unlimited; else keep the lowest-R rows first (drops counted).
        // --- Phase B (WL integer fixing) — config contract only; NOT yet implemented. ---
        bool   fix_enable{ false };           ///< Phase-B master (requires @c enable).  Parsed for the contract; inert + warned (the un-fix wiring per standing invariant #1 is a follow-up increment).
        double fix_sigma_cyc{ 0.10 };         ///< Phase-B FFR gate: fix a cycle when its constrained WL posterior σ < this.
        double fix_frac_cyc{ 0.15 };          ///< Phase-B FFR gate: and |frac| < this.
        double fix_weight_sigma_cyc{ 0.01 };  ///< Phase-B fixed-row σ (soft-hard; rollback-able).  REUSED by WL-2 as the hold-row σ (SPEC_ewl_wl_cascade_card.md §3.3).
        // ============================================================================
        // M5 card WL-2: EWL-first tri-frequency cascade (docs/SPEC_ewl_wl_cascade_card.md).
        // Supersedes the WL-1 float layer (@c enable becomes diagnostic-only) with an
        // EWL statistic lane (uncarried third band ⇒ rows win with no fight) that fixes
        // by ep ~3, a phase bridge to a fast WL integer, and the NL-preserving inject
        // primitive that writes the WL truth into the Card-4 carry.  Default OFF ⇒
        // byte-identical (no third-band carry exclusion, no lanes, no inject).
        // ============================================================================
        bool   ewl_enable{ false };           ///< WL-2 EWL lane master (stats scope + rows + statistic-gated fix; NO inject — the lane is uncarried).  Requires @c network.carry_invariant.  OFF ⇒ byte-identical.
        int    ewl_min_arc_epochs{ 2 };       ///< Lagged Welford n before an EWL cycle is decision-eligible (first decision ep 3 under the F1 lag).
        double ewl_fix_sigma_cyc{ 0.10 };     ///< EWL FFR gate: fix when the statistic-row σ < this (cyc).
        double ewl_fix_frac_cyc{ 0.20 };      ///< EWL FFR gate: and |frac(z_EWL)| < this (cyc).
        bool   wl_inject_enable{ false };     ///< WL-2 WL lane master (bridge on EWL-fixed cycles + MW(1,2) fallback → injectLaneTarget).  Requires @c carry_invariant; the bridge path additionally requires @c ewl_enable.  OFF ⇒ byte-identical.
        double wl_fix_sigma_cyc{ 0.10 };      ///< WL FFR gate: fix when the WL statistic σ < this (cyc).
        double wl_fix_frac_cyc{ 0.15 };       ///< WL FFR gate: and |frac(z_WL)| < this (cyc).
        double bridge_max_sigma_cyc{ 0.15 };  ///< Iono-honesty guard: no WL fix from a bridge row wider than this (cyc).
        double inject_disagree_min_cyc{ 0.75 };///< Inject trigger threshold on |statistic − carried lane value| (cyc); post-inject ~0 ⇒ self-limiting.
        double lane_max_abs_xlin_m{ 1000.0 }; ///< Pre-flight quarantine: skip a lane cycle if ANY arc's ambiguity linearization |x_lin| exceeds this (m).  Guards the station-1 ~256 km receiver-clock-seed pathology (§7 pre-flight) — those cycles ill-condition the correction-frame rows and fail to converge even at authority.  0 = disabled.
        // --- Diagnostics (log-only; default OFF => byte-identical). ---
        bool   f6_dump{ false };              ///< One-shot F6 attribution dump: on the FIRST epoch that folds ≥1 WL row, write logs/network_mw_wl_f6.csv with per-arc (sat/sig/coeff/MW̄/x_lin) + per-row (z_MW/z_lin/z_corr/reconstructed_abs/innovation).  Pure diagnostic; does NOT change the solve.
        double debug_r_scale{ 1.0 };          ///< DIAGNOSTIC ONLY: multiply every WL row's Pz by this (1.0 = honest default).  <<1 lets the WL rows dominate the stiff carry — the F6 probe for "is separate-Woodbury composition fatal or is authority enough?".  Changes the solve; NOT a production knob.
        bool   ewl_fold_rows{ false };        ///< F-A RESOLUTION (measured, Jul-16 15-ep A/B): folding the EWL integer rows into the solve is PURE COST — the WL cascade reads the DETECTOR statistic (never the posterior), and 147 σ0.01 rows perturb the shared clock/PB border enough to elevate D5 network-wide ~2× (679 vs control 342; 24% of flags on secondary-channel groups the EWL never touches).  Default FALSE = decide/FFR-fix/expose the integers, fold nothing (D5 348-358 ≈ control).  true = the study shape that re-creates the elevation; NOT a production knob.
        // ============================================================================
        // M5 card UWL-1: per-constellation ultra-wide-lane top tier (docs/SPEC_uwl_card.md).
        // Generalizes the cascade's hardcoded GPS band digits to the per-constellation
        // lane-tier table (include/slip/MwLaneTiers.hpp) and adds the single-epoch cold
        // fix the UWL wavelength earns.  Default OFF ⇒ byte-identical, including under
        // multi-GNSS active_systems (arming a constellation without the card changes
        // nothing); GPS-only runs stay BIT-identical even when armed (the table
        // reproduces the legacy digits).
        // ============================================================================
        bool   uwl_enable{ false };           ///< UWL-1 master: arm the per-constellation top tier for constellations with a close-frequency pair (currently Galileo; BDS-3 blocked on its clock divergence).  Requires @c ewl_enable (shared machinery + telemetry).  OFF ⇒ the legacy GPS-only digit scan everywhere.
        char   uwl_gal_top_band{ '7' };       ///< Galileo top-tier band digit: '7' = (E5b,E5a) Δf 30.69 MHz λ 9.768 m (default); '8' = (E5 AltBOC,E5a) Δf 15.345 MHz λ 19.537 m — twice the wavelength AND cm-level AltBOC code at the cost of C8/L8 tracking sparsity (measure, don't assume).
        bool   uwl_mid_enable{ false };       ///< E6 mid-ladder bridge hop (census-motivated, Jul-16): chain the WL determination through the Galileo (E6,E5a) λ 2.931 m rung — hop-hi bridges the fixed top integer to a ROUNDED mid integer, hop-lo bridges that to the WL — cutting each hop's λ-scale phase/iono amplification ~3.3× vs the direct 9.768→0.751 hop (the measured ~70% Galileo WL FFR-unfixed rate is the target).  Decision-side only; the direct hop remains the per-cycle fallback and cross-check.  Requires @c uwl_enable.  OFF ⇒ byte-identical (single bridge triple per constellation, as before).
        bool   uwl_dual_top{ false };         ///< Increment 5 (§1b/§7.5): per-CYCLE preferred/fallback Galileo top rung — band '8' (E5 AltBOC, λ 19.537 m) decides where tracked across the whole cycle, the (5,7) λ 9.768 m rung decides what '8' could not, BOTH bands {7,8} uncarried, and the untaken sibling is a CLOSURE statistic (2·N(8,5) = N(7,5) — f_E5 = (f_E5a+f_E5b)/2 exactly; measured first, hard-gated only under @c uwl_sib_gate).  Supersedes @c uwl_gal_top_band (ignored, noted at load).  Requires @c uwl_enable; forces @c ewl_fold_rows=false (a doubly-decided row must never fold twice — the F-A refold class).  OFF ⇒ the single-top tier verbatim (byte-identical).
        bool   uwl_sib_gate{ false };         ///< Increment 5 sibling closure HARD gate: block a top-tier decision whose untaken-sibling MW statistic disagrees with fx_integer·λ_top/λ_sib by > 0.5+3σ_sib (the blocked cycle falls to the fallback rung the same epoch).  Default OFF = measure-only (receiver half-cycle conventions on AltBOC may offset the identity — a half-integer residual population means the convention differs and the gate must first learn a constant; SPEC reconciliation note 2).  Requires @c uwl_dual_top.
        bool   uwl_family_pairs{ false };     ///< v2 census recovery: admit MINORITY receiver-family primary pairs (e.g. Galileo (L1C,L5Q) beside the majority (L1X,L5X)) into their OWN lane pair groups instead of leaving them per-band.  A pair is admitted when its joint-census count ≥ @c uwl_family_min_arcs and stays admitted for the run (latched — membership flicker refloats carried chords, the WL-3 "micro pair-group" hazard).  The measured stake: ~40/104 (101/154 with the mid hop) decided Galileo integers die at skip_basis on minority-family cycles.  Requires @c uwl_enable + @c lane_carry.enable (the pair groups are lane machinery); inert-warned otherwise.
        int    uwl_family_min_arcs{ 8 };      ///< First-admission threshold for @c uwl_family_pairs: a census pair joins the admissible set when ≥ this many (rx,sat) arcs carry it in one epoch's census (clamped ≥ 1).  The fixture families measure ~115–132 arcs, so 8 is far from the decision boundary — it exists to keep one-off channel oddities from latching a permanent group.
        bool   uwl_cold_fix{ false };         ///< §3.3 single-epoch cold fix: a top-tier arc with n=1 UNLAGGED sample is FFR-decision-eligible using the a-priori σ below, on tiers with λ ≥ @c uwl_cold_min_lambda_m only.  DECISION-ONLY (which integers the bridge may use); folds nothing; kills the measured ~3-epoch slip re-warm latency.
        double uwl_cold_sigma_m{ 0.7 };       ///< A-priori per-sample MW statistic σ (m) substituted while the Welford variance is immature (n < 2).  Per ARC: a re-warming arc inside an otherwise-mature cycle contributes ≈ this/λ_top cycles to the row σ (0.7 m ⇒ 0.072 cyc at λ 9.768) — an ALL-cold cycle aggregates √Σc² larger and correctly gates itself out at the default FFR σ gate.
        double uwl_cold_min_lambda_m{ 8.0 };  ///< Wavelength floor for cold-fix eligibility (m).  Excludes GPS's 5.861 m EWL by design — the cold fix is a UWL privilege earned by wavelength, deliberately NOT extended to GPS in v1.
        bool   bridge_sample_debias{ false }; ///< 60-epoch age-bias fix (measured Jul-16: constrained-lane circR 0.89→0.24 over 60 ep as the whole-arc-lifetime Welford aged): fold each bridge sample as GF_k − C_iono·ı̃(t_k) (per-sample estimable-iono de-bias; one-epoch-lagged push) instead of subtracting C_iono·ı̃(now) from the lifetime mean at READ time — the statistic becomes stationary, fixing BOTH the mean bias and the drift-inflated Welford σ that starved re-decisions.  S-conformant: Σc·ı̃ over a closed cycle = ∇ΔI exactly (Odijk 2016 Table 8 — the S-basis lumps cancel by sign balance).  Requires @c ewl_enable (the bridge).  OFF ⇒ raw accumulation + read-time de-bias, byte-identical.
        bool   d5_dump{ false };              ///< F-A DIAGNOSTIC (log-only): per-epoch CSV of every Card-4 D5 innovation flag — group id, chord (u,v), station, ambiguity key, formal-standardized innovation, and adjacency tags {EWL-fixed (station,sat) member, hold-cycle chord, injected-last-epoch chord} — to logs/network_d5_flags.csv.  Does NOT change the solve.
        bool   lane_trace{ false };           ///< F-B INSTRUMENT (log-only): per-epoch CSV of EVERY primary cycle's POST-install carried WL lane value + fractional (both the held/injected population and the untouched control population in one run) — to logs/network_wl_lane_trace.csv; graded by scripts/grade_lane_trace.py (the injected-cycle circR the star dd-frac trace cannot resolve).  Requires @c wl_inject_enable OR @c lane_carry.enable (WL-3 acceptance instrument).  Does NOT change the solve.
    };

    /// M5 card WL-3 — lane-coordinate carry (docs/SPEC_lane_carry_card.md).  Carry each
    /// primary-pair chord's two band DDs as ONE jointly-covarianced block: stiff along the
    /// phase-determined NL direction, honestly loose along the WL lane, with per-epoch MW /
    /// bridge lane INCREMENTS as measurements (the estimator owns the code-coherence
    /// accumulator — the measured F-A/F6 refold class dissolves structurally).
    /// Mechanism landed Jul-16 2026 (SPEC §7 increments 2-4): pair-group regroup +
    /// equivalence shape, lane prior/q + per-epoch MW lane rows, bridge rows +
    /// fold-on-change FFR fixing.  Supersedes mw_wl.wl_inject_enable at runtime (warned);
    /// the inject primitive remains as the pair-block fallback.
    struct LaneCarryConfig {
        bool   enable{ false };               ///< Master.  Requires carry_invariant + mw_wl.ewl_enable (statistics source).  OFF ⇒ byte-identical.
        double q_wl_scale{ 0.01 };            ///< q along the WL lane = this × carry_invariant_qz.  DEFAULT 0.01 ≈ (0.04 cyc)²/epoch at the lineage qz=1e-4/30 s — keeps the √N lane accumulation + FFR fixes alive (measured circR 0.471-vs-0.251, Jul-16).  The SPEC §3.1 10³× numerology assumed qz≈0 and would swamp every lane measurement (~12 WL cyc of lane q per epoch).  1.0 = isotropic (part of the §6.1 equivalence shape).
        double sigma_wl_fresh_cyc{ 0.0 };     ///< Fresh-chord lane prior σ (cycles).  DEFAULT 0 = diffuse birth: the SPEC §3.1 mean-zero code-seed prior is FALSIFIED (seed-time clock diversity mis-centers the correction-frame lane origin by O(10²) cycles — D5 691-vs-125 measured Jul-16); the honest code-seed info enters via the FIRST MW lane increment instead.  >0 = study shape (loud warn).
        bool   lane_meas{ true };             ///< Per-epoch MW(1,2) lane increments folded into the carried block (SPEC §3.2).
        bool   bridge_meas{ true };           ///< Per-epoch iono-de-biased bridge increments on EWL-fixed cycles (phase-precision lane source).
    };

    struct NetworkArConfig {
        bool   enable{ false };               ///< Master switch; OFF => float product unchanged.
        double min_success_rate{ 0.9999 };    ///< FFR bootstrap acceptance gate (stricter than the user's 0.999).
        double ratio_threshold{ 3.0 };        ///< LAMBDA ratio-test backstop.
        double ratio_upper_bound{ 1e4 };      ///< Reject ratios above this (degenerate covariance).
        double nl_max_per_pair_frac{ 0.10 };  ///< Max per-DD |float − fixed| (cycles).
        bool   enable_partial_ar{ true };     ///< Fix the decisively-fixable subset; leave the rest float.
        bool   wl_cascade{ false };           ///< WL->NL cascade (needs per-station MW aggregation; off in V1).
        int    min_epochs{ 10 };              ///< Let the network float converge before fixing.
        int    ils_max_dim{ 0 };              ///< Cap on the ILS (MLAMBDA) search dimension: when > 0, only the
                                              ///< @c ils_max_dim lowest-formal-σ DDs enter the integer search
                                              ///< (ascending @f$Q_{aa}@f$ — the standard partial-AR subset), the
                                              ///< rest stay float.  0 (default) = uncapped legacy behaviour.
                                              ///< REQUIRED at scale: the uncapped ~2500-dim basis at 305 stations
                                              ///< does not return (Jul-15); ~100 is a sane armed value.
    };

    /**
     * @brief Producer a-priori satellite orbit/clock source for the PPP-RTK
     *        network producer (@c network.apriori_source).
     *
     * Selects what the network producer's per-satellite a-priori clock (and the
     * orbit its geometry uses) is referenced to; the estimable satellite-clock
     * state then absorbs the error of that a-priori.  Frozen conventions:
     * @c docs/rt_service_conventions.md §1 (RTF-1) and the card decomposition in
     * @c docs/rtf12_brdc_rts_plan.md.
     */
    enum class NetworkAprioriSource {
        Sp3,      ///< (default) Resolved precise SP3(+CLK) files; the emitted clock C0 is a delta against the SP3/CLK product identity (today's behaviour — byte-identical).
        BrdcRts   ///< Broadcast ephemeris (@c LiveEphemerisStore) + external IGS-SSR (4076) orbit corrections; the emitted C0 is a delta against broadcast(IODE) + the applied RTS orbit (the self-sufficient real-time service mode).
    };

    struct NetworkConfig {
        std::vector<StationConfig> stations;  ///< Peer stations (>=2); each needs a known ECEF (held fixed).
        std::string clock_datum_station;      ///< Pivot receiver whose clock defines network time ("" => first station).
        std::string clock_datum_satellite;    ///< Optional reference satellite pinned for the clock gauge ("" => auto: highest-elevation tracked, per constellation).
        double epoch_sync_tolerance_s{ 0.01 };///< Max time-tag difference when matching epochs across stations.
        ClosedLoopConfig closed_loop;         ///< Optional closed-loop integer-recovery harness (held-out consumer).
        NetworkArConfig  ambiguity_resolution;///< Producer-side integer fixing (default OFF => float product).
        MwWlConfig       mw_wl;               ///< M5 card WL-1: MW-seeded wide-lane pseudo-obs layer (default OFF => byte-identical).
        LaneCarryConfig  lane_carry;          ///< M5 card WL-3: lane-coordinate carry (default OFF => byte-identical; config CONTRACT only — mechanism is a follow-up increment, docs/SPEC_lane_carry_card.md).
        std::string product_output_path;      ///< Optional path: serialize each emitted NetworkSolutionEpoch (+ datum-change flag) here for replay (empty => disabled).  Used verbatim (absolute, or relative to the working directory).
        /// RTA-6: flush the product file once per emitted epoch (after the epoch
        /// block, incl. any RTA-5 S re-broadcast preceding it) so a LIVE tail
        /// consumer (@c NetworkProductTail via realtime.network_products_path)
        /// sees each epoch as soon as it is written instead of at
        /// ofstream-buffer boundaries.  Flushing changes NO bytes — an OFF run's
        /// file is byte-identical — but a live tail REQUIRES it for bounded
        /// product latency.  Default false per the default-off toggle discipline.
        bool product_flush_per_epoch{false};

        // --- Per-station compute parallelism (PASS A geometry + PASS B assembly) ---
        // The per-station geometry/measurement-model work is embarrassingly
        // parallel (each station owns an independent StateManager/PppModel/
        // MeasurementModel); only the shared-estimator scatter (est.addStation)
        // serializes.  When activated, PASS A/B compute runs one thread per
        // station and the scatter stays serial in deterministic station order, so
        // the float solution is BIT-IDENTICAL to a serial run regardless of thread
        // count.  Below the threshold the loop runs serially (no fork-join cost).
        bool parallel_stations{ true };   ///< Master enable for per-station threading (only takes effect above @ref parallel_min_stations).
        int  parallel_min_stations{ 10 }; ///< Parallelize only when the network has STRICTLY MORE than this many stations.
        int  parallel_max_threads{ 0 };   ///< Optional worker-thread cap (0 => omp_get_max_threads()).
        // Fan the per-epoch Schur reduction across threads too (the @ref
        // NetworkNormalEquations::reduce station loop).  OFF by default and kept
        // SEPARATE from @ref parallel_stations because, unlike the build phase (whose
        // serial-ordered scatter keeps it bit-identical to a serial run), the reduce
        // fan-out combines per-thread partials in a different summation grouping — so
        // its result is reproducible for a fixed thread count but ULP-different from
        // the serial reduce (the network's ill-conditioning amplifies that to ~1e-5
        // in the products; physically null, but it breaks serial≡parallel
        // byte-identity).  Enable only for max-speed runs that don't rely on a
        // serial-vs-parallel byte-diff for regression.  Uses the same thread budget
        // (@ref parallel_max_threads) and only when @ref parallel_stations is active.
        bool parallel_reduce{ false };    ///< Parallelize the Schur reduce loop (opt-in; trades serial≡parallel byte-identity for speed).
        // Deterministic-parallel fan-out of the post-solve SERIAL SEAM (the ~72% of a
        // 305-station epoch that reduce/assembly parallelism does NOT cover: the Card-4
        // DD carry-fold @ref NetworkNormalEquations::foldReducedDdPriors and the D5
        // innovation-gate readouts in @ref NetworkInvariantCarryDriver::installPosterior).
        // Unlike @ref parallel_reduce this preserves serial≡parallel BYTE-IDENTITY: the
        // independent per-(const,signal)-block algebra (G, M=(Pᶻ+B)⁻¹, GMGᵀ, the posterior
        // mean/cov + innovation GᵀΣG products) is computed into pre-sized per-block slots,
        // then ACCUMULATED / installed SERIALLY in a fixed block-index order — so the
        // floating-point summation order never changes and the output is bit-identical to
        // the serial fold regardless of thread count.  Only takes effect when
        // @ref parallel_stations is active (the win exists only at scale, where Eigen is
        // already pinned to one thread for the epoch — the deterministic-parallel region
        // reuses that pin).  Default OFF ⇒ the seam runs its original serial path, so an
        // OFF run is byte-for-byte the pre-seam producer (no new code on the OFF path).
        bool parallel_seam{ false };      ///< Deterministic-parallel the post-solve carry-fold / D5-readout seam (byte-identical, opt-in; needs @ref parallel_stations).
        // Thread the DENSE PER-BLOCK linear algebra of the Card-4 invariant carry across all
        // cores.  @ref parallel_seam parallelizes the carry only over the ~few
        // (const,signal) blocks (byte-identical), leaving each block's O(K²–K³) dense work
        // — @ref NetworkInvariantCarryDriver::buildDdPriors (W·Pᶻ·Wᵀ), the per-station
        // reduction inside @ref NetworkNormalEquations::foldReducedDdPriors, and
        // @ref NetworkInvariantCarryDriver::installPosterior's posterior-covariance readback
        // — SINGLE-THREADED (Eigen is pinned to one thread for the epoch).  With K≈8k carried
        // DD chords on a 305-station network that dense algebra is ~80% of the epoch
        // (measured; see docs/HANDOFF_perlos_parallel_sweeps.md).  When armed:
        // buildDdPriors + installPosterior thread their big dense GEMMs via an Eigen un-pin,
        // while foldReducedDdPriors (whose cost is the per-station reduction with small
        // inner-dim matmuls + a serial LDLT — which Eigen cannot thread) OpenMP-fans that
        // reduction across stations (per-thread partials + ascending combine) with Eigen
        // PINNED, and column-parallelizes the K×K middle inverse (byte-id given Pᶻ+B).  Like
        // @ref parallel_reduce this REASSOCIATES the dense reduction, so ON is reproducible
        // for a FIXED thread count but NOT byte-identical to serial — and NOT merely at ULP:
        // through this ill-conditioned, stiff (near-zero-q_z) carry the seam card measured
        // reassociation reaching ~1e-4 AT THE SOLUTION LEVEL over a run (physically null —
        // sub-mm — but a real, GRADED difference, not round-off).
        //
        // USAGE RULE: a parallel_carry-ON run is for WALL-CLOCK A/Bs ONLY.  Take NO graded /
        // physics verdict from an ON run until an ON-vs-OFF dd-frac equivalence grade has
        // passed for that config; every graded conclusion comes from an OFF (byte-identical)
        // run.  And this is a WALL-CLOCK MITIGATION, not the structural fix — the fold is
        // still O(K³) in the DD-chord count; the factored-Pᶻ restructure
        // (docs/HANDOFF_carry_driver_perf.md, INC-C2) is the O(K·n_s²+n_s³) cut, and its C0
        // census (K growth / killChord reconciliation) is still open.
        //
        // Not combinable with @ref parallel_reduce on the same solve — the config loader forces
        // parallel_carry OFF (with a warning) when both are armed, since est.solve() is itself
        // Eigen-threaded and would nest OpenMP × Eigen inside reduce().  Only takes effect when
        // @ref parallel_stations is active.  Default OFF ⇒ Eigen stays pinned and the carry
        // runs its @ref parallel_seam / serial path, byte-for-byte the pre-card producer.
        bool parallel_carry{ false };     ///< Thread the Card-4 carry's dense per-block algebra (opt-in; trades serial≡parallel byte-identity — ~1e-4 solution-level, so graded verdicts OFF-only — for ~2.4× wall-clock; needs @ref parallel_stations).

        // --- Visualization traces (consumed by the pyviz network viewers) ---
        bool trace_stations{ true };      ///< Emit a static @c network_stations.csv roster (station id, role, ECEF) once at startup.  Cheap (one tiny file); drives the worldmap viewer so it reflects the stations that actually loaded.
        bool trace_ionosphere{ false };   ///< Emit @c network_ionosphere.csv: per-station, per-satellite back-substituted slant ionosphere (m) + elevation/azimuth each emitted epoch, for the ionospheric pierce-point map.  O(stations x sats x epochs) — off by default; enable for the iono viz.
        bool trace_estimability{ false }; ///< Emit the one-shot @c Network[estimability] warn dump on the first epoch that reaches the datum gate: the tail of the reduced shared-normal eigen-spectrum plus the shared parameters spanning each near-zero (unpinned-gauge) direction.  A full eigenVECTOR decomposition of the reduced normals (O(n_shared³) — ~1156³ at 305 stations), so it is OFF by default; enable only to debug an over/under-specified datum.  The @ref NetworkSbasis rank gate itself is unaffected and always runs.
        bool trace_clock_full{ false };   ///< L4: append @c nis,mode columns to @c network_clock_monitor.csv (the exception to the append-only column rule — a trailing-column format break) and, when true, write EVERY sat-epoch row (both mode filters replaced with @c true) instead of only near/over-threshold rows.  The file itself still only opens under @ref trace_slip_events && (@ref clock_monitor || @ref clock_model); this only widens/annotates what it records.  Off by default.
        bool trace_slip_events{ false };  ///< Emit the deep-investigation slip log: @c network_if_slip.csv (per-(station,sat,pair) between-epoch IF ΔΔφ + the elevation-normalized / per-station-EWMA threshold decision, every epoch) and @c network_raim.csv (per-(sat,station) post-fit standardized residual + cross-station consensus at any spike).  O(stations x sats x epochs) for the IF file — off by default; enable to debug IF-phase pre-detector and RAIM events.
        bool trace_slip_raw{ false };     ///< Emit @c network_slip_raw.csv — per-(station,sat) RAW cycle-slip observables every epoch: Melbourne-Wübbena (m), geometry-free L1−L2 (m), first-signal phase-minus-code (m), and the per-band RINEX LLI flag, for the primary dual-frequency pair.  Computed directly from raw_observed and INDEPENDENT of whether any slip DETECTOR is enabled — so "was there a slip on ANY sat at this station/epoch?" is always answerable (in particular the receiver LLI, the ground-truth loss-of-lock flag that no other network CSV surfaces).  Epoch-to-epoch differences are trivially formed offline.  O(stations x sats x epochs); off by default.  Needs a wavelength provider (built lazily like the DD trace).
        bool trace_anchor_info{ false };  ///< B7: emit @c network_anchor_info.csv — per-satellite a-priori-clock-anchor information fraction (anchor_info / reduced-normals total_info) each emitted epoch, so validation runs can CERTIFY the estimable clock is data-dominated over the a-priori CODE-product anchor.  Pure diagnostic (anchor fold unchanged); off by default.  O(clock_sats x epochs).
        bool trace_dd_fractional{ false };///< B5: emit @c network_dd_fractional.csv — per-epoch cross-station between-satellite double-differenced float ambiguity (from reconstructStation), keyed (constellation,signal), with its fractional-cycle residual: a sign-convention self-check + empirical <0.1-cycle-support evidence for user AR.  O(stations x sats x epochs); off by default.  Pure additional CSV, no state mutation.
        int    trace_dd_fractional_max_pairs{ 0 };  ///< T2b: cap on the DD pairs emitted (and covariance-propagated) per epoch across the whole @c network_dd_fractional.csv write — a deterministic seeded subsample keyed on the epoch (week,tow).  0 = unlimited (current full-pair behaviour, reproduces the benchmark).  Bounds the O(pairs) emit AND the T2a @c jointConditionalCovariance request count (the dominant 305-station tracer cost).  Inert unless @ref trace_dd_fractional; diagnostic file only, no state change.
        int    trace_dd_fractional_epoch_stride{ 1 };  ///< T2e: write @c network_dd_fractional.csv only every Nth PROCESSED epoch (1 = every epoch = current).  Fraction statistics don't need every epoch; combines with @ref trace_dd_fractional_max_pairs to make a traced convergence run affordable.  TRACE-ONLY (never folded into the warm-resume fingerprint).  Inert unless @ref trace_dd_fractional.
        int    epoch_decimation_n{ 1 };  ///< T2e: process only every Nth SYNCED epoch in the network loop (1 = off = every epoch).  N=2 → 60 s effective rate, ~halving convergence wall time; the convergence horizon is physical-time-driven so a coarser rate loses little.  ESTIMATION-RELEVANT (changes the carried-state cadence / process-noise dt) — folded into the warm-resume fingerprint when != 1 so a decimation mismatch invalidates a resume.  The physical-time start-offset and max-duration guards still evaluate on every raw epoch (overshoot-safe).
        bool   iono_supply_formal_sigma{ false };  ///< T3f: when supplying the co-estimated SH-VTEC slant iono to the closed-loop consumer (@c closed_loop.consume_network_iono), RSS the SH-coefficient FORMAL σ (√(aᵀ Q_sh a), mapped to slant metres) onto the per-LOS representativeness floor, enforce a floor after the scale, AND skip the supply for satellites without FRESH OSB (satellite-code-bias) coverage this epoch (an under-observed field value is not trustworthy).  In-process supply only (no product bytes); OFF ⇒ the representativeness-floor-only σ (byte-identical consumer solution).
        bool trace_flag_rates{ false };   ///< B8: log a per-tier flag-rate summary at end of run — for each RAIM/QC tier (IF-phase slip, within-epoch trigger, post-fit RAIM cell/consensus, clock-model NIS, clock-monitor) accumulate {opportunities, flags} over the run and report realized rate vs the implied one-sided Gaussian α for the configured k.  Also writes @c network_flag_rates.csv + @c network_run_summary.csv (L3).  Measurement only — no threshold or decision change; OFF (default) = byte-identical.
        int  neq_export_epoch{ -1 };      ///< One-shot full-arrowhead NEQ export: at the Nth PROCESSED epoch (1-based; <=0 = off, the default) dump the assembled normal-equations system to @c logs/neq_export/ — border @f$N_{ss}@f$ (raw + reduced) and RHS, every station's @f$N_{sr}@f$ / @f$N_{rr}@f$ (rebuilt from its LDLᵀ) / @f$u_r@f$ as row-major little-endian double .bin files with a CSV station index, plus shared-column labels (@ref NetworkEstimator::sharedColumnLabel) and per-station local-column labels (StateManager::getStateLabels via the assembly h2local map).  Enables exact offline spectral / weak-direction attribution of the FULL system (the @ref trace_estimability dump covers only the reduced border's near-zero tail).  Post-solve read-only dump — the solution is byte-identical; ~100-200 MB once at 305 stations.
        bool trace_epoch_status{ false }; ///< L1: emit @c network_epoch_status.csv — ONE row per processed epoch (emitted OR skipped) recording the disposition (emitted / gate_withheld / sigma0_withheld / solve_failed / datum_failed / sync_skip), σ̂0, within-epoch adapt count, refloat/retro counts, datum-change flag, and the clock-Δ median+std+worst-sat (the median+std+worst convention, unlike @c network_trace.csv's rms/max).  Makes per-epoch disposition + σ̂0 level-shifts mechanically judgeable (V1/V3/V6).  O(epochs); off by default — byte-identical when OFF (no new file).
        bool trace_section_timers{ false };  ///< D0: per-epoch section-timer breakdown of the network solve (@c NetworkEstimator wall-clock accumulators around assembly / Schur reduce / constrained point-solve / the explicit shared-covariance full inverse / the per-station covariance consumers).  Arms @c NetworkNormalEquations::setPerfTiming so those primitives record their milliseconds; the driver logs a per-emitted-epoch @c "Network (D0)" line plus an end-of-run RUN-TOTALS line, so a profiling run can see which term dominates and thus which Tier-D scaling lever (D1 selected inverse / D2 rate-split / D3-D4 channel-and-ordering) is worth pulling.  Diagnostic only — no product bytes, no decision change; when OFF every instrumented primitive skips the clock read entirely (no measurable overhead, numerically byte-identical).
        bool write_run_manifest{ false }; ///< L15: write @c network_run_manifest.json (JSONL, two records) — a run-provenance snapshot (config path + FNV-1a-64 hash of the config bytes, build stamp, and the full network.* toggle/knob dump) at startup, plus an end-of-run record with the epoch counts + loop wall.  Makes an archived @c logs/ folder self-describing weeks later (which toggle set produced it) when everything is uncommitted.  Does NOT read git state.  Off by default.
        double health_interval_s{ 0.0 };  ///< RTB-6: operator health-record cadence in seconds of PROCESSED (content) time.  When > 0 the producer appends a machine-readable @c network_service_health.csv row every @c health_interval_s of data — cumulative epochs emitted/skipped/gate-withheld/σ̂0-withheld/runaway, the σ̂0 EWMA (−1 sentinel = "unavailable" when its feeding tiers @ref emit_ura + @ref raim + @ref phase_leveling are not ALL armed; the value would otherwise sit frozen at its 1.0 init), stations present, clock sats, mean ms/epoch over the just-ended health window, last-emit age, and the realized per-constellation clock pivots ("G:G05;E:E11"; empty before the first emit) — so a SERVICE run is observable WITHOUT waiting for the exit-time summary (all health surfaces else assume a finite run).  Distinct from the RTA-10 per-epoch product @c H status record (this is an aggregate OPERATOR heartbeat, not product content) and from the pyviz @c network_health VIEWER (which reads @c network_trace.csv, a different surface — hence this file's @c service name).  Column semantics: @c docs/rt_service_operations.md.  Negative values clamp to 0 with a warning.  Default 0 = off ⇒ no file, byte-identical.
        double trace_rotate_mb{ 0.0 };    ///< P1: size-based rotation threshold (MiB) for the unbounded service CSVs — the always-on @c network_trace.csv / @c network_solution.csv and the RTB-6 @c network_service_health.csv.  When > 0, a file crossing the threshold is closed, the suffix chain shifts (<tt>file.csv.k → file.csv.(k+1)</tt> up to @ref trace_rotate_keep, oldest deleted), the live file becomes <tt>file.csv.1</tt>, and a fresh live file reopens WITH the header re-written, so every rolled generation parses standalone.  NOTE: the pyviz viewers auto-discover only the LIVE file name — rotated @c .1…@c .keep suffixes are NOT read; the operator concatenates the chain (headers of non-first generations stripped) or points a viewer at one generation (see @c docs/rt_service_operations.md).  Checked once per processed epoch, so a file may overshoot by up to one epoch's rows.  Negative values clamp to 0 with a warning.  Default 0 = off ⇒ no rename/close ever happens, byte-identical.
        int    trace_rotate_keep{ 3 };    ///< P1: rolled generations retained per rotating CSV (<tt>.1 … .keep</tt>; the shift deletes <tt>.keep</tt> first).  Inert unless @ref trace_rotate_mb > 0.  Values < 1 clamp to 1 with a warning.  Default 3.
        double warmup_ura_floor_m{ 0.0 }; ///< RTB-7(b): cold-start quality-ramp URA floor (m).  While the startup ramp is ACTIVE the emitted per-sat URA is floored at this value, so a freshly (re)started producer advertises DEGRADED quality and a consumer down-weights/withholds instead of trusting a not-yet-converged network solution.  The ramp LIFTS (see @c networkWarmupRampLifted) when BOTH: (1) emitted epochs ≥ max(@ref warmup_epochs, @c phase_leveling_warmup_epochs) — the carry population is warm, not merely counted; AND (2) the σ̂0 EWMA has received ≥ @ref warmup_epochs updates, OR the EWMA is structurally unavailable this run (the RTB-6 sentinel condition: @c emit_ura + @c raim + @c phase_leveling not all armed — then the epoch count alone decides).  The lift is logged with the RTB-7(c) cold-start-to-product-grade metric (also a @c network_service_health.csv column).  On an RTB-7(a) WARM resume the ramp state restores from the snapshot — a warm-resumed producer is NOT degraded.  Requires @c network.emit_ura (a floor without it is warned as a silent no-op at load).  Default 0 = off ⇒ emitted URA unchanged (byte-identical).
        int    warmup_epochs{ 0 };        ///< RTB-7(b): the quality-ramp horizon — the minimum EMITTED-epoch count AND the minimum σ̂0-EWMA update count required before the @c warmup_ura_floor_m lifts (combined with @c phase_leveling_warmup_epochs; see @ref warmup_ura_floor_m for the exact rule).  Inert unless @c warmup_ura_floor_m > 0.  Default 0 = off (the ramp then waits only for the phase-leveling warmup).
        // --- RTB-7(a): producer state snapshot + warm resume -----------------
        /// Path of the producer state-snapshot file (empty = OFF, byte-identical).
        /// A relative path lands under the network log directory.  When set, the
        /// producer atomically rewrites this file every @ref state_snapshot_interval_s
        /// of product time (temp-file + rename replace — a crash mid-write never
        /// corrupts the previous good snapshot) with BOTH cross-epoch state layers
        /// (driver carries/oscillators/pivots/pending-bump sets/generation id +
        /// the emitter's discontinuity stream state).  At startup, a snapshot that
        /// parses, matches the run's datum-relevant config fingerprint, and is
        /// younger than the carry-validity horizon (max of
        /// @c phase_leveling_max_gap_s / @c clock_model_max_gap_s vs the first
        /// live epoch) triggers a WARM resume: state restores, the RTA-3
        /// generation id is PRESERVED (no consumer reset), and per-key staleness
        /// is judged by the existing downstream gap machinery.  Anything else
        /// cold-starts safely with a warning and a fresh generation id.  See
        /// @c products/NetworkStateSnapshot.hpp.
        std::string state_snapshot_path;
        /// RTB-7(a): snapshot cadence in seconds of PRODUCT (content) time.
        /// Checked at the end of each EMITTED epoch.  Non-positive values clamp
        /// back to the default with a warning.  Inert while
        /// @ref state_snapshot_path is empty.  Default 10.
        double state_snapshot_interval_s{ 10.0 };
        bool trace_station_residuals{ false }; ///< L13: emit @c network_station_residuals.csv — EVERY post-fit RAIM cell (per non-pivot station with carried ambiguities, per sat) with its measurement-standardized residual @c s_raw (already B2-normalized), the σ̂0-normalized @c s_norm, and the epoch σ̂0.  Answers "which station is poisoning the network".  VERY heavy (≈ stations x sats x RAIM-epochs — e.g. 50x40x3600 ≈ 7M rows); requires @ref raim (+ @ref phase_leveling) at runtime.  Off by default — byte-identical when OFF.
        bool trace_ssr_consumer{ false }; ///< L10: CONSUMER-side trace — the RealTimeEngine SSR consumer writes @c ssr_consumer_trace.csv (correction age, clk-applied, c0, URA, the recomputed R-inflation add-variance, phase-bias age/freshness, and disc counters) per modeled sat per processed epoch.  The V4 consumer-side evidence that the producer's URA/disc actually reach the user solution.  Off by default — byte-identical when OFF.
        bool trace_zwd{ false };          ///< L7: emit @c network_zwd.csv — per present station each emitted epoch, the estimated ZWD (a-priori state + reconstructed correction) and its datum-conditioned formal σ, plus a carried flag (a ZWD carry prior was actually injected).  The V7 instrument for border-station formal-σ sharpening under @ref carry_zwd — no data source for it exists otherwise.  O(stations x epochs); off by default — byte-identical when OFF.
        bool trace_events{ false };       ///< L2: emit @c network_events.csv — one structured LONG-format row per discrete QC ACTION (tiers T1 IF-slip … T6 clock-monitor, plus GATE / SIGMA0 / PIVOT / RETRO), replacing spdlog regex-scraping of which sat/station/arc was acted on and when.  Note pivot handover + retro-bump arming are otherwise SILENT.  Rows are written even on subsequently-withheld epochs (that IS the disposition story).  O(QC events); off by default — byte-identical when OFF (no new file).
        bool serialize_sbasis_contract{ false }; ///< B6: stamp the S-basis contract (declaration + a-priori orbit/clock product identity + phase-bias discontinuity threshold) into the product-file header (v2 "S" line) so a consumer can cross-check compatibility.  OFF (default) => the product file stays byte-identical to the v1 format.

        // --- RTA-12: product-contract identity + survey-grade station coordinates ---
        /// Declared reference frame of the fixed station coordinates, e.g. "IGS20".
        /// Pure declaration (no auto-detection); travels in the S-contract line as an
        /// optional @c frame= field — empty (default) writes nothing (byte-identity).
        std::string frame_id;
        /// Declared coordinate epoch — a decimal year ("2024.5") or an ISO date
        /// ("2024-07-01").  Travels as the optional @c cepoch= S-line field; empty
        /// (default) writes nothing.  When @ref use_sif_coordinates propagated any
        /// roster coordinate and this was left empty, the loader defaults it to the
        /// propagation date (decimal year).
        std::string coord_epoch;
        /// Resolve roster coordinates from the loaded SIF registry
        /// (@c processing.enable_station_info) for stations whose @c position_ecef
        /// was NOT typed in the JSON: the occupation valid at the run date is
        /// velocity-propagated to that date (x(t) = x_ref + v·(t − t_ref) + periodic
        /// + post-seismic; see @c resolveStationCoordinate).  Explicit JSON ECEF
        /// always wins.  OFF (default): a missing @c position_ecef stays the
        /// pre-RTA-12 hard load error.
        bool use_sif_coordinates{ false };
        /// Cheap coordinate-age advisory: when @ref coord_epoch is set and parseable
        /// and the run date lies more than this many years beyond it, log a single
        /// WARN at roster build (plate motion is ~2-7 cm/yr — recommend
        /// re-propagation).  0 (default) = off.  Suggested: 1.0.  Advisory only; a
        /// datum-event machinery (automatic re-propagation / product re-basing) is an
        /// RTF-scale follow-on, not this knob.
        double coord_age_warn_years{ 0.0 };
    };

    /**
     * @brief Configuration for real-time streaming mode.
     *
     * When AppMode::RealTime is selected these settings control the NTRIP
     * caster connection, observation source and output behaviour.
     */
    struct RealTimeConfig {
        std::string ntrip_host;                   ///< NTRIP caster hostname.
        int         ntrip_port{ 2101 };           ///< NTRIP caster port.
        std::string ntrip_mountpoint;             ///< NTRIP mountpoint name.
        std::string ntrip_user;                   ///< NTRIP username (empty = anonymous).
        std::string ntrip_password;               ///< NTRIP password.
        std::string nmea_gga;                     ///< Optional GGA sentence for VRS.
        int         reconnect_delay_s{ 5 };       ///< Seconds before reconnect on failure.
        bool        ntrip_use_tls{ false };       ///< Connect to the primary caster over HTTPS (port-443 casters such as CDDIS).  Transport setting, not a GnssConfig feature flag.  Default false keeps plain-HTTP casters byte-identical.
        bool        ntrip_tls_insecure{ false };  ///< When @c ntrip_use_tls, skip TLS certificate verification (testing only).
        std::string input_type{ "ntrip" };        ///< Transport for the primary stream: "ntrip", "tcp", "file".
        std::string input_format{ "rtcm" };       ///< Wire format of the primary stream: "rtcm" or "sbf" (Septentrio Binary Format).
        std::string input_path;                   ///< Path for "file" input.
        std::string tcp_host;                     ///< Host for "tcp" input (e.g. receiver IP).
        int         tcp_port{ 28784 };            ///< Port for "tcp" input.
        std::string output_type{ "console" };     ///< Output destination: "console", "file", "nmea".
        std::string output_path;                  ///< Path for file-based output.
        size_t      max_epoch_buffer{ 100 };      ///< Max epochs to buffer before dropping oldest.
        /// RTB-4: pin PRECISE (e.g. ultra-rapid SP3) orbits while satellite CLOCKS
        /// still arrive from the live/network stream ("ULT orbits + network clocks").
        /// Default false ⇒ the orbit source follows the clock source exactly as
        /// before (@ref GnssConfig::realtimeUsesLiveOrbitClockSource), byte-identical.
        /// When true (real-time only) the engine loads/resolves precise SP3 and
        /// skips only the precise CLK; the file-boundary switchover between
        /// consecutive ultra-rapid orbit files is a datum event the producer and
        /// consumer must cross at the SAME epoch (see RTF-1).
        bool        use_precise_orbits{ false };

        /**
         * @brief Optional second RTCM stream carrying SSR corrections.
         *
         * Independent of the primary stream above: the primary feeds rover
         * observations into the engine; this feeds RTCM SSR (1057-1068,
         * 1240-1270, eventually 4076) into the SsrCorrectionStore.  Both
         * streams may be active concurrently — the realtime loop polls
         * each with a short timeout and dispatches frames to the engine.
         *
         * Disabled by default; setting @c enabled=true selects the
         * stream type via @c input_type ("ntrip" or "file").
         */
        struct Corrections {
            bool        enabled{ false };
            std::string input_type{ "ntrip" };    ///< "ntrip", "tcp", or "file" (replay).
            std::string ntrip_host;
            int         ntrip_port{ 2101 };
            std::string ntrip_mountpoint;         ///< e.g. "IGS01" on products.igs-ip.net
            std::string ntrip_user;
            std::string ntrip_password;
            std::string nmea_gga;
            int         reconnect_delay_s{ 5 };
            bool        ntrip_use_tls{ false };   ///< Connect this aux caster over HTTPS (CDDIS-class port-443 SSR/nav mountpoints).
            bool        ntrip_tls_insecure{ false }; ///< When @c ntrip_use_tls, skip TLS certificate verification (testing only).
            std::string input_path;               ///< Path for "file" input_type.
            std::string tcp_host;                 ///< Host for "tcp" input.
            int         tcp_port{ 28784 };        ///< Port for "tcp" input.

            /**
             * @brief RTA-8(b): per-stream SSR ingest accept policy
             *        (@c corrections_accept key inside this stream's block).
             *
             * @c "all" (default) pushes every decoded SSR record from this
             * stream into the correction store — byte-identical legacy
             * behaviour.  @c "orbit_only" pushes orbit and URA records as
             * today but DROPS standalone clock, code-bias, phase-bias and
             * high-rate-clock records, and DECOMPOSES combined orbit+clock
             * records (1060/1066-class) into an orbit-only push.  This is the
             * ingest half of the "orbits from the external RTS stream,
             * clocks/biases from OUR network product" wiring: without it the
             * RTS stream's clock content would land in the singleton store
             * and shadow the injected network clock (the store's fresh
             * combined record wins over a standalone clock).  Unknown values
             * warn at load and fall back to @c "all".  Applied per frame in
             * @c RealTimeEngine::feedRtcmFrame via
             * @c RealTimeEngine::ssrPushAction.
             */
            std::string accept{ "all" };
        };
        Corrections corrections;                   ///< Optional SSR corrections stream.

        /**
         * @brief Optional third RTCM stream carrying broadcast ephemeris (nav).
         *
         * For an observation-only station mountpoint — one that streams MSM
         * obs but no broadcast nav (1019/1020/1042/1044/1046) — this supplies
         * the broadcast orbits/clocks that the SSR overlay corrects.  Without
         * it, @c LiveEphemerisStore stays empty for the obs constellations and
         * every satellite is unpositionable, so @c PppModel::computeModels
         * fails for the whole epoch ("No valid PPP observations modeled") and
         * the session yields 0 epochs.  Point this at a dedicated broadcast-
         * ephemeris caster mountpoint (e.g. @c BCEP00BKG0 on
         * products.igs-ip.net).  Frames route to
         * @c RealTimeEngine::feedRtcmFrame exactly like the corrections stream,
         * so decoded nav lands in the live store transparently.
         *
         * Independent of @c corrections: a PPP-RTK run off an obs-only station
         * needs all three sources (obs / nav / SSR).  Disabled by default — a
         * station that bundles its own nav (e.g. BRUX00BEL0) needs only the
         * primary + corrections streams.  Reuses the @c Corrections transport
         * descriptor (host/port/mountpoint/file/tcp); the two streams differ
         * only in payload (SSR vs broadcast nav), not transport.
         */
        using Ephemeris = Corrections;
        Ephemeris ephemeris;                       ///< Optional broadcast-ephemeris (nav) stream.

        /**
         * @brief RTA-6: path to a LIVE (growing) network product file to consume.
         *
         * When non-empty, @c Application::runRealTime owns an
         * @c SsrCorrectionStore + @c NetworkProductTail pair: the tail is polled
         * once per poll-loop iteration, newly completed product epochs are
         * replayed into the owned store, the store is injected into the engine
         * via @c RealTimeEngine::setSsrStore (consume flags from the
         * @c processing.network_consumer profile when enabled), and the engine's
         * obs↔SSR hold buffer is woken via @c notifyProductsAdvanced.  The
         * producer must run @c network.product_flush_per_epoch (and ideally
         * @c network.contract_rebroadcast_s + @c network.serialize_disc_counters
         * for late-join robustness).  Empty (default) = off; the realtime path
         * is byte-identical.
         */
        std::string network_products_path;

        /**
         * @brief RTA-12(3): blunder gate on the RTCM 1005/1006 a-priori seed (m).
         *
         * The engine seeds the filter a-priori from a decoded station ARP when no
         * configured reference position pins it.  With this gate armed (> 0) and a
         * plausible a-priori already present (a RINEX-header / SBF-PVT / earlier-ARP
         * seed), a decoded ARP that disagrees with it by more than this many metres
         * is REJECTED with a WARN carrying both coordinates and the distance — a
         * mis-configured caster (wrong mountpoint, stale 1005 coordinates) can
         * otherwise teleport the a-priori across the country.  A configured
         * reference position is never overwritten regardless; with the gate armed it
         * is additionally cross-checked against the broadcast ARP (WARN on
         * disagreement).  0 (default) = off — the legacy seed-always behaviour,
         * byte-identical.  Suggested: 10.0.
         */
        double arp_blunder_gate_m{ 0.0 };

        /**
         * @brief RTB-1(c): GPS-UTC leap-second count for the RT wall-clock →
         *        GPS conversion (@c realtime.leap_seconds).
         *
         * Default 18 = the compiled @c TimeUtils::LEAP_SECONDS (valid from
         * 2017-01-01), so an absent key changes nothing.  Applied via
         * @c TimeUtils::setLeapSecondsOverride at @c RealTimeEngine::initialize,
         * it feeds @c TimeUtils::nowContinuousGpsSeconds — the host-clock
         * anchor of the RTCM decode paths (cold-start week promotion, GLONASS
         * day resolution, obs-lag measurement) — so a future leap-second
         * insertion can be accommodated by config before a rebuild.  Deriving
         * the count from the broadcast nav message (LNAV ΔtLS) is the
         * RTF-scale follow-on.
         */
        int leap_seconds{ 18 };

        /**
         * @brief RTB-3: derive the LLI loss-of-lock bit (0x1) from MSM lock
         *        times (@c realtime.msm_lli_from_locktime).
         *
         * RTCM MSM carries per-(sat, signal) LOCK TIMES, not RINEX LLI flags;
         * only the half-cycle 0x2 bit can be set statelessly at decode.  With
         * this armed, @c RealTimeEngine runs a @c LockTimeTracker over the
         * decoded lock times at epoch assembly and ORs 0x1 into the assembled
         * observation's LLI when lock decreased (RTKLIB @c lossoflock rule —
         * see LockTimeTracker.hpp).  Default false: arming CHANGES
         * slip-detection behaviour on RT replays (the LLI-triggered reset
         * paths start firing), so it must be an explicit choice.
         */
        bool msm_lli_from_locktime{ false };

        /**
         * @brief RTB-5(c): honor the SSR orbit reference point in the satellite
         *        PCO term (@c realtime.honor_ssr_reference_point).
         *
         * Legacy RTCM SSR (1057/1060/1240-series) corrects the broadcast orbit
         * to the satellite ANTENNA PHASE CENTER; IGS-SSR 4076 and SP3 correct
         * to the CENTER OF MASS.  @c PppModel applies the satellite PCO
         * unconditionally — correct for CoM baselines, but a DOUBLE COUNT of
         * the PCO z-offset (0.6-2.6 m along the LOS) when an APC-referenced
         * legacy-SSR orbit is in effect.  When armed, the satellite-PCO range
         * term is SKIPPED for exactly those satellites whose applied orbit
         * correction this epoch is tagged @c SsrReferencePoint::APC (recorded
         * per satellite by @c Geometry at the orbit-overlay apply sites).
         * Default false = byte-identical legacy behaviour, including for
         * users knowingly running legacy SSR with the historical double count.
         */
        bool honor_ssr_reference_point{ false };
    };

    /**
     * @brief First-class NETWORK-CONSUMER profile for a standalone user (RTA-7).
     *
     * The closed-loop harness inside @c Application::runNetwork derives its
     * consumer config from the producer config
     * (@c GnssConfig::deriveClosedLoopConsumerConfig).  A STANDALONE real-time
     * consumer — a separate process tailing a live product file
     * (@c realtime.network_products_path) — has no producer config to derive
     * from, so this profile block (@c processing.network_consumer) declares the
     * same intent explicitly and @c GnssConfig::applyNetworkConsumerProfile
     * applies the SAME couplings through the shared
     * @c GnssConfig::applyNetworkConsumerCouplings helper (one source of truth
     * with the closed-loop derivation).  Default @c enable=false leaves every
     * path byte-identical.
     *
     * RTA-9(a): when @c bias is on and the user did not explicitly set
     * @c processing.enable_ssr_bias_max_age, @c applyNetworkConsumerProfile
     * arms the bias staleness gate (an explicit @c false is respected) —
     * consuming network biases without an age gate would silently apply
     * unboundedly stale biases across producer withhold gaps.
     *
     * RTA-9(c) — cadence sizing: the consumer's staleness gates only work if
     * the producer emit cadence is MUCH shorter than the acceptance window —
     * size @c processing.ssr_max_age_s so that emit cadence ≪ ssr_max_age/2
     * (e.g. a 5 s product cadence comfortably fits the 30 s default; a 30 s
     * cadence needs ssr_max_age ≥ 60 s or every other epoch drops to
     * broadcast-grade).  @c processing.ssr_clock_stale_hold_factor buys
     * additional margin for producer WITHHOLD gaps (RAIM/QC epochs that emit
     * nothing): held clocks stay applied with honest R inflation instead of
     * falling off the broadcast-grade cliff.
     *
     * Card 7 — the AGE-SKEW rung of the same ladder: beyond each product's OWN
     * age, the ORBIT and CLOCK products must agree with each other.  A brdc_rts
     * clock echoes the RTS orbit it was estimated against;
     * @c orbit_clock_max_skew_s (below) withholds the clock when the applied
     * orbit disagrees with that echo — the withheld satellite then rides the
     * ordinary staleness rungs above (stale-held extrapolation, then
     * broadcast-grade drop), and the per-sat mismatch counters (store-side,
     * summarized by @c RealTimeEngine beside the RTA-13 counters) make the
     * incoherence visible either way.
     */
    struct NetworkConsumerConfig {
        bool enable{ false };        ///< Master switch.  Only takes effect when a product source exists (realtime.network_products_path set).
        bool clock{ true };          ///< Consume the network CLOCK (setSsrStore consume_clock).  false = keep precise SP3/CLK, consume only the bias products.
        bool bias{ true };           ///< Consume the network code/phase BIAS (setSsrStore consume_bias).
        bool undifferenced{ true };  ///< Consumer user model: true = undifferenced uncombined PPP-RTK user (direct per-band LAMBDA, no BSSD); false = legacy BSSD consumer.
        /**
         * @brief RTA-8(a): where the SSR ORBIT overlay reads from
         *        (@c processing.network_consumer.orbit_source).
         *
         * @c "external" (default): the orbit overlay stays on the process-wide
         * singleton store — fed by the external RTS corrections stream (which
         * should be ingest-filtered to @c corrections_accept="orbit_only",
         * RTA-8(b), so its clock content cannot shadow the network clock) —
         * while clocks/biases come from the injected network-product store.
         * @c "network": the orbit overlay reads the injected store too, for a
         * future network product that carries its own orbit corrections.
         * Unknown values warn at load and fall back to @c "external".  Maps to
         * @c RealTimeEngine::SsrOrbitSource at the @c setSsrStore call site in
         * @c Application::runRealTime; ignored when the profile is off (the
         * legacy coupled routing is used).
         */
        std::string orbit_source{ "external" };
        /// Optional EXPLICIT override for @c RealTimeEngine::setArIgnoreMwWl
        /// (tri-state: -1 = absent → derive exactly as the closed-loop driver
        /// does, @c !getArWlOnly() after the couplings; 0/1 = forced).  The MW
        /// detector's WL integers are in the .BIA frame — feeding them to AR
        /// while the float carries NETWORK phase biases injects a conflicting WL
        /// datum, so the derived value is true for the undifferenced consumer.
        int  ar_ignore_mw_wl{ -1 };
        /**
         * @brief Card 7 (RTF-2): orbit/clock coherence age-skew budget (s)
         *        (@c processing.network_consumer.orbit_clock_max_skew_s).
         *
         * A @c brdc_rts network clock record echoes the RTS orbit update it was
         * estimated against (IOD-SSR + content epoch); broadcast radial-orbit
         * error absorbs 1:1 into that clock, so consuming it against a
         * DIFFERENT orbit update is a silent dm–m range bias invisible to
         * residual QC.  With this budget set > 0 the consumer WITHHOLDS the
         * clock for a satellite whose applied orbit disagrees with the echo
         * (different IOD-SSR, no applied orbit at all, or a content epoch
         * skewed by more than the budget) — the RTA-9 staleness ladder then
         * governs (stale-hold rung, then broadcast-grade drop), exactly as for
         * a missing clock.  Default 0 = the epoch-skew comparison and the
         * withhold are OFF; IOD-SSR identity mismatches are still counted
         * (per-sat counters on the store, RTA-13 count-only convention).
         * Armed on the injected/owned product store and the singleton at the
         * @c setCodeBiasBandFallback sites.  Size it to the producer's RTS
         * orbit update interval (one generation ≈ 5–30 s).
         */
        double orbit_clock_max_skew_s{ 0.0 };
        std::string expect_reference_receiver;  ///< Optional S-contract expectation: producer reference receiver id ("" = don't pin).
        std::string expect_apriori_product_id;  ///< Optional S-contract expectation: a-priori orbit/clock product identity ("" = don't pin).
        std::string expect_frame_id;            ///< RTA-12 optional S-contract expectation: reference frame id, e.g. "IGS20" ("" = don't pin).
        std::string expect_atx_id;              ///< RTA-12 optional S-contract expectation: producer ATX basename, e.g. "igs20.atx" ("" = don't pin).
        std::string expect_orbit_ref;           ///< RTA-12 optional S-contract expectation: orbit reference point, "CoM" or "APC" ("" = don't pin).
    };

    /**
     * @brief Enumeration for GNSS processing modes.
     *
     * Defines the available processing modes for the GNSS processor.
     */
    enum class ProcessingMode {
        PPP,             ///< Precise Point Positioning (single receiver, external products).
        PPP_RTK,         ///< PPP with RTK enhancements (single receiver, uncombined).
        BaselineSingle,  ///< Double-differenced single baseline: one rover + one reference station.
        BaselineMulti,   ///< Double-differenced multi-receiver network (star topology: N rovers against one reference).
        PPP_RTK_Network, ///< Minimum-constraint S-system network PRODUCER: joint multi-station estimator
                         ///< emitting satellite clock + code/phase bias (+ atmosphere) under a declared S-basis.
        PPP_RTK_User     ///< PPP-RTK CONSUMER of network PPP_RTK_Network products (post-processing closed-loop
                         ///< counterpart): uncombined PPP-RTK + network SSR bias/atmosphere consume + S-basis cross-check.
    };

    /**
     * @brief Frequency bands the Kalman filter estimates.
     *
     * Renamed from @c SignalMode — "frequency" reflects what this enum
     * actually controls (band scope), leaving "signal" to mean the full
     * (band, tracking-attribute) tuple.  Pair this with @c ChannelMode
     * to express the full KF active-signal set.
     */
    enum class FrequencyMode {
        Dual,   ///< Two bands per constellation (e.g. GPS L1+L2).
        Triple, ///< Three bands per constellation (e.g. GPS L1+L2+L5).
        All,    ///< Every band a constellation publishes.
        Independent ///< Like @c All, but drops a constellation's E5/B2 *sidebands*
                    ///< (Galileo E5a+E5b, BeiDou B2a+B2b — RINEX bands 5 & 7) for
                    ///< any satellite that also reports the coherent **wideband**
                    ///< (RINEX band 8: Galileo E5 alt-BOC, BeiDou B2(a+b)).  The
                    ///< wideband is the joint coherent processing of those same
                    ///< sidebands, so feeding all three to the diagonal-R filter
                    ///< double-counts the band as if independent — inflating its
                    ///< weight and letting one signal's bias drag the shared
                    ///< (iono/clock/tropo/position) states.  Pruning keeps only
                    ///< the independent information.  Decided per-satellite from
                    ///< the observed signals: when the wideband is absent the
                    ///< sidebands are retained (graceful fallback — no band lost).
    };

    /**
     * @brief Tracking-mode channel selection within an active band.
     *
     * @c Single picks one canonical tracking attribute per band via
     * @c signalAttributePriority (e.g. GPS L1 → L1W); @c All keeps every
     * tracked channel as an independent ambiguity state (e.g. L1W and
     * L1C both get their own ambiguity).  State-vector size scales with
     * the channel count, but the redundancy aids cross-consistency.
     */
    enum class ChannelMode {
        Single, ///< Canonical tracking attribute per band (default).
        All     ///< Every tracking-mode channel present in the band.
    };

    /**
     * @brief MW slip detector frequency-band scope.
     *
     * Independent of the KF's @c FrequencyMode so the detector can be
     * tuned for cross-consistency without inflating the state vector.
     * @c Mirror inherits the KF's @c FrequencyMode; the other values
     * override it.
     */
    enum class MwFrequencyMode {
        Mirror, ///< Match the KF's @c FrequencyMode (default).
        Dual,   ///< Override: two bands.
        Triple, ///< Override: three bands.
        All     ///< Override: every available band (from @c raw_observed).
    };

    /**
     * @brief MW slip detector channel-multiplicity scope.
     *
     * Independent of the KF's @c ChannelMode.  @c Mirror inherits the
     * KF setting; @c Single or @c All overrides it.  A common
     * combination is KF @c Single + MW @c All — lean state vector but
     * every tracked channel feeds slip cross-checks.
     */
    enum class MwChannelMode {
        Mirror, ///< Match the KF's @c ChannelMode (default).
        Single, ///< Override: canonical channel only.
        All     ///< Override: every tracked channel.
    };

    /**
     * @brief Carrier-phase ambiguity parameterization in the Kalman state.
     *
     * Selects how phase ambiguities are stored in the state vector — and,
     * by extension, how the measurement Jacobian is built and how
     * @c AmbiguityResolver / @c MwSlipDetector interact with the float
     * solution.
     *
     *  - @c PerBand : one ambiguity per (satellite, signal/IF combo), the
     *    historical layout.  Key format @c "<sat>_<sig>" (e.g. @c "G01_L1C",
     *    @c "G01_L1C_L5Q").  The MW detector resolves WL integers
     *    independently and feeds them to AmbiguityResolver as a
     *    pseudo-observation map.
     *
     *  - @c NlWl : one narrow-lane state per (sat, band) plus one
     *    wide-lane state per (sat, band-pair).  Key formats
     *    @c "<sat>_NL_<band>" and @c "<sat>_WL_<band_hi>_<band_lo>"
     *    (e.g. @c "G01_NL_1", @c "G01_WL_1_2").  WL slip-detection
     *    output writes directly into the WL state covariance, and
     *    LAMBDA can search the NL sub-vector with WL pre-constrained.
     *    Halves the integer search dimension and exposes the full WL
     *    information to the filter rather than only as an
     *    out-of-band hold update.
     *
     * Default is @c PerBand to preserve existing behavior; switch via
     * @c estimation.state_ambiguity_basis.
     */
    enum class StateAmbiguityBasis {
        PerBand, ///< One ambiguity per (sat, signal) — historical layout.
        NlWl     ///< Narrow-lane + wide-lane state pair per (sat, band-pair).
    };

    /**
     * @brief Enumeration for satellite velocity interpolation.
     *
     * Specifies the type of interpolation used for satellite velocity computation.
     */
    enum class SatelliteInterpolationMode {
        Lagrange,///< Lagrange analytical derivative.
        Finite,  ///< Finite central difference.
        Hermite, ///< Monotone Hermite slopes.
        Spline   ///< Cubic spline.
    };

    /**
     * @brief Enumeration for satellite clock interpolation.
     *
     * Specifies the type of interpolation used for satellite clock computation.
     */
    enum class SatelliteClockInterpolationMode {
        Linear,         ///< 2-node bracket linear interpolation (MMSE-optimal for pure Brownian motion with noise-free samples).
        Lagrange,       ///< Bracket-centered polynomial Lagrange (overfits noisy random-walk clocks; not recommended for 30 s IGS Final).
        LsqSatClock     ///< Weighted linear LSQ over multi-node window, anchored by explicit clock-rate observations at the
                        ///< coarse-cadence epochs (typ. every 5 min in IGS Final).  Reduces observation-noise component by
                        ///< ~sqrt(N/2) vs Linear at the cost of mm-level RW bias and discontinuity at clock epochs (window
                        ///< slides → LSQ refit).  Degrades gracefully to Linear when too few nodes are available.
    };

    /**
     * @brief Source of a priori pressure and temperature used by the
     *        Saastamoinen zenith hydrostatic delay.
     */
    enum class TroposphereAprioriSource {
        StandardAtmosphere, ///< Frozen sea-level @c receiver.zhd_pressure_mbar / @c zhd_temp_kelvin reduced by standard lapse.
        Gpt3                ///< Lookup pressure & temperature from the GPT3 5° grid at the receiver lat/lon/h/doy (Landskron & Boehm 2018).
    };

    /**
     * @brief Tropospheric mapping function used to map zenith to slant
     *        delays.
     *
     * @c Vmf3 supplies @c a_h/@c a_w from the GPT3 5° grid (empirical
     * climatology, no operational 6-hourly product chain) combined with
     * the published degree/order-12 spherical-harmonic expansion of
     * @c b/c (Landskron & Boehm 2018).  GPT3 must be loaded — selecting
     * @c Vmf3 automatically populates the GPT3 grid even when
     * @c troposphere_apriori_source is left at @c StandardAtmosphere.
     */
    enum class TroposphereMappingFunction {
        Gmf,   ///< Global Mapping Function (Boehm et al. 2006).  No external grid required.
        Vmf3   ///< VMF3 empirical from GPT3 (Landskron & Boehm 2018).  Requires gpt3_5.grd.
    };

    /**
     * @brief Enumeration for measurement stochastic (weighting) models.
     *
     * Selects how observation variances are scaled based on satellite geometry
     * or signal quality indicators.
     */
    /**
     * @brief Configuration for the post-fit residual history tracker.
     *
     * The tracker (see @c filter/ResidualHistoryTracker.hpp) maintains
     * an EWMA mean and variance of the post-fit residual for each
     * (satellite, signal) row.  When the spatial pivot-slip detector
     * in @c KalmanFilter flags a candidate group, the tracker
     * supplies a temporal corroboration signal: the fraction of
     * rows in the group whose residual exceeds @c z_threshold sigma
     * relative to its own EWMA history.  When that fraction exceeds
     * @c fraction_threshold the temporal evidence and spatial
     * evidence agree → high-confidence pivot slip.
     *
     * All settings live under the JSON path
     * @c estimation.residual_history.{enabled,alpha,warmup_n,
     * z_threshold,fraction_threshold}.
     */
    struct ResidualHistoryConfig {
        bool        enabled            = false; ///< Master toggle.  Default off; opt in via JSON.
        double      alpha              = 0.05;  ///< EWMA forgetting factor (≈30-epoch effective window at α=0.05).
        std::size_t warmup_n           = 20;    ///< Min samples per (sat, signal) before z-tests fire.
        double      z_threshold        = 4.0;   ///< |z| threshold for declaring a row temporally anomalous.
        double      fraction_threshold = 0.5;   ///< Group-level fraction-of-anomalous-rows threshold for corroborating a spatial pivot-slip request.
    };

    /**
     * @brief Configuration for the stuck-code-bias detector + reset path.
     *
     * The sat-bias estimator can fall into a closed loop where the
     * code residual on one (satellite, signal) row keeps tripping
     * the outlier gate: the bias state has over-converged around a
     * value that no longer matches the truth, every new innovation
     * shows up as a large residual, the gate excludes the row, and
     * the bias state never gets the chance to update.  Either or
     * both of two signals fire the reset:
     *  - @c min_streak consecutive outlier-rejection epochs on the
     *    same (sat, signal) — the "already-stuck" case where the
     *    bias is wrong enough that nothing ever survives the gate.
     *  - EWMA |mean| of post-fit residuals exceeding @c bias_floor_m
     *    on a warmed-up row — the slow-drift case where the row
     *    still survives the gate but its converged bias has drifted.
     *
     * On trigger the corresponding sat-bias state is re-seeded
     * (value snapped by the EWMA mean if available; variance
     * inflated back to @c initial_std.sat_bias²) via
     * @c StateManager::resetSatBiasState.  The tracker's history for
     * that row is also cleared so the next handful of epochs
     * re-establish the baseline against the fresh state.
     *
     * All settings live under
     * @c estimation.stuck_code_bias.{enabled,min_streak,bias_floor_m}.
     */
    struct StuckCodeBiasConfig {
        bool        enabled       = true; ///< Master toggle.  Default on — cheap to evaluate and protects against a well-known failure mode.
        std::size_t min_streak    = 5;    ///< Consecutive-rejection epochs that trigger a reset.  0 disables the streak branch.
        double      bias_floor_m  = 3.0;  ///< EWMA |mean| (m) above which a warmed-up row's bias is considered drifted.  0 disables the drift branch.
    };

    /**
     * @brief Configuration for sat-wide state re-inflation after a
     *        cycle-slip / LLI ambiguity reset.
     *
     * In PPP-RTK uncombined mode the per-sat iono state and the
     * per-(sat, signal) sat-bias states can co-converge to a joint
     * partition that absorbs slant iono in a non-unique way — the
     * iono state holds I_true minus some leakage δ/α, and the
     * sat-bias state holds b_true plus δ.  Phase ambiguities anchor
     * the partition while they're alive.  When a sat-wide cycle slip
     * or LLI lock-loss resets *every* phase ambiguity for that
     * satellite, the new ambiguities reseed against whatever the
     * current iono/sat-bias state say — baking the (possibly wrong)
     * pre-reset partition into the rest of the arc.  The residual
     * on the unanchored-by-phase signals (typically L5 in a
     * triple-frequency setup) then carries the
     * elevation-dependent unmodelable component for hundreds of
     * epochs.
     *
     * On sat-wide ambiguity reset the iono state's diagonal is
     * re-inflated to @c iono_std_m² and every sat-bias state for
     * that satellite has its diagonal re-inflated to
     * @c sat_bias_std_m² — values are preserved (slant iono and
     * hardware delays don't change just because the receiver lost
     * phase lock), only the cross-state covariance is broken so the
     * filter can re-partition cleanly via the next several epochs
     * of measurements.  Pivots are skipped at the StateManager layer
     * (their frozen (0, 0) row is detected by the diag==0 guard).
     *
     * All settings live under
     * @c estimation.slip_state_reinflation.{enabled,iono_std_m,sat_bias_std_m}.
     */
    struct SlipStateReinflationConfig {
        bool   enabled        = true; ///< Master toggle.  Default on — the partition-baking failure mode is structural to uncombined PPP-RTK with sat-bias estimation.
        double iono_std_m     = 5.0;  ///< Re-inflated iono diagonal std (m).  0 or negative disables the iono branch (StateManager falls back to initial_std.ionosphere).
        double sat_bias_std_m = 5.0;  ///< Re-inflated sat-bias diagonal std (m).  0 or negative disables the sat-bias branch (StateManager falls back to initial_std.sat_bias).
    };

    /**
     * @brief Real-time startup gate: defer estimation until the SSR correction
     *        stream is ready, so the filter seeds code+phase together instead of
     *        cold-starting code-only on metre-grade broadcast clocks.
     *
     * Cold-starting before SSR is in place is harmful: the filter converges
     * position / receiver clock / per-sat iono / sat biases against metre-level
     * broadcast clocks, and then each satellite's SSR flips it from
     * broadcast-grade to cm-grade at a DIFFERENT epoch — turning phase on
     * mid-arc and seeding a fresh ambiguity against an already-contaminated
     * (iono, bias) partition.  Waiting until corrections are ready avoids the
     * wasted code-only convergence and the staggered partition-baking flips.
     *
     * Readiness criterion (real-time only): at least @c min_satellites satellites
     * are broadcast+SSR (cm-grade clock, phase retained — NOT @c
     * PppObservation::broadcast_only).  By default that QUORUM is sufficient and
     * stragglers still on a broadcast-only clock join later as their SSR arrives
     * — a late sat joining an already-converged RTK filter is benign (its fresh
     * ambiguity initialises against a healthy state; the harm the gate prevents
     * is the GLOBAL code-only cold-start, not individual late joiners).  Set @c
     * require_all_above_mask to instead block until EVERY modeled above-mask sat
     * is broadcast+SSR (no straggler flips at all, but one lagging sat stalls the
     * start).  Until satisfied the engine accumulates nav/SSR, keeps refreshing
     * the a-priori, and runs no predict/update.  No-op in post-processing (all
     * products present at once, so @c computeModels never yields broadcast_only).
     *
     * Config: @c estimation.defer_until_corrections_ready.{enabled,min_satellites,require_all_above_mask,timeout_s}.
     */
    struct CorrectionsReadyGateConfig {
        bool   enabled        = false; ///< Master toggle.  Default off — preserves the incremental cold-start for existing configs.
        int    min_satellites = 5;     ///< Global quorum / solvable floor: start once at least this many sats (across all constellations) are cm-grade (broadcast+SSR).
        int    min_satellites_per_system = 4; ///< Per-constellation quorum: each active constellation with >=2 above-mask sats must have at least min(this, its above-mask count) cm-grade before start.  Guarantees multi-constellation phase geometry at start (prevents the GPS-only-phase cold start that drives the ZWD/height degeneracy).  <=0 disables the per-system check (flat global floor only).
        bool   require_all_above_mask = false; ///< If true, ALSO require every above-mask sat to be broadcast+SSR (no stragglers).  Default false = quorum (start at the floors; late sats join as SSR arrives).
        bool   drop_broadcast_grade = false; ///< If true, drop broadcast-grade (broadcast_only) sats from the solution ENTIRELY each epoch (not just their phase) once estimation runs — so metre-grade code never pollutes a precise SSR PPP-RTK solution and a broadcast-grade sat can never seat as a BSSD pivot.  Default false = keep broadcast-grade code (broadcast-only float).
        double timeout_s      = 0.0;   ///< If > 0, start in degraded mode after this many seconds without readiness.  0 = wait indefinitely.
    };

    /**
     * @brief Soft a-priori constraint on the zenith wet delay (ZWD) state.
     *
     * Under BSSD the only receiver-side common-mode states are position + ZWD,
     * and the [up, ZWD] subspace is near-collinear (both enter the single-
     * differenced range via monotone elevation functions).  With no anchor a
     * common-mode start error runs equal-and-opposite into that near-null
     * direction (ZWD and height to ~1000 m) until satellite geometry re-
     * conditions it.  A recurring soft pseudo-observation pulls ZWD toward a
     * climatological value AND shrinks the [up, ZWD] covariance correlation each
     * epoch — the covariance-consistent form of a "don't let ZWD run away"
     * clamp (a value-only clamp would fight the filter and never recover the
     * height).  At loose @c sigma_m it costs the converged solution nothing (the
     * cm-level geometric ZWD signal overrides the prior once geometry is good).
     *
     * Config: @c estimation.zwd_constraint.{enabled,apriori_m,sigma_m}.
     */
    struct ZwdConstraintConfig {
        bool   enabled   = false; ///< Master toggle.  Default off — opt in (set true for real-time BSSD PPP-RTK).
        double apriori_m = 0.10;  ///< Climatological ZWD prior mean (m).  ~0.1 m is a temperate-latitude default.
        double sigma_m   = 0.30;  ///< Prior 1-sigma (m).  Loose enough not to bias the converged cm-level ZWD; tight enough to bound the null-space runaway.  Tighten toward ~0.1 m for a harder anchor.
    };

    /**
     * @brief Reference level the MW detector measures slip deviations against.
     *
     * The MW slip test is |mw[k] − reference| > kσ·spread.  The choice of
     * reference determines what "deviation" means and which spread estimate
     * is statistically consistent with it:
     *
     *  - @c HatchMean — legacy behavior.  Reference is the slow Hatch
     *    (recursive) mean (α = 1/100 at maturity, ~100-epoch memory), spread
     *    is the MAD of first time-differences.  Under a smooth MW trend of
     *    slope s/epoch the lag offset settles at ≈ 99·s while the TD1 MAD
     *    stays at the noise floor — the statistics are mismatched, and mild
     *    drifts (≲ 1 mm/epoch) produce persistent false slips on mature
     *    arcs; steeper drifts (~7–9 mm/epoch) can even pass the repair
     *    fractional gate and publish a confident wrong integer.
     *
     *  - @c LocalMedian — reference is the median of a short causal window
     *    of recent accepted MW values (length @c mw_local_window), spread is
     *    the MAD of the same window (×√2 so the configured
     *    @c mw_sigma_multiplier keeps its legacy TD1-equivalent scale).
     *    Deviation and spread now measure the same quantity, so a trend
     *    lifts both and cancels to first order, while a genuine step still
     *    spikes the deviation.  Causal — no future data; the current epoch
     *    is excluded from its own reference.  Residual weakness: a smooth
     *    QUADRATIC signal (curvature) still walks the level away from the
     *    window median faster than the window forgets, leaking into the
     *    deviation as a soft ramp.
     *
     *  - @c TimeDifference — no level reference at all: the deviation IS
     *    the time-difference of the MW series (order selected by
     *    @c mw_td_order), gated by the MAD of the same time-difference
     *    history.  Memoryless, hence immune to trends AND curvature: a
     *    linear drift contributes a near-constant offset (first order) or
     *    exactly zero (second order) that the matching MAD absorbs, while
     *    a slip still spikes by dN·λ_WL for one epoch.  The price is
     *    noise amplification (σ_TD1 = √2·σ_MW, σ_TD2 = √6·σ_MW) — smaller
     *    real slips sit closer to the gate.  Second order additionally
     *    echoes a real slip with opposite sign one epoch later; the echo
     *    is contained by the erase-on-slip arc restart (unlocked arcs)
     *    or absorbed (locked arcs).
     *
     * In every mode the Hatch mean and the WL integer/lock machinery are
     * unaffected by this selection.
     *
     * Configured via @c estimation.cycle_slip.mw_detect_reference
     * ("hatch_mean" | "local_median" | "time_difference").
     */
    enum class MwDetectReference {
        HatchMean,      ///< Legacy: deviation from the slow Hatch mean, TD1-MAD spread.
        LocalMedian,    ///< Deviation from a short causal window median, window-MAD spread.
        TimeDifference  ///< Deviation IS the MW time-difference (order per MwTdOrder), TD-MAD spread.
    };

    /**
     * @brief Differencing order for @c MwDetectReference::TimeDifference.
     *
     *  - @c First  — dev = mw[k] − mw[k−1].  A linear MW trend of slope s
     *    per epoch appears as a constant ≈ s (millimeters — far below the
     *    √2-amplified noise), a quadratic as a slow ramp the matching TD1
     *    MAD tracks.  The default and the recommended starting point.
     *  - @c Second — dev = mw[k] − 2·mw[k−1] + mw[k−2].  Exactly zero for
     *    any linear trend and a tiny constant for a quadratic, at the cost
     *    of √3 further noise amplification over first order and a
     *    sign-flipped echo of every real slip one epoch later.  Reserve
     *    for data where curvature defeats the first-order statistic.
     *
     * Configured via @c estimation.cycle_slip.mw_td_order
     * ("first" | "second").
     */
    enum class MwTdOrder {
        First,   ///< mw[k] − mw[k−1] (default).
        Second   ///< mw[k] − 2·mw[k−1] + mw[k−2].
    };

    /**
     * @brief Selects which cycle-slip detectors run.
     *
     * Drives config-driven composition of the @c CycleSlipManager detector
     * set via @c SlipDetectorRegistry::createEnabled.  The Melbourne-Wübbena
     * wide-lane detector is the primary detector and the source of the WL
     * integers consumed by ambiguity resolution and the BSSD pivot machinery,
     * so @c mw defaults on; the auxiliary detectors default off (preserving
     * the historical MW-only behavior) and are opted in per kind.  Kinds whose
     * implementation is still STAGED (geometry-free @c gf, ionosphere-free
     * phase @c ifp) are accepted for forward compatibility but the registry
     * skips them with a warning until their .cpp is compiled in.
     *
     * All detector-selection settings live under
     * @c estimation.cycle_slip.{mw,graphic,ewl,gf,if}.
     *
     * @c turbo_edit is not a detector kind but a *coordination mode*: when set
     * (and the MW detector is enabled) the @c CycleSlipManager / @c RealTimeEngine
     * pair the MW wide-lane integer @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$
     * with the geometry-free phase jump
     * @f$\Delta\Phi_{GF}=\lambda_1\Delta N_1-\lambda_2\Delta N_2@f$ to solve for
     * the per-signal integer slips @f$(\Delta N_1,\Delta N_2)@f$ (Blewitt
     * TurboEdit).  This resolves a dual-frequency slip into a targeted
     * covariance-preserving per-signal repair — which MW alone cannot do
     * (one equation, two unknowns) — and catches MW's @f$\Delta N_1=\Delta N_2@f$
     * blind spot.  It is RATE-ADAPTIVE: at high rate (≤1 s) the GF jump is
     * essentially iono-free and the full repair machinery engages; at coarser
     * rates (e.g. 30 s RINEX) the un-cancelled ionospheric residual honestly
     * inflates the measured GF noise, and every GF-backed decision is gated
     * on that noise against the integer lattice it must discriminate
     * (@c turbo_edit_split_max_sigma) — large MW-detected slips still split
     * and repair under quiet ionosphere, and flagged satellites fall back to
     * the conservative reset as activity rises.  A per-epoch cross-satellite
     * IVI gate additionally suppresses repair when the ionosphere is
     * disturbed, regardless of rate.  See the @c TurboEditResolver header's
     * "Rate adaptivity" note.
     */
    /**
     * @brief Which navigation aiding sources participate in the estimator.
     *
     * Selection tokens consumed by @c AidingSourceRegistry::createEnabled to
     * compose the @c AidingSourceManager source set — the navigation-aiding
     * analog of @c CycleSlipConfig / @c SlipDetectorRegistry.  Each flag maps to
     * an @c AidingSourceKind; the IMU is the primary (built first).  For
     * back-compatibility, @c imu defaults to the existing @c imu.enabled
     * (@c enableImu()) when @c estimation.aiding.imu is not set explicitly.
     * @c vision is STAGED (its .cpp is excluded from the build; the registry
     * warns and skips it if selected).
     *
     * @see docs/aiding_source_abstraction_design.md
     */
    struct AidingConfig {
        bool imu    = false;  ///< IMU INS error-state aiding (primary; default = enableImu()).
        bool zupt   = false;  ///< Zero-velocity update pseudo-measurement.
        bool nhc    = false;  ///< Non-holonomic constraint (lateral/vertical body velocity ~ 0).
        bool wheel  = false;  ///< Wheel/odometer speed.
        bool baro   = false;  ///< Barometric height.
        bool vision = false;  ///< Visual odometry / landmarks (STAGED — registry skips with a warning).

        /// ZUPT pseudo-measurement std (m/s) per velocity axis: the assumed
        /// uncertainty of the "velocity == 0 while stationary" constraint.
        /// Smaller => stronger pull to zero.  Via estimation.aiding.zupt_sigma_mps.
        double zupt_sigma_mps = 0.05;
    };

    struct CycleSlipConfig {
        bool mw         = true;   ///< Melbourne-Wübbena wide-lane detector (primary; default on).
        bool graphic    = false;  ///< GRAPHIC code+phase per-signal detector.
        bool ewl        = false;  ///< Extra-wide-lane MW detector (3+ frequency satellites).
        bool gf         = false;  ///< Geometry-free detector (STAGED — registry skips with a warning if set).
        bool ifp        = false;  ///< Ionosphere-free phase detector (STAGED — registry skips with a warning if set).
        bool turbo_edit = false;  ///< MW+GF joint per-signal slip repair (TurboEdit); rate-adaptive. See struct doc.
        /// MW-only code-domain veto (effective only with @c turbo_edit): an MW
        /// flag whose geometry-free phase is affirmatively stable (|TD2| within
        /// the arc's own MAD noise — with the zero gate sharp enough to
        /// discriminate the slip lattice — under quiet iono / mature arc /
        /// no LLI) is classified as a code-domain artifact (multipath, NLOS,
        /// DLL stress) and the mandated phase ambiguity reset is SKIPPED — no
        /// integer pair of plausible size is consistent with a flat GF, and the
        /// KF phase-innovation (residual) gate backstops the residual
        /// null-line-pair risk.  See the TurboEditResolver header's
        /// "code-domain veto" note.  Via
        /// @c estimation.cycle_slip.turbo_edit_mw_only_veto.
        bool turbo_edit_mw_only_veto = true;
        /// Gap-spanning GF bridge (effective only with @c turbo_edit): when a
        /// GF arc's chain breaks for at most this many seconds (per-signal
        /// dropout or full receiver gap), the pre-gap GF level is stashed and
        /// the ACROSS-GAP GF jump — drift-corrected by the arc's own EWMA GF
        /// rate — serves as the joint-solve evidence at the first post-gap
        /// epochs, instead of being blind until three consecutive epochs
        /// rebuild a TD2.  A λ-quantized jump attributes the re-lock slip per
        /// signal (continuous signal → integer repair; dropped signal → its
        /// ambiguity was pruned-and-reseeded on return, so the slip is moot
        /// and the mandate is suppressed); a flat jump classifies the MW flag
        /// as a code artifact (re-acquisition DLL transient).  Bounded by
        /// ionospheric coherence: beyond ~15 s even quiet-iono drift
        /// approaches the integer-pair spacing.  0 disables (pre-bridge
        /// behavior: post-gap MW flags are unadjudicated → reset).  Via
        /// @c estimation.cycle_slip.turbo_edit_max_gap_s.
        double turbo_edit_max_gap_s = 15.0;
        /// Soft-LLI bridge (effective only with @c turbo_edit + a non-zero
        /// @c turbo_edit_max_gap_s, high-rate only): when a receiver-declared
        /// loss of lock (LLI) on an AR-capable dual-frequency satellite would
        /// otherwise trigger an immediate full ambiguity reset, instead HOLD
        /// the satellite's phase ambiguities across the (short) LLI gap — keep
        /// the GF arc, snapshot the pre-gap values, exclude the held phase
        /// measurements from the KF update across the MW one-epoch confirmation
        /// delay — and let the gap bridge apply a covariance-preserving integer
        /// REPAIR at gap return (@c snapshot + dN·λ) rather than discarding the
        /// converged ambiguity precision and AR continuity.  This bets that the
        /// carrier loop held phase coherence across the LLI; if it instead
        /// re-acquired (arbitrary integer), the KF phase-innovation gate resets
        /// the wrong held-repair one epoch late — the same cost as the
        /// re-acquisition reset path.  Beyond @c turbo_edit_max_gap_s the
        /// classic full reset stands.  Default OFF (baseline unchanged).  Via
        /// @c estimation.cycle_slip.turbo_edit_bridge_lli.  See
        /// docs/turbo_edit_lli_bridge_design.md.
        bool turbo_edit_bridge_lli = false;
        /// WL-settle deferral cap (s) for the gap-bridge joint solve
        /// (effective only with @c turbo_edit + @c turbo_edit_bridge_lli +
        /// a non-zero @c turbo_edit_max_gap_s): when the across-gap GF jump
        /// is solid but the MW wide-lane evidence is float-only and
        /// AMBIGUOUS (two or more integer candidates fit the GF lattice —
        /// candidates k and k±2 differ by a null-line pair only a few mm
        /// apart in GF), the adjudication is DEFERRED instead of guessed:
        /// the soft-LLI holds stay in place (measurements excluded, states
        /// prune-protected), the pre-gap GF level stays stashed, and each
        /// subsequent epoch the post-restart MW arc's Hatch mean
        /// re-estimates the across-gap WL step with √n-averaged code noise
        /// (the instantaneous float at re-acquisition is NL-code-biased by
        /// up to several cycles).  The joint solve re-runs once that
        /// estimate is confidently roundable (the WL-lock criterion:
        /// |frac| + 3·σ_mean < 0.5 cycles, n ≥ 10); at this cap with no
        /// settle — or if the post-gap GF chain flags a new event — the
        /// satellite falls back to the conservative reset.  0 disables
        /// deferral (an ambiguous bridged solve resets immediately).  Via
        /// @c estimation.cycle_slip.turbo_edit_wl_settle_max_s.
        double turbo_edit_wl_settle_max_s = 45.0;
        /// Split-capability gate (effective only with @c turbo_edit): the
        /// maximum 1σ uncertainty, in CYCLES, of the float per-signal integer
        /// the TurboEdit joint solve rounds (σ_TD2 / |λ1−λ2|).  This is the
        /// lever that makes the resolver rate-adaptive: the per-signal split
        /// (and the bridged across-gap attribution) emits integers only when
        /// the arc's measured GF noise satisfies
        /// σ_TD2 ≤ this × |λ1−λ2| — at the 0.2 default a 3.5σ GF error still
        /// lands inside the rounding well of the true integer (the
        /// wrong-integer acceptance distance is 1 − frac_max = 0.7 cycles).
        /// Millimetric 1 s phase noise passes trivially (historical high-rate
        /// behavior unchanged); at 30 s the un-cancelled iono residual passes
        /// only while quiet, demoting MW-flagged satellites to the
        /// conservative reset otherwise.  Raising this admits repairs from
        /// noisier GF evidence (more repairs, more wrong-integer risk);
        /// lowering it tightens toward reset-always.  Via
        /// @c estimation.cycle_slip.turbo_edit_split_max_sigma.
        double turbo_edit_split_max_sigma = 0.2;

        // --- Per-detector MAD threshold scale factors (kσ gate multipliers) ---
        // Each adaptive slip threshold is `multiplier × max(σ_MAD, floor)`,
        // where σ_MAD is the robust (MAD×1.4826) noise estimate.  Raising the
        // multiplier widens the gate (fewer detections, fewer false resets, but
        // smaller real slips can slip through); lowering it tightens detection.
        // Defaults preserve the historical hardcoded values; each must be > 0.
        // Configured via @c estimation.cycle_slip.<name>_sigma_multiplier.
        double mw_sigma_multiplier         = 8.0;  ///< MW (Melbourne-Wübbena) wide-lane gate.
        double turbo_edit_sigma_multiplier = 6.0;  ///< TurboEdit GF-only (MW blind-spot) gate.
        double graphic_sigma_multiplier    = 6.0;  ///< GRAPHIC code+phase gate.
        double ewl_sigma_multiplier        = 5.0;  ///< Extra-wide-lane gate.

        // --- MW detection-reference selection (see MwDetectReference doc) ---
        /// Reference the MW slip deviation is measured against.
        /// "hatch_mean" (legacy) | "local_median" (trend-immune level test) |
        /// "time_difference" (trend- AND curvature-immune jump test).  Via
        /// @c estimation.cycle_slip.mw_detect_reference.
        MwDetectReference mw_detect_reference = MwDetectReference::HatchMean;
        /// Differencing order for the "time_difference" reference:
        /// "first" (default) | "second".  Ignored in the other modes.  Via
        /// @c estimation.cycle_slip.mw_td_order.
        MwTdOrder mw_td_order = MwTdOrder::First;
        /// Length (epochs) of the causal local-median window used when
        /// @c mw_detect_reference is @c LocalMedian.  Must be ≥ 15 (the
        /// detector's MAD-maturity floor, MwSlipDetector::kMinEpochsForAdaptive)
        /// and ≤ 64 (the fixed per-arc ring capacity); the loader clamps with
        /// a warning.  Via @c estimation.cycle_slip.mw_local_window.
        int mw_local_window = 20;
    };

    /**
     * @brief Configuration for the adaptive ionospheric process-noise scheme.
     *
     * In PPP-RTK uncombined mode the per-satellite slant ionospheric state
     * is driven by a single static process-noise spectral density
     * (@c ProcessNoise::ionosphere).  When @c enabled, @c IonoVariabilityMonitor
     * scales that baseline per satellite, per epoch, from a direct measure
     * of ionospheric disturbance: the running variability of the
     * geometry-free (L1-L2) carrier-phase second time-difference, a
     * TEC-acceleration proxy.
     *
     * The per-satellite multiplier is the squared ratio of a recent-window
     * MAD-sigma to a baseline-window MAD-sigma of that satellite's TD2
     * series, clamped to [1, max_multiplier].  Using the ratio
     * self-normalizes: the slowly varying geometric (obliquity-curvature)
     * component sits in both windows and cancels, so quiet conditions
     * yield a multiplier near 1 at any elevation and the existing
     * 1/sin^2(el) obliquity scaling is not double-counted.
     *
     * JSON: estimation.adaptive_iono_process_noise.{enable,recent_window,
     * baseline_window,max_multiplier,min_epochs,trace}.
     */
    /**
     * @brief Configuration for GIM-based ionosphere weighting (PPP-RTK).
     *
     * When enabled, an external IONEX global ionosphere map is applied
     * each epoch as a soft pseudo-observation on the per-satellite slant
     * ionosphere states.  A large per-constellation common-mode variance
     * leaves the absolute ionospheric level unconstrained (it carries an
     * un-modellable receiver datum bias), so only the trustworthy
     * between-satellite structure of the GIM is imposed.
     *
     * JSON: estimation.ionosphere_weighting.{enable,common_mode_sigma_m,
     * std_scale,floor_m,elevation_cutoff_deg}.
     */
    struct IonosphereWeightingConfig {
        bool   enabled = false;             ///< Master toggle.  Default off; opt in via JSON.
        double common_mode_sigma_m = 50.0;  ///< Per-constellation common-mode std (m); large => absolute level free.
        double std_scale = 1.0;             ///< Scale on the GIM RMS-derived per-row pseudo-obs std.
        double floor_m = 0.05;              ///< Lower floor on the per-row pseudo-obs std (m).
        double elevation_cutoff_deg = 10.0; ///< Satellites below this elevation are not GIM-constrained.
    };

    /**
     * @brief Co-estimated global single-layer SH-VTEC ionosphere (network producer).
     *
     * When @c network.estimate_iono_sh is on, the PPP-RTK network producer
     * co-estimates its OWN global ionosphere — a sun-fixed spherical-harmonic VTEC
     * field — jointly with the satellite code biases (W6 OSB), so it no longer
     * imports an external GIM (`.INX`).  The SH coefficients are SHARED columns in
     * @ref NetworkEstimator (SharedParamKind::VtecSh); the per-(rx,sat) slant-iono
     * states are kept and the field is an informative (Kaula-regularized) prior on
     * their between-satellite structure, reusing the GIM weighting's common-mode-
     * free R unchanged (so the receiver-DCB ↔ iono level stays free).
     *
     * JSON: estimation.global_vtec_sh.{enable, max_degree, layer_height_km,
     * kaula_scale_tecu, q_per_degree, mean_vtec_prior_tecu, mean_vtec_prior_sigma_tecu,
     * representation_sigma_tecu, sun_fixed, trace}.
     */
    struct GlobalVtecShConfig {
        bool   enabled = false;                ///< Master toggle (set by network.estimate_iono_sh).
        int    max_degree = 4;                 ///< Triangular truncation degree N → (N+1)² coeffs.
        double layer_height_km = 450.0;        ///< Single-layer shell height H (km).
        double earth_radius_km = 6371.0;       ///< Shell base radius R_e (km).
        double kaula_scale_tecu = 10.0;        ///< Kaula prior scale: σ_n = kaula_scale/n (TECU) for n≥1.
        double q_per_degree = 1.0e-2;          ///< Per-degree random-walk q (TECU²/s), scaled by 1/n² for n≥1.
        double mean_vtec_prior_tecu = 15.0;    ///< a₀₀ climatology mean (TECU) — data-determined, loosely anchored.
        double mean_vtec_prior_sigma_tecu = 30.0;  ///< a₀₀ prior σ (TECU) — LOOSE (climatology, not an IONEX value).
        double representation_sigma_tecu = 5.0;    ///< Per-LOS single-layer representativeness σ (VTEC TECU), mapped to slant.  The field is an INFORMATIVE prior on the slant states, not a tight constraint — too tight aliases unrepresentable small-scale TEC into huge low-degree coefficients at global scale.
        bool   sun_fixed = true;               ///< Expand the field in sun-fixed longitude (λ_ipp − λ_mean_sun).
        bool   trace = false;                  ///< Write the per-epoch SH coefficient trace (network_vtec_sh.csv) + the per-epoch health trace (network_vtec_sh_health.csv, T1a).
        double a00_warn_low_tecu  = 2.0;       ///< Physical-band lower bound on the estimated mean VTEC a₀₀ (TECU).  a₀₀ below this fires a per-epoch divergence WARN (T1a health probe); the field co-estimation can drive a₀₀ to unphysical values (e.g. −91 TECU) when the SH surface cannot represent the GF code-bias lump structure.
        double a00_warn_high_tecu = 150.0;     ///< Physical-band upper bound on a₀₀ (TECU); above this fires the divergence WARN.  Set low ≥ high to silence the WARN.
        int    row_decimation_epochs = 0;      ///< T1b temporal treatment of the (quasi-static) SH pseudo-obs constraint (0/1 = off = current behavior).  The sun-fixed field is quasi-static, so re-applying the SH↔slant rows as an INDEPENDENT white obs every epoch is a σ-incest channel (collapses the formal σ without new information).  Mechanism: the rows are still applied EVERY epoch (so the satellite-code-bias observability they provide is preserved and no epoch is withheld — pure row-skipping instead makes the observability gate withhold every skipped epoch) but their noise is inflated by N (σ ×√N), so each epoch's white row carries only 1/N of the information, matching the information rate of "one full look per N epochs".
        double rep_error_tau_s = 0.0;          ///< T5 (FABLE-GATE, 2026-07-10): correlated per-(station,sat) SH representativeness-error deviation.  0 = OFF = byte-identical.  When > 0, the SH↔slant pin row `ι − SH·c = 0 + ε(white)` becomes `ι − SH·c = d̂ₖ|ₖ₋₁ + ε(floor² + Pₖ|ₖ₋₁)` where d is a per-LOS Gauss–Markov deviation (τ = this value, steady-state σ = representation_sigma_tecu mapped to slant) carried by a serial scalar filter outside the solve.  The single-layer surface structurally cannot represent per-LOS slant structure (measured: ~80 % of LOS residuals > 3σ at ANY degree, T4 2026-07-10); modelling the misfit as a tracked correlated deviation stops it leaking dispersively into the float ambiguities (the uniform-DD-fraction root cause) while preserving the SH rows' DCB observability.  Supersedes row_decimation_epochs when set (the GM model IS the correlated-error treatment; the σ-inflation was its white-noise stopgap).
    };

    struct AdaptiveIonoQConfig {
        bool   enabled         = false; ///< Master toggle.  Default off; opt in via JSON.
        int    recent_window   = 20;    ///< Recent-window length (epochs) for the short-term TD2 MAD-sigma.
        int    baseline_window = 240;   ///< Baseline-window length (epochs) for the long-term TD2 MAD-sigma.
        double max_multiplier  = 100.0; ///< Upper clamp on the per-satellite process-noise multiplier (lower clamp 1.0).
        int    min_epochs      = 30;    ///< Min TD2 samples on an arc before its multiplier may deviate from 1.0.
        bool   trace           = false; ///< Write a per-satellite GF/TD2/variability CSV trace to the log directory.
    };

    /**
     * @brief Station-adaptive SH-VTEC-constraint deweight (PPP-RTK network producer).
     *
     * The per-LOS slant-iono states couple to the shared global SH-VTEC field in
     * BOTH directions through a single joint pseudo-observation per (station, LOS):
     * (a) the station's LOS feed the field fit; (b) the field constrains the
     * station's per-LOS iono.  A station whose TRUE slant ionosphere is beyond the
     * single-layer field's representability (e.g. an equatorial-anomaly /
     * scintillation-belt site at solar max) injects that misfit through (a) into
     * every other station (the global support functions export it) and through (b)
     * into its own ambiguities → the shared clocks / code / phase biases.  This
     * config relaxes that coupling per station by a scalar factor @c w_s ∈ (0,1]
     * that multiplies the INFORMATION of the SH iono rows (σ² ×= 1/w_s ⇒ information
     * ×= w_s).  @c w_s→0 makes the station's per-LOS iono float freely on its own
     * GF code/phase (noisier, still observable — no rank hole; the dual-freq code
     * bias stays lumped into iono per Odijk 2016) while the station KEEPS serving
     * the shared clocks, biases, datum graph, and position.  It is the surgical
     * alternative to dropping the station (the amputation falsifier) — geometry and
     * the datum survive.
     *
     * The single joint pseudo-obs means ONE w_s uniformly deweights both directions
     * and their cross-coupling.  Two paths:
     *  - v1 (static): every station in @c stations is deweighted by @c factor.
     *  - v2 (adaptive, @c adaptive): an IGG-style trailing gate engages a station
     *    when its smoothed per-station SH-misfit statistic becomes a robust outlier
     *    against the network's own distribution (median/MAD-standardized z-score),
     *    with hysteresis (@c trigger_sigma / @c release_sigma) and a per-epoch rate
     *    limit so the deweight⇄fit feedback loop cannot oscillate.  The statistic is
     *    RELATIVE to the network robust center on purpose: the single-layer surface
     *    leaves ~70–90 % of LOS residuals > 3σ at ANY degree (a chronic, common-mode
     *    background), so an ABSOLUTE 3σ gate would trigger every station — only the
     *    network-relative excess isolates the genuinely hostile sites.
     *
     * JSON (all under @c network.iono_constraint_deweight_*): @c stations, @c factor,
     * @c adaptive, @c window_epochs, @c sigma_hi, @c sigma_lo, @c floor,
     * @c rate_limit, @c min_epochs.  Empty @c stations + @c adaptive=false ⇒ every
     * w_s is 1.0 ⇒ byte-identical to OFF (invariant #6).
     */
    struct IonoConstraintDeweightConfig {
        std::vector<std::string> stations;              ///< v1: station long-names whose SH iono coupling is deweighted by @c factor (explicit list always wins over the adaptive gate).
        double factor = 0.0;                            ///< v1 deweight strength w_s for listed stations.  0 = free iono (full decouple; the station's per-LOS iono floats on its own GF code/phase); clamped to (0,1] otherwise.  Default 0.
        bool   adaptive = false;                        ///< v2: enable the IGG-style network-relative auto-trigger (in addition to the static list).  Default OFF.
        double window_epochs = 60.0;                    ///< v2: half-life (epochs) of the per-station SH-misfit EWMA (forgetting-window robust-stat idiom, cf. NetworkCarryVce).
        double sigma_hi = 3.0;                          ///< v2: engage the deweight when a station's smoothed misfit z-score (median/MAD-standardized across the network) rises above this.
        double sigma_lo = 2.0;                          ///< v2: release when the z-score falls back below this (hysteresis band; must be < @c sigma_hi).
        double floor = 0.0;                             ///< v2: minimum w_s the ramp drives toward at/above @c sigma_hi (0 = full decouple at strong outlier).  Clamped to [0,1).
        double rate_limit = 0.25;                       ///< v2: maximum |Δw_s| per clean epoch (anti-oscillation slew limit).
        int    min_epochs = 30;                         ///< v2: minimum clean EWMA samples on a station before its gate may fire.
    };

    enum class StochasticModelType {
        Constant,   ///< Use the a-priori ?� directly, no geometry scaling.
        Elevation,      ///< Classic 1/sin�(el) weighting (default).
        SNR,            ///< SNR-based weighting using the SIGMA-? model.
        SatelliteBlock  ///< Block/frequency-dependent zenith noise with 1/sin�(el) scaling.
    };

    /**
     * @brief Struct representing receiver antenna orientation.
     *
     * Contains azimuth and elevation angles for antenna orientation.
     */
    struct ReceiverAntennaOrientation {
        double azimuth_deg{ 0.0 };   ///< Antenna azimuth orientation in degrees.
        double elevation_deg{ 90.0 }; ///< Antenna elevation orientation in degrees.
    };

    /**
     * @brief Struct for process noise spectral densities (variance rates).
     *
     * Each field is a spectral density in m*m/s. The Kalman filter multiplies
     * by dt (seconds) to obtain the variance (m*m) added per prediction step:
     *   variance_per_epoch = q * dt
     * Exception: @c acceleration_psd is an acceleration PSD (m*m/s^3) and
     * expands to the coupled CWNA block, not a simple q*dt term.
     */
    struct ProcessNoise {
        /// Position (m*m/s). Zero for static positioning; use ~100 for kinematic.
        double position{ 0.0 };

        /// Receiver clock (m*m/s). 1e8 models the clock as white noise,
        /// making it effectively unconstrained each epoch. This is standard
        /// PPP practice for GNSS receivers without atomic clocks.
        /// At 30 s: sqrt(1e8 * 30) ~ 54 km, i.e. no constraint.
        /// However, clock is initialized by WLS at each epoch.
        double clock{ 1e4 };

        /// Troposphere ZWD (m*m/s). 1e-8 yields ~0.5 mm/sqrt(hr),
        /// consistent with mid-latitude IGS analysis center defaults
        /// (Boehm et al.). Allows the ZWD to track weather-driven changes
        /// over multi-hour arcs without over-constraining the estimate.
        double troposphere{ 1e-8 };

        /// Ionosphere zenith delay (m*m/s). Only active in PPP-RTK
        /// uncombined mode. Scaled to slant by 1/sin�(el) per satellite.
        /// 0.02 is within the typical 0.01-0.1 range for the zenith value.
        /// This is the random-walk driving noise; when @c enable_iono_rate is
        /// on the per-sat iono becomes a constant-velocity [I,Idot] block and
        /// this field is superseded by @c iono_rate_psd (see below).
        double ionosphere{ 0.10 };

        /// Per-satellite slant-iono RATE acceleration PSD (m*m/s^3).  Active
        /// only when @c enable_iono_rate is on: the per-sat slant iono is
        /// modelled as a constant-velocity pair [I_s, Idot_s] with transition
        /// [[1,dt],[0,1]] and the coupled CWNA (continuous white-noise
        /// acceleration) block Q[I]=q*dt^3/3, Q[I,Idot]=q*dt^2/2, Q[Idot]=q*dt
        /// — exactly the receiver clock-bias/clock-rate template (@c clock_rate).
        /// The rate carries the largely-deterministic slant-iono ramp (obliquity
        /// change + pierce-point motion + diurnal VTEC trend), leaving only the
        /// turbulent 2nd-order part as process noise, so the iono prior tightens
        /// ~1000x vs the random walk (stronger AR/position + in-band iono-fault
        /// detection).  Obliquity-scaled to slant by 1/sin^2(el) like the level
        /// state.  Default 1e-10 was fit from real 24 h GPS+Galileo arc data
        /// (the zenith accel PSD reproducing the observed second-difference
        /// scatter of 2 mm at zenith to 8 mm at 12 deg elevation).  Orthogonal
        /// to @c adaptive_iono_process_noise, which inflates this PSD in storms.
        double iono_rate_psd{ 1e-10 };

        /// Carrier phase ambiguity (m*m/s).  Default 1e-12 is a "safety
        /// floor" — sub-mm accumulation per day, indistinguishable from
        /// the constant-ambiguity model in physical terms, but enough
        /// to keep the per-(sat, signal) ambiguity covariance from
        /// underflowing toward zero on long arcs.  Variance shrinks via
        /// every measurement update; without a floor, after hours of
        /// clean tracking the joint covariance Cholesky can lose
        /// positive-definiteness and trigger numerical errors.  Set to
        /// 0.0 to recover the strict constant-ambiguity model when
        /// you're sure the joint covariance can absorb the conditioning.
        double ambiguity{ 1e-12 };

        /// Receiver code bias / inter-frequency bias (m*m/s). In uncombined
        /// PPP-RTK, one code bias per non-reference signal absorbs the
        /// receiver hardware delay difference. Very stable short-term;
        /// 1e-4 allows ~3 mm/sqrt(30 s) drift.
        double code_bias{ 1e-4 };

        /// Inter-system time bias (m*m/s). ISBs are very stable short-term;
        /// 1e-7 allows ~0.17 mm/sqrt(30 s) drift, sufficient for
        /// GPS-Galileo/GLONASS/BeiDou time offset evolution.
        double system_time_offset{ 1e-7 };

        /// Troposphere horizontal gradient (m*m/s). North and East gradients
        /// use the same spectral density. 5e-12 yields ~0.01 mm/sqrt(hr),
        /// appropriate for slow azimuthal asymmetry evolution.
        double tropo_gradient{ 5e-12 };

        /// Phase clock offset delta (m*m/s). In the "common clock + delta"
        /// parameterization the delta captures the receiver code-phase hardware
        /// bias difference, which is physically stable (nearly constant over
        /// hours). A very small random-walk spectral density keeps the delta
        /// tightly constrained so the common clock absorbs epoch-to-epoch
        /// timing variations while ambiguities remain estimable.
        /// 1e-9 yields ~1 mm/sqrt(hr) drift.
        double phase_clock{ 1e-9 };

        /// White-noise acceleration power spectral density (m*m/s^3).  The
        /// single CWNA (continuous white-noise acceleration) parameter: when
        /// velocity is estimated it drives the coupled position+velocity
        /// process-noise block — Q[pos]=q*dt^3/3, Q[pos,vel]=q*dt^2/2,
        /// Q[vel]=q*dt — so position and velocity noise derive from one
        /// physical quantity with the correct cross-covariance.  Unlike the
        /// other fields it is not a simple q*dt term.  ~1e0-1e2 for
        /// pedestrian/vehicular dynamics, near 0 for a static receiver.
        /// Config key: acceleration_psd_q (legacy alias: velocity_q).
        double acceleration_psd{ 1e2 };

        /// Per-satellite signal bias (m*m/s).  Absorbs uncorrected satellite
        /// hardware delays on signals without BIA product coverage.  Nearly
        /// constant over hours; 1e-10 allows ~0.01 mm/sqrt(30 s) drift.
        double sat_bias{ 1e-10 };

        /// Per-satellite signal CODE bias (m*m/s).  Network producer
        /// (PPP_RTK_Network) only — the estimable satellite code-bias (OSB) state
        /// when @c network.estimate_sat_code_bias is on.  Satellite code hardware
        /// delays are very stable (geodetic-grade DCBs drift ~mm/day), so the
        /// default mirrors @c sat_bias (1e-10, ~0.01 mm/sqrt(30 s)).  Separate
        /// from @c sat_bias so the network can tune the shared OSB carry-forward
        /// independently of the single-receiver fallback bias.  Config key:
        /// @c sat_code_bias_q.
        double sat_code_bias{ 1e-10 };

        /// Per-satellite signal PHASE bias (m*m/s).  Network producer
        /// (PPP_RTK_Network) only — the estimable satellite phase-bias state.
        /// Phase hardware delays are even more stable than code, so the default
        /// is two orders below @c sat_bias (~0.001 mm/sqrt(30 s)).  Strictly
        /// constant (q=0) would over-converge the diagonal P and leave the
        /// satellite column deaf to genuine slow drift, so a tiny RW is used.
        double sat_phase_bias{ 1e-12 };

        /// Per-satellite clock random-walk spectral density (m*m/s) for the
        /// PPP_RTK_Network producer's Δδtˢ states.  A loose default — satellite
        /// clocks drift far faster than biases; the carried-forward prior plus
        /// this RW gives epoch-to-epoch continuity without over-constraining.
        double sat_clock{ 1e-2 };

        /// Receiver clock-rate spectral density (m*m/s^3).  Under the CWNA
        /// model on the (clock-bias, clock-rate) pair, clock-bias picks up
        /// q*dt^3/3, clock-rate picks up q*dt, with q*dt^2/2 cross-covariance.
        /// 1e-4 is appropriate for a typical TCXO; reduce by a few orders of
        /// magnitude for OCXO-grade receivers.  Active only when
        /// @c estimate_velocity is on (Doppler processing).
        double clock_rate{ 1e-4 };

        /// Inter-system clock-rate offset spectral density (m*m/s^3).  The
        /// rate counterpart of @c system_time_offset.  Single shared
        /// oscillator drives all bands, so ISBR is physically near zero —
        /// the tight default reflects this.  Active only when
        /// @c estimate_velocity is on.
        double system_time_rate_offset{ 1e-10 };
    };

    /**
     * @brief Struct for initial state standard deviations.
     *
     * Specifies initial uncertainties for state variables.
     */
    struct InitialStd {
        double position{ 1e2 };       ///< Initial position standard deviation (m).
        double velocity{ 1e2 };       ///< Initial velocity standard deviation (m/s).
        double clock{ 1e3 };          ///< Initial clock bias standard deviation (m).
        double troposphere{ 0.5 };      ///< Initial tropospheric zenith delay standard deviation (m).
        double ionosphere{ 5.0 };       ///< Initial ionospheric zenith delay standard deviation (m).
        double ionosphere_rate{ 0.1 };  ///< Initial slant-iono RATE standard deviation (m/s) — cold-start Idot_s uncertainty when @c enable_iono_rate is on. Generous; quickly informed by the F-coupling into I_s.
        double ambiguity{ 1e3 };      ///< Initial ambiguity standard deviation (m).
        double code_bias{ 10.0 };         ///< Initial receiver code bias (IFB) standard deviation (m).
        double system_time_offset{ 1e2 }; ///< Initial inter-system time bias standard deviation (m).
        double tropo_gradient{ 1e-3 };     ///< Initial tropo gradient standard deviation (m).
        double phase_clock{ 1e3 };          ///< Initial phase clock offset standard deviation (m).
        double sat_bias{ 10.0 };              ///< Initial per-satellite signal bias standard deviation (m).
        double sat_phase_bias{ 10.0 };        ///< Initial per-satellite signal PHASE bias standard deviation (m) — PPP_RTK_Network only.
        double sat_clock{ 1000.0 };           ///< Initial per-satellite clock standard deviation (m) — PPP_RTK_Network only (loose; Δδtˢ vs a-priori product clock).
        double clock_rate{ 1.0 };             ///< Initial receiver clock-rate standard deviation (m/s).
        double system_time_rate_offset{ 1e-2 }; ///< Initial inter-system clock-rate offset standard deviation (m/s).
    };

    /**
     * @brief Struct for default measurement noise parameters.
     *
     * Provides fallback noise values when RINEX does not specify variances.
     */
    struct DefaultMeasurementNoise {
        double code_std_m{ 0.30 };   ///< Code measurement standard deviation (m).
        double phase_std_m{ 0.002 }; ///< Phase measurement standard deviation (m).
        double doppler_std_m{ 0.5 }; ///< Doppler measurement standard deviation (Hz).
    };

    /**
     * @brief Parameters for the SNR-based stochastic model.
     *
     * The model computes measurement variance using the
     * variance-additive form (Hadas & Bosy 2015, building on the
     * Hartinger & Brunner 1999 SIGMA-ε framework):
     *
     *   σ² = a² + b² · 10^(-CN0_dBHz / 10)
     *
     * with @c a the high-SNR noise floor (the asymptote at
     * CN0 → ∞) and @c b the SNR-sensitivity coefficient.  Both have
     * units of metres.  Variance-additive is preferred over the
     * legacy σ-additive form (σ = a + b · 10^(-SNR/10)) because
     * independent noise contributions add in *variance*, not in
     * standard deviation; squaring the σ-additive form generates an
     * unphysical 2ab cross-term.
     *
     * On top of the variance, the model applies:
     *  - Per-band chip-rate scaling for *code* observations
     *    (reuses @c SatBlockNoiseParams::freq_band_scale).  Phase
     *    observations get no per-band scaling — chip rate doesn't
     *    enter the carrier-loop noise formula.
     *  - Wieser 2002 hybrid elevation amplification: the SNR-derived
     *    σ² is multiplied by 1/sin²(el) (gated by
     *    @c phase_elevation_weighting for phase rows).  This captures
     *    multipath-driven noise that pure SNR weighting misses.
     *
     * When the receiver-reported S-observable is missing for a given
     * row, the model synthesizes a CN0 from elevation using a typical
     * antenna gain pattern (CN0_zenith ≈ 50 dBHz, drop ≈ 15 dB to
     * horizon) rather than discontinuously switching to a different
     * functional form.
     *
     * Reference values (variance-additive form):
     *   code  : a ≈ 0.10–0.30 m,    b ≈ 30–100 m
     *   phase : a ≈ 0.001–0.003 m,  b ≈ 0.3–1.0 m
     *
     * References:
     *  - Hartinger H., Brunner F.K. (1999),
     *    "Variances of GPS phase observations: the SIGMA-ε model",
     *    GPS Solutions.
     *  - Wieser A. (2002),
     *    "Robust and fuzzy techniques for parameter estimation and
     *    quality assessment in GPS", Engineering Geodesy / TU Graz.
     *  - Hadas T., Bosy J. (2015),
     *    "IGS RTS precise orbits and clocks verification and accuracy
     *    of real time PPP", GPS Solutions 19(1).
     */
    struct SnrModelParams {
        double code_a{ 0.30 };        ///< High-SNR code σ floor (m).  σ → a as CN0 → ∞.
        double code_b{ 50.0 };        ///< Code SNR-sensitivity coefficient (m).
        double phase_a{ 0.001 };      ///< High-SNR phase σ floor (m).
        double phase_b{ 0.5 };        ///< Phase SNR-sensitivity coefficient (m).
        double snr_floor_dbhz{ 20.0 };///< CN0 floor; observations below this are clamped (or rejected upstream).
        double snr_ceil_dbhz{ 55.0 }; ///< CN0 ceiling; observations above this are clamped to avoid σ → a noise floor exactly.
    };

    /**
     * @brief Parameters for the satellite-block-dependent stochastic model.
     *
     * Assigns zenith-direction code and phase noise based on the satellite
     * block type extracted from the ATX antenna model (e.g. "BLOCK IIF",
     * "BLOCK IIIA", "GALILEO-2").  A per-frequency-band scale factor is
     * applied on top.  The result is then scaled by 1/sin�(el) like the
     * standard Elevation model.
     *
     * Block types are matched by substring (case-insensitive) so that
     * "BLOCKIIIA" matches an ATX type of "G04_BLOCKIIIA_G074".
     */
    struct SatBlockNoiseParams {
        /// Per-block scale factor (unitless).  Multiplied at config-
        /// load time by @c measurement_noise.code_std_m and
        /// @c measurement_noise.phase_std_m to populate the absolute
        /// @c block_code_std_m / @c block_phase_std_m tables below.
        ///
        /// The ratios encode the relative hardware-noise hierarchy:
        /// legacy blocks (Block II/IIR, GLONASS-M legacy, BeiDou-2) sit
        /// at 1.0× the configured baseline; modern hardware (Block III,
        /// Galileo FOC, BeiDou-3) gets ~0.85.  Intermediate blocks fall
        /// in between.  The spread is driven by satellite EIRP and
        /// signal-generation quality, which move received C/N0 by only
        /// ~1-3 dB block-to-block; since code σ ∝ 1/√(C/N0) that is a
        /// ~6-15% noise effect, so the block axis is deliberately the
        /// *narrowest* of the four (block × band × attribute × baseline).
        /// The user tunes the *baseline* σ once via
        /// @c measurement_noise.{code,phase}_std_m and the per-block
        /// proportions multiply it (consistently between code and phase).
        ///
        /// Overridable from JSON under
        /// @c filter.satellite_block.block_scale_factors as an ordered
        /// array of [substring, scale] pairs; omitting the JSON key
        /// keeps these defaults.
        ///
        /// Keys are substrings matched against the ATX type field;
        /// first hit wins.  Ordering matters — specific blocks must
        /// precede their parents (BLOCKIIIA before BLOCKIII before
        /// BLOCKII; BEIDOU-3 before BEIDOU; GLONASS-M+ before
        /// GLONASS-K before GLONASS).
        /// Defaults tuned for high-quality IGS-grade receivers
        /// (Septentrio PolaRx5/mosaic, Trimble Alloy/NetR9, Javad Delta,
        /// Leica GR50, etc).  The range is compressed to ~1.18×
        /// (best/worst), reflecting the empirical reality that on such a
        /// receiver the self-noise floor swamps block-to-block payload
        /// differences — chip-rate and signal-design effects (per-band,
        /// per-attribute) dominate over which satellite block
        /// transmitted them.  The earlier 3× spread was extrapolated
        /// from legacy / early-2000s receivers and no longer reflects
        /// production hardware.
        std::vector<std::pair<std::string, double>> block_scale_factors{
            {"BLOCKIIIA",  0.85},  ///< GPS III/IIIA — modern digital payload
            {"BLOCKIIIF",  0.85},  ///< GPS IIIF — same generation as IIIA
            {"BLOCKIII",   0.85},  ///< Fallback for unqualified BLOCK III
            {"BLOCKIIF",   0.90},  ///< GPS IIF — modernized but pre-III
            {"BLOCKIIR-M", 0.95},  ///< IIR-M — modernized civil L2C/M-code
            {"BLOCKIIR",   1.00},  ///< IIR — older but capable
            {"BLOCKII",    1.00},  ///< Legacy reference (mostly retired)
            {"GALILEO",    0.85},  ///< Modern signal design (BOC, pilots); IOV lumped in
            {"BEIDOU-3",   0.85},  ///< BDS-3 — modern, BOC signals
            {"BEIDOU",     1.00},  ///< BDS-2 — legacy, multipath-prone
            {"GLONASS-K",  0.90},  ///< CDMA-capable, modern
            {"GLONASS-M+", 1.00},  ///< Mid-tier GLONASS-M variants
            {"GLONASS",    1.00},  ///< Legacy FDMA reference
            {"QZSS-2",     0.85},  ///< QZSS Block II — modern signals
            {"QZSS",       0.90}   ///< QZSS fallback
        };

        /// Zenith code noise per block type (meters).  **Derived** at
        /// config-load time as @c block_scale_factors × @c
        /// measurement_noise.code_std_m.  Direct edits here are
        /// silently overwritten when the loader runs; this field
        /// exists so that the lookup-time consumer
        /// (@c StochasticModel) can keep its existing two-table
        /// interface without needing live access to the
        /// measurement-noise baseline.
        std::vector<std::pair<std::string, double>> block_code_std_m;

        /// Zenith phase noise per block type (meters).  **Derived** as
        /// @c block_scale_factors × @c measurement_noise.phase_std_m,
        /// preserving the same per-block ratio structure as the code
        /// table.  This lets the per-block phase differentiation
        /// follow the user's chosen baseline phase σ automatically
        /// rather than living in a parallel hand-tuned table.
        std::vector<std::pair<std::string, double>> block_phase_std_m;

        /// Per-observable (code / phase) scale tables.
        ///
        /// Wraps the per-band and per-(system, attribute) scale factors
        /// for a single observable type.  @c SatBlockNoiseParams holds
        /// two instances: @c code (chip-rate-driven defaults — narrow
        /// correlator C/A penalty, high-chip-rate band gains) and
        /// @c phase (carrier-tracking-driven defaults — no chip-rate
        /// effects, gentler per-band gradient, attribute differences
        /// largely flat).  Splitting the axis lets each observable use
        /// physically-motivated defaults instead of forcing one table
        /// to compromise between the two regimes — the chip-rate-
        /// driven 1.8× C/A penalty on GPS @c C, for example, has no
        /// analogue on phase (the carrier is the same physical signal
        /// regardless of the code attached), so phase keeps GPS @c C
        /// at 1.0.
        ///
        /// Lookup for @c tracking_attr_scale is: (1) @c [system][attr]
        /// → (2) @c ["default"][attr] → (3) 1.0.  System name matches
        /// @c PppUtils::getSystemString (e.g. "GPS", "Galileo").
        /// @c freq_band_scale is keyed by band digit (RINEX signal code
        /// second character) with 1.0 as the implicit fallback.
        struct ObservableScale {
            /// Per-band scale factor keyed by RINEX band digit.
            std::map<char, double> freq_band_scale;
            /// Per-system, per-attribute scale factor.  Outer key is
            /// system name (or "default"); inner key is the RINEX
            /// attribute character.
            std::map<std::string, std::map<char, double>> tracking_attr_scale;
        };

        /// Code-observable scale tables.  Chip-rate-driven calibration.
        ///
        /// @c freq_band_scale: anchored at band '1' = 1.0 (L1 C/A and
        /// P(Y) share this slot, with the C/A penalty captured under
        /// the attribute axis).  Band '5' family (GPS L5, Galileo
        /// E5a, BeiDou B2a) and '7' (E5b / B2b) get meaningful chip-
        /// rate gain; '8' (Galileo E5 alt-BOC wideband) is the cleanest
        /// code measurement available.
        ///
        /// @c tracking_attr_scale: the attribute axis encodes the
        /// *tracking-method* penalty/bonus relative to clean full-code
        /// BPSK of the same band.  GPS @c C/A carries a small narrow-
        /// correlator penalty (~1.15 on a high-quality receiver).  The
        /// big penalty is semi-codeless P(Y) (@c W = 1.40): cross-
        /// correlation / Z-tracking has a real SNR loss, so on a good
        /// receiver L1/L2 P(Y) code is actually *noisier* than C/A —
        /// the band axis gives band 2 no chip-rate bonus (L2C is
        /// BPSK(1)), so the W penalty is what makes C2W > C1C.  GLONASS
        /// FDMA @c C/A (@c C = 1.50) is the noisiest code in GNSS — its
        /// 0.511 Mcps half-chip-rate can't be seen on the GPS-anchored
        /// band axis, so it lives on the (system-specific) attribute.
        /// Combined I+Q channels (@c X = 0.85) are cleaner — coherent
        /// 3 dB carrier-power gain.  Pilot-only channels (@c Q, @c L,
        /// Galileo @c C, BeiDou B1C @c P) are data-free; data-only
        /// channels (@c I, @c S, @c B, @c D) carry a light data-
        /// modulation tracking penalty.
        ObservableScale code{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference, BPSK(1)-class)
                {'2', 1.0},   ///< L2 — L2C is BPSK(1) ≈ band 1; P(Y) noise handled by 'W' attr
                {'5', 0.4},   ///< L5 / E5a / B2a — BPSK(10), ~3.2× sharper thermal
                {'6', 0.5},   ///< E6 BPSK(5) / B3I — intermediate chip rate
                {'7', 0.4},   ///< E5b / B2b — BPSK(10), like band 5
                {'8', 0.25},  ///< E5 alt-BOC(15,10) wideband — cleanest code available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                {"default", {
                    {'P', 1.0},   ///< P-code reference (also: B1C pilot — see BeiDou override)
                    {'W', 1.40},  ///< Semi-codeless P(Y) — cross-correlation/Z-tracking SNR loss
                    {'Y', 1.0},   ///< Explicit Y-code
                    {'M', 1.0},   ///< Military M-code
                    {'L', 0.9},   ///< L2C(L) pilot — data-free
                    {'S', 1.05},  ///< L2C(M+L) data/mixed — light data-modulation penalty
                    {'X', 0.85},  ///< Combined I+Q — coherently 3 dB stronger
                    {'I', 1.05},  ///< Data-only — light data-modulation penalty
                    {'Q', 0.9},   ///< Pilot-only — data-free
                }},
                // ---- GPS ----
                {"GPS", {
                    {'C', 1.15},  ///< C/A (narrow correlator) — small penalty vs P(Y)
                    // 'W' (semi-codeless P(Y)) inherits default 'W' = 1.40.
                }},
                // ---- Galileo ----
                // 'C' is the E1C pilot, NOT the noisy GPS C/A.  E5a/E5b
                // pilots are 'Q'; data channels 'I'; combined 'X'.
                {"Galileo", {
                    {'A', 1.0},   ///< E1A PRS (placeholder; usually masked)
                    {'B', 1.05},  ///< E1B OS data (CBOC) — light data penalty
                    {'C', 0.85},  ///< E1C pilot — data-free
                    {'X', 0.85},  ///< E1 B+C / E5 I+Q (incl. alt-BOC) — coherent gain
                    {'Q', 0.9},   ///< E5a/E5b pilot-only
                    {'I', 1.05},  ///< E5a/E5b data-only
                    {'Z', 0.9},   ///< E6 B+C combined
                }},
                // ---- BeiDou ----
                // 'P' is the B1C pilot, not the GPS P-code.  'I' is the
                // legacy B1I/B2I/B3I data channel.
                {"BeiDou", {
                    {'I', 1.05},  ///< B1I / B2I / B3I data-only (legacy BPSK)
                    {'Q', 0.9},   ///< B2a / B2b / B3 pilot-only
                    {'X', 0.85},  ///< Combined I+Q — coherent gain
                    {'D', 1.05},  ///< B1C data
                    {'P', 0.85},  ///< B1C pilot (overrides default P-code reference)
                }},
                // ---- GLONASS ----
                // FDMA legacy signals: 'C' is the 0.511 Mcps C/A (lowest
                // chip rate in GNSS), 'P' the 5.11 Mcps P-code.  The band
                // axis is GPS-anchored and can't see GLONASS's chip rate,
                // so the chip-rate effect is carried here.  CDMA L3OC
                // (band 3, I/Q/X) falls through to the 'default' table.
                {"GLONASS", {
                    {'C', 1.50},  ///< L1OF/L2OF FDMA C/A — 0.511 Mcps
                    {'P', 0.70},  ///< L1SF/L2SF FDMA P — 5.11 Mcps
                }},
            }
        };

        /// Phase-observable scale tables.  Carrier-tracking-driven.
        ///
        /// Conservative defaults: the phase noise hierarchy on real
        /// data tracks code in *direction* (high-chip-rate bands
        /// cleaner, pilots slightly cleaner) but with *smaller spread*
        /// (chip rate doesn't enter the PLL noise formula at all; the
        /// observed phase-band differences are signal-strength and
        /// receiver-tracking artefacts, not 10× chip-rate effects).
        /// These are "effective σ in metres" ratios, so they already
        /// fold in the wavelength scaling (a fixed cycle-noise maps to
        /// more metres on a longer-λ carrier) together with C/N0 and
        /// processing gain.  Measured ratios on a recent IGS day show
        /// band 7 ≈ 0.35 and band 8 ≈ 0.33 of band 1; these defaults
        /// (0.45 / 0.40) sit just above the measured floor, leaving a
        /// little headroom for noisier receivers and stronger
        /// ionospheric conditions.  Users with calibrated data can
        /// tighten further via JSON override.
        ///
        /// @c freq_band_scale: gentler gradient than code.  Band 2 is
        /// only modestly below band 1 — the longer L2 wavelength works
        /// *against* a metre-noise reduction, and semi-codeless P(Y)
        /// degrades the carrier too (handled on the 'W' attribute), so
        /// the old aggressive 0.6 was too optimistic; 0.85 is closer to
        /// observed L2 phase.  Bands 5/7/8 are genuinely cleaner
        /// (stronger, wider modern signals overcome the wavelength
        /// penalty).
        ///
        /// @c tracking_attr_scale: flatter than code by system — the
        /// chip-rate-driven C/A penalty doesn't apply to phase, so GPS
        /// @c C is 1.0 (not 1.15 like code).  The one non-trivial entry
        /// is @c W = 1.10: semi-codeless tracking degrades the recovered
        /// carrier as well as the code, just far less than the 1.40 code
        /// penalty.  Pilot-vs-data differences are present but small
        /// (5-10%, not 20-30%) — phase tracks the carrier regardless of
        /// data wipe-off success.
        ObservableScale phase{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference)
                {'2', 0.85},  ///< L2 — modest gain; longer λ + semi-codeless offset the bonus
                {'5', 0.55},  ///< L5 / E5a / B2a — modern modulation, stronger signal
                {'6', 0.70},  ///< E6 / L6 / B3 — mid-tier
                {'7', 0.45},  ///< E5b / B2b — cleaner than band 5 on Galileo FOC
                {'8', 0.40},  ///< E5 alt-BOC wideband — cleanest phase available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                // Smaller deviations than code: phase carrier-loop noise
                // is less sensitive to the data-modulation distinction
                // than code correlator noise.
                {"default", {
                    {'P', 1.0},  ///< P-code reference
                    {'W', 1.10}, ///< Semi-codeless P(Y) — carrier degraded (far less than code's 1.40)
                    {'Y', 1.0},  ///< Explicit Y-code
                    {'M', 1.0},  ///< Military M-code
                    {'L', 0.95}, ///< L2C(L) pilot — light pilot bonus
                    {'S', 1.05}, ///< L2C(M+L) data/mixed — light data penalty
                    {'X', 0.9},  ///< Combined I+Q — coherent power gain (smaller than code)
                    {'I', 1.05}, ///< Data-only — light penalty
                    {'Q', 0.95}, ///< Pilot-only — light pilot bonus
                }},
                // ---- GPS ----
                // No chip-rate-driven C/A penalty on phase — the carrier
                // is the same physical signal regardless of code.  GPS C
                // phase residuals match GPS L (L1C pilot) and Galileo C
                // (E1C pilot) on real data.  'W' inherits default 1.10.
                {"GPS", {
                    {'C', 1.0},  ///< C/A — no narrow-correlator penalty on phase
                }},
                // ---- Galileo ----
                {"Galileo", {
                    {'A', 1.0},
                    {'B', 1.0},
                    {'C', 0.95}, ///< E1C pilot — light bonus
                    {'X', 0.90}, ///< E1 B+C / E5 I+Q (incl. alt-BOC) — coherent gain
                    {'Q', 0.95}, ///< E5a/E5b pilot-only — light bonus
                    {'I', 1.05}, ///< E5a/E5b data-only — light penalty
                    {'Z', 0.95}, ///< E6 B+C combined
                }},
                // ---- BeiDou ----
                {"BeiDou", {
                    {'I', 1.0},  ///< B1I/B2I/B3I data
                    {'Q', 0.95}, ///< pilot-only — light bonus
                    {'X', 0.90}, ///< combined I+Q — coherent gain
                    {'D', 1.0},  ///< B1C data
                    {'P', 0.95}, ///< B1C pilot — light bonus
                }},
                // ---- GLONASS ----
                // Phase noise is chip-rate-independent, so FDMA C/A gets
                // only a small C/N0-driven penalty (not the 1.50 code one).
                {"GLONASS", {
                    {'C', 1.05}, ///< L1OF/L2OF FDMA C/A — light penalty
                }},
            }
        };

        /// Fallback code noise when no block match is found (meters).
        /// **Derived** at config-load time as
        /// @c measurement_noise.code_std_m (i.e.\ scale factor 1.0,
        /// equivalent to the worst legacy block).
        double default_code_std_m{ 0.15 };
        /// Fallback phase noise when no block match is found (meters).
        /// **Derived** at config-load time as
        /// @c measurement_noise.phase_std_m.
        double default_phase_std_m{ 0.001 };

        /// Whether to route phase observations through the
        /// block × @c phase.freq_band_scale × @c phase.tracking_attr_scale
        /// × elevation pipeline (@c true, default), or use the simpler
        /// @c default_phase_std_m × elevation path (@c false).
        ///
        /// The flat path was historically the default on a PLL-physics
        /// argument: per-block carrier-loop noise differential sits
        /// below the multipath / atmospheric noise floor, and the
        /// chip-rate-driven freq scaling doesn't apply post-
        /// correlation.  Real-data phase residuals do show structure
        /// across bands and tracking attributes — just *different*
        /// structure than code — so the model keeps the structural
        /// shape but uses dedicated @c phase tables with carrier-
        /// tracking-driven defaults (no chip-rate C/A penalty,
        /// smaller band gradient, near-flat attribute differences).
        /// See @c ObservableScale doxygen for the per-axis rationale.
        ///
        /// When @c true, phase uses the per-block @c block_phase_std_m
        /// table (populated from @c block_scale_factors ×
        /// @c measurement_noise.phase_std_m at config load), the
        /// @c phase.freq_band_scale table, and the
        /// @c phase.tracking_attr_scale per-system table.  Elevation
        /// amplification uses @c phase_elevation_power (gentler than
        /// code's) via @c computeElevation's per-observable dispatch.
        ///
        /// Has no effect when the active stochastic model is anything
        /// other than @c SatelliteBlock — @c Constant / @c Elevation /
        /// @c SNR models keep their own phase paths unchanged.
        bool apply_to_phase{ true };
    };

    /**
     * @brief Selects how GNSS and INS measurements are fused.
     *
     * - LooseCoupled: GNSS position/velocity solutions update INS states
     *   as pseudo-measurements.  The existing MeasurementModel operates
     *   independently and IMU only drives the prediction step.
     * - TightCoupled: Raw GNSS code/phase/Doppler observations update the
     *   full INS+GNSS error-state vector directly.  The MeasurementModel
     *   Jacobian is extended with attitude-error partials so that GNSS
     *   measurements constrain attitude, sensor biases and ambiguities
     *   simultaneously.
     */
    enum class ImuCouplingMode {
        LooseCoupled,   ///< IMU prediction + GNSS solution update.
        TightCoupled    ///< IMU prediction + raw GNSS observation update.
    };

    /**
     * @brief Configuration for IMU (Inertial Measurement Unit) integration.
     *
     * When enabled, the Kalman filter uses IMU specific-force and angular-rate
     * measurements to propagate position, velocity and attitude between GNSS
     * epochs.  The coupling_mode selects loosely- or tightly-coupled fusion.
     * Nine additional states (3 attitude error + 3 accelerometer bias +
     * 3 gyroscope bias) are added to the filter.
     */
    struct ImuConfig {
        bool enabled{ false };                       ///< Master enable for IMU integration.
        ImuCouplingMode coupling_mode{ ImuCouplingMode::LooseCoupled }; ///< Fusion strategy.
        std::filesystem::path imu_file_path;         ///< Path to the IMU CSV data file.

        /// Accelerometer noise density (m/s�/?Hz).  Typical MEMS: 0.01�0.1.
        double accel_noise_density{ 0.05 };
        /// Gyroscope noise density (rad/s/?Hz).  Typical MEMS: 1e-4�1e-3.
        double gyro_noise_density{ 5e-4 };
        /// Accelerometer bias instability (m/s�/?Hz, random-walk spectral density).
        double accel_bias_instability{ 1e-4 };
        /// Gyroscope bias instability (rad/s�/?Hz, random-walk spectral density).
        double gyro_bias_instability{ 1e-6 };

        /// Initial accelerometer bias standard deviation (m/s�).
        double initial_accel_bias_std{ 0.1 };
        /// Initial gyroscope bias standard deviation (rad/s).
        double initial_gyro_bias_std{ 1e-3 };

        /// Initial attitude uncertainty (rad).  Used to seed the attitude
        /// block of the error-state covariance.
        double initial_attitude_std{ 0.1 };
    };


    /**
     * @brief Grouped enable/disable flags for core PPP model corrections.
     *
     * Each flag controls whether the corresponding correction term is
     * computed and applied during observation modelling.  When a flag is
     * false, the associated correction evaluates to zero (the neutral
     * element for an additive range correction) so the rest of the
     * processing chain is unaffected.
     *
     * All flags default to \c true so that the full model is active
     * unless the user explicitly disables individual terms.
     */
    struct ModelCorrections {
        // --- Atmosphere ---
        bool enable_troposphere{ true };               ///< Saastamoinen ZHD + GMF dry mapping (slant hydrostatic delay).
        bool enable_klobuchar_ionosphere{ false };     ///< Klobuchar ionospheric delay model (single-frequency fallback only).  IF combinations and DD short-baselines cancel ionosphere geometrically; PPP-RTK uncombined estimates per-satellite slant iono states.  In all three of those configs the Klobuchar model is a no-op — but constructing the @c Ionosphere instance still pays for an extra @c Model::Geometry load and the Klobuchar coefficient parse.  Default off.  Enable only for single-frequency PPP runs that need a-priori iono.
        bool enable_higher_order_ionosphere{ false };  ///< Apply second-order (B·cos(θ)·STEC/f³) and third-order (η·STEC²/f⁴) ionospheric delays as a per-signal observation-level correction, evaluated at the GIM-derived IPP using a centered tilted dipole geomagnetic field.  Requires a loaded IONEX GIM as the absolute-STEC source; the engine auto-loads the GIM when this flag is on even if @c estimation.ionosphere_weighting.enabled is false.  No-op when no GIM resolves (warning logged at startup).  Magnitudes: I₂ ~1–2 cm at L1 during high TEC, I₃ ~mm; the IF combination only partially cancels these, so the largest visible benefit is in standard PPP (IF).  For PPP-RTK uncombined and short-baseline DD the higher-order term mostly cancels in the SD / DD differencing, so this flag is closer to a consistency knob than a bias remover.  Default off.

        // --- Relativistic / propagation ---
        bool enable_relativistic_correction{ true };  ///< Periodic relativistic clock correction (-2 r�v / c�).
        bool enable_shapiro_delay{ true };             ///< Shapiro (gravitational path) delay.

        // --- Geophysical site displacements ---
        bool enable_solid_earth_tide{ true };          ///< IERS 2010 solid Earth tide displacement.
        bool enable_ocean_tide_loading{ true };        ///< Ocean tidal loading displacement (BLQ).
        bool enable_pole_tide{ true };                 ///< Pole tide displacement (from ERP): IERS 2010 Sec. 7.1.4 rotational deformation.
        bool enable_ocean_pole_tide{ false };          ///< Ocean pole tide load displacement (IERS 2010 Sec. 7.1.5 / Desai 2002), added on top of @c enable_pole_tide.  Requires the opole load grid (files.opt_grid_path or <data>/static/opole/).  ~0.1-2 mm; default off keeps runs byte-identical.
        bool enable_atmospheric_loading{ false };      ///< Atmospheric pressure loading (S1/S2 thermal-tide site displacement). Requires per-station S1/S2 load coefficients (files.atm_tide_grid_path or <data>/static/apl/). Sun-synchronous → does NOT average out in static daily PPP. Default off keeps runs byte-identical.

        // --- Antenna phase centre ---
        bool enable_satellite_pco{ true };             ///< Satellite antenna phase centre offset.
        bool enable_satellite_pcv{ true };             ///< Satellite antenna phase centre variation.
        bool enable_receiver_pco{ true };              ///< Receiver antenna phase centre offset.
        bool enable_receiver_pcv{ true };              ///< Receiver antenna phase centre variation.
        std::string receiver_antenna_type_override{ "" };  ///< Receiver antenna type when no RINEX header supplies one (live SBF/RTCM). Format "MODEL RADOME", e.g. "LEIAR25         NONE". Empty = use RINEX header (which always wins). Real-time path only.
        bool enable_receiver_cross_system_atx_fallback{ false };  ///< RECEIVER-ONLY: when the receiver ATX entry lacks a non-GPS frequency, substitute the nearest GPS band (E1->G01, E5a->G05, B1C->G01, B2a->G05, ...). Valid: receiver phase pattern is frequency-dependent on shared hardware. NEVER applied to the satellite antenna (E01 and G01 are different spacecraft). Opt-in.
        bool enable_atx_frequency_fallback{ true };    ///< When the ATX entry doesn't have PCO/PCV for the requested frequency (common for L5 / E5* / B2a on older satellites or receiver antennas), look up the nearest available frequency on the same antenna and use its values instead of silently skipping the correction.  Cross-frequency PCO drift on a single physical antenna is typically <1 cm — better than the few-cm vertical residual that comes from skipping a frequency entirely.  Set false to revert to the legacy "skip if missing" behavior for A/B comparison.
        bool require_atx_coverage{ false };            ///< When true, drop observations whose (sat, signal) lacks complete ATX coverage on **both** the satellite and receiver entries — i.e., satellite PCO+PCV and receiver PCO+PCV must all resolve to non-empty entries for the row's exact frequency, *without* @c enable_atx_frequency_fallback being used to substitute a neighbouring frequency.  Default false (legacy behaviour: observations are kept and any missing PCO/PCV is silently zeroed).  Set true when you've explicitly chosen `signal_mode: "all"` and want the ATX file to be the authoritative whitelist, dropping rows that would otherwise process without proper antenna calibration.
        bool require_atx_coverage_for_ar{ false };     ///< When true, AR (WL constraints, LAMBDA NL search) skips (sat, signal) pairs lacking strict ATX coverage even though the underlying observations are still used in the float Kalman update.  Softer counterpart to @c require_atx_coverage: lets you keep "all"-signal float estimation alive while restricting integer fixing to the ATX-curated subset.  Default false (no AR-side gating).
        bool restrict_ar_for_substituted_atx{ false }; ///< When true, AR additionally skips (sat, signal) pairs whose ATX correction was computed via the frequency-fallback substitution chain (rather than an exact entry).  Targets specifically the cases where the antenna model is approximate; signals lacking ATX entirely (no substitution applied) are still allowed.  Use alongside or instead of @c require_atx_coverage_for_ar depending on which kind of imperfection you want to gate on.  Default false.
        bool loosen_amb_q_for_substituted_atx{ false }; ///< When true, ambiguity states whose underlying (sat, signal) had their ATX correction frequency-fallback-substituted use a looser process noise (@c ambiguity_q_substituted_atx) instead of the standard @c process_noise.ambiguity.  Compensates for the small but non-zero modeling error introduced by the substituted PCV/PCO.  Default false.
        double ambiguity_q_substituted_atx{ 1e-12 };   ///< Process noise (m²/s) applied to ambiguity states for substituted-ATX (sat, signal) pairs when @c loosen_amb_q_for_substituted_atx is true.  The PCV/PCO substitution does not inject *random* noise — it introduces a **deterministic, slowly-varying bias** in the LOS direction, driven by the mismatch between the requested-frequency antenna pattern and the substituted-frequency pattern (e.g. L2 standing in for L5).  That mismatch projects onto LOS as the satellite sweeps elevation/azimuth across the arc.  Order-of-magnitude estimate: a ~10 mm PCO_Up substitution mismatch combined with an MEO elevation rate of ~1.5e-4 rad/s yields a LOS bias drift of ~1 μm/s at mid-elevation, i.e. Q ≈ (1 μm/s)² · 1 s ≈ 1e-12 m²/s.  This coincides with the standard @c process_noise.ambiguity default — the correct conclusion that a well-formed substitution adds negligible Q on top of the baseline ambiguity random walk.  The override therefore exists as a deliberate "I've audited the substituted-ATX bias and the standard Q is sized to absorb it" affirmation rather than a noise-injection knob; raise to ~1e-11 only if you have direct evidence (e.g. an arc-correlated residual signature on substituted signals) that the standard Q is too tight.  Values above ~1e-10 will start leaking real estimation strength out of the ambiguity into other states.
        double ambiguity_std_substituted_atx{ 1e3 };   ///< Initial ambiguity standard deviation (m) for newly-seeded ambiguities whose (sat, signal) had their ATX correction frequency-fallback-substituted.  Replaces @c getAmbiguityStd() at addAmbiguity time when @c loosen_amb_q_for_substituted_atx is true.  Default 1e3 — same scale as the standard ambiguity init; raise to 5e3 or larger if you want the filter to be especially loose on substituted-ATX seeds because the substituted PCV can introduce a per-epoch sub-cycle bias that the float ambiguity has to absorb.
        double atx_substitution_phase_std_m{ 0.0 };    ///< Zenith standard deviation (m) of an **additive** carrier-phase measurement-noise term applied to phase rows whose receiver ATX correction was frequency-fallback-substituted (@c wasAtxSubstituted true).  Summed in quadrature onto the stochastic-model phase variance as @f$(\sigma/\sin\,\mathrm{el})^2@f$ *before* any iono-free combination (so the IF coefficients propagate it).  @c 0 disables — default off, opt-in like the other substituted-ATX knobs.  **Complementary to, not a duplicate of, @c ambiguity_q_substituted_atx.**  That knob lets the per-(sat, signal) ambiguity random walk absorb the *per-satellite, slowly-varying* LOS bias of a substituted PCO/PCV (which does not bias coordinates).  It cannot absorb the *azimuthally-symmetric, elevation-dependent* receiver-PCV residual, which is **common across satellites** at a given elevation and therefore aliases into the elevation-structured states — ZWD and station height — like a small tropo mismodel.  Tightening per-signal phase σ (e.g. Galileo E5 alt-BOC at sub-mm) raises the Kalman gain on exactly those rows, so without this term the unmodeled PCV residual leaks into the vertical with high confidence.  This inflation down-weights substituted phase to an honest effective σ while leaving properly-calibrated signals at full earned precision; elevation amplification is @f$1/\sin^2@f$ to match the phase thermal term and because the PCV residual and its vertical aliasing are both worst at low elevation.  Recommended ~0.003 (3 mm) for a fallback between neighbouring GNSS bands; raise toward 0.005–0.010 for a large inter-frequency PCO gap or if you observe an elevation-correlated phase-residual / ZWD signature on substituted signals.
        bool apply_phase_shifts{ false };              ///< When true, RINEX phase observations have the receiver-applied @c "SYS / PHASE SHIFT" values subtracted at parse time, recovering the unshifted phase convention.  Default false because most modern BIA products (CODE, CAS, GFZ) are calibrated against the receiver-shifted phases as recorded in RINEX, so leaving the shifts in place is the standard PPP convention.  Set true only when your bias product is explicitly calibrated for the unshifted convention, or when running without external BIA and using in-house bias estimation that benefits from a uniform shift-free input.

        // --- Carrier-phase ---
        bool enable_phase_windup{ true };              ///< Carrier-phase wind-up correction.

        // --- Product biases ---
        bool enable_code_phase_bias{ true };            ///< Code/phase bias corrections from BIA product files.
        bool bias_coverage_requires_apply{ false };     ///< When true, the "already corrected?" gates in
                                                        ///< @c MeasurementModel (per-(sat,signal) sat-bias absorber
                                                        ///< allocation and the no-BIA code downweight) treat a .BIA
                                                        ///< entry as coverage ONLY while @c enable_code_phase_bias is
                                                        ///< actually applying it.  Default false preserves the legacy
                                                        ///< behaviour, where a loaded-but-unapplied .BIA reports
                                                        ///< phantom coverage — suppressing the absorber states and the
                                                        ///< downweight even though the rows carry the raw satellite
                                                        ///< OSB content (the PPP-RTK closed-loop consumer defect:
                                                        ///< network configs force @c enable_code_phase_bias off for
                                                        ///< @c estimate_iono_sh, the derived consumer inherits it, and
                                                        ///< the untreated code rows bias the float vertically).  The
                                                        ///< network PRODUCER path (@c ProcessingMode::PPP_RTK_Network)
                                                        ///< is exempt: its code rows are handled by network-estimated
                                                        ///< shared code-bias states, so coverage semantics there stay
                                                        ///< legacy regardless of this flag.  Detector- and seed-side
                                                        ///< uses of @c hasBiasEntry (MW/EWL alignment, WLS seeds) are
                                                        ///< intentionally unaffected — they consume product VALUES
                                                        ///< internally and do not double-count against the filter.

        // --- Processing strategy ---
        bool refine_emission_time{ true };              ///< When true, @c Geometry::computeSatelliteGeometry runs a
                                                        ///< second pass through @c computeEmissionState with the
                                                        ///< slant tropospheric ZHD and IF-combined satellite + receiver
                                                        ///< ATX LOS projection subtracted from the seed pseudorange.
                                                        ///< This drives the residual t_em error from tens of ns (driven
                                                        ///< by the ~3 m tropo + ~1-7 m PCO content of the raw pseudorange)
                                                        ///< down to ~ns / ~μm of sat displacement.  Cost: one extra
                                                        ///< Neville orbit interp + clock interp + 1-2 ATX evaluations
                                                        ///< per sat per epoch (~15-20% overall per-epoch overhead).
                                                        ///< Set false to disable the refinement pass entirely — useful
                                                        ///< for A/B comparison or when chasing artifacts that may be
                                                        ///< sensitive to the second-pass logic.  Independent of
                                                        ///< @c enable_troposphere and the antenna model: when this
                                                        ///< flag is true the refinement only adds whichever components
                                                        ///< (tropo, ATX) are individually enabled / wired in.
    };

    /**
     * @brief Configuration for automatic data download and local caching.
     *
     * When enabled, the application automatically downloads satellite orbit,
     * clock, bias, ERP and broadcast navigation products from CDDIS, and
     * optionally observation RINEX data.  Previously downloaded files are
     * reused from the local cache.
     */
    struct DownloadConfig {
        bool enabled{ false };                       ///< Master enable for auto-download.
        std::filesystem::path data_root;             ///< Local cache root directory.
        std::string product_source{ "COD" };         ///< 3-char analysis centre prefix.
        std::string product_type{ "FIN" };            ///< Product latency: FIN, RAP, ULT.
        std::filesystem::path netrc_path;            ///< Path to .netrc file (empty = default).
        std::string station;                         ///< 4- or 9-char station name for RINEX download.
        std::string station_country{ "USA" };         ///< 3-char country code.
        int year{ 0 };                               ///< Observation year (0 = auto-detect).
        int doy{ 0 };                                ///< Observation day-of-year (0 = auto-detect).
        bool download_rinex{ false };                ///< Download observation RINEX from CDDIS.
        bool download_products{ true };              ///< Download SP3/CLK/BIA/ERP/BRDC.
        bool download_gim{ false };                  ///< Download the IONEX GIM (global ionosphere map).
        std::filesystem::path atx_path;              ///< Path to local ATX file (not downloaded).
        std::filesystem::path nc_path;               ///< Path to local ocean-loading NC file.
        std::filesystem::path crx2rnx_path;          ///< Path to crx2rnx executable for Hatanaka decompression.
        bool keep_zip_after_unzip{ false };          ///< If true, keep the .gz after decompression. Default false (delete to save disk).
        bool atx_auto_refresh{ false };              ///< Opt-in: refresh igs20.atx from IGS at run start (shells out to scripts/refresh_atx.py). Default off for reproducibility.
        double atx_refresh_max_age_days{ 7.0 };      ///< Skip the network check when the local ATX is younger than this (per-run throttle). 0 = always check.
        std::string atx_refresh_url;                 ///< Optional ATX source URL override (empty = script default, files.igs.org/.../igs20.atx).
    };

    /**
     * @brief Configuration for spdlog file loggers and CSV trace outputs.
     *
     * Each flag controls whether the corresponding log file or trace CSV
     * is created and written at runtime.  Defaults are all-on so that
     * disabling output requires an explicit opt-out in the JSON config.
     */
    struct LoggingConfig {
        // --- spdlog file loggers (.log) ---
        bool enable_main_log{ true };              ///< logs/main_log.log
        bool enable_gnss_config{ true };            ///< logs/gnss_config.log
        bool enable_kalman_filter{ true };          ///< logs/kalman_filter.log
        bool enable_smoother{ true };               ///< logs/smoother.log
        bool enable_state_manager{ true };          ///< logs/state_manager.log
        bool enable_ppp_model{ true };              ///< logs/ppp_model.log
        bool enable_measurement_model{ true };      ///< logs/measurement_model.log
        bool enable_mw_slip_detector{ true };       ///< logs/mw_slip_detector.log
        bool enable_if_slip_detector{ true };       ///< logs/if_slip_detector.log
        bool enable_gf_slip_detector{ true };       ///< logs/gf_slip_detector.log
        bool enable_turbo_edit_resolver{ true };    ///< logs/turbo_edit_resolver.log
        bool enable_cycle_slip_manager{ true };     ///< logs/cycle_slip_manager.log
        bool enable_ambiguity_resolver{ true };     ///< logs/ambiguity_resolver.log
        bool enable_corrections{ true };            ///< logs/corrections.log
        bool enable_geometry{ true };               ///< logs/geometry.log
        bool enable_troposphere{ true };            ///< logs/troposphere.log
        bool enable_ionosphere{ true };             ///< logs/ionosphere.log
        bool enable_antenna{ true };                ///< logs/antenna.log
        bool enable_antenna_model{ true };          ///< logs/antenna_model.log
        bool enable_ocean_tide{ true };             ///< logs/ocean_tide.log
        bool enable_planetary_model{ true };        ///< logs/planetary_model.log
        bool enable_stochastic_model{ true };       ///< logs/stochastic_model.log
        bool enable_rinex_parser{ true };           ///< logs/rinex_parser.log
        bool enable_product_parser{ true };         ///< logs/product_parser.log
        bool enable_product_cache{ true };          ///< logs/product_cache.log
        bool enable_resolver{ true };               ///< logs/resolver.log
        bool enable_state_initializer{ true };      ///< logs/state_initializer.log
        bool enable_bssd_pivot_manager{ true };     ///< logs/bssd_pivot_manager.log
        bool enable_bssd_transform{ true };         ///< logs/bssd_transform.log
        bool enable_ppp_utils{ true };              ///< logs/ppp_utils.log
        bool enable_downloader{ true };             ///< logs/downloader.log
        bool enable_ntrip{ true };                  ///< logs/ntrip.log
        bool enable_rt_engine{ true };              ///< logs/rt_engine.log
        bool enable_imu{ true };                    ///< logs/imu.log

        // --- CSV trace files ---
        bool enable_state_trace{ true };            ///< logs/state_trace.csv
        bool enable_ambiguity_trace{ true };        ///< logs/ambiguity_trace.csv
        bool enable_ionosphere_trace{ true };       ///< logs/ionosphere_trace.csv
        bool enable_sat_bias_trace{ true };         ///< logs/sat_bias_trace.csv
        bool enable_code_bias_trace{ true };        ///< logs/code_bias_trace.csv (receiver DCB/IFB)
        bool enable_sat_status_trace{ true };       ///< logs/satellite_status_trace.csv
        bool enable_dop_trace{ true };              ///< logs/dop_trace.csv
        bool enable_residual_trace{ true };         ///< logs/measurement_residuals.csv
        bool enable_wls_trace{ true };              ///< logs/wls_trace.csv
        bool enable_smoother_trace{ true };         ///< logs/smoother_trace.csv
        bool enable_mw_slip_trace{ true };          ///< logs/mwwl_trace.csv
        bool enable_if_slip_trace{ true };          ///< logs/if_slip_trace.csv
        bool enable_gf_slip_trace{ true };          ///< logs/gf_slip_trace.csv
        bool enable_turbo_edit_trace{ true };       ///< logs/turbo_edit_trace.csv
        bool enable_windup_trace{ false };          ///< logs/windup_trace.csv (phase wind-up diagnostic; off by default)

        /**
         * @brief Concise per-epoch logging for the Kalman-filter logger.
         *
         * When @c true, the @c kalman_filter logger only emits the FIRST
         * and LAST measurement-update pass within each epoch (regardless
         * of whether outliers were found). All intermediate rebuild
         * passes are suppressed.  Lets the user keep tight phase noise +
         * aggressive outlier rejection without the per-pass log volume
         * blowing up — typical reduction is 5-10× on outlier-heavy runs.
         *
         * Pass-1 messages are written immediately at their original
         * level; intermediate passes are buffered into a per-pass slot
         * and discarded; the final pass's slot is replayed when the
         * caller signals end-of-epoch.  No call-site changes — the
         * gate is implemented as a sink wrapper around the existing
         * @c kalman_filter sinks, applied at startup.
         */
        bool kalman_filter_concise_log{ false };

        /**
         * @brief Look up whether a given spdlog logger name is enabled.
         * @param name Logger name (e.g. "kalman_filter").
         * @return true if the logger should produce file output.
         */
        bool isLoggerEnabled(const std::string& name) const;

        /**
         * @brief Per-logger severity overrides parsed from JSON @c logging.levels.
         *
         * Map of logger name → LogLevel. Loggers absent from this map fall
         * back to the global level set in @c LogManager::initialize() (Info
         * by default). Disabled loggers (via the @c enable_X flags above)
         * take precedence over any entry here.
         *
         * Example JSON:
         * @code
         * "logging": {
         *   "levels": {
         *     "kalman_filter": "debug",
         *     "geometry": "warn",
         *     "ppp_model": "info"
         *   }
         * }
         * @endcode
         */
        std::map<std::string, LogLevel> per_logger_levels;
    };

} // namespace GnssProcessor
