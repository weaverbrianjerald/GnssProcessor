#pragma once

/**
 * @file TurboEditResolver.hpp
 * @brief Melbourne-Wübbena + geometry-free joint cycle-slip *repair* resolver
 *        (Blewitt TurboEdit) — shared by the post-processing and real-time paths.
 *
 * @par Motivation
 * The Melbourne-Wübbena (MW) wide-lane combination measures only the wide-lane
 * difference @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$: one equation in two
 * unknowns.  For a satellite tracked on 3+ frequencies @c CycleSlipManager can
 * already cross-check overlapping MW pairs to isolate the culprit signal, but
 * for the common **dual-frequency** case MW alone cannot attribute a slip to a
 * single signal, so the only safe response is a full ambiguity reset (which
 * discards the converged ambiguity precision).  MW is also blind to the
 * @f$\Delta N_1=\Delta N_2@f$ case, where the wide-lane is unchanged.
 *
 * TurboEdit closes both gaps by adding the geometry-free (GF) phase combination
 * as the missing second equation:
 * @f[
 *    \Delta N_{WL}  = \Delta N_1 - \Delta N_2              \quad\text{(MW, integer)}
 * @f]
 * @f[
 *    \Delta\Phi_{GF} = \lambda_1\,\Delta N_1 - \lambda_2\,\Delta N_2
 *                                                          \quad\text{(GF jump, metres)}
 * @f]
 * solved for the per-signal integers
 * @f[
 *    \Delta N_2 = \frac{\Delta\Phi_{GF} - \lambda_1\,\Delta N_{WL}}{\lambda_1-\lambda_2},
 *    \qquad \Delta N_1 = \Delta N_2 + \Delta N_{WL}.
 * @f]
 * @f$\Delta\Phi_{GF}@f$ is estimated by the second-order time difference of the
 * geometry-free phase, @f$GF(t)-2\,GF(t\!-\!1)+GF(t\!-\!2)@f$, which cancels the
 * ionospheric delay up to quadratic order so that, at the slip epoch, it equals
 * the slip jump plus only phase noise.
 *
 * @par Rate adaptivity (evidence-gated GF trust)
 * The 2nd-order time difference cancels the ionospheric delay only up to
 * quadratic order *over the sampling interval*, so the un-cancelled residual
 * grows with the cadence: sub-mm at 1 s, typically mm–cm at 30 s (quiet
 * ionosphere), and beyond the integer-lattice spacing when the ionosphere is
 * active.  That residual is not modeled away — it lands in the arc's TD2 MAD
 * window and honestly inflates every gate derived from it.  GF trust is
 * therefore EVIDENCE-gated rather than rate-gated; each GF-backed decision
 * demands that the measured noise be sharp enough to discriminate the
 * specific integer lattice it adjudicates:
 *  - Per-signal split (repair): the float @f$\Delta N_2@f$ carries
 *    @f$\sigma_{TD2}/|\lambda_1-\lambda_2|@f$ cycles of noise; the joint
 *    solve emits integers only when that is at most
 *    @c turbo_edit_split_max_sigma (default 0.2 cycles), so a 3.5σ GF error
 *    still lands inside the rounding well of the true integer (the
 *    wrong-integer acceptance distance is @f$1-@f$@c kMaxFractional = 0.7
 *    cycles).  An MW-flagged pair failing the gate falls back to the reset
 *    mandate; a GF-only exceedance on a split-incapable arc is IGNORED — an
 *    unattributable iono transient is not a slip mandate, and a genuine
 *    equal slip's @f$|dN\cdot\lambda|\ge 19@f$ cm phase-innovation signature
 *    leaves it to the KF residual backstop.
 *  - Code-event veto (zero affirmation): the affirmative-zero gate
 *    @f$3.5\sigma@f$ is additionally capped at
 *    @c kVetoZeroGateMaxLambdaFrac @f$\times|\lambda_1-\lambda_2|@f$ — a
 *    "the phase did not move" claim wider than half the smallest GF-visible
 *    slip quantum cannot discriminate stillness from a small slip, so the
 *    veto stands down and the MW mandate governs.
 *  - Cross-satellite IVI: unchanged — its threshold was always lattice-scale
 *    (≈@f$|\lambda_1-\lambda_2|@f$), hence rate-free by construction.
 * At 1 s these gates pass at typical phase noise and reproduce the historical
 * high-rate behavior; at 30 s (daily RINEX) they pass under quiet ionosphere
 * — large MW-detected slips are then split and repaired — and stand down
 * honestly as iono activity rises, demoting flagged satellites to the
 * pre-TurboEdit reset.  A one-time advisory log (and
 * @c EpochResolution::rate_ok) still notes intervals beyond
 * @c kHighRateMaxIntervalS.  See @c project_vertical_instability_solidtide /
 * the @c GfSlipDetector banner for the iono-contamination rationale.
 *
 * @par Separation of concerns
 * This class is a *pure resolver*: it owns the per-arc GF jump history and the
 * MW+GF arithmetic, and returns integer per-signal repairs plus a reset-fallback
 * set.  It does **not** mutate filter state and is unaware of BSSD framing — the
 * caller applies the integers using its own state convention (the post-
 * processing @c CycleSlipManager feeds them through the BSSD-aware repair /
 * pivot-handover machinery; @c RealTimeEngine applies a covariance-preserving
 * additive shift for non-pivot satellites and resets otherwise).  The MW
 * wide-lane integers are taken as input (from @c MwSlipDetector) rather than
 * recomputed, so the empirically-tuned MW detection path remains the single
 * source of @f$\Delta N_{WL}@f$.
 *
 * @par GF-only flags require affirmative MW cleanliness
 * For a pair the GF jump flagged but MW did not, @f$\Delta N_{WL}=0@f$ (the
 * equal-slip split) is assumed **only** when the caller supplies affirmative
 * evidence that the pair's MW observable was clean this epoch
 * (@c MwSlipDetector::getEpochCleanPairs).  MW silence alone is not that
 * evidence: a wl-locked arc deliberately absorbs a real slip into its Welford
 * stats without emitting a result (drift demotion fires only epochs later),
 * and an immature arc could not have flagged at all.  Splitting those states
 * under an assumed @f$\Delta N_{WL}=0@f$ aliases unequal slips into confident
 * wrong dual-signal repairs — the @f$\lambda_1/(\lambda_1-\lambda_2)\approx
 * -3.53@f$ factor (GPS L1/L2) lands even @f$\Delta N_{WL}@f$ values well
 * inside the fractional acceptance gate.  Unaffirmed GF-only flags therefore
 * fall back to the reset path.
 *
 * @par MW-only flags with affirmatively stable GF (code-domain veto)
 * The complementary asymmetric case: MW flagged a pair but the GF jump is
 * affirmatively ZERO — |TD2| within the arc's own measured phase noise
 * (@c kCodeEventZeroSigma × MAD, a deliberately tighter bound than the
 * detection gate).  The integer geometry then actively contradicts every
 * physically plausible slip pair: for @f$\Delta N_{WL}=k@f$ the GF jump any
 * integer pair @f$(\Delta N_1,\,\Delta N_1-k)@f$ must produce is
 * @f$\Delta\Phi_{GF}=\lambda_2 k-(\lambda_2-\lambda_1)\Delta N_1@f$, whose
 * zero crossing sits at @f$\Delta N_1\approx 4.53\,k@f$ (GPS L1/L2;
 * @f$\approx 3.95\,k@f$ for the L1/L5 family).  Every pair with
 * @f$|\Delta N_1|\le 3@f$ predicts @f$|\Delta\Phi_{GF}|\ge 8@f$ cm (L1/L2;
 * ≥ 6 cm L1/L5) — orders above phase noise — so a flat GF leaves only
 * null-line pairs of (4,3) / (9,7)-class magnitude: multi-cycle simultaneous
 * slips on both signals, with no loss of lock, under quiet ionosphere.
 * Meanwhile MW = WL phase − NL code, so a narrow-lane CODE excursion
 * (multipath, NLOS transition, DLL re-lock; 1 WL cycle ≈ 0.86 m of NL code
 * on L1/L2) produces exactly this signature and needs nothing exotic.
 *
 * Under trusted conditions — a discrimination-capable zero gate (see the
 * "Rate adaptivity" note: @f$3.5\sigma@f$ at or below
 * @c kVetoZeroGateMaxLambdaFrac @f$\times|\lambda_1-\lambda_2|@f$),
 * IVI-stable ionosphere, mature GF arc, no post-slip cooldown echo, no LLI
 * on either signal — such an event
 * is therefore classified as a code-domain artifact
 * (@c EpochResolution::code_events): both signals are adjudicated clean
 * (reported via @c clean_signals) and the caller must SKIP the mandated
 * phase reset.  The safety net is the Kalman phase innovation: if the
 * event nonetheless was a null-line slip, that signal's innovation jumps
 * by @f$\ge|4\lambda_1|\approx 0.76@f$ m at this very epoch's update —
 * far beyond any standardized residual gate — and the residual-driven
 * reset machinery corrects it one epoch late.  The cost of a wrong veto
 * is one rejected measurement, while a wrongly honored MW mandate
 * destroys converged ambiguity precision (and AR continuity)
 * immediately.  For a BSSD pivot the same backstop exists in SD form: a
 * missed pivot slip disturbs every peer residual coherently and the KF
 * coherent-disturbance handover machinery re-references.  When another
 * pair of the same satellite simultaneously demands a reset or stages a
 * conflicting integer, the satellite-level reset wins and the code event
 * is withdrawn — contradictory evidence is never silently split.
 * Disable via @c estimation.cycle_slip.turbo_edit_mw_only_veto.
 *
 * @par Gap-spanning GF bridge (across-gap attribution)
 * The TD2 chain demands three consecutive epochs, so any interruption — a
 * per-signal dropout or a full receiver gap — re-seeds the chain and makes
 * the post-gap TD2 BLIND to whatever happened across the gap: a re-lock
 * slip's GF level jump lives between the chains and the trace shows
 * "stable" GF on both sides (the artifact that originally masked a
 * λ₂-quantized +0.73 m jump as "GF-invisible").  Meanwhile the MW arc
 * deliberately spans short dropouts, so its across-gap flag arrives with no
 * GF evidence to adjudicate it and the caller blanket-resets both signals —
 * including the one that held lock, and including a dropped signal whose
 * ambiguity the state already pruned-and-reseeded on return.
 *
 * The bridge closes this: when a chain break is detected within
 * @c estimation.cycle_slip.turbo_edit_max_gap_s, the pre-gap GF level,
 * its timestamp, the arc's EWMA GF rate, and the pre-gap epoch spacing are
 * stashed (and the TD2 MAD calibration window is preserved — gap length
 * does not change arc noise).  At the first post-gap epochs (until the
 * chain owns a TD2 again) the pair is evaluated against the drift-corrected
 * ACROSS-GAP jump @f$\Delta GF_{gap} = GF_{now} - GF_{ref} -
 * \dot{GF}\,\Delta t@f$ instead of a TD2:
 *  - affirmatively zero (within the code-event gate widened by a
 *    random-walk factor @f$\sqrt{\Delta t/dt_0}@f$ — the arc's own
 *    per-interval variability extrapolated over the gap) → code-domain
 *    artifact (re-acquisition DLL transient), as in the MW-only veto;
 *  - otherwise the joint solve runs with the across-gap jump.  The WL
 *    integer comes from the MW result, or — because the locked-arc
 *    TD-confirmation path withholds its integer by contract — from the
 *    float estimate @c MwSlipResult::slip_wl_float, trying round(f)±1 and
 *    demanding a UNIQUE candidate inside the fractional gate (the GF
 *    equation alone is degenerate along the null-line lattice: +0.735 m
 *    fits both (0,−3) and (9,4) at mm level; only k separates them).
 *  - the accepted pair is dispatched per-signal by OBSERVATION-DOMAIN
 *    continuity (tracked per signal across epochs): a signal observed at
 *    every epoch through the gap kept its ambiguity (full receiver gaps
 *    prune nothing), so its nonzero dN is a genuine integer repair; a
 *    signal that MISSED an epoch was pruned-and-reseeded on return
 *    (prune-on-first-miss), so its nonzero dN is MOOT — already absorbed
 *    by the fresh seed — and is reported via
 *    @c EpochResolution::moot_signals for mandate suppression with no
 *    state action.  dN = 0 signals are adjudicated clean as usual.
 *  - trust gates mirror the veto (affirmed rate, IVI-stable, no LLI,
 *    mature preserved MAD); an ambiguous or failed solve demotes to the
 *    reset fallback, and an untrusted bridge leaves the mandate to the
 *    caller exactly as before the bridge existed.
 * A bridge is consumed by its first actioned adjudication, expires when
 * the chain regains its own TD2, and dies with the arc on external resets
 * and LLI (a receiver-declared loss of lock voids the level reference).
 *
 * @note The GF combination arithmetic here is intentionally self-contained.  It
 *       mirrors the (currently staged) @c GfSlipDetector and the active
 *       @c IonoVariabilityMonitor; consolidating the three onto one shared
 *       GF-combination helper is a worthwhile future cleanup but is out of scope
 *       for this component.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"   // MwSlipResult (input)
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class TurboEditResolver
 * @brief Resolves dual-frequency cycle slips into per-signal integer repairs by
 *        pairing the MW wide-lane integer with the geometry-free phase jump.
 */
