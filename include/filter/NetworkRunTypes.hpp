#pragma once
/**
 * @file NetworkRunTypes.hpp
 * @brief @internal Run-loop record types for Application::runNetwork — the
 *        per-station bundle, PASS-B assembly slot, and the D2/Card-1 carry PODs.
 *
 * Hoisted VERBATIM from the ApplicationNetwork.cpp anonymous namespace
 * (decomposition INC-2, 2026-07-16; see
 * docs/network_decomposition_plan.md).  Support header for the
 * network run loop — not a public API seam; the authoritative documentation
 * of each type stays on the type itself.  Former file-static helpers
 * (@ref sharedCarryKey, @ref whitenCouplingRows) are now @c inline with
 * external linkage; nothing else changed.
 */

#include "filter/StateManager.hpp"
#include "filter/NetworkEstimator.hpp"      // SharedParamKind
#include "model/PppModel.hpp"
#include "measurement/MeasurementModel.hpp"
#include "common/PppState.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"
#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/IonoVariabilityMonitor.hpp"

#include <Eigen/Dense>

#include <algorithm>      // rxCbStepScale's median / MAD (nth_element, remove_if)
#include <array>
#include <cmath>          // the invariant step z (std::abs / std::isfinite)
#include <cstdint>        // §7.6 lane-observatory freeing-cause bitmask
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Per-station runtime bundle: an independent single-receiver StateManager /
/// PppModel / MeasurementModel (single-receiver invariants intact — the network
/// couples stations only through the shared satellite border, never by aliasing
/// their core indices).  Coordinates are held fixed at @c ecef.
struct NetStation {
    std::string                       name;
    Eigen::Vector3d                   ecef{Eigen::Vector3d::Zero()};
    bool                              is_pivot{false};
    std::unique_ptr<StateManager>     sm;
    std::unique_ptr<PppModel>         model;
    std::unique_ptr<MeasurementModel> mm;
    PppState                          pstate;
    // MW cycle-slip arc state: persistent per-station.  Constructed for NON-pivot
    // stations when network.phase_leveling && (network.cycle_slip.turbo_edit ||
    // network.cycle_slip.mw_detect); null otherwise (no MW detection runs).  The
    // turbo_edit RESOLVER (MW+GF integer REPAIR, 1 s data) is built only for
    // turbo_edit; under mw_detect the same detector drives a detect→refloat with
    // no resolver (null turbo_edit ⇒ the refloat branch in PASS A).
    std::unique_ptr<MwSlipDetector>   mw_slip;

    /// Adaptive iono q (network.adaptive_iono_q): per-station GF-TD2 variability
    /// monitor.  Fed in the PARALLEL PASS A produce (station-local state, no
    /// shared write — same contract as mw_slip above); its per-satellite
    /// multipliers are consumed SERIALLY by the T5v2 GM deviation aging, the
    /// only per-(station,satellite) time-linked slant-iono element on the
    /// network path (slant states themselves are white per epoch).  Null when
    /// disarmed (no calls, byte-identical OFF).
    std::unique_ptr<IonoVariabilityMonitor> iono_var;
    /// Invariant-#4 feed counter for iono_var (run-end readout sums over
    /// stations serially, like SdEwlCounters below).
    long long iono_var_epochs_fed{ 0 };

    /// SD pre-reduce EWL constraint (docs/SPEC_sd_ewl_prereduce.md) — per-station
    /// eligibility tallies for `Application::networkAppendSdEwlRows_`.  Held on the
    /// STATION (not a shared accumulator) because the row build runs inside the
    /// PARALLEL produce phase; the run-end readout sums over stations serially.
    /// Standing invariant #4: a zero `rows_built` with everything else zero means the
    /// wiring never ran, which is NOT the same as "the feature had no effect".
    struct SdEwlCounters {
        long long sat_seen{ 0 };                ///< (station,epoch,satellite) visits on a constellation that HAS an EWL tier — the denominator every skip below is measured against.
        long long rows_built{ 0 };              ///< SD EWL rows appended to this station's block.
        long long rows_capped{ 0 };             ///< eligible rows dropped by max_rows_per_station.
        long long skip_no_detector{ 0 };        ///< st.mw_slip == nullptr (the clock-datum station, or phase_leveling/detector off) — counted ONCE per station-epoch.
        long long skip_no_pivot{ 0 };           ///< fewer than two resolved arcs at this station for a constellation ⇒ no between-satellite difference to form.
        long long skip_immature{ 0 };           ///< an MW arc EXISTS on a candidate signal pair but its lagged Welford count < min_arc_epochs.
        long long skip_no_arc{ 0 };             ///< the detector holds NO MW arc on ANY candidate (band1,band2) signal pair — the "does the L2/L5 MW exist at all" tally.  Distinct from skip_immature by construction: this one means the pair was never formed.
        long long skip_frame_recon{ 0 };        ///< the arc is stored SD (bia_corrected) but its group MW pivot arc was absent, so arcZdMean could not rebuild the ZD frame (valid=false at count>0).
        long long skip_mixed_frame{ 0 };        ///< the two arcs disagree on bia_corrected (satellite-bias cancellation needs ONE frame).
        long long skip_missing_amb_col{ 0 };    ///< no (band1,band2) phase-signal pair has BOTH ambiguities in this station's local block this epoch (band untracked, or the arc is not estimated).
        long long skip_missing_shared_col{ 0 }; ///< a satellite PB/CB shared column is unregistered this epoch.
        long long skip_missing_code{ 0 };       ///< the arc did not report the CODE channels its MW consumed, so the satellite code-bias column cannot be named without guessing.
        long long skip_bad_lambda{ 0 };         ///< non-finite / non-positive wavelength or frequency.
        long long skip_big_z{ 0 };              ///< |z_SD| exceeded max_abs_z_m.
        double    sum_var_m2{ 0.0 };            ///< sum of the built rows' pre-whitening SD variances (m^2) — for the mean-R readout.
        // --- increment 2b (network.sd_ewl.fix_enable); all zero on the float-only path.
        long long rows_fixed{ 0 };              ///< rows that folded a FIXED integer (their four local ambiguity coefficients are absent, not merely small).
        long long rows_float{ 0 };              ///< rows that took the increment-1 float path.  rows_fixed + rows_float == rows_built.
        long long groups_fixed{ 0 };            ///< whitened ALL-FIXED groups emitted (a fixed and a float group never share one R — see Application.hpp).
        long long fix_pair_mismatch{ 0 };       ///< an integer was published but the arc the row settled on is a DIFFERENT tracking-channel pair, so the integer belongs to another bias frame and the row went float.
        long long fix_datum_only{ 0 };          ///< arcs demoted to the float group because their whole fixed group was DATUM values only (frame choices, no rounding decision) — a hard constraint with zero integer information is not worth a removed degree of freedom, and admitting it would make the feature undisarmable through its own gates.
        long long fix_lone_arc{ 0 };            ///< exactly ONE arc of a constellation had an integer: nothing to difference it against, so it was demoted to the float group (note it keeps the fix sigma floor, which only ever DE-weights).

        void operator+=(const SdEwlCounters& o) {
            sat_seen += o.sat_seen;
            rows_built += o.rows_built;
            rows_capped += o.rows_capped;
            skip_no_detector += o.skip_no_detector;
            skip_no_pivot += o.skip_no_pivot;
            skip_immature += o.skip_immature;
            skip_no_arc += o.skip_no_arc;
            skip_frame_recon += o.skip_frame_recon;
            skip_mixed_frame += o.skip_mixed_frame;
            skip_missing_amb_col += o.skip_missing_amb_col;
            skip_missing_shared_col += o.skip_missing_shared_col;
            skip_missing_code += o.skip_missing_code;
            skip_bad_lambda += o.skip_bad_lambda;
            skip_big_z += o.skip_big_z;
            sum_var_m2 += o.sum_var_m2;
            rows_fixed += o.rows_fixed;
            rows_float += o.rows_float;
            groups_fixed += o.groups_fixed;
            fix_pair_mismatch += o.fix_pair_mismatch;
            fix_datum_only += o.fix_datum_only;
            fix_lone_arc += o.fix_lone_arc;
        }
    };
    SdEwlCounters sd_ewl_ctr;   ///< accumulated across this run for this station.
    std::unique_ptr<TurboEditResolver> turbo_edit;
    // IF-phase triple-difference slip DETECTION (this increment): persistent
    // per-station between-satellite/between-epoch IF-phase OMC state.  Constructed
    // for NON-pivot stations when network.cycle_slip.if_phase_td; null otherwise
    // (no detection runs).  Station-local — parallel-safe, no shared write.
    std::unique_ptr<IfPhaseSlipDetector> if_slip;
    // IF-phase slip thresholding (this increment): per-signal-pair EWMA of the
    // elevation-normalized robust noise scale for THIS station, with a clean-sample
    // counter driving the pool→per-station shrinkage.  Persists across epochs; keyed
    // by the IF pair "sig1_sig2" (e.g. "L1W_L2W").  See the PASS-A IF-phase block.
    struct IfPairScale {
        double ewma_scale{0.0};  ///< EWMA of 1.4826·median(|z|), z = residual·sin(el) (m, zenith-equiv).
        long   count{0};         ///< Clean samples folded in (shrinkage weight vs the network pool).
    };
    std::map<std::string, IfPairScale> if_pair_scale;
    // Prioritized phase-bias anchor (network.phase_datum_prioritized) / eq34 chain
    // (network.phase_datum_eq34): the "sat_signal" ambiguity keys THIS station
    // gauge-pins THIS epoch, each mapped to its integer PIN TARGET (m).  Each such
    // ambiguity gets the tight-prior (1e10) pin folded into this station's OWN
    // local block.  Prioritized: target ≡ 0 (drive the absolute ambiguity to 0 so
    // the phase bias absorbs THIS receiver's lump).  eq34: one entry per
    // spanning-tree edge (r,s) rooted at THIS station — target 0 on first sighting,
    // λ·round(carried float) on a re-pin (pin-to-round, so the emitted-bias frame
    // step is an integer, usually 0 cycles).  Cleared + rebuilt every epoch by the
    // serial election (PASS A tail).  EMPTY on the legacy single-pivot path — there
    // the pivot pins the whole shared set via is_pivot, so no station carries this.
    // A station gauges SOME (sat,signal) and is an ordinary carry-eligible station
    // for the rest: membership is per-(sat,signal), never a station-level role.
    std::map<std::string, double> pinned_amb_targets;
    // eq34 (Card 3) ONLY: the "<sys>_RPB_<σ>" receiver-phase-bias keys THIS station
    // is the datum receiver r1 for this epoch — each folded as a 1e10 pin on the RPB
    // column in this station's OWN block (δ_{r1,σ}=0, the receiver half of the
    // single-receiver-chain gauge).  Cleared + rebuilt every epoch by the chain
    // election.  EMPTY except at an eq34 component's datum receiver (and always
    // empty when network.phase_datum_eq34 is off).
    std::set<std::string> pinned_rpb_keys;
    // eq34 reattach hygiene INC-3 (docs/SPEC_eq34_reattach_hygiene.md): per-pin σ
    // MULTIPLIER for the (few) entries of @ref pinned_amb_targets that had to be
    // pinned on a host younger than the eligibility threshold.  The fold divides the
    // 1e10 hard-pin weight by scale² — the pin still fixes the gauge, but the datum
    // row is soft enough for the data to pull the cold host's ambiguity during the
    // ramp instead of re-levelling the whole component onto its code-seeded value.
    // ABSENT key ⇒ scale 1 ⇒ the exact legacy 1e10 weight, so an OFF run never reads
    // this map and the fold is byte-identical.  Cleared + rebuilt with the pins.
    std::map<std::string, double> pinned_amb_sigma_scale;
    // Card 3 SEED-ROUND (docs/SPEC_eq34_seed_round.md §3.2, fold-deferred path):
    // wavelength per elected pin whose ambiguity STATE did not exist at election
    // time (cold start; a genuinely new arc — PASS B's pre-pass allocates it), so
    // the election could not read a value to round.  The fold computes
    // target = λ·round(piv_x/λ) from this map and reports the realized target
    // back through StationAssembly::pin_seed_targets.  EMPTY unless
    // network.phase_datum_eq34_seed_round; cleared + rebuilt with the pins.
    std::map<std::string, double> pinned_amb_seed_lambda;

    // --- flicker/gap signal integrity INC-2 (docs/SPEC_flicker_gap_integrity.md) ---
    // Per-SATELLITE σ MULTIPLIER for this station's PHASE rows while that line of
    // sight is UNVETTED (not STABLE — re-entered, flickered, degraded, or brand new,
    // i.e. inside the structural window where no time-difference detector can operate:
    // MW needs Welford history, IF-TD needs three consecutive epochs, GF is
    // doctrine-barred at 30 s).  The split at the fold divides the row's information
    // by scale², CODE rows untouched — they are slip-immune and they are what
    // re-anchors the float.  Keyed by SATELLITE, not by ambiguity key, because the
    // demotion is a property of the LINE OF SIGHT: across a loss of lock the slip
    // magnitude is unbounded on EVERY band at once (the G21 case carries LLI=1 on
    // both), so demoting one band and trusting the other would be the same
    // absence-of-evidence claim the card exists to refuse.
    // ABSENT key ⇒ scale 1 ⇒ the exact legacy 1/σ² weight, so an OFF run never reads
    // this map and the assembly is byte-identical.  Cleared + rebuilt each epoch by
    // @ref networkSignalIntegrity, before the parallel PASS-B produce reads it
    // (station-local ⇒ thread-safe).
    std::map<std::string, double> phase_sigma_scale;
    /// Card GL (docs/SPEC_event_harness.md §24.18): per-SATELLITE sigma scale for this
    /// station's CODE rows while that line of sight is declared GROSS.  The exact CODE
    /// counterpart of @ref phase_sigma_scale above and it obeys the same contract: the
    /// fold divides the row's information by scale^2 (sigma x scale), PHASE rows are
    /// untouched (they have their own responses -- the erase + pending-reseed the
    /// declaration already performed), and an ABSENT key means scale 1, i.e. the exact
    /// legacy 1/sigma^2 weight.  Cleared + rebuilt by @ref networkGrossLosPublishScales,
    /// SERIALLY at the end of the previous epoch's QC block, before the parallel PASS-B
    /// produce reads it (station-local => thread-safe).  Never populated when
    /// network.gross_los_screen is off, so an OFF run never reads this map and the
    /// assembly is byte-identical.
    std::map<std::string, double> gl_code_sigma_scale;
    /// Card GL: CODE rows this station folded at the deweighted sigma over the run.
    /// Accumulated inside the PARALLEL produce phase, so it must live per-station;
    /// summed into the run-end report at teardown (the @c si_rows_demoted precedent).
    long long gl_rows_deweighted{ 0 };
    /// Phase rows this station folded at a demoted weight over the run (INC-2's ROW
    /// telemetry).  Accumulated inside the PARALLEL produce phase, so it must live
    /// per-station; summed into the run-end report at teardown (the
    /// @c iono_var_epochs_fed / @c sd_ewl_ctr precedent).
    long long si_rows_demoted{ 0 };
    /// Card L5P (`network.extra_band_code_sigma_scale`): CODE rows of an EXTRA-BAND
    /// signal (one whose band is neither band of the constellation's ionosphere-free
    /// pin pair) this station folded at the scaled sigma over the run.  Accumulated
    /// inside the PARALLEL produce phase, so it lives per-station and is summed into
    /// the run-end report at teardown (the @c gl_rows_deweighted precedent).
    long long eb_rows_deweighted{ 0 };
    /// Card L5P: code rows the scale could NOT be decided for, because the row's
    /// constellation has no resolvable ionosphere-free pin pair in the BIA product.
    /// Those rows keep their unscaled weight; a nonzero count means the knob is
    /// partly inert and the run-end line says so (standing invariant #4).
    long long eb_rows_no_pin{ 0 };

    // --- card SCC (docs/refound/SCC_second_code_channel_2026-09-21.md) ----------
    /// THIS epoch's COMPANION cells at this station, as "<satellite>|<signal>" keys
    /// (@c StateKeys::makeSatBiasKey's shape).  Republished SERIALLY from the PASS-A
    /// observations, before the parallel PASS-B produce reads it (station-local ⇒
    /// thread-safe), which is what lets @ref Application::networkSplitStation_ tell a
    /// COMPANION row from a FALLBACK row: both route through the receiver inter-signal
    /// state, and only the companion arrived beside its canonical code.  EMPTY on
    /// every unarmed posture, so the split never reads it and the assembly is
    /// byte-identical.
    std::set<std::string> scc_cells;
    /// Card SCC (R7, standing invariant #7): the PREVIOUS epoch's @ref scc_cells, so
    /// the serial sweep can diff the two and emit the opening and closing edges.  Not
    /// cleared between epochs — a station absent this epoch keeps its set, and its
    /// cells close when it returns without them.
    std::set<std::string> scc_cells_prev;
    /// Card SCC (R7): receiver inter-signal state keys this station held at the END of
    /// the previous epoch.  A key that is absent here and present now, at a station
    /// that has held it before, is a RESEED — the state cold-started rather than being
    /// restored by the receiver code-bias carry, which is the one way this card can
    /// silently determine a receiver bias from a single epoch of code.
    std::set<std::string> scc_isc_prev;
    /// Card SCC (R7): receiver inter-signal state keys this station has EVER held.
    /// The reseed test needs "had one before", not "had one last epoch" — a state that
    /// legitimately disappears for an hour and returns is still a reseed.
    std::set<std::string> scc_isc_seen;
    /// Card SCC (R7): COMPANION code rows this station folded over the run.
    /// Accumulated inside the PARALLEL produce phase, so it lives per-station and is
    /// summed into the run-end report at teardown (the @c eb_rows_deweighted
    /// precedent).
    long long scc_rows{ 0 };
    /// Card SCC (R7): code rows this station routed through the receiver inter-signal
    /// state that were NOT companions, i.e. a band-1 code that arrived without its
    /// latched canonical beside it (the FALLBACK case R2 covers).  On a lineage whose
    /// canonical latch is decided this reads 0; a nonzero count means some station's
    /// canonical code is regularly missing, which is a finding about the latch.
    long long scc_fallback_rows{ 0 };

    // --- card SID phase 6a (docs/refound/SID_signal_identity_2026-09-22.md) ------
    /// THIS epoch's SECOND-PHASE cells at this station, as
    /// @c StateKeys::makeSatPhaseBiasKey keys.  Republished SERIALLY from the PASS-A
    /// observations, before the parallel PASS-B produce reads it (station-local, so
    /// thread-safe), which is what lets @ref Application::networkSplitStation_ tell a
    /// second-phase row from the latched one: nothing else distinguishes them, because
    /// this card deliberately builds no receiver state a row could be keyed on.
    /// EMPTY on every unarmed posture, so the split never reads it and the assembly is
    /// byte-identical.
    std::set<std::string> spc_cells;
    /// Card SID phase 6a (R7, standing invariant #7): the PREVIOUS epoch's
    /// @ref spc_cells, so the serial sweep can diff the two and emit the opening and
    /// closing edges.  Not cleared between epochs — a station absent this epoch keeps
    /// its set, and its cells close when it returns without them.
    std::set<std::string> spc_cells_prev;
    /// Card SID phase 6a (R7): every second-phase cell this station has EVER opened.
    /// The run-end "arcs born" count is its size, summed over stations; a re-opening
    /// after a dropout is an EDGE, not a birth.
    std::set<std::string> spc_cells_seen;
    /// Card SID phase 6a (R7): SECOND-PHASE rows this station folded over the run.
    /// Accumulated inside the PARALLEL produce phase, so it lives per-station and is
    /// summed into the run-end report at teardown (the @c scc_rows precedent).
    long long spc_rows{ 0 };
    /// Card SID phase 6a (R8): the between-channel WITNESS memory, keyed
    /// "<satellite>|<second signal>" -> (latched minus second, in CYCLES; the loop
    /// epoch it was measured at).  Epoch-to-epoch differencing of a quantity in which
    /// a one-channel slip is an integer and a common slip cancels.  Station-local and
    /// written only by the serial sweep.  EMPTY unless the knob is armed.
    std::map<std::string, std::pair<double, long long>> spc_witness;
};

/// One SD extra-wide-lane INTEGER decision for a (station, constellation, satellite),
/// as published by @ref networkSdEwlAr (increment 2a, Route B + the A2 DD-to-SD datum)
/// and consumed by @c Application::networkAppendSdEwlRows_ (increment 2b).
///
/// @c n_cyc is in the r0-anchored frame: it equals the TRUE undifferenced EWL integer
/// plus a per-station constant that cancels in the between-satellite difference and a
/// per-satellite constant set by the component reference station.  So only the SD
/// difference @c n_cyc(s) − @c n_cyc(pivot) is meaningful — never the value itself.
///
/// @c sig1 / @c sig2 record the NETWORK-COMMON channel pair the integer was decided on.
/// The row builder must fold on that exact pair: the satellite bias combination
/// @f$B_{EWL}^s@f$ is per-signal, so a row built on a different tracking channel would
/// assert an integer derived from a different bias frame.
struct SdEwlFix {
    long long   n_cyc{ 0 };        ///< propagated SD-frame integer, CYCLES of lambda_EWL.
    std::string sig1, sig2;        ///< the network-common phase pair it was decided on.
    double      frac_cyc{ 0.0 };   ///< |fractional residue| of the DD that produced it (0 for the link/reference satellite — a DATUM choice, not a measurement).
    double      ffr{ 1.0 };        ///< rounding success probability of that DD (1 for the link/reference satellite).
    bool        usable{ false };   ///< passed the frac + FFR gates AND was not vetoed by a flip this epoch.
    /// This value came from a ROUNDED double difference, not from a frame choice.
    /// Datum values (the reference station's own satellites, each station's link
    /// satellite, and a satellite framed on first sighting) carry no rounding decision
    /// and are therefore UNGATED by construction — no FFR or |frac| setting can reject
    /// them.  A whitened group built only from datum values would assert a hard
    /// constraint with ZERO integer information behind it while still removing a real
    /// local degree of freedom, and — measured — it also makes the feature impossible to
    /// disarm through its own gates (the S2 `inert` arm differed from the float arm on 17
    /// artifacts until this flag existed).  So the row builder requires at least one
    /// MEASURED integer per fixed group.
    bool        measured{ false };
};

