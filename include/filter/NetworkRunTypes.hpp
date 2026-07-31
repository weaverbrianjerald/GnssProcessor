#pragma once
/**
 * @file NetworkRunTypes.hpp
 * @brief @internal Run-loop record types for Application::runNetwork — the
 *        per-station bundle, PASS-B assembly slot, and the D2/Card-1 carry PODs.
 *
 * Hoisted VERBATIM from the ApplicationNetwork.cpp anonymous namespace
 * (decomposition INC-2, 2026-07-16; see
 * docs/network_decomposition_addendum_2026-07-16.md).  Support header for the
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

#include <Eigen/Dense>

#include <array>
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
};

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

/// One carried-forward ambiguity (phase-leveling, Increment 1): the float
/// ambiguity's ABSOLUTE value, its posterior variance, and the continuous-GPS-
/// second time it was last seen (for the inter-epoch gap inflation / stale-carry
/// drop).  Keyed in @c local_carry by (station index, satellite, signal) so it
/// survives a satellite blinking in/out.
struct LocalCarry {
    double value_abs;     ///< Carried absolute ambiguity estimate (m).
    double var;           ///< Last-solve posterior variance (m²): the MARGINAL (conditionalLocalVarianceDiag) on the legacy diagonal carry, or — under network.carry_conditional_coupling — the PURE CONDITIONAL @f$(N_{rr}^{-1})_{rr}@f$.
    double last_seen_s;   ///< Continuous-GPS seconds when last stashed (gap inflation).
    std::vector<CoupledShared> coupling{};  ///< Card 1: the regression on the FREE shared border.  Empty on the legacy diagonal path AND for a warm-resume-restored entry (not persisted in the v1 snapshot) — an empty coupling injects as a plain diagonal row (graceful one-epoch degrade).
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
};

/// One present station ZWD column this epoch (B9): its A_r column and the ZWD
/// linearization point, for the post-solve harvest/stash (mirrors @ref PlAmb).
struct PlZwd {
    Eigen::Index col;     ///< A_r column of the ZWD state (= h2local[tropo_idx]).
    double       x_lin;   ///< This epoch's ZWD linearization point (state value).
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
    std::string      err;                  ///< Diagnostic (mismatch detail or exception text).
    // Phase-leveling carry (Increment 1) — built in PASS B, consumed serially.
    std::vector<Eigen::Index> carry_cols;  ///< A_r columns receiving a carried prior (ASCENDING — determinism).
    Eigen::VectorXd           carry_dNrr;  ///< Per-column information increment Λ = 1/var_eff.
    Eigen::VectorXd           carry_dur;   ///< Per-column RHS increment η = Λ·(value_abs − x_lin).
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
    std::vector<PlAmb>        pl_amb;       ///< Present non-pivot ambiguities, for the post-solve stash.
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
    /// SD pre-reduce EWL (docs/SPEC_sd_ewl_prereduce.md, @c network.sd_ewl.trace):
    /// pre-formatted `network_sd_ewl.csv` rows for THIS station-epoch — one per
    /// satellite visit, carrying the elected signal pair, the arc outcome, and the
    /// row's z/σ.  Buffered here rather than written in place because the row build
    /// runs in the PARALLEL produce phase; PASS B's SERIAL consume drains these in
    /// station order, so the file is deterministic for any thread count.  Empty
    /// unless the trace is armed (log-only — it changes no solve).
    std::vector<std::string>  sd_ewl_trace;
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
};

} // namespace GnssProcessor