class TurboEditResolver {
public:
    /**
     * @brief A confident per-(satellite, signal) integer slip repair.
     *
     * @c dN is the zero-differenced integer cycle slip in @em this signal's
     * cycles; the caller applies @c dN·λ as an additive, covariance-preserving
     * shift to the corresponding carrier-phase ambiguity (BSSD pivots excepted).
     */
    struct SignalRepair {
        std::string satellite; ///< Satellite identifier (e.g. "G01").
        std::string signal;    ///< Signal identifier (e.g. "L1C").
        int    dN{ 0 };        ///< Integer slip in this signal's cycles (never 0 in emitted repairs).
        double lambda{ 0.0 };  ///< Signal wavelength (m).
        double frac{ 0.0 };    ///< Rounding residual of the float estimate (0 = exact, ≤0.5 worst).
    };

    /**
     * @brief An MW-flagged pair adjudicated as a code-domain artifact.
     *
     * MW flagged the pair but the geometry-free phase was affirmatively
     * stable under trusted conditions (see the file-level "code-domain
     * veto" note): the event lives in the code observable, not the phase.
     * The caller must NOT reset the pair's phase ambiguities — the KF
     * phase-innovation (residual) gate is the contracted backstop for the
     * residual null-line-pair risk.  Both signals also appear in
     * @c EpochResolution::clean_signals.  Never emitted for a satellite
     * that ended the epoch in @c reset_sats (conflicting evidence on
     * another pair withdraws the event).
     */
    struct CodeEvent {
        std::string satellite;   ///< Satellite identifier (e.g. "G01").
        std::string sig1;        ///< Higher-frequency phase signal of the MW pair.
        std::string sig2;        ///< Lower-frequency phase signal of the MW pair.
        int    dN_wl{ 0 };       ///< MW wide-lane integer reported for the flag (0 if unknown).
        bool   wl_known{ true }; ///< False when MW flagged without a usable integer.
        double td2_m{ 0.0 };     ///< Measured GF jump (2nd-order time difference, m).
        double zero_gate_m{ 0.0 }; ///< Affirmative-zero bound used (kCodeEventZeroSigma × arc MAD, m).
    };