/// One shared parameter carried across epochs (D2): its value and posterior
/// variance, re-keyed by (kind, sat, signal) identity so it survives layout
/// changes (a satellite blinking in/out).
struct CarryPrior {
    SharedParamKind kind;
    std::string     sat;
    std::string     signal;
    int             sh_n{-1};    ///< VtecSh degree (VtecSh only)
    int             sh_m{-1};    ///< VtecSh order  (VtecSh only)
    char            sh_trig{0};  ///< VtecSh 'C'/'S' (VtecSh only)
    int             sh_layer{0}; ///< VtecSh shell index (VtecSh only; 0 = legacy layer)
    double          value;
    double          var;
    bool            inflate_q{false};  ///< Clock-monitor flagged this satellite: fold NEXT epoch with the large reinit variance (a soft large-q reinitialization that lets a frozen clock walk toward truth while retaining a weak anchor) instead of the stiff process noise.
    /// READMIT continuity (E2 seed-first re-admission, network.readmit_seed_horizon_s;
    /// SPEC_synthetic_minimum_network §18.4 / SPEC_amb_iono_split_overcount §7 FIRST):
    /// continuous-GPS seconds at which this entry's shared column LEFT the layout
    /// (0 = live).  A PARKED entry is the satellite's pre-gap realization held for the
    /// seed horizon: it is never folded while its column is absent, it is walked by
    /// every frame re-base that walks @c prev_carry, it is excluded from the snapshot,
    /// and on re-registration it seeds the column's D2 prior at @c var + q·gap (the Q3
    /// anchor folds ANCHORED over the true gap instead of tripping `gap` onto a
    /// code-only LOOSE re-draw that its own re-couple then freezes into a tail).
    double          t_park_s{0.0};
};

/// READMIT continuity constants.  @c kReadmitMinGapEp: the re-admission detector's
/// minimum absence in loop epochs (a single withheld epoch skips the stash for EVERY
/// satellite and must not read as a fleet-wide re-admission).  @c kReadmitVerifyWindowS:
/// a Q3 nis/abs trip this soon after a clock seed is attributed to the seed (the data
/// REJECTED it — invariant #7's mark-if-moved).  @c kReadmitSeedK: the stash-side
/// seed-vs-solve check's standardized-residual threshold (telemetry).
inline constexpr long long kReadmitMinGapEp      = 3;
inline constexpr double    kReadmitVerifyWindowS = 300.0;
inline constexpr double    kReadmitSeedK         = 6.0;

/// Identity key of a dense-carried shared parameter (C1) — mirrors
/// NetworkEstimator's internal key construction so the stash and fold agree.
/// SH-VTEC never enters the dense block (its carry is the diagonal pre-gate
/// fold), so the (n,m,trig) family needs no key here.
inline std::string sharedCarryKey(SharedParamKind kind, const std::string& sat,
                                  const std::string& signal) {
    switch (kind) {
        case SharedParamKind::Clock:     return sat + "|CLK";
        case SharedParamKind::CodeBias:  return sat + "|CB|" + signal;
        case SharedParamKind::PhaseBias: return sat + "|PB|" + signal;
        default:                         return sat;  // unreachable for the dense carry
    }
}

/// C1 (network.shared_carry_full_cov): the dense posterior covariance block
/// over the D2-carried Clock/PhaseBias/CodeBias entries of @c prev_carry,
/// stashed beside it at emit and folded next epoch as ONE information block
/// Λ = (P_prev + Q)⁻¹ (@ref NetworkEstimator::addSharedPriorDense) — the
/// information-form Kalman time update on the shared border.  Identity-keyed
/// (row lookup by the same (kind,sat,signal) key as the estimator layout) so
/// it survives cross-epoch layout changes and every existing @c prev_carry
/// value mutation (eph-cutover re-level) without re-indexing: @c prev_carry
/// stays the single source of truth for carried values and flags, this is a
/// passive covariance side-car.  Empty ⇒ the fold degrades per-key to the
/// legacy diagonal path (toggle OFF, warm resume from the v1 snapshot, or a
/// stash-side non-finite screen).
struct SharedCarryCov {
    std::map<std::string, std::size_t> row;  ///< identity key → row/col in P
    Eigen::MatrixXd P;                       ///< dense posterior covariance (m²)
    void clear() { row.clear(); P.resize(0, 0); }
};

/// Card 1 (network.carry_conditional_coupling): one non-negligible entry of a
/// carried local column's regression @f$M_r@f$ on the shared border — the coupling
/// that makes the between-epoch carry CONDITIONAL (a pseudo-observation row over
/// @f$(x_s\mid x_r)@f$) rather than an independent marginal.  Identified by the
/// shared parameter's IDENTITY (kind + sat/signal or SH n/m/trig) so the entry
/// re-maps across the epoch's changing shared-column layout, plus the coefficient
/// @f$M_{rj}@f$, last epoch's solved value @f$\hat s_{\text{prev},j}=x_s(j)@f$ (the
/// coupling-row RHS term), and last epoch's datum-frame marginal variance
/// @f$P_{\text{prev},jj}@f$ (used to MARGINALIZE the term into the row variance if
/// the shared column has DEPARTED this epoch).  Datum-pinned / ensemble-gauge
/// columns are carried WITH their coefficient but with @c p_prev = 0 (exactly
/// known — the datum re-applies them each epoch): keeping them preserves the
/// row's exact gauge-invariance (Card 1e); amputating them made every coupling
/// row observe the clock gauge and the W5 null-space gate withheld each rows-epoch.
struct CoupledShared {
    SharedParamKind kind{SharedParamKind::Clock};  ///< shared parameter kind
    std::string sat;      ///< satellite id (Clock/PhaseBias/CodeBias)
    std::string signal;   ///< signal id (PhaseBias/CodeBias)
    int  n{0};            ///< SH degree (VtecSh)
    int  m{0};            ///< SH order  (VtecSh)
    char trig{'C'};       ///< SH cos/sin (VtecSh)
    int  sh_layer{0};     ///< SH shell index (VtecSh; 0 = legacy layer)
    double coeff{0.0};    ///< @f$M_{rj}@f$ (regression coeff of the local on this shared col)
    double s_prev{0.0};   ///< @f$\hat s_{\text{prev},j}=x_s(j)@f$ last epoch (correction frame)
    double p_prev{0.0};   ///< @f$P_{\text{prev},jj}@f$ last-epoch datum-frame variance (departed-col marginalization)
};

/// Relative sparsification threshold for a carried column's regression row: an
/// entry @f$M_{rj}@f$ is stashed only when @f$|M_{rj}| > @f$ @c kCouplingSparsifyRelThresh
/// @f$\cdot\max_k|M_{rk}|@f$ (Card 1).  1e-8 keeps only the numerically meaningful
/// couplings; the dropped near-zero entries contribute negligibly to the row.
inline constexpr double kCouplingSparsifyRelThresh = 1e-8;

/// Card 1b (network.carry_coupling_joint_whiten): one conditional coupling row's
/// pre-whitening ingredients, retained per station so a station's rows can be
/// JOINTLY whitened at build time AND re-whitened if a within-epoch RAIM refloat
/// later drops some.  @c shared holds the PRESENT shared terms kept as ordinary
/// @f$A_s@f$ coefficients (unchanged from Card 1); @c departed holds the DEPARTED
/// shared terms that are marginalized JOINTLY (each references an @f$M_w@f$ column
/// index, shared across the station's rows so their common departed direction is
/// correlated — the between-row information the scalar path discards).
struct CplRowRaw {
    Eigen::Index local_col{0};                            ///< carried local (A_r) column this row targets (+1).
    std::vector<std::pair<Eigen::Index, double>> shared;  ///< present shared terms: (shared col, M_rj).
    std::vector<std::pair<int, double>>          departed;///< marginalized shared terms: (M_w column index, M_rj).
    double rhs{0.0};                                      ///< (r̂_prev − r_lin) + Σ_present M_rj·ŝ_prev,j.
    double var_eff{0.0};                                  ///< pure conditional variance (+ process noise) — this row's own diagonal.
};

/// F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount): ONE
/// station's jointly-whitened phase-leveling carry block — the pre-whitening
/// ingredients retained per station so the block can be whitened at build time
/// AND re-whitened by the within-epoch RAIM refloat with the refloated columns
/// MARGINALIZED (their rows/cols deleted from Σ before the factor — never
/// conditioned on; standing invariant #7's decoupling edge).  @c rows are the
/// per-column SCALAR-path ingredients: the same +1/local, PRESENT shared terms
/// (Card-1d κ-shrunk, Card-1f gauge-restored), RHS and
/// σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt the Card-1 scalar row
/// would have carried — @c var_eff here IS that σ², i.e. Σ's diagonal (the
/// departed-column marginalization stays per-row/diagonal, the scalar-path
/// treatment; Card 1b's joint departed marginalization is mutually exclusive
/// with this block in the present build).  @c Sigma is the n×n joint prior
/// covariance over those columns: that diagonal, and on the off-diagonals the
/// HARVESTED conditional cross covariances (N_rr⁻¹)_{c_i c_j} (VCE-factor
/// scaled), 0 where the pair was harvested at different epochs — Σ is then
/// block-diagonal by harvest stamp, each block a principal sub-matrix of one
/// posterior, so PD is preserved by construction.  @c row0 = the block's first
/// row index within the station's coupling block (block row k sits at A_s/A_r
/// row @c cpl_row_start + row0 + k).  Empty unless the knob built a block for
/// this station this epoch.
struct PlBlockRaw {
    std::vector<CplRowRaw> rows;    ///< per-column ingredients (@c departed unused — already folded into @c var_eff)
    Eigen::MatrixXd        Sigma;   ///< n×n joint prior covariance (m²)
    Eigen::Index           row0{0}; ///< offset of the block's first row within the coupling block
};

/// F3 (network.phase_leveling_local_block): ONE station-epoch's harvested
/// conditional cross-covariance block over its carried ambiguity columns —
/// C(a,b) = (N_rr⁻¹)_{c_a c_b} = n_{c_a}(c_b), column a's N_rr⁻¹e_c solve read
/// at column b.  That solve is the one @ref NetworkNormalEquations::stationRegression
/// already pays per column for @c cond_var and @c row, so the cross terms are a
/// GATHER, not a second solve (LocalCarry::var is C's diagonal).  Row a is NaN
/// when column a's harvest failed.  Shared BY POINTER among the station's
/// @ref LocalCarry entries stashed at that harvest, each holding its own index:
/// "same harvest" is pointer equality, so a pair whose two entries point at
/// different blocks (one column re-harvested later than the other, or a
/// warm-resume-restored entry with no block at all) has NO valid cross term and
/// is zeroed + counted at inject.  Released when the last entry referencing it
/// is re-stashed or pruned.  Deliberately NOT snapshotted: the first post-resume
/// inject degrades to the diagonal block for exactly one epoch (read as
/// pair-miss counts), then the first post-resume harvest rebuilds it.
struct PlHarvestBlock {
    Eigen::MatrixXd C;        ///< n×n conditional cross covariance (m²); NaN rows = failed harvests
    double          stamp_s{0.0}; ///< continuous-GPS seconds of the harvest (diagnostic)
};

/// One carried-forward ambiguity (phase-leveling, Increment 1): the float
/// ambiguity's ABSOLUTE value, its posterior variance, and the continuous-GPS-
/// second time it was last seen (for the inter-epoch gap inflation / stale-carry
/// drop).  Keyed in @c local_carry by (station index, satellite, signal) so it
/// survives a satellite blinking in/out.
struct LocalCarry {
    double value_abs;     ///< Carried absolute ambiguity estimate (m).
    double var;           ///< Last-solve posterior variance (m²): the MARGINAL (conditionalLocalVarianceDiag) on the legacy diagonal carry, or — under network.carry_conditional_coupling — the PURE CONDITIONAL @f$(N_{rr}^{-1})_{rr}@f$.
    double last_seen_s;   ///< Continuous-GPS seconds when last stashed (gap inflation).
    std::vector<CoupledShared> coupling{};  ///< Card 1: the regression on the FREE shared border.  Empty on the legacy diagonal path, and for a warm-resume-restored entry unless network.resume_coupling_restore rebuilt it from a v3 snapshot's QL/QZ record (§18.12); empty on a v1/v2 file or with the knob off — an empty coupling injects as a plain diagonal row (graceful one-epoch degrade).
    std::shared_ptr<const PlHarvestBlock> blk{};  ///< F3 (network.phase_leveling_local_block): the harvest cross-covariance block this entry's off-diagonals live in — null when the knob is off, on the ZWD carry, and for a warm-resume-restored entry (no block ⇒ every pair with it is zeroed + counted at inject).
    int blk_idx{-1};                              ///< F3: this entry's row/column in @c blk->C (−1 = none).
    double var_first{0.0};                        ///< The arc's FIRST-STASH conditional variance (m²; 0 = unknown): set once at birth, kept across re-stashes (an erase-and-rebirth starts it over, a re-level keeps it).  The honest-σ-scale denominator of network.phase_datum_handover_max_var_ratio; snapshotted only when that knob is armed.
    /// RESTORED-ARC sentinel for @ref birth_ep (SPEC_event_harness §15.4).  A
    /// warm-resume-restored entry is stamped with this instead of being left at 0,
    /// because 0 also means "never stashed" — which is exactly what the harvest's
    /// first-stash-wins rule reads it as, so a restored arc used to be re-branded
    /// as born at the RESUME epoch by the first post-resume harvest and every age
    /// gate then saw a network of newborns for @c min_age epochs.  A negative stamp
    /// is unreachable by the harvest (@c n_loop_epochs is incremented before the
    /// harvest, so a genuine first stash is ≥ 1), survives the `!= 0` first-stash
    /// test, and reads MATURE at every age reader (all of which test `> 0`).
    static constexpr long long kBirthEpRestored = -1;
    /// RESTORED-ARC sentinel for @ref birth_s — the seconds twin of
    /// @ref kBirthEpRestored, on the same rule (a real stamp is a continuous-GPS
    /// second, so it is always ≫ 0).
    static constexpr double    kBirthSRestored  = -1.0;
    long long birth_ep{0};                        ///< Loop-epoch index (n_loop_epochs) of the arc's FIRST stash: set once at birth, kept across re-stashes, restarted by an erase-and-rebirth.  0 = never stashed, @ref kBirthEpRestored (−1) = warm-resume-restored with its true birth un-snapshotted; BOTH read as MATURE (every reader tests `> 0`), and both survive the harvest's first-stash-wins rule, which tests `!= 0`.  network.phase_leveling_local_block_min_age_ep keeps a leg OUT of the F3 block (per-column row instead) until n_loop_epochs − birth_ep ≥ the gate — a leg born at a satellite's re-admission carries its code-level birth error, and the block's joint whitening would spread it into the station's mature columns.  Not snapshotted: a restored entry is sentinel-stamped and reads mature FOR ITS WHOLE LIFE (until an erase-and-rebirth), which is the documented restored-entry contract.
    double birth_s{0.0};                          ///< Continuous-GPS seconds (week*604800 + tow) of the arc's FIRST stash — the seconds twin of @c birth_ep, set once at birth from the harvest's @c pl_now_s, kept across re-stashes, restarted by an erase-and-rebirth (a T4 attribution erase deletes the entry and the next harvest re-creates it with a level that ALREADY contains any earlier slip).  Read by the SIM-ONLY slip oracle (@c network.cycle_slip.oracle_relevel) as the hard lower edge of its truth-slip window, so a repair can never re-apply a jump the re-created level already absorbed.  NOT snapshotted (like @c birth_ep): a warm-resume-restored entry carries @ref kBirthSRestored (−1), which never wins the window's `max` and so leaves the window at its @c oracle_window_s latency bound for the arc's whole restored life — acceptable for a sim-only diagnostic and the conservative direction.
    /// Continuous-GPS seconds of the arc's LAST RE-LEVEL — the age clock of the
    /// AGE-AWARE young-arc variance inflation
    /// (@c network.phase_leveling_young_inflate_var_m2,
    /// SPEC_amb_iono_split_overcount §6.17–§6.18).  Set to the harvest's @c pl_now_s
    /// on exactly two events, both of which produce a level whose VALUE is
    /// CODE-QUALITY while its harvested VARIANCE is phase-stiff (conditioned on the
    /// station's mature arcs):
    ///   * the entry is CREATED (no previous entry) — a first stash, or the re-stash
    ///     after a T4 attribution erase / gap prune deleted the entry, and
    ///   * the arc was FREED this epoch by the slip response
    ///     (@c network.cycle_slip.relevel_free_epoch's latched @c free_ambs set, the
    ///     SAME set the PASS-B inject consulted), so this harvest re-levelled it from
    ///     one epoch of code.
    /// PRESERVED across every ordinary re-stash, so the age keeps growing while the
    /// arc matures.  The inject inflates the column's prior variance by
    /// V0·exp(−age/τ) until @c phase_leveling_young_inflate_max_age_ep, which is what
    /// stops the T4 tier erasing an arc that is merely UNREFINED rather than wrong
    /// (the erase–re-level treadmill, 21 erases/station-hour).  NOT snapshotted (like
    /// @c birth_s / @c birth_ep): a warm-resume-restored entry reads 0, which this
    /// card treats as UNSTAMPED ⇒ MATURE ⇒ no inflation — the conservative direction.
    /// (Unlike @c birth_s / @c birth_ep it needs no restore sentinel: it is stamped
    /// only on CREATE and on FREE, never on an ordinary re-stash, so a restored
    /// entry's 0 survives every harvest until the arc is actually re-levelled.)
    double relevel_s{0.0};
    /// RF-1 §3.3(b)/§7.2: the BIRTH PRIOR actually in force for this arc (m²) — the
    /// object a DROP restores.  Set ONCE at the arc's birth stash to
    /// @c max(var_first, network.amb_float_birth_var_m2), kept across every re-stash
    /// and every re-level exactly like @ref var_first, and restarted only by an
    /// erase-and-rebirth (which deletes the entry).
    ///
    /// It is a SEPARATE field from @ref var_first on purpose.  @c var_first is the
    /// arc's own first-stash conditional variance — a measurement of what the model
    /// reported — and is the denominator of
    /// @c network.phase_datum_handover_max_var_ratio, which must keep reading the
    /// unfloored quantity.  @c var_birth is what the arc is WORTH at birth: the same
    /// number floored by the posture's code-noise scale, because two routes hand back
    /// a phase-STIFF variance for a CODE-quality value (a band-scoped free determined
    /// against its sibling band's still-stiff carry, and the SH tie row that separates
    /// a new arc's ambiguity from its iono at the tie's optimistic precision — design
    /// §1.4).  Flooring @c var_first in place would silently move the handover ratio's
    /// denominator.
    ///
    /// ⚠ RF-1 STEP 1: maintained WRITE-ONLY.  With the default floor of 0.0 it equals
    /// @c var_first exactly, nothing reads it, and the solve is byte-identical; the
    /// drop that restores it lands in step 4.  NOT snapshotted today — design §6.1
    /// names it as the field that must join the warm-resume record when seams come
    /// back, or a restored arc cannot be dropped correctly.
    double var_birth{0.0};
    /// RF-1 §7.4: how many times this arc's carried prior has actually been FOLDED —
    /// the abscissa of the D1 variance-trajectory fit (@c log var = a − b·log n over
    /// the first 30 folds) and the bucket key of the D4 σ̂₀(pl)-by-arc-age read.
    ///
    /// It is deliberately NOT derivable from @ref birth_ep: an arc's age counts every
    /// epoch since its first stash, while the fold count skips exactly the epochs the
    /// carry did not act — the free-epoch latch, the gap cliff, a pending reseed, a
    /// non-finite @c x_lin — which are precisely the epochs RF-1 is about.  Incremented
    /// at the harvest under the SAME reproduced inject predicate the prior-residual
    /// census standardizes on, preserved across every re-stash, and restarted only by
    /// an erase-and-rebirth (which deletes the entry).
    ///
    /// TRACE-SUPPORT ONLY: maintained whenever @c network.trace_amb_state is armed and
    /// read by nothing on the solve path, so an unarmed run never touches it.
    long long n_folds{0};
    /// RF-1 §7.2: loop-epoch index (@c n_loop_epochs) of this arc's LAST DROP — the
    /// re-coupling edge's own clock, distinct from @ref relevel_s (which is stamped by
    /// the leveling vehicle's CREATE / FREE events) and from @ref birth_ep.  0 = never
    /// dropped.  Read by the @c network_amb_state.csv event column and, from step 4,
    /// by the drop's own idempotence guard (one drop per arc per epoch).
    /// ⚠ RF-1 STEP 1: declared and never written — the drop lands in step 4.
    long long dropped_ep{0};
    /// RF-3a A2 (docs/refound/RF-3_code_row_qc_2026-09-22.md §4): the WALK metric's
    /// per-arc phase.  A drop sets it to 1; the next harvest of the arc records
    /// @c |value_abs(after) − value_abs(before)| as the change the FIRST fold after the
    /// drop absorbed and advances it to 2; the fold after that records the same
    /// quantity as the residual WALK and clears it.  MEASURE-ONLY and maintained in
    /// both knob states, because the point of the metric is that an OFF run reports the
    /// walk it suffers: a stiff restored prior lets a 40 m level error through a few
    /// metres per epoch for tens of epochs, a floored one lands the correction at once.
    /// Not snapshotted, like every other RF-1 stamp — a restored entry reads 0, which
    /// is "no drop pending" and the conservative direction.
    int drop_walk_ph{0};
};

