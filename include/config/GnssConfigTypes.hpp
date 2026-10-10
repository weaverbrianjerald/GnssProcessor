#pragma once

/**
 * @file GnssConfigTypes.hpp
 * @brief Configuration vocabulary for GnssConfig: the enums and POD
 *        config sub-structs consumed by GnssConfig and its callers.
 *
 * Kept apart from GnssConfig.hpp so that a caller needing only a config enum
 * or struct can include this lighter header instead of the full config header.
 * GnssConfig.hpp includes this file. All types are in namespace GnssProcessor.
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
     *   any turnkey run without a network connection. Forces
     *   @c DownloadConfig::enabled = false.
     * - Production: PathResolver delegates to @c ProductResolver, which
     *   computes IGS long-name filenames from year/doy/station and
     *   downloads any missing products from CDDIS into
     *   @c DownloadConfig::data_root.  Used for real PPP / PPP-RTK runs
     *   on arbitrary stations and dates.
     *
     * The two modes share the same on-disk layout (orbit/, clock/,
     * dcb/, erp/, brdc/, rinex/, atx/, nc/), so switching data sources is
     * a one-line config change.
     */
    enum class DataSource {
        Internal,   ///< Use files under @c internal_data_root only.
        Production  ///< Auto-download from CDDIS into @c data_root.
    };

    /**
     * @brief Per-station live-transport descriptor for the multi-station
     *        streaming front-end (@c network.streaming) (RTF-3).
     *
     * Describes where one roster station's live observation bytes come from.
     * An empty @c input_type (the default) means the station has no stream.
     * Under batch processing that holds for every station.  Under streaming, such
     * a station contributes no live epochs: the front-end logs a warning when it
     * is built, and the station is absent from every dispatched epoch, a case the
     * downstream code handles as normal.  The field set mirrors @c NtripConfig and
     * the realtime aux-stream descriptors so the front-end maps 1:1 onto the
     * existing transports.
     */
    struct StationTransportConfig {
        std::string input_type;           ///< "" (no stream) | "ntrip" | "tcp" | "file" (RTCM3 dump replay).
        std::string host;                 ///< Caster / TCP server hostname.
        int         port{ 2101 };         ///< Caster / TCP port.
        std::string mountpoint;           ///< NTRIP mountpoint (ntrip only).
        std::string username;             ///< NTRIP username ("" = anonymous).
        std::string password;             ///< NTRIP password.
        bool        use_tls{ false };     ///< HTTPS caster (e.g. port-443 casters).
        bool        tls_insecure{ false };///< Skip TLS peer/host verification (testing only).
        std::string input_path;           ///< RTCM3 dump path for "file" replay lanes.
        int         reconnect_delay_s{ 5 };///< Reconnect back-off base seconds (per-attempt jitter added).

        /// True when the station declares any live transport.
        bool configured() const { return !input_type.empty(); }
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
        /// Provenance: true when @c position_ecef was resolved from a loaded SIF
        /// registry (velocity-propagated occupation) under
        /// @c network.use_sif_coordinates rather than given in the JSON.  Explicit
        /// JSON coordinates take precedence, so this is false whenever the config
        /// supplies them.  Logged per station at roster build (RTA-12).
        bool position_from_sif{ false };
        /// Live transport for the streaming front-end (default: none).
        /// Ignored entirely under batch processing (network.streaming off).
        StationTransportConfig transport;
    };

    /**
     * @brief Configuration for double-differenced baseline / network processing.
     *
     * Used when @c ProcessingMode is @c BaselineSingle or @c BaselineMulti.
     * A single reference station is required in both cases.  For
     * @c BaselineSingle exactly one rover is expected; for @c BaselineMulti
     * one or more rovers may be listed, each double-differenced independently
     * against the master reference (star topology).
     */
    struct BaselineConfig {
        StationConfig reference;              ///< Master reference station (fixed coordinates).
        std::vector<StationConfig> rovers;    ///< Rover stations (one for Single, >=1 for Multi).
        double max_baseline_km{ 30.0 };       ///< Soft upper bound for the short-baseline atmosphere assumption.
        double epoch_sync_tolerance_s{ 0.01 };///< Max time-tag difference when matching epochs across receivers.
        bool code_only{ false };              ///< Debug toggle: skip phase DD updates (filter uses only code DDs, no ambiguity states).
        bool use_doppler{ false };            ///< Include Doppler ('D') observables when forming DDs.  Only the loader reads this flag (it parses and logs it); the baseline row builder does not consult it.  The baseline filter has no velocity state, so Doppler DDs are not consumed.
        bool signal_aliasing{ true };         ///< Pair signals across receivers at the band level when tracking attributes differ (e.g. rover C2W against reference C2C, both GPS L2).  When false, only exact RINEX-3 code matches form DD rows (the strict behavior).  On by default because mixed-receiver networks are common and the strict path drops usable measurements without a warning.  The satellite-DCB residual on cross-attribute code rows is small (typically < 1 m); on phase rows the float ambiguity absorbs it.  DCB harmonization for ambiguity fixing is planned (phase 2), not implemented.
        bool use_iono_free{ false };          ///< Use the dual-frequency ionosphere-free combination instead of per-band uncombined DDs.  On long baselines (> 10–20 km) the ionosphere does not cancel between receivers; the IF combination removes the first-order term at the cost of about 3× code noise and a non-integer ambiguity (float only: AR is excluded for IF rows).  On short baselines IF is unnecessary and noisier than uncombined processing; the toggle exists for testing and teaching.  Per satellite pair and observable type, the row builder takes the two highest-frequency bands available at both rover and reference and combines them into one IF row.  Satellite pairs with fewer than two shared bands are dropped in IF mode.
        bool enable_pivot_handover{ true };   ///< When the per-constellation sticky pivot changes (the incumbent left the shared-satellite set), apply an S-transformation that re-references the existing DD ambiguities to the new pivot instead of erasing and re-seeding them.  This preserves integer cycle information across the change; without it, each pivot handover produces a position discontinuity while the re-seeded float ambiguities absorb the rover's geometry residual again.  The transform is x_new = T·x_old, P_new = T·P_old·Tᵀ, with T the standard DD S-transform from DDs against the old pivot to DDs against the new one.  Default true; false restores the reset-on-handover behavior for testing or comparison.
        bool estimate_differential_tropo{ false };  ///< Estimate a single differential zenith wet delay (Δzwd ≡ ZWD_rov − ZWD_ref) as a state, mapped to slant via the wet mapping function.  PppModel applies only the Saastamoinen ZHD a-priori; the wet delay is otherwise unmodeled, and on short baselines (< ~10 km) it cancels in BRSD because both stations see nearly identical wet path delays.  On longer baselines (especially with altitude differences) Δzwd can reach several centimeters, and the residual leaks into vertical position and float ambiguities.  Set true with use_iono_free for the typical long-baseline configuration.  Per phase DD row the H column entry is (mf_w_rov_i − mf_w_rov_p); this short-baseline approximation assumes the mapping functions match between rover and reference.  Single state (one Δzwd, one process-noise channel); not adequate for > 100 km baselines, where rover and reference mapping functions diverge.
        double differential_tropo_init_std_m{ 0.1 };  ///< Initial standard deviation of the differential ZWD state (m).  Default covers ~1 cm/10 km horizontal × 30 km plus modest altitude-difference contribution.  Used only when @c estimate_differential_tropo is true.
        double differential_tropo_process_noise_m_per_sqrt_s{ 1e-4 };  ///< Random-walk process noise spectral density (m/√s) for the differential ZWD state.  Default 1e-4 ≈ 6 mm/√h, a typical published value for short-to-medium-baseline differential troposphere drift.  Squared to a per-second variance rate inside @c getProcessNoiseMatrix.  Used only when @c estimate_differential_tropo is true.
    };

    /**
     * @brief Closed-loop integer-recovery harness configuration (held-out consumer).
     *
     * Closed-loop acceptance test for the network producer.  A held-out station,
     * not one of the @c NetworkConfig::stations in the joint solve, consumes the
     * network's SSR products (satellite clock and phase bias) epoch by epoch as
     * they are emitted, through the user-side PPP-RTK and ambiguity-resolution
     * pipeline.  The products reach it through a @c SsrCorrectionStore owned by
     * the harness, never the process-wide singleton.  The test passes when the
     * project's own user engine recovers integers from the project's own products
     * at an independent station.  The consumer's coordinates are held as truth
     * for scoring the recovered position; integer recovery (WL fix rate and
     * count) is the primary metric.
     */
    struct ClosedLoopConfig {
        bool          enable{ false };   ///< Run the held-out consumer after each network solve epoch.
        StationConfig station;           ///< Held-out consumer station (not in the network solve); needs rinex_path and a known ECEF position (truth).
        int           ar_mode{ 1 };      ///< Consumer AR mode (0 float, 1 continuous, 2 instantaneous, 3 fix-and-hold).  Producer is float; the consumer enables AR.
        bool          ar_wl_only{ true };///< WL-only AR, matching the float-only producer (validates wide-lane recovery; full NL fixing waits on the NL-LAMBDA validation, which is not scheduled).
        std::vector<std::string> ar_active_systems;  ///< Constellations eligible for consumer AR (empty => inherit the base config's AR active systems).
        bool          consume_network_products{ true }; ///< Inject the harness-owned network store into the consumer.  False gives the A/B baseline: the consumer then runs on precise SP3/CLK and .BIA only (an ordinary PPP-RTK user), which separates the network product's contribution to integer recovery and position from the consumer's own modeling.
        bool          consume_network_clock{ true };    ///< When consuming products, also consume the network clock.  False keeps precise SP3/CLK for the clock and consumes only the network phase bias: the cleanest integer-recovery test, since it isolates the phase-bias product, which integer recovery needs, from the clock product, which is marginal on a thin network.
        bool          consume_network_iono{ false };     ///< Consume the network's co-estimated SH-VTEC ionosphere.  The producer evaluates its VTEC field at the consumer's pierce points and supplies an undifferenced per-satellite slant-iono value (metres on L1); the consumer applies it as a tight, quality-scaled BLUP (ionosphere-weighted) prior on its slant-iono states instead of floating them at ionosphere_q.  This is the ionosphere-corrected user model that Odijk (2012) and TK-2015 identify as the enabler of fast AR.  Default off; off is byte-identical (the producer pushes nothing and the consumer applies nothing).
        bool          consume_combined_sigma{ false };   ///< Use the network product's per-key combined-correction σ (age-inflated) as the measurement variance of a network-corrected observation, in place of the RSS of URA and phase-bias σ.  A reconverging satellite can have cm-level clock and bias σ while its combined correction is good to mm; the RSS would deweight it by a factor of 10–100.  Falls back per key to the v1 weighting when the sigc/sigcc tokens are absent, so mixed streams are safe.  Default off; off is byte-identical (M4, contract v2).
        bool          consumer_undifferenced{ false };   ///< Consumer user model.  false: BSSD (the between-satellite single-difference S-transform in the filter).  true: the undifferenced, uncombined PPP-RTK user (the target model): no observation-space differencing; the network satellite phase biases are applied directly to the raw float, and per-band ambiguities are fixed individually through the resolver's internal-SD LAMBDA (no WL/NL cascade, no pivot-handover machinery).  Dropping the MW WL removes the conflict between the network bias frame and the .BIA frame.
        /**
         * @brief Carry the consumer's float ambiguity across a declared phase-bias
         *        step instead of resetting it (card CC, part A).
         *
         * Key @c network.closed_loop.pb_disc_carry, default false (byte-identical).
         * @c deriveClosedLoopConsumerConfig copies it into the consumer engine's
         * config as @c processing.ssr_phase_bias_disc_carry
         * (@c GnssConfig::ssrPhaseBiasDiscCarry); a standalone real-time consumer
         * sets that @c processing key directly.  The mechanism and its refusal
         * branches are in @c RealTimeEngine::carryOnPhaseBiasDisc_
         * (RealTimeEngineSsrBreaks.cpp).
         */
        bool          pb_disc_carry{ false };
        /**
         * @brief Answer a presence edge of a held phase bias (card PBF).
         *
         * Key @c network.closed_loop.pb_hold_presence_flip, default false
         * (byte-identical).  @c deriveClosedLoopConsumerConfig copies it into the
         * consumer engine's config as @c processing.ssr_pb_hold_presence_flip
         * (@c GnssConfig::ssrPbHoldPresenceFlip); a standalone real-time consumer sets
         * that @c processing key directly.  Under the producer's hold (card PBH) a held
         * bias that appears on, or leaves, a phase row the consumer already uses steps
         * the applied correction by the whole bias, and the card-PBH dispatch answers
         * neither edge.  Armed, both edges take the A5 reaction of an unheld key.  It
         * never reaches the producer's solve or its resume fingerprint.
         */
        bool          pb_hold_presence_flip{ false };
        /**
         * @brief Channel mode of the consumer itself
         *        (@c network.closed_loop.channel_mode, "single" (default) or "all")
         *        (card SID, phase 6b).
         *
         * The producer's `processing.channel_mode` acts on the producer: it sets how
         * many rows each network station contributes and therefore what the wire
         * carries.  The held-out consumer is one station with no signal family to
         * support, and widening it together with the producer would change two
         * things at once.  This key sets the consumer's widening independently;
         * @c deriveClosedLoopConsumerConfig copies it onto the consumer engine's
         * `processing.channel_mode`.
         *
         * The default "single" keeps the consumer at one tracking attribute per
         * band whatever the producer's setting.
         */
        std::string   channel_mode{ "single" };
        /**
         * @brief The consumer's band latch and its freshness response follow the
         *        producer's declared roster
         *        (@c network.closed_loop.roster_latch, default false, inert)
         *        (card SID, phase 5b).
         *
         * @c deriveClosedLoopConsumerConfig copies it into the consumer engine's
         * config as @c GnssConfig::consumerRosterLatch.  When armed, the consumer
         * takes its canonical signal per band from the declaration wherever it
         * tracks a declared signal (in the declaration's own order, which is the
         * producer's station-support order), and a fresh-to-stale flip on a signal
         * the declaration does not name is not treated as a staleness event.
         *
         * Inert, with one log line, when the wire carries no declaration, which is
         * the case for every producer that does not arm @c network.ledger_emit.  It
         * never reaches the producer's solve or its resume fingerprint.
         */
        bool          roster_latch{ false };
        /**
         * @brief The consumer translates every phase-bias product step into its own
         *        ambiguities, declared or not (card PBT, part A).
         *
         * Key @c network.closed_loop.pb_step_translate, default false (byte-identical).
         * @c deriveClosedLoopConsumerConfig copies it into the consumer engine's
         * config as @c processing.ssr_phase_bias_step_translate
         * (@c GnssConfig::ssrPhaseBiasStepTranslate); a standalone real-time consumer
         * sets that @c processing key directly.  The mechanism, the applied-value
         * guard and the refusal branches are in
         * @c RealTimeEngine::translatePhaseBiasStep_ (RealTimeEngineSsrBreaks.cpp).
         * It replaces both the reset reaction and @ref pb_disc_carry, so a declared
         * step is translated exactly once even with both knobs armed.
         */
        bool          pb_step_translate{ false };
        /**
         * @brief Dead band of @ref pb_step_translate in metres
         *        (@c network.closed_loop.pb_step_translate_eps_m, default 0.005)
         *        (card PBT, part A).
         *
         * At or below it nothing is written to the ambiguity state and the filter
         * absorbs the move as noise.  For the same reason an empty declaration (a
         * generation bump with no product motion) costs nothing.
         */
        double        pb_step_translate_eps_m{ 0.005 };
        /**
         * @brief Card CLV2: the consumer answers a declared code-level step by raising its
         *        variance along the stepped satellite's phase-blind ionosphere-ambiguity
         *        direction (@c network.closed_loop.level_step_inflate, default false ⇒
         *        byte-identical).
         *
         * A declared step is the C record's lvl= token (the satellite clock level, which
         * moves every code row of the satellite and no phase row) and the B record's lvl=
         * token (a free code-bias column's own step).  Derived into the consumer engine's
         * config as @c processing.ssr_level_step_inflate
         * (@c GnssConfig::ssrLevelStepInflate) by @c deriveClosedLoopConsumerConfig; a
         * STANDALONE consumer sets that key directly.  See
         * @c RealTimeEngine::reactToDeclaredLevelSteps_ (RealTimeEngineSsrBreaks.cpp).
         */
        bool          level_step_inflate{ false };
        /// Card CLV2: the inflation's standard deviation over the declared code step
        /// (@c network.closed_loop.level_step_inflate_k, default 1).
        double        level_step_inflate_k{ 1.0 };
        /**
         * @brief The consumer re-validates each float ambiguity against its own code
         *        level and refloats the ones the code rejects (card ALC).
         *
         * Key @c network.closed_loop.amb_level_check, default false (byte-identical).
         * This is the re-coupling edge (invariant #7) for cards CC and PBT: a carried
         * or translated ambiguity is continuous, which does not make it correct.
         * @c deriveClosedLoopConsumerConfig copies it into the consumer engine's
         * config as @c processing.amb_level_check (@c GnssConfig::ambLevelCheck); a
         * standalone real-time consumer sets that @c processing key directly.  See
         * @c RealTimeEngine::checkAmbiguityLevel_ (RealTimeEngineEstimate.cpp) and
         * @ref AmbLevelCheck.
         */
        bool          amb_level_check{ false };
        /// Samples per satellite before the test may run
        /// (@c amb_level_check_window_ep, default 20 = 10 min at 30 s) (card ALC).
        int           amb_level_check_window_ep{ 20 };
        /// Trip threshold: |state − level| > k · σ_level
        /// (@c amb_level_check_k, default 4.0) (card ALC).
        double        amb_level_check_k{ 4.0 };
        /// Action on a trip: @c "act" (refloat both bands through the existing reset
        /// path) or @c "observe" (count only, no state written) (card ALC).
        std::string   amb_level_check_mode{ "act" };
        /**
         * @brief Warm start of the closed-loop consumer in the event harness
         *        (@c network.closed_loop.state_snapshot, default false; false is
         *        byte-identical and writes no file) (card CC, part B).
         *
         * When true and the producer writes a network history snapshot copy
         * (@c state_snapshot_keep_history: @c <state_snapshot_path>.epNNNN), the
         * consumer engine's RtStateSnapshot is written beside it as
         * @c <state_snapshot_path>.consumer.epNNNN, with the same @c n_emitted suffix
         * and the same epoch tow (asserted at the write site; a consumer that did not
         * process that epoch writes nothing, and the skip is counted).
         * @c scripts/event_leg.py copies that file into a resume leg and points
         * @c realtime.state_snapshot_path at it, so the leg's consumer resumes at
         * the seam instead of cold-starting.  Consumer side only: the producer's
         * solve, its products and its own snapshot are untouched (not fingerprinted).
         */
        bool          state_snapshot{ false };

        /**
         * @brief Path to an already-written network product file to consume instead
         *        of running the producer (@c network.closed_loop.replay_products_path)
         *        (Q5 replay).
         *
         * When non-empty, @c Application::runNetwork hands off to
         * @c Application::runConsumerReplay before any producer setup: no joint
         * solve, no station roster, no emit.  The held-out consumer station is
         * driven epoch by epoch from its own RINEX while this file's records are
         * replayed into an owned @c SsrCorrectionStore through the same
         * @c NetworkProductReader::replay path the live harness uses, so the
         * consumer sees a byte-faithful reconstruction of the recorded stream
         * (including every discontinuity counter).
         *
         * This gives the position-domain arm of a correction-domain investigation
         * in minutes per arm instead of a producer day.  Empty (default) = off; the
         * producer path is untouched (the key is not in the resume fingerprint,
         * since it never reaches the producer).
         */
        std::string   replay_products_path;
    };

    /**
     * @brief MW-seeded wide-lane pseudo-observation layer (@c network.mw_wl.*)
     *        (M5 card WL-1).  See docs/SPEC_mw_wl_card.md.
     *
     * Feeds the per-arc Melbourne-Wubbena Welford means, double-differenced over the
     * fundamental-cycle chord basis of the invariant carry, into the producer solve
     * as rank-K pseudo-observation blocks on the cross-band ambiguity columns.  The
     * per-epoch network has no per-arc code-coherence accumulator; these blocks
     * supply one.  Off by default; off is byte-identical (no detectors built for
     * this layer, no blocks folded).  Requires @c network.carry_invariant (the chord
     * basis belongs to that carry; checked at load).  GLONASS is excluded upstream.
     */
    struct MwWlConfig {
        bool   enable{ false };               ///< Phase-A master switch.  Off is byte-identical.
        int    min_arc_epochs{ 10 };          ///< Welford n per arc before its cycles are eligible.
        double sigma_floor_cyc{ 0.05 };       ///< per-row R floor (cycles): R = max(Σ var/λ_WL², floor²).
        int    max_rows_per_epoch{ 0 };       ///< 0 = unlimited; else keep the lowest-R rows first (drops counted).
        // --- Phase B (WL integer fixing): config contract only, not implemented. ---
        bool   fix_enable{ false };           ///< Phase-B master switch (requires @c enable).  Parsed for the contract; inert, with a warning (the un-fix response that standing invariant #1 requires is not built).
        double fix_sigma_cyc{ 0.10 };         ///< Phase-B FFR gate: fix a cycle when its constrained WL posterior σ < this.
        double fix_frac_cyc{ 0.15 };          ///< Phase-B FFR gate: and |frac| < this.
        double fix_weight_sigma_cyc{ 0.01 };  ///< Phase-B fixed-row σ (a soft-hard constraint that can be rolled back).  WL-2 reuses it as the hold-row σ (SPEC_ewl_wl_cascade_card.md §3.3).
        // ============================================================================
        // EWL-first tri-frequency cascade (M5 card WL-2).
        // Supersedes the WL-1 float layer (@c enable becomes diagnostic-only) with an
        // EWL statistic lane that fixes by about epoch 3 (the third band is uncarried,
        // so its rows do not compete with a carried state), a phase bridge to a fast
        // WL integer, and the NL-preserving inject primitive that writes the fixed WL
        // into the invariant carry.  Off by default; off is byte-identical (no
        // third-band carry exclusion, no lanes, no inject).
        // ============================================================================
        bool   ewl_enable{ false };           ///< EWL lane master switch (statistics scope, rows and statistic-gated fix; no inject, since the lane is uncarried) (WL-2).  Requires @c network.carry_invariant.  Off is byte-identical.
        int    ewl_min_arc_epochs{ 2 };       ///< Lagged Welford n before an EWL cycle is eligible for a decision (first decision at epoch 3 under the F1 lag).
        double ewl_fix_sigma_cyc{ 0.10 };     ///< EWL FFR gate: fix when the statistic-row σ < this (cyc).
        double ewl_fix_frac_cyc{ 0.20 };      ///< EWL FFR gate: and |frac(z_EWL)| < this (cyc).
        bool   wl_inject_enable{ false };     ///< WL lane master switch (bridge on EWL-fixed cycles, MW(1,2) fallback, then injectLaneTarget) (WL-2).  Requires @c carry_invariant; the bridge path also requires @c ewl_enable.  Off is byte-identical.
        double wl_fix_sigma_cyc{ 0.10 };      ///< WL FFR gate: fix when the WL statistic σ < this (cyc).
        double wl_fix_frac_cyc{ 0.15 };       ///< WL FFR gate: and |frac(z_WL)| < this (cyc).
        double bridge_max_sigma_cyc{ 0.15 };  ///< Ionosphere-error guard: no WL fix from a bridge row whose σ exceeds this (cyc).
        double inject_disagree_min_cyc{ 0.75 };///< Inject trigger threshold on |statistic − carried lane value| (cyc).  After an inject the difference is about 0, so the trigger limits itself.
        double lane_max_abs_xlin_m{ 1000.0 }; ///< Pre-flight quarantine: skip a lane cycle if any arc's ambiguity linearization |x_lin| exceeds this (m).  Guards against the ~256 km receiver-clock seed at station 1 (§7 pre-flight): such cycles ill-condition the correction-frame rows and do not converge even at full authority.  0 = disabled.
        // --- Diagnostics (log-only; off by default, off is byte-identical). ---
        bool   f6_dump{ false };              ///< One-shot F6 attribution dump: on the first epoch that folds ≥ 1 WL row, write logs/network_mw_wl_f6.csv with per-arc (sat/sig/coeff/MW̄/x_lin) and per-row (z_MW/z_lin/z_corr/reconstructed_abs/innovation) fields.  Diagnostic only; does not change the solve.
        int    f6_max_rows{ 8 };              ///< F6 dump row cap.  The default dumps 8 rows of one block: a sample, not a population, so skip and quarantine rates cannot be inferred from it.  Raise it (with f6_all_blocks) for a population-scale cycle census, e.g. the per-cycle station membership that a lane-chord baseline histogram needs.  ≤ 0 = uncapped.  Log-only.
        bool   f6_all_blocks{ false };        ///< F6 dump walks every WL block instead of only wl_blocks.front().  Default false keeps the single-block sample, byte for byte.  Log-only.
        double debug_r_scale{ 1.0 };          ///< Diagnostic only: multiply every WL row's Pz by this (1.0 = unscaled default).  A value ≪ 1 lets the WL rows dominate the stiff carry; this is the F6 probe of whether separate-Woodbury composition is itself the failure or whether more authority suffices.  Changes the solve; not a production knob.
        bool   ewl_fold_rows{ false };        ///< Fold the EWL integer rows into the solve.  Folding adds cost and no benefit: the WL cascade reads the detector statistic, never the posterior, and the σ = 0.01 rows perturb the shared clock/PB border enough to raise the D5 flag count network-wide by about 2× (679 against 342 for the control), including on secondary-channel groups the EWL never touches (F-A).  Default false: decide, FFR-fix and expose the integers, fold nothing (D5 at the control level).  true is the study shape that reproduces the elevation; not a production knob.
        // ============================================================================
        // Per-constellation ultra-wide-lane top tier (M5 card UWL-1).
        // Generalizes the cascade's hardcoded GPS band digits to the per-constellation
        // lane-tier table (include/slip/MwLaneTiers.hpp) and adds the single-epoch cold
        // fix that the UWL wavelength allows.  Off by default; off is byte-identical,
        // including under multi-GNSS active_systems (arming a constellation without
        // uwl_enable changes nothing).  GPS-only runs stay bit-identical even when
        // armed, because the table reproduces the legacy digits.
        // ============================================================================
        bool   uwl_enable{ false };           ///< Master switch: arm the per-constellation top tier for constellations with a close-frequency pair (currently Galileo; BDS-3 is blocked on its clock divergence) (UWL-1).  Requires @c ewl_enable (shared machinery and telemetry).  Off: the legacy GPS-only digit scan everywhere.
        char   uwl_gal_top_band{ '7' };       ///< Galileo top-tier band digit: '7' = (E5b,E5a), Δf 30.69 MHz, λ 9.768 m (default); '8' = (E5 AltBOC,E5a), Δf 15.345 MHz, λ 19.537 m: twice the wavelength and cm-level AltBOC code, at the cost of sparser C8/L8 tracking (to be measured, not assumed).
        bool   uwl_mid_enable{ false };       ///< E6 mid-ladder bridge hop: chain the WL determination through the Galileo (E6,E5a) rung, λ 2.931 m.  The high hop bridges the fixed top integer to a rounded mid integer and the low hop bridges that to the WL, which cuts each hop's wavelength-ratio amplification of phase and iono error by about 3.3× against the direct 9.768 → 0.751 m hop.  Target: the measured ~70 % Galileo WL FFR-unfixed rate.  Decision side only; the direct hop remains the per-cycle fallback and cross-check.  Requires @c uwl_enable.  Off is byte-identical (a single bridge triple per constellation).
        bool   uwl_dual_top{ false };         ///< Per-cycle preferred and fallback Galileo top rung: band '8' (E5 AltBOC, λ 19.537 m) decides where it is tracked across the whole cycle, and the (5,7) rung (λ 9.768 m) decides what '8' could not.  Both bands {7,8} are uncarried, and the untaken sibling serves as a closure statistic (2·N(8,5) = N(7,5), since f_E5 = (f_E5a+f_E5b)/2 exactly), measured and hard-gated only under @c uwl_sib_gate (increment 5, §1b/§7.5).  Supersedes @c uwl_gal_top_band (ignored, noted at load).  Requires @c uwl_enable; forces @c ewl_fold_rows=false, since a row decided twice must not fold twice (the F-A refold class).  Off: the single-top tier unchanged (byte-identical).
        bool   uwl_sib_gate{ false };         ///< Sibling-closure hard gate: block a top-tier decision whose untaken-sibling MW statistic disagrees with fx_integer·λ_top/λ_sib by more than 0.5 + 3σ_sib; the blocked cycle falls to the fallback rung in the same epoch (increment 5).  Default off = measure only: receiver half-cycle conventions on AltBOC may offset the identity, and a population of half-integer residuals means the convention differs, so the gate would first have to learn a constant (SPEC reconciliation note 2).  Requires @c uwl_dual_top.
        bool   uwl_family_pairs{ false };     ///< Census recovery (v2): admit minority receiver-family primary pairs (e.g. Galileo (L1C,L5Q) beside the majority (L1X,L5X)) into lane pair groups of their own instead of leaving them per band.  A pair is admitted when its joint-census count is ≥ @c uwl_family_min_arcs and stays admitted for the run (latched: membership flicker would refloat carried chords, the WL-3 micro pair-group hazard).  Measured stake: about 40 of 104 decided Galileo integers (101 of 154 with the mid hop) are lost at skip_basis on minority-family cycles.  Requires @c uwl_enable and @c lane_carry.enable (the pair groups are lane machinery); otherwise inert, with a warning.
        int    uwl_family_min_arcs{ 8 };      ///< First-admission threshold for @c uwl_family_pairs: a census pair joins the admissible set when at least this many (rx,sat) arcs carry it in one epoch's census (clamped ≥ 1).  The fixture families measure about 115–132 arcs, so 8 is far from the decision boundary; it keeps one-off channel anomalies from latching a permanent group.
        bool   uwl_cold_fix{ false };         ///< Single-epoch cold fix (§3.3): a top-tier arc with one unlagged sample (n = 1) is eligible for an FFR decision using the a-priori σ below, on tiers with λ ≥ @c uwl_cold_min_lambda_m only.  Decision side only (which integers the bridge may use); folds nothing; removes the measured ~3-epoch re-warm latency after a slip.
        double uwl_cold_sigma_m{ 0.7 };       ///< A-priori per-sample MW statistic σ (m), substituted while the Welford variance is immature (n < 2).  Applied per arc: a re-warming arc inside an otherwise mature cycle contributes about this/λ_top cycles to the row σ (0.7 m gives 0.072 cycles at λ 9.768 m).  A cycle whose arcs are all cold aggregates to a σ larger by √Σc² and fails the default FFR σ gate, as intended.
        double uwl_cold_min_lambda_m{ 8.0 };  ///< Wavelength floor for cold-fix eligibility (m).  Excludes the 5.861 m GPS EWL by design: the cold fix is reserved for UWL wavelengths and is not extended to GPS.
        bool   bridge_sample_debias{ false }; ///< Fold each bridge sample as GF_k − C_iono·ı̃(t_k) (per-sample de-bias by the estimable ionosphere, pushed with a one-epoch lag) instead of subtracting C_iono·ı̃(now) from the lifetime mean at read time.  The statistic becomes stationary, which removes both the mean bias and the drift-inflated Welford σ that blocked re-decisions as the whole-arc Welford aged (without it, constrained-lane circR fell from 0.89 to 0.24 over 60 epochs).  S-conformant: Σc·ı̃ over a closed cycle = ∇ΔI exactly (Odijk 2016, Table 8: the S-basis lumps cancel by sign balance).  Requires @c ewl_enable (the bridge).  Off: raw accumulation with read-time de-bias, byte-identical.
        bool   d5_dump{ false };              ///< Diagnostic (log-only, F-A): per-epoch CSV of every D5 innovation flag of the invariant carry (group id, chord (u,v), station, ambiguity key, formally standardized innovation, and adjacency tags {EWL-fixed (station,sat) member, hold-cycle chord, chord injected in the previous epoch}) to logs/network_d5_flags.csv.  Does not change the solve.
        bool   lane_trace{ false };           ///< Instrument (log-only, F-B): per-epoch CSV of every primary cycle's carried WL lane value and fractional part after install, for both the held or injected population and the untouched control population in one run, to logs/network_wl_lane_trace.csv.  Graded by scripts/grade_lane_trace.py, which resolves the injected-cycle circR that the star dd-frac trace cannot.  Requires @c wl_inject_enable or @c lane_carry.enable (WL-3 acceptance instrument).  Does not change the solve.
    };

    /// SD pre-reduce EWL constraint.  The between-satellite single-differenced
    /// extra-wide-lane MW statistic is folded as a pseudo-observation row into each
    /// station's own pre-reduce block, so it reaches the shared satellite phase and
    /// code bias columns directly instead of through a DD chord.  Most DD chords
    /// (80.7 %) are eq34 tree pins, and folding the EWL rows as chords
    /// (@c mw_wl.ewl_fold_rows) moved the soft-end eigenvalue of reduced_N_ss by only
    /// 0.6 % and mass_pb/span_pb not at all.  Folded before the reduction, the row
    /// composes exactly with the post-reduce DD fold of the invariant carry
    /// (@c DdPriorBlock in NetworkNormalEquations.hpp), unlike two post-reduce
    /// Woodbury folds over coupled local columns, which drop the cross term.
    ///
    /// The row must be between-satellite: the eq34 gauge is per (constellation,
    /// signal), so an undifferenced cross-signal bias difference is not estimable
    /// (the per-component constants c_b1 − c_b2 are free).  The between-satellite SD
    /// cancels them and leaves exactly the EWL integer lattice {lam_b1*m − lam_b2*n}.
    /// See docs/SPEC_sd_ewl_prereduce.md §2.
    ///
    /// Increment 1 folds float rows.  Increments 2a and 2b (@ref fix_enable below)
    /// decide and fold the integer; the zero-q hold (increment 2c) is not built.  The
    /// network producer is float-only by design, because a wrongly fixed integer
    /// corrupts the emitted product for every consumer.
    struct SdEwlConfig {
        bool   enable{ false };               ///< Master switch.  Off appends zero rows (byte-identical).
        int    min_arc_epochs{ 10 };          ///< Minimum lagged Welford count on both arcs of a row (mirrors mw_wl.min_arc_epochs).
        double sigma_floor_m{ 0.05 };         ///< Per-arc sigma floor (m) applied to var_of_mean before the R assembly: arcZdMean's variance of the mean shrinks as 1/n, so a long quiet arc would otherwise dominate the shared border.
        double max_abs_z_m{ 0.0 };            ///< Reject a row whose |z_SD| exceeds this (m).  0 = disabled.  Guards against the cold-clock x_lin seed at station 1, which ill-conditions correction-frame rows.
        int    max_rows_per_station{ 0 };     ///< 0 = unlimited; else keep the lowest-variance rows first (drops counted).
        bool   trace{ false };                ///< Log-only per-row CSV (logs/network_sd_ewl.csv).  Does not change the solve.

        // --- Increments 2a and 2b: fix the EWL integer (docs/SPEC_sd_ewl_prereduce.md
        // §8, as amended by docs/SPEC_sd_ewl_prereduce.md §10).  Increment 2c (the
        // zero-q hold) is not built, by design: the decision is re-derived every epoch,
        // so there is no stiff cross-epoch carry that would need a slip response.
        /// Master switch for the integer path.  On: @ref networkSdEwlAr decides this
        /// epoch's SD EWL integers by Route B (between-station DD, which needs no
        /// a-priori satellite bias and so cannot inherit an error from one), and the
        /// row builder folds the fixed value, removing the local ambiguity degree of
        /// freedom instead of estimating it.  This is where increment 2 adds value: a
        /// float MW row adds no new rank (measured: +0.51 % of ev1 at a realistic
        /// sigma), because integrality is the only content the normal equations do
        /// not already hold.  Off: no integer is decided and every row takes the
        /// increment-1 float path (byte-identical).  Requires @ref enable.
        bool   fix_enable{ false };
        /// Minimum rounding success probability (FFR, not ADOP, per the standing AR
        /// criterion) for a DD integer to be usable: @f$\mathrm{erf}(0.5/(\sigma\sqrt2))@f$
        /// with sigma the DD's own formed sigma in cycles.  0.999 is decisive on the
        /// EWL lanes at lambda 5.861 / 9.768 m and still rejects a 2-sample arc.
        double fix_min_ffr{ 0.999 };
        /// Reject a DD whose |fractional residue| exceeds this (cycles).  0.15 mirrors
        /// the MW detector's own confident-integer-jump gate, and the measured EWL DD
        /// fractional spread is 0.019-0.029 cyc (circR 0.96-0.98), so the gate is loose
        /// by design: it screens gross mis-pairings, not noise.
        double fix_max_frac_cyc{ 0.15 };
        /// Per-sample MW noise scale (m) (amendment A1).  One knob serves two
        /// quantities that both derive from the per-sample noise:
        ///  * Decision: the per-arc variance of the mean is floored at
        ///    @f$\sigma^2/n@f$, because @c arcZdMean serves a variance that shrinks as
        ///    1/n and measured 13-60x optimistic against the rows' own residual spread
        ///    (median 1.02 m); an optimistic sigma inflates FFR and admits wrong fixes.
        ///  * Fixed row: the row sigma is floored at this value flat (per sample, not
        ///    per mean).  @c arcZdMean is arc-cumulative, so consecutive-epoch rows
        ///    share n-1 of n samples (rho ~ 1 - 1/2n); refolding a variance of the mean
        ///    as white every epoch over-claims information as sqrt(k), and the
        ///    over-claim grows as the fixed-row residuals drop to code-noise scale.
        ///    Folding the current mean at per-sample weight adds about one sample of
        ///    information per epoch instead: asymptotically consistent, with no
        ///    detector change.  1.0 m is the point the increment-1 ladder measured as
        ///    consistent with the residuals.
        double fix_sigma_floor_m{ 1.0 };
    };

    /// Lane-coordinate carry (M5 card WL-3).  Carries each primary-pair chord's two
    /// band DDs as one jointly-covarianced block: stiff along the phase-determined NL
    /// direction, loose along the WL lane as its uncertainty warrants, with per-epoch
    /// MW and bridge lane increments as measurements.  The estimator then owns the
    /// code-coherence accumulator, which removes the F-A/F6 refold class by
    /// construction.  Built in docs/SPEC_lane_carry_card.md §7, increments 2-4:
    /// pair-group regroup and equivalence shape, lane prior/q and per-epoch MW lane
    /// rows, bridge rows and fold-on-change FFR fixing.  Supersedes
    /// mw_wl.wl_inject_enable at runtime (with a warning); the inject primitive
    /// remains as the pair-block fallback.
    struct LaneCarryConfig {
        bool   enable{ false };               ///< Master switch.  Requires carry_invariant and mw_wl.ewl_enable (the statistics source).  Off is byte-identical.
        double q_wl_scale{ 0.01 };            ///< q along the WL lane = this × carry_invariant_qz.  The default 0.01 gives about (0.04 cyc)² per epoch at qz = 1e-4 per 30 s, which keeps the √N lane accumulation and the FFR fixes working (measured circR 0.471 against 0.251).  The SPEC §3.1 factor of 10³ assumed qz ≈ 0 and would swamp every lane measurement (about 12 WL cycles of lane q per epoch).  1.0 = isotropic (part of the §6.1 equivalence shape).
        double sigma_wl_fresh_cyc{ 0.0 };     ///< Fresh-chord lane prior σ (cycles).  Default 0 = diffuse birth.  Measurement falsifies the SPEC §3.1 mean-zero code-seed prior: seed-time clock diversity mis-centers the correction-frame lane origin by O(10²) cycles (D5 flags 691 against 125).  The code-seed information enters through the first MW lane increment instead.  > 0 = study shape (logged with a warning).
        bool   lane_meas{ true };             ///< Per-epoch MW(1,2) lane increments folded into the carried block (SPEC §3.2).
        bool   bridge_meas{ true };           ///< Per-epoch iono-de-biased bridge increments on EWL-fixed cycles (phase-precision lane source).
        // --- Performance knobs, not lane-shape knobs (LF-1 perf card,
        //     docs/SPEC_lane_carry_card.md Appendix LF-1).  They must stay out of the
        //     equivalence_shape discriminator because they do not change what folds. ---
        bool   fold_blocked{ false };         ///< Optimized blocked/sparse lane fold: O(K) sparse applyLaneFix, with the per-row re-symmetrize hoisted to one pass per block per epoch (LF-1 INC-1).  Changes the operation order, so it belongs to the parallel_carry equivalence class and is not bit-identical (§3.2).  Off: the sequential per-row fold (byte-identical).
        bool   skip_pinned_increments{ false };///< Skip the per-epoch MW/bridge lane increments for a chord with a committed decision: its lane is pinned at σ = 0.01, so an increment with σ ≈ 0.06–0.2 carries no information (LF-1 INC-2, §3.3).  Any slip, D5 flag or re-tap resumes increments in the same epoch.  Equivalence class (nearly inert, not exactly null).  Off is byte-identical.
        long long skip_pinned_within_n{ 3 };  ///< Skip grace window (epochs) after a chord's last re-decision; past it, increments resume so that a drifting lane re-converges (LF-1 INC-2).  Inert unless skip_pinned_increments.
        bool   parallel_components{ false };   ///< Spread the per-epoch MW lane-increment folds across cores, one component (joint block) per thread (LF-1 INC-4, §3.5).  Components are independent (no shared Pᶻ) and each block's folds stay in order, so the result is byte-identical to serial (unlike fold_blocked).  Takes effect only with parallel_stations (n_threads > 1); inert unless lane_joint.  Eigen is re-pinned to one thread inside the region; the pcarry un-pin stays outside it (the dev300s nesting trap).
    };

    /**
     * @brief Slip-QC hardening knobs (docs/SPEC_slip_qc_hardening.md).
     *
     * Three independent cards, each off by default, that qualify existing slip
     * decisions.  They never drop a station, satellite or observation (project
     * rule 5), and every gate derives from the observable stream itself, never
     * from the stochastic model (rule 4).  All keys live under
     * @c network.cycle_slip.*, beside the detector arms they harden.
     *
     *  - The IF-TD reference-satellite slip gate (card R, @c if_phase_td_ref_*)
     *    becomes scale-aware, coherence-discriminated, widened on thin families
     *    and persistence-capped.  See IfPhaseSlipDetector::RefGateParams.
     *  - LLI corroboration (card L, @c mw_lli_*): at a station whose LLI behaviour
     *    is demonstrably not credible, an LLI assertion must be corroborated by an
     *    empirical MW/GF step; elsewhere LLI stays decisive.
     *  - Gap instrumentation (card G, @c gap_instrument), counters only: arcs that
     *    resume across a tracking break with a shifted raw level, and whether they
     *    re-entered with bridged carry state.
     *
     * With all three masters off, the producer is byte-identical.
     */
    struct SlipQcConfig {
        // --- Hardened IF-TD reference-satellite gate (card R) --------------
        bool   ref_gate{ false };            ///< Master arm (network.cycle_slip.if_phase_td_ref_gate).
        /// Run the reference arm on the additional gradient chains of
        /// `if_phase_td_pairs = all` as well
        /// (`network.cycle_slip.if_phase_td_ref_arm_all`) (REF-FAN,
        /// SPEC_event_harness §22.6).  Without it the arm runs on the first chain
        /// only (§14.10.4).  A slip on an additional chain's reference, whose own
        /// double difference is identically 0 by construction, is then never
        /// attributed; it spreads across every other satellite of that chain as
        /// false IF-TD refloats and can re-realize the network-wide phase-bias
        /// datum.  On: a reference the chain elected itself is tested on that chain
        /// through the empirical gate with the chain's own dispersion state; an
        /// adopted one inherits the first chain's verdict.  Requires
        /// `if_phase_td_pairs = all` and @ref ref_gate: the legacy candidate
        /// condition |median| > 0.5·λ_IF is met by noise on the wide combinations
        /// and must not gate one again (§14.10.3).  Otherwise the loader warns and
        /// disarms the knob, and the detector refuses it a second time.  Default
        /// false (byte-identical).
        bool   ref_arm_all{ false };
        double ref_k{ 6.0 };                 ///< Empirical multiplier on the effective DD-dispersion scale.
        int    ref_min_n{ 4 };               ///< Minimum non-reference DDs before the median is trusted.
        double ref_thin_widen{ 3.0 };        ///< Physical-floor multiplier below @ref ref_min_n.
        double ref_coherence{ 0.5 };         ///< Max MAD(non-ref DDs)/|median| for a common step (separates a step from drift).
        /// Consecutive fires on one reference before escalating to re-selection.
        /// Default 1, the literal reading of spec R2 ("a genuine ref slip fires
        /// once"): a real one-epoch common step never fires twice in a row, so the
        /// cap costs it nothing, while a persistent drift is escalated on its second
        /// epoch instead of its third.  On the audit config, 2 left a drift family
        /// re-qualifying in fire-fire-escalate cycles; 1 halves that.
        int    ref_max_consecutive{ 1 };
        double ref_ewma_alpha{ 0.15 };       ///< EWMA weight for the dispersion scale.
        double ref_shrink_kappa{ 20.0 };     ///< Empirical-Bayes shrinkage constant (samples).
        double ref_floor_frac{ 0.5 };        ///< Physical floor in units of λ_IF (0.5 = the legacy fixed gate).
        /// Abstain while the empirical scale holds fewer than @ref ref_shrink_kappa
        /// samples (network.cycle_slip.if_phase_td_ref_warmup_abstain).  Without it
        /// the cold prior makes the hardened gate revert to the legacy fixed bar for
        /// its first few epochs, where every fire is only `diagnostic`: carry_warm is
        /// false and a cold detection cannot act.  §6.14.6 item 1.
        bool   ref_warmup_abstain{ false };
        /// Elevation-normalize the empirical scale and rebuild the bar as the sigma of
        /// the median (network.cycle_slip.if_phase_td_ref_elev_norm): the EWMA stores
        /// disp·⟨sin el⟩ and the test forms hypot(s_z/sin(el_ref), 1.2533·s_raw/√n).
        /// The flat k·s_eff form ignores both el_ref and n, and its state outlives the
        /// reference satellite it describes.  §6.14.6 item 2.
        bool   ref_elev_norm{ false };
        double ref_elev_clamp_deg{ 5.0 };    ///< Elevation clamp for @ref ref_elev_norm (matches the pool arm's).

        // --- LLI corroboration in the MW drain (card L) --------------------
        bool   lli_corroborate{ false };     ///< Master arm (network.cycle_slip.mw_lli_corroborate).
        double lli_chatter_frac_hi{ 0.6 };   ///< LLI-set-fraction EWMA above which a station is demoted to corroboration-required.
        double lli_chatter_frac_lo{ 0.3 };   ///< …and below which it is promoted back (hysteresis band).
        int    lli_min_epochs{ 1 };          ///< EWMA history before the two-state verdict may move.
        double lli_ewma_alpha{ 0.4 };        ///< EWMA weight of the current epoch's LLI-set fraction.
        double lli_k_mw{ 5.0 };              ///< MW corroboration gate = max(k·scale, floor_cyc·λ_WL).
        double lli_k_gf{ 5.0 };              ///< GF corroboration gate = max(k·scale, floor_m).
        double lli_mw_floor_cyc{ 0.5 };      ///< Physical MW floor in wide-lane cycles (the audit's support test).
        double lli_gf_floor_m{ 0.04 };       ///< Physical GF floor in metres (the audit's support test).
        double lli_step_alpha{ 0.15 };       ///< EWMA weight for the per-station raw-step scales.

        // --- Gap-coincident slip instrumentation (card G) ------------------
        bool   gap_instrument{ false };      ///< Master arm (network.cycle_slip.gap_instrument).  Counters and trace rows only.

        // --- Gap re-entry response: mark a slipped re-entry at the re-entry (card G) ---
        /**
         * @brief Master arm (@c network.cycle_slip.gap_reentry_relevel) for the gap
         *        re-entry slip response (docs/SPEC_slip_qc_hardening.md §G3/W1, the
         *        follow-up that card G's decision rule called for).
         *
         * Solve-affecting, unlike @ref gap_instrument: it marks ambiguities slipped,
         * which frees the zero-q phase-leveling level for one epoch.  Default off
         * (byte-identical).
         *
         * An arc that reappears within @c network.phase_leveling_max_gap_s has no
         * detector covering its return epoch (MW needs an immediately adjacent
         * predecessor, IF-TD needs three consecutive epochs, and project rules bar
         * the GF integer path at 30 s).  Its pre-gap leveling carry is therefore
         * re-folded at converged weight across a break that, according to the raw
         * MW/GF level or the receiver's own LLI, it did not survive, and the
         * station's other states (ZWD among them) absorb the tension.  Standing
         * invariant #1.
         *
         * Arms the arc-resumption collection by itself (it does not require
         * @ref gap_instrument) and interlocks with @c cycle_slip.relevel_free_epoch
         * on the leveling vehicle: without that latch, the PASS-B pre-pass consumes
         * the pending-reseed mark before the inject reads it, so the loader warns
         * and the drain counts the arcs it would have freed instead of doing
         * nothing unnoticed.
         */
        bool   gap_reentry{ false };
        /// Sub-gate (@c network.cycle_slip.gap_reentry_relevel_lli, default on): a
        /// RINEX LLI assertion on either band of the primary pair at the return epoch
        /// counts as slipped even when the raw level did not clear the empirical
        /// gate.  LLI is the receiver's own loss-of-lock verdict and the only
        /// evidence an unmeasurable re-entry can carry; the measured field case
        /// asserted it on both bands.
        bool   gap_reentry_lli{ true };
        /// Sub-gate (@c network.cycle_slip.gap_reentry_relevel_unmeasurable,
        /// default off): treat a re-entry with no pre-gap dual-band level and no
        /// LLI as slipped.  Off by default because absence of evidence is not
        /// evidence of a slip; on, it is the conservative-reset posture (the
        /// unmeasurable bridged population measured 24 arcs, against 88 shifted).
        bool   gap_reentry_unmeasurable{ false };

        // --- Re-entry keyed on the signal (card GRS) -----------------------
        /**
         * @brief Per-signal rider (@c network.cycle_slip.gap_reentry_per_signal) of
         *        @ref gap_reentry (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md).
         *
         * The row-level response reacts to a raw row that went absent, and the row
         * is rebuilt each epoch from whichever phase signals have a matching code.
         * A receiver that loses lock on the semi-codeless L1/L2 phases while a civil
         * signal keeps tracking therefore keeps its row, returns with an adjacent
         * predecessor and never counts as a re-entry: no detector covers the return
         * epoch, and the returning phases' carried ambiguities fold across the
         * break.  @c raw_observed carries band 5 even on a dual-frequency run, so
         * this blind spot is not confined to the triple posture.
         *
         * Solve-affecting; default off (byte-identical).  Armed without
         * @ref gap_reentry, the loader warns and this tier stays inert: it
         * de-duplicates against the row-level resumption list, which is not
         * collected then, so the two tiers could count one break twice.
         */
        bool   gap_reentry_per_signal{ false };
        /// Sub-gate (@c network.cycle_slip.gap_reentry_per_signal_unmeasurable,
        /// default on): free a per-signal re-entry that has no usable partner to
        /// difference the level against.  The default is the opposite of
        /// @ref gap_reentry_unmeasurable on purpose: a partial dropout with no
        /// witness is the measured failure case, and unlike after a whole-satellite
        /// gap, the carry of a satellite that never left is certain to be folded.
        bool   gap_reentry_per_signal_unmeasurable{ true };

        // --- The tracker's signal per band, keyed on the model (card GRK) --
        /**
         * @brief Arm (@c network.cycle_slip.tracker_modeled_first) for the
         *        arc-level tracker's per-band signal choice
         *        (docs/refound/GRK_gap_reentry_key_mismatch_2026-09-22.md).
         *
         * Unarmed, the arc-level tracker builds its primary pair from
         * @c raw_observed as the lexically lowest phase per band that has a
         * matching code, so a station reporting L1C and L1W tracks band 1 as L1C.
         * The station's carried ambiguity is keyed on its canonical band-1 signal,
         * which under @c processing.canonical_signal_rule = priority is L1W at the
         * stations that report both.  The gap re-entry response then looks the
         * carry up under the tracker's key, finds nothing, counts @c no_carry and
         * returns, and the stale band-1 level folds across the break.
         *
         * Armed, the tracker prefers on each band the phase signal the station
         * latched for that band (@c PppModel::latchedCanonicalPhaseOnBandDigit),
         * and falls back to the lexically lowest raw phase with a matching code
         * only where the latch names none or the latched phase is not reported
         * this epoch (band 5 on a dual posture).  The tracker, the carry and the
         * response then agree by construction.
         *
         * The tracker reads the latch rather than the epoch's admitted row set
         * because the admission and readmit machinery can withhold a returning
         * arc's phase from the admitted set for its first epochs, and the re-entry
         * epoch is the one the tracker has to name correctly.
         *
         * Solve-affecting through the detections and the response it enables;
         * default off, so an unarmed run is byte-identical.  Not fingerprinted: the
         * choice is forward-only and no carried quantity changes meaning across a
         * flip.  The per-signal tier of card GRS is unaffected (it keys on the
         * signal itself).
         */
        bool   tracker_modeled_first{ false };

        // --- Oracle slip response, simulation only (the level-preserving ceiling) ---
        /**
         * @brief Master arm (@c network.cycle_slip.oracle_relevel) for the
         *        truth-fed oracle slip response (include/slip/NetworkSlipOracle.hpp).
         *
         * When a detector actuates a refloat on a carried arc, the oracle looks up
         * the true integer jump(s) in the fixture's `truth_slips.csv` and shifts the
         * carried level by exactly the sum d*lambda instead of freeing the arc.
         * This measures the ceiling of a level-preserving re-level before an
         * engineering build (EWL / WL-MW integer estimation) is attempted.  The
         * truth-graded census showed that the standard response injects about 2.5×
         * the bed's ambient DD error, takes about 20 min to walk back, and feeds the
         * metre-class ZWD plateau.
         *
         * Solve-affecting; default off (byte-identical).  Simulation only, and inert
         * on field data by construction: without a readable @ref oracle_truth_root
         * the loader fails with an error, and setup fails on any roster station
         * missing its truth file.  A diagnostic: not for promotion and never in a
         * golden config.
         *
         * Interlocks: requires @c network.phase_leveling (the carry vehicle it acts
         * on is @c LocalCarry::value_abs), and is disabled under
         * @c network.carry_invariant (that vehicle has no LocalCarry).
         */
        bool        oracle_relevel{ false };
        /// Fixture root (@c network.cycle_slip.oracle_truth_root): the directory
        /// holding `<STATION>/truth_slips.csv` for every roster station, e.g. the
        /// `simnet60_campaign_wu93` fixture directory.  A hard error when
        /// @ref oracle_relevel is on and the directory does not exist; that error is
        /// what keeps the oracle inert on field data.
        std::string oracle_truth_root{};
        /// 1-sigma (cycles) of a deterministic zero-mean Gaussian added to each
        /// applied sum d (@c network.cycle_slip.oracle_noise_cyc, default 0 = the
        /// exact ceiling).  Seeded from (station, satellite, signal, epoch), so a
        /// re-run reproduces every draw.  This is the knob of a precision ladder:
        /// how good the integer estimate has to be.
        double      oracle_noise_cyc{ 0.0 };
        /// Action when a detector fires on a carried arc with no truth slip in the
        /// window (@c network.cycle_slip.oracle_false_alarm): `"keep"` (default)
        /// leaves the carry untouched, which is the ceiling (an oracle knows the fire
        /// was a false alarm); `"free"` falls through to the standard response, so
        /// the arm measures the repair alone and keeps the false-alarm cost.  Any
        /// other value is a loader error.
        std::string oracle_false_alarm{ "keep" };
        /// Action when a detector fires on a carried arc and truth does place a slip
        /// in the window (@c network.cycle_slip.oracle_true_slip): `"repair"`
        /// (default) is the ceiling, shifting the carried level by the true sum
        /// d*lambda; `"free"` declines the arc, so the caller runs the ordinary
        /// free/re-level response for it.
        ///
        /// `"free"` isolates the false-alarm cost.  Paired with
        /// @ref oracle_false_alarm `"keep"`, the only difference from the base arm is
        /// that actuations with no truth slip in the window keep their level, while
        /// every real slip still takes the standard response.  The arm thus shows
        /// whether the detector's false alarms are a material injection source or
        /// whether the true-slip re-level accounts for the whole effect, without
        /// confounding the two.  Any other value is a loader error.
        ///
        /// `"free"` makes @c applied 0 by construction, so under that policy the
        /// run-end armed-and-inert check reads @c fa_kept instead.  `"free"` together
        /// with @ref oracle_false_alarm `"free"` is a pure no-op arm (both halves
        /// decline); the loader warns about it rather than refusing it, because it
        /// is a legitimate byte-identity control.
        std::string oracle_true_slip{ "repair" };
        /**
         * @brief Detector-latency window in seconds
         *        (@c network.cycle_slip.oracle_window_s, default 90 = three 30 s
         *        epochs, the latency the truth census counts as detected).
         *
         * The oracle's truth-slip window is
         * `(max(last_applied_s, carry.birth_s, now_s − oracle_window_s), now_s]`.
         * This term makes a late detection repairable: a detector normally fires
         * 1–3 epochs after the slip, and the intervening solves do not re-set a
         * zero-q carried level (the stiff prior barely moves), so the jump is still
         * unabsorbed even though @c LocalCarry::last_seen_s has advanced past it.
         * Bounding the window at the latency, rather than at the carry's last
         * stash, separates a late detection from a real false alarm.  Without it,
         * the census labels most of the normal population as false alarms, and the
         * ceiling it reports is a strongly biased lower bound.
         *
         * Clamped to >= 0 by the loader.  0 is a degenerate setting, not "same
         * epoch only": the lower bound is exclusive, so a window of 0 collapses to
         * `(now_s, now_s]` and admits nothing.  Every actuation then reports a false
         * alarm, and the run-end line issues its armed-and-inert warning.  A
         * same-epoch-only arm needs a value in (0, one epoch]; the default 90 s
         * admits the three 30 s stamps `now`, `now − 30` and `now − 60`.
         */
        double      oracle_window_s{ 90.0 };
    };

    /**
     * @brief Flicker / gap signal-integrity knobs
     *        (docs/SPEC_flicker_gap_integrity.md).
     *
     * ## Rationale
     *
     * Across any loss of lock the slip magnitude is unbounded and cannot be known:
     * the phase counter is arbitrary at re-acquisition, and a proportional L1/L2
     * counter reset is invisible to both the geometry-free and the Melbourne-Wübbena
     * combinations.  Recovery is safe only if every path treats a re-entry as fresh
     * and anchored to code, and if the first epochs after recovery, where no
     * time-difference detector can operate at all, never carry full phase weight.
     * That blind window is structural, not a tuning problem: MW needs Welford
     * history, IF-TD needs three consecutive epochs, LLI is at the receiver's
     * discretion, and project rules bar GF at 30 s.
     *
     * Measured case, G21 at a JAVAD TRE_3 DELTA station within ten minutes of raw
     * RINEX: a mid-arc GF step with no receiver flag; L2W missing for one epoch; a
     * re-entry with LLI=1 on both bands; then a further unflagged step.  Both
     * unflagged slips fell where MW and IF-TD were structurally blind (arc 0-1
     * epochs old).  Scale: about 1.7 (station, satellite) re-entries per epoch over
     * 24 h at 63 stations, so this is routine, not a corner case.
     *
     * ## What the block does (four increments, one master arm)
     *
     *  - INC-0, `census`: the master.  Classifies every present
     *    (station, satellite) line of sight each epoch and counts the behaviour:
     *    re-entries by outage length, band losses and regains, DEGRADED
     *    continuations (a dual-born arc still contributing with fewer bands), and
     *    standing single-frequency arcs.  Diagnostics only.
     *  - INC-1: the band-state classifier itself (STABLE / NEW / REENTRY /
     *    FLICKER-REGAIN / DEGRADED), which feeds INC-2 and INC-3.
     *  - INC-2, `unvetted_sigma_scale` / `withhold_instead`: while a LOS is not
     *    STABLE, its phase rows fold at an inflated σ (fallback in preference to
     *    exclusion, `feedback_prefer_fallback_over_drop`).  Code rows are
     *    untouched: they are immune to slips, and they re-anchor the float.
     *  - INC-3, `refloat_on_regain` / `revet_ep`: a band that returns without a
     *    producer-registered gap refloats the LOS's ambiguities on all bands
     *    through the existing slip funnel (reset rather than repair,
     *    `feedback_reset_over_repair_biasless`), and STABLE is re-earned only after
     *    @ref revet_ep consecutive full-band epochs.
     *
     * With @ref census false the whole block is inert: no map is allocated, no row
     * weight changes, no observable is touched, and the producer is byte-identical.
     * Deliberately not part of the warm-resume fingerprint: it is a forward-only
     * weight and refloat policy that changes no serialized state's meaning (as with
     * `adaptive_iono_q` and the eq34 hygiene), which keeps a shared-snapshot A/B
     * valid.
     */
    struct SignalIntegrityConfig {
        /// Master arm (@c network.signal_integrity.census).  On: the classifier
        /// runs and every counter below is live.  INC-2 and INC-3 require it, since
        /// they act on the band state it tracks; otherwise the loader warns and
        /// disarms them.
        bool   census{ false };
        /// σ multiplier on the phase rows of a LOS that is not STABLE
        /// (weight /= scale²).  <= 1 disables the demotion; that is how the spec's
        /// §6C census-only leg is expressed (`census: true`,
        /// `unvetted_sigma_scale: 1.0`, `refloat_on_regain: false`).  <= 0 is an
        /// alternative spelling of @ref withhold_instead.
        double unvetted_sigma_scale{ 10.0 };
        /// Consecutive full-band epochs a LOS must accumulate after any transition
        /// before its phase returns to full weight (INC-3(b)).  Default 3, so that
        /// MW (2 time-difference samples) and IF-TD (3 epochs) are both live for at
        /// least one epoch before the vet ends: the vet ends exactly when
        /// monitoring begins.
        int    revet_ep{ 3 };
        /// A reference band absent for at most this many consecutive epochs while
        /// the LOS itself stayed present is a flicker; its return is a
        /// FLICKER-REGAIN (refloat).  Beyond that the band is treated as gone and the
        /// LOS re-vets on the reduced reference set, so a permanently
        /// single-frequency arc is not demoted forever (spec §7 defers that policy
        /// until after the census).
        int    flicker_max_ep{ 5 };
        /// On a FLICKER-REGAIN, refloat the LOS's ambiguities on all bands through
        /// the slip funnel (INC-3(a)).  Default true when the block is armed: the
        /// G21 case shows that a flicker window can slip both bands and that nothing
        /// bounds the slip sizes, so reset rather than repair is the only safe
        /// response.
        bool   refloat_on_regain{ true };
        /// Alternative INC-2 disposition: instead of deweighting, remove the
        /// unvetted LOS's phase observables for the epoch (code rows remain).
        /// Available but not the default: withholding a phase row can also remove
        /// the only support of an elected datum pin, leaving the rank gate as the
        /// backstop.  Arm only once the W5 gate is graded.
        bool   withhold_instead{ false };
        /**
         * @brief Vet every arc for @ref revet_ep epochs after a warm resume (spec
         *        addendum).  Default true when the block is armed.
         *
         * A warm resume is the network-wide analogue of the flicker case.  The
         * snapshot restores tight carries but no detector state: nothing in
         * @c NetworkStateSnapshot serializes MW Welford counts or IF-TD arc history,
         * so both re-converge from scratch.  The first `revet_ep` epochs of every
         * resumed run therefore have fully warm state and no time-difference
         * coverage, which is the flicker hazard at network scale.
         *
         * Trade-off to note when grading: this perturbs the first K epochs of a
         * replay leg relative to the continuous run (a branch-faithfulness cost).
         * That is acceptable for event-replay legs seeded >= K epochs before the
         * event; a byte-grade resume A/B must arm or disarm it identically on both
         * legs.  The alternative, serializing detector arc state into the RTB-7
         * snapshot, is a format change and is deferred.
         */
        bool   resume_warmup_vet{ true };
    };

    /**
     * @brief Producer-side network ambiguity-resolution gates.
     *
     * Controls integer fixing of the network's own between-station,
     * between-satellite double-difference ambiguities, so that the emitted
     * satellite phase biases are integer-leveled (the integer part referred to a
     * stable network datum, the fractional part the satellite UPD).  Disabled by
     * default; the float product is bit-identical until @c enable is set.  The
     * gates are stricter than on the single-station user path because a wrong
     * network fix corrupts the product for every downstream user (see
     * @c docs/network_ambiguity_resolution_design.md §5).
     */
    struct NetworkArConfig {
        bool   enable{ false };               ///< Master switch; off leaves the float product unchanged.
        double min_success_rate{ 0.9999 };    ///< FFR bootstrap acceptance gate (stricter than the user's 0.999).
        double ratio_threshold{ 3.0 };        ///< LAMBDA ratio-test backstop.
        double ratio_upper_bound{ 1e4 };      ///< Reject ratios above this (degenerate covariance).
        double nl_max_per_pair_frac{ 0.10 };  ///< Max per-DD |float − fixed| (cycles).
        bool   enable_partial_ar{ true };     ///< Fix the decisively-fixable subset; leave the rest float.
        bool   wl_cascade{ false };           ///< WL->NL cascade (needs per-station MW aggregation; off in V1).
        /// Card ARL: fix in the decorrelated space (ils::ilsFixDecorrelatedPartial) instead of
        /// the entry-by-entry partial fix: the trailing decorrelated components whose bootstrap
        /// success rate clears min_success_rate are searched, and a single-signal DD is reported
        /// fixed when the fixed components determine it.  Off (default) = the existing search.
        bool   decorrelated_partial{ false };
        /// Conditional sigma (cycles) at or below which a single-signal DD counts as determined
        /// by the fixed decorrelated components (decorrelated_partial only).
        double decorrelated_determined_sigma_cyc{ 0.05 };
        /// Card ARL: build the search's DD basis as a greedy spanning tree of station baselines
        /// (each new station attached to the in-tree station it shares the most satellites with;
        /// one DD per common satellite against the first common one) instead of the star (one
        /// reference station and satellite), which reaches only the reference station's sky.
        /// Off (default) = the star.
        bool   tree_basis{ false };
        int    min_epochs{ 10 };              ///< Let the network float converge before fixing.
        int    ils_max_dim{ 0 };              ///< Cap on the ILS (MLAMBDA) search dimension: when > 0, only the
                                              ///< @c ils_max_dim lowest-formal-σ DDs enter the integer search
                                              ///< (ascending @f$Q_{aa}@f$, the standard partial-AR subset), and
                                              ///< the rest stay float.  0 (default) = uncapped legacy behaviour.
                                              ///< Required at scale: the uncapped ~2500-dimensional basis at
                                              ///< 305 stations does not return; about 100 is a reasonable value.
        /**
         * @brief Per-DD trace of the Phase-2 search (@c logs/network_producer_ar.csv).
         *
         * Without it, the Phase-2 diagnostic writes one spdlog line per epoch with a
         * fix count, which is not a grade (docs/network_ambiguity_resolution_design.md,
         * 2026-09-10 note §4.1).  With it, every DD in the basis gets a row with its
         * identity (constellation / signal / sat_j / ref_sat / sta_r / ref_sta), its
         * float value and formal σ in cycles, whether it entered the ils_max_dim
         * search subset, and, for the DDs the ILS returned, the fixed integer, so the
         * search can be joined to truth offline (scripts/simnet_producer_ar_grade.py).
         * Requires @ref enable (the search produces the rows); the file is not opened
         * without it.  Read-only: the emitted product is byte-identical with it on
         * or off.
         */
        bool   trace{ false };
    };

    /**
     * @brief Producer a-priori satellite orbit/clock source for the PPP-RTK
     *        network producer (@c network.apriori_source).
     *
     * Selects what the network producer's per-satellite a-priori clock (and the
     * orbit its geometry uses) is referenced to; the estimable satellite-clock
     * state then absorbs the error of that a-priori.  Frozen conventions:
     * @c docs/rt_service_conventions.md §1 and the card decomposition in
     * @c docs/rt_service_conventions.md §6 (D1–D10) / §7 (Cards 1–8).
     */
    enum class NetworkAprioriSource {
        Sp3,      ///< (default) Resolved precise SP3(+CLK) files; the emitted clock C0 is a delta against the SP3/CLK product identity (the original behaviour, byte-identical).
        BrdcRts,  ///< Broadcast ephemeris (@c LiveEphemerisStore) + external IGS-SSR (4076) orbit corrections; the emitted C0 is a delta against broadcast(IODE) + the applied RTS orbit (the self-sufficient real-time service mode).
        /// SP3 orbits and the broadcast clock of the navigation file, no CLK product (card
        /// NBP).  The a priori clock is continuous across ephemeris cut-overs through a
        /// per-ephemeris offset table (@c BrdcClockApriori); the wire's clock refers to the
        /// stamped ephemeris (C_wire = C_est + O, @c iode=k).  Keeps sp3's anchor-sigma auto.
        Sp3BrdcClock
    };

    /**
     * @brief Ephemeris selection rule of the native broadcast clock a priori
     *        (@c network.brdc_select, card NBP).
     */
    enum class NetworkBrdcSelect {
        Ttom   ///< The latest healthy record transmitted at or before the epoch (the bed's rule).
    };

    /**
     * @brief Denominator by which the clock-anchor guard (and the prior-residual
     *        census's @c clk family) standardizes (card Q3 §2.1).
     *
     * The guard's statistic is `ê / √(P⁻ − P⁺)` on the common-removed residual
     * ê = (x̂ₛ − μₛ) − median_c.  Both variances therefore have to be variances of
     * that contrast, not of the raw state; the original pairing matched only the
     * posterior half.  This enum makes the two halves independently selectable,
     * so that the fold change and the denominator change can be bisected (one
     * earlier A/B changed both at once and could not separate them).
     *
     * For a group of n carried clocks of one constellation with covariance C,
     * the contrast variance is
     *   Var(xₛ − mean_t x_t) = C_ss − 2·mean_t C_st + mean_{t,u} C_tu,
     * which is @ref GnssProcessor::clockAnchorGaugeProject.  Applied to the
     * posterior block it gives the matched P⁺; applied to the prior (diagonal
     * `var_eff`, plus the gauge-free fold's implicit ∞ along 1, to which the
     * contrast is orthogonal) it gives the matched P⁻.
     */
    enum class NetworkClockAnchorStat {
        Auto,   ///< (default) follow the fold: @c Post under `clock_anchor_gauge_free`, @c Raw otherwise.  Reproduces the behaviour from before this knob existed, byte for byte.
        Raw,    ///< Both halves are the raw marginals (P⁻ = var_eff, P⁺ = cov(c,c)): the original statistic.  Mis-specified whenever the prior constrains the gauge, because the raw P⁺ still carries the common-mode variance the contrast removed.
        Post,   ///< Posterior half projected, prior half raw: the pairing the gauge-free arm was first built with.  Kept selectable for exact reproduction, not because it is correct.
        Proj    ///< Both halves projected onto the contrast.  The consistent statistic, and the only one that is correct for both folds.
    };

    /**
     * @brief Which satellite @c network_dd_fractional.csv differences every chord against.
     *
     * The DD trace forms, per (constellation, signal), one between-satellite SD against a single
     * reference satellite, so every graded chord inherits that satellite's own between-station
     * ambiguity error (SPEC_amb_iono_split_overcount §6.12).  Under the lexically-lowest rule a
     * satellite such as G01 or E02 can remain the reference all day, and its eclipse return then
     * re-references the entire trace to legs that were just reborn at every station; this is the
     * "post-eclipse network-wide DD episode" of §6.11.
     *
     * Trace only: this changes which chords appear in one diagnostic CSV and nothing else.  No
     * solve input, state or product is affected, so it is fingerprint-safe and legal on a warm
     * resume.
     */
    enum class NetworkDdFracRef {
        Lowest, ///< (default) the lexically-lowest satellite present at >= 2 stations: the original rule, byte-identical.
        Oldest  ///< The satellite with the longest continuous presence at >= 2 stations (ties go to the lexically lowest).  A settled leg makes a quieter reference whose returns do not re-reference the trace, but the trace is still not reference-free (the chosen reference carries its own slip and re-level history), so grading still uses `scripts/simnet_dd_reref.py` / `--ref-free`.
    };

    /**
     * @brief Hard per-satellite geometry-free code-bias datum
     *        (@c network.sat_gf_datum) (card RF-0c,
     *        docs/refound/RF-0b_dcb_datum_2026-09-15.md, last section).
     *
     * RF-0b measured the gap this closes.  Under @ref NetworkW6IonoConstraint::None each
     * satellite keeps one geometry-free direction (Odijk et al. 2016 rank deficiency 3b:
     * @f$I_r^s \to I_r^s + \beta^s@f$ compensated by @f$d^s_j \to d^s_j - \mu_j\beta^s@f$)
     * that nothing self-contained closes: the ionosphere-free pin is invariant under it
     * by construction, and the per-(constellation, signal) sum-to-zero rows change by
     * @f$-\mu_j\sum_s\beta^s@f$ and so see only the constellation-wide shift.  With 32 GPS
     * satellites that is 31 open directions.  The loose 3 m diagonal
     * @c network.cb_kaula_prior then becomes their datum without any report (with the
     * prior off, the gate sees the near-zero directions and the run emits nothing).
     *
     * A datum is the correct answer here, not a prior.  The satellite geometry-free code
     * bias is not separately estimable from the slant ionosphere under two frequencies, so
     * the S-basis fixes it and the estimable ionosphere absorbs the true value (their
     * Table 8: @f$\tilde\iota = \iota + d_{r,GF} - d^s_{GF}@f$).  That is consistent for
     * any consumer that floats its own per-satellite slant ionosphere, as the project's
     * consumer does.
     *
     * @ref Zero pins the combination at 0, the same arbitrary but legal choice that
     * @c network.code_bias_gf_datum makes.  That knob implements the identical row but is
     * refused under @c frequency_mode = dual, where it would also fix the ionosphere-free
     * half and leave nothing estimable; RF-0c reaches the row at dual because it is armed
     * independently of that gate.  @ref BroadcastTgd pins it at the value the navigation
     * message already asserts: the broadcast clock is referenced to the L1/L2
     * ionosphere-free combination, and the single-frequency user corrections
     * @f$\Delta t_{L1} = \Delta t_{SV} - T_{GD}@f$, @f$\Delta t_{L2} = \Delta t_{SV} -
     * \gamma T_{GD}@f$ imply @f$d^s_1 = c\,T_{GD}@f$ and @f$d^s_2 = c\,\gamma\,T_{GD}@f$
     * (IS-GPS-200 §20.3.3.3.3.2).  These satisfy the ionosphere-free pin exactly
     * (@f$a_1 + a_2\gamma = 0@f$ by the definition of the weights), and their geometry-free
     * combination is exactly @f$c\,T_{GD}@f$, so the two pins compose without conflict,
     * and the emitted ionosphere is referenced to the broadcast DCB convention rather
     * than to an arbitrary zero.  Signal in space only: no external product is read.
     *
     * Default @ref NoneMode is byte-identical (no row is built).  Solve-affecting
     * otherwise.  Requires @c network.w6_iono_constraint = "none": an ionosphere
     * constraint already determines this direction, and two datums for one deficiency
     * over-constrain it.
     */
    enum class NetworkSatGfDatum {
        NoneMode,     ///< (default) no geometry-free datum row: the original producer, byte-identical.  Under `w6_iono_constraint: "none"` the between-satellite geometry-free split is then left to whatever loose prior sits on the OSB columns (RF-0b's finding).
        Zero,         ///< One hard pin row per satellite on `(d_2 - d_1)/(mu_2 - mu_1)` at 0, over the ionosphere-free pin's own signal pair.  The arbitrary-but-legal S-basis choice; the geometry-free lump moves into the slant ionosphere.
        BroadcastTgd  ///< The same row pinned at `c * T_GD` from the navigation message (GPS Tgd / the first group delay), run-latched per satellite so the datum cannot churn mid-run.  A satellite whose ionosphere-free pin pair is not an L1/L2 pair, or that has no navigation record, falls back to 0 and is counted.
    };

    /**
     * @brief Ionosphere constraint of the W6 satellite code-bias slice
     *        (@c network.w6_iono_constraint) (card RF-0b,
     *        docs/refound/RF-0_baseline_2026-09-14.md).
     *
     * The satellite DCB (the geometry-free part of the satellite code bias) is collinear
     * with each station's free per-(receiver, satellite) slant ionosphere: shifting
     * @f$I_r^s \to I_r^s + \beta^s@f$ at every station is exactly compensated by
     * @f$d^s_j \to d^s_j - \mu_j \beta^s@f$ (Odijk et al. 2016, J Geod 90:15-44, rank
     * deficiency 3b).  Something has to close that per-satellite direction.  The other
     * two answers are a single-layer constraint (the co-estimated SH-VTEC field, or the
     * external IONEX GIM) and the hard geometry-free pin (@c network.code_bias_gf_datum),
     * which needs a third frequency to leave anything estimable and is refused under
     * @c processing.frequency_mode = dual.
     *
     * Without this option, GPS-only dual-frequency processing has no posture that works
     * without external ionosphere data: the loader forces
     * @c estimation.ionosphere_weighting on, and the run fails without a `.INX` (RF-0
     * finding 2).  This option adds a third answer: no ionosphere constraint at all,
     * with the satellite code-bias datum carried by the rows the tree already folds
     * every epoch (@ref NetworkSbasis::buildCodeBiasDatumRows: the per-satellite
     * ionosphere-free pin plus the per-(constellation, signal) sum-to-zero rows, i.e.
     * the IGS @f$\sum_s DCB^s = 0@f$ convention).  The slant ionosphere then absorbs the
     * geometry-free lump, so a consumer must apply the emitted ionosphere and the
     * emitted code biases as a pair.
     *
     * @warning Read §5 of card RF-0b before grading a product from this posture.  The
     *          sum-to-zero rows close exactly one direction per (constellation, signal),
     *          the constellation-wide level, and cannot see the n_sat-1
     *          between-satellite directions that the GIM's common-mode-free R closes
     *          when a GIM is used.  Whatever loose prior sits on the OSB columns
     *          (@c network.cb_kaula_prior) therefore becomes the operative datum for
     *          that split.  The run-end @c W6-IONO-NONE line reports the epochs
     *          folded; the null-space gate reports whether any direction is left open.
     *
     * Default @ref Auto reproduces the original selection exactly (byte-identical,
     * standing invariant #6).  Solve-affecting when set to @ref None.
     */
    enum class NetworkW6IonoConstraint {
        Auto, ///< (default) the original three-way selection: the co-estimated SH-VTEC field if `network.estimate_iono_sh`, else the free-slant Odijk posture if `network.code_bias_gf_datum`, else the external IONEX GIM (hard error without a `.INX`).  Byte-identical to the producer before this knob.
        None  ///< No ionosphere constraint: free per-LOS slant ionosphere, no GIM loaded, `estimation.ionosphere_weighting` forced off, and the satellite code-bias datum left to the IF pin and the per-(constellation, signal) sum-to-zero rows.  Refuses `network.estimate_iono_sh` and `network.code_bias_gf_datum` (each is a different answer to the same rank deficiency); inert without `network.estimate_sat_code_bias`.
    };

    /**
     * @brief Band scope of the T4 attribution erase (docs/SPEC_event_harness.md §Z).
     *
     * On three fault-free arcs of the quiet sim bed (study 1), the persistent level
     * floor of 0.445 cycles that a T4 erase leaves comes from the band scope alone,
     * not from anything else on the erase path: restricting the same erase to 2 of
     * a 3-band arc lowers the floor 9× (0.445 → 0.050) on every band, including the
     * band that was never touched.  Freeing all bands of a line of sight removes
     * that LOS's last level anchor, so (ρ, I, N₁..N₃) must be re-drawn together
     * from one epoch of code, which is exactly the re-coupling edge of standing
     * invariant #1.  With one band left pinned, the re-tapped levels land on the
     * survivor's ionosphere instead of on code noise.
     */
    enum class NetworkRaimEraseBandScope {
        All,        ///< (default) erase every signal of the flagged (station, satellite): the original path, byte-identical.
        MinKeepOne  ///< Erase the bands whose own post-fit normalized residual |r|/σ̂₀ stands above the T4 gate, and always keep the band with the smallest |r|/σ̂₀ pinned as the LOS's level anchor.  A single-band arc is skipped entirely (there is no band to spare).
    };

    /**
     * @brief Which vehicle carries a network ambiguity between epochs
     *        (@c network.ambiguity_vehicle, docs/refound/RF-1_float_ambiguities_design.md §7.1).
     *
     * The re-founding program names the ambiguity's carry as the first mechanism to
     * replace.  Under @c Level an ambiguity is a per-epoch free parameter made
     * time-linked by a zero-process-noise scalar prior that carries the previous
     * epoch's marginal variance: it is born with no prior at all, from one epoch of data,
     * at a variance the model reports rather than the one the value merits, and any of
     * four uninstrumented reset paths returns it to that state (design §1.8).
     * @c FloatKf makes it a float state of the station's local block whose carried joint
     * covariance is folded as a proper prior, with a recorded and floored birth prior and
     * a slip response that drops the prior back to that birth prior instead of skipping
     * one inject or erasing the entry (design §3).
     *
     * Selecting @c FloatKf arms the joint local block (RF-1 step 3) and the bounded drop
     * as the slip response.  The birth-variance floor (step 4, @c var_birth) and the
     * declared anchor S-transform (step 5) are armed by their own knobs on top of this
     * enum.  The run manifest records which vehicle a product was made on.
     *
     * Solve-affecting, hence fingerprint-relevant: a warm resume requires the vehicle in
     * the datum-relevant config fingerprint and @ref LocalCarry::var_birth in the
     * snapshot record, since without var_birth a restored arc cannot be dropped
     * correctly (design §6.1).
     */
    enum class NetworkAmbiguityVehicle {
        Level,  ///< (default) the zero-q phase-leveling scalar carry: the original vehicle, byte-identical.
        FloatKf ///< The float state with a carried joint covariance, a floored birth prior sized to the value, and a bounded drop as the slip response (RF-1 steps 3–5).
    };

    /**
     * @brief How the gross-LOS screen reads @c network.gross_los_min_rows (RF-3b B1)
     *        (docs/refound/RF-3_code_row_qc_2026-09-22.md §4, requirement F1).
     *
     * The constant's rationale ("a gross line of sight is wrong on every code band at
     * once; one heavy-tailed row is one band's multipath") holds for a line of sight
     * that has several code rows and is false for one that has lost every signal but
     * a single band-1 code, where the bar cannot be met by construction.
     */
    enum class NetworkGrossLosMinRowsRule {
        Fixed,     ///< (default) at least @c gross_los_min_rows rows over the bar, whatever the line of sight has.
        Available  ///< The bar is @c min(gross_los_min_rows, the LOS's code rows this epoch): a one-row LOS can reach it, a three-row LOS still needs two.
    };

    /**
     * @brief Band scope of the whole-satellite detector refloats
     *        (docs/SPEC_event_harness.md §Z).
     *
     * The detector-side counterpart of @ref NetworkRaimEraseBandScope.  MW and LLI
     * already act per pair or per signal; the paths that can free an entire arc are
     * the IF-TD pair on a 2-band arc, an LLI asserted on every band, and the wind-up
     * accumulator restart drain (a band-common model step, which implicates all bands
     * by construction).  @c Implicated frees only what the detector named and, when
     * that covers every live band, keeps the least suspect one pinned.
     */
    enum class NetworkRefloatBandScope {
        All,        ///< (default) free every band the detector named: the original path, byte-identical.
        Implicated  ///< Free only the implicated bands; when they cover the whole arc, keep the band with the smallest raw detector evidence (the MW/GF per-band decomposition) pinned and count the fallback.
    };

    /**
     * @brief How many band pairs the IF-TD detector evaluates per satellite
     *        (`network.cycle_slip.if_phase_td_pairs`, SPEC_event_harness §14.10).
     *
     * The iono-free triple difference is formed over a pair of phase bands.  With
     * @c First the detector forms exactly one pair per satellite, from the first two
     * distinct bands it finds (L1C+L2W on GPS, L1C+L5Q on Galileo).  Every other pair
     * is then unwatched, and the equal-count case that the common-band scope's IF-TD
     * veto exists to catch (§14.2) goes unchecked on all of them: two bands slipping
     * by the same integer leave their Melbourne-Wübbena lane silent while the
     * iono-free combination moves 0.107 m per cycle, so a silent MW pair with no
     * IF-TD pair behind it exonerates two bands that both moved.
     *
     * Measured on the storm bed: a true L5Q slip of −31 cycles followed by L1C −29
     * moved the (L1C+L5Q) MW lane by 2 wide-lane cycles, which stayed below the
     * detector, and the verdict freed L2W, the one band that had not slipped.  An
     * IF-TD on (L1C,L5Q) would have read −2.5 m and vetoed it.
     *
     * Pooling (centre / scale / shrinkage) is per pair kind in both the driver and
     * the verdict's preview, and each additional pair gets its own gradient chain and
     * its own reference election, so @c All is a pure addition: the first pair's
     * decisions are unchanged.
     */
    enum class NetworkIfPhaseTdPairs {
        First,  ///< (default) one pair per satellite, from the first two distinct phase bands: the original path, byte-identical.
        All     ///< Every distinct pair of the satellite's tracked phase bands (3 on a triple-band arc, 1 on a dual-band one), each with its own pool, gradient chain, reference election and reset keys.
    };

    /**
     * @brief Which Melbourne-Wubbena pairs the network's wide-lane detector forms
     *        (`network.cycle_slip.mw_detect_pairs`) (card L5P).
     *
     * On a two-band lineage there is one pair and the knob is inert.  On the
     * three-band lineage of card B5D the detector forms three pairs per satellite,
     * and the two pairs that carry band 5 produced most of the `mw_wl_slip`
     * detections (55 of 66 in the measured run).  An earlier emulation removed those
     * pairs and the band-5 code rows of the solve in one step; this knob removes the
     * pairs alone, so the two effects can be attributed separately (L5P section 2).
     *
     * @c Primary forms only the pair whose two signals both sit on the
     * constellation's ionosphere-free pin bands, which on a lineage that latches one
     * signal per band is the station's latched band-1 / band-2 pair.  The band-5
     * phase keeps every other response it has (LLI, gap re-entry, IF-TD, the band
     * scope); only the wide-lane pair goes, and standing invariant #1 is graded on
     * exactly that for this knob (gate G2).
     */
    enum class NetworkMwDetectPairs {
        All,     ///< (default) every pair the detector produced: the original path, byte-identical.
        Primary  ///< Only the primary pair (both signals on the pin bands); a pair carrying any other band is not formed.
    };

    /**
     * @brief The network producer's time-linked parameter classes: the key space
     *        of the @c network.process_noise table.
     *
     * One entry per network-owned state whose covariance is inflated by a
     * per-second driving-noise rate between epochs.  The network-owned scope is a
     * deliberate boundary: the per-station states (receiver clock, station
     * ZWD/gradients, per-sat slant iono, station ambiguities, receiver code
     * biases) are propagated by @c StateManager, which is shared unchanged with
     * the PPP and real-time paths, so they keep reading
     * @c filter.process_noise.*.  Routing those through a network-scoped table
     * would be a shared-path edit reaching two other estimators; the class list
     * in @ref NetworkProcessNoise points users to the per-station keys instead.
     *
     * The enumerator order is the index order of @ref kNetworkQClassTable and of
     * the @ref NetworkProcessNoise arrays: append only, never reorder.
     */
    enum class NetworkQClass : std::size_t {
        SatClock = 0,       ///< Satellite clock Δδtˢ random walk (the shared arrowhead column).
        SatPhaseBias,       ///< Satellite phase bias (OSB) random walk.
        SatCodeBias,        ///< Satellite code bias (OSB) random walk; W6.
        VtecSh,             ///< Global SH-VTEC layer-1 coefficient q, per degree (the n>0 terms take q/n²).
        VtecShLayer2,       ///< Global SH-VTEC layer-2 coefficient q, per degree (dual-layer / Iono-2).
        StationZwd,         ///< Station ZWD carry across the epoch seam.
        LevelingAmbiguity,  ///< Phase-leveling carried station ambiguity (the zero-q lock-in state).
        ChainDatumRpb,      ///< Eq-34 chain-datum per-receiver phase-bias split.
        DdCarry,            ///< Invariant DD carry (@c carry_invariant), z-store.
        SatClockWfm,        ///< Two-state satellite clock model, white-FM term (q1) on the bias.
        SatClockRwfm,       ///< Two-state satellite clock model, random-walk-FM term (q2) on the rate.
        IonoCarry           ///< Per-LOS slant-iono carry across the epoch seam (queue item 8b).
    };

    /// Number of entries in @ref NetworkQClass / @ref kNetworkQClassTable.
    inline constexpr std::size_t kNetworkQClassCount = 12;

    /**
     * @brief Static descriptor for one @ref NetworkQClass: the single place that
     *        names a class, its legacy key, its fallback and its units.
     *
     * The loader, the toggle-summary log line and the run-manifest writer all iterate
     * @ref kNetworkQClassTable rather than repeating a per-class switch, so adding a
     * class is one row here plus one enumerator.
     */
    struct NetworkQDesc {
        const char* name;        ///< Key under @c network.process_noise (and the manifest field name).
        const char* legacy_path; ///< Dotted legacy key this class subsumes ("" if none).
        const char* legacy_flat; ///< Flat legacy alias accepted by @c jval ("" if none).
        double      fallback;    ///< Hard-coded default; must equal the legacy key's own default.
        const char* units;       ///< Physical units of the rate.
        const char* gate;        ///< Config toggle(s) that must be armed for this q to do anything.
    };

    /**
     * @brief The network process-noise class table.
     *
     * Every @c fallback below equals the loader's default for the legacy key beside
     * it (enforced by tests/test_network_process_noise_config.cpp).  That equality
     * lets the table be always on without changing any result: with no
     * @c network.process_noise block present, every class resolves to the value the
     * run would have used anyway, so a stock config is byte-identical and every
     * warm-resume fingerprint (which hashes the resolved q's) is unchanged.
     */
    inline constexpr std::array<NetworkQDesc, kNetworkQClassCount> kNetworkQClassTable{ {
        { "sat_clock",          "filter.process_noise.sat_clock_q",       "sat_clock_q",       1e-2,   "m^2/s",   "always (network producer)" },
        { "sat_phase_bias",     "filter.process_noise.sat_phase_bias_q",  "sat_phase_bias_q",  1e-12,  "m^2/s",   "always (network producer)" },
        { "sat_code_bias",      "filter.process_noise.sat_code_bias_q",   "sat_code_bias_q",   1e-10,  "m^2/s",   "network.estimate_sat_code_bias" },
        { "vtec_sh",            "estimation.global_vtec_sh.q_per_degree", "global_vtec_sh_q_per_degree", 1.0e-2, "TECU^2/s", "network.estimate_iono_sh" },
        { "vtec_sh_layer2",     "estimation.global_vtec_sh.layer2_q_per_degree", "global_vtec_sh_layer2_q_per_degree", 1.0e-2, "TECU^2/s", "network.estimate_iono_sh + layer2" },
        { "station_zwd",        "network.zwd_carry_q",                    "zwd_carry_q",       1e-9,   "m^2/s",   "network.zwd_carry" },
        { "leveling_ambiguity", "network.phase_leveling_q_amb",           "phase_leveling_q_amb", 0.0, "m^2/s",   "network.phase_leveling" },
        { "chain_datum_rpb",    "network.phase_datum_eq34_rpb_q_m2s",     "network_phase_datum_eq34_rpb_q_m2s", 3e-6, "m^2/s", "network.phase_datum_eq34 (INERT under carry_invariant)" },
        { "dd_carry",           "network.carry_invariant_qz",             "network_carry_invariant_qz", 0.0, "m^2/s", "network.carry_invariant" },
        { "sat_clock_wfm",      "network.clock_model_q1",                 "network_clock_model_q1", 0.0, "m^2/s",  "network.clock_model" },
        { "sat_clock_rwfm",     "network.clock_model_q2",                 "network_clock_model_q2", 1.5e-7, "m^2/s^3", "network.clock_model" },
        { "iono_carry",         "network.iono_carry_q",                   "iono_carry_q",      1e-4,   "m^2/s",   "network.carry_iono" },
    } };

    /**
     * @brief Resolved per-class process-noise rates for the network producer
     *        (@c network.process_noise).
     *
     * A scoped authoring surface for q values whose legacy keys are spread across
     * several config sections.  Always live, with no master enable, because it
     * changes nothing on its own: each class defaults to its legacy key's own
     * default (@ref kNetworkQClassTable), so an unmodified config resolves to the
     * same numbers exactly.
     *
     * Precedence per class, highest first:
     *  1. the legacy key when set explicitly, so existing configs behave
     *     identically and a delta config can override one class without
     *     restating the table;
     *  2. @c network.process_noise.<name>;
     *  3. @ref NetworkQDesc::fallback.
     *
     * Setting both is legal and logged as a warning, so a shadowed value is always
     * reported.  @ref source records which rung won, and the run manifest emits it
     * per class: a q that reads as @c Default when the operator believed it was set
     * is the inert-wiring failure that run-end telemetry exists to catch.
     *
     * Resolution happens once, in the network section loader, writing through to
     * the canonical config members the estimator already reads.  No apply site, no
     * @c NetworkKnobs field and no fingerprint input changes: this is an authoring
     * and provenance surface, not a second source of truth.
     *
     * Deliberately not in the table: three mechanisms are genuine covariance-growth
     * rates but not process noise on a network state.
     *  - @c network.emit_pb_filter_q: the emitted phase-bias smoother, which lives in
     *    the emission frame, never in the estimator.  A low-q PB in the estimator
     *    and one on the wire are different things.
     *  - @c estimation.global_vtec_sh.rep_error_tau_s: a Gauss-Markov time constant
     *    in seconds, not a rate; its covariance growth is set jointly with
     *    @c representation_sigma_tecu, which a q-keyed table cannot express.
     *  - @c network.lane_carry.q_wl_scale: a dimensionless anisotropy multiplier
     *    shaping @ref NetworkQClass::DdCarry into the (NL, WL) lane basis.  It
     *    selects a direction, not a magnitude.
     *
     * Per-station classes are absent for the scope reason given on
     * @ref NetworkQClass: @c StateManager propagates them, they are shared with the
     * PPP and real-time estimators, and they stay on @c filter.process_noise.*.
     *
     * @warning Apart from a wired event response, a process-noise rate is the only
     *          mechanism that lets a time-linked state move off a stale value after
     *          an event.  Lowering a class toward zero makes the corresponding
     *          carry stiff, and a stiff carry across an unmodelled jump distorts the
     *          whole solution rather than degrading gracefully (standing
     *          invariant 1).  Any class driven to 0 here needs a wired, tested
     *          slip/jump response on the state it governs before a field run with
     *          it on.
     */
    struct NetworkProcessNoise {
        /// Which precedence rung supplied a class's resolved value.
        enum class Source {
            Default,  ///< No key set anywhere; @ref NetworkQDesc::fallback.
            Legacy,   ///< The pre-existing scattered key, set explicitly.
            Table,    ///< @c network.process_noise.<name>.
            Derived   ///< Overwritten after resolution by another loader rule (a validity-restore, a clamp, or the service-mode bundle): the value is real but neither key chose it.
        };

        std::array<double, kNetworkQClassCount> q{};    ///< Resolved rate, indexed by @ref NetworkQClass.
        std::array<Source, kNetworkQClassCount> src{};  ///< Winning precedence rung, indexed by @ref NetworkQClass.

        /// Fill every class with its descriptor fallback (@c Source::Default).
        NetworkProcessNoise() {
            for (std::size_t i = 0; i < kNetworkQClassCount; ++i) {
                q[i]   = kNetworkQClassTable[i].fallback;
                src[i] = Source::Default;
            }
        }

        /// Resolved rate for @p c.
        double get(NetworkQClass c) const { return q[static_cast<std::size_t>(c)]; }
        /// Winning precedence rung for @p c.
        Source source(NetworkQClass c) const { return src[static_cast<std::size_t>(c)]; }

        /// Record @p value for class @p c as having come from @p s.
        void set(NetworkQClass c, double value, Source s) {
            q[static_cast<std::size_t>(c)]   = value;
            src[static_cast<std::size_t>(c)] = s;
        }

        /// Stable lowercase label for @p s (manifest field + log line).
        static const char* sourceLabel(Source s) {
            switch (s) {
                case Source::Legacy:  return "legacy";
                case Source::Table:   return "table";
                case Source::Derived: return "derived";
                case Source::Default: break;
            }
            return "default";
        }
    };

    /**
     * @brief Split-cadence clocks-only fast pass (FC-1).
     *
     * The full pipeline (solve, carry, QC, emit) keeps running at its own cadence.
     * Epochs that @c network.epoch_decimation_n decimates out, which otherwise reach
     * a bare @c continue at the T2e hook, instead run a clocks-only tick that
     * measures each satellite's clock change since the last emitted full solve and
     * optionally emits it.  Everything else is frozen and held by ZOH, as a consumer
     * already does within one full-solve era; the fast tick refreshes the one state
     * whose ZOH error dominates.
     *
     * Rationale (measured): the 305-station all-on baseline spends 27.7 s per epoch,
     * of which the border solve takes 41 ms, so a 1 s product cadence cannot be
     * reached by optimizing the per-epoch pipeline, while the estimation itself is
     * cheap once the expensive structure (ambiguities, iono, biases, datum, QC) is
     * known.  Card C1 gives the theoretical basis: the emitted c0 is a random walk
     * (0.157 m per 30 s), so ZOH is already the MMSE predictor given a 30 s solve,
     * and better fast clocks require measuring them at the served rate.  C1's own
     * rate product measured net-harmful and stays off (@c network.emit_clock_rate).
     *
     * The observable is a per-band delta-from-fit against the anchor epoch t0,
     * IF-combined (which removes the iono change) and phase-only; frozen
     * ambiguities, biases and wind-up cancel exactly in the delta on a live arc.
     * Station clocks are re-estimated per tick and Schur-eliminated, leaving a dense
     * n_sat x n_sat solve.
     *
     * Default off: the T2e hook keeps its bare @c continue, and nothing is
     * allocated, traced or emitted (byte-identical).  Isolation contract: full
     * epochs are byte-identical whether or not fast ticks ran, because a tick
     * mutates nothing outside @ref FastClockCarry.
     */
    struct FastClockConfig {
        /// Master switch.  Off: the decimation hook keeps its bare @c continue (byte-identical).
        bool   enable{ false };
        /// Run the tick computation on full epochs too, immediately before the full
        /// pipeline, logging fast-versus-full deltas and discarding the result.  No
        /// downstream effect (gate: everything but the trace CSV is byte-identical).
        /// This is the standing grading instrument: it gives a per-epoch reference
        /// (the full solve) on any run at no cost, in the same instrument-only shape
        /// (no feedback) as the M2 LS-VCE carry-variance monitor.  Requires @ref enable.
        bool   shadow{ false };
        /// Emit the fast clocks as product records.  Off: the tick runs and traces
        /// but writes nothing to the product wire (the INC-2 posture).
        /// Requires @ref enable.
        bool   emit{ false };
        /// A satellite needs this many unscreened dual-band LOS with live t0 arcs to be
        /// fast-estimated on a tick.  Below the floor the satellite is not refreshed:
        /// the consumer keeps the last emitted value and its age grows.  Never
        /// extrapolated (no fabricated corrections).
        int    min_stations_per_sat{ 3 };
        /// A station needs this many usable satellites to contribute its clock nuisance.
        int    min_sats_per_station{ 2 };
        /// Per-LOS post-fit screen in sigmas: |res| > k*sigma_pred excludes the LOS from
        /// this tick only (re-tested on the next).  This screen is the fast tier's only
        /// slip response: a slipped arc's delta jumps by lambda*dN, far outside
        /// k*sigma.  Repair stays with the full pipeline (reset rather than repair; no
        /// GF/TurboEdit here, per the 30/60 s data-rate directive).  A coherent
        /// whole-row (satellite) move is not screened: that is the signal being
        /// measured.
        double screen_k_sigma{ 5.0 };
        /// Admit single-band arcs (no IF combination, so the iono change survives in the
        /// residual).  Useless at a 300 s dev cadence; potentially worth it at a 1 s RT
        /// tick with iono-rate-inflated R.  Off by default.
        bool   allow_single_band{ false };
        /// Emission-continuity bound (s) (R11).  If the time since the last emitted full
        /// solve exceeds this, fast emission stops (ticks may still run for telemetry).
        /// Keyed on emission, not on solve continuity: a withheld era advances the
        /// solve clock while the consumer receives nothing, so a solve-keyed gate would
        /// publish a delta measured inside an era the consumer never received.  Degrades
        /// by silence, never by fabrication.
        double emit_max_gap_s{ 60.0 };
        /// Log-only per-tick CSV (logs/network_fast_clock.csv).  Does not change the
        /// solve or the emission.
        bool   trace{ false };
        /// Per-satellite detail rows in the trace: 0 = per-tick summary rows only,
        /// N > 0 = also write per-satellite dc/sigma rows every Nth tick.  A per-tick
        /// per-satellite row at 305 stations x 1 s would grow without bound, so the
        /// detail is strided and off by default.
        int    trace_sat_stride{ 0 };
    };

    /**
     * @brief Multi-station streaming front-end (@c network.streaming) (RTF-3).
     *
     * When @c enable is set, @c runNetwork replaces its batch epoch source (the
     * pre-built @c MultiRinexSync synced vector) with live per-station streams: one
     * transport, framer and RTCM decoder lane per roster station that carries a
     * @c StationTransportConfig, a wait-budget aligner that dispatches each epoch at
     * T+@c wait_budget_s with the stations that have arrived, and a bounded solve
     * queue with epoch-granular drop-oldest backpressure.  Everything else (station
     * build, per-epoch body, teardown) runs the same code path.
     *
     * Constraint (documented in the front-end header): station construction still
     * reads each roster station's @c rinex_path for header metadata (receiver
     * antenna type, GLONASS FCN slots, first-epoch seed), so the files must exist
     * even though their observations are not the epoch source.  A header-free
     * StationMetadata factory is a planned follow-up of RTF-3.
     *
     * Default off: none of this is constructed, and the batch path is untouched.
     */
    struct NetworkStreamingConfig {
        bool   enable{ false };            ///< Master switch (default off: batch, byte-identical).
        /// Wait budget from an epoch's first arrival to dispatch (s).  The design
        /// range is 0.3-1 s; an epoch whose full expected station set arrives
        /// earlier dispatches immediately.
        double wait_budget_s{ 0.7 };
        /// Minimum stations present to dispatch an epoch (below => the epoch is
        /// dropped and counted, never solved).  2 is the estimability floor for
        /// any between-station content.
        int    min_stations{ 2 };
        /// Require the clock-datum station in a dispatched epoch.  The legacy
        /// pivot-first PASS A skips a pivot-less epoch anyway; gating here spends
        /// no queue slot on a known skip.  Prioritized/eq34 datums may relax this.
        bool   require_datum_station{ true };
        /// Bounded solve-queue depth in whole epochs.  Overflow drops the oldest
        /// queued epoch.  Backpressure is epoch-granular, never byte-granular, so the
        /// framer and decode state stay coherent; dropping bytes instead
        /// desynchronizes them (documented in the front-end header).
        int    queue_depth_epochs{ 8 };
        /// Nominal observation cadence (s) for bucketing arrivals into epochs.
        /// A uniform cadence across streams is required (IGS MSM standard: 1 s).
        double epoch_interval_s{ 1.0 };
        /// Aligner memory bound: pending (not yet dispatched) epoch buckets are
        /// capped at this many; the oldest stale bucket is dropped and counted.
        int    max_pending_epochs{ 16 };
        /// Derive LLI 0x1 from MSM lock-time regressions, tracked per
        /// (station, sat, signal) through one shared LockTimeTracker (reuses RTB-3).
        bool   msm_lli_from_locktime{ false };
        /// Front-end heartbeat cadence (s of wall clock) for the ingest summary
        /// log line (lanes up, epochs dispatched/dropped, queue depth).
        double log_interval_s{ 30.0 };
    };

    /**
     * @brief One scripted producer-side fault, from @c network.inject.events[].
     *
     * Event harness (docs/SPEC_event_harness.md): converge once into a state
     * snapshot, then resume for a few tens of epochs with exactly one of these
     * armed, so that a single event's response can be read against a null leg in
     * minutes instead of a day-long run.  Fields a @c kind does not use are
     * ignored, and rejected at load if they are the required field of another
     * kind, so a typo cannot arm a different event unnoticed.
     *
     * Every event is a deliberate corruption of the producer's own input or of a
     * decision it would otherwise make on the data.  None is a modelling option:
     * with @c network.inject absent (the default), no branch of the seam is
     * reachable and the run is byte-identical.
     */
    struct NetworkInjectEvent {
        /// @c slip | @c outage | @c anchor_handover | @c arc_erase | @c sat_clock_step
        /// | @c rx_clock_step | @c oracle_pin.
        std::string kind;
        /// GPS seconds-of-week at which the event starts.  The run's week is implied
        /// (a leg never spans a rollover); matched with a +/-0.25 s half-epoch guard.
        double      tow{ -1.0 };
        std::string station;        ///< Station name (slip, outage, arc_erase, rx_clock_step).
        std::string sat;            ///< Satellite id, e.g. "G05" (every kind but a station-wide one).
        /// RINEX-3 phase code, e.g. "L1C" (slip); "all" or a code (anchor_handover).
        std::string signal;
        double      delta_cycles{ 0.0 };  ///< slip: cycles added to the phase observable.
        bool        flag_lli{ false };    ///< slip: also raise LLI bit 0x1 on the onset epoch.
        double      duration_s{ 0.0 };    ///< outage: window length; the return is a natural re-admission.
        /// @c anchor_handover: "cold" | "continuous".
        ///
        /// @c arc_erase: the diagnostic sub-knob (§9 study 1): "erase" (the default
        /// and the production T4 path) | "free_epoch" (act as a detector refloat
        /// instead: keep the @c LocalCarry entry, latch the free-epoch set, no iono
        /// co-erase, no C3c queue) | "erase_keep_iono" | "erase_no_reseed" |
        /// "erase_no_funnel" (the production erase minus the one named action).
        /// Harness only: it changes what the injected event does, never what a real
        /// attribution does.
        std::string mode;
        /// Metres of non-dispersive range step.  @c sat_clock_step adds it to the
        /// a-priori satellite clock of @ref sat (at every station); @c rx_clock_step
        /// adds it to the receiver clock of @ref station (for every satellite), the
        /// exact dual.  In both cases the step persists from @ref tow on.
        double      step_m{ 0.0 };
        /// @c oracle_pin only: which arcs are held at the fixture's true integers
        /// (include/filter/NetworkOraclePin.hpp, docs/SPEC_oracle_pin.md).  One of
        ///   - @c all                        : every carried non-anchor arc;
        ///   - @c band:<sig>[+<sig>…]        : e.g. `band:L1C+L2W`;
        ///   - @c station:<name>             : one roster station;
        ///   - @c sat:<id>                   : e.g. `sat:G08`;
        ///   - @c arc:<station>/<sat>/<sig>  : one exact arc.
        /// Anything else is refused by name at load: a scope that widened (or
        /// emptied) unnoticed would be graded as though it were the one the operator
        /// wrote.  Required for @c oracle_pin, ignored by every other kind.
        std::string scope;
        /// @c oracle_pin only: the pin prior's sigma in metres (default 1e-4 m
        /// ≈ 0.0005 cyc, weight 1e8).  Stiff, but two decades below the 1e10 datum
        /// pins, so the gauge stays with the anchors.  Must be > 0.
        double      sigma_m{ 1.0e-4 };
    };

    /**
     * @brief @c network.inject: the producer-side fault injector (default off).
     *
     * @c enable defaults to true when an @c events array is present, so the
     * common case is one key.  An armed event that never fires is a hard run-end
     * error (the simulator's scripted-schedule rule), so a test fixture that did
     * not take effect cannot pass unnoticed.
     */
    struct NetworkInjectConfig {
        bool enable{ false };                    ///< Master switch; false => the seam is unreachable.
        std::vector<NetworkInjectEvent> events;  ///< Scripted events (the loader caps it at 4096, a guard against paste accidents).
    };

    /**
     * @brief Configuration for the PPP-RTK network producer (@c PPP_RTK_Network).
     *
     * A set of peer stations (no master/rover asymmetry) whose single-receiver
     * normal equations are reduced jointly onto a shared satellite-parameter
     * border (clock and code/phase bias) under an explicit minimum-constraint
     * S-basis.  Each station holds its coordinates fixed or tightly constrained.
     * The datum fields name the network time reference (a pivot receiver clock)
     * and, optionally, a reference satellite per constellation for the clock gauge.
     */
    struct NetworkConfig {
        std::vector<StationConfig> stations;  ///< Peer stations (>=2); each needs a known ECEF (held fixed).
        NetworkInjectConfig inject;           ///< Event-harness fault injector (network.inject.*; absent => off, byte-identical; docs/SPEC_event_harness.md).
        std::string clock_datum_station;      ///< Pivot receiver whose clock defines network time ("" => first station).
        std::string clock_datum_satellite;    ///< Optional reference satellite pinned for the clock gauge ("" => auto: highest-elevation tracked, per constellation).
        double epoch_sync_tolerance_s{ 0.01 };///< Max time-tag difference when matching epochs across stations.
        ClosedLoopConfig closed_loop;         ///< Optional closed-loop integer-recovery harness (held-out consumer).
        NetworkArConfig  ambiguity_resolution;///< Producer-side integer fixing (default off: float product).
        MwWlConfig       mw_wl;               ///< MW-seeded wide-lane pseudo-obs layer (M5 card WL-1; default off, byte-identical).
        LaneCarryConfig  lane_carry;          ///< Lane-coordinate carry (M5 card WL-3; default off, byte-identical).  Increments 2-4 are implemented: @c applyLaneMeasurement, @c applyLaneFix, @c carriedPairValue and @c injectCarriedChordPair live in @c NetworkInvariantCarryDriver (docs/SPEC_lane_carry_card.md).
        SdEwlConfig      sd_ewl;              ///< SD pre-reduce EWL constraint (default off, byte-identical; docs/SPEC_sd_ewl_prereduce.md).
        SlipQcConfig     slip_qc;             ///< Slip-QC hardening, cards R/L/G and their follow-ups (network.cycle_slip.*; every master default off, byte-identical; docs/SPEC_slip_qc_hardening.md).
        SignalIntegrityConfig signal_integrity; ///< Flicker/gap signal integrity: band-state classifier, degraded-phase demotion and re-vet (network.signal_integrity.*; default off, byte-identical; docs/SPEC_flicker_gap_integrity.md).
        FastClockConfig  fast_clock;          ///< Split-cadence clocks-only fast pass (FC-1; network.fast_clock.*; default off: the T2e decimation hook is untouched, byte-identical; docs/SPEC_fast_clock_split_cadence.md).
        NetworkStreamingConfig streaming;     ///< Multi-station streaming front-end (RTF-3; network.streaming.*; default off: batch epoch source, byte-identical).
        NetworkProcessNoise process_noise;    ///< Scoped per-parameter-class process-noise table (network.process_noise). Always live; every class defaults to its legacy key's default => byte-identical unless a value is edited.
        std::string product_output_path;      ///< Optional path: serialize each emitted NetworkSolutionEpoch (+ datum-change flag) here for replay (empty => disabled).  Used verbatim (absolute, or relative to the working directory).
        /// Flush the product file once per emitted epoch (after the epoch block,
        /// including any RTA-5 S re-broadcast preceding it), so that a live tail
        /// consumer (@c NetworkProductTail via realtime.network_products_path) sees
        /// each epoch as soon as it is written instead of at ofstream-buffer
        /// boundaries.  Flushing changes no bytes (the file is byte-identical either
        /// way), but a live tail requires it for bounded product latency.  Default
        /// false, following the default-off toggle convention.
        bool product_flush_per_epoch{false};

        // --- Per-station compute parallelism (PASS A geometry + PASS B assembly) ---
        // The per-station geometry/measurement-model work is embarrassingly
        // parallel (each station owns an independent StateManager/PppModel/
        // MeasurementModel); only the shared-estimator scatter (est.addStation)
        // serializes.  When activated, PASS A/B compute runs one thread per
        // station and the scatter stays serial in deterministic station order, so
        // the float solution is bit-identical to a serial run regardless of thread
        // count.  Below the threshold the loop runs serially (no fork-join cost).
        bool parallel_stations{ true };   ///< Master enable for per-station threading (only takes effect above @ref parallel_min_stations).
        int  parallel_min_stations{ 10 }; ///< Parallelize only when the network has strictly more than this many stations.
        int  parallel_max_threads{ 0 };   ///< Optional worker-thread cap (0 => omp_get_max_threads()).
        // Spread the per-epoch Schur reduction across threads too (the @ref
        // NetworkNormalEquations::reduce station loop).  Off by default and kept
        // separate from @ref parallel_stations because, unlike the build phase (whose
        // serial-ordered scatter keeps it bit-identical to a serial run), the reduce
        // fan-out combines per-thread partials in a different summation grouping.  Its
        // result is reproducible for a fixed thread count but differs from the serial
        // reduce at the ULP level, which the network's ill-conditioning amplifies to
        // ~1e-5 in the products: physically null, but it breaks serial≡parallel
        // byte-identity.  Enable only for maximum-speed runs that do not rely on a
        // serial-versus-parallel byte comparison for regression.  Uses the same thread
        // budget (@ref parallel_max_threads) and acts only when @ref parallel_stations
        // is active.
        bool parallel_reduce{ false };    ///< Parallelize the Schur reduce loop (opt-in; trades serial≡parallel byte-identity for speed).
        bool adaptive_iono_q{ false };    ///< Network storm response: per-station GF-TD2 variability monitors (IonoVariabilityMonitor) scale the T5v2 GM deviation innovation per (station,satellite); tuning from estimation.adaptive_iono_process_noise (whose .enable stays user-path-only). Requires the deviation channel (estimate_iono_sh + rep_error_tau_s > 0); validated at load. Deliberately not in the warm-resume fingerprint (a forward-only Q modifier).

        // --- eq34 reattach hygiene ---------
        // The phase datum may be re-hosted only onto an arc whose ambiguity has
        // converged.  A one-epoch-old arc's float is code-seeded (± metres), so
        // realizing the gauge on it re-levels the whole component by an arbitrary
        // amount that discharges along the soft phase-bias directions (observed as a
        // PHASE_DATUM/eq34_reattach event followed by a lasting between-satellite
        // total_IF offset).  The three increments below (INC-1 to INC-3) are
        // default-off and, off, add no state and no rows: the election is structurally
        // unchanged.  Deliberately not in the warm-resume fingerprint: they are a
        // forward-only election and row policy and do not change the meaning of any
        // serialized state (as with adaptive_iono_q), which keeps a shared-snapshot
        // A/B valid.
        int  eq34_anchor_min_arc_ep{ 0 };        ///< Minimum consecutive-present arc age (epochs) for a (station, ambiguity) arc to be an eligible anchor host on a reattach (INC-1).  0 = legacy (any host, byte-identical).  First-sighting keys (no incumbent) are never gated: this bounds re-hosting, not cold start.
        double eq34_prior_realization_max_gap_s{ 1800.0 };  ///< Warm conditioning of INC-1 (spec addendum): a cold host is dangerous only in proportion to the warm memory that contradicts it, so the block applies only while the key's last emitted datum realization is younger than this (s).  A network restart or cold start has no fresh realization, hence no blocking and the legacy convergence behaviour; an outage longer than this leaves nothing to disturb (the carried state has expired through its own staleness handling).  <= 0 disables the conditioning (always block on cold hosts).
        bool phase_datum_time_linked{ false };   ///< On a handover with no eligible host (or, with eq34_anchor_min_arc_ep = 0, on every handover), hold the key's shared phase bias at its last solved level with a soft pseudo-obs row instead of dropping or cold-pinning it (INC-2).  The phase-datum analogue of `code_datum_time_linked`.
        double phase_datum_bridge_sigma_m{ 0.05 };  ///< σ of the bridge row (m) (INC-2).  Loose relative to the phase data (which carries no information along the gauge direction the row realizes) but ≫ the rank-gate cutoff; a load-time K2-style margin check reports the actual margin in decades.
        int  phase_datum_bridge_max_ep{ 20 };    ///< Maximum consecutive epochs one bridge may be folded before it is dropped as stale (INC-2; standing invariant #1: a stiff time-linked quantity gets a bounded, event-responsive lifetime).
        double eq34_fresh_pin_sigma_scale{ 1.0 };   ///< σ multiplier applied to a datum row that had to be pinned on a host younger than eq34_anchor_min_arc_ep + eq34_fresh_pin_ramp_ep, decaying linearly over the ramp (INC-3).  1.0 = off (the exact 1e10 hard pin, byte-identical).
        int  eq34_fresh_pin_ramp_ep{ 0 };        ///< Length (epochs) of the fresh-pin deweight ramp above (INC-3).  0 = off.

        // --- Iono-deweight release hygiene ---
        // A station held at w_s = 0 by the adaptive SH-constraint deweight gate has no
        // tie rows folded, so its per-LOS slant-iono states spend the era unconstrained
        // by the field.  Because the network producer's per-station StateManager is a
        // frozen linearization point, a bad state does not decay during that era; it
        // stays parked.  When the gate releases, the tie rows re-couple those parked
        // states to the SH block with no signal of their own: the row is identical
        // across the break and only the coupling changes, so a parked state can move
        // the SH field by orders of magnitude in one epoch.  The increments below
        // (INC-1 to INC-3b) are default-off and, off, allocate no state, write no
        // state and remove no row.  Deliberately not in the warm-resume fingerprint:
        // this is a forward-only release-time state and row policy that changes the
        // meaning of no serialized quantity (as with adaptive_iono_q and the eq34
        // hygiene), which keeps the shared-snapshot event A/B valid.
        bool   iono_dw_release_reseed{ false };   ///< When a station's gate weight rises above iono_dw_release_floor_eps (its zero-weight era ends), reseed every per-LOS slant-iono state of that station from the current SH field evaluation, refloat its float ambiguities through the standard slip funnel (deferred one epoch), and erase its per-LOS deviation-stash entries (INC-1).  false = legacy (the release re-couples whatever was parked).
        double iono_dw_release_floor_eps{ 1e-9 }; ///< The decoupled-weight threshold (INC-1).  A station at or below this has its SH block dropped entirely (the assembly's w_s <= 0 early return), so a rise above it is the release edge.  Detected around the gate's rate-limited slew, never by comparing to exactly 0.0.
        double iono_dw_park_bound_tecu{ 0.0 };    ///< While a station is decoupled (weight <= eps), and, for warm-resume coverage, while the gate holds no samples for it at all, any slant-iono state whose magnitude exceeds this bound (slant TECU) is reset to the field evaluation immediately (INC-2).  0 = off.  A physical bound of O(300 TECU) sits three orders of magnitude below the parked values observed (about 1.15e6 TECU equivalent).
        double iono_dw_release_validate_k{ 0.0 }; ///< Fold a re-engaged SH<->slant tie row only if its standardized deviation |dev| / representation_sigma_tecu passes this k; a failing LOS stays decoupled and is re-tested every epoch (INC-3).  0 = off.  Reads the solved deviation that networkShDevVtec harvests, never the linearization point (which is provably frame-arbitrary; see NetworkIonoDwRelease.hpp).  Requires the T5 deviation channel (global_vtec_sh.rep_error_tau_s > 0); validated at load.
        double iono_dw_admit_validate_k{ 0.0 };   ///< The same screen at a LOS's first tie row (INC-3b).  A LOS crossing the elevation cutoff into a station's tie stack couples a slant column not yet validated to the SH field; this was the entry path in the measured event, at a station whose gate weight stayed at 1.0 throughout (its only IONO_DW event is an engage at the break, and that epoch's release wave first acts one epoch later).  A LOS is held for one epoch, during which its deviation is still harvested, then admitted if and only if that deviation passes k.  0 = off.  Requires the T5 deviation channel; validated at load.
        // Deterministic-parallel fan-out of the post-solve serial seam: the ~72 % of a
        // 305-station epoch that reduce/assembly parallelism does not cover (the DD
        // carry-fold of the invariant carry, @ref NetworkNormalEquations::foldReducedDdPriors,
        // and the D5 innovation-gate readouts in
        // @ref NetworkInvariantCarryDriver::installPosterior).  Unlike
        // @ref parallel_reduce this preserves serial≡parallel byte-identity: the
        // independent per-(const,signal)-block algebra (G, M=(Pᶻ+B)⁻¹, GMGᵀ, the
        // posterior mean/cov and innovation GᵀΣG products) is computed into pre-sized
        // per-block slots, then accumulated and installed serially in a fixed
        // block-index order, so the floating-point summation order never changes and
        // the output is bit-identical to the serial fold regardless of thread count.
        // Acts only when @ref parallel_stations is active (the gain exists only at
        // scale, where Eigen is already pinned to one thread for the epoch; the
        // deterministic-parallel region reuses that pin).  Default off: the seam runs
        // its original serial path, with no new code on the off path.
        bool parallel_seam{ false };      ///< Run the post-solve carry-fold / D5-readout seam deterministically in parallel (byte-identical, opt-in; needs @ref parallel_stations).
        // Thread the dense per-block linear algebra of the invariant carry across all
        // cores.  @ref parallel_seam parallelizes the carry only over the few
        // (const,signal) blocks (byte-identical) and leaves each block's O(K²–K³) dense
        // work single-threaded (Eigen is pinned to one thread for the epoch):
        // @ref NetworkInvariantCarryDriver::buildDdPriors (W·Pᶻ·Wᵀ), the per-station
        // reduction inside @ref NetworkNormalEquations::foldReducedDdPriors, and the
        // posterior-covariance readback of @ref NetworkInvariantCarryDriver::installPosterior.
        // With K ≈ 8k carried DD chords on a 305-station network that dense algebra is
        // ~80 % of the epoch (measured; see docs/SPEC_dev_tier_perf_cards.md).  When
        // armed, buildDdPriors and installPosterior thread their large dense GEMMs
        // through an Eigen un-pin, while foldReducedDdPriors (whose cost is the
        // per-station reduction with small inner-dimension matmuls and a serial LDLT,
        // which Eigen cannot thread) spreads that reduction across stations with OpenMP
        // (per-thread partials, ascending combine) with Eigen pinned, and
        // column-parallelizes the K×K middle inverse (byte-identical given Pᶻ+B).  Like
        // @ref parallel_reduce this reassociates the dense reduction, so a run with it on
        // is reproducible for a fixed thread count but not byte-identical to serial, and
        // not merely at the ULP level: through this ill-conditioned, stiff (near-zero
        // q_z) carry, the reassociation reached ~1e-4 at the solution level over a run
        // (physically null, sub-mm, but a real graded difference, not round-off).
        //
        // Usage rule: a run with parallel_carry on is for wall-clock A/Bs only.  No graded
        // or physics verdict comes from such a run until an on-versus-off dd-frac
        // equivalence grade has passed for that config; every graded conclusion comes
        // from an off (byte-identical) run.  This is a wall-clock mitigation, not a
        // structural fix: the fold is still O(K³) in the DD-chord count.  The
        // factored-Pᶻ restructure requested by INC-C2 is provably impossible for a
        // dense Σ_ss (§2 there), and INC-C0's census closed as case (a), so no
        // O(K·n_s²+n_s³) reduction is pending behind this knob.
        //
        // Not combinable with @ref parallel_reduce on the same solve: the config loader
        // forces parallel_carry off (with a warning) when both are armed, since
        // est.solve() is itself Eigen-threaded and would nest OpenMP × Eigen inside
        // reduce().  Acts only when @ref parallel_stations is active.  Default off: Eigen
        // stays pinned and the carry runs its @ref parallel_seam / serial path, byte for
        // byte as without this knob.
        bool parallel_carry{ false };     ///< Thread the invariant carry's dense per-block algebra (opt-in; gives up serial≡parallel byte-identity, ~1e-4 at the solution level, so graded verdicts come from off runs only, for ~2.4× wall-clock; needs @ref parallel_stations).

        // --- Visualization traces (consumed by the pyviz network viewers) ---
        bool trace_stations{ true };      ///< Emit a static @c network_stations.csv roster (station id, role, ECEF) once at startup.  Cheap (one tiny file); drives the worldmap viewer so it reflects the stations that actually loaded.
        bool trace_ionosphere{ false };   ///< Emit @c network_ionosphere.csv each emitted epoch, per station and satellite: the back-substituted slant-ionosphere correction x_r (column iono_m, relative to the state's seeded linearization point, not the absolute slant ionosphere), elevation/azimuth, the linearization point x_lin and the absolute value iono_total_m = x_lin + x_r (the column to grade).  O(stations x sats x epochs); off by default; enable for the iono viz.
        bool trace_estimability{ false }; ///< Emit the one-shot @c Network[estimability] warn dump on the first epoch that reaches the datum gate: the tail of the reduced shared-normal eigen-spectrum plus the shared parameters spanning each near-zero (unpinned-gauge) direction.  A full eigenvector decomposition of the reduced normals (O(n_shared³), about 1156³ at 305 stations), so it is off by default; enable only to debug an over- or under-specified datum.  The @ref NetworkSbasis rank gate itself is unaffected and always runs.
        int  trace_estimability_budget{ 6 };      ///< Number of gate evaluations the @ref trace_estimability eigen-census may spend (first-N latch; the default 6 equals the former hard-coded constant, so output is byte- and trace-identical) (RF-NS).  Seeded once into @c NetworkServiceCounters::spectrum_diag_budget by the @ref NetworkRunContext constructor and decremented at the dump site in @c NetworkSolveGlue.  Raise it to census a later deficit: with six dumps a field run's census falls entirely inside ignition, so a mid-run withhold stretch (such as the S-basis stretches of RF-2d) has no spectrum at all.  <= 0 disables the census (and the co-located one-shot W6-cb log) entirely.  Diagnostic only: the @ref NetworkSbasis rank gate and the solve are unaffected.
        bool trace_estimability_deficit_only{ false }; ///< Spend the @ref trace_estimability_budget only on epochs whose census shows a rank deficit (@c n_below > @c declared_dim) (RF-NS).  false (the default) spends it on the first N evaluations whatever they show, the original behaviour.  With it true the eigen-decomposition still runs on every budgeted epoch (it is the only way to know), but a clean epoch writes no rows and consumes no budget, so the budget lands on withheld epochs instead of on the first clean ones.  Trace only; no solve-affecting path reads it.
        bool trace_clock_full{ false };   ///< Append @c nis,mode columns to @c network_clock_monitor.csv (the exception to the append-only column rule: a trailing-column format break) and write every sat-epoch row (both mode filters replaced with @c true) instead of only near- or over-threshold rows (L4).  The file itself still opens only under @ref trace_slip_events && (@ref clock_monitor || @ref clock_model); this only widens and annotates what it records.  Off by default.
        bool trace_slip_events{ false };  ///< Emit the deep-investigation slip log: @c network_if_slip.csv (per-(station,sat,pair) between-epoch IF ΔΔφ and the elevation-normalized / per-station-EWMA threshold decision, every epoch) and @c network_raim.csv (per-(sat,station) post-fit standardized residual and cross-station consensus at any spike).  O(stations x sats x epochs) for the IF file; off by default; enable to debug IF-phase pre-detector and RAIM events.
        bool trace_slip_raw{ false };     ///< Emit @c network_slip_raw.csv: per-(station,sat) raw cycle-slip observables every epoch (Melbourne-Wübbena (m), geometry-free L1−L2 (m), first-signal phase-minus-code (m), and the per-band RINEX LLI flag) for the primary dual-frequency pair.  Computed directly from raw_observed and independent of whether any slip detector is enabled, so whether any satellite slipped at a station and epoch is always answerable (in particular through the receiver LLI, the loss-of-lock flag that no other network CSV reports).  Epoch-to-epoch differences are formed offline.  O(stations x sats x epochs); off by default.  Needs a wavelength provider (built lazily like the DD trace).
        bool trace_anchor_info{ false };  ///< Emit @c network_anchor_info.csv: the per-satellite information fraction of the a-priori clock anchor (anchor_info / reduced-normals total_info) each emitted epoch, so validation runs can confirm that the estimable clock is dominated by data rather than by the a-priori code-product anchor (B7).  Diagnostic only (anchor fold unchanged); off by default.  O(clock_sats x epochs).
        bool trace_dd_fractional{ false };///< Emit @c network_dd_fractional.csv: the per-epoch cross-station between-satellite double-differenced float ambiguity (from reconstructStation), keyed (constellation,signal), with its fractional-cycle residual; a sign-convention self-check and empirical evidence of < 0.1-cycle support for user AR (B5).  O(stations x sats x epochs); off by default.  An additional CSV only, no state mutation.
        int    trace_dd_fractional_max_pairs{ 0 };  ///< Cap on the DD pairs emitted (and covariance-propagated) per epoch across the whole @c network_dd_fractional.csv write: a deterministic seeded subsample keyed on the epoch (week,tow) (T2b).  0 = unlimited (the full-pair behaviour, which reproduces the benchmark).  Bounds the O(pairs) emit and the T2a @c jointConditionalCovariance request count (the dominant tracer cost at 305 stations).  Inert unless @ref trace_dd_fractional; diagnostic file only, no state change.
        /// Which satellite every chord of @c network_dd_fractional.csv is differenced against
        /// (§6.12.5).  @c Lowest reproduces the original trace byte for byte; @c Oldest ranks by
        /// continuous presence (see @ref NetworkDdFracRef).  Age is counted in traced epochs: the
        /// map advances inside the stride-gated emit block, so it costs nothing when the trace is
        /// off and is consistent with what the file contains.  Inert unless
        /// @ref trace_dd_fractional.  Trace only: never folded into the warm-resume fingerprint.
        NetworkDdFracRef trace_dd_fractional_ref{ NetworkDdFracRef::Lowest };
        /// Ionosphere constraint of the W6 satellite code-bias slice
        /// (@ref NetworkW6IonoConstraint) (RF-0b).  Default @c Auto reproduces the original
        /// three-way selection byte for byte; @c None runs the slice with no ionosphere
        /// constraint (no GIM, no SH field, @c estimation.ionosphere_weighting forced
        /// off), leaving the satellite code-bias datum to the IF pin and the
        /// per-(constellation, signal) sum-to-zero rows that
        /// @ref NetworkSbasis::buildCodeBiasDatumRows already folds.  Solve-affecting when
        /// set to @c None.
        NetworkW6IonoConstraint w6_iono_constraint{ NetworkW6IonoConstraint::Auto };
        /// Hard per-satellite geometry-free code-bias datum (@ref NetworkSatGfDatum)
        /// (RF-0c).  Default @c NoneMode builds no row and is byte-identical; @c Zero and
        /// @c BroadcastTgd each add one constraint row per satellite beside the
        /// ionosphere-free pin, folded before the gate at the same gauge weight, so
        /// @c Plan::declared_dimension is untouched.  Requires
        /// @c w6_iono_constraint = @c None.  Solve-affecting when armed.
        NetworkSatGfDatum sat_gf_datum{ NetworkSatGfDatum::NoneMode };
        /// One absolute sum-to-zero row per constellation on the ionosphere-free
        /// combination of the satellite code biases,
        /// @f$\sum_s (a_1 d^s_1 + a_2 d^s_2) = 0@f$, folded at the gauge weight in place
        /// of the per-signal sum-to-zero rows that @ref sat_gf_datum suppresses (RF-0e,
        /// arm e1).
        ///
        /// Why this row and not the per-signal ones: a per-signal sum-to-zero is
        /// inconsistent with per-satellite geometry-free pins.  The pins fix each
        /// satellite's code biases absolutely, so the channel sums are then
        /// `mu_j*c*Sum_s T_GD^s`, not zero, and the two families conflict (RF-0d removed
        /// that conflict).  This row constrains only the ionosphere-free part, which the
        /// geometry-free pins leave entirely free: they fix differences within a
        /// satellite, never the level.  Under the broadcast convention the row is also
        /// exactly true: the broadcast clock is referenced to the L1/L2 ionosphere-free
        /// combination, so every satellite's ionosphere-free code bias is zero there and
        /// so is the sum, which is why the residual against a `broadcast_tgd`-pinned
        /// roster is identically 0.
        ///
        /// What it fixes: with `code_datum_time_linked` armed, the per-satellite IF pins
        /// are change pins (@ref NetworkSbasis::buildCodeBiasDatumRows).  The RHS is the
        /// previous kept epoch's solved IF combination whenever both signals have a
        /// previous (veteran) value and the pair is unchanged; otherwise it is 0, an
        /// absolute anchor, and a satellite seen before (or any satellite of a resumed
        /// run) is marked for a re-seed bump.  Each satellite's level is therefore
        /// anchored once, at first sighting, and carried by a finite-weight row
        /// thereafter; nothing holds the level of the set, and over a 6 h run the clock
        /// / IF-bias split walks.  This row is the absolute anchor, and a strong one: its
        /// Jacobian carries one entry per satellite, so its leverage on the level scales
        /// with n and outweighs the n individual single-entry change pins.  At equal
        /// gauge weight the realized level lands at `carried/(n+1)`: the level is
        /// re-zeroed each epoch while the per-satellite differences stay carried.  The
        /// unit test computes this (L = 0.75, n = 32 → 0.0227).
        ///
        /// Default off: no row, byte-identical.  Inert (with a warning) without
        /// @ref sat_gf_datum, whose suppression it replaces.
        bool sat_if_level_datum{ false };
        /// Time-link the RHS of the geometry-free rows: a change pin on the
        /// geometry-free combination that carries the previous kept epoch's solved
        /// value, with the absolute `c*T_GD` (or 0 under @c Zero) used only at first
        /// sighting (RF-0e, arm e2).
        ///
        /// The alternative answer to the same RF-0d regression: instead of adding a
        /// level row, the geometry-free rows behave like every other datum row on a
        /// `code_datum_time_linked` lineage, so the absolute and the carried families
        /// stop pulling against each other.  Mutually exclusive with
        /// @ref sat_if_level_datum by configuration convention, not by construction:
        /// the two are the arms of one A/B, and arming both measures neither.
        ///
        /// Default off: the RF-0c/RF-0d absolute RHS, byte-identical.  Inert (with a
        /// warning) without @ref sat_gf_datum.
        bool sat_gf_datum_time_linked{ false };
        /// The broadcast T_GD pin applies only to the P-code pair (RF-0f).  A same-band
        /// companion on the pin's second band (GPS C2X beside C2W) is left unpinned, so
        /// its satellite code bias is `b(C2W) + ISC^s` with the inter-signal correction
        /// free.  This is exactly a reparameterization, since `b(C2X)` is already a
        /// per-(satellite, signal) shared column (registered through
        /// @c NetworkEstimator::registerCodeBias in ApplicationNetworkAssembly.cpp) and
        /// `ISC^s = b(C2X) - b(C2W)`.
        ///
        /// Why: pinning every cross-band companion at `c*T_GD` (RF-0d) asserts ISC = 0, a
        /// value RINEX navigation does not carry.  RF-0e measured the cost: the
        /// per-satellite dispersion of the ionosphere-free correction (the part a
        /// consumer's phase cannot absorb) rose from p90 0.55 to 1.53 m, and removing the
        /// channel outright gave the best field consumer then measured, except that the
        /// two stations carrying C2X as their only band-2 code were left without one.
        /// The ISC is a pure bias difference (no ionosphere, no clock), so it is
        /// estimable at any station tracking both signals of that satellite, and
        /// nothing needs to be asserted.
        ///
        /// Where it is not estimable: a satellite with no station tracking both signals
        /// has an unobservable ISC; its companion column is then pinned as under RF-0d
        /// (ISC = 0), counted, and its bias is not emitted (the wire carries `isc=obs`
        /// only on records whose ISC is observed).  The band-1 companion rule is
        /// unchanged, since that companion was already free; this is the band-2 half of
        /// the same principle.
        ///
        /// Default off: RF-0d's pin-everything behaviour, byte-identical.  Inert without
        /// @ref sat_gf_datum.
        bool sat_isc_state{ false };
        /// Leave an extra-band satellite code-bias column free where the data determine
        /// it, and let its (constellation, signal) channel carry the level with its
        /// sum-to-zero row (card B5D, docs/refound/B5D_band5_code_datum_2026-09-20.md).
        ///
        /// Why: RF-0d pins every registered code column beyond the ionosphere-free pair
        /// through its geometry-free combination `(d_k - d_ref)/(mu_k - 1) = c*T_GD`, an
        /// identity under the broadcast convention `d_j = c*T_GD*mu_j`, so the row
        /// asserts an inter-signal correction of zero.  That rule was meant for the
        /// same-band companions (freed by RF-0f), where the correction is centimetres.
        /// On a third band it is metres: in the estimator-free triple-code combination
        /// card TBI found the Block III satellites 4 to 5 m above the Block IIF ones,
        /// with the wire carrying only 0.43 m of a 2.97 m truth, the network pushing the
        /// rest into band 2, and a dual consumer replayed on the triple wire lagging by
        /// the same amount.
        ///
        /// Why it is estimable: with the slant ionosphere and the satellite clock fixed
        /// by the pinned pair, a cell carrying code on both of the pair's bands and on
        /// the extra signal determines `d_k` up to one constant per signal family, and
        /// that constant is exactly what the receiver's own extra-band code bias
        /// absorbs.  The channel's sum-to-zero row is the constant's datum: the same row
        /// RF-0d suppresses once every column of the channel is pinned, which re-enables
        /// by itself here because a freed column is not in the pinned set.
        ///
        /// Where it is not: a column the data do not determine this epoch never brings
        /// the absolute broadcast-frame target into a family that now stands in the
        /// sum-to-zero frame.  It is carried at its own previous solved geometry-free
        /// combination or, at a first sighting with nothing to carry, held by a single
        /// level row in the channel's own frame and withheld from the wire (as in RF-0f:
        /// a value no consumer can tell from an estimate is not emitted).  Both cases
        /// are counted.
        ///
        /// Default off: RF-0d's pin-everything behaviour, byte-identical.  Inert without
        /// @ref sat_gf_datum.
        bool sat_gf_extra_band_free{ false };
        /// A network station contributes, as code-only rows, every allow-listed band-1
        /// code it reports for a satellite besides the code its canonical latch selected
        /// (card SCC, docs/refound/SCC_second_code_channel_2026-09-21.md, R1).
        ///
        /// Why: with one canonical code per station and band, a satellite's C1W code
        /// bias is supported only by the stations that latched C1W (under the
        /// `priority` latch, about a third of the roster), so the wire carries a C1W
        /// bias for fewer satellites per epoch than C1C and many satellite-epochs lack
        /// it (card CSL §9).  A consumer whose code bias vanishes departs by tens of
        /// metres (card MSC).  The two families do not compete for information but for
        /// the same stations; letting a station report both makes them share the
        /// stations.
        ///
        /// What it does not add: no second phase signal, no ambiguity, no phase-bias
        /// column (the exact-integer slip witness is a separate card) and no datum
        /// change (R3): a same-band companion of the pin's first signal is already free,
        /// and the family's own time-linked sum-to-zero row closes the one null
        /// direction the new receiver state opens.
        ///
        /// Default off (byte-identical).  Under `processing.channel_mode: all` the knob
        /// is subsumed: it stays armed, the allow-list is ignored, and
        /// @ref second_code_sigma_scale applies to every non-canonical code row (card
        /// SID phase 6b).  Refused without @c network.carry_rx_code_bias (the new
        /// receiver state has to be restored across epochs like every other receiver
        /// code bias) and with fewer than two entries in the allow-list.  Fingerprinted
        /// only when armed.
        bool second_code_channel{ false };
        /// Explicit allow-list of band-1 code signals a station may contribute under
        /// @ref second_code_channel (card SCC).  The loader refuses an entry that is not
        /// a 3-character code type starting with 'C', and any entry whose band digit is
        /// not '1' (band 2 is left to a follow-up card: it would make the
        /// @ref sat_isc_state bridging roster non-empty for the first time).  C1X and
        /// C1L are admitted only if listed, never by default, and are unvalidated.
        std::vector<std::string> second_code_signals{ "C1C", "C1W" };
        /// Multiplier on the code sigma of the rows that R2 routes through the receiver
        /// inter-signal state: the companion rows and any fallback code (card SCC, R5).
        /// Under `processing.channel_mode: all` it applies to every non-canonical code
        /// row.  At least 1; the default 1.0 is never applied (byte-identical).  Values
        /// below 1 are refused rather than clamped, as for
        /// @ref extra_band_code_sigma_scale: a config that trusts a second code more
        /// than the canonical one asks for the opposite of this knob and is treated as a
        /// typo.
        double second_code_sigma_scale{ 1.0 };
        /// A network station contributes, as ordinary float-ambiguity phase rows, every
        /// allow-listed band-1 carrier phase it reports for a satellite besides the
        /// phase its canonical latch selected (card SID phase 6a,
        /// docs/refound/SID_signal_identity_2026-09-22.md §4; the phase counterpart of
        /// card SCC).
        ///
        /// Why: with one phase per station and band, a satellite's L1W phase bias is
        /// supported only by the stations that latched L1W (18 of 58, a median of 4
        /// carried stations per column-epoch), and 23.6 % of satellite-epochs have a
        /// clock record but no L1W phase bias.  The same thinness causes the
        /// coverage-gate drop edges of card EV7, all on L1W keys.
        ///
        /// What it does not add: no receiver inter-signal phase state (a float
        /// ambiguity already absorbs an arbitrary constant per arc, so such a state
        /// would sit in the null space; the audit's risk 5), and no new datum row (the
        /// per-(satellite, signal) anchor election simply gains candidates).
        ///
        /// Default off (byte-identical).  Under `processing.channel_mode: all` the knob
        /// is subsumed: the allow-list is ignored and @ref second_phase_sigma_scale
        /// applies to every non-canonical phase row (card SID phase 6b).  Otherwise
        /// refused unless the float vehicle runs with a held basis
        /// (@c network.ambiguity_vehicle = float_kf with
        /// @c network.phase_datum_held_basis), and with fewer than two entries in the
        /// allow-list.  Fingerprinted only when armed.
        bool second_phase_channel{ false };
        /// Explicit allow-list of band-1 phase signals a station may contribute under
        /// @ref second_phase_channel (card SID phase 6a).  The loader refuses an entry
        /// that is not a 3-character phase type starting with 'L', and any entry whose
        /// band digit is not '1' (band 2 belongs to phase 6b, which lifts the interlock
        /// on `processing.channel_mode` instead).
        std::vector<std::string> second_phase_signals{ "L1C", "L1W" };
        /// Multiplier on the phase sigma of the second-phase rows (card SID phase 6a,
        /// R5).  Under `processing.channel_mode: all` it applies to every non-canonical
        /// phase row.  At least 1; the default 1.0 is never applied (byte-identical).
        /// Values below 1 are refused rather than clamped, as for
        /// @ref second_code_sigma_scale.
        double second_phase_sigma_scale{ 1.0 };
        /// Derived at load, never read from JSON (card SID phase 6a, R6).  Set when the
        /// second phase channel is armed without
        /// `network.cycle_slip.gap_reentry_per_signal`, the only tier that can respond
        /// to a second-phase arc's re-entry.  This is a warning, not a refusal; the
        /// run-end readout restates it so the posture is clear from the log alone
        /// (standing invariant #1).
        bool second_phase_warn_no_per_signal{ false };
        /// Derived at load (card SID phase 6a, R4).  Set when the second phase channel is
        /// armed with the IF-phase time-difference tier on and `if_phase_td_pairs` not
        /// 'all': two band-1 phases share a frequency, so that tier's single pair is
        /// degenerate and produces no rows for such a satellite.
        bool second_phase_warn_iftd_pairs{ false };
        /// Band scope of the T4 attribution erase (@ref NetworkRaimEraseBandScope) (§Z).
        /// Default @c All reproduces the original instruction stream byte for byte.
        /// Solve-affecting when armed (it changes which carried levels survive an
        /// erase), so it is fingerprinted like any other carry-shaping knob.
        NetworkRaimEraseBandScope raim_erase_band_scope{ NetworkRaimEraseBandScope::All };
        /// Which vehicle carries a network ambiguity between epochs
        /// (@ref NetworkAmbiguityVehicle) (RF-1 §7.1).  Default @c Level is the zero-q
        /// phase-leveling scalar carry, byte-identical.  @c FloatKf arms the joint local
        /// block (RF-1 step 3); the drop floor and the declared anchor S-transform
        /// (steps 4 and 5) are armed by their own knobs below and act only under it.
        NetworkAmbiguityVehicle ambiguity_vehicle{ NetworkAmbiguityVehicle::Level };
        /// Floor (m²) on the birth prior a dropped or newborn arc carries, applied as
        /// @c var_birth = max(var_first, amb_float_birth_var_m2) (RF-1 §3.3(b)).  0.0 =
        /// use the arc's own first-stash conditional variance unfloored (the original
        /// quantity).  The value is set empirically: it starts at the posture's
        /// code-noise scale (an a-priori (0.5 m)² at the reference elevation,
        /// elevation-mapped like the row weights) and is then set once per bed from the
        /// σ̂₀(pl) that RF-1 step 2's D4 read measures in the young arc-age bucket on the
        /// RF-0 posture.  It is not a tuning constant to be swept.  Applied only under
        /// @c FloatKf (on the level vehicle the loader warns that it is inert); recorded
        /// in the run manifest.
        double amb_float_birth_var_m2{ 0.0 };
        /// Floor (m²) applied at a drop only, so the restored prior is
        /// @c max(var_birth, var_first, amb_float_birth_var_m2, amb_float_drop_var_m2),
        /// elevation-mapped exactly as @ref amb_float_birth_var_m2 is (RF-3a,
        /// docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  The birth stash is
        /// untouched, which is the reason for a separate knob: a birth floor of 4 m²
        /// removes the post-drop walk (post-fit code rows above 3 m at full weight fell
        /// from 404 to 101, and the settled consumer improved by about 1 cm in hours 3
        /// to 6) but costs every birth (hour 1 worse by about 0.1 m).  The defect is the
        /// re-coupling edge of the slip and gap re-entry response, not the birth, so
        /// the floor belongs at the drop.
        ///
        /// Mechanism (RF-1 §3.4): a refloat restores the arc's first-stash variance
        /// (0.036 m² in the measured case, from two hours earlier), so a level wrong by
        /// 40 m folds at 0.19 m and decays geometrically over 24 epochs while the code
        /// rows carry the misfit at the ionospheric band ratio.  A drop floor states
        /// what a re-determined level is worth.
        ///
        /// Solve-affecting when armed; 0.0 is the original behaviour, byte-identical.
        /// Not fingerprinted, for the same reason RF-1's drop is not: it is a
        /// forward-only variance at a response and changes no carried state's meaning.
        double amb_float_drop_var_m2{ 0.0 };
        /// Warm-up gate on @ref amb_float_drop_var_m2 (RF-3c): the floor applies only to
        /// a drop taken at processed epoch >= this value; before it, a drop restores
        /// @c max(var_birth, var_first, amb_float_birth_var_m2).  0 (default) = no gate,
        /// which is RF-3a's behaviour.
        ///
        /// Measured motivation (RF-3a's gate G1): a drop-only floor does not leave hour
        /// 1 alone, because drops also happen during convergence.  The floor improved
        /// the settled hours it was aimed at and cost hour 1 (about 0.09 m at the
        /// reference station).  The gate separates the two populations by age: an arc
        /// dropped while the network is still finding its clocks is a convergence
        /// event, and the floor's statement about what a re-determined level is worth
        /// applies to a settled network.
        ///
        /// The clock is the producer's processed-epoch counter (@c n_loop_epochs), the
        /// same abscissa that @c network.gross_los_warmup_epochs and the phase-leveling
        /// warm-up read, so 120 is one hour at 30 s.  Solve-affecting when it changes an
        /// armed floor's reach; inert when @ref amb_float_drop_var_m2 is 0.
        int amb_float_drop_floor_after_ep{ 0 };
        /// Process noise (m²/s) on the ambiguity block under @c FloatKf (RF-1 §3.5).
        /// The card's posture is 0.0; any nonzero value is a separate arm that needs its
        /// own evidence.  An ambiguity is a random constant punctuated by discrete
        /// slips, and q = 0 is exact recursive least squares for it.  A q-ladder is
        /// falsified as a fix for the sibling lock-in (settled bias unchanged or worse,
        /// the first-injection jump identical on every rung, integers strictly worse),
        /// and the level-channel corollary of CLAUDE.md amendment 6 says the same: the
        /// level is not fixed by tuning a carry's memory model in either direction.  The
        /// knob exists so the default is explicit and the run manifest records it.
        /// Parsed, validated (>= 0) and reported at run end; no code applies it.
        double amb_float_q_m2s{ 0.0 };
        /// Express a (satellite, signal) phase-anchor handover as the exact, declared
        /// carry S-transformation (RF-1 §3.6, step 5, its own arm): shift every
        /// station's carried @c value_abs of that family by the incoming anchor's own
        /// level, leave every variance untouched, and check that the incoming anchor's
        /// column then reads 0, instead of the network-wide erase of
        /// @c networkApplyPhaseAnchorCarryReref.  A state that is erased at every anchor
        /// handover does not persist as a state.  Solve-affecting when armed; default
        /// @c false is byte-identical.  Acts only under @c FloatKf.
        bool amb_float_declared_handover{ false };
        /// The per-line-of-sight phase nuisance state (card LGM,
        /// docs/refound/LGM_los_phase_nuisance_2026-10-01.md section 4.1): one band-common,
        /// phase-only Gauss-Markov state per (station, satellite), +1 on every phase row
        /// of the line of sight and nothing on code rows, carried in the station's
        /// whitened local block with transition phi = exp(-gap / tau), born at
        /// N(0, sigma(e)^2) by a pseudo-observation row.  It models the stations'
        /// time-correlated phase error (card SFR) that the network otherwise states as
        /// white.  Requires the float vehicle, phase_leveling and the conditional
        /// coupling; refuses the joint whitening and carry_invariant.  Default false is
        /// byte-identical.
        bool phase_colour_state{ false };
        /// Zenith sigma of the state's stationary prior (m); the elevation law is
        /// sigma(e) = sigma_zen / sin(max(e, phase_colour_elev_floor_deg)), the sim
        /// fixture's (card SFR section 4.2).  Must be > 0.
        double phase_colour_sigma_zen_m{ 0.005 };
        /// Time constant tau (s) of the Gauss-Markov transition.  Must be > 0.
        double phase_colour_tau_s{ 300.0 };
        /// Elevation (deg) below which sigma(e) stops growing.  In [0, 90).
        double phase_colour_elev_floor_deg{ 20.0 };
        /// A carried entry older than this (s) is not folded (the line of sight re-starts
        /// from its birth prior) and is pruned at the harvest.  Must be > 0.
        double phase_colour_max_gap_s{ 900.0 };
        /// Per-line-of-sight trace of the state (@c network_phase_colour.csv: epoch,
        /// station, satellite, elevation, estimate, its standard deviation, born or
        /// carried), written at the harvest.  Trace class; inert without the state.
        bool trace_phase_colour{ false };
        /// Card AIL: the ionosphere-free ambiguity information trace
        /// (@c network_amb_if.csv).  Per station-epoch, per line of sight whose two bands
        /// (the first two of its constellation's traits table: GPS L1/L2, Galileo E1/E5a;
        /// BeiDou is not traced) are both carried in the station's whitened block, the
        /// variance of the ionosphere-free ambiguity combination at the inject (from the
        /// block's prior covariance) and at the harvest (from the harvest block), both
        /// conditional on the border, in full and with the station's common mode over the
        /// lines of sight of the same constellation projected out.  Trace class: no solve
        /// effect.  Inert without the float vehicle's local block.
        bool trace_amb_if{ false };
        /// Per-arc carry-state trace (@c network.trace_amb_state) to
        /// @c network_amb_state.csv (RF-1 §7.4).  One row per carried (station,
        /// satellite, signal) per traced epoch, with what was carried (@c value_abs,
        /// @c var, @c var_first, @c var_birth), what was folded (@c fold_var,
        /// @c folded, @c route), the trajectory's abscissa (@c n_folds, @c age_ep,
        /// @c age_since_relevel_ep) and the arc's events (birth / drop / erase /
        /// stransform).  It gives the per-arc trajectory of the carry itself, which
        /// @c network_prior_residual.csv cannot (it is aggregated per (family, label)
        /// per epoch) and which a per-LOS code-residual statistic only approximates
        /// (design §7.3 step 2).  Measure-only: every value written is read off state
        /// the harvest / inject already computed.  Default off: no file, no rows,
        /// byte-identical.
        bool trace_amb_state{ false };
        /// Epoch stride of @ref trace_amb_state: rows are written every N-th processed
        /// epoch (RF-1 §7.4).  1 (default) = every epoch, which the pre-registered D1/D2
        /// fold-trajectory reads need; raise it only to bound the file on a multi-hour,
        /// many-station run (with a stride > 1, @c n_folds and the consecutive-row prior
        /// residual are read on a subsample, so D1's fit abscissa stays @c n_folds,
        /// never the row index).  Event rows (birth / drop / erase) are written every
        /// epoch regardless of the stride, so the event inventory is complete.
        int trace_amb_state_every{ 1 };
        /// Band scope of the whole-satellite detector refloats
        /// (@ref NetworkRefloatBandScope) (§Z).  Default @c All is byte-identical.
        NetworkRefloatBandScope cycle_slip_refloat_band_scope{ NetworkRefloatBandScope::All };
        /// How many band pairs the IF-TD detector evaluates per satellite
        /// (@ref NetworkIfPhaseTdPairs) (§14.10).  Default @c First is the original
        /// single pair, byte-identical.  Solve-affecting when armed (new pairs can
        /// flag, and their flags refloat and veto), so it is fingerprinted like any
        /// other detection-shaping knob.  Inert without
        /// @c network.cycle_slip.if_phase_td.
        NetworkIfPhaseTdPairs cycle_slip_if_phase_td_pairs{ NetworkIfPhaseTdPairs::First };
        /// Which MW pairs the network forms (@ref NetworkMwDetectPairs) (card L5P).
        /// Default @c All is the original path, byte-identical.  Detection-shaping
        /// when armed (a pair that is not formed cannot flag, refloat or feed the
        /// common-band scope); inert on a two-band lineage and without
        /// @c network.cycle_slip.mw_detect.
        NetworkMwDetectPairs cycle_slip_mw_detect_pairs{ NetworkMwDetectPairs::All };
        /// Multiplier on the code sigma of an extra-band signal (one whose band is
        /// neither band of the constellation's ionosphere-free pin pair) in the
        /// network solve (card L5P).  The default 1.0 is byte-identical: the scale is
        /// never applied at 1, so the row weight keeps the exact 1/sigma^2 value.
        /// Values below 1 are refused at load.  The closed-loop consumer is pinned to
        /// 1 in @c deriveClosedLoopConsumerConfig: this is a producer-side lever, and
        /// the consumer's own code weighting is a separate graded variable.
        double extra_band_code_sigma_scale{ 1.0 };
        /// Anchor-slip handover (docs/SPEC_event_harness.md §12.7): when a slip
        /// response frees the arc that anchors a (sat, signal) phase-bias family, stage
        /// the one-epoch candidacy ban so that the same epoch's election hands the
        /// family to a successor at the successor's own carried @c value_abs, instead
        /// of re-realizing the gauge from an arc whose level is about to be re-drawn
        /// from one epoch of code.  Default @c false is byte-identical.
        /// Solve-affecting when armed, but deliberately not fingerprinted: it is a
        /// forward-only response knob (the relevel_free_epoch exemption class), which
        /// changes what a run does after a slip, never the meaning of the state a
        /// snapshot restores.
        bool cycle_slip_anchor_slip_handover{ false };
        /// MW evidence refloat: admit a detect-only MW wide-lane flag to the PASS-A
        /// refloat path on the strength of the detection (how far above its own
        /// adaptive threshold the step stood, on a mature bias-free lane) rather than
        /// on the cleanliness of an integer the consumer never applies.  The legacy
        /// admission reuses the T0 repair gate (known integer, rounding residual
        /// <= turbo_edit_frac_max), which drops real slips: a -99 cycle L5 step
        /// flagged at 78x threshold with a 0.183 cycle residual (1.07 m on a 5.86 m
        /// lane, inside the lane's own 1 sigma) was refused.  Default @c false is
        /// byte-identical; the counters are collected either way, so a run with it
        /// off reports what the rule would have admitted.  Solve-affecting when armed,
        /// fingerprint-exempt (forward-only response knob, the relevel_free_epoch /
        /// anchor_slip_handover class).
        bool cycle_slip_mw_evidence_refloat{ false };
        /// Evidence gate: the flag's |deviation| / threshold must reach this.  3.0 is
        /// the value the false-alarm census was taken at: every mature bias-free
        /// detection at or above it on a quiet 240-epoch, 60-station leg was a real
        /// slip.
        double cycle_slip_mw_evidence_ratio_min{ 3.0 };
        /// Evidence gate: the arc's sample count at the flag must reach this.
        /// Young arcs have no established level, so their "deviation" is measured
        /// against a threshold that is itself still converging.
        int cycle_slip_mw_evidence_min_arc_epochs{ 10 };
        /// Common-band scope (SPEC_event_harness.md §14): free only the band the
        /// evidence names.  A single-band slip on a triple-frequency arc moves every
        /// MW pair that contains the band (an L5 slip fires (1,5) and (2,5) and leaves
        /// (1,2) silent), and each firing pair's drain refloats both of its bands, so
        /// the per-epoch union is all three: the §9 all-band floor (0.45-0.65 cyc for
        /// 45 epochs at low elevation) paid for a slip that touched one band.  This
        /// knob forms one per-(station, satellite, epoch) verdict from positive
        /// exoneration (a silent, mature MW pair exonerates both of its bands), and
        /// every drain then frees only those of its bands that are implicated.
        /// Without exoneration the response is unchanged.  Deliberately not the
        /// study-3 @ref cycle_slip_refloat_band_scope mechanism, which picked the band
        /// from the MW/GF decomposition and was falsified on the storm bed; this rule
        /// is set-theoretic and needs no decomposition.  Default @c false is
        /// byte-identical (no verdict object is consulted).  Solve-affecting when
        /// armed, fingerprint-exempt (forward-only response knob, the
        /// relevel_free_epoch / anchor_slip_handover class).
        bool cycle_slip_common_band_scope{ false };
        /// Exoneration maturity gate: a silent MW pair may exonerate its bands
        /// only when its arc has at least this many samples.  An immature arc could
        /// not have flagged at all, so its silence carries no information, and
        /// affirming cleanliness from it would keep a genuinely slipped band pinned.
        /// The detector's own affirmative-cleanliness set already requires 10
        /// samples, so the default is inert by construction; a larger value tightens
        /// it, and the counter @c cb_immature_pairs reports what it refused.
        int cycle_slip_common_band_min_arc_epochs{ 10 };
        /// Reference-arc clause of the common-band scope (§22).  The scope's IF-TD
        /// veto refuses a silent MW pair only when the pooled IF-TD test flagged it,
        /// and on the chain's reference satellite that test is structurally
        /// unavailable: its double difference is identically 0
        /// (@c IfJumpInfo::is_reference), so `|dd − med| > thr` is false however large
        /// the slip, and the pair lands in the silent set as if it had been cleared.
        /// Armed, such a pair is untested instead: refused as an exonerator, counted,
        /// and named `iftd(<pair>)=ref_arc` in the verdict basis.  An equal-count slip
        /// on a reference arc then falls back to the all-band response instead of
        /// narrowing to an innocent band (measured on field data, SPEC_event_harness
        /// §20.1/§22).  Applies to every chain's reference, the first chain included:
        /// the per-station reference gate is a separate instrument whose decision the
        /// preview never sees.  Cost: a genuine single-band slip on that arc also
        /// stops narrowing when its only silent pair is the untested one.  Requires
        /// @ref cycle_slip_common_band_scope (the loader disarms and warns without it).
        /// Default @c false is byte-identical: the untested set is never populated and
        /// the rule's branch is unreachable.  Solve-affecting when armed,
        /// fingerprint-exempt (forward-only response knob, the
        /// @ref cycle_slip_common_band_scope class).
        bool cycle_slip_common_band_ref_arc{ false };
        int    trace_dd_fractional_epoch_stride{ 1 };  ///< Write @c network_dd_fractional.csv only every Nth processed epoch (1 = every epoch) (T2e).  Fraction statistics do not need every epoch; combined with @ref trace_dd_fractional_max_pairs this makes a traced convergence run affordable.  Trace only (never folded into the warm-resume fingerprint).  Inert unless @ref trace_dd_fractional.
        /// Emit @c network_phase_anchor.csv (S1): per processed epoch, one row per
        /// gauge-pinned ambiguity as (week,tow,cons,signal,sat,anchor_sta), i.e. which
        /// station realizes the phase datum for each (constellation, signal,
        /// satellite) freedom.  This is the anchor-partition input of the lane-ladder
        /// spectrometer: the open datum hypothesis predicts that DD satellite pairs
        /// pinned at different stations are non-integer while same-anchor pairs stay
        /// clean, and that split cannot be tested without the map.  No other artifact
        /// provides it: @c network_anchor_info.csv is the a-priori clock anchor (no
        /// signal, no station) and @c network_dd_fractional.csv deliberately skips
        /// pinned ambiguities.  The row count is bounded by the number of gauge
        /// freedoms (satellites x signals), not by station pairs, so it is affordable
        /// even at 305 stations.  Trace only: read straight off the election that
        /// already ran, never folded into the warm-resume fingerprint; off is
        /// byte-identical.
        bool   trace_phase_anchor{ false };
        int    epoch_decimation_n{ 1 };  ///< Process only every Nth synced epoch in the network loop (1 = off = every epoch) (T2e).  N = 2 gives a 60 s effective rate and roughly halves convergence wall time; the convergence horizon is driven by physical time, so a coarser rate loses little.  Estimation-relevant (it changes the carried-state cadence and the process-noise dt), so it is folded into the warm-resume fingerprint when != 1 and a decimation mismatch invalidates a resume.  The physical-time start-offset and max-duration guards still evaluate on every raw epoch (no overshoot).
        bool   iono_supply_formal_sigma{ false };  ///< When supplying the co-estimated SH-VTEC slant iono to the closed-loop consumer (@c closed_loop.consume_network_iono), RSS the formal σ of the SH coefficients (√(aᵀ Q_sh a), mapped to slant metres) onto the per-LOS representativeness floor, enforce a floor after the scale, and skip the supply for satellites without fresh OSB (satellite code bias) coverage this epoch, since an under-observed field value is not reliable (T3f).  In-process supply only (no product bytes); off: the representativeness-floor-only σ (byte-identical consumer solution).
        bool trace_flag_rates{ false };   ///< Log a per-tier flag-rate summary at the end of the run (B8): for each RAIM/QC tier (IF-phase slip, within-epoch trigger, post-fit RAIM cell/consensus, clock-model NIS, clock monitor), accumulate {opportunities, flags} over the run and report the realized rate against the one-sided Gaussian α implied by the configured k.  Also writes @c network_flag_rates.csv and @c network_run_summary.csv (L3).  Measurement only, no threshold or decision change; off (default) is byte-identical.
        double rhs_attrib_tow{ -1.0 };    ///< Reduced-RHS class attribution (filter/NetworkRhsAttrib; diagnostics only, not fingerprinted): first target TOW at/after which the per-class attribution dump fires (< 0 = off, the default).  Dumps `logs/rhs_attrib/attrib_<tow>.csv` plus per-class full vectors, with an explicit closure check against the kernel's reduced RHS.
        int    rhs_attrib_n_ep{ 2 };      ///< Number of consecutive processed epochs to attribute starting at @ref rhs_attrib_tow (default 2: a pre-break/break pair).

        // --- Prefit census / structural fuse / topology ledger / flight recorder ---
        // docs/SPEC_prefit_census_observability.md.  All default-off, structurally
        // byte-identical when off, and none fingerprinted (forward-only observability,
        // as with adaptive_iono_q and the eq34 hygiene), so a shared-snapshot replay
        // leg with any of them armed stays a valid A/B against an unarmed control.  The
        // fuse is the one solve-affecting member of the family and has its own arm.
        bool   trace_prefit_census{ false };  ///< Per-data-class common-mode-removed prefit statistics into @c network_epoch_status.csv, plus PREFIT-tier `prefit_exceedance` events (INC-1).  Measure-only.  The metric is per station and row type, robust-centred (s = |y − median_t(y)|·√w), in effect a between-satellite single difference, so a whole-station receiver-clock step (ms-scale, |y| ≈ 3e5 m on every row) cancels while a single parked cell does not.  It is the one pre-solve screen that sees such a cell: post-fit σ̂0 absorbs it through the free ambiguities, and `raim_within_epoch` is gated on carry_warm.
        /**
         * @brief Event domain of INC-1: which data classes may raise a
         *        `prefit_exceedance` event (default @c "code,sh,other"; @c "all"
         *        includes phase).  The per-class columns always report all four.
         *
         * On a 63-station, 121-epoch replay at threshold 1e3, the four classes behave
         * very differently as tripwires:
         *
         *  - @c obs_code: 6 events, all on the injected fault cell and none elsewhere,
         *    with s growing as the fault intensified.  No false positives.
         *  - @c iono_sh_row: 51 events over 44 stations, clustered just over the
         *    threshold (s ≈ 1.2–2.4e3), with one outlier 40× higher on the epoch after
         *    the discharge.  Usable: the real event stands out from the floor.
         *  - @c obs_other: 0 events.
         *  - @c obs_phase: 608 events over 55 stations.  A phase row's prefit
         *    innovation is arc-specific (it contains that arc's ambiguity
         *    linearization point), so a station's phase rows are not a common-mode
         *    population: the station median can move by more than 100 km within one
         *    leg while individual rows sit over 100 km away from it.  The median is
         *    not a centre there, and the statistic carries no information: 91 % of
         *    all events, none of them findings.
         *
         * Phase events at that rate would bury the real ones, so phase is out of the
         * event domain by default and its suppression is counted
         * (`events_class_skipped`), never silent.  Tokens: @c code, @c phase,
         * @c other, @c sh, @c all, @c none.
         */
        std::string prefit_event_classes{ "code,sh,other" };
        double prefit_event_sigma{ 1000.0 }; ///< Event threshold on the standardized deviation (INC-1).  Deliberately a factor of about 25 above the storm-time level (~40σ): this instrument targets the wiring, seed and model-fault regime, not robustness tuning.
        int    prefit_event_holdoff_ep{ 20 };///< Per-(event, station, satellite, class) event rate limit, in processed epochs (INC-1/INC-2).  A parked cell keeps logging its identity (so its duration is readable) without writing 120 rows per hour.
        double prefit_fuse_sigma{ 0.0 };     ///< Structural fuse threshold (INC-2; 0 = off).  Solve-affecting: a data row whose common-mode-removed s exceeds this has its weight capped to w·(S/s)², a continuous, order-preserving cap rather than a drop (the prefer-fallback directive), so the row still carries sign information.  Skips rows with w ≥ 1e6 (pin/constraint regime) and rows whose station type-median was unavailable (n < 4).  Recommended value when armed: 1e3, a factor of about 260 below the measured fault cell (s ≈ 2.6e5) and about 25 above the storm-time level (~40σ).  It also bounds the discharge magnitude of any future release of a parked bad state, whatever the valve.
        /**
         * @brief Fuse domain of INC-2: the comma-separated data classes the cap may act
         *        on (default @c "code,sh"; @c "all" = every data class = the spec's §4
         *        form).
         *
         * Deviates from SPEC_prefit_census_observability §4 ("For DATA-class rows only")
         * on measured evidence.  A phase row's prefit innovation contains the arc's free
         * ambiguity linearization point, and the state initializer seeds a fresh arc so
         * that its innovation is ~0 while its converged peers carry the station's
         * residual phase level.  In steady state that level is ~1 m (against a code
         * median of ~20 m) and nothing is near the fuse; but on a warm-resume or
         * cold-start epoch every arc is re-seeded at once, and the pool's centre is
         * whatever the seeding produced (a phase median of −4248 m two epochs after a
         * resume gave s ≈ 3.8e5 on a fault-free row).  Capping those weights would slow the
         * re-convergence of every new arc for no fault: a row whose own free state
         * absorbs the offset exactly is a birth, not a fault.
         *
         * @c obs_code and @c iono_sh_row have no such free per-row absorber (the
         * receiver clock and code bias are shared across satellites, so a
         * single-satellite excursion survives the centring), which is why they form
         * the default domain; the fault class the fuse targets is an @c obs_code cell,
         * so the designed target is fully covered.  Widen to @c "all" deliberately, on
         * its own arm, if a phase-side fault is ever the thing being fused.
         *
         * Tokens: @c code, @c phase, @c other, @c sh, @c all, @c none.  Inert unless
         * @ref prefit_fuse_sigma > 0.
         */
        std::string prefit_fuse_classes{ "code,sh" };
        bool   prefit_fuse_require_uncheckable{ false }; ///< Composition with the redundancy census (docs/SPEC_redundancy_census.md) (INC-2): fuse only rows that the census marks r < ε, i.e. act only where the misfit is invisible to every residual test.  The census runs post-solve while the fuse acts pre-solve, and no persistent set of uncheckable cells connects them, so the loader warns and forces this off.  The field exists so that a config written against the companion card fails loudly instead of fusing every large row.
        double seed_birth_check_sigma{ 0.0 };///< Seed-birth tripwire (INC-3; 0 = off).  Measure-only: applies the INC-1 statistic at this tighter threshold to the rows of a (station, satellite) whose per-LOS states were born this epoch.  A correctly self-seeded birth has s ≈ 0 by construction, so any fire localizes a seed-path defect to an exact epoch and site (recommended 100).  Makes the census walk run even with @ref trace_prefit_census off.
        bool   trace_topology{ false };      ///< Topology ledger (INC-4): TOPOLOGY-tier events for estimation-topology changes: per-station SH-tie row-count deltas (the deweight valve, whose weight change from 0 to 0.25 on one added tie row is otherwise unlogged), per-(station, satellite) row-kind changes while the satellite stays present, and shared-column registry deltas.  Measure-only; makes the census walk run.
        bool   rhs_attrib_on_trip{ false };  ///< Flight recorder (INC-5): arm the estimator's shared-info capture recorder every epoch and fire @ref networkRhsAttribution automatically at the first trip of a QC era (SH-gate engage, A4 σ̂0 withhold, clock runaway, a₀₀-band exit), instead of at a hand-picked @ref rhs_attrib_tow.  With this armed, the next discharge of any class is attributed to its source in the run where it first happens, without a separate investigation.
        bool   rhs_attrib_on_trip_prev{ true }; ///< Also dump the retained previous epoch on a trip (INC-5).  The pre-trip frame is the informative one: it shows the weight flip that the trip epoch alone shows only as its aftermath.  Costs one epoch of retained assemblies and kernel blocks (order 100 MB at 63 stations), so it disables itself above @ref rhs_attrib_prev_max_stations.
        int    rhs_attrib_prev_max_stations{ 128 }; ///< Network size above which previous-epoch retention is disabled, with a one-shot warning (INC-5).  A trip still dumps the current epoch.
        // --- Redundancy / testability census (docs/SPEC_redundancy_census.md;
        //     filter/NetworkRedundancyCensus) --------------------------------
        // Measure-only and not fingerprinted, like the rhs_attrib block above: a
        // shared-snapshot replay leg with only these armed stays a valid A/B against
        // an unarmed control.  Magnitude alone cannot separate a fault from an
        // extreme (a 1 ms receiver clock step is 3e5 m on every row of a station at
        // once); the discriminator is testability, and the alarm is the cross-flag
        // "uncheckable and large", never either half alone.
        int    redundancy_census_stride{ 0 };   ///< Row-redundancy and unobservable-local sweep every Nth processed epoch (0 = off, the default).  Recommended 10 when armed, one sweep per 5 min at 30 s cadence: a parked state persists for many epochs, so minutes of detection latency are acceptable for an alarm, and the 100-300 ms sweep cost is amortized.  A flight-recorder trip epoch (an armed @ref rhs_attrib_tow window) always forces a full sweep regardless of the stride phase.
        double redundancy_eps{ 0.05 };          ///< Local redundancy floor: a data row with @f$r_i < \varepsilon@f$ is uncheckable, since its misfit is invisible to every residual-based test (post-fit RAIM, sigma0, robust weighting) because Baarda's minimal detectable bias scales as @f$1/\sqrt{r_i}@f$.  Always counted; evented only in combination with @ref redundancy_hot_sigma.
        double redundancy_hot_sigma{ 1000.0 };  ///< Magnitude half of the cross-flag: the centred standardized prefit deviation above which an uncheckable row becomes a @c uncheckable_hot_cell event.  The measured parked fault cell sat at 2.6e5 and the storm-time level tops out near 40, so the default leaves a margin of a factor of about 260 and about 25 respectively.  An uncheckable row below this (for example a fault-free single-frequency rising satellite) is counted and never evented, per the prefer-fallback directive.
        double redundancy_pivot_eps{ 1e-8 };    ///< Unobservable-local floor (INC-2), relative to the station's median @f$N_{rr}@f$ LDLT pivot (the absolute scale is set by the measurement weights and varies by decades between an ambiguity column and a ZWD column).  A column below it carries essentially no information this epoch.
        double redundancy_escalate_sigma{ 0.0 };///< Free-tier escalation trigger (0 = off).  When > 0, the O(rows) centred prefit census runs on every station every epoch, and any station whose largest centred deviation exceeds this gets a targeted single-station redundancy sweep at the next epoch (~2-5 ms).  The free tier detects that something is large; the redundancy sweep tells whether it is checkable.  Works with @ref redundancy_census_stride = 0.
        int    redundancy_event_holdoff_ep{ 20 };///< Per-(station, satellite, class) event rate limit in sweep passes: a parked cell keeps logging its identity, but not 120 times per hour.
        // --- Baarda reliability numbers / MDB (docs/SPEC_redundancy_census.md;
        //     filter/NetworkMdb) ----------------------------------------------
        // Measure-only, default-off and not fingerprinted, like the two blocks
        // above.  A port of the quartet in KalmanFilter::computeResidualQc_, not a
        // new mechanism: wherever @f$r_i@f$ already exists, one division gives
        // @f$\mathrm{MDB}_i = \sigma_i\delta_0/\sqrt{r_i}@f$, the size in metres of a
        // fault that could hide in that row at this epoch.  It makes the census's
        // uncheckable flag quantitative and the RAIM thresholds interpretable; no
        // decision is wired to it.
        bool   mdb{ false };                    ///< Master switch.  Requires an @f$r_i@f$ source: @ref redundancy_census_stride / @ref redundancy_escalate_sigma, or network.raim_redundancy_normalize on a live network.raim.  The loader fails with an error when neither is armed: without a redundancy source there is nothing to divide by, and the run would report zero rows, indistinguishable from a genuine negative result.
        double mdb_event_m{ 1.0 };              ///< Reporting threshold in metres (0 = no events): a finite MDB above it emits an @c MDB / @c mdb_exceedance event.  Not a gate: no deweight, admission or withhold reads it.  Per (station x row class) the worst finite row is the one named; the full above-threshold population is in the trace's @c n_over column.
        int    mdb_event_holdoff_ep{ 20 };      ///< Per-(station, satellite, class) MDB event rate limit, in flush passes (the PREFIT/redundancy holdoff convention).  MDB moves slowly (it is geometry), so an unthrottled stream would repeat the same cell every epoch for hours.
        // --- Gross line-of-sight screen, card GL (docs/SPEC_event_harness.md §24.18;
        //     filter/NetworkGrossLos) ----------------------------------------
        // The one acting mechanism among these census blocks; everything else here is
        // measure-only.  Default off, byte-identical when off, not fingerprinted.
        //
        // Why it exists: every post-fit tier the producer owns pools carried-ambiguity
        // phase rows (the pool in NetworkRaim.cpp admits a row by
        // `|A_r(row, amb_col)| > 0.5`, which is +1 on a phase row and 0 on a code row),
        // so a code-only gross error on one line of sight is structurally invisible to
        // RAIM's consensus, to its median-polish attribution and to its σ̂₀.  MDB sees
        // the row, but its statistic is a reliability number (δ₀σ/√r; it never reads
        // the residual) and its action column is the literal `"observe"`
        // (NetworkMdb.cpp:286).  Measured consequence: a line of sight 75 to 142 m off
        // on three code signals lived 31 epochs with `network_raim.csv` filing no rows,
        // and over 6 h such lines accumulate until a whole station's solution is
        // corrupted (code rms 54 m, ZWD 197 m).
        //
        // What it is: a per-(station, satellite) LOS state machine over the post-fit
        // code residuals, standardized by each row's own σ.  The pooled σ̂₀ is
        // deliberately not the denominator: on a corrupted hour the pool inflates and
        // would disarm the screen exactly when it is needed.  Both edges are events
        // with counters (standing invariants #4 and #7).
        bool   gross_los_screen{ false };       ///< Master arm.  Off: the seam returns on entry (the driver calls it every epoch), no state is allocated, no row weight moves and the assembly's code-deweight map is never populated (byte-identical).  On: per (station, satellite), when at least @ref gross_los_min_rows code rows have |v|·√w above @ref gross_los_k for @ref gross_los_epochs consecutive epochs, the LOS is declared gross: its carried ambiguity levels are erased, its per-LOS iono carry is erased (so the LOS re-draws its slant iono from the SH tie), its ambiguities are marked pending-reseed, and its code rows fold at σ × @ref gross_los_deweight until the residual returns under @ref gross_los_clear_sigma for @ref gross_los_clear_epochs epochs.  The epoch always emits (adapt-and-emit): the screen never withholds an epoch, although the GL-2 wire withhold and the emission exclusion can drop single satellites from an emission.
        double gross_los_k{ 8.0 };              ///< Declaration bar in units of the row's own σ (the statistic is @f$|v_i|\sqrt{w_i}@f$ with @f$w_i = 1/\sigma_i^2@f$, so the bar means that the code observation missed by k of its own sigmas).  8.0 is §24.18's pre-registered value: the motivating gross rows ran at ~150σ and the quiet code population's q99 is well under 8, so the bar sits between them with about a decade of margin on each side.  Report the |v|/σ distribution over all LOS before tuning this (§24.18's third frozen reading).
        int    gross_los_min_rows{ 2 };         ///< Minimum number of the LOS's own code rows that must clear @ref gross_los_k in the same epoch.  §24.18's "≥ 2 signals": a real gross line of sight is wrong on every code band at once (its error is a range error, not a dispersive one), while a single heavy-tailed row is one band's multipath.  Counted as rows rather than distinct signal labels on purpose: a code row of a posture whose satellite code biases are external, or of the W6 datum signal, carries no @c CB: column and therefore has an empty signal label, so a distinct-label test would under-count exactly there.  The distinct labels that were over the bar still go into the event's detail.
        int    gross_los_epochs{ 2 };           ///< Consecutive epochs of evidence before the declaration.  1 = act immediately.  Absence resets the streak (an unobserved epoch is not evidence either way), the same "rebuild the streak map from this epoch's flagged set" rule the RAIM isolated-arc persistence gate uses.
        double gross_los_clear_sigma{ 3.0 };    ///< Re-coupling bar, same units as @ref gross_los_k: the LOS's worst code row must sit under this to count toward clearing.  Deliberately far below @ref gross_los_k: the gap is hysteresis, so a line of sight hovering at the declaration bar cannot flap between the two edges every epoch.
        int    gross_los_clear_epochs{ 2 };     ///< Consecutive epochs under @ref gross_los_clear_sigma before the LOS is released.  The re-coupling edge of standing invariant #7: the response is lifted only after the data have re-validated the line of sight, and the lift is itself an event.
        double gross_los_deweight{ 100.0 };     ///< σ multiplier applied to a gross LOS's code rows while it is declared: the row folds at @f$w/f^2@f$ (σ × f), so the default 100 leaves the row contributing @f$10^{-4}@f$ of its nominal information.  A cap, not a drop (the prefer-fallback directive): the row keeps its sign and still re-anchors the float if everything else about the LOS goes away.  Phase rows are untouched (the phase side has its own responses: the erase and pending-reseed above).
        double gross_los_max_gap_s{ 900.0 };    ///< The absent-satellite edge of standing invariant #1.  A declared LOS not seen for longer than this expires (its own event) and its deweight lifts, because the next sighting is a new arc with a new ambiguity and the old evidence does not apply to it.  Without this, a deweight set once would carry over to a fresh arc unnoticed, the failure that the re-coupling edge of standing invariant #7 names.
        int    gross_los_warmup_epochs{ 120 }; ///< Maturity horizon: no declaration before this many emitted epochs (0 = act from epoch 1).  Measured, not chosen: without it, a 20-epoch smoke declared 39.5 % of LOS-epochs gross, the ×100 code deweight removed the information the cold solve needs to determine the receiver clocks, and the SH health gate then withheld 9 of 20 epochs.  A cold post-fit code residual reflects the unconverged station block, not a fault; every other acting tier here carries the same guard for the same reason (@c network.raim_maturity_gate / @c networkWarmupRampLifted, which suppress isolated arcs until the filter has settled, since before that they drive a self-sustaining warm-up churn).  120 epochs = 1 h at 30 s, the horizon over which the measured over-bar fraction settles.  The screen still screens during warm-up (its counters and its |v|/σ census are live); it only does not declare, so the run reports what it would have done.
        double gross_los_max_station_frac{ 0.5 }; ///< Whole-station stand-down: if more than this fraction of a station's screened lines of sight clear the bar in one epoch, the screen makes no new declaration at that station this epoch and reports this in its own event.  A gross line of sight is isolated by definition: when most of a station's lines of sight are over the bar, the fault is the station's (or with the estimator's own state for it, such as a 197 m ZWD, rather than ten bad measurements), and deweighting every code row of that station removes exactly the information that determines its clock and troposphere.  This screen cannot repair a station, so it stands down.  1.0 disables the guard.
        // --- Whole-satellite stand-down (GL-2, §24.19) ------------------------
        bool   gross_los_sat_standdown{ false }; ///< Master switch of GL-2.  Off: no satellite tally is built, no satellite state is allocated, and the run is byte-identical.  On: when the gross-LOS rule puts the same satellite over the bar at ≥ @ref gross_los_sat_min_stations stations in one epoch, that is a satellite fault, not N line-of-sight faults.  The per-LOS declarations for it are refused (and any standing one released), and the satellite is routed to the two responses the producer already has for a cross-station consensus fault: the per-satellite retro disc bump (`ura_retro_sats` → `NetworkSolPrep` → `markSatDiscontinuity`) and, under @ref gross_los_sat_wire_withhold, the per-satellite wire withhold (pruned from `sol.sats` as the RT0-5 runaway quarantine and the RTA-10 carve-out prune theirs).  Measured motivation: in one epoch the per-LOS screen declared ten lines of sight, all on one satellite at ten stations on four continents, and deweighting them all degraded the held-out consumer to 56 m for an hour.
        int    gross_los_sat_min_stations{ 3 }; ///< Number of stations that must have the same satellite over the bar in one epoch for the satellite stand-down to fire.  Deliberately the same number and meaning as @c network.raim_min_stations ("a satellite so flagged at >= 3 stations is refloated network-wide"): this is that rule's code-side counterpart, and the two must agree on what counts as a satellite fault.  The trigger is the over-bar count, not the declared count, so the satellite is caught on its first bad epoch rather than after the per-LOS persistence gate has spent two.
        bool   gross_los_sat_wire_withhold{ true }; ///< Whether a stood-down satellite is also pruned from the epoch's emission (a per-satellite wire withhold, never an epoch withhold: every other satellite still emits, and the epoch is still adapt-and-emit).  The consumer is informed by the retro disc bump, which lands with mark-==-land semantics at the satellite's next actual emission.  False arms the disc bump alone: the A/B that separates "the consumer needed the satellite dropped" from "the consumer needed to be told".
        int    gross_los_event_holdoff_ep{ 0 }; ///< Per-(station, satellite) rate limit on GROSSLOS events, in screened epochs.  0 = no limit, the right default here: unlike MDB/PREFIT this stream is edge-triggered (one declare and one clear per episode), so it limits itself, and every row of it is an identity the grade needs.
        // --- Completing the screen (RF-3b; docs/refound/RF-3_code_row_qc_2026-09-22.md
        //     §4, requirements F1 to F4 of runs/_msc/rf3_read_report_2026-09-22.md).
        //     Each of the four defaults to the original behaviour.
        /// How @ref gross_los_min_rows is read (RF-3b B1, F1).  @c Fixed (the original
        /// behaviour): at least that many of the LOS's code rows must clear the bar
        /// together, which a line of sight with fewer rows than the constant can never
        /// meet.  The RF-3 read found this to be why four of the six largest episodes
        /// were unreachable: those lines of sight had one code row, their stations having
        /// lost every other signal.  @c Available reads the bar as
        /// @c min(gross_los_min_rows, the LOS's code rows this epoch), so a one-row line
        /// of sight is reachable while a multi-row one keeps the multi-band
        /// corroboration the rationale above relies on.  A flat @c gross_los_min_rows = 1
        /// also reaches family S (gross rows at full weight 101 -> 33) but removes that
        /// corroboration everywhere; the rule keeps both.
        NetworkGrossLosMinRowsRule gross_los_min_rows_rule{ NetworkGrossLosMinRowsRule::Fixed };
        /// The decoupling edge of standing invariant #7 (RF-3b B2, F2).  Off (the
        /// original behaviour): a declaration erases the line of sight's carried level
        /// and per-LOS iono, and the freed states then re-determine themselves from an
        /// epoch whose only code row is folded at σ × @ref gross_los_deweight, an
        /// under-determined fit, after which the folded row's residual is a random walk
        /// rather than a measure of the fault.  Measured: two declared rows grew from
        /// about 110 and 195 m to over 400 m, and because the release test ("the worst
        /// row under @ref gross_los_clear_sigma") cannot fire on a runaway, both ended by
        /// expiry at the 900 s horizon rather than by release.  On: the freed states are
        /// held.  The line of sight's carried ambiguity levels and its iono carry are
        /// frozen at their pre-declaration values and variances with zero process noise,
        /// their border coupling and joint block cleared (the decoupling half), and
        /// re-applied after every harvest for as long as the declaration stands, so the
        /// phase rows fold against a fixed prior and the folded code row's residual stays
        /// a measure of the fault.  Release then becomes the normal exit and expiry the
        /// exception.  Solve-affecting; requires @ref gross_los_screen.
        bool   gross_los_bound{ false };
        /// Same-epoch emission exclusion (RF-3b B3, F3).  Off (the original behaviour):
        /// the screen runs after the epoch's emission, so a declaration first reaches the
        /// wire one epoch later, and the two largest rows of every episode are emitted
        /// before any response lands.  On: the screen's decision half (assessment, state
        /// machine, both edges, the deweight publish) moves ahead of the emission, and
        /// the declared line of sight is excluded from its satellite's contributor set for
        /// that epoch.  If the satellite's remaining contributing stations fall below
        /// @ref gross_los_sat_min_stations, its records are withheld for the epoch (a
        /// per-satellite withhold, never an epoch withhold: every other satellite still
        /// emits); otherwise the satellite is emitted with the producer's existing
        /// per-satellite discontinuity mark, so the consumer is told the contributor set
        /// changed.  The emitted clock and code bias are joint-solve shared states copied
        /// straight out of @c x_s, so there is no per-contributor decomposition to
        /// subtract: changing the value would require a second network solve of the
        /// epoch, which this knob does not do.  The carry response still runs after the
        /// harvest, where it must.  Solve-affecting (through the moved decision's
        /// one-epoch shift of the maturity horizon and the earlier deweight publish);
        /// requires @ref gross_los_screen.
        bool   gross_los_emit_exclude{ false };
        /// Station-level tier of the whole-station guard (RF-3b B4, F4).  Off (the
        /// original behaviour): when the guard fires (@ref gross_los_max_station_frac) it
        /// only holds this epoch's declarations, and the next epoch declares the same
        /// lines of sight one by one on the same evidence, which turns a station fault
        /// into N line-of-sight faults.  On: the guard becomes a station-level verdict
        /// with its own two edges (@c station_declare / @c station_release), folding
        /// every one of the station's code rows at @ref gross_los_deweight and, under
        /// @ref gross_los_bound, holding its freed states as a per-LOS declaration does.
        /// Released when the station's over-bar fraction sits at or under the cap for
        /// @ref gross_los_clear_epochs consecutive screened epochs.  Solve-affecting;
        /// requires @ref gross_los_screen.
        bool   gross_los_station_tier{ false };
        /// Cold-start absolute bound, metres (card ICF response 1,
        /// docs/refound/ICF_ignition_code_fault_2026-10-09.md section 3).  0 (the default)
        /// = off.  Before the maturity horizon (@ref gross_los_warmup_epochs) the screen
        /// makes no declaration on its σ-standardized bar, because a cold post-fit code
        /// residual measures the unconverged station block.  A residual of tens of metres
        /// is not that: on the two 2 h goldens the largest post-fit code |v| of any line
        /// of sight in the first 120 epochs is 6.70 m (field) and 4.88 m (sim), while the
        /// faulty L1 channel of BSHM00ISR G07 reaches 24.2 m at its first visible epoch
        /// and grows 40 to 50 m per epoch after it.  With a positive value, inside the
        /// maturity horizon a line of sight whose largest post-fit code |v| this epoch
        /// exceeds the bound is declared at once (one epoch, no persistence), with the
        /// screen's own response: its carried ambiguities are erased and marked
        /// pending-reseed, its per-LOS iono and phase-colour carries are erased, and its
        /// code rows fold at σ × @ref gross_los_deweight.  While that declaration stands,
        /// every epoch on which the bound is exceeded again re-applies the erase, so a
        /// re-stashed ambiguity cannot make the line of sight redundant again (with the
        /// ambiguity free, the phase rows carry nothing to the shared states).  The
        /// release is the screen's own test (@ref gross_los_clear_sigma for
        /// @ref gross_los_clear_epochs), and it erases the carry once more, so the line
        /// of sight reconnects on a fresh ambiguity rather than one that absorbed the
        /// fault.  A station with more than @ref gross_los_max_station_frac of its lines
        /// of sight over the bound declares none of them (a station fault).  Requires
        /// @ref gross_los_screen; inert under @ref gross_los_bound (the hold would keep
        /// the faulted level the response erases).  Solve-affecting; not fingerprinted,
        /// like the screen's other knobs (it changes which rows are trusted, not what a
        /// carried state means).
        double gross_los_cold_bound_m{ 0.0 };
        /// Test-only (card ARL, the integer-ambiguity ladder): when > 0, PASS B appends
        /// one row per slant-ionosphere column tying it to absolute zero with this sigma
        /// (metres), so the estimator matches a fixture simulated without ionosphere
        /// (gnss_simulator: iono.background_source analytic, quiet_vtec_tecu 0,
        /// storm_scale 0).  Never meaningful on real data.  0 (the default) = off,
        /// byte-identical.  Solve-affecting; not fingerprinted.
        double sim_zero_iono_sigma_m{ 0.0 };
        // --- Datum graph census (docs/SPEC_datum_graph_census.md;
        //     filter/NetworkDatumGraph) ----------------------------------------
        // Measure-only, default-off and fingerprint-safe, like the blocks around it.
        // The S-basis dimension is detected spectrally (the null-space gate counts
        // near-zero eigenvalues; SBC derives the epoch's extras from the same
        // normals) but never explained.  LASER 5-62/5-63 gives the structural form
        // (rank deficiency = d.k over k disjoint components; "knowing the type of
        // observations involved could indicate the rank of A"), and the measured
        // GPS+Galileo extras were exactly a support-structure split (per-satellite
        // channel-family PB<->iono trades on the Galileo C/Q-versus-X station
        // partition).  This census predicts from topology and reconciles against the
        // spectrum; nothing consumes its output.
        bool   datum_graph_census{ false };     ///< Master arm.  Off: the seam is never entered, no file is opened, no counter moves.  On: per epoch, union-find over the admitted (station, satellite) support graph per constellation (tier 1) plus the (satellite x channel-family) code-support partition (tier 2), joined to the epoch's spectral observation into the predicted/observed contingency.  Never feeds the gate, SBC or @c Plan::declared_dimension; acting on a prediction is left to a successor card.
        int    datum_graph_event_holdoff_ep{ 20 };///< Per-identity DATUMGRAPH event rate limit, in processed epochs (the PREFIT/MDB/block-Baarda convention).  Datum topology moves slowly, so an unthrottled stream would repeat the same (sat, family) every epoch for hours; the CSV rows are never rate-limited, only the individual identities.
        // --- Prior-residual census (docs/SPEC_prior_residual_census.md;
        //     filter/NetworkPriorResidual) -------------------------------------
        // Measure-only, default-off and not fingerprinted, like the two blocks above.
        // The response layer of standing invariant #1 is per mechanism (MW / LLI /
        // IF-TD on the observables, D5 on the DD chords, jump tests on the code
        // bias, eq34_reref on the pin displacement), and nothing identifies which of
        // the remaining stiff priors carries a stale value.  This census applies D5's
        // own statistic (LASER 5-74's residual of prior information and its
        // dispersion, algebraically the partial redundancy that
        // NetworkCarryVce::addRow already forms) to the carry families that have no
        // screen at all, and makes them comparable on one axis.
        //
        // It adds no gate, deweight, exclusion or refloat: the solve is
        // byte-identical with it armed, which is the acceptance criterion.  The
        // response question is a separate card whose design input is this census's
        // per-family exceedance ladders.
        bool   prior_residual_census{ false };   ///< Master arm.  Off: the accumulator is inert, the seam is never entered, and no column, row or event exists.
        std::string prior_residual_families{ "pl,pb,cb" }; ///< Comma-separated carry families to census (parsed by @c NetworkCarryVce::parseComponents).  Known: @c pl (phase-leveling ambiguity carry, the origin of standing invariant #1 and the largest population by two orders of magnitude), @c pb / @c cb (satellite phase- / code-bias carry; @c cb carried the DOY-129 pivot code-bias ratchet), @c zwd (B9 ZWD carry) and @c rxcb (receiver code-bias carry).  A name with no hook in this build produces a warning at load.
        int    prior_residual_window_epochs{ 60 };///< sigma0 exponential-forgetting half-life in epochs; mirrors @c carry_vce_window_epochs.  <= 0 ⇒ whole-run accumulation.
        int    prior_residual_min_rows{ 200 };   ///< sigma0 warm-up in effective rows (D5 v2's value).  Below it the family reports the raw formal t == s and marks this in the output column, rather than standardizing by a seed without notice.
        double prior_residual_report_sigma{ 4.0 };///< Per-row trace/event threshold in sigma0 units.  A reporting threshold, not a gate: nothing in the solve reads it back.  The ladder, not this knob, is the deliverable.
        int    prior_residual_event_holdoff_ep{ 20 };///< Per-(family, station, satellite, signal) PRIORRES event rate limit in census epochs (rate-limited like the PREFIT events), so a persistently stale prior keeps logging its identity, but not 120 times per hour.  The ladder is never rate-limited; only the individual identities are.
        bool   prior_residual_corroborate{ false };///< Join every row to the raw-observable verdict the slip-QC pass already computes and emit the per-family contingency table (INC-3).  Auto-arms @c network.cycle_slip.gap_instrument (instrumentation only, verified byte-identical) with a warning; deliberately not @c mw_lli_corroborate, which is a decision card and would perturb what is being measured.  Requires @ref prior_residual_census (hard error otherwise).
        // --- BlockFormal: (station, satellite) LOS block outlier census, riding on
        //     the redundancy census's swept stations -----------------------------
        // The middle tier.  A per-row w-test sits below the unit a physical fault
        // occupies and station-level median polish sits above it, so one fault on one
        // line of sight, spread by the adjustment across that LOS's 2-8 code and
        // phase rows, can be sub-threshold on every row while the block is jointly
        // large.  LASER 5-98: "not necessarily a one-to-one correspondence between
        // large residuals and outliers".  Measure-only: the solve is byte-identical
        // with this armed, and no response reads any of it.
        bool   block_baarda{ false };            ///< Master arm.  Off: the accumulator is inert, the sweep is never entered, and no column, row or event exists.  Requires a live @ref redundancy_census_stride / @ref redundancy_escalate_sigma: that sweep is the host whose stride, targeted escalation and trip schedule this census uses instead of its own scheduler.  The loader fails with an error when neither is armed: with no host there would be nothing to sweep, and a clean zero-block run would be indistinguishable from a genuine negative result.
        std::string block_baarda_families{ "G,E,R,C" }; ///< Comma-separated constellation letters to census.  The prior-residual census's families are carry families and do not apply to a line of sight; the constellation is the only partition that can label a whole block, because a block spans code and phase rows by construction.  A letter with no satellites in this run reports zero blocks and states so at run end.
        int    block_baarda_window_epochs{ 60 }; ///< sigma0 exponential-forgetting half-life in epochs; mirrors @ref prior_residual_window_epochs.  <= 0 ⇒ whole-run accumulation.
        int    block_baarda_min_blocks{ 200 };   ///< sigma0 warm-up in effective blocks (D5 v2's value, in this census's unit).  Below it the family reports the raw formal t_B == T_B and marks this in the output column, rather than standardizing by a seed without notice.
        double block_baarda_report_sigma{ 4.0 }; ///< Trace/event threshold on @f$t_B@f$.  Reporting only: nothing in the solve reads it back.  Note the unit: @f$t_B@f$ carries the @f$1/b@f$, so it is a variance ratio with @f$E=1@f$ under @f$H_0@f$ for every block size; 4.0 means four times the variance the model allows, not four sigma.  The ladder, not this knob, is the deliverable (SPEC 2.1: empirical critical values, never tabular).
        int    block_baarda_event_holdoff_ep{ 20 };///< Per-(station, satellite) BLOCKBAARDA event rate limit in census epochs, so a persistently large LOS keeps logging its identity, but not 120 times per hour.  The ladder is never rate-limited; only the individual identities are.
        bool   block_baarda_corroborate{ false };///< Join every reported block to the raw-observable verdict the slip-QC pass already computes, and emit the per-family contingency table whose b-cell (large and raw-flat) is the number SPEC 8.3 grades (INC-2).  Auto-arms @c network.cycle_slip.gap_instrument with a warning, as @ref prior_residual_corroborate does.  Requires @ref block_baarda (hard error otherwise).
        // --- Station congruence screen: coordinate-fault detection in position
        //     space, a second rider on the redundancy census's swept stations ----
        // The tier above the block census, and the one fault class no other armed
        // tier can see.  Station coordinates are held; a wrong held coordinate
        // (survey blunder, ARP/eccentricity mistake, monument motion) projects
        // coherently into every row of that station.  Median polish removes exactly
        // that common effect, per-row w-tests see only what remains after the
        // absorption, and the (station, satellite) block test pools one line of
        // sight.  A 6.10 cm ARP error at one station is the measured precedent; it
        // was detected by manual inspection, not by QC.  Caspary (2000) congruence
        // combined with the LASER guide's Baarda material (5-97..5-101);
        // multi-dimensional outlier form per Snow & Schaffrin (2003).
        // Measure-only: the solve is byte-identical with
        // this armed, and nothing moves or reweights a coordinate.  The per-station,
        // per-direction coordinate MDB is the census's service deliverable; on a
        // carry_invariant lineage the true Up answer may be "large", and the SPEC
        // forbids tuning that away.
        bool   congruence_screen{ false };       ///< Master arm.  Off: the accumulator is inert, StationAssembly::row_pos_enu is never populated, the sweep is never entered, and no column, row or event exists.  Requires a live @ref redundancy_census_stride / @ref redundancy_escalate_sigma: that sweep is the host whose stride, targeted escalation and trip schedule this screen uses instead of its own scheduler.  The loader fails with an error when neither is armed: a clean zero-sweep run would be indistinguishable from a genuine negative.
        double congruence_report_sigma{ 4.0 };   ///< Trace/event threshold on @f$t_i@f$.  Reporting only: nothing in the solve reads it back.  Note the unit: @f$T_i@f$ carries the @f$1/q@f$, so it is a variance ratio with @f$E=1@f$ under @f$H_0@f$ for every observable dimension; 4.0 means four times the coordinate tension the model allows, not four sigma.  The per-station ladder and the MDB table, not this knob, are the deliverable.
        int    congruence_event_holdoff_ep{ 20 };///< Per-station CONGRUENCE event rate limit in census epochs, so a persistently offset station keeps logging its identity, but not 120 times per hour.  The per-station trace row is never rate-limited; only the individual events are.
        // --- Constraint-aware global variance component, a rider on the census
        //     accumulator above ------------------------------------------------
        // The network's within-epoch sigma0 is a robust median of standardized
        // data-row residuals.  That is the right statistic for RAIM attribution and
        // it stays untouched, but it cannot see tension absorbed by the carried
        // priors, the largest constraint population in the solve by orders of
        // magnitude (a parked cell absorbed through the free ambiguities leaves the
        // post-fit sigma0 clean).  LASER 5-74's global form puts the prior residuals
        // in the numerator and the constraints in the redundancy; slide 5-85 shows
        // why only the non-minimal stochastic rows need the correction (a datum pin's
        // contribution vanishes at both ends).  Two sums per side and one division,
        // and nothing consumes the result: the A4 raim_withhold_sigma0 guard, every
        // gate and every deweight stay on the median sigma0.
        bool   prior_residual_global_sigma0{ false };///< Master arm for the rider.  Requires @ref prior_residual_census (hard error otherwise).  Warns without @c network.raim_redundancy_normalize, whose per-row Baarda r_i is the only valid data-side redundancy; the r == 1 fallback is then taken and flagged in-band per epoch.  Measure-only: no gate, deweight or withhold reads it; the solve is byte-identical.
        // --- Product-referenced measurement admission
        //     (docs/SPEC_measurement_admission.md; filter/NetworkMeasurementAdmission)
        // The corrupt-channel screen: the first layer of a layered defense (this
        // screen -> iono_dw_admit_validate_k -> prefit_fuse_sigma -> the SH health
        // gate).  A receiver can emit a channel offset by ~1e5 m against SP3+CLK
        // (measured: two satellites at one station, +198.2 km and +99.9 km).  Because
        // the offset rides both the code and the phase of the channel, it is invisible
        // to every differential check (TD, phase-minus-code, GF and MW all pass by
        // construction), and on a single-band line of sight the free slant state
        // absorbs it exactly, beyond the reach of any residual test.  Only an
        // absolute, product-referenced test can see this class.
        //
        // Like the two blocks above, and unlike an estimation knob: default-off,
        // structurally byte-identical when off, and not fingerprinted.  It is a
        // forward-only admission policy (RAIM-class), so a stored snapshot's state
        // means the same thing armed or not, and an off/on pair on one shared
        // snapshot stays a valid A/B.  @ref adm_screen is solve-affecting when armed;
        // @ref adm_trace alone is measure-only.
        bool   adm_screen{ false };      ///< Reject a (station, satellite, band) code channel whose common-mode-removed |z| exceeds @ref adm_bound_m: at arc birth (never seeded), at a warm resume's first restored epoch (a restored corrupt arc is not a birth), and mid-arc (withhold and refloat) (INC-1/INC-2).  Solve-affecting.  The statistic is `observed - modeled` on code only, centred on the station median; `modeled` is the state-free deterministic model (range - c*dt^s - c*rel + ZHD*mf + antenna + bias + tides), so unlike a prefit innovation it does not vanish at a cell's birth, which is why the prefit census sees this class only after the fact.  Phase is deliberately not screened: a constant phase offset is indistinguishable from an ambiguity and is therefore gauge, not corruption.  Implies @ref adm_trace's columns.
        double adm_bound_m{ 1000.0 };    ///< |z| rejection bound (m).  Deliberately at least 10× above the worst legitimate |z| (low-elevation storm slant, tropo residual, multipath and ephemeris error, ~<= 100 m; eleven fault-free satellites at one station on the storm day measured -12.5…+19.1 m) and at least 100× below the observed corrupt class (1e5 m).  A bound sweep is a validation gate, not a tuning exercise.
        int    adm_min_sats{ 4 };        ///< Median-validity floor: the station's centre is trusted only when at least this many distinct satellites carry a finite term.  Below it the station-epoch is skipped and counted: a median of two or three values can be the very outlier it is meant to remove, and a screen that rejects against an untrusted centre is worse than no screen.
        int    adm_heal_ep{ 5 };         ///< Consecutive in-bound epochs a rejected channel must show before it is readmitted.  A rejected band is re-tested every epoch; readmission goes through the normal funnel with a fresh seed and never restores the old states.
        double adm_max_frac{ 0.5 };      ///< Station guard: if more than this fraction of a station's screened channels fail in one epoch, the median itself is untrusted (bad a-priori coordinates, a product outage, a receiver-wide fault) and no per-channel verdict derived from it is valid.  The response follows the IF-TD reference gate's incoherent-station precedent: act at the station's scope (hold its screened bands for the epoch, refloat the arcs behind the holes) and refuse every per-channel claim, leaving the admission records untouched so the next epoch re-tests from the evidence.
        bool   adm_trace{ false };       ///< Absolute-residual census (INC-3): two per-epoch |z| columns in @c network_epoch_status.csv (the median over stations of each station's worst, and the network maximum), plus ADMISSION-tier events naming every over-bound channel, rate-limited per cell.  Measure-only and armable without @ref adm_screen: it is the scan that detects a corrupt channel which differential checks of the raw RINEX pass as clean.
        // --- Arc re-entry vet (INC-4) ---
        //
        // The admission record is keyed per (station, satellite, band) and kept for a
        // full day, so a channel that leaves the model set and returns hours later
        // still carries `admitted = true` from before the gap.  Without the vet, the
        // seam sends it down the established-arc path (INC-2 onset) instead of the
        // birth path, on a verdict inherited from a channel that is not necessarily
        // the one that returned.
        //
        // Measured on a 24 h, 63-station run: 16 of the 17 mid-arc `adm_onset` events
        // were preceded by a `T0_gap arc_resume` on the same cell within 5 minutes.
        // There were no genuine mid-arc onsets that day, only unscreened re-entries,
        // in two sub-classes:
        //
        //  * Twelve fired at the same epoch as the resume, after gaps of 3-2025
        //    epochs.  These were mislabelled: the response was already correct, but
        //    the label depended on how long the run had been going (a leg replay whose
        //    record map starts empty correctly calls the same channel
        //    `adm_reject_birth`).
        //  * Four were re-admitted and crossed the bound 2-8 epochs later, and those
        //    epochs were costly: a00 moved by 12 to 20 TECU inside the exposure window
        //    and recovered only once the withhold landed.  The birth rejections of the
        //    same day moved a00 by a median of -0.11 TECU.  Refusing at admission
        //    costs nothing; amputating an established arc does (standing invariant #7,
        //    both edges).
        //
        // The vet therefore stops inheriting the verdict: a return after a gap is
        // re-screened as a birth.  That alone handles the twelve at no cost (a clean
        // re-entry passes the bound and is admitted with no withhold, no funnel
        // insertion and no solve change).  The four need the second half: a vet
        // window in which the prefit census, which named each of them 1-4 epochs
        // before the bound did, is admitted as evidence.
        bool   adm_revet{ false };       ///< Re-screen an arc re-entry as a birth rather than inheriting the pre-gap `admitted` verdict, and vet it for @ref adm_revet_ep epochs afterwards (INC-4).  Requires @ref adm_screen (the vet is a verdict, and the census-only arm makes no verdicts).  Solve-affecting when it changes an admission, but inert on a clean re-entry by construction: a returning channel inside the bound is admitted on its return epoch as it would be without the vet, with no withhold and no refloat.
        int    adm_revet_gap_ep{ 1 };    ///< Screened epochs a channel must be absent before its return counts as a re-entry (1 = any missing epoch).  Absence is counted in epochs in which this seam screened the channel, so a station below @ref adm_min_sats or erased by the two-station observability gate reads as absent, the conservative direction (it re-screens).  Measured over a full day: 746 of 4657 `arc_resume` rows have gap 1, and the exposed sub-class spans gaps of 1-9, so a threshold above 1 would miss part of it.
        int    adm_revet_ep{ 8 };        ///< Screened epochs the vet window stays open after a re-entry (coverage runs from the return epoch through return + this - 1).  Sized from field data: the census cannot flag the return epoch itself (a re-entering arc's ambiguity is fresh, so the census defers it as a birth) and its flags are read one epoch late, so the first readable flag is at return + 2; the four exposed cells of the measured day were flagged at return + 1, +3, +4 and +1, i.e. readable at return + 2, +4, +5 and +2.  8 covers every measured case with two epochs to spare (the window closes after return + 7), and a window that is too long costs nothing on a clean channel (the census never flags it); the asymmetry is deliberate.  Re-armed to full by any refusal inside the window, and opened afresh by a readmission, which is a re-entry in effect.
        double adm_revet_bound_scale{ 0.2 }; ///< Fraction of @ref adm_bound_m that applies to a channel while it is re-entering or being vetted (1.0 = no tightening).  0.2 = 200 m at the default bound.  Measured, not chosen (card §10.10): over two legs (272 epochs, 63 stations, 49 `arc_resume` cells) with the census bound lowered to 50 m, no fault-free station's re-entry cell exceeded 50 m; the whole > 50 m re-entry population came from one faulty station, arriving at 670.3, 347.6 and 81.5 m on the return epoch.  The fault-free tail (tropical low-elevation satellites at a few stations) tops out at 61.9 m and is entirely steady-state.  200 m therefore sits 3.2× above the worst legitimate |z| in the sample and refuses the two large cells on arrival, which prevents the a00 excursion (+19.2 TECU) that the census trigger structurally cannot reach.  It deliberately does not reach the 81.5 m cell: 1.3× the fault-free maximum is not separable by any absolute bound, so that cell is left to the census trigger (@ref adm_revet_census).  The two mechanisms cover disjoint failure shapes (fast ramp and large on arrival, against slow ramp and small on arrival), and neither subsumes the other.  Not applied to the INC-3 census, whose domain is @ref adm_bound_m by definition.
        bool   adm_revet_census{ true }; ///< Inside a vet window, a PREFIT-census `obs_code` exceedance on the line of sight (@c network.trace_prefit_census, previous epoch) is also grounds for refusal: the census serves as the detection net named in §1 of the card, wired to the response.  The census statistic is standardized (@f$|y-m|\sqrt{w}@f$), so it sees a ramping corruption while it is still under the absolute metre bound (in the measured case, one epoch before the bound fired).  Ignored, and counted, when the census is not armed or its flags are stale; the vet then runs on the bound alone rather than becoming inert without notice.
        bool   adm_revet_hold{ false };  ///< Strict posture: withhold a re-entering channel until it re-earns admission through the normal readmission path (@ref adm_heal_ep) instead of admitting it provisionally for the vet window.  Default off, per the prefer-fallback directive and the flicker card's INC-3 deviation 9: acting on every re-entry would add ~1.6 funnel insertions per epoch (4657 re-entries over 2880 epochs in the measured day) to a channel on which the Iono-2 v3k erase policy is tuned.  Arm it only to bound a run where the census trigger is unavailable.
        int  neq_export_epoch{ -1 };      ///< One-shot full-arrowhead NEQ export: at the Nth processed epoch (1-based; <= 0 = off, the default), dump the assembled normal-equations system to @c logs/neq_export/: border @f$N_{ss}@f$ (raw and reduced) and RHS, and every station's @f$N_{sr}@f$ / @f$N_{rr}@f$ (rebuilt from its LDLᵀ) / @f$u_r@f$ as row-major little-endian double .bin files with a CSV station index, plus shared-column labels (@ref NetworkEstimator::sharedColumnLabel) and per-station local-column labels (StateManager::getStateLabels via the assembly h2local map).  Enables exact offline spectral / weak-direction attribution of the full system (the @ref trace_estimability dump covers only the reduced border's near-zero tail).  Post-solve read-only dump: the solution is byte-identical; ~100-200 MB once at 305 stations.
        bool trace_epoch_status{ false }; ///< Emit @c network_epoch_status.csv (L1): one row per processed epoch (emitted or skipped) with the disposition (emitted / gate_withheld / sigma0_withheld / solve_failed / datum_failed / sync_skip), σ̂0, within-epoch adapt count, refloat/retro counts, datum-change flag, and the clock-Δ median, std and worst satellite (the median+std+worst convention, unlike the rms/max of @c network_trace.csv).  Makes per-epoch dispositions and σ̂0 level shifts checkable by script (V1/V3/V6).  O(epochs); off by default, and byte-identical when off (no new file).
        /**
         * @brief Event response for the satellite code-bias carry
         *        (@c network.cb_excursion_reinit) (SPEC_day129_frame_guards phase 2.1).
         *
         * Standing invariant #1 for the CB carry.  @c sat_code_bias_q = 1e-10 m²/s
         * makes the satellite code biases near-static (steady-state Kalman gain
         * K ≈ 2.2e-3/epoch, bleed time constant τ ≈ 3.8 h), and the clock datum pins
         * one satellite's clock to zero.  A network-wide one-sided clock excursion
         * therefore cannot be absorbed by the pivot's soft clock the way every other
         * satellite absorbs it; it ratchets into the pivot's near-static CB states and
         * bleeds out over hours.  On the reference day this left a +49 m error on one
         * satellite's IF(C1W,C2W) that persisted to the end of the day, with no emitted
         * discontinuity.  The C3c/D5 machinery guards the ambiguity carry; this knob
         * guards the code-bias carry.
         *
         * When armed, a per-epoch network-wide common-mode detector (the median over
         * satellites of |Δclock| against @ref cb_excursion_median_m) marks the
         * clock-pivot satellite's CB carry for reinitialization (the existing
         * @c CarryPrior::inflate_q path, folded next epoch at the loose
         * @c network.clock_monitor_reinit_sigma variance) instead of the stiff
         * near-static carry, and queues that satellite for a code-bias discontinuity
         * bump at the next emit (card 2 rule: a value step must coincide with a
         * signalled discontinuity).  Reset rather than repair; no measurement is ever
         * dropped.
         *
         * Default off: the detector never runs, nothing is marked, nothing is bumped
         * (byte-identical).  Solve-determining when armed, so it is conditionally
         * appended to the warm-resume fingerprint: every existing (unarmed) snapshot
         * fingerprint is unchanged, and a resume may not cross a flip of this toggle.
         */
        bool cb_excursion_reinit{ false };
        /**
         * @brief Trip threshold on the per-epoch median |Δclock| over satellites (m).
         *
         * Derived empirically from the DOY-129 reference run per the MW-thresholds
         * directive (no stochastic-model σ).  Measured distribution of the statistic
         * over the 2159 inter-epoch deltas of the day:
         *
         *   median 0.123 m · 1.4826·MAD 0.071 m · q0.99 13.0 m · q0.999 611 m
         *
         * and at the three known events:
         *
         *   07:32 (tow 286320)  median    5.03 m, max  529 m  (max/med 105, two-sided,
         *                                                       no persistent damage)
         *   19:21 (tow 328920)  median   77.4  m, max  334 m  (max/med 4.3)
         *   20:47 (tow 334020)  median 4581.9  m, max 5624 m  (max/med 1.23, one-sided,
         *                                                       the +49 m persistent fault)
         *
         * The default 50.0 m sits ~400× the quiet median (~700 robust σ) and a factor of
         * about 90 below the damaging event, so the choice is not delicate: it trips on
         * 10 of 2159 epochs (0.46 %).  The median is the discriminator: it ignores the
         * two-sided 07:32 excursion (5.03 m, no trip) and is saturated by the one-sided
         * 20:47 one.  That is the common/differential discriminator the mechanism
         * analysis derives: only the one-sided projection reaches the pins.
         */
        double cb_excursion_median_m{ 50.0 };
        /**
         * @brief Reinitialize every satellite's CB carry on a trip, not only the clock
         *        pivot's (@c network.cb_excursion_reinit_all_sats) (D1b).
         *
         * Default false = pivot only, the minimal response to the measured mechanism
         * (the pivot is the only satellite whose clock↔code split is both exposed to
         * the data and stiffly carried).  True is the safer choice if a pivot handover
         * can move the vulnerable satellite mid-excursion, at the cost of a mass
         * discontinuity bump that makes every consumer reset every satellite's CB
         * state.  Either way the run-end telemetry reports how many non-pivot
         * satellites would have tripped a per-satellite rule, so the choice can be
         * revisited from data.
         */
        bool cb_excursion_reinit_all_sats{ false };
        /**
         * @brief Per-epoch spectrum of the reduced shared normals, split by parameter
         *        family (@c network.trace_sh_spectrum) (SPEC_day129_frame_guards P1-3).
         *
         * Writes @c network_sh_spectrum.csv: one row per emitted epoch with λ_min /
         * λ_max / condition number of @f$N_{ss}@f$ and, for each of the smallest few
         * directions, how its eigenvector mass partitions across the clock /
         * code-bias / phase-bias / SH-VTEC column families.  That partition directly
         * tests the "joint clock↔iono↔field near-null slide" hypothesis for the a00
         * collapses (REPORT §3.2): a slide needs a smallest mode that spans both the
         * clocks and the field, not one confined to either.
         *
         * Distinct from @ref trace_estimability, a one-shot text dump of the near-zero
         * tail for datum debugging; this is a per-epoch time series for locating a
         * trigger, and it writes no log lines (the estimability warning text stays
         * golden).  Cost: one @c SelfAdjointEigenSolver of @f$N_{ss}@f$ per emitted
         * epoch (O(n_shared³), ~800³ GPS-only at 305 stations, a few % of a 41 s
         * epoch).  Trace only: never folded into the warm-resume fingerprint.  Default
         * off: no file, no eigensolve, byte-identical.
         */
        bool trace_sh_spectrum{ false };
        /// Write the smallest mode's heaviest per-column participation entries
        /// (@c network_sh_spectrum_mode.csv) every Nth traced epoch (P1-3).  The
        /// per-family mass partition is written every epoch regardless; this is the
        /// detail that is expensive to read.  <= 0 disables the detail file.
        /// Inert unless @ref trace_sh_spectrum.
        int  trace_sh_spectrum_stride{ 60 };
        /**
         * @brief Measure-only post-fit variance factor on the emit path
         *        (@c network.trace_postfit_sigma0) (SPEC_day129_frame_guards P1-4).
         *
         * The @c sigma0 column of @c network_epoch_status.csv is @c sigma0_adapt, the
         * within-epoch RAIM's σ̂0, which is computed only inside
         * @c if(within_epoch_armed && carry_warm).  With @c network.raim_within_epoch
         * off, that statistic never runs, the column reads 0.0000 on every row, and the
         * emit path has no post-fit residual statistic at all: an epoch with a
         * kilometre-scale clock error cannot be told from a clean one.
         *
         * When this is on, a robust variance factor is computed over every station's
         * full post-fit residual vector (not only the carried-ambiguity phase rows the
         * RAIM pools), split into code and phase pools, and traced as appended
         * @c network_epoch_status.csv columns.  Measure-only: nothing reads it, no gate
         * consumes it, and the A4 ceiling keeps using @c sigma0_adapt.  Costs one extra
         * per-station reconstruct and residual pass per emitted epoch.  Default off:
         * blank columns, byte-identical.
         */
        bool trace_postfit_sigma0{ false };
        /**
         * @brief Keyed robust post-fit residual aggregates
         *        (@c network.trace_postfit_keyed) (SPEC_day129_frame_guards P1-4, RHS
         *        localization).
         *
         * Phase 1 located the collapse trigger on the right-hand side (the onset
         * epoch's misfit is global, 303/303 stations > 3σ, and the normal matrix does
         * not move), so the next question is where the misfit sits: which stations
         * (geography: iono front or global), which satellites (per-satellite
         * structure: orbit/clock input), which signals (dispersive split: iono or
         * clock).  When this is on, the P1-4 residual pass also pools
         * @f$|v_i|\sqrt{w_i}@f$ per station, per satellite and per signal
         * ({code, phase} pools each), and writes one @c network_pf_keyed.csv row per
         * key per emitted epoch (σ̂0 = 1.4826·median, rms, max, n).  Makes the P1-4
         * pass run even with @ref trace_postfit_sigma0 off.  Trace only: never folded
         * into the warm-resume fingerprint.  Default off: no file, byte-identical.
         */
        bool trace_postfit_keyed{ false };
        /**
         * @brief Signed per-row post-fit residual dump (@c network.trace_postfit_rows)
         *        (SPEC_event_harness §24.6, per-LOS code-residual attribution).
         *
         * The keyed pools above pool per station, per satellite and per signal
         * separately and drop the sign, and @c network_station_residuals.csv is a
         * per-cell RAIM maximum without sign or signal, so no other output gives a
         * signed per-(station, satellite, signal) residual with its kind.  When this
         * is on, the same P1-4 residual pass writes one @c network_pf_rows.csv line per
         * data row it visits and can attribute to a family (kind ∈ {code, phase}):
         * @c week,tow,station,sat,signal,kind,resid_m,w,elev_deg, with @c resid_m the
         * signed post-fit residual @f$v = y - A_s x_s - A_r x_r@f$ (observed −
         * predicted, metres, not standardized), @c w the row's information weight (the
         * pass standardizes @f$|v|\sqrt{w}@f$), and @c elev_deg the LOS elevation from
         * the station's modeled observation set (empty when unavailable).  Rows the
         * pass attributes to neither family (local-column-only pseudo-observations)
         * are counted on the run-end line, not written.  Makes the P1-4 pass run (like
         * the keyed rider).  ~2.5 k rows per emitted epoch at 61 stations, streamed
         * (never buffered).  Trace only: never folded into the warm-resume
         * fingerprint.  Default off: no file, byte-identical.
         */
        bool trace_postfit_rows{ false };
        /**
         * @brief Per-station local-block audit (@c network.trace_station_block) to
         *        @c network_station_block.csv (SPEC_event_harness §24.10, card ZP).
         *
         * §24.9 measured a station whose own code rows fit to 0.3–0.55 m while its
         * reconstructed ZWD read −18 ± 4.5 m and 36 % of its rows carried zero
         * redundancy.  No output reported the object that explains both: the
         * station's local normal block
         * @f$N_{rr}=A_r^\top W A_r+\text{(folded station priors)}@f$, the matrix that
         * @ref NetworkEstimator::reconstructStation inverts with the shared states held
         * at their solved values @f$x_s@f$ (@f$x_r=N_{rr}^{-1}(u_r-N_{rs}x_s)@f$).
         * Armed, one row per station per traced epoch (the first 20 emitted epochs,
         * then every 60th) carries its numerical rank and condition number, its three
         * smallest eigenpairs with the local states that span them (clock / ZWD /
         * receiver code bias / per-LOS ionosphere / ambiguity), the ZWD column's
         * effective information after eliminating every other local state against its
         * raw @f$\sum w\,m_\text{wet}^2@f$, the reconstructed local correction's norm
         * and its ZWD element, and the count of zero-redundancy rows
         * (@f$r_i=1-w_i h_i^\top N_{rr}^{+}h_i<10^{-6}@f$) by kind.
         *
         * Measure-only: the sweep runs in the trigger-instrumentation group after the
         * solve and reads @ref NetworkEstimator through const accessors only; no
         * @c x, no @c N and no order of operations changes, on or off.  Cost: one
         * symmetric eigen-decomposition of a ~60–100-dimensional block per station on
         * the traced epochs only.  Trace only: never folded into the warm-resume
         * fingerprint.  Default off: no file, byte-identical.
         */
        bool trace_station_block{ false };
        /**
         * @brief Name the null local column behind every @c addStation PD rejection
         *        (@c network.trace_station_pd_null), as @c TOPOLOGY /
         *        @c sta_pd_dropped rows in @c network_events.csv (card RF-PD).
         *
         * @c NetworkNormalEquations::addStation rejects a station whose local block
         * @f$N_{rr}^r@f$ fails the relative PD test, and the driver drops it for the
         * epoch (@c present[i]=false) while @c network_epoch_status.csv still reports
         * the station as present.  On the RF-0g field posture this dropped a median of
         * 31 of 59 stations per epoch, so the 59-station configuration solved as a
         * ~28-station network: an uninstrumented drop (standing invariant #4) whose
         * only record was a warning line in a rotating log that names a pivot
         * magnitude and no parameter.
         *
         * Armed, each rejection also carries the spectral read of the rejected block
         * (@ref NetworkNormalEquations::StationPdFailure): the smallest eigenvalue, the
         * eigenvalue multiplicity under the relative cutoff, the pivot triple the
         * decision used, and the five largest-|weight| entries of the null eigenvector
         * resolved to StateManager labels (@c AMB:… / @c IONO:… / @c CBIAS:… / @c ZWD /
         * @c CLK) through the assembly's own @c h2local map.  The drop then names the
         * parameter the station cannot determine, which separates a standing
         * structural null (a datum the station cannot satisfy) from a newborn-arc
         * transient.  A per-epoch @c n_sta_pd_dropped count joins the run-end tail.
         *
         * Measure-only: the rejection, the drop and the message are unchanged; the
         * eigen-decomposition runs only on a block that is already being discarded.
         * Trace only: never folded into the warm-resume fingerprint.  Default off: no
         * eigen work, no rows, byte-identical.
         */
        bool trace_station_pd_null{ false };
        /**
         * @brief Per-satellite readout of the closed-loop consumer
         *        (@c network.trace_consumer_sat) to @c logs/closed_loop_sat_trace.csv
         *        (card CST).
         *
         * @c closed_loop_trace.csv carries week, tow, @c n_sats, the AR columns and the
         * position error, and nothing per satellite.  EV6
         * (@c docs/refound/EV6_hour6_decouple_event_2026-09-15.md §9 item 1) named this
         * as the blocker for the hour-6 consumer events on the RF-0hv field posture:
         * without per-satellite consumer content, a float re-convergence cannot be
         * separated from the slant-iono half of the RF-0b applied pair, and an
         * undeclared wire step cannot be seen landing.
         *
         * Armed, the consumer engine harvests one row per (satellite, raw phase signal)
         * at the tail of every @c processEpoch that runs the estimation tail, and the
         * producer's emit/store seam writes them beside @c closed_loop_trace.csv: the
         * applied clock / code-bias / phase-bias corrections (the consumer's own gated
         * reads, not a re-derivation), the producer's discontinuity counters as the
         * store presents them, what the consumer did with each declaration that epoch
         * (reset / carry / re-inflate / none: the re-coupling edge of invariant #7 on
         * the consumer side), the pre-fit innovations and post-fit residuals, and the
         * ambiguity and slant-iono state values with their variances (a declared reset
         * shows as variance re-inflation; an undeclared float re-convergence shows as
         * the ambiguity moving with large residuals and no variance step).
         *
         * Measure-only: the harvest reads state the epoch has already committed and
         * writes nothing back; the consumer's solve is byte-identical with the knob on.
         * Trace only: never folded into the warm-resume fingerprint.  Requires the
         * closed-loop harness (@c network.closed_loop.enable); without a consumer there
         * is nothing to read, and the loader reports it.  Default off: no buffer, no
         * event recording, no file.
         */
        bool trace_consumer_sat{ false };
        /**
         * @brief Screen the phase-bias discontinuity test on the de-commoned step
         *        (@c network.disc_jump_test_decommon) (card JT).
         *
         * The emitter's phase-bias jump test (@c NetworkProductEmitter::emitToStore)
         * compares the raw epoch-to-epoch @f$\Delta PB@f$ of each (sat, signal) against
         * @c 0.25\,\lambda and declares a discontinuity (one @c R-record counter bump)
         * when it is exceeded.  The raw test ignores the frame, and standing
         * invariant #5 explains why that matters: a satellite-common phase-bias move
         * is absorbed by a consumer's own receiver phase clock, so the quantity that
         * lands in a consumer ambiguity is the step minus the network's median step on
         * that signal.  CST §4 measured the failure: in one epoch the L1W phase-bias frame
         * moved @f$-1.451@f$ m, so one satellite's real @f$+1.409@f$ m de-commoned step
         * read as a raw @f$-0.042@f$ m and went undeclared into a live consumer
         * ambiguity, 29.6× its own bar; over the run, 451 of 545 de-commoned wire steps
         * above 0.10 m were undeclared (EV6).
         *
         * On, the test forms @f$\Delta PB(sat,s) - c_s@f$, where @f$c_s@f$ is the
         * median @f$\Delta PB@f$ over the satellites emitted on signal @f$s@f$ at both
         * epochs (the jump test's own fresh-baseline population), and screens that
         * against the same bar.  The median is taken within each signal: L1W and L2W
         * biases do not share a common mode.  A signal whose population is below
         * @c NetworkProductEmitter::kDiscDecommonMinSats falls back to the raw test and
         * is counted (@c fallback_raw).
         *
         * Invariant #7, both edges with identities: every cell the test decides
         * differently from the raw one writes a @c network_events.csv row
         * (@c PB_JUMP @c decl_decommon_only / @c decl_both / @c raw_only_suppressed
         * / @c fallback_raw) carrying sat, signal, raw ΔPB, @f$c_s@f$ and the
         * de-commoned step, and the four classes are run-end counters.
         *
         * Ordering (CST §7.3): this adds declarations, and a reset-only consumer turns
         * every added declaration into an added mass ambiguity reset, so on its own it
         * is predicted to make the consumer worse.  Arm it only on a lineage that
         * already carries @c network.closed_loop.pb_disc_carry, where an added
         * declaration costs a translation and not an arc.
         *
         * Solve-inert on the producer: only the emitted discontinuity counters change,
         * never an estimate.  Travels in the S contract (@c discdecom=), so a file
         * replay reproduces the counters.  Default off: byte-identical.
         */
        bool disc_jump_test_decommon{ false };
        /**
         * @brief Bump only the generation of the channel that moved
         *        (@c network.disc_mark_scope, @c "sat" (default) | @c "channel")
         *        (card MS).
         *
         * `NetworkProductEmitter::markSatDiscontinuity` drops a satellite into one
         * `forced_bump_sats_` set, and `satForced()` is read by the clock, code-bias and
         * phase-bias emit blocks alike, so a clock-only guard (the Q3 anchor decouple,
         * the CLK-R step test) declares a phase-bias discontinuity that did not happen.
         * Measured on RF-0hv: 117 of 390 declared consumer cell-epochs were empty
         * (< 0.02 m of phase-bias motion), and the hour-6 ramp was the consumer
         * resetting five satellites for a 20-satellite clock decouple (CST §7.1 / EV6).
         *
         * Under @c "channel" each marking site names the channels it reports, and only
         * those generations step; a site that moves two channels names both.  The
         * re-coupling edge is unchanged: the emitter's own per-(sat, signal) phase and
         * (opt-in) code jump tests still declare a genuine re-realization on a channel
         * that no mark named, so such re-realizations stay declared.
         *
         * Replay: the @c R record carries satellite ids and no channel set, so a
         * @c NetworkProductIO replay re-marks with the full satellite scope.  The scope
         * travels in the S contract (@c discscope=), so @c checkContract names the
         * divergence rather than hiding it; a channel-scoped lineage that must be
         * replay-exact needs an R-record grammar extension (open item).
         *
         * Emit side only: no estimate, no datum, no row of the normal equations.
         * Default @c "sat" is byte-identical.
         */
        std::string disc_mark_scope{ "sat" };
        /**
         * @brief Hold the emitted phase bias of a (satellite, signal) at the first value
         *        of its generation (@c network.emit_pb_hold) (card PBH).
         *
         * PBH read 3 measured the wire's phase-bias movement against truth on both
         * beds: the within-arc RMS of the phase bias alone is 0.0227 m (sim) and
         * 0.0467 m (field), against 0.0030 / 0.0274 m for the clock alone, and the
         * correlation between the first differences of the two errors is +0.006: the
         * phase bias does not compensate the clock, it adds to it.  On the simulator
         * the true satellite phase bias is constant in time, so every movement of the
         * emitted value is error by construction.
         *
         * The resulting contract passes identities, like the @c iode= ephemeris
         * cutover, rather than signed frame deltas: the emitted value of a key is
         * frozen at the first value of its generation, and a new generation is
         * declared (@ref SsrPhaseBiasEntry::hold_generation, the @c gen= wire token),
         * so a consumer re-floats on the declaration instead of integrating a
         * wandering correction.  Handover steps, cold or continuous, never reach the
         * wire and therefore need no delta.
         *
         * Emit side only.  The estimator's own value keeps evolving behind the hold:
         * the solve, the clock records and the code-bias records are byte-identical
         * with the knob on, and the held value is written before the emitter's
         * phase-bias value-jump test, so that test screens what the consumer receives.
         *
         * Default off: byte-identical (no hold, no @c gen= token, no @c pbhold= in the
         * S contract).
         */
        bool emit_pb_hold{ false };
        /**
         * @brief Arc gap (s) beyond which a returning key starts a new hold generation
         *        (@c network.emit_pb_hold_max_gap_s, default 300) (card PBH).
         *
         * A key not emitted for longer than this is a re-acquisition, not a
         * continuation: holding it at a value estimated before the gap would assert a
         * frame the estimator has since abandoned.  The gap is measured in product
         * content time between the key's own emissions, and crossing it is an event at
         * both edges (a @c PB_HOLD / @c gen_start row with cause @c gap, plus the
         * run-end counters), as a first sighting is.
         *
         * Inert unless @ref emit_pb_hold.
         */
        double emit_pb_hold_max_gap_s{ 300.0 };
        /**
         * @brief Hold the phase-visible clock instead of the phase bias
         *        (@c network.emit_pb_hold_phase_clock, default false) (card LAH).
         *
         * With @ref emit_pb_hold alone, a held phase bias is frozen while the emitted
         * clock moves, so a level move (clock and every phase bias of the satellite
         * together, invisible to phase) reaches the wire's phase side
         * @f$c - p@f$.  With this mode the held emission is
         * @f$p(t_0) + [\tilde c(t) - \tilde c(t_0)]@f$, where @f$\tilde c@f$ is the
         * emitted clock less the accumulated per-epoch median clock step of the
         * satellite's constellation over its satellites present at consecutive
         * emissions: the phase-visible clock is constant over a generation up to a
         * per-constellation common mode the consumer's receiver clock and inter-system
         * bias absorb, and every clock move (the level included) reaches the code only.
         * Refuses @c network.fast_clock.enable (card FC-1): its K records move the clock
         * between full epochs without the held phase bias.
         *
         * Posture limit: within a generation the consumer's phase-visible clock motion
         * is the a priori clock product's.  Valid with a precise a priori clock (the
         * simulator's truth, the COD finals on the field); with a broadcast a priori
         * (@c apriori_source brdc_rts) it would drop the real clock motion.  Emit-side
         * only: the solve, the C and B records are byte-identical.  Declared on the S
         * line as @c pbhold=2.  Requires @ref emit_pb_hold; refuses
         * @c network.clock_level_carry (card CLV), whose transfer it replaces.
         */
        bool emit_pb_hold_phase_clock{ false };
        /**
         * @brief The smoothed hold (@c network.emit_pb_hold_smoothed, default false)
         *        (card SMH).
         *
         * The held key keeps @f$(C - P)(t) = (c - p)(t_0|t_0) + \Phi(t)@f$ with
         * @f$\Phi = c(t|t) - c(t_0|t)@f$: the phase side keeps the generation-start
         * phase-visible clock plus the estimator's smoothed clock motion since @f$t_0@f$,
         * and a revision of @f$c(t_0)@f$ (the level) reaches the phase bias, so the code
         * side only.  It is written as the phase-clock transfer of
         * @ref emit_pb_hold_phase_clock minus @f$\Phi@f$ accumulated frame-free, so the
         * members share one frame (card SMH section 10).  @f$c(t_0|t)@f$ is a clock clone
         * per hold key, a coupling row with no observation rows time-updated by card CTU's
         * exact rule (@ref NetworkPbHoldClone.hpp).  A clone lives while card Q3's anchor holds
         * its clock: it is born at the first stash at which the anchor holds, dies at the
         * epoch the anchor withdraws the clock, and the key bridges on the phase-clock
         * transfer (@ref emit_pb_hold_phase_clock's rule, its common-mode median included)
         * until the next birth.  A clone that would take card 1d's route dies and its key
         * carries the phase-clock transfer for the rest of the generation.  Requires
         * @ref emit_pb_hold, @c network.carry_coupling_exact_update,
         * @c network.raim_within_epoch (whose fold capture records which keys the final
         * solve folded) and @c network.clock_temporal_anchor without
         * @c network.clock_model, all refused at load time on their resolved values; excludes
         * @ref emit_pb_hold_phase_clock (its fallback); refuses
         * @c network.clock_level_carry and @c network.fast_clock.enable for the phase-clock
         * mode's reasons.  Emit-side only: the solve, the C and B records are
         * byte-identical.  Declared on the S line as @c pbhold=3.
         */
        bool emit_pb_hold_smoothed{ false };
        /**
         * @brief The smoothed hold's phase-visible member
         *        (@c network.emit_pb_hold_smoothed_block, default false) (card SMD).
         *
         * The held key passes @f$\Phi_D = D(t|t) - D(t_0|t)@f$ of the ionosphere-free
         * phase-visible clock @f$D = c - \sum_j w_j p_j@f$ (one phase bias per band, fixed at
         * the clone's birth) instead of the clock's @f$\Phi@f$: a level walk or revision
         * cancels (@f$\sum w = 1@f$), a clock walk passes.  @f$D(t_0|t)@f$ is a block clone's
         * row (@ref NetworkPbHoldClone.hpp), which retires at every re-reference, new anchor
         * tenure and emitter phase-channel mark of its pair, the key bridging on the
         * phase-clock transfer until it is born again.  The block clones are carried whenever
         * @ref emit_pb_hold_smoothed is armed (they report the split of the passed motion);
         * this knob only selects them for the wire.  Requires @ref emit_pb_hold_smoothed.
         * Emit-side only.  Declared on the S line as @c pbhold=4.
         */
        bool emit_pb_hold_smoothed_block{ false };
        /**
         * @brief Fold the station's own datum rows into its local block before the
         *        @c addStation positive-definiteness test
         *        (@c network.station_prior_before_pd) (card RF-PD-fix).
         *
         * `NetworkNormalEquations::addStation` judges a station on its observation rows
         * alone, and the driver drops a rejected station for the epoch
         * (`present[i]=false`), while the station's own prior rows (above all the
         * RXCB-DATUM row) are installed by `addStationPrior` one call later.  Without
         * this knob the station is judged without the only row that gauges its
         * receiver DCB, and discarded for lacking it.  Card RF-PD measured 23 633
         * (golden) / 30 340 (vehicle) rejections over 720 epochs of the 6 h RF-0g field
         * posture, every one with the same rank-1 null (the receiver-DCB ↔ slant-iono ↔
         * ambiguity confound; mass split AMB 77.8 % / IONO 21.0 % / CBIAS 1.2 %;
         * `n_eig = 1` on all rows); the stations that passed did so only because a
         * prioritized phase-datum anchor's 1e10 pin happened to land in their block
         * (anchored station-epochs rejected: 0 / 14 370).
         *
         * On, the RXCB-DATUM rows travel to @ref NetworkNormalEquations::addStation as a
         * @ref NetworkNormalEquations::StationPrior and are folded into @f$N_{rr}@f$ /
         * @f$u_r@f$ before the LDLᵀ factorization, so the test sees the matrix the solve
         * uses.  The same rows are then withheld from the later `addStationPrior` call:
         * the two paths do not sum, so a row that entered here must not enter again.
         *
         * @par Scope: datum rows only
         * The phase-leveling / rx-CB carry rows stay on the post-test path deliberately
         * (card RF-PD §6 (a)): folding a carried value into the admission decision
         * would let the carry's own staleness decide whether a station is in the solve,
         * which is the decoupling edge of invariant #7 and needs its own card.
         *
         * Solve-affecting (it changes which stations are in the network), and inert
         * without @c network.rx_code_bias_datum, since there are then no datum rows to
         * move.  Default off: nothing is collected or folded early, byte-identical.
         */
        bool station_prior_before_pd{ false };
        /**
         * @brief SH-field health gate: withhold emission, not only warn
         *        (@c network.sh_health_gate) (SPEC_day129_frame_guards phase 2.2).
         *
         * Without it, a run can emit a non-physical ionosphere for many epochs with
         * @c gate_ok=1 (on the reference day: 80 minutes, a₀₀ up to 2151 TECU), because
         * the a₀₀ physical-band check alone is a warn-only probe, edge-triggered and
         * throttled (9 warnings for 214 out-of-band epochs).  With this on, an epoch
         * whose total mean VTEC a₀₀ (layer sum) leaves the physical band
         * [@c a00_warn_low_tecu, @c a00_warn_high_tecu], or whose total-field
         * coefficient RMS exceeds @ref sh_gate_rms_max_tecu, for @ref sh_gate_min_epochs
         * consecutive epochs is withheld from emission: no store push, no product
         * write, no closed-loop consume, no @c n_emitted advance.  Emission only: the
         * solve, the carry stash and every QC tier still run, so state evolution is
         * untouched (a multi-hour out-of-band era must not expire the carry the way the
         * A4 skip-epoch semantics would), and the toggle is not in the warm-resume
         * fingerprint (same class as @c raim / @c clock_runaway_check).  Engage and
         * release transitions are logged without throttling and counted per epoch
         * (run-end and @c network_events.csv @c SH_GATE rows), per the handoff §4.2
         * lesson.  Default off: byte-identical.
         */
        bool sh_health_gate{ false };
        /// Consecutive out-of-band epochs before the gate engages (phase 2.2).  1 =
        /// withhold from the onset epoch: the code-bias ratchet of the reference event
        /// lands in one epoch, so waiting would emit the damaged product.  Release is
        /// immediate on the first in-band epoch.  Inert unless @ref sh_health_gate.
        int sh_gate_min_epochs{ 1 };
        /// Total-field coefficient RMS ceiling (TECU; the same all-coefficient RMS
        /// that the T1a health trace reports as @c rms_all), the spec's "or
        /// total-field RMS" band clause (phase 2.2).  Catches a blow-up that an
        /// in-band a₀₀ hides (e.g. a degree-1 tilt excursion).  0 = off (a₀₀ band
        /// only).  Inert unless @ref sh_health_gate.
        double sh_gate_rms_max_tecu{ 0.0 };
        bool trace_section_timers{ false };  ///< Per-epoch section-timer breakdown of the network solve (D0): @c NetworkEstimator wall-clock accumulators around assembly / Schur reduce / constrained point-solve / the explicit shared-covariance full inverse / the per-station covariance consumers.  Arms @c NetworkNormalEquations::setPerfTiming so those primitives record their milliseconds; the driver logs a @c "Network (D0)" line per emitted epoch and an end-of-run "RUN TOTALS" line, so a profiling run shows which term dominates and therefore which Tier-D scaling option (D1 selected inverse / D2 rate split / D3-D4 channel and ordering) is worth pursuing.  Diagnostic only: no product bytes, no decision change; when off, every instrumented primitive skips the clock read entirely (no measurable overhead, numerically byte-identical).
        bool write_run_manifest{ false }; ///< Write @c network_run_manifest.json (JSONL, two records) (L15): a run-provenance snapshot at startup (config path and FNV-1a-64 hash of the config bytes, build stamp, and the full network.* toggle/knob dump), plus an end-of-run record with the epoch counts and loop wall time.  Makes an archived @c logs/ folder self-describing (which toggle set produced it) even when the code is uncommitted.  Does not read git state.  Off by default.
        double health_interval_s{ 0.0 };  ///< Operator health-record cadence in seconds of processed (content) time (RTB-6).  When > 0, the producer appends a machine-readable @c network_service_health.csv row every @c health_interval_s of data: cumulative epochs emitted / skipped / gate-withheld / σ̂0-withheld / runaway, the σ̂0 EWMA (−1 sentinel = unavailable when its feeding tiers @ref emit_ura, @ref raim and @ref phase_leveling are not all armed; the value would otherwise stay at its 1.0 initial value), stations present, clock satellites, mean ms per epoch over the health window just ended, last-emit age, and the realized per-constellation clock pivots ("G:G05;E:E11"; empty before the first emit).  A service run is thereby observable without waiting for the exit-time summary (every other health output assumes a finite run).  Distinct from the RTA-10 per-epoch product @c H status record (this is an aggregate operator heartbeat, not product content) and from the pyviz @c network_health viewer (which reads @c network_trace.csv, a different output, hence this file's @c service name).  Column semantics: @c docs/rt_service_operations.md.  Negative values clamp to 0 with a warning.  Default 0 = off: no file, byte-identical.
        double trace_rotate_mb{ 0.0 };    ///< Size-based rotation threshold (MiB) for the unbounded service CSVs: the always-on @c network_trace.csv / @c network_solution.csv and the RTB-6 @c network_service_health.csv (P1).  When > 0, a file crossing the threshold is closed, the suffix chain shifts (<tt>file.csv.k → file.csv.(k+1)</tt> up to @ref trace_rotate_keep, oldest deleted), the live file becomes <tt>file.csv.1</tt>, and a fresh live file reopens with the header rewritten, so every rolled generation parses standalone.  The pyviz viewers discover only the live file name; rotated @c .1…@c .keep suffixes are not read, so the operator concatenates the chain (stripping the headers of non-first generations) or points a viewer at one generation (see @c docs/rt_service_operations.md).  Checked once per processed epoch, so a file may overshoot by up to one epoch's rows.  Negative values clamp to 0 with a warning.  Default 0 = off: no rename or close ever happens, byte-identical.
        int    trace_rotate_keep{ 3 };    ///< Rolled generations retained per rotating CSV (<tt>.1 … .keep</tt>; the shift deletes <tt>.keep</tt> first) (P1).  Inert unless @ref trace_rotate_mb > 0.  Values < 1 clamp to 1 with a warning.  Default 3.
        double warmup_ura_floor_m{ 0.0 }; ///< Cold-start quality-ramp URA floor (m) (RTB-7(b)).  While the start-up ramp is active, the emitted per-satellite URA is floored at this value, so a freshly (re)started producer advertises degraded quality and a consumer deweights or withholds instead of trusting an unconverged network solution.  The ramp lifts (see @c networkWarmupRampLifted) when both hold: (1) emitted epochs ≥ max(@ref warmup_epochs, @c phase_leveling_warmup_epochs), i.e. the carry population is warm, not merely counted; and (2) the σ̂0 EWMA has received ≥ @ref warmup_epochs updates, or the EWMA is structurally unavailable this run (the RTB-6 sentinel condition: @c emit_ura, @c raim and @c phase_leveling not all armed; the epoch count alone then decides).  The lift is logged with the RTB-7(c) cold-start-to-product-grade metric (also a @c network_service_health.csv column).  On an RTB-7(a) warm resume the ramp state is restored from the snapshot, so a warm-resumed producer is not degraded.  Requires @c network.emit_ura (without it the loader warns that the floor has no effect).  Default 0 = off: emitted URA unchanged (byte-identical).
        int    warmup_epochs{ 0 };        ///< Quality-ramp horizon (RTB-7(b)): the minimum emitted-epoch count and the minimum σ̂0-EWMA update count required before @c warmup_ura_floor_m lifts (combined with @c phase_leveling_warmup_epochs; see @ref warmup_ura_floor_m for the exact rule).  Inert unless @c warmup_ura_floor_m > 0.  Default 0 = off (the ramp then waits only for the phase-leveling warm-up).
        // --- Producer state snapshot and warm resume (RTB-7(a)) ---------------
        /// Path of the producer state-snapshot file (empty = off, byte-identical).
        /// A relative path lands under the network log directory.  When set, the
        /// producer atomically rewrites this file every @ref state_snapshot_interval_s
        /// of product time (temp file and rename, so a crash mid-write never corrupts
        /// the previous good snapshot) with both cross-epoch state layers (driver
        /// carries / oscillators / pivots / pending-bump sets / generation id, and the
        /// emitter's discontinuity stream state).  At start-up, a snapshot that
        /// parses, matches the run's datum-relevant config fingerprint, and is younger
        /// than the carry-validity horizon (the max of @c phase_leveling_max_gap_s /
        /// @c clock_model_max_gap_s against the first live epoch) triggers a warm
        /// resume: state is restored, the RTA-3 generation id is preserved (no consumer
        /// reset), and per-key staleness is judged by the existing downstream gap
        /// handling.  Anything else cold-starts safely with a warning and a fresh
        /// generation id.  See @c products/NetworkStateSnapshot.hpp.
        std::string state_snapshot_path;
        /// Snapshot cadence in seconds of product (content) time (RTB-7(a)).  Checked
        /// at the end of each emitted epoch.  Non-positive values clamp back to the
        /// default with a warning.  Inert while @ref state_snapshot_path is empty.
        /// Default 10.
        double state_snapshot_interval_s{ 10.0 };
        /// Also serialize the invariant DD carry (the z-store branch and the
        /// carry_sat_vertex / carry_group_id numbering) into the snapshot, so that a warm
        /// resume is branch-faithful: the phase-ambiguity integer/lane realization
        /// resumes identically instead of the phase side re-converging onto a different
        /// branch, as it does when the carry is not serialized (RTB-7(c)).  Requires
        /// @ref carry_invariant and @ref state_snapshot_path.  Off by default:
        /// byte-identical snapshots (no SV/GG/CARRY records emitted).  Adds ~Σ K_c²
        /// doubles per snapshot (the dense per-component @f$P^z@f$), so the snapshot
        /// grows with the carried DD count.
        bool state_snapshot_carry{ false };
        /// Write the carry blob's per-component @f$P^z@f$ as its diagonal only (blob v3,
        /// one @c PD row per block instead of @c K_c dense @c P rows) (RTB-7(e)).  The
        /// dense @f$P^z@f$ is @f$\sum K_c^2@f$ numbers against @f$\sum K_c@f$ means and
        /// dominates the file: at 305 stations, 13,086,007 covariance numbers against
        /// 7,869 means, i.e. 99% of a 258 MB snapshot, while every other layer (D2
        /// border, emitter, pivots, counters) totals 48 KB.  Diagonal-only is ~1663x
        /// smaller (258 MB → ~160 KB), on the hypothesis that the resumed branch (which
        /// integer / lane realization the phase side lands on) lives in the means and
        /// the cross-covariance is re-earned from data within an epoch or two, as the
        /// C1 dense shared border already resumes.  It is a real trade: a diagonal prior
        /// is not a uniform inflation; it tightens the common-mode direction of
        /// correlated chords while loosening the coordinate directions, so it must be
        /// A/B-tested on datum-invariant quantities before it is trusted.
        ///
        /// Write side only: @c restoreCarry dispatches on the blob's row tag, so either
        /// shape restores under either setting, and this knob is deliberately not in the
        /// warm-resume config fingerprint (it is a serialization representation, not a
        /// datum-relevant rule), which lets a full-blob and a diagonal-blob resume arm
        /// be compared against one shared converge run.  Off (default): the v2 blob,
        /// byte-identical.  Inert unless @ref state_snapshot_carry.
        bool state_snapshot_carry_diagonal{ false };
        /// Also write each chord's linearization double-difference
        /// @f$z_\ell=\sum c\,x_{\mathrm{lin}}@f$ into the carry blob (one @c ZL row per
        /// block, beside @c Z) (RS-2, docs/SPEC_event_harness.md §21.4).
        ///
        /// The serialized @f$\hat z@f$ is the correction-frame DD (@f$\lambda N@f$ minus
        /// @f$\sum c\,x_{\mathrm{lin}}@f$), so it is meaningful only against the writer's
        /// own linearization.  A resumed run re-seeds every station ambiguity @c x_lin
        /// from code at its first epoch, and the prior fold then forms
        /// @f$v=z_{\rm free}-G^\top x_s-\hat z@f$ against that linearization, so every
        /// restored chord is wrong by
        /// @f$\sum c\,(x_{\mathrm{lin,writer}}-x_{\mathrm{lin,now}})@f$ and is folded
        /// stiff (measured on the field golden: the whole first post-resume solve
        /// discharges through one prior class, @c dd_fold norm 7.8e7 against 616 for
        /// every code observation of 61 stations).  This row is the writer half of the
        /// repair; @c network.resume_carry_rebase is the reader half.
        ///
        /// Write side only and format-forward: a reader that finds no @c ZL row behaves
        /// as before, so the knob is not in the warm-resume fingerprint.  Products and
        /// every trace stay byte-identical when armed; only the snapshot gains the rows.
        /// Inert unless @ref state_snapshot_carry.
        bool state_snapshot_carry_lin{ false };
        /// One-shot resume-event response for the restored stiff carry: σ (m) of the
        /// variance added to every carried DD's Pᶻ diagonal right after a successful
        /// carry restore (RTB-7(d) RE-2).  0.0 (default) = off, byte-identical.
        /// Rationale: the restored means are the branch and are kept; the per-station
        /// filter layer (deliberately excluded from the snapshot) re-converges over the
        /// following epochs and pushes real transient tension through the phase rows
        /// that the restored DD priors make redundant.  Widening the carried variance
        /// lets the fold absorb that instead of the D5 gate discarding the branch, with
        /// the gate still armed (Cov(v) inflates too), so invariant #1 is satisfied, not
        /// bypassed.  Scale: the restored prior already carries q_z·Δt (1e-4·30 s =
        /// 3e-3 m², σ ≈ 55 mm ≈ 0.29 L1 cycles), so a shock below that has no effect.
        /// Inert unless @ref state_snapshot_carry.
        double state_snapshot_resume_shock_m{ 0.0 };
        /// Make any failure to complete a warm resume (missing file, parse failure,
        /// fingerprint mismatch, age gate, emitter-section failure) a hard run error
        /// instead of a warning and a cold start (RTB-7(d) HB-1).  Off by default
        /// (byte-identical).  Guards the same-snapshot A/B hazard: a resume overwrites
        /// the snapshot it read, so a relaunch without re-seeding sees a snapshot from
        /// its own future, the age gate rejects it, and the run cold-starts with only a
        /// warning, which a development harness would then grade as if it were warm.
        /// Arm this in any A/B.
        bool state_snapshot_require{ false };
        /// Also write every snapshot to `<state_snapshot_path>.ep<NNNN>` (zero-padded
        /// epoch index, same atomic temp-file and rename), giving a per-epoch blob
        /// history for branch grading (`scripts/diff_carry_blobs.py`) (RTB-7(d) BR-1).
        /// Write side only: the read path is untouched and the estimation is
        /// structurally byte-identical (the writer runs after emit over const refs).
        /// Off by default; heavy on disk.
        bool state_snapshot_keep_history{ false };
        /// Resume frame capture (§18.10): also serialize the realization-frame state
        /// (phase-anchor incumbents and sticky pin targets, Q3 clock-anchor
        /// per-satellite state, CLK-R held frame) into every snapshot, under the
        /// format-v2 header.  Off (default): the v1 format byte for byte (old readers
        /// keep parsing); a resume can use the rows (network.resume_frame_restore) only
        /// when the writer captured them.  Write side only; inert unless
        /// state_snapshot_path.
        bool state_snapshot_frame_capture{ false };
        /// Resume coupling-regressor capture (§18.12; the write side of
        /// network.resume_coupling_restore): also serialize every carried leveling / ZWD
        /// row's card-1 regression on the free shared border, under the format-v3
        /// header: a QS shared-column dictionary keyed on (identity, ŝ_prev, P_prev), one
        /// packed QL/QZ line per carried row holding only (dictionary id, M_rj) in
        /// harvest order, and the QR write-epoch clock-refloat set (a hard-refloated
        /// satellite is skip-stashed, so no other record can carry the flag).
        /// Deliberately a separate knob, not a clause of
        /// @ref state_snapshot_frame_capture: the regressors are ~37x the rest of the
        /// file (~9.9 MB at 63 stations, 131 terms per row; ~14 GB over a 12 h leg with
        /// @ref state_snapshot_keep_history), so tying them to the frame knob would
        /// inflate every inexpensive frame recipe and make the stored branch library
        /// ambiguous.  Requires state_snapshot_frame_capture (hard error otherwise): the
        /// restore is reachable only behind network.resume_frame_restore, which is inert
        /// on a snapshot with no frame rows.  Write side only: the capture block reads
        /// @c ctx.carry after the emit and writes only into the snapshot, so the writer
        /// run's products and every trace stay byte-identical.  Off (default): the v1/v2
        /// bytes are unchanged (the containers stay empty, so @c hasCouplingState() is
        /// false and the header selector and the @c END line count are untouched).
        /// Inert unless state_snapshot_path.
        bool state_snapshot_coupling_capture{ false };
        /// Resume datum capture (RS-3; the write side of network.resume_datum_restore,
        /// docs/SPEC_event_harness.md §21.7): also serialize the two datum memories the
        /// snapshot otherwise lacks: the eq34 chain datum's last-epoch spanning tree
        /// (@c EQD sticky datum receiver per (constellation,signal), @c EQP each
        /// receiver's parent-edge satellite, @c EQS each satellite's component root,
        /// @c EQT last epoch's pin targets, @c EQE the longevity elevations, @c EQR the
        /// gap-retained edges) and the time-linked code-bias datum memory (@c CBP the
        /// previous-epoch solved split, @c CBS the ever-harvested satellite set).
        /// Without them a warm resume re-elects the tree from scratch and re-seeds every
        /// code-bias channel: measured at the field seam, 110 @c PHASE_DATUM/eq34_reref
        /// and 30 @c CB_DATUM/reseed_disc_landed events at the first post-resume epoch,
        /// an R record naming 30 satellites, and a 14 m per-satellite phase-bias offset.
        /// A separate knob, not a clause of @ref state_snapshot_frame_capture: the frame
        /// rows are the satellite side of the same tree (@c anchor_incumbent) and are
        /// already in wide use, while these are the receiver side plus a second,
        /// unrelated datum; pairing them is a recipe decision, not a format one.  The rows
        /// use the existing header version (the optional-@c var_first precedent): a
        /// reader that finds none behaves as before, and an older binary fails the tag
        /// check and cold-starts, which is this format's documented safe path.  Write
        /// side only: the capture block reads @c ctx.carry after the emit and writes only
        /// into the snapshot, so the writer's products and every trace stay
        /// byte-identical.  Inert unless state_snapshot_path.
        bool state_snapshot_datum_capture{ false };
        /// Resume emit-layer capture (RS-5; the write side of network.resume_emit_restore,
        /// docs/SPEC_event_harness.md §21.10): also serialize the two output-only emit
        /// engines the snapshot otherwise lacks: @c NetworkEmitRebase's emission frame
        /// (@c ER, one previous emitted bias per "sat|signal") and @c NetworkPbEmitFilter's
        /// per-key scalar-KF posterior (@c PF) plus its per-constellation σ̂_step
        /// accumulators (@c PS).  Both are plain run-locals, so without these rows every
        /// warm resume cold-starts them, and the first post-resume epoch emits the raw
        /// solve-frame phase bias while the continuous wire carries the whole
        /// accumulated re-base (measured at the field seam: 17.666 m p50 per (sat,signal)
        /// emitted against 1.695 m in the estimator's own state, the difference
        /// accounted for exactly by the two emit layers, §21.10).  A separate knob, not a
        /// clause of @ref state_snapshot_frame_capture: those rows are the estimator's
        /// realization frame, while these are the emit layer on top of it, and pairing
        /// them is a recipe decision.  The rows use the existing header version (the
        /// RS-3 / optional-@c var_first precedent), so a reader that finds none behaves
        /// as before.  Write side only: the capture block reads the engines after the
        /// emit and writes only into the snapshot, so the writer's products and every
        /// trace stay byte-identical.  Inert unless state_snapshot_path.
        bool state_snapshot_emit_capture{ false };
        /// Resume station-layer capture (RS-6; the write side of
        /// network.resume_station_restore, docs/SPEC_event_harness.md §21.13): also
        /// serialize the two per-station cross-epoch memories the snapshot otherwise
        /// lacks: the rx-CB carry (@c RC, one receiver code-bias posterior per (station
        /// roster index, CB state key), with exactly the @c Z row's field set) and the
        /// T5 / Iono-2 per-LOS SH representation-error deviation carry (@c SD, one
        /// @c ShDevCarry per "STATION|SAT").  Both live in @c NetworkCarryState, and
        /// without this knob neither is in the snapshot, so every warm resume restarts
        /// them cold: the rx-CB restart re-splits code bias / iono / clock per station
        /// and signal (its 10σ inject gate withholds 26 % of priors on a leg against
        /// 13 % on the seed), and the deviation restart cold-seeds ~400 line-of-sight
        /// entries at the first leg epoch, which causes the SH a₀₀ transient (§21.13).
        /// A separate knob, not a clause of @ref state_snapshot_emit_capture: those rows
        /// are the emit layer, these are the station layer underneath the estimator.
        /// The rows use the existing header version (the RS-3 / RS-5 precedent), so a
        /// reader that finds none behaves as before.  Write side only: the capture block
        /// copies @c ctx.carry after the emit into the snapshot alone, so the writer's
        /// products and every trace stay byte-identical.  Inert unless
        /// state_snapshot_path.
        bool state_snapshot_station_capture{ false };
        /// Canonical-signal capture (RS-8, SPEC_event_harness §21.16; the write half of
        /// @c network.resume_signal_canon_restore): every snapshot also carries each
        /// station's per-(constellation, band, obs-type) canonical signal, the latch that
        /// @c PppModel::decideConstellationCanonicals picks from the station's first
        /// processed epoch's satellite counts and then locks for the run, as
        /// format-forward @c CN rows under the existing header.  Without it a warm
        /// resume re-decides the latch from the resume epoch's counts; in the measured
        /// case three stations flipped between L2X and L2W, which changed the L2X
        /// tracker set, left the eq34 L2X component without its datum receiver, and
        /// stepped the six L2X phase-bias families by ~25 m one epoch after the resume
        /// (§21.16).  A separate knob, not a clause of
        /// @ref state_snapshot_station_capture: those rows are estimator carries, this is
        /// a per-station signal-selection latch.  Write side only: the capture reads each
        /// station's model after the emit and writes into the snapshot alone, so the
        /// writer's products and every trace stay byte-identical.  Inert unless
        /// state_snapshot_path.
        bool state_snapshot_signal_capture{ false };
        /// Code-bias linearization-point capture (RS-9; the write side of
        /// network.resume_rxcb_lin_restore, docs/SPEC_event_harness.md §21.18): beside
        /// every @c RC row, also serialize an @c RL row holding the station
        /// @c StateManager's current @c x_lin for that code-bias state.  The @c RC row
        /// carries the rx-CB carry's numerator; this is the reference its inject gate
        /// differences against (@c cb_innov_raw = @c value_abs − @c x_lin(cb), against a
        /// fixed 5.13 m bar), seeded once from one epoch of code behind
        /// @c StateInitializer::isCodeBiasInitialized and never written back.  With only
        /// the numerator restored (RS-6), a warm-resume leg re-derives the reference
        /// from the resume epoch, and 17 of 61 field stations sat outside the bar every
        /// epoch against 7 in the continuous run, losing their receiver-DCB memory for
        /// the whole leg (§21.18).  Requires @ref state_snapshot_station_capture (an RL
        /// row with no RC row would restore a reference for a carry that is not there);
        /// the loader warns and disarms without it.  The rows use the existing header
        /// version (the RS-3 / RS-5 / RS-6 precedent).  Write side only: the capture
        /// block reads the station state vectors after the emit and writes only into the
        /// snapshot, so the writer's products and every trace stay byte-identical.
        /// Inert unless state_snapshot_path.
        bool state_snapshot_rxcb_lin_capture{ false };
        bool trace_station_residuals{ false }; ///< Emit @c network_station_residuals.csv (L13): every post-fit RAIM cell (per non-pivot station with carried ambiguities, per sat) with its measurement-standardized residual @c s_raw (already B2-normalized), the σ̂0-normalized @c s_norm, and the epoch σ̂0.  Identifies the station that corrupts the network solution.  Very heavy (≈ stations x sats x RAIM epochs, e.g. 50x40x3600 ≈ 7M rows); requires @ref raim at runtime with the RAIM ambiguity pool populated (by @ref phase_leveling, or by network.raim_carry_invariant on a carry_invariant lineage).  Off by default; byte-identical when off.
        bool trace_raim_redundancy{ false }; ///< Emit @c network_raim_redundancy.csv (SPEC_t3_null_derivation §7.1): one row per post-fit RAIM phase row (station x sat x signal x row) with its weight, Baarda redundancy @c r_i, raw residual, measurement-standardized @c z_meas = |v|·√w and Baarda-standardized @c z_baarda = z_meas/√r_i.  The RAIM cell is a maximum over signals, so @ref trace_station_residuals cannot separate L1C from L2W/L5Q; this trace can.  Diagnostic: it makes @c row_r be computed but never used, so unlike @ref raim_redundancy_normalize (which is solve-affecting) the census is solve-neutral.  Costs one @c rowRedundancy call per station per epoch.  Requires @ref raim at runtime with the RAIM ambiguity pool populated (by @ref phase_leveling, or by network.raim_carry_invariant on a carry_invariant lineage).  Off by default; byte-identical when off.
        bool trace_ssr_consumer{ false }; ///< Consumer-side trace (L10): the RealTimeEngine SSR consumer writes @c ssr_consumer_trace.csv (correction age, clk-applied, c0, URA, the recomputed R-inflation add-variance, phase-bias age/freshness, and disc counters) per modeled satellite per processed epoch.  The V4 consumer-side evidence that the producer's URA and discontinuity counters reach the user solution.  Off by default; byte-identical when off.
        bool trace_zwd{ false };          ///< Emit @c network_zwd.csv (L7): per present station each emitted epoch, the estimated ZWD (a-priori state plus reconstructed correction) and its datum-conditioned formal σ, plus a carried flag (a ZWD carry prior was injected).  The V7 instrument for border-station formal-σ sharpening under @ref carry_zwd; no other output provides it.  O(stations x epochs); off by default, byte-identical when off.
        /// Emit @c network_sat_bias_states.csv (L14): per emitted (satellite, code signal)
        /// each epoch, the emitted satellite code bias and its σ̂0-scaled formal σ from the
        /// shared posterior covariance.  This fills the one export gap of the truth-graded
        /// fixture: the B product record has no σ field in any configuration, so the
        /// code-bias variance factor (the field "cb vf ≈ 10" question) cannot be obtained
        /// otherwise.  Deliberately a trace rather than a `sigma=` token on the B record:
        /// a wire token would change the product grammar (and any consumer fingerprint
        /// keyed on the header legend).  The covariance gets the same treatment as in the
        /// URA and combined-σ channels (gauge-nulled, datum pins skipped,
        /// positive-variance guarded), so the numbers are comparable.
        /// O(sats x signals x epochs); off by default, byte-identical when off.
        bool trace_sat_bias_states{ false };
        /// Emit @c network_rx_states.csv (L14b): per present station each emitted epoch,
        /// the receiver clock and every receiver code-bias (DCB/IFB) state with its
        /// datum-conditioned formal σ, on the @c network_zwd.csv pattern
        /// (reconstructStation + conditionalLocalVarianceDiag).  The receiver half of
        /// §4.8: under the W6 slice @ref estimate_ifb is forced on, yet those columns
        /// appear nowhere else as estimates with σ (only as census labels carrying
        /// standardized prior residuals), so without this trace the Table-8 receiver
        /// term cannot be graded against an injected truth.
        /// O(stations x (1 + codes) x epochs); off by default, byte-identical when off.
        bool trace_rx_states{ false };
        bool trace_events{ false };       ///< Emit @c network_events.csv (L2): one structured long-format row per discrete QC action (tiers T1 IF-slip … T6 clock-monitor, plus GATE / SIGMA0 / PIVOT / RETRO), in place of scraping spdlog with regular expressions for which satellite, station or arc was acted on and when.  Pivot handover and retro-bump arming are logged nowhere else.  Rows are written even on epochs that are later withheld (they are part of the disposition record).  O(QC events); off by default, byte-identical when off (no new file).
        /// Stamp the S-basis contract (declaration, a-priori orbit/clock product
        /// identity, phase-bias discontinuity threshold) into the product-file header
        /// (v2 "S" line) (B6).  Inert (card OWD): the S line is part of every wire,
        /// because the replay emitter is built from it, and a declaration must not depend
        /// on an output option.  The key is still accepted so that older configs load;
        /// the loader forces this field true (the realized posture that the manifest and
        /// the posture readout report) and logs the key as inert.
        bool serialize_sbasis_contract{ true };
        /// Replay a product file that has no S contract line (card OWD, O1).  Such a
        /// wire (from before card OWD) replays with the default emitter (no
        /// clock-discontinuity channel, the flat default phase-bias threshold), so it
        /// cannot reproduce the producer's declarations; the consumer replay refuses it
        /// with an error naming the cause unless this is set.  It is the switch for
        /// grading a legacy leg on purpose.  Default false.
        bool replay_allow_undeclared_wire{ false };

        // --- Product-contract identity and survey-grade station coordinates (RTA-12) ---
        /// Declared reference frame of the fixed station coordinates, e.g. "IGS20".
        /// Pure declaration (no auto-detection); travels in the S-contract line as an
        /// optional @c frame= field; empty (default) writes nothing (byte-identity).
        std::string frame_id;
        /// Declared coordinate epoch, a decimal year ("2024.5") or an ISO date
        /// ("2024-07-01").  Travels as the optional @c cepoch= S-line field; empty
        /// (default) writes nothing.  When @ref use_sif_coordinates propagated any
        /// roster coordinate and this was left empty, the loader defaults it to the
        /// propagation date (decimal year).
        std::string coord_epoch;
        /// Resolve roster coordinates from the loaded SIF registry
        /// (@c processing.enable_station_info) for stations whose @c position_ecef
        /// was not given in the JSON: the occupation valid at the run date is
        /// velocity-propagated to that date (x(t) = x_ref + v·(t − t_ref) + periodic
        /// + post-seismic; see @c resolveStationCoordinate).  Explicit JSON ECEF
        /// always takes precedence.  Off (default): a missing @c position_ecef remains
        /// a hard load error.
        bool use_sif_coordinates{ false };
        /// Coordinate-age advisory: when @ref coord_epoch is set and parseable and the
        /// run date is more than this many years after it, log a single warning at
        /// roster build (plate motion is ~2-7 cm/yr, so re-propagation is advisable).
        /// 0 (default) = off.  Suggested: 1.0.  Advisory only; automatic
        /// re-propagation or product re-basing would be a larger follow-on (RTF
        /// scale), not this knob.
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
        /// Pin precise (e.g. ultra-rapid SP3) orbits while satellite clocks still
        /// arrive from the live/network stream ("ULT orbits + network clocks").
        /// Default false: the orbit source follows the clock source
        /// (@ref GnssConfig::realtimeUsesLiveOrbitClockSource), byte-identical.  When
        /// true (real-time only), the engine loads and resolves precise SP3 and skips
        /// only the precise CLK; the switchover at the boundary between consecutive
        /// ultra-rapid orbit files is a datum event that producer and consumer must
        /// cross at the same epoch.
        bool        use_precise_orbits{ false };

        /**
         * @brief Optional second RTCM stream carrying SSR corrections.
         *
         * Independent of the primary stream above: the primary feeds rover
         * observations into the engine; this stream feeds RTCM SSR (1057-1068,
         * 1240-1270 and IGS-SSR 4076) into the SsrCorrectionStore.  Both streams may
         * be active concurrently; the realtime loop polls each with a short timeout
         * and dispatches frames to the engine.
         *
         * Disabled by default; with @c enabled=true, @c input_type selects the
         * stream type.
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
             * @brief Per-stream SSR ingest accept policy (@c corrections_accept key
             *        inside this stream's block) (RTA-8(b)).
             *
             * @c "all" (default) pushes every decoded SSR record from this stream into
             * the correction store (the legacy behaviour, byte-identical).
             * @c "orbit_only" pushes orbit and URA records unchanged but drops
             * standalone clock, code-bias, phase-bias and high-rate-clock records, and
             * decomposes combined orbit+clock records (1060/1066-class) into an
             * orbit-only push.  This is the ingest half of the wiring "orbits from the
             * external RTS stream, clocks and biases from the project's network
             * product": without it, the RTS stream's clock content would land in the
             * singleton store and shadow the injected network clock (the store's fresh
             * combined record takes precedence over a standalone clock).  Unknown
             * values warn at load and fall back to @c "all".  Applied per frame in
             * @c RealTimeEngine::feedRtcmFrame via @c RealTimeEngine::ssrPushAction.
             */
            std::string accept{ "all" };
        };
        Corrections corrections;                   ///< Optional SSR corrections stream.

        /**
         * @brief Optional third RTCM stream carrying broadcast ephemeris (nav).
         *
         * For an observation-only station mountpoint (one that streams MSM obs but no
         * broadcast nav, 1019/1020/1042/1044/1046), this supplies the broadcast
         * orbits and clocks that the SSR overlay corrects.  Without it,
         * @c LiveEphemerisStore stays empty for the obs constellations and every
         * satellite is unpositionable, so @c PppModel::computeModels fails for the
         * whole epoch ("No valid PPP observations modeled") and the session yields
         * 0 epochs.  It is typically pointed at a dedicated broadcast-ephemeris caster
         * mountpoint (e.g. @c BCEP00BKG0 on products.igs-ip.net).  Frames route to
         * @c RealTimeEngine::feedRtcmFrame as for the corrections stream, so decoded
         * nav lands in the live store transparently.
         *
         * Independent of @c corrections: a PPP-RTK run off an obs-only station needs
         * all three sources (obs / nav / SSR).  Disabled by default; a station that
         * bundles its own nav (e.g. BRUX00BEL0) needs only the primary and corrections
         * streams.  Reuses the @c Corrections transport descriptor
         * (host/port/mountpoint/file/tcp); the two streams differ only in payload (SSR
         * against broadcast nav), not in transport.
         */
        using Ephemeris = Corrections;
        Ephemeris ephemeris;                       ///< Optional broadcast-ephemeris (nav) stream.

        /**
         * @brief Path to a live (growing) network product file to consume (RTA-6).
         *
         * When non-empty, @c Application::runRealTime owns an @c SsrCorrectionStore
         * and @c NetworkProductTail pair: the tail is polled once per poll-loop
         * iteration, newly completed product epochs are replayed into the owned store,
         * the store is injected into the engine via @c RealTimeEngine::setSsrStore
         * (consume flags from the @c processing.network_consumer profile when
         * enabled), and the engine's obs↔SSR hold buffer is woken via
         * @c notifyProductsAdvanced.  The producer must run
         * @c network.product_flush_per_epoch (and preferably
         * @c network.contract_rebroadcast_s and @c network.serialize_disc_counters for
         * late-join robustness).  Empty (default) = off; the realtime path is
         * byte-identical.
         */
        std::string network_products_path;

        /**
         * @brief RT filter-state snapshot path (@c realtime.state_snapshot_path).
         *
         * The consumer-side warm start, complementing RTB-7: the producer snapshot
         * deliberately excludes per-station filter state, and the closed-loop / user
         * engine's own KF memory (position, ZWD, per-sat iono, receiver biases,
         * ambiguities and the full covariance) otherwise re-converges for hours after
         * every restart.  Non-empty arms both halves on the engine: a periodic atomic
         * snapshot write (every @ref state_snapshot_interval_s of data time), and a
         * staged warm resume at start-up, applied at the first processed epoch when
         * the fingerprint matches and the age is within @ref state_snapshot_horizon_s,
         * and otherwise discarded with a logged reason and a clean cold start.  The
         * closed-loop consumer inherits this via @c deriveClosedLoopConsumerConfig
         * (full config copy), so setting it in a network run's config arms the
         * held-out consumer, not the producer.  Empty (default) = off, byte-identical.
         * See @c src/core/realtime/RealTimeEngineStateSnap.cpp.
         */
        std::string state_snapshot_path;

        /**
         * @brief RT snapshot write cadence in data seconds
         *        (@c realtime.state_snapshot_interval_s, default 10).
         */
        double state_snapshot_interval_s{ 10.0 };

        /**
         * @brief RT warm-resume staleness horizon in seconds
         *        (@c realtime.state_snapshot_horizon_s, default 300).
         *
         * Beyond it the staged snapshot is discarded: every restored arc would be
         * stale, the exposure being undetected cycle slips within the gap, and the
         * engine's own LLI / slip QC / SSR-break reactions are the wired response only
         * for bounded gaps.  The first post-resume predict inflates over the true gap
         * (the previous-epoch time is restored to the snapshot epoch), so within the
         * horizon the covariance reflects the outage.
         */
        double state_snapshot_horizon_s{ 300.0 };

        /**
         * @brief Warm-resume seam response: refloat the restored stiff time-linked
         *        states (@c realtime.state_snapshot_seam_refloat, default true).
         *
         * The response that standing invariant #1 requires, shown to be necessary on
         * the sim closed-loop ladder in two steps.  A producer that itself
         * warm-resumed emits a deliberately unsignalled product transient (generation
         * id preserved, no disc bumps; that continuity is what protects cold
         * consumers), so none of the restored datum-break baselines fire on it.  The
         * restored state then resists the transient wherever it is stiff:
         *
         *  - The restored ZWD (troposphere q ~1e-9) pins the pre-outage
         *    clock/tropo/vertical partition.  A cold consumer parks the transient's
         *    elevation-common part in ZWD (measured: an excursion from 0.16 to 0.64 m
         *    off the converged 0.10 m before relaxing back); the warm consumer's
         *    pinned ZWD sends the same tension into the vertical at mapping-amplified
         *    scale, a monotone Up walk to ~-9 m over the first minutes (h6 rms 7.44
         *    against 1.07 cold), recovering only as the producer's own transient
         *    decays.
         *  - Restored stiff carrier ambiguities do the same on engines that carry them
         *    across epochs (this consumer re-seeds per epoch, so the ambiguity half
         *    measured inert here; it is kept for the lineages where it is not).
         *
         * On (default): every restored ambiguity is marked pending-reseed through the
         * same @c StateManager::resetAmbiguity path the disc-counter and slip
         * reactions use, and the restored ZWD variance is re-inflated to
         * @c initial_std.troposphere with its cross-covariances zeroed.  The value
         * stays as the (correct) seed, so the transient can park in ZWD as on a cold
         * start while position starts warm.  Per-satellite iono is left alone (its own
         * q relaxes it within an epoch, and its value re-anchors the geometry-free
         * split).  Off keeps everything stiff, which suits only a producer proven to be
         * emission-continuous across the outage.
         */
        bool state_snapshot_seam_refloat{ true };

        /**
         * @brief What the warm resume restores
         *        (@c realtime.state_snapshot_restore_scope, default "core").
         *
         * "core": restore the core block only (position / receiver clock / ZWD as
         * seed / ISBs, plus the producer-continuity baselines: generation id, disc
         * counters, A5 applied maps, epoch counter and previous-epoch time).  Dynamic
         * states (ambiguities, per-sat iono, receiver DCBs) are not restored; they are
         * re-created per epoch as on a cold start.  Measured on the sim ladder: across
         * a producer that also warm-resumed, every restored dynamic state resists the
         * producer's deliberately unsignalled resume transient (stiff ambiguities
         * walked Up to −9 m, h6 rms 7.44 against 1.07 cold, and freeing them together
         * with the ZWD only moved the tension into the freed states: ZWD rose to
         * 2.9 m, h6 10.7), while the core memory is the part that starts warm
         * (epoch-0 3D 1.7 m restored against 4.5 cold).
         *
         * "full": also rebuild and restore every dynamic state (exact-layout replay
         * and covariance).  The right scope when the product stream is continuous
         * across the outage, i.e. a consumer-only restart against a producer that
         * kept running (the RT service-bounce case this knob was built for); the sim
         * co-restart venue is its measured worst case.  The seam-refloat policy
         * applies in both scopes.
         */
        std::string state_snapshot_restore_scope{ "core" };

        /**
         * @brief Blunder gate on the RTCM 1005/1006 a-priori seed (m) (RTA-12(3)).
         *
         * The engine seeds the filter a-priori from a decoded station ARP when no
         * configured reference position pins it.  With this gate armed (> 0) and a
         * plausible a-priori already present (a RINEX-header / SBF-PVT / earlier-ARP
         * seed), a decoded ARP that disagrees with it by more than this many metres is
         * rejected with a warning carrying both coordinates and the distance; a
         * misconfigured caster (wrong mountpoint, stale 1005 coordinates) could
         * otherwise move the a-priori across the country.  A configured reference
         * position is never overwritten; with the gate armed it is also cross-checked
         * against the broadcast ARP (warning on disagreement).  0 (default) = off: the
         * legacy seed-always behaviour, byte-identical.  Suggested: 10.0.
         */
        double arp_blunder_gate_m{ 0.0 };

        /**
         * @brief GPS-UTC leap-second count for the RT wall-clock → GPS conversion
         *        (@c realtime.leap_seconds) (RTB-1(c)).
         *
         * Default 18 = the compiled @c TimeUtils::LEAP_SECONDS (valid from
         * 2017-01-01), so an absent key changes nothing.  Applied via
         * @c TimeUtils::setLeapSecondsOverride at @c RealTimeEngine::initialize; it
         * feeds @c TimeUtils::nowContinuousGpsSeconds, the host-clock anchor of the
         * RTCM decode paths (cold-start week promotion, GLONASS day resolution,
         * obs-lag measurement), so a future leap-second insertion can be handled by
         * config without a rebuild.  Deriving the count from the broadcast nav message
         * (LNAV ΔtLS) is a larger follow-on (RTF scale).
         */
        int leap_seconds{ 18 };

        /**
         * @brief Derive the LLI loss-of-lock bit (0x1) from MSM lock times
         *        (@c realtime.msm_lli_from_locktime) (RTB-3).
         *
         * RTCM MSM carries per-(sat, signal) lock times, not RINEX LLI flags; only the
         * half-cycle 0x2 bit can be set statelessly at decode.  With this armed,
         * @c RealTimeEngine runs a @c LockTimeTracker over the decoded lock times at
         * epoch assembly and ORs 0x1 into the assembled observation's LLI when the lock
         * time decreased (RTKLIB @c lossoflock rule; see LockTimeTracker.hpp).  Default
         * false: arming changes slip-detection behaviour on RT replays (the
         * LLI-triggered reset paths start firing), so it must be an explicit choice.
         */
        bool msm_lli_from_locktime{ false };

        /**
         * @brief Honor the SSR orbit reference point in the satellite PCO term
         *        (@c realtime.honor_ssr_reference_point) (RTB-5(c)).
         *
         * Legacy RTCM SSR (1057/1060/1240-series) corrects the broadcast orbit to the
         * satellite antenna phase center; IGS-SSR 4076 and SP3 correct to the center of
         * mass.  @c PppModel applies the satellite PCO unconditionally, which is
         * correct for CoM baselines but counts the PCO z-offset twice (0.6-2.6 m along
         * the LOS) when an APC-referenced legacy-SSR orbit is in effect.  When armed,
         * the satellite-PCO range term is skipped for exactly those satellites whose
         * applied orbit correction this epoch is tagged @c SsrReferencePoint::APC
         * (recorded per satellite by @c Geometry at the orbit-overlay apply sites).
         * Default false: the legacy behaviour, byte-identical, including for users who
         * knowingly run legacy SSR with the double count.
         */
        bool honor_ssr_reference_point{ false };
    };

    /**
     * @brief Network-consumer profile for a standalone user.
     *
     * The closed-loop harness inside @c Application::runNetwork derives its consumer
     * config from the producer config (@c GnssConfig::deriveClosedLoopConsumerConfig).
     * A standalone real-time consumer, a separate process tailing a live product file
     * (@c realtime.network_products_path), has no producer config to derive from, so
     * this profile block (@c processing.network_consumer) declares the same intent
     * explicitly, and @c GnssConfig::applyNetworkConsumerProfile applies the same
     * couplings through the shared @c GnssConfig::applyNetworkConsumerCouplings helper
     * (one source of truth with the closed-loop derivation).  Default
     * @c enable=false leaves every path byte-identical.
     *
     * RTA-9(a): when @c bias is on and the config does not set
     * @c processing.enable_ssr_bias_max_age explicitly,
     * @c applyNetworkConsumerProfile arms the bias staleness gate (an explicit
     * @c false is respected); consuming network biases without an age gate would
     * apply arbitrarily stale biases across producer withhold gaps without notice.
     *
     * RTA-9(c), cadence sizing: the consumer's staleness gates work only if the
     * producer emit cadence is much shorter than the acceptance window, so
     * @c processing.ssr_max_age_s must satisfy emit cadence ≪ ssr_max_age/2 (e.g. a
     * 5 s product cadence fits the 30 s default comfortably; a 30 s cadence needs
     * ssr_max_age ≥ 60 s, or every other epoch drops to broadcast grade).
     * @c processing.ssr_clock_stale_hold_factor adds margin for producer withhold
     * gaps (RAIM/QC epochs that emit nothing): held clocks stay applied with
     * inflated R instead of dropping abruptly to broadcast grade.
     *
     * Age skew, the next rung of the same ladder: beyond each product's own age, the
     * orbit and clock products must agree with each other.  A brdc_rts clock echoes
     * the RTS orbit it was estimated against; @c orbit_clock_max_skew_s (below)
     * withholds the clock when the applied orbit disagrees with that echo.  The
     * withheld satellite then falls to the ordinary staleness rungs above
     * (stale-held extrapolation, then broadcast-grade drop), and the per-satellite
     * mismatch counters (store side, summarized by @c RealTimeEngine beside the
     * RTA-13 counters) make the incoherence visible either way.
     */
    struct NetworkConsumerConfig {
        bool enable{ false };        ///< Master switch.  Only takes effect when a product source exists (realtime.network_products_path set).
        bool clock{ true };          ///< Consume the network clock (setSsrStore consume_clock).  false = keep precise SP3/CLK, consume only the bias products.
        bool bias{ true };           ///< Consume the network code/phase bias (setSsrStore consume_bias).
        bool undifferenced{ true };  ///< Consumer user model: true = undifferenced uncombined PPP-RTK user (direct per-band LAMBDA, no BSSD); false = legacy BSSD consumer.
        /**
         * @brief Where the SSR orbit overlay reads from
         *        (@c processing.network_consumer.orbit_source) (RTA-8(a)).
         *
         * @c "external" (default): the orbit overlay stays on the process-wide
         * singleton store, fed by the external RTS corrections stream (which should
         * be ingest-filtered with @c corrections_accept="orbit_only", RTA-8(b), so
         * that its clock content cannot shadow the network clock), while clocks and
         * biases come from the injected network-product store.  @c "network": the
         * orbit overlay reads the injected store too, for a network product that
         * carries its own orbit corrections.  Unknown values warn at load and fall
         * back to @c "external".  Maps to @c RealTimeEngine::SsrOrbitSource at the
         * @c setSsrStore call site in @c Application::runRealTime; ignored when the
         * profile is off (the legacy coupled routing is used).
         */
        std::string orbit_source{ "external" };
        /// Optional explicit override for @c RealTimeEngine::setArIgnoreMwWl
        /// (tri-state: -1 = absent, derive as the closed-loop driver does,
        /// @c !getArWlOnly() after the couplings; 0/1 = forced).  The MW detector's
        /// WL integers are in the .BIA frame; feeding them to AR while the float
        /// carries network phase biases injects a conflicting WL datum, so the
        /// derived value is true for the undifferenced consumer.
        int  ar_ignore_mw_wl{ -1 };
        /**
         * @brief Orbit/clock coherence age-skew budget (s)
         *        (@c processing.network_consumer.orbit_clock_max_skew_s) (card 7).
         *
         * A @c brdc_rts network clock record echoes the RTS orbit update it was
         * estimated against (IOD-SSR and content epoch); broadcast radial-orbit error
         * is absorbed 1:1 into that clock, so consuming it against a different orbit
         * update produces a dm–m range bias that residual QC cannot see.  With this
         * budget set > 0, the consumer withholds the clock for a satellite whose
         * applied orbit disagrees with the echo (different IOD-SSR, no applied orbit
         * at all, or a content epoch skewed by more than the budget); the RTA-9
         * staleness ladder then governs (stale-hold rung, then broadcast-grade drop),
         * as for a missing clock.  Default 0: the epoch-skew comparison and the
         * withhold are off; IOD-SSR identity mismatches are still counted
         * (per-satellite counters on the store, RTA-13 count-only convention).  Armed
         * on the injected/owned product store and the singleton at the
         * @c setCodeBiasBandFallback sites.  It should match the producer's RTS orbit
         * update interval (one generation ≈ 5–30 s).
         */
        double orbit_clock_max_skew_s{ 0.0 };
        std::string expect_reference_receiver;  ///< Optional S-contract expectation: producer reference receiver id ("" = don't pin).
        std::string expect_apriori_product_id;  ///< Optional S-contract expectation: a-priori orbit/clock product identity ("" = don't pin).
        std::string expect_frame_id;            ///< Optional S-contract expectation (RTA-12): reference frame id, e.g. "IGS20" ("" = don't pin).
        std::string expect_atx_id;              ///< Optional S-contract expectation (RTA-12): producer ATX basename, e.g. "igs20.atx" ("" = don't pin).
        std::string expect_orbit_ref;           ///< Optional S-contract expectation (RTA-12): orbit reference point, "CoM" or "APC" ("" = don't pin).
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
        PPP_RTK_Network, ///< Minimum-constraint S-system network producer: joint multi-station estimator
                         ///< emitting satellite clock + code/phase bias (+ atmosphere) under a declared S-basis.
        PPP_RTK_User     ///< PPP-RTK consumer of network PPP_RTK_Network products (post-processing closed-loop
                         ///< counterpart): uncombined PPP-RTK + network SSR bias/atmosphere consume + S-basis cross-check.
    };

    /**
     * @brief Frequency bands the Kalman filter estimates.
     *
     * Formerly @c SignalMode.  "Frequency" names what this enum controls (band
     * scope), leaving "signal" to mean the full (band, tracking-attribute) tuple.
     * Combined with @c ChannelMode it expresses the full KF active-signal set.
     */
    enum class FrequencyMode {
        Dual,   ///< Two bands per constellation (e.g. GPS L1+L2).
        Triple, ///< Three bands per constellation (e.g. GPS L1+L2+L5).
        All,    ///< Every band a constellation publishes.
        Independent ///< Like @c All, but drops a constellation's E5/B2 *sidebands*
                    ///< (Galileo E5a+E5b, BeiDou B2a+B2b, RINEX bands 5 and 7) for
                    ///< any satellite that also reports the coherent **wideband**
                    ///< (RINEX band 8: Galileo E5 alt-BOC, BeiDou B2(a+b)).  The
                    ///< wideband is the joint coherent processing of those same
                    ///< sidebands, so feeding all three to the diagonal-R filter
                    ///< counts the band twice as if independent, inflating its
                    ///< weight and letting one signal's bias drag the shared
                    ///< (iono/clock/tropo/position) states.  Pruning keeps only
                    ///< the independent information.  Decided per satellite from
                    ///< the observed signals: when the wideband is absent the
                    ///< sidebands are retained (fallback; no band is lost).
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
     * @brief How a station's canonical tracking attribute is chosen per band (card CSL).
     *
     * @c ChannelMode::Single keeps one tracking attribute per band and observation
     * type; this enum selects the rule that decides which one, at the station's first
     * processed epoch, in @c PppModel::decideConstellationCanonicals.
     *
     *  - @c Count : the signal observed on the most satellites of that epoch, ties
     *    going to the earlier entry of the priority list.  The original rule and the
     *    default.  One satellite missing the semi-codeless pair at that epoch is
     *    enough to latch the civil signal (L1C) for the whole run.
     *  - @c Priority : the first signal of the priority list whose satellite count
     *    reaches @c canonical_signal_min_coverage times the largest count of that
     *    band and observation type; the code then follows the phase's tracking
     *    attribute when that code exists at the station under the same coverage
     *    condition (so a station that latches L1W takes C1W).
     *
     * Selected by @c processing.canonical_signal_rule.  @c Count is structurally
     * byte-identical to the original producer.
     */
    enum class CanonicalSignalRule {
        Count,   ///< Highest satellite count wins, ties to priority order (default, the original rule).
        Priority ///< Priority-list order wins above a coverage floor; code follows the phase attribute.
    };

    /**
     * @brief What the ledger-driven gap re-entry response does with an arc that the
     *        raw slip tiers did not see this epoch (@c network.ledger_reentry_no_verdict)
     *        (card SID, phase 3).
     *
     * The population of the response is the ledger's admitted lifecycle, which differs
     * from the raw tiers' resumptions: an arc can be raw-Tracking and admitted-Gap at
     * the same epoch (the estimator stopped carrying a signal the receiver kept
     * reporting), and no raw tier then has a verdict for it.
     */
    enum class LedgerReentryNoVerdict {
        Bridge,  ///< the original behaviour: leave it to the PASS-B inject, but count it and write an event.
        Refloat  ///< treat it as unmeasurable, which @c gap_reentry_relevel_unmeasurable then decides.
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
     * combination is KF @c Single + MW @c All: a lean state vector, while
     * every tracked channel feeds the slip cross-checks.
     */
    enum class MwChannelMode {
        Mirror, ///< Match the KF's @c ChannelMode (default).
        Single, ///< Override: canonical channel only.
        All     ///< Override: every tracked channel.
    };

    /**
     * @brief Carrier-phase ambiguity parameterization in the Kalman state.
     *
     * Selects how phase ambiguities are stored in the state vector and, by
     * extension, how the measurement Jacobian is built and how
     * @c AmbiguityResolver / @c MwSlipDetector interact with the float
     * solution.
     *
     *  - @c PerBand : one ambiguity per (satellite, signal/IF combo), the
     *    original layout.  Key format @c "<sat>_<sig>" (e.g. @c "G01_L1C",
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
     * Default is @c PerBand (the original behaviour); selected by
     * @c estimation.state_ambiguity_basis.
     */
    enum class StateAmbiguityBasis {
        PerBand, ///< One ambiguity per (sat, signal): the original layout.
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
     * @brief Tropospheric mapping function that maps zenith delays to
     *        slant delays.
     *
     * @c Vmf3 supplies @c a_h/@c a_w from the GPT3 5° grid (empirical
     * climatology, no operational 6-hourly product chain) combined with
     * the published degree/order-12 spherical-harmonic expansion of
     * @c b/c (Landskron & Boehm 2018).  GPT3 must be loaded: selecting
     * @c Vmf3 populates the GPT3 grid automatically even when
     * @c troposphere_apriori_source is left at @c StandardAtmosphere.
     */
    enum class TroposphereMappingFunction {
        Gmf,   ///< Global Mapping Function (Boehm et al. 2006).  No external grid required.
        Vmf3   ///< VMF3 empirical from GPT3 (Landskron & Boehm 2018).  Requires gpt3_5.grd.
    };

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
     * value that has drifted from the truth, every new innovation
     * shows up as a large residual, the gate excludes the row, and
     * the bias state never gets the chance to update.  Either or
     * both of two signals fire the reset:
     *  - @c min_streak consecutive outlier-rejection epochs on the
     *    same (sat, signal): the "already-stuck" case, where the
     *    bias is wrong enough that nothing ever survives the gate.
     *  - EWMA |mean| of post-fit residuals exceeding @c bias_floor_m
     *    on a warmed-up row: the slow-drift case, where the row
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
        bool        enabled       = true; ///< Master toggle.  Default on: cheap to evaluate, and it guards against a known failure mode.
        std::size_t min_streak    = 5;    ///< Consecutive-rejection epochs that trigger a reset.  0 disables the streak branch.
        double      bias_floor_m  = 3.0;  ///< EWMA |mean| (m) above which a warmed-up row's bias is considered drifted.  0 disables the drift branch.
    };

    /**
     * @brief Configuration for sat-wide state re-inflation after a
     *        cycle-slip / LLI ambiguity reset.
     *
     * In PPP-RTK uncombined mode the per-sat iono state and the
     * per-(sat, signal) sat-bias states can co-converge to a joint
     * partition that absorbs slant iono in a non-unique way: the
     * iono state holds I_true minus some leakage δ/α, and the
     * sat-bias state holds b_true plus δ.  Phase ambiguities anchor
     * the partition while they are alive.  When a sat-wide cycle slip
     * or LLI lock loss resets every phase ambiguity for that
     * satellite, the new ambiguities re-seed against whatever the
     * current iono/sat-bias states hold, which fixes the (possibly
     * wrong) pre-reset partition for the rest of the arc.  The
     * residual on the signals not anchored by phase (typically L5 in
     * a triple-frequency setup) then carries the elevation-dependent
     * component that cannot be modelled, for hundreds of epochs.
     *
     * On a sat-wide ambiguity reset the iono state's diagonal is
     * re-inflated to @c iono_std_m² and every sat-bias state for
     * that satellite has its diagonal re-inflated to
     * @c sat_bias_std_m².  The values are preserved (slant iono and
     * hardware delays do not change because the receiver lost phase
     * lock); only the cross-state covariance is removed, so the
     * filter can re-partition cleanly over the next several epochs
     * of measurements.  Pivots are skipped at the StateManager layer
     * (their frozen (0, 0) row is detected by the diag==0 guard).
     *
     * All settings live under
     * @c estimation.slip_state_reinflation.{enabled,iono_std_m,sat_bias_std_m}.
     */
    struct SlipStateReinflationConfig {
        bool   enabled        = true; ///< Master toggle.  Default on: the partition failure mode is structural to uncombined PPP-RTK with sat-bias estimation.
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
     * broadcast clocks, and then SSR moves each satellite from broadcast grade to
     * cm grade at a different epoch, turning phase on mid-arc and seeding a fresh
     * ambiguity against an already contaminated (iono, bias) partition.  Waiting
     * until corrections are ready avoids the wasted code-only convergence and the
     * staggered partition-fixing transitions.
     *
     * Readiness criterion (real-time only): at least @c min_satellites satellites
     * are broadcast+SSR (cm-grade clock, phase retained; not
     * @c PppObservation::broadcast_only).  By default that quorum is sufficient, and
     * stragglers still on a broadcast-only clock join later as their SSR arrives: a
     * late satellite joining an already converged RTK filter is benign (its fresh
     * ambiguity initialises against a converged state; the harm the gate prevents is
     * the global code-only cold start, not individual late joiners).
     * @c require_all_above_mask instead blocks until every modeled above-mask
     * satellite is broadcast+SSR (no straggler transitions at all, but one lagging
     * satellite delays the start).  Until satisfied, the engine accumulates nav/SSR,
     * keeps refreshing the a-priori, and runs no predict/update.  No-op in
     * post-processing (all products are present at once, so @c computeModels never
     * yields broadcast_only).
     *
     * Config: @c estimation.defer_until_corrections_ready.{enabled,min_satellites,require_all_above_mask,timeout_s}.
     */
    struct CorrectionsReadyGateConfig {
        bool   enabled        = false; ///< Master toggle.  Default off: keeps the incremental cold start for existing configs.
        int    min_satellites = 5;     ///< Global quorum / solvable floor: start once at least this many sats (across all constellations) are cm-grade (broadcast+SSR).
        int    min_satellites_per_system = 4; ///< Per-constellation quorum: each active constellation with >=2 above-mask sats must have at least min(this, its above-mask count) cm-grade before start.  Guarantees multi-constellation phase geometry at start (prevents the GPS-only-phase cold start that drives the ZWD/height degeneracy).  <=0 disables the per-system check (flat global floor only).
        bool   require_all_above_mask = false; ///< If true, also require every above-mask sat to be broadcast+SSR (no stragglers).  Default false = quorum (start at the floors; late sats join as SSR arrives).
        bool   drop_broadcast_grade = false; ///< If true, drop broadcast-grade (broadcast_only) sats from the solution entirely each epoch (not only their phase) once estimation runs, so that metre-grade code never enters a precise SSR PPP-RTK solution and a broadcast-grade sat can never become a BSSD pivot.  Default false = keep broadcast-grade code (broadcast-only float).
        double timeout_s      = 0.0;   ///< If > 0, start in degraded mode after this many seconds without readiness.  0 = wait indefinitely.
    };

    /**
     * @brief Soft a-priori constraint on the zenith wet delay (ZWD) state.
     *
     * Under BSSD the only receiver-side common-mode states are position + ZWD,
     * and the [up, ZWD] subspace is near-collinear (both enter the single-
     * differenced range via monotone elevation functions).  With no anchor, a
     * common-mode start error runs equal-and-opposite into that near-null
     * direction (ZWD and height to ~1000 m) until satellite geometry
     * re-conditions it.  A recurring soft pseudo-observation pulls ZWD toward a
     * climatological value and shrinks the [up, ZWD] covariance correlation each
     * epoch: the covariance-consistent form of a clamp that keeps ZWD from running
     * away (a value-only clamp would fight the filter and never recover the
     * height).  At a loose @c sigma_m it costs the converged solution nothing (the
     * cm-level geometric ZWD signal overrides the prior once geometry is good).
     *
     * Config: @c estimation.zwd_constraint.{enabled,apriori_m,sigma_m}.
     */
    struct ZwdConstraintConfig {
        bool   enabled   = false; ///< Master toggle.  Default off; set true for real-time BSSD PPP-RTK.
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
     *  - @c HatchMean : the legacy behaviour.  The reference is the slow Hatch
     *    (recursive) mean (α = 1/100 at maturity, ~100-epoch memory), and the
     *    spread is the MAD of first time-differences.  Under a smooth MW trend of
     *    slope s per epoch the lag offset settles at ≈ 99·s while the TD1 MAD
     *    stays at the noise floor; the two statistics are mismatched, so mild
     *    drifts (≲ 1 mm/epoch) produce persistent false slips on mature arcs, and
     *    steeper drifts (~7–9 mm/epoch) can even pass the repair fractional gate
     *    and publish a confident wrong integer.
     *
     *  - @c LocalMedian : the reference is the median of a short causal window of
     *    recent accepted MW values (length @c mw_local_window), and the spread is
     *    the MAD of the same window (×√2, so the configured
     *    @c mw_sigma_multiplier keeps its legacy TD1-equivalent scale).
     *    Deviation and spread measure the same quantity, so a trend lifts both and
     *    cancels to first order, while a genuine step still produces a spike in the
     *    deviation.  Causal: no future data, and the current epoch is excluded from
     *    its own reference.  Remaining weakness: a smooth quadratic signal
     *    (curvature) moves the level away from the window median faster than the
     *    window forgets, leaking into the deviation as a soft ramp.
     *
     *  - @c TimeDifference : no level reference at all; the deviation is the
     *    time-difference of the MW series (order selected by @c mw_td_order),
     *    gated by the MAD of the same time-difference history.  Memoryless, hence
     *    immune to trends and curvature: a linear drift contributes a near-constant
     *    offset (first order) or exactly zero (second order) that the matching MAD
     *    absorbs, while a slip still produces a spike of dN·λ_WL for one epoch.
     *    The cost is noise amplification (σ_TD1 = √2·σ_MW, σ_TD2 = √6·σ_MW), so
     *    smaller real slips sit closer to the gate.  Second order additionally
     *    echoes a real slip with opposite sign one epoch later; the echo is
     *    contained by the erase-on-slip arc restart (unlocked arcs) or absorbed
     *    (locked arcs).
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
        TimeDifference  ///< Deviation is the MW time-difference (order per MwTdOrder), TD-MAD spread.
    };

    /**
     * @brief Differencing order for @c MwDetectReference::TimeDifference.
     *
     *  - @c First : dev = mw[k] − mw[k−1].  A linear MW trend of slope s per epoch
     *    appears as a constant ≈ s (millimetres, far below the √2-amplified noise),
     *    a quadratic as a slow ramp that the matching TD1 MAD tracks.  The default
     *    and the recommended starting point.
     *  - @c Second : dev = mw[k] − 2·mw[k−1] + mw[k−2].  Exactly zero for any
     *    linear trend and a small constant for a quadratic, at the cost of a
     *    further √3 noise amplification over first order and a sign-flipped echo of
     *    every real slip one epoch later.  Intended for data where curvature
     *    defeats the first-order statistic.
     *
     * Configured via @c estimation.cycle_slip.mw_td_order
     * ("first" | "second").
     */
    enum class MwTdOrder {
        First,   ///< mw[k] − mw[k−1] (default).
        Second   ///< mw[k] − 2·mw[k−1] + mw[k−2].
    };

    /**
     * @brief Which navigation aiding sources participate in the estimator.
     *
     * Selection tokens consumed by @c AidingSourceRegistry::createEnabled to
     * compose the @c AidingSourceManager source set, the navigation-aiding
     * analogue of @c CycleSlipConfig / @c SlipDetectorRegistry.  Each flag maps to
     * an @c AidingSourceKind; the IMU is the primary (built first).  For
     * backward compatibility, @c imu defaults to the existing @c imu.enabled
     * (@c enableImu()) when @c estimation.aiding.imu is not set explicitly.
     * @c vision is staged (its .cpp is excluded from the build; the registry
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
        bool vision = false;  ///< Visual odometry / landmarks (staged; the registry skips it with a warning).

        /// ZUPT pseudo-measurement std (m/s) per velocity axis: the assumed
        /// uncertainty of the "velocity == 0 while stationary" constraint.
        /// Smaller => stronger pull to zero.  Via estimation.aiding.zupt_sigma_mps.
        double zupt_sigma_mps = 0.05;
    };

    /**
     * @brief Selects which cycle-slip detectors run.
     *
     * Drives the config-driven composition of the @c CycleSlipManager detector
     * set via @c SlipDetectorRegistry::createEnabled.  The Melbourne-Wübbena
     * wide-lane detector is the primary detector and the source of the WL
     * integers consumed by ambiguity resolution and the BSSD pivot machinery,
     * so @c mw defaults on; the auxiliary detectors default off (the original
     * MW-only behaviour) and are opted in per kind.  Kinds whose
     * implementation is still staged (geometry-free @c gf, ionosphere-free
     * phase @c ifp) are accepted for forward compatibility, but the registry
     * skips them with a warning until their .cpp is compiled in.
     *
     * All detector-selection settings live under
     * @c estimation.cycle_slip.{mw,graphic,ewl,gf,if}.
     *
     * @c turbo_edit is not a detector kind but a coordination mode: when set
     * (and the MW detector is enabled), the @c CycleSlipManager / @c RealTimeEngine
     * pair the MW wide-lane integer @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$
     * with the geometry-free phase jump
     * @f$\Delta\Phi_{GF}=\lambda_1\Delta N_1-\lambda_2\Delta N_2@f$ to solve for
     * the per-signal integer slips @f$(\Delta N_1,\Delta N_2)@f$ (Blewitt
     * TurboEdit).  This turns a dual-frequency slip into a targeted,
     * covariance-preserving per-signal repair, which MW alone cannot do (one
     * equation, two unknowns), and covers MW's @f$\Delta N_1=\Delta N_2@f$ blind
     * spot.  It adapts to the data rate: at high rate (≤1 s) the GF jump is
     * essentially iono-free and the full repair machinery engages; at coarser
     * rates (e.g. 30 s RINEX) the uncancelled ionospheric residual inflates the
     * measured GF noise, and every GF-backed decision is gated on that noise
     * against the integer lattice it must discriminate
     * (@c turbo_edit_split_max_sigma).  Large MW-detected slips still split and
     * repair under a quiet ionosphere, and flagged satellites fall back to the
     * conservative reset as activity rises.  A per-epoch cross-satellite IVI gate
     * also suppresses repair when the ionosphere is disturbed, regardless of
     * rate.  See the "Rate adaptivity" note in the @c TurboEditResolver header.
     */
    struct CycleSlipConfig {
        bool mw         = true;   ///< Melbourne-Wübbena wide-lane detector (primary; default on).
        bool graphic    = false;  ///< GRAPHIC code+phase per-signal detector.
        bool ewl        = false;  ///< Extra-wide-lane MW detector (3+ frequency satellites).
        bool gf         = false;  ///< Geometry-free detector (staged; the registry skips it with a warning if set).
        bool ifp        = false;  ///< Ionosphere-free phase detector (staged; the registry skips it with a warning if set).
        bool turbo_edit = false;  ///< MW+GF joint per-signal slip repair (TurboEdit); rate-adaptive. See struct doc.
        /// MW-only code-domain veto (effective only with @c turbo_edit): an MW flag
        /// whose geometry-free phase is affirmatively stable (|TD2| within the arc's
        /// own MAD noise, with the zero gate sharp enough to discriminate the slip
        /// lattice, under quiet iono, a mature arc and no LLI) is classified as a
        /// code-domain artifact (multipath, NLOS, DLL stress), and the mandated phase
        /// ambiguity reset is skipped: no integer pair of plausible size is
        /// consistent with a flat GF, and the KF phase-innovation (residual) gate
        /// backstops the remaining null-line-pair risk.  See the "code-domain veto"
        /// note in the TurboEditResolver header.  Via
        /// @c estimation.cycle_slip.turbo_edit_mw_only_veto.
        bool turbo_edit_mw_only_veto = true;
        /// Gap-spanning GF bridge (effective only with @c turbo_edit): when a GF
        /// arc's chain breaks for at most this many seconds (per-signal dropout or
        /// full receiver gap), the pre-gap GF level is stashed, and the across-gap
        /// GF jump, drift-corrected by the arc's own EWMA GF rate, serves as the
        /// joint-solve evidence at the first post-gap epochs, instead of the
        /// resolver having no evidence until three consecutive epochs rebuild a
        /// TD2.  A λ-quantized jump attributes the re-lock slip per signal
        /// (continuous signal → integer repair; dropped signal → its ambiguity was
        /// pruned and re-seeded on return, so the slip is moot and the mandate is
        /// suppressed); a flat jump classifies the MW flag as a code artifact
        /// (re-acquisition DLL transient).  Bounded by ionospheric coherence:
        /// beyond ~15 s even quiet-iono drift approaches the integer-pair spacing.
        /// 0 disables the bridge (post-gap MW flags are then not adjudicated and
        /// lead to a reset).  Via @c estimation.cycle_slip.turbo_edit_max_gap_s.
        double turbo_edit_max_gap_s = 15.0;
        /// Soft-LLI bridge (effective only with @c turbo_edit and a non-zero
        /// @c turbo_edit_max_gap_s, high rate only): when a receiver-declared loss
        /// of lock (LLI) on an AR-capable dual-frequency satellite would otherwise
        /// trigger an immediate full ambiguity reset, the satellite's phase
        /// ambiguities are held across the (short) LLI gap instead: the GF arc is
        /// kept, the pre-gap values are snapshotted, the held phase measurements are
        /// excluded from the KF update across the MW one-epoch confirmation delay,
        /// and the gap bridge applies a covariance-preserving integer repair at gap
        /// return (@c snapshot + dN·λ) rather than discarding the converged
        /// ambiguity precision and AR continuity.  This assumes that the carrier loop
        /// held phase coherence across the LLI; if it instead re-acquired (arbitrary
        /// integer), the KF phase-innovation gate resets the wrong held repair one
        /// epoch late, the same cost as the re-acquisition reset path.  Beyond
        /// @c turbo_edit_max_gap_s the full reset applies.  Default off (baseline
        /// unchanged).  Via @c estimation.cycle_slip.turbo_edit_bridge_lli.  See
        /// docs/turbo_edit_lli_bridge_design.md.
        bool turbo_edit_bridge_lli = false;
        /// WL-settle deferral cap (s) for the gap-bridge joint solve (effective only
        /// with @c turbo_edit, @c turbo_edit_bridge_lli and a non-zero
        /// @c turbo_edit_max_gap_s): when the across-gap GF jump is solid but the MW
        /// wide-lane evidence is float-only and ambiguous (two or more integer
        /// candidates fit the GF lattice; candidates k and k±2 differ by a null-line
        /// pair only a few mm apart in GF), the adjudication is deferred instead of
        /// guessed.  The soft-LLI holds stay in place (measurements excluded, states
        /// protected from pruning), the pre-gap GF level stays stashed, and in each
        /// subsequent epoch the post-restart MW arc's Hatch mean re-estimates the
        /// across-gap WL step with √n-averaged code noise (the instantaneous float at
        /// re-acquisition is NL-code-biased by up to several cycles).  The joint
        /// solve re-runs once that estimate can be rounded with confidence (the
        /// WL-lock criterion: |frac| + 3·σ_mean < 0.5 cycles, n ≥ 10); at this cap
        /// with no settle, or if the post-gap GF chain flags a new event, the
        /// satellite falls back to the conservative reset.  0 disables deferral (an
        /// ambiguous bridged solve resets immediately).  Via
        /// @c estimation.cycle_slip.turbo_edit_wl_settle_max_s.
        double turbo_edit_wl_settle_max_s = 45.0;
        /// Split-capability gate (effective only with @c turbo_edit): the maximum 1σ
        /// uncertainty, in cycles, of the float per-signal integer that the
        /// TurboEdit joint solve rounds (σ_TD2 / |λ1−λ2|).  This parameter makes the
        /// resolver rate-adaptive: the per-signal split (and the bridged across-gap
        /// attribution) emits integers only when the arc's measured GF noise
        /// satisfies σ_TD2 ≤ this × |λ1−λ2|.  At the 0.2 default a 3.5σ GF error
        /// still lands inside the rounding well of the true integer (the
        /// wrong-integer acceptance distance is 1 − frac_max = 0.7 cycles).
        /// Millimetric 1 s phase noise passes trivially (high-rate behaviour
        /// unchanged); at 30 s the uncancelled iono residual passes only while the
        /// ionosphere is quiet, and MW-flagged satellites otherwise fall back to the
        /// conservative reset.  Raising this admits repairs from noisier GF evidence
        /// (more repairs, more wrong-integer risk); lowering it moves toward
        /// reset-always.  Via @c estimation.cycle_slip.turbo_edit_split_max_sigma.
        double turbo_edit_split_max_sigma = 0.2;

        // --- Per-detector MAD threshold scale factors (kσ gate multipliers) ---
        // Each adaptive slip threshold is `multiplier × max(σ_MAD, floor)`,
        // where σ_MAD is the robust (MAD×1.4826) noise estimate.  Raising the
        // multiplier widens the gate (fewer detections, fewer false resets, but
        // smaller real slips can slip through); lowering it tightens detection.
        // The defaults equal the former hard-coded values; each must be > 0.
        // Configured via @c estimation.cycle_slip.<name>_sigma_multiplier.
        double mw_sigma_multiplier         = 8.0;  ///< MW (Melbourne-Wübbena) wide-lane gate.
        double turbo_edit_sigma_multiplier = 6.0;  ///< TurboEdit GF-only (MW blind-spot) gate.
        double graphic_sigma_multiplier    = 6.0;  ///< GRAPHIC code+phase gate.
        double ewl_sigma_multiplier        = 5.0;  ///< Extra-wide-lane gate.

        // --- MW detection-reference selection (see MwDetectReference doc) ---
        /// Reference the MW slip deviation is measured against.
        /// "hatch_mean" (legacy) | "local_median" (trend-immune level test) |
        /// "time_difference" (jump test immune to trend and curvature).  Via
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
     * co-estimates its own global ionosphere, a sun-fixed spherical-harmonic VTEC
     * field, jointly with the satellite code biases (W6 OSB), so it does not import
     * an external GIM (`.INX`).  The SH coefficients are shared columns in
     * @ref NetworkEstimator (SharedParamKind::VtecSh); the per-(rx,sat) slant-iono
     * states are kept, and the field is an informative (Kaula-regularized) prior on
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
        double mean_vtec_prior_tecu = 15.0;    ///< a₀₀ climatology mean (TECU); the value is data-determined and only loosely anchored.
        double mean_vtec_prior_sigma_tecu = 30.0;  ///< a₀₀ prior σ (TECU), deliberately loose (climatology, not an IONEX value).
        double representation_sigma_tecu = 5.0;    ///< Per-LOS single-layer representativeness σ (VTEC TECU), mapped to slant.  The field is an informative prior on the slant states, not a tight constraint: a tight value aliases small-scale TEC that the field cannot represent into very large low-degree coefficients at global scale.
        bool   sun_fixed = true;               ///< Expand the field in sun-fixed longitude (λ_ipp − λ_mean_sun).
        bool   trace = false;                  ///< Write the per-epoch SH coefficient trace (network_vtec_sh.csv) + the per-epoch health trace (network_vtec_sh_health.csv, T1a).
        double a00_warn_low_tecu  = 2.0;       ///< Physical-band lower bound on the estimated mean VTEC a₀₀ (TECU).  a₀₀ below this fires a per-epoch divergence warning (T1a health probe); the field co-estimation can drive a₀₀ to unphysical values (e.g. −91 TECU) when the SH surface cannot represent the GF code-bias lump structure.
        double a00_warn_high_tecu = 150.0;     ///< Physical-band upper bound on a₀₀ (TECU); above this the divergence warning fires.  Set low ≥ high to silence the warning.
        // ---- Optional second thin shell, dual-layer field (Iono-2 mechanism A) ----
        // 0 = off (single legacy layer, byte-identical).  When > 0, a second SH
        // coefficient set at layer2_height_km joins the shared border, and the pin row
        // sums both layers: ι = Σ_l M_l(el)·tecu·SH_l(IPP_l)·c_l.  The two mapping
        // profiles (450 against 1300 km: 2.55 against 1.74 at el = 10°) plus the IPP
        // displacement give the field an elevation-dependent per-LOS structure that a
        // single shell cannot represent.  The a00 split between the layers is
        // quasi-null from ground-only geometry, so layer 2 carries a tight
        // climatology anchor (mean ± sigma below) while the total mean VTEC stays
        // data-determined through layer 1's loose 15 ± 30; the a00 physical-band
        // warning monitors the layer sum.  Keys: layer 1 keeps "SH|n|m|C"; layer 2
        // uses "SH2|n|m|C".
        int    layer2_max_degree = 0;          ///< Second-shell truncation degree N₂ (0 = off: single layer).
        double layer2_height_km = 1300.0;      ///< Second-shell height H₂ (km), plasmasphere-side.
        double layer2_kaula_scale_tecu = 3.0;  ///< Layer-2 Kaula scale (smoother, smaller upper field).
        double layer2_q_per_degree = 1.0e-2;   ///< Layer-2 per-degree RW q (TECU²/s), 1/n² scaled for n≥1.
        double layer2_mean_vtec_prior_tecu = 3.0;       ///< Layer-2 a₀₀ climatology mean (TECU).
        double layer2_mean_vtec_prior_sigma_tecu = 5.0; ///< Layer-2 a₀₀ prior σ (TECU), deliberately tight (split constraint).
        int    row_decimation_epochs = 0;      ///< Temporal treatment of the quasi-static SH pseudo-obs constraint (T1b; 0/1 = off = the original behaviour).  The sun-fixed field is quasi-static, so re-applying the SH↔slant rows as independent white observations every epoch is a data-incest channel (it collapses the formal σ without new information).  Mechanism: the rows are still applied every epoch (so the satellite-code-bias observability they provide is preserved and no epoch is withheld; pure row-skipping instead makes the observability gate withhold every skipped epoch), but their variance is inflated by N (σ × √N), so each epoch's white row carries only 1/N of the information, matching the information rate of one full look per N epochs.
        double rep_error_tau_s = 0.0;          ///< Correlated per-(station,sat) SH representativeness-error deviation (T5/Iono-2, v2 exact harvest).  0 = off, byte-identical.  When > 0, the SH↔slant pin row `ι − SH·c = 0 + ε(white)` becomes `ι − SH·c = d̂ₖ|ₖ₋₁ + ε(floor² + Pₖ|ₖ₋₁)`, where d is a per-LOS Gauss–Markov deviation (τ = this value, steady-state σ = representation_sigma_tecu, carried in vertical TECU so that level and variance are elevation-stationary).  v2 replaces the v1 scalar filter (falsified as circular) with the exact joint-posterior harvest: d̂ = d_pred + [P/(P+f²)]·(û − d_pred), with û/V̂ the solved posterior mean/variance of the pin functional (one NetworkEstimator::stationRegression per LOS plus the datum-frame shared covariance), and P⁺ = P·(U+f²)/(U+P+f²), with the data-only information U recovered from V̂ (see docs/SPEC_iono2_mechB_v3.md §6-§10).  This is the per-LOS iono temporal accumulation channel that the per-epoch producer otherwise lacks (the NL-AR representation lever; the single-layer surface leaves ~70 % of LOS residuals > 3σ at any degree, per the T4 A/B).  Slip handling: the C3c PASS-A funnel (carry_slipped_ambs, per-signal → per-sat projection) erases the entry (reset rather than repair, cold re-tap at σ_ss).  Supersedes row_decimation_epochs when set.
        bool rep_error_d5_keep = false;        ///< Erase-policy split for the deviation carry's slip response (Iono-2 v3k).  Default false = v2/v3 behaviour (every C3c-funnel entry erases the (station,sat) deviation).  When true, only slips confirmed by a raw detector (MW/LLI/IF-TD firing this epoch: the carry_slipped_raw provenance set captured at PASS-A insertion) erase; D5-only re-taps (the prior-epoch innovation drain, of which 97 % were measured to coincide with no raw observable step, i.e. iono-representation tension; docs/SPEC_slip_qc_hardening.md §8.2 F3 and SPEC_iono2_mechB_v3) keep the accumulated level and re-tap the variance to steady state (shDevD5Retap: var → var_ss, mean held).  Motivation: with every D5 re-tap erasing, the mean deviation lifetime measured ≈ 0.7·τ, so the accumulation channel never matures.  Safety: the weak re-tapped prior lets a genuinely missed small slip be re-estimated away within a few harvests, and the v3 de-mean removes any common mode of the kept levels (arm with rep_error_demean).  Inert without rep_error_tau_s.
        bool rep_error_demean = false;         ///< Once per epoch, remove the information-weighted global mean and the per-station means from the carried deviation priors (serial PASS-B pre-pass, after slip erase and GM aging) (Iono-2 v3).  A common vertical-TECU deviation has the row signature M(el)·tecu_to_m, identical to the a00 column, so the pair is quasi-null, and the harvest → age → re-assert alternation ratchets it (measured: a00 from 25 to 104 TECU in 3 h with mechanism B alone, 207–255 with the dual layer amplifying ~2.2×; docs/SPEC_iono2_mechB_v3.md).  The de-mean enforces the deviation state's contract (per-LOS structure not representable by SH plus receiver lumps): the common component stays with a00 / the receiver DCB, where the data put it each epoch, and the carry never re-asserts it as independent prior information.  Variances unchanged (conservative).  Inert unless rep_error_tau_s > 0.  Default false (standing invariant #6).
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
     * both directions through a single joint pseudo-observation per (station, LOS):
     * (a) the station's LOS feed the field fit; (b) the field constrains the
     * station's per-LOS iono.  A station whose true slant ionosphere is beyond what
     * the single-layer field can represent (e.g. an equatorial-anomaly /
     * scintillation-belt site at solar maximum) injects that misfit through (a) into
     * every other station (the global support functions export it) and through (b)
     * into its own ambiguities and from there into the shared clocks / code / phase
     * biases.  This config relaxes that coupling per station by a scalar factor
     * @c w_s ∈ (0,1] that multiplies the information of the SH iono rows
     * (σ² ×= 1/w_s ⇒ information ×= w_s).  @c w_s → 0 lets the station's per-LOS iono
     * float freely on its own GF code/phase (noisier but still observable, no rank
     * hole; the dual-frequency code bias stays lumped into iono per Odijk 2016),
     * while the station keeps serving the shared clocks, biases, datum graph and
     * position.  It is the targeted alternative to dropping the station (the
     * amputation falsifier): geometry and the datum survive.
     *
     * The single joint pseudo-obs means that one w_s deweights both directions and
     * their cross-coupling uniformly.  Two paths:
     *  - v1 (static): every station in @c stations is deweighted by @c factor.
     *  - v2 (adaptive, @c adaptive): an IGG-style trailing gate engages a station
     *    when its smoothed per-station SH-misfit statistic becomes a robust outlier
     *    against the network's own distribution (median/MAD-standardized z-score),
     *    with hysteresis (@c sigma_hi / @c sigma_lo) and a per-epoch rate
     *    limit so the deweight⇄fit feedback loop cannot oscillate.  The statistic is
     *    relative to the network's robust centre on purpose: the single-layer surface
     *    leaves ~70–90 % of LOS residuals > 3σ at any degree (a chronic, common-mode
     *    background), so an absolute 3σ gate would trigger every station; only the
     *    network-relative excess isolates the genuinely problematic sites.
     *
     * JSON (all under @c network.iono_constraint_deweight_*): @c stations, @c factor,
     * @c adaptive, @c window_epochs, @c sigma_hi, @c sigma_lo, @c floor,
     * @c rate_limit, @c min_epochs.  Empty @c stations with @c adaptive=false makes
     * every w_s 1.0, byte-identical to off (invariant #6).
     */
    struct IonoConstraintDeweightConfig {
        std::vector<std::string> stations;              ///< v1: station long-names whose SH iono coupling is deweighted by @c factor (explicit list always wins over the adaptive gate).
        double factor = 0.0;                            ///< v1 deweight strength w_s for listed stations.  0 = free iono (full decouple; the station's per-LOS iono floats on its own GF code/phase); clamped to (0,1] otherwise.  Default 0.
        bool   adaptive = false;                        ///< v2: enable the IGG-style network-relative auto-trigger (in addition to the static list).  Default off.
        double window_epochs = 60.0;                    ///< v2: half-life (epochs) of the per-station SH-misfit EWMA (forgetting-window robust-stat idiom, cf. NetworkCarryVce).
        double sigma_hi = 3.0;                          ///< v2: engage the deweight when a station's smoothed misfit z-score (median/MAD-standardized across the network) rises above this.
        double sigma_lo = 2.0;                          ///< v2: release when the z-score falls back below this (hysteresis band; must be < @c sigma_hi).
        double floor = 0.0;                             ///< v2: minimum w_s the ramp drives toward at/above @c sigma_hi (0 = full decouple at strong outlier).  Clamped to [0,1).
        double rate_limit = 0.25;                       ///< v2: maximum |Δw_s| per clean epoch (anti-oscillation slew limit).
        int    min_epochs = 30;                         ///< v2: minimum clean EWMA samples on a station before its gate may fire.
    };

    /**
     * @brief Enumeration for measurement stochastic (weighting) models.
     *
     * Selects how observation variances are scaled based on satellite geometry
     * or signal quality indicators.
     */
    enum class StochasticModelType {
        Constant,   ///< Use the a-priori σ² directly, no geometry scaling.
        Elevation,      ///< Classic 1/sin²(el) weighting (default).
        SNR,            ///< SNR-based weighting using the SIGMA-ε model.
        SatelliteBlock  ///< Block/frequency-dependent zenith noise with 1/sin²(el) scaling.
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

        /// Receiver clock (m*m/s).  The default 1e4 allows sqrt(1e4 * 30) ≈ 550 m
        /// per 30 s epoch.  A value of 1e8 (sqrt(1e8 * 30) ≈ 55 km at 30 s) models
        /// the clock as white noise, effectively unconstrained each epoch, which is
        /// standard PPP practice for GNSS receivers without atomic clocks.  The
        /// clock is also initialized by WLS at each epoch.
        double clock{ 1e4 };

        /// Troposphere ZWD (m*m/s). 1e-8 yields ~6 mm/sqrt(hr),
        /// consistent with mid-latitude IGS analysis center defaults
        /// (Boehm et al.). Allows the ZWD to track weather-driven changes
        /// over multi-hour arcs without over-constraining the estimate.
        double troposphere{ 1e-8 };

        /// Ionosphere zenith delay (m*m/s). Only active in PPP-RTK
        /// uncombined mode. Scaled to slant by 1/sin²(el) per satellite.
        /// The default 0.10 is at the upper end of the typical 0.01-0.1 range
        /// for the zenith value.
        /// This is the random-walk driving noise; when @c enable_iono_rate is
        /// on the per-sat iono becomes a constant-velocity [I,Idot] block and
        /// this field is superseded by @c iono_rate_psd (see below).
        double ionosphere{ 0.10 };

        /// Per-satellite slant-iono rate acceleration PSD (m*m/s^3).  Active
        /// only when @c enable_iono_rate is on: the per-sat slant iono is
        /// modelled as a constant-velocity pair [I_s, Idot_s] with transition
        /// [[1,dt],[0,1]] and the coupled CWNA (continuous white-noise
        /// acceleration) block Q[I]=q*dt^3/3, Q[I,Idot]=q*dt^2/2, Q[Idot]=q*dt,
        /// the same template as the receiver clock-bias/clock-rate pair
        /// (@c clock_rate).  The rate carries the largely deterministic slant-iono
        /// ramp (obliquity change + pierce-point motion + diurnal VTEC trend),
        /// leaving only the turbulent second-order part as process noise, so the
        /// iono prior tightens ~1000x relative to the random walk (stronger
        /// AR/position and in-band iono-fault detection).  Obliquity-scaled to
        /// slant by 1/sin^2(el) like the level state.  The default 1e-10 was fit
        /// from real 24 h GPS+Galileo arc data (the zenith acceleration PSD that
        /// reproduces the observed second-difference scatter of 2 mm at zenith to
        /// 8 mm at 12 deg elevation).  Orthogonal to
        /// @c adaptive_iono_process_noise, which inflates this PSD in storms.
        double iono_rate_psd{ 1e-10 };

        /// Carrier phase ambiguity (m*m/s).  Default 1e-12 is a "safety
        /// floor": sub-mm accumulation per day, indistinguishable from
        /// the constant-ambiguity model in physical terms, but enough
        /// to keep the per-(sat, signal) ambiguity covariance from
        /// underflowing toward zero on long arcs.  Variance shrinks with
        /// every measurement update; without a floor, after hours of
        /// clean tracking the joint covariance Cholesky can lose
        /// positive-definiteness and trigger numerical errors.  0.0
        /// recovers the strict constant-ambiguity model, for cases where
        /// the joint covariance is known to tolerate the conditioning.
        double ambiguity{ 1e-12 };

        /// Receiver code bias / inter-frequency bias (m*m/s). In uncombined
        /// PPP-RTK, one code bias per non-reference signal absorbs the
        /// receiver hardware delay difference. Very stable short-term;
        /// 1e-4 allows ~55 mm of drift per 30 s (sqrt(1e-4 * 30)).
        double code_bias{ 1e-4 };

        /// Inter-system time bias (m*m/s). ISBs are very stable short-term;
        /// 1e-7 allows ~1.7 mm of drift per 30 s, sufficient for
        /// GPS-Galileo/GLONASS/BeiDou time offset evolution.
        double system_time_offset{ 1e-7 };

        /// Troposphere horizontal gradient (m*m/s). North and East gradients
        /// use the same spectral density. 5e-12 yields ~0.13 mm/sqrt(hr),
        /// appropriate for slow azimuthal asymmetry evolution.
        double tropo_gradient{ 5e-12 };

        /// Phase clock offset delta (m*m/s). In the "common clock + delta"
        /// parameterization the delta captures the receiver code-phase hardware
        /// bias difference, which is physically stable (nearly constant over
        /// hours). A very small random-walk spectral density keeps the delta
        /// tightly constrained so the common clock absorbs epoch-to-epoch
        /// timing variations while ambiguities remain estimable.
        /// 1e-9 yields ~2 mm/sqrt(hr) drift.
        double phase_clock{ 1e-9 };

        /// White-noise acceleration power spectral density (m*m/s^3).  The
        /// single CWNA (continuous white-noise acceleration) parameter: when
        /// velocity is estimated it drives the coupled position+velocity
        /// process-noise block (Q[pos]=q*dt^3/3, Q[pos,vel]=q*dt^2/2,
        /// Q[vel]=q*dt), so position and velocity noise derive from one
        /// physical quantity with the correct cross-covariance.  Unlike the
        /// other fields it is not a simple q*dt term.  ~1e0-1e2 for
        /// pedestrian/vehicular dynamics, near 0 for a static receiver.
        /// Config key: acceleration_psd_q (legacy alias: velocity_q).
        double acceleration_psd{ 1e2 };

        /// Per-satellite signal bias (m*m/s).  Absorbs uncorrected satellite
        /// hardware delays on signals without BIA product coverage.  Nearly
        /// constant over hours; 1e-10 allows ~0.05 mm of drift per 30 s.
        double sat_bias{ 1e-10 };

        /// Per-satellite signal code bias (m*m/s).  Network producer
        /// (PPP_RTK_Network) only: the estimable satellite code-bias (OSB) state
        /// when @c network.estimate_sat_code_bias is on.  Satellite code hardware
        /// delays are very stable (geodetic-grade DCBs drift ~mm/day), so the
        /// default mirrors @c sat_bias (1e-10, ~0.05 mm per 30 s).  Separate
        /// from @c sat_bias so the network can tune the shared OSB carry-forward
        /// independently of the single-receiver fallback bias.  Config key:
        /// @c sat_code_bias_q.
        double sat_code_bias{ 1e-10 };

        /// Per-satellite signal phase bias (m*m/s).  Network producer
        /// (PPP_RTK_Network) only: the estimable satellite phase-bias state.
        /// Phase hardware delays are even more stable than code, so the default
        /// is two orders below @c sat_bias (~0.005 mm per 30 s).  Strictly
        /// constant (q=0) would over-converge the diagonal P and leave the
        /// satellite column deaf to genuine slow drift, so a tiny RW is used.
        double sat_phase_bias{ 1e-12 };

        /// Per-satellite clock random-walk spectral density (m*m/s) for the
        /// PPP_RTK_Network producer's Δδtˢ states.  A loose default: satellite
        /// clocks drift far faster than biases; the carried-forward prior plus
        /// this RW gives epoch-to-epoch continuity without over-constraining.
        double sat_clock{ 1e-2 };

        /// Receiver clock-rate spectral density (m*m/s^3).  Under the CWNA
        /// model on the (clock-bias, clock-rate) pair, clock-bias picks up
        /// q*dt^3/3, clock-rate picks up q*dt, with q*dt^2/2 cross-covariance.
        /// 1e-4 is appropriate for a typical TCXO; OCXO-grade receivers warrant
        /// a value a few orders of magnitude smaller.  Active only when
        /// @c estimate_velocity is on (Doppler processing).
        double clock_rate{ 1e-4 };

        /// Inter-system clock-rate offset spectral density (m*m/s^3).  The
        /// rate counterpart of @c system_time_offset.  A single shared
        /// oscillator drives all bands, so ISBR is physically near zero,
        /// which the tight default reflects.  Active only when
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
        double ionosphere_rate{ 0.1 };  ///< Initial slant-iono rate standard deviation (m/s): the cold-start Idot_s uncertainty when @c enable_iono_rate is on. Generous; quickly informed by the F-coupling into I_s.
        double ambiguity{ 1e3 };      ///< Initial ambiguity standard deviation (m).
        double code_bias{ 10.0 };         ///< Initial receiver code bias (IFB) standard deviation (m).
        double system_time_offset{ 1e2 }; ///< Initial inter-system time bias standard deviation (m).
        double tropo_gradient{ 1e-3 };     ///< Initial tropo gradient standard deviation (m).
        double phase_clock{ 1e3 };          ///< Initial phase clock offset standard deviation (m).
        double sat_bias{ 10.0 };              ///< Initial per-satellite signal bias standard deviation (m).
        double sat_phase_bias{ 10.0 };        ///< Initial per-satellite signal phase bias standard deviation (m); PPP_RTK_Network only.
        double sat_clock{ 1000.0 };           ///< Initial per-satellite clock standard deviation (m); PPP_RTK_Network only (loose; Δδtˢ against the a-priori product clock).
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
     *    observations get no per-band scaling: chip rate does not
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
     * applied on top.  The result is then scaled with elevation like the
     * standard Elevation model: the variance is divided by sin^(2p)(el),
     * with p the configured code or phase elevation power (1/sin²(el) at
     * the default p = 1.0).
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
        /// ~11-29% noise effect, so the block axis is deliberately the
        /// *narrowest* of the four (block × band × attribute × baseline).
        /// The baseline σ is tuned once via
        /// @c measurement_noise.{code,phase}_std_m, and the per-block
        /// proportions multiply it (consistently between code and phase).
        ///
        /// Overridable from JSON under
        /// @c filter.satellite_block.block_scale_factors as an ordered
        /// array of [substring, scale] pairs; omitting the JSON key
        /// keeps these defaults.
        ///
        /// Keys are substrings matched against the ATX type field;
        /// first hit wins.  Ordering matters: specific blocks must
        /// precede their parents (BLOCKIIIA before BLOCKIII before
        /// BLOCKII; BEIDOU-3 before BEIDOU; GLONASS-M+ before
        /// GLONASS-K before GLONASS).
        /// Defaults tuned for high-quality IGS-grade receivers
        /// (Septentrio PolaRx5/mosaic, Trimble Alloy/NetR9, Javad Delta,
        /// Leica GR50, etc).  The range is compressed to ~1.18×
        /// (best/worst), because on such a receiver the self-noise floor
        /// dominates block-to-block payload differences: chip-rate and
        /// signal-design effects (per band, per attribute) matter more
        /// than which satellite block transmitted the signal.  A 3×
        /// spread, as extrapolated from legacy / early-2000s receivers,
        /// does not reflect production hardware.
        std::vector<std::pair<std::string, double>> block_scale_factors{
            {"BLOCKIIIA",  0.85},  ///< GPS III/IIIA: modern digital payload
            {"BLOCKIIIF",  0.85},  ///< GPS IIIF: same generation as IIIA
            {"BLOCKIII",   0.85},  ///< Fallback for unqualified BLOCK III
            {"BLOCKIIF",   0.90},  ///< GPS IIF: modernized but pre-III
            {"BLOCKIIR-M", 0.95},  ///< IIR-M: modernized civil L2C/M-code
            {"BLOCKIIR",   1.00},  ///< IIR: older but capable
            {"BLOCKII",    1.00},  ///< Legacy reference (mostly retired)
            {"GALILEO",    0.85},  ///< Modern signal design (BOC, pilots); IOV lumped in
            {"BEIDOU-3",   0.85},  ///< BDS-3: modern, BOC signals
            {"BEIDOU",     1.00},  ///< BDS-2: legacy, multipath-prone
            {"GLONASS-K",  0.90},  ///< CDMA-capable, modern
            {"GLONASS-M+", 1.00},  ///< Mid-tier GLONASS-M variants
            {"GLONASS",    1.00},  ///< Legacy FDMA reference
            {"QZSS-2",     0.85},  ///< QZSS Block II: modern signals
            {"QZSS",       0.90}   ///< QZSS fallback
        };

        /// Zenith code noise per block type (meters).  Derived at
        /// config-load time as @c block_scale_factors × @c
        /// measurement_noise.code_std_m.  Direct edits here are
        /// overwritten when the loader runs; this field exists so
        /// that the lookup-time consumer (@c StochasticModel) can
        /// keep its existing two-table interface without needing
        /// live access to the measurement-noise baseline.
        std::vector<std::pair<std::string, double>> block_code_std_m;

        /// Zenith phase noise per block type (meters).  Derived as
        /// @c block_scale_factors × @c measurement_noise.phase_std_m,
        /// preserving the same per-block ratio structure as the code
        /// table.  The per-block phase differentiation therefore
        /// follows the configured baseline phase σ automatically
        /// rather than living in a parallel hand-tuned table.
        std::vector<std::pair<std::string, double>> block_phase_std_m;

        /// Per-observable (code / phase) scale tables.
        ///
        /// Wraps the per-band and per-(system, attribute) scale factors
        /// for a single observable type.  @c SatBlockNoiseParams holds
        /// two instances: @c code (chip-rate-driven defaults: narrow
        /// correlator C/A penalty, high-chip-rate band gains) and
        /// @c phase (carrier-tracking-driven defaults: no chip-rate
        /// effects, gentler per-band gradient, attribute differences
        /// largely flat).  Splitting the axis lets each observable use
        /// physically motivated defaults instead of forcing one table
        /// to compromise between the two regimes.  The chip-rate-driven
        /// 1.8× C/A penalty on GPS @c C, for example, has no analogue on
        /// phase (the carrier is the same physical signal regardless of
        /// the code attached), so phase keeps GPS @c C at 1.0.
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
        /// large penalty is semi-codeless P(Y) (@c W = 1.40): cross-
        /// correlation / Z-tracking has a real SNR loss, so on a good
        /// receiver L1/L2 P(Y) code is actually *noisier* than C/A.
        /// The band axis gives band 2 no chip-rate bonus (L2C is
        /// BPSK(1)), so the W penalty is what makes C2W > C1C.  GLONASS
        /// FDMA @c C/A (@c C = 1.50) is the noisiest code in GNSS: its
        /// 0.511 Mcps half-chip-rate cannot be seen on the GPS-anchored
        /// band axis, so it lives on the (system-specific) attribute.
        /// Combined I+Q channels (@c X = 0.85) are cleaner (coherent
        /// 3 dB carrier-power gain).  Pilot-only channels (@c Q, @c L,
        /// Galileo @c C, BeiDou B1C @c P) are data-free; data-only
        /// channels (@c I, @c S, @c B, @c D) carry a light data-
        /// modulation tracking penalty.
        ObservableScale code{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference, BPSK(1)-class)
                {'2', 1.0},   ///< L2: L2C is BPSK(1) ≈ band 1; P(Y) noise handled by 'W' attr
                {'5', 0.4},   ///< L5 / E5a / B2a: BPSK(10), ~3.2× sharper thermal
                {'6', 0.5},   ///< E6 BPSK(5) / B3I: intermediate chip rate
                {'7', 0.4},   ///< E5b / B2b: BPSK(10), like band 5
                {'8', 0.25},  ///< E5 alt-BOC(15,10) wideband: cleanest code available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                {"default", {
                    {'P', 1.0},   ///< P-code reference (also: B1C pilot; see BeiDou override)
                    {'W', 1.40},  ///< Semi-codeless P(Y): cross-correlation/Z-tracking SNR loss
                    {'Y', 1.0},   ///< Explicit Y-code
                    {'M', 1.0},   ///< Military M-code
                    {'L', 0.9},   ///< L2C(L) pilot: data-free
                    {'S', 1.05},  ///< L2C(M+L) data/mixed: light data-modulation penalty
                    {'X', 0.85},  ///< Combined I+Q: coherently 3 dB stronger
                    {'I', 1.05},  ///< Data-only: light data-modulation penalty
                    {'Q', 0.9},   ///< Pilot-only: data-free
                }},
                // ---- GPS ----
                {"GPS", {
                    {'C', 1.15},  ///< C/A (narrow correlator): small penalty against P(Y)
                    // 'W' (semi-codeless P(Y)) inherits default 'W' = 1.40.
                }},
                // ---- Galileo ----
                // 'C' is the E1C pilot, not the noisy GPS C/A.  E5a/E5b
                // pilots are 'Q'; data channels 'I'; combined 'X'.
                {"Galileo", {
                    {'A', 1.0},   ///< E1A PRS (placeholder; usually masked)
                    {'B', 1.05},  ///< E1B OS data (CBOC): light data penalty
                    {'C', 0.85},  ///< E1C pilot: data-free
                    {'X', 0.85},  ///< E1 B+C / E5 I+Q (incl. alt-BOC): coherent gain
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
                    {'X', 0.85},  ///< Combined I+Q: coherent gain
                    {'D', 1.05},  ///< B1C data
                    {'P', 0.85},  ///< B1C pilot (overrides default P-code reference)
                }},
                // ---- GLONASS ----
                // FDMA legacy signals: 'C' is the 0.511 Mcps C/A (lowest
                // chip rate in GNSS), 'P' the 5.11 Mcps P-code.  The band
                // axis is GPS-anchored and cannot see GLONASS's chip rate,
                // so the chip-rate effect is carried here.  CDMA L3OC
                // (band 3, I/Q/X) falls through to the 'default' table.
                {"GLONASS", {
                    {'C', 1.50},  ///< L1OF/L2OF FDMA C/A: 0.511 Mcps
                    {'P', 0.70},  ///< L1SF/L2SF FDMA P: 5.11 Mcps
                }},
            }
        };

        /// Phase-observable scale tables.  Carrier-tracking-driven.
        ///
        /// Conservative defaults: the phase noise hierarchy on real
        /// data tracks code in *direction* (high-chip-rate bands
        /// cleaner, pilots slightly cleaner) but with *smaller spread*
        /// (chip rate does not enter the PLL noise formula at all; the
        /// observed phase-band differences are signal-strength and
        /// receiver-tracking artefacts, not 10× chip-rate effects).
        /// These are "effective σ in metres" ratios, so they already
        /// fold in the wavelength scaling (a fixed cycle-noise maps to
        /// more metres on a longer-λ carrier) together with C/N0 and
        /// processing gain.  Measured ratios on a recent IGS day show
        /// band 7 ≈ 0.35 and band 8 ≈ 0.33 of band 1; these defaults
        /// (0.45 / 0.40) sit just above the measured floor, leaving a
        /// little headroom for noisier receivers and stronger
        /// ionospheric conditions.  Calibrated data can justify a
        /// tighter JSON override.
        ///
        /// @c freq_band_scale: gentler gradient than code.  Band 2 is
        /// only modestly below band 1: the longer L2 wavelength works
        /// *against* a metre-noise reduction, and semi-codeless P(Y)
        /// degrades the carrier too (handled on the 'W' attribute), so
        /// a value as aggressive as 0.6 is too optimistic; 0.85 is
        /// closer to observed L2 phase.  Bands 5/7/8 are genuinely
        /// cleaner (stronger, wider modern signals overcome the
        /// wavelength penalty).
        ///
        /// @c tracking_attr_scale: flatter than code by system.  The
        /// chip-rate-driven C/A penalty does not apply to phase, so GPS
        /// @c C is 1.0 (not 1.15 like code).  The one non-trivial entry
        /// is @c W = 1.10: semi-codeless tracking degrades the recovered
        /// carrier as well as the code, just far less than the 1.40 code
        /// penalty.  Pilot-versus-data differences are present but small
        /// (5-10%, not 20-30%): phase tracks the carrier regardless of
        /// data wipe-off success.
        ObservableScale phase{
            // freq_band_scale
            {
                {'1', 1.0},   ///< L1 / E1 / B1 (reference)
                {'2', 0.85},  ///< L2: modest gain; longer λ + semi-codeless offset the bonus
                {'5', 0.55},  ///< L5 / E5a / B2a: modern modulation, stronger signal
                {'6', 0.70},  ///< E6 / L6 / B3: mid-tier
                {'7', 0.45},  ///< E5b / B2b: cleaner than band 5 on Galileo FOC
                {'8', 0.40},  ///< E5 alt-BOC wideband: cleanest phase available
            },
            // tracking_attr_scale
            {
                // ---- Cross-system fallback ----
                // Smaller deviations than code: phase carrier-loop noise
                // is less sensitive to the data-modulation distinction
                // than code correlator noise.
                {"default", {
                    {'P', 1.0},  ///< P-code reference
                    {'W', 1.10}, ///< Semi-codeless P(Y): carrier degraded (far less than code's 1.40)
                    {'Y', 1.0},  ///< Explicit Y-code
                    {'M', 1.0},  ///< Military M-code
                    {'L', 0.95}, ///< L2C(L) pilot: light pilot bonus
                    {'S', 1.05}, ///< L2C(M+L) data/mixed: light data penalty
                    {'X', 0.9},  ///< Combined I+Q: coherent power gain (smaller than code)
                    {'I', 1.05}, ///< Data-only: light penalty
                    {'Q', 0.95}, ///< Pilot-only: light pilot bonus
                }},
                // ---- GPS ----
                // No chip-rate-driven C/A penalty on phase: the carrier
                // is the same physical signal regardless of code.  GPS C
                // phase residuals match GPS L (L1C pilot) and Galileo C
                // (E1C pilot) on real data.  'W' inherits default 1.10.
                {"GPS", {
                    {'C', 1.0},  ///< C/A: no narrow-correlator penalty on phase
                }},
                // ---- Galileo ----
                {"Galileo", {
                    {'A', 1.0},
                    {'B', 1.0},
                    {'C', 0.95}, ///< E1C pilot: light bonus
                    {'X', 0.90}, ///< E1 B+C / E5 I+Q (incl. alt-BOC): coherent gain
                    {'Q', 0.95}, ///< E5a/E5b pilot-only: light bonus
                    {'I', 1.05}, ///< E5a/E5b data-only: light penalty
                    {'Z', 0.95}, ///< E6 B+C combined
                }},
                // ---- BeiDou ----
                {"BeiDou", {
                    {'I', 1.0},  ///< B1I/B2I/B3I data
                    {'Q', 0.95}, ///< pilot-only: light bonus
                    {'X', 0.90}, ///< combined I+Q: coherent gain
                    {'D', 1.0},  ///< B1C data
                    {'P', 0.95}, ///< B1C pilot: light bonus
                }},
                // ---- GLONASS ----
                // Phase noise is chip-rate-independent, so FDMA C/A gets
                // only a small C/N0-driven penalty (not the 1.50 code one).
                {"GLONASS", {
                    {'C', 1.05}, ///< L1OF/L2OF FDMA C/A: light penalty
                }},
            }
        };

        /// Fallback code noise when no block match is found (meters).
        /// Derived at config-load time as
        /// @c measurement_noise.code_std_m (i.e.\ scale factor 1.0,
        /// equivalent to the worst legacy block).
        double default_code_std_m{ 0.15 };
        /// Fallback phase noise when no block match is found (meters).
        /// Derived at config-load time as
        /// @c measurement_noise.phase_std_m.
        double default_phase_std_m{ 0.001 };

        /// Whether to route phase observations through the
        /// block × @c phase.freq_band_scale × @c phase.tracking_attr_scale
        /// × elevation pipeline (@c true, default), or use the simpler
        /// @c default_phase_std_m × elevation path (@c false).
        ///
        /// The flat path rests on a PLL-physics argument: the per-block
        /// carrier-loop noise differential sits below the multipath /
        /// atmospheric noise floor, and the chip-rate-driven frequency
        /// scaling does not apply after correlation.  Real-data phase
        /// residuals do show structure across bands and tracking
        /// attributes, just a different structure from code, so the
        /// model keeps the structural shape but uses dedicated @c phase
        /// tables with carrier-tracking-driven defaults (no chip-rate
        /// C/A penalty, smaller band gradient, near-flat attribute
        /// differences).  See the @c ObservableScale documentation for
        /// the per-axis rationale.
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
        /// other than @c SatelliteBlock: @c Constant / @c Elevation /
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

        /// Accelerometer noise density (m/s²/√Hz).  Typical MEMS: 0.01–0.1.
        double accel_noise_density{ 0.05 };
        /// Gyroscope noise density (rad/s/√Hz).  Typical MEMS: 1e-4–1e-3.
        double gyro_noise_density{ 5e-4 };
        /// Accelerometer bias instability (m/s³/√Hz, random-walk spectral density).
        double accel_bias_instability{ 1e-4 };
        /// Gyroscope bias instability (rad/s²/√Hz, random-walk spectral density).
        double gyro_bias_instability{ 1e-6 };

        /// Initial accelerometer bias standard deviation (m/s²).
        double initial_accel_bias_std{ 0.1 };
        /// Initial gyroscope bias standard deviation (rad/s).
        double initial_gyro_bias_std{ 1e-3 };

        /// Initial attitude uncertainty (rad).  Seeds the attitude
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
     * The core correction terms default to \c true, so the full model is
     * active unless individual terms are disabled.  The optional models
     * (Klobuchar and higher-order ionosphere, ocean pole tide, atmospheric
     * loading) and the ATX/bias policy flags default to \c false, as noted
     * on each member.
     */
    struct ModelCorrections {
        // --- Atmosphere ---
        bool enable_troposphere{ true };               ///< Saastamoinen ZHD + GMF dry mapping (slant hydrostatic delay).
        bool enable_klobuchar_ionosphere{ false };     ///< Klobuchar ionospheric delay model (single-frequency fallback only).  IF combinations and DD short-baselines cancel ionosphere geometrically; PPP-RTK uncombined estimates per-satellite slant iono states.  In all three of those configs the Klobuchar model is a no-op, but constructing the @c Ionosphere instance still pays for an extra @c Model::Geometry load and the Klobuchar coefficient parse.  Default off; intended only for single-frequency PPP runs that need an a-priori ionosphere.
        bool enable_higher_order_ionosphere{ false };  ///< Apply second-order (B·cos(θ)·STEC/f³) and third-order (η·STEC²/f⁴) ionospheric delays as a per-signal observation-level correction, evaluated at the GIM-derived IPP using a centered tilted dipole geomagnetic field.  Requires a loaded IONEX GIM as the absolute-STEC source; the engine auto-loads the GIM when this flag is on even if @c estimation.ionosphere_weighting.enabled is false.  No-op when no GIM resolves (warning logged at startup).  Magnitudes: I₂ ~1–2 cm at L1 during high TEC, I₃ ~mm; the IF combination only partially cancels these, so the largest visible benefit is in standard PPP (IF).  For PPP-RTK uncombined and short-baseline DD the higher-order term mostly cancels in the SD / DD differencing, so this flag is closer to a consistency knob than a bias remover.  Default off.

        // --- Relativistic / propagation ---
        bool enable_relativistic_correction{ true };  ///< Periodic relativistic clock correction (-2 r·v / c²).
        bool enable_shapiro_delay{ true };             ///< Shapiro (gravitational path) delay.

        // --- Geophysical site displacements ---
        bool enable_solid_earth_tide{ true };          ///< IERS 2010 solid Earth tide displacement.
        bool enable_ocean_tide_loading{ true };        ///< Ocean tidal loading displacement (BLQ).
        bool enable_pole_tide{ true };                 ///< Pole tide displacement (from ERP): IERS 2010 Sec. 7.1.4 rotational deformation.
        bool enable_ocean_pole_tide{ false };          ///< Ocean pole tide load displacement (IERS 2010 Sec. 7.1.5 / Desai 2002), added on top of @c enable_pole_tide.  Requires the opole load grid (files.opt_grid_path or <data>/static/opole/).  ~0.1-2 mm; default off keeps runs byte-identical.
        bool enable_atmospheric_loading{ false };      ///< Atmospheric pressure loading (S1/S2 thermal-tide site displacement). Requires per-station S1/S2 load coefficients (files.atm_tide_grid_path or <data>/static/apl/). Sun-synchronous, so it does not average out in static daily PPP. Default off keeps runs byte-identical.

        // --- Antenna phase centre ---
        bool enable_satellite_pco{ true };             ///< Satellite antenna phase centre offset.
        bool enable_satellite_pcv{ true };             ///< Satellite antenna phase centre variation.
        bool enable_receiver_pco{ true };              ///< Receiver antenna phase centre offset.
        bool enable_receiver_pcv{ true };              ///< Receiver antenna phase centre variation.
        std::string receiver_antenna_type_override{ "" };  ///< Receiver antenna type when no RINEX header supplies one (live SBF/RTCM). Format "MODEL RADOME", e.g. "LEIAR25         NONE". Empty = use RINEX header (which always wins). Real-time path only.
        bool enable_receiver_cross_system_atx_fallback{ false };  ///< Receiver only: when the receiver ATX entry lacks a non-GPS frequency, substitute the nearest GPS band (E1->G01, E5a->G05, B1C->G01, B2a->G05, ...). Valid because the receiver phase pattern is frequency-dependent on shared hardware. Never applied to the satellite antenna (E01 and G01 are different spacecraft). Opt-in.
        bool enable_atx_frequency_fallback{ true };    ///< When the ATX entry does not have PCO/PCV for the requested frequency (common for L5 / E5* / B2a on older satellites or receiver antennas), look up the nearest available frequency on the same antenna and use its values instead of skipping the correction without notice.  Cross-frequency PCO drift on a single physical antenna is typically <1 cm, better than the few-cm vertical residual that comes from skipping a frequency entirely.  false restores the legacy "skip if missing" behaviour for A/B comparison.
        bool require_atx_coverage{ false };            ///< When true, drop observations whose (sat, signal) lacks complete ATX coverage on both the satellite and receiver entries, i.e., satellite PCO+PCV and receiver PCO+PCV must all resolve to non-empty entries for the row's exact frequency, without @c enable_atx_frequency_fallback substituting a neighbouring frequency.  Default false (legacy behaviour: observations are kept and any missing PCO/PCV is zeroed without notice).  True suits a run with `frequency_mode: "all"` (legacy alias `signal_mode`) in which the ATX file serves as the authoritative whitelist, dropping rows that would otherwise be processed without proper antenna calibration.
        bool require_atx_coverage_for_ar{ false };     ///< When true, AR (WL constraints, LAMBDA NL search) skips (sat, signal) pairs lacking strict ATX coverage even though the underlying observations are still used in the float Kalman update.  Softer counterpart to @c require_atx_coverage: keeps "all"-signal float estimation while restricting integer fixing to the ATX-curated subset.  Default false (no AR-side gating).
        bool restrict_ar_for_substituted_atx{ false }; ///< When true, AR additionally skips (sat, signal) pairs whose ATX correction was computed via the frequency-fallback substitution chain (rather than an exact entry).  Targets specifically the cases where the antenna model is approximate; signals lacking ATX entirely (no substitution applied) are still allowed.  Usable alongside or instead of @c require_atx_coverage_for_ar, depending on which kind of imperfection is to be gated.  Default false.
        bool loosen_amb_q_for_substituted_atx{ false }; ///< When true, ambiguity states whose underlying (sat, signal) had their ATX correction frequency-fallback-substituted use a looser process noise (@c ambiguity_q_substituted_atx) instead of the standard @c process_noise.ambiguity.  Compensates for the small but non-zero modeling error introduced by the substituted PCV/PCO.  Default false.
        double ambiguity_q_substituted_atx{ 1e-12 };   ///< Process noise (m²/s) applied to ambiguity states for substituted-ATX (sat, signal) pairs when @c loosen_amb_q_for_substituted_atx is true.  The PCV/PCO substitution does not inject random noise; it introduces a deterministic, slowly varying bias in the LOS direction, driven by the mismatch between the requested-frequency antenna pattern and the substituted-frequency pattern (e.g. L2 standing in for L5).  That mismatch projects onto the LOS as the satellite sweeps elevation/azimuth across the arc.  Order-of-magnitude estimate: a ~10 mm PCO_Up substitution mismatch combined with an MEO elevation rate of ~1.5e-4 rad/s yields a LOS bias drift of ~1 μm/s at mid-elevation, i.e. Q ≈ (1 μm/s)² · 1 s ≈ 1e-12 m²/s.  This coincides with the standard @c process_noise.ambiguity default, so a well-formed substitution adds negligible Q on top of the baseline ambiguity random walk.  The override therefore serves as an explicit statement that the substituted-ATX bias has been audited and the standard Q is sized to absorb it, not as a noise-injection knob; raise it to ~1e-11 only with direct evidence (e.g. an arc-correlated residual signature on substituted signals) that the standard Q is too tight.  Values above ~1e-10 start moving real estimation strength out of the ambiguity into other states.
        double ambiguity_std_substituted_atx{ 1e3 };   ///< Initial ambiguity standard deviation (m) for newly-seeded ambiguities whose (sat, signal) had their ATX correction frequency-fallback-substituted.  Replaces @c getAmbiguityStd() at addAmbiguity time when @c loosen_amb_q_for_substituted_atx is true.  Default 1e3, the same scale as the standard ambiguity initialization; 5e3 or larger makes the filter especially loose on substituted-ATX seeds, since the substituted PCV can introduce a per-epoch sub-cycle bias that the float ambiguity has to absorb.
        double atx_substitution_phase_std_m{ 0.0 };    ///< Zenith standard deviation (m) of an additive carrier-phase measurement-noise term applied to phase rows whose ATX correction (satellite or receiver side) was frequency-fallback-substituted (@c wasAtxSubstituted true).  Summed in quadrature onto the stochastic-model phase variance as @f$(\sigma/\sin\,\mathrm{el})^2@f$ before any iono-free combination (so the IF coefficients propagate it).  @c 0 disables it; default off, opt-in like the other substituted-ATX knobs.  Complementary to @c ambiguity_q_substituted_atx, not a duplicate: that knob lets the per-(sat, signal) ambiguity random walk absorb the per-satellite, slowly varying LOS bias of a substituted PCO/PCV (which does not bias coordinates).  It cannot absorb the azimuthally symmetric, elevation-dependent receiver-PCV residual, which is common across satellites at a given elevation and therefore aliases into the elevation-structured states (ZWD and station height) like a small tropo mismodel.  Tightening per-signal phase σ (e.g. Galileo E5 alt-BOC at sub-mm) raises the Kalman gain on exactly those rows, so without this term the unmodeled PCV residual leaks into the vertical with high confidence.  This inflation downweights substituted phase to a realistic effective σ while leaving properly calibrated signals at full precision; the elevation amplification is @f$1/\sin^2@f$, matching the phase thermal term, because the PCV residual and its vertical aliasing are both worst at low elevation.  Recommended ~0.003 (3 mm) for a fallback between neighbouring GNSS bands; 0.005–0.010 for a large inter-frequency PCO gap or when an elevation-correlated phase-residual / ZWD signature appears on substituted signals.
        bool apply_phase_shifts{ false };              ///< When true, RINEX phase observations have the receiver-applied @c "SYS / PHASE SHIFT" values subtracted at parse time, recovering the unshifted phase convention.  Default false because most modern BIA products (CODE, CAS, GFZ) are calibrated against the receiver-shifted phases as recorded in RINEX, so leaving the shifts in place is the standard PPP convention.  True only when the bias product is explicitly calibrated for the unshifted convention, or when running without external BIA and using in-house bias estimation that benefits from a uniform shift-free input.

        // --- Carrier-phase ---
        bool enable_phase_windup{ true };              ///< Carrier-phase wind-up correction.

        // --- Product biases ---
        bool enable_code_phase_bias{ true };            ///< Code/phase bias corrections from BIA product files.
        bool bias_coverage_requires_apply{ false };     ///< When true, the "already corrected?" gates in
                                                        ///< @c MeasurementModel (per-(sat,signal) sat-bias absorber
                                                        ///< allocation and the no-BIA code downweight) treat a .BIA
                                                        ///< entry as coverage only while @c enable_code_phase_bias is
                                                        ///< actually applying it.  Default false preserves the legacy
                                                        ///< behaviour, where a loaded-but-unapplied .BIA reports
                                                        ///< coverage it does not provide, suppressing the absorber
                                                        ///< states and the downweight even though the rows carry the
                                                        ///< raw satellite OSB content (the PPP-RTK closed-loop
                                                        ///< consumer defect: network configs force
                                                        ///< @c enable_code_phase_bias off for @c estimate_iono_sh, the
                                                        ///< derived consumer inherits it, and the untreated code rows
                                                        ///< bias the float vertically).  The network producer path
                                                        ///< (@c ProcessingMode::PPP_RTK_Network) is exempt: its code
                                                        ///< rows are handled by network-estimated shared code-bias
                                                        ///< states, so coverage semantics there stay legacy regardless
                                                        ///< of this flag.  Detector- and seed-side uses of
                                                        ///< @c hasBiasEntry (MW/EWL alignment, WLS seeds) are
                                                        ///< intentionally unaffected: they consume product values
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
                                                        ///< false disables the refinement pass entirely, for A/B
                                                        ///< comparison or when investigating artifacts that may be
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
     * optionally observation RINEX data.  Files already downloaded are
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
     * is created and written at runtime.  Defaults are on (except where a
     * member notes otherwise), so disabling output requires an explicit
     * opt-out in the JSON config.
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
         * When @c true, the @c kalman_filter logger emits only the first
         * and last measurement-update pass within each epoch (regardless
         * of whether outliers were found); all intermediate rebuild
         * passes are suppressed.  This bounds the per-pass log volume
         * when tight phase noise and aggressive outlier rejection are in
         * use; the typical reduction is 5-10× on outlier-heavy runs.
         *
         * Pass-1 messages are written immediately at their original
         * level; intermediate passes are buffered into a per-pass slot
         * and discarded; the final pass's slot is replayed when the
         * caller signals end-of-epoch.  No call-site changes are needed:
         * the gate is implemented as a sink wrapper around the existing
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