    /**
     * @brief Per-epoch resolution result.
     */
    struct EpochResolution {
        std::vector<SignalRepair> repairs;   ///< Confident per-signal integer repairs to apply.
        /// Per-satellite signals a confident joint solve adjudicated as
        /// @c dN==0 (did **not** slip) — the zero-side partners of the
        /// emitted repairs.  Lets the caller distinguish "verified clean"
        /// from "never evaluated" when arbitrating its own reset mandates
        /// against this resolution (a mandate on an unevaluated signal must
        /// still be honored; one on an adjudicated-clean signal must not
        /// destroy that signal's converged precision).
        std::map<std::string, std::set<std::string>> clean_signals;
        /// MW-flagged pairs adjudicated as code-domain artifacts (GF
        /// affirmatively stable) — the caller must SKIP the phase reset
        /// for these pairs.  Disjoint from @c reset_sats by construction.
        std::vector<CodeEvent>    code_events;
        /// Per-satellite signals whose across-gap slip the gap bridge
        /// attributed but adjudicated MOOT: the signal missed at least one
        /// epoch during the bridged gap, so its ambiguity was pruned and
        /// freshly reseeded on return — the slip is already absorbed and a
        /// further reset would only destroy the new seed.  The caller must
        /// SUPPRESS its reset mandates with no state action (same handling
        /// as @c clean_signals / code-event signals).  Disjoint from
        /// @c reset_sats by construction (withdrawn like code events).
        std::map<std::string, std::set<std::string>> moot_signals;
        /// Satellites whose bridged adjudication is DEFERRED pending a
        /// settled across-gap MW wide-lane estimate (the WL-settle
        /// deferral; see @c CycleSlipConfig::turbo_edit_wl_settle_max_s).
        /// The instantaneous re-acquisition float is NL-code-biased by up
        /// to several cycles, and two integer candidates separated by a
        /// null-line pair fit the GF jump at mm level — guessing mints a
        /// confident wrong repair, so the resolver waits for the
        /// post-restart MW Hatch mean to become confidently roundable.
        /// The caller must keep the satellite's soft-LLI holds alive
        /// (measurements excluded, states prune-protected) and SUPPRESS
        /// its MW reset mandates while listed; the deferral always
        /// terminates in a later epoch's repair, reset, or timeout-reset.
        /// Disjoint from @c reset_sats and the repair set by construction.
        std::set<std::string>     pending_sats;
        std::set<std::string>     reset_sats; ///< Satellites flagged but unresolved — caller should reset.
        bool   iono_stable{ true };          ///< False ⇒ GF jumps untrusted this epoch (IVI gate tripped).
        double max_ivi{ 0.0 };               ///< Worst-case cross-satellite IVI this epoch (m).
        /// Advisory only: false ⇒ the sampling interval exceeded the nominal
        /// high-rate regime (@c kHighRateMaxIntervalS).  Decisions are NOT
        /// gated on this — GF trust is evidence-scaled per arc (see the
        /// header's "Rate adaptivity" note).
        bool   rate_ok{ true };
    };