/// RF-1 §7.4: WHICH ROUTE the PASS-B inject actually folded a carried ambiguity
/// column through this epoch — the `route` column of @c network_amb_state.csv.
///
/// It is recorded at the inject rather than inferred at the harvest from the knob
/// posture, because the two can disagree on a single station-epoch: @c
/// CouplingRowBuilder::buildLocalBlock returns false on a non-SPD factor after its
/// ridge retry and the caller falls back to per-column rows.  Under the float_kf
/// vehicle that fallback is a VEHICLE CHANGE on one station-epoch — design risk #3 —
/// and this column is what makes it visible per arc rather than only as a run total.
enum class AmbFoldRoute {
    None,           ///< No prior was folded for this column this epoch (skipped: freed / stale-gap / pending reseed / no carry / non-finite / gauge-pinned).
    Diag,           ///< A plain diagonal local prior row (Λ, η) — the legacy per-column route.
    Block,          ///< A row of the station-wide jointly-whitened local block (F3 / the float_kf vehicle's structural fold).
    CouplingScalar  ///< A Card-1 conditional COUPLING row: +1 on the local column, M_r on the free shared border — RF-0's route.
};

/// RF-1 §7.4: ONE row of @c network_amb_state.csv — the per-arc carry-state trace.
///
/// Fields are exactly the design's pre-registered header, in its order, so a reader
/// (scripts/amb_state_read.py) can parse by name and a grader can reproduce the
/// pre-registered reads D1–D4 without re-deriving what a column means.  Rows are
/// buffered per epoch in @c NetworkCarryState and drained serially by
/// @ref networkAmbStateTrace, on the @c sd_ewl_trace precedent, so the file order is
/// thread-count independent and the four erase sites (which run in different seams,
/// some after the harvest) can all contribute to the same epoch's block.
struct AmbStateRow {
    long long   week{0};            ///< GPS week of the epoch.
    double      tow{0.0};           ///< GPS time of week (s) — the join key of every trace CSV in the run.
    std::string station;            ///< Station name.
    std::string sat;                ///< Satellite (e.g. G07).
    std::string signal;             ///< RINEX signal (e.g. L1C).
    double      value_abs_m{0.0};   ///< The carried ABSOLUTE float ambiguity after this epoch's harvest (m).  On an erase row: the value the entry held when it was deleted.
    double      var_m2{0.0};        ///< The carried posterior variance after this epoch's harvest (m²) — the conditional under Card-1.
    double      var_first_m2{0.0};  ///< @ref LocalCarry::var_first — the arc's FIRST-stash variance, unfloored.
    double      var_birth_m2{0.0};  ///< @ref LocalCarry::var_birth — the floored birth prior the drop restores (RF-1 step 4).
    double      fold_var_m2{0.0};   ///< The variance the fold ACTUALLY used this epoch (0 when @ref folded is false).
    long long   n_folds{0};         ///< @ref LocalCarry::n_folds AFTER this epoch — D1's abscissa.
    long long   age_ep{0};          ///< Loop epochs since the arc's first stash (−1 = warm-resume-restored, 0 = unknown).
    long long   age_since_relevel_ep{0}; ///< Epochs since @ref LocalCarry::relevel_s (−1 = unstamped ⇒ mature).
    bool        folded{false};      ///< Did a carried prior actually act on this column this epoch?
    AmbFoldRoute route{AmbFoldRoute::None}; ///< …and through which route (@ref AmbFoldRoute).
    std::string event;              ///< "" | birth | drop:<cause> | erase:<cause> | stransform.
};

/// One carried-ambiguity entry as a warm resume restores it from a snapshot's
/// `L` / `Z` row — the ONE place the RESTORED-ARC AGE convention is realized.
///
/// The snapshot deliberately does not serialize the arc's birth (see
/// @ref LocalCarry::birth_ep): the documented contract is that a restored entry
/// reads MATURE, because its true birth is at least as old as the snapshot and
/// refusing it would refuse the whole network for @c min_age epochs after every
/// resume.  Leaving the fields at their 0 default did NOT deliver that contract —
/// the harvest's first-stash-wins rule reads 0 as "never stamped" and re-stamps
/// it with the CURRENT loop epoch, so one post-resume harvest re-branded every
/// restored arc as newborn (measured: 390 of 414 candidacy skips read `young` on a
/// 60-epoch leg, SPEC_event_harness §15.4).  The sentinel is what makes "restored"
/// distinguishable from "never stashed" without touching the snapshot format or
/// the warm-resume fingerprint.
///
/// @param value_abs   Carried absolute estimate (m) — the snapshot's field.
/// @param var         Carried posterior variance (m²) — the snapshot's field.
/// @param last_seen_s Continuous-GPS seconds of the stash that wrote the row.
/// @param var_first   First-stash variance if the row carried one (0 = absent).
/// @return A @ref LocalCarry with both birth stamps at their RESTORED sentinels.
inline LocalCarry makeRestoredLocalCarry(double value_abs, double var,
                                        double last_seen_s,
                                        double var_first = 0.0) {
    LocalCarry lc{value_abs, var, last_seen_s};
    lc.var_first = var_first;
    lc.birth_ep  = LocalCarry::kBirthEpRestored;
    lc.birth_s   = LocalCarry::kBirthSRestored;
    return lc;
}

/// Is this carried entry YOUNGER than the maturity bar — ONE definition, two
/// readers (the F3 block admission gate `network.phase_leveling_local_block_min_
/// age_ep` in ApplicationNetworkAssembly, and the cold anchor-fallback candidacy
/// bar `network.phase_datum_candidacy_min_age_ep` in NetworkPhaseDatum).
///
/// The RESTORED-ARC AGE convention lives here: an entry whose @c birth_ep is not
/// POSITIVE has no usable age — 0 (never stashed, or a pre-sentinel vintage) and
/// @ref LocalCarry::kBirthEpRestored (warm-resume-restored) alike — and is never
/// young.  The conservative direction is to let the variance ceiling decide rather
/// than refuse a carry the ledger says has converged.
///
/// @param lc          The station's carried entry for the arc.
/// @param now_ep      This epoch's @c n_loop_epochs.
/// @param min_age_ep  The maturity bar in loop epochs; ≤ 0 disables the test.
/// @return True only when the arc has a positive birth stamp AND fewer than
///         @p min_age_ep loop epochs have elapsed since it.
inline bool localCarryYoung(const LocalCarry& lc, long long now_ep,
                            int min_age_ep) {
    return min_age_ep > 0 && lc.birth_ep > 0
           && (now_ep - lc.birth_ep) < static_cast<long long>(min_age_ep);
}

/// Stamp @p fresh with the arc's birth on the FIRST-STASH-WINS rule — the one
/// definition of the rule the phase-leveling harvest applies at every stash.
///
/// ⚠ The "already stamped" test is `!= 0`, not `> 0` (SPEC_event_harness §15.4).
/// Both non-zero states mean "this arc's birth is already settled": a positive
/// stamp is a real first stash, and @ref LocalCarry::kBirthEpRestored is a warm
/// resume saying "older than this run, true age not snapshotted ⇒ MATURE".  A
/// `> 0` test cannot tell the sentinel from an unstashed 0, which is exactly how
/// the first post-resume harvest used to re-brand every restored arc as born at
/// the resume epoch.
///
/// An erase-and-rebirth (a T4 attribution erase, a gap prune, a re-admission)
/// needs no clause here: it DELETES the entry, so @p prev is null and the arc is
/// stamped with today's epoch — which is the honest answer for a re-born arc.
///
/// @param fresh   The entry about to be stashed (its birth fields are written).
/// @param prev    The station's previous entry for the same key, or null when
///                there is none (a first stash, or a re-stash after an erase).
/// @param now_ep  This epoch's @c n_loop_epochs (≥ 1 at any harvest).
/// @param now_s   This harvest's continuous-GPS seconds (@c pl_now_s).
inline void localCarryStampBirth(LocalCarry& fresh, const LocalCarry* prev,
                                 long long now_ep, double now_s) {
    fresh.birth_ep = (prev && prev->birth_ep != 0) ? prev->birth_ep : now_ep;
    fresh.birth_s  = (prev && prev->birth_s != 0.0) ? prev->birth_s : now_s;
}

/// SPEC_slip_relevel_walk §6 — the slip re-level response's per-epoch state
/// (network.cycle_slip.relevel_free_epoch / relevel_delta_bridge).
///
/// Root defect this exists to fix: a detector's pending-reseed mark is CONSUMED
/// (arc re-seeded, flag cleared) by the PASS-B measurement pre-pass
/// (MeasurementModel::ensureRequiredStates) BEFORE the inject-skip check at the
/// leveling-carry fold ever reads it — so the documented "re-float from data
/// this epoch rather than clamp to the stale carry" NEVER happened for
/// detector-caught slips: the pre-slip level injected at converged weight and
/// the level channels random-walked (~6.5 m ZWD / 6 h at a realistic slip rate,
/// the 60-sta campaign).  @c free_ambs is the latched marked set the inject
/// site consults INSTEAD of the consumed flag; @c mw_cands defers the MW
/// drain's confident WL integers into the IF-TD driver (the only scope holding
/// the pooled IF delta + scale) for the float 2×2 delta bridge; @c touched
/// feeds the harvest-side VCE / prior-residual census guard so a bridged or
/// freed arc's slip-scale innovation is never ingested as a Clean row.
///
/// Lifetime: ONE epoch — filled by the PASS-A serial drains, read by the
/// PASS-B inject + the post-solve harvest, cleared at the next epoch's PASS-A
/// entry.  Deliberately NOT snapshotted (forward-only response policy; a
/// post-resume epoch degrades to the pre-fix path, the conservative direction).
struct MwBridgeCand {
    int         sta{-1};          ///< Station index.
    std::string sat, sig1, sig2;  ///< Satellite + the MW pair's phase signals.
    int         dn_wl{0};         ///< Confident ZD wide-lane integer (cycles).
    double      sigma_wl_cyc{0};  ///< WL confidence (cycles; MW frac, floored).
    double      lambda_wl{0};     ///< WL wavelength (m).
    bool        consumed{false};  ///< Set when the IF-TD driver bridged it.
};
/// Bridge v2 (SPEC_slip_relevel_walk §8.2, pend-and-replace): one pended slip
/// awaiting its MW wide-lane integer.  Created at the T1 flag epoch with the
/// PRE-slip carry capture (the PASS-A carry still holds the previous harvest)
/// and the measured δIF; the interim epochs run the v1 free re-level; when the
/// confident MW integer arrives (same epoch via the candidate index, or 1-2
/// epochs later via the MW drain — the measured arrival lag that left 3 959
/// candidates unpaired in v1) the 2×2 solves and the carry is REPLACED with
/// pre_slip + shift — gated by the kernel caps AND the free-level
/// CORROBORATION (the §8.1 audit's demand: correct bridges err ≤ 0.5 cyc
/// against truth, wrong ones 6–113 cyc, while the interim free level sits
/// within ~2 cyc of truth — agreement with it separates the two populations).
struct RelevelPend {
    double pre1{0}, pre_var1{0};   ///< Pre-slip carry (band 1): value_abs (m), var (m²).
    double pre2{0}, pre_var2{0};   ///< Pre-slip carry (band 2).
    double dif_m{0};               ///< T1-measured IF step, common-mode removed (m).
    double sigma_if_m{0};          ///< Its 1σ (m).
    double lambda_if{0};           ///< (1,1) IF wavelength (m).
    std::string sig1, sig2;        ///< The pair's phase signals.
    double tow_s{0};               ///< Continuous-GPS-s of the flag (expiry base).
    double el_deg{0};              ///< Elevation at the flag epoch (v3: maps the EWL σ floor at a belated fill).
    bool   has_dn{false};          ///< The MW integer has arrived.
    int    dn_wl{0};               ///< Confident ZD wide-lane integer (cycles).
    double sigma_wl{0};            ///< Its 1σ (cycles, floored).
    double lambda_wl{0};           ///< Wide-lane wavelength (m).  v3: ALSO set
                                   ///  (without has_dn) at pend creation when the
                                   ///  GF row is captured — the kernel derives the
                                   ///  per-band frequencies from (λ_WL, λ_IF)
                                   ///  alone, so the E1+E4 route needs it too.
    // --- Bridge v3, E4: the GF-phase TD row on the SAME pair -----------------
    // d_gf = λ1·ΔN1 − λ2·ΔN2 + (α2−α1)·δI + ε (geometry/clock-free, undifferenced).
    // Its σ is EMPIRICAL (the station's own clean-step EWMA gf_step_scale, which
    // naturally includes the iono-TD content), never a model σ.  Together with
    // δIF it closes the (1,2) plane at 0.15-0.3 cyc without waiting for MW —
    // whenever σ_gf ≲ 0.75·σ_IF the GF row is worth as much as a CORRECT WL
    // integer, with no integer-wrongness risk.
    bool   has_gf{false};
    double dgf_m{0};               ///< T1-measured ΔGF across the slip epoch (m).
    double sigma_gf_m{0};          ///< Its 1σ (m; empirical, floored).
    // --- Bridge v3, E3: the EWL-MW integer slot (band-3 coverage) ------------
    // dn_ewl_b3 is NORMALIZED at fill time to ΔN(band3) − ΔN(shared IF band)
    // regardless of the candidate's frequency-sorted pair order — the GAL EWL
    // (L7Q,L5Q) is sign-flipped relative to GPS (L2W,L5Q) in raw candidate
    // order, and a missed flip passes every σ cap (the panel's #1 defect).
    bool   has_ewl{false};
    int    dn_ewl_b3{0};           ///< ΔN(band3) − ΔN(shared band), cycles.
    double sigma_ewl{0};           ///< Its 1σ (cycles; el-mapped floor).
    double lambda_ewl{0};          ///< EWL wavelength (m).
    int    ewl_shared_band{0};     ///< Which IF-pair band the EWL pair shares (1 or 2).
    bool   ewl_b3_hi{false};       ///< Band 3 is the higher-frequency band of the EWL pair.
    std::string sig3;              ///< The third band's phase signal ("L5Q"/"L7Q").
    bool   has_pre3{false};        ///< Band-3 pre-slip carry captured at the FLAG epoch
                                   ///  (a belated capture would read the interim free
                                   ///  re-level, not the pre-slip level — never do it).
    double pre3{0}, pre_var3{0};   ///< Pre-slip carry (band 3).
};

struct RelevelEpochState {
    /// §7.6(c) freeing-cause bits for the lane observatory.  A bitmask because
    /// one (station, amb key) can be freed by several detectors in the same
    /// epoch (an LLI assertion corroborated by an MW step marks twice); the
    /// T4 bits are used only by the RAIM-side pre-erase capture (§7.6(d)) —
    /// the RAIM path never touches @c free_ambs (verified §7.6 confound (1)).
    enum : std::uint8_t {
        kCauseMw        = 1,   ///< MW wide-lane integer-jump drain.
        kCauseLli       = 2,   ///< Receiver LLI loss-of-lock drain.
        kCauseIfTd      = 4,   ///< IF-phase time-difference pool drain.
        kCauseT4Attrib  = 8,   ///< RAIM prominence-ATTRIBUTION erase (T4).
        kCauseT4Consens = 16,  ///< RAIM per-satellite CONSENSUS erase (T3).
        kCauseWindup    = 32,  ///< Wind-up accumulator RESTART response (§10.4).
        kCauseGap       = 64,  ///< Gap RE-ENTRY response (Card G / §G3-W1): the arc came back across a tracking break the raw level, or the receiver's LLI, says it did not survive.
    };
    /// (station idx, amb key) marked pending-reseed by a detector this epoch —
    /// the inject site skips these (the free epoch the mark always promised).
    std::set<std::pair<int, std::string>> free_ambs;
    /// §7.6(c) cause tag, parallel to @c free_ambs (same one-epoch lifetime;
    /// measure-only — nothing downstream reads it but the lane observatory).
    std::map<std::pair<int, std::string>, std::uint8_t> free_cause;
    /// §7.7 handover cross-guard: satellites whose datum-station slip
    /// re-reference APPLIED this epoch (network-wide carry erase + reseed —
    /// a family frame step on every signal).  Same one-epoch lifetime; the
    /// apply edge runs in PASS A after clear(), the T4 measurement reads it
    /// at the harvest.  Measure-only.
    std::set<std::string> datum_reref_sats;
    /// Deferred MW candidates for the IF-TD-driver delta bridge.
    std::vector<MwBridgeCand> mw_cands;
    /// Arcs whose carry was delta-bridged this epoch (station, sat, signal) —
    /// harvest-side statistics guard (exclude from LS-VCE, relabel prires).
    std::set<std::tuple<int, std::string, std::string>> bridged;

    /// SPEC_lane_slip_response §7.3 / rung 3 v3 — the CARRY-DOMAIN lane
    /// observatory's per-(station, satellite) raw-MW input for THIS epoch.
    ///
    /// Why the MW delta travels here rather than being read at the measuring
    /// site: the raw MW is a pure observable, so it is unaffected by the eq34
    /// D4 / phase-datum S-transforms that shift @c LocalCarry::value_abs
    /// between PASS A and the harvest.  Carrying only the OBSERVABLE across
    /// the seam, and reading BOTH lane levels inside the harvest, keeps the
    /// pre- and post-event lanes in one gauge by construction — which is the
    /// hazard that has to be designed out rather than interlocked around.
    ///
    /// Lifetime: ONE epoch (same contract as @c free_ambs).  Populated by the
    /// PASS-A raw-observable pass when the observatory is armed; consumed by
    /// the harvest; cleared at the next PASS-A entry.  Not snapshotted.
    struct LaneMw {
        std::string sig1, sig2;    ///< The raw pass's primary (lo, hi) band pair.
        double lam1_m{0.0};        ///< sig1 wavelength (m).
        double lam2_m{0.0};        ///< sig2 wavelength (m).
        double lambda_wl_m{0.0};   ///< Wide-lane wavelength 1/(1/lam1 - 1/lam2) (m).
        double d_mw_cyc{0.0};      ///< ΔMW across the epoch in WL CYCLES (d_mw / lambda_wl_m).
        double el_deg{0.0};        ///< §7.6(a) elevation at this epoch (deg; 0 = unavailable).
        bool   measured{false};    ///< An adjacent-epoch predecessor existed, so the step is real.
        bool   pair_changed{false};///< That predecessor used a DIFFERENT band pair — d_mw_cyc is not in this lane's cycles.
    };
    std::map<std::pair<int, std::string>, LaneMw> lane_mw;

    /// §7.6(a) one per-event record of the lane observatory — one row per
    /// ATTEMPT (measured AND skipped; the skip channel is the outcome).  Filled
    /// by the harvest, emitted onto the EXISTING events stream by the leveling
    /// seam right after the harvest returns (the LV-2 precedent: the trace
    /// sinks and the epoch time live on that side of the seam), then cleared —
    /// belt-and-suspenders, clear() wipes it too.  Measure-only.
    struct LaneObsRow {
        int          sta{-1};        ///< Station index.
        std::string  sat;            ///< RINEX satellite id.
        std::string  sig1, sig2;     ///< The measured band pair (carry namespace; empty when unresolved).
        std::uint8_t cause{0};       ///< Freeing-cause bitmask (kCause*).
        bool         t4{false};      ///< §7.6(d) T4-side row (pre-erase capture), not a detector-freed one.
        bool         vertex_wide{false}; ///< Both bands freed (detector rows; T4 rows always vertex-wide).
        double       el_deg{0.0};    ///< Elevation at the measuring epoch (deg; 0 = unavailable).
        double       delta_cyc{0.0}; ///< The lane delta (cycles; only valid when measured).
        double       d_mw_cyc{0.0};  ///< The raw ΔMW in WL cycles (0 when unmeasured).
        bool         measured{false};///< True = delta valid; false = see outcome.
        const char*  outcome{"measured"}; ///< "measured" or the skip channel (string literals only).
    };
    std::vector<LaneObsRow> lane_obs_rows;

    void clear() {
        free_ambs.clear(); free_cause.clear(); datum_reref_sats.clear();
        mw_cands.clear(); bridged.clear(); lane_mw.clear(); lane_obs_rows.clear();
    }
};

/// One present, carry-eligible (non-pivot) ambiguity this epoch, recorded in
/// PASS B for the post-solve harvest/stash: its A_r column, identity, and the
/// linearization point used this epoch (so the absolute value is
/// @c x_lin + x_r(col) and a carried prior targets @c value_abs − x_lin).
struct PlAmb {
    Eigen::Index col;     ///< A_r column of this ambiguity (= h2local[amb_idx]).
    std::string  sat;     ///< RINEX satellite id.
    std::string  signal;  ///< RINEX signal / IF combo.
    double       x_lin;   ///< This epoch's ambiguity linearization point (state value).
    std::string  amb_key; ///< Full ambiguity key (clears a one-shot IF-phase refloat mark post-stash).
    /// RF-1 step 4: this LOS's elevation (degrees) this epoch, or −1 when the station's
    /// modeled observation set did not carry it.  Recorded at the inject ONLY when
    /// @c network.amb_float_birth_var_m2 is armed (the map walk is otherwise skipped
    /// entirely), and read by the harvest to map the BIRTH-PRIOR FLOOR exactly as the
    /// code row weights are mapped — σ²(e) = σ²_zen / sin^{2p}(e).  A flat zenith floor
    /// would be optimistic at precisely the low elevations that own the metre-plus tail.
    double       elev_deg{-1.0};
};

/// ---------------------------------------------------------------------------
/// TWO-EDGE ANCHOR HANDOVER (docs/SPEC_pivot_visibility_gauge_defect.md §5.1)
/// ---------------------------------------------------------------------------
/// The phase-bias gauge is one constraint per (sat,signal): "station r*'s
/// ambiguity for this key = 0".  When r* changes — the clock pivot stopped
/// seeing the satellite, or the anchor election moved — re-pinning the SUCCESSOR
/// at 0 realizes a DIFFERENT frame, and the whole family steps by the
/// inter-station ambiguity difference.  The satellite clock absorbs it.
///
/// Measured on a truth-clean 24-station fixture: a legacy pivot losing G15
/// stepped the emitted clock -12.06 m and never recovered; the prioritized
/// election converts that into 15 smaller steps instead of one large one.  Both
/// are ONE-EDGE events — the gauge is changed with no S-transform.
///
/// The fix is to hand the pin over AT THE INCOMING ANCHOR'S OWN VALUE IN THE
/// OUTGOING FRAME, so the realization is continuous by construction.  That value
/// is @ref AnchorHandoverLedger, harvested post-solve one epoch earlier.
///
/// ⚠ WHY A LEDGER AND NOT `getStateVector()(amb_idx)`.  That is `x_lin` — a
/// LINEARIZATION FRAME, not an estimate.  The network solve is never written
/// back into the per-station StateManager (`setStateVector` runs once, at
/// setup), and an `x_lin` shift on a local column is a proven exact solve
/// no-op, so pinning to it would carry no information about where the arc
/// actually sits.  The absolute estimate is `x_lin + back-substituted Δx`, which
/// only exists after the solve — hence the one-epoch deferral, the same shape as
/// `carry_innov_pending` / `dw_release_pending` / `raim_refloat_pending`.
struct AnchorHandoverLedger {
    /// (station index, ambiguity key "sat_signal") -> that station's ABSOLUTE
    /// ambiguity estimate from the PREVIOUS epoch's solve, in that epoch's
    /// realized frame.  Rebuilt wholesale each epoch: a one-epoch lifetime is
    /// the bound on what a decoupled gauge may carry (invariant #7).
    /// value = the absolute estimate; var = its conditional variance (m²).  The
    /// VARIANCE is not decoration: the pin is a ~1e10-weight hard constraint, so
    /// stamping an ill-determined estimate in is worse than re-zeroing — measured,
    /// a +17.08 m target taken from an unvalidated arc produced a 378-sigma residual
    /// spike one epoch later.  Invariant #7's re-coupling edge must RE-VALIDATE
    /// what it reconnects; this is what makes that check possible.
    struct Entry {
        double value{0.0};
        double var{-1.0};   ///< < 0 = unavailable, treated as unusable.
        double var_first{0.0};  ///< the arc's FIRST-STASH conditional variance (m²; 0 = unknown ⇒ the ratio ceiling refuses it) — network.phase_datum_handover_max_var_ratio's denominator.
    };
    std::map<std::pair<int, std::string>, Entry> value_abs;
    /// True once a harvest has run, so the election can tell "armed but the
    /// first epoch has no history yet" from "not armed".
    bool primed{false};
};

/// One anchor handover, reported out of the election for the caller's
/// @c trace_events rows.  Purely observational — emitting it never changes which
/// station is elected or what target is pinned.  Both EDGES are in one record on
/// purpose: a handover with only its incoming half logged is exactly the silent
/// re-referencing invariant #7 exists to forbid.
/// RF-2 item 1 — ONE entry of the HELD S-BASIS: which station's ambiguity realizes a
/// (satellite, signal) phase-bias datum, at what CONSTANT value, and since when.
///
/// The card's sentence: "the anchor station's ambiguity for each pinned (satellite,
/// signal) is the S-basis — HELD at its value at election, not carried as a float
/// state".  RF-1 made every ambiguity a live state and, with it, the one that IS the
/// datum: on the field it converged, dropped, and was re-flagged by T4 454 times an
/// hour, and every one of those moves is a phase-bias re-realization the consumer has
/// to re-converge through.  The level vehicle's frozen zero-q anchor had been supplying
/// this property by accident; this makes it deliberate.
///
/// @c value is FROZEN at election — for a CONTINUOUS handover it is the successor's
/// carried level in the outgoing frame (so the family does not step), for a COLD one it
/// is 0 (so the family DOES step, by exactly @c value_prev, which item 2 declares).  It
/// is never re-derived mid-tenure: that re-derivation is precisely the churn.
struct PhaseBasisEntry {
    std::string station;        ///< The station whose ambiguity holds the datum.
    double      value{0.0};     ///< The CONSTANT the column is pinned at (m), frozen at election.
    long long   elected_ep{0};  ///< Loop epoch of the election that set it (tenure clock).
    bool        cold{false};    ///< True when the election could not inherit a level (value = 0 and the family stepped).
    double      prev_value{0.0};///< The OUTGOING basis value this one replaced (m) — item 2's Δ.
    std::string prev_station;   ///< …and the station that held it ("" = first sighting).
};

struct AnchorHandoverEvent {
    std::string amb;            ///< "sat_signal" (the ambiguity key).
    std::string from;           ///< Outgoing anchor station; empty = first pin (no prior frame).
    std::string to;             ///< Incoming anchor station.
    double      target_m{0.0};  ///< Pin target applied to the incoming anchor (m).
    bool        continuous{false}; ///< true = frame preserved; false = COLD re-realization.
    double      var{-1.0};        ///< the successor's ledger conditional variance (m²; < 0 = no ledger entry) — traced when the ratio ceiling is armed.
    double      var_first{0.0};   ///< the successor arc's first-stash variance (m²; 0 = unknown).
    /// COLD ANCHOR-FALLBACK (network.phase_datum_candidacy_usable, SPEC_event_harness
    /// §15): how many higher-ranked candidates the usable-first pass SKIPPED before
    /// this successor, and the first one it skipped.  0 / empty when the knob is off
    /// (the pass is not entered) — so the storm arm's cold-handover census can be
    /// re-run from the trace alone instead of from a second run.
    int         cand_skipped{0};
    std::string cand_first_unusable{};
};

/// One present station ZWD column this epoch (B9): its A_r column and the ZWD
/// linearization point, for the post-solve harvest/stash (mirrors @ref PlAmb).
struct PlZwd {
    Eigen::Index col;     ///< A_r column of the ZWD state (= h2local[tropo_idx]).
    double       x_lin;   ///< This epoch's ZWD linearization point (state value).
};

/// RX-CLK-in-block (@c network.carry_rx_clock_in_block): one of a station's carried
/// RECEIVER-CLOCK-family local columns as PASS B recorded it — the primary receiver
/// clock (@c kind empty) or one per-system ISB (@c kind = the system name).  The
/// record is written for EVERY present column, folded or not, so the post-solve
/// harvest can stash the new absolute value even on the epochs the carry could not
/// act (the @c pl_zwd_present pattern).
/// RX-CLK-in-block (@c network.carry_rx_clock_in_block): the carried receiver clock's
/// one-step @b (bias, drift) prediction.
///
/// ⚠ SHARED BY BOTH EDGES ON PURPOSE.  PASS B needs it to build the injected prior
/// (mean and @c var_eff); the post-solve harvest needs the SAME prediction to form the
/// innovation its clock-JUMP gate tests.  If the two ever computed it separately they
/// would drift apart silently — the injected prior would be tested against a different
/// prediction than the one that was injected — so the algebra lives here, once.
struct RxClkPred {
    double mean{0.0};  ///< value_abs + rate*gap  (m)
    double var{0.0};   ///< var + q*gap_ep + rate_var*gap^2  (m^2)
};

/// RX-CLK-in-block: propagate a carried receiver clock over @p gap_s seconds.
///
/// The state is (bias, drift).  The bias is a random walk of @p q_m2 per EPOCH — the
/// gap is supplied in epochs (@p gap_ep) as well as seconds because the knob's unit is
/// per-epoch while the drift's contribution is per-second-squared.  The drift's own
/// uncertainty enters as @f$\sigma_d^2\,\Delta t^2@f$; the (bias, drift) cross
/// covariance is deliberately NOT carried, which makes the returned variance an upper
/// bound whenever that correlation is positive — conservative, i.e. the prior is never
/// stiffer than the information supports.
///
/// An ISB column calls this with @p rate = @p rate_var = 0: a pure random walk.
///
/// @param value_abs Last harvested ABSOLUTE value (m) — x_lin + x_r, never a correction.
/// @param var       Its harvested conditional variance (m^2).
/// @param rate      Carried drift (m/s); 0 when unknown or not applicable.
/// @param rate_var  Its variance (m^2/s^2); the large @c carry_rx_clock_rate_var0 until
///                  the harvest has folded one increment.
/// @param gap_s     Elapsed time since @c value_abs was harvested (s).
/// @param gap_ep    The same gap expressed in EPOCHS (>= 1), for the per-epoch q.
/// @param q_m2      Bias random-walk variance per epoch (m^2).
/// @return The predicted mean and variance.
inline RxClkPred rxClockPredict(double value_abs, double var, double rate,
                                double rate_var, double gap_s, double gap_ep,
                                double q_m2) {
    RxClkPred p;
    p.mean = value_abs + rate * gap_s;
    p.var  = var + q_m2 * gap_ep + rate_var * gap_s * gap_s;
    return p;
}

/// RX-CLK-in-block: scalar Kalman update of the carried DRIFT from one harvested
/// increment.
///
/// @param rate      [in,out] the drift estimate (m/s).
/// @param rate_var  [in,out] its variance (m^2/s^2).  A value < 0 on entry means
///                  "never updated" and the caller must pass the configured
///                  @c carry_rx_clock_rate_var0 instead — this function assumes a
///                  finished prior.
/// @param obs_rate  the increment's implied rate, (c_k - c_{k-1}) / dt (m/s).
/// @param r_rate    that increment's own noise variance, (var_k + var_{k-1} +
///                  q*gap_ep) / dt^2 (m^2/s^2).
/// @param rate_q_ep the drift's process noise over this gap (m^2/s^2).
/// @return true when the update was applied (a non-degenerate denominator and a finite
///         observation); false leaves both arguments untouched.
inline bool rxClockRateUpdate(double& rate, double& rate_var, double obs_rate,
                              double r_rate, double rate_q_ep) {
    if (!std::isfinite(obs_rate) || !std::isfinite(r_rate) || r_rate < 0.0) return false;
    const double var_pred = rate_var + rate_q_ep;
    const double denom = var_pred + r_rate;
    if (!(denom > 0.0) || !std::isfinite(denom)) return false;
    const double k = var_pred / denom;
    rate     += k * (obs_rate - rate);
    rate_var  = (1.0 - k) * var_pred;
    return true;
}

struct PlRxClk {
    Eigen::Index col;      ///< A_r column of the state (= h2local[state index]).
    double       x_lin;    ///< This epoch's linearization point.  ⚠ For the clock this is a RUN-CONSTANT: the network producer never writes its solution back into the station StateManager, so the whole time-varying clock lives in x_r.
    std::string  kind;     ///< "" = the primary receiver clock (the one with the carried DRIFT); otherwise the ISB's system name ("Galileo", ...), which takes a pure random walk.
};