    /**
     * @brief Construct the resolver.
     * @param config      Processing configuration (elevation mask, etc.).
     * @param log_manager Logging facility.
     */
    TurboEditResolver(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Provide satellite BIA corrections.
     *
     * The GF combination is bias-free in its time-difference, so this is a
     * no-op today; retained for interface parity with the detectors and for
     * forward compatibility.
     *
     * @param corrections Bias source (may be nullptr); owned externally.
     */
    void setCorrections(const GnssCorrections* corrections) { corrections_ = corrections; }

    /**
     * @brief Resolve cycle slips for one epoch.
     *
     * Must be called every epoch (it rolls the GF jump arcs forward), even when
     * @p mw_results is empty — otherwise the GF history needed for the next
     * jump estimate is lost.
     *
     * @param mw_results   MW slip results from @c MwSlipDetector::detectLargeMwResets
     *                     for this epoch (the source of @f$\Delta N_{WL}@f$).
     * @param mw_clean_pairs Arc keys ("SAT_SIG1_SIG2") whose MW observable was
     *                     affirmatively clean this epoch, from
     *                     @c MwSlipDetector::getEpochCleanPairs().  Gates the
     *                     GF-only equal-slip split (see the file-level
     *                     "GF-only flags require affirmative MW cleanliness"
     *                     note); pairs absent from this set fall back to
     *                     reset when GF-only flagged.
     * @param observations Modeled observations for this epoch (raw carrier phase, cycles).
     * @param interval_s   Effective sampling interval (s); ≤0 means unknown (the
     *                     rate warning is then suppressed).
     * @param wl_gap_evidence Settling across-gap WL estimates per arc key,
     *                     from @c MwSlipDetector::getAcrossGapWlEvidence —
     *                     consumed by the WL-settle deferral to re-run a
     *                     deferred bridged solve once the post-restart MW
     *                     mean is confidently roundable.  Empty disables
     *                     re-evaluation (pending sats then run to the
     *                     deferral timeout).
     * @return Per-signal integer repairs and the reset-fallback set.
     */
    EpochResolution resolve(const std::vector<MwSlipResult>& mw_results,
                            const std::set<std::string>& mw_clean_pairs,
                            const std::vector<PppObservation>& observations,
                            double interval_s,
                            const std::map<std::string,
                                           MwSlipDetector::AcrossGapWlEvidence>&
                                wl_gap_evidence = {});

    /**
     * @brief Mark a (satellite, signal) as HELD across a receiver LLI gap.
     *
     * Soft-LLI bridge support (see @c GnssConfig::CycleSlipConfig
     * @c turbo_edit_bridge_lli).  When the caller decides to hold a
     * satellite's ambiguity across a short LLI gap rather than reset it, it
     * tells the resolver so that the gap-bridge adjudication treats this
     * signal as CONTINUOUS (a genuine integer repair) instead of MOOT
     * (pruned-and-reseeded) — the moot premise is "the state side reseeded
     * the ambiguity", which is exactly what the hold prevents.  The flag is
     * one-shot: it is consumed when the bridge actions the pair (repair /
     * reset / code event), and it is cleared by any arc reset for the
     * (satellite, signal).
     *
     * @param satellite Satellite identifier (e.g. "G03").
     * @param signal    Signal identifier (e.g. "L2W").
     */
    void markHeldAcrossLli(const std::string& satellite, const std::string& signal);

    /**
     * @brief Clear all GF arc state for a satellite (every signal pair).
     * @param satellite Satellite identifier (e.g. "G01").
     */
    void resetSatellite(const std::string& satellite);

    /**
     * @brief Clear GF arc state for pairs involving a specific (satellite, signal).
     * @param satellite Satellite identifier (e.g. "G01").
     * @param signal    Signal identifier (e.g. "L1C").
     */
    void resetSatelliteSignal(const std::string& satellite, const std::string& signal);

    /**
     * @brief Drop all GF arc state (hard-gap restart).
     */
    void resetAll();

    /**
     * @brief Override the inter-epoch gap tolerance before a GF arc is reset.
     * @param seconds New gap tolerance; values ≤ 0 are ignored.
     */
    void setMaxGapSeconds(double seconds);

    /**
     * @brief Enable CSV tracing of the per-pair joint solve.
     * @param log_dir Directory in which to create @c turbo_edit_trace.csv.
     * @return true if the trace file was opened.
     */
    bool enableTracing(const std::filesystem::path& log_dir);

    /// @brief Whether tracing is currently active.
    bool isTracingEnabled() const { return trace_file_.is_open(); }

private:
    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;
    const GnssCorrections* corrections_{ nullptr };

    /// Effective inter-epoch gap tolerance (s); overridable via @c setMaxGapSeconds.
    double max_gap_seconds_{ kDefaultMaxGapSeconds };

    /// One-shot guard so the coarse-rate warning is logged only once per run.
    bool rate_warned_{ false };

    /// Continuous GPS seconds of the previous epoch that carried observations.
    /// The 2nd-order time difference requires three *consecutive* epochs, so an
    /// arc whose last update predates this (it sat out the previous epoch — a
    /// per-signal dropout) is non-consecutive and must be restarted, regardless
    /// of the absolute @c max_gap_seconds_ tolerance.  0 until the first epoch.
    double prev_epoch_s_{ 0.0 };

    /**
     * @brief A dual-frequency carrier-phase pair, ordered higher-frequency first.
     */
    struct PhasePair {
        std::string phase1;  ///< Higher-frequency phase signal (e.g. "L1C").
        std::string phase2;  ///< Lower-frequency phase signal (e.g. "L2W").
    };

    /**
     * @brief Per-(satellite, phase-pair) geometry-free jump arc.
     */
    struct GfArc {
        double prev_gf{ 0.0 };           ///< GF at epoch t-1 (m).
        double prev_prev_gf{ 0.0 };      ///< GF at epoch t-2 (m).
        double last_epoch_s{ 0.0 };      ///< Continuous GPS seconds of last update.
        std::size_t count{ 0 };          ///< Epochs accumulated in this arc.
        /// Recent TD2 values (m) for the MAD threshold.  CALIBRATION, not
        /// chain state: the TD2 noise level is a slowly varying property of
        /// the arc (elevation / tracking / multipath), so it SURVIVES both
        /// the short-gap partial reset (gap ≤ @c max_gap_seconds_) AND the
        /// slip reset — a level jump does not change the noise, and the
        /// window only ever held clean (non-exceeding) TD2s.  Only an
        /// absolute-gap reset or an external (KF-driven) arc erase clears it.
        std::deque<double> diff_window;
        std::size_t cooldown_remaining{ 0 }; ///< Anti-echo cooldown after a slip (epochs).

        // --- Gap bridge (see the file-level "gap-spanning GF bridge" note) --
        /// EWMA of the GF rate (m/s) over consecutive epochs — the
        /// drift-correction source for the across-gap jump.  NaN until two
        /// consecutive epochs exist; survives chain re-seeds only via the
        /// stash below (a fresh chain re-learns its own rate).
        double rate_ewma_mps{ std::numeric_limits<double>::quiet_NaN() };
        /// Epoch spacing (s) of the arc's last consecutive update — the
        /// random-walk reference dt₀ for the bridge gate widening.
        double last_spacing_s{ 0.0 };
        /// True while a stashed pre-gap level is available for bridging.
        bool   bridge_valid{ false };
        double bridge_gf{ 0.0 };        ///< Last pre-gap GF level (m).
        double bridge_epoch_s{ 0.0 };   ///< Its continuous GPS seconds.
        double bridge_rate_mps{ std::numeric_limits<double>::quiet_NaN() }; ///< Pre-gap EWMA rate (m/s).
        double bridge_dt0_s{ 0.0 };     ///< Pre-gap epoch spacing (s) for the √(Δt/dt₀) widening.
        /// WL-settle deferral: true while the bridged solve for this pair
        /// is deferred pending a settled across-gap MW estimate (the float
        /// at re-acquisition was code-biased and the candidate set
        /// ambiguous).  Keeps the bridge stash alive past the chain
        /// rebuild; resolved by a later repair / reset / timeout.
        bool   wl_settle_pending{ false };
        double wl_settle_start_s{ 0.0 };  ///< Deferral start (continuous GPS s).
    };

    /// Per-(satellite + phase-pair) GF jump arcs.  Key: "G01_L1C_L2W".
    std::map<std::string, GfArc> gf_arcs_;

    /// Cross-satellite TD2 σ per (constellation × band-pair) group, keyed
    /// "G_L1_L2" (tracking attribute collapsed, mirroring the MW detector's
    /// cross-sat pool).  EWMA-smoothed (@c kXsatEwmaAlpha), zenith-referenced
    /// (the pool normalizes each TD2 by 1/sin(el); @c xsatSigmaFloor
    /// re-expands on use).  Consumed ONLY while an arc's own MAD window is
    /// immature — warmup-only, exactly the MW contract — so fresh arcs (true
    /// cold start, post-gap, post-slip) get a statistically grounded gate
    /// from their FIRST TD2 instead of a NaN.  One-epoch lag by construction
    /// (finalized after all pairs are processed).  On quiet data k×σ_xsat
    /// sits under @c kMinThresholdM and the quantum floor still governs —
    /// the pool's real work is lifting fresh-arc gates during iono activity
    /// or on noisy receivers, where the bare floor would false-flag → reset.
    std::map<std::string, double> xsat_sigma_group_;
    /// Constellation-tier fallback for sparse band-pair groups ("G").
    std::map<std::string, double> xsat_sigma_cons_;
    /// Global (non-GLONASS) tier-3 fallback, mirroring the MW cascade: a
    /// constellation contributing fewer than @c kMinXsatSamples poolable
    /// TD2s per epoch (1-2 visible sats) never populates its own tiers and
    /// would otherwise sit at the bare quantum floor for the whole run.
    /// GLONASS is excluded (FDMA inter-channel noise is least comparable),
    /// as are narrow-spacing intra-E5/B2-family pairs (their sub-mm TD2
    /// noise would drag the coarse tiers an order of magnitude low).
    double xsat_sigma_global_{ std::numeric_limits<double>::quiet_NaN() };
    /// Per-epoch staging pool (zenith-normalized TD2 of non-exceeding pairs),
    /// finalized into the tiers at the end of each resolve() and cleared.
    std::map<std::string, std::vector<double>> xsat_devs_;

    // --- Tuning constants ------------------------------------------------

    /// Rolling window length for the MAD-based GF-only detection threshold.
    static constexpr std::size_t kGfWindowSize = 30;
    /// Minimum TD2 samples before the adaptive MAD threshold is trusted.
    static constexpr std::size_t kMinEpochsForAdaptive = 5;
    /// kσ multiplier for the GF-only (blind-spot) detection threshold, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_sigma_multiplier (default 6.0).
    double sigma_multiplier_;
    /// Split-capability gate: maximum 1σ uncertainty of the float per-signal
    /// integer, in CYCLES of the @f$\lambda_1-\lambda_2@f$ lattice, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_split_max_sigma
    /// (default 0.2).  The joint solve emits integers only when
    /// @f$\sigma_{TD2}\le@f$ this @f$\times|\lambda_1-\lambda_2|@f$ — the
    /// mechanism that makes the resolver rate-adaptive (see the header's
    /// "Rate adaptivity" note): millimetric 1 s noise passes trivially,
    /// coarse-rate iono residual passes only while quiet.
    double split_max_sigma_;
    /// MW-only code-domain veto enable, from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_mw_only_veto (default true).
    /// See the file-level "code-domain veto" note.
    bool mw_only_veto_{ true };
    /// Gap-bridge cap (s), from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_max_gap_s (default 15;
    /// 0 disables).  See the file-level "gap-spanning GF bridge" note.
    double bridge_max_gap_s_{ 15.0 };
    /// WL-settle deferral cap (s), from
    /// @c GnssConfig::getCycleSlipConfig().turbo_edit_wl_settle_max_s
    /// (default 45; 0 disables).  Effective only with the soft-LLI bridge
    /// (@c turbo_edit_bridge_lli) — the deferral relies on the caller's
    /// holds to keep the affected phase measurements out of the filter
    /// while it waits.
    double wl_settle_max_s_{ 45.0 };
    /// Whether the WL-settle deferral may engage (bridge_lli && cap > 0).
    bool   wl_settle_enabled_{ false };
    /// Half-width of the float-derived WL integer candidate window
    /// (round(float) ± this).  The re-acquisition float is NL-code-biased
    /// by up to a few cycles (observed 2.25 WL on the G03 field event), so
    /// a ±1 window can EXCLUDE the true integer and make a wrong candidate
    /// window-relatively "unique".  ±3 admits the bias; ambiguity among
    /// the admitted candidates is then resolved by the sibling-pair
    /// zero-affirmation constraint or the WL-settle deferral, never by
    /// window truncation.
    static constexpr int kBridgeKWindow = 3;

    /// Observation-domain per-(sat, signal) phase-continuity tracking for the
    /// gap bridge's moot-vs-repair dispatch.  Key "G03_L2W".
    /// @c sig_continuous_since_s_ holds the start of the signal's current
    /// unbroken run of observed epochs: a signal whose run started at or
    /// before a bridge's reference epoch was observed at EVERY epoch through
    /// the gap (full receiver gaps break nothing — no epochs occurred), so
    /// its ambiguity survived; a signal that missed an epoch that DID occur
    /// was pruned-and-reseeded by the state side (prune-on-first-miss).
    /// Below-elevation-mask epochs count as missing (those observations are
    /// excluded from the filter too).  Cleared only by @c resetAll —
    /// continuity is observation-domain truth, independent of filter resets.
    std::map<std::string, double> sig_last_seen_s_;
    std::map<std::string, double> sig_continuous_since_s_;
    /// (sat, signal) keys ("G03_L2W") the caller is HOLDING across a receiver
    /// LLI gap (@c markHeldAcrossLli).  A held signal is adjudicated as
    /// CONTINUOUS by the gap-bridge dispatch (integer repair) rather than
    /// MOOT — the caller's hold preserves the pre-gap ambiguity value, so the
    /// across-gap integer is a real repair, not an already-absorbed reseed.
    /// One-shot: erased when the bridge actions the pair, and on any arc reset
    /// touching the (sat, signal).  Empty unless @c turbo_edit_bridge_lli.
    std::set<std::string> held_across_lli_;
    /// Floor on the GF-only detection threshold (m).  Below |λ1−λ2| (~0.054 m
    /// for GPS L1/L2) so a ±1-cycle equal slip (MW's blind spot) is detectable.
    static constexpr double kMinThresholdM = 0.04;
    /// Maximum rounding residual of the float ΔN2 to accept the integer split.
    static constexpr double kMaxFractional = 0.30;
    /// kσ multiplier for the code-event AFFIRMATIVE-ZERO test: an MW-flagged
    /// pair is adjudicated a code-domain artifact only when |TD2| ≤ this ×
    /// the arc's own TD2 MAD-σ (≈ 99.95% two-sided coverage for clean phase
    /// noise).  Deliberately TIGHTER than the detection gate
    /// (@c sigma_multiplier_, default 6–8): affirming "the phase did not
    /// move" demands more discrimination than flagging an outlier, and any
    /// genuine GF movement beyond this bound must instead reach the joint
    /// solve (e.g. a real (4,3) L1/L2 pair at 2.9 cm against mm-level σ).
    /// Purely empirical — scales with the arc's own measured noise, no
    /// absolute floor — so a sloppy arc widens the bound honestly (GF that
    /// cannot discriminate must not pretend to) and a quiet arc keeps the
    /// veto from swallowing repairable small pairs.
    static constexpr double kCodeEventZeroSigma = 3.5;
    /// Minimum TD2 MAD-window samples before the affirmative-zero test is
    /// allowed.  Deliberately stricter than @c kMinEpochsForAdaptive (5):
    /// a 5-sample MAD has far too much estimator variance to underwrite a
    /// state-PRESERVING decision at the claimed ~3.5σ coverage — detection
    /// merely risks a false flag, affirmation risks protecting a slipped
    /// ambiguity.  Below this the veto stands down and the MW mandate
    /// takes its reset as before.
    static constexpr std::size_t kCodeEventMinMadSamples = 10;
    /// Discrimination cap on the code-event affirmative-zero gate, as a
    /// fraction of the pair's @f$|\lambda_1-\lambda_2|@f$ (the smallest
    /// GF-visible slip quantum, the dN1=dN2=±1 pair).  A zero gate wider
    /// than this cannot tell "the phase did not move" apart from a small
    /// slip — nor from the near-null-line small pairs ((4,3)/(5,4) L1/L2 at
    /// 2.9/2.5 cm) the veto must let reach the joint solve — so the veto
    /// stands down and the MW mandate governs.  At 1 s the mm-scale MAD
    /// keeps the gate far below this cap (historical behavior unchanged);
    /// the cap binds at coarse rates, where the iono-inflated MAD would
    /// otherwise let the veto swallow genuine small pairs.
    static constexpr double kVetoZeroGateMaxLambdaFrac = 0.5;
    /// EWMA smoothing factor for the per-arc GF rate (m/s) that drift-corrects
    /// the across-gap bridge jump.  A single TD1 carries √2·σ_GF of noise —
    /// projecting it over a 7–15 s gap would AMPLIFY error beyond the quiet-
    /// iono drift it corrects; the EWMA (noise ≈ √(α/(2−α)) ≈ 0.23× a single
    /// TD1 at α=0.1) tracks the slowly varying iono trend without that
    /// amplification.
    static constexpr double kBridgeRateAlpha = 0.1;
    /// Default inter-epoch gap (s) before a GF arc is reset.
    static constexpr double kDefaultMaxGapSeconds = 120.0;
    /// Anti-echo cooldown after a slip (epochs): the TD2 echo of a prior event
    /// must not retrigger a GF-only detection.
    static constexpr std::size_t kCooldownEpochs = 3;
    /// Cross-satellite IVI threshold (m): above this the ionosphere is deemed
    /// active and GF jumps are not trusted.  Matches @c GfSlipDetector.
    static constexpr double kIviThreshold = 0.06;
    /// Minimum cross-sat pool size before its MAD is statistically meaningful;
    /// below this the previous EWMA value persists.  Matches the MW detector.
    static constexpr std::size_t kMinXsatSamples = 5;
    /// EWMA smoothing factor for the cross-sat TD2 σ tiers (first finite MAD
    /// bootstraps).  Matches the MW detector's default cross-sat alpha.
    static constexpr double kXsatEwmaAlpha = 0.1;
    /// Sanity bound on a resolved per-signal integer slip.
    static constexpr int kMaxSlipCycles = 1000;
    /// Sampling interval (s) above which the one-time coarse-rate advisory is
    /// logged and @c EpochResolution::rate_ok reports false.  Advisory only —
    /// no decision is gated on it (see the header's "Rate adaptivity" note).
    static constexpr double kHighRateMaxIntervalS = 1.0;

    // --- GF combination helpers (self-contained; see file note) ----------

    /**
     * @brief Collect every dual-frequency carrier-phase pair from an observation,
     *        ordered higher-frequency first.
     *
     * All tracked phase attributes are paired (not one canonical per band) so
     * that whatever (sig1, sig2) the MW detector reported has a matching GF arc.
     */
    std::vector<PhasePair> collectPhasePairs(const PppObservation& obs) const;

    /**
     * @brief Geometry-free phase observable @f$L_1-L_2@f$ in metres.
     * @return GF value (m), or NaN on missing/invalid input.
     */
    double computeGfMeters(const PppObservation& obs,
                           const std::string& sig1,
                           const std::string& sig2) const;

    /// @brief MAD-derived Gaussian-equivalent sigma of a TD2 window (NaN if immature).
    static double computeMadSigma(const std::deque<double>& window);

    /**
     * @brief Best available TD2 noise σ for an arc (m).
     *
     * The arc's own MAD when the window is mature, else the cross-sat tier
     * floor (elevation re-expanded), else NaN (true cold start).  This is the
     * evidence every rate-adaptive gate is scaled by.
     *
     * @param arc      Arc whose TD2 window is consulted.
     * @param arc_key  "SAT_SIG1_SIG2" key for the cross-sat fallback lookup.
     * @param elev_deg Arc elevation (deg) for the zenith re-expansion.
     * @return σ in meters, or NaN when no estimate exists.
     */
    double effectiveSigma(const GfArc& arc, const std::string& arc_key,
                          double elev_deg) const;

    /**
     * @brief Split-capability gate (see the header's "Rate adaptivity" note).
     *
     * True when the GF evidence is sharp enough to attribute per-signal
     * integers on the @f$\lambda_1-\lambda_2@f$ lattice:
     * @f$\sigma\le@f$ @c split_max_sigma_ @f$\times|denom|@f$.  False on a
     * NaN σ (no evidence is not evidence of sharpness) and on a degenerate
     * pair.
     *
     * @param sigma_m TD2 noise σ in meters (for bridged adjudications, the
     *                gap-widened value).
     * @param denom_m Lattice spacing @f$\lambda_1-\lambda_2@f$ (m).
     */
    bool splitCapable(double sigma_m, double denom_m) const;

    /**
     * @brief Adaptive GF-only detection threshold (m) for an arc.
     *
     * Two regimes, both floored at @c kMinThresholdM and both finite —
     * the gate is never NaN once a TD2 exists:
     *
     *  - MATURE arc (≥ @c kMinEpochsForAdaptive TD2 samples): kσ × the
     *    arc's own MAD governs.  The cross-sat σ is NOT consulted here
     *    (warmup-only contract, as in the MW detector).
     *  - WARMUP arc: kσ × the cross-sat TD2 σ for this arc's
     *    (constellation × band-pair) group (zenith-pooled, EWMA-smoothed,
     *    re-expanded to this arc's elevation via @c xsatSigmaFloor),
     *    resolved through the breadth cascade band-pair → constellation →
     *    global non-GLONASS so sparse constellations borrow from the
     *    broader population; the bare @c kMinThresholdM quantum floor
     *    gates alone only at the true cold start (first ~2 epochs of a
     *    run) and for GLONASS when its own tiers are thin.
     *
     * On clean arcs the floor governs in BOTH regimes by design — the
     * smallest physically meaningful GF event is the dN1=dN2=±1 quantum
     * |λ1−λ2| (~5.4 cm GPS L1/L2), so the adaptive terms exist to LIFT
     * the gate on noisy arcs / disturbed iono, never to sharpen quiet
     * ones below slip-physical scale.
     *
     * @param arc       Arc whose TD2 window is consulted.
     * @param arc_key   "SAT_SIG1_SIG2" key, reduced via @c xsatGroupKey
     *                  for the cross-sat lookup.
     * @param elev_deg  Arc elevation (deg) for the zenith re-expansion.
     * @param out_unfloored Optional; receives the active adaptive term
     *        with NO floor applied (mature: kσ·MAD(arc); warmup:
     *        kσ·σ_xsat; NaN only when neither exists) — traced as the
     *        @c thresh_unfloored_m diagnostic column.
     */
    double computeAdaptiveThreshold(const GfArc& arc,
                                    const std::string& arc_key,
                                    double elev_deg,
                                    double* out_unfloored = nullptr) const;

    /**
     * @brief Reduce an arc key to its cross-sat pooling key.
     *
     * "G01_L1C_L2W" → "G_L1_L2": constellation char plus the 2-char
     * frequency-band prefixes with the tracking attribute dropped —
     * pooling by FREQUENCY pair joins L1C/L2W with L1C/L2L etc., exactly
     * mirroring the MW detector's group key so sparse attribute
     * combinations still reach @c kMinXsatSamples.
     *
     * @return Group key, or empty when the arc key is malformed.
     */
    static std::string xsatGroupKey(const std::string& arc_key);

    /**
     * @brief Cross-sat TD2 σ for an arc, resolved band-pair group →
     *        constellation, re-expanded to the arc's elevation.
     * @return σ in meters, or NaN when no tier is populated.
     */
    double xsatSigmaFloor(const std::string& arc_key, double elev_deg) const;

    /**
     * @brief Fold the epoch's staging pool into the EWMA cross-sat tiers
     *        and clear it.  Called at the end of every @c resolve() —
     *        the floor therefore lags one epoch, like the MW pool.
     */
    void finalizeXsatSigmas();

    /**
     * @brief Cross-satellite ionospheric-variability index for a constellation.
     * @param constellation_td2 Per-arc |TD2| values for one constellation this epoch.
     * @return IVI (m): median(|TD2|) + MAD-sigma(|TD2|); 0 if insufficient data.
     */
    static double computeIvi(const std::vector<double>& constellation_td2);

    /// @brief Build the GF arc key "SAT_SIG1_SIG2".
    static std::string arcKey(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2);

    /// Output stream for the per-pair joint-solve CSV trace.
    mutable std::ofstream trace_file_;
    /// Epoch time captured for tracing.
    GnssTime current_epoch_;
};

} // namespace GnssProcessor