/// PASS B produce-phase result for one station: the assembled local Jacobian
/// split plus the disposition flags the serial consume phase needs.  Each slot
/// is written by exactly one thread (indexed, never push_back) and consumed
/// serially in station order, so est.addStation sees identical inputs in an
/// identical order regardless of thread count — the determinism guarantee.
struct StationAssembly {
    bool             ok{false};            ///< Produced a usable H/A_s/A_r block.
    bool             empty_innov{false};   ///< computeMeasurementUpdate yielded no rows.
    bool             state_mismatch{false};///< H column count != state size.
    bool             threw{false};         ///< computeMeasurementUpdate / splitStation threw.
    Eigen::MatrixXd  A_s, A_r;             ///< Shared-border + local-nuisance Jacobian.
    Eigen::VectorXd  w, y;                 ///< Per-row information weights + innovations.
    int              np{0};                ///< Pivot ambiguity pins folded in (pivot only).
    std::map<int,int> h2local;             ///< H-column -> A_r-column map (back-substitute the slant-iono nuisance for the viz trace).
    /// Card OWD (O5): the observation's OWN signal of each data row [0, H.rows()) — the
    /// measurement's @c signal_types, row-aligned with H by the assembly's invariant; rows
    /// appended later (datum pins, ionosphere ties, coupling rows) have none.  Filled only
    /// under @c network.trace_postfit_rows (the only reader is the post-fit row writer, which
    /// used to name a row's signal from the first labelled column and so wrote an empty
    /// signal for a row whose only shared column is the clock).
    std::vector<std::string> row_sig;
    std::string      err;                  ///< Diagnostic (mismatch detail or exception text).
    // Phase-leveling carry (Increment 1) — built in PASS B, consumed serially.
    std::vector<Eigen::Index> carry_cols;  ///< A_r columns receiving a carried prior (ASCENDING — determinism).
    Eigen::VectorXd           carry_dNrr;  ///< Per-column information increment Λ = 1/var_eff.
    Eigen::VectorXd           carry_dur;   ///< Per-column RHS increment η = Λ·(value_abs − x_lin).
    // Card RF-PD-fix (network.station_prior_before_pd): the station's own DATUM rows,
    // held SEPARATE from the carry above so they can be handed to addStation and folded
    // into N_rr/u_r BEFORE its positive-definiteness test — the test otherwise judges a
    // station without the only row that gauges its receiver DCB and drops it for lacking
    // it (docs/refound/RF-PD_station_rejection_2026-09-15.md).  A row lands in EXACTLY
    // one of the two vectors: with the knob OFF the RXCB-DATUM rows stay in carry_* at
    // their historical position in the coalesce (byte-identical), with it ON they move
    // here and are withheld from the later addStationPrior call, which does not sum.
    // Empty ⇒ nothing to fold early.
    std::vector<Eigen::Index> prior_cols;  ///< A_r columns receiving a pre-PD datum row (ASCENDING).
    Eigen::VectorXd           prior_dNrr;  ///< Per-column information increment Λ = 1/σ².
    Eigen::VectorXd           prior_dur;   ///< Per-column RHS increment η = Λ·(0 − x_lin).
    // Card 1 (network.carry_conditional_coupling): the appended conditional coupling
    // rows live IN A_s/A_r (not in carry_cols), so the within-epoch RAIM refloat must
    // know which rows to withhold.  cpl_row_start = the first A_s/A_r row index of the
    // coupling block (−1 when none); cpl_row_local_cols = the carried LOCAL column each
    // coupling row targets, in append order (so a refloated column's row can be zeroed).
    Eigen::Index              cpl_row_start{-1};
    std::vector<Eigen::Index> cpl_row_local_cols;
    // Card 1b (network.carry_coupling_joint_whiten): the coupling rows' pre-whitening
    // ingredients + the per-M_w-column marginalization variances, retained so the
    // within-epoch RAIM refloat can re-whiten the surviving rows (the whitening mixes
    // rows within the station, so a refloat can't just zero one output row).  Empty on
    // the Card-1 scalar path.  cpl_joint_fallback = the joint LLT was non-SPD after the
    // ridge retry → this station fell back to the scalar path this epoch (run WARN count).
    std::vector<CplRowRaw>    cpl_raw;
    std::vector<double>       cpl_white_pw;
    bool                      cpl_joint_fallback{false};
    // Card 1/1d observability: coupling-term counts of THIS station-epoch's injected
    // rows — layout-present terms, departed (marginalized) terms, and present terms
    // κ-shrunk by the border time update.  Summed serially into run totals.
    long long                 cpl_terms_present{0};
    long long                 cpl_terms_departed{0};
    long long                 cpl_terms_shrunk{0};
    /// Card 1f (network.carry_coupling_gauge_project): (constellation,kind) families whose
    /// coefficient sum was HELD at its harvest value against a truncated column, and those
    /// with truncated mass but NO present recipient to carry it (the row stays
    /// gauge-dirty for that family — the residue this card cannot remove this epoch).
    /// Both stay 0 with the card off, so a nonzero pair is proof the wiring ran.
    long long                 cpl_gauge_held{0};
    long long                 cpl_gauge_orphan{0};
    /// Orphan ATTRIBUTION (owed by the 2026-08-19 multi-constellation arm, which
    /// measured the first nonzero orphan count — 62, all multi-GNSS): per
    /// (constellation, SharedParamKind) family, the orphan COUNT and the summed
    /// |truncated mass| left gauge-dirty.  Recipient-fallback DECISION (recorded
    /// here so nobody re-derives it): there is NO fallback — the coefficient sums
    /// realize per-constellation because the datum does, so holding an orphaned
    /// family's mass against the row's OTHER constellation family would move a
    /// different datum's gauge.  The residue stays counted, attributed, and
    /// unfixed by construction; a fallback needs its own derivation card.
    std::map<std::pair<char,int>, std::pair<long long,double>> cpl_orphan_fam;
    /// Card 1g (network.carry_coupling_reinit_sources): coupling terms diverted to the
    /// departed route by the WIDENED (kind,sat,signal) probe specifically — i.e. terms the
    /// legacy sat-wide probe would have folded as present.  This is the card's own
    /// denominator: the run-total departed count moves for many reasons and cannot
    /// distinguish armed-and-working from armed-and-inert.
    long long                 cpl_reinit_diverted{0};
    /// READMIT re-coupling edge (network.readmit_coupling_marg_ep): coupling terms diverted
    /// to the departed route because their satellite is inside its post-re-admission
    /// marginalization window — this station-epoch.
    long long                 cpl_readmit_diverted{0};
    /// SPEC_slip_relevel_walk: inject-site free-epoch skips actually taken this
    /// station-epoch (the arc had a live carry the latched set suppressed).
    long long                 relevel_free_skips{0};
    /// SPEC_withhold_era_release §8: carried leveling priors this station SKIPPED
    /// because their gap exceeded @c phase_leveling_max_gap_s.  This — not the
    /// post-harvest prune — is the "cliff" mechanism: the entry is not erased,
    /// it is simply not injected, so the arc solves FREE and is re-stashed from
    /// one epoch of code.  Accumulated per station in the PARALLEL produce phase
    /// and folded into the run counters SERIALLY afterwards, exactly as
    /// @c relevel_free_skips is.
    long long                 inject_stale_skips{0};
    /// SIM-ONLY ORACLE AMBIGUITY PIN (filter/NetworkOraclePin.hpp): this
    /// station-epoch's pin accounting, accumulated in the PARALLEL produce phase
    /// and folded into the run counters SERIALLY afterwards, exactly as
    /// @c relevel_free_skips is.  All four are 0 on every run with no
    /// `oracle_pin` event armed (the table pointer is null, the block is
    /// unreachable).  Closure: `opin_offered == opin_rows + opin_gauge_skipped +
    /// opin_no_column`.
    long long                 opin_offered{0};        ///< Ambiguity columns of this station found in the pin table.
    long long                 opin_rows{0};           ///< …folded as a diagonal pin prior row.
    long long                 opin_gauge_skipped{0};  ///< …skipped: the arc is GAUGE-PINNED (an anchor), whose 1e10 datum pin owns the family's frame.
    long long                 opin_no_column{0};      ///< …skipped: the column is not in this epoch's A_r (or its x_lin / the row is non-finite).
    /// F1 (network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount): per
    /// INJECTED (sat, signal) ambiguity column, the B_eff the inject multiplied its
    /// carried variance by — the count of carried ambiguity columns injected on that
    /// (station, satellite) LOS this epoch.  Filled only when the knob is armed
    /// (empty OFF); read by the serial harvest so its fold-variance mirror (carry-VCE
    /// / prires standardization) reports the weight the fold ACTUALLY used.
    std::map<std::pair<std::string, std::string>, int> pl_los_beff;
    /// RF-1 §7.4 (@c network.trace_amb_state): per FOLDED (sat, signal) ambiguity
    /// column of this station-epoch, the variance the fold ACTUALLY used and the
    /// ROUTE it took.  Filled only when the trace is armed (empty OFF — the map is
    /// never touched, so an unarmed run pays one bool test per column and nothing
    /// else); read by the serial harvest, which owns the row write.  Exactly the
    /// @c pl_los_beff discipline: recorded in the PARALLEL produce phase into this
    /// station's own object, consumed serially afterwards.
    ///
    /// A column ABSENT from this map was not folded this epoch — the harvest writes
    /// its row with @c folded = 0 and names the cause in @c event.  That is the
    /// population D2 and D3 exist to count, so the absence is the record, not a gap.
    std::map<std::pair<std::string, std::string>, std::pair<double, AmbFoldRoute>>
        amb_state_fold;
    long long                 pl_los_scale_rows{0};            ///< F1: rows whose variance was multiplied by a B_eff > 1 (produce-phase local; summed serially).
    long long                 pl_los_scale_hist[4]{0, 0, 0, 0}; ///< F1: LOS groups by B_eff = 1 / 2 / 3 / ≥4 (produce-phase local; summed serially).
    /// F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount): this
    /// station-epoch's jointly-whitened leveling-carry block ingredients — empty when
    /// the knob is off, the station had no injectable carried column, or the block's
    /// factorization fell back to per-column rows.  Read by the within-epoch RAIM
    /// refloat, which re-whitens the SURVIVING columns (the whitening mixes the
    /// block's rows, so a refloat cannot zero one output row) — the same contract
    /// as @c cpl_raw for Card 1b.  All F3 counts below are produce-phase local and
    /// summed serially, the pl_los_scale discipline.
    PlBlockRaw                pl_blk;
    long long                 pl_blk_blocks{0};        ///< F3: blocks whitened (0 or 1 per station-epoch).
    long long                 pl_blk_rows{0};          ///< F3: whitened rows emitted.
    long long                 pl_blk_marg_cols{0};     ///< F3: carried entries of this station NOT injected this epoch (departed / stale / freed / reseed / non-finite) — marginalized out of the block.
    long long                 pl_blk_pair_missing{0};  ///< F3: (i,j) pairs zeroed because the two columns were harvested at different epochs (or one carries no harvest block).
    long long                 pl_blk_col_refused{0};   ///< F3: block columns dropped by the scalar-row "never poison" screen (non-finite RHS / σ² ≤ 0).
    long long                 pl_blk_fallback{0};      ///< F3: block factorization failed after the ridge retry → this station-epoch emitted F1/diagonal rows instead.
    /// RF-1 step 3 (design risk #3): the DIMENSION of the block that failed to factorize
    /// this station-epoch, 0 when none did.  Under the @c float_kf vehicle a fallback is
    /// not a degradation of an optional knob — it is a VEHICLE CHANGE on one
    /// station-epoch — so the serial scatter logs it at WARN with the station's identity
    /// and this dimension, and the run-end bar fails the run above 1 % of attempted
    /// station-epochs.  Produce-phase local, like every other @c pl_blk_* count.
    long long                 pl_blk_fallback_dim{0};
    long long                 pl_blk_young_cols{0};    ///< F3 age gate (network.phase_leveling_local_block_min_age_ep): injectable columns YOUNGER than the gate this station-epoch — kept OUT of the block and emitted as per-column rows (F1-scaled if armed).
    long long                 pl_blk_yng_scaled{0};    ///< AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2): block columns whose row/column of Σ was CONGRUENCE-scaled this station-epoch.  Read against @c yng_cols below: a gap means columns were computed young but their block never reached the scaling (refused by the scalar screen, or the station took the per-column route).
    // RX-CLK-in-block (network.carry_rx_clock_in_block): the station's receiver-clock
    // (and ISB) columns carried INSIDE the same F3 block.  Like the iono card and
    // UNLIKE the ZWD card there is deliberately NO diagonal fallback row — a diagonal
    // prior on the most station-COMMON direction of all is precisely the n_LOS
    // over-count that falsified the diagonal ZWD carry — so `pl_blk_rxclk_not_folded`
    // is a true "carried nothing this station-epoch" count.
    long long                 pl_blk_rxclk_cols{0};          ///< Receiver-clock-family columns ADMITTED into the block build this station-epoch (survived the scalar "never poison" screen and became rows of Sigma).  A block that then failed to factorize still counts here — read it against @c pl_blk_rxclk_not_folded.
    long long                 pl_blk_rxclk_isb_cols{0};      ///< …of which are ISB columns (network.carry_rx_clock_isb).  0 is NOT an alarm: a GPS-only or BSSD posture allocates no per-system time-offset state.
    long long                 pl_blk_rxclk_not_folded{0};    ///< Clock-family columns PUSHED but not folded: the block was non-SPD this station-epoch and this card has no diagonal row to fall back to, so their carry did not act.
    long long                 pl_blk_rxclk_missing_cross{0}; ///< (clock-family, other) pairs zeroed for a missing harvest cross term — a SUBSET of @c pl_blk_pair_missing, split out so the clock carry's own coupling can be read separately.
    long long                 pl_blk_rxclk_gap_drop{0};      ///< Clock-family carries REFUSED at inject because the sighting gap exceeded network.carry_rx_clock_max_gap_s (invariant #1's absent-station edge; the harvest prune then erases them).
    /// RX-CLK-in-block: the receiver-clock-family columns PRESENT this station-epoch, in
    /// push order — the post-solve harvest's record of what to stash.  Written for every
    /// present column whether or not its carry was folded.
    std::vector<PlRxClk>      pl_rxclk;
    /// RX-CLK-in-block: the A_r columns of the clock-family entries that were PUSHED into
    /// this station-epoch's block.  Read by the post-fit RAIM pool census to recognise the
    /// whitened clock block rows (@c n_raim_pool_rxclk_rows); empty OFF, on every
    /// station-epoch that pushed none, and on a non-SPD block (no whitened row exists).
    std::vector<Eigen::Index> pl_rxclk_blk_cols;
    // AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2,
    // SPEC_amb_iono_split_overcount §6.17–§6.18) — per-station telemetry, folded
    // serially like every other block counter.  Counted at the COMPUTE site (once per
    // carried ambiguity column, whichever route then folds it), so it is the population
    // the card acted on, not a per-route split.
    long long                 yng_cols{0};             ///< column-epochs handed a strictly positive var_add this station-epoch.
    long long                 yng_skip_age{0};         ///< STAMPED column-epochs refused for age > max_age_ep (an UNSTAMPED entry — relevel_s == 0 — is treated as mature and counted in neither).
    double                    yng_var_add_sum{0.0};    ///< Σ of those var_add values (m²).
    double                    yng_age_sum{0.0};        ///< Σ of their ages (loop epochs); ÷ yng_cols = this station-epoch's mean inflated age.
    long long                 pl_blk_hist[5]{0, 0, 0, 0, 0}; ///< F3: block dimension histogram <10 / 10–24 / 25–49 / 50–74 / ≥75.
    long long                 pl_blk_max_dim{0};       ///< F3: largest block dimension this station-epoch.
    // B9-in-block (network.carry_zwd_in_block): the ZWD column carried INSIDE the F3 block.
    long long                 pl_blk_zwd_cols{0};           ///< ZWD column ADMITTED into the block build this station-epoch (0 or 1): it survived the scalar "never poison" screen and became a row of Σ.  A block that then failed to factorize still counts here — read it against @c pl_blk_fallback and @c pl_blk_zwd_fallback_diag, which say how many of those admissions did not end up folded.
    long long                 pl_blk_zwd_fallback_diag{0};  ///< ZWD fell back to the B9 DIAGONAL row this station-epoch (no block, or the block's factorization failed).
    long long                 pl_blk_zwd_missing_cross{0};  ///< (ambiguity, ZWD) pairs zeroed for a missing harvest cross term — a SUBSET of pl_blk_pair_missing, split out so the ZWD's own coupling can be read separately.
    /// B9-in-block: true when this station-epoch's ZWD folded through the whitened block
    /// (so @c pl_zwd.col names one of the block's local columns and the block row whose
    /// @c cpl_row_local_cols entry equals it IS the ZWD row).  The post-fit RAIM pool
    /// reads this to count how many pool-admitted rows are that row.
    bool                      pl_zwd_in_block{false};
    // I-in-block (network.carry_iono_in_block): the station's per-LOS slant-iono columns
    // carried INSIDE the same F3 block.  There is deliberately NO diagonal fallback row
    // for these — a diagonal per-LOS iono prior IS the falsified QUEUE-8(b) vehicle — so
    // `pl_blk_iono_not_folded` is a true "carried nothing this station-epoch" count, not
    // a "degraded to the old row" count.
    long long                 pl_blk_iono_cols{0};          ///< Per-LOS iono columns ADMITTED into the block build this station-epoch (survived the scalar "never poison" screen and became rows of Σ).  A block that then failed to factorize still counts here — read it against @c pl_blk_iono_not_folded.
    long long                 pl_blk_iono_not_folded{0};    ///< Iono columns PUSHED but not folded: the block was non-SPD (or was never built) this station-epoch, and this card has no diagonal row to fall back to, so their carry simply did not act.  The ZWD card's @c pl_blk_zwd_fallback_diag slot, with the honest name.
    long long                 pl_blk_iono_missing_cross{0}; ///< (iono, other) pairs zeroed for a missing harvest cross term — a SUBSET of @c pl_blk_pair_missing, split out so the iono carry's own coupling can be read separately.
    long long                 pl_blk_iono_gap_drop{0};      ///< Carried LOS entries refused at inject because the sighting gap exceeded network.carry_iono_in_block_max_gap_s (invariant #1's absent-satellite edge; the harvest prune then erases them).
    /// I-in-block: the A_r columns of the iono entries that were PUSHED into this
    /// station-epoch's block, in push order.  Read by the post-fit RAIM pool census to
    /// recognise the whitened iono block rows (@c n_raim_pool_iono_rows); empty OFF and
    /// on every station-epoch that pushed none.
    std::vector<Eigen::Index> pl_iono_blk_cols;
    std::vector<PlAmb>        pl_amb;       ///< Present non-pivot ambiguities, for the post-solve stash.  CARRY-ELIGIBLE SUBSET: filled only when the phase-leveling ambiguity carry is the live vehicle (`carry_warm && !carry_invariant`), because it doubles as that carry's inject/harvest record.  QC tiers must NOT key on it — see `qc_amb`.
    /// POOL RE-HOST (docs/SPEC_raim_pool_rehost.md): the CARRY-VEHICLE-NEUTRAL census of
    /// ambiguity columns that are present, unpinned and therefore CHECKABLE this epoch —
    /// the same (col, sat, signal, x_lin, key) tuple as @c pl_amb, built by the same walk,
    /// but WITHOUT the two carry gates that empty @c pl_amb on a Card-4 lineage
    /// (`legacy_local_inject`'s carry_warm, and `do_amb_carry`'s `!carry_invariant`).
    ///
    /// The distinction is the whole point of the card: "which columns get a carried prior
    /// injected" is a CARRY question, while "which columns are screenable by a post-fit
    /// residual test" is an OBSERVABILITY question.  Hosting the second on the first made
    /// every post-fit RAIM tier structurally inert under @c network.carry_invariant.
    ///
    /// Empty unless @c ctx.knobs.raim_ci_on, so every existing @c pl_amb reader (the
    /// leveling harvest, the carry-VCE row feed, the sticky tree, the within-epoch tier)
    /// is untouched and an OFF run is byte-identical.
    std::vector<PlAmb>        qc_amb;
    /// Fix A (network.phase_datum_ensemble): every present ambiguity with a phase
    /// row this epoch whose (sat,signal) has a shared phase-bias column — (sat,
    /// signal, A_r column) — aggregated across stations into the per-(sat,signal)
    /// sum-to-zero gauge.  Empty unless the ensemble datum is armed.
    std::vector<std::tuple<std::string, std::string, Eigen::Index>> ens_amb_cols;
    bool                      pl_zwd_present{false};  ///< This station had a carry-eligible ZWD column this epoch (B9).
    bool                      pl_zwd_injected{false}; ///< L7: a ZWD carry prior was actually injected this epoch (true "carried" flag for network_zwd.csv, distinct from carry-eligible).
    PlZwd                     pl_zwd{};               ///< The ZWD present-record for the post-solve stash (B9).
    /// Card 3 (eq34 RPB time-linking): present UNPINNED RPB columns recorded for
    /// the post-solve stash — (rpb_key, A_r column, linearization point).  Empty
    /// unless network.phase_datum_eq34 with phase_datum_eq34_rpb_q_m2s > 0.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_rpb;
    /// Slant-iono carry (QUEUE 8b): present per-LOS iono columns recorded for the
    /// post-solve harvest — (sat, A_r column, linearization point).  Recorded
    /// UNCONDITIONALLY when armed (the harvest stashes every epoch; the inject is
    /// scope-gated separately).  Same tuple shape as pl_rpb.  Empty unless
    /// network.carry_iono.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_iono;
    long long iono_carry_injected{0};   ///< QUEUE 8b: iono carry priors folded this station-epoch (produce-phase local; summed serially).
    long long iono_carry_floor_hits{0}; ///< QUEUE 8b: injects clamped up to iono_carry_var_floor (expected ~100% mid-arc — the floor IS the effective weight; the q term owns long gaps).
    std::set<std::string> iono_injected_sats;  ///< QUEUE 8b: sats whose iono prior actually folded this epoch (prires ground-truth `folded`, the pl_zwd_injected pattern).
    /// rx-CB carry (docs/SPEC_rx_cb_carry.md): present receiver code-bias columns
    /// (state key, A_r col, x_lin) recorded for the post-solve harvest.  Filled only
    /// under network.carry_rx_code_bias; same tuple shape as pl_rpb.
    std::vector<std::tuple<std::string, Eigen::Index, double>> pl_cb;
    int pl_cb_datum_rows{0};   ///< RXCB-DATUM: zero-mean DCB prior rows folded this station-epoch (summed serially).
        int pl_cb_injected{0};     ///< rx-CB priors folded this epoch (produce-phase local; summed serially).
    int pl_cb_innov_skips{0};  ///< rx-CB priors withheld by the innovation gate (receiver-reset response).
    /// Card LV-2 (docs/SPEC_level_information_budget.md): this station-epoch's
    /// contribution to the information-weighted mean carry innovation, keyed by the
    /// receiver code-bias STATE KEY ("GPS_DCB", "GPS_IFB5", …).  Accumulated over
    /// the rows this station actually FOLDED (a gate-withheld row exerts no force,
    /// so it must not vote on the force being removed) and reduced SERIALLY at the
    /// harvest into μ = Σλ·innov / Σλ.
    ///
    /// The key is the state key rather than the bare constellation on purpose: it
    /// already CARRIES the constellation (every key is "<system>_<family>"), and it
    /// additionally separates the DCB from each per-band IFB — physically distinct
    /// quantities whose innovations must never be averaged together.  In the GPS
    /// dual-frequency posture the two keyings coincide (one family, "GPS_DCB").
    /// Empty unless network.rx_cb_carry_demean.
    std::map<std::string, double> pl_cb_sum_lam_innov;
    std::map<std::string, double> pl_cb_sum_lam;
    int pl_cb_demeaned{0};     ///< LV-2: folds that actually applied a non-zero μ (0 through the lag-1 warm-up and any frozen-from-cold era — invariant #4).
    int pl_cb_frame_gate_would_skip{0};  ///< LV-4 same-run A/B: folds the RETIRED frame-dependent screen would have withheld.  Counted only when the invariant step gate is armed.
    /// Station congruence screen (docs/SPEC_redundancy_census.md CG-INC-0): the
    /// per-row POSITION PARTIAL @f$\partial y_i/\partial\delta p@f$, expressed in
    /// the station's local ENU frame — @c m_data × 3, row-aligned with the FIRST
    /// @c m_data rows of @c A_s / @c A_r / @c w / @c y.
    ///
    /// The network holds every station coordinate fixed, so @c networkSplitStation_
    /// DROPS the three position columns of @c H (they would break @c A_r's
    /// linear-in-observation invariant — see the A6 guard there) and no other
    /// structure in the run retains the geometry of a row.  A wrong held coordinate
    /// is consequently invisible to every armed tier: median polish removes the
    /// station common effect, per-row w-tests see only the post-absorption crumbs,
    /// and the (station, satellite) block test pools ONE line of sight while a
    /// coordinate error is spread coherently across ALL of them.  This block is the
    /// one piece of state the congruence screen needs to ask the question at all.
    ///
    /// It is the JACOBIAN, not a raw line-of-sight vector, and deliberately so: it
    /// is lifted verbatim out of @c em.H (whose position partial is @f$-\hat u@f$
    /// per @c MeasurementModel::buildJacobianRow) and rotated by the ENU→ECEF
    /// matrix at the station's held coordinate.  Taking the partial the measurement
    /// model actually built — rather than re-deriving a sign per row type — is what
    /// makes the screen's convention correct by construction for every current and
    /// future row class: @c em.innovations and @c em.H are one linearization, so
    /// @f$y \approx H\,\delta x@f$ holds row by row whatever the observable is.  A
    /// row whose observable carries no geometry (an SH↔slant tie, a pin, a coupling
    /// row) has an exactly-zero partial here, or lies past @c rows() entirely —
    /// every append site in the assembly APPENDS past the data rows, so only rows
    /// @f$[0,\text{rows}())@f$ can ever carry a non-zero triple.
    ///
    /// EMPTY unless @c network.congruence_screen is armed: an unarmed run allocates
    /// nothing and executes not one extra floating-point operation, which is what
    /// the card's OFF byte-identity A/B checks.
    Eigen::MatrixXd           row_pos_enu;
    /// SD pre-reduce EWL (docs/SPEC_sd_ewl_prereduce.md, @c network.sd_ewl.trace):
    /// pre-formatted `network_sd_ewl.csv` rows for THIS station-epoch — one per
    /// satellite visit, carrying the elected signal pair, the arc outcome, and the
    /// row's z/σ.  Buffered here rather than written in place because the row build
    /// runs in the PARALLEL produce phase; PASS B's SERIAL consume drains these in
    /// station order, so the file is deterministic for any thread count.  Empty
    /// unless the trace is armed (log-only — it changes no solve).
    std::vector<std::string>  sd_ewl_trace;
    /// Card 3 chain-pin AUDIT (docs/SPEC_synthetic_minimum_network.md §14 T1 (measured record §12.8)): per
    /// (constellation,signal) fold tallies for THIS station-epoch's elected datum
    /// pins — key `"<cons>|<signal>"` (the election's @c dr_key), value
    /// {folded, miss_state, miss_row} counting @c foldAmb / @c foldRpb outcomes.
    /// Those lambdas early-return SILENTLY when the elected ambiguity/RPB has no
    /// state index or no live phase row this epoch, so an armed chain datum that
    /// places fewer pins than the election declared reads exactly like a working
    /// one (standing invariant #4) — these tallies are the missing number.
    /// Written in the parallel produce phase (one writer per slot), summed
    /// SERIALLY after the consume, so run totals are thread-count independent.
    /// Empty unless @c network.phase_datum_eq34.
    std::map<std::string, std::array<int, 3>> pin_audit;
    /// Card 3 SEED-ROUND fold-deferred realized targets (docs/SPEC_eq34_seed_round.md
    /// §3.4): (ambiguity key, realized target m, pin innovation y m) for every pin
    /// whose target the FOLD computed (state absent at election time).  Drained
    /// SERIALLY after the consume into @c ctx.carry.eq34_prev_target for ACCEPTED
    /// stations — without this feedback the next election reads a provisional 0,
    /// computes a phantom km-scale pinmove every epoch, and permanently re-references
    /// the (sat,σ).  |y| ≤ λ/2 by construction of std::round; the consume asserts it
    /// into the run counter.  Empty unless @c network.phase_datum_eq34_seed_round.
    std::vector<std::tuple<std::string, double, double>> pin_seed_targets;
};

/// Card 1b (network.carry_coupling_joint_whiten): JOINTLY whiten the KEPT coupling
/// rows of one station — those whose @c local_col is not in @p skip (the refloat
/// drop-set; empty for the primary build).  Builds the joint row covariance
/// @f$R = \mathrm{diag}(\text{var\_eff}_r) + M_w\,\mathrm{diag}(P_w)\,M_w^\top@f$ over
/// the kept rows (@p white_pw = @f$P_w@f$ per @f$M_w@f$ column), LLᵀ-factorizes it,
/// and fills the whitened @c +1/local (@p Hr), @c M_r/shared (@p Hs) and RHS (@p z)
/// blocks as UNIT-information rows (weight 1) — mirroring @c appendShIonoRows.  On a
/// non-SPD factor it retries once with a @f$10^{-12}\cdot\max|\mathrm{diag}|@f$ ridge;
/// still non-SPD ⇒ returns false and the caller falls back to the Card-1 scalar path.
/// @p Hr is @c m×ncol_r, @p Hs is @c m×nshared, @p z is length @c m, with @c m the
/// kept-row count (0 ⇒ empty outputs, returns true).
inline bool whitenCouplingRows(const std::vector<CplRowRaw>& rows,
                               const std::vector<double>& white_pw,
                               const std::set<Eigen::Index>& skip,
                               Eigen::Index ncol_r, Eigen::Index nshared,
                               Eigen::MatrixXd& Hr, Eigen::MatrixXd& Hs,
                               Eigen::VectorXd& z) {
    std::vector<const CplRowRaw*> keep;
    keep.reserve(rows.size());
    for (const CplRowRaw& r : rows)
        if (skip.find(r.local_col) == skip.end()) keep.push_back(&r);
    const Eigen::Index m = static_cast<Eigen::Index>(keep.size());
    Hr = Eigen::MatrixXd::Zero(m, ncol_r);
    Hs = Eigen::MatrixXd::Zero(m, nshared);
    z.resize(m);
    if (m == 0) return true;
    const Eigen::Index nw = static_cast<Eigen::Index>(white_pw.size());
    Eigen::MatrixXd Mw = Eigen::MatrixXd::Zero(m, nw);
    Eigen::MatrixXd Rr = Eigen::MatrixXd::Zero(m, m);
    for (Eigen::Index k = 0; k < m; ++k) {
        const CplRowRaw& r = *keep[static_cast<std::size_t>(k)];
        if (r.local_col >= 0 && r.local_col < ncol_r) Hr(k, r.local_col) = 1.0;
        for (const auto& sc : r.shared)
            if (sc.first >= 0 && sc.first < nshared) Hs(k, sc.first) = sc.second;
        for (const auto& dc : r.departed)
            if (dc.first >= 0 && dc.first < nw) Mw(k, dc.first) = dc.second;
        z(k) = r.rhs;
        Rr(k, k) = r.var_eff;
    }
    // R += M_w diag(P_w) M_wᵀ — the JOINT departed-column marginalization (its
    // DIAGONAL alone is the Card-1 scalar dep_var; the off-diagonals are the
    // between-row correlation the scalar path drops).
    for (Eigen::Index j = 0; j < nw; ++j)
        if (white_pw[static_cast<std::size_t>(j)] > 0.0)
            Rr.noalias() += white_pw[static_cast<std::size_t>(j)]
                          * (Mw.col(j) * Mw.col(j).transpose());
    Rr = (0.5 * (Rr + Rr.transpose())).eval();   // clean numerical asymmetry before LLT
    Eigen::LLT<Eigen::MatrixXd> llt(Rr);
    if (llt.info() != Eigen::Success) {
        const double dmax = Rr.diagonal().cwiseAbs().maxCoeff();
        Rr.diagonal().array() += (dmax > 0.0 ? 1e-12 * dmax : 1e-12);
        llt.compute(Rr);
        if (llt.info() != Eigen::Success) return false;   // caller falls back to scalar
    }
    Hr = llt.matrixL().solve(Hr);
    Hs = llt.matrixL().solve(Hs);
    z  = llt.matrixL().solve(z);
    return true;
}

/// F3 (network.phase_leveling_local_block): whiten ONE station's phase-leveling
/// carry block over its KEPT columns — those whose @c local_col is not in @p skip
/// (the within-epoch RAIM refloat drop-set; empty for the primary build).  The
/// dropped columns are MARGINALIZED — their rows/cols are deleted from Σ before
/// the factor, never conditioned on — which is exact: the kept columns' joint
/// prior is the corresponding principal sub-block of Σ.  Factor Σ_kept = LLᵀ
/// (one ridge retry at 1e-12·max|diag|, as @ref whitenCouplingRows), then emit
/// the UNIT-information rows L⁻¹·[I | M | y]: @p Hr (m×ncol_r — the dense
/// lower-triangular L⁻¹ placed on the block's local columns), @p Hs (m×nshared —
/// L⁻¹ times the per-column present-shared regressor rows), @p z (L⁻¹y).
/// @p kept receives the indices into @c blk.rows of the surviving rows, in order
/// (output row k belongs to @c blk.rows[kept[k]]).  Returns false when the factor
/// is non-SPD after the retry (the caller falls back to per-column rows, or drops
/// the block for a refloat re-solve); m = 0 returns true with empty outputs.
/// Header-inline for the same reason as @ref whitenCouplingRows: its two callers
/// (the builder's primary build and the run-TU refloat re-whiten) must share one
/// codegen so the re-whitened survivors are the rows the primary build would
/// have emitted for that column set.
inline bool whitenLocalBlock(const PlBlockRaw& blk,
                             const std::set<Eigen::Index>& skip,
                             Eigen::Index ncol_r, Eigen::Index nshared,
                             Eigen::MatrixXd& Hr, Eigen::MatrixXd& Hs,
                             Eigen::VectorXd& z, std::vector<Eigen::Index>& kept) {
    kept.clear();
    const Eigen::Index n = static_cast<Eigen::Index>(blk.rows.size());
    for (Eigen::Index k = 0; k < n; ++k)
        if (skip.find(blk.rows[static_cast<std::size_t>(k)].local_col) == skip.end())
            kept.push_back(k);
    const Eigen::Index m = static_cast<Eigen::Index>(kept.size());
    Hr = Eigen::MatrixXd::Zero(m, ncol_r);
    Hs = Eigen::MatrixXd::Zero(m, nshared);
    z.resize(m);
    if (m == 0) return true;
    if (blk.Sigma.rows() != n || blk.Sigma.cols() != n) return false;
    Eigen::MatrixXd S(m, m);
    for (Eigen::Index a = 0; a < m; ++a)
        for (Eigen::Index b = 0; b < m; ++b)
            S(a, b) = blk.Sigma(kept[static_cast<std::size_t>(a)],
                                kept[static_cast<std::size_t>(b)]);
    for (Eigen::Index k = 0; k < m; ++k) {
        const CplRowRaw& r = blk.rows[static_cast<std::size_t>(kept[static_cast<std::size_t>(k)])];
        if (r.local_col >= 0 && r.local_col < ncol_r) Hr(k, r.local_col) = 1.0;
        for (const auto& sc : r.shared)
            if (sc.first >= 0 && sc.first < nshared) Hs(k, sc.first) = sc.second;
        z(k) = r.rhs;
    }
    S = (0.5 * (S + S.transpose())).eval();   // clean numerical asymmetry before LLT
    Eigen::LLT<Eigen::MatrixXd> llt(S);
    if (llt.info() != Eigen::Success) {
        const double dmax = S.diagonal().cwiseAbs().maxCoeff();
        S.diagonal().array() += (dmax > 0.0 ? 1e-12 * dmax : 1e-12);
        llt.compute(S);
        if (llt.info() != Eigen::Success) return false;   // caller falls back / drops
    }
    Hr = llt.matrixL().solve(Hr);
    Hs = llt.matrixL().solve(Hs);
    z  = llt.matrixL().solve(z);
    return true;
}

/// Two-state (offset + drift) stochastic clock model for a carried satellite clock — the
/// Zucca–Tavella oscillator model, x = [offset Δδtˢ (m), drift (m/s)], driven by white-FM
/// (@c q1, m²/s) and random-walk-FM (@c q2, m²/s³) noise whose diffusion coefficients come
/// from the clock's Allan / Hadamard deviation.  Over a step @c dt the transition is
/// Φ = [[1, dt], [0, 1]] and the process-noise covariance is
///   Q(dt) = [[q1·dt + q2·dt³/3,  q2·dt²/2], [q2·dt²/2,  q2·dt]].
/// Carried across epochs in the driver (the @c clock_osc map).  When network.clock_model is
/// on it REPLACES the scalar random-walk clock carry (var_eff = var + q_clock·dt): the
/// physically-tight, drift-following prediction turns a self-consistent lock-in into a large
/// NORMALIZED innovation (a detectable dynamics fault) instead of an invisible magnitude, and
/// its predicted variance sizes the detection band per satellite/epoch rather than by a
/// hand-tuned metre threshold.
struct ClockOsc {
    double x0{0.0};    ///< clock offset Δδtˢ (m)
    double x1{0.0};    ///< clock drift (m/s)
    double p00{0.0};   ///< cov(offset, offset)
    double p01{0.0};   ///< cov(offset, drift)
    double p11{0.0};   ///< cov(drift, drift)
    double t_last{0.0};///< P0-3: continuous-GPS-seconds of the last update for THIS satellite (0 = never); the prediction + drift finite-difference use the true per-sat elapsed time, so a satellite that vanishes (eclipse) and returns predicts over its real gap — Q inflated over the true dt, not one epoch — instead of a falsely-tight hours-old state.
    bool   init{false};///< false until the first stash seeds it
};

/// One-step predicted ClockOsc (the carry-prior mean/variance) — Φ·x and Φ·P·Φᵀ + Q(dt).
struct ClockOscPred { double x0, x1, p00, p01, p11; };

/// Card Q3 (docs/SPEC_clock_temporal_anchor.md) — per-satellite state of the
/// GUARDED temporal clock anchor.  Two states: ANCHORED (the clock's D2 carry
/// folds at @c network.clock_anchor_q) and LOOSE (it folds at the legacy
/// @c filter.process_noise.sat_clock_q — i.e. exactly the pre-card behaviour,
/// which is what bounds a decoupled satellite; standing invariant #7).
///
/// A satellite is born LOOSE and must EARN the anchor by staying in band for
/// @c network.clock_anchor_recouple_epochs consecutive epochs — the invariant-#7
/// re-coupling re-validation.  Every transition is an event with an identity.
struct ClockAnchorSat {
    double t_last{0.0};        ///< continuous-GPS seconds of this satellite's last FOLD (0 = never).  The anchor inflates over `pl_now_s − t_last`, NEVER the global epoch dt: every skip path (gate withhold, σ̂₀ withhold, solve failure) advances prev_tow while the carry survives, so the global dt understates a stale carry's age without bound (DOY-129 withheld 278 consecutive epochs).  Same defect class as the clock model's own P0-3 / R11 fix.
    double last_value{0.0};    ///< prior MEAN folded this epoch (the guard's μ)
    double last_var_eff{0.0};  ///< prior VARIANCE folded this epoch (the guard's P⁻; marginal, so it is identical on the diagonal and dense D2 branches)
    int    in_band{0};         ///< consecutive in-band epochs accumulated while LOOSE
    bool   anchored{false};    ///< false = LOOSE (born LOOSE; re-validation required)
    bool   folded{false};      ///< a clock prior is folded for this satellite THIS epoch and is still standing (guard eligibility; cleared when the guard withdraws it)
    bool   folded_anchored{false};  ///< …and it was the ANCHORED variance
    bool   stale_pending{false};    ///< the fold found the carry older than the staleness bound and fell back to the LOOSE variance; the guard owns the state transition + event + mark, so every decoupling edge lands through ONE path
};

/// Card Q3 — run state of the guarded temporal clock anchor.  Lives in
/// NetworkCarryState; deliberately NOT snapshotted (three-place rule
/// exemption): a warm resume IS an invariant-#1 event (the F1 rule), so every
/// satellite must re-earn the anchor from LOOSE after a resume rather than
/// inherit a stiffness whose evidence did not travel.
struct ClockAnchorState {
    std::map<std::string, ClockAnchorSat> sat;
    /// Satellites decoupled THIS epoch (the guard's output for the epoch) —
    /// consumed by the wire-mark queue and the event/counter tier.
    std::set<std::string> tripped;
};

/// Card CLV (docs/refound/CLV_clock_level_2026-09-22.md): one satellite's code-only
/// LEVEL filter.  The level is the direction that moves the satellite clock and every
/// one of its phase biases by the same amount (+1 on each: the clock enters every
/// observation at -1 and a phase bias enters a phase row at +1), so no phase row sees
/// it and its only information is the ionosphere-free code.  Each kept epoch the
/// filter measures the level error from the post-fit ionosphere-free code residuals
/// and re-bases the carried state along the direction by its update; the solve itself
/// is never loosened.
/// Card CLV2 R-A2 (@c network.clock_level_free_cb_rebase): the filter of one free same-band
/// code-bias column, re-based from the same-band code difference (the free signal minus the
/// pin pair's first signal) at the stations that track both.  The difference is free of the
/// clock, the geometry, the troposphere and the ionosphere, so it reads the column's error
/// against the pinned signal directly, up to a per-station offset.
struct ClockLevelFreeCb {
    double info{0.0};     ///< information about the column's error (1/m^2), valid at @c t_info; 0 = not armed
    double t_info{0.0};   ///< continuous-GPS seconds of the last update
    bool   armed{false};  ///< an update has landed; while armed the column no longer follows the level (R-A)
    int    n_cells{0};    ///< station cells of the last update
    double e_last{0.0};   ///< the last update's effect (m): positive when the wire's column is low against the stations
    double d_last{0.0};   ///< the last step applied to the column (m)
    double d_sum{0.0};    ///< the accumulated step since the run start (m)
};

struct ClockLevelSat {
    double info{0.0};     ///< I_L: the filter's code-only information about the level (1/m^2), valid at @c t_info; 0 = not armed
    double t_info{0.0};   ///< continuous-GPS seconds of the last update
    bool   armed{false};  ///< the filter is running (the Q3 anchor holds the clock and an update has landed since)
    int    n_pairs{0};    ///< ionosphere-free (station) pairs of the last update
    double e_last{0.0};   ///< the last update's level residual (m): the satellite's two-way effect, positive when the clock is high
    double d_last{0.0};   ///< the last shift applied along the level direction (m)
    double d_sum{0.0};    ///< the accumulated shift since the run start (m)
    double pending_step{0.0}; ///< the shifts applied since this satellite's last emission (m): declared on its next C record (lvl=) and reset
    /// Per phase-bias signal: the accumulated shift the emit-side hold follows (m), so
    /// the wire's phase side does not move with the level.
    std::map<std::string, double> xfer;
    /// R-A (@c network.clock_level_free_cb_follow): per free same-band code-bias signal,
    /// the follow steps applied since this satellite's last emission (m), declared on its
    /// next B record (lvl=) and reset.
    std::map<std::string, double> cb_pending;
    /// R-B (@c network.clock_level_datum_pause): the filter was armed when the datum
    /// changed, so while the anchor re-validates the satellite is skipped with its
    /// information kept.  Cleared when the satellite is next updated or disarmed.
    bool paused_datum{false};
    /// R-B (@c network.clock_level_confirm_epochs): the epochs accumulated towards an arm
    /// or a break (0 = none pending), their summed information, their information-weighted
    /// effect sum, and the sign of a pending break (+1 or -1; 0 for an arm).
    int    pend_n{0};
    double pend_info{0.0};
    double pend_esum{0.0};
    int    pend_sign{0};
    /// Card CLV2 R-A2 (@c network.clock_level_free_cb_rebase): per free same-band code-bias
    /// signal (GPS C1W, Galileo C1X), the column's own filter.  Empty unless the knob is on.
    std::map<std::string, ClockLevelFreeCb> free_cb;
};

/// Card CLV: run state of the level filter.  Lives in NetworkCarryState.  NOT
/// snapshotted (three-place-rule exemption, deliberate): the knob is in the resume
/// fingerprint, the phase-bias hold restarts its generations on a resume anyway, and a
/// filter that re-arms after a resume starts from the level the resumed carry holds.
struct ClockLevelState {
    std::map<std::string, ClockLevelSat> sat;
};

/// Predict a ClockOsc forward by @p dt with white-FM @p q1 and random-walk-FM @p q2.
inline ClockOscPred clockOscPredict(const ClockOsc& s, double dt, double q1, double q2) {
    const double q11 = q1 * dt + q2 * dt * dt * dt / 3.0;
    const double q12 = q2 * dt * dt / 2.0;
    const double q22 = q2 * dt;
    ClockOscPred p;
    p.x0  = s.x0 + s.x1 * dt;
    p.x1  = s.x1;
    p.p00 = s.p00 + 2.0 * dt * s.p01 + dt * dt * s.p11 + q11;
    p.p01 = s.p01 + dt * s.p11 + q12;
    p.p11 = s.p11 + q22;
    return p;
}

/// Measurement update of @p s from prediction @p pr with a network-solved offset @p z (var
/// @p r) whose datum-common-mode has already been removed via @p innov (z − predicted, minus
/// the per-constellation median innovation).  H = [1 0].  Returns S = innovation variance so
/// the caller can form the normalized innovation innov/√S (the dynamics-fault statistic).
/// @note Retained for reference; the clock-model carry now sets the osc state DIRECTLY from the
///       joint-solve posterior (B3 data-incest fix), so this scalar KF update is no longer wired.
[[maybe_unused]] inline double clockOscUpdate(ClockOsc& s, const ClockOscPred& pr, double z, double r) {
    const double S  = pr.p00 + r;
    const double dz = z - pr.x0;                 // raw innovation (carry follows the raw clock)
    const double K0 = (S > 0.0) ? pr.p00 / S : 0.0;
    const double K1 = (S > 0.0) ? pr.p01 / S : 0.0;
    s.x0  = pr.x0 + K0 * dz;
    s.x1  = pr.x1 + K1 * dz;
    s.p00 = (1.0 - K0) * pr.p00;
    s.p01 = (1.0 - K0) * pr.p01;
    s.p11 = pr.p11 - K1 * pr.p01;
    s.init = true;
    return S;
}

// ---- FC-1 split-cadence fast clock (docs/SPEC_fast_clock_split_cadence.md) --------
// The cached anchor state a clocks-only fast tick measures against.  Rebuilt from
// scratch at every EMITTED full solve, so an arc that reset between full epochs is
// simply re-harvested; a slip WITHIN a fast window is caught by the per-LOS k·σ screen
// (the fast tier's only detector — repair belongs to the full pipeline).
// (Map instances live in NetworkCarryState::fast_clock.)

/// FC-1: one cached band of one line-of-sight at the anchor epoch t0.
///
/// The "lump" M_j is everything the full solve modeled or estimated that is NOT the
/// instantaneous geometric range and NOT the a-priori satellite clock: receiver clock,
/// tropo (mf·ZWD), iono (μ_j·I), ambiguity, phase biases, wind-up, PCO/PCV projections,
/// tides, Shapiro, and the RESIDUAL (beyond-a-priori) satellite clock — the quantity the
/// producer emits.  Splitting the a-priori-clock-corrected phase
/// Φ̃_j = Φ_j + c·δt_ap as Φ̃_j(t0) = ρ(t0) + M_j(t0) is what lets a fast tick recompute
/// ONLY ρ (the one nonlinear term) and ride the rest linearly.  The a-priori clock
/// (polynomial + relativistic, the full model's convention) is corrected OUT so the lump
/// rate — and the ċ recovered from it — is the emitted-frame residual rate, not the full
/// physical rate the consumer's own a-priori already advances (NetworkFastClock.cpp
/// @c fc1_apriori).
struct FastClockLosBand {
    std::string signal;        ///< RINEX observation code of the cached phase observable (e.g. "L1C").
    double freq_hz{0.0};       ///< Carrier frequency (Hz); the IF coefficients are formed from the pair.
    double wavelength_m{0.0};  ///< Cycles->metres factor, CACHED so the tick re-uses the anchor's exact
                               ///< value.  Recomputing it at the tick — even by the same formula — risks a
                               ///< last-bit difference, and an ~1e8-cycle phase count turns 1e-9 m of
                               ///< wavelength disagreement into ~0.1 m of fake clock.
    double lump{0.0};          ///< M_j(t0) = Φ̃_j(t0) − ρ(t0)  (m), on a-priori-clock-corrected phase.
    double lump_rate{0.0};     ///< Ṁ_j (m/s), finite-differenced against the PREVIOUS emitted full epoch.
    bool   has_rate{false};    ///< false until two consecutive emitted full epochs have been harvested.
};

/// FC-1: one cached line-of-sight (station × satellite) at the anchor epoch t0.
struct FastClockLos {
    int    sta_idx{-1};        ///< Index into the run's station vector.
    double rho0{0.0};          ///< Geometric range at t0 (m) — kept for the tick's consistency check.
    double elevation_rad{0.0}; ///< Elevation at t0; sizes the elevation-weighted R at the tick.
    std::array<FastClockLosBand, 2> band{};  ///< The arc's two cached bands (index 1 unused when n_band==1).
    int    n_band{0};          ///< 1 or 2.  A single-band arc leaves μ_jΔI in the residual and is
                               ///< admitted only under @c network.fast_clock.allow_single_band.
};

/// FC-1: the whole carried fast-clock anchor.  Deliberately NOT snapshotted (RTB-7a) —
/// a warm-resumed run re-arms only after two fresh EMITTED full epochs, which costs one
/// full-solve era and removes any question of a restored anchor referring to a frame the
/// new leg never emitted.
struct FastClockCarry {
    bool   armed{false};              ///< True once two consecutive emitted full epochs have been harvested.
    std::size_t consecutive_emitted{0};///< Emitted-full-epoch streak feeding @ref armed (reset by any withhold).
    double t0_s{0.0};                 ///< Continuous-GPS seconds of the anchor (the LAST EMITTED full solve).
    double t_prev_s{0.0};             ///< The emitted full epoch before the anchor (the Ṁ finite-difference base).
    int    t0_week{0};                ///< GPS week of the anchor (for trace/product stamping).
    double t0_tow{0.0};               ///< GPS TOW of the anchor.
    std::string datum_station;        ///< The clock-datum pin the anchor was built under; a tick whose
                                      ///< datum station is absent or screened is WITHHELD, never re-datumed.
    std::map<std::string, std::vector<FastClockLos>> los;  ///< sat -> its cached LOS at t0.
    std::map<std::string, double> c0_emitted;              ///< sat -> the EMITTED clock c0 (m) at t0; the fast
                                                           ///< product is c0_emitted + ċ·Δt + δc, so it lives in
                                                           ///< the emitted frame by construction.
    /// sat -> RESIDUAL satellite clock RATE ċ (m/s), in the same datum frame as the fast
    /// δc and the same a-priori-corrected (emitted) frame as the lump.
    ///
    /// Load-bearing, and easy to miss: the fast observable subtracts Ṁ·Δt, and Ṁ contains
    /// the residual satellite clock's own drift.  What the tick estimates is therefore
    /// the clock change BEYOND that linear trend, not the whole change.  Emitting
    /// anchor + δc alone would drop the drift term and de-trend the product; emitting a
    /// FULL-physical-rate drift (the raw-phase lump of the original build) double-counts
    /// the broadcast af1 + relativistic rate the consumer's a-priori already advances.
    /// This map carries the RESIDUAL drift back so the emitted value is
    /// anchor + ċ·Δt + δc, advancing exactly what the a-priori does not.
    ///
    /// Recovered from the same per-LOS Ṁ by the same bipartite separation: Ṁ = τ̇_r − ċ,
    /// so the satellite (column) effect IS −ċ once the datum station's row effect is
    /// pinned to zero.  Same datum as δc, hence the same frame.
    std::map<std::string, double> c1_rate;
    std::size_t tick_seq{0};          ///< Ticks run since the anchor (drives the trace stride).
};

// Iono-2 / T5 v2 (2026-07-17): per-(station,sat) SH representativeness-error
// Gauss–Markov deviation carry — the per-LOS iono TEMPORAL-ACCUMULATION channel
// of the per-epoch network producer.  The SH surface cannot represent per-LOS
// slant-iono structure (~70 % of LOS residuals > 3σ at ANY degree/station count,
// T4 + the Jul-17 150-sta/deg-15 A/B); re-applying the pin as white forces that
// misfit dispersively into the float ambiguities (the NL-AR blocker).  With
// rep_error_tau_s > 0 the pin constrains (ι − Σ_l SH_l·c_l) to a TRACKED
// deviation d (GM: τ = rep_error_tau_s, steady-state σ = representation_sigma_tecu)
// instead of to 0.  v2 units: dev/var are carried in VERTICAL TECU (converted by
// the epoch's layer-0 mapping M₁(el)·tecu_to_L1m at consumption/harvest), so the
// carried level AND its steady state are elevation-stationary — the v1
// slant-metre carry never re-scaled the level as elevation changed.  var_ss is
// the constant representation_sigma_tecu² (kept per-entry so the aging pre-pass
// needs no config re-read).  Threading contract: the map is AGED (and
// slip-erased via the C3c funnel) once per epoch in a serial pre-pass and
// measurement-UPDATED in the serial post-solve harvest via shDevExactUpdate();
// the parallel assembly only READS (find, never insert) — race-free by
// construction.  t_s = 0 marks a never-harvested (cold) entry.  OFF ⇒
// byte-identical.  (Map instance @c sh_dev_carry lives in NetworkCarryState.)
struct ShDevCarry { double dev{0.0}; double var{0.0}; double var_ss{0.0}; double t_s{0.0}; };

/// Iono-2: EXACT scalar posterior update of one GM deviation d from the solved
/// pin functional — replaces the falsified-circular T5 v1 scalar filter (which
/// re-consumed the pin-regularized residual as an independent floor-noise
/// observation, collapsing P to floor² after one update).
///
/// Model: prior d ~ N(d_pred, P_pred); pin row u − d = ε, ε ~ N(0, f²), with u
/// the functional (ι − Σ SH·c) whose data-only likelihood is N(u₀, U).  The
/// assembly folds the Schur-eliminated row u = d_pred + ε(f² + P_pred); the
/// solve returns the posterior û (= the pin residual) and V̂ = Var_post(u) =
/// U·a/(U+a) with a = P_pred + f².  Exact posterior of d:
///     d̂  = d_pred + (P_pred/a)·(û − d_pred)          (gain from the PREDICTED P)
///     P⁺ = P_pred·(U + f²)/(U + a),  U = V̂·a/(a − V̂)
/// Limits: U→∞ (uninformative epoch) ⇒ P⁺ = P_pred, d̂ → d_pred; U→0 (data pins
/// u) ⇒ P⁺ = P_pred·f²/a — the classic scalar KF update against a floor-noise
/// observation.  V̂ ≥ a (numerically no information) ⇒ P⁺ = P_pred.  All inputs
/// in ONE consistent unit frame (the caller converts slant ↔ vertical).
///
/// @param d_pred   GM-aged prior deviation.
/// @param p_pred   GM-aged prior variance (> 0).
/// @param u_hat    Solved posterior mean of the pin functional.
/// @param v_hat    Solved posterior variance of the pin functional (≥ 0).
/// @param floor2   Pin-row floor variance f² (> 0; station-deweight-effective).
/// @return {d̂, P⁺}.
inline std::pair<double, double> shDevExactUpdate(double d_pred, double p_pred,
                                                  double u_hat, double v_hat,
                                                  double floor2) {
    const double a = p_pred + floor2;
    if (!(a > 0.0)) return {d_pred, p_pred};
    const double d_post = d_pred + (p_pred / a) * (u_hat - d_pred);
    if (!(v_hat > 0.0))                       // data pins u exactly (or degenerate)
        return {d_post, p_pred * floor2 / a};
    if (v_hat >= a) return {d_post, p_pred};  // no information beyond the row itself
    const double U = v_hat * a / (a - v_hat); // recovered data-only variance
    return {d_post, p_pred * (U + floor2) / (U + a)};
}

/// rx-CB carry (docs/SPEC_rx_cb_carry.md): the inject-time decision for one
/// carried receiver code-bias prior.  Read-only w.r.t. the carry map — the
/// produce phase is PARALLEL, so a breach is answered by WITHHOLDING the fold
/// (the serial post-solve harvest then overwrites the stale entry with the
/// fresh posterior: an implicit, race-free drop).  The innovation gate is the
/// invariant-#1 event response for receiver resets / firmware bias jumps:
/// |value_abs − x_lin| beyond k·√(var_eff + guard²) means the carried level
/// and the station filter's own estimate disagree at hardware-jump scale.
///
/// ⚠ THE INNOVATION TEST IS FRAME-DEPENDENT — see @ref rxCbStepGate.  @p innov_m is
/// @c value_abs − x_lin(cb), and x_lin is a FROZEN linearization point (seeded once
/// by @c StateInitializer::initializeCodeBiases, never written back from the solve),
/// so this statistic measures how far the carried value sits from an arbitrary
/// frame, not whether anything happened.  @p innov_test = false disables it, leaving
/// the gap + variance sanity checks that are frame-free; the harvest-side
/// @ref rxCbStepGate then owns the event response.
enum class RxCbInjectAction { Fold, SkipGap, SkipInnov };
inline RxCbInjectAction rxCbInjectGate(double gap_s, double max_gap_s,
                                       double var, double q_m2s, double innov_m,
                                       double k_sigma, double guard_var_m2,
                                       double& var_eff_out, bool innov_test = true) {
    if (gap_s > max_gap_s) return RxCbInjectAction::SkipGap;
    const double var_eff = var + q_m2s * (gap_s > 0.0 ? gap_s : 0.0);
    // No <cmath> in this header: NaN via self-compare, runaway via a bound.
    if (!(var_eff > 0.0) || var_eff != var_eff || !(var_eff < 1e30))
        return RxCbInjectAction::SkipGap;
    var_eff_out = var_eff;
    if (innov_test
        && innov_m * innov_m > k_sigma * k_sigma * (var_eff + guard_var_m2))
        return RxCbInjectAction::SkipInnov;
    return RxCbInjectAction::Fold;
}

/// Card LV-4 (docs/SPEC_level_information_budget.md §11): the FRAME-INVARIANT
/// receiver-code-bias carry screen — the epoch-over-epoch STEP of the station's own
/// ABSOLUTE estimate.
///
/// WHY THE OLD SCREEN COULD NOT WORK.  @ref rxCbInjectGate tests
/// |value_abs − x_lin(cb)|.  Both the carried value and x_lin are absolute, but
/// x_lin is FROZEN at its first-epoch seed, so that difference is just the standing
/// correction x_r — a property of where the seed happened to land, not of anything
/// the receiver did.  Two consequences, both measured on DOY-129:
///   * a station whose true bias sits far from its seed trips it EVERY epoch for the
///     whole run (~50–67 % of all folds withheld, with no receiver event anywhere);
///   * a real receiver jump between epochs t−1 and t is INVISIBLE to it — at t's
///     inject the carried value is still the pre-jump one and x_lin has not moved,
///     so the statistic contains no information about the jump at all.
///
/// THE INVARIANT STATISTIC.  Both the freshly harvested posterior and the carried
/// value are ABSOLUTE (x_lin + x_r), so their difference is invariant under any
/// shift of the linearization point:
///
///     e   = value_abs_new − value_abs_prev
///     var = var_new + var_prev + q·Δt          (both posteriors + the RW drift)
///     z   = |e| / √(var + guard)
///
/// z > k means the receiver's OWN estimate stepped at hardware-jump scale between
/// two solves — exactly the invariant-#1 event the carry needs a response to, and a
/// quantity no reseeding, reframing or datum change can move.
///
/// THE SCALE MUST BE ROBUST AND PEER-RELATIVE, not formal — measured 2026-08-05.
/// A first cut standardized e by its FORMAL variance (var_new + var_prev + q·Δt +
/// 0.5 m guard ⇒ σ ≈ 0.52 m).  On the DOY-129 EXT-CB arm that erased **432 of 777**
/// carried entries in 20 epochs (max z 48, 10 of 61 stations still carrying at run
/// end) and the level went unstable.  The cause is not the receivers: at ignition
/// the confounded (level ↔ rx-DCB) direction genuinely RE-REALIZES by metres per
/// epoch, so the formal variance understates the true step scatter by two to three
/// orders and any formal-σ test is a guaranteed mass-trigger.  Two corrections,
/// both of which the network already uses elsewhere:
///   * take the scale from THIS epoch's own steps (a Baarda-style robust σ̂ =
///     1.4826·MAD, floored at the formal σ so a freakishly quiet epoch cannot make
///     the screen hypersensitive) — the same idiom as the RAIM σ̂₀;
///   * remove the MEDIAN step first.  A step COMMON to the whole network is a frame
///     move, not 61 simultaneous receiver resets — the same distinction LV-2 draws
///     for the fold.  What survives is the per-station outlier: exactly the event
///     the carry needs a response to.
struct RxCbStepScale {
    double median{0.0};   ///< median step this epoch (the common frame move — NOT an event).
    double sigma{0.0};    ///< robust scale, 1.4826·MAD, floored at the formal σ.
    int    n{0};          ///< contributing entries.
    bool   valid{false};  ///< false ⇒ too few entries to define a scale; screen stands down.
};

/// Build the robust per-epoch step scale.  @p steps is REORDERED in place.
/// @param formal_sigma_floor √(var_new + var_prev + q·Δt) class floor (m).
/// @param min_n Minimum entries for a defensible scale; below it the screen stands
///        down entirely (invariant #7: a thin epoch must not define what "normal"
///        is, and a false erase costs a real prior).
inline RxCbStepScale rxCbStepScale(std::vector<double>& steps,
                                   double formal_sigma_floor, int min_n) {
    RxCbStepScale out;
    steps.erase(std::remove_if(steps.begin(), steps.end(),
                               [](double v) { return !std::isfinite(v); }),
                steps.end());
    out.n = static_cast<int>(steps.size());
    if (out.n < min_n || out.n < 1) return out;
    const std::size_t mid = steps.size() / 2;
    std::nth_element(steps.begin(), steps.begin() + mid, steps.end());
    double med = steps[mid];
    if (steps.size() % 2 == 0) {
        std::nth_element(steps.begin(), steps.begin() + (mid - 1), steps.end());
        med = 0.5 * (steps[mid - 1] + med);
    }
    std::vector<double> dev(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) dev[i] = std::abs(steps[i] - med);
    std::nth_element(dev.begin(), dev.begin() + mid, dev.end());
    double mad = dev[mid];
    if (dev.size() % 2 == 0) {
        std::nth_element(dev.begin(), dev.begin() + (mid - 1), dev.end());
        mad = 0.5 * (dev[mid - 1] + mad);
    }
    out.median = med;
    out.sigma  = std::max(1.4826 * mad,
                          std::max(formal_sigma_floor, 0.0));
    out.valid  = std::isfinite(out.median) && std::isfinite(out.sigma) && out.sigma > 0.0;
    return out;
}

/// @return z (0 on degenerate input) and whether it exceeded k·σ̂.  Degenerate or
///        non-finite input, or an invalid scale, reports NO jump: a false erase
///        costs a real prior, and the harvest's own finite checks already reject
///        unusable values.
struct RxCbStepVerdict { double z{0.0}; bool jumped{false}; };
inline RxCbStepVerdict rxCbStepVerdict(double step_m, const RxCbStepScale& scale,
                                       double k_sigma) {
    RxCbStepVerdict out;
    if (!scale.valid || !std::isfinite(step_m)) return out;
    const double z = std::abs(step_m - scale.median) / scale.sigma;
    if (!std::isfinite(z)) return out;
    out.z = z;
    out.jumped = (z > k_sigma);
    return out;
}

/// Card LV-2 (docs/SPEC_level_information_budget.md): the outcome of reducing one
/// receiver-code-bias family's accumulated (Σλ·innov, Σλ, n_stations) into the
/// information-weighted mean innovation the NEXT epoch's folds subtract.
struct RxCbDemeanReduce {
    double mu{0.0};       ///< the μ to apply (fresh, or @p prev_mu held).
    bool   fresh{false};  ///< false ⇒ FROZEN: the epoch was too thin to define a frame.
};

/// Card LV-2: μ = Σλ·innov / Σλ over the rows that actually FOLDED this epoch.
///
/// Subtracting it makes Σλ(innov − μ) ≡ 0 in the fold's DELTA coordinates, so the
/// carry transports every between-station DIFFERENCE at full stiffness and stops
/// re-asserting LAST EPOCH's common-mode realization — the flywheel this card was
/// written to remove.
///
/// ⚠ WHAT IT DOES **NOT** DO (measured 2026-08-05, and the card's own docs said
/// otherwise until then): it does not leave the common mode FREE.  The fold's
/// information is unchanged by the de-mean — only its RHS moves — so the carry
/// still puts Σλ (~100 units) on the common-mode direction.  In ABSOLUTE terms
/// each row now targets `value_abs_i − μ`, whose information-weighted mean is
///
///     mean_λ(value_abs) − μ  =  mean_λ(x_lin)
///
/// i.e. the de-meaned carry PINS the receiver-DCB common mode to the mean of the
/// frozen first-epoch linearization points.  It swaps a rolling memory for a
/// frozen one; the designed authority (the rx-DCB datum at 0.01, the a₀₀ anchor at
/// 0.25) is still outvoted ~400:1.  Consequence, measured: changing what
/// `initializeCodeBiases` seeds — which is exactly what card LV-1 does — moves the
/// ionosphere level by ~10 TECU through this channel and nothing else (arm 23 vs
/// arm 27: settled 35.24 vs 26.45, identical row sets).  Freeing the direction
/// needs the common mode removed from the carry's INFORMATION, which the
/// per-station diagonal channel cannot express — see SPEC §13.
///
/// Invariant #7, decoupling edge: an epoch whose folded set is thin (mass re-seed,
/// RAIM re-reduce, a station-poor era) must NOT be allowed to define the frame, so
/// below @p min_sta contributing stations the previous μ is HELD and the caller
/// counts the freeze.  A non-finite or non-positive Σλ freezes the same way — a
/// mean with no weight behind it is not a measurement.
///
/// Pure function (no <cmath> in this header: NaN via self-compare), so the
/// arithmetic is unit-testable away from the run loop.
inline RxCbDemeanReduce rxCbDemeanMu(double sum_lam_innov, double sum_lam,
                                     int n_sta, int min_sta, double prev_mu) {
    RxCbDemeanReduce out;
    out.mu = prev_mu;
    if (n_sta < min_sta) return out;
    if (!(sum_lam > 0.0) || sum_lam != sum_lam) return out;
    if (sum_lam_innov != sum_lam_innov) return out;
    const double mu = sum_lam_innov / sum_lam;
    if (mu != mu || !(mu < 1e30) || !(mu > -1e30)) return out;
    out.mu = mu;
    out.fresh = true;
    return out;
}

/// Card S2H (docs/SPEC_level_information_budget.md §17 item 3 / §27 item 1): the
/// outcome of reducing one receiver-code-bias family's freshly harvested ABSOLUTE
/// realizations into the Λ-weighted common-mode mean μ the caller subtracts from
/// every carried entry of that family — the "S-consistent harvest".
struct RxCbHarvestReduce {
    double mu{0.0};        ///< Λ-weighted mean over the screened member set (m).
    double sum_lam{0.0};   ///< Σλ over the voting members (drives the exact diagonal covariance co-transform, 1/Σλ).
    int    n{0};           ///< usable family entries offered (finite value, var > 0).
    int    n_used{0};      ///< entries that voted after the robust peer screen.
    int    n_screened{0};  ///< entries excluded from the vote by the peer screen (still transformed).
    bool   applied{false}; ///< false ⇒ IDENTITY transform this epoch (thin/degenerate set — counted, never a frozen μ).
};

/// Card S2H: μ = Σλ·value / Σλ over the family's harvested ABSOLUTE realizations,
/// λ = 1/var, after a robust peer screen.  The caller subtracts μ from EVERY
/// carried entry of the family (voters and screened alike — the frame moves for
/// everyone), making the carried set zero-mean-by-construction and coherent with
/// the per-station rx-CB datum row, whose target is the ABSOLUTE zero.
///
/// This is LASER's `applySTransformationForBigSystemOfEquations`
/// (C:\LASER Net3DLsa.cpp:5573 — subset mean subtracted from all points, O(n))
/// generalized to information weights: S = I − 1·λᵀ/(λᵀ1) per family, still a
/// projector (S² = S).  TWO deliberate deviations from the LASER original, both
/// load-bearing:
///
///  1. TARGET.  LASER de-means the correction about the a-priori ξ₀ (its
///     minimum-norm objective).  Here the de-mean is about the DATUM ROW's
///     target — absolute zero — because the objective is coherence with
///     `rx_code_bias_datum` (η = λ(0 − x_lin)), not minimum shift from x_lin.
///     De-meaning about x_lin is precisely card LV-2's measured failure: it
///     pins the common mode to mean(x_lin), the frozen first-epoch seed frame
///     (SPEC §13.2).  Zero-mean ABSOLUTES leave the carry with zero net force
///     on the common mode in the fold's own metric: next epoch's folds pull the
///     common mode toward mean_λ(value_abs) = 0 = where the datum row already
///     points.  The frame is re-derived from THIS epoch's solution — no frozen
///     pin, no lag-1 feedback, nothing fed back into any solve.
///
///  2. WEIGHTS.  LASER's mean is unweighted because its datum members carry
///     equal (unit) D0 entries.  Our folds carry per-station λ = 1/var, and the
///     common-mode force each fold exerts is proportional to λ — so only the
///     Λ-weighted mean zeroes the NET force.  λ_i ≡ const reduces exactly to
///     the LASER/oracle case (GPS3710_test_PMINOLESS).
///
/// Robust peer screen (SPEC §16/12.1 rider — a mean vote is a contamination
/// window; one ~200 km cold-start realization must not move the frame):
/// entries with |value − median| > screen_z · max(1.4826·MAD, median formal σ)
/// are excluded from the VOTE (counted) but still receive the transform.
/// screen_z ≤ 0 disables the screen (every usable entry votes).
///
/// Thin/degenerate epochs (n_used < min_n, or Σλ non-positive/non-finite)
/// APPLY NOTHING — identity transform, counted by the caller.  Unlike LV-2's
/// frozen μ there is no held frame to re-assert: the carried set simply keeps
/// the zero-mean frame of the last applied epoch (invariant #7's decoupling
/// edge, visible in the counters rather than silently defining a new frame).
///
/// @param values     harvested absolute realizations (screen scratch: copied).
/// @param vars       matching posterior variances; entries with non-finite value
///                   or var ≤ 0 are unusable (excluded from n and the vote; the
///                   caller still transforms their value, never their var).
/// @param min_n      vote floor (reuses network.rx_cb_carry_demean_min_stations,
///                   the LV-4 precedent).
/// @param screen_z   robust peer-screen threshold (≤ 0 disables).
/// @param used_out   optional per-entry vote mask (1 = voted), sized to values.
inline RxCbHarvestReduce rxCbHarvestMu(const std::vector<double>& values,
                                       const std::vector<double>& vars,
                                       int min_n, double screen_z,
                                       std::vector<unsigned char>* used_out = nullptr) {
    RxCbHarvestReduce out;
    const std::size_t m = values.size() < vars.size() ? values.size() : vars.size();
    if (used_out) used_out->assign(m, 0);
    std::vector<double> usable;
    usable.reserve(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double v = values[i], w = vars[i];
        if (v != v || !(v < 1e30) || !(v > -1e30)) continue;
        if (!(w > 0.0) || w != w) continue;
        usable.push_back(v);
    }
    out.n = static_cast<int>(usable.size());
    if (out.n < 1) return out;
    // Robust centre + scale of the usable values (LV-4 idiom: median / MAD,
    // floored at the median formal σ so a family of near-identical values does
    // not produce a zero scale that screens everything).
    double med = 0.0, sig = 0.0;
    bool screen_on = screen_z > 0.0 && out.n >= 2;
    if (screen_on) {
        std::vector<double> scratch = usable;
        const std::size_t mid = scratch.size() / 2;
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.end());
        med = scratch[mid];
        if (scratch.size() % 2 == 0) {
            std::nth_element(scratch.begin(), scratch.begin() + (mid - 1), scratch.end());
            med = 0.5 * (scratch[mid - 1] + med);
        }
        std::vector<double> dev(scratch.size());
        for (std::size_t i = 0; i < scratch.size(); ++i)
            dev[i] = scratch[i] > med ? scratch[i] - med : med - scratch[i];
        std::nth_element(dev.begin(), dev.begin() + mid, dev.end());
        double mad = dev[mid];
        if (dev.size() % 2 == 0) {
            std::nth_element(dev.begin(), dev.begin() + (mid - 1), dev.end());
            mad = 0.5 * (dev[mid - 1] + mad);
        }
        std::vector<double> fsig;
        fsig.reserve(m);
        for (std::size_t i = 0; i < m; ++i)
            if (vars[i] > 0.0 && vars[i] == vars[i]) fsig.push_back(vars[i]);
        double fs = 0.0;   // median formal σ = sqrt(median var) (monotonic map)
        if (!fsig.empty()) {
            const std::size_t fm = fsig.size() / 2;
            std::nth_element(fsig.begin(), fsig.begin() + fm, fsig.end());
            fs = std::sqrt(fsig[fm]);
        }
        sig = 1.4826 * mad;
        if (fs > sig) sig = fs;
        if (!std::isfinite(sig) || !(sig > 0.0)) screen_on = false;
    }
    double sum_lam = 0.0, sum_lam_v = 0.0;
    int used = 0, screened = 0;
    for (std::size_t i = 0; i < m; ++i) {
        const double v = values[i], w = vars[i];
        if (v != v || !(v < 1e30) || !(v > -1e30)) continue;
        if (!(w > 0.0) || w != w) continue;
        if (screen_on) {
            const double d = v > med ? v - med : med - v;
            if (d > screen_z * sig) { ++screened; continue; }
        }
        const double lam = 1.0 / w;
        sum_lam += lam;
        sum_lam_v += lam * v;
        if (used_out && i < used_out->size()) (*used_out)[i] = 1;
        ++used;
    }
    out.n_used = used;
    out.n_screened = screened;
    if (used < min_n || used < 1) return out;
    if (!(sum_lam > 0.0) || sum_lam != sum_lam || sum_lam_v != sum_lam_v) return out;
    const double mu = sum_lam_v / sum_lam;
    if (mu != mu || !(mu < 1e30) || !(mu > -1e30)) return out;
    out.mu = mu;
    out.sum_lam = sum_lam;
    out.applied = true;
    return out;
}

/// Iono-2 v3k (erase-policy split): the D5-only slip response — KEEP the
/// accumulated deviation level, re-tap the confidence to the GM steady state.
/// Applied when a (station,sat) LOS is in the C3c funnel WITHOUT a raw-detector
/// confirmation this epoch (measured: 97 % of D5 re-taps have no raw observable
/// step — they are iono-representation tension, the very signal the deviation
/// carry exists to absorb; erasing on them capped the mean deviation lifetime
/// at ≈ 0.7·τ).  Raw-confirmed slips still ERASE (invariant #1 unchanged) —
/// see the PASS-B pre-pass.  Idempotent under the subsequent GM aging step
/// (λ²·σ_ss² + (1−λ²)·σ_ss² = σ_ss²).
inline void shDevD5Retap(ShDevCarry& dc) { dc.var = dc.var_ss; }

/// Iono-2 v3 result of one de-mean pass over the deviation carry
/// (docs/SPEC_iono2_mechB_v3.md; see @c shDevDemean).
struct ShDevDemeanStats {
    bool   applied{false};  ///< Global mean removed this epoch (n_used ≥ min_global_los).
    int    n_used{0};       ///< Warm entries that participated.
    double g_tecu{0.0};     ///< The removed global weighted mean (signed, vertical TECU).
    int    n_sta{0};        ///< Stations whose per-station mean was also removed.
};

/// Iono-2 v3: contract-enforcing de-mean of the carried deviation priors — the
/// anti-ratchet.  A component of the deviation field that is COMMON in vertical
/// TECU across LOS has the row signature M(el)·tecu_to_m, IDENTICAL to the a00
/// column's, so (common deviation ↔ a00) is an exactly quasi-null pair the data
/// cannot allocate; the per-epoch harvest→age→re-assert alternation ratchets it
/// (measured a00 25 → 104 TECU in 3 h with the carry alone, 207–255 with the
/// dual layer — runs/iono2attr_*).  The per-(station) mean is the receiver-DCB
/// lump one level down.  Removing both from the carried PRIOR leaves them to
/// a00 / the receiver states, where each epoch's data re-derives them — the
/// carry keeps only the per-LOS structure its contract names.
///
/// Serial-context only (the PASS-B pre-pass, after slip-erase + GM aging).
/// Information weights w = 1/max(var, ε) over WARM entries (t_s > 0); the
/// global weighted mean is removed when n ≥ @p min_global_los, then each
/// station's weighted mean of the residual when that station has
/// ≥ @p min_station_los warm LOS.  Variances are left UNCHANGED (conservative:
/// the prior loses an estimated mean, keeps its spread).  Cold entries are
/// untouched.  Key layout "STATION|SAT" (the sh_dev_carry contract).
///
/// @param carry            The deviation-carry map (mutated in place).
/// @param min_global_los   Minimum warm LOS for the global de-mean (spec: 30).
/// @param min_station_los  Minimum warm LOS per station for its mean (spec: 4).
/// @return Per-epoch stats for the run-end counters.
inline ShDevDemeanStats shDevDemean(std::map<std::string, ShDevCarry>& carry,
                                    std::size_t min_global_los,
                                    std::size_t min_station_los) {
    ShDevDemeanStats st;
    constexpr double kEps = 1e-12;
    double sw = 0.0, swd = 0.0;
    for (const auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;   // cold — not yet a prior
        const double w = 1.0 / std::max(kv.second.var, kEps);
        sw += w;
        swd += w * kv.second.dev;
        ++st.n_used;
    }
    if (st.n_used < static_cast<int>(min_global_los) || !(sw > 0.0)) return st;
    st.g_tecu = swd / sw;
    st.applied = true;
    // Per-station accumulation of the residual (single pass: subtract g inline).
    std::map<std::string, std::pair<double, double>> sta;  // station -> {Σw, Σw·dev}
    std::map<std::string, std::size_t> sta_n;
    for (auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;
        kv.second.dev -= st.g_tecu;
        const std::string s = kv.first.substr(0, kv.first.find('|'));
        const double w = 1.0 / std::max(kv.second.var, kEps);
        auto& acc = sta[s];
        acc.first += w;
        acc.second += w * kv.second.dev;
        ++sta_n[s];
    }
    for (auto& kv : carry) {
        if (kv.second.t_s <= 0.0) continue;
        const std::string s = kv.first.substr(0, kv.first.find('|'));
        const auto itn = sta_n.find(s);
        if (itn == sta_n.end() || itn->second < min_station_los) continue;
        const auto& acc = sta.at(s);
        if (acc.first > 0.0) kv.second.dev -= acc.second / acc.first;
    }
    for (const auto& kv : sta_n)
        if (kv.second >= min_station_los) ++st.n_sta;
    return st;
}

// L1: one network_epoch_status.csv row per processed epoch.  Fields not yet in
// scope at a given disposition site are left BLANK (std::nullopt), NOT zero — the
// pyviz timeline viewers (L5) distinguish "unavailable" from a real 0.
// (Hoisted VERBATIM from ApplicationNetwork.cpp, decomposition INC-4; the writer
// body is NetworkRunTraces::emitEpochStatus.)
struct EpochStatusRow {
    std::optional<double> sigma0;
    std::optional<int>    n_adapt;
    std::optional<int>    n_present;
    std::optional<int>    n_clock_sats;
    std::optional<double> clock_median_m;
    std::optional<double> clock_std_m;
    std::string           worst_sat;        // empty string => blank
    std::optional<double> worst_dev_m;
    std::optional<int>    n_refloat_sats;
    std::optional<int>    n_retro_applied;
    std::optional<int>    datum_changed;
    std::optional<int>    n_arc_refloat;   ///< T2d: isolated-arc RAIM refloats THIS epoch (the churn metric; distinct from n_refloat_sats which is the CLOCK-refloat set).
    // --- SPEC_day129_frame_guards P1-1: per-epoch CARRY churn ----------------
    // The Card-4 run-end log line reports RUN totals only (2.55 M chord births,
    // 2.49 M slip refloats, 2.42 M D5 flags/re-taps over DOY-129), which cannot
    // answer "did the churn SPIKE at the collapse epoch?".  These are the same
    // quantities differenced per epoch (NetworkInstallCarry).  Appended columns,
    // existing positions preserved (T2d precedent).
    std::optional<long long> n_d5_flags;
    std::optional<long long> n_d5_retaps;
    std::optional<long long> n_chord_births;
    std::optional<long long> n_slip_refloats;
    // --- P1-2: per-epoch iono-DEWEIGHT state ---------------------------------
    // n_adapt (already above) is a DIFFERENT counter (the within-epoch RAIM
    // adaptation count, 0 all day on the reference run).  These are the SH-VTEC
    // constraint deweight gate's weight field: engaged count, the count driven to
    // w == 0 (whole SH block dropped, per-LOS iono floating), and the min/mean.
    std::optional<int>    n_dw_engaged;
    std::optional<int>    n_dw_zero;
    std::optional<double> dw_min_w;
    std::optional<double> dw_mean_w;
    // --- P1-4: MEASURE-ONLY post-fit variance factor on the emit path --------
    // Present only under network.trace_postfit_sigma0.  sigma0 (above) is the
    // within-epoch RAIM's statistic and is 0.0000 whenever that RAIM is off.
    std::optional<double> sigma0_pf;        ///< robust σ̂0 over ALL post-fit residual rows
    std::optional<double> sigma0_pf_code;   ///< …code rows only
    std::optional<double> sigma0_pf_phase;  ///< …phase rows only
    std::optional<long long> n_pf_rows;     ///< rows in the pooled statistic (0 ⇒ inert)
    // --- redundancy census (docs/SPEC_redundancy_census.md INC-1/INC-2) -------
    // Present only on an epoch the census actually SWEPT (strided, escalated or
    // trip-forced).  A non-sweep epoch leaves all three BLANK rather than
    // repeating the last sweep's numbers — the timeline viewers must be able to
    // see where the sweeps were, and a carried-forward value would read as a
    // per-epoch measurement that was never taken.
    std::optional<long long> n_redun_rows;         ///< DATA rows swept — the DENOMINATOR (1461 of 3300 and 1461 of 40000 are different networks)
    std::optional<long long> n_uncheckable_rows;   ///< of those, rows with r_i < redundancy_eps
    std::optional<double>    uncheckable_max_s;    ///< largest CENTRED standardized prefit deviation among those rows (0 ⇒ swept, none carried a finite metric)
    std::optional<long long> n_unobservable_local; ///< local columns below the relative N_rr pivot floor (INC-2)
    // --- PREFIT census (docs/SPEC_prefit_census_observability.md INC-1) -------
    // Per DATA class {obs_code, obs_phase, obs_other, iono_sh_row} — the
    // enumerator order of NetRowClass, which is also the trace column order.
    // The statistic is the COMMON-MODE-REMOVED standardized prefit deviation
    // s = |y − median_type(y)|·√w (m ≡ 0 for iono_sh_row, which carries no
    // receiver clock), so a whole-station receiver-clock step cancels to ~0 and
    // a single parked cell does not.  Present only under
    // network.trace_prefit_census AND on an epoch that classified rows; blank
    // otherwise (the "unavailable is not 0" convention).
    struct PrefitCensusStats {
        double    max_s[4]{};      ///< largest s in the class this epoch
        double    p99_s[4]{};      ///< 99th percentile of s in the class
        long long n_gt_10[4]{};    ///< rows with s > 10   (the storm-sea shoulder)
        long long n_gt_1000[4]{};  ///< rows with s > 1000 (the fault regime)
    };
    std::optional<PrefitCensusStats> prefit_census;
    // --- measurement-admission census (docs/SPEC_measurement_admission.md INC-3)
    // The ABSOLUTE, product-referenced |z| population — the modality the prefit
    // census structurally lacks, because its statistic subtracts the filter state
    // and therefore reads ~0 on a cell that was born wrong.  The two columns exist
    // in the schema only when network.adm_trace / network.adm_screen is armed, and
    // are blank on an epoch the screen did not run (the "unavailable is not 0"
    // convention: a genuine 0.0 worst-|z| and a skipped epoch say different things).
    std::optional<double> adm_sta_worst_z;  ///< MEDIAN over screened stations of each station's worst |z| (m) — the healthy sea
    std::optional<double> adm_net_worst_z;  ///< network MAXIMUM |z| (m) — the extreme; its identity goes to the ADMISSION events
    // --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md INC-0) -------
    // The per-epoch face of the fold's PD screen.  Always-on and solve-neutral —
    // these are reads of a factorization the fold already computed — because the
    // branch they observe (`M := 0` on a non-PD Pᶻ+B) previously had NO counter,
    // NO log and NO event, and 78 % of the carried state was measured diffusing
    // behind it (2026-08-02).  Blank on an epoch the invariant carry did not fold
    // on, per the "unavailable is not 0" convention: zero degenerate blocks and
    // "the carry never ran" say very different things.
    std::optional<long long> ddfold_degen_blocks;  ///< blocks that folded INERTLY this epoch (PD screen tripped ⇒ M=0)
    std::optional<long long> ddfold_degen_rows;    ///< Σ K over those blocks — carried dimension that took NO data
    std::optional<double>    ddfold_min_pivot_rel; ///< worst relative pivot d_min/d_max this epoch (≤ 0 ⇒ indefinite)
    // --- prior-residual census (docs/SPEC_prior_residual_census.md) -----------
    // The per-epoch face of the carried-prior census.  In the schema only when
    // network.prior_residual_census is armed (the adm precedent — this card's OFF
    // contract is STRUCTURAL byte-identity), and blank on an epoch the census did
    // not run.  The per-(family × label) detail is in network_prior_residual.csv;
    // these five are the timeline channel, and prires_rows is the DENOMINATOR
    // without which the reported count means nothing.
    std::optional<long long> prires_rows;        ///< carried priors censused this epoch, all families and labels
    std::optional<long long> prires_rows_resp;   ///< of those, RESPONDED (an independent mechanism had already fired) — the positive control's population
    std::optional<long long> prires_rows_frame;  ///< of those, FRAME (datum handover / reref / inflate-q / hold) — the invariant-#7 edges
    std::optional<long long> prires_n_reported;  ///< of those, |t| above network.prior_residual_report_sigma (a TRACE threshold; nothing acted)
    std::optional<double>    prires_max_t;       ///< the worst |t| this epoch
    // --- constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14) ----
    // A SEPARATE schema flag from the five above, deliberately: the census's own
    // §9.1 A/B arms compare network_epoch_status.csv across builds, so a
    // census-only run must keep exactly the schema it had before this rider
    // existed.  Present only when network.prior_residual_global_sigma0 is armed.
    std::optional<double>    s0g_sigma0;         ///< σ̂₀_global = sqrt((Σw·v² + Σw·ê²)/(Σr_data + Σr_prior)); blank when Σr ≤ 0 (never 0)
    std::optional<double>    s0g_tension;        ///< Σw·ê² / Σw·v² — the fraction of the epoch's weighted misfit living in the CARRY.  THE HEADLINE.
    std::optional<int>       s0g_flags;          ///< in-band degradation bitmask (@c NetworkPriorResidual::GlobalFlag): 1 APPROX(r≡1), 2 NO_DATA, 4 NO_PRIOR, 8 DEGENERATE, 16 RNEG_DROP
    // --- R0 within-epoch tier observability (SPEC_pivot_visibility_gauge_defect
    // §5.07.5).  Present in the schema only when network.raim_within_epoch is
    // armed (the adm/prires precedent: the OFF contract stays STRUCTURAL
    // byte-identity), blank on an epoch the tier did not assess.  sigma0_first is
    // the pre-adaptation σ̂₀ — the null sample QUEUE item 4 needs on disk, which
    // previously existed only inside a trip event's detail span.  n_we_refloat_sats
    // is the tier's OWN whole-sat refloat count (n_refloat_sats above counts the
    // clock-COUPLE set, empty unless clock_monitor_couple_raim).  we_clk_move_* is
    // the §5.07.2 impulse-channel meter: how far the within-epoch re-solves moved
    // a satellite clock off the PRIMARY solve (0.0 = tier assessed, no re-solve).
    std::optional<double> sigma0_first;
    std::optional<int>    n_we_refloat_sats;
    std::optional<double> we_clk_move_max_m;
    std::string           we_clk_move_max_sat;   // empty = blank
};

/// Decomposition INC-7 (plan §Control flow): disposition of a relocated
/// epoch-loop phase, consumed at the runNetwork call site.  SkipEpoch replaces
/// a former epoch-loop `continue` (the site-specific bookkeeping — n_skipped,
/// trace row, epoch-status row, prev_week/prev_tow advance — stays verbatim
/// INSIDE the relocated body); StopRun is reserved for phases that formerly
/// `break` (none at INC-7 — the max-duration break stays in the prologue).
enum class EpochAction { Proceed, SkipEpoch, StopRun };

/// @internal Per-epoch workspace of Application::runNetwork (decomposition
/// INC-4).  Constructed FRESH at the top of every epoch iteration — NEVER
/// hoisted for reuse: fresh construction IS the per-epoch reset semantics.
/// v1 scope is deliberately the RAIM-replay capture channel plus the two
/// clock-model prediction maps; per-epoch state joins as its consuming blocks
/// move out of the run-loop TU (INC-7/9).
///
/// RAIM-replay capture channel (network.raim_within_epoch): the within-epoch
/// re-solve (resolve_dropping) replays the epoch over a fresh beginEpoch(), so
/// every shared-border input folded OUTSIDE the per-station assembly must be
/// captured when armed and re-applied verbatim.  WRITER SITES (the complete
/// list — a new prior fold elsewhere MUST mirror into this channel):
///   - cap_H / cap_z / cap_constraint_weight / cap_has_constraint: the W6
///     satellite code-bias datum constraint rows (SB_SOLVEGLUE, W6 block).
///   - cap_sbc_H / cap_sbc_z: the SBC S-basis completion gauge rows
///     (SB_SOLVEGLUE, network.sbasis_complete block; re-applied at
///     NetworkSbasis::kSbasisCompleteWeight — the weight is the compile
///     constant, so no weight field travels).  rows() == 0 ⇒ nothing to
///     re-apply.
///   - cap_priors: SH-VTEC pre-gate prior (networkInjectShVtecPrior_), Fix-A
///     held phase-bias datum prior (networkInjectPhaseHoldPrior_), the
///     drop-and-heal soft anchors (NetworkPhaseDatum healNearNullPhaseBias, gate-failure path),
///     the D2 diagonal carry prior + dense-fallback leftovers
///     (networkInjectD2CarryPrior_), the two-state clock-model prior
///     (networkInjectClockOscPrior_), and the a-priori satellite-clock
///     Tikhonov anchor.
///   - cap_dense_keys / cap_dense_cov: the C1 dense shared-carry fold
///     (networkInjectD2CarryPrior_, dense-success branch only; its non-PD
///     fallback routes through cap_priors like every diagonal prior).
struct EpochWorkspace {
    Eigen::MatrixXd cap_H;
    Eigen::VectorXd cap_z;
    double cap_constraint_weight = 0.0;
    bool   cap_has_constraint = false;
    Eigen::MatrixXd cap_sbc_H;   ///< SBC completion rows (see writer-site list)
    Eigen::VectorXd cap_sbc_z;   ///< SBC completion targets
    std::vector<NetworkEstimator::SharedPrior> cap_priors;
    std::vector<NetworkEstimator::SharedPrior> cap_dense_keys;
    Eigen::MatrixXd cap_dense_cov;
    /// Two-state clock-model predictions stashed by the prior fold
    /// (networkInjectClockOscPrior_), consumed by the post-solve normalized-
    /// innovation test (T5) later in the same epoch.
    std::map<std::string, ClockOscPred> clk_pred;
    /// B7 (P1-4): per-satellite folded a-priori-clock-anchor information
    /// (Λ diagonal) — populated only when the anchor-info trace is open;
    /// consumed by the B7 anchor-fraction diagnostic after the solve.
    std::map<std::string, double> clk_prior_info;
};

/// Epoch-scratch candidate census consumed by EVERY prioritized phase-bias
/// datum realization (ensemble / eq34 chain / anchor election).  Built once per
/// epoch by @ref networkBuildPhaseDatumCandidates (filter/NetworkPhaseDatum.hpp)
/// and passed by const reference to whichever election the mode dispatch picks;
/// nothing downstream of the election reads it.
///
/// Member names are deliberately the FORMER run-TU local names, so the bodies
/// relocated at decomposition INC-8b alias them without renaming.
///
/// ⚠ `union_keys` is POST-gate: the network.phase_datum_min_stations coverage
/// gate erases from it IN PLACE inside the builder.  There is no pre-gate copy
/// and none may be added — a pre-gate census would re-admit every one-tracker
/// (sat,signal) into shared_phase_keys, which the Card-3 D3c RPB allocation
/// reads directly (the non-PD-ejection / one-station-re-level / split-leak
/// class the gate exists to prevent).
struct PhaseDatumCandidates {
    /// Per station: satellite -> elevation (deg), from the PRUNED observations.
    std::vector<std::map<std::string, double>>      sat_el;
    /// Per station: "sat_signal" -> "sat|signal|PB" (the per-(sat,signal)
    /// phase-bias STATES this station allocated in PASS A — the ANCHORABLE keys).
    std::vector<std::map<std::string, std::string>> usable_keys;
    /// constellation -> station index -> # satellites tracked at >= pref_elev.
    std::map<char, std::map<std::size_t, int>>      cover;
    /// constellation -> candidate station indices (can anchor >= 1 of its keys).
    std::map<char, std::set<std::size_t>>           cand_set;
    /// "sat_signal" -> "sat|signal|PB", coverage-gated union over present stations.
    std::map<std::string, std::string>              union_keys;
    /// Card EV7 §6: the "sat_signal" keys the coverage gate EXCLUDED this epoch —
    /// exactly what was erased from @ref union_keys above, kept because the DROP
    /// EDGE (filter/NetworkPhaseGateDrop.hpp) is the difference between this set
    /// and the previous epoch's anchor incumbents.  Empty when the gate is off
    /// (network.phase_datum_min_stations = 1) and whenever it excluded nothing.
    /// Observational: no election reads it, and the hygiene pass's own erasures
    /// from @ref union_keys are deliberately NOT in it (a different mechanism with
    /// its own bridge lifecycle).
    std::set<std::string>                           gated_keys;
};

} // namespace GnssProcessor
