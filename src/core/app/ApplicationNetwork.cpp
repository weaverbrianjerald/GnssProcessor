/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * Extracted from Application.cpp to keep the per-mode processing pipelines in
 * dedicated translation units (mirrors ApplicationBaseline.cpp /
 * ApplicationRealtime.cpp).  The network producer is a joint multi-station
 * estimator: per-station local normals reduced onto a shared satellite-
 * parameter block via an exact Schur complement, estimating the estimable
 * satellite clock + satellite phase bias under an explicitly declared
 * minimum-constraint S-basis, emitted in IGS-SSR convention for the companion
 * PPP_RTK_User consumer.
 *
 * See docs/ssystem_conformance_2026-06-15.md (§7) and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * ===========================================================================
 * W4-S2b — the live epoch driver
 * ===========================================================================
 *
 * Per solution epoch the driver runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  for each present station: PppModel::computeModels →
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-sat clock + per-(sat,signal) phase bias) using the
 *       SAME usability gate the measurement build uses.  The DATUM (pivot)
 *       station is processed first so its usable phase-signal set restricts the
 *       phase-bias columns to ones backed by a pivot-receiver ambiguity pin.
 *       The estimator's shared column layout is then registered (clock for every
 *       satellite any station sees; phase bias for the pivot's signal set).
 *
 *   PASS B (assembly)  for each present station: MeasurementModel::compute-
 *       MeasurementUpdate produces an undifferenced H/innovations/R block (no
 *       receiver-side differencing — DifferencingMode::None).  Its columns are
 *       split: the satellite clock (−1) and phase-bias (+1) columns scatter into
 *       the shared border A_s; the 3 receiver-position columns are DROPPED
 *       (coordinates held fixed); the rest become the station's private nuisance
 *       block A_r.  The pivot station additionally folds tight-prior pins on its
 *       own ambiguities (the LOCAL half of the S-basis — the emitted phase bias
 *       then carries that pivot-receiver ambiguity lump, Odijk Eq. 4).
 *
 *   SOLVE  NetworkSbasis builds the SHARED minimum-constraint datum (one
 *       satellite-clock pin per constellation), the null-space rank gate
 *       verifies the reduced shared system carries exactly that deficiency, then
 *       NetworkEstimator::solve produces the shared estimate.  NetworkProduct-
 *       Emitter serializes it into an OWNED SsrCorrectionStore (closed-loop
 *       isolation — never the process-wide singleton the user side reads).
 *
 *   D2 (between-epoch)  the shared posterior (value, variance) is carried
 *       forward as a diagonal prior on the next epoch's shared border, inflated
 *       by the per-parameter process noise over the epoch gap.
 *
 * Scope (V1): estimable satellite CLOCK + PHASE BIAS, coordinates/orbits fixed,
 * GPS + Galileo, per-band uncombined, float (no network AR).  Satellite code
 * bias, atmosphere products, real-time branch, GLONASS FCN bias, and network QC
 * are later slices (W6/W7/Phase 2).  Each station's coordinates MUST be known
 * (held fixed) and its observation RINEX path MUST be explicit in the config —
 * the PathResolver network gate is deferred, so resolveData() here serves only
 * products + broadcast nav.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // Card 4 (M1) phase D: emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // M5 prereq: PB emission-continuity filter
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // M4: combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // RTB-7: atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // Card 5 (RTF-1): broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/StateKeySchema.hpp"
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "slip/MwLaneTiers.hpp"         // UWL-1 per-constellation lane-tier band table
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

namespace {

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
    double          value;
    double          var;
    bool            inflate_q{false};  ///< Clock-monitor flagged this satellite: fold NEXT epoch with the large reinit variance (a soft large-q reinitialization that lets a frozen clock walk toward truth while retaining a weak anchor) instead of the stiff process noise.
};

/// Identity key of a dense-carried shared parameter (C1) — mirrors
/// NetworkEstimator's internal key construction so the stash and fold agree.
/// SH-VTEC never enters the dense block (its carry is the diagonal pre-gate
/// fold), so the (n,m,trig) family needs no key here.
static std::string sharedCarryKey(SharedParamKind kind, const std::string& sat,
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
    double coeff{0.0};    ///< @f$M_{rj}@f$ (regression coeff of the local on this shared col)
    double s_prev{0.0};   ///< @f$\hat s_{\text{prev},j}=x_s(j)@f$ last epoch (correction frame)
    double p_prev{0.0};   ///< @f$P_{\text{prev},jj}@f$ last-epoch datum-frame variance (departed-col marginalization)
};

/// Relative sparsification threshold for a carried column's regression row: an
/// entry @f$M_{rj}@f$ is stashed only when @f$|M_{rj}| > @f$ @c kCouplingSparsifyRelThresh
/// @f$\cdot\max_k|M_{rk}|@f$ (Card 1).  1e-8 keeps only the numerically meaningful
/// couplings; the dropped near-zero entries contribute negligibly to the row.
constexpr double kCouplingSparsifyRelThresh = 1e-8;

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
static bool whitenCouplingRows(const std::vector<CplRowRaw>& rows,
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

/// Elevation mapping for the IF-phase slip stochastic model: the multiplicative
/// factor by which between-epoch double-differenced carrier-phase noise inflates
/// toward the horizon.  A residual at elevation @p el_deg is normalized to a
/// zenith-equivalent by DIVIDING by this factor (≡ multiplying by sin el); the
/// empirical scale (pooled or per-station EWMA) is re-inflated by it per satellite
/// when thresholding, so ONE scale is valid across the whole sky.  1/sin(el) is the
/// standard first-order phase-noise shape; clamped at 5° so it cannot blow up at the
/// (already elevation-masked) horizon.  A future refinement is a fitted a+b/sin(el).
inline double ifElevMap(double el_deg) {
    constexpr double kMinElDeg = 5.0;
    const double el_rad = std::max(el_deg, kMinElDeg) * (Constants::PI / 180.0);
    const double s = std::sin(el_rad);
    return (s > 1e-3) ? (1.0 / s) : 1.0;
}

/// Median of a copy (upper-middle for even counts, matching this file's convention).
inline double medianCopy(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
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

/// Robust prominence of each entry in @p eff: how many robust-sigmas its effect stands
/// above the dimension's own median (z = (effect − median) / (1.4826·MAD)).  This is the
/// "peak stands above the surrounding terrain" measure used to attribute a network
/// residual spike to a bad satellite / station / signal.
template <class KeyT>
std::map<KeyT, double> robustProminence(const std::map<KeyT, double>& eff) {
    std::vector<double> vals;
    vals.reserve(eff.size());
    for (const auto& kv : eff) vals.push_back(kv.second);
    const double m = medianCopy(vals);
    std::vector<double> ad;
    ad.reserve(vals.size());
    for (double v : vals) ad.push_back(std::abs(v - m));
    double mad = 1.4826 * medianCopy(ad);
    if (!(mad > 0.0)) mad = 1.0;
    std::map<KeyT, double> out;
    for (const auto& kv : eff) out[kv.first] = (kv.second - m) / mad;
    return out;
}

/// Result of a Tukey 2-way median polish: value ≈ overall + row_eff[row] + col_eff[col]
/// + interaction (parallel to the input cell order).
struct MedianPolish {
    double overall{0.0};
    std::map<int, double> row_eff;          ///< per-row (station) effect
    std::map<std::string, double> col_eff;  ///< per-column (satellite) effect
    std::vector<double> interaction;        ///< leftover per cell (isolated arc anomaly)
};

/// Robust additive decomposition of a sparse (row=int, col=string) → value table by
/// alternately sweeping out row and column medians (Tukey median polish).  Medians make
/// it robust: a minority of huge cells (the real outliers) do not distort the additive
/// row/column effects — which is the whole point (separate a bad satellite that lights up
/// a whole column from a bad station that lights up a whole row, and leave the isolated
/// (station,sat) arc slips in the interaction term).  O(iters · cells); no re-solve.
inline MedianPolish medianPolish2Way(
        const std::vector<std::tuple<int, std::string, double>>& cells, int iters = 5) {
    MedianPolish mp;
    std::vector<double> R(cells.size());
    std::map<int, std::vector<std::size_t>> by_row;
    std::map<std::string, std::vector<std::size_t>> by_col;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        R[k] = std::get<2>(cells[k]);
        by_row[std::get<0>(cells[k])].push_back(k);
        by_col[std::get<1>(cells[k])].push_back(k);
    }
    for (const auto& kv : by_row) mp.row_eff[kv.first] = 0.0;
    for (const auto& kv : by_col) mp.col_eff[kv.first] = 0.0;
    auto med_idx = [&](const std::vector<std::size_t>& idx) {
        std::vector<double> v;
        v.reserve(idx.size());
        for (std::size_t k : idx) v.push_back(R[k]);
        return medianCopy(v);
    };
    for (int it = 0; it < iters; ++it) {
        for (const auto& [row, idx] : by_row) {
            const double rm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= rm;
            mp.row_eff[row] += rm;
        }
        {
            std::vector<double> re;
            for (const auto& kv : mp.row_eff) re.push_back(kv.second);
            const double m = medianCopy(re);
            mp.overall += m;
            for (auto& kv : mp.row_eff) kv.second -= m;
        }
        for (const auto& [col, idx] : by_col) {
            const double cm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= cm;
            mp.col_eff[col] += cm;
        }
        {
            std::vector<double> ce;
            for (const auto& kv : mp.col_eff) ce.push_back(kv.second);
            const double m = medianCopy(ce);
            mp.overall += m;
            for (auto& kv : mp.col_eff) kv.second -= m;
        }
    }
    mp.interaction = std::move(R);
    return mp;
}

/// Parse an IfPhaseSlipDetector reset key ("G01_L1C_L2W") into the satellite and
/// the two band signals of the IF pair it was formed from.  Returns false on a
/// malformed key.  The network carries per-band ambiguities, so a flagged IF pair
/// must refloat BOTH bands — the IF combination cannot localize which one slipped.
inline bool parseIfPairResetKey(const std::string& key, std::string& sat,
                                std::string& sig1, std::string& sig2) {
    const auto p1 = key.find('_');
    if (p1 == std::string::npos || p1 == 0) return false;
    const auto p2 = key.find('_', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1 || p2 + 1 >= key.size()) return false;
    sat  = key.substr(0, p1);
    sig1 = key.substr(p1 + 1, p2 - p1 - 1);
    sig2 = key.substr(p2 + 1);
    return !sat.empty() && !sig1.empty() && !sig2.empty();
}

} // namespace

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

    // --- Phase 0: validate inputs (explicit RINEX + fixed coordinates) ------
    // V1 requires an explicit observation RINEX path per station: the
    // PathResolver network gate (filling paths from the internal data tree by
    // station name) is deferred to avoid editing the shared Application.cpp
    // resolve path while concurrent sessions are in flight.  Coordinates are
    // validated by loadNetworkSection_ (every station needs a known position).
    for (const auto& s : nc.stations) {
        if (s.rinex_path.empty()) {
            log->error("Network: station '{}' has no observation rinex_path. V1 "
                       "requires an explicit network.stations[].rinex_path for "
                       "every station (PathResolver network auto-discovery is "
                       "deferred).", s.name);
            return 1;
        }
    }

    // --- Phase 1: resolve products + broadcast nav --------------------------
    // Station RINEX is explicit, so resolveData() here only fills product /
    // navigation paths (orbits + clocks needed for per-station geometry).  A
    // failure is non-fatal: the product-cache load below surfaces a precise
    // "no orbits" diagnostic if products are genuinely missing.
    if (!resolveData()) {
        log->warn("Network: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config (product-cache load will report "
                  "if orbits are missing).");
    }

    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }

    // --- Phase 2: multi-station RINEX ingestion + epoch alignment -----------
    // MultiRinexSync is driven by a BaselineConfig roster; adapt the network
    // stations into it with the clock-datum station as the "reference" so the
    // synced schedule walks its epochs (by_station includes the reference).
    BaselineConfig roster;
    roster.epoch_sync_tolerance_s = nc.epoch_sync_tolerance_s;
    bool pivot_found = false;
    for (const auto& s : nc.stations) {
        if (s.name == nc.clock_datum_station) {
            roster.reference = s;
            pivot_found = true;
        } else {
            roster.rovers.push_back(s);
        }
    }
    if (!pivot_found) {
        log->error("Network: clock_datum_station '{}' not in stations list",
                   nc.clock_datum_station);
        return 1;
    }

    // Closed-loop integer-recovery harness: a HELD-OUT consumer station, added to
    // the sync roster ONLY so its observations are time-aligned with the network
    // schedule — it is deliberately kept OUT of the joint estimator stations
    // (built below) and instead consumes the producer's emitted SSR products.
    const ClosedLoopConfig& cl = nc.closed_loop;
    const bool closed_loop_on = cl.enable && !cl.station.name.empty();

    // Card 5 (RTF-1): a-priori orbit/clock source.  sp3 (default) = the precise
    // SP3/CLK cache is the a-priori (today's behaviour, byte-identical).  brdc_rts =
    // the a-priori is BROADCAST ephemeris (LiveEphemerisStore) — the self-sufficient
    // real-time service mode.  Threaded through Phase 3 (nav load + SP3-error lift),
    // the producer station gate-suppression (D1), the anchor-σ re-base (D6), the
    // clock-IODE stamp, and the apriori_product_id.  Every use is guarded on this
    // flag so the sp3 path is textually unchanged.
    const bool brdc_rts =
        (config_.networkAprioriSource() == NetworkAprioriSource::BrdcRts);
    if (closed_loop_on) {
        roster.rovers.push_back(cl.station);
        log->info("Network: closed-loop consumer '{}' added to the sync roster "
                  "(held OUT of the joint solve)", cl.station.name);
    }

    MultiRinexSync sync(config_, roster, log_manager_, *product_parser_);

    // Clip each station's parse to the processing window.  The epoch loop below
    // is bounded by processing.max_duration_s (anchored on the first synced
    // epoch); without this, load() would parse — and synchronize() would copy —
    // the FULL 24h of every station even when only the first minutes are used,
    // which dominates wall-time at ~50 full-day stations.  A small margin covers
    // per-station first-epoch jitter; max_duration_s <= 0 keeps full-file parsing.
    //
    // The parse window is anchored at the file start, but network.start_offset_hours
    // fast-forwards the epoch loop PAST that offset before the max_duration cutoff
    // begins counting (the cutoff anchors on the first PROCESSED epoch).  So the
    // window must cover [0, start_offset + max_duration + margin]; clipping to only
    // max_duration would strip out the offset window and leave zero epochs to solve.
    if (config_.getMaxDuration() > 0.0) {
        const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;
        const double span_s = start_offset_s + config_.getMaxDuration() + 60.0;
        sync.setParseWindowSeconds(span_s);
        if (start_offset_s > 0.0)
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each station "
                      "(start_offset {:.0f} s + max_duration_s {:.0f} s + 60 s margin)",
                      span_s, start_offset_s, config_.getMaxDuration());
        else
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each "
                      "station (max_duration_s {:.0f} s + 60 s margin)",
                      span_s, config_.getMaxDuration());
    }

    if (!sync.load()) {
        // Non-fatal: a station that failed to parse is excluded from the aligned
        // schedule (synchronize() skips it; the datum/pivot is guarded below in
        // buildStation).  This lets a large roster proceed when some stations'
        // observation files are missing or corrupt — the expected case when
        // fetching ~50 stations from a public archive.
        log->warn("Network: one or more station RINEX files failed to parse; "
                  "proceeding with the stations that loaded (the datum station "
                  "is required and is checked next).");
    }
    if (!sync.synchronize()) {
        log->error("Network: failed to synchronise epochs across stations");
        return 1;
    }
    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Network: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether the station files overlap in time");
        return 1;
    }
    log->info("Network: multi-station sync complete — {} synchronised epochs", synced.size());

    // --- Phase 3: products + cache for geometry -----------------------------
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Network: failed to initialise product cache — no broadcast nav / orbits");
        return 1;
    }
    log->info("Network: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    // Card 8 (replay parity): staged broadcast nav records + feed cursor for the
    // progressive LiveEphemerisStore insertion in the epoch loop (brdc_rts only;
    // empty otherwise).  Availability = GPS-continuous-scale toc (BDS toc is
    // native BDT → shifted to the GPS scale, mirroring LiveEphemerisStore's
    // toe convention).
    auto navAvailabilityGpsSeconds = [](const BroadcastEphemeris& e) {
        double c = static_cast<double>(e.kepler.toc.week) * 604800.0 + e.kepler.toc.tow;
        if (e.system == System::BeiDou) c += 1356.0 * 604800.0 + 14.0;   // BDT week 0 = GPS week 1356, BDT = GPS − 14 s
        return c;
    };
    std::vector<BroadcastEphemeris> brdc_nav_staged;
    std::size_t brdc_nav_cursor = 0;
    if (brdc_rts) {
        // Card 5 (RTF-1): the a-priori is BROADCAST ephemeris, not SP3/CLK.  Parse the
        // RINEX navigation file into the shared LiveEphemerisStore — the SAME store the
        // real-time engine fills from RTCM 1019/1042/1044/1046 — so replay resolves
        // satellite orbit+clock through the identical code path as a live run (frame
        // consistency, conventions §1 / plan D4).  Geometry::getSatellitePV falls back
        // to this store when no SP3 is loaded, and GnssCorrections::getSatelliteClock-
        // Correction supplies the broadcast SV clock (marked broadcast-grade) when no
        // precise CLK covers the epoch — exactly the broadcast a-priori this mode needs.
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (brdc_rts): network.apriori_source = brdc_rts requires a "
                       "broadcast navigation file, but files.navigation_path is empty. "
                       "Set it, or let PathResolver auto-resolve a BRDC / _MN.rnx file "
                       "covering the observation day.");
            return 1;
        }
        // Parse first so a per-constellation loaded-sat summary is available.
        //
        // Card 8 fix (replay parity, plan D4): do NOT bulk-insert the whole day.
        // LiveEphemerisStore retains only CURRENT+PREVIOUS per satellite (live
        // streaming semantics — the retention Cards 3/6/7 pin against), so a
        // full-file load leaves each satellite holding its LAST two records of
        // the day; at any processing epoch more than a fit window (~4 h GPS,
        // ~2 h GAL/BDS) before end-of-day the staleness guard then rejects
        // EVERY satellite and the producer degenerates to n_present = 0.
        // Instead STAGE the records sorted by availability and feed them to
        // the store progressively as the epoch loop advances, so the store's
        // current/previous evolve exactly as they would receiving a live nav
        // stream.  Availability proxy = GPS-scale toc: the file carries no
        // receipt stamp (RinexNavParser sets received_time = {0,0}), and toc
        // is the closest file-derivable stand-in for "a live receiver holds
        // this record" (live GPS broadcasts a data set from ~toe−2h, so the
        // toc proxy is conservative by up to one fit half-window — records
        // roll slightly LATER than live, never prematurely).
        std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        std::map<char, std::set<std::string>> nav_by_const;
        for (const auto& e : nav)
            if (!e.satellite.empty()) nav_by_const[e.satellite[0]].insert(e.satellite);
        if (nav.empty()) {
            log->error("Network (brdc_rts): parsed 0 usable broadcast ephemerides from '{}'. "
                       "Cannot form a broadcast a-priori — check the file is a "
                       "RINEX 3/4 navigation file covering the observation day.",
                       nav_path.string());
            return 1;
        }
        std::stable_sort(nav.begin(), nav.end(),
                         [&](const BroadcastEphemeris& a, const BroadcastEphemeris& b) {
                             return navAvailabilityGpsSeconds(a) < navAvailabilityGpsSeconds(b);
                         });
        brdc_nav_staged = std::move(nav);
        {
            std::size_t n_sats = 0;
            std::string summary;
            for (const auto& kv : nav_by_const) {
                n_sats += kv.second.size();
                summary += fmt::format("{}{}={}", summary.empty() ? "" : " ",
                                       kv.first, kv.second.size());
            }
            log->info("Network (brdc_rts): broadcast a-priori staged from '{}' — {} records, "
                      "{} satellites [{}]; fed to LiveEphemerisStore progressively by toc "
                      "(live-arrival replay).", nav_path.filename().string(),
                      brdc_nav_staged.size(), n_sats, summary);
        }
        // ANY precise product present in brdc_rts is an unsupported combination — refuse
        // rather than mask it (revisit when the RTS-orbit seam, plan D9, lands).  BOTH the
        // orbit AND the clock cache must be empty: a precise SP3 shadows the broadcast
        // a-priori via getSatellitePV node interpolation, and a precise CLK shadows the
        // broadcast clock in getSatelliteClockCorrection (interpolateClock wins over the
        // broadcast fallback for covered epochs → used_broadcast=false, NOT broadcast-grade).
        // Either alone is a silent orbit/clock frame split: the estimated C0 becomes a delta
        // against the wrong baseline while its stamped IODE still names the broadcast eph, so
        // the consumer reconstructs brdc(iode)+C0 and is metre-level wrong.  Gate on orbit OR
        // clock count (getOrbitCount alone missed a CLK-without-SP3 product set).
        if (product_cache.getOrbitCount() > 0 || product_cache.getClockCount() > 0) {
            log->error("Network (brdc_rts): precise products are loaded alongside "
                       "apriori_source = brdc_rts ({} SP3 orbit node(s), {} CLK sample(s)). This "
                       "combination is not supported — a precise orbit/clock would shadow the "
                       "broadcast a-priori (a silent orbit/clock frame split). Remove the SP3/CLK "
                       "product paths, or use apriori_source = sp3.",
                       product_cache.getOrbitCount(), product_cache.getClockCount());
            return 1;
        }
    } else if (product_cache.getOrbitCount() == 0) {
        // sp3 mode (default): the precise orbit cache IS the a-priori — unchanged.
        log->error("Network: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path / products to cover the observation day.");
        return 1;
    }

    // --- Phase 3b: GIM for satellite code-bias estimation (W6) ---------------
    // The satellite DCB (geometry-free code bias) is co-linear with each station's
    // free per-(rx,sat) slant ionosphere until an external constraint imposes
    // between-satellite structure.  When network.estimate_sat_code_bias is on,
    // load the IONEX GIM and fold a common-mode-free pseudo-observation per station
    // (see appendGimIonoRows); without it the satellite DCBs stay in the null space
    // and the rank gate withholds every epoch, so this is a hard prerequisite.
    // The satellite-DCB iono constraint comes from ONE of two mutually-exclusive
    // sources: the external GIM (legacy W6) or the co-estimated SH-VTEC field
    // (estimate_iono_sh).  They are never both applied — when the network estimates
    // its own field it does NOT load a GIM.
    std::unique_ptr<SphericalHarmonicIonosphere> sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    if (config_.estimateNetworkIonoSh()) {
        sh_model = std::make_unique<SphericalHarmonicIonosphere>(shc.max_degree);
        log->info("Network (SH-iono): co-estimating own global SH-VTEC field — degree {} "
                  "({} coeffs), shell {:.0f} km, {} longitude; external GIM NOT loaded.",
                  sh_model->maxDegree(), sh_model->numCoeffs(), shc.layer_height_km,
                  shc.sun_fixed ? "sun-fixed" : "geographic");
        // T1a: echo the a₀₀ physical band the per-epoch divergence WARN uses.
        if (shc.a00_warn_low_tecu < shc.a00_warn_high_tecu)
            log->info("Network (SH-iono): a₀₀ physical band [{:.1f}, {:.1f}] TECU — a per-epoch "
                      "WARN fires if the estimated mean VTEC leaves it (field-divergence probe).",
                      shc.a00_warn_low_tecu, shc.a00_warn_high_tecu);
        else
            log->info("Network (SH-iono): a₀₀ physical-band WARN disabled (low ≥ high).");
    } else if (config_.estimateNetworkSatCodeBias()) {
        // Card 5 belt-and-braces: the external-GIM code-bias path is post-processing-only
        // (a GIM is a daily product, unavailable in the self-sufficient brdc_rts service).
        // Card 1's load-time validation rejects brdc_rts + estimate_sat_code_bias +
        // !estimate_iono_sh as a hard error, so reaching this branch in brdc_rts means the
        // guard was bypassed — refuse rather than silently loading a GIM the service can't have.
        if (brdc_rts) {
            log->error("Network (brdc_rts): reached the external-GIM code-bias path, which is "
                       "post-processing-only. Set network.estimate_iono_sh = true to co-estimate "
                       "the ionosphere field, or disable network.estimate_sat_code_bias "
                       "(this combination should have been rejected at config load).");
            return 1;
        }
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log->error("Network (W6): network.estimate_sat_code_bias is on but no usable IONEX "
                       "GIM was resolved (looked for a .INX under the date's products/). The "
                       "satellite code bias is unobservable without the GIM ionosphere "
                       "constraint — supply a GIM (same analysis centre as the orbits/clocks for "
                       "bias consistency), enable network.estimate_iono_sh to co-estimate the "
                       "field instead, or disable estimate_sat_code_bias.");
            return 1;
        }
        log->info("Network (W6): GIM loaded for the satellite code-bias ionosphere constraint "
                  "(common-mode-free per-constellation weighting).");
    }

    // --- Network-wide signal coverage gate (network.signal_min_station_fraction) ---
    // A RINEX code enters the solve only if enough of the network tracks it, so a
    // sparse channel (e.g. GPS L2S, ~13% of stations) is dropped EVERYWHERE rather
    // than mixed in only at the stations that happen to track it (the flaw of the
    // per-station channel_mode).  Every retained signal's satellite bias is then
    // estimated from a dense, consistent station set.  Coverage is tallied over the
    // synced epochs: a station "tracks" a code if it reports it for any satellite in
    // any epoch; coverage = (#stations tracking the code) / (#stations observing that
    // constellation).  Off (0) leaves the allowlist empty ⇒ every tracked channel is
    // admitted (byte-identical to the ungated producer).
    // Two independent gates share ONE coverage tally: signal_min_station_fraction
    // DROPS a sparse code (PppModel::setSignalAllowlist); phase_bias_min_station_
    // fraction instead FLOATS it — keeps the observation but withholds its satellite
    // phase-bias state (MeasurementModel::setPhaseBiasSignalAllowlist), so the float
    // ambiguity absorbs the bias and the signal is not AR-eligible.  Both default 0
    // (empty allowlists ⇒ byte-identical).
    std::map<std::string, std::set<std::string>> signal_allowlist;       // DROP gate (code + phase)
    std::map<std::string, std::set<std::string>> phase_bias_allowlist;   // FLOAT gate (phase only)
    const double sig_cover_min = config_.networkSignalMinStationFraction();
    const double pb_cover_min  = config_.networkPhaseBiasMinStationFraction();
    if (sig_cover_min > 0.0 || pb_cover_min > 0.0) {
        std::map<std::string, std::set<std::pair<std::string, std::string>>> sta_tracks; // station -> {(system, code)}
        for (const auto& se : synced) {
            for (const auto& [sta, sat_obs] : se.by_station) {
                if (sat_obs.empty()) continue;
                auto& trk = sta_tracks[sta];
                for (const auto& ob : sat_obs) {
                    const std::string sys = PppUtils::getSystemString(ob.system);
                    if (sys == "Unknown") continue;
                    for (const auto& [code, val] : ob.measurements) {
                        (void)val;
                        if (code.empty()) continue;
                        const char t = code[0];
                        if (t != 'C' && t != 'L') continue;   // code + phase channels only
                        trk.insert({sys, code});
                    }
                }
            }
        }
        std::map<std::string, int> sys_n;                          // system -> #stations observing it
        std::map<std::pair<std::string, std::string>, int> code_n; // (system, code) -> #stations tracking it
        for (const auto& [sta, trk] : sta_tracks) {
            (void)sta;
            std::set<std::string> sys_here;
            for (const auto& sc : trk) { ++code_n[sc]; sys_here.insert(sc.first); }
            for (const auto& sys : sys_here) ++sys_n[sys];
        }
        for (const auto& [key, n] : code_n) {
            if (sys_n[key.first] <= 0) continue;
            const double frac = static_cast<double>(n) / sys_n[key.first];
            if (sig_cover_min > 0.0 && frac >= sig_cover_min)
                signal_allowlist[key.first].insert(key.second);
            // Float gate applies to PHASE codes only (it withholds phase-bias states).
            if (pb_cover_min > 0.0 && key.second[0] == 'L' && frac >= pb_cover_min)
                phase_bias_allowlist[key.first].insert(key.second);
        }
        auto reportGate = [&](const char* label, double thr,
                              const std::map<std::string, std::set<std::string>>& allow,
                              bool phase_only) {
            if (thr <= 0.0) return;
            for (const auto& [sys, ns] : sys_n) {
                std::set<std::string> codes;
                for (const auto& [key, cnt] : code_n) {
                    (void)cnt;
                    if (key.first != sys) continue;
                    if (phase_only && key.second[0] != 'L') continue;
                    codes.insert(key.second);
                }
                std::string keep, gate;
                auto ait = allow.find(sys);
                for (const auto& c : codes) {
                    const std::string tag = fmt::format("{}({:.0f}%)", c, 100.0 * code_n[{sys, c}] / ns);
                    const bool admitted = ait != allow.end() && ait->second.count(c);
                    std::string& tgt = admitted ? keep : gate;
                    tgt += (tgt.empty() ? "" : " ") + tag;
                }
                log->info("Network {} gate [{}≥{:.0f}%]: {} stations observe {} — "
                          "KEEP [{}] | GATE [{}]", label, sys, 100.0 * thr, ns, sys, keep, gate);
            }
        };
        reportGate("signal-coverage (drop)", sig_cover_min, signal_allowlist, false);
        reportGate("phase-bias (float)",     pb_cover_min,  phase_bias_allowlist, true);
    }

    // --- Phase 4: per-station Kalman resources (pivot at index 0) -----------
    // Each station: a single-receiver StateManager (coords fixed at the
    // configured ECEF), PppModel bound to its RinexParser, and a MeasurementModel.
    // Pivot first so PASS A can seed its phase-signal set before the others.
    std::vector<NetStation> stations;
    // Stations whose RINEX receiver antenna matched no loaded ATX entry (zero receiver
    // PCO/PCV applied).  Collected during buildStation for a single startup summary WARN
    // (the per-observation site logs only at trace, invisible at 305-station scale).
    std::vector<std::string> unmatched_antenna_stations;
    // Tier 1 (always-on radome→NONE fallback): stations recovered by the
    // type-preserving radome-agnostic NONE calibration (mm-level) — matched, but
    // via the fallback rather than an exact (type, radome) hit.  Reported once.
    std::vector<std::string> radome_fallback_stations;
    // Tier 2 (network.exclude_unmodeled_antenna_stations): stations dropped from
    // the network because their receiver antenna TYPE is unknown to the loaded
    // ANTEX even after the radome→NONE fallback (unmodelled receiver PCO/PCV).
    std::vector<std::string> excluded_unmodeled_antenna_stations;
    // buildStation outcome: Built (pushed to `stations`), ExcludedUnmodeledAntenna
    // (Tier 2 policy drop — counted, not built), or Failed (parser/load error).
    enum class StationBuild { Built, ExcludedUnmodeledAntenna, Failed };
    auto buildStation = [&](const StationConfig& cfg, bool is_pivot) -> StationBuild {
        const RinexParser* parser = sync.parserOf(cfg.name);
        if (!parser) {
            log->error("Network: station '{}' parser unavailable", cfg.name);
            return StationBuild::Failed;
        }
        NetStation st;
        st.name     = cfg.name;
        st.ecef      = Eigen::Vector3d(cfg.position_ecef.data());
        st.is_pivot  = is_pivot;
        st.sm        = std::make_unique<StateManager>(5, log_manager_, config_);  // pos(3)+clk(1)+tropo(1)
        Eigen::VectorXd x0 = st.sm->getStateVector();
        x0.head<3>() = st.ecef;
        st.sm->setStateVector(x0);
        st.sm->setStateEpochTime(synced.front().time);
        st.model = std::make_unique<PppModel>(
            config_, *product_parser_, log_manager_, *parser, *st.sm);
        // Network coverage gate: restrict this station's model to the codes that
        // enough of the whole network tracks (empty allowlist ⇒ no-op, admit all).
        if (!signal_allowlist.empty()) st.model->setSignalAllowlist(signal_allowlist);
        // Card 5 (RTF-1 / plan D1): in brdc_rts the a-priori satellite clock is
        // broadcast-grade, but the network ESTIMATES the clock (Δδtˢ vs that
        // a-priori), so this PRODUCER station's carrier phase is informative and
        // must be kept.  Suppress the code-only broadcast-phase gate on producer
        // models only — buildStation never builds the held-out closed-loop consumer
        // (the roster loop below skips it), so the consumer keeps the default gate.
        // sp3 mode passes false ⇒ byte-identical.
        st.model->setBroadcastPhaseGateSuppressed(brdc_rts);
        st.mm = std::make_unique<MeasurementModel>(
            *st.sm, *st.model, config_, log_manager_);
        // Phase-bias FLOAT gate: withhold satellite phase-bias states for signals
        // below network coverage (empty allowlist ⇒ no-op, every phase signal gets one).
        if (!phase_bias_allowlist.empty()) st.mm->setPhaseBiasSignalAllowlist(phase_bias_allowlist);
        // Seed this station's measurement-model frequency helper with its own
        // RINEX-header GLONASS FCN slots, so the GLONASS iono coefficient and
        // carrier wavelength in the Jacobian use the actual channel (not the
        // compiled-in default table).  Harmless for GPS/Galileo-only stations
        // (empty map).  Matters only under enableGlonassNetwork().
        st.mm->setGlonassSlots(parser->getGlonassFreqSlots());

        // Receiver-antenna ATX match check (startup diagnostic + optional phase-bias
        // exclusion).  A receiver antenna type that matches no loaded ANTEX entry gets
        // ZERO receiver PCO/PCV — getAntennaCorrection returns only the satellite-side
        // terms — a cm-level, elevation-dependent phase-centre error otherwise visible
        // ONLY at trace level (antenna.log is empty at default verbosity in a 305-station
        // run).  Checked once per station here; the per-observation trace still fires.
        // Only meaningful when a receiver correction would actually be applied.
        const auto& mc = config_.getModelCorrections();
        const bool rx_corr_on = (mc.enable_receiver_pco || mc.enable_receiver_pcv);
        if (rx_corr_on && !st.model->hasReceiverAntennaMatch()) {
            std::string rx_ant = parser->getReceiverAntennaType();
            const auto last = rx_ant.find_last_not_of(" \t\r\n");
            rx_ant = (last == std::string::npos) ? std::string{} : rx_ant.substr(0, last + 1);
            log->warn("Network: station '{}' receiver antenna '{}' has NO ATX model match "
                      "(even after the radome→NONE fallback) — receiver PCO/PCV NOT applied "
                      "(zero antenna correction on the receiver side).",
                      cfg.name, rx_ant.empty() ? "<none in RINEX header>" : rx_ant);

            // Tier 2 (network.exclude_unmodeled_antenna_stations): the always-on
            // radome→NONE fallback has already run, so a still-unmatched antenna is a
            // genuinely unknown TYPE with entirely unmodelled receiver PCO/PCV — a
            // systematic in every observation this station feeds into the shared
            // clocks/biases.  DROP it from the network (policy backstop, default OFF).
            if (config_.networkExcludeUnmodeledAntennaStations()) {
                // The clock-datum pivot realizes the datum and cannot be silently
                // dropped — surface it to the caller as a hard build error.
                if (is_pivot) return StationBuild::ExcludedUnmodeledAntenna;
                excluded_unmodeled_antenna_stations.push_back(cfg.name);
                log->warn("Network: station '{}' EXCLUDED from the network — unknown "
                          "receiver antenna TYPE (no ATX model; unmodelled receiver "
                          "PCO/PCV would be a systematic in the shared clocks/biases) "
                          "(network.exclude_unmodeled_antenna_stations).", cfg.name);
                return StationBuild::ExcludedUnmodeledAntenna;
            }

            // Kept (not hard-excluded) but unmatched → record for the loaded-station
            // summary WARN below.
            unmatched_antenna_stations.push_back(cfg.name);
            // Optional (network.phase_bias_exclude_unmatched_antenna): keep this station's
            // uncalibrated carrier phase out of the emitted satellite phase biases.  The
            // clock-datum pivot is NEVER excluded — it realizes the phase-bias datum, and
            // in the legacy single-pivot gauge its states ARE the shared set, so excluding
            // it would zero all emission; fix its antenna model instead.
            if (config_.networkPhaseBiasExcludeUnmatchedAntenna()) {
                if (is_pivot) {
                    log->warn("Network: clock-datum station '{}' has the unmatched antenna "
                              "but realizes the phase-bias datum — NOT excluded "
                              "(network.phase_bias_exclude_unmatched_antenna); supply its "
                              "ATX model to remove the receiver phase-centre bias.",
                              cfg.name);
                } else {
                    st.mm->setPhaseBiasEmissionExcluded(true);
                    log->info("Network: station '{}' excluded from phase-bias emission "
                              "contributions (unmatched receiver antenna).", cfg.name);
                }
            }
        } else if (rx_corr_on) {
            // Matched — but note when the match came only through the always-on
            // type-preserving radome→NONE fallback (mm-level radome-agnostic
            // calibration), so the substitution is visible once per station rather
            // than only at per-observation trace level.
            std::string detail;
            if (st.model->receiverAntennaViaRadomeFallback(&detail)) {
                log->warn("Network: station '{}' receiver antenna {}", cfg.name, detail);
                radome_fallback_stations.push_back(cfg.name);
            }
        }

        // Phase-leveling T0 (Increment 2): a persistent per-station MW + GF
        // TurboEdit detector pair for cycle-slip REPAIR of the carried ambiguities.
        // NON-pivot only on the legacy path — the single pivot's ambiguities are
        // ALL gauge-pinned and a repair there needs Increment 4 (design §6).  Under
        // the prioritized datum EVERY station (incl. the former pivot) carries the
        // arcs it does not anchor, so it MUST slip-detect them — build for all; the
        // PASS-A refloat still no-ops on a station's anchored (uncarried) arcs.
        // Constructed only when the carry AND a detector are on, so OFF is
        // byte-identical.  setCorrections(&this station's GnssCorrections) engages
        // the BIA-corrected wide-lane path (integer dN) where biases are available;
        // arc state lives on the detector and is station-local (parallel-safe).
        // M5 card WL-1: the MW-seeded wide-lane layer also needs the per-station MW
        // detector running (its per-arc Welford means feed the WL fold), and it must
        // work in the 3card lineage where phase_leveling is OFF.  Relax the gate to
        // build the detector on EVERY station when network.mw_wl.enable, independent of
        // phase_leveling / turbo_edit / mw_detect.  OFF (mw_wl disabled) is unchanged.
        const bool build_slip_for_pl =
            (config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkPhaseLeveling()
            && (config_.networkCycleSlipTurboEdit() || config_.networkCycleSlipMwDetect());
        // M5 card WL-2 (SPEC_ewl_wl_cascade_card.md §4): the EWL/WL lanes also need the
        // per-station MW detector running (its per-arc Welford means feed the EWL/MW(1,2)
        // statistics).  OFF (all lanes disabled) ⇒ unchanged.
        const auto& mwc = config_.getNetworkConfig().mw_wl;
        const auto& lcc = config_.getNetworkConfig().lane_carry;   // M5 WL-3
        if (build_slip_for_pl || config_.networkMwWlSeed()
            || mwc.ewl_enable || mwc.wl_inject_enable || lcc.enable) {
            st.mw_slip = std::make_unique<MwSlipDetector>(config_, log_manager_);
            st.mw_slip->setCorrections(&st.model->getCorrections());
            // M5 card WL-2 (SPEC §3.4): arm the tri-frequency GF_b phase bridge accumulator
            // only when a consumer of the bridge exists — the WL-2 inject path or the WL-3
            // lane bridge increments — together with the EWL integers it bridges from.
            // OFF ⇒ the detector never touches a bridge arc (byte-identical).
            st.mw_slip->setBridgeEnabled(
                (mwc.wl_inject_enable || (lcc.enable && lcc.bridge_meas)) && mwc.ewl_enable);
            // M5 card UWL-1 (SPEC_uwl_card.md §3.1): push the per-constellation lane-tier
            // band digits for the bridge band pick.  Not pushed (empty) unless the card is
            // armed ⇒ the legacy {1,2,5} scan for every constellation, byte-identical —
            // including under multi-GNSS active_systems (§6.1(b)).  With the table pushed,
            // a constellation ABSENT from it (BDS-3, deferred on its clock divergence)
            // forms NO bridge arc; the wiring warns loudly instead of skipping silently.
            if (mwc.uwl_enable) {
                std::map<char, MwLaneTier> tiers;
                for (const char cons : { 'G', 'E' })
                    if (const auto t = mwLaneTierOf(cons, mwc.uwl_gal_top_band,
                                                    mwc.uwl_mid_enable, mwc.uwl_dual_top))
                        tiers[cons] = *t;
                st.mw_slip->setLaneTiers(std::move(tiers));
            }
            // The TurboEdit MW+GF integer REPAIR resolver is built ONLY for turbo_edit
            // (higher-rate 1 s data, where the geometry-free ionospheric drift over the
            // epoch gap is small enough to trust an integer).  mw_detect uses the SAME MW
            // detector as a pure DETECTOR → refloat, so it needs no resolver — and the null
            // st.turbo_edit is exactly what steers the PASS-A branch to the refloat path.
            if (config_.networkCycleSlipTurboEdit()) {
                st.turbo_edit = std::make_unique<TurboEditResolver>(config_, log_manager_);
                st.turbo_edit->setCorrections(&st.model->getCorrections());
            }
        }

        // IF-phase triple-difference slip DETECTION (this increment): a persistent
        // per-station between-satellite/between-epoch ionosphere-free phase OMC
        // detector.  NON-pivot only on the legacy path; under the prioritized datum
        // build for EVERY station (the former pivot carries non-anchored arcs that
        // need detection).  Constructed whenever network.cycle_slip.if_phase_td is
        // on — independent of phase-leveling, so it can run as a pure diagnostic in
        // the absolute solve; the refloat ACTION it drives is gated on the carry
        // being active at the call site.  Station-local detector state (parallel-safe).
        if ((config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkCycleSlipIfPhaseTd()) {
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);
            st.if_slip->setDeferFlagging(true);  // network pools per-pair MAD; the driver flags
        }

        st.pstate.position        = st.ecef;
        st.pstate.clock_bias      = 0.0;
        st.pstate.troposphere_zwd = 0.0;
        stations.push_back(std::move(st));
        return StationBuild::Built;
    };
    switch (buildStation(roster.reference, /*is_pivot=*/true)) {
        case StationBuild::Built:
            break;
        case StationBuild::ExcludedUnmodeledAntenna:
            log->error("Network: clock-datum station '{}' has an unknown receiver antenna "
                       "TYPE with no ATX model (even after the radome→NONE fallback) and "
                       "network.exclude_unmodeled_antenna_stations is set — the datum "
                       "station cannot be dropped. Supply its ATX model or choose a "
                       "different network.clock_datum_station. Aborting.",
                       roster.reference.name);
            return 1;
        case StationBuild::Failed:
            log->error("Network: clock datum station '{}' failed to load; the network "
                       "cannot be built without its datum. Aborting.",
                       roster.reference.name);
            return 1;
    }
    for (const auto& rv : roster.rovers) {
        if (closed_loop_on && rv.name == cl.station.name) continue;  // held-out consumer
        switch (buildStation(rv, /*is_pivot=*/false)) {
            case StationBuild::Built:
                break;
            case StationBuild::ExcludedUnmodeledAntenna:
                // Already warned + counted inside buildStation; the network proceeds
                // with the remaining stations.
                continue;
            case StationBuild::Failed:
                log->warn("Network: station '{}' failed to load; skipping it (the "
                          "network proceeds with the remaining stations).", rv.name);
                continue;
        }
    }
    log->info("Network: {} station estimators constructed (pivot '{}')",
              stations.size(), stations.front().name);
    if (!unmatched_antenna_stations.empty()) {
        std::string names;
        for (size_t i = 0; i < unmatched_antenna_stations.size(); ++i) {
            if (i) names += ", ";
            names += unmatched_antenna_stations[i];
        }
        log->warn("Network: {} of {} loaded station(s) have NO receiver antenna ATX match "
                  "(receiver PCO/PCV zeroed there): {}{}",
                  unmatched_antenna_stations.size(), stations.size(), names,
                  config_.networkPhaseBiasExcludeUnmatchedAntenna()
                      ? " — non-pivot ones excluded from phase-bias emission"
                      : " — supply their ATX models, or set "
                        "network.phase_bias_exclude_unmatched_antenna to withhold them");
    }
    // Join a name vector into a comma-separated list for the summaries below.
    auto joinNames = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s;
    };
    // Tier 1 (always-on radome→NONE): report the stations whose receiver PCO/PCV
    // came from the radome-agnostic NONE calibration (mm-level) — matched, not
    // dropped, but worth a single summary so the substitution is visible.
    if (!radome_fallback_stations.empty()) {
        log->info("Network: {} of {} loaded station(s) matched their receiver antenna via "
                  "the type-preserving radome→NONE fallback (radome uncalibrated; NONE "
                  "PCO/PCV applied, mm-level): {}",
                  radome_fallback_stations.size(), stations.size(),
                  joinNames(radome_fallback_stations));
    }
    // Tier 2 (network.exclude_unmodeled_antenna_stations): run-end counter for the
    // stations dropped because their receiver antenna TYPE is unknown to the ANTEX
    // even after the radome→NONE fallback (invariant #4 — ON-path telemetry).
    if (!excluded_unmodeled_antenna_stations.empty()) {
        log->warn("Network: {} station(s) excluded (unmodeled receiver antenna): {}",
                  excluded_unmodeled_antenna_stations.size(),
                  joinNames(excluded_unmodeled_antenna_stations));
    } else if (config_.networkExcludeUnmodeledAntennaStations()) {
        log->info("Network: 0 stations excluded (unmodeled receiver antenna) — every "
                  "loaded station's receiver antenna resolved (radome→NONE fallback "
                  "covered any uncalibrated radomes).");
    }

    // --- Station roster (viz) -----------------------------------------------
    // A tiny static CSV of the stations that actually loaded (id, role, ECEF) so
    // the pyviz worldmap reflects the real run, not just the config.  The
    // held-out closed-loop consumer (if any) is listed with role "held_out".
    if (nc.trace_stations) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        const std::filesystem::path roster_path = log_dir_ / "network_stations.csv";
        std::ofstream roster(roster_path);
        if (roster) {
            roster << "station,role,ecef_x,ecef_y,ecef_z\n";
            for (const auto& st : stations)
                roster << st.name << ',' << (st.is_pivot ? "datum" : "network") << ','
                       << fmt::format("{:.4f}", st.ecef.x()) << ','
                       << fmt::format("{:.4f}", st.ecef.y()) << ','
                       << fmt::format("{:.4f}", st.ecef.z()) << '\n';
            if (closed_loop_on) {
                const auto& p = cl.station.position_ecef;
                roster << cl.station.name << ",held_out,"
                       << fmt::format("{:.4f}", p[0]) << ','
                       << fmt::format("{:.4f}", p[1]) << ','
                       << fmt::format("{:.4f}", p[2]) << '\n';
            }
            log->info("Network: wrote station roster {}", roster_path.string());
        } else {
            log->warn("Network: failed to open station roster {}", roster_path.string());
        }
    }

    // === Per-station compute parallelism ====================================
    // PASS A (geometry) and the produce half of PASS B (measurement assembly) are
    // embarrassingly parallel across stations — each owns an independent
    // StateManager / PppModel / MeasurementModel, and the audited call trees
    // touch no shared mutable state (product stores are const post-load; the
    // per-instance model caches are thread-private; the live SSR/ephemeris
    // singletons are mutex-guarded and empty in post-processing).  Only the
    // shared-estimator scatter (est.addStation) and everything downstream of it
    // stays serial, in deterministic station order, so the float solution is
    // BIT-IDENTICAL to a serial run regardless of thread count.  Activated only
    // above the configured station-count threshold (small networks run serially —
    // no fork-join cost, and the internal 3-station fixture is unchanged).
    int hw_max_threads = 1;
#ifdef _OPENMP
    hw_max_threads = omp_get_max_threads();
#endif
    const int n_sta_total = static_cast<int>(stations.size());
    const bool parallel_on =
        nc.parallel_stations
        && n_sta_total > nc.parallel_min_stations
        && hw_max_threads > 1;
    const int n_threads = parallel_on
        ? (nc.parallel_max_threads > 0
               ? std::min(nc.parallel_max_threads, hw_max_threads)
               : hw_max_threads)
        : 1;
    // Deterministic-parallel post-solve seam (network.parallel_seam): the per-block
    // DD carry-fold + D5 innovation-gate readouts.  Rides on the per-station parallel
    // path (same thread budget, Eigen already pinned to one thread for the epoch) and,
    // unlike parallel_reduce, is BYTE-IDENTICAL at any thread count (compute per block
    // in parallel, accumulate/install serially in fixed block order).  seam_threads == 1
    // ⇒ the seam runs its original serial path (OFF is byte-for-byte the pre-seam run).
    const bool seam_on      = parallel_on && nc.parallel_seam;
    const int  seam_threads = seam_on ? n_threads : 1;
    if (seam_on)
        log->info("Network: post-solve carry-fold/D5 seam PARALLEL across {} threads "
                  "(deterministic — byte-identical to the serial fold)", n_threads);
    // network.parallel_carry: thread the Card-4 carry's dense per-block algebra
    // (buildDdPriors W·Pᶻ·Wᵀ, foldReducedDdPriors, installPosterior) via Eigen's own
    // multi-threaded GEMM.  The seam only parallelizes the carry over the ~few blocks;
    // this un-pins Eigen so each block's O(K²–K³) matmul fans across cores.  Runs those
    // stages SERIAL-over-blocks (n_threads=1) so Eigen threading does not nest with the
    // seam's OpenMP.  Reassociates the dense products ⇒ ~1e-X ULP drift vs serial (like
    // parallel_reduce); default OFF keeps Eigen pinned and the seam/serial path (byte-id).
    const bool carry_par = parallel_on && nc.parallel_carry;
    if (carry_par)
        log->info("Network: Card-4 carry dense algebra Eigen-THREADED across {} threads "
                  "(buildDdPriors / foldReducedDdPriors / installPosterior; serial≡parallel "
                  "byte-identity intentionally relaxed to ~1e-X — physically null)", n_threads);
#ifndef _OPENMP
    if (nc.parallel_stations && n_sta_total > nc.parallel_min_stations)
        log->warn("Network: parallel_stations requested but this build has no OpenMP "
                  "support; per-station compute will run serially.");
#endif
    if (parallel_on)
        log->info("Network: per-station compute PARALLEL — {} stations across {} threads "
                  "(shared-estimator scatter stays serial; solution is thread-count-invariant)",
                  n_sta_total, n_threads);
    else
        log->info("Network: per-station compute SERIAL ({} stations)", n_sta_total);

    // Prewarm every logger the per-station model path can touch.  getLogger()'s
    // first-touch CREATE path mutates a shared std::map; fetching an existing
    // logger is lock-free and data-race-safe.  Creating each one now (single-
    // threaded) guarantees no insert ever races a concurrent fetch inside the
    // parallel region — the only shared-mutable hazard the thread-safety audit
    // found in the per-station call tree.
    if (parallel_on) {
        for (const char* lname : {
                "ppp_model", "geometry", "antenna", "antenna_model", "corrections",
                "planetary_model", "ocean_tide", "ppp_utils", "ionosphere",
                "troposphere", "stochastic_model", "measurement_model",
                "state_manager", "state_initializer", "bssd_transform",
                "ambiguity_resolver", "main_log" })
            (void)log_manager_.getLogger(lname);
    }

    // --- Owned product sink (closed-loop isolation) -------------------------
    SsrCorrectionStore network_store;
    // A2: pass the clock-disc toggle so the emitter maintains + stamps the per-sat clock
    // discontinuity counter.  R9: the phase-bias disc threshold is the emitter's own default
    // constant (single source of truth shared with the serialized contract's disc_threshold_m).
    // RTA-11: the staleness horizon (product-time seconds; 0 = off) makes a key returning
    // after a longer gap a first sighting (no spurious disc bump against an hours-stale
    // baseline); like the other two parameters it travels in the serialized contract.
    NetworkProductEmitter emitter(NetworkProductEmitter::kDefaultDiscThresholdM,
                                  config_.networkEmitClockDisc(),
                                  config_.networkEmitterStalenessHorizonS(),
                                  config_.networkCodeBiasDiscJumpTest(),
                                  config_.networkClockDiscOnSetChange(),
                                  config_.networkDiscThresholdFracCycles());

    // --- Optional on-disk product serialization -----------------------------
    // A reusable per-epoch dump of the emitted solution (the same pre-emit
    // NetworkSolutionEpoch the in-memory closed loop consumes, plus this epoch's
    // datum-change flag).  Lets consumer-side experiments — notably the consumer
    // AR-frame work — replay the products WITHOUT re-running the multi-station
    // network: NetworkProductReader::replay reproduces this store byte-for-byte
    // (discontinuity counters included).
    std::unique_ptr<NetworkProductWriter> product_writer;
    std::filesystem::path prod_path;
    // B6 (P1-1): when the S-basis contract is serialized, DEFER opening the writer until
    // the first emit so the realized per-constellation clock pivots (auto-picked, unknown
    // until the first solve) can be stamped into the v2 header — not just the config
    // intent.  pending_contract holds everything known up front; realized pivots merge in
    // at the first emit.  contract_pending gates that one-shot construction.
    SbasisContract pending_contract;
    bool           contract_pending = false;
    if (!nc.product_output_path.empty()) {
        // A relative path lands under the network trace dir, alongside
        // network_solution.csv etc.; an absolute path is used verbatim.
        prod_path = nc.product_output_path;
        if (prod_path.is_relative()) prod_path = log_dir_ / prod_path;
        if (nc.serialize_sbasis_contract) {
            // B6: stamp the S-basis contract into a v2 header.  Declaration semantics are
            // the A3-corrected per-constellation sat-clock-pin basis.
            pending_contract.decl.reference_receiver_id = nc.clock_datum_station;
            pending_contract.decl.emit_code_bias        = config_.estimateNetworkSatCodeBias();
            // T3b: self-describe the phase-bias gauge in the on-disk contract too.  Under the
            // anchor-free ensemble datum the emitted bias references the receiver ENSEMBLE, not
            // a pivot receiver; the runtime per-solve decl already flips (see ~AmbDatum below),
            // but the startup contract omitted it and mis-declared the default pivot frame.
            // Mirrors the isb_datum self-description that follows.  OFF ⇒ default ⇒ byte-identical.
            if (config_.networkPhaseDatumEnsemble())
                pending_contract.decl.amb_datum =
                    SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
            if (config_.networkClockDatumBroadcastAnchored()) {
                // Broadcast-anchored clock datum (brdc_rts): declare the per-satellite
                // broadcast-anchor basis and leave clock_pivot_sat empty (no pivot; a
                // config pin is ignored, warned at load).  The anchor sigma is part of
                // the realized gauge, so record it in the declaration.
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
                pending_contract.decl.broadcast_anchor_sigma_m =
                    config_.resolveNetworkAprioriClockAnchorSigmaM();
            } else if (config_.networkClockDatumSumToZero()) {
                // Anchor-free clock datum: declare the sum-to-zero basis and leave
                // clock_pivot_sat empty (there is no pivot; a config pin is ignored).
                // (Local clock_sum_to_zero is declared further down; the getter is
                // cheap and this one-shot startup block precedes it.)
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
            } else if (!nc.clock_datum_satellite.empty()) {
                pending_contract.decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                    = nc.clock_datum_satellite;
            }
            // P1-1: resolve the a-priori orbit/clock identity to the ACTUAL resolved
            // SP3 + CLK file basenames (the baseline the emitted clock C0 is a delta
            // against).  Config intent alone is "CODFIN00000000" under auto-detect.
            const auto& dl = config_.getDownloadConfig();
            std::string apriori_id =
                fmt::format("{}{}{:04d}{:03d}", dl.product_source, dl.product_type, dl.year, dl.doy);
            if (brdc_rts) {
                // Card 5 (plan D10): the a-priori is broadcast ephemeris, so the identity
                // is the nav-file basename ("brdc:<file>").  Once an RTS-orbit source is
                // attached (plan D9 seam / RTF-3), this becomes "brdc+rts:<mount>".
                const std::string nav = config_.getNavigationPath().filename().string();
                apriori_id = "brdc:" + (nav.empty() ? std::string("?") : nav);
            } else {
                const std::string sp3 = config_.getProductPath("SP3").filename().string();
                const std::string clk = config_.getProductPath("CLK").filename().string();
                if (!sp3.empty() || !clk.empty())
                    apriori_id = (sp3.empty() ? std::string("?") : sp3) + "+"
                               + (clk.empty() ? std::string("?") : clk);
            }
            pending_contract.apriori_product_id = apriori_id;
            // RTA-12: contract identity fields (each serialized as an optional
            // key=value S-line field ONLY when non-empty).
            //  - frame_id / coord_epoch are config DECLARATIONS (network.frame_id /
            //    network.coord_epoch; the SIF-coordinate path defaults coord_epoch
            //    to the propagation date) — no auto-detection.
            //  - atx_id is the RESOLVED antenna-calibration file this run actually
            //    loaded (basename of the merged product path).  Failure mode: when
            //    no .atx resolved, getProductPath returns an empty path and the
            //    atx= field is simply absent — a consumer that pins expect_atx_id
            //    then flags "file '' vs expected ...".
            pending_contract.frame_id    = nc.frame_id;
            pending_contract.coord_epoch = nc.coord_epoch;
            pending_contract.atx_id      = config_.getProductPath("ATX").filename().string();
            //  - orbit_ref: SP3 orbits + ATX satellite PCO/PCV mean the emitted
            //    clock C0 is referred to the satellite CENTER OF MASS.  Hardcoded
            //    until an SSR orbit source (APC-referred, per the RTCM SSR
            //    convention) lands, at which point this becomes a variable of the
            //    orbit path selection.
            pending_contract.orbit_ref   = "CoM";
            pending_contract.disc_threshold_m   = NetworkProductEmitter::kDefaultDiscThresholdM;  // R9: same constant as the emitter above
            // The replay-emitter construction parameters (disc threshold above + the
            // clock-disc channel flag + the RTA-11 staleness horizon) travel in the
            // contract so a consumer can replay the file with byte-identical
            // discontinuity counters and no out-of-band knowledge of the producer's
            // config.  The horizon serializes as an optional stale= field only when
            // non-zero, so an OFF run's v2 header is byte-identical.
            pending_contract.emit_clock_disc    = config_.networkEmitClockDisc();
            pending_contract.staleness_horizon_s = config_.networkEmitterStalenessHorizonS();
            pending_contract.code_bias_jump_test = config_.networkCodeBiasDiscJumpTest();
            pending_contract.clock_disc_on_set_change = config_.networkClockDiscOnSetChange();
            pending_contract.disc_threshold_frac_cycles = config_.networkDiscThresholdFracCycles();  // H3: per-λ jump gate (discfrac=)
            // M4 (contract v2): advertise the combined-σ age-scaling coefficient on the
            // S line ONLY when the combined-σ channel is on and a coefficient is set, so
            // an OFF (or agek=0) run's v2 header is byte-identical (agek= token absent).
            if (config_.networkEmitCombinedSigma())
                pending_contract.age_scale_k = config_.networkCombinedSigmaAgek();
            pending_contract.present            = true;
            contract_pending = true;
            // The deferred writer only opens (and truncates) at the FIRST emit; a run
            // that emits zero epochs would otherwise leave a PREVIOUS run's complete
            // product file at this path for a downstream consumer to silently replay.
            // Match the v1 branch's truncate-at-startup by removing any existing file.
            std::error_code stale_ec;
            std::filesystem::remove(prod_path, stale_ec);
            if (stale_ec && std::filesystem::exists(prod_path))
                log->warn("Network: could not remove a stale product file at '{}' ({}) — "
                          "if this run emits no epochs, a downstream consumer would replay "
                          "the PREVIOUS run's products from that path.",
                          prod_path.string(), stale_ec.message());
            log->info("Network: will serialize per-epoch products with a v2 S-basis contract "
                      "(realized pivots stamped at first emit) to '{}'", prod_path.string());
        } else {
            product_writer = std::make_unique<NetworkProductWriter>(prod_path.string());
            if (product_writer->good()) {
                log->info("Network: serializing per-epoch products to '{}'", prod_path.string());
            } else {
                log->error("Network: failed to open product_output_path '{}' for writing; "
                           "disabling on-disk serialization for this run.", prod_path.string());
                product_writer.reset();
            }
        }
    }

    // --- Closed-loop consumer (held-out station) ----------------------------
    // The held-out station runs the user-side PPP-RTK + AR pipeline through a
    // RealTimeEngine fed epoch-by-epoch from RINEX replay, with its SSR
    // consumption repointed at the OWNED network_store (never the singleton).
    // Each epoch: the producer emits into network_store, then the consumer reads
    // that fresh epoch — closing the loop "does our own user engine recover
    // integers from our own products on an independent station?".
    GnssConfig consumer_config = config_.deriveClosedLoopConsumerConfig();
    std::unique_ptr<RealTimeEngine> consumer;
    if (closed_loop_on) {
        // Point the consumer's RinexParser at the held-out station so its header
        // (antenna type, approximate position) seeds the consumer correctly; the
        // per-epoch observations themselves come from the synced roster below.
        consumer_config.setObservationPath(cl.station.rinex_path);
        consumer = std::make_unique<RealTimeEngine>(consumer_config, log_manager_);
        if (!consumer->initialize()) {
            log->error("Network closed-loop: consumer engine initialize() failed; "
                       "disabling the harness for this run.");
            consumer.reset();
        } else {
            // Direct LAMBDA on the network-bias-corrected float must NOT also take
            // the MW WL (it is in the .BIA frame; see setArIgnoreMwWl).  wl_only AR
            // keeps the MW WL by definition.
            consumer->setArIgnoreMwWl(!consumer_config.getArWlOnly());
            const bool undiff = cl.consumer_undifferenced;
            if (undiff)
                log->info("Network closed-loop: UNDIFFERENCED user model — direct per-band "
                          "LAMBDA on the network-bias float (no BSSD, no WL/NL cascade); the "
                          "resolver forms the between-satellite difference internally. AR fires "
                          "once the 3D position σ converges below the FORCE-CAPPED gate "
                          "({:.2f} m; undiff NL LAMBDA is clamped to ≤0.20 m at load — "
                          "GnssConfigLoad applyNetworkConsumerCouplings). T2c: the per-epoch "
                          "'ar_gate' column of closed_loop_trace.csv attributes which gate "
                          "(sigma_gate/min_epochs/no_candidates/ratio_battery) zeroed AR.",
                          consumer_config.getArMaxPositionSigma());
            else if (!consumer_config.getArWlOnly())
                log->warn("Network closed-loop: BSSD consumer with ar_wl_only=false runs PURE "
                          "full NL LAMBDA on the float — EXPERIMENTAL and slow on the v1 product "
                          "(the WL-float-level position makes the NL search ill-conditioned).  "
                          "Prefer ar_wl_only=true, or consumer_undifferenced=true.");
            if (cl.consume_network_products) {
                // Consume OUR products (never the singleton).  consume_network_clock
                // gates the clock overlay: false keeps precise SP3/CLK and consumes
                // only the network phase bias (the clean integer-recovery test).
                consumer->setSsrStore(&network_store, cl.consume_network_clock, /*bias=*/true);
                // Strong (ionosphere-corrected) user model: consume the network's
                // SH-VTEC slant iono as a tight per-sat BLUP prior (the producer
                // fills the store's slant-iono channel each epoch below).  Default
                // OFF ⇒ the consumer floats its own iono (byte-identical).
                consumer->setConsumeNetworkIono(cl.consume_network_iono);
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — reading the OWNED network store ({}{}).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off",
                          cl.consume_network_clock
                              ? "full: network clock + phase bias"
                              : "bias-only: precise clock + network phase bias",
                          cl.consume_network_iono ? " + SH-VTEC slant iono" : "");
            } else {
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — BASELINE mode (precise products only, network "
                          "store NOT injected).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off");
            }
        }
    }

    // Closed-loop metrics + trace.
    const Eigen::Vector3d cl_truth(cl.station.position_ecef.data());
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    std::size_t cl_epochs = 0, cl_ar_ran = 0, cl_ar_fixed = 0;
    long long   cl_sum_fixed = 0;
    double cl_first_fix_tow = -1.0; int cl_first_fix_week = -1;
    std::deque<double> cl_recent_err3d;   // last-N 3D errors → converged-segment RMS
    constexpr std::size_t kClRecentWindow = 30;
    std::ofstream cl_trace;

    // D2 process noise (variance growth per second) for the carry-forward prior.
    const double q_clock = config_.getProcessNoise().sat_clock;        // m²/s
    const double q_pb    = config_.getProcessNoise().sat_phase_bias;   // m²/s
    const double q_cb    = config_.getProcessNoise().sat_code_bias;    // m²/s (W6 OSB carry-forward)

    // --- Trace --------------------------------------------------------------
    // P1: the three UNBOUNDED service CSVs (always-on network_trace.csv +
    // network_solution.csv, and the RTB-6 health heartbeat) rotate size-based
    // behind network.trace_rotate_mb.  Their header lines are hoisted into named
    // constants so the rotation reopen re-writes the IDENTICAL header the
    // startup open wrote — every rolled generation parses standalone.  Rotated
    // .1….keep suffixes are NOT auto-discovered by the pyviz viewers (they read
    // only the live file name); the operator concatenates the chain or points a
    // viewer at one generation — see docs/rt_service_operations.md.
    std::ofstream trace;
    const std::filesystem::path trace_path = log_dir_ / "network_trace.csv";
    static constexpr const char* kTraceHeader =
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,"
        "gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins\n";
    // Per-satellite product trace: the actual emitted solution (long format, one
    // row per shared parameter per epoch) so the producer's output is inspectable
    // — per-sat clock-Δ continuity, phase-bias stability, and discontinuity bumps.
    std::ofstream soln;
    const std::filesystem::path soln_path = log_dir_ / "network_solution.csv";
    static constexpr const char* kSolnHeader = "week,tow,sat,kind,signal,value_m,disc_counter\n";
    // Per-station, per-satellite back-substituted slant ionosphere (viz): one row
    // per (station, sat) each emitted epoch, with the measured elevation/azimuth
    // so the pyviz viewer can map each estimate to its ionospheric pierce point.
    std::ofstream iono_trace;
    const std::filesystem::path iono_path = log_dir_ / "network_ionosphere.csv";
    // Per-epoch co-estimated SH-VTEC coefficient trace (estimate_iono_sh +
    // global_vtec_sh.trace): one row per (n,m,trig) coefficient with its value and
    // formal std (TECU), so the estimated field is inspectable / comparable to a GIM.
    std::ofstream vtec_sh_trace;
    const std::filesystem::path vtec_sh_path = log_dir_ / "network_vtec_sh.csv";
    // T1a: per-epoch SH-field HEALTH trace (same gate as the coefficient trace) —
    // one row per emitted epoch summarising the estimated mean VTEC (a₀₀ + formal σ),
    // the coefficient RMS by degree, the ratio of that RMS to the per-degree Kaula
    // prior σ, and the network-wide SH→slant misfit (mean/rms/max |residual| plus the
    // 3σ-exceedance counts).  Makes the a₀₀ divergence (+296 → −91 TECU) and the
    // per-(station,sat) misfit squeezed into the ambiguities visible + attributable.
    std::ofstream vtec_sh_health_trace;
    const std::filesystem::path vtec_sh_health_path = log_dir_ / "network_vtec_sh_health.csv";
    // Deep-investigation slip/RAIM diagnostics (network.trace_slip_events): the IF-phase
    // pre-detector's per-(station,sat,pair) ΔΔφ_IF and full threshold decision each epoch,
    // and RAIM's per-(sat,station) post-fit standardized residual + cross-station consensus.
    // The pair of files that make an IF-slip "need to reset" and a RAIM trigger inspectable.
    std::ofstream if_slip_trace;
    const std::filesystem::path if_slip_trace_path = log_dir_ / "network_if_slip.csv";
    std::ofstream raim_trace;
    const std::filesystem::path raim_trace_path = log_dir_ / "network_raim.csv";
    std::ofstream raim_attrib_trace;
    const std::filesystem::path raim_attrib_trace_path = log_dir_ / "network_raim_attrib.csv";
    // Per-satellite clock-monitor diagnostics (network.clock_monitor + trace_slip_events):
    // one row per satellite near or over the refloat bound each epoch, so the abs/jump
    // decision and the resulting reinit are inspectable against the emitted clock series.
    std::ofstream clk_mon_trace;
    const std::filesystem::path clk_mon_trace_path = log_dir_ / "network_clock_monitor.csv";
    // B7: per-satellite a-priori-clock-anchor information fraction (anchor_info /
    // reduced-normals total_info) each emitted epoch, so validation can certify the
    // estimable clock is DATA-dominated, not defined by the a-priori CODE product it
    // is softly anchored to.  Pure diagnostic (network.trace_anchor_info).
    std::ofstream anchor_info_trace;
    const std::filesystem::path anchor_info_trace_path = log_dir_ / "network_anchor_info.csv";
    // B5: cross-station between-satellite DD float ambiguities (from reconstructStation),
    // per (const,signal), with their fractional-cycle residual — a sign-convention
    // self-check + empirical <0.1-cycle-support evidence for user AR (gate for producer
    // AR).  Pure diagnostic (network.trace_dd_fractional).
    std::ofstream dd_frac_trace;
    const std::filesystem::path dd_frac_trace_path = log_dir_ / "network_dd_fractional.csv";
    std::unique_ptr<PppUtils> dd_ppp_utils;   // B5: wavelength provider (built only when the DD trace is on)
    // Producer-side network AR (network.ambiguity_resolution, Phase 2+): a
    // wavelength provider for the between-station DD meters→cycles conversion,
    // built only when armed (default OFF => the AR block is skipped entirely =>
    // byte-identical float product).
    const auto& netar = nc.ambiguity_resolution;
    std::unique_ptr<PppUtils> ar_ppp_utils;
    if (netar.enable) ar_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    // RAW cycle-slip observables per (station,sat) every epoch (network.trace_slip_raw):
    // Melbourne-Wübbena, geometry-free L1−L2, first-signal phase-minus-code (all m) and the
    // per-band RINEX LLI — computed from raw_observed, INDEPENDENT of any detector being on.
    // The always-available "was there a slip on ANY sat here?" table; in particular the only
    // network CSV that surfaces the receiver LLI ground truth.  Epoch diffs are formed offline.
    std::ofstream slip_raw_trace;
    const std::filesystem::path slip_raw_trace_path = log_dir_ / "network_slip_raw.csv";
    std::unique_ptr<PppUtils> slip_raw_ppp_utils;  // wavelength provider (built only when the raw-slip trace is on)
    // L1: per-epoch disposition + σ̂0 + clock-stats trace (network.trace_epoch_status).
    // ONE row per processed epoch (emitted OR skipped) — the disposition timeline the
    // V1/V3/V6 validation cards are judged on.  Off by default (byte-identical when OFF).
    std::ofstream epoch_status_trace;
    const std::filesystem::path epoch_status_trace_path = log_dir_ / "network_epoch_status.csv";
    // RTB-6: operator health heartbeat (network.health_interval_s > 0) — a periodic
    // machine-readable @c network_service_health.csv appended every N s of CONTENT
    // time so a long-running service is observable without waiting for the exit
    // summary.  Named "service_health" because the pyviz viewer called
    // network_health reads network_trace.csv — a DIFFERENT surface (per-epoch
    // estimability, not service liveness).  sigma0_ewma reports −1 ("unavailable")
    // unless its feeding tiers (emit_ura + raim + phase_leveling) are all armed —
    // the EWMA only updates inside the warm post-fit RAIM block, so without them it
    // would silently freeze at its 1.0 init.  Off by default (byte-identical when
    // OFF: no file opened, no rows written).  Column semantics:
    // docs/rt_service_operations.md.
    std::ofstream health_trace;
    const std::filesystem::path health_trace_path = log_dir_ / "network_service_health.csv";
    static constexpr const char* kHealthHeader =
        "week,tow,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,"
        "n_runaway_epochs,sigma0_ewma,n_present,n_clock_sats,"
        "ms_per_epoch_window,last_emit_age_s,pivot,cold_start_grade_s\n";
    // L2: structured per-event QC log (network.trace_events).  One LONG-format row per
    // discrete QC action across every tier — the machine-readable replacement for
    // spdlog regex-scraping.  Off by default (byte-identical when OFF).
    std::ofstream events_trace;
    const std::filesystem::path events_trace_path = log_dir_ / "network_events.csv";
    // L7: per-station ZWD trace (network.trace_zwd) — estimated ZWD + datum-conditioned
    // formal σ + carried flag, per present station each emitted epoch (V7 instrument).
    std::ofstream zwd_trace;
    const std::filesystem::path zwd_trace_path = log_dir_ / "network_zwd.csv";
    // L13: per-station residual field (network.trace_station_residuals) — EVERY post-fit RAIM
    // cell (non-pivot station x sat) with s_raw / s_norm / σ̂0.  Heavy opt-in deep trace.
    std::ofstream station_res_trace;
    const std::filesystem::path station_res_trace_path = log_dir_ / "network_station_residuals.csv";
    // M2 (LS-VCE): per-hour measured carry-variance components — one row per
    // (family, constellation) whenever the content hour rolls over.  floor_m2 is
    // the honest constant-cofactor σ̂ to compare against the hand var_floor;
    // rate_m2ps the honest Δt-cofactor σ̂ to compare against q_amb; ratio =
    // floor_m2 / config floor (>1 ⇒ the hand floor is optimistic).  Off by
    // default (byte-identical: no file, no rows).
    std::ofstream carry_vce_trace;
    const std::filesystem::path carry_vce_trace_path = log_dir_ / "network_carry_vce.csv";
    static constexpr const char* kCarryVceHeader =
        "week,tow,hour,family,const,n_rows,n_eff,robust_floor_m2,mean_floor_m2,"
        "mean_floor_std,rate_m2ps,rate_std,cond,rate_frozen,cfg_floor_m2,"
        "robust_ratio,mean_ratio,floor_clamp,rate_clamp\n";
    {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        trace.open(trace_path);
        if (trace) {
            trace << kTraceHeader;
        } else {
            log->warn("Network: failed to open trace file {}", trace_path.string());
        }
        soln.open(soln_path);
        if (soln) soln << kSolnHeader;
        else log->warn("Network: failed to open solution trace {}", soln_path.string());

        if (nc.trace_ionosphere) {
            iono_trace.open(iono_path);
            if (iono_trace)
                iono_trace << "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg\n";
            else
                log->warn("Network: failed to open ionosphere trace {}", iono_path.string());
        }

        if (config_.estimateNetworkIonoSh() && shc.trace) {
            vtec_sh_trace.open(vtec_sh_path);
            if (vtec_sh_trace)
                vtec_sh_trace << "week,tow,n,m,trig,value_tecu,std_tecu\n";
            else
                log->warn("Network: failed to open SH-VTEC trace {}", vtec_sh_path.string());
            // T1a health trace — one row per emitted epoch.  The per-degree columns are
            // sized from the truncation degree N (deterministic header).
            vtec_sh_health_trace.open(vtec_sh_health_path);
            if (vtec_sh_health_trace && sh_model) {
                std::string hdr = "week,tow,a00_tecu,a00_std_tecu,rms_all_tecu,"
                                  "slant_resid_mean_m,slant_resid_rms_m,slant_resid_max_m,"
                                  "n_los,n_los_gt3sigma,n_stations,n_stations_gt3sigma,a00_in_band";
                for (int n = 0; n <= sh_model->maxDegree(); ++n)
                    hdr += ",rms_deg" + std::to_string(n) + "_tecu";
                for (int n = 0; n <= sh_model->maxDegree(); ++n)
                    hdr += ",kaula_ratio_deg" + std::to_string(n);
                hdr += "\n";
                vtec_sh_health_trace << hdr;
            } else if (!vtec_sh_health_trace) {
                log->warn("Network: failed to open SH-VTEC health trace {}",
                          vtec_sh_health_path.string());
            }
        }

        if (closed_loop_on && consumer) {
            const std::filesystem::path cl_path = log_dir_ / "closed_loop_trace.csv";
            cl_trace.open(cl_path);
            if (cl_trace)
                cl_trace << "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
                            "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate\n";  // T2c: +ar_gate
            else
                log->warn("Network: failed to open closed-loop trace {}", cl_path.string());
        }

        if (nc.trace_slip_events) {
            if_slip_trace.open(if_slip_trace_path);
            if (if_slip_trace)
                // dd = between-sat/between-epoch IF ΔΔφ (m); med = network pool median; s_pool
                // = elevation-normalized pooled scale; ewma_scale/count = this station-pair's
                // own EWMA scale + clean-sample count (decision-time, pre-update); w/s_eff =
                // shrinkage weight + effective scale; elev_map = 1/sin el; thr/floor = gate;
                // abs_dev = |dd−med|; flagged/carry_warm = decision + whether it refloated.
                if_slip_trace << "week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,"
                                 "s_pool_m,ewma_scale_m,count,w_shrink,s_eff_m,elev_map,"
                                 "thr_m,floor_m,abs_dev_m,flagged,carry_warm\n";
            else
                log->warn("Network: failed to open IF-slip trace {}", if_slip_trace_path.string());
            raim_trace.open(raim_trace_path);
            if (raim_trace)
                // One row per (sat, station) whose normalized post-fit residual spiked, or any
                // station of a refloated sat: s_max_raw = v·√w (measurement-standardized), s_max_norm
                // = that ÷ the robust a-posteriori variance factor sigma0, vs raim_k; the per-sat
                // spiked-station count vs the min_sta consensus; and whether the sat was refloated.
                raim_trace << "week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,"
                              "n_sta_spiked,min_sta,refloated\n";
            else
                log->warn("Network: failed to open RAIM trace {}", raim_trace_path.string());
            raim_attrib_trace.open(raim_attrib_trace_path);
            if (raim_attrib_trace)
                // Prominence attribution (LOG-ONLY, does not change the refloat action):
                // robust 2-way median polish of the (station × sat) post-fit residual table
                // each RAIM epoch → per-dimension effect + robust-z prominence.  dim ∈
                // {SAT, STA, SIG, ARC}; acted = whether today's per-sat consensus already
                // refloats that entity (SAT/ARC only).  Only entities above the report
                // prominence are emitted, so quiet epochs cost nothing.
                raim_attrib_trace << "week,tow,dim,entity,effect,prominence,n_cells,overall,"
                                     "sigma0,acted\n";
            else
                log->warn("Network: failed to open RAIM attribution trace {}",
                          raim_attrib_trace_path.string());
        }
        if (nc.trace_slip_events && (config_.networkClockMonitor() || config_.networkClockModel())) {
            clk_mon_trace.open(clk_mon_trace_path);
            if (clk_mon_trace)
                // clk = solved Δδtˢ (m); median = network median (gauge); dev = clk−median;
                // prev_clk/jump = previous adjacent-epoch value + step (blank if unavailable);
                // flag_abs/flag_jump = this monitor's own bounds; flag_ext = already flagged by
                // the within-epoch / post-fit RAIM; refloated = carried clock reinitialized.
                clk_mon_trace << "week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,"
                                 "abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated,"
                                 "nis,mode\n";   // L4: nis,mode appended (trailing-column format break)
            else
                log->warn("Network: failed to open clock-monitor trace {}",
                          clk_mon_trace_path.string());
        }
        if (nc.trace_anchor_info) {
            anchor_info_trace.open(anchor_info_trace_path);
            if (anchor_info_trace)
                // anchor_info = constant 1/initial_std.sat_clock² (m⁻²), the a-priori
                // CODE-product Tikhonov pull; total_info = reduced shared-normals diagonal
                // on this sat's clock column (data + carry prior + datum pin for the pivot);
                // carry_info = the carried clock-prior info folded this epoch (scalar D2 or
                // the two-state model); data_info = total − carry − anchor (pivot pin still
                // in it, hence is_pivot); anchor_frac_data = anchor/(anchor+data) — the
                // HONEST data-vs-anchor fraction, INDEPENDENT of carry stiffness (P1-4);
                // carry_frac = carry/total exposes a carry-dominated lock-in (high ⇒ the
                // clock is defined by its own past, not by data); frac = the legacy
                // anchor/total (kept for continuity; misleads for a locked-in clock).
                anchor_info_trace << "week,tow,sat,anchor_info,carry_info,data_info,total_info,"
                                     "anchor_frac_data,carry_frac,frac,is_pivot\n";
            else
                log->warn("Network: failed to open anchor-info trace {}",
                          anchor_info_trace_path.string());
        }
        if (nc.trace_dd_fractional) {
            dd_frac_trace.open(dd_frac_trace_path);
            if (dd_frac_trace) {
                // Between-station DD of undifferenced float ambiguities (m): sat_i minus the
                // per-(const,signal) reference sat, differenced across station pair (sta_a,
                // sta_b).  P1-2: lambda_m / dd_cyc / frac_cyc are now computed here (frac_cyc =
                // dd_cyc − round(dd_cyc)) so the CSV is directly a fractional-cycle histogram —
                // the go/no-go instrument for float-bias user AR.  GLONASS (FDMA) and the pivot
                // station (pinned ambiguities) are excluded from the pool below.
                dd_frac_trace << "week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,"
                                 "dd_m,lambda_m,dd_cyc,frac_cyc,dd_sigma_cyc\n";  // T2a: +dd_sigma_cyc
                dd_ppp_utils = std::make_unique<PppUtils>(log_manager_);
            } else {
                log->warn("Network: failed to open DD-fractional trace {}",
                          dd_frac_trace_path.string());
            }
        }
        if (nc.trace_slip_raw) {
            slip_raw_trace.open(slip_raw_trace_path);
            if (slip_raw_trace) {
                // Per-(station,sat) RAW slip observables for the primary dual-freq pair, every
                // epoch, computed straight from raw_observed (phase in cycles, code in metres) —
                // NO dependence on any detector being armed.  mw_m/gf_m/l1_c1_m are absolute (the
                // WL slip step is λ_WL≈0.86 m/cyc in mw_m, an L1/L2 slip jumps gf_m); lli1/lli2
                // are the RINEX loss-of-lock flags for sig1/sig2 (bit 0 = slip).  Diff adjacent
                // epochs offline for the slip signature.  Pivot station included (diagnostic).
                slip_raw_trace << "week,tow,station,sat,sig1,sig2,elev_deg,"
                                  "mw_m,gf_m,l1_c1_m,lli1,lli2\n";
                slip_raw_ppp_utils = std::make_unique<PppUtils>(log_manager_);
            } else {
                log->warn("Network: failed to open raw-slip trace {}",
                          slip_raw_trace_path.string());
            }
        }
        if (nc.trace_epoch_status) {
            epoch_status_trace.open(epoch_status_trace_path);
            if (epoch_status_trace)
                // ONE row per PROCESSED epoch, written before each `continue` (skip) and at
                // the emit site.  disposition classifies WHY the epoch was skipped; sigma0 /
                // n_adapt / n_refloat_sats / n_retro_applied / datum_changed capture the QC
                // actions; clock_median_m/std_m/worst_sat/worst_dev_m are the clock-Δ spread
                // (median+std+worst-sat convention).  BLANK (not 0) fields = not-in-scope at
                // that disposition site.  sigma0_trigger/withhold_ceiling are config echoes so
                // viewers draw honest threshold lines.  Note: fast-forward (start_offset_hours)
                // and max-duration epochs are uncounted by design and write NO row.
                epoch_status_trace << "week,tow,disposition,sigma0,n_adapt,n_present,"
                                      "n_clock_sats,clock_median_m,clock_std_m,worst_sat,"
                                      "worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,"
                                      "sigma0_trigger,withhold_ceiling,n_arc_refloat\n";  // T2d: +n_arc_refloat (appended, positions preserved)
            else
                log->warn("Network: failed to open epoch-status trace {}",
                          epoch_status_trace_path.string());
        }
        if (nc.health_interval_s > 0.0) {
            // RTB-6: periodic operator health heartbeat.  Cumulative epoch tallies +
            // σ̂0 EWMA + windowed ms/epoch + last-emit age + stations/clock-sats +
            // realized pivots, appended every network.health_interval_s of content
            // time.  A service supervisor (or a human) polls this instead of the
            // exit-only summary.
            health_trace.open(health_trace_path);
            if (health_trace)
                health_trace << kHealthHeader;
            else
                log->warn("Network: failed to open service-health trace {}",
                          health_trace_path.string());
            // P4(a): the σ̂0 EWMA updates ONLY inside the warm post-fit RAIM block
            // (emit_ura && raim && phase_leveling); with any of them unarmed the
            // column would silently report its frozen 1.0 init.  It emits the −1
            // "unavailable" sentinel instead — warn once so the operator knows the
            // quality channel of this heartbeat is dark, not clean.
            if (!(config_.networkEmitUra() && config_.networkRaim()
                  && config_.networkPhaseLeveling()))
                log->warn("Network (RTB-6): health heartbeat is armed but the σ̂0 EWMA's "
                          "feeding tiers are not (needs network.emit_ura + network.raim + "
                          "network.phase_leveling; ura={}, raim={}, phase_leveling={}) — "
                          "the sigma0_ewma column will report the -1 'unavailable' "
                          "sentinel for this run.",
                          config_.networkEmitUra(), config_.networkRaim(),
                          config_.networkPhaseLeveling());
        }
        if (config_.networkCarryVce()) {
            carry_vce_trace.open(carry_vce_trace_path);
            if (carry_vce_trace)
                carry_vce_trace << kCarryVceHeader;
            else
                log->warn("Network (M2/LS-VCE): failed to open carry-VCE trace {}",
                          carry_vce_trace_path.string());
        }
        if (nc.trace_events) {
            events_trace.open(events_trace_path);
            if (events_trace)
                // Long format: one row per discrete QC action.  tier in {T1_if_slip,
                // T2_within_epoch, T3_raim, T4_attrib, T5_clock_model, T6_clock_monitor,
                // GATE, SIGMA0, PIVOT, RETRO}.  statistic/threshold blank where not in
                // scope; detail is comma-sanitized (',' -> ';').  Rows are written even on
                // epochs that are subsequently WITHHELD — that is the disposition story.
                events_trace << "week,tow,tier,event,sat,station,statistic,threshold,"
                                "action,detail\n";
            else
                log->warn("Network: failed to open events trace {}", events_trace_path.string());
        }
        if (nc.trace_zwd) {
            zwd_trace.open(zwd_trace_path);
            if (zwd_trace)
                // zwd_m = a-priori state + reconstructed correction (the estimated correction
                // about the modeled a-priori); std_m = datum-conditioned formal σ; carried = a
                // ZWD carry prior was actually injected this epoch.
                zwd_trace << "week,tow,station,zwd_m,std_m,carried\n";
            else
                log->warn("Network: failed to open ZWD trace {}", zwd_trace_path.string());
        }
        if (nc.trace_station_residuals) {
            station_res_trace.open(station_res_trace_path);
            if (station_res_trace)
                // One row per post-fit RAIM cell: non-pivot station (pivot never appears — it is
                // skipped in the cell loop) x carried-ambiguity sat.  s_raw = measurement-
                // standardized residual (B2-normalized when raim_redundancy_normalize on),
                // s_norm = s_raw / σ̂0, sigma0 = the epoch robust variance factor.  s values are
                // ABSOLUTE (non-negative).  Rows exist only for RAIM epochs (raim && carry_warm).
                station_res_trace << "week,tow,station,sat,s_raw,s_norm,sigma0\n";
            else
                log->warn("Network: failed to open station-residual trace {}",
                          station_res_trace_path.string());
        }
    }

    // L1: one network_epoch_status.csv row per processed epoch.  Fields not yet in
    // scope at a given disposition site are left BLANK (std::nullopt), NOT zero — the
    // pyviz timeline viewers (L5) distinguish "unavailable" from a real 0.
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
    };
    const double es_sigma0_trigger   = config_.networkRaimSigma0Trigger();   // config echo
    const double es_withhold_ceiling = config_.networkRaimWithholdSigma0();  // config echo

    // === Epoch loop =========================================================
    std::vector<CarryPrior> prev_carry;                 // D2 stash from the last solve
    std::set<std::string> prev_clock_refloat_sats;      // Card 1d: clock-monitor flags at the last stash (hard-refloat mode skip-stashes the sat, so prev_carry cannot carry the flag)
    std::map<std::pair<SharedParamKind, std::string>, double> tu_reinit_hist;  // Card 1d: (kind, sat) → content time of the last reinit FOLD (multi-epoch-gap coverage)
    SharedCarryCov prev_carry_cov;                      // C1: dense covariance side-car of prev_carry (empty when OFF)
    std::map<std::string, std::string> prev_pivot;      // constellation -> pivot sat (handover detect)
    std::set<std::string> c1_zoh_active;                 // L2/T5: sats currently emitting c1=0 (ZOH) — edge-trigger for the R6 event
    // A1: persistent last-EVER pivot per constellation; MERGE-updated (never
    // wholesale-overwritten) so a constellation that vanishes for an epoch and
    // returns with a DIFFERENT pivot still raises datum_changed.  Read only when
    // network.pivot_vanish_return_signal is on (otherwise inert).
    std::map<std::string, std::string> last_pivot;
    // network.clock_signal_basis_hold: per-satellite HELD ionosphere-free clock
    // reference pair (sat -> {sig1, sig2}) for the W6 IF pin.  Latched the first
    // epoch a satellite's code-bias columns register a usable dual-frequency pair
    // and reused every subsequent epoch while BOTH signals stay registered, so the
    // emitted (δtˢ, dˢ) split is anchored to ONE signal basis for the whole run —
    // the IGS fixed-clock-convention analogue that keeps the differential clock
    // time-consistent.  Re-latched (to the current epoch's freshly-selected pair)
    // ONLY when a held reference signal leaves the ≥2-station set.  Empty / unread
    // unless clock_signal_basis_hold (so the IF-pin datum is byte-identical OFF).
    // Not snapshotted (v1): a warm resume re-latches from first sight after the
    // resume — one small per-sat basis step (trace-logged only, not wire-signalled,
    // exactly like the per-epoch re-selection it replaces); the toggle is in the
    // resume fingerprint so a toggle change already cold-starts.
    std::map<std::string, std::pair<std::string, std::string>> held_clock_ref;
    // Deterministic IF-pin selection discontinuity (hold OFF).  With
    // clock_signal_basis_hold OFF the pin is re-derived from every epoch's
    // coverage; the deterministic NetworkSbasis::selectIfPinPair makes it a pure
    // function of the registered signal SET, so a change between EMITTED products
    // means a channel genuinely entered/left the ≥2-station set — stepping the
    // emitted (δtˢ, dˢ) split by the satellite's inter-channel code bias, a per-sat
    // datum move the consumer must reset for.  Two maps mirror the phase-anchor
    // discipline (this_epoch_anchor / emitted_anchor) so a transient pin move on a
    // WITHHELD epoch cannot fabricate a bump:
    //  - this_epoch_if_pin_sel: the (sig1,sig2) SELECTED this epoch in the main
    //    pass (scratch, cleared each epoch, decoupled from the emit decision);
    //  - last_if_pin_sel: the pair of the LAST EMITTED product per satellite,
    //    compared + advanced ONLY at the emit site (like emitted_anchor/prev_pivot),
    //    so a withheld epoch leaves it untouched and its transient pair is dropped.
    // Both populated only under !clock_signal_basis_hold && emit_clock_disc (the
    // hold path owns its own re-latch signalling via held_clock_ref) ⇒ byte-
    // identical otherwise.  Not snapshotted: a warm resume has no prior for its
    // first emit, so it cannot — and must not — fabricate a disc it cannot verify.
    std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_sel;
    std::map<std::string, std::pair<std::string, std::string>> last_if_pin_sel;
    // Prioritized phase-bias anchor (network.phase_datum_prioritized) — all three
    // keyed by the "sat_signal" ambiguity form, all empty/unread on the legacy path:
    //  - anchor_incumbent: the STICKY incumbent anchor STATION NAME per (sat,signal),
    //    read when electing (kept while it still tracks the sat), advanced at election.
    //  - this_epoch_anchor: the anchor STATION NAME elected THIS epoch, captured at
    //    election and read at the emit site (decouples emit from the PASS-A scratch).
    //  - emitted_anchor: the anchor NAME of the LAST EMITTED product per (sat,signal),
    //    advanced ONLY at emit (mirrors prev_pivot/last_pivot); a change vs this drives
    //    the per-satellite handover discontinuity.  Never erased (a returning key is a
    //    first sighting, not a fault — matches the last_pivot never-erase policy).
    std::map<std::string, std::string> anchor_incumbent;
    std::map<std::string, std::string> this_epoch_anchor;
    std::map<std::string, std::string> emitted_anchor;
    // Card 3 (network.phase_datum_eq34) chain election, all empty/unread otherwise:
    //  - eq34_datum_rx: the STICKY datum receiver r1 STATION NAME per (constellation
    //    |signal) component, keyed "c|σ".  Kept while the incumbent is still present
    //    in the component (fewest datum-receiver handovers); advanced at election
    //    (mirrors anchor_incumbent's per-election stickiness).
    //  - anchor_incumbent / this_epoch_anchor / emitted_anchor are REUSED for the
    //    tree bookkeeping, holding each satellite's PARENT-EDGE station (unique per
    //    "sat_signal" key — receiver-side parent edges live in eq34_rx_parent, NOT
    //    here, so an interior satellite's several tree edges can never collide in
    //    one map slot).  The existing emit-site handover machinery (jump-test disc +
    //    trace) applies unchanged: a parent-edge reattach == an anchor change.
    //  - eq34_rx_parent: each receiver's parent-edge SATELLITE in last epoch's
    //    spanning tree, keyed "<station>|c|σ".  anchor_incumbent (sat side) +
    //    eq34_rx_parent (receiver side) together ARE last epoch's tree — each edge
    //    lives in exactly one map, both replaced wholesale per election — so the
    //    still-live subset seeded into the sticky rebuild is always a forest.
    //  - eq34_sat_root: each sat's component datum receiver r1 last epoch, keyed
    //    "sat_signal".  A root change (r1 handover, component migration) shifts the
    //    whole component frame by a common fractional + per-sat integers, so it
    //    re-references every sat even where all tree edges survived.
    std::map<std::string, std::string> eq34_datum_rx;
    std::map<std::string, std::string> eq34_rx_parent;
    std::map<std::string, std::string> eq34_sat_root;
    // Card 3 (I4 longevity election): last epoch's per-(station idx, sat) arc
    // elevation, for the rising-vs-setting test that ranks NEW spanning-tree edges
    // by how long they will survive.  Rebuilt each epoch from sat_el; empty and
    // unread unless network.phase_datum_eq34_longevity_election.
    std::map<std::pair<size_t, std::string>, double> eq34_prev_el;
    // Card 3: the datum receiver r1 (pdr=) of the LAST EMITTED contract, so a root
    // handover triggers an S-line re-broadcast (advanced only at emit, like the
    // pivot baselines).  Empty on every non-eq34 run.
    std::string eq34_emitted_pdr;
    // Card 3: wavelength provider for the pin-to-round re-pin target (λ·round(float/λ));
    // built lazily on first eq34 use.  Null on every non-eq34 run.
    std::unique_ptr<PppUtils> eq34_ppp_utils;
    // Card 3 run-scope observability counters, logged once in the run-end producer
    // summary (the per-epoch reref/erase lines are debug-only, which left the
    // Jul-10 field A/B blind to the reref volume).  Untouched on non-eq34 runs.
    long long eq34_reref_total = 0, eq34_erase_total = 0;
    long long eq34_shift_total = 0;   // carry values S-transformed (v2-shift) over the run
    long long eq34_gap_repin_total = 0;   // I2: tree edges resumed from a carry-alive gap (re-pinned to their pre-gap integer instead of re-leveling from 0)
    int eq34_mismatch_epochs = 0;
    // Card 3 (I2 extend-only): tree edges retained across the current gap because
    // their local_carry is still alive — {receiver index, "sat_signal"}.  Rebuilt at
    // each election's sticky-map update; read at the next election's recordEdge to
    // count a gap re-pin.  Empty unless network.phase_datum_eq34_extend_only.
    std::set<std::pair<int, std::string>> eq34_retained_edges;
    // Card 3: last epoch's pin target per (STATION index, "sat_signal") — the
    // exact same-station pinmove baseline for the shift walk.  MUST be keyed per
    // station: an interior sat carries several pins (its parent edge + one per
    // child-receiver edge), each at a different station with its own target —
    // an amb-only key collides exactly like the original this_epoch_anchor
    // defect, and the resulting garbage pinmoves feed back through the carry
    // shifts into next epoch's rounded targets (measured: 4e8-cycle blowup).
    // Replaced wholesale each election.
    std::map<std::pair<int, std::string>, double> eq34_prev_target;
    // Card 3 per-epoch re-reference set (sat, signal): cleared + rebuilt at each
    // eq34 election, read (1) by the local-carry erase right after the election
    // and (2) by the D2/C1 shared-carry consume, which must MARGINALIZE the
    // re-referenced PB keys for that epoch — their carried priors belong to the
    // retired frame, and folding them yanks the new frame by up to the full
    // re-level (the 03:46 silent-re-level mechanism).  Always empty when eq34 off.
    std::set<std::pair<std::string, std::string>> eq34_reref;
    // Per-satellite clock sanity monitor (network.clock_monitor): the previous emitted
    // clock value + the continuous-GPS second it was stashed, per satellite, for the
    // adjacent-epoch jump test.  Erased on refloat so the post-reinit clock re-baselines.
    std::map<std::string, std::pair<double, double>> prev_clk;  // sat -> {Δδtˢ (m), seen_s}
    // Two-state oscillator clock state per satellite (network.clock_model): the carried
    // [offset, drift] + 2×2 covariance, persisted across epochs.  Empty / inert unless clk_model_on.
    std::map<std::string, ClockOsc> clock_osc;   // per-sat carried [offset,drift]+cov+t_last (P0-3: t_last drives per-sat prediction/drift-FD across gaps)
    // network.emit_clock_rate: decoupled per-sat rate state used ONLY to derive the emitted C1 by
    // finite-differencing the posterior solved clock (x0=prev solved offset, x1=drift, t_last).  Never
    // feeds a prior or a refloat ⇒ the solution is byte-identical; only clock_c1_m_s changes.  Empty /
    // inert unless emit_clock_rate_on (which is off whenever clk_model_on supplies C1 from clock_osc).
    std::map<std::string, ClockOsc> clock_rate_est;
    // Card 6 (RTF-2, network.apriori_source = brdc_rts): the broadcast-ephemeris IODE
    // that each satellite's CARRIED clock state (clock_osc.x0 / the D2 clock carry /
    // the clock-monitor jump baseline) is currently referenced to.  Advanced at the
    // eph-cutover RE-LEVEL (before the solve), NOT deferred to the emit site like
    // prev_pivot — the re-level MUTATES the carried state whether or not this epoch
    // emits (the post-emit carry updates are skipped on a withhold/solve-fail, so the
    // shifted values persist), so deferring the advance would re-detect the same roll
    // next epoch and DOUBLE-shift.  Empty / inert unless brdc_rts.  (The plan called
    // this last_emitted_iode_; "referenced-to" is the honest name since it advances on
    // withheld epochs too.)
    std::map<std::string, int> relevel_iode;
    // Card 6: sats whose eph rolled but whose PRIOR version was already evicted from
    // the store, so Δ_eph is incomputable — a forced per-sat discontinuity is queued
    // and lands at the sat's next actual emit (present/pending split like
    // ura_retro_sats).  Effectively unreachable in steady state (per-epoch checking
    // makes two rolls between consecutive checks impossible); a defensive belt for a
    // snapshot gap / two-roll corner.
    std::set<std::string> eph_cutover_disc_pending;
    // T3c: sats whose clock_signal_basis_hold IF-pin reference pair RE-LATCHED this epoch —
    // the emitted δtˢ↔dˢ split moved (a genuine per-sat clock step), so force a per-sat disc
    // bump at the next emit (present/pending split like eph_cutover_disc_pending).  Only ever
    // populated under clock_signal_basis_hold && emit_clock_disc ⇒ byte-identical otherwise.
    // (The hold-OFF deterministic re-selection uses the emit-only this_epoch_if_pin_sel /
    // last_if_pin_sel path, NOT this queue — see that comment near held_clock_ref.)
    std::set<std::string> basis_relatch_disc_pending;
    // Card 2 (network.code_datum_time_linked): the code-bias datum's previous-epoch
    // solved values, keyed "<constellation>|<signal>" -> satellite -> value.  The
    // veteran-referenced sum-to-zero RHS and the IF-pin RHS read this at gauge
    // construction; it is cleared + rebuilt from the solved shared vector at the D2
    // harvest (kept epochs only — so a departed satellite is dropped and its return
    // re-seeds IT alone).  NOT snapshotted (a warm resume re-seeds, signalled).
    std::map<std::string, std::map<std::string, double>> code_bias_prev_;
    // Card 2: the IF-pin reference pair (sat -> {sig1,sig2}) that produced the values
    // in code_bias_prev_ — the SAME kept-epoch lifecycle (rebuilt at the D2 harvest
    // from the scratch below).  A pair CHANGE between kept epochs re-references the
    // emitted clock↔code split by the satellite's inter-channel bias (the band-1
    // C1C↔C1W flip), so the IF-pin RHS re-seeds and the disc bumps even when the new
    // pair's signals both have previous values.  NOT snapshotted.
    std::map<std::string, std::pair<std::string, std::string>> code_bias_ifpin_pair_;
    // Card 2: this epoch's selected IF-pin pair per satellite (valid pairs only),
    // captured in the main pass and copied into code_bias_ifpin_pair_ at the harvest
    // (kept epochs only — mirrors code_bias_prev_, so a pair recorded on a withheld
    // epoch never advances the comparison baseline).  Cleared each epoch.
    std::map<std::string, std::pair<std::string, std::string>> this_epoch_cb_ifpin;
    // Card 2: satellites that RE-SEED this epoch (IF-pin split or a cold sum-to-zero
    // channel).  A per-epoch scratch (cleared each epoch) populated in the main pass and
    // MERGED into cb_reseed_disc_pending only at the emit site — which every whole-epoch
    // withhold path `continue`s before reaching.  So a transient re-seed on a since-
    // withheld epoch (the frame reverts by the next kept epoch) never enters the pending
    // set and cannot fire a bump with no emitted value step (the emit-only discipline
    // this_epoch_if_pin_sel / last_if_pin_sel use), WITHOUT an erase that would destroy a
    // bump legitimately deferred for a satellite re-seeded while pruned from the emission.
    std::set<std::string> this_epoch_cb_reseed;
    // Card 2: satellites whose code-bias split has EVER been harvested — a later
    // re-seed of one of these is a RETURN (disc-bump), not a genuine first appearance
    // (seed silently).  Accumulates for the whole process; NOT snapshotted.
    std::set<std::string> code_bias_sat_seen_;
    // Card 2: a warm resume starts code_bias_prev_/seen EMPTY, but the pre-snapshot
    // product was emitted, so every first post-resume re-seed IS a re-reference the
    // consumer must be told about.  Set true in the resume block; makes re-seeds bump
    // regardless of code_bias_sat_seen_ for the rest of a resumed run (conservative —
    // harmless on a genuinely-new satellite's first emit).  NOT snapshotted.
    bool code_bias_datum_resumed_ = false;
    // Card 2: satellites to disc-bump for a time-linked datum RE-SEED (a no-veteran
    // sum-to-zero channel or an IF-pin split forced to the 0 seed → the emitted value
    // stepped).  Fed at the emit site from this_epoch_cb_reseed (kept epochs only), then
    // flushed with the same present/pending split as eph_cutover_disc_pending so the bump
    // lands with the sat present (emit-only, replay-faithful).  Only ever populated under
    // code_datum_time_linked ⇒ empty (no-op, byte-identical) otherwise.
    std::set<std::string> cb_reseed_disc_pending;
    double start_anchor_s = -1.0;                        // first-epoch time anchor for network.start_offset_hours
    // Phase-leveling per-station ambiguity carry (Increment 1): persistent across
    // epochs, keyed by (station index, sat, signal); ORDERED so the inject order
    // (which feeds rank-1 LDLᵀ updates) is byte-identical serial≡parallel.  Inert
    // unless network.phase_leveling.
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> local_carry;
    // Per-station ZWD carry (B9): persistent across epochs, keyed by station index
    // (ZWD is one-per-station).  Inert unless network.carry_zwd.
    std::map<int, LocalCarry> zwd_carry;
    // Card 3 (eq34): per-(station index, "sys_RPB_σ") receiver phase-bias carry —
    // LOCAL time-linking of δ_{r,σ} (spec D6 v2 in its minimal local form, no
    // border promotion).  Re-estimating the RPB fresh each epoch injects
    // anti-correlated per-epoch wander into every individual ambiguity float of
    // that (station,σ): DDs cancel it exactly, but the per-key phase-leveling
    // carry does NOT — measured Jul-10 as 10–100× datum-invariant DD-step damage.
    // Read-only during the parallel matrix build (fold), written at the serial
    // post-solve stash, erased for DIRTY receiver vertices at the election (their
    // chain frame stepped) and pruned on gap like local_carry.  Inert unless
    // network.phase_datum_eq34 with phase_datum_eq34_rpb_q_m2s > 0.
    std::map<std::pair<int, std::string>, LocalCarry> rpb_carry;
    // B1 URA quality channel carried state (inert unless network.emit_ura).
    double                       ura_sigma0_ewma = 1.0;   ///< network-wide σ̂0 EWMA scaling the emitted URA (one-epoch lag).
    std::map<std::string, int>   ura_refloat_horizon;     ///< sat -> epochs remaining of post-refloat URA inflation.
    // Isolated-arc persistence gate carried state (inert unless network.raim_arc_persist_epochs > 1):
    // (station index, satellite) -> number of CONSECUTIVE epochs this isolated arc has tripped the
    // attribution interaction gate WITHOUT yet being refloated.  Reset to 0 (dropped) on any epoch the
    // arc is not flagged, and cleared the epoch it refloats.  Not part of the warm-resume snapshot — a
    // resume simply re-accrues the streak (a one-time K-epoch delay after resume), never mis-refloats.
    std::map<std::pair<int, std::string>, int> arc_spike_streak;
    // B1 retro-bump (P0-2/P1-3): per-SATELLITE, tier-independent, edge-triggered.
    // ura_retro_sats = sats a post-emit QC tier flagged LAST epoch, applied as a
    // per-sat discontinuity at the sat's NEXT ACTUAL emit (markSatDiscontinuity):
    // a flagged sat absent from an emitted epoch (RT0-5 quarantine, vanish) stays
    // pending here rather than being consumed without landing.  retro_active =
    // sats currently flagged and already bumped, so a sat that stays flagged during
    // recovery is bumped ONCE (on its rising edge) — no per-constellation reset storm.
    std::set<std::string>        ura_retro_sats;
    std::set<std::string>        retro_active;
    // RTB-7(b/c): startup quality-ramp state.  The DEGRADED URA floor
    // (nc.warmup_ura_floor_m) applies until ramp_lifted; the lift rule is
    // networkWarmupRampLifted (emitted-epoch horizon AND σ̂0-EWMA update count,
    // the count waived when the EWMA is structurally unavailable).  All inert
    // (never read) while the floor is 0 — byte-identical.  On an RTB-7(a) WARM
    // resume these restore from the snapshot: a warm-resumed producer is NOT
    // degraded and reports grade 0 ("warm").
    size_t ura_ewma_updates     = 0;     ///< σ̂0 EWMA updates received (post-fit RAIM tier).
    bool   ramp_lifted          = false; ///< Warmup URA floor lifted (product grade reached).
    double first_emit_content_s = -1.0;  ///< Content time of the FIRST emit (grade anchor).
    double grade_metric_s       = -1.0;  ///< RTB-7(c) cold-start-to-product-grade (s); −1 before lift / ramp off; 0 = warm resume.
    const bool   pl_on         = config_.networkPhaseLeveling();
    const double pl_q_amb      = config_.networkPhaseLevelingQAmb();
    const int    pl_warmup     = config_.networkPhaseLevelingWarmupEpochs();
    const double pl_max_gap_s  = config_.networkPhaseLevelingMaxGapS();
    const double pl_var_floor  = config_.networkPhaseLevelingVarFloorM2();  // lock-in guard; 0 = off
    const bool   pl_warmup_on_conv  = config_.networkPhaseLevelingWarmupOnConvergence();  // T1d
    const bool   pl_refloat_on_conv = config_.networkPhaseLevelingRefloatAtConvergence(); // T1d
    const double pl_conv_rms_thresh = config_.networkPhaseLevelingConvRmsThreshTecu();     // T1d
    const double pb_var_floor  = config_.networkSharedCarryVarFloorPbM2();  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = config_.networkSharedCarryVarFloorCbM2();  // B4: D2 code-bias carry floor;  0 = off
    const bool   fullcov_on    = config_.networkSharedCarryFullCov();       // C1: dense (P_prev+Q)⁻¹ shared carry
    const bool   carry_cpl_cond = config_.networkCarryConditionalCoupling(); // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = config_.networkCarryCouplingJointWhiten(); // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_tu   = config_.networkCarryCouplingTimeUpdate();  // Card 1d: border time update of the coupling rows at injection (κ shrink + M²κq inflation) — requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_shmarg = config_.networkCarryCouplingShMarginalize(); // Card 1d-SH: fully marginalize VtecSh coupling terms (GM border ≠ RW κ model — severs the amb↔SH carry feedback) — requires carry_cpl_cond (enforced at load)
    const bool   zwd_on        = config_.networkCarryZwd();                 // B9: carry per-station ZWD across epochs
    const double zwd_q         = config_.networkZwdCarryQ();                // B9: ZWD carry random-walk process noise (m²/s)
    // M2 (LS-VCE carry-q): the carry-row variance-component MONITOR.  Accumulates
    // the post-fit carry innovations (carried − solved) into a residual-form
    // LS-VCE estimator and logs σ̂ (Floor m² + Rate m²/s) per family per
    // constellation against the hand floors.  NO feedback into the solve — the
    // whole path is gated on carry_vce, so OFF is byte-identical.  The estimator
    // is seeded with the PL floor/q as its weak a-priori (worth ~1 row; immaterial
    // vs the thousands of carry rows/epoch — pb/cb families share that weak PL-
    // scale prior, which only sets the regularisation and the no-data fallback).
    const bool   carry_vce     = config_.networkCarryVce();
    const std::vector<std::string> vce_families =
        carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                  : std::vector<std::string>{};
    const bool   vce_wants_pb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("pb")) != vce_families.end();
    const bool   vce_wants_cb = std::find(vce_families.begin(), vce_families.end(),
                                          std::string("cb")) != vce_families.end();
    NetworkCarryVce carry_vce_est(vce_families,
        static_cast<double>(config_.networkCarryVceWindowEpochs()),
        /*split_by_const=*/true,
        /*seed_floor=*/pl_var_floor, /*seed_rate=*/pl_q_amb);
    int last_vce_hour = -1;   // content-hour bucket for the per-hour VCE log line
    double first_vce_s = -1.0;  // content time of the first monitored epoch (hour anchor)
    const bool   ura_on        = config_.networkEmitUra();                  // B1: emit per-sat URA quality channel
    const double ura_alpha     = config_.networkUraEwmaAlpha();             // B1: σ̂0 EWMA smoothing
    const bool   ura_retro     = config_.networkUraRetroBump();             // B1: retro-bump flagged SATELLITE(s) on next emit
    const double ura_refloat_inflate = config_.networkUraRefloatInflate(); // B1: URA multiplier post-refloat
    const bool   raim_on       = config_.networkRaim();            // post-fit network RAIM (Increment 3, T2)
    const double raim_k        = config_.networkRaimKSigma();      // standardized post-fit residual flag threshold
    const double raim_s0_floor = config_.networkRaimSigma0Floor(); // floor on the robust a-posteriori σ̂0 (1.0 default; lower lets σ̂0 < 1)
    const int    raim_min_sta  = config_.networkRaimMinStations(); // cross-station consensus count
    const int    raim_arc_persist = config_.networkRaimArcPersistEpochs(); // consecutive-epoch persistence gate for isolated-arc refloats (1 = act immediately, byte-identical)
    const bool   raim_maturity_gate   = config_.networkRaimRefloatMaturityGate();   // T2d: gate isolated-arc refloats on maturity
    const int    raim_maturity_epochs = config_.networkRaimRefloatMaturityEpochs();  // T2d: maturity horizon
    const double raim_withhold = config_.networkRaimWithholdSigma0(); // A4: >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off
    const bool   raim_redun    = config_.networkRaimRedundancyNormalize(); // B2: Baarda per-row r_i normalization of the RAIM residual
    const bool   clk_mon_on    = config_.networkClockMonitor();       // per-sat carried-clock sanity monitor + refloat
    const double clk_mon_abs   = config_.networkClockMonitorAbsMaxM();  // |Δδtˢ − median| refloat bound (m)
    const double clk_mon_jump  = config_.networkClockMonitorJumpMaxM(); // one-epoch clock jump refloat bound (m)
    const bool   clk_mon_couple = clk_mon_on && config_.networkClockMonitorCoupleRaim();  // also refloat RAIM-flagged sats' clocks (opt-in)
    const double clk_mon_reinit_sig = config_.networkClockMonitorReinitSigmaM();  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    const double clk_mon_reinit_var = clk_mon_reinit_sig * clk_mon_reinit_sig;   // reinit variance folded next epoch for a flagged sat
    const bool   pivot_vr_signal = config_.networkPivotVanishReturnSignal();  // A1: also signal a vanish-return pivot change
    const bool   emit_clock_disc = config_.networkEmitClockDisc();            // A2: clock disc channel + per-constellation bump scoping
    const bool   sticky_pivot    = config_.networkStickyPivot();              // RTA-1: keep the incumbent clock pivot while it stays registered
    const bool   clock_sum_to_zero = config_.networkClockDatumSumToZero();    // anchor-free per-constellation clock gauge (Σδtˢ=0) instead of a pivot pin — no pivot/handover machinery
    const bool   clock_brdc_anchored = config_.networkClockDatumBroadcastAnchored(); // brdc_rts: per-sat broadcast anchors ARE the clock gauge (pre-gate) — no pivot machinery AND no sum-to-zero membership wander
    const bool   clock_signal_basis_hold = config_.networkClockSignalBasisHold(); // W6: HOLD each sat's IF-pin reference pair fixed across epochs (time-consistent δtˢ↔dˢ split) instead of re-selecting from each epoch's coverage — inert without estimate_sat_code_bias
    const bool   cb_time_linked  = config_.networkCodeDatumTimeLinked();      // Card 2 (W6): impose the code-bias datum (IF pin + per-(const,signal) s2z) as a TIME datum — pin the CHANGE of the previous epoch's solved values, not the level to zero; mutually exclusive with clock_signal_basis_hold (load-validated)
    const bool   scoped_handover = config_.networkScopedHandoverResponse();   // RTA-2: per-constellation scoping of the INTERNAL handover response
    const bool   pb_excl_unmatched = config_.networkPhaseBiasExcludeUnmatchedAntenna(); // withhold no-ATX-match (uncalibrated receiver) stations from satellite phase-bias emission
    const bool   emit_gen_time   = config_.networkEmitGenerationTime();       // RT0-4: stamp wall-clock time-of-generation per emitted epoch
    const bool   emit_gen_id     = config_.networkEmitGenerationId();         // RTA-3: stamp a per-run solution-generation id (first-emit epoch time)
    const bool   ser_disc_ctrs   = config_.networkSerializeDiscCounters();    // RTA-4: serialize post-emit disc-counter VALUES on C/B/P records
    const double contract_rebroadcast_s = config_.networkContractRebroadcastS(); // RTA-5: S-contract re-broadcast period (product time, s; 0 = off)
    const bool   clk_runaway_on  = config_.networkClockRunawayCheck();        // RT0-5: per-constellation clock-runaway quarantine
    const double clk_runaway_mag = config_.networkClockRunawayMagM();         // RT0-5: median |clock−global median| ceiling (m)
    const double clk_runaway_rate= config_.networkClockRunawayRateMps();      // RT0-5: median |drift| ceiling (m/s)
    const bool   perconst_on       = config_.networkPerConstWithhold();          // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold
    const int    perconst_min_rows = config_.networkPerConstWithholdMinRows();   // RTA-10: min σ̂0-pool rows for a constellation to be judgeable
    const bool   emit_epoch_status = config_.networkEmitEpochStatusRecord();     // RTA-10: per-epoch H heartbeat in the product file (incl. withheld epochs)
    // Two-state (offset+drift) oscillator clock model (network.clock_model): when on, the
    // satellite clock is carried via the Allan-derived Q model in clock_osc (below) instead of
    // the scalar prev_carry random walk, and a satellite is flagged by the normalized-innovation
    // dynamics-fault test rather than the absolute/jump bounds.
    const bool   clk_model_on   = config_.networkClockModel();
    const double clk_model_q1   = config_.networkClockModelQ1();
    const double clk_model_q2   = config_.networkClockModelQ2();
    const double clk_model_k    = config_.networkClockModelKSigma();
    const double clk_model_r =   // B3: σ_floor² lower bound on the clock-model fault variance S
        config_.networkClockModelMeasStdM() * config_.networkClockModelMeasStdM();
    const double clk_model_max_gap = config_.networkClockModelMaxGapS();  // R11: clock-model carry reseed / drift-reset / C1-ZOH bound (defaults to pl_max_gap_s)
    // Emit C1 (clock rate) from the finite-differenced solved clock WITHOUT the two-state carry
    // (network.emit_clock_rate): decoupled rate estimator (clock_rate_est below) — no prior, no
    // refloat ⇒ byte-identical solution.  Inert when clk_model_on (which already supplies C1).
    const bool   emit_clock_rate_on = config_.networkEmitClockRate() && !clk_model_on;
    const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;  // testing fast-forward (0 = off)

    // L15: run-provenance manifest (network.write_run_manifest).  JSONL — a startup record
    // here (config hash + build + full toggle/knob dump) plus an end-of-run record appended
    // after the completion line.  Hand-emitted (no JSON lib); does NOT read git state (the
    // tree is shared across concurrent sessions — config hash + toggle dump is the honest
    // provenance).  create_directories(log_dir_) already ran in the trace-open scope above.
    const std::filesystem::path manifest_path = log_dir_ / "network_run_manifest.json";
    if (nc.write_run_manifest) {
        auto jesc = [](const std::string& s) {
            std::string o; o.reserve(s.size() + 8);
            for (char c : s) {
                if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
                else if (c == '\n') o += "\\n";
                else if (c == '\r') o += "\\r";
                else o.push_back(c);
            }
            return o;
        };
        uint64_t cfg_hash = 1469598103934665603ULL;   // FNV-1a-64 over the raw config bytes
        {
            std::ifstream cf(config_path_, std::ios::binary);
            char buf[4096];
            while (cf && (cf.read(buf, sizeof buf) || cf.gcount())) {
                const std::streamsize n = cf.gcount();
                for (std::streamsize k = 0; k < n; ++k)
                    cfg_hash = (cfg_hash ^ static_cast<unsigned char>(buf[k])) * 1099511628211ULL;
                if (n < static_cast<std::streamsize>(sizeof buf)) break;
            }
        }
        char utc[32] = "unknown";
        {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
#ifdef _WIN32
            gmtime_s(&tmv, &t);
#else
            gmtime_r(&t, &tmv);
#endif
            std::strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        }
        std::ofstream mf(manifest_path, std::ios::trunc);
        if (mf) {
            mf << "{\"record\":\"startup\","
               << "\"written_utc\":\"" << utc << "\","
               << "\"config_path\":\"" << jesc(config_path_.string()) << "\","
               << fmt::format("\"config_hash_fnv1a64\":\"{:016x}\",", cfg_hash)
               << "\"build\":{\"compiled\":\"" << __DATE__ " " __TIME__ << "\"},"
               << "\"network\":{"
               // RTB-8/P5: bundle provenance — records that the run's toggle set
               // came from the service profile rather than hand-assembly.
               << "\"service_mode\":" << (config_.networkServiceMode() ? "true" : "false") << ','
               << "\"stations\":" << stations.size() << ','
               << "\"clock_datum_station\":\"" << jesc(nc.clock_datum_station) << "\","
               << "\"clock_datum_satellite\":\"" << jesc(nc.clock_datum_satellite) << "\","
               << fmt::format("\"epoch_sync_tolerance_s\":{},", nc.epoch_sync_tolerance_s)
               << "\"product_output_path\":\"" << jesc(nc.product_output_path) << "\","
               << "\"product_flush_per_epoch\":" << (nc.product_flush_per_epoch ? "true" : "false") << ','
               << "\"parallel_stations\":" << (nc.parallel_stations ? "true" : "false") << ','
               << "\"parallel_min_stations\":" << nc.parallel_min_stations << ','
               << "\"parallel_max_threads\":" << nc.parallel_max_threads << ','
               << "\"parallel_reduce\":" << (nc.parallel_reduce ? "true" : "false") << ','
               << "\"parallel_seam\":" << (nc.parallel_seam ? "true" : "false") << ','
               << "\"parallel_carry\":" << (nc.parallel_carry ? "true" : "false") << ','
               << "\"n_threads\":" << n_threads << ','
               << "\"trace_stations\":" << (nc.trace_stations ? "true" : "false") << ','
               << "\"trace_ionosphere\":" << (nc.trace_ionosphere ? "true" : "false") << ','
               << "\"trace_estimability\":" << (nc.trace_estimability ? "true" : "false") << ','
               << "\"trace_slip_events\":" << (nc.trace_slip_events ? "true" : "false") << ','
               << "\"trace_anchor_info\":" << (nc.trace_anchor_info ? "true" : "false") << ','
               << "\"trace_dd_fractional\":" << (nc.trace_dd_fractional ? "true" : "false") << ','
               << "\"trace_flag_rates\":" << (nc.trace_flag_rates ? "true" : "false") << ','
               << "\"trace_epoch_status\":" << (nc.trace_epoch_status ? "true" : "false") << ','
               << "\"trace_events\":" << (nc.trace_events ? "true" : "false") << ','
               << "\"trace_zwd\":" << (nc.trace_zwd ? "true" : "false") << ','
               << "\"trace_clock_full\":" << (nc.trace_clock_full ? "true" : "false") << ','
               << "\"trace_station_residuals\":" << (nc.trace_station_residuals ? "true" : "false") << ','
               // RTB-6/P1: service-observability knobs (health heartbeat + trace rotation).
               << fmt::format("\"health_interval_s\":{},", nc.health_interval_s)
               << fmt::format("\"trace_rotate_mb\":{},", nc.trace_rotate_mb)
               << "\"trace_rotate_keep\":" << nc.trace_rotate_keep << ','
               // RTB-7: startup quality ramp + state snapshot / warm resume.
               << fmt::format("\"warmup_ura_floor_m\":{},", nc.warmup_ura_floor_m)
               << "\"warmup_epochs\":" << nc.warmup_epochs << ','
               << "\"state_snapshot_path\":\"" << jesc(nc.state_snapshot_path) << "\","
               << fmt::format("\"state_snapshot_interval_s\":{},", nc.state_snapshot_interval_s)
               << "\"serialize_sbasis_contract\":" << (nc.serialize_sbasis_contract ? "true" : "false") << ','
               << "\"estimate_sat_code_bias\":" << (config_.estimateNetworkSatCodeBias() ? "true" : "false") << ','
               << "\"estimate_iono_sh\":" << (config_.estimateNetworkIonoSh() ? "true" : "false") << ','
               << fmt::format("\"iono_constraint_deweight_factor\":{},",
                              config_.getIonoConstraintDeweightConfig().factor)
               << "\"iono_constraint_deweight_adaptive\":"
                   << (config_.getIonoConstraintDeweightConfig().adaptive ? "true" : "false") << ','
               << "\"iono_constraint_deweight_stations\":["
                   << [&] {
                          std::string s; bool f = true;
                          for (const auto& nm : config_.getIonoConstraintDeweightConfig().stations) {
                              if (!f) s += ','; s += '"'; s += jesc(nm); s += '"'; f = false;
                          }
                          return s;
                      }()
                   << "],"
               << "\"phase_leveling\":" << (pl_on ? "true" : "false") << ','
               << fmt::format("\"phase_leveling_q_amb\":{},", pl_q_amb)
               << "\"phase_leveling_warmup_epochs\":" << pl_warmup << ','
               << fmt::format("\"phase_leveling_max_gap_s\":{},", pl_max_gap_s)
               << fmt::format("\"phase_leveling_var_floor_m2\":{},", pl_var_floor)
               << fmt::format("\"shared_carry_var_floor_pb_m2\":{},", pb_var_floor)
               << fmt::format("\"shared_carry_var_floor_cb_m2\":{},", cb_var_floor)
               << "\"carry_zwd\":" << (zwd_on ? "true" : "false") << ','
               << fmt::format("\"zwd_carry_q\":{},", zwd_q)
               << "\"raim\":" << (raim_on ? "true" : "false") << ','
               << fmt::format("\"raim_k_sigma\":{},", raim_k)
               << "\"raim_min_stations\":" << raim_min_sta << ','
               << fmt::format("\"raim_sigma0_trigger\":{},", es_sigma0_trigger)
               << fmt::format("\"raim_withhold_sigma0\":{},", raim_withhold)
               << "\"per_const_withhold\":" << (perconst_on ? "true" : "false") << ','
               << "\"per_const_withhold_min_rows\":" << perconst_min_rows << ','
               << "\"raim_redundancy_normalize\":" << (raim_redun ? "true" : "false") << ','
               << "\"raim_attribution\":" << (config_.networkRaimAttribution() ? "true" : "false") << ','
               << "\"raim_within_epoch\":" << (config_.networkRaimWithinEpoch() ? "true" : "false") << ','
               << "\"clock_monitor\":" << (clk_mon_on ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_abs_max_m\":{},", clk_mon_abs)
               << fmt::format("\"clock_monitor_jump_max_m\":{},", clk_mon_jump)
               << "\"clock_monitor_couple_raim\":" << (config_.networkClockMonitorCoupleRaim() ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_reinit_sigma_m\":{},", clk_mon_reinit_sig)
               << "\"clock_model\":" << (clk_model_on ? "true" : "false") << ','
               << fmt::format("\"clock_model_q1\":{},", clk_model_q1)
               << fmt::format("\"clock_model_q2\":{},", clk_model_q2)
               << fmt::format("\"clock_model_k_sigma\":{},", clk_model_k)
               << fmt::format("\"clock_model_meas_std_m\":{},", config_.networkClockModelMeasStdM())
               << fmt::format("\"clock_model_max_gap_s\":{},", clk_model_max_gap)
               << "\"emit_clock_rate\":" << (config_.networkEmitClockRate() ? "true" : "false") << ','
               << "\"emit_ura\":" << (ura_on ? "true" : "false") << ','
               << fmt::format("\"ura_ewma_alpha\":{},", ura_alpha)
               << "\"consume_ura\":" << (config_.networkConsumeUra() ? "true" : "false") << ','
               << "\"ura_retro_bump\":" << (ura_retro ? "true" : "false") << ','
               << fmt::format("\"ura_refloat_inflate\":{},", ura_refloat_inflate)
               << fmt::format("\"ura_staleness_q2\":{},", config_.networkUraStalenessQ2())
               << "\"pivot_vanish_return_signal\":" << (pivot_vr_signal ? "true" : "false") << ','
               << "\"emit_clock_disc\":" << (emit_clock_disc ? "true" : "false") << ','
               << "\"sticky_pivot\":" << (sticky_pivot ? "true" : "false") << ','
               << "\"carry_invariant\":" << (config_.networkCarryInvariant() ? "true" : "false") << ','
               << fmt::format("\"carry_invariant_qz\":{},", config_.networkCarryInvariantQz())
               << "\"carry_invariant_emit_rebase\":"
               << (config_.networkCarryInvariantEmitRebase() ? "true" : "false") << ','
               << "\"carry_invariant_honesty\":"
               << (config_.networkCarryInvariantHonesty() ? "true" : "false") << ','
               << "\"carry_invariant_innov_gate\":"
               << (config_.networkCarryInvariantInnovGate() ? "true" : "false") << ','
               << fmt::format("\"carry_invariant_innov_k\":{},", config_.networkCarryInvariantInnovK())
               << "\"carry_invariant_innov_calibrate\":"
               << (config_.networkCarryInvariantInnovCalibrate() ? "true" : "false") << ','
               << "\"mw_wl_enable\":" << (config_.networkMwWlSeed() ? "true" : "false") << ','   // M5 card WL-1
               << fmt::format("\"mw_wl_min_arc_epochs\":{},", config_.getNetworkConfig().mw_wl.min_arc_epochs)
               << fmt::format("\"mw_wl_sigma_floor_cyc\":{},", config_.getNetworkConfig().mw_wl.sigma_floor_cyc)
               << fmt::format("\"mw_wl_max_rows_per_epoch\":{},", config_.getNetworkConfig().mw_wl.max_rows_per_epoch)
               << "\"emit_pb_filter\":" << (config_.networkEmitPbFilter() ? "true" : "false") << ','
               << fmt::format("\"emit_pb_filter_q\":{},", config_.networkEmitPbFilterQ())
               << fmt::format("\"emit_pb_filter_jump_k\":{},", config_.networkEmitPbFilterJumpK())
               << fmt::format("\"emit_pb_filter_meas_sigma_m\":{},", config_.networkEmitPbFilterMeasSigmaM())
               << fmt::format("\"emit_pb_filter_max_gap_s\":{},", config_.networkEmitPbFilterMaxGapS())
               << fmt::format("\"emit_pb_filter_window_epochs\":{},", config_.networkEmitPbFilterWindowEpochs())
               << "\"emit_pb_filter_emit_sigma\":"
               << (config_.networkEmitPbFilterEmitSigma() ? "true" : "false") << ','
               << "\"emit_pb_filter_gate_v2\":"   // PB-gate-v2 merge point (config echo)
               << (config_.networkEmitPbFilterGateV2() ? "true" : "false") << ','
               << "\"clock_signal_basis_hold\":" << (clock_signal_basis_hold ? "true" : "false") << ','
               << "\"scoped_handover_response\":" << (scoped_handover ? "true" : "false") << ','
               << "\"phase_bias_exclude_unmatched_antenna\":" << (pb_excl_unmatched ? "true" : "false") << ','
               << "\"exclude_unmodeled_antenna_stations\":"
               << (config_.networkExcludeUnmodeledAntennaStations() ? "true" : "false") << ','
               << "\"emit_generation_id\":" << (emit_gen_id ? "true" : "false") << ','
               << "\"serialize_disc_counters\":" << (ser_disc_ctrs ? "true" : "false") << ','
               << "\"emit_epoch_status_record\":" << (emit_epoch_status ? "true" : "false") << ','
               << fmt::format("\"contract_rebroadcast_s\":{},", contract_rebroadcast_s)
               << fmt::format("\"emitter_staleness_horizon_s\":{},",
                              config_.networkEmitterStalenessHorizonS())
               << fmt::format("\"start_offset_hours\":{},", config_.networkStartOffsetHours())
               << fmt::format("\"process_noise_sat_clock\":{},", q_clock)
               << fmt::format("\"process_noise_sat_phase_bias\":{},", q_pb)
               << fmt::format("\"process_noise_sat_code_bias\":{}", q_cb)
               << "}}\n";
            mf.close();
        } else {
            log->warn("Network: failed to open run-manifest {}", manifest_path.string());
        }
    }
    double prev_tow = -1.0;
    int    prev_week = -1;
    int    iod = 0;
    // RTA-3: per-run solution-generation id, minted at the FIRST emit from that
    // epoch's GPS time (deterministic under replay — the id is serialized per
    // epoch; different across live restarts, which first-emit later).  0 = unset.
    uint32_t generation_id = 0;
    // RTA-5: product-time (s) of the last written S contract line (the deferred v2
    // header counts as the first); -1 until the contract writer opens.
    double last_contract_emit_s = -1.0;
    size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
    // RTB-6: operator health-record state (network.health_interval_s > 0).
    double last_health_write_s = -1.0;   // content time of the last health row (s)
    double last_emit_content_s = -1.0;   // content time of the last EMITTED epoch (s)
    int    last_n_present      = 0;      // stations present in the most recent processed epoch
    int    last_n_clock_sats   = 0;      // registered clock sats in the most recent processed epoch (P4b)
    // P4(b): ms_per_epoch_window anchors — wall ms + loop-epoch count as of the
    // PREVIOUS health row, so each row reports the mean over ITS OWN window (the
    // operationally useful "is it slowing down NOW" signal), not a cumulative
    // run mean mislabeled "rolling".
    double health_win_ms_anchor     = 0.0;
    size_t health_win_epochs_anchor = 0;
    size_t n_sigma0_withheld = 0;   // A4/R8: epochs skipped specifically by the σ̂0 withhold ceiling (subset of n_skipped)
    // Prioritized-datum drop-and-heal observability: epochs where the gate threw and the
    // near-null phase-bias soft-anchor recovered it, and total columns anchored.  A run
    // that heals every epoch "succeeds" silently otherwise (the heal-success log is debug).
    size_t n_healed_epochs = 0, n_healed_cols = 0;
    // Card 1b (network.carry_coupling_joint_whiten): #station-epochs whose joint
    // coupling-row covariance was non-SPD after the ridge retry and fell back to the
    // Card-1 per-row scalar marginalization.  Warned once at run end.
    size_t n_cpl_joint_fallback = 0;
    // Card 1/1d observability: run-total coupling-carry volume — harvest-side stash
    // counts (non-empty regression vs EMPTY = plain-diagonal degrade vs outright
    // harvest failure) and inject-side term counts (present / departed / κ-shrunk).
    // A run whose coupling carry silently degrades to diagonal rows (or never runs)
    // is indistinguishable from a healthy one without these — the Card-1b/1d
    // byte-identical-A/B trap.  Logged once at run end when the card is armed.
    size_t n_cpl_stash = 0, n_cpl_stash_empty = 0, n_cpl_harvest_fail = 0;
    long long n_cpl_terms_present = 0, n_cpl_terms_departed = 0, n_cpl_terms_shrunk = 0;
    int n_cpl_row_dump = 12;   // Card 1d debug: one-shot assembled coupling-row dump budget
    int n_cpl_solve_dump = 8;  // Card 1d debug: one-shot full-precision solve-checksum budget
    // Coverage-threshold union gate (network.phase_datum_min_stations): epochs on
    // which >=1 thin column was excluded, and the cumulative column-epoch count.
    size_t n_gated_epochs = 0, n_gated_key_epochs = 0;
    size_t n_perconst_partial_epochs = 0;     // RTA-10: epochs published PARTIALLY (σ̂0 carve-out pruned >=1 constellation)
    size_t n_perconst_diffuse_published = 0;  // RTA-10: epochs published in FULL although the global σ̂0 tripped (every constellation individually clean — the diffuse cross-constellation edge of the per-const approximation)

    // --- B8 empirical-alpha flag-rate counters (network.trace_flag_rates) -----
    // Pure observation: {opportunities, flags} per QC tier, summarized at end of run
    // as realized rate vs the implied one-sided Gaussian α for the configured k.  Never
    // fed back into any decision; every increment is guarded so OFF is byte-identical.
    const bool trace_flag_rates = nc.trace_flag_rates;
    struct FlagRate { long opp = 0, flag = 0; };
    FlagRate fr_if_slip;    // T1 per (station,sat,pair) IF-phase sample   k=if_phase_td_k_mad
    FlagRate fr_we_trigger; // T2 per warm within-epoch epoch: σ̂0_first>trig k=sigma0_trigger
    FlagRate fr_raim_cell;  // T3 per (sat,station) standardized residual   k=raim_k
    FlagRate fr_raim_sat;   // T3 consensus per spiked sat (>=min_sta)       k=raim_k
    FlagRate fr_attrib;     // T4 per median-polish SAT column: eff/σ̂0>k & σ̂0>trigger (B8/P1-4)
    FlagRate fr_clk_model;  // T5 per sat clock-model NIS (own, !ext)        k=clock_model_k
    FlagRate fr_clk_mon;    // T6 per sat clock-monitor abs/jump (own, !ext) absolute bounds
    double max_dur_anchor_s = 0.0;
    bool   max_dur_anchored = false;
    int    spectrum_diag_budget = 6;    // estimability spectrum dump: first N gate evaluations (was one-shot; a rows-epoch needs a later firing than epoch 1)

    // Single source of truth for "is (sat,signal) at station i a gauge-pinned
    // ambiguity this epoch?" — the predicate that replaces the scattered is_pivot
    // gauge tests.  Legacy (network.phase_datum_prioritized OFF): membership ==
    // station is THE pivot, which pins the whole shared_phase_keys set, so this
    // returns exactly is_pivot and every rewritten site is byte-identical.
    // Prioritized: membership == this "sat_signal" key is in station i's elected
    // pinned_amb_keys (the anchor for that gauge freedom, its pin folded into its
    // OWN local block).  Key form matches the pin fold (parts->satellite + "_" +
    // parts->signal) and the ambiguity-state key.
    // Fix A (network.phase_datum_ensemble): the anchor-free sum-to-zero phase-bias
    // datum REUSES all the prioritized SPOF-cure machinery (no pivot-first, every
    // station carries its arcs, per-(sat,signal) shared set) — it differs only in
    // the gauge REALIZATION (a kernel sum-to-zero fold instead of a per-(sat,signal)
    // anchor pin) — so drive the prioritized code paths under EITHER mode and
    // diverge only at the anchor election (skipped under ensemble) and the pin fold
    // (naturally inert: no station gets a pinned_amb_key under ensemble).
    const bool phase_datum_ensemble    = config_.networkPhaseDatumEnsemble();
    // Card 3: the Eq-34 / de Jonge single-receiver-chain datum.  Like the ensemble
    // (Fix A), it drives ALL the prioritized SPOF-cure machinery (no pivot-first,
    // every station carries its arcs, union shared set) and diverges only at the
    // election (a spanning-tree chain election replacing the anchor election) and
    // the pin fold (per-tree-edge ambiguity pins with a target + a datum-receiver
    // RPB pin).  Mutually exclusive with the ensemble (enforced at config load).
    const bool phase_datum_eq34        = config_.networkPhaseDatumEq34();
    // Card 4 (M1) C3b: the DD cycle-basis invariant carry gate (declared early so the
    // superseded eq34 hygiene knobs below can gate on it).  Requires phase_datum_eq34.
    const bool carry_invariant         = phase_datum_eq34 && config_.networkCarryInvariant();
    // Card 3: information weight (1/σ²) of the per-epoch zero-mean RPB soft prior
    // (the station-local floor for the near-null RPB split); 0 = disabled.
    const double eq34_rpb_prior_sigma = config_.networkPhaseDatumEq34RpbPriorSigmaM();
    const double eq34_rpb_prior_w = (phase_datum_eq34 && eq34_rpb_prior_sigma > 0.0)
        ? 1.0 / (eq34_rpb_prior_sigma * eq34_rpb_prior_sigma) : 0.0;
    // Card 3: q (m²/s) of the local RPB time-linking carry; 0 = v1 fresh re-estimation.
    // Card 4 supersedes the local δ_{r,σ} time-link (the DD carry is the phase time-link);
    // under carry_invariant the RPB re-estimates fresh (δ cancels in the gauge-free DD).
    const double eq34_rpb_q = (phase_datum_eq34 && !carry_invariant)
                                  ? config_.networkPhaseDatumEq34RpbQ() : 0.0;
    // Card 3 EXPERIMENTAL: S-transform carries on pin displacement (OFF = erase-based hygiene).
    // Card 4 supersedes the carry S-transform hygiene (the datum is re-realized freely).
    const bool eq34_shift_on = phase_datum_eq34 && config_.networkPhaseDatumEq34CarryShift()
                             && !carry_invariant;
    // Card 3 (I4): grow NEW spanning-tree edges by arc longevity (OFF = legacy lexical growth).
    const bool eq34_longevity = phase_datum_eq34 && config_.networkPhaseDatumEq34LongevityElection();
    // Card 3 (I2): retain sticky-tree identity across carry-alive row gaps (OFF = wholesale replace).
    const bool eq34_extend_only = phase_datum_eq34 && config_.networkPhaseDatumEq34ExtendOnly();
    // Card 4 (M1) C3b: the DD cycle-basis invariant carry driver + its persistent state
    // (the carried z-store lives across epochs).  SUPERSEDES the S-dependent PhaseBias/
    // local/rpb carries + eq34 pin-round/shift hygiene for phase keys (bypassed below);
    // GLONASS (FDMA) excluded.  OFF = no z-store, all folds gated (byte-identical).
    NetworkInvariantCarryDriver::Config carry_cfg;
    carry_cfg.qz = config_.networkCarryInvariantQz();
    // Card 4 (M1) D5: the estimation-side fine-slip innovation gate (spec D5 Def-1) — the
    // Card-4-native replacement for the RAIM fine-slip layer the invariant bypass empties.
    // Requires carry_invariant (config-rejected otherwise); OFF = byte-identical.
    const bool carry_innov_gate = carry_invariant && config_.networkCarryInvariantInnovGate();
    carry_cfg.innov_gate = carry_innov_gate;
    carry_cfg.innov_k    = config_.networkCarryInvariantInnovK();
    // D5 v2: LS-VCE calibration of the gate threshold (σ̂₀²); requires innov_gate (config-rejected).
    carry_cfg.innov_calibrate        = carry_innov_gate && config_.networkCarryInvariantInnovCalibrate();
    carry_cfg.innov_calibrate_window = config_.networkCarryInvariantInnovCalibrateWindow();
    // M5 card WL-3 (docs/SPEC_lane_carry_card.md): the lane-coordinate carry.  The loader
    // enforces carry_invariant + mw_wl.ewl_enable.  lane_joint discriminates the LANE
    // shape (joint 2K pair blocks, lane prior/q, lane measurement rows, fold-on-change
    // fixing) from the §6.1 EQUIVALENCE shape (all lane features at their isotropic/off
    // sentinels ⇒ split per-band emission, information-identical to the per-band carry).
    const auto& lane_cfg = config_.getNetworkConfig().lane_carry;
    const bool lane_on = carry_invariant && lane_cfg.enable;
    const bool lane_joint = lane_on
        && (lane_cfg.q_wl_scale != 1.0 || lane_cfg.sigma_wl_fresh_cyc > 0.0
            || lane_cfg.lane_meas || lane_cfg.bridge_meas);
    if (lane_on) {
        carry_cfg.q_wl_scale         = lane_cfg.q_wl_scale;
        carry_cfg.sigma_wl_fresh_cyc = lane_cfg.sigma_wl_fresh_cyc;
        carry_cfg.lane_joint         = lane_joint;
    }
    NetworkInvariantCarryDriver carry_driver(carry_cfg);
    // M5 card WL-1: the MW-seeded wide-lane pseudo-observation layer (docs/SPEC_mw_wl_card.md).
    // Rides the carry driver's chord basis, so it requires carry_invariant (config-rejected
    // otherwise).  OFF (mw_wl.enable=false) ⇒ NetworkMwWlLayer::build returns no blocks and the
    // per-station MW detectors are not built for it, so the producer is byte-identical.
    const bool mw_wl_on = carry_invariant && config_.networkMwWlSeed();
    // M5 card WL-2 (docs/SPEC_ewl_wl_cascade_card.md §3.1): when the EWL lane is armed the
    // constellation's THIRD band (the EWL pair's uncarried leg — GPS L5, Galileo E5b) is
    // deliberately EXCLUDED from the invariant-carry driver so that lane acquires no unearned
    // stiff prior (the F6 wall) and its EWL rows win cleanly.  The band stays in the eq34
    // election/datum (shared_phase_keys/pins are built upstream, untouched).  OFF ⇒ the third
    // band is carried exactly as today (byte-identical).  v1 = GPS+Galileo (GLONASS never carried).
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    const bool ewl_carry_exclude = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1 (docs/SPEC_uwl_card.md §3.1): under the armed card the uncarried band
    // comes from the per-constellation lane-tier table (Galileo's top band is selectable:
    // '7' = E5b λ 9.768 m, '8' = E5 AltBOC λ 19.537 m).  uwl_enable=false keeps the legacy
    // digits verbatim — the §6.1(b) gate: arming a constellation without the card changes
    // NOTHING.  For GPS (and Galileo at the default '7') the table reproduces the legacy
    // digits, so armed GPS-only runs stay bit-identical too.
    const bool uwl_excl_on = ewl_carry_exclude && nmw.uwl_enable;
    const char uwl_gal_top = nmw.uwl_gal_top_band;
    const bool uwl_dual    = nmw.uwl_dual_top;   // increment 5: BOTH {b_top, b_top2} uncarried
    auto isUncarriedThirdBand = [uwl_excl_on, uwl_gal_top, uwl_dual](
                                    char cons, const std::string& signal) -> bool {
        const char band = signal.size() >= 2 ? signal[1] : '\0';   // "L5Q" → '5'
        if (uwl_excl_on) {
            const auto tier = mwLaneTierOf(cons, uwl_gal_top, false, uwl_dual);
            return tier && (band == tier->b_top
                            || (tier->b_top2 != '\0' && band == tier->b_top2));
        }
        switch (cons) {
            case 'G': return band == '5';   // GPS  EWL = (L2,L5) ⇒ L5 uncarried
            case 'E': return band == '7';   // Galileo EWL = (E5b,E5a) ⇒ E5b(7) uncarried
            default:  return false;         // BDS/QZSS deferred; GLONASS excluded from the carry anyway
        }
    };
    NetworkMwWlLayer::Config mw_wl_cfg;
    mw_wl_cfg.enable             = mw_wl_on;
    mw_wl_cfg.min_arc_epochs     = config_.getNetworkConfig().mw_wl.min_arc_epochs;
    mw_wl_cfg.sigma_floor_cyc    = config_.getNetworkConfig().mw_wl.sigma_floor_cyc;
    mw_wl_cfg.max_rows_per_epoch = config_.getNetworkConfig().mw_wl.max_rows_per_epoch;
    mw_wl_cfg.debug_r_scale      = config_.getNetworkConfig().mw_wl.debug_r_scale;   // F6 probe (diagnostic)
    NetworkMwWlLayer mw_wl_layer(mw_wl_cfg);
    if (mw_wl_on && mw_wl_cfg.debug_r_scale != 1.0)
        log->warn("Network (M5/WL-1): network.mw_wl.debug_r_scale = {} — DIAGNOSTIC probe, the WL "
                  "rows' Pz is scaled (not honest MW variance).  Do NOT grade this run.",
                  mw_wl_cfg.debug_r_scale);
    // M5 card WL-2 EWL lane (SPEC_ewl_wl_cascade_card.md §3.2): the SAME layer, run on the
    // (band2, band5) pair with FFR statistic-gated fixing.  The third band is uncarried
    // (excluded above) so these rows win with no F6 fight; a passed FFR folds an integer-hard
    // row.  Rides the primary-sig1 cycle basis exactly as WL-1.
    const bool ewl_on = carry_invariant && nmw.ewl_enable;
    // M5 card UWL-1: the per-constellation top tier.  uwl_on generalizes the EWL lane's
    // hardcoded GPS band digits to the lane-tier table (Galileo UWL (E5b|E5,E5a)); the
    // cold fix additionally admits n=1 UNLAGGED statistics to the DECISION on tiers with
    // λ ≥ uwl_cold_min_lambda_m (SPEC §3.3 — GPS's 5.861 m EWL deliberately excluded).
    const bool uwl_on      = ewl_on && nmw.uwl_enable;
    const bool uwl_cold_on = uwl_on && nmw.uwl_cold_fix;
    // E6 mid-ladder bridge hop (census-motivated): chain the WL decision through the
    // Galileo (E6,E5a) λ 2.931 m rung — re-rounding at λ_MID strips the top hop's
    // λ-scale phase/iono amplification before it reaches the 0.751 m WL statistic.
    const bool uwl_mid_on  = uwl_on && nmw.uwl_mid_enable;
    // Increment 5: per-cycle preferred('8')/fallback('7') Galileo top rung + the
    // untaken-sibling closure statistic (2·N(8,5) = N(7,5)); hard gate default OFF
    // (measure the residual distribution first — SPEC reconciliation note 2).
    const bool uwl_dual_on = uwl_on && nmw.uwl_dual_top;
    const bool uwl_sib_gate_on = uwl_dual_on && nmw.uwl_sib_gate;
    // v2 census recovery: minority receiver-family primary pairs get their OWN lane
    // pair groups (admissible census-pair SET instead of the single majority pair).
    const bool uwl_family_on = uwl_on && lane_on && nmw.uwl_family_pairs;
    NetworkMwWlLayer::Config ewl_cfg;
    ewl_cfg.enable              = ewl_on;
    ewl_cfg.min_arc_epochs      = nmw.ewl_min_arc_epochs;
    ewl_cfg.sigma_floor_cyc     = nmw.sigma_floor_cyc;
    ewl_cfg.max_rows_per_epoch  = nmw.max_rows_per_epoch;
    ewl_cfg.ffr_fix             = true;
    ewl_cfg.fix_sigma_cyc       = nmw.ewl_fix_sigma_cyc;
    ewl_cfg.fix_frac_cyc        = nmw.ewl_fix_frac_cyc;
    ewl_cfg.fix_weight_sigma_cyc= nmw.fix_weight_sigma_cyc;
    ewl_cfg.max_abs_xlin_m      = nmw.lane_max_abs_xlin_m;
    // UWL-1: one statistic-layer instance per TABLED constellation when the card is armed
    // (per-cons telemetry — the F-A dump taught us pooled lines hide which lane is
    // storming); a single filterless instance otherwise (the legacy shape — for a
    // GPS-only run the 'G'-filtered instance is bit-identical to it, the filter never
    // fires).  An armed constellation that decides 0 cycles prints a loud 0-line
    // (invariant #4), never silence.
    struct EwlLane {
        char cons;                        ///< cons filter (0 = all — the legacy single lane).
        char top;                         ///< UWL-1: the instance's top-tier band digit ('\0' = legacy single lane).
        NetworkMwWlLayer layer;
        NetworkMwWlLayer::Counters run, epoch;
    };
    std::vector<EwlLane> ewl_lanes;
    if (ewl_on) {
        if (uwl_on) {
            for (const char cons : { 'G', 'E' }) {
                const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                               nmw.uwl_mid_enable, nmw.uwl_dual_top);
                if (!tier) continue;
                NetworkMwWlLayer::Config cfg = ewl_cfg;
                cfg.cons_filter = cons;
                // Increment 5: the PREFERRED rung's instance runs FIRST (its ewl_fixed
                // entries precede the fallback's, so the walk's dedupe keeps the wider-λ
                // decision); the fallback instance decides only what the preferred one
                // did not (a cycle with any preferred-band-less arc dies there as
                // skip_missing_band — the per-cycle "where tracked" resolution).
                ewl_lanes.push_back(EwlLane{ cons, tier->b_top, NetworkMwWlLayer(cfg), {}, {} });
                if (tier->b_top2 != '\0')
                    ewl_lanes.push_back(EwlLane{ cons, tier->b_top2,
                                                 NetworkMwWlLayer(cfg), {}, {} });
            }
        } else {
            ewl_lanes.push_back(EwlLane{ '\0', '\0', NetworkMwWlLayer(ewl_cfg), {}, {} });
        }
    }
    // M5 card WL-2 WL lane (SPEC §3.3/§3.4): the phase-bridge WL decision + the NL-preserving
    // inject.  The bridge path needs the EWL integers, so it runs only when BOTH masters are on;
    // wl_inject alone runs the MW(1,2) fallback path (§3.5, deferred to a later increment — v1
    // requires the bridge, else no WL decision is produced and the lane is inert but armed).
    // WL-3 supersedes the WL-2 hold/inject walk at runtime (loader-warned): under
    // lane_carry the decided integers become fold-on-change LANE fixes on the pair
    // blocks and the inject primitive survives only as the pair-block fallback.
    const bool wl_inject_on = ewl_on && nmw.wl_inject_enable && !lane_on;
    // WL-3 per-epoch lane measurement sources (§3.2); the FFR fix walk runs whenever
    // the joint lane shape is armed (fold-on-change is the §3.3 core, no separate knob).
    const bool lane_meas_on   = lane_on && lane_joint && lane_cfg.lane_meas;
    const bool lane_bridge_on = lane_on && lane_joint && lane_cfg.bridge_meas;
    const bool lane_fix_on    = lane_on && lane_joint;
    // One decided WL inject: the two bands' carry groups + the shared chord ArcKey + the integer
    // target + the per-band linearization DD sums (F5 correction frame) + wavelengths.  Computed
    // in buildCarryDdPriors from DETECTOR statistics (§3.0.1 decision source), APPLIED post-solve
    // against the fresh posterior (so the preserved NL sum is this epoch's).  Refreshed each build
    // (the last before the accepted solve wins, mirroring ewl_epoch).
    struct WlInjectPlan {
        int    group_l1{-1}, group_l2{-1};
        ArcKey chord{0, 0};
        double w{0.0};                 ///< integer WL DD target ∇ΔN_WL (cyc).
        double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
        double lambda1{0.0}, lambda2{0.0};
        bool   pair{false};            ///< WL-3: group_l1 (== group_l2) is a PAIR group — read/edit via the pair accessors (the fallback path; ~0 in steady state).
        char   cons{'\0'};             ///< UWL-1: the chord's constellation (per-cons inject telemetry).
    };
    std::vector<WlInjectPlan> wl_inject_epoch;   // per-build; consumed post-installPosterior.
    // F-A attribution dump (network.mw_wl.d5_dump, log-only): adjacency tags for each D5
    // flag — the (station, sat) pairs an EWL-FIXED cycle touched this build, the chords of
    // hold-folded cycles this build, and the chords injected LAST epoch (an inject's
    // residual tension lands in the NEXT install; the injected chord itself is exempt, its
    // component siblings are not).  All rebuilt per accepted build; empty unless d5_dump.
    const bool mw_d5_dump = (ewl_on || wl_inject_on || lane_on) && nmw.d5_dump;
    // F-B lane trace (network.mw_wl.lane_trace, log-only): per epoch, the carried WL lane
    // value of EVERY primary cycle whose chord is carried in both bands — the constrained
    // (held/injected) and unconstrained populations in one run, so the injected-cycle circR
    // is finally gradable (scripts/grade_lane_trace.py).  Entries built per accepted build;
    // written post-install against the FRESH posterior.
    const bool mw_lane_trace = (wl_inject_on || lane_fix_on) && nmw.lane_trace;
    const bool mw_tagsets    = mw_d5_dump || mw_lane_trace;   // hold/inject tag sets feed both
    struct LaneTraceEntry {
        ArcKey chord{0, 0};
        int    g1{-1}, g2{-1};          ///< the two bands' carry group ids.
        double z_lin1{0.0}, z_lin2{0.0};///< Σ coeff·x_lin per band over the cycle (m).
        double lam1{0.0}, lam2{0.0};
        std::string sat;                ///< chord satellite (chord.u = station index).
    };
    std::vector<LaneTraceEntry> lane_trace_epoch;   // per-build; consumed post-install.
    bool lane_trace_header_written = false;
    std::set<std::pair<int, std::string>> d5_ewl_stasat;
    std::set<ArcKey> d5_hold_chords;
    std::set<ArcKey> d5_inject_chords_prev, d5_inject_chords_now;
    bool d5_dump_header_written = false;
    struct WlCounters {
        long long rows_seen{0};          ///< FFR-fixed EWL rows inspected by the bridge.
        long long cycles_decided{0};     ///< bridge cycles that passed the WL FFR gate.
        long long holds_folded{0};       ///< hold rows folded (carry already lane-consistent).
        long long injects{0};            ///< injects applied post-solve (carry moved to the integer).
        double    inject_abs_sum{0.0};   ///< Σ |w − carried lane| at inject (median-ish |Δ| via /injects).
        long long skip_bridge_missing{0};///< an arc lacked a mature GF_b bridge arc.
        long long skip_no_iono{0};       ///< §3.4 v2: an arc lacked a slant-iono estimate ⇒ can't de-bias.
        long long skip_bridge_wide{0};   ///< bridge σ_row (incl. iono variance) > bridge_max_sigma_cyc.
        long long skip_not_fixed{0};     ///< bridge FFR failed (σ or |frac|).
        long long skip_closure{0};       ///< MW(1,2) cross-check disagreed with the bridge integer.
        long long skip_quarantine{0};    ///< an arc's |x_lin| exceeded lane_max_abs_xlin_m.
        long long skip_basis{0};         ///< inject: the chord is not 1:1 across BOTH bands' carry groups.
        double    sum_bridge_R{0.0};     ///< Σ decided rows' bridge R (cyc²) for the mean-R report.
        // --- WL-3 lane-carry telemetry (SPEC_lane_carry_card.md §3.2/§3.3). ---
        long long lane_mw_rows{0};       ///< per-epoch MW(1,2) lane increments folded into pair blocks.
        long long lane_bridge_rows{0};   ///< per-epoch bridge lane increments folded (EWL-fixed cycles).
        long long lane_fix_folded{0};    ///< fold-on-change FFR fixes FOLDED (decision new/changed).
        long long lane_fix_unchanged{0}; ///< FFR decisions seen with an unchanged integer (no fold — the F-A once-only discipline).
        long long lane_skip_meas{0};     ///< lane-increment cycles skipped (immature/stale/mixed-frame/slipped arc/quarantine).
        // --- UWL-1 cold-fix telemetry (SPEC_uwl_card.md §3.3). ---
        long long cold_bridge_cycles{0}; ///< decided cycles whose bridge read was the UNLAGGED cold serve (post-slip early re-pin).
        // --- E6 mid-ladder hop telemetry (uwl_mid_enable). ---
        long long mid_decided{0};        ///< decided cycles whose WL statistic came through the two-hop mid chain.
        long long mid_fallback{0};       ///< cycles with a live mid chain whose mid FFR failed ⇒ fell back to the direct hop.
        long long mid_closure{0};        ///< mid integers blocked by the MW(6,5) closure ⇒ fell back to the direct hop.
        long long mid_disagree{0};       ///< two-hop and direct integers BOTH FFR-credible yet different ⇒ cycle skipped (wrong-integer backstop).
        // --- Increment 5: dual-top tier telemetry (uwl_dual_top). ---
        long long top2_decided{0};       ///< decided cycles that came through the FALLBACK rung ('7' — the preferred '8' could not decide them).
        long long dual_dedupe{0};        ///< fallback-rung entries skipped because the preferred rung already decided the (block,row) this build.
        long long sib_checked{0};        ///< decided cycles whose untaken-sibling MW statistic was measurable across every arc.
        long long sib_unavail{0};        ///< decided cycles whose sibling statistic was NOT measurable (an arc lacks the sibling band / MW arc).
        long long sib_int_off{0};        ///< checked cycles whose sibling residual rounds to a NONZERO integer (wrong-integer signal — or a convention offset; measure first).
        long long sib_blocked{0};        ///< decisions blocked by the armed sibling gate (uwl_sib_gate; the cycle falls to the fallback rung the same epoch).
        double    sib_abs_frac_sum{0.0}; ///< Σ |frac(sibling residual)| over checked cycles (mean = the convention-offset dial: ~0 clean, ~0.5 half-cycle convention).
    };
    WlCounters wl_run, wl_epoch;
    // UWL-1: per-constellation WL-walk tallies when the card is armed (pooled lines hide
    // which lane is storming).  wl_run stays the aggregate — the per-epoch cons tallies
    // fold into BOTH at the accepted-install accumulation.
    std::map<char, WlCounters> wl_cons_epoch, wl_cons_run;
    // Accepted-install accumulation helper (one place — the field list bit-rots in two).
    auto wlAccumulate = [](WlCounters& a, const WlCounters& b) {
        a.rows_seen           += b.rows_seen;
        a.cycles_decided      += b.cycles_decided;
        a.holds_folded        += b.holds_folded;
        a.injects             += b.injects;
        a.inject_abs_sum      += b.inject_abs_sum;
        a.skip_bridge_missing += b.skip_bridge_missing;
        a.skip_no_iono        += b.skip_no_iono;
        a.skip_bridge_wide    += b.skip_bridge_wide;
        a.skip_not_fixed      += b.skip_not_fixed;
        a.skip_closure        += b.skip_closure;
        a.skip_quarantine     += b.skip_quarantine;
        a.skip_basis          += b.skip_basis;
        a.sum_bridge_R        += b.sum_bridge_R;
        a.lane_mw_rows        += b.lane_mw_rows;
        a.lane_bridge_rows    += b.lane_bridge_rows;
        a.lane_fix_folded     += b.lane_fix_folded;
        a.lane_fix_unchanged  += b.lane_fix_unchanged;
        a.lane_skip_meas      += b.lane_skip_meas;
        a.cold_bridge_cycles  += b.cold_bridge_cycles;
        a.mid_decided         += b.mid_decided;
        a.mid_fallback        += b.mid_fallback;
        a.mid_closure         += b.mid_closure;
        a.mid_disagree        += b.mid_disagree;
        a.top2_decided        += b.top2_decided;
        a.dual_dedupe         += b.dual_dedupe;
        a.sib_checked         += b.sib_checked;
        a.sib_unavail         += b.sib_unavail;
        a.sib_int_off         += b.sib_int_off;
        a.sib_blocked         += b.sib_blocked;
        a.sib_abs_frac_sum    += b.sib_abs_frac_sum;
    };
    // WL-3 regroup census (run totals; the equivalence leg reads these).
    long long lane_pair_arcs = 0;           ///< arcs routed into pair groups.
    long long lane_resid_arcs = 0;          ///< primary-band arcs left in residual per-band groups (partner missing/unshared).
    long long lane_pair_tree_mismatch = 0;  ///< pair arcs whose two bands' eq34 tree tags disagreed (band-1's tag governs).
    // FDMA-correct per-signal wavelengths for the WL/EWL-row coefficients and z.  Built when
    // EITHER lane is armed (mirrors ar_ppp_utils).
    std::unique_ptr<PppUtils> mw_wl_ppp_utils;
    if (mw_wl_on || ewl_on || lane_on) mw_wl_ppp_utils = std::make_unique<PppUtils>(log_manager_);
    NetworkMwWlLayer::Counters mw_wl_run;    // run-total eligibility/skip tallies (invariant #4)
    NetworkMwWlLayer::Counters mw_wl_epoch;  // per-build tallies (the last build before the accepted solve wins)
    // M5 card WL-1 F6 diagnostic: one-shot per-arc/per-row attribution dump of the FIRST
    // arming epoch's WL rows (docs/HANDOFF_mw_wl_card.md §6c).  Populated in
    // buildCarryDdPriors (build-time: z_MW/z_lin/z_corr + per-arc MW̄/x_lin, where the
    // resolver + revmap are in scope) and finalized post-solve (reconstructed_abs from
    // reconstructStation + the kernel's WL-block innovation).  Log-only; default OFF.
    const bool mw_wl_f6_dump = mw_wl_on && config_.getNetworkConfig().mw_wl.f6_dump;
    struct F6Arc {
        int         est_station{-1};
        int         col1{-1}, col2{-1};
        std::string sat, sig1, sig2;
        double      coeff{0.0};       // signed cycle coefficient c (±1 for a plain DD)
        double      mw_mean_m{0.0};   // ZD-consistent arc MW mean (m)
        double      mw_var_m2{0.0};
        long long   mw_count{0};
        bool        bia_corrected{false};
        double      x_lin1{0.0}, x_lin2{0.0};              // amb linearization values (m)
        double      lambda1{0.0}, lambda2{0.0}, lambda_wl{0.0};
    };
    struct F6Row {
        std::vector<F6Arc> arcs;
        double z_mw{0.0}, z_lin{0.0}, z_corr{0.0}, zhat_block{0.0};
    };
    struct F6Capture {
        bool                 pending{false};   // captured this epoch, awaiting the post-solve write
        bool                 written{false};   // one-shot latch (whole run)
        double               week{0.0}, tow{0.0};
        int                  loop_epoch{-1};
        std::size_t          wl_block_index{0};
        std::vector<F6Row>   rows;
    } mw_wl_f6;
    // Card 4 (M1) phase D: the emit-layer frame re-base (docs/HANDOFF_card4_phaseD.md).
    // Re-references the emitted per-(sat,σ) phase biases onto the previous EMISSION
    // frame so the freely-re-realized eq34 datum's per-epoch gauge churn does not
    // storm the consumer with spurious ambiguity resets; a genuine reference-path
    // slip's integer step is left intact so the emitter jump test still fires.
    // Default-ON when carry_invariant is armed (spec D7); the OFF path is
    // carry_invariant=false (byte-identical — the engine is never touched).
    const bool emit_rebase_on = carry_invariant && config_.networkCarryInvariantEmitRebase();
    NetworkEmitRebase emit_rebase_engine;
    // M5 prerequisite: PB emission-continuity filter — a slip-gated per-key scalar
    // KF on the RE-BASED emitted biases + a self-tuning k·σ̂_step jump gate that
    // clears the threshold-in-noise discontinuity floor.  Runs INSIDE the re-base
    // block (it filters the re-based emission frame), so it requires emit_rebase_on;
    // OFF (default) ⇒ filterEpoch is never called (byte-identical — the engine is
    // constructed like emit_rebase_engine but stays inert).
    const bool emit_pb_filter_on = emit_rebase_on && config_.networkEmitPbFilter();
    NetworkPbEmitFilter::Config pb_filter_cfg;
    pb_filter_cfg.q_m2_s         = config_.networkEmitPbFilterQ();
    pb_filter_cfg.jump_k         = config_.networkEmitPbFilterJumpK();
    pb_filter_cfg.meas_sigma_m   = config_.networkEmitPbFilterMeasSigmaM();
    pb_filter_cfg.max_gap_s      = config_.networkEmitPbFilterMaxGapS();
    pb_filter_cfg.window_epochs  = config_.networkEmitPbFilterWindowEpochs();
    pb_filter_cfg.gate_v2        = config_.networkEmitPbFilterGateV2();  // PB-gate-v2 merge point (leaf wiring)
    NetworkPbEmitFilter pb_filter_engine(pb_filter_cfg);
    const bool pb_filter_emit_sigma = config_.networkEmitPbFilterEmitSigma();
    std::map<std::string, int>                    carry_sat_vertex;  // stable satellite -> vertex id (disjoint high range)
    std::map<std::pair<char, std::string>, int>   carry_group_id;    // (constellation,signal) -> stable group key
    bool carry_glonass_warned = false;   // one-time WARN when a GLONASS ambiguity is excluded (FDMA)
    // UWL-1 §3.1: once-per-run loud-skip warns — a constellation whose cycles enter
    // the EWL machinery without a lane tier (untabled under the armed card, or any
    // non-GPS constellation under the legacy digits) must fail LOUDLY, not as a
    // silent skip_missing_mw pile (the measured Galileo death, SPEC §1 fact 3).
    std::set<char> uwl_warned_cons;
    // WL-3 census visibility (once per run per cons; log-only).
    std::set<char> lane_census_logged;
    // v2 (uwl_family_pairs): the run-persistent admissible pair set per constellation.
    // A pair joins when its joint-census count first reaches uwl_family_min_arcs and
    // stays admitted for the run (LATCHED — membership flicker would refloat the
    // family group's carried chords every flip, the WL-3 micro-pair-group hazard;
    // a latched pair whose channels vanish simply routes no arcs, harmless).
    std::map<char, std::set<std::pair<std::string, std::string>>> lane_admit_pairs;
    std::set<std::pair<char, std::string>> lane_family_logged;   // once-per-pair INFO
    double carry_prev_tow = std::numeric_limits<double>::quiet_NaN();
    long long carry_epochs = 0, carry_born_total = 0, carry_marg_total = 0,
              carry_merges_total = 0, carry_splits_total = 0, carry_swaps_total = 0,
              carry_zdim_last = 0, carry_slipkills_total = 0, carry_dropped_total = 0;
    // Card 4 (M1) D5: arcs the innovation gate flagged in the PREVIOUS epoch, to be
    // treated as slipped THIS epoch (the estimation-side fine-slip re-tap runs a post-solve
    // detection ⇒ it feeds the NEXT epoch's slip funnel).  Persists across epochs (the
    // per-epoch carry_slipped_ambs is rebuilt each epoch).  Empty unless carry_innov_gate.
    std::set<std::pair<int, std::string>> carry_innov_pending;
    long long carry_innov_flags_total = 0, carry_innov_retaps_total = 0;
    long long carry_innov_map_miss = 0;   // D5 flag→ambiguity reverse-map misses (should stay 0)
    const bool phase_datum_prioritized = config_.networkPhaseDatumPrioritized()
                                       || phase_datum_ensemble
                                       || phase_datum_eq34;
    // Fix A seed-once + time-lock (network.phase_datum_hold_sigma_m).  The un-held
    // ensemble references the emitted bias to mean_r(ẑ_r) — a per-epoch-re-realized
    // float-ambiguity mean whose absolute LEVEL is code/iono-precision-limited — so
    // the product churns ~metre/epoch (a dynamic datum on a static product).  When
    // >0 the gauge is SEEDED once per (sat,signal) and thereafter the shared bias is
    // HELD by a stiff temporal prior of this sigma: the Σz=0 fold is skipped for a
    // (sat,signal) that already has a carried bias, and the D2 stash carries that
    // bias with a fixed (clean) hold-variance instead of dropping it.  0 = re-realize
    // every epoch (the churny baseline); inert unless phase_datum_ensemble.
    const double phase_hold_sigma = phase_datum_ensemble
                                        ? config_.networkPhaseDatumHoldSigmaM() : 0.0;
    const bool   phase_hold_on    = phase_hold_sigma > 0.0;
    const double phase_hold_var   = phase_hold_sigma * phase_hold_sigma;
    auto isGaugePinned = [&](size_t i, const std::string& sat,
                             const std::string& signal) -> bool {
        if (!phase_datum_prioritized) return stations[i].is_pivot;
        return stations[i].pinned_amb_targets.count(sat + "_" + signal) != 0;
    };

    // Column-split helper: split a station's H into the shared border A_s
    // (clock/phase-bias columns) and the local nuisance block A_r (everything
    // except the 3 fixed-position columns and the shared columns).  Returns the
    // H-column → A_r-column map (used for the pivot ambiguity pins).
    auto splitStation = [&](NetStation& st, const NetworkEstimator& est,
                            const EpochMeasurement& em,
                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                            Eigen::VectorXd& w, Eigen::VectorXd& y,
                            std::map<int, int>& h2local) {
        const Eigen::Index m       = em.H.rows();
        const Eigen::Index nstate  = em.H.cols();
        const Eigen::Index nshared = est.numShared();
        A_s = Eigen::MatrixXd::Zero(m, nshared);

        std::set<int> shared_cols;  // H-column indices that map to the shared border
        for (const auto& kv : st.sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }
        for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(kv.first);
            if (!parts) continue;
            Eigen::Index col;
            try { col = est.phaseBiasColumn(parts->satellite, parts->signal); }
            catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }
        // Satellite CODE bias (W6): scatter each per-(sat,signal) sat-bias column
        // into the shared border.  In network mode getSatBiasStates() holds ONLY
        // the network-owned satellite code biases (the single-receiver fallback is
        // gated off in ensureRequiredStates); GLONASS gets none (code-only scope).
        for (const auto& kv : st.sm->getSatBiasStates()) {
            const auto parts = StateKeys::parseSatBiasKey(kv.first);
            if (!parts) continue;
            Eigen::Index col;
            try { col = est.codeBiasColumn(parts->satellite, parts->signal); }
            catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }

        // Receiver inter-frequency-bias (IFB) columns on NON-primary bands: when
        // estimating the satellite code bias, drop the receiver IFB on every band
        // outside the iono-free reference pair (the "sys_IFB<band>" states).  An
        // EXPLICIT non-primary receiver IFB drains the code information that
        // otherwise separates that band's satellite clock from its phase bias —
        // regressing the L5/E5b/... phase-bias product to a near-gauge (verified:
        // keeping it drives the per-constellation L5Q phase-bias common mode below
        // the null-space cutoff and withholds the whole epoch).  Dropping it folds
        // the receiver's non-primary code delay into the per-(rx,constellation)
        // slant-iono common mode the GIM weighting deliberately leaves free (the
        // L1-referenced convention); the SATELLITE code bias on that band stays
        // observable through the GIM's BETWEEN-satellite iono structure (a per-
        // satellite quantity, so the per-receiver delay averages out across the
        // network), not through an explicit receiver counterpart.  The two primary-
        // band receiver DCB ("sys_DCB") is kept, and the GLONASS per-slot IFB (a
        // different state family) is untouched.
        std::set<int> drop_cols;
        if (config_.estimateNetworkSatCodeBias()) {
            for (const auto& kv : st.sm->getCodeBiasStates()) {
                if (StateKeys::isGlonassPerSlotIfbKey(kv.first)) continue;
                if (kv.first.find("_IFB") != std::string::npos)
                    drop_cols.insert(static_cast<int>(kv.second));
            }
        }

        const int pos = static_cast<int>(st.sm->getPositionIndex());  // 0
        std::vector<int> local_cols;
        local_cols.reserve(static_cast<size_t>(nstate));
        for (int c = 0; c < static_cast<int>(nstate); ++c) {
            if (c == pos || c == pos + 1 || c == pos + 2) continue;  // coords fixed
            if (shared_cols.count(c)) continue;                      // shared border
            if (drop_cols.count(c)) continue;                        // non-primary rx IFB (dropped)
            // Drop a structurally-unobservable local state (all-zero H column this
            // epoch).  The usual case: the Galileo inter-system-bias state of a
            // station that tracks no Galileo this epoch (the StateManager allocates
            // an ISB per active non-GPS system regardless of per-station, per-epoch
            // visibility); also a stale ambiguity/iono column for a satellite the
            // cross-station observability gate just removed.  Keeping the dead
            // column makes the local normal block N_rr singular, and the kernel
            // then rejects the WHOLE station — throwing away its perfectly good GPS
            // observations (this is why GPS-only NVSK00RUS / MDVJ00RUS were dropped
            // wholesale).  Excluding ONLY the dead column lets the station
            // contribute every observable state it has; its receiver clock absorbs
            // its GPS rows, so the GPS clock common-mode gauge stays intact.
            if (em.H.col(c).cwiseAbs().maxCoeff() == 0.0) continue;
            h2local[c] = static_cast<int>(local_cols.size());
            local_cols.push_back(c);
        }
        // --- Linearity invariant (A6, insurance) --------------------------------
        // Every column retained in local_cols becomes a column of A_r, i.e. a
        // parameter the Schur kernel treats as LINEAR in the observations.  That is
        // valid ONLY because the receiver position is held fixed at its known ECEF:
        // the 3 position columns (pos..pos+2) are dropped above ("coords fixed"), so
        // A_r contains only genuinely-linear nuisances (rx clock, tropo ZWD, per-sat
        // slant iono, ambiguities, code/phase biases, ISBs).  A position column is
        // NONLINEAR in the geometry (its H entry is the line-of-sight unit vector,
        // re-linearized each epoch about a moving point) and must never enter A_r for
        // a fixed-coordinate station.  Should a future float-coordinate (non-fiducial)
        // station ever survive here, the fixed-linear-block assumption is violated and
        // the estimate would be silently wrong — hard-fail instead.  This scan never
        // fires on the current fixed-coord path (the loop above already `continue`s on
        // pos..pos+2), so it is byte-identical; the throw rides the produce-phase
        // try/catch (per-station, thread-safe) that dispositions a failed station.
        for (int lc : local_cols) {
            if (lc == pos || lc == pos + 1 || lc == pos + 2) {
                throw std::runtime_error(fmt::format(
                    "Network splitStation: position column {} of station '{}' survived "
                    "into the linear local block A_r. The network holds all station "
                    "coordinates fixed; a position column here breaks the "
                    "linear-in-observation invariant of A_r (is_pivot={}).",
                    lc, st.name, st.is_pivot));
            }
        }
        A_r.resize(m, static_cast<Eigen::Index>(local_cols.size()));
        for (size_t j = 0; j < local_cols.size(); ++j)
            A_r.col(static_cast<Eigen::Index>(j)) = em.H.col(local_cols[j]);

        w = em.R_v.cwiseInverse();   // per-obs information = 1/σ²
        y = em.innovations;
    };

    // GIM ionosphere pseudo-observation (W6 satellite code-bias prerequisite).
    // Ports Application::applyGimIonoUpdate's common-mode-free R into tight-prior
    // rows folded into a station's OWN block: one row per GPS/Galileo satellite
    // with a usable slant-iono column and a valid GIM value, with a per-
    // constellation R = diag(var) + σ_c²·11ᵀ.  The all-ones block leaves the
    // per-constellation common mode (the receiver iono-datum level) UNCONSTRAINED,
    // imposing only the GIM's between-satellite structure — exactly what separates
    // the satellite DCB from the slant iono while leaving the constellation-wide
    // DCB level for the explicit sum-to-zero datum.  The correlated R is whitened
    // per constellation (L⁻¹ from LLᵀ=R) into unit-information rows so it stays in
    // the station's diagonal-weight block.  GLONASS is handled by its own loose
    // anchors above (no satellite code bias).  Built into the station's own block —
    // no shared write, safe in the parallel produce phase (GIM query is const).
    auto appendGimIonoRows = [&](NetStation& st,
                                 const std::vector<PppObservation>& sta_obs,
                                 const std::map<int, int>& h2local, const GnssTime& t,
                                 Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                 Eigen::VectorXd& w, Eigen::VectorXd& y) {
        if (!gim_ || !gim_->isLoaded()) return;
        const auto& iw = config_.getIonosphereWeightingConfig();
        if (!iw.enabled) return;
        const Eigen::VectorXd x = st.sm->getStateVector();

        struct GRow { int col; double innov; double var; };
        std::map<char, std::vector<GRow>> by_const;   // constellation char -> rows
        for (const auto& o : sta_obs) {
            if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
            if (o.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const auto hit = h2local.find(iono_idx);
            if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
            const GimQuery q =
                gim_->slantIonoDelay(t, st.ecef, o.sat_pos, Constants::GPS_L1_FREQ);
            if (!q.valid) continue;
            const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
            by_const[o.satellite.front()].push_back(
                GRow{hit->second, q.delay_m - x(iono_idx), sigma * sigma});
        }

        int total = 0;
        for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
        if (total == 0) return;

        const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
        const Eigen::Index m0     = A_s.rows();
        const Eigen::Index ncol_r = A_r.cols();
        A_s.conservativeResize(m0 + total, Eigen::NoChange);
        A_s.bottomRows(total).setZero();
        A_r.conservativeResize(m0 + total, Eigen::NoChange);
        A_r.bottomRows(total).setZero();
        w.conservativeResize(m0 + total);
        y.conservativeResize(m0 + total);

        Eigen::Index out = m0;
        for (auto& kv : by_const) {
            const std::vector<GRow>& rws = kv.second;
            const int mc = static_cast<int>(rws.size());
            Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
            for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
            Eigen::MatrixXd Hc = Eigen::MatrixXd::Zero(mc, ncol_r);
            Eigen::VectorXd zc(mc);
            for (int k = 0; k < mc; ++k) { Hc(k, rws[k].col) = 1.0; zc(k) = rws[k].innov; }
            // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows that contribute exactly
            // Hᵀ R⁻¹ H / Hᵀ R⁻¹ z to the station normals.
            const Eigen::LLT<Eigen::MatrixXd> llt(R);
            A_r.middleRows(out, mc) = llt.matrixL().solve(Hc);
            y.segment(out, mc)      = llt.matrixL().solve(zc);
            w.segment(out, mc).setOnes();
            out += mc;
        }
    };

    // Co-estimated SH-VTEC ionosphere pseudo-observation (the design's hinge —
    // replaces the external-GIM target of appendGimIonoRows with the network's OWN
    // spherical-harmonic field).  Per GPS/Galileo satellite with a usable slant-iono
    // column it folds one row asserting  I_local(rx,sat) = M(el)·VTEC_SH(IPP) :
    //   H:  +1 on the local slant-iono column (A_r, exactly as the GIM row), and
    //      −M(el)·tecu_to_L1m·Ȳ_nm(IPP) on each SHARED SH coefficient column (A_s) —
    //      the ONE new write that turns the prior into a joint estimate.
    // The innovation is −x(iono_idx) (the field at coeff 0 is 0; absolute solve).
    // The common-mode-free per-constellation R = diag(var)+σ_c²·11ᵀ is REUSED
    // unchanged: its all-ones block leaves the per-(rx,const) iono datum level FREE
    // (the receiver DCB rides in it), so the field constrains only the between-
    // satellite slant-iono STRUCTURE — a₀₀ (global mean) is observable only weakly,
    // through the mapping-function spread, and is otherwise held by its loose Kaula
    // prior (data-determined mean, not clamped to an external value).  GLONASS is
    // excluded from the field (FDMA per-slot IFB would leak into the global field;
    // see design §8).  Built into the station's own block + the shared border; the
    // SH column lookups are const, safe in the parallel produce phase.
    // T5 (FABLE-GATE, 2026-07-10): per-(station,sat) SH representativeness-error
    // Gauss–Markov deviation carry.  The single-layer SH surface cannot represent
    // per-LOS slant-iono structure (~80 % of LOS residuals > 3σ at ANY degree, T4);
    // re-applying the pin as white forces that misfit dispersively into the float
    // ambiguities (the uniform-DD-fraction root cause).  With rep_error_tau_s > 0 the
    // pin constrains (ι − SH·c) to a TRACKED deviation d (GM: τ = rep_error_tau_s,
    // steady-state σ = representation_sigma_tecu mapped to slant) instead of to 0.
    // Threading contract: the map is AGED once per epoch in a serial pre-pass and
    // measurement-UPDATED in the serial post-solve harvest; the parallel assembly
    // only READS (find, never insert) — race-free by construction.  dev/var in slant
    // L1 metres.  var_ss = per-LOS steady-state σ² (mapping-dependent, refreshed at
    // harvest).  t_s = 0 marks a never-harvested (cold) entry.  OFF ⇒ byte-identical.
    struct ShDevCarry { double dev_m{0.0}; double var_m2{0.0}; double var_ss{0.0}; double t_s{0.0}; };
    std::map<std::string, ShDevCarry> sh_dev_carry;   // key "STATION|SAT"
    const double sh_dev_tau = sh_model ? shc.rep_error_tau_s : 0.0;
    const bool   sh_dev_on  = sh_dev_tau > 0.0;

    // ---- Station-adaptive SH-VTEC-constraint deweight (iono deweight card) ----------
    // Per-station factor w_s ∈ (0,1] scaling the INFORMATION of this station's SH↔slant
    // iono pseudo-obs (σ² ×= 1/w_s ⇒ info ×= w_s).  w_s→0 decouples the station's per-LOS
    // iono from the global SH field in BOTH directions (the single joint pseudo-obs means
    // one w_s covers both) while the station still serves the shared
    // clocks/biases/datum/position.  All the gate math (v1 static list + v2 network-relative
    // IGG auto-trigger, EWMA + hysteresis + slew) lives in NetworkIonoDeweightGate; here we
    // just feed it per-station statistics in the serial post-solve harvest and READ
    // iono_deweight.weight() in the parallel assembly (const find, race-free — the
    // sh_dev_carry contract).  Empty static list + adaptive OFF ⇒ weight() is 1.0 for every
    // station ⇒ byte-identical to OFF (invariant #6).
    const IonoConstraintDeweightConfig& idw = config_.getIonoConstraintDeweightConfig();
    NetworkIonoDeweightGate iono_deweight(
        std::set<std::string>(idw.stations.begin(), idw.stations.end()), idw.factor,
        idw.adaptive, idw.window_epochs, idw.sigma_hi, idw.sigma_lo, idw.floor,
        idw.rate_limit, idw.min_epochs);
    const bool idw_on = iono_deweight.active();
    if (idw_on && !sh_model)
        log->warn("Network (iono deweight): configured but network.estimate_iono_sh is OFF — "
                  "there is no SH field to deweight; the setting is inert this run.");
    if (sh_model && iono_deweight.staticCount() > 0) {
        std::set<std::string> idw_present;
        for (const auto& st : stations) idw_present.insert(st.name);
        for (const auto& nm : iono_deweight.staticStations())
            if (!idw_present.count(nm))
                log->warn("Network (iono deweight): statically-listed station '{}' is not in this "
                          "network — ignored (check the name).", nm);
    }

    auto appendShIonoRows = [&](NetStation& st, const NetworkEstimator& est,
                                const std::vector<PppObservation>& sta_obs,
                                const std::map<int, int>& h2local, const GnssTime& t,
                                Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                Eigen::VectorXd& w, Eigen::VectorXd& y) {
        if (!sh_model) return;
        const auto& iw = config_.getIonosphereWeightingConfig();
        if (!iw.enabled) return;
        // Iono-constraint deweight factor for THIS station (READ-only const lookup;
        // race-free per the threading contract above).  w_s==1 ⇒ untouched (byte-identical
        // OFF); w_s<=0 ⇒ append NO SH iono rows (the per-LOS iono floats freely on its own
        // GF code/phase — full decouple).
        const double w_s = iono_deweight.weight(st.name);
        if (w_s <= 0.0) return;
        const Eigen::VectorXd x = st.sm->getStateVector();
        const double Re = shc.earth_radius_km * 1000.0;
        const double H  = shc.layer_height_km * 1000.0;
        const double tecu_to_L1m =
            40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
        const double lon_sun = shc.sun_fixed ? meanSunLongitude(t) : 0.0;
        const int K = sh_model->numCoeffs();

        struct SRow {
            int    local_col;        ///< A_r column of this satellite's slant-iono state.
            double innov;            ///< −x(iono_idx).
            double var;              ///< per-LOS representativeness variance (m²).
            double map_tecu;         ///< M(el)·tecu_to_L1m (the STEC scale).
            std::vector<double> basis;  ///< Ȳ_nm(IPP) in coeff order.
        };
        std::map<char, std::vector<SRow>> by_const;
        std::vector<double> bvals;
        for (const auto& o : sta_obs) {
            if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
            if (o.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const auto hit = h2local.find(iono_idx);
            if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
            const IppGeometry g = computeIppGeometry(st.ecef, o.sat_pos, Re, H);
            if (!g.valid) continue;
            const double lon_sf = g.ipp_lon_rad - lon_sun;
            sh_model->basis(g.ipp_lat_rad, lon_sf, bvals);
            const double sigma =
                std::max(iw.floor_m, shc.representation_sigma_tecu * g.mapping * tecu_to_L1m);
            SRow r;
            r.local_col = hit->second;
            r.innov     = -x(iono_idx);
            r.var       = sigma * sigma;
            // T5: the pin constrains (ι − SH·c) to the PREDICTED deviation at the
            // prediction's variance + the floor, instead of to 0 at the full
            // representativeness σ.  READ-ONLY on the carry (serial pre-pass ages it;
            // a missing key means cold seed: dev 0, P = steady-state σ² — identical
            // information to the untreated row, so cold LOS behave as before).
            if (sh_dev_on) {
                const double sig_ss =
                    shc.representation_sigma_tecu * g.mapping * tecu_to_L1m;
                double dev_pred = 0.0;
                double var_pred = sig_ss * sig_ss;
                const auto dit = sh_dev_carry.find(st.name + "|" + o.satellite);
                if (dit != sh_dev_carry.end() && dit->second.t_s > 0.0) {
                    dev_pred = dit->second.dev_m;
                    var_pred = dit->second.var_m2;
                }
                r.innov = -x(iono_idx) + dev_pred;
                r.var   = iw.floor_m * iw.floor_m + var_pred;
            }
            r.map_tecu  = g.mapping * tecu_to_L1m;
            r.basis     = bvals;
            by_const[o.satellite.front()].push_back(std::move(r));
        }

        int total = 0;
        for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
        if (total == 0) return;

        // SH column indices (shared border), looked up once per call.
        std::vector<Eigen::Index> sh_cols(static_cast<std::size_t>(K));
        for (int k = 0; k < K; ++k) {
            const auto& c = sh_model->coeffs()[static_cast<std::size_t>(k)];
            sh_cols[static_cast<std::size_t>(k)] = est.vtecCoeffColumn(c.n, c.m, c.trig);
        }

        const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
        const Eigen::Index m0      = A_s.rows();
        const Eigen::Index ncol_r  = A_r.cols();
        const Eigen::Index nshared = A_s.cols();
        A_s.conservativeResize(m0 + total, Eigen::NoChange);
        A_s.bottomRows(total).setZero();
        A_r.conservativeResize(m0 + total, Eigen::NoChange);
        A_r.bottomRows(total).setZero();
        w.conservativeResize(m0 + total);
        y.conservativeResize(m0 + total);

        Eigen::Index out = m0;
        for (auto& kv : by_const) {
            const std::vector<SRow>& rws = kv.second;
            const int mc = static_cast<int>(rws.size());
            Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
            for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
            // T1b: temporal treatment of the quasi-static SH constraint.  Re-applying the
            // (sun-fixed, quasi-static) SH↔slant pseudo-obs as an INDEPENDENT white row
            // every epoch is a σ-incest channel — the formal σ collapses without new
            // information.  Inflate the row noise by N (σ ×√N) so each epoch's white row
            // carries only 1/N of the information: this matches the information rate of
            // "one full look per N epochs" but WITHOUT withholding.  (Pure row-skip drops
            // the satellite-code-bias observability the SH rows provide → the observability
            // gate withholds every skipped epoch; the empirical A/B confirmed 6/10 epochs
            // withheld at N=3, so σ-inflation is the observability-preserving mechanism.)
            // dec ≤ 1 ⇒ no scaling ⇒ byte-identical.
            if (shc.row_decimation_epochs >= 2)
                R *= static_cast<double>(shc.row_decimation_epochs);
            // Station-adaptive iono-constraint deweight: scale this station's whole SH-iono
            // block information (σ² ×= 1/w_s ⇒ info ×= w_s), uniformly on the per-LOS
            // diagonal var AND the common-mode off-diagonal — preserving the receiver-DCB-
            // free structure.  w_s==1 ⇒ no scaling ⇒ byte-identical (mirrors the
            // row_decimation σ-inflation precedent above).  w_s<=0 already returned early.
            if (w_s < 1.0)
                R *= 1.0 / w_s;
            Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);    // local +1 (slant iono)
            Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, nshared);   // −M·tecu·basis (SH cols)
            Eigen::VectorXd zc(mc);
            for (int k = 0; k < mc; ++k) {
                Hr(k, rws[k].local_col) = 1.0;
                for (int j = 0; j < K; ++j)
                    Hs(k, sh_cols[static_cast<std::size_t>(j)]) =
                        -rws[k].map_tecu * rws[k].basis[static_cast<std::size_t>(j)];
                zc(k) = rws[k].innov;
            }
            // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows; whiten the local +1,
            // the SH partials, AND the innovation by the SAME factor so the common-
            // mode-free structure carries through to both blocks identically.
            const Eigen::LLT<Eigen::MatrixXd> llt(R);
            A_r.middleRows(out, mc) = llt.matrixL().solve(Hr);
            A_s.middleRows(out, mc) = llt.matrixL().solve(Hs);
            y.segment(out, mc)      = llt.matrixL().solve(zc);
            w.segment(out, mc).setOnes();
            out += mc;
        }
    };

    // Pin Eigen to a single thread for the duration of the (parallelized) epoch
    // loop.  _OPENMP is defined project-wide so Eigen threads its own dense
    // kernels; nesting those inside our per-station OpenMP region would cause
    // thread oversubscription AND non-deterministic reduction order in the
    // per-station matmuls.  Single-threaded Eigen inside each station thread is
    // fully thread-safe (separate matrices) and deterministic; restored after the
    // loop.  Only touched when parallelism is active, so the serial path (and the
    // small-network case) is byte-identical to before.
    const int eigen_threads_saved = Eigen::nbThreads();
    if (parallel_on) Eigen::setNbThreads(1);

    // [TIMING] Wall-clock of just the per-epoch loop (excludes ~startup: product
    // cache load + per-station RINEX parse).  Isolates the per-epoch compute that
    // scales with station count — the scaling metric.  Counts only epochs that
    // actually entered processing (past the max-duration cutoff).
    const auto t_loop_begin = std::chrono::steady_clock::now();
    size_t n_loop_epochs = 0;
    size_t decim_counter = 0;   // T2e: counts post-offset raw epochs for epoch_decimation_n
    // T1a: persistent state for the a₀₀ physical-band divergence WARN — start "in band"
    // so the first out-of-band epoch is always reported (transition edge), then throttle.
    bool sh_a00_was_in_band = true;
    // T1d: SH-field convergence latch + previous coefficient vector for the epoch-over-
    // epoch RMS-change stationarity test that (optionally) gates the leveling-carry warmup
    // and/or the one-shot carry refloat.  Empty/false until the first emitted SH epoch.
    bool sh_converged_latch = false;
    Eigen::VectorXd sh_coeff_prev;

    // The joint estimator is HOISTED out of the epoch loop and reset()/reused each
    // epoch.  Constructing a fresh NetworkEstimator per epoch reallocated one dense
    // Schur block PER STATION every epoch; the process heap retained that churn as
    // committed memory, saturating at ~0.5 GB/station (≈146 GB at 305 stations over
    // a 121-epoch run → exhausted RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator est;
    // OPT-IN fan-out of the Schur reduction.  Kept SEPARATE from the build-phase
    // parallelism: the build's serial-ordered scatter is byte-identical to a serial
    // run, but the reduce fan-out reassociates the per-station summation, so it is
    // reproducible per thread count yet ULP-different from the serial reduce (the
    // network's ill-conditioning amplifies that to ~1e-5 in the products — physically
    // null, but it breaks serial≡parallel byte-identity).  Default OFF: the reduce
    // stays serial (byte-identical serial≡parallel) while the build still threads and
    // the deterministic serial-tail wins (eigensolver gate, no double-reduce, factor
    // reuse) still apply.  Enable network.parallel_reduce for max-speed runs.
    //
    // Guardrail (Fable lean review, Jul-14 eve): parallel_reduce OpenMP-fans reduce()'s
    // per-station Schur loop; parallel_carry OpenMP-fans the fold's per-station reduction
    // and est.solve()'s dense algebra (fold GEMMs + the K×K middle inverse) is itself
    // Eigen-threaded — so armed together they nest OpenMP × Eigen inside reduce() and
    // oversubscribe.  They are therefore MUTUALLY EXCLUSIVE, resolved at LOAD time
    // (loadNetworkSection_ forces parallel_carry OFF when both are set), so by here at most
    // one of nc.parallel_reduce / nc.parallel_carry is true.  The `!carry_par` below is thus
    // belt-and-suspenders: it can no longer be false while reduce is armed.
    const bool reduce_par   = parallel_on && nc.parallel_reduce && !carry_par;
    const int  reduce_threads = reduce_par ? n_threads : 1;
    est.setReduceThreads(reduce_threads);
    // Deterministic-parallel DD carry-fold fan-out (network.parallel_seam); 1 = serial.
    est.setSeamThreads(seam_threads);
    // network.parallel_carry: when armed, solve()'s foldReducedDdPriors runs SERIAL-over-
    // blocks with WITHIN-BLOCK per-station threading (Eigen stays PINNED, so the driver does
    // NOT un-pin it around solve()).  OFF ⇒ the fold keeps seam_threads (block-parallel
    // seam), byte-identical.
    est.setCarryParallelThreads(carry_par ? n_threads : 1);
    // network.parallel_carry: RAII un-pin of Eigen's internal threading around ONE
    // compute-bound-GEMM carry stage.  Used at exactly TWO sites — buildDdPriors (W·Pᶻ·Wᵀ)
    // and installPosterior (posterior-covariance read-back) — whose cost is big dense GEMMs
    // Eigen threads well.  The THIRD stage, the fold (foldReducedDdPriors), is deliberately
    // NOT wrapped: its cost is the per-station reduction (small inner-dim matmuls + a serial
    // LDLT), which Eigen can't thread — it self-fans with its OWN OpenMP and Eigen PINNED,
    // so wrapping solve() here would nest that OpenMP with Eigen's GEMM threads.  The epoch
    // loop pins Eigen to one thread (line ~3086, to avoid nesting inside the per-station
    // OpenMP assembly region); these two stages run OUTSIDE that region, so un-pinning fans
    // their GEMMs across cores.  Restores the pin on EVERY exit path (an early `continue`, a
    // withheld-epoch skip, or a thrown call) so the next epoch's assembly / the fold's own
    // OpenMP is never left with Eigen un-pinned.  Inert unless @c on — so an OFF (default)
    // run never touches Eigen's thread count here (byte-id).
    struct CarryEigenScope {
        bool active;
        int  saved;
        CarryEigenScope(bool on, int nthreads) : active(on), saved(Eigen::nbThreads()) {
            if (active) Eigen::setNbThreads(nthreads);
        }
        ~CarryEigenScope() { if (active) Eigen::setNbThreads(saved); }
        CarryEigenScope(const CarryEigenScope&) = delete;
        CarryEigenScope& operator=(const CarryEigenScope&) = delete;
    };
    // Pin-aware station PD guard (network.station_pd_pin_aware): median-|pivot|
    // reference scale for the local-block rank test — the prioritized datum's
    // in-block 1e10 anchor pins otherwise eject marginal stations every epoch.
    est.setStationPdMedianScale(config_.networkStationPdPinAware());
    if (reduce_par)
        log->info("Network: Schur reduce PARALLEL across {} threads "
                  "(serial≡parallel byte-identity intentionally relaxed to ~1e-5)",
                  n_threads);

    // === Tier D0: per-epoch section timers (network.trace_section_timers) =====
    // Arm the estimator's wall-clock accumulators around assembly / reduce / solve
    // / the shared-covariance full inverse / the per-station covariance consumers,
    // so a profiling run can see which term dominates the per-epoch cost (⇒ which
    // Tier-D scaling lever is worth pulling).  The estimator accumulates CONTINUOUSLY
    // across the run; the driver snapshots it per epoch for the per-epoch delta line
    // and reads the run total at exit.  OFF (default) ⇒ no clock reads, byte-identical.
    const bool sec_timers_on = nc.trace_section_timers;
    est.setPerfTiming(sec_timers_on);
    est.resetSectionTimers();
    // INC-C0: arm the carry driver's per-op timers on the SAME toggle — splits the
    // SB_DDBUILD / SB_INSTALL wall brackets into basis-projection vs posterior read-back
    // vs D5-gate (the dense-K² ops the perf-restructure card ranks).  Byte-identical off.
    carry_driver.setPerfTiming(sec_timers_on);
    carry_driver.resetPerfTimers();
    double sec_asm_wall_ms = 0.0;      // D0: run-total driver-side assembly wall (produce + serial scatter)
    std::size_t sec_profiled_epochs = 0;  // D0: #epochs that reached the post-solve per-epoch readout
    if (sec_timers_on)
        log->info("Network (D0): per-epoch section timers ARMED "
                  "(assembly / reduce / solve / cov-inverse / cov-consumers).");

    // === Tier D0b: sub-instrument the "other" bucket (network.trace_section_timers) ==
    // "other" = loop wall − assembly − reduce − solve − cov-inverse − cov-consumers was
    // measured at ~81 % (42.9 s/ep) of the epoch on the 300 s GPS-only tier and scales
    // with LOS count (docs/HANDOFF_perlos_parallel_sweeps.md §1) — yet none of it was
    // timed individually.  These per-block driver-side wall timers attribute it.  For each
    // block, net = wall − Δ(reduce + solve + cov-inverse + cov-consumers) incurred INSIDE
    // it, so the recon/solve time already counted in the five D0 sections is NOT double-
    // counted: net is the block's TRUE contribution to "other" (a block that reconstructs
    // N stations shows its driver glue as net, its O(nₛ²) recon as cov-consumers).  The
    // brackets are disjoint and tile the loop body between the assembly wall and the
    // emitted-epoch readout; the residual (other − Σ net) is logged as "unattributed".
    // Inert (no clock reads, no est.sectionTimers() calls) when the toggle is OFF — the
    // subBegin/subEnd lambdas early-return, so byte-identity with an untimed run holds.
    enum SubBlock {
        SB_SLIP = 0,     // block 1: PASS-A slip scans (MW refloat + IF-TD jumps), pre-assembly
        SB_PREASM,       //          pre-assembly measurement build (post-slip → assembly wall)
        SB_GAUGEBUILD,   //          buildEnsembleGauges (Σz=0 gauges) — OFF (ensemble) in the tier
        SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors
                         //          (W·Pᶻ·Wᵀ dense DD-cov projection over K carried chords) [Card-4]
        SB_SOLVEGLUE,    //          pre-solve prep + clock predict + solve + within-epoch re-solve
                         //          (net removes the internal solve/reduce)
        SB_INSTALL,      //          carry_driver.installPosterior (DD posterior mean/cov readback) [Card-4]
        SB_NETAR,        //          producer-side network AR Phase-2 (log-only; OFF in the tier)
        SB_SOLPREP,      //          toSolutionEpoch + per-sat discontinuity signalling
        SB_EMITSTORE,    // block 7: emit re-base + PB-emit filter + store push + solution CSV
        SB_IONODIAG,     // block 4: iono / ZWD / anchor-info viz traces (per-station recon sweeps)
        SB_DDFRAC,       // block 6: cross-station DD fractional-residual (B5) trace
        SB_SHDEV_VTEC,   // block 4: T5 SH-deviation carry KF + co-estimated VTEC coeff trace
        SB_T1A,          // block 3: T1a SH-field health probe + fused H1 adaptive-deweight gate
        SB_LEVELING,     // block 5: phase-leveling carry harvest + D2/C1 stash + T1d SH-conv
        SB_RAIM,         // block 2: post-fit network RAIM pass-1/2 + median-polish attribution
        SB_CLOCKUPD,     // block 8: two-state clock update + fault QC + retro-flag feed
        SB_STASH_STATUS, // block 5/8: D2/C1/SH carry re-stash + VCE fold + epoch-status QC
        SB_COUNT
    };
    static constexpr const char* kSubNames[SB_COUNT] = {
        "slip-scans", "pre-asm", "gauge-build", "dd-build", "solve-glue", "install-post",
        "netar", "sol-prep", "emit-store", "iono-diag", "dd-frac", "shdev-vtec",
        "t1a-health", "leveling", "raim", "clock-upd", "stash-status"
    };
    struct SubBlockTimer {
        double      wall_ms{0.0};    // driver wall bracketing the block (run cumulative)
        double      timed_ms{0.0};   // Δ(reduce+solve+cov-inv+cov-consumers) inside it (subtract for net)
        double      covcon_ms{0.0};  // of which cov-consumers (reported alongside; the D1 lever)
        std::size_t covcalls{0};     // cov-consumer calls inside it
    };
    std::vector<SubBlockTimer> sec_sub(SB_COUNT);        // run totals
    std::vector<SubBlockTimer> sec_sub_ep0(SB_COUNT);    // per-epoch loop-top snapshot of sec_sub
    std::vector<std::chrono::steady_clock::time_point> sub_wall0(SB_COUNT);
    std::vector<NetworkNormalEquations::SectionTimers>  sub_tmr0(SB_COUNT);
    auto subBegin = [&](SubBlock b) {
        if (!sec_timers_on) return;
        sub_wall0[b] = std::chrono::steady_clock::now();
        sub_tmr0[b]  = est.sectionTimers();
    };
    auto subEnd = [&](SubBlock b) {
        if (!sec_timers_on) return;
        const auto& t = est.sectionTimers();
        sec_sub[b].wall_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sub_wall0[b]).count();
        sec_sub[b].timed_ms +=
            (t.reduce_ms        - sub_tmr0[b].reduce_ms)
          + (t.solve_ms         - sub_tmr0[b].solve_ms)
          + (t.cov_inverse_ms   - sub_tmr0[b].cov_inverse_ms)
          + (t.cov_consumers_ms - sub_tmr0[b].cov_consumers_ms);
        sec_sub[b].covcon_ms += t.cov_consumers_ms   - sub_tmr0[b].cov_consumers_ms;
        sec_sub[b].covcalls  += t.cov_consumer_calls - sub_tmr0[b].cov_consumer_calls;
    };

    // === RTB-7(a): state snapshot + warm resume ==============================
    // Entirely OUTSIDE the product-file replay path: the snapshot restores the
    // producer's in-memory cross-epoch state; NetworkProductIO replay still
    // reproduces every discontinuity counter with no snapshot involved.  Path
    // empty (default) ⇒ nothing below runs — byte-identical.
    std::filesystem::path snap_path;
    if (!nc.state_snapshot_path.empty()) {
        snap_path = nc.state_snapshot_path;
        if (snap_path.is_relative()) snap_path = log_dir_ / snap_path;
    }
    double   last_snapshot_write_s = -1.0;   // product time of the last snapshot write
    uint64_t live_fingerprint      = 0;
    if (!snap_path.empty()) {
        // Config fingerprint over the DATUM-relevant fields: resuming under a
        // changed S-basis/pivot pin, station roster (order defines the carry's
        // station indices), carry q's / gap bounds / floors, clock-model
        // parameters, or emitter channel config must COLD-start — the carried
        // state would be interpreted in the wrong frame or under the wrong rules.
        std::string fp;
        auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
        for (const auto& st : stations) { add(st.name); add(st.is_pivot ? "P" : "-"); }
        add(nc.clock_datum_station); add(nc.clock_datum_satellite);
        // The phase-bias gauge realization (single-pivot vs prioritized anchor)
        // defines which station's ambiguities carry the datum; a resume across a
        // toggle change would interpret the carried anchors in the wrong frame.
        add(fmt::format("{}", phase_datum_prioritized));
        // Fix A: the anchor-free ensemble datum references the receiver ENSEMBLE
        // (Σ_r z=0), a different carried-ambiguity frame than the single-anchor
        // pivot/prioritized datum (both of which set phase_datum_prioritized).
        // Appended ONLY when armed, so every pre-existing (non-ensemble) snapshot
        // fingerprint is unchanged (byte-identical).
        if (phase_datum_ensemble) add("phdatum:ens");
        // Card 3: the Eq-34 chain datum references a different carried-ambiguity
        // frame (per-receiver RPB split + spanning-tree pins) than the anchor/pivot
        // or ensemble datums; a resume across a toggle flip must cold-start.
        // Appended ONLY when armed, so every non-eq34 snapshot stays byte-identical.
        if (phase_datum_eq34) add("phdatum:eq34");
        // Card 4 (M1) OPEN-4: the invariant carry snapshots a DD z-store the legacy
        // carries don't have; a resume across a carry_invariant flip must cold-start.
        // Appended ONLY when armed, so every non-carry snapshot stays byte-identical.
        if (carry_invariant) add("carry:inv");
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        add(fmt::format("{}", pl_var_floor));    add(fmt::format("{}", pb_var_floor));
        add(fmt::format("{}", cb_var_floor));    add(fmt::format("{}", zwd_on));
        add(fmt::format("{}", zwd_q));           add(fmt::format("{}", clk_model_on));
        add(fmt::format("{}", clk_model_q1));    add(fmt::format("{}", clk_model_q2));
        add(fmt::format("{}", clk_model_k));     add(fmt::format("{}", clk_model_max_gap));
        add(fmt::format("{}", config_.networkEmitClockRate()));
        add(fmt::format("{}", ura_on));          add(fmt::format("{}", ura_alpha));
        add(fmt::format("{}", ura_retro));       add(fmt::format("{}", ura_refloat_inflate));
        add(fmt::format("{}", emit_clock_disc));
        add(fmt::format("{}", config_.networkEmitterStalenessHorizonS()));
        add(fmt::format("{}", sticky_pivot));    add(fmt::format("{}", scoped_handover));
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // Card 6: apriori_source + anchor σ are datum-relevant.  The carried clock
        // frame is Δδtˢ-vs-broadcast in brdc_rts but Δδtˢ-vs-precise in sp3 (differ by
        // ~metres), and only brdc_rts carries the relevel_iode baseline — so a warm
        // resume must NOT cross an apriori_source change or it would fold a clock carry
        // in the wrong frame and re-level against a stale baseline.
        add(fmt::format("{}", static_cast<int>(config_.networkAprioriSource())));
        add(fmt::format("{}", config_.networkAprioriClockAnchorSigmaM()));
        // The clock-gauge REALIZATION defines the frame the carried clock states
        // live in (pin / sum-to-zero / broadcast-anchored differ by per-const or
        // common-mode constants) — a resume across a realization change must
        // cold-start.  Appended ONLY when a non-default realization is armed so
        // every pre-existing (pin-mode) snapshot fingerprint is unchanged.
        if (clock_sum_to_zero || clock_brdc_anchored)
            add(fmt::format("clkdatum:s2z={},bca={}", clock_sum_to_zero,
                            clock_brdc_anchored));
        // Optional emitter discontinuity channels are construction parameters of
        // the snapshotted emitter state (restoreState hard-fails on a mismatch) —
        // same conditional-append rule keeps default fingerprints unchanged.
        if (config_.networkCodeBiasDiscJumpTest() || config_.networkClockDiscOnSetChange())
            add(fmt::format("emitchan:cbj={},cks={}", config_.networkCodeBiasDiscJumpTest(),
                            config_.networkClockDiscOnSetChange()));
        // The held clock signal basis defines which IF combination each carried
        // clock state is referenced to; a resume across a toggle change would carry
        // clock states in a re-selected-per-epoch frame vs a held frame.  Appended
        // ONLY when armed so every pre-existing snapshot fingerprint is unchanged.
        if (clock_signal_basis_hold)
            add(fmt::format("clkbasishold={}", clock_signal_basis_hold));
        // Excluding unmatched-antenna stations from phase-bias emission changes which
        // per-(sat,signal) phase-bias states the carried solution holds; a resume across a
        // toggle change would carry a different shared-state set.  Appended ONLY when armed
        // so every pre-existing snapshot fingerprint is unchanged.
        if (pb_excl_unmatched)
            add(fmt::format("pbexclunmatched={}", pb_excl_unmatched));
        // T2e: epoch decimation changes the carried-state cadence, so a resume must match it.
        // Conditional-append (only when != 1) keeps every existing config's fingerprint byte-
        // identical.  The DD-trace stride is TRACE-ONLY and deliberately NOT folded in — arming
        // a trace on resume must not invalidate an untraced snapshot.
        if (nc.epoch_decimation_n != 1)
            add(fmt::format("edecim={}", nc.epoch_decimation_n));
        // T5: the GM deviation changes the estimation (not trace-only), and its carry
        // is NOT persisted in the snapshot — a resume across a tau change must cold-start.
        if (shc.rep_error_tau_s > 0.0)
            add(fmt::format("shdevtau={}", shc.rep_error_tau_s));
        // Card 1: the conditional local carry changes what the per-station carry MEANS
        // (a shared-coupled conditional row vs an independent diagonal marginal) and the
        // per-column coupling data (M_r rows, ŝ_prev, P_prev) is NOT persisted in the v1
        // snapshot — a resume across a toggle change would fold a carry of the wrong kind.
        // Conditional-append (only when armed) keeps every existing fingerprint byte-identical.
        if (carry_cpl_cond)
            add(fmt::format("carrycplcond={}", carry_cpl_cond));
        // Card 2: the time-linked code-bias datum changes what the emitted OSB/clock
        // split MEANS (a time-continuity datum vs a per-epoch set-mean datum) and its
        // per-(const,signal) previous-value map is NOT persisted in the snapshot — a
        // resume across a toggle change would compare a solve in one datum against a
        // carry in the other.  Conditional-append (only when armed) keeps every
        // existing fingerprint byte-identical.
        if (cb_time_linked)
            add(fmt::format("cbtimelink={}", cb_time_linked));
        // Card 1d: the border time update changes what an injected coupling row MEANS
        // (a κ-shrunk, re-independenced pseudo-obs of the MOVED border vs the raw
        // harvest-epoch row) and the κ inputs (per-column P_prev + q ladder) are not
        // persisted in the v1 snapshot.  Conditional-append (only when armed) keeps
        // every existing fingerprint byte-identical.
        if (carry_cpl_tu)
            add(fmt::format("carrycpltu={}", carry_cpl_tu));
        // Card 1d-SH: marginalized-vs-carried SH coupling changes what an armed carry
        // MEANS; conditional-append keeps existing fingerprints byte-identical.
        if (carry_cpl_shmarg)
            add(fmt::format("cplshmarg={}", carry_cpl_shmarg));
        // Card 1b: JOINT-whitening reshapes the injected coupling rows (departed cols →
        // shared M_w marginalization vs a per-row scalar σ²) — a solve-determining
        // treatment on the SAME footing as carry_cpl_cond/_tu/_shmarg above, all of
        // which gate a resume.  It was the lone Card-1 coupling knob missing here, so a
        // warm resume flipping joint-whiten (with carry_cpl_cond still ON) matched
        // fingerprints and was permitted to proceed and diverge.  Conditional-append
        // (only when armed) keeps every non-joint fingerprint byte-identical; joint
        // requires carry_cpl_cond so an armed-joint config already carries carrycplcond.
        if (carry_cpl_joint)
            add(fmt::format("carrycpljoint={}", carry_cpl_joint));
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
    }
    if (!snap_path.empty() && std::filesystem::exists(snap_path)) {
        NetworkStateSnapshot snap;
        std::string err;
        if (!NetworkStateSnapshotIO::read(snap_path, snap, &err)) {
            log->warn("Network (RTB-7): state snapshot '{}' failed to parse ({}) — COLD START "
                      "(a new generation id is minted at first emit as usual).",
                      snap_path.string(), err);
        } else {
            // First epoch this run will actually process (honouring the
            // start_offset_hours fast-forward) — the age reference of the gate.
            double first_live_s = -1.0;
            if (!synced.empty()) {
                const double anchor_s = TimeUtils::continuousGpsSeconds(synced.front().time);
                for (const auto& sf : synced) {
                    const double t_s = TimeUtils::continuousGpsSeconds(sf.time);
                    if (start_offset_s <= 0.0 || (t_s - anchor_s) >= start_offset_s) {
                        first_live_s = t_s;
                        break;
                    }
                }
            }
            // Carry-validity horizon = max of the per-key gap bounds.  Past it
            // every restored key would be dropped/reseeded downstream anyway;
            // WITHIN it, per-key staleness after restore is handled by the
            // EXISTING machinery (local-carry max-gap drop, D2 q·dt inflation
            // over the true gap via the restored prev_week/prev_tow, clock-model
            // per-sat t_last gap logic) — restore everything, let the
            // established per-key logic judge each key.
            const double horizon_s = std::max(pl_max_gap_s, clk_model_max_gap);
            std::string gate;
            if (first_live_s < 0.0) {
                log->warn("Network (RTB-7): state snapshot '{}' found but there are no live "
                          "epochs to resume against — COLD START.", snap_path.string());
            } else if (!NetworkStateSnapshotIO::warmResumeAllowed(
                           snap, live_fingerprint, first_live_s, horizon_s, &gate)) {
                log->warn("Network (RTB-7): state snapshot '{}' NOT warm-resumable — {} — "
                          "COLD START (a new generation id is minted at first emit as usual).",
                          snap_path.string(), gate);
            } else {
                // Emitter layer FIRST (all-or-nothing): a failed emitter restore
                // discards the whole warm resume — restoring the driver carries
                // without the emitter's baselines/pending flags would emit WRONG
                // discontinuity counters (the silent consumer datum break).
                std::istringstream es(snap.emitter_state);
                if (!emitter.restoreState(es)) {
                    log->warn("Network (RTB-7): state snapshot '{}' passed the gate but its "
                              "EMITTER section failed to restore (corrupt or parameter "
                              "mismatch) — COLD START; driver state deliberately NOT "
                              "restored (the two layers only make sense together).",
                              snap_path.string());
                } else {
                    // --- Driver layer -----------------------------------------
                    prev_carry.clear();
                    prev_carry.reserve(snap.shared_carry.size());
                    for (const auto& sc : snap.shared_carry) {
                        CarryPrior cp;
                        cp.kind      = static_cast<SharedParamKind>(sc.kind);
                        cp.sat       = sc.sat;
                        cp.signal    = sc.signal;
                        cp.sh_n      = sc.sh_n;
                        cp.sh_m      = sc.sh_m;
                        cp.sh_trig   = static_cast<char>(sc.sh_trig);
                        cp.value     = sc.value;
                        cp.var       = sc.var;
                        cp.inflate_q = sc.inflate_q;
                        prev_carry.push_back(std::move(cp));
                    }
                    // C1: the v1 snapshot serializes per-key (value, var) only — the
                    // dense covariance block deliberately is not persisted.  The first
                    // post-resume fold therefore runs the diagonal path (identical
                    // legacy semantics); the dense block re-seeds at the first emit.
                    if (fullcov_on && !prev_carry.empty())
                        log->info("Network (C1): warm resume restored {} diagonal carries — "
                                  "the full-covariance carry re-seeds at the first emitted "
                                  "epoch.", prev_carry.size());
                    prev_pivot    = snap.prev_pivot;
                    last_pivot    = snap.last_pivot;
                    c1_zoh_active = snap.c1_zoh_active;
                    prev_clk      = snap.prev_clk;
                    clock_osc.clear();
                    for (const auto& [sat, o] : snap.clock_osc) {
                        ClockOsc c;
                        c.x0 = o.x0; c.x1 = o.x1;
                        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
                        c.t_last = o.t_last; c.init = o.init;
                        clock_osc[sat] = c;
                    }
                    clock_rate_est.clear();
                    for (const auto& [sat, o] : snap.clock_rate_est) {   // network.emit_clock_rate decoupled C1 state
                        ClockOsc c;
                        c.x0 = o.x0; c.x1 = o.x1;
                        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
                        c.t_last = o.t_last; c.init = o.init;
                        clock_rate_est[sat] = c;
                    }
                    local_carry.clear();
                    for (const auto& [key, l] : snap.local_carry)
                        local_carry[key] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    zwd_carry.clear();
                    for (const auto& [idx, l] : snap.zwd_carry)
                        zwd_carry[idx] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    ura_sigma0_ewma     = snap.ura_sigma0_ewma;
                    ura_refloat_horizon = snap.ura_refloat_horizon;
                    ura_retro_sats      = snap.ura_retro_sats;
                    retro_active        = snap.retro_active;
                    // Card 6: restore the eph-cutover re-level baseline + any pending
                    // forced disc so a warm resume does not re-level against a stale
                    // baseline (or lose a pending forced bump).  Empty in an sp3 or
                    // pre-Card-6 snapshot ⇒ the first brdc_rts epoch re-seeds cleanly.
                    relevel_iode             = snap.relevel_iode;
                    eph_cutover_disc_pending = snap.eph_cutover_disc_pending;
                    // Card 2: the time-linked code-bias previous-value map is NOT
                    // persisted, so it re-seeds empty here.  A satellite emitted before
                    // the snapshot whose split now re-seeds IS a re-reference the
                    // consumer must see — mark the run "resumed" so every first post-
                    // resume re-seed disc-bumps (conservative; a genuinely-new sat's
                    // first emit absorbs a harmless bump).  Inert unless code_datum_time_linked.
                    code_bias_datum_resumed_ = true;
                    // RTA-3 seam (the critical one): PRESERVE the snapshot's
                    // generation id.  The first-emit minting site only mints when
                    // generation_id == 0, so a restored non-zero id SUPPRESSES the
                    // mint — consumers keep the same product datum and never
                    // hard-reset.  (Same-config guarantee: emit_gen_id is in the
                    // fingerprint, so an id-less snapshot only warm-resumes an
                    // id-less run, where 0 is correct.)
                    generation_id = snap.generation_id;
                    iod           = snap.iod;
                    prev_week     = snap.prev_week;
                    prev_tow      = snap.prev_tow;
                    n_emitted           = static_cast<size_t>(snap.n_emitted);
                    n_skipped           = static_cast<size_t>(snap.n_skipped);
                    n_gate_withheld     = static_cast<size_t>(snap.n_gate_withheld);
                    n_sigma0_withheld   = static_cast<size_t>(snap.n_sigma0_withheld);
                    n_runaway_epochs    = static_cast<size_t>(snap.n_runaway_epochs);
                    n_perconst_partial_epochs    = static_cast<size_t>(snap.n_perconst_partial);
                    n_perconst_diffuse_published = static_cast<size_t>(snap.n_perconst_diffuse);
                    last_emit_content_s = snap.last_emit_content_s;
                    last_health_write_s = snap.last_health_write_s;
                    // RTB-7(b): ramp state restores — a warm-resumed producer is
                    // NOT degraded.  Already-lifted ⇒ grade 0 ("warm"); mid-ramp
                    // ⇒ the ramp CONTINUES from the restored counts (never
                    // restarts), with the grade anchored at the original first
                    // emit (honest total product time to grade).
                    ura_ewma_updates     = static_cast<size_t>(snap.ura_ewma_updates);
                    ramp_lifted          = snap.ramp_lifted;
                    first_emit_content_s = snap.first_emit_content_s;
                    if (nc.warmup_ura_floor_m > 0.0 && ura_on && ramp_lifted) {
                        grade_metric_s = 0.0;
                        log->info("Network (RTB-7): cold-start-to-product-grade: 0 s (warm "
                                  "resume — ramp already lifted before the bounce).");
                    }
                    const double snap_s =
                        static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
                    log->info("Network (RTB-7): WARM RESUME from '{}' (written {}) — snapshot "
                              "age {:.1f} s vs first live epoch (horizon {:.1f} s); generation "
                              "id {} PRESERVED (first-emit minting suppressed).  Restored: {} "
                              "D2 shared-carry, {} ambiguity-carry, {} ZWD-carry, {} clock-"
                              "oscillator, {} clock-monitor baseline, {}/{} pivot baseline "
                              "(prev/last-ever), {} pending retro-bump, {} refloat-horizon "
                              "entries; emitter stream state (counters, baselines, first-"
                              "sighting sets, pending bumps, last-seen stamps) restored; "
                              "n_emitted {} (carry stays warm).",
                              snap_path.string(), snap.wall_utc, first_live_s - snap_s,
                              horizon_s, generation_id, prev_carry.size(), local_carry.size(),
                              zwd_carry.size(), clock_osc.size(), prev_clk.size(),
                              prev_pivot.size(), last_pivot.size(), ura_retro_sats.size(),
                              ura_refloat_horizon.size(), n_emitted);
                }
            }
        }
    }

    // Card 5 (RTF-1 / plan D6): resolve the a-priori satellite-clock anchor σ ONCE
    // (loop-invariant, config-only) so the anchor fold and its B7 information-fraction
    // diagnostic below share one value.  The resolution policy (explicit > 0 wins;
    // brdc_rts auto → 3.0 m; sp3 auto → initial_std.sat_clock, unchanged) lives in
    // GnssConfig so it is a single source of truth and unit-testable in isolation.
    // The < 100 m binding gate at the fold site is unchanged and stays in the driver.
    const double clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();

    for (const auto& se : synced) {
        // Optional testing fast-forward (network.start_offset_hours): skip epochs until the
        // elapsed data span from the first synced epoch reaches the offset, then cold-start the
        // solve there.  Skipped epochs are not counted, modeled, solved, or stashed, so the
        // carry begins empty at the offset — reproduces an externally-driven late-hour event
        // (e.g. the ~4.3 h lock-in) after a fresh warm-up, not the exact accumulated history.
        if (start_offset_s > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) continue;
        }
        LogManager::setCurrentEpochTime(se.time);

        // Card 8 (replay parity): feed the staged broadcast nav records whose
        // availability (toc, GPS scale) this epoch has reached.  The store's
        // current/previous retention then evolves exactly as under live
        // arrival — IODE rolls happen at their true times (Card 6 re-levels
        // fire), and the fit-window guard sees fresh records instead of the
        // end-of-day pair a bulk load would leave.  The first processed epoch
        // catches up over any start_offset span in one sorted batch, which
        // ends in the identical store state as epoch-by-epoch feeding.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            while (brdc_nav_cursor < brdc_nav_staged.size()
                   && navAvailabilityGpsSeconds(brdc_nav_staged[brdc_nav_cursor]) <= now_s) {
                eph_store.insert(brdc_nav_staged[brdc_nav_cursor], /*from_igs=*/true);
                ++brdc_nav_cursor;
            }
        }

        // Optional max-duration cutoff (processing.max_duration_s): anchor on the
        // first synced epoch, stop once the elapsed data span reaches the limit.
        if (config_.getMaxDuration() > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
            else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
                log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                          config_.getMaxDuration());
                break;
            }
        }
        // T2e: epoch decimation — process only every Nth synced epoch (N=1 ⇒ modulo 0 always
        // ⇒ never skips ⇒ byte-identical).  Placed AFTER the start-offset + max-duration guards
        // (so the physical-time window/break stay evaluated on every raw epoch) and BEFORE
        // ++n_loop_epochs (so decimated-out epochs are not counted — n_loop_epochs stays the
        // processed-epoch count).  The brdc_rts nav catch-up is idempotent across skips.
        {
            const size_t dn = static_cast<size_t>(std::max(1, nc.epoch_decimation_n));
            if ((decim_counter++ % dn) != 0) continue;
        }
        ++n_loop_epochs;  // [TIMING]

        // D0: per-epoch section-timer baseline.  Deltas of the estimator's (run-
        // cumulative) accumulators against this snapshot give THIS epoch's reduce /
        // solve / cov-inverse / cov-consumer cost; sec_epoch_t0 anchors the epoch
        // wall; sec_asm_ms_epoch is filled at the assembly wall below.  Inert (no
        // clock read) when the toggle is OFF.
        NetworkNormalEquations::SectionTimers sec_t0;
        std::chrono::steady_clock::time_point sec_epoch_t0;
        double sec_asm_ms_epoch = 0.0;
        if (sec_timers_on) {
            sec_t0 = est.sectionTimers();
            sec_epoch_t0 = std::chrono::steady_clock::now();
            sec_sub_ep0 = sec_sub;   // D0b: per-epoch deltas of the sub-block run totals
        }

        // P1: size-based rotation of the unbounded service CSVs
        // (network.trace_rotate_mb > 0).  Checked once per processed epoch,
        // BEFORE this epoch's rows land, so a file overshoots by at most one
        // epoch of rows.  Each roll shifts file.csv.k → file.csv.(k+1) (oldest
        // deleted), retires the live file to file.csv.1, and reopens fresh with
        // the header re-written — every rolled generation parses standalone.
        // OFF (default 0) ⇒ maybeRotateCsv touches nothing (byte-identical).
        if (nc.trace_rotate_mb > 0.0) {
            const struct {
                std::ofstream*               os;
                const std::filesystem::path* path;
                const char*                  header;
            } rotating[] = {
                {&trace,        &trace_path,        kTraceHeader},
                {&soln,         &soln_path,         kSolnHeader},
                {&health_trace, &health_trace_path, kHealthHeader},
            };
            for (const auto& r : rotating)
                if (maybeRotateCsv(*r.os, *r.path, r.header,
                                   nc.trace_rotate_mb, nc.trace_rotate_keep))
                    log->info("Network (P1): rotated {} (live file exceeded {:.1f} MiB; "
                              "keeping {} rolled generation(s)).",
                              r.path->filename().string(), nc.trace_rotate_mb,
                              nc.trace_rotate_keep);
        }

        // RTB-6: operator health heartbeat every network.health_interval_s of content
        // time.  Reports cumulative state THROUGH the previous epoch (this epoch's
        // per-present/emit outcomes are computed below), which is exactly the "is the
        // service alive, emitting, and how contaminated/fast" signal a supervisor
        // polls.  Flushed per write so a live tail sees it immediately.  Guarded ⇒
        // OFF (default) writes nothing (byte-identical).
        if (health_trace.is_open()) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (last_health_write_s < 0.0
                || (now_s - last_health_write_s) >= nc.health_interval_s) {
                const double loop_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_loop_begin).count();
                // P4(b): mean wall ms per loop epoch over the window since the
                // PREVIOUS health row (first row: since loop start) — a windowed
                // "is it slowing down NOW" signal, not a cumulative run mean.
                const size_t win_epochs = n_loop_epochs - health_win_epochs_anchor;
                const double ms_per_epoch_window =
                    win_epochs > 0
                        ? (loop_ms - health_win_ms_anchor) / static_cast<double>(win_epochs)
                        : 0.0;
                const double emit_age =
                    last_emit_content_s >= 0.0 ? (now_s - last_emit_content_s) : -1.0;
                // P4(a): σ̂0-EWMA availability — the EWMA updates only inside the
                // warm post-fit RAIM block (emit_ura && raim && phase_leveling);
                // with any of them unarmed the column emits the −1 "unavailable"
                // sentinel, never the frozen 1.0 init.  (During the phase-leveling
                // warmup it still reads 1.0 — updates begin once carry_warm.)
                const bool ewma_avail = ura_on && raim_on && pl_on;
                health_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_emitted << ',' << n_skipped << ',' << n_gate_withheld
                             << ',' << n_sigma0_withheld << ',' << n_runaway_epochs << ','
                             << (ewma_avail ? fmt::format("{:.4f}", ura_sigma0_ewma)
                                            : std::string("-1"))
                             << ',' << last_n_present << ',' << last_n_clock_sats
                             << ',' << fmt::format("{:.3f}", ms_per_epoch_window)
                             << ',' << fmt::format("{:.3f}", emit_age)
                             // P2: realized per-constellation pivots of the last EMITTED
                             // product (the emit-baseline last_pivot map, merge-updated
                             // only at the emit site) — empty before the first emit.
                             << ',' << formatPivotMap(last_pivot)
                             // RTB-7(c): cold-start-to-product-grade (s of product time
                             // from first emit to the quality-ramp lift).  −1 before the
                             // lift or when the ramp is off; 0 = warm resume.
                             << ',' << (grade_metric_s >= 0.0
                                            ? fmt::format("{:.1f}", grade_metric_s)
                                            : std::string("-1"))
                             << '\n';
                health_trace.flush();   // a supervisor tails this live
                last_health_write_s      = now_s;
                health_win_ms_anchor     = loop_ms;
                health_win_epochs_anchor = n_loop_epochs;
            }
        }

        // L1: write one network_epoch_status.csv row for this epoch (guarded; no-op when OFF).
        auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
            if (!epoch_status_trace.is_open()) return;
            auto& o = epoch_status_trace;
            o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << disposition << ',';
            if (r.sigma0)          o << fmt::format("{:.4f}", *r.sigma0);
            o << ',';
            if (r.n_adapt)         o << *r.n_adapt;
            o << ',';
            if (r.n_present)       o << *r.n_present;
            o << ',';
            if (r.n_clock_sats)    o << *r.n_clock_sats;
            o << ',';
            if (r.clock_median_m)  o << fmt::format("{:.4f}", *r.clock_median_m);
            o << ',';
            if (r.clock_std_m)     o << fmt::format("{:.4f}", *r.clock_std_m);
            o << ',';
            o << r.worst_sat << ',';
            if (r.worst_dev_m)     o << fmt::format("{:.4f}", *r.worst_dev_m);
            o << ',';
            if (r.n_refloat_sats)  o << *r.n_refloat_sats;
            o << ',';
            if (r.n_retro_applied) o << *r.n_retro_applied;
            o << ',';
            if (r.datum_changed)   o << *r.datum_changed;
            o << ',';
            o << fmt::format("{:.4f}", es_sigma0_trigger) << ','
              << fmt::format("{:.4f}", es_withhold_ceiling) << ',';
            if (r.n_arc_refloat)   o << *r.n_arc_refloat;   // T2d
            o << '\n';
        };

        // L2: write one network_events.csv row for a discrete QC action (guarded; no-op OFF).
        auto emitEvent = [&](const char* tier, const char* event,
                             const std::string& sat, const std::string& station,
                             std::optional<double> statistic, std::optional<double> threshold,
                             const char* action, const std::string& detail) {
            if (!events_trace.is_open()) return;
            std::string d = detail;
            for (char& c : d) if (c == ',') c = ';';   // sanitize: keep the CSV single-field
            auto& o = events_trace;
            o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << tier << ',' << event << ',' << sat << ',' << station << ',';
            if (statistic) o << fmt::format("{:.6f}", *statistic);
            o << ',';
            if (threshold) o << fmt::format("{:.6f}", *threshold);
            o << ',' << action << ',' << d << '\n';
        };

        const double dt = (prev_tow < 0.0) ? 0.0
            : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

        // Phase-leveling carry timing (Increment 1): continuous-GPS seconds of this
        // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start
        // warmup has elapsed.  Count EMITTED product epochs (not raw loop epochs) so
        // the carry waits for the clock/SH convergence horizon — locking an
        // unconverged ambiguity onto a contaminated cold-start value would propagate
        // it (design §3.7).  n_emitted here excludes the current epoch (not yet emitted).
        const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
        // T1d: when convergence-gated warmup is armed AND an SH field is active, warm the
        // carry only once the field has gone stationary (sh_converged_latch, set post-solve
        // below from the epoch-over-epoch coefficient RMS Δ + a₀₀ physical band) — otherwise
        // the fixed emitted-epoch count.  Falls back to the epoch count when there is no SH
        // field to converge.  OFF (default) ⇒ the exact epoch-count gate ⇒ byte-identical.
        const bool   carry_warm = pl_on && ((pl_warmup_on_conv && sh_model)
                                                ? sh_converged_latch
                                                : (static_cast<int>(n_emitted) >= pl_warmup));

        // Card 4 (M1) C3c: this epoch's slip event feed for the DD invariant carry —
        // (app station index, ambiguity key) pairs the PASS-A slip detectors flagged.
        // Populated at the DETECTION site (below, in refloat_signal / refloat_pair)
        // rather than read from pending-reseed at fold time, because PASS-B
        // MeasurementModel::compute re-seeds and CLEARS the pending-reseed flag before
        // buildCarryDdPriors runs (the flag is a one-shot the assembly consumes).  Also
        // recorded REGARDLESS of carry_warm (the detectors run cold, and the DD carry is
        // live from epoch 1) so the carry is slip-protected through the whole warmup —
        // not only after the SH field's convergence gate releases.  carry_invariant-only.
        std::set<std::pair<int, std::string>> carry_slipped_ambs;
        // Card 4 (M1) D5: seed this epoch's slip set with the innovation gate's PREVIOUS-epoch
        // flags (a fine slip caught post-solve last epoch re-taps its arc fresh here, via the
        // same C3c membership-exclusion path the coarse detectors feed).  One-shot: consumed now.
        if (carry_innov_gate && !carry_innov_pending.empty()) {
            carry_slipped_ambs.insert(carry_innov_pending.begin(), carry_innov_pending.end());
            carry_innov_pending.clear();
        }

        // --- PASS A: per-station geometry + shared-state layout -------------
        std::vector<std::vector<PppObservation>> modeled(stations.size());
        std::vector<bool> present(stations.size(), false);

        // Legacy single-pivot datum: model the pivot FIRST so its usable phase
        // signals restrict the shared phase-bias columns to ones with a pivot-
        // receiver ambiguity pin, and an absent pivot sinks the epoch (no datum).
        // Prioritized mode treats the pivot as an ordinary station — modeled in the
        // tail loop from i_lo=0, no special skip — and elects the per-(sat,signal)
        // anchors AFTER the cross-station prune, so this block is skipped and the
        // pivot blinking out no longer costs the whole epoch (the SPOF cure).
        const int i_lo = phase_datum_prioritized ? 0 : 1;
        if (!phase_datum_prioritized) {
            NetStation& piv = stations.front();
            auto it = se.by_station.find(piv.name);
            if (it == se.by_station.end() || it->second.empty()) {
                // No datum station this epoch: no clean phase-bias gauge — skip.
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                emitEpochStatus("sync_skip", {});   // only week/tow available
                continue;
            }
            piv.pstate.epoch_time = se.time;
            if (!piv.model->computeModels(it->second, piv.pstate, modeled.front())
                || modeled.front().empty()) {
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                emitEpochStatus("sync_skip", {});   // only week/tow available
                continue;
            }
            piv.mm->ensureNetworkSharedStates(modeled.front());  // allow all pivot phase signals
            present.front() = true;
        }

        // Legacy: the pivot's allocated phase-bias keys ARE the shared set.
        // Prioritized: seeded empty here; the anchor election after the cross-
        // station observability prune rebuilds it as the anchored UNION over
        // present stations (each key backed by exactly one elected pin).
        std::set<std::string> shared_phase_keys;
        if (!phase_datum_prioritized)
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);

        // PASS A tail (stations[1..]) — PARALLEL over stations when enabled.
        // computeModels + ensureNetworkSharedStates touch ONLY station i's own
        // sm/model/mm/pstate; shared_phase_keys is read-only here and modeled[i]
        // is a distinct slot per i — no shared write.  Each iteration catches its
        // own exceptions: an exception must never escape an OpenMP structured
        // block (UB).  present[] is std::vector<bool> (packed bits → concurrent
        // writes to adjacent indices race), so threads record into a per-index
        // char buffer that is folded into present[] serially afterwards.
        subBegin(SB_SLIP);   // D0b block 1: PASS-A slip scans (pre-assembly)
        {
            std::vector<char>        a_ok(stations.size(), 0);
            std::vector<std::string> a_err(stations.size());
            // Phase-leveling T0 (Increment 2): per-station integer slip repairs,
            // produced in the parallel loop into a private slot, applied to the
            // shared local_carry in a SERIAL station-ordered drain below.
            std::vector<std::vector<TurboEditResolver::SignalRepair>>
                sta_repairs(stations.size());
            // IF-phase slip detector output: per-station flagged IF-pair reset keys
            // ("G01_L1C_L2W"), produced station-local in the parallel loop, consumed
            // in a serial station-ordered marking pass below (deterministic).
            std::vector<std::vector<std::string>> sta_if_resets(stations.size());
            // MW DETECTOR (mw_detect) output: confident wide-lane integer slips to REFLOAT,
            // as (satellite, sig1, sig2) tuples produced station-local in the parallel loop
            // and consumed in the serial MW-detect drain below.  Empty when turbo_edit runs
            // the integer-repair path instead, or when mw_detect is off.
            std::vector<std::vector<std::tuple<std::string, std::string, std::string>>>
                sta_mw_refloat(stations.size());

            #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
            for (int i = i_lo; i < n_sta_total; ++i) {
                try {
                    NetStation& st = stations[i];
                    auto it = se.by_station.find(st.name);
                    if (it == se.by_station.end() || it->second.empty()) continue;
                    st.pstate.epoch_time = se.time;
                    if (!st.model->computeModels(it->second, st.pstate, modeled[i])
                        || modeled[i].empty())
                        continue;
                    // Prioritized: every station (incl. the former pivot, i=0)
                    // allocates ALL its own trackable phase signals — the anchored
                    // shared set is the post-election union, not a pivot restriction.
                    if (phase_datum_prioritized)
                        st.mm->ensureNetworkSharedStates(modeled[i]);
                    else
                        st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys);
                    a_ok[i] = 1;

                    // T0 slip detection: roll this station's MW + GF arcs forward and
                    // collect per-(sat,signal) integer repairs.  Station-local detector
                    // state — no shared write, safe in the parallel region.  Run EVERY
                    // epoch (the resolver MUST advance its GF arcs even with no slips)
                    // and regardless of the carry warmup, so the adaptive noise floors
                    // are warm when the carry activates.  A detection failure costs only
                    // this station's repairs this epoch, never the station itself.
                    if (st.mw_slip) {
                        try {
                            const auto mwr = st.mw_slip->detectLargeMwResets(modeled[i]);
                            if (st.turbo_edit) {
                                // turbo_edit (1 s data): MW+GF → integer REPAIR of the carry.
                                auto er = st.turbo_edit->resolve(
                                    mwr, st.mw_slip->getEpochCleanPairs(), modeled[i], dt,
                                    st.mw_slip->getAcrossGapWlEvidence(pl_now_s));
                                sta_repairs[i] = std::move(er.repairs);
                            } else if (config_.networkCycleSlipMwDetect()) {
                                // mw_detect (any rate): detect-only → REFLOAT confident
                                // wide-lane integer jumps (no GF, no integer applied).  frac
                                // gates rounding cleanliness — a BIA-less float re-init rounds
                                // to a large spurious integer but with a large frac (LEFT); a
                                // KNOWN non-zero integer with small frac is a real WL slip.
                                // LLI loss-of-lock is handled directly in the serial drain.
                                // (Explicitly mw_detect-gated: under mw_wl-only the detector is
                                // built for its Welford means but must NOT refloat the carry —
                                // the WL layer is observation-side only.  Byte-identical for
                                // existing configs: the detector was previously built only when
                                // turbo_edit OR mw_detect, so this branch already implied mw_detect.)
                                const double frac_max = config_.networkCycleSlipTurboEditFracMax();
                                for (const auto& r : mwr) {
                                    if (r.slip_wl_known && r.slip_wl_cycles != 0
                                        && r.slip_frac <= frac_max
                                        && !r.sig1.empty() && !r.sig2.empty())
                                        sta_mw_refloat[i].emplace_back(r.satellite, r.sig1, r.sig2);
                                }
                            }
                        } catch (...) { /* best-effort: no MW action this epoch */ }
                    }

                    // IF-phase triple-difference slip detection: between-satellite,
                    // between-epoch IF-phase OMC.  Station positions are fixed at
                    // near-truth, so position_sigma_3d = 0 disables the pre-convergence
                    // gate (the sole reason this detector was staged) and st.ecef makes
                    // the inter-epoch geometry correction a no-op (Δpos ≡ 0).  Detector
                    // state is station-local — no shared write, safe here.  Best-effort:
                    // a failure costs only this station's flags this epoch.
                    if (st.if_slip) {
                        try {
                            sta_if_resets[i] = st.if_slip->detect(modeled[i], 0.0, st.ecef);
                        } catch (...) { /* best-effort: no IF-phase flags this epoch */ }
                    }
                } catch (const std::exception& ex) {
                    a_err[i] = ex.what();
                } catch (...) {
                    a_err[i] = "unknown exception";
                }
            }

            // Serial drain: commit present[] + log diagnostics in station order.
            // i_lo folds in station 0 too under the prioritized datum (it is an
            // ordinary station there; on the legacy path i_lo==1 and station 0's
            // present was already set by the pivot-first block above).
            for (int i = i_lo; i < n_sta_total; ++i) {
                if (a_ok[i]) present[i] = true;
                else if (!a_err[i].empty())
                    log->warn("Network: station '{}' PASS A geometry threw ({}); "
                              "skipping it this epoch", stations[i].name, a_err[i]);
            }

            // Phase-leveling T0 (serial, station-ordered → deterministic), applied
            // BEFORE PASS-B reads the carry to build the prior.
            //   • REPAIR  — a confident, sane-magnitude integer (frac ≤ frac_max AND
            //     |dN| ≤ dn_max): shift the carried value continuously
            //     (value_abs += dN·λ, variance unchanged, covariance-preserving).
            //   • LEAVE   — a less-confident emitted repair (frac/magnitude reject):
            //     do nothing.  This is the data-driven choice for the BIA-less network:
            //     the float wide-lane's emitted-but-low-confidence repairs are
            //     predominantly FALSE positives (e.g. a re-initialising arc reporting
            //     dN=675 at frac≈0.27), so acting on them — REFLOAT or DROP — discards
            //     a GOOD carry and measurably degrades the products, whereas leaving
            //     them matches carry-only.  The residual lock-in risk on a RARE
            //     real-but-rejected slip is bounded by phase_leveling_var_floor_m2 and
            //     the max-gap drop, and is properly resolved by Increment 3's
            //     cross-station RAIM consensus (which CAN tell a real slip from a
            //     single-station false positive — something a per-station T0 cannot).
            //     Wiring external BIA into the network MW would make the wide-lane
            //     integer trustworthy and let this become a REFLOAT.
            // The resolver's `reset_sats` is likewise NOT acted on (BIA-less, it is a
            // liberal catch-all of tens of sats/epoch).  The pivot (stations[0]) is
            // never touched (its gauge-pinned ambiguities are Increment-4 territory).
            int n_t0_repairs = 0, n_t0_left = 0;
            std::string t0_detail;
            const double t0_frac_max = config_.networkCycleSlipTurboEditFracMax();
            const int    t0_dn_max   = config_.networkCycleSlipTurboEditDnMax();
            for (int i = i_lo; i < n_sta_total; ++i) {
                if (!present[i]) continue;
                for (const auto& r : sta_repairs[i]) {
                    if (r.dN == 0 || !(std::isfinite(r.lambda) && r.lambda > 0.0)) continue;
                    auto it = local_carry.find(std::make_tuple(i, r.satellite, r.signal));
                    if (it == local_carry.end()) continue;
                    const int adn = (r.dN < 0) ? -r.dN : r.dN;
                    if (r.frac <= t0_frac_max && adn <= t0_dn_max) {
                        it->second.value_abs += static_cast<double>(r.dN) * r.lambda;  // REPAIR
                        ++n_t0_repairs;
                        if (n_t0_repairs <= 12)
                            t0_detail += fmt::format("{}{}/{}/{} dN={} f={:.2f}",
                                                     t0_detail.empty() ? "" : ", ",
                                                     stations[i].name, r.satellite, r.signal,
                                                     r.dN, r.frac);
                    } else {
                        ++n_t0_left;  // LEAVE (untrusted integer — likely a float-MW false positive)
                        if (n_t0_left <= 6)
                            log->debug("Network[phase-leveling T0]: TOW {:.1f} LEFT low-confidence "
                                       "{}/{}/{} dN={} frac={:.2f} (untrusted; carry kept)",
                                       se.time.tow, stations[i].name, r.satellite, r.signal,
                                       r.dN, r.frac);
                    }
                }
            }
            if (n_t0_repairs > 0 || n_t0_left > 0)
                log->info("Network[phase-leveling T0]: TOW {:.1f} applied {} repair(s) [{}]; "
                          "left {} low-confidence detection(s) untouched",
                          se.time.tow, n_t0_repairs, t0_detail, n_t0_left);

            // MW DETECTOR (mw_detect) → refloat: the iono-safe complement to IF-phase TD.
            // Two signals, neither of which uses the geometry-free ionosphere (so both stay
            // valid at the 30 s product rate, unlike the TurboEdit MW+GF integer REPAIR):
            //   (a) a CONFIDENT wide-lane integer jump collected in PASS A (sta_mw_refloat);
            //   (b) the receiver's own LLI loss-of-lock flag (bit 0) on a carried signal —
            //       the ground truth a receiver sets on a cycle slip / re-acquisition (this
            //       is what a real slip like G23@GAMB carries when MW/IF can't see it).
            // Either REFLOATS the affected ambiguity (marks it pending-reseed so PASS B
            // re-estimates it from this epoch's data) — never applying an integer, so a wrong
            // wide-lane integer can never bias the gauge.  Serial, station-ordered →
            // deterministic.  The refloat is meaningful only when the carry is warm; cold, the
            // detection is diagnostic (the free per-epoch estimate needs no reseed).  Pivot
            // (stations[0]) is never touched (its ambiguities are gauge-pinned).
            if (config_.networkCycleSlipMwDetect()) {
                auto refloat_signal = [&](int i, const std::string& sat,
                                          const std::string& signal) -> bool {
                    bool any = false;
                    StateManager& sm = *stations[i].sm;
                    for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                        if (!parts || parts->satellite != sat) continue;
                        if (parts->signal == signal) {
                            // Card 4 C3c: feed the DD invariant carry from epoch 1,
                            // independent of the legacy carry's warm gate (the DD carry is
                            // live throughout; pending-reseed cannot serve it — PASS-B
                            // MeasurementModel consumes+clears it before the fold).
                            if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                            // Legacy phase-leveling refloat: only meaningful once warm.
                            if (carry_warm) { sm.markAmbiguityPendingReseed(amb_key); any = true; }
                        }
                    }
                    return any;
                };
                int n_mw = 0, n_lli = 0;
                std::string mw_detail;
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i]) continue;
                    // (a) confident wide-lane integer slips detected in PASS A.  A satellite
                    // can slip on several signal-pairs at once (multi-freq); refloat every
                    // band but count / log / emit ONCE per (station, satellite).
                    std::set<std::string> mw_done;
                    for (const auto& [sat, sig1, sig2] : sta_mw_refloat[i]) {
                        const bool a = refloat_signal(i, sat, sig1);
                        const bool b = refloat_signal(i, sat, sig2);
                        if ((a || b) && mw_done.insert(sat).second) {
                            emitEvent("T0_mw_detect", "mw_wl_slip", sat, stations[i].name,
                                      std::nullopt, std::nullopt, "refloat", sig1 + ";" + sig2);
                            if (++n_mw <= 12)
                                mw_detail += fmt::format("{}{}/{}", mw_detail.empty() ? "" : ", ",
                                                         stations[i].name, sat);
                        }
                    }
                    // (b) receiver LLI loss-of-lock (bit 0) on any carried phase signal.
                    // modeled[i] holds one entry per satellite; refloat every LLI'd band but
                    // count / emit ONCE per satellite (an LLI on both L1 and L2 is one slip).
                    for (const auto& obs : modeled[i]) {
                        bool refloated = false;
                        for (const auto& [code, lli] : obs.lli_flags) {
                            if (code.empty() || code[0] != 'L' || !(lli & 1)) continue;
                            if (refloat_signal(i, obs.satellite, code)) refloated = true;
                        }
                        if (refloated) {
                            emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                                      stations[i].name, std::nullopt, std::nullopt,
                                      "refloat", "LLI");
                            ++n_lli;
                        }
                    }
                }
                if (n_mw > 0 || n_lli > 0)
                    log->info("Network[MW detect]: TOW {:.1f} refloated {} wide-lane slip(s){} + "
                              "{} LLI loss-of-lock(s)", se.time.tow, n_mw,
                              mw_detail.empty() ? std::string{} : (" [" + mw_detail + "]"), n_lli);
            }

            // IF-phase triple-difference slip detection + refloat (this increment).
            // Each non-pivot station's detector ran in DEFERRED mode above: it computed
            // the between-satellite, between-epoch ionosphere-free phase double
            // differences (ΔΔφ_IF) per (sat, signal-pair) and cached them, but did NOT
            // flag.  The driver decides, elevation-aware and per (station, signal-pair):
            //
            //  1. CENTER — pool ΔΔφ_IF across ALL present non-pivot stations BY SIGNAL
            //     PAIR, take the median `med` (raw m): the network common-mode (frame /
            //     datum wobble), which is elevation-independent, so it is removed in raw
            //     space.
            //  2. SCALE — normalize each residual to a zenith-equivalent, z = (dd−med)·
            //     sin(el) = (dd−med)/ifElevMap, so satellites at every elevation share one
            //     scale.  The pooled s_pool = 1.4826·median(|z|) is the warmup PRIOR; each
            //     (station, pair) also keeps an EWMA of its OWN z-scale (if_pair_scale).
            //     The effective scale shrinks from the pool toward the station's EWMA as
            //     that station-pair accumulates clean samples (empirical-Bayes, weight
            //     count/(count+κ)) — so a quiet reference station and a noisy modernized-
            //     signal station each get their OWN envelope, not one pooled threshold set
            //     by the majority (the fix for the L5Q/L2L over-firing).
            //  3. THRESHOLD sat i — max(k·s_eff·ifElevMap(el_i), floor_frac·λ_IF(1,1)):
            //     the empirical term re-inflated to sat i's elevation, the physical
            //     (1,1)-integer floor kept in raw m so a real slip is caught at ANY
            //     elevation.  Flag if |dd_i − med| exceeds it.
            //  4. A flagged arc has its detector gradient reset (fresh arc next epoch —
            //     arc hygiene, every epoch) and, when the carry is active, BOTH band
            //     ambiguities marked pending-reseed so PASS B refloats them.  Clean
            //     survivors then update the station-pair EWMA (slips excluded, so the
            //     scale never absorbs the event it must catch).
            //
            // The detector's own reference-satellite common-mode slip keys (which the
            // per-pair pool cannot see) are folded in too.  Thresholds are deliberately
            // loose; the sharp pass is a future post-estimation phase validation, not yet
            // built.  Serial, station-ordered → deterministic.  Pivot has no detector.
            if (config_.networkCycleSlipIfPhaseTd()) {
                const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
                const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
                const int    min_pool     = config_.networkCycleSlipIfPhaseTdMinPool();
                const double ewma_alpha   = config_.networkCycleSlipIfPhaseTdEwmaAlpha();
                const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

                // Refloat both band ambiguities of (sat, sig1, sig2) at station i (only
                // when the carry is active; cold, the detection is diagnostic only).
                auto refloat_pair = [&](int i, const std::string& sat,
                                        const std::string& sig1, const std::string& sig2) {
                    StateManager& sm = *stations[i].sm;
                    for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                        if (!parts || parts->satellite != sat) continue;
                        if (parts->signal == sig1 || parts->signal == sig2) {
                            // Card 4 C3c: feed the DD carry from epoch 1 (see refloat_signal).
                            if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                            if (carry_warm) sm.markAmbiguityPendingReseed(amb_key);
                        }
                    }
                };

                // Pool this epoch's ΔΔφ_IF by signal pair across present non-pivot stations.
                // Carry elevation (for the stochastic normalization) and the per-sat (1,1)
                // IF wavelength (for the physical floor — exact even for GLONASS FDMA).
                struct PairSample { int sta; std::string sat, sig1, sig2; double dd, el, lambda; };
                std::map<std::string, std::vector<PairSample>> by_pair;
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i] || !stations[i].if_slip) continue;
                    for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                        if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                        if (jp.sig1.empty() || jp.sig2.empty()) continue;
                        const std::string pair = jp.sig1 + "_" + jp.sig2;
                        by_pair[pair].push_back({i, sat, jp.sig1, jp.sig2,
                                                 jp.delta_grad_m, jp.elevation_deg, jp.lambda_if_m});
                    }
                }

                int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;
                std::string if_detail;
                for (auto& [pair, samples] : by_pair) {
                    // (1) Network common-mode center: median of the raw DDs (m).
                    std::vector<double> raw;
                    raw.reserve(samples.size());
                    for (const auto& s : samples) raw.push_back(s.dd);
                    std::nth_element(raw.begin(), raw.begin() + raw.size() / 2, raw.end());
                    const double med = raw[raw.size() / 2];

                    // (2) Pooled elevation-normalized robust scale (zenith-equiv m):
                    //     s_pool = 1.4826·median(|z|), z = (dd−med)/ifElevMap(el).
                    std::vector<double> zabs;
                    zabs.reserve(samples.size());
                    for (const auto& s : samples)
                        zabs.push_back(std::abs((s.dd - med) / ifElevMap(s.el)));
                    std::nth_element(zabs.begin(), zabs.begin() + zabs.size() / 2, zabs.end());
                    const double s_pool = 1.4826 * zabs[zabs.size() / 2];
                    if (static_cast<int>(samples.size()) < min_pool) ++n_sparse;

                    // Group the pair's samples by station (ordered → deterministic) so each
                    // station-pair thresholds with its OWN shrunk scale, then updates its
                    // OWN EWMA once this epoch's flags are known.
                    std::map<int, std::vector<const PairSample*>> by_sta;
                    for (const auto& s : samples) by_sta[s.sta].push_back(&s);

                    for (auto& [sta, plist] : by_sta) {
                        auto& sc = stations[sta].if_pair_scale[pair];

                        // Empirical-Bayes shrinkage: pool prior (cold) → station EWMA (warm).
                        const double w = static_cast<double>(sc.count) /
                                         (static_cast<double>(sc.count) + shrink_kappa);
                        double s_eff = (sc.count > 0) ? (w * sc.ewma_scale + (1.0 - w) * s_pool)
                                                      : s_pool;
                        if (!(s_eff > 0.0)) s_eff = s_pool;  // degenerate all-equal pool
                        if (sc.count >= static_cast<long>(shrink_kappa)) ++n_warm_sp;

                        std::vector<double> clean_zabs;
                        for (const PairSample* sp : plist) {
                            const double emap    = ifElevMap(sp->el);
                            const double floor_m = floor_frac * sp->lambda;
                            const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                            const double abs_dev = std::abs(sp->dd - med);
                            const bool   flagged = (abs_dev > thr);
                            if (trace_flag_rates) { ++fr_if_slip.opp; if (flagged) ++fr_if_slip.flag; }  // B8/T1
                            if (flagged) {
                                stations[sp->sta].if_slip->resetSatellite(sp->sat);  // restart arc
                                refloat_pair(sp->sta, sp->sat, sp->sig1, sp->sig2);
                                emitEvent("T1_if_slip", "if_slip", sp->sat,
                                          stations[sp->sta].name, abs_dev, thr,
                                          carry_warm ? "refloat" : "diagnostic",
                                          sp->sig1 + ";" + sp->sig2);
                                ++n_if_arcs;
                                if (n_if_arcs <= 12)
                                    if_detail += fmt::format("{}{}/{}_{}_{}", if_detail.empty() ? "" : ", ",
                                                             stations[sp->sta].name, sp->sat, sp->sig1, sp->sig2);
                            } else {
                                clean_zabs.push_back(abs_dev / emap);
                            }
                            // Deep-investigation row: the full decision context for this
                            // (station, sat, pair) sample.  sc.ewma_scale / sc.count are the
                            // decision-time (pre-update) values used to form s_eff.
                            if (if_slip_trace.is_open())
                                if_slip_trace
                                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                                    << stations[sp->sta].name << ',' << sp->sat << ','
                                    << sp->sig1 << ',' << sp->sig2 << ','
                                    << fmt::format("{:.2f}", sp->el) << ','
                                    << fmt::format("{:.6f}", sp->dd) << ',' << fmt::format("{:.6f}", med) << ','
                                    << fmt::format("{:.6f}", s_pool) << ',' << fmt::format("{:.6f}", sc.ewma_scale) << ','
                                    << sc.count << ',' << fmt::format("{:.3f}", w) << ','
                                    << fmt::format("{:.6f}", s_eff) << ',' << fmt::format("{:.3f}", emap) << ','
                                    << fmt::format("{:.6f}", thr) << ',' << fmt::format("{:.6f}", floor_m) << ','
                                    << fmt::format("{:.6f}", abs_dev) << ',' << (flagged ? 1 : 0) << ','
                                    << (carry_warm ? 1 : 0) << '\n';
                        }

                        // (4) Fold clean survivors into the station-pair EWMA (slips excluded).
                        if (!clean_zabs.empty()) {
                            std::nth_element(clean_zabs.begin(),
                                             clean_zabs.begin() + clean_zabs.size() / 2, clean_zabs.end());
                            const double s_epoch = 1.4826 * clean_zabs[clean_zabs.size() / 2];
                            sc.ewma_scale = (sc.count == 0)
                                ? s_epoch
                                : (1.0 - ewma_alpha) * sc.ewma_scale + ewma_alpha * s_epoch;
                            sc.count += static_cast<long>(clean_zabs.size());
                        }
                    }
                }

                // Reference-satellite common-mode slips (per station; pool can't see them).
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i]) continue;
                    for (const auto& rkey : sta_if_resets[i]) {
                        std::string sat, sig1, sig2;
                        if (!parseIfPairResetKey(rkey, sat, sig1, sig2)) continue;
                        refloat_pair(i, sat, sig1, sig2);
                        emitEvent("T1_if_slip", "refsat_reset", sat, stations[i].name,
                                  std::nullopt, std::nullopt,
                                  carry_warm ? "refloat" : "diagnostic", sig1 + ";" + sig2);
                        ++n_if_arcs;
                        if (n_if_arcs <= 12)
                            if_detail += fmt::format("{}{}/{} (refsat)", if_detail.empty() ? "" : ", ",
                                                     stations[i].name, rkey);
                    }
                }

                if (n_if_arcs > 0)
                    log->info("Network[IF-phase slip]: TOW {:.1f} flagged {} arc(s){} across {} pair(s) "
                              "({} station-pair(s) warm){}{}",
                              se.time.tow, n_if_arcs,
                              carry_warm ? " for refloat" : " (carry cold; diagnostic)",
                              by_pair.size(), n_warm_sp,
                              n_sparse > 0 ? fmt::format(", {} pair(s) under min_pool({})", n_sparse, min_pool)
                                           : std::string{},
                              if_detail.empty() ? std::string{} : (" [" + if_detail + "]"));
            }
        }
        subEnd(SB_SLIP);       // D0b: close block 1 (slip scans)
        subBegin(SB_PREASM);   // D0b: pre-assembly measurement build (post-slip → assembly wall)

        // RAW cycle-slip observables (network.trace_slip_raw), computed straight from
        // raw_observed — INDEPENDENT of whether any slip detector is armed.  Written BEFORE
        // the observability pre-filter so a satellite about to be dropped is still captured.
        // For each present station+satellite pick the primary dual-frequency pair (two lowest
        // DISTINCT bands whose phase AND matching code "C"+sig[1:] are present) and emit MW,
        // GF (=L1−L2), first-signal phase-minus-code (all m) plus the per-band RINEX LLI.  The
        // factor c cancels out of MW/GF, so wavelengths alone (FDMA-correct via the satellite)
        // suffice.  Absolute values — diff adjacent epochs offline for the slip signature.
        if (slip_raw_trace.is_open() && slip_raw_ppp_utils) {
            for (int i = 0; i < n_sta_total; ++i) {
                if (!present[i]) continue;
                for (const auto& obs : modeled[i]) {
                    std::vector<std::string> ph;  // phase signals with a matching code
                    for (const auto& kv : obs.raw_observed) {
                        const std::string& k = kv.first;
                        if (k.size() < 2 || k[0] != 'L') continue;
                        if (obs.raw_observed.count("C" + k.substr(1))) ph.push_back(k);
                    }
                    if (ph.empty()) continue;
                    std::sort(ph.begin(), ph.end(),
                              [](const std::string& a, const std::string& b) { return a[1] < b[1]; });
                    const std::string s1 = ph.front();
                    std::string s2;
                    for (const auto& k : ph) if (k[1] != s1[1]) { s2 = k; break; }  // distinct band

                    const double lam1 = slip_raw_ppp_utils->getWavelength(obs.system, s1, obs.satellite);
                    if (!(std::isfinite(lam1) && lam1 > 0.0)) continue;
                    const double L1_cyc = obs.raw_observed.at(s1);
                    const double P1_m   = obs.raw_observed.at("C" + s1.substr(1));
                    const double L1_m   = L1_cyc * lam1;
                    const double l1c1   = L1_m - P1_m;
                    const int    lli1   = obs.lli_flags.count(s1) ? obs.lli_flags.at(s1) : 0;

                    bool dual = !s2.empty();
                    double mw = 0.0, gf = 0.0;
                    int    lli2 = 0;
                    if (dual) {
                        const double lam2 = slip_raw_ppp_utils->getWavelength(obs.system, s2, obs.satellite);
                        if (std::isfinite(lam2) && lam2 > 0.0) {
                            const double inv1 = 1.0 / lam1, inv2 = 1.0 / lam2;
                            const double L2_cyc = obs.raw_observed.at(s2);
                            const double P2_m   = obs.raw_observed.at("C" + s2.substr(1));
                            gf = L1_m - L2_cyc * lam2;
                            mw = (L1_cyc - L2_cyc) / (inv1 - inv2)
                               - (P1_m * inv1 + P2_m * inv2) / (inv1 + inv2);
                            lli2 = obs.lli_flags.count(s2) ? obs.lli_flags.at(s2) : 0;
                        } else {
                            dual = false;
                        }
                    }
                    slip_raw_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << stations[i].name << ',' << obs.satellite << ','
                        << s1 << ',' << (dual ? s2 : std::string{}) << ','
                        << fmt::format("{:.2f}", obs.elevation) << ','
                        << (dual ? fmt::format("{:.4f}", mw) : std::string{}) << ','
                        << (dual ? fmt::format("{:.4f}", gf) : std::string{}) << ','
                        << fmt::format("{:.4f}", l1c1) << ',' << lli1 << ',' << lli2 << '\n';
                }
            }
        }

        // --- Cross-station observability pre-filter -------------------------
        // A satellite clock is only well-determined for a network product when at
        // least kMinStationsPerSat stations observe it: a single-station satellite
        // clock is tied to just that one receiver clock and, at marginal geometry,
        // becomes the extra near-zero direction that trips the rank gate.  Drop
        // such satellites from the network ENTIRELY this epoch — both the shared
        // clock/phase-bias states AND the underlying observations.  Excluding the
        // observations is essential: a satellite kept in the design with NO shared
        // clock column would pin its station's receiver clock absolutely and so
        // erase the very clock gauge the datum is meant to pin (declared > observed
        // → the gate would then reject every epoch).  Dropping the whole satellite
        // lets one weak satellite no longer force the WHOLE epoch to be withheld;
        // the gate stays as the final safety net for a genuine residual deficiency.
        constexpr int kMinStationsPerSat = 2;
        int n_dropped_sats = 0;
        {
            std::map<std::string, int> sat_station_count;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatClockStates())
                    ++sat_station_count[StateKeys::satelliteFromSatClockKey(kv.first)];
            }
            std::set<std::string> dropped_sats;
            for (const auto& kv : sat_station_count)
                if (kv.second < kMinStationsPerSat) { dropped_sats.insert(kv.first); ++n_dropped_sats; }

            if (!dropped_sats.empty()) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!present[i]) continue;
                    // Remove the dropped satellites' observations so their rows are
                    // not built (and so cannot pin the receiver clock).
                    auto& obs = modeled[i];
                    obs.erase(std::remove_if(obs.begin(), obs.end(),
                                  [&](const PppObservation& o) { return dropped_sats.count(o.satellite) != 0; }),
                              obs.end());
                    // Prune the now-orphaned shared states.
                    StateManager& sm = *stations[i].sm;
                    std::set<std::string> keep_clk, keep_pb;
                    for (const auto& kv : sm.getSatClockStates())
                        if (!dropped_sats.count(StateKeys::satelliteFromSatClockKey(kv.first)))
                            keep_clk.insert(kv.first);
                    for (const auto& kv : sm.getSatPhaseBiasStates()) {
                        const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                        if (p && !dropped_sats.count(p->satellite)) keep_pb.insert(kv.first);
                    }
                    sm.pruneSatClockStatesExcept(keep_clk);
                    sm.pruneSatPhaseBiasStatesExcept(keep_pb);
                }
            }
        }
        // The final shared phase-bias set + which station anchors (gauge-pins) each
        // (sat,signal).  Two realizations of the SAME minimum constraint (exactly
        // one ambiguity pin per phase-bias freedom):
        //   Legacy (network.phase_datum_prioritized OFF): the single pivot's pruned
        //     phase-bias states ARE the shared set (each backed by a pivot-receiver
        //     pin).  Byte-identical.
        //   Prioritized: elect, per (sat,signal), the highest-coverage receiver that
        //     tracks it with a live ambiguity as its anchor — decoupled from the
        //     clock pivot, so a satellite low/unseen at the clock pivot keeps a clean
        //     gauge at another station instead of jumping.
        if (!phase_datum_prioritized) {
            shared_phase_keys.clear();
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);
        } else {
            const double pref_el = config_.networkPhaseDatumPrefElevDeg();
            const bool   sticky  = config_.networkPhaseDatumSticky();
            const int    pdp_min_sta = config_.networkPhaseDatumMinStations();

            // (a) Per present station: satellite elevation (from the pruned obs) and
            //     the ANCHORABLE keys — the per-(sat,signal) phase-bias STATES this
            //     station allocated in PASS A.  NOTE the timing: the ambiguity states
            //     are allocated later, in PASS B's computeMeasurementUpdate, so we
            //     canNOT test getAmbiguityIndex here (it is -1 for everything at this
            //     point).  ensureNetworkSharedStates allocates a phase-bias state via
            //     the SAME usability gate as the row build, so a live phase-bias state
            //     ⟹ a phase row ⟹ a pinnable ambiguity in PASS B; the pin fold's own
            //     getAmbiguityIndex + h2local check is the final guard (a miss orphans
            //     the key, caught by the null-space gate → withhold + heal, rare).
            //     usable_keys[i]: "sat_signal" -> "sat|signal|PB".
            std::vector<std::map<std::string, double>>      sat_el(stations.size());
            std::vector<std::map<std::string, std::string>> usable_keys(stations.size());
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& o : modeled[i]) sat_el[i][o.satellite] = o.elevation;
                StateManager& sm = *stations[i].sm;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (!p) continue;
                    usable_keys[i].emplace(p->satellite + "_" + p->signal, kv.first);
                }
            }

            // (b) Per-constellation candidate receivers, ranked by DESCENDING
            //     coverage (# satellites tracked at >= pref_el) then lexical station
            //     name (determinism).  A station is a candidate for a constellation
            //     iff it can anchor >=1 of its keys; a low-elevation-only tracker
            //     stays a (last-ranked) candidate so it can still anchor a setting
            //     satellite nobody else sees.
            std::map<char, std::map<size_t, int>> cover;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& [sat, el] : sat_el[i])
                    if (!sat.empty() && el >= pref_el) ++cover[sat.front()][i];
            }
            auto cov = [&](char c, size_t i) -> int {
                auto it = cover.find(c); if (it == cover.end()) return 0;
                auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
            };
            std::map<char, std::set<size_t>> cand_set;
            std::map<std::string, std::string> union_keys;   // "sat_signal" -> "sat|signal|PB"
            for (size_t i = 0; i < stations.size(); ++i)
                for (const auto& [amb, pb] : usable_keys[i]) {
                    if (!amb.empty()) cand_set[amb.front()].insert(i);
                    union_keys.emplace(amb, pb);
                }

            // Coverage-threshold gating of the anchored union
            // (network.phase_datum_min_stations; 1 = off, byte-identical): drop
            // every (sat,signal) tracked by fewer than k present stations BEFORE
            // the election.  A one/two-tracker column is exactly the pathology
            // class the pdp validation surfaced: it (a) ejects a marginal
            // tracking station as non-PD (its local block barely separates the
            // column from that station's own ambiguity — the TWTF/VOIM symptom),
            // (b) re-levels by a full ambiguity lump when its one-station anchor
            // pool thins, and (c) leaks its anchor lump into the clock↔phase-bias
            // split.  A gated key's PHASE rows still serve the satellite clock:
            // each station's float ambiguity absorbs the bias, exactly as legacy
            // non-shared signals (and GLONASS network phase) always have.  The
            // tracking stations' allocated phase-bias states are pruned so the
            // per-station column sets stay consistent with the gated union (the
            // kMinStationsPerSat prune pattern above); the emitter sees a clean
            // per-key absence (staleness / first-sighting semantics on return).
            if (pdp_min_sta > 1) {
                std::map<std::string, int> n_trk;
                for (size_t i = 0; i < stations.size(); ++i)
                    for (const auto& kv : usable_keys[i]) ++n_trk[kv.first];
                std::set<std::string> gated_amb;   // "sat_signal" excluded this epoch
                for (auto it = union_keys.begin(); it != union_keys.end();) {
                    if (n_trk[it->first] < pdp_min_sta) {
                        gated_amb.insert(it->first);
                        it = union_keys.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!gated_amb.empty()) {
                    ++n_gated_epochs;
                    n_gated_key_epochs += gated_amb.size();
                    for (size_t i = 0; i < stations.size(); ++i) {
                        if (!present[i]) continue;
                        StateManager& sm = *stations[i].sm;
                        std::set<std::string> keep_pb;
                        bool drop_any = false;
                        for (const auto& kv : sm.getSatPhaseBiasStates()) {
                            const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                            if (p && gated_amb.count(p->satellite + "_" + p->signal)) {
                                drop_any = true;
                                continue;
                            }
                            keep_pb.insert(kv.first);
                        }
                        if (drop_any) sm.pruneSatPhaseBiasStatesExcept(keep_pb);
                    }
                }
            }

            if (phase_datum_ensemble) {
                // Fix A: anchor-free ENSEMBLE datum — the shared phase-bias set is the
                // (coverage-gated) UNION, with NO anchor election and NO pins.  The
                // gauge is realized by the kernel sum-to-zero fold (built from every
                // tracking station's ambiguity column AFTER the reduce), so no station
                // gets a pinned_amb_key (⇒ do_fold stays false, isGaugePinned false,
                // every station carries all its arcs) and there is no anchor to hand
                // over.  The candidate ranking + election below are skipped.
                for (auto& st : stations) {
                    st.pinned_amb_targets.clear();
                    st.pinned_rpb_keys.clear();
                }
                this_epoch_anchor.clear();
                shared_phase_keys.clear();
                for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
            } else if (phase_datum_eq34) {
                // Card 3: Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN datum.  Per
                // (constellation,signal) connected component of the bipartite
                // (receiver × satellite) observation graph, elect a datum receiver
                // r1 (its RPB is pinned to 0) and pin the ambiguities on the edges
                // of a spanning tree rooted at r1.  Then every between-satellite
                // reference is an integer combination across the whole component —
                // including satellites r1 never sees — because each chain hop
                // contributes only an integer (the hop receiver's fractional bias
                // is its OWN estimable RPB state δ_{r,σ}, no longer part of the
                // pin).  Full-visibility regional case degenerates to the Odijk
                // Eq-34 star (CC-R).  The shared set is the ungated union (as
                // ensemble); the chain pins fix the enlarged (receiver+satellite)
                // gauge.  Per (const,σ) component with n_c stations, m_c sats:
                // (n_c+m_c−1) tree-edge ambiguity pins + 1 RPB pin = n_c+m_c =
                // the exact gauge dimension.  Every RPB-carrying station is a tree
                // vertex, so it gets ≥1 pin ⇒ its N_rr stays PD (the load-bearing
                // rank invariant, D3c).
                for (auto& st : stations) {
                    st.pinned_amb_targets.clear();
                    st.pinned_rpb_keys.clear();
                }
                this_epoch_anchor.clear();
                shared_phase_keys.clear();
                for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
                if (!eq34_ppp_utils) eq34_ppp_utils = std::make_unique<PppUtils>(log_manager_);
                // This epoch's receiver-side parent edges + per-sat component roots;
                // become eq34_rx_parent / eq34_sat_root after the election (wholesale
                // replacement — the maps always describe exactly one epoch's tree).
                std::map<std::string, std::string> new_rx_parent, new_sat_root;
                // This epoch's pin targets (→ eq34_prev_target) and the carry
                // S-transformation shift maps collected by the per-component shift
                // walk: Δb per (sat,σ), Δδ per (station,cons,σ) / (station,rpb_key).
                std::map<std::pair<int, std::string>, double> new_prev_target;
                std::map<std::pair<std::string, std::string>, double> shift_sat;
                std::map<std::tuple<int, char, std::string>, double>  shift_rxg;
                std::map<std::pair<int, std::string>, double>         shift_rpb;

                const std::string eq34_pref_rx = config_.networkPhaseDatumEq34Receiver();
                // Card 4: the per-epoch datum targets are plain 0 (the frame is re-realized
                // freely; continuity moves to the invariant DD carry), so pin-to-round is off.
                const bool        eq34_round   = config_.networkPhaseDatumEq34PinRound()
                                              && !carry_invariant;
                auto eqSysOfChar = [](char c) -> System {
                    switch (c) {
                        case 'G': return System::GPS;    case 'E': return System::Galileo;
                        case 'C': return System::BeiDou; case 'J': return System::QZSS;
                        default:  return System::Unknown;
                    }
                };

                // Group the ungated (sat,σ) keys into (constellation,signal)
                // sub-problems.  Recover (sat,σ) from the pbkey value so the
                // "sat_signal" '_' split is unambiguous.
                struct GKey {
                    char cons; std::string signal;
                    bool operator<(const GKey& o) const {
                        return cons != o.cons ? cons < o.cons : signal < o.signal;
                    }
                };
                std::map<GKey, std::set<std::string>>                      grp_sats;
                std::map<GKey, std::map<size_t, std::vector<std::string>>> grp_arcs;  // group -> station -> sats
                std::map<GKey, std::map<std::string, std::vector<size_t>>> grp_satrx; // group -> sat -> stations
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!present[i]) continue;
                    for (const auto& [amb, pbkey] : usable_keys[i]) {
                        if (!union_keys.count(amb)) continue;          // gated out
                        const auto p = StateKeys::parseSatPhaseBiasKey(pbkey);
                        if (!p || p->satellite.empty()) continue;
                        const GKey g{ p->satellite.front(), p->signal };
                        grp_sats[g].insert(p->satellite);
                        grp_arcs[g][i].push_back(p->satellite);
                        grp_satrx[g][p->satellite].push_back(i);
                    }
                }

                // Rank bookkeeping (D3, theory ref §7.1): the recorded pins must
                // equal the exact gauge dimension Σ_components (n_c + m_c).  A cheap
                // always-on cross-check of the W5 null-space gate; logged below.
                int eq34_n_pins = 0, eq34_gauge_dim = 0;
                // Carry hygiene (D4): (sat,σ) whose chain path changed vs last
                // epoch — the emitted b_s re-references, so the phase-leveling local
                // carries for it are stale by an integer AND its D2/C1 shared PB
                // prior belongs to the retired frame.  Collected into the member
                // set (cleared here), erased below BEFORE PASS B consumes the local
                // carry, and marginalized at the shared-carry consume.
                eq34_reref.clear();
                for (auto& [g, sats] : grp_sats) {
                    // Normal locals (NOT the structured bindings g/sats) so the
                    // recordEdge lambda below can capture them — C++17 forbids
                    // capturing structured bindings.
                    const char        grp_cons = g.cons;
                    const std::string grp_sig  = g.signal;
                    const System      system  = eqSysOfChar(grp_cons);
                    const std::string sys_str = PppUtils::getSystemString(*sats.begin());
                    const std::string rpb_key = StateKeys::makeReceiverPhaseBiasKey(sys_str, grp_sig);
                    const std::string dr_key  = std::string(1, grp_cons) + "|" + grp_sig;
                    auto& arcs  = grp_arcs[g];
                    auto& satrx = grp_satrx[g];
                    auto rV = [](size_t i){ return "R" + std::to_string(i); };
                    auto sV = [](const std::string& s){ return "S" + s; };

                    // I4 longevity score of arc (station ri, sat): higher = longer
                    // expected life ⇒ a better NEW tree edge (fewer future deaths ⇒
                    // fewer b_s re-references).  A RISING arc (elevation grew vs last
                    // epoch, tracked in eq34_prev_el) will not set for a long time, so
                    // it is lifted above EVERY setting arc (+90°, one full quadrant);
                    // within a class higher elevation wins.  The churn source is the
                    // low-elevation SETTING arc — this ranks it dead last.  Unread
                    // unless eq34_longevity (sat_el/eq34_prev_el stay valid here).
                    auto arcLife = [&](size_t ri, const std::string& sat) -> double {
                        auto ei = sat_el[ri].find(sat);
                        if (ei == sat_el[ri].end()) return -1e9;   // no elevation (unreachable for a live arc)
                        const double el = ei->second;
                        auto pe = eq34_prev_el.find({ri, sat});
                        const bool rising = (pe != eq34_prev_el.end() && el > pe->second + 1e-6);
                        return el + (rising ? 90.0 : 0.0);
                    };

                    // STICKY-tree seed (spec D2 step 3): the incumbent tree edges —
                    // sat-side parents in anchor_incumbent ("sat_signal" → station),
                    // receiver-side parents in eq34_rx_parent ("station|c|σ" → sat) —
                    // that are still LIVE this epoch (station present in this group's
                    // arcs and still tracking the satellite).  Both maps are replaced
                    // wholesale each election, so together they are exactly last
                    // epoch's tree: each edge in exactly one map, and any live subset
                    // is a forest — seeding it can never close a cycle.
                    std::set<std::pair<size_t, std::string>> arc_set;
                    std::map<std::string, size_t> name2idx;
                    for (const auto& [i, slist] : arcs) {
                        name2idx[stations[i].name] = i;
                        for (const auto& s : slist) arc_set.insert({i, s});
                    }
                    std::map<std::string, std::vector<std::string>> inc_adj;
                    auto seedEdge = [&](const std::string& sta, const std::string& sat) {
                        auto ni = name2idx.find(sta);
                        if (ni == name2idx.end() || !arc_set.count({ni->second, sat})) return;
                        inc_adj[rV(ni->second)].push_back(sV(sat));
                        inc_adj[sV(sat)].push_back(rV(ni->second));
                    };
                    const std::string sig_suffix = "_" + grp_sig;
                    for (const auto& [amb, sta] : anchor_incumbent) {
                        if (amb.empty() || amb.front() != grp_cons) continue;
                        if (amb.size() <= sig_suffix.size()
                            || amb.compare(amb.size() - sig_suffix.size(),
                                           sig_suffix.size(), sig_suffix) != 0) continue;
                        seedEdge(sta, amb.substr(0, amb.size() - sig_suffix.size()));
                    }
                    const std::string rx_suffix =
                        "|" + std::string(1, grp_cons) + "|" + grp_sig;
                    for (const auto& [rxkey, sat] : eq34_rx_parent) {
                        if (rxkey.size() <= rx_suffix.size()
                            || rxkey.compare(rxkey.size() - rx_suffix.size(),
                                             rx_suffix.size(), rx_suffix) != 0) continue;
                        seedEdge(rxkey.substr(0, rxkey.size() - rx_suffix.size()), sat);
                    }

                    // --- Union-find over vertices "R<idx>" / "S<sat>". ---
                    std::map<std::string, std::string> uf;
                    auto find = [&](std::string v) -> std::string {
                        auto it = uf.find(v);
                        while (it != uf.end() && it->second != v) { v = it->second; it = uf.find(v); }
                        return v;
                    };
                    auto unite = [&](const std::string& a, const std::string& b) {
                        if (!uf.count(a)) uf[a] = a;
                        if (!uf.count(b)) uf[b] = b;
                        const std::string ra = find(a), rb = find(b);
                        if (ra != rb) uf[ra] = rb;
                    };
                    for (const auto& [i, slist] : arcs)
                        for (const auto& s : slist) unite(rV(i), sV(s));

                    std::map<std::string, std::vector<size_t>>      comp_rx;
                    std::map<std::string, std::vector<std::string>> comp_sat;
                    for (const auto& [i, slist] : arcs)  comp_rx[find(rV(i))].push_back(i);
                    for (const auto& s : sats)           comp_sat[find(sV(s))].push_back(s);

                    std::string best_datum_rx; size_t best_comp = 0;
                    for (auto& [root, rx_list] : comp_rx) {
                        std::sort(rx_list.begin(), rx_list.end());   // determinism

                        // Elect r1: config pref → per-COMPONENT sticky (sat-root
                        // vote) → group incumbent → highest coverage.
                        size_t r1 = static_cast<size_t>(-1);
                        if (!eq34_pref_rx.empty())
                            for (size_t i : rx_list)
                                if (stations[i].name == eq34_pref_rx) { r1 = i; break; }
                        // Per-COMPONENT stickiness: the component's sats remember
                        // last epoch's root (eq34_sat_root) — re-elect the majority-
                        // remembered station still present in this component.  The
                        // group-level eq34_datum_rx slot below only remembers the
                        // LARGEST component's r1, which left small fragments
                        // re-electing by coverage every epoch (Jul-10 field: ~8
                        // chronic (sat,σ) re-references per epoch, each a full
                        // fragment re-level).  Vote ties break by station name.
                        if (r1 == static_cast<size_t>(-1)) {
                            std::map<std::string, int> root_votes;
                            for (const auto& s : comp_sat[root]) {
                                auto rt = eq34_sat_root.find(s + "_" + grp_sig);
                                if (rt != eq34_sat_root.end()) ++root_votes[rt->second];
                            }
                            int best_v = 0; std::string best_name;
                            for (const auto& [nm, v] : root_votes)
                                if (v > best_v || (v == best_v && nm < best_name)) {
                                    best_v = v; best_name = nm;
                                }
                            if (best_v > 0)
                                for (size_t i : rx_list)
                                    if (stations[i].name == best_name) { r1 = i; break; }
                        }
                        if (r1 == static_cast<size_t>(-1)) {
                            auto it = eq34_datum_rx.find(dr_key);
                            if (it != eq34_datum_rx.end())
                                for (size_t i : rx_list)
                                    if (stations[i].name == it->second) { r1 = i; break; }
                        }
                        if (r1 == static_cast<size_t>(-1)) {
                            int best = -1;
                            for (size_t i : rx_list) {
                                const int cv = cov(g.cons, i);
                                if (cv > best
                                    || (cv == best && stations[i].name < stations[r1].name)) {
                                    best = cv; r1 = i;
                                }
                            }
                        }
                        if (r1 == static_cast<size_t>(-1)) continue;   // empty (unreachable)

                        if (rx_list.size() > best_comp) {
                            best_comp = rx_list.size();
                            best_datum_rx = stations[r1].name;
                        }

                        // r1's RPB pin — the receiver half of the gauge (δ_{r1,σ}=0).
                        stations[r1].pinned_rpb_keys.insert(rpb_key);
                        ++eq34_n_pins;
                        eq34_gauge_dim += static_cast<int>(rx_list.size()
                                        + comp_sat[root].size());   // n_c + m_c

                        // STICKY spanning tree (two-queue BFS from r1).  The
                        // incumbent queue drains to exhaustion before any NEW graph
                        // edge is explored, so a surviving old subtree — even one
                        // detached upstream — is re-attached through its OWN old
                        // edges (one new edge per detached fragment), never re-wired
                        // vertex-by-vertex: a tree-edge pin moves only when its arc
                        // actually died.  One ambiguity pin per tree edge; each
                        // attachment records (child, parent, edge-changed) for the
                        // dirty walk below.
                        //
                        // Records one tree edge (r,s): pin ambiguity z^s_r at station
                        // r.  Target = 0 on a first-sighting edge; on a re-pin of an
                        // already-lit (sat,σ) (pin-to-round), the |·|-minimizing
                        // integer λ·round(carried float/λ) so the emitted-bias frame
                        // step ≈ 0 cyc.  sat_is_child: which endpoint this edge
                        // ATTACHES — the sat (parent-edge → this_epoch_anchor) or the
                        // receiver (parent-edge → new_rx_parent).
                        std::set<std::string> visited;
                        std::deque<std::string> inc_q, new_q;
                        // (child, parent, pinmove, known): pinmove = this pin's
                        // exact displacement of its pinned float vs last epoch's
                        // frame; known = false where the step is NOT computable
                        // (re-pin without a carry; root handover — a common
                        // FRACTIONAL step no integer transform can express).
                        std::vector<std::tuple<std::string, std::string, double, bool>> order;
                        auto recordEdge = [&](size_t ri, const std::string& sat,
                                              bool sat_is_child) {
                            const std::string amb = sat + "_" + grp_sig;
                            // I2 metric: this edge was retained across a gap (its
                            // carry outlived the row absence) and is resuming now —
                            // the incumbent seed made it a recognized edge, so the
                            // pin-to-round below re-pins it to its pre-gap integer
                            // (step ~0) instead of the first-sighting 0 a wholesale
                            // replace would have forced.  Count the save.
                            if (eq34_extend_only
                                && eq34_retained_edges.count({static_cast<int>(ri), amb}))
                                ++eq34_gap_repin_total;
                            // Pin target: 0 on first sighting; else λ·round(carry/λ)
                            // at the (possibly new) edge station.  carry_val also
                            // feeds the pinmove computation below.
                            double target = 0.0;
                            double carry_val = std::numeric_limits<double>::quiet_NaN();
                            if (eq34_round && anchor_incumbent.count(amb)) {
                                auto lc = local_carry.find(std::make_tuple(
                                    static_cast<int>(ri), sat, grp_sig));
                                if (lc != local_carry.end() && system != System::Unknown) {
                                    const double lam =
                                        eq34_ppp_utils->getWavelength(system, grp_sig, sat);
                                    if (std::isfinite(lam) && lam > 0.0) {
                                        carry_val = lc->second.value_abs;
                                        target = lam * std::round(carry_val / lam);
                                    }
                                }
                            }
                            double pinmove = 0.0;
                            bool known = true;
                            bool edge_changed = false;
                            if (sat_is_child) {
                                auto ai = anchor_incumbent.find(amb);
                                edge_changed = (ai != anchor_incumbent.end()
                                                && ai->second != stations[ri].name);
                                this_epoch_anchor[amb] = stations[ri].name;
                                // Root handover: the component frame steps by the
                                // datum receivers' fractional δ difference — not an
                                // integer, not computable here ⇒ unknown.
                                auto rt = eq34_sat_root.find(amb);
                                if (rt != eq34_sat_root.end()
                                    && rt->second != stations[r1].name) known = false;
                                new_sat_root[amb] = stations[r1].name;
                            } else {
                                const std::string rxkey = stations[ri].name + rx_suffix;
                                auto rp = eq34_rx_parent.find(rxkey);
                                edge_changed = (rp != eq34_rx_parent.end()
                                                && rp->second != sat);
                                new_rx_parent[rxkey] = sat;
                            }
                            if (edge_changed) {
                                // Re-pin at a NEW station: the float there sat at
                                // carry_val; the pin drives it to target — an exact,
                                // known displacement iff the carry exists.
                                if (std::isfinite(carry_val)) pinmove = target - carry_val;
                                else known = false;
                            } else {
                                // Same station: its float was hard-pinned at last
                                // epoch's target, so the displacement is exactly the
                                // target difference (no carry noise involved).
                                auto pt = eq34_prev_target.find(
                                    {static_cast<int>(ri), amb});
                                pinmove = target
                                    - (pt != eq34_prev_target.end() ? pt->second : 0.0);
                            }
                            new_prev_target[{static_cast<int>(ri), amb}] = target;
                            stations[ri].pinned_amb_targets[amb] = target;
                            ++eq34_n_pins;
                            order.emplace_back(sat_is_child ? sV(sat) : rV(ri),
                                               sat_is_child ? rV(ri) : sV(sat),
                                               pinmove, known);
                        };
                        auto attach = [&](const std::string& v) {
                            visited.insert(v);
                            inc_q.push_back(v);
                            new_q.push_back(v);
                        };
                        attach(rV(r1));
                        while (!inc_q.empty() || !new_q.empty()) {
                            if (!inc_q.empty()) {
                                const std::string v = inc_q.front(); inc_q.pop_front();
                                auto ia = inc_adj.find(v);
                                if (ia == inc_adj.end()) continue;
                                for (const auto& u : ia->second) {
                                    if (visited.count(u)) continue;
                                    if (u[0] == 'S')
                                        recordEdge(static_cast<size_t>(
                                                       std::stoul(v.substr(1))),
                                                   u.substr(1), /*sat_is_child=*/true);
                                    else
                                        recordEdge(static_cast<size_t>(
                                                       std::stoul(u.substr(1))),
                                                   v.substr(1), /*sat_is_child=*/false);
                                    attach(u);
                                }
                            } else {
                                const std::string v = new_q.front(); new_q.pop_front();
                                if (v[0] == 'R') {
                                    const size_t ri =
                                        static_cast<size_t>(std::stoul(v.substr(1)));
                                    std::vector<std::string> ss = arcs[ri];
                                    // I4: attach this receiver's longest-lived
                                    // unvisited sats first (rising/high-elevation),
                                    // so the NEW edge is picked to outlive the epoch.
                                    // OFF = legacy sat-name order (byte-identical).
                                    if (eq34_longevity)
                                        std::sort(ss.begin(), ss.end(),
                                                  [&](const std::string& a, const std::string& b) {
                                                      const double la = arcLife(ri, a),
                                                                   lb = arcLife(ri, b);
                                                      if (la != lb) return la > lb;
                                                      return a < b;   // deterministic tie-break
                                                  });
                                    else
                                        std::sort(ss.begin(), ss.end());
                                    for (const auto& s : ss) {
                                        if (visited.count(sV(s))) continue;
                                        recordEdge(ri, s, /*sat_is_child=*/true);
                                        attach(sV(s));
                                    }
                                } else {
                                    const std::string s = v.substr(1);
                                    std::vector<size_t> rr = satrx[s];
                                    // I4: attach this sat through its most durable
                                    // unvisited tracker first — high per-constellation
                                    // coverage (the robust receiver, spec D2 step 3),
                                    // then the arc's own longevity at that station.
                                    // OFF = legacy station-index order (byte-identical).
                                    if (eq34_longevity)
                                        std::sort(rr.begin(), rr.end(),
                                                  [&](size_t a, size_t b) {
                                                      const int ca = cov(grp_cons, a),
                                                                cb = cov(grp_cons, b);
                                                      if (ca != cb) return ca > cb;
                                                      const double la = arcLife(a, s),
                                                                   lb = arcLife(b, s);
                                                      if (la != lb) return la > lb;
                                                      return a < b;   // deterministic tie-break
                                                  });
                                    else
                                        std::sort(rr.begin(), rr.end());
                                    for (size_t ri : rr) {
                                        if (visited.count(rV(ri))) continue;
                                        recordEdge(ri, s, /*sat_is_child=*/false);
                                        attach(rV(ri));
                                    }
                                }
                            }
                        }
                        // Shift walk — the carry S-TRANSFORMATION (spec D4 "v2 =
                        // SHIFT", realized at election: no emit-site plumbing
                        // needed, the step is known here).  A pin displacement
                        // propagates down the chain with alternating sign:
                        //   Δb_s = −Δδ_parent − pinmove(edge)
                        //   Δδ_r = −Δb_parent − pinmove(edge)
                        // so the whole subtree's frame move is EXACT and every
                        // carried quantity can be transformed into the new basis
                        // (variances untouched — zero information loss) instead of
                        // erased.  Erase/skip (eq34_reref) remains ONLY where the
                        // step is unknown: a re-pin with no carry, or a root
                        // handover (common fractional).  order is attachment-
                        // ordered, parents strictly first — one forward pass.
                        std::map<std::string, double> vshift;
                        std::map<std::string, bool>   vknown;
                        vshift[rV(r1)] = 0.0;
                        vknown[rV(r1)] = true;
                        for (const auto& [v, p, pinmove, known] : order) {
                            // Shift OFF (default): any nonzero pin displacement is
                            // treated as unknown ⇒ the erase/skip hygiene handles it
                            // (the validated B4/B7-class behavior).  Shift ON: exact
                            // transforms flow to the apply block below.
                            const bool   k = known && vknown[p]
                                          && (eq34_shift_on || pinmove == 0.0);
                            const double s = k ? (-vshift[p] - pinmove) : 0.0;
                            vshift[v] = s;
                            vknown[v] = k;
                            if (v[0] == 'S') {
                                const std::string sat = v.substr(1);
                                if (!k)          eq34_reref.insert({sat, grp_sig});
                                else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;
                            } else {
                                const int idx =
                                    static_cast<int>(std::stoul(v.substr(1)));
                                if (!k)
                                    // Unknown frame step: the carried δ̂_{r,σ}
                                    // belongs to an unrecoverable retired frame.
                                    rpb_carry.erase({idx, rpb_key});
                                else if (s != 0.0) {
                                    shift_rxg[std::make_tuple(idx, grp_cons,
                                                              grp_sig)] = s;
                                    shift_rpb[{idx, rpb_key}] = s;
                                }
                            }
                        }
                    }
                    if (!best_datum_rx.empty()) eq34_datum_rx[dr_key] = best_datum_rx;
                }
                // Rank cross-check (D3): recorded pins == Σ (n_c + m_c).  A mismatch
                // means a component was mis-pinned before the W5 gate ever runs.
                if (eq34_n_pins != eq34_gauge_dim) {
                    ++eq34_mismatch_epochs;
                    log->warn("Network[eq34]: pin/gauge mismatch — recorded {} pins vs "
                              "gauge dimension {} (Σ n_c+m_c over components); the W5 "
                              "null-space gate will catch an under/over-constrained datum.",
                              eq34_n_pins, eq34_gauge_dim);
                } else
                    log->debug("Network[eq34]: chain datum — {} pins == gauge dim {} "
                               "(1 RPB + tree edges per component).",
                               eq34_n_pins, eq34_gauge_dim);
                // Carry hygiene (D4): erase the phase-leveling local carries for any
                // re-referenced (sat,σ) at EVERY station, before PASS B consumes the
                // carry — the b_s frame stepped by an integer, so the carried floats
                // are stale.  Refloat next epoch (safe; the realized wire step is
                // usually 0 under pin-to-round, but erasing an already-integer carry
                // costs only one epoch's continuity).
                if (!eq34_reref.empty()) {
                    eq34_reref_total += static_cast<long long>(eq34_reref.size());
                    int n_erased = 0;
                    for (auto it = local_carry.begin(); it != local_carry.end();) {
                        const auto& key = it->first;   // (station idx, sat, signal)
                        if (eq34_reref.count({std::get<1>(key), std::get<2>(key)})) {
                            it = local_carry.erase(it);
                            ++n_erased;
                        } else {
                            ++it;
                        }
                    }
                    eq34_erase_total += n_erased;
                    if (n_erased > 0)
                        log->debug("Network[eq34]: {} local ambiguity carry(ies) erased for "
                                   "{} re-referenced (sat,σ) — refloat next epoch.",
                                   n_erased, eq34_reref.size());
                }
                // Carry S-transformation apply: shift every carried quantity into
                // this epoch's frame (Δz = −Δb_s − Δδ_r on the local ambiguity
                // carries; Δδ_r on the RPB carries; Δb_s on the shared PB carried
                // priors).  Variances untouched — this is an exact basis change,
                // not a refloat.  Runs BEFORE PASS B consumes any carry.
                if (!shift_sat.empty() || !shift_rxg.empty()) {
                    long long n_shifted = 0;
                    for (auto& [key, lc] : local_carry) {
                        const int idx          = std::get<0>(key);
                        const std::string& sat = std::get<1>(key);
                        const std::string& sig = std::get<2>(key);
                        double d = 0.0;
                        auto bs = shift_sat.find({sat, sig});
                        if (bs != shift_sat.end()) d -= bs->second;
                        if (!sat.empty()) {
                            auto dr = shift_rxg.find(
                                std::make_tuple(idx, sat.front(), sig));
                            if (dr != shift_rxg.end()) d -= dr->second;
                        }
                        if (d != 0.0 && std::isfinite(d)) {
                            lc.value_abs += d;
                            ++n_shifted;
                        }
                    }
                    for (auto& [key, lc] : rpb_carry) {
                        auto it = shift_rpb.find(key);
                        if (it != shift_rpb.end() && std::isfinite(it->second)
                            && it->second != 0.0) {
                            lc.value_abs += it->second;
                            ++n_shifted;
                        }
                    }
                    for (auto& cp : prev_carry) {
                        if (cp.kind != SharedParamKind::PhaseBias) continue;
                        auto bs = shift_sat.find({cp.sat, cp.signal});
                        if (bs != shift_sat.end() && std::isfinite(bs->second)
                            && bs->second != 0.0) {
                            cp.value += bs->second;
                            ++n_shifted;
                        }
                    }
                    eq34_shift_total += n_shifted;
                }
                // Sticky baselines for next epoch: this epoch's tree becomes the
                // incumbent forest (sat parents + receiver parents, replaced
                // wholesale so the pair of maps always describes exactly one tree),
                // each sat remembers its component root for the r1-handover check,
                // and each pin remembers its target for the same-station pinmove.
                // anchor_incumbent doubles as the was-lit gate for pin-to-round
                // (every tree sat has exactly one parent edge).
                //
                // I2 (de Jonge §3.5 extend-only) — before the wholesale swap, MERGE
                // back the identity of any tree edge that was NOT seen this epoch but
                // whose phase-leveling memory (local_carry) is still alive: its
                // sat-side parent (this_epoch_anchor), receiver-side parent
                // (new_rx_parent), component root (new_sat_root) and pinned integer
                // (new_prev_target).  A momentary row gap is then NOT a dead arc — the
                // edge sits inertly in the incumbent maps (never seeded, because the
                // seed admits LIVE arcs only ⇒ the spanning-forest invariant holds),
                // and the epoch its arc RESUMES it is a recognized incumbent whose
                // pin-to-round re-pins to the SAME integer (emitted-bias step ≈ 0)
                // rather than re-leveling from a first-sighting 0.  local_carry
                // liveness is the age stamp: it is gap-pruned at phase_leveling_max
                // _gap_s, and a genuine reroute/handover erases it — so an edge past
                // that horizon (or truly re-referenced) is dropped and re-floats as a
                // first sighting, by which point the peer carries are gone too.  Keys
                // are all receiver-indexed pins {receiver idx, "sat_signal"}; the two
                // parent maps hold each edge in exactly one direction (unchanged).
                if (eq34_extend_only) {
                    std::map<std::string, size_t> gname2idx;
                    for (size_t i = 0; i < stations.size(); ++i)
                        gname2idx[stations[i].name] = i;
                    auto splitAmb = [](const std::string& a, std::string& sat,
                                       std::string& sig) -> bool {
                        const auto us = a.find('_');
                        if (us == std::string::npos) return false;
                        sat = a.substr(0, us); sig = a.substr(us + 1); return true;
                    };
                    auto carryAlive = [&](size_t idx, const std::string& sat,
                                          const std::string& sig) -> bool {
                        return local_carry.count(std::make_tuple(
                            static_cast<int>(idx), sat, sig)) != 0;
                    };
                    eq34_retained_edges.clear();
                    // Sat-side parents (amb → parent station) + their component roots.
                    for (const auto& [amb, sta] : anchor_incumbent) {
                        if (this_epoch_anchor.count(amb)) continue;    // re-seen live
                        std::string sat, sig;
                        if (!splitAmb(amb, sat, sig)) continue;
                        auto gi = gname2idx.find(sta);
                        if (gi == gname2idx.end()
                            || !carryAlive(gi->second, sat, sig)) continue;  // dead ⇒ drop
                        this_epoch_anchor.emplace(amb, sta);
                        eq34_retained_edges.emplace(static_cast<int>(gi->second), amb);
                        auto sr = eq34_sat_root.find(amb);
                        if (sr != eq34_sat_root.end())
                            new_sat_root.emplace(amb, sr->second);
                    }
                    // Receiver-side parents ("<name>|<c>|<σ>" → sat).
                    for (const auto& [rxkey, sat] : eq34_rx_parent) {
                        if (new_rx_parent.count(rxkey)) continue;
                        const auto p2 = rxkey.rfind('|');
                        if (p2 == std::string::npos || p2 == 0) continue;
                        const auto p1 = rxkey.rfind('|', p2 - 1);
                        if (p1 == std::string::npos) continue;
                        const std::string name = rxkey.substr(0, p1);
                        const std::string sig  = rxkey.substr(p2 + 1);
                        auto gi = gname2idx.find(name);
                        if (gi == gname2idx.end()
                            || !carryAlive(gi->second, sat, sig)) continue;
                        new_rx_parent.emplace(rxkey, sat);
                        eq34_retained_edges.emplace(static_cast<int>(gi->second),
                                                    sat + "_" + sig);
                    }
                    // Pinned integers ({receiver idx, "sat_signal"} → target).
                    for (const auto& [k, tgt] : eq34_prev_target) {
                        if (new_prev_target.count(k)) continue;
                        std::string sat, sig;
                        if (!splitAmb(k.second, sat, sig)) continue;
                        if (!carryAlive(static_cast<size_t>(k.first), sat, sig)) continue;
                        new_prev_target.emplace(k, tgt);
                    }
                }
                anchor_incumbent = this_epoch_anchor;
                eq34_rx_parent   = std::move(new_rx_parent);
                eq34_sat_root    = std::move(new_sat_root);
                eq34_prev_target = std::move(new_prev_target);
                // I4: snapshot this epoch's arc elevations so next epoch's arcLife
                // can tell a rising arc from a setting one.  Rebuilt wholesale (a
                // pruned arc simply drops out); skipped entirely when longevity
                // election is off, so it stays empty on the B9 baseline.
                if (eq34_longevity) {
                    eq34_prev_el.clear();
                    for (size_t i = 0; i < stations.size(); ++i) {
                        if (!present[i]) continue;
                        for (const auto& [sat, el] : sat_el[i])
                            eq34_prev_el[{i, sat}] = el;
                    }
                }
            } else {
            std::map<char, std::vector<size_t>> cand;
            for (auto& [c, s] : cand_set) {
                std::vector<size_t> v(s.begin(), s.end());
                std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
                    const int ca = cov(c, a), cb = cov(c, b);
                    if (ca != cb) return ca > cb;
                    return stations[a].name < stations[b].name;
                });
                cand[c] = std::move(v);
            }

            // (c) Elect one anchor per (sat,signal): keep the STICKY incumbent while
            //     it still tracks the key (fewest handovers), else the highest-ranked
            //     candidate that tracks it — concentrating most of a constellation on
            //     one receiver (frame-consistent) with graceful per-satellite
            //     fallback.  Record the pin in the anchor's OWN pinned_amb_keys (its
            //     local block — the load-bearing invariant), the shared column, and
            //     this_epoch_anchor (read at the emit site for handover signalling).
            for (auto& st : stations) {
                st.pinned_amb_targets.clear();
                st.pinned_rpb_keys.clear();   // eq34-only; always empty here (no-op)
            }
            this_epoch_anchor.clear();
            shared_phase_keys.clear();
            std::map<std::string, std::string> new_incumbent;
            for (const auto& [amb, pbkey] : union_keys) {
                const char c = amb.empty() ? char(0) : amb.front();
                size_t anchor = static_cast<size_t>(-1);
                if (sticky) {
                    auto it = anchor_incumbent.find(amb);
                    if (it != anchor_incumbent.end())
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (present[i] && stations[i].name == it->second
                                && usable_keys[i].count(amb)) { anchor = i; break; }
                }
                if (anchor == static_cast<size_t>(-1)) {
                    auto cit = cand.find(c);
                    if (cit != cand.end())
                        for (size_t i : cit->second)
                            if (usable_keys[i].count(amb)) { anchor = i; break; }
                }
                if (anchor == static_cast<size_t>(-1)) continue;   // no tracker (unreachable)
                stations[anchor].pinned_amb_targets[amb] = 0.0;   // prioritized: pin to 0
                this_epoch_anchor[amb] = stations[anchor].name;
                new_incumbent[amb]     = stations[anchor].name;
                shared_phase_keys.insert(pbkey);
            }
            anchor_incumbent.swap(new_incumbent);
            }  // end !phase_datum_ensemble (anchor election)
        }

        // Card 3 (eq34) RPB allocation (rule D3c): a present station gets the
        // per-(receiver,system,signal) split state δ_{r,σ} iff it carries an
        // UNGATED shared phase-bias column on σ (a phase-bias state whose key is in
        // shared_phase_keys) — i.e. iff it is a vertex of the D2 graph.  A station
        // whose σ-keys are all gated allocates no RPB and keeps legacy lumping.
        // Re-derived fresh each epoch (allocate what's needed, prune the rest); runs
        // BEFORE est.reset()/PASS B so buildJacobianRow's +1 RPB coefficient sees the
        // column and splitStation routes it into the local block A_r.  A spanning
        // tree touches every vertex, so every RPB-carrying station carries ≥1
        // tree-edge pin ⇒ its N_rr stays PD.  Inert (no allocation) when eq34 off.
        if (phase_datum_eq34) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> need_rpb;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    if (!shared_phase_keys.count(kv.first)) continue;   // gated → legacy lump
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (!p || p->satellite.empty()) continue;
                    need_rpb.insert(StateKeys::makeReceiverPhaseBiasKey(
                        PppUtils::getSystemString(p->satellite), p->signal));
                }
                for (const auto& rk : need_rpb)
                    if (sm.getReceiverPhaseBiasIndex(rk) < 0)
                        sm.addReceiverPhaseBiasState(rk, 0.0);
                sm.pruneReceiverPhaseBiasStatesExcept(need_rpb);
            }
        }

        // --- Register the shared column layout (reset the hoisted estimator) ---
        est.reset();
        std::vector<std::string> registered_clock_sats;
        std::set<std::string> clk_seen;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates()) {
                const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
                est.registerClock(sat);
                if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
            }
        }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (parts) est.registerPhaseBias(parts->satellite, parts->signal);
        }
        // Register the satellite CODE-bias (OSB) columns (W6, full multi-band):
        // EVERY (sat,signal) — every frequency band AND every tracking channel
        // (attribute) — that at least two present stations carry an OSB state for.
        // Collected (deduplicated, first-seen order) for the code-bias datum
        // constraints, the D2 carry-forward, and the solution trace.  Empty unless
        // estimate_sat_code_bias.
        //
        // The ONLY admission gate is >=2-station coverage: a single-station code
        // bias is perfectly confounded with that one station's receiver code bias
        // and cannot be separated, so it stays a local nuisance (its sat-bias state
        // is pruned below, leaving its raw code row to fold into the receiver
        // clock / slant iono — the pre-W6 path).  Everything with >=2 stations is a
        // first-class shared OSB column and is datumed below: the GIM between-
        // satellite iono structure makes the inter-band DCBs observable, while
        // same-band channels (C1C vs C1W, C2W vs C2L, ...) need no iono at all
        // (equal frequency → the receiver and ionosphere terms cancel in the raw
        // between-channel difference, leaving the satellite inter-channel bias).
        // The earlier slice confined the OSB to the two lowest bands and so emitted
        // nothing on a 3rd band; the datum below now pins every registered column.
        std::vector<std::pair<std::string, std::string>> registered_code_keys;
        if (config_.estimateNetworkSatCodeBias()) {
            std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatBiasStates())
                    ++cb_station_count[kv.first];
            }
            std::set<std::string> keep_keys;
            for (const auto& [key, cnt] : cb_station_count)
                if (cnt >= 2) keep_keys.insert(key);

            std::set<std::string> cb_seen;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    if (!keep_keys.count(kv.first)) continue;
                    station_keep.insert(kv.first);
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;
                    est.registerCodeBias(parts->satellite, parts->signal);
                    if (cb_seen.insert(kv.first).second)
                        registered_code_keys.emplace_back(parts->satellite, parts->signal);
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        }

        // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
        // layout is FIXED — all (N+1)² coefficients every epoch, independent of the
        // satellite set — so the field is continuous across epochs and the Kaula
        // prior can keep every coefficient soft-pinned (the rank gate then stays at
        // #constellations regardless of coverage; design §6.1).
        if (config_.estimateNetworkIonoSh() && sh_model) {
            for (const auto& c : sh_model->coeffs())
                est.registerVtecCoeff(c.n, c.m, c.trig);
        }

        const int n_clock = static_cast<int>(registered_clock_sats.size());
        const int n_pb    = static_cast<int>(shared_phase_keys.size());
        const int n_cb    = static_cast<int>(registered_code_keys.size());
        const int n_present = static_cast<int>(std::count(present.begin(), present.end(), true));
        last_n_present    = n_present;   // RTB-6: most-recent station-presence count for the health heartbeat
        last_n_clock_sats = n_clock;     // RTB-6 (P4b): most-recent registered-clock-sat count for the health heartbeat

        // Column -> human descriptor map (W6 estimability diagnostic): lets the
        // one-shot spectrum dump name which shared parameters span the near-zero
        // eigen-directions (unpinned gauges) instead of guessing.  Populated only
        // when the (default-OFF) estimability trace is armed — it feeds nothing but
        // that dump, so a production run skips the string-building entirely.
        std::vector<std::string> col_desc(static_cast<size_t>(est.numShared()));
        if (spectrum_diag_budget > 0 && nc.trace_estimability) {
            for (const std::string& sat : registered_clock_sats)
                col_desc[static_cast<size_t>(est.clockColumn(sat))] = "CLK|" + sat;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) col_desc[static_cast<size_t>(est.phaseBiasColumn(p->satellite, p->signal))]
                           = "PB|" + p->satellite + "|" + p->signal;
            }
            for (const auto& kv : registered_code_keys)
                col_desc[static_cast<size_t>(est.codeBiasColumn(kv.first, kv.second))]
                    = "CB|" + kv.first + "|" + kv.second;
            // Name the SH-VTEC columns — a HARD requirement (design §6.4): otherwise
            // a missing SH datum prints as an anonymous colNNN in the spectrum dump,
            // indistinguishable from a clock-datum fault.
            if (config_.estimateNetworkIonoSh() && sh_model) {
                for (const auto& c : sh_model->coeffs())
                    col_desc[static_cast<size_t>(est.vtecCoeffColumn(c.n, c.m, c.trig))] =
                        fmt::format("SH|{}|{}|{}", c.n, c.m, c.trig);
            }
        }

        if (est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ",0,0,0,0,0,0,0,0\n";
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("sync_skip", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        est.beginEpoch();

        // Capture the shared-border folds (code-bias gauge constraint + all carried /
        // anchor priors) applied to THIS epoch's arrowhead, so the within-epoch
        // adaptation (drop a culprit → re-solve) can replay the identical shared system
        // over a fresh beginEpoch().  addSharedPrior is additive, so the three prior
        // sets concatenate into one replayable vector.  Populated only when the
        // adaptation is armed; otherwise the appends are cheap no-ops.
        const bool within_epoch_armed = config_.networkRaimWithinEpoch();
        Eigen::MatrixXd cap_H;
        Eigen::VectorXd cap_z;
        double cap_constraint_weight = 0.0;
        bool   cap_has_constraint = false;
        std::vector<NetworkEstimator::SharedPrior> cap_priors;
        // C1: the dense D2 fold is captured separately (keys + effective covariance)
        // and replayed via addSharedPriorDense — deterministic, since the layout is
        // frozen for the epoch.  Non-empty only when the dense fold SUCCEEDED (its
        // fallback routes through cap_priors like every diagonal prior).
        std::vector<NetworkEstimator::SharedPrior> cap_dense_keys;
        Eigen::MatrixXd cap_dense_cov;

        subEnd(SB_PREASM);   // D0b: close pre-assembly (the assembly wall below is timed separately)

        // --- PASS B: per-station assembly, then SERIAL scatter -------------
        // Split into a PARALLEL produce phase (pure per-station compute into
        // pre-sized slots — no shared write) and a SERIAL consume phase that
        // scatters each block into the shared arrowhead in deterministic station
        // order.  computeMeasurementUpdate + splitStation + the pivot pin fold
        // touch only station i's own state and CONST queries on `est` (its column
        // layout is frozen after registration; addStation is the only mutation
        // and runs serially below) — so the produced blocks and their add order
        // are identical for any thread count.
        //
        // D0: the assembly wall brackets the parallel produce (per-station row
        // build) + the serial scatter (est.addStation) — the "assembly" section.
        std::chrono::steady_clock::time_point sec_asm_t0;
        if (sec_timers_on) sec_asm_t0 = std::chrono::steady_clock::now();
        std::vector<StationAssembly> asm_res(stations.size());

        // T5: age the SH-deviation carries ONCE per epoch (SERIAL — the parallel
        // assembly below only reads).  GM prediction: dev *= λ, P → λ²P + σ_ss²(1−λ²);
        // entries unseen for > 5τ are dropped (fully decorrelated — the cold seed the
        // pin then uses is statistically equivalent).
        if (sh_dev_on) {
            const double now_s = se.time.week * 604800.0 + se.time.tow;
            for (auto it = sh_dev_carry.begin(); it != sh_dev_carry.end();) {
                ShDevCarry& dc = it->second;
                const double dt = now_s - dc.t_s;
                if (dc.t_s <= 0.0 || dt > 5.0 * sh_dev_tau) {
                    it = sh_dev_carry.erase(it);
                    continue;
                }
                if (dt > 0.0) {
                    const double lam = std::exp(-dt / sh_dev_tau);
                    dc.dev_m  *= lam;
                    dc.var_m2  = lam * lam * dc.var_m2 + dc.var_ss * (1.0 - lam * lam);
                }
                ++it;
            }
        }

        // Card 1d (network.carry_coupling_time_update): record the (kind, sat) shared
        // keys whose carried state folds THIS epoch with the loose clock-monitor
        // REINIT variance (the D2 inflate_q path — the stash marks the flagged sat's
        // clock AND its phase-/code-bias entries, kind-independently; the phase-hold
        // path deliberately keeps its pb time-locked and is stashed inflate_q=false,
        // so mirroring the stash flags reproduces the fold's exact per-kind
        // treatment).  A coupling term on such a column must not shrink-carry — the
        // reinit decorrelates the border column from its stashed value, so the term
        // is fully marginalized (κ=0, σ² += M²·P_prev), mirroring the dense fold's
        // decorrelation rule.  Kept as a HISTORY map (key → content time of the last
        // reinit fold): a coupling row injects against its own harvest time, so a
        // reinit inside a MULTI-EPOCH carry gap still marginalizes (a q=∞ segment
        // anywhere inside the gap composes to κ=0).  The Clock entry also covers the
        // clock_model=ON oscillator reinit (the clock never rides prev_carry there)
        // and the HARD-refloat mode (clk_mon_reinit_sig ≤ 0 skip-stashes the flagged
        // sat entirely — no prev_carry entry to mirror; all three kinds decorrelate,
        // matching the skip-stash).  Updated SERIALLY; PASS B only reads it.
        if (carry_cpl_tu) {
            for (const CarryPrior& cp : prev_carry)
                if (cp.inflate_q)
                    tu_reinit_hist[{cp.kind, cp.sat}] = pl_now_s;
            for (const std::string& sat : prev_clock_refloat_sats) {
                tu_reinit_hist[{SharedParamKind::Clock, sat}] = pl_now_s;
                if (!(clk_mon_reinit_sig > 0.0)) {
                    tu_reinit_hist[{SharedParamKind::PhaseBias, sat}] = pl_now_s;
                    tu_reinit_hist[{SharedParamKind::CodeBias, sat}] = pl_now_s;
                }
            }
        }

        #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            StationAssembly& R = asm_res[i];
            try {
                NetStation& st = stations[i];

                EpochMeasurement em = st.mm->computeMeasurementUpdate(
                    modeled[i], st.sm->getStateVector());
                if (em.innovations.size() == 0) { R.empty_innov = true; continue; }
                if (em.H.cols() != static_cast<Eigen::Index>(st.sm->getStateSize())) {
                    R.state_mismatch = true;
                    R.err = fmt::format("{} vs {}", em.H.cols(), st.sm->getStateSize());
                    continue;
                }

                // Alias into the assembly slot so the H->A_r column map survives
                // for the post-solve slant-iono back-substitution (viz trace).
                std::map<int, int>& h2local = R.h2local;
                splitStation(st, est, em, R.A_s, R.A_r, R.w, R.y, h2local);

                // Fix A (network.phase_datum_ensemble): record every present
                // ambiguity's (sat, signal, A_r column) so the driver can build the
                // per-(sat,signal) sum-to-zero gauge over all tracking stations after
                // the reduce.  h2local is keyed by state index (== the pl_amb carry's
                // key), so a present phase row ⟹ an entry here.  Filtered to the sats
                // with a shared phase-bias column at gauge-build time.  Pure
                // per-station read — safe in the parallel produce phase.
                if (phase_datum_ensemble) {
                    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts) continue;
                        const auto hit = h2local.find(static_cast<int>(sidx));
                        if (hit == h2local.end()) continue;  // no phase row this epoch
                        R.ens_amb_cols.emplace_back(
                            parts->satellite, parts->signal,
                            static_cast<Eigen::Index>(hit->second));
                    }
                }

                // The anchor station(s) fold tight-prior pins on the ambiguities
                // they gauge (the LOCAL half of the S-basis): drive each pinned
                // ambiguity's ABSOLUTE value to its integer TARGET so the estimable
                // phase bias absorbs THIS receiver's ambiguity lump (Odijk Eq. 4).
                // Built into the anchor's OWN block — no shared write.  Legacy: the
                // single pivot pins the whole shared_phase_keys set (target 0).
                // Prioritized: each station pins exactly its elected
                // pinned_amb_targets (target 0), the load-bearing invariant — the pin
                // must live in the block whose local ambiguity column it constrains.
                // eq34 (Card 3): the spanning-tree edge pins (target 0 or the
                // pin-to-round integer) PLUS the datum receiver's ONE RPB pin
                // (δ_{r1,σ}=0), both in this station's OWN block.
                const bool do_fold = phase_datum_prioritized
                    ? (!st.pinned_amb_targets.empty() || !st.pinned_rpb_keys.empty())
                    : st.is_pivot;
                if (do_fold) {
                    const Eigen::VectorXd piv_x = st.sm->getStateVector();
                    std::vector<int> pin_col;       // A_r column of each pin
                    std::vector<double> pin_y;       // innovation = target - current absolute value
                    std::vector<double> pin_w;       // row information weight: 1e10 hard pins, 1/σ² RPB priors
                    auto foldAmb = [&](const std::string& amb_key, double target) {
                        const int amb_idx = st.sm->getAmbiguityIndex(amb_key);
                        if (amb_idx < 0) return;   // no usable phase row for it this epoch
                        const auto hit = h2local.find(amb_idx);
                        if (hit == h2local.end()) return;  // column dropped (all-zero H)
                        pin_col.push_back(hit->second);
                        // Preserve the exact legacy expression (−piv_x) when pinning
                        // to 0 — "0.0 − x" flips the sign of a zero x, which would
                        // perturb the OFF golden hash; a nonzero eq34 re-pin target
                        // uses the general form.
                        pin_y.push_back(target == 0.0 ? -piv_x(amb_idx)
                                                      : target - piv_x(amb_idx));
                        pin_w.push_back(1e10);   // hard pin (exact gauge)
                    };
                    // eq34: r1's RPB pin — δ_{r1,σ}=0 on the RPB column in this
                    // station's OWN block (the receiver half of the chain gauge).
                    auto foldRpb = [&](const std::string& rpb_key) {
                        const int rpb_idx = st.sm->getReceiverPhaseBiasIndex(rpb_key);
                        if (rpb_idx < 0) return;
                        const auto hit = h2local.find(rpb_idx);
                        if (hit == h2local.end()) return;  // no phase row on σ this epoch
                        pin_col.push_back(hit->second);
                        pin_y.push_back(-piv_x(rpb_idx));
                        pin_w.push_back(1e10);   // hard pin (exact gauge)
                    };
                    if (phase_datum_prioritized) {
                        for (const auto& [amb_key, target] : st.pinned_amb_targets)
                            foldAmb(amb_key, target);
                        for (const auto& rpb_key : st.pinned_rpb_keys)
                            foldRpb(rpb_key);
                    } else {
                        for (const auto& pk : shared_phase_keys) {
                            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                            if (!parts) continue;
                            foldAmb(parts->satellite + "_" + parts->signal, 0.0);
                        }
                    }
                    const int n_hard = static_cast<int>(pin_col.size());
                    // eq34: loose zero-mean prior (w = 1/σ², default σ = 1 m; 0
                    // disables) on every UNPINNED RPB column with a live phase row.
                    // The tree pins fix the GAUGE exactly, but the estimable
                    // RPB↔ambiguity↔iono separation INSIDE a block can be nearly
                    // rank-deficient (few sats / one signal): without this floor
                    // the W5 gate withholds the epoch, or — under
                    // station_pd_pin_aware — the near-singular block stays in and
                    // degrades the shared solve (Jul-10 field A/B: datum-invariant
                    // DD step 0.085 → 36–118 cyc/ep).  NOT a second datum: the
                    // gauge is already fixed; the pull toward 0 is ≪ one phase
                    // row.  GLONASS local-anchor precedent.  The datum receiver's
                    // pinned RPB is skipped (already hard-pinned to 0).
                    if (phase_datum_eq34
                        && (eq34_rpb_prior_w > 0.0 || eq34_rpb_q > 0.0)) {
                        for (const auto& [rpb_key, rpb_idx] :
                                 st.sm->getReceiverPhaseBiasStates()) {
                            if (st.pinned_rpb_keys.count(rpb_key)) continue;
                            const auto hit = h2local.find(static_cast<int>(rpb_idx));
                            if (hit == h2local.end()) continue;  // no phase row on σ
                            const double x0 =
                                piv_x(static_cast<Eigen::Index>(rpb_idx));
                            if (eq34_rpb_prior_w > 0.0) {   // rank floor (zero-mean)
                                pin_col.push_back(hit->second);
                                pin_y.push_back(-x0);
                                pin_w.push_back(eq34_rpb_prior_w);
                            }
                            if (eq34_rpb_q > 0.0) {
                                // Local δ_{r,σ} time-linking (D6 v2, local form):
                                // prior toward the carried previous estimate with
                                // variance var + q·gap — the continuity the fresh
                                // per-epoch re-estimate lacks.  The carry map is
                                // read-only here (parallel build); stash happens
                                // post-solve, erase at the election dirty walk.
                                auto rc = rpb_carry.find(
                                    {static_cast<int>(i), rpb_key});
                                if (rc != rpb_carry.end()) {
                                    const double gap =
                                        pl_now_s - rc->second.last_seen_s;
                                    if (gap >= 0.0 && gap <= pl_max_gap_s) {
                                        const double v = rc->second.var
                                                       + eq34_rpb_q * gap;
                                        if (v > 0.0 && std::isfinite(v)) {
                                            pin_col.push_back(hit->second);
                                            pin_y.push_back(
                                                rc->second.value_abs - x0);
                                            pin_w.push_back(1.0 / v);
                                        }
                                    }
                                }
                                R.pl_rpb.emplace_back(
                                    rpb_key,
                                    static_cast<Eigen::Index>(hit->second), x0);
                            }
                        }
                    }
                    const int np = static_cast<int>(pin_col.size());
                    if (np > 0) {
                        const Eigen::Index m0 = R.A_s.rows();
                        R.A_s.conservativeResize(m0 + np, Eigen::NoChange);
                        R.A_s.bottomRows(np).setZero();
                        R.A_r.conservativeResize(m0 + np, Eigen::NoChange);
                        R.A_r.bottomRows(np).setZero();
                        R.w.conservativeResize(m0 + np);
                        R.y.conservativeResize(m0 + np);
                        for (int k = 0; k < np; ++k) {
                            R.A_r(m0 + k, pin_col[k]) = 1.0;
                            R.w(m0 + k) = pin_w[k];   // 1e10 pins / 1/σ² RPB priors
                            R.y(m0 + k) = pin_y[k];
                        }
                        // HARD pins only — pd_pin_aware and the total_pins ledger
                        // must not count the soft RPB prior rows as gauge pins.
                        R.np = n_hard;
                    }
                }

                // GLONASS local-nuisance anchors (code-only network scope).
                // The per-epoch ABSOLUTE network solve carries no Kalman prior on the
                // local nuisances, and GLONASS — unlike GPS/Galileo — has a per-FCN-
                // slot receiver IFB on EVERY band, so there is no IFB-free reference-
                // band code to anchor the slant iono.  Two unobservable families make
                // the local block N_rr non-positive-definite (then the kernel drops
                // the WHOLE station, losing its good GPS/Galileo rows too):
                //   (1) each GLONASS satellite's slant iono I_s — fully confounded
                //       with that satellite's IFB(band,k) + ambiguity (the code rows
                //       determine only the SUM IFB + alpha*I, leaving I_s free);
                //   (2) the {IFB(k)} <-> GLONASS-ISB common mode — unanchored unless a
                //       central-channel k=0 satellite is up (GLONASS has only ~2-3).
                // Append one LOOSE zero-mean prior row per GLONASS per-slot IFB column
                // AND per GLONASS per-satellite iono column to regularize exactly
                // those directions.  PRODUCT-SAFE: the emitted satellite clock depends
                // only on the data-determined IFB+alpha*I sum (and the cross-station
                // consensus), never on how this unobservable split is resolved, so
                // anchoring the split toward zero does not perturb the shared clock.
                // Weight 1.0 (sigma 1 m) sits well above the kernel's relative PD floor
                // (1e-12 * d_max, d_max ~ the 1e10 pivot ambiguity pins), so even the
                // pivot block stays PD.  Built into the station's OWN block (no shared
                // write); inert for GPS/Galileo-only stations and when the flag is off.
                if (config_.enableGlonassNetwork()) {
                    std::vector<int> anchor_cols;
                    for (const auto& [key, sidx] : st.sm->getCodeBiasStates()) {
                        if (!StateKeys::isGlonassPerSlotIfbKey(key)) continue;
                        const auto hit = h2local.find(static_cast<int>(sidx));
                        if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                    }
                    for (const auto& [sat, sidx] : st.sm->getIonosphereStates()) {
                        if (sat.empty() || sat.front() != 'R') continue;  // GLONASS iono only
                        const auto hit = h2local.find(static_cast<int>(sidx));
                        if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                    }
                    const int ni = static_cast<int>(anchor_cols.size());
                    if (ni > 0) {
                        constexpr double kGloAnchorWeight = 1.0;  // 1/(1 m)^2
                        const Eigen::Index m0 = R.A_s.rows();
                        R.A_s.conservativeResize(m0 + ni, Eigen::NoChange);
                        R.A_s.bottomRows(ni).setZero();
                        R.A_r.conservativeResize(m0 + ni, Eigen::NoChange);
                        R.A_r.bottomRows(ni).setZero();
                        R.w.conservativeResize(m0 + ni);
                        R.y.conservativeResize(m0 + ni);
                        for (int k = 0; k < ni; ++k) {
                            R.A_r(m0 + k, anchor_cols[k]) = 1.0;
                            R.w(m0 + k) = kGloAnchorWeight;
                            R.y(m0 + k) = 0.0;  // toward 0 (absolute solve, lin pt 0)
                        }
                    }
                }

                // Ionosphere constraint (W6): the between-satellite structure that
                // makes the satellite DCB observable against the free slant iono.
                // Co-estimated SH-VTEC field (network's own iono) when enabled, else
                // the external GIM.  Folded into the station's OWN block (+ the shared
                // SH border); inert when neither is on.
                if (config_.estimateNetworkIonoSh())
                    appendShIonoRows(st, est, modeled[i], h2local, se.time,
                                     R.A_s, R.A_r, R.w, R.y);
                else if (config_.estimateNetworkSatCodeBias())
                    appendGimIonoRows(st, modeled[i], h2local, se.time,
                                      R.A_s, R.A_r, R.w, R.y);

                // --- Phase-leveling carry: record present ambiguities + build the
                //     diagonal carried prior (Increment 1) ----------------------
                // For every present NON-pivot ambiguity: record it for the
                // post-solve stash, and — if a converged carry exists and is not
                // stale — build its diagonal prior (Λ=1/var_eff, η=Λ·(value_abs−
                // x_lin)) to inject into this station's local block AFTER addStation,
                // BEFORE reduce.  Pivot ambiguities are SKIPPED: their 1e10 pins
                // define the phase-bias gauge exactly, so a carried diagonal there is
                // redundant.  Pure per-station compute + read-only local_carry lookup
                // — no shared write, safe in the parallel produce phase.
                if (carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on)) {
                    const Eigen::VectorXd x_lin = st.sm->getStateVector();
                    std::vector<std::tuple<Eigen::Index, double, double>> inj;  // (col, Λ, η) — legacy diagonal path
                    // Card 1 (network.carry_conditional_coupling): each carried column
                    // instead becomes ONE pseudo-observation ROW coupling it to the free
                    // shared border — A_r:+1@col, A_s:+M_rj@(present free shared cols),
                    // y = (r̂_prev − r_lin) + Σ_present M_rj·ŝ_prev,j, and
                    // σ² = var_eff + Σ_departed M_rj²·P_prev,jj (a shared column ABSENT this
                    // epoch is marginalized into the row variance).  Built entirely from the
                    // stashed LocalCarry.coupling + this epoch's shared layout (est), so it is
                    // a pure per-station compute (no shared write) — safe in the parallel
                    // produce phase, appended to R.A_s/R.A_r/R.w/R.y like appendShIonoRows.
                    // An EMPTY coupling (legacy stash / warm-resume restore) degrades to a
                    // plain diagonal row (A_s all zero) — identical to the addStationPrior fold.
                    struct PendCplRow {
                        Eigen::Index local_col{0};
                        std::vector<std::pair<Eigen::Index, double>> shared;  ///< (present free shared col, M_rj)
                        double y{0.0};
                        double w{0.0};
                    };
                    std::vector<PendCplRow> crows;
                    // Card 1b (carry_cpl_joint): the joint-whitening ingredients — one
                    // CplRowRaw per coupling row + a station-local map from each DISTINCT
                    // departed shared identity to its M_w column (so rows sharing a departed
                    // column share the M_w column → their common departed direction stays
                    // CORRELATED, the between-row DD info the scalar path drops).  Unused on
                    // the scalar path.
                    std::vector<CplRowRaw> craw;
                    std::vector<double>    white_pw;   // P_w per M_w column (departed marginal var)
                    std::map<std::string, int> white_map;  // departed identity → M_w column
                    auto resolveSharedCol = [&est](const CoupledShared& cs) -> Eigen::Index {
                        try {
                            switch (cs.kind) {
                                case SharedParamKind::Clock:     return est.clockColumn(cs.sat);
                                case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cs.sat, cs.signal);
                                case SharedParamKind::CodeBias:  return est.codeBiasColumn(cs.sat, cs.signal);
                                case SharedParamKind::VtecSh:    return est.vtecCoeffColumn(cs.n, cs.m, cs.trig);
                            }
                        } catch (...) {}
                        return -1;  // departed (unregistered this epoch)
                    };
                    // Card 1b: stable identity key of a departed shared column (so the SAME
                    // departed parameter shared across a station's coupling rows maps to ONE
                    // M_w column).  Distinct family tags keep the kinds disjoint.
                    auto couplingId = [](const CoupledShared& cs) -> std::string {
                        switch (cs.kind) {
                            case SharedParamKind::Clock:     return "C|" + cs.sat;
                            case SharedParamKind::PhaseBias: return "P|" + cs.sat + "|" + cs.signal;
                            case SharedParamKind::CodeBias:  return "B|" + cs.sat + "|" + cs.signal;
                            case SharedParamKind::VtecSh:    return "V|" + std::to_string(cs.n) + "|"
                                                                    + std::to_string(cs.m) + "|" + cs.trig;
                        }
                        return {};
                    };
                    // Card 1d (carry_cpl_tu): the per-kind process noise (m²/s) driving a
                    // coupled shared column between epochs — EXACTLY the dense border fold's
                    // q ladder (Clock→q_clock, CodeBias→q_cb, PhaseBias→q_pb) plus the SH GM
                    // carry's per-degree q (q_per_degree/n², n=0 ⇒ q_per_degree).  The clock
                    // entry is used regardless of clock_model (under the two-state model the
                    // column moves by the oscillator Q instead; the scalar q_clock is the
                    // documented conservative v1 stand-in).
                    auto couplingQ = [&](const CoupledShared& cs) -> double {
                        switch (cs.kind) {
                            case SharedParamKind::Clock:     return q_clock;
                            case SharedParamKind::CodeBias:  return q_cb;
                            case SharedParamKind::PhaseBias: return q_pb;
                            case SharedParamKind::VtecSh:    return (cs.n == 0)
                                ? shc.q_per_degree
                                : shc.q_per_degree / static_cast<double>(cs.n * cs.n);
                        }
                        return 0.0;
                    };
                    // Card 1d: one buffered PRESENT-column term of a coupling row under the
                    // border time update — the κ-shrunk coefficient is held back so the row
                    // can be gauge-RE-PROJECTED before it is finalized.
                    struct TuEnt {
                        Eigen::Index c;   ///< shared column
                        double m_shr;     ///< κ-shrunk coefficient (post-restore = final)
                        double m_un;      ///< unshrunk harvested coefficient
                        double s_prev;    ///< last epoch's solved value (RHS term)
                        char   cc;        ///< constellation letter ('\0' = none/SH)
                        SharedParamKind kind{SharedParamKind::Clock};  ///< coupling kind
                    };
                    // Card 1d gauge re-projection: the per-column κ shrink must NOT move the
                    // row's gauge functionals.  The UNSHRUNK row is exactly gauge-invariant
                    // (Card 1e); the measured null directions are per-constellation with
                    // (near-)uniform weights WITHIN a kind (the estimability dump shows the
                    // clock gauge as a pure uniform CLK-column shift), so preserving the
                    // per-(constellation × kind) coefficient SUMS preserves every direction
                    // whose weights are constant within each kind — a superset of the
                    // observed gauges.  A gauge is a convention, not dynamics: the border
                    // process noise has no component along it, so the shrink acts only in
                    // the complement.  Restore each (constellation, kind) sum to its
                    // unshrunk value by distributing the deficit equally over the row's
                    // columns of that group (= the projection of the shrink delta onto the
                    // group direction restricted to the row's support).  Without this every
                    // shrunk row observes the gauge (the clock κ is by far the strongest
                    // shrink) and the W5 null-space gate withholds the epoch.
                    auto tuGaugeRestore = [](std::vector<TuEnt>& ents) {
                        std::map<std::pair<char, int>, std::pair<double, int>> deficit;
                        for (const TuEnt& e : ents)
                            if (e.cc && e.kind != SharedParamKind::VtecSh) {
                                auto& d = deficit[{e.cc, static_cast<int>(e.kind)}];
                                d.first += e.m_un - e.m_shr;
                                ++d.second;
                            }
                        for (TuEnt& e : ents)
                            if (e.cc && e.kind != SharedParamKind::VtecSh) {
                                const auto it = deficit.find({e.cc, static_cast<int>(e.kind)});
                                if (it != deficit.end() && it->second.second > 0)
                                    e.m_shr += it->second.first
                                             / static_cast<double>(it->second.second);
                            }
                    };
                    // Card 1d: true when the (kind, sat) coupled column had a reinit FOLD
                    // after `since_s` (this row's harvest time) — the q=∞ marginalization
                    // trigger, covering multi-epoch carry gaps.
                    auto tuReinitSince = [&](const CoupledShared& cs, double since_s) -> bool {
                        if (!carry_cpl_tu) return false;
                        const auto it = tu_reinit_hist.find({cs.kind, cs.sat});
                        return it != tu_reinit_hist.end() && it->second > since_s;
                    };
                    auto buildCplRow = [&](Eigen::Index col, double x_lin_val,
                                           const LocalCarry& lc, double var_eff,
                                           double gap) {
                        if (carry_cpl_joint) {
                            // Card 1b: same present/departed split as the scalar path, but the
                            // DEPARTED columns become shared M_w columns (marginalized JOINTLY
                            // at append) instead of a per-row scalar σ² addend.
                            CplRowRaw raw;
                            raw.local_col = col;
                            raw.var_eff   = var_eff;
                            double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
                            std::vector<TuEnt> ents;                 // Card 1d: buffered present terms
                            for (const CoupledShared& cs : lc.coupling) {
                                Eigen::Index c = resolveSharedCol(cs);
                                // Card 1d: a (kind, sat) column reinit-folded since this row's
                                // harvest is decorrelated from its stashed value — the coupling
                                // term's κ=0 (q=∞) limit IS the departed treatment, so route it
                                // to the joint M_w marginalization.  Card 1d-SH: VtecSh terms
                                // take the same route (GM border ≠ the κ update's RW model).
                                if (c >= 0 && (tuReinitSince(cs, lc.last_seen_s)
                                               || (carry_cpl_shmarg
                                                   && cs.kind == SharedParamKind::VtecSh)))
                                    c = -1;
                                if (c < 0) {                          // DEPARTED → M_w column
                                    const std::string id = couplingId(cs);
                                    auto it = white_map.find(id);
                                    int widx;
                                    if (it == white_map.end()) {
                                        widx = static_cast<int>(white_pw.size());
                                        white_map.emplace(id, widx);
                                        white_pw.push_back(cs.p_prev);
                                    } else {
                                        widx = it->second;
                                        // Rows harvested at different epochs may carry slightly
                                        // different P_prev for the same departed column; take the
                                        // larger (conservative) so the shared marginalization is
                                        // never optimistic.
                                        if (cs.p_prev > white_pw[static_cast<std::size_t>(widx)])
                                            white_pw[static_cast<std::size_t>(widx)] = cs.p_prev;
                                    }
                                    raw.departed.emplace_back(widx, cs.coeff);
                                    ++R.cpl_terms_departed;
                                    continue;
                                }
                                ++R.cpl_terms_present;
                                // Card 1d: shrink the PRESENT column by κ_j and inflate this
                                // row's own diagonal by M²·κ_j·q_j·Δt — identical to the scalar
                                // path below; only the departed-column treatment differs (M_w).
                                if (carry_cpl_tu) {
                                    const double qj = couplingQ(cs) * gap;
                                    double m = cs.coeff;
                                    if (qj > 0.0 && cs.p_prev > 0.0) {
                                        const double kap = cs.p_prev / (cs.p_prev + qj);
                                        raw.var_eff += cs.coeff * cs.coeff * kap * qj;
                                        m = kap * cs.coeff;
                                        ++R.cpl_terms_shrunk;
                                    }
                                    ents.push_back(TuEnt{c, m, cs.coeff, cs.s_prev,
                                                         cs.sat.empty() ? '\0' : cs.sat[0],
                                                         cs.kind});
                                    continue;
                                }
                                raw.shared.emplace_back(c, cs.coeff);
                                rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
                            }
                            if (carry_cpl_tu) {
                                tuGaugeRestore(ents);   // shrink only gauge-orthogonally
                                for (const TuEnt& e : ents) {
                                    raw.shared.emplace_back(e.c, e.m_shr);
                                    rhs += e.m_shr * e.s_prev;
                                }
                            }
                            if (!(raw.var_eff > 0.0) || !std::isfinite(rhs)) return;  // never poison
                            raw.rhs = rhs;
                            craw.push_back(std::move(raw));
                            return;
                        }
                        // Card 1 scalar path (unchanged — byte-identical when carry_cpl_joint off).
                        PendCplRow row;
                        row.local_col = col;
                        double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
                        double dep_var = 0.0;                    // Σ_departed M_rj²·P_prev,jj
                        double tu_var  = 0.0;                    // Card 1d: Σ_present M_rj²·κ_j·q_j·Δt
                        std::vector<TuEnt> ents;                 // Card 1d: buffered present terms
                        for (const CoupledShared& cs : lc.coupling) {
                            const Eigen::Index c = resolveSharedCol(cs);
                            if (c < 0) {                          // DEPARTED → marginalize into σ²
                                dep_var += cs.coeff * cs.coeff * cs.p_prev;
                                ++R.cpl_terms_departed;
                                continue;
                            }
                            // Card 1d: a (kind, sat) column reinit-folded since this row's
                            // harvest is the q=∞ limit of the time update: fully marginalize
                            // (κ=0), mirroring the dense fold's decorrelation rule.  Card 1d-SH:
                            // VtecSh terms take the same treatment (GM border ≠ RW κ model).
                            if (tuReinitSince(cs, lc.last_seen_s)
                                || (carry_cpl_shmarg && cs.kind == SharedParamKind::VtecSh)) {
                                dep_var += cs.coeff * cs.coeff * cs.p_prev;
                                ++R.cpl_terms_departed;
                                continue;
                            }
                            ++R.cpl_terms_present;
                            if (carry_cpl_tu) {
                                // Card 1d: the row observed u = r + M_rᵀs at HARVEST; the border
                                // has since moved s⁺ = s + w, w~N(0,Q).  Exact re-independence
                                // against s⁺: shrink M' = κ·M_r (coefficient AND its ŝ_prev RHS
                                // term) and inflate σ² by M²·κ·q·Δt = Mᵀ(P⁻¹+Q⁻¹)⁻¹M per column.
                                const double qj = couplingQ(cs) * gap;
                                double m = cs.coeff;
                                if (qj > 0.0 && cs.p_prev > 0.0) {
                                    const double kap = cs.p_prev / (cs.p_prev + qj);
                                    tu_var += cs.coeff * cs.coeff * kap * qj;
                                    m = kap * cs.coeff;
                                    ++R.cpl_terms_shrunk;
                                }
                                ents.push_back(TuEnt{c, m, cs.coeff, cs.s_prev,
                                                     cs.sat.empty() ? '\0' : cs.sat[0],
                                                     cs.kind});
                                continue;
                            }
                            row.shared.emplace_back(c, cs.coeff);
                            rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
                        }
                        if (carry_cpl_tu) {
                            tuGaugeRestore(ents);   // shrink only gauge-orthogonally
                            for (const TuEnt& e : ents) {
                                row.shared.emplace_back(e.c, e.m_shr);
                                rhs += e.m_shr * e.s_prev;
                            }
                        }
                        const double sigma2 = var_eff + dep_var + tu_var;
                        if (!(sigma2 > 0.0) || !std::isfinite(rhs)) return;  // never poison the row
                        row.y = rhs;
                        row.w = 1.0 / sigma2;
                        crows.push_back(std::move(row));
                    };
                    // Ambiguity carry.  Legacy: the whole pivot station is skipped (all
                    // its ambiguities are gauge pins).  Prioritized: every station
                    // carries the arcs it does NOT anchor, so the skip is PER-(sat,
                    // signal) — an anchored ambiguity's 1e10 pin already defines the
                    // gauge, a carried diagonal there is redundant (isGaugePinned).
                    // Inject and harvest partition on the SAME predicate so a pinned
                    // amb never gets a spurious carry that fights its pin.  B9 (P1-4)
                    // keeps the ZWD carry below OUT of any pivot/anchor exclusion.
                    // Card 4 supersedes the phase-leveling ambiguity carry as a solve input
                    // (the DD carry is the ambiguity time-link); skip the inject to avoid
                    // double-counting.  Its harvest still feeds the VCE monitor + sticky tree.
                    const bool do_amb_carry =
                        (phase_datum_prioritized || !st.is_pivot) && !carry_invariant;
                    if (do_amb_carry)
                    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts) continue;
                        if (isGaugePinned(i, parts->satellite, parts->signal))
                            continue;  // anchored (gauge-pinned) — never carried
                        const int amb_idx = static_cast<int>(sidx);
                        if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                        const auto hit = h2local.find(amb_idx);
                        if (hit == h2local.end()) continue;  // ambiguity column not in A_r this epoch
                        const Eigen::Index col = static_cast<Eigen::Index>(hit->second);
                        R.pl_amb.push_back(PlAmb{col, parts->satellite, parts->signal,
                                                 x_lin(amb_idx), key});

                        // A pending re-seed marks the prior arc value as no longer
                        // trustworthy (the network's analogue of a slip): re-float from
                        // data this epoch rather than clamp to the stale carry.  The
                        // stash below then captures the fresh value.  (Slip-driven
                        // REFLOAT/DROP proper is Increment 2+; this is the cheap guard
                        // for the only reset trigger the network can raise today.)
                        if (st.sm->isAmbiguityPendingReseed(key)) continue;
                        if (!std::isfinite(x_lin(amb_idx))) continue;  // non-finite state → don't inject

                        const auto cit = local_carry.find(
                            std::make_tuple(i, parts->satellite, parts->signal));
                        if (cit == local_carry.end()) continue;       // no carry yet (new arc)
                        const double gap = pl_now_s - cit->second.last_seen_s;
                        if (gap > pl_max_gap_s) continue;             // stale — housekeeping will drop it
                        // var_eff = var + q·gap, floored by the optional lock-in guard
                        // (var_floor>0 caps Λ so a slowly-biasing ambiguity can never
                        // become a rigid prior the data cannot override).  Only present
                        // (data-bearing) columns are injected, so a tiny Λ never leaves a
                        // column PD-held by the prior alone — the §3.4 floor is not
                        // load-bearing here; this guards the OPPOSITE (lock-in) direction.
                        double var_eff = cit->second.var + pl_q_amb * std::max(0.0, gap);
                        if (pl_var_floor > 0.0) var_eff = std::max(var_eff, pl_var_floor);
                        if (!(var_eff > 0.0)) continue;
                        if (carry_cpl_cond) {
                            buildCplRow(col, x_lin(amb_idx), cit->second, var_eff,
                                        std::max(0.0, gap));
                            continue;
                        }
                        const double lambda = 1.0 / var_eff;
                        const double eta = lambda * (cit->second.value_abs - x_lin(amb_idx));
                        if (!std::isfinite(eta)) continue;            // belt-and-suspenders: never poison u_r
                        inj.emplace_back(col, lambda, eta);
                    }
                    // --- ZWD carry (B9): treat the tropo ZWD column like one more carried
                    //     local state (random WALK, not random constant → positive q).  The
                    //     prior tuple joins `inj` so it is ordered by the same ascending sort
                    //     and folded by the single addStationPrior inject (byte-identical OFF).
                    if (zwd_on) {
                        const size_t tropo_idx_sz = st.sm->getTropoIndex();
                        if (tropo_idx_sz != std::numeric_limits<size_t>::max()) {
                            const int tropo_idx = static_cast<int>(tropo_idx_sz);
                            if (tropo_idx >= 0 && tropo_idx < x_lin.size()) {
                                const auto zhit = h2local.find(tropo_idx);
                                if (zhit != h2local.end() && std::isfinite(x_lin(tropo_idx))) {
                                    const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                                    R.pl_zwd_present = true;   // record for the post-solve stash
                                    R.pl_zwd = PlZwd{zcol, x_lin(tropo_idx)};
                                    const auto zit = zwd_carry.find(static_cast<int>(i));
                                    if (zit != zwd_carry.end()) {
                                        const double zgap = pl_now_s - zit->second.last_seen_s;
                                        if (zgap <= pl_max_gap_s) {
                                            const double zvar_eff =
                                                zit->second.var + zwd_q * std::max(0.0, zgap);
                                            if (zvar_eff > 0.0) {
                                                if (carry_cpl_cond) {
                                                    buildCplRow(zcol, x_lin(tropo_idx),
                                                                zit->second, zvar_eff,
                                                                std::max(0.0, zgap));
                                                    R.pl_zwd_injected = true;   // L7: a carry was actually folded
                                                } else {
                                                    const double zlambda = 1.0 / zvar_eff;
                                                    const double zeta =
                                                        zlambda * (zit->second.value_abs - x_lin(tropo_idx));
                                                    if (std::isfinite(zeta)) {
                                                        inj.emplace_back(zcol, zlambda, zeta);
                                                        R.pl_zwd_injected = true;   // L7: a carry was actually folded
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (carry_cpl_cond && carry_cpl_joint) {
                        // Card 1b: JOINTLY whiten this station's coupling rows and append them
                        // as unit-information rows (mirroring appendShIonoRows).  Retain the
                        // pre-whitening ingredients so a within-epoch RAIM refloat can
                        // re-whiten the surviving rows.  R.carry_cols stays EMPTY (the coupling
                        // lives in R.A_s/R.A_r, not the addStationPrior diagonal).
                        if (!craw.empty()) {
                            Eigen::MatrixXd Hr, Hs;
                            Eigen::VectorXd z;
                            const bool ok = whitenCouplingRows(craw, white_pw, /*skip=*/{},
                                                               R.A_r.cols(), R.A_s.cols(), Hr, Hs, z);
                            const Eigen::Index add = static_cast<Eigen::Index>(craw.size());
                            const Eigen::Index m0  = R.A_s.rows();
                            R.A_s.conservativeResize(m0 + add, Eigen::NoChange);
                            R.A_r.conservativeResize(m0 + add, Eigen::NoChange);
                            R.w.conservativeResize(m0 + add);
                            R.y.conservativeResize(m0 + add);
                            R.cpl_row_start = m0;                       // within-epoch-RAIM refloat handle
                            R.cpl_row_local_cols.resize(static_cast<size_t>(add));
                            for (Eigen::Index k = 0; k < add; ++k)
                                R.cpl_row_local_cols[static_cast<size_t>(k)] =
                                    craw[static_cast<size_t>(k)].local_col;
                            if (ok && Hr.rows() == add) {
                                R.A_s.bottomRows(add) = Hs;
                                R.A_r.bottomRows(add) = Hr;
                                R.w.segment(m0, add).setOnes();         // unit-information whitened rows
                                R.y.segment(m0, add) = z;
                                R.cpl_raw      = std::move(craw);        // for the refloat re-whiten
                                R.cpl_white_pw = std::move(white_pw);
                            } else {
                                // Non-SPD joint covariance after the ridge retry → fall back to
                                // the Card-1 per-row scalar marginalization for THIS station this
                                // epoch (dep_var = Σ_departed M_rj²·P_w = the joint diagonal), and
                                // leave cpl_raw EMPTY so the refloat takes the scalar zero-row path.
                                R.cpl_joint_fallback = true;
                                R.A_s.bottomRows(add).setZero();
                                R.A_r.bottomRows(add).setZero();
                                for (Eigen::Index k = 0; k < add; ++k) {
                                    const CplRowRaw& row = craw[static_cast<size_t>(k)];
                                    double dep_var = 0.0;
                                    for (const auto& dc : row.departed)
                                        dep_var += dc.second * dc.second
                                                 * white_pw[static_cast<size_t>(dc.first)];
                                    const double sigma2 = row.var_eff + dep_var;
                                    R.A_r(m0 + k, row.local_col) = 1.0;
                                    for (const auto& sc : row.shared)
                                        R.A_s(m0 + k, sc.first) = sc.second;
                                    R.w(m0 + k) = (sigma2 > 0.0) ? 1.0 / sigma2 : 0.0;
                                    R.y(m0 + k) = row.rhs;
                                }
                            }
                        }
                    } else if (carry_cpl_cond) {
                        // Card 1: batch-append the coupling rows to this station's block
                        // (one conservativeResize, mirroring appendShIonoRows).  R.carry_cols
                        // is left EMPTY so the serial consume's addStationPrior is skipped —
                        // the coupling now lives entirely in R.A_s/R.A_r consumed by addStation.
                        const Eigen::Index add = static_cast<Eigen::Index>(crows.size());
                        if (add > 0) {
                            const Eigen::Index m0 = R.A_s.rows();
                            R.A_s.conservativeResize(m0 + add, Eigen::NoChange);
                            R.A_s.bottomRows(add).setZero();
                            R.A_r.conservativeResize(m0 + add, Eigen::NoChange);
                            R.A_r.bottomRows(add).setZero();
                            R.w.conservativeResize(m0 + add);
                            R.y.conservativeResize(m0 + add);
                            R.cpl_row_start = m0;                       // within-epoch-RAIM refloat handle
                            R.cpl_row_local_cols.resize(static_cast<size_t>(add));
                            for (Eigen::Index k = 0; k < add; ++k) {
                                const PendCplRow& row = crows[static_cast<size_t>(k)];
                                R.A_r(m0 + k, row.local_col) = 1.0;
                                for (const auto& sc : row.shared)
                                    R.A_s(m0 + k, sc.first) = sc.second;
                                R.w(m0 + k) = row.w;
                                R.y(m0 + k) = row.y;
                                R.cpl_row_local_cols[static_cast<size_t>(k)] = row.local_col;
                            }
                        }
                    } else {
                        // ASCENDING column order → deterministic rank-1 update sequence.
                        std::sort(inj.begin(), inj.end(),
                                  [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                        const Eigen::Index ninj = static_cast<Eigen::Index>(inj.size());
                        R.carry_cols.resize(static_cast<size_t>(ninj));
                        R.carry_dNrr.resize(ninj);
                        R.carry_dur.resize(ninj);
                        for (Eigen::Index k = 0; k < ninj; ++k) {
                            R.carry_cols[static_cast<size_t>(k)] = std::get<0>(inj[static_cast<size_t>(k)]);
                            R.carry_dNrr(k) = std::get<1>(inj[static_cast<size_t>(k)]);
                            R.carry_dur(k)  = std::get<2>(inj[static_cast<size_t>(k)]);
                        }
                    }
                }
                R.ok = true;
            } catch (const std::exception& ex) {
                R.threw = true; R.err = ex.what();
            } catch (...) {
                R.threw = true; R.err = "unknown exception";
            }
        }

        // Card 1/1d observability: fold the per-station coupling-term counts into the
        // run totals (SERIAL — the parallel produce above only writes its own slot).
        if (carry_cpl_cond)
            for (const StationAssembly& Rc : asm_res) {
                n_cpl_terms_present  += Rc.cpl_terms_present;
                n_cpl_terms_departed += Rc.cpl_terms_departed;
                n_cpl_terms_shrunk   += Rc.cpl_terms_shrunk;
            }
        // Card 1d debug dump: the first few ASSEMBLED coupling rows of the run
        // (weight, RHS, A_s sparsity/magnitude) — the ground truth of what addStation
        // actually receives, for A/B-arm comparison.  One-shot budget; log-only.
        std::map<Eigen::Index, CoupledShared> cpl_col2desc_dbg;
        if (carry_cpl_cond && n_cpl_row_dump > 0) {
            for (const std::string& sat : registered_clock_sats) {
                try { CoupledShared d; d.kind = SharedParamKind::Clock; d.sat = sat;
                      cpl_col2desc_dbg.emplace(est.clockColumn(sat), std::move(d)); } catch (...) {}
            }
            for (const auto& pk : shared_phase_keys) {
                const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                if (!parts) continue;
                try { CoupledShared d; d.kind = SharedParamKind::PhaseBias;
                      d.sat = parts->satellite; d.signal = parts->signal;
                      cpl_col2desc_dbg.emplace(
                          est.phaseBiasColumn(parts->satellite, parts->signal), std::move(d)); } catch (...) {}
            }
            for (const auto& kv : registered_code_keys) {
                try { CoupledShared d; d.kind = SharedParamKind::CodeBias;
                      d.sat = kv.first; d.signal = kv.second;
                      cpl_col2desc_dbg.emplace(
                          est.codeBiasColumn(kv.first, kv.second), std::move(d)); } catch (...) {}
            }
        }
        if (carry_cpl_cond && n_cpl_row_dump > 0)
            for (size_t i = 0; i < asm_res.size() && n_cpl_row_dump > 0; ++i) {
                const StationAssembly& Rc = asm_res[i];
                if (!Rc.ok || Rc.cpl_row_start < 0) continue;
                const auto nrows = static_cast<Eigen::Index>(Rc.cpl_row_local_cols.size());
                for (Eigen::Index r = 0; r < nrows && n_cpl_row_dump > 0; ++r, --n_cpl_row_dump) {
                    const Eigen::Index row = Rc.cpl_row_start + r;
                    if (row >= Rc.A_s.rows()) break;
                    const Eigen::Index nnz =
                        (Rc.A_s.row(row).array() != 0.0).count();
                    // Exact-vanishing probe: a coupling row on a local column with ZERO
                    // data rows is absorbed COMPLETELY by the free local in the Schur
                    // elimination (adds nothing to the reduced border, any weight).
                    const Eigen::Index lc = Rc.cpl_row_local_cols[static_cast<size_t>(r)];
                    const Eigen::Index data_rows_on_col =
                        (Rc.A_r.col(lc).head(Rc.cpl_row_start).array() != 0.0).count();
                    // Gauge diagnostic: Σ of the row's A_s coefficients over each
                    // constellation's {clk, pb, cb} columns — ≈0 for a gauge-invariant
                    // row (the structural null direction has weight +1 on each).
                    std::map<char, double> gsum;
                    for (const auto& [col, d] : cpl_col2desc_dbg)
                        if (d.kind != SharedParamKind::VtecSh && col < Rc.A_s.cols()
                            && !d.sat.empty())
                            gsum[d.sat[0]] += Rc.A_s(row, col);
                    std::string gs;
                    for (const auto& [cch, v] : gsum) gs += fmt::format("{}:{:+.3e} ", cch, v);
                    log->info("Network (Card 1d dbg): TOW {:.1f} sta {} cplrow {} local_col {}: "
                              "w={:.9e} y={:.9e} A_s nnz={} max|A_s|={:.9e} data_rows_on_local={} "
                              "gauge_sums[{}]",
                              se.time.tow, i, r, lc, Rc.w(row), Rc.y(row), nnz,
                              nnz > 0 ? Rc.A_s.row(row).cwiseAbs().maxCoeff() : 0.0,
                              data_rows_on_col, gs);
                }
            }

        // Serial consume: scatter each station's block into the shared arrowhead
        // in deterministic station order, preserving the exact pivot-aborts-epoch
        // / drop-non-pivot disposition of the original single-pass loop.
        int  total_pins = 0;
        bool epoch_ok   = true;
        // est-station index per station (the order addStation was called) so the
        // post-solve back-substitution can recover each present station's local
        // slant-iono block; -1 for stations not added this epoch.
        std::vector<int> est_index_of(stations.size(), -1);
        for (size_t i = 0; i < stations.size() && epoch_ok; ++i) {
            if (!present[i]) continue;
            NetStation&      st = stations[i];
            StationAssembly& R  = asm_res[i];

            if (R.empty_innov) { present[i] = false; continue; }
            if (R.state_mismatch) {
                log->warn("Network: station '{}' H/state mismatch ({})", st.name, R.err);
                // Legacy: the single pivot carries the whole datum, so its loss sinks
                // the epoch.  Prioritized: the gauge is distributed across anchors, so
                // drop this station like any other — any keys it anchored are pruned
                // by the null-space gate (drop-and-heal; the epoch self-heals next).
                if (!phase_datum_prioritized && st.is_pivot) { epoch_ok = false; break; }
                present[i] = false;
                continue;
            }
            if (R.threw || !R.ok) {
                // computeMeasurementUpdate / splitStation threw.  The original
                // single-pass loop did not guard these (an exception escaped the
                // run); under threads they MUST be caught, so their disposition is
                // decided here, matching the addStation policy below: a failed
                // pivot sinks the epoch, any other station is dropped.
                const std::string why = R.err.empty() ? "no usable block" : R.err;
                if (!phase_datum_prioritized && st.is_pivot) {
                    log->warn("Network: PIVOT '{}' assembly failed ({}); skipping epoch",
                              st.name, why);
                    epoch_ok = false;
                    break;
                }
                log->warn("Network: station '{}' assembly failed ({}); dropping it this epoch",
                          st.name, why);
                present[i] = false;
                continue;
            }

            total_pins += R.np;
            if (R.cpl_joint_fallback) ++n_cpl_joint_fallback;   // Card 1b: joint LLT degraded to scalar

            try {
                est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
            } catch (const std::exception& ex) {
                // One station's rank-deficient / ill-conditioned local block must
                // not sink the whole epoch in a large heterogeneous network: drop
                // it and keep the rest (the null-space gate still guards the
                // result).  On the legacy path the PIVOT is the exception — it
                // carries the S-basis, so without it the epoch has no datum and must
                // be skipped.  Prioritized: the gauge is distributed, so drop it too
                // (an anchor loss is caught by the null-space gate → withhold + heal).
                if (!phase_datum_prioritized && st.is_pivot) {
                    log->warn("Network: PIVOT '{}' addStation rejected ({}); skipping epoch",
                              st.name, ex.what());
                    epoch_ok = false;
                    break;
                }
                log->warn("Network: station '{}' addStation rejected ({}); dropping it this epoch",
                          st.name, ex.what());
                present[i] = false;
                continue;
            }

            // Phase-leveling carry: fold this station's diagonal carried prior into
            // its local block NOW (after addStation, before the gate/solve trigger
            // reduce) so the single arrowhead solve already reflects it.  The pivot's
            // ambiguity columns are gauge-pinned (never carried), but its ZWD column
            // may be (B9), so gate on a non-empty carry set — not on !is_pivot: for the
            // pivot R.carry_cols holds at most the ZWD column, and is empty (no call,
            // byte-identical) unless network.carry_zwd is on.  A reject here drops only
            // this station's carry, not the station — its data is already in the system.
            if (carry_warm && !R.carry_cols.empty()) {
                try {
                    est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                        R.carry_cols, R.carry_dNrr, R.carry_dur);
                } catch (const std::exception& ex) {
                    log->warn("Network[phase-leveling]: station '{}' carry inject rejected "
                              "({}); proceeding without its carry this epoch", st.name, ex.what());
                }
            }
        }

        // D0: close the assembly wall (produce + scatter).  Accumulated even when the
        // epoch skips below — the assembly work was still spent.
        if (sec_timers_on) {
            sec_asm_ms_epoch = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sec_asm_t0).count();
            sec_asm_wall_ms += sec_asm_ms_epoch;
        }

        if (!epoch_ok || est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << 0 << ",0,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("sync_skip", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }
        subBegin(SB_GAUGEBUILD); // D0b: buildEnsembleGauges + carry-driver lambda defs (pre-DD-build)

        // Fix A (network.phase_datum_ensemble): build the anchor-free sum-to-zero
        // phase-bias gauges from every tracking station's ambiguity column and hand
        // them to the estimator, which folds Σ_r zˢ_{r,σ}=0 through the kernel
        // (Schur-projected) at the gate/solve reduce.  One gauge per (sat,signal)
        // that has a registered shared phase-bias column (thin/gated keys have no
        // column → their ambiguity floats).  Re-invoked on the within-epoch RAIM
        // re-solve (station set / indices may change).  Empty / no-op when OFF.
        auto buildEnsembleGauges = [&]() {
            if (!phase_datum_ensemble) return;
            // The (sat,signal) that actually have a shared phase-bias column this
            // epoch (a gauge is needed only for those; thin/gated signals float).
            std::set<std::string> shared_ss;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) shared_ss.insert(p->satellite + "\x1f" + p->signal);
            }
            // Seed-once + time-lock: a (sat,signal) that already carries a HELD bias
            // (from the previous epoch's D2 stash) is datumed by that stiff carried
            // prior — do NOT also fold Σz=0 for it (a hard w→∞ fold would override the
            // finite hold prior and re-realize the churny level).  Only fresh /
            // re-seeded (sat,signal) get a gauge this epoch.  Empty when hold is off.
            std::set<std::string> held;
            if (phase_hold_on)
                for (const CarryPrior& cp : prev_carry)
                    // var>0 mirrors the hold-prior injector's guard EXACTLY, so a
                    // (sat,signal) is fold-skipped iff it will actually get a hold
                    // prior — a var<=0 carry (e.g. a corrupt warm-resume snapshot) is
                    // NOT held here, so it re-seeds via the fold instead of ending up
                    // un-datumed.
                    if (cp.kind == SharedParamKind::PhaseBias && cp.var > 0.0)
                        held.insert(cp.sat + "\x1f" + cp.signal);
            std::map<std::string, NetworkNormalEquations::SumZeroGauge> by_key;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                for (const auto& [sat, sig, col] : asm_res[i].ens_amb_cols) {
                    const std::string ss = sat + "\x1f" + sig;
                    if (!shared_ss.count(ss)) continue;   // no shared phase bias → floats
                    if (held.count(ss)) continue;         // held by the carried prior → skip the fold
                    by_key[ss].terms.emplace_back(
                        static_cast<std::size_t>(est_index_of[i]), col);
                }
            }
            std::vector<NetworkNormalEquations::SumZeroGauge> gauges;
            gauges.reserve(by_key.size());
            for (auto& [k, gz] : by_key) {
                if (gz.terms.empty()) continue;
                // Record the shared phase-bias column this gauge fixes, so the
                // estimator can treat it as an EXACT datum column in the datum-frame
                // conditional covariance (the fold leaves it carrying an artificial
                // gauge-direction variance — see SumZeroGauge::shared_col).  k is
                // "sat\x1fsignal"; every key in by_key came from shared_ss, so it has
                // a registered phase-bias column.
                const auto us = k.find('\x1f');
                if (us != std::string::npos) {
                    const std::string sat = k.substr(0, us);
                    const std::string sig = k.substr(us + 1);
                    try { gz.shared_col = est.phaseBiasColumn(sat, sig); }
                    catch (...) { gz.shared_col = -1; }
                }
                gauges.push_back(std::move(gz));
            }
            est.setPhaseSumZeroGauges(std::move(gauges));
        };
        buildEnsembleGauges();

        // Card 4 (M1) C3c: within-epoch RAIM culprit set forwarded into the DD-carry
        // re-solve rebuild (spec §3 culprit-withhold).  The pointers alias the RAIM
        // block's drop sets while it runs, else null.  The PASS-A slip feed
        // (carry_slipped_ambs, above) is the primary source; both are carry_invariant-only
        // ⇒ OFF byte-identical (buildCarryDdPriors early-returns when the toggle is off).
        const std::set<std::string>*                carry_we_drop_sats = nullptr;
        const std::map<int, std::set<std::string>>* carry_we_drop_arcs = nullptr;

        // Card 4 (M1) D5: reverse map (group key → chord ArcKey → ambiguity state key) so a
        // post-solve innovation flag (group, chord) maps back to its (station, ambiguity) for
        // the next epoch's slip funnel.  Rebuilt with the blocks each buildCarryDdPriors call
        // (the last call before the solve is authoritative).  Empty unless carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key;
        // WL-3: the pair groups' BAND-2 ambiguity keys, parallel to carry_arc_amb_key
        // (band-1).  A D5 flag on a pair chord feeds BOTH bands into the slip funnel
        // (§3.4 union refloat).  Empty unless lane_carry + carry_innov_gate.
        std::map<int, std::map<ArcKey, std::string>> carry_arc_amb_key2;

        // Card 4 (M1) C3b: build this epoch's DD-prior blocks from the eq34 election
        // tree and fold the carried invariant DDs into the producer solve.  Each arc
        // (receiver, satellite) ambiguity on a (const,signal) is resolved to its
        // estimator (station, local ambiguity column); pinned_amb_targets marks the
        // spanning-tree edges (the gauge), the rest are chords (the carried DDs).
        // GLONASS (FDMA) excluded.  Vertex/group ids are stable across epochs (sat
        // label anchored; receiver = app index).  Re-invoked on the RAIM re-solve
        // (est.beginEpoch there clears the priors + rebuilds est_index_of), mirroring
        // buildEnsembleGauges.  No-op / OFF byte-identical unless carry_invariant.
        auto buildCarryDdPriors = [&]() {
            if (!carry_invariant) return;
            constexpr int kSatBase = 1000000;   // satellite vertices sit above every receiver index
            std::map<int, NetworkInvariantCarryDriver::GroupInput> groups;
            if (carry_innov_gate) { carry_arc_amb_key.clear(); carry_arc_amb_key2.clear(); }  // D5: rebuilt with this call's arcs

            // --- Shared WL-1/WL-2/WL-3 machinery (revmap / arc_signals / lin_of / iono_of /
            // primary / band_top_of / λ cache).  HOISTED above the group build because the WL-3
            // pair regroup routes each primary-band ambiguity by its (rx,sat) primary pair.
            // Byte-identical when every lane is off: nothing here is populated, and building
            // it earlier has no effect on the solve (pure reads of station state).
            const bool mw_mach_on = mw_wl_on || ewl_on || lane_on;
            auto sysFromSat = [](const std::string& sat) -> System {
                if (sat.empty()) return System::Unknown;
                switch (sat.front()) {
                    case 'G': return System::GPS;     case 'E': return System::Galileo;
                    case 'R': return System::GLONASS; case 'C': return System::BeiDou;
                    case 'J': return System::QZSS;    case 'S': return System::SBAS;
                    default:  return System::Unknown;
                }
            };
            // Reverse map (est_station,col)->arc identity and per-(rx,sat) ambiguity
            // signals over ALL resolvable ambiguities (both bands, incl. those NOT in
            // shared_phase_keys — the WL row needs the L2 column even when L2 is not an
            // eq34 arc).  GLONASS excluded (never enters the carry blocks anyway).
            struct Rev { int rx; std::string sat; std::string signal; };
            std::map<std::pair<int, int>, Rev> revmap;
            std::map<std::pair<int, std::string>, std::vector<std::pair<std::string, int>>> arc_signals;
            // (est_station, local col) → ambiguity LINEARIZATION value (m).  The kernel
            // solves corrections about x_lin (no KF write-back), so the WL layer must
            // re-express the absolute MW measurement in the correction frame (F5).
            std::map<std::pair<int, int>, double> lin_of;
            // WL-2 §3.4 v2: (rx app index, sat) → the estimated L1-referenced slant iono (m)
            // for the bridge de-bias.  The v1 Welford-only guard is falsified by a STABLE
            // differential iono (Fable): a constant iono bias has ~0 Welford variance yet
            // biases the rounded WL integer (near-integer bias ⇒ slips the FFR frac gate).
            // We subtract the network's own iono estimate; the residual DD-iono estimate
            // error surfaces in |frac| and is caught by the FFR gate + MW(1,2) closure.
            std::map<std::pair<int, std::string>, double> iono_of;
            // Estimator primary pair per (rx,sat): the two lowest DISTINCT bands (ties
            // lexicographic for determinism); v1 restricts WL rows to this pair (§11 Q3).
            struct PP { std::string sig1, sig2; int col1{ -1 }, col2{ -1 }; bool ok{ false }; };
            std::map<std::pair<int, std::string>, PP> primary;
            // WL-2 EWL: the band-5 (GPS L5 / Galileo E5a) signal + column per (rx,sat),
            // for the (band2, band5) extra-wide lane.  arc_signals is sorted by band digit
            // then name, so the first '5' is deterministic.
            std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top_of;
            // Increment 5 (uwl_dual_top): the FALLBACK top rung's signal + column per
            // (rx,sat) (Galileo '7' when the preferred b_top is '8').  Empty unless a
            // tier arms a fallback rung; the fallback lane instance's resolver and the
            // walk's fallback-tagged rows read THIS map instead of band_top_of.
            std::map<std::pair<int, std::string>, std::pair<std::string, int>> band_top2_of;
            // E6 mid hop (uwl_mid_enable): the mid-rung band signal per (rx,sat) — only
            // the SIGNAL is needed (the hops are phase-only detector reads; no estimator
            // column enters the chain).  Empty unless the tier arms a mid rung.
            std::map<std::pair<int, std::string>, std::string> band_mid_of;
            // WL-3: the constellation's ONE census-preferred primary pair — only arcs whose
            // primary pair matches it join the pair group; minority-channel arcs stay fully
            // per-band (as today), so they keep their own carried cycles instead of spawning
            // flickering micro pair-groups.  The third (EWL) band never becomes a pair band.
            std::map<char, std::pair<std::string, std::string>> lane_pref_pair;
            // Per-(sat,signal) wavelength cache (FDMA-correct via the satellite id).
            std::map<std::pair<std::string, std::string>, double> lam_cache;
            auto lam = [&](const std::string& sat, const std::string& sig) -> double {
                const auto k = std::make_pair(sat, sig);
                const auto it = lam_cache.find(k);
                if (it != lam_cache.end()) return it->second;
                const double v = mw_wl_ppp_utils->getWavelength(sysFromSat(sat), sig, sat);
                lam_cache.emplace(k, v);
                return v;
            };
            if (mw_mach_on) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0) continue;
                    const int es = est_index_of[i];
                    const auto& h2l = asm_res[i].h2local;
                    const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                    for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts || parts->satellite.empty() || parts->signal.empty()) continue;
                        if (parts->satellite.front() == 'R') continue;   // GLONASS excluded
                        const auto hit = h2l.find(static_cast<int>(sidx));
                        if (hit == h2l.end() || hit->second < 0) continue;
                        const int col = static_cast<int>(hit->second);
                        revmap[{es, col}] = { static_cast<int>(i), parts->satellite, parts->signal };
                        arc_signals[{static_cast<int>(i), parts->satellite}]
                            .push_back({ parts->signal, col });
                        if (static_cast<Eigen::Index>(sidx) < x_lin.size())
                            lin_of[{es, col}] = x_lin(static_cast<Eigen::Index>(sidx));
                    }
                    if (wl_inject_on || lane_bridge_on)
                        for (const auto& [sat, iidx] : stations[i].sm->getIonosphereStates()) {
                            const Eigen::Index ii = static_cast<Eigen::Index>(iidx);
                            if (ii >= 0 && ii < x_lin.size())
                                iono_of[{ static_cast<int>(i), sat }] = x_lin(ii);
                        }
                }
                // WL-3: network-preferred channel per (constellation, band).  The
                // lexicographic tie-break fragments the primary pairs across stations
                // (L2L where tracked, L2W elsewhere), which splits the pair groups'
                // graphs and forfeits every cross-boundary cycle (~1/3 of the primary
                // cycle space at 50 stations, measured).  Under lane_carry the pair
                // prefers the constellation's MAJORITY channel per band (deterministic
                // census over this build's resolvable ambiguities; ties lexicographic),
                // falling back to the arc's own lowest channel where the majority one
                // is untracked.  WL-1/WL-2-only shapes keep the legacy tie-break.
                std::map<std::pair<char, char>, std::string> chan_pref;
                if (lane_on) {
                    std::map<std::pair<char, std::string>, long long> chan_count;
                    for (const auto& [rs, sigs] : arc_signals) {
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        for (const auto& [sig, col] : sigs) ++chan_count[{cons, sig}];
                    }
                    for (const auto& [k, cnt] : chan_count) {
                        const char band = k.second.size() >= 2 ? k.second[1] : '\0';
                        if (band == '\0') continue;
                        std::string& best = chan_pref[{k.first, band}];
                        long long bc = -1;
                        if (!best.empty()) {
                            const auto bit = chan_count.find({k.first, best});
                            bc = (bit != chan_count.end()) ? bit->second : -1;
                        }
                        if (best.empty() || cnt > bc || (cnt == bc && k.second < best))
                            best = k.second;
                    }
                    // UWL-1 (measured falsification of the per-band census): channel
                    // choices are CORRELATED per receiver family — Galileo splits into
                    // (L1C,L5Q) and (L1X,L5X) families, and the per-band majorities
                    // composed the PHANTOM pair (L1C,L5X) that NO station tracks: zero
                    // pair arcs formed and the whole constellation silently stayed
                    // per-band (group census, 10-ep GPS+GAL smoke).  Under the armed
                    // card the census is therefore JOINT: count each (rx,sat)'s raw
                    // two-lowest-band channel pair and elect the majority pair per
                    // constellation (ties lexicographic — the §8.2 deterministic seam),
                    // then steer the per-band preferences to the elected pair so
                    // preferChan / the bridge keys align with it.  The minority FAMILY
                    // stays fully per-band (v1 accepts ~half the Galileo pair space;
                    // per-family pair groups are the v2 recovery — the group keying
                    // already supports them).  uwl_enable=false keeps the per-band
                    // census verbatim (the WL-3 validated shape, byte-identical).
                    if (uwl_on) {
                        std::map<std::pair<char, std::pair<std::string, std::string>>,
                                 long long> pair_count;
                        for (auto& [rs, sigs] : arc_signals) {
                            std::sort(sigs.begin(), sigs.end(),
                                      [](const std::pair<std::string, int>& a,
                                         const std::pair<std::string, int>& b) {
                                const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                                const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                                if (ba != bb) return ba < bb;
                                return a.first < b.first;
                            });
                            if (sigs.empty()) continue;
                            const char cons = rs.second.empty() ? '?' : rs.second.front();
                            // Two lowest CARRIED bands (the uncarried top band never
                            // pairs — mirror the routing's exclusion exactly).
                            std::string s1;
                            char b1 = '\0';
                            for (const auto& [sig, col] : sigs)
                                if (sig.size() >= 2 && !isUncarriedThirdBand(cons, sig)) {
                                    s1 = sig; b1 = sig[1]; break;
                                }
                            if (b1 == '\0') continue;
                            for (const auto& [sig, col] : sigs)
                                if (sig.size() >= 2 && sig[1] != b1
                                    && !isUncarriedThirdBand(cons, sig)) {
                                    ++pair_count[{cons, { s1, sig }}];
                                    break;
                                }
                        }
                        std::map<char, std::pair<std::string, std::string>> joint_best;
                        std::map<char, long long> joint_cnt;
                        for (const auto& [k, cnt] : pair_count) {
                            auto itb = joint_best.find(k.first);
                            if (itb == joint_best.end() || cnt > joint_cnt[k.first]
                                || (cnt == joint_cnt[k.first] && k.second < itb->second)) {
                                joint_best[k.first] = k.second;
                                joint_cnt[k.first]  = cnt;
                            }
                        }
                        for (const auto& [cons, pr] : joint_best) {
                            const char b1 = pr.first.size()  >= 2 ? pr.first[1]  : '\0';
                            const char b2 = pr.second.size() >= 2 ? pr.second[1] : '\0';
                            if (b1 == '\0' || b2 == '\0') continue;
                            chan_pref[{cons, b1}] = pr.first;
                            chan_pref[{cons, b2}] = pr.second;
                        }
                    }
                }
                for (auto& [rs, sigs] : arc_signals) {
                    std::sort(sigs.begin(), sigs.end(),
                              [](const std::pair<std::string, int>& a,
                                 const std::pair<std::string, int>& b) {
                        const char ba = a.first.size() >= 2 ? a.first[1] : '\0';
                        const char bb = b.first.size() >= 2 ? b.first[1] : '\0';
                        if (ba != bb) return ba < bb;
                        return a.first < b.first;
                    });
                    PP pp;
                    if (!sigs.empty()) { pp.sig1 = sigs.front().first; pp.col1 = sigs.front().second; }
                    const char b1 = pp.sig1.size() >= 2 ? pp.sig1[1] : '\0';
                    for (const auto& [sig, col] : sigs) {
                        if (sig.size() >= 2 && b1 != '\0' && sig[1] != b1) {
                            pp.sig2 = sig; pp.col2 = col; break;
                        }
                    }
                    if (lane_on && !sigs.empty()) {
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        const char b2 = pp.sig2.size() >= 2 ? pp.sig2[1] : '\0';
                        auto preferChan = [&](char band, std::string& sig_io, int& col_io) {
                            const auto pit2 = chan_pref.find({cons, band});
                            if (pit2 == chan_pref.end() || pit2->second == sig_io) return;
                            for (const auto& [sig, col] : sigs)
                                if (sig == pit2->second) { sig_io = sig; col_io = col; return; }
                        };
                        if (b1 != '\0') preferChan(b1, pp.sig1, pp.col1);
                        if (b2 != '\0') preferChan(b2, pp.sig2, pp.col2);
                    }
                    pp.ok = (!pp.sig1.empty() && !pp.sig2.empty());
                    primary[rs] = pp;
                }
                // v2 (uwl_family_pairs): admit MINORITY family pairs into the run-latched
                // admissible set.  Counted over the STEERED primary pairs — the exact
                // quantity the routing below matches — NOT the raw two-lowest census
                // (measured defect, smoke E: L2L < L2W lexicographically, so every GPS
                // dual-tracker's RAW pair read (L1C,L2L) and inflated its count to 143
                // while routing saw 1 steered arc — a stranded 1-arc micro pair-group
                // that forfeits the arc's per-band cycles).  The majority pair routes
                // through lane_pref_pair exactly as v1 (its admission here is a harmless
                // duplicate); a minority pair is admitted the first epoch its steered
                // count reaches the floor, then stays latched (see the declaration).
                if (uwl_family_on) {
                    std::map<std::pair<char, std::pair<std::string, std::string>>,
                             long long> pp_count;
                    for (const auto& [rs, pp] : primary) {
                        if (!pp.ok) continue;
                        const char cons = rs.second.empty() ? '?' : rs.second.front();
                        ++pp_count[{ cons, { pp.sig1, pp.sig2 } }];
                    }
                    for (const auto& [k, cnt] : pp_count) {
                        if (cnt < nmw.uwl_family_min_arcs) continue;
                        if (lane_admit_pairs[k.first].insert(k.second).second
                            && lane_family_logged
                                   .insert({ k.first,
                                             k.second.first + "+" + k.second.second })
                                   .second)
                            log->info("Network (M5/UWL-1 family census): constellation '{}' "
                                      "admitted pair ({}, {}) — {} steered primary arc(s) "
                                      "(floor {}); latched for the run.",
                                      k.first, k.second.first, k.second.second, cnt,
                                      nmw.uwl_family_min_arcs);
                    }
                }
                if (lane_on) {
                    // The constellation's ONE preferred pair: the two lowest census bands,
                    // skipping the EWL third band (it must stay UNCARRIED — WL-2 §3.1).
                    std::map<char, std::vector<char>> cons_bands;
                    for (const auto& [k, sig] : chan_pref) {
                        const std::string probe = std::string("L") + k.second;   // "L<band>"
                        if (isUncarriedThirdBand(k.first, probe)) continue;
                        cons_bands[k.first].push_back(k.second);
                    }
                    for (auto& [cons, bands] : cons_bands) {
                        std::sort(bands.begin(), bands.end());
                        if (bands.size() < 2) continue;
                        lane_pref_pair[cons] = { chan_pref[{cons, bands[0]}],
                                                 chan_pref[{cons, bands[1]}] };
                        // Census visibility (once per run per cons): which pair the lane
                        // carries — fragmentation across channels is the WL-3 lesson and
                        // a Galileo pre-flight (SPEC_uwl_card.md §8.1); measure, don't assume.
                        if (lane_census_logged.insert(cons).second)
                            log->info("Network (M5/WL-3 census): constellation '{}' preferred "
                                      "primary pair = ({}, {}) over census bands [{}].",
                                      cons, lane_pref_pair[cons].first,
                                      lane_pref_pair[cons].second,
                                      std::string(bands.begin(), bands.end()));
                    }
                    // Align the detector-side bridge arcs with the census: without this,
                    // every (rx,sat) whose min-name channel differs from the census pair
                    // has NO bridge arc under the caller's key and the WL decision path
                    // starves (measured 9-of-29 decided cycles).  Applies from the next
                    // detector pass; the re-keyed arcs re-warm in ~2 epochs.
                    if (lane_bridge_on && !chan_pref.empty())
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (stations[i].mw_slip)
                                stations[i].mw_slip->setBridgePreferredChannels(chan_pref);
                }
                for (auto& [rs, sigs] : arc_signals) {
                    // UWL-1 §3.1: the top-tier partner band digit.  Legacy (card OFF) =
                    // first '5' for EVERY constellation — which is what silently killed
                    // Galileo (its '5' is E5a, the primary sig2 itself ⇒ the degenerate
                    // (L5Q,L5Q) pair with no detector arc).  Armed = the lane-tier table
                    // (GPS '5' reproduces the legacy pick bit-exactly; Galileo '7'/'8').
                    char top_digit  = '5';
                    char mid_digit  = '\0';
                    char top2_digit = '\0';   // increment 5: the fallback rung's digit
                    const char cons = rs.second.empty() ? '?' : rs.second.front();
                    if (uwl_on) {
                        const auto tier = mwLaneTierOf(cons, nmw.uwl_gal_top_band,
                                                       nmw.uwl_mid_enable, nmw.uwl_dual_top);
                        if (!tier) {
                            if (ewl_on && cons != '?' && uwl_warned_cons.insert(cons).second)
                                log->warn("Network (M5/UWL-1): constellation '{}' has NO lane "
                                          "tier in the armed table — its cycles contribute no "
                                          "EWL/UWL statistic and no bridge (BDS-3 deferred on "
                                          "clock divergence; GLONASS FDMA-excluded).  Loud by "
                                          "design (SPEC §3.1).", cons);
                            continue;   // no top-band entry ⇒ counted skip_missing_band downstream
                        }
                        top_digit  = tier->b_top;
                        mid_digit  = tier->b_mid;
                        top2_digit = tier->b_top2;
                    } else if (ewl_on && cons != 'G' && cons != '?'
                               && uwl_warned_cons.insert(cons).second) {
                        log->warn("Network (M5/UWL-1): constellation '{}' enters the EWL lane "
                                  "under the LEGACY GPS band digits — its top-tier pair is "
                                  "degenerate and every cycle dies silently as skip_missing_mw "
                                  "(SPEC_uwl_card.md §1 fact 3).  Arm network.mw_wl.uwl_enable "
                                  "for the per-constellation lane-tier table.", cons);
                    }
                    for (const auto& [sig, col] : sigs)
                        if (sig.size() >= 2 && sig[1] == top_digit) { band_top_of[rs] = { sig, col }; break; }
                    if (top2_digit != '\0')
                        for (const auto& [sig, col] : sigs)
                            if (sig.size() >= 2 && sig[1] == top2_digit) { band_top2_of[rs] = { sig, col }; break; }
                    if (mid_digit != '\0')
                        for (const auto& [sig, col] : sigs)
                            if (sig.size() >= 2 && sig[1] == mid_digit) { band_mid_of[rs] = sig; break; }
                }
            }
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const auto& h2l = asm_res[i].h2local;
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts || parts->satellite.empty()) continue;
                    const char cons = parts->satellite.front();
                    if (cons == 'R') {   // GLONASS FDMA: no common-λ integer DD (spec OPEN-3)
                        if (!carry_glonass_warned) {
                            log->warn("Network[Card4]: GLONASS ambiguities are EXCLUDED from the "
                                      "invariant carry (FDMA — no common wavelength for an integer "
                                      "DD; IE-FDMA is a v2 item).  They keep their legacy treatment.");
                            carry_glonass_warned = true;
                        }
                        continue;
                    }
                    // The satellite phase-bias key is per-SATELLITE ("<sat>|<sig>|PB") — NOT
                    // the per-system receiver-bias key.  Only ambiguities backing an ungated
                    // shared phase bias (a vertex of the eq34 graph) become DD arcs.
                    // WL-2 §3.1: keep the constellation's EWL third band OUT of the carry
                    // (uncarried lane ⇒ no stiff prior ⇒ EWL rows win).  OFF ⇒ not taken.
                    if (ewl_carry_exclude && isUncarriedThirdBand(cons, parts->signal)) continue;
                    const std::string pbkey =
                        StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal);
                    if (!shared_phase_keys.count(pbkey)) continue;   // not an eq34 arc (gated/legacy lump)
                    const auto hit = h2l.find(static_cast<int>(sidx));
                    if (hit == h2l.end() || hit->second < 0) continue;   // unresolvable ⇒ chord refloats
                    // C3c slip feed: a PASS-A slip detection (MW wide-lane / LLI / IF-phase,
                    // recorded at the detector so the PASS-B pending-reseed clear can't hide
                    // it) or a within-epoch RAIM culprit (§3) invalidates this arc's carried
                    // DD — re-tap it fresh this epoch instead of folding a stiff pre-slip prior.
                    auto ambSlipped = [&](const std::string& amb_key) -> bool {
                        if (carry_slipped_ambs.count({static_cast<int>(i), amb_key})) return true;
                        if (carry_we_drop_sats
                            && carry_we_drop_sats->count(parts->satellite)) return true;
                        if (carry_we_drop_arcs) {
                            const auto dit = carry_we_drop_arcs->find(static_cast<int>(i));
                            if (dit != carry_we_drop_arcs->end()
                                && dit->second.count(parts->satellite)) return true;
                        }
                        return false;
                    };
                    int& sv = carry_sat_vertex[parts->satellite];
                    if (sv == 0) sv = kSatBase + static_cast<int>(carry_sat_vertex.size());

                    // --- WL-3 pair routing (SPEC_lane_carry_card.md §2): a primary-pair
                    // ambiguity whose partner band ALSO passes every per-band test (eq34-
                    // shared pbkey + resolvable column) joins the constellation's PAIR
                    // group (emitted once, from the band-1 iteration, with both columns);
                    // otherwise it falls through to the residual per-band group as today.
                    if (lane_on) {
                        const auto pit = primary.find({ static_cast<int>(i), parts->satellite });
                        const PP* pp = (pit != primary.end() && pit->second.ok)
                                           ? &pit->second : nullptr;
                        // Only the constellation's census-preferred pair joins the pair
                        // group — a minority-channel (rx,sat) keeps today's per-band carry
                        // (its cycles survive per band instead of spawning a flickering
                        // micro pair-group), and the EWL third band never pairs.
                        // v2 (uwl_family_pairs): additionally, a LATCHED minority-family
                        // pair routes into its OWN pair group (the group key is already
                        // the pair string, so families stay disjoint graphs).
                        const auto lpp = lane_pref_pair.find(cons);
                        bool pair_admissible =
                            pp && lpp != lane_pref_pair.end()
                            && pp->sig1 == lpp->second.first && pp->sig2 == lpp->second.second;
                        if (!pair_admissible && uwl_family_on && pp) {
                            const auto fit = lane_admit_pairs.find(cons);
                            pair_admissible = fit != lane_admit_pairs.end()
                                && fit->second.count({ pp->sig1, pp->sig2 }) != 0;
                        }
                        if (pair_admissible
                            && (parts->signal == pp->sig1 || parts->signal == pp->sig2)) {
                            const bool is1 = parts->signal == pp->sig1;
                            const std::string& other_sig = is1 ? pp->sig2 : pp->sig1;
                            const int other_col          = is1 ? pp->col2 : pp->col1;
                            const bool other_ok = other_col >= 0
                                && shared_phase_keys.count(
                                       StateKeys::makeSatPhaseBiasKey(parts->satellite, other_sig));
                            if (other_ok) {
                                if (!is1) continue;   // the pair arc was/will be emitted from band-1
                                int& pgk = carry_group_id[{cons, pp->sig1 + "+" + pp->sig2}];
                                if (pgk == 0) pgk = static_cast<int>(carry_group_id.size());
                                auto& G = groups[pgk];
                                G.key = pgk;
                                if (!G.pair) {
                                    G.pair    = true;
                                    G.lambda1 = lam(parts->satellite, pp->sig1);
                                    G.lambda2 = lam(parts->satellite, pp->sig2);
                                }
                                const std::string key2 =
                                    StateKeys::makeAmbiguityKey(parts->satellite, pp->sig2);
                                NetworkInvariantCarryDriver::ArcInput a;
                                a.rx_vertex   = static_cast<int>(i);
                                a.sat_vertex  = sv;
                                a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
                                a.est_station = est_index_of[i];
                                a.local_col   = static_cast<int>(hit->second);
                                a.local_col2  = pp->col2;
                                // Per-band slip states; the driver unions them under the LANE
                                // shape (§3.4 — either band refloats both) and keeps them
                                // per-band under the §6.1 equivalence shape (exactly the two
                                // per-band groups' behavior).
                                a.slipped     = ambSlipped(key);
                                a.slipped2    = ambSlipped(key2);
                                // Diagnostic: band-1's eq34 tree tag governs the pair basis;
                                // count disagreements with band-2's election (a mismatch means
                                // the two per-band trees differ on the shared graph, so the
                                // §6.1 equivalence leg is not exact for this component).
                                if (a.tree_edge
                                    != (stations[i].pinned_amb_targets.count(key2) != 0))
                                    ++lane_pair_tree_mismatch;
                                ++lane_pair_arcs;
                                G.arcs.push_back(a);
                                if (carry_innov_gate) {
                                    carry_arc_amb_key[pgk][ArcKey(static_cast<int>(i), sv)]  = key;
                                    carry_arc_amb_key2[pgk][ArcKey(static_cast<int>(i), sv)] = key2;
                                }
                                continue;
                            }
                            ++lane_resid_arcs;   // partner missing/unshared → residual per-band group
                        }
                    }

                    int& gk = carry_group_id[{cons, parts->signal}];
                    if (gk == 0) gk = static_cast<int>(carry_group_id.size());
                    NetworkInvariantCarryDriver::ArcInput a;
                    a.rx_vertex   = static_cast<int>(i);
                    a.sat_vertex  = sv;
                    a.tree_edge   = stations[i].pinned_amb_targets.count(key) != 0;
                    a.est_station = est_index_of[i];
                    a.local_col   = static_cast<int>(hit->second);
                    a.slipped     = ambSlipped(key);
                    groups[gk].key = gk;
                    groups[gk].arcs.push_back(a);
                    // D5: record chord arcs for the innovation-flag reverse lookup (tree edges
                    // never carry a DD, so they cannot be flagged — but record all; harmless).
                    if (carry_innov_gate)
                        carry_arc_amb_key[gk][ArcKey(static_cast<int>(i), sv)] = key;
                }
            }
            std::vector<NetworkInvariantCarryDriver::GroupInput> glist;
            glist.reserve(groups.size());
            for (auto& [k, g] : groups) glist.push_back(std::move(g));
            auto blocks = carry_driver.buildDdPriors(glist);
            // UWL-1 group census (once per run, log-only): which carry groups exist and
            // which are PAIR groups — the WL-3 fragmentation pre-flight (SPEC §8.1).
            if (uwl_on && lane_census_logged.insert('#').second)
                for (const auto& [gk, gid] : carry_group_id) {
                    std::size_t n_arcs = 0; bool is_pair = false;
                    for (const auto& gi : glist)
                        if (gi.key == gid) { n_arcs = gi.arcs.size(); is_pair = gi.pair; break; }
                    log->info("Network (M5/UWL-1 group census): group {} = cons '{}' '{}'"
                              "{} ({} arc(s) this epoch).",
                              gid, gk.first, gk.second, is_pair ? " [PAIR]" : "", n_arcs);
                }

            // --- M5 card WL-1: append the MW-seeded wide-lane pseudo-obs blocks. ---
            // Ride the carry's chord basis: each carry block row is a fundamental cycle on
            // ONE band's ambiguity columns; the WL layer pairs each cycle arc to its L2
            // ambiguity column and folds Σ c·MW̄/λ_WL as a rank-K constraint on the
            // cross-band columns.  Appended AFTER the carry blocks so their indices [0,Nc)
            // stay valid for installPosterior (which reads only its own Nc blocks).
            if (mw_mach_on) {
                // Both lanes read the PURE carry blocks (not each other's appended rows), so
                // collect their outputs here and append after both build.  The WL bridge lane
                // adds a third block set (integer-hard hold rows on the (1,2) columns).
                std::vector<NetworkNormalEquations::DdPriorBlock> wl_out, ewl_out, wl_hold_out;
                std::vector<NetworkMwWlLayer::FixedEwlRow> ewl_fixed;   // WL-2 §3.4 bridge input
                // Increment 5: per-entry top-rung tag, parallel to ewl_fixed ('\0' on the
                // legacy single lane; the lane's b_top / b_top2 digit under the UWL card).
                std::vector<char> ewl_fixed_top;
              if (mw_wl_on) {   // --- WL-1 float lane (primary L1/L2 pair) ---
                mw_wl_epoch = NetworkMwWlLayer::Counters{};   // per-build; the last build before the accepted solve wins
                auto resolver = [&](std::size_t est_station, Eigen::Index local_col,
                                    NetworkMwWlLayer::ArcData& out) -> bool {
                    const auto rit = revmap.find(
                        { static_cast<int>(est_station), static_cast<int>(local_col) });
                    if (rit == revmap.end()) return false;
                    const Rev& rv = rit->second;
                    const auto pit = primary.find({ rv.rx, rv.sat });
                    const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
                    if (pp && pp->ok && rv.signal == pp->sig1
                        && static_cast<int>(local_col) == pp->col1) {
                        out.is_primary_sig1 = true;
                        out.col1    = pp->col1;
                        out.col2    = pp->col2;
                        out.lambda1 = lam(rv.sat, pp->sig1);
                        out.lambda2 = lam(rv.sat, pp->sig2);
                        MwSlipDetector::MwArcZdMean zd;
                        if (stations[rv.rx].mw_slip)
                            zd = stations[rv.rx].mw_slip->arcZdMean(rv.sat, pp->sig1, pp->sig2);
                        out.have_mw       = zd.valid;
                        out.mw_zd_mean_m  = zd.mean_m;
                        out.mw_var_m2     = zd.var_of_mean_m2;
                        out.mw_count      = static_cast<long long>(zd.count);
                        out.lambda_wl     = zd.lambda_wl;
                        out.bia_corrected = zd.bia_corrected;   // frame flag (mixed-frame cycle guard)
                        {   // correction-frame re-expression inputs (F5)
                            const auto l1it = lin_of.find({ static_cast<int>(est_station),
                                                            static_cast<int>(pp->col1) });
                            const auto l2it = lin_of.find({ static_cast<int>(est_station),
                                                            static_cast<int>(pp->col2) });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) {
                                out.have_mw = false;   // no linearization value ⇒ cannot frame the row
                            } else {
                                out.x_lin1 = l1it->second;
                                out.x_lin2 = l2it->second;
                            }
                        }
                    } else {
                        // Not the primary L1 signal: a secondary/other-band carry group
                        // (→ skip_not_primary) or a single-band band-1 arc lacking L2
                        // (is_primary_sig1 true + col2=-1 → skip_missing_band).
                        out.is_primary_sig1 =
                            (!pp || !pp->ok) && (rv.signal.size() >= 2 && rv.signal[1] == '1');
                        out.col1    = local_col;
                        out.col2    = -1;
                        out.have_mw = false;
                    }
                    return true;
                };
                auto wl_blocks = mw_wl_layer.build(blocks, resolver, mw_wl_epoch);
                // --- F6 attribution capture: the FIRST arming epoch's WL rows (log-only). ---
                // Re-resolve each cycle arc (resolver + revmap in scope here) to record the
                // per-arc MW̄/x_lin and the row aggregates z_MW/z_lin/z_corr; the post-solve
                // site adds reconstructed_abs + the kernel innovation.  The last build before
                // the accepted solve wins (tow-matched at the write site).
                if (mw_wl_f6_dump && !mw_wl_f6.written && !wl_blocks.empty()) {
                    const std::size_t Nc = blocks.size();      // WL blocks append AFTER the carry blocks
                    const auto& wb = wl_blocks.front();        // first carry component's WL block
                    mw_wl_f6.rows.clear();
                    mw_wl_f6.week           = se.time.week;
                    mw_wl_f6.tow            = se.time.tow;
                    mw_wl_f6.loop_epoch     = static_cast<int>(n_loop_epochs);
                    mw_wl_f6.wl_block_index = Nc;              // first WL block's index in `blocks`
                    const std::size_t nrow = std::min<std::size_t>(wb.rows.size(), std::size_t{8});
                    for (std::size_t r = 0; r < nrow; ++r) {
                        const auto& terms = wb.rows[r];
                        F6Row fr;
                        fr.zhat_block = (static_cast<Eigen::Index>(r) < wb.zhat.size())
                                            ? wb.zhat(static_cast<Eigen::Index>(r)) : 0.0;
                        for (std::size_t t = 0; t + 1 < terms.size(); t += 2) {
                            const auto& tL1 = terms[t];       // rows are [L1_0,L2_0, L1_1,L2_1, ...]
                            NetworkMwWlLayer::ArcData ad;
                            if (!resolver(tL1.station, tL1.local_col, ad)) continue;
                            F6Arc a;
                            a.est_station   = static_cast<int>(tL1.station);
                            a.col1          = static_cast<int>(ad.col1);
                            a.col2          = static_cast<int>(ad.col2);
                            a.coeff         = (ad.lambda1 > 0.0) ? tL1.coeff * ad.lambda1 : tL1.coeff;
                            a.mw_mean_m     = ad.mw_zd_mean_m;
                            a.mw_var_m2     = ad.mw_var_m2;
                            a.mw_count      = ad.mw_count;
                            a.bia_corrected = ad.bia_corrected;
                            a.x_lin1        = ad.x_lin1;
                            a.x_lin2        = ad.x_lin2;
                            a.lambda1       = ad.lambda1;
                            a.lambda2       = ad.lambda2;
                            a.lambda_wl     = ad.lambda_wl;
                            const auto r1 = revmap.find({ a.est_station, a.col1 });
                            if (r1 != revmap.end()) { a.sat = r1->second.sat; a.sig1 = r1->second.signal; }
                            const auto r2 = revmap.find({ a.est_station, a.col2 });
                            if (r2 != revmap.end()) a.sig2 = r2->second.signal;
                            if (a.lambda_wl > 0.0) fr.z_mw += a.coeff * a.mw_mean_m / a.lambda_wl;
                            if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                                fr.z_lin += (a.coeff / a.lambda1) * a.x_lin1
                                          - (a.coeff / a.lambda2) * a.x_lin2;
                            fr.arcs.push_back(std::move(a));
                        }
                        fr.z_corr = fr.z_mw - fr.z_lin;
                        mw_wl_f6.rows.push_back(std::move(fr));
                    }
                    mw_wl_f6.pending = !mw_wl_f6.rows.empty();
                }
                wl_out = std::move(wl_blocks);
              }  // --- end WL-1 float lane ---

              if (ewl_on) {   // --- WL-2 EWL lane: (band2, band-top) pair, FFR statistic-gated fix ---
                for (auto& lane : ewl_lanes) lane.epoch = NetworkMwWlLayer::Counters{};
                // Same eligibility as WL-1 (a primary-sig1 cycle), but the row is the top-tier
                // DD on the SAME (rx,sat) arcs, pairing the estimator's primary sig2 (channel
                // discipline) with the constellation's top band: GPS EWL (L2,L5); Galileo UWL
                // (E5b|E5,E5a) under the armed card.  (hi,lo) is ordered by ACTUAL frequency —
                // GPS's shared band is the pair's HIGH side, Galileo's its LOW side — so the
                // MW statistic keys (sat, hi, lo) and λ_top stays positive (UWL-1 §2).  The
                // top band is uncarried (excluded above) so the rows win with no fight.
                // Increment 5: the lane instance under construction selects which top-rung
                // map its resolver reads — band_top_of for the preferred (and every
                // single-top) instance, band_top2_of for the fallback instance.  Set per
                // lane in the build loop below; single-top shapes never repoint it.
                const auto* ewl_top_map = &band_top_of;
                auto ewl_resolver = [&](std::size_t est_station, Eigen::Index local_col,
                                        NetworkMwWlLayer::ArcData& out) -> bool {
                    const auto rit = revmap.find(
                        { static_cast<int>(est_station), static_cast<int>(local_col) });
                    if (rit == revmap.end()) return false;
                    const Rev& rv = rit->second;
                    out.cons = rv.sat.empty() ? '?' : rv.sat.front();
                    const auto pit = primary.find({ rv.rx, rv.sat });
                    const PP* pp = (pit != primary.end()) ? &pit->second : nullptr;
                    if (pp && pp->ok && rv.signal == pp->sig1
                        && static_cast<int>(local_col) == pp->col1) {
                        out.is_primary_sig1 = true;
                        const auto b5 = ewl_top_map->find({ rv.rx, rv.sat });
                        if (b5 == ewl_top_map->end()) { out.col2 = -1; out.have_mw = false; return true; }
                        const std::string& sig_m = pp->sig2;           // shared band = primary sig2
                        const int          col_m = pp->col2;
                        const std::string& sig_t = b5->second.first;   // top-tier partner band
                        const int          col_t = b5->second.second;
                        const double lam_m = lam(rv.sat, sig_m);
                        const double lam_t = lam(rv.sat, sig_t);
                        // (hi, lo) by actual frequency (smaller λ = higher f).  GPS: L2 > L5
                        // reproduces the legacy assignment bit-exactly.
                        const bool m_hi = lam_m > 0.0 && lam_t > 0.0 && lam_m < lam_t;
                        const std::string& sig_hi = m_hi ? sig_m : sig_t;
                        const std::string& sig_lo = m_hi ? sig_t : sig_m;
                        const int col_hi = m_hi ? col_m : col_t;
                        const int col_lo = m_hi ? col_t : col_m;
                        out.col1    = col_hi;                          // high-frequency side
                        out.col2    = col_lo;                          // low-frequency side
                        out.lambda1 = m_hi ? lam_m : lam_t;
                        out.lambda2 = m_hi ? lam_t : lam_m;
                        MwSlipDetector::MwArcZdMean zd;
                        if (stations[rv.rx].mw_slip)                    // (higher, lower)-freq order
                            zd = stations[rv.rx].mw_slip->arcZdMean(rv.sat, sig_hi, sig_lo);
                        out.have_mw       = zd.valid;
                        out.mw_cold       = false;
                        out.mw_zd_mean_m  = zd.mean_m;
                        out.mw_var_m2     = zd.var_of_mean_m2;
                        out.mw_count      = static_cast<long long>(zd.count);
                        out.lambda_wl     = zd.lambda_wl;              // = λ_top = c/(f_hi − f_lo)
                        out.bia_corrected = zd.bia_corrected;
                        // UWL-1 §3.3 cold fix: when the LAGGED statistic is immature, serve the
                        // UNLAGGED n≥1 cold read — DECISION-ONLY, and only on tiers whose λ
                        // earns it (the floor excludes GPS's 5.861 m EWL).  At n=1 the Welford
                        // variance does not exist; substitute the a-priori per-sample σ.
                        if (uwl_cold_on && stations[rv.rx].mw_slip
                            && (!zd.valid
                                || zd.count < static_cast<std::size_t>(ewl_cfg.min_arc_epochs))) {
                            const auto zc = stations[rv.rx].mw_slip->arcZdMeanCold(
                                                rv.sat, sig_hi, sig_lo);
                            if (zc.valid && zc.lambda_wl >= nmw.uwl_cold_min_lambda_m) {
                                out.have_mw       = true;
                                out.mw_cold       = true;
                                out.mw_zd_mean_m  = zc.mean_m;
                                out.mw_var_m2     = std::isfinite(zc.var_of_mean_m2)
                                                        ? zc.var_of_mean_m2
                                                        : nmw.uwl_cold_sigma_m * nmw.uwl_cold_sigma_m;
                                out.mw_count      = static_cast<long long>(zc.count);
                                out.lambda_wl     = zc.lambda_wl;
                                out.bia_corrected = zc.bia_corrected;
                            }
                        }
                        const auto lmit = lin_of.find({ static_cast<int>(est_station), col_hi });
                        const auto ltit = lin_of.find({ static_cast<int>(est_station), col_lo });
                        if (lmit == lin_of.end() || ltit == lin_of.end()) out.have_mw = false;
                        else { out.x_lin1 = lmit->second; out.x_lin2 = ltit->second; }
                    } else {
                        out.is_primary_sig1 =
                            (!pp || !pp->ok) && (rv.signal.size() >= 2 && rv.signal[1] == '1');
                        out.col1    = local_col;
                        out.col2    = -1;
                        out.have_mw = false;
                    }
                    return true;
                };
                ewl_out.clear();
                ewl_fixed.clear();
                ewl_fixed_top.clear();
                for (auto& lane : ewl_lanes) {
                    // Increment 5: the fallback instance ('7' under a dual tier) resolves
                    // against band_top2_of; everything else keeps band_top_of.  The tag
                    // rides along per fixed row so the WL walk reads the SAME rung's
                    // bridge/cold arcs the decision came from.
                    const bool is_top2 = uwl_dual_on && lane.top != '\0'
                        && [&] {
                               const auto t = mwLaneTierOf(lane.cons, nmw.uwl_gal_top_band,
                                                           nmw.uwl_mid_enable, nmw.uwl_dual_top);
                               return t && t->b_top2 != '\0' && lane.top == t->b_top2;
                           }();
                    ewl_top_map = is_top2 ? &band_top2_of : &band_top_of;
                    std::vector<NetworkMwWlLayer::FixedEwlRow> lane_fixed;
                    auto lane_blocks = lane.layer.build(blocks, ewl_resolver, lane.epoch, &lane_fixed);
                    for (auto& b : lane_blocks) ewl_out.push_back(std::move(b));
                    ewl_fixed.insert(ewl_fixed.end(), lane_fixed.begin(), lane_fixed.end());
                    ewl_fixed_top.insert(ewl_fixed_top.end(), lane_fixed.size(), lane.top);
                }
                ewl_top_map = &band_top_of;   // leave the pointer in its legacy state
                if (mw_d5_dump) {   // F-A dump: the (station,sat) pairs EWL-FIXED cycles touch
                    d5_ewl_stasat.clear();
                    for (const auto& fx : ewl_fixed)
                        for (const auto& t : blocks[fx.block].rows[fx.row]) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit != revmap.end())
                                d5_ewl_stasat.insert({ rit->second.rx, rit->second.sat });
                        }
                }
              }  // --- end WL-2 EWL lane ---

              // Per-build reset (the last build before the accepted solve wins; RAIM re-solve safe).
              if (wl_inject_on || lane_on) {
                  wl_inject_epoch.clear();
                  wl_epoch = WlCounters{};
                  wl_cons_epoch.clear();   // UWL-1 per-cons walk tallies
              }
              if (mw_tagsets) d5_hold_chords.clear();

              if (lane_meas_on) {
                // --- WL-3 §3.2: per-epoch MW(1,2) lane increments on EVERY eligible pair
                // chord — the carried block becomes the code-coherence accumulator.  Each
                // arc contributes its detector-LAGGED raw sample exactly once (the Welford
                // mean stays decision-side); a cycle with an immature/stale/mixed-frame arc,
                // a this-epoch slip (its lagged sample is pre-slip), or a quarantined
                // linearization is skipped for the epoch.  Serial in (block, chord) order —
                // the §8.2 deterministic seam (runs before the bridge/fix walk below).
                constexpr double kLaneMwSigmaFloorCyc = 0.2;   // young-arc Welford collapse guard
                for (std::size_t b = 0; b < carry_driver.numBlocks(); ++b) {
                    if (carry_driver.blockPairBand(b) != 3) continue;
                    const auto& chords_b = carry_driver.blockChordsForTest(b);
                    for (std::size_t k = 0; k < chords_b.size(); ++k) {
                        if (k >= blocks[b].rows.size()) break;
                        const auto& terms = blocks[b].rows[k];   // band-1 cycle terms
                        if (terms.empty()) continue;
                        double z_mw_m = 0.0, R_m2 = 0.0, z_lin_wl = 0.0, lam_wl = 0.0;
                        double stamp = 0.0;
                        bool   have_stamp = false, ok = true;
                        int    frame = -1;   // uniform bia_corrected frame guard
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { ok = false; break; }
                            const Rev& rv = rit->second;
                            const auto pit = primary.find({ rv.rx, rv.sat });
                            if (pit == primary.end() || !pit->second.ok
                                || rv.signal != pit->second.sig1
                                || static_cast<int>(t.local_col) != pit->second.col1) {
                                ok = false; break;
                            }
                            const PP& pp = pit->second;
                            // A slip re-tapped this chord this epoch — the lagged (pre-slip)
                            // sample would poison the fresh lane; skip for one epoch.
                            bool slipped = carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                                        || carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                            if (!slipped && carry_we_drop_sats
                                && carry_we_drop_sats->count(rv.sat)) slipped = true;
                            if (!slipped && carry_we_drop_arcs) {
                                const auto dit = carry_we_drop_arcs->find(rv.rx);
                                if (dit != carry_we_drop_arcs->end()
                                    && dit->second.count(rv.sat)) slipped = true;
                            }
                            if (slipped) { ok = false; break; }
                            MwSlipDetector::MwArcZdLast zl;
                            if (stations[rv.rx].mw_slip)
                                zl = stations[rv.rx].mw_slip->arcZdLast(rv.sat, pp.sig1, pp.sig2);
                            if (!zl.valid || !(zl.lambda_wl > 0.0)) { ok = false; break; }
                            if (frame < 0) frame = zl.bia_corrected ? 1 : 0;
                            else if (frame != (zl.bia_corrected ? 1 : 0)) { ok = false; break; }
                            if (!have_stamp) { stamp = zl.epoch_s; have_stamp = true; }
                            else if (std::abs(zl.epoch_s - stamp) > 0.1) { ok = false; break; }
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                            if (nmw.lane_max_abs_xlin_m > 0.0 &&
                                (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                                 std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { ok = false; break; }
                            const double la1 = lam(rv.sat, pp.sig1), la2 = lam(rv.sat, pp.sig2);
                            if (!(la1 > 0.0 && la2 > 0.0)) { ok = false; break; }
                            z_mw_m   += t.coeff * zl.value_m;
                            R_m2     += t.coeff * t.coeff * zl.var_m2;
                            z_lin_wl += (t.coeff / la1) * l1it->second
                                      - (t.coeff / la2) * l2it->second;
                            lam_wl = zl.lambda_wl;
                        }
                        if (!ok || !(lam_wl > 0.0)) { ++wl_epoch.lane_skip_meas; continue; }
                        const double y = z_mw_m / lam_wl - z_lin_wl;
                        const double R = std::max(R_m2 / (lam_wl * lam_wl),
                                                  kLaneMwSigmaFloorCyc * kLaneMwSigmaFloorCyc);
                        if (carry_driver.applyLaneMeasurement(blocks, b, k, y, R))
                            ++wl_epoch.lane_mw_rows;
                        else
                            ++wl_epoch.lane_skip_meas;
                    }
                }
              }

              if ((wl_inject_on || lane_fix_on || lane_bridge_on) && !ewl_fixed.empty()) {
                // --- WL-2 WL lane: phase bridge → WL integer → hold row + inject plan (§3.3/§3.4) ---
                // For each FFR-fixed EWL cycle, bridge phase-only from the fixed N_EWL to a fast
                // WL integer w = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL, cross-check the plain MW(1,2), and
                // either FOLD an integer-hard hold row (carry already lane-consistent ⇒ innovation
                // ~0, no fight) or STASH an inject (carry moves to w post-solve, NL preserved).
                // sat_vertex → satellite label (reverse of carry_sat_vertex) to map a chord's
                // ArcKey.v back to its satellite (for the primary-pair + group-id lookups).
                std::map<int, std::string> sat_of_vertex;
                for (const auto& [s, v] : carry_sat_vertex) sat_of_vertex[v] = s;
                // Group hold rows by their source carry block so shared (1,2) columns fold jointly.
                std::map<std::size_t, std::size_t> hold_block_of;   // carry block b → wl_hold_out index
                // Increment 5 (uwl_dual_top): a (block,row) decided by the PREFERRED rung
                // is consumed — the fallback rung's entry for the same row dedupes (the
                // preferred instance's entries precede it in ewl_fixed by construction).
                // A preferred entry that FAILS a gate does NOT consume, so the fallback
                // proceeds the same epoch (including a sibling-gate block — the demotion
                // is free).  The lane bridge increment is separately once-per-row: both
                // rungs' bridges measure the same lane and must not double-fold.
                std::set<std::pair<std::size_t, std::size_t>> walk_decided, lane_incr_done;

                for (std::size_t fxi = 0; fxi < ewl_fixed.size(); ++fxi) {
                    const auto& fx = ewl_fixed[fxi];
                    const char  fx_top = fxi < ewl_fixed_top.size() ? ewl_fixed_top[fxi] : '\0';
                    if (fx.block >= blocks.size() || fx.block >= carry_driver.numBlocks()) {
                        ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
                    }
                    const auto& chords_b = carry_driver.blockChordsForTest(fx.block);
                    if (fx.row >= chords_b.size() || fx.row >= blocks[fx.block].rows.size()) {
                        ++wl_epoch.rows_seen; continue;   // defensive; stays pooled
                    }
                    const ArcKey chord = chords_b[fx.row];
                    // UWL-1: resolve the chord's constellation up front — the walk tallies
                    // per-cons under the armed card (wc == wl_epoch otherwise, bit-identical).
                    const auto svit0 = sat_of_vertex.find(chord.v);
                    const std::string sat_lbl =
                        svit0 != sat_of_vertex.end() ? svit0->second : std::string{};
                    const char ccons = sat_lbl.empty() ? '?' : sat_lbl.front();
                    WlCounters& wc = uwl_on ? wl_cons_epoch[ccons] : wl_epoch;
                    ++wc.rows_seen;
                    // Increment 5: resolve the chord constellation's tier once — which rung
                    // this entry came from, which top map its bridge reads, and whether an
                    // untaken sibling exists for the closure statistic.
                    const auto ctier = uwl_on
                        ? mwLaneTierOf(ccons, nmw.uwl_gal_top_band, nmw.uwl_mid_enable,
                                       nmw.uwl_dual_top)
                        : std::optional<MwLaneTier>{};
                    const bool fx_is_top2 = ctier && ctier->b_top2 != '\0'
                                            && fx_top == ctier->b_top2;
                    const bool dual_cons  = uwl_dual_on && ctier && ctier->b_top2 != '\0';
                    const auto& walk_top_map = fx_is_top2 ? band_top2_of : band_top_of;
                    const auto& sib_top_map  = fx_is_top2 ? band_top_of : band_top2_of;
                    if (dual_cons && walk_decided.count({ fx.block, fx.row })) {
                        ++wc.dual_dedupe;   // the preferred rung already decided this row
                        continue;
                    }
                    const auto&  terms = blocks[fx.block].rows[fx.row];   // the fundamental cycle
                    if (terms.empty()) continue;

                    // Walk the cycle arcs: accumulate the bridge DD, the MW(1,2) closure DD, and
                    // the per-band linearization DD (F5).  Bail on the first ineligible arc.
                    double sum_gf = 0.0, R_bridge = 0.0;          // Σc·GF_b (m), Σc²·var/λ_wl² (cyc²)
                    double z_mw12 = 0.0, R_mw12 = 0.0;            // plain MW(1,2) DD (cyc) + var
                    double z_lin1 = 0.0, z_lin2 = 0.0;            // Σc·x_lin per band (m)
                    double iono_dd = 0.0, iono_co = 0.0;         // §3.4 v2 de-bias: Σc·Î₁ (m), C_iono
                    double lam_wl = 0.0, lam_ewl = 0.0, lam1 = 0.0, lam2 = 0.0;
                    bool   ok = true, mw12_ok = true, quarantined = false, iono_ok = true;
                    // E6 mid hop (uwl_mid_enable): hop-hi (mid lane vs top lane) + hop-lo
                    // (WL lane vs mid lane) accumulators, plus the MW(6,5) closure for the
                    // intermediate integer.  mid_ok stays true only if EVERY arc serves both
                    // hop bridges mature (else the cycle falls back to the direct hop).
                    bool   mid_ok = uwl_mid_on;
                    double sum_gf_hi = 0.0, R_hi = 0.0;           // Σc·GF_hop-hi (m), Σc²·var/λ_mid² (cyc²)
                    double sum_gf_lo = 0.0, R_lo = 0.0;           // Σc·GF_hop-lo (m), Σc²·var/λ_wl² (cyc²)
                    double iono_co_hi = 0.0, iono_co_lo = 0.0, lam_mid = 0.0;
                    double z_mw65 = 0.0, R_mw65 = 0.0;            // MW(mid,b2) closure DD (mid cyc) + var
                    bool   mw65_ok = true;
                    // Increment 5 sibling closure: the untaken {7,8} rung's MW statistic as
                    // a DD over the same cycle.  Measured on every dual-tier decision; the
                    // hard gate is separate (uwl_sib_gate).  Any arc without the sibling
                    // band / a valid MW arc drops the check (counted sib_unavail).
                    bool   sib_ok = dual_cons;
                    double z_sib = 0.0, R_sib = 0.0, lam_sib = 0.0;
                    // WL-3 §3.2 bridge lane increment: the LAGGED RAW GF_b DD (once-only,
                    // phase-precision) accumulated alongside the Welford DECISION statistic.
                    double sum_gf_raw = 0.0, R_gf_raw = 0.0;      // Σc·GF_b_raw (m), Σc²·var/λ_wl² (cyc²)
                    double raw_stamp = 0.0;
                    bool   raw_ok = lane_bridge_on, raw_have_stamp = false;
                    bool   bridge_cold = false;   // UWL-1 §3.3: any arc served by the cold bridge
                    for (const auto& t : terms) {
                        const auto rit = revmap.find({ static_cast<int>(t.station),
                                                       static_cast<int>(t.local_col) });
                        if (rit == revmap.end()) { ok = false; break; }
                        const Rev& rv = rit->second;
                        const auto pit = primary.find({ rv.rx, rv.sat });
                        if (pit == primary.end() || !pit->second.ok) { ok = false; break; }
                        const PP& pp = pit->second;
                        const auto b5 = walk_top_map.find({ rv.rx, rv.sat });
                        if (b5 == walk_top_map.end()) { ok = false; break; }
                        auto brg = stations[rv.rx].mw_slip
                            ? stations[rv.rx].mw_slip->arcBridge(rv.sat, pp.sig1, pp.sig2, b5->second.first)
                            : MwSlipDetector::MwBridgeMean{};
                        const std::size_t brg_need =
                            static_cast<std::size_t>(std::max(2, ewl_cfg.min_arc_epochs));
                        if (uwl_cold_on && stations[rv.rx].mw_slip
                            && (!brg.valid || brg.count < brg_need)) {
                            // UWL-1 §3.3: post-slip the LAGGED bridge trails by one epoch; the
                            // UNLAGGED cold serve (n≥2) re-pins one epoch earlier.  DECISION-
                            // ONLY — the lane increment below stays on the lagged arcBridgeLast,
                            // and the λ floor keeps this a top-tier (Galileo) privilege.
                            const auto bc = stations[rv.rx].mw_slip->arcBridgeCold(
                                                rv.sat, pp.sig1, pp.sig2, b5->second.first);
                            if (bc.valid && bc.count >= brg_need
                                && bc.lambda_ewl >= nmw.uwl_cold_min_lambda_m) {
                                brg = bc;
                                bridge_cold = true;
                            }
                        }
                        if (!brg.valid || brg.count < brg_need) {
                            ok = false; break;
                        }
                        const double l1 = lam(rv.sat, pp.sig1), l2 = lam(rv.sat, pp.sig2);
                        if (!(l1 > 0.0 && l2 > 0.0 && brg.lambda_wl > 0.0 && brg.lambda_ewl > 0.0)) {
                            ok = false; break;
                        }
                        const double coeff = t.coeff;
                        sum_gf   += coeff * brg.gf_mean_m;
                        R_bridge += coeff * coeff * brg.var_of_mean_m2 / (brg.lambda_wl * brg.lambda_wl);
                        // E6 mid hop: read this arc's two hop bridges (lagged; the cold
                        // serve stays a direct-hop privilege) + the MW(mid,b2) closure.
                        // Any missing/immature piece drops the WHOLE cycle to the direct
                        // hop (λ_mid must be uniform across the cycle's arcs).
                        if (mid_ok) {
                            const auto b6 = band_mid_of.find({ rv.rx, rv.sat });
                            MwSlipDetector::MwBridgeMean bh, blo;
                            if (b6 != band_mid_of.end() && stations[rv.rx].mw_slip) {
                                bh  = stations[rv.rx].mw_slip->arcBridge(
                                          rv.sat, b6->second, pp.sig2, b5->second.first);
                                blo = stations[rv.rx].mw_slip->arcBridge(
                                          rv.sat, pp.sig1, pp.sig2, b6->second);
                            }
                            if (!bh.valid || bh.count < brg_need
                                || !blo.valid || blo.count < brg_need
                                || !(bh.lambda_wl > 0.0) || !(blo.lambda_ewl > 0.0)) {
                                mid_ok = false;
                            } else {
                                sum_gf_hi += coeff * bh.gf_mean_m;
                                R_hi      += coeff * coeff * bh.var_of_mean_m2
                                             / (bh.lambda_wl * bh.lambda_wl);
                                sum_gf_lo += coeff * blo.gf_mean_m;
                                R_lo      += coeff * coeff * blo.var_of_mean_m2
                                             / (blo.lambda_wl * blo.lambda_wl);
                                iono_co_hi = bh.iono_coeff;
                                iono_co_lo = blo.iono_coeff;
                                lam_mid    = bh.lambda_wl;   // == blo.lambda_ewl (CDMA-uniform)
                                if (mw65_ok) {   // MW(mid,b2) closure for the mid integer
                                    const auto mw65 = stations[rv.rx].mw_slip->arcZdMean(
                                                          rv.sat, b6->second, pp.sig2);
                                    if (mw65.valid && mw65.lambda_wl > 0.0) {
                                        z_mw65 += coeff * mw65.mean_m / mw65.lambda_wl;
                                        R_mw65 += coeff * coeff * mw65.var_of_mean_m2
                                                  / (mw65.lambda_wl * mw65.lambda_wl);
                                    } else {
                                        mw65_ok = false;
                                    }
                                }
                            }
                        }
                        // Increment 5: this arc's untaken-sibling MW read (7-fx → the '8'
                        // rung's (8,5) statistic, 8-fx → the finer (7,5) one).  Ordered
                        // (hi,lo) by actual frequency like every MW key.
                        if (sib_ok) {
                            const auto bs = sib_top_map.find({ rv.rx, rv.sat });
                            MwSlipDetector::MwArcZdMean mws;
                            if (bs != sib_top_map.end() && stations[rv.rx].mw_slip) {
                                const double lam_s = lam(rv.sat, bs->second.first);
                                const double lam_m = lam(rv.sat, pp.sig2);
                                const bool s_hi = lam_s > 0.0 && lam_m > 0.0 && lam_s < lam_m;
                                mws = stations[rv.rx].mw_slip->arcZdMean(
                                          rv.sat,
                                          s_hi ? bs->second.first : pp.sig2,
                                          s_hi ? pp.sig2 : bs->second.first);
                            }
                            if (mws.valid && mws.lambda_wl > 0.0) {
                                z_sib  += coeff * mws.mean_m / mws.lambda_wl;
                                R_sib  += coeff * coeff * mws.var_of_mean_m2
                                          / (mws.lambda_wl * mws.lambda_wl);
                                lam_sib = mws.lambda_wl;
                            } else {
                                sib_ok = false;
                            }
                        }
                        if (raw_ok) {   // WL-3: the raw lagged sample (skip on slip/stale/immature)
                            bool slipped = carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig1) })
                                        || carry_slipped_ambs.count(
                                               { rv.rx, StateKeys::makeAmbiguityKey(rv.sat, pp.sig2) });
                            if (!slipped && carry_we_drop_sats
                                && carry_we_drop_sats->count(rv.sat)) slipped = true;
                            if (!slipped && carry_we_drop_arcs) {
                                const auto dit = carry_we_drop_arcs->find(rv.rx);
                                if (dit != carry_we_drop_arcs->end()
                                    && dit->second.count(rv.sat)) slipped = true;
                            }
                            MwSlipDetector::MwBridgeLast bl;
                            if (!slipped && stations[rv.rx].mw_slip)
                                bl = stations[rv.rx].mw_slip->arcBridgeLast(
                                         rv.sat, pp.sig1, pp.sig2, b5->second.first);
                            if (slipped || !bl.valid || !(bl.lambda_wl > 0.0)) {
                                raw_ok = false;
                            } else if (!raw_have_stamp) {
                                raw_stamp = bl.epoch_s; raw_have_stamp = true;
                            } else if (std::abs(bl.epoch_s - raw_stamp) > 0.1) {
                                raw_ok = false;   // mixed epochs — the DD is not any epoch's
                            }
                            if (raw_ok) {
                                sum_gf_raw += coeff * bl.gf_m;
                                R_gf_raw   += coeff * coeff * bl.var_m2
                                              / (bl.lambda_wl * bl.lambda_wl);
                            }
                        }
                        // §3.4 v2 iono de-bias: subtract the network's own slant-iono estimate DD.
                        // (We do NOT price the local MARGINAL iono variance into the σ-guard: it is
                        // dominated by the ABSOLUTE / common-mode iono uncertainty that CANCELS in a
                        // closed DD cycle, so Σc²·var grossly over-states the true DD-iono error and
                        // would gate the bridge past its ep~4-6 advantage.  The DD-iono ESTIMATE
                        // error — small, since common-mode cancels — surfaces directly in |frac| and
                        // is caught by the FFR gate; the iono-free MW(1,2) closure is the backstop.
                        // The exact joint-DD-iono covariance / iono-column fold is a v3 refinement.)
                        const auto iit = iono_of.find({ rv.rx, rv.sat });
                        if (iit == iono_of.end()) { iono_ok = false; break; }
                        iono_dd += coeff * iit->second;
                        const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                        const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                        if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                        z_lin1 += coeff * l1it->second;
                        z_lin2 += coeff * l2it->second;
                        if (nmw.lane_max_abs_xlin_m > 0.0 &&
                            (std::abs(l1it->second) > nmw.lane_max_abs_xlin_m ||
                             std::abs(l2it->second) > nmw.lane_max_abs_xlin_m)) { quarantined = true; break; }
                        // Plain MW(1,2) closure statistic (integrity cross-check, §3.4).
                        MwSlipDetector::MwArcZdMean mw = stations[rv.rx].mw_slip
                            ? stations[rv.rx].mw_slip->arcZdMean(rv.sat, pp.sig1, pp.sig2)
                            : MwSlipDetector::MwArcZdMean{};
                        if (mw.valid && mw.lambda_wl > 0.0) {
                            z_mw12 += coeff * mw.mean_m / mw.lambda_wl;
                            R_mw12 += coeff * coeff * mw.var_of_mean_m2 / (mw.lambda_wl * mw.lambda_wl);
                        } else {
                            mw12_ok = false;
                        }
                        lam_wl = brg.lambda_wl; lam_ewl = brg.lambda_ewl; lam1 = l1; lam2 = l2;
                        iono_co = brg.iono_coeff;
                    }
                    if (quarantined) { ++wc.skip_quarantine; continue; }
                    if (!ok || !(lam_wl > 0.0)) { ++wc.skip_bridge_missing; continue; }
                    if (!iono_ok)  { ++wc.skip_no_iono; continue; }  // no iono estimate ⇒ can't de-bias

                    // --- WL-3 §3.2: fold the per-epoch BRIDGE lane increment (raw lagged GF_b
                    // DD, iono-de-biased, honest per-sample R) — BEFORE the FFR decision gates,
                    // because the continuous lane tightening is wanted even on epochs where the
                    // integer decision is withheld.  Once-only: the raw sample enters here and
                    // nowhere else; the Welford mean below stays decision-side.
                    // Increment 5: both rungs' bridges measure the SAME lane — at most one
                    // bridge increment folds per (block,row) per build (a failed preferred
                    // attempt does not consume, so the fallback may still tighten).
                    if (lane_bridge_on
                        && !(dual_cons && lane_incr_done.count({ fx.block, fx.row }))) {
                        constexpr double kLaneBridgeSigmaFloorCyc = 0.02;   // phase-precision floor
                        if (raw_ok) {
                            const double z_raw = (sum_gf_raw - iono_co * iono_dd
                                                  + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;
                            const double y_raw = z_raw - (z_lin1 / lam1 - z_lin2 / lam2);
                            const double R_raw = std::max(R_gf_raw,
                                kLaneBridgeSigmaFloorCyc * kLaneBridgeSigmaFloorCyc);
                            if (carry_driver.applyLaneMeasurement(blocks, fx.block, fx.row, y_raw, R_raw)) {
                                ++wc.lane_bridge_rows;
                                if (dual_cons) lane_incr_done.insert({ fx.block, fx.row });
                            } else {
                                ++wc.lane_skip_meas;
                            }
                        } else {
                            ++wc.lane_skip_meas;
                        }
                    }

                    // z_WL^bridge = [Σc·GF_b − C_iono·Σc·Î₁ + λ_EWL·N_EWL]/λ_WL (cyc); FFR on the
                    // iono-de-biased DETECTOR stat (decision source, never the posterior).  The
                    // de-bias removes the integer-valued differential-iono bias (the v1
                    // falsification); the residual DD-iono ESTIMATE error surfaces in |frac|.
                    const double sigma_direct = std::sqrt(std::max(R_bridge, 0.0));   // Welford phase noise
                    const double z_wl_direct  = (sum_gf - iono_co * iono_dd
                                                 + lam_ewl * static_cast<double>(fx.integer)) / lam_wl;

                    // --- E6 mid hop: chain top → mid → WL.  Hop-hi determines the mid-rung
                    // integer m = round([Σc·GF_hi − C_hi·Σc·Î₁ + λ_TOP·N_TOP]/λ_MID) under its
                    // own FFR gates + the MW(mid,b2) closure; hop-lo then re-expresses the WL
                    // statistic against the ROUNDED m — stripping the top hop's λ_TOP-scale
                    // phase/iono amplification (~3.3× smaller λ multiplier at λ_MID 2.931 m).
                    // Any failure falls back to the direct hop (counted, never silent).
                    double z_wl = z_wl_direct, sigma_bridge = sigma_direct;
                    bool   used_mid = false;
                    if (mid_ok && lam_mid > 0.0) {
                        const double sigma_hi = std::sqrt(std::max(R_hi, 0.0));
                        const double z_mid = (sum_gf_hi - iono_co_hi * iono_dd
                                              + lam_ewl * static_cast<double>(fx.integer)) / lam_mid;
                        const double m = std::round(z_mid);
                        bool mid_fix = sigma_hi < nmw.wl_fix_sigma_cyc
                                    && std::abs(z_mid - m) < nmw.wl_fix_frac_cyc;
                        if (mid_fix && mw65_ok) {
                            // The mid integer has its own free integrity check: the plain
                            // MW(mid,b2) DD must agree within its σ (invariant #1).
                            const double tol65 = 0.5 + 3.0 * std::sqrt(std::max(R_mw65, 0.0));
                            if (std::abs(z_mw65 - m) > tol65) { mid_fix = false; ++wc.mid_closure; }
                        } else if (!mid_fix) {
                            ++wc.mid_fallback;
                        }
                        if (mid_fix) {
                            z_wl = (sum_gf_lo - iono_co_lo * iono_dd + lam_mid * m) / lam_wl;
                            sigma_bridge = std::sqrt(std::max(R_lo, 0.0));
                            used_mid = true;
                        }
                    }

                    if (sigma_bridge > nmw.bridge_max_sigma_cyc) { ++wc.skip_bridge_wide; continue; }
                    const double w = std::round(z_wl);
                    if (!(sigma_bridge < nmw.wl_fix_sigma_cyc &&
                          std::abs(z_wl - w) < nmw.wl_fix_frac_cyc)) { ++wc.skip_not_fixed; continue; }
                    // Wrong-integer backstop: if the DIRECT hop is ALSO FFR-credible on its own
                    // and rounds to a different integer, decide neither (counted).
                    if (used_mid) {
                        const double wd = std::round(z_wl_direct);
                        if (sigma_direct < nmw.wl_fix_sigma_cyc
                            && std::abs(z_wl_direct - wd) < nmw.wl_fix_frac_cyc
                            && wd != w) { ++wc.mid_disagree; continue; }
                    }

                    // Closure: the plain MW(1,2) DD must agree with w within its own σ (free
                    // integrity check).  Blocked only when MW(1,2) is present AND clearly off.
                    // NOTE (UWL-1 §3.3): in a post-slip cold window the primary-pair MW arc has
                    // usually reset too (mw12_ok=false ⇒ not blocked) — a cold decision is
                    // frac-gated + D5-backstopped + re-decided idempotently as stats mature.
                    if (mw12_ok) {
                        const double tol = 0.5 + 3.0 * std::sqrt(std::max(R_mw12, 0.0));
                        if (std::abs(z_mw12 - w) > tol) { ++wc.skip_closure; continue; }
                    }

                    // --- Increment 5 sibling closure (§1b): the untaken rung's statistic
                    // must sit on N_top·λ_top/λ_sib.  8-fx → z(7,5) vs 2·N₈ (the FINER
                    // sibling: a ±1 error in N₈ moves the residual by 2 — strongly
                    // detectable); 7-fx → z(8,5) vs N₇/2 (an odd N₇ makes the target
                    // half-integer — the AltBOC-coherence parity, visible in |frac|).
                    // MEASURED on every dual decision; the hard gate blocks only a
                    // ≥1-sibling-cycle disagreement and DEMOTES the cycle to the fallback
                    // rung (the fallback entry follows in ewl_fixed and is not deduped,
                    // because a blocked decision never consumes the row).
                    if (dual_cons) {
                        if (sib_ok && lam_sib > 0.0 && lam_ewl > 0.0) {
                            ++wc.sib_checked;
                            const double expect =
                                static_cast<double>(fx.integer) * (lam_ewl / lam_sib);
                            const double r     = z_sib - expect;
                            const double rfrac = r - std::round(r);
                            wc.sib_abs_frac_sum += std::abs(rfrac);
                            if (std::llround(r) != 0) ++wc.sib_int_off;
                            if (uwl_sib_gate_on) {
                                const double tol_s =
                                    0.5 + 3.0 * std::sqrt(std::max(R_sib, 0.0));
                                if (std::abs(r) > tol_s) { ++wc.sib_blocked; continue; }
                            }
                        } else {
                            ++wc.sib_unavail;
                        }
                    }

                    ++wc.cycles_decided;
                    wc.sum_bridge_R += R_bridge;
                    if (bridge_cold) ++wc.cold_bridge_cycles;
                    if (used_mid)    ++wc.mid_decided;
                    if (fx_is_top2)  ++wc.top2_decided;
                    if (dual_cons)   walk_decided.insert({ fx.block, fx.row });

                    // Group ids for the chord's two carried bands (chord.v → sat → primary pair,
                    // resolved at the loop top).
                    if (sat_lbl.empty()) { ++wc.skip_basis; continue; }
                    const char cons = ccons;
                    const auto pcit = primary.find({ chord.u, sat_lbl });
                    if (pcit == primary.end() || !pcit->second.ok) { ++wc.skip_basis; continue; }

                    if (lane_fix_on) {
                        // --- WL-3 §3.3: the FFR fix on the PAIR block — an information-
                        // idempotent lane-coordinate SET applied every DECIDED epoch (recenter
                        // + recollapse; a one-shot fold-on-change measured out — the decided
                        // lane drifts back to MW quality under q + noisy increments), plus the
                        // pair inject-fallback plan (applied post-solve only if the carry still
                        // disagrees by > inject_disagree_min_cyc; ≈0 in steady state — the
                        // retirement telemetry proves it).
                        const auto pgit = carry_group_id.find(
                            { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
                        if (pgit == carry_group_id.end()) { ++wc.skip_basis; continue; }
                        const double z_lin_wl = z_lin1 / lam1 - z_lin2 / lam2;
                        const int rc = carry_driver.applyLaneFix(
                            blocks, fx.block, fx.row, static_cast<long long>(std::llround(w)),
                            w - z_lin_wl, nmw.fix_weight_sigma_cyc);
                        if (rc == 1) {
                            ++wc.lane_fix_folded;
                            if (mw_tagsets) d5_hold_chords.insert(chord);
                        } else if (rc == 0) {
                            ++wc.lane_fix_unchanged;
                        } else {
                            ++wc.skip_basis;   // not a pair chord (residual-group cycle)
                            continue;
                        }
                        WlInjectPlan pl;
                        pl.pair = true;
                        pl.group_l1 = pl.group_l2 = pgit->second;
                        pl.chord = chord;
                        pl.w = w;
                        pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
                        pl.lambda1 = lam1;  pl.lambda2 = lam2;
                        pl.cons = cons;
                        wl_inject_epoch.push_back(pl);
                        continue;
                    }

                    const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
                    const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
                    if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) {
                        ++wc.skip_basis; continue;
                    }
                    const int group_l1 = g1it->second, group_l2 = g2it->second;

                    // Pre-solve carried lane value (for the hold-row gate): fold the integer-hard
                    // hold row ONLY when the carry already agrees (innovation ~0 ⇒ no F6 fight);
                    // otherwise skip it this epoch and let the post-solve inject move the value.
                    double z1p = 0.0, v1p = 0.0, z2p = 0.0, v2p = 0.0;
                    const bool have_l1 = carry_driver.carriedChordValue(group_l1, chord, z1p, v1p);
                    const bool have_l2 = carry_driver.carriedChordValue(group_l2, chord, z2p, v2p);
                    bool agree = false;
                    if (have_l1 && have_l2) {
                        const double A1 = (z1p + z_lin1) / lam1, A2 = (z2p + z_lin2) / lam2;
                        agree = std::abs(w - (A1 - A2)) <= nmw.inject_disagree_min_cyc;
                    }
                    if (agree) {
                        // Build/append the WL hold row (WL-cycle correction frame): +c/λ1 on col1,
                        // −c/λ2 on col2; zhat = w − Σ[(c/λ1)x_lin1 − (c/λ2)x_lin2]; Pz = fix_weight².
                        std::size_t ob;
                        auto hit = hold_block_of.find(fx.block);
                        if (hit == hold_block_of.end()) {
                            ob = wl_hold_out.size();
                            hold_block_of[fx.block] = ob;
                            wl_hold_out.emplace_back();
                        } else { ob = hit->second; }
                        std::vector<NetworkNormalEquations::DdPriorBlock::Term> hrow;
                        double z_lin_wl = 0.0;
                        hrow.reserve(terms.size() * 2);
                        bool hok = true;
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { hok = false; break; }
                            const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                            if (pit == primary.end() || !pit->second.ok) { hok = false; break; }
                            const PP& pp = pit->second;
                            const double l1 = lam(rit->second.sat, pp.sig1);
                            const double l2 = lam(rit->second.sat, pp.sig2);
                            if (!(l1 > 0.0 && l2 > 0.0)) { hok = false; break; }
                            hrow.push_back({ t.station, pp.col1, +t.coeff / l1 });
                            hrow.push_back({ t.station, pp.col2, -t.coeff / l2 });
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { hok = false; break; }
                            z_lin_wl += (t.coeff / l1) * l1it->second - (t.coeff / l2) * l2it->second;
                        }
                        if (hok) {
                            auto& dst = wl_hold_out[ob];
                            dst.rows.push_back(std::move(hrow));
                            const Eigen::Index k = static_cast<Eigen::Index>(dst.rows.size());
                            dst.zhat.conservativeResize(k);
                            dst.zhat(k - 1) = w - z_lin_wl;
                            dst.Pz.conservativeResize(k, k);
                            dst.Pz.row(k - 1).setZero();
                            dst.Pz.col(k - 1).setZero();
                            dst.Pz(k - 1, k - 1) = nmw.fix_weight_sigma_cyc * nmw.fix_weight_sigma_cyc;
                            ++wc.holds_folded;
                            if (mw_tagsets) d5_hold_chords.insert(chord);
                        }
                    }
                    // Stash the inject plan (applied post-solve against the FRESH posterior).  The
                    // 1:1-basis requirement is re-checked there (both bands must carry the chord).
                    WlInjectPlan pl;
                    pl.group_l1 = group_l1; pl.group_l2 = group_l2; pl.chord = chord;
                    pl.w = w; pl.z_lin1 = z_lin1; pl.z_lin2 = z_lin2;
                    pl.lambda1 = lam1; pl.lambda2 = lam2;
                    pl.cons = cons;
                    wl_inject_epoch.push_back(pl);
                }
              }  // --- end WL-2 WL lane ---

              if (mw_lane_trace) {   // F-B: lane-trace entries for EVERY resolvable primary cycle
                lane_trace_epoch.clear();
                std::map<int, std::string> sat_of_v;
                for (const auto& [s, v] : carry_sat_vertex) sat_of_v[v] = s;
                for (std::size_t b = 0; b < blocks.size(); ++b) {   // pure carry blocks (pre-append)
                    const auto& chords_b = carry_driver.blockChordsForTest(b);
                    for (std::size_t r = 0; r < blocks[b].rows.size(); ++r) {
                        if (r >= chords_b.size()) break;
                        const auto& terms = blocks[b].rows[r];
                        if (terms.empty()) continue;
                        LaneTraceEntry e;
                        e.chord = chords_b[r];
                        bool ok = true;
                        for (const auto& t : terms) {
                            const auto rit = revmap.find({ static_cast<int>(t.station),
                                                           static_cast<int>(t.local_col) });
                            if (rit == revmap.end()) { ok = false; break; }
                            const auto pit = primary.find({ rit->second.rx, rit->second.sat });
                            if (pit == primary.end() || !pit->second.ok
                                || rit->second.signal != pit->second.sig1
                                || static_cast<int>(t.local_col) != pit->second.col1) { ok = false; break; }
                            const PP& pp = pit->second;
                            const auto l1it = lin_of.find({ static_cast<int>(t.station), pp.col1 });
                            const auto l2it = lin_of.find({ static_cast<int>(t.station), pp.col2 });
                            if (l1it == lin_of.end() || l2it == lin_of.end()) { ok = false; break; }
                            e.z_lin1 += t.coeff * l1it->second;
                            e.z_lin2 += t.coeff * l2it->second;
                            e.lam1 = lam(rit->second.sat, pp.sig1);
                            e.lam2 = lam(rit->second.sat, pp.sig2);
                        }
                        if (!ok || !(e.lam1 > 0.0 && e.lam2 > 0.0)) continue;
                        const auto svit = sat_of_v.find(e.chord.v);
                        if (svit == sat_of_v.end()) continue;
                        e.sat = svit->second;
                        const char cons = e.sat.empty() ? '?' : e.sat.front();
                        const auto pcit = primary.find({ e.chord.u, e.sat });
                        if (pcit == primary.end() || !pcit->second.ok) continue;
                        if (lane_on) {
                            // WL-3: both bands live in ONE pair group; the write site reads
                            // the joint block via carriedPairValue (g1 == g2 marks it).
                            const auto pgit = carry_group_id.find(
                                { cons, pcit->second.sig1 + "+" + pcit->second.sig2 });
                            if (pgit == carry_group_id.end()) continue;
                            e.g1 = e.g2 = pgit->second;
                        } else {
                            const auto g1it = carry_group_id.find({ cons, pcit->second.sig1 });
                            const auto g2it = carry_group_id.find({ cons, pcit->second.sig2 });
                            if (g1it == carry_group_id.end() || g2it == carry_group_id.end()) continue;
                            e.g1 = g1it->second; e.g2 = g2it->second;
                        }
                        lane_trace_epoch.push_back(std::move(e));
                    }
                }
              }
                for (auto& wb : wl_out)      blocks.push_back(std::move(wb));   // WL-1 first ([Nc, Nc+n_wl))
                if (nmw.ewl_fold_rows)                                          // F-A discriminant: false ⇒
                    for (auto& wb : ewl_out) blocks.push_back(std::move(wb));   // decide/expose but fold nothing
                for (auto& wb : wl_hold_out) blocks.push_back(std::move(wb));   // then WL hold rows
            }
            est.setDdPriors(std::move(blocks));
        };
        subEnd(SB_GAUGEBUILD);   // D0b: close gauge build + carry-driver setup
        subBegin(SB_DDBUILD);    // D0b: carry_driver.beginEpoch + buildCarryDdPriors (W·Pᶻ·Wᵀ)
        if (carry_invariant) {
            // The z-store predict (q_z·Δt on the carried DD covariance) runs ONCE per
            // epoch; the build/fold may re-run on the RAIM re-solve.  D5 v2: the epoch time
            // (σ̂₀² reporting bucket) + clean flag (always clean here — under carry_invariant
            // the eq34 datum re-realizes freely, so there is no re-level storm to gate; the
            // robust geo-mean shrugs off the content-hr-4 event tail on its own).
            const double dt = std::isfinite(carry_prev_tow) ? (se.time.tow - carry_prev_tow) : 0.0;
            carry_driver.beginEpoch(dt > 0.0 ? dt : 0.0, se.time.tow, /*clean=*/true);
        }
        { CarryEigenScope es(carry_par, n_threads); buildCarryDdPriors(); }  // parallel_carry: thread W·Pᶻ·Wᵀ
        subEnd(SB_DDBUILD);      // D0b: close buildCarryDdPriors (the Card-4 DD-cov projection)
        subBegin(SB_SOLVEGLUE);  // D0b: pre-solve prep + clock predict + solve + within-epoch re-solve

        // --- Satellite code-bias datum constraints (W6, full multi-band) -----
        // Each shared per-(sat,signal) code bias opens gauge freedoms the data
        // cannot resolve; fold them here — BEFORE the null-space gate — as pure-
        // gauge rows so the reduced shared system keeps only the per-constellation
        // clock deficiency the explicit clock datum pins (declared_dimension
        // unchanged).  Generalised to ALL bands and tracking channels, the gauges:
        //   (1) per-satellite CLOCK-COMMON direction — a constant added to every
        //       one of a satellite's code biases is absorbed by its estimable
        //       clock.  Pinned by an ionosphere-free pin on the two lowest distinct-
        //       frequency signals (a1+a2=1), keeping the emitted OSB pure-DCB with
        //       its IF lump in the clock (else clock and OSB double-count it); and
        //   (2) per-(CONSTELLATION, SIGNAL) absolute level — each registered code
        //       signal's constellation-wide common mode is a gauge the GIM common-
        //       mode-free iono weighting leaves free.  Pinned by a sum-to-zero over
        //       the satellites carrying that signal — one row per (constellation,
        //       signal).  This adopts the standard IGS/AC OSB convention that every
        //       channel's per-constellation mean is zero (cross-checked against the
        //       COD .BIA: same-band inter-channel means are ~0 — exactly 0 for
        //       Galileo and GPS L2, <=0.14 m only on the handful of GPS L1C sats).
        //       The earlier two-band slice pinned only the lowest band, so every 3rd
        //       band (GPS L5, Galileo E5b/E6/E5) left an unpinned exact gauge that
        //       withheld the whole epoch.
        //       Why per-SIGNAL, not per-band: pinning one representative channel per
        //       band leaves a *non-co-tracked* same-band channel (e.g. Galileo C5Q
        //       vs C5X, seen by disjoint receivers so their inter-channel bias is
        //       unobservable) an unpinned gauge that withholds the epoch at network
        //       scale.  Per-signal pins every channel's level robustly.  For the
        //       minority of co-tracked same-band channels whose inter-channel mean IS
        //       observable, this also fixes that mean to the conventional zero (a
        //       sub-decimetre effect, per the .BIA cross-check); a connected-component
        //       datum that pins only genuinely-unobservable levels is future work.
        // These are pure gauge directions (no data information), so any positive
        // weight fixes them without biasing the estimable (between-satellite) DCB
        // structure the GIM makes observable.  Over-covering one level with both a
        // sum-to-zero and the IF pin is harmless: redundant positive-semidefinite
        // information cannot create a new near-zero direction (Weyl), so generous
        // coverage cannot trip the gate — only an UNpinned gauge can.  If the GIM
        // iono constraint is absent the between-satellite DCBs stay confounded with
        // the slant iono and the gate trips ("under-specified") — the safety net.
        //
        // Reset the hold-OFF IF-pin selection scratch for THIS epoch (mirrors the
        // per-epoch this_epoch_anchor.clear()).  Unconditional — done even when the
        // code-bias block below is skipped (no registered keys) so a stale prior-epoch
        // selection can never reach the emit-site comparison.  A no-op on the empty
        // map in the default (emit_clock_disc OFF) config.
        this_epoch_if_pin_sel.clear();
        this_epoch_cb_ifpin.clear();   // Card 2: this epoch's IF-pin pairs (persisted at harvest)
        this_epoch_cb_reseed.clear();  // Card 2: this epoch's re-seeds (merged into the pending disc set at the emit site — withheld epochs never reach it)
        if (config_.estimateNetworkSatCodeBias() && !registered_code_keys.empty()) {
            constexpr double kCodeBiasGaugeWeight = 1.0e3;  // » gate cutoff (1e-9·σmax), « σmax → no spectral inflation
            const Eigen::Index nshared = est.numShared();

            // Carrier frequency of a (sat,signal) from any present station's model.
            auto freqOf = [&](const std::string& sat, const std::string& sig) -> double {
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i]) {
                        const double f = stations[i].mm->carrierFrequency(sig, sat);
                        if (std::isfinite(f) && f > 0.0) return f;
                    }
                return 0.0;
            };

            std::map<std::string, std::vector<std::string>> sat_signals;  // sat -> code signals
            std::vector<std::string> sat_order;
            for (const auto& kv : registered_code_keys) {
                auto& v = sat_signals[kv.first];
                if (v.empty()) sat_order.push_back(kv.first);
                v.push_back(kv.second);
            }

            std::vector<Eigen::VectorXd> rows;                 // length-nshared constraint Jacobians
            std::vector<double>          zs;                   // Card 2: per-row RHS target (all 0.0 unless code_datum_time_linked)

            // Card 2 (network.code_datum_time_linked): fetch a satellite's PREVIOUS
            // kept-epoch solved code bias for a signal — a "veteran" reference.  An
            // empty map (off, or first appearance) ⇒ always false ⇒ the zero-target
            // legacy path (byte-identical).
            auto prevCb = [&](const std::string& sat, const std::string& sig,
                              double& out) -> bool {
                const auto it = code_bias_prev_.find(
                    NetworkSbasis::constellationOf(sat) + "|" + sig);
                if (it == code_bias_prev_.end()) return false;
                const auto jt = it->second.find(sat);
                if (jt == it->second.end()) return false;
                out = jt->second;
                return true;
            };

            // (2) Per-(constellation, signal) sum-to-zero — one row per registered
            // signal, summed over the satellites that carry it (keyed
            // "<constellation>|<signal>").  Pins every channel's absolute level; see
            // the block header for why this is per-SIGNAL (robust to non-co-tracked
            // same-band channels at network scale) rather than per-band.  Collected as
            // (sat, column) per channel (std::map → sorted-skey order, unchanged); the
            // rows are BUILT after the IF-pin loop so their order in `rows` is preserved
            // (IF pins first, then sum-to-zero) — and, under time-linking, each row
            // spans the channel's VETERANS with RHS Σ b̂_prev instead of all present
            // satellites with RHS 0.
            std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> st0_chan;
            for (const auto& kv : registered_code_keys) {
                Eigen::Index col;
                try { col = est.codeBiasColumn(kv.first, kv.second); } catch (...) { continue; }
                const std::string skey =
                    NetworkSbasis::constellationOf(kv.first) + "|" + kv.second;
                st0_chan[skey].emplace_back(kv.first, col);
            }

            // (1) Per-satellite ionosphere-free pin (clock-common gauge).
            for (const std::string& sat : sat_order) {
                std::vector<std::string>& sigs = sat_signals[sat];
                // IF pin: two lowest DISTINCT-frequency code signals → a1+a2=1.
                // Selection is DETERMINISTIC in the signal SET (sorted by band
                // digit THEN full signal name), so same-band channels (C1C vs
                // C1W) no longer swap with registered_code_keys insertion order
                // and silently re-datum the emitted clock — see
                // NetworkSbasis::selectIfPinPair.
                const auto if_pin = NetworkSbasis::selectIfPinPair(
                    sigs, [&](const std::string& s) { return freqOf(sat, s); });
                std::string sig1 = if_pin.first;
                std::string sig2 = if_pin.second;
                double      f1   = sig1.empty() ? 0.0 : freqOf(sat, sig1);
                double      f2   = sig2.empty() ? 0.0 : freqOf(sat, sig2);
                // network.clock_signal_basis_hold: anchor this satellite's clock to
                // ONE ionosphere-free reference pair for the whole run so the emitted
                // (δtˢ, dˢ) split is time-consistent, instead of re-selecting the
                // pair from THIS epoch's coverage (which re-references the clock by
                // the satellite's inter-channel code bias whenever a sparse channel
                // enters/leaves the ≥2-station set — the defect-2 differential-clock
                // wander a receiver clock cannot absorb).  Byte-identical OFF for the
                // HOLD feature: held_clock_ref is neither read nor written (the
                // deterministic selection above stands unchanged).
                if (clock_signal_basis_hold) {
                    // Is a held reference signal still in this satellite's registered
                    // ≥2-station set this epoch?  (Named to avoid shadowing the outer
                    // per-station `present` vector.)
                    auto held_present = [&](const std::string& s) {
                        return !s.empty() &&
                               std::find(sigs.begin(), sigs.end(), s) != sigs.end();
                    };
                    auto held = held_clock_ref.find(sat);
                    if (held != held_clock_ref.end() &&
                        held_present(held->second.first) && held_present(held->second.second)) {
                        // HOLD: both held reference signals are still registered —
                        // reuse them so the clock stays on the prior signal basis.
                        sig1 = held->second.first;
                        sig2 = held->second.second;
                        f1   = freqOf(sat, sig1);
                        f2   = freqOf(sat, sig2);
                    } else if (!sig2.empty() && f2 > 0.0) {
                        // LATCH (first sight) / RE-LATCH (a held reference signal left
                        // the ≥2-station set): adopt the freshly-selected pair and hold
                        // it forward.  A re-latch is a genuine (rare) datum move — log
                        // it for observability (network.trace_events); the emitted clock
                        // step it causes is the satellite's inter-channel bias.
                        if (held != held_clock_ref.end() &&
                            (held->second.first != sig1 || held->second.second != sig2)) {
                            emitEvent("CLK_BASIS", "relatch", sat, "", std::nullopt,
                                      std::nullopt, "held_ref_change",
                                      held->second.first + "+" + held->second.second +
                                          "->" + sig1 + "+" + sig2);
                            // T3c: the re-latch stepped the emitted clock↔code split — signal it as
                            // a per-sat discontinuity (bumped at the next emit) so the consumer resets
                            // instead of integrating across the unsignalled step.  Guarded by the
                            // clock-disc channel; the CLK_BASIS event above is logged either way.
                            if (emit_clock_disc) basis_relatch_disc_pending.insert(sat);
                        }
                        held_clock_ref[sat] = { sig1, sig2 };
                    }
                }
                // Hold OFF: the pin is re-derived from THIS epoch's coverage every
                // epoch.  With the deterministic selection above the pair is a pure
                // function of the registered signal SET, so a change BETWEEN EMITTED
                // products means a channel actually entered/left the ≥2-station set —
                // the same defect-2 clock↔code re-reference a hold re-latch signals,
                // only here it happens silently every time coverage churns.  Record
                // THIS epoch's selection in the main-pass scratch and defer the compare
                // + disc bump to the EMIT site (against the last EMITTED pair), so a
                // transient pin move on a withheld epoch cannot fabricate a bump — the
                // same emit-only discipline prev_pivot / emitted_anchor follow.  Guarded
                // by emit_clock_disc AND a real IF pair (f1,f2 > 0) ⇒ byte-identical when
                // the disc channel is off or the sat is single-frequency.  Skipped under
                // hold (that path owns its own re-latch signalling above).
                if (!clock_signal_basis_hold && emit_clock_disc && f1 > 0.0 && f2 > 0.0)
                    this_epoch_if_pin_sel[sat] = { sig1, sig2 };
                Eigen::VectorXd row = Eigen::VectorXd::Zero(nshared);
                bool row_ok = false;
                double zval = 0.0;   // Card 2: legacy zero target unless time-linked
                if (f1 > 0.0 && f2 > 0.0) {
                    const double den = f1 * f1 - f2 * f2;
                    const double a1 =  f1 * f1 / den;     // ionosphere-free (clock-preserving) weights
                    const double a2 = -f2 * f2 / den;
                    try {
                        row(est.codeBiasColumn(sat, sig1)) = a1;
                        row(est.codeBiasColumn(sat, sig2)) = a2;
                        row_ok = true;
                    } catch (...) { row.setZero(); row_ok = false; }
                    // Card 2: pin the CHANGE of the clock↔code split — RHS is the
                    // satellite's PREVIOUS solved ionosphere-free combination
                    // a1·b̂_prev(σ1)+a2·b̂_prev(σ2).  This is genuine continuity ONLY
                    // when the pair is UNCHANGED from the last kept epoch AND both
                    // signals had a previous solved value.  A pair CHANGE (the band-1
                    // C1C↔C1W flip, or a band entering/leaving) re-references the split
                    // by the satellite's inter-channel bias even if the new pair's
                    // signals both have prior values, so it must re-seed to 0 and bump —
                    // otherwise the emitted clock steps unsignalled.  On any re-seed that
                    // is NOT the satellite's first appearance (seen before, or the run
                    // resumed), bump its disc so the value step coincides with a
                    // signalled discontinuity.  Record this epoch's pair for the next
                    // epoch's comparison (persisted at the harvest, kept epochs only).
                    if (row_ok && cb_time_linked) {
                        double v1 = 0.0, v2 = 0.0;
                        const bool h1 = prevCb(sat, sig1, v1);
                        const bool h2 = prevCb(sat, sig2, v2);
                        const auto pit = code_bias_ifpin_pair_.find(sat);
                        const bool pair_same =
                            pit != code_bias_ifpin_pair_.end() &&
                            pit->second.first == sig1 && pit->second.second == sig2;
                        if (h1 && h2 && pair_same)
                            zval = a1 * v1 + a2 * v2;
                        else if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_)
                            this_epoch_cb_reseed.insert(sat);
                        this_epoch_cb_ifpin[sat] = { sig1, sig2 };
                    }
                } else {
                    // Single distinct frequency (degenerate): pin the all-ones gauge
                    // so the satellite's code-bias common mode is still fixed.  No IF
                    // split to time-link (kept at the zero target).
                    for (const std::string& s : sigs)
                        try { row(est.codeBiasColumn(sat, s)) = 1.0; row_ok = true; } catch (...) {}
                }
                if (row_ok && row.cwiseAbs().sum() > 0.0) {
                    rows.push_back(std::move(row));
                    zs.push_back(zval);
                }
            }
            // (2) Build the sum-to-zero rows now (after the IF pins, preserving row
            // order).  OFF: span every present satellite with RHS 0 (byte-identical).
            // Time-linked: span the channel's VETERANS with RHS Σ b̂_prev (pin the
            // change of the veteran mean); a no-veteran channel falls back to the
            // all-present Σ = 0 seed and disc-bumps its present satellites (the level
            // genuinely re-references) — first-ever appearances excepted, matching the
            // IF-pin rule.
            for (const auto& [skey, entries] : st0_chan) {
                Eigen::VectorXd srow = Eigen::VectorXd::Zero(nshared);
                double zval = 0.0;
                if (cb_time_linked) {
                    std::vector<std::string>            present_sats;
                    std::map<std::string, Eigen::Index> col_of;
                    present_sats.reserve(entries.size());
                    for (const auto& [sat, col] : entries) {
                        present_sats.push_back(sat);
                        col_of[sat] = col;
                    }
                    std::map<std::string, double> prev_vals;
                    const auto pit = code_bias_prev_.find(skey);
                    if (pit != code_bias_prev_.end())
                        for (const auto& sat : present_sats) {
                            const auto vit = pit->second.find(sat);
                            if (vit != pit->second.end()) prev_vals[sat] = vit->second;
                        }
                    const auto tl = NetworkSbasis::codeS2zTimeLink(present_sats, prev_vals);
                    for (const auto& sat : tl.jac_sats) srow(col_of[sat]) += 1.0;
                    zval = tl.rhs;
                    if (tl.reseed)
                        for (const auto& sat : present_sats)
                            if (code_bias_sat_seen_.count(sat) || code_bias_datum_resumed_)
                                this_epoch_cb_reseed.insert(sat);
                } else {
                    for (const auto& [sat, col] : entries) srow(col) += 1.0;
                }
                if (srow.cwiseAbs().sum() > 0.0) {
                    rows.push_back(std::move(srow));
                    zs.push_back(zval);
                }
            }
            // Card 2: this_epoch_cb_reseed now holds every satellite whose datum
            // re-referenced THIS epoch.  It is NOT merged into the persistent
            // cb_reseed_disc_pending here — that merge is deferred to the EMIT site
            // (below), which every whole-epoch withhold path `continue`s BEFORE reaching.
            // So a transient re-seed on a since-withheld epoch (the frame reverts by the
            // next kept epoch) never enters the pending set and cannot fire a bump with no
            // emitted value step — the emit-only discipline, without needing to erase (an
            // erase would destroy a bump legitimately DEFERRED for a satellite re-seeded
            // while pruned from the emission).

            if (!rows.empty()) {
                Eigen::MatrixXd H(static_cast<Eigen::Index>(rows.size()), nshared);
                for (size_t r = 0; r < rows.size(); ++r)
                    H.row(static_cast<Eigen::Index>(r)) = rows[r];
                // Card 2: per-row targets — all 0 on the legacy path (byte-identical),
                // the veteran-referenced / previous-IF values under time-linking.
                Eigen::VectorXd z(static_cast<Eigen::Index>(rows.size()));
                for (size_t r = 0; r < zs.size(); ++r)
                    z(static_cast<Eigen::Index>(r)) = zs[r];
                est.addSharedConstraintRows(H, z, kCodeBiasGaugeWeight);
                if (within_epoch_armed) {
                    cap_H = H; cap_z = z;
                    cap_constraint_weight = kCodeBiasGaugeWeight;
                    cap_has_constraint = true;
                }
            }
            if (spectrum_diag_budget > 0)   // first-N (spectrum block decrements below)
                log->warn("Network[W6-cb]: nshared={}, n_cb={}, folded {} code-bias gauge "
                          "rows (IF pin for {} sats + sum-to-zero for {} (const,signal) levels)",
                          est.numShared(), n_cb, rows.size(), sat_order.size(), st0_chan.size());
        }

        // --- Datum + null-space gate + solve --------------------------------
        NetworkSbasis sb;
        SbasisDeclaration decl;
        decl.reference_receiver_id = nc.clock_datum_station;
        decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // W6: self-describe the OSB datum
        // Fix A: self-describe the anchor-free ensemble phase-bias frame so a
        // consumer knows the emitted bias references the receiver ensemble (Σ_r z=0),
        // not a single pivot receiver's ambiguity.
        if (phase_datum_ensemble)
            decl.amb_datum = SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
        // Card 3: self-describe the Eq-34 single-receiver-chain phase-bias frame so a
        // consumer knows the emitted between-satellite reference is integer-consistent
        // (each chain hop an integer) — wire value 2 rides the existing v2 S line.
        else if (phase_datum_eq34)
            decl.amb_datum = SbasisDeclaration::AmbDatum::SingleReceiverChain;
        if (clock_brdc_anchored) {
            // Broadcast-anchored clock gauge: buildSharedDatum branches on this to
            // emit one per-satellite anchor row (Δδtˢ→0 = its own broadcast
            // a-priori) at 1/σ² — the anchors ARE the datum (no pivot, empty datum,
            // declared dimension 0), and unlike sum-to-zero the frame does not
            // redistribute when membership changes.
            decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
            decl.broadcast_anchor_sigma_m = clk_anchor_sigma;  // D6-resolved (brdc_rts auto = 3.0 m; load-validated binding)
        } else if (clock_sum_to_zero) {
            // Anchor-free clock gauge: buildSharedDatum branches on this to emit
            // per-constellation sum-to-zero constraint rows (no pivot, empty datum).
            decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
        } else if (!nc.clock_datum_satellite.empty()) {
            decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                = nc.clock_datum_satellite;
        }

        NetworkSbasis::Plan plan;
        try {
            // RTA-1: in sticky mode the incumbent baseline is last_pivot, NOT
            // prev_pivot — prev_pivot is wholesale-replaced at each emit, so a
            // constellation absent from the last emitted epoch drops out of it,
            // and its return would spuriously re-select (and, with the
            // vanish-return arm off, SILENTLY re-reference the datum).
            // last_pivot is merge-updated at the same emit site, so per
            // constellation it holds the pivot of the last emitted product that
            // CONTAINED it — exactly the datum a consumer of that constellation
            // last adopted.  Both maps advance only at the emit site (P0-1), so
            // stickiness tracks the datum of the last EMITTED product: a
            // handover epoch that is subsequently withheld leaves the incumbent
            // unchanged, and if the incumbent returns before any new-datum epoch
            // emits, the datum simply never moves.
            plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                       sticky_pivot ? &last_pivot : nullptr);
        } catch (const std::exception& ex) {
            log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("datum_failed", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Constraint-realized clock datums: fold the gauge rows onto the shared
        // border BEFORE the null-space gate — sum-to-zero mode carries one
        // per-constellation Σδtˢ=0 row, broadcast-anchored mode one per-satellite
        // Δδtˢ→0 anchor row at 1/σ² (the anchors ARE the datum there) — so plan
        // carries constraint rows instead of hard pins and declared_dimension is 0.
        // Folded exactly like the code-bias gauge (additive information; the
        // within-epoch re-solve re-applies them from plan, which is in scope
        // there).  Empty (0 rows) in PIN mode → this is a no-op, byte-identical.
        if (plan.clock_constraint_H.rows() > 0)
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);

        // Pivot handover: the previous epoch's shared estimate lives in the OLD
        // datum, so a changed clock pivot invalidates the D2 carry-forward (and
        // forces a discontinuity bump downstream).
        // A1/A2: detect a clock-pivot datum re-reference and record WHICH
        // constellations changed (for the A2 per-constellation emit-bump scoping).
        // Arm (b) (vanish-return) is gated on network.pivot_vanish_return_signal;
        // when off this reproduces the legacy adjacent-epoch-only detection.
        std::set<char> changed_consts;
        const bool datum_changed = pivotHandoverSignalled(
            plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
        // RTA-2: does THIS key's carried state belong to a retired datum?  With
        // network.scoped_handover_response off this is exactly datum_changed (the
        // legacy global response, byte-identical); when on, only keys of the
        // constellation(s) whose pivot changed — the SAME changed_consts set the
        // A2 emit bump consumes (single source of truth) — are invalidated.
        const auto handover_affects = [&](const std::string& sat) {
            return datum_changed
                && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
        };

        // --- SH-VTEC prior (Kaula + temporal), folded BEFORE the gate ----------
        // The SH coefficients are SHARED columns in the reduced normals; without a
        // bound on every one, a coefficient whose IPP support rotated over an
        // ocean/polar gap is near-null and trips the all-or-nothing rank gate
        // (design §6.1).  A degree-dependent Kaula prior (σ_n = kaula_scale/n; a₀₀
        // a LOOSE climatology prior so the mean VTEC stays data-determined and is
        // NOT clamped to an external value, §6.2) keeps every coefficient PSD-bounded
        // → declared_dimension stays = #constellations regardless of coverage.  The
        // SAME prior carries the field forward in time as a bounded random walk
        // (anchor the previous estimate, inflate by the per-degree q, but never
        // exceed the Kaula bound).  Folded via addSharedPrior BEFORE the gate (so the
        // gate sees the regularized SH block) — unlike the clock/bias D2 carry-
        // forward, which must stay AFTER the gate (it would otherwise fill the clock
        // gauge the datum pins).
        if (config_.estimateNetworkIonoSh() && sh_model && est.numShared() > 0) {
            std::map<std::tuple<int, int, char>, const CarryPrior*> sh_carry;
            // RTA-2 scope decision: the SH-VTEC carry skip deliberately stays
            // GLOBAL even under network.scoped_handover_response.  The SH
            // coefficients are constellation-LESS shared columns, so no scoped
            // subset exists for them; and although the pure clock-gauge
            // S-transform leaves the iono columns invariant in exact arithmetic,
            // the carried SH posterior was estimated JOINTLY with the old-datum
            // clock/bias states, so the conservative (existing) response is kept:
            // on ANY datum change the RW-toward-previous term is dropped for one
            // epoch and the coefficients fall back to the always-on Kaula
            // climatology anchor below (still PSD-bounded, gate-safe) — a cheap,
            // bounded cost that avoids scoping a column that belongs to no
            // constellation.
            if (!datum_changed)
                for (const CarryPrior& cp : prev_carry)
                    if (cp.kind == SharedParamKind::VtecSh)
                        sh_carry.emplace(std::make_tuple(cp.sh_n, cp.sh_m, cp.sh_trig), &cp);

            std::vector<NetworkEstimator::SharedPrior> sh_priors;
            sh_priors.reserve(sh_model->coeffs().size());
            for (const auto& c : sh_model->coeffs()) {
                const double kaula_sigma = (c.n == 0)
                    ? shc.mean_vtec_prior_sigma_tecu
                    : shc.kaula_scale_tecu / static_cast<double>(c.n);
                const double kaula_var = kaula_sigma * kaula_sigma;
                const double clim = (c.n == 0 && c.m == 0 && c.trig == 'C')
                    ? shc.mean_vtec_prior_tecu : 0.0;   // a₀₀ → mean VTEC; others → 0

                // Mean-reverting (first-order Gauss-Markov) prior: ALWAYS apply the
                // Kaula climatology anchor (toward clim, σ = kaula_sigma) AND, when a
                // posterior was carried forward, a random-walk anchor (toward the
                // previous estimate, inflated by the per-degree q).  Combining them in
                // information space pulls every coefficient back toward climatology
                // each epoch (so the field cannot random-walk away — the design's
                // "unobserved coefficients are pulled to their prior", §6.1) while the
                // RW term preserves temporal continuity for well-observed ones.  A
                // pure RW-toward-previous (no climatology re-assertion) let the global
                // field drift to unphysical magnitudes; this is the bound.
                double info  = 1.0 / kaula_var;       // climatology Tikhonov (always on)
                double sumiv = info * clim;
                const auto it = sh_carry.find(std::make_tuple(c.n, c.m, c.trig));
                if (it != sh_carry.end() && std::isfinite(it->second->value)
                    && it->second->var > 0.0) {
                    const double q_deg = (c.n == 0)
                        ? shc.q_per_degree
                        : shc.q_per_degree / static_cast<double>(c.n * c.n);
                    const double var_rw = it->second->var + q_deg * std::max(0.0, dt);
                    if (var_rw > 0.0) {
                        const double info_rw = 1.0 / var_rw;
                        info  += info_rw;
                        sumiv += info_rw * it->second->value;
                    }
                }
                if (!(info > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::VtecSh;
                p.sh_n = c.n; p.sh_m = c.m; p.sh_trig = c.trig;
                p.value = sumiv / info; p.info = info;
                sh_priors.push_back(p);
            }
            est.addSharedPrior(sh_priors);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), sh_priors.begin(), sh_priors.end());
        }

        // --- Fix A hold: held satellite phase-bias datum (seed-once + time-lock) ---
        // Inject the held b_s priors BEFORE the null-space gate — like the SH-VTEC
        // prior above, and UNLIKE the clock/code D2 carry below (which must stay
        // post-gate so the gate sees the data-only clock-gauge structure).  A held
        // (sat,signal) has its Σz=0 fold SKIPPED in buildEnsembleGauges (a hard w→∞
        // fold would override a finite prior), so this stiff fixed-variance prior is
        // that column's DATUM: it must be in the reduced normals the gate inspects, or
        // the held column reads as a near-null gauge and trips the O(n³) heal / a
        // withhold every epoch.  (In practice the tight local ambiguity carry usually
        // already lifts it above the cutoff, but this makes the datum explicit and
        // robust to a weak/cold carry on a fresh arc.)  The phase-ensemble datum is
        // orthogonal to the clock-pivot datum_changed handover, so it is injected
        // EVERY epoch regardless.  Phase biases are excluded from the clock-frame D2
        // carry loops below under hold, so this is the single injection (no double
        // fold).  Inert / no-op unless phase_hold_on (byte-identical).
        if (phase_hold_on && !prev_carry.empty()) {
            std::vector<NetworkEstimator::SharedPrior> hp;
            hp.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind != SharedParamKind::PhaseBias) continue;
                if (!(cp.var > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
                p.value = cp.value; p.info = 1.0 / cp.var;
                hp.push_back(p);
            }
            est.addSharedPrior(hp);  // skips keys absent from this epoch's layout
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
        }

        // Relative eigenvalue tolerance for the null-space rank gate (see the gate
        // block below for the derivation of 1e-9); hoisted here so the prioritized
        // drop-and-heal uses the SAME cutoff as the gate it is protecting.
        constexpr double kNullSpaceTol = 1e-9;

        // --- Prioritized phase-bias datum: spectrum-based drop-and-heal helper ----
        // The prioritized union registers a phase-bias column for every (sat,signal)
        // that any two stations track.  Two things can leave such a column below the
        // rank-gate cutoff and would withhold the WHOLE epoch (every well-anchored
        // satellite lost to one bad column): (1) its ONLY ambiguity pin lived in an
        // anchor station that was DROPPED in the serial consume (a rank-deficient
        // local block) — a pure near-null orphan; (2) the joint W6 code-bias
        // estimation leaves a marginally-tracked satellite's phase bias weakly
        // separated from its clock — a near-null-but-real column.  This helper
        // identifies every near-null direction of the reduced shared normals spanned
        // DOMINANTLY by phase-bias columns (NOT the per-constellation clock gauges —
        // those have ~zero phase-bias weight) and soft-anchors exactly those columns
        // toward 0 (σ = 1 m: far above the cutoff so the residual gauge is resolved,
        // negligible against the 1e10 ambiguity pins so well-anchored columns are
        // untouched), returning the count healed.  A soft-anchored column is a
        // bounded, signalled degradation for one epoch (the emitter bias-jump disc
        // bumps when its proper ambiguity anchor returns).
        //
        // ★ PERF: this does a FULL eigenVECTOR decomposition of the reduced normals
        // (O(n_shared³) — ~1156³ at 305 stations), so it is invoked ONLY from the
        // gate-failure path below, never every epoch.  A healthy network passes the
        // cheap eigenvalue-only gate on the first try and never pays this cost (the
        // earlier unconditional placement made every 305 epoch grind on a needless
        // eigensolve).
        auto healNearNullPhaseBias = [&]() -> int {
            if (!phase_datum_prioritized || est.numShared() == 0 || shared_phase_keys.empty())
                return 0;
            const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
            if (es.info() != Eigen::Success) return 0;
            const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
            const double smax = ev.size() ? ev(ev.size() - 1) : 0.0;
            const double cutoff = kNullSpaceTol * std::max(smax, 1.0);
            std::map<int, std::pair<std::string, std::string>> pbcol;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) pbcol[static_cast<int>(
                    est.phaseBiasColumn(p->satellite, p->signal))] = {p->satellite, p->signal};
            }
            // Every phase-bias column meaningfully spanning a near-null direction
            // (|component| > 0.3) is weakly/un-observable → heal it.  A clock-gauge
            // direction has ~zero phase-bias weight, so its minor PB participants
            // stay below 0.3 and are left alone.
            constexpr double kSpanFrac = 0.3;
            std::set<int> heal_cols;
            for (Eigen::Index j = 0; j < ev.size(); ++j) {
                if (ev(j) >= cutoff) continue;                 // observable direction
                const Eigen::VectorXd v = es.eigenvectors().col(j).cwiseAbs();
                for (const auto& [c, ss] : pbcol)
                    if (c < v.size() && v(c) > kSpanFrac) heal_cols.insert(c);
            }
            if (heal_cols.empty()) return 0;
            std::vector<NetworkEstimator::SharedPrior> hp;
            hp.reserve(heal_cols.size());
            for (int c : heal_cols) {
                NetworkEstimator::SharedPrior p;
                p.kind   = SharedParamKind::PhaseBias;
                p.sat    = pbcol[c].first;
                p.signal = pbcol[c].second;
                p.value  = 0.0;
                p.info   = 1.0;   // σ = 1 m
                hp.push_back(p);
            }
            est.addSharedPrior(hp);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
            return static_cast<int>(hp.size());
        };

        // Null-space rank gate.  The reduced shared normals are deliberately
        // ill-conditioned: the phase rows pin the satellite-clock / phase-bias
        // SUM to ~σ_phase precision (weight ~1e5), while CODE alone separates the
        // two (the clock-vs-bias split), so that split is orders of magnitude
        // weaker — yet still firmly non-zero.  Empirically the spectrum shows a
        // clean ~7-decade gap between the true gauge freedoms (the per-
        // constellation clock datum, σ ~ 1e-10 of σ_max) and the weakest REAL
        // direction (σ ~ 1e-2).  The default 1e-6 relative tolerance sits above
        // the weak-but-real directions and miscounts them as deficiencies; a
        // 1e-9 relative tolerance lands squarely in the gap, counting only the
        // genuine gauges while still catching a truly unpinned datum.
        // (kNullSpaceTol is declared just above the drop-and-heal block, which
        // shares this cutoff.)

        // ---- One-shot estimability diagnostic (default-OFF) ---------------
        // On the FIRST epoch that reaches the gate, optionally dump the tail of
        // the reduced shared-normal eigen-spectrum, so we can see WHERE the null
        // space sits relative to the declared datum dimension: a clean ~7-decade
        // gap at exactly declared_dim ⇒ the 1e-9 tolerance is mis-set; one or more
        // EXTRA tiny eigenvalues above the gap ⇒ a genuinely unpinned direction (a
        // missing datum constraint).
        //
        // The reduced shared normals are SYMMETRIC positive-SEMIdefinite (a Schur
        // complement of weighted Gram normals — see NetworkSbasis), so their
        // eigenvalues EQUAL their singular values: a self-adjoint eigensolve yields
        // the exact same spectrum the rank gate below tests, but at a small fraction
        // of a 1156×1156 JacobiSVD(ComputeFullV)'s cost — that one-shot SVD added
        // ~90 s to EVERY 305-station epoch-1.  Naming the gauge parameters still
        // needs the eigenVECTORs, so this stays a full decomposition and is gated
        // behind network.trace_estimability (default OFF); a production run pays
        // nothing.  The one-shot LATCH is always taken so the co-located W6-cb
        // one-shot log above still fires exactly once.  Eigenvalues come back
        // ASCENDING, so the largest is the LAST entry and the gauges are the FIRST.
        if (spectrum_diag_budget > 0) {
            --spectrum_diag_budget;
            if (nc.trace_estimability) {
                const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
                Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(
                    Nss, Eigen::ComputeEigenvectors);
                if (es.info() == Eigen::Success) {
                    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
                    const Eigen::Index k = ev.size();
                    const double smax = (k > 0) ? ev(k - 1) : 0.0;  // largest = last
                    const double cutoff = kNullSpaceTol * std::max(smax, 1.0);
                    int below = 0;
                    for (Eigen::Index i = 0; i < k; ++i) if (ev(i) < cutoff) ++below;
                    // Smallest ntail eigenvalues, printed largest-first so the very
                    // smallest (the gauge freedoms) sit at the end of the list.
                    const Eigen::Index ntail = std::min<Eigen::Index>(14, k);
                    std::string tail;
                    for (Eigen::Index i = ntail - 1; i >= 0; --i)
                        tail += fmt::format("{}{:.3e}", (i == ntail - 1 ? "" : ", "), ev(i));
                    log->warn("Network[estimability]: TOW {:.1f} reduced_N_ss {}x{}, "
                              "declared_dim={}, ev_max={:.3e}, cutoff={:.3e}, {} ev<cutoff; "
                              "smallest {} ev = [{}]",
                              se.time.tow, Nss.rows(), Nss.cols(), plan.declared_dimension,
                              smax, cutoff, below, ntail, tail);
                    // Name the parameters spanning each near-zero (unpinned-gauge)
                    // direction, printed largest-first to mirror the tail line above.
                    const Eigen::MatrixXd& V = es.eigenvectors();
                    const Eigen::Index ndump = std::min<Eigen::Index>(below + 2, k);
                    for (Eigen::Index j = ndump - 1; j >= 0; --j) {
                        const Eigen::VectorXd v = V.col(j).cwiseAbs();
                        std::vector<Eigen::Index> idx(v.size());
                        for (Eigen::Index t = 0; t < v.size(); ++t) idx[t] = t;
                        std::partial_sort(idx.begin(),
                                          idx.begin() + std::min<Eigen::Index>(5, v.size()), idx.end(),
                                          [&](Eigen::Index a, Eigen::Index b){ return v(a) > v(b); });
                        std::string top;
                        for (int t = 0; t < 5 && t < v.size(); ++t) {
                            const Eigen::Index c = idx[t];
                            top += fmt::format("{}{}={:.2f}", (t ? ", " : ""),
                                               (c < static_cast<Eigen::Index>(col_desc.size())
                                                && !col_desc[c].empty()) ? col_desc[c]
                                                                         : fmt::format("col{}", c),
                                               v(c));
                        }
                        log->warn("Network[estimability]:   ev={:.3e} dir spanned by [{}]", ev(j), top);
                    }
                }
            }
        }

        bool gate_ok = true;
        try {
            sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                                   kNullSpaceTol);
        } catch (const std::exception& ex) {
            // The reduced shared system carries MORE gauge freedoms than the
            // declared datum can pin — typically a satellite that is momentarily
            // unobservable (seen by a single station at marginal geometry), or —
            // under the prioritized datum — a phase-bias column whose anchor was
            // dropped / whose separation is weak.  For the prioritized case, try to
            // soft-anchor exactly those near-null phase-bias columns and RE-GATE
            // before withholding (the eigenVECTOR heal runs ONLY here, not every
            // epoch).  If the datum is still deficient after healing (or on the
            // legacy path), the product would be a meaningless least-norm split, so
            // WITHHOLD (the plan's "gate emission until convergence" safeguard).
            bool recovered = false;
            try {
                const int healed = healNearNullPhaseBias();   // 0 unless prioritized + near-null PB
                if (healed > 0) {
                    sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                           plan.declared_dimension, kNullSpaceTol);
                    recovered = true;
                    ++n_healed_epochs;
                    n_healed_cols += static_cast<size_t>(healed);
                    log->debug("Network: TOW {:.1f} soft-anchored {} near-null phase-bias "
                               "column(s) (dropped anchor / weak separation) — gate recovered.",
                               se.time.tow, healed);
                }
            } catch (const std::exception& ex2) {
                // The heal (eigensolve / addSharedPrior) or the re-gate reported a
                // deficiency it could not resolve — withhold rather than crash.
                log->warn("Network: TOW {:.1f} drop-and-heal failed ({}); withholding.",
                          se.time.tow, ex2.what());
            }
            if (!recovered) {
                gate_ok = false;
                ++n_gate_withheld;
                log->debug("Network: observability gate at TOW {:.1f} withheld product ({})",
                           se.time.tow, ex.what());
                emitEvent("GATE", "gate_withheld", "", "", std::nullopt, std::nullopt,
                          "skip_epoch", ex.what());
            }
        }
        if (!gate_ok) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",0,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("gate_withheld", es); }
            // RTA-10: product-level heartbeat for a WITHHELD epoch — an H line with NO
            // E line, so a live tail can tell "gate withheld" from "producer died".
            // Only once the deferred product writer has opened (the first emit): earlier
            // withheld epochs precede the product file's existence by construction.
            // σ̂0 is unavailable pre-solve (0 per the H grammar); withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldGate;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Two-state clock model warmup gate (mirrors carry_warm): seed/predict only after the
        // cold-start convergence horizon.  Its per-epoch predictions (for the post-solve update
        // + normalized-innovation test) are stashed here.
        const bool clk_model_warm = clk_model_on && (static_cast<int>(n_emitted) >= pl_warmup);
        // network.emit_clock_rate: same warmup gate as the clock model — skip the cold-start
        // epochs where the floated clock would finite-difference into a huge spurious C1 rate.
        const bool emit_rate_warm = emit_clock_rate_on && (static_cast<int>(n_emitted) >= pl_warmup);
        std::map<std::string, ClockOscPred> clk_pred;
        // B7 (P1-4): the carried clock-prior information folded onto each clock column this
        // epoch (0 = none) — either the scalar D2 carry (1/var_eff) or the two-state model
        // prior (1/p00).  The anchor-fraction diagnostic subtracts this from the reduced-normals
        // diagonal so it certifies DATA dominance, not "prior (carry) dominance" masquerading as
        // data.  Populated only when the diagnostic trace is open; inert otherwise.
        std::map<std::string, double> clk_prior_info;

        // --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level ------------------------------
        // The producer's estimable per-sat clock is Δδtˢ = (true sat clock) − (broadcast
        // a-priori clock of the version this epoch's geometry uses).  When that broadcast
        // version rolls (old IODE → new IODE) the a-priori jumps by
        // Δ_eph(t) = c·(brdc_new(t) − brdc_old(t)); the TRUE clock is continuous, so the
        // estimable Δδtˢ must jump by −Δ_eph to keep representing the same satellite.  Left
        // alone, the carried clock prior — the TIGHT two-state clock_osc prediction, or the
        // D2 q_clock carry — is a full Δ_eph out of frame and FIGHTS the new-frame
        // measurements: a slow transient that also trips the two-state NIS / clock-monitor
        // jump test as a spurious per-sat fault.  So we deterministically re-base the CARRIED
        // clock state by −Δ_eph, and the solve runs entirely in the new frame — emitted C0 is
        // continuous-in-reconstruction (the consumer forms brdc(new_iode)+C0, Card 7) with NO
        // discontinuity bump.
        //
        // ORDERING (deliberate deviation from the plan's "after the solve, before the C1
        // pass"): the re-base MUST precede the prior injection (below) and the solve.  The
        // plan's post-solve placement anchored to where Card 5 STAMPS clock_iode (after
        // toSolutionEpoch); but by then x_s was already solved against the OLD-frame prior, so
        // a post-hoc x_s shift is exact ONLY when the prior dominates (tight) and OVER-corrects
        // a data-dominated (loose) solve.  Shifting the PRIOR is unconditionally exact for any
        // prior weight AND — being a deterministic function of the broadcast ephemeris alone —
        // is independent of the solve, so it applies identically on every disposition (emit /
        // σ̂0-withhold / solve-fail).  The IODE this epoch uses is read straight from the
        // shared LiveEphemerisStore (Card 5's own equivalence note: toe granularity ≫
        // intra-epoch light-travel, so the emit-site query lands on the version the emission
        // path used), so no dependence on the post-solve stamp remains.
        //
        // Placed after the observability-gate `continue`: a gate-withheld epoch leaves the
        // carried clock state untouched (its post-emit update never runs), so deferring the
        // re-level to the next non-gate-withheld epoch keeps relevel_iode and the carried
        // state in lock-step.  On the σ̂0/solve-fail paths the block DID run, the shift landed,
        // and the (skipped) post-emit updates leave it in place — no double-shift either way.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            for (const std::string& sat : registered_clock_sats) {
                if (sat.empty() || sat[0] == 'R') continue;          // GLONASS out of scope (plan D8)
                const auto now = eph_store.computeSatelliteClock(sat, se.time);
                if (!now) continue;                                  // no eph held → nothing to track
                const int new_iode = now->iode;
                auto bit = relevel_iode.find(sat);
                if (bit == relevel_iode.end()) {                     // first sighting: seed, no shift
                    relevel_iode[sat] = new_iode;
                    continue;
                }
                if (bit->second == new_iode) continue;               // no roll — the common case
                const int old_iode = bit->second;
                const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
                relevel_iode[sat] = new_iode;                        // baseline advances regardless
                if (shift) {
                    // Re-base the carried clock state by −Δ_eph.  Offset / level term ONLY —
                    // the drift (c1) and the 2×2 covariance are frame-invariant and untouched.
                    const double d = *shift;
                    if (auto oit = clock_osc.find(sat); oit != clock_osc.end() && oit->second.init)
                        oit->second.x0 -= d;                          // two-state offset (clock_model)
                    for (CarryPrior& cp : prev_carry)                 // D2 scalar clock carry (model off)
                        if (cp.kind == SharedParamKind::Clock && cp.sat == sat) cp.value -= d;
                    if (auto jit = prev_clk.find(sat); jit != prev_clk.end())
                        jit->second.first -= d;                       // clock-monitor jump baseline
                    // Card 1: the coupling stash references this clock via ŝ_prev terms —
                    // re-level them into the new frame too, else every coupling row's RHS
                    // is off by M·Δ_eph after a cutover (the row's functional u = r + Mᵀs
                    // is frame-consistent only if ALL carried references move together).
                    if (carry_cpl_cond) {
                        for (auto& [lk, lcv] : local_carry)
                            for (CoupledShared& cs2 : lcv.coupling)
                                if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                                    cs2.s_prev -= d;
                        for (auto& [zk, zcv] : zwd_carry)
                            for (CoupledShared& cs2 : zcv.coupling)
                                if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                                    cs2.s_prev -= d;
                    }
                    log->info("Network (Card 6): {} eph cutover IODE {}->{} — re-leveled carried "
                              "clock by {:.4f} m (−Δ_eph); no discontinuity bump.",
                              sat, old_iode, new_iode, -d);
                    emitEvent("RELEVEL", "eph_cutover_relevel", sat, "", -d, std::nullopt,
                              "relevel", fmt::format("iode={}->{}", old_iode, new_iode));
                } else {
                    // Prior version no longer retained → Δ_eph incomputable → the clock stepped
                    // by an unknown amount.  Force a per-sat discontinuity (consumer resets this
                    // sat's network-corrected ambiguity across the step); it lands at the sat's
                    // next actual emit (below).  Effectively unreachable with per-epoch checking.
                    eph_cutover_disc_pending.insert(sat);
                    log->warn("Network (Card 6): {} eph cutover IODE {}->{} but the prior version "
                              "is no longer held — forcing a per-sat discontinuity (Δ_eph "
                              "incomputable).", sat, old_iode, new_iode);
                    emitEvent("RELEVEL", "eph_cutover_disc", sat, "", std::nullopt, std::nullopt,
                              "disc", fmt::format("iode={}->{}", old_iode, new_iode));
                }
            }
        }

        // --- D2 carry-forward prior (diagonal, inflated over the gap) -------
        // Applied AFTER the gate (which must see the DATA-only gauge structure)
        // and BEFORE the solve.  The prior anchors each non-datum shared
        // parameter to its previous-epoch estimate; it is skipped on a pivot
        // handover, when the carried values belong to a now-retired datum.
        // RTA-2 (network.scoped_handover_response): only the CHANGED
        // constellation(s)' datum retired — a per-constellation clock gauge is
        // independent of its siblings, so carries of unaffected constellations
        // remain valid and fold normally; the per-key skip below drops exactly
        // the retired-datum keys.  OFF reproduces the legacy all-or-nothing
        // skip byte-identically (the outer gate collapses to !datum_changed and
        // the per-key skip can never trigger inside it).
        if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on) {
            std::vector<NetworkEstimator::SharedPrior> priors;
            priors.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
                // Fix A hold: the held phase-bias prior is injected by its own block
                // BELOW, unconditionally — the phase-ensemble datum is orthogonal to
                // the CLOCK pivot handover this clock-frame carry is gated on, so a
                // clock datum change must NOT drop it (else a held sat, whose Σz=0 fold
                // buildEnsembleGauges skipped, is left un-datumed and the epoch withholds).
                if (phase_hold_on && cp.kind == SharedParamKind::PhaseBias) continue;
                if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
                // Card 3 (eq34) D4 hygiene: a re-referenced (sat,σ) stepped its b_s
                // frame at this epoch's election — marginalize its carried PB prior
                // for ONE epoch (it belongs to the retired frame; folding it yanks
                // the new frame by up to the full re-level).  Mirrors the RTA-2
                // scoped skip; always empty when eq34 is off.
                if (cp.kind == SharedParamKind::PhaseBias
                    && eq34_reref.count({cp.sat, cp.signal})) continue;
                if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
                const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                               : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                        : q_pb;
                // A clock-monitor-flagged satellite folds with the large REINIT variance (a soft
                // large-q reinitialization): the weak prior still anchors the parameter at its
                // previous value — curbing the collective datum drift a hard refloat causes — but
                // is loose enough for the data to walk a frozen clock back toward truth.
                double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                              : cp.var + q * std::max(0.0, dt);
                // B4: floor the D2 phase-bias / code-bias carry variance (caps Λ so a stiff
                // zero-q carry cannot lock a biased product in), mirroring the ambiguity-carry
                // floor.  Only on the non-refloat path (a clk-monitor refloat is deliberately
                // loose); clock is out of scope (own two-state model / monitor).  0 = off.
                if (!cp.inflate_q) {
                    const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                       : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                                 : 0.0;
                    if (floor > 0.0) var_eff = std::max(var_eff, floor);
                }
                if (!(var_eff > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
                p.value = cp.value; p.info = 1.0 / var_eff;
                if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                    clk_prior_info[cp.sat] = p.info;   // B7: scalar clock carry info on this column
                priors.push_back(p);
            }
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), priors.begin(), priors.end());
        } else if ((!datum_changed || scoped_handover) && !prev_carry.empty()) {
            // C1 (network.shared_carry_full_cov): fold the carried block as ONE dense
            // information prior Λ = (P_prev + Q)⁻¹ — the information-form KF time
            // update on the shared border, recovering the cross-covariances (most
            // critically clock↔phase-bias) the diagonal stash above discards.  Entry
            // selection, per-key skip rules, and the per-key var_eff surgery
            // (inflate_q reinit, q·dt inflation, B4 floors) are IDENTICAL to the
            // diagonal branch; the only new content is the retained off-diagonals.
            // Keys with no matrix row (first fold after a warm resume, or after a
            // stash-side non-finite screen) fold through the legacy diagonal path —
            // the mode degrades per-key, it never breaks.  Keys skipped by rule are
            // MARGINALIZED (their rows/cols of P are simply not selected), which is
            // the exact treatment for a departed/retired parameter.
            std::vector<NetworkEstimator::SharedPrior> dkeys;     // dense-eligible set
            std::vector<std::size_t> drow;                        // row in prev_carry_cov.P
            std::vector<double> dvar_eff;                         // per-key var_eff (diag + fallback)
            std::vector<char> dinflate;                           // inflate_q per dense key
            std::vector<NetworkEstimator::SharedPrior> leftovers; // no matrix row → diagonal fold
            dkeys.reserve(prev_carry.size());
            drow.reserve(prev_carry.size());
            dvar_eff.reserve(prev_carry.size());
            dinflate.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
                if (phase_hold_on && cp.kind == SharedParamKind::PhaseBias) continue;  // Fix A hold: injected separately (datum-change-independent)
                if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
                // Card 3 (eq34) D4 hygiene: marginalize re-referenced PB keys for
                // this epoch (retired frame) — see the diagonal branch above.
                if (cp.kind == SharedParamKind::PhaseBias
                    && eq34_reref.count({cp.sat, cp.signal})) continue;
                if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
                const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                               : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                        : q_pb;
                double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                              : cp.var + q * std::max(0.0, dt);
                if (!cp.inflate_q) {
                    const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                       : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                                 : 0.0;
                    if (floor > 0.0) var_eff = std::max(var_eff, floor);
                }
                if (!(var_eff > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
                p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
                const auto rit = prev_carry_cov.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                if (rit == prev_carry_cov.row.end()) {
                    if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                        clk_prior_info[cp.sat] = p.info;   // B7: diagonal-carry info (no dense row)
                    leftovers.push_back(p);
                    continue;
                }
                dkeys.push_back(p);
                drow.push_back(rit->second);
                dvar_eff.push_back(var_eff);
                dinflate.push_back(cp.inflate_q ? 1 : 0);
            }
            const std::size_t kd = dkeys.size();
            if (kd > 0) {
                // P_eff over the dense set: subset of the stashed posterior covariance;
                // diagonal := var_eff (the stashed diagonal equals cp.var by
                // construction, so this IS the legacy q·dt inflation + B4 floor); a
                // clock-monitor-flagged key is DECORRELATED before its diagonal becomes
                // the reinit variance (its old cross-covariances are stale once the
                // variance is replaced — and a swapped diagonal under live off-diagonals
                // could make P_eff indefinite).
                Eigen::MatrixXd cov_eff(kd, kd);
                for (std::size_t a = 0; a < kd; ++a)
                    for (std::size_t b = 0; b < kd; ++b)
                        cov_eff(a, b) = prev_carry_cov.P(drow[a], drow[b]);
                for (std::size_t a = 0; a < kd; ++a) {
                    if (dinflate[a]) {
                        cov_eff.row(static_cast<Eigen::Index>(a)).setZero();
                        cov_eff.col(static_cast<Eigen::Index>(a)).setZero();
                    }
                    cov_eff(a, a) = dvar_eff[a];
                }
                try {
                    Eigen::VectorXd lam;
                    est.addSharedPriorDense(dkeys, cov_eff,
                                            anchor_info_trace.is_open() ? &lam : nullptr);
                    if (anchor_info_trace.is_open())
                        for (std::size_t a = 0; a < kd; ++a)
                            if (dkeys[a].kind == SharedParamKind::Clock)
                                clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // B7: folded Λ(c,c)
                    if (within_epoch_armed) {
                        cap_dense_keys = dkeys;      // replayed in resolve_dropping
                        cap_dense_cov  = cov_eff;
                    }
                } catch (const std::exception& e) {
                    // Numerical fallback: fold the SAME keys as independent diagonal
                    // priors with 1/var_eff — exactly the legacy semantics for this
                    // epoch.  Self-healing: the next emit re-stashes a fresh block.
                    log->warn("Network (C1): dense carried-covariance fold failed ({}) — "
                              "falling back to the diagonal carry for this epoch.",
                              e.what());
                    emitEvent("D2_DENSE", "fallback_diagonal", "", "",
                              static_cast<double>(kd), std::nullopt, "fallback",
                              e.what());   // non-PD / non-finite / conditioning budget — emitEvent sanitizes commas
                    for (std::size_t a = 0; a < kd; ++a) {
                        if (anchor_info_trace.is_open() &&
                            dkeys[a].kind == SharedParamKind::Clock)
                            clk_prior_info[dkeys[a].sat] = dkeys[a].info;
                        leftovers.push_back(dkeys[a]);
                    }
                }
            }
            est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), leftovers.begin(), leftovers.end());
        }

        // (Fix A hold: the held phase-bias datum is injected PRE-gate above, next to
        // the SH-VTEC prior — it is a datum, so the gate must see it.)

        // --- Two-state oscillator clock carry-forward prior (network.clock_model) ------------
        // The satellite clock's prior comes from the physical clock model: predict [offset,drift]
        // forward and fold the offset (mean = drift-following prediction, info = 1/predicted-var).
        // Because the prediction is tight (~cm), the post-solve innovation exposes a lock-in that
        // the scalar random walk would freeze.  Skipped on a pivot handover (retired datum) and
        // for the first warm epoch of each satellite (no prediction until it is seeded).
        // RTA-2 (network.scoped_handover_response): a handover retires only the changed
        // constellation(s)' datum, so under scoping their satellites alone skip the prediction
        // (they re-seed from this epoch's new-datum solve in the post-solve update); unaffected
        // constellations' oscillators predict normally.  OFF is byte-identical (outer gate
        // collapses to !datum_changed; the per-sat skip can never trigger inside it).
        if (clk_model_warm && (!datum_changed || scoped_handover)) {
            std::vector<NetworkEstimator::SharedPrior> clkp;
            clkp.reserve(clock_osc.size());
            for (const std::string& sat : registered_clock_sats) {
                if (handover_affects(sat)) continue;  // RTA-2: retired-datum sat (scoped mode only)
                auto it = clock_osc.find(sat);
                if (it == clock_osc.end() || !it->second.init) continue;
                // P0-3: predict over THIS satellite's true elapsed time, not the global
                // epoch dt.  A satellite absent for several epochs (eclipse exclusion is
                // ON in the clock-model config) would otherwise get an hours-old state
                // predicted over ONE dt of Q — a falsely-tight prior.  Past a gap bound
                // its carried state is stale: drop the prior entirely (reseed) so the
                // clock re-acquires from data + the a-priori anchor rather than trusting
                // an ancient value.  (Within the bound the Q·dt inflation over the true
                // gap naturally de-weights the prior — the honest, self-limiting behavior.)
                const double sat_dt = (it->second.t_last > 0.0)
                                          ? (pl_now_s - it->second.t_last) : std::max(0.0, dt);
                if (!(sat_dt >= 0.0) || sat_dt > clk_model_max_gap) {
                    // T5 (V2 blind spot): the stale-carry reseed is otherwise SILENT.  Naturally
                    // edge-triggered — the update reseeds t_last, so next epoch is fresh.
                    emitEvent("T5_clock_model", "stale_prior_reseed", sat, "", sat_dt,
                              clk_model_max_gap, "reseed", "carry past gap");
                    continue;   // R11: stale → no prior (reseed)
                }
                const ClockOscPred pr = clockOscPredict(it->second, sat_dt,
                                                        clk_model_q1, clk_model_q2);
                if (!(pr.p00 > 0.0) || !std::isfinite(pr.x0)) continue;
                clk_pred[sat] = pr;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::Clock; p.sat = sat; p.signal = "";
                p.value = pr.x0; p.info = 1.0 / pr.p00;
                if (anchor_info_trace.is_open()) clk_prior_info[sat] = p.info;  // B7: two-state clock-model prior info
                clkp.push_back(p);
            }
            est.addSharedPrior(clkp);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), clkp.begin(), clkp.end());
        }

        // --- A-priori satellite-clock anchor (cold-start conditioning) ------
        // The shared clock column is Δδtˢ relative to the a-priori PRECISE product
        // clock, so it is genuinely small (sub-metre for precise post-processing).
        // With no absolute anchor the per-epoch joint solve floats it by hundreds
        // of metres at cold start to absorb the clock↔iono↔bias confusion (no
        // carry-forward exists yet at epoch 1, and epoch 2+ then anchors to that
        // floated value).  A Tikhonov prior pulling each Δδtˢ toward 0 with
        // σ = initial_std.sat_clock (the documented "Δδtˢ vs a-priori product clock"
        // quality: sub-m precise, looser for RT broadcast) regularizes ONLY the
        // ill-conditioned cold-start direction: the per-constellation clock DATUM
        // stays fixed by the explicit Datum (a large-weight pin that dominates this
        // soft prior), so the few-metre datum offset the data determines survives.
        // Applied POST-gate (like the carry-forward) so the null-space rank gate
        // still sees the clock datum as a gauge, and it combines in information space
        // with the carry-forward (anchor-toward-0 + RW-toward-previous).  Gated to a
        // BINDING std (< 100 m): the legacy loose default (1000 m) leaves the anchor
        // off, so every existing network config is byte-identical.
        //
        // SCOPE (measured, 305-sta GPS+Gal, 2026-06-30; see [[project_global_sh_iono]]):
        // σ=0.05 m collapses cold-start clock-Δ rms 56→2.9 m for ~6 epochs, then the
        // W6 clock↔bias↔ambiguity systematic (data wants ~80 m, info ≫ the anchor)
        // reasserts.  This improves cold-start CLOCK-PRODUCT quality only — the
        // co-estimated SH-VTEC field divergence is a SEPARATE, DECOUPLED cold-start
        // estimability defect (its large-scale level is unobservable under the
        // common-mode-free iono R); pinning the clock does NOT move the field.
        {
            const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
            // Broadcast-anchored datum: the SAME per-satellite anchors were already
            // folded PRE-gate as the gauge realization (buildSharedDatum constraint
            // rows) — folding them again here would double the anchor information
            // (effective σ/√2).  Skip; every other mode keeps the post-gate
            // regularizer exactly as before.
            if (!clock_brdc_anchored
                && std::isfinite(clk_sigma) && clk_sigma > 0.0 && clk_sigma < 100.0) {
                const double clk_info = 1.0 / (clk_sigma * clk_sigma);
                std::vector<NetworkEstimator::SharedPrior> clk_anchor;
                clk_anchor.reserve(registered_clock_sats.size());
                for (const std::string& sat : registered_clock_sats) {
                    NetworkEstimator::SharedPrior p;
                    p.kind  = SharedParamKind::Clock;
                    p.sat   = sat;
                    p.value = 0.0;          // anchor Δδtˢ toward the a-priori clock
                    p.info  = clk_info;
                    clk_anchor.push_back(p);
                }
                est.addSharedPrior(clk_anchor);
                if (within_epoch_armed)
                    cap_priors.insert(cap_priors.end(), clk_anchor.begin(), clk_anchor.end());
            }
        }

        // A clock-pivot handover re-references the whole shared solution; the
        // forced bump steps BOTH phase and code discontinuity counters next
        // emit (the code per-(const,signal) sum-to-zero and IF-pin re-references
        // are detected by the emitter from the emitted epoch itself — see
        // NetworkProductEmitter; only this clock-pivot signal is driver-known).
        // Both the emitter datum bump (markDatumChange) AND the prev_pivot /
        // last_pivot baseline advance are DEFERRED to the emit site: the maps
        // must describe the datum of the last EMITTED product (the consumer's
        // view).  Advancing them here — before the solve — would make a handover
        // epoch that is subsequently skipped (solve failure, σ̂0 withhold)
        // compare equal next epoch: the discontinuity signal would be lost
        // forever AND the old-datum prev_carry/clock_osc would fold as priors
        // into the new-datum solve.  With the baseline frozen across skipped
        // epochs, every skip path re-detects the handover until one epoch
        // actually emits in the new datum.

        Eigen::VectorXd x_s;
        try {
            // parallel_carry: the WHOLE est.solve() is Eigen-threaded work — foldReducedDdPriors
            // fans its per-station reduction across threads itself (Eigen PINNED), and its dense
            // GEMMs + the K×K middle inverse run under Eigen's own threading.  So, unlike the two
            // isolated build/install GEMM stages, solve() is NOT wrapped in a CarryEigenScope: an
            // un-pin here would nest the fold's OpenMP with Eigen's GEMM threads.  It is also why
            // parallel_carry cannot coexist with parallel_reduce (which would OpenMP-fan this same
            // Eigen-threaded solve across stations) — the loader keeps them mutually exclusive.
            x_s = est.solve(plan.shared_datum);
            // Card 1d debug: full-precision solve checksum for A/B-arm differencing
            // (print-precision-proof).  One-shot budget; log-only.
            if (carry_cpl_cond && n_cpl_solve_dump > 0) {
                --n_cpl_solve_dump;
                log->info("Network (Card 1d dbg): solve checksum TOW {:.1f}: sum={:.17e} "
                          "norm={:.17e} n={}", se.time.tow, x_s.sum(), x_s.norm(), x_s.size());
            }
        } catch (const std::exception& ex) {
            log->warn("Network: solve failed at TOW {:.1f} ({}); skipping epoch",
                      se.time.tow, ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("solve_failed", es); }   // x_s empty — no clock stats
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Satellites whose carried SHARED state (clock + coupled phase/code bias) is to be
        // REFLOATED at the end-of-epoch stash: the per-satellite clock monitor (below, the
        // detector for a lone lock-in the σ̂0 RAIM misses) plus — only when
        // clock_monitor_couple_raim is opted in — every satellite the within-epoch / post-fit
        // RAIM already flags (so a slip that leaked into the shared clock reinitializes the
        // clock too, not just the ambiguity).  Empty unless network.clock_monitor, so the stash
        // stays byte-identical when off.
        std::set<std::string> clock_refloat_sats;
        int n_arc_refloat_epoch = 0;   // T2d: isolated-arc RAIM refloats executed this epoch (heartbeat metric)
        // B1 retro-bump / URA-inflate feed (P0-2): the union of satellites ANY QC tier
        // flags this epoch, independent of clock_monitor_couple_raim.  clock_refloat_sats
        // only captures the RAIM tiers under the couple opt-in (it drives the CARRY
        // refloat, a numeric action), so a separate accumulator is needed so the
        // retro-bump reaches every tier without forcing a carry refloat.  Populated only
        // when a URA feature that consumes it is on; inert (byte-identical) otherwise.
        std::set<std::string> retro_flag_sats;
        const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;

        // --- Within-epoch adaptation: drop the dominant culprit(s) and re-solve --------
        // When σ̂0 (the robust a-posteriori variance factor) shows the joint solve is
        // contaminated, peel the single most-prominent culprit — a whole SATELLITE (its
        // rows de-weighted network-wide) or a whole STATION (excluded) — re-solve THIS epoch
        // over a fresh beginEpoch() replay of the captured shared system, and repeat until
        // σ̂0 falls below the trigger or a cap.  Only the (rare, already-contaminated) trigger
        // epochs pay the re-solve; try/catch reverts to the last good solve on any failure.
        // Uses the same robust median-polish attribution as the RAIM refloat.
        int  n_adapt = 0;
        bool we_adapt_attempted = false;   // a within-epoch re-solve was launched (est layout was rebuilt)
        double sigma0_adapt = 0.0;         // A4: final within-epoch σ̂0 (0 = adaptation did not run this epoch)
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION partition of the LAST
        // assess()'s standardized-residual pool — the very rows whose median is the global
        // σ̂0 the A4 withhold compares, keyed by the ambiguity's satellite constellation
        // (pa.sat[0]).  Cleared and refilled on every assess() so, like sigma0_adapt, it
        // describes the FINAL within-epoch solve.  Populated only when the carve-out is
        // armed (empty otherwise — no cost, no behavior change on the default path).
        std::map<char, std::vector<double>> we_sigma0_pool_c;
        // R12: the B8/T2 flag-rate (first-solve σ̂0 > trigger) is DECIDED here but COMMITTED at
        // the emit site, so a subsequently-withheld epoch is excluded from T2's denominator —
        // matching T3–T6, which only accrue on emitted epochs.  Without this, T2 counted withheld
        // (contaminated) epochs while the post-fit tiers did not, giving incomparable alphas.
        bool we_t2_opp = false, we_t2_flag = false;
        if (within_epoch_armed && carry_warm) {
            const double trig      = config_.networkRaimSigma0Trigger();
            const bool   surgical  = config_.networkRaimSurgicalStation();
            const double sta_bias  = surgical ? config_.networkRaimStationDropBias() : 1.0;
            constexpr int    kMaxAdapt   = 5;

            std::vector<char> orig_added(stations.size(), 0);
            for (size_t i = 0; i < stations.size(); ++i)
                orig_added[i] = (est_index_of[i] >= 0) ? 1 : 0;

            // Culprit sets.  A bad AMBIGUITY (cycle slip — the dominant contamination the
            // carried-phase RAIM sees) needs REINITIALIZATION, not measurement de-weighting: the
            // re-solve REFLOATS the culprit's ambiguities (withholds their stale carry prior) so
            // they re-estimate from full-weight data and absorb the slip locally.  A whole bad
            // STATION (a data-quality problem, not a slip) is EXCLUDED — the de-weight/exclude
            // tool, which is the right one for a bad measurement.
            std::set<std::string> drop_sats;                       // refloat that sat's ambiguities network-wide
            std::set<int>         drop_stations;                   // excluded entirely (bad-data case)
            std::map<int, std::set<std::string>> drop_arcs;        // (station → sats) refloat at that station ONLY
            // C3c §3: the DD-carry re-solve (buildCarryDdPriors, via resolve_dropping)
            // treats a mid-epoch RAIM culprit as slipped, so its stale carry is not
            // re-folded (refold from unchanged predicted state, H13 + culprit-withhold).
            // Aliases live only while this block runs; nulled before the sets leave scope.
            carry_we_drop_sats = &drop_sats;
            carry_we_drop_arcs = &drop_arcs;

            // σ̂0 + median-polish effects of the CURRENT shared solution.  The (station × sat)
            // residual table (carried-ambiguity PHASE rows only) decomposes additively into a
            // per-satellite column effect, a per-station row effect, and an isolated-arc
            // interaction.  Computed over EVERY present ambiguity — a refloated ambiguity fits
            // its own data (its residual collapses after the re-solve), so σ̂0 stays an HONEST
            // measure of the REMAINING contamination without special-casing; the culprit is kept
            // out of the next pick by the candidate-set exclusion, not by hiding it here.
            auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,
                              std::map<std::string, double>& sat_eff,
                              std::map<int, double>& sta_eff,
                              std::map<std::pair<int, std::string>, double>& arc_eff) {
                std::vector<double> r_pool;
                if (perconst_on) we_sigma0_pool_c.clear();   // RTA-10: re-partition per assess
                std::map<std::pair<int, std::string>, double> cell_max;
                for (size_t i = 0; i < stations.size(); ++i) {
                    // Prioritized: keep the former pivot in the σ̂0 pool — it now
                    // carries fault-testable non-anchored arcs; its anchored (pinned)
                    // keys are already absent from R.pl_amb (per-(sat,signal) partition).
                    if (!orig_added[i] || est_index_of[i] < 0
                        || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                    const StationAssembly& R = asm_res[i];
                    if (R.pl_amb.empty() || R.A_s.cols() != xs.size()) continue;
                    Eigen::VectorXd x_r;
                    try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), xs); }
                    catch (...) { continue; }
                    if (x_r.size() != R.A_r.cols()) continue;
                    const Eigen::VectorXd resid = R.y - R.A_s * xs - R.A_r * x_r;
                    Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
                    if (raim_redun) {
                        try {
                            row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                                      R.A_s, R.A_r, R.w, plan.shared_datum);
                        } catch (...) { row_r.resize(0); }
                    }
                    for (const auto& pa : R.pl_amb) {
                        if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                        double r_max = 0.0;
                        for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                            if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;
                            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                            double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                            if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                                constexpr double kRedFloor = 1e-6;
                                const double ri = row_r(row);
                                if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                                r /= std::sqrt(ri);
                            }
                            r_pool.push_back(r);
                            // RTA-10: the SAME standardized row (weighting and B2 √r_i
                            // normalization included; redundancy-floored rows excluded by
                            // the continue above), partitioned by constellation.
                            if (perconst_on && !pa.sat.empty())
                                we_sigma0_pool_c[pa.sat[0]].push_back(r);
                            if (r > r_max) r_max = r;
                        }
                        const auto key = std::make_pair(static_cast<int>(i), pa.sat);
                        auto it = cell_max.find(key);
                        if (it == cell_max.end() || r_max > it->second) cell_max[key] = r_max;
                    }
                }
                sigma0_out = 1.0;
                if (!r_pool.empty()) {
                    std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
                    sigma0_out = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
                }
                std::vector<std::tuple<int, std::string, double>> tbl;
                tbl.reserve(cell_max.size());
                for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
                const MedianPolish mp = medianPolish2Way(tbl);
                sat_eff = mp.col_eff;
                sta_eff = mp.row_eff;
                arc_eff.clear();
                for (std::size_t k = 0; k < tbl.size(); ++k)
                    arc_eff[std::make_pair(std::get<0>(tbl[k]), std::get<1>(tbl[k]))] = mp.interaction[k];
            };

            // Replay the epoch over a fresh arrowhead (beginEpoch reopens the frozen layout;
            // only reset() would clear it), REFLOATING the culprit ambiguities.  Rebuilds
            // est_index_of so reconstructStation / harvest stay valid on the final solve.  The
            // measurements stay at FULL weight — de-weighting is the tool for a bad measurement,
            // whereas a slipped ambiguity needs a new VALUE.  A satellite culprit refloats its
            // ambiguities at every station; an arc culprit at one station only; a whole-station
            // culprit is excluded (its data, not a single ambiguity, is bad).  A refloated
            // ambiguity keeps its full-weight data (so its local block stays positive-definite —
            // no drop-induced singularity) but WITHHOLDS its carry prior, so the re-solve lets it
            // find its new value and absorb the slip locally; the harvest then re-stashes that
            // corrected value (self-contained reinit — no reliance on the post-fit refloat).
            auto resolve_dropping = [&]() -> Eigen::VectorXd {
                est.beginEpoch();
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!orig_added[i] || drop_stations.count(static_cast<int>(i))) {
                        est_index_of[i] = -1;
                        continue;
                    }
                    const StationAssembly& R = asm_res[i];
                    // A_r columns of THIS station's ambiguities to refloat (withhold their prior).
                    // A SATELLITE culprit refloats its ambiguities at every station tracking it: a
                    // satellite-level contamination pulls the shared clock at ALL its stations, and
                    // its arcs are contaminated to varying degrees — a strict per-arc gate on the
                    // (inflated) σ̂0 would miss the moderately-slipped ones and leave a worse
                    // mixed state, so the whole satellite is reinitialized consistently.  An ARC
                    // culprit refloats at ONE station only (surgical, isolated slip).
                    std::set<Eigen::Index> refloat_cols;
                    const auto dai = drop_arcs.find(static_cast<int>(i));
                    for (const auto& pa : R.pl_amb)
                        if (drop_sats.count(pa.sat) ||
                            (dai != drop_arcs.end() && dai->second.count(pa.sat)))
                            refloat_cols.insert(pa.col);
                    // Card 1: a refloated ambiguity's carry is a coupling ROW inside A_s/A_r
                    // (not a carry_cols diagonal prior), so withholding it means ZEROING that
                    // row before re-adding the block — the coupling-row analogue of the
                    // diagonal-path prior withhold below.  The refloated ambiguity keeps its
                    // full-weight DATA rows, so its local block stays PD.  A copy so the
                    // primary R (and any iterated replay) is untouched.
                    if (carry_cpl_cond && carry_cpl_joint && !R.cpl_raw.empty()
                        && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                        // Card 1b: the joint whitening MIXES a station's coupling rows, so a
                        // refloat cannot zero individual output rows — re-whiten the SURVIVING
                        // rows (dropping the refloated locals) and re-append them after the
                        // unchanged data rows.  A re-whiten failure drops all coupling rows for
                        // the epoch (safe degrade: the refloated arcs re-float from data anyway).
                        Eigen::MatrixXd Hr, Hs;
                        Eigen::VectorXd z;
                        const bool ok = whitenCouplingRows(R.cpl_raw, R.cpl_white_pw, refloat_cols,
                                                           R.A_r.cols(), R.A_s.cols(), Hr, Hs, z);
                        const Eigen::Index nd = R.cpl_row_start;             // data + SH rows (unchanged)
                        const Eigen::Index nk = ok ? Hr.rows() : 0;          // surviving coupling rows
                        Eigen::MatrixXd As(nd + nk, R.A_s.cols()), Ar(nd + nk, R.A_r.cols());
                        Eigen::VectorXd rw(nd + nk), ry(nd + nk);
                        As.topRows(nd) = R.A_s.topRows(nd);
                        Ar.topRows(nd) = R.A_r.topRows(nd);
                        rw.head(nd)    = R.w.head(nd);
                        ry.head(nd)    = R.y.head(nd);
                        if (nk > 0) {
                            As.bottomRows(nk) = Hs;
                            Ar.bottomRows(nk) = Hr;
                            rw.tail(nk).setOnes();
                            ry.tail(nk) = z;
                        }
                        est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
                    } else if (carry_cpl_cond && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                        Eigen::MatrixXd As = R.A_s, Ar = R.A_r;
                        Eigen::VectorXd rw = R.w, ry = R.y;
                        for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                            if (refloat_cols.count(R.cpl_row_local_cols[rr])) {
                                const Eigen::Index row =
                                    R.cpl_row_start + static_cast<Eigen::Index>(rr);
                                As.row(row).setZero(); Ar.row(row).setZero();
                                rw(row) = 0.0; ry(row) = 0.0;
                            }
                        est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
                    } else {
                        est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
                    }
                    // B9: re-inject the pivot's ZWD carry too (its R.carry_cols is empty
                    // unless network.carry_zwd), so the within-epoch re-solve matches the
                    // primary solve.  refloat_cols only ever names ambiguity columns, so a
                    // pivot's lone ZWD column is never in it (empty → the fast path).
                    if (carry_warm && !R.carry_cols.empty()) {
                        if (refloat_cols.empty()) {
                            est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                R.carry_cols, R.carry_dNrr, R.carry_dur);
                        } else {
                            // Inject the carry prior for every carried ambiguity EXCEPT the
                            // refloated ones (those are left free → re-estimated from data).
                            std::vector<Eigen::Index> cc;
                            Eigen::VectorXd cdn(R.carry_cols.size()), cdu(R.carry_cols.size());
                            cc.reserve(R.carry_cols.size());
                            Eigen::Index m = 0;
                            for (std::size_t k = 0; k < R.carry_cols.size(); ++k)
                                if (!refloat_cols.count(R.carry_cols[k])) {
                                    cc.push_back(R.carry_cols[k]);
                                    cdn(m) = R.carry_dNrr(static_cast<Eigen::Index>(k));
                                    cdu(m) = R.carry_dur(static_cast<Eigen::Index>(k));
                                    ++m;
                                }
                            if (!cc.empty()) {
                                cdn.conservativeResize(m);
                                cdu.conservativeResize(m);
                                est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                    cc, cdn, cdu);
                            }
                        }
                    }
                }
                if (cap_has_constraint) est.addSharedConstraintRows(cap_H, cap_z, cap_constraint_weight);
                // Re-apply the sum-to-zero clock gauge on the fresh arrowhead (plan is
                // in scope; its rows are layout-frozen for the epoch so they match the
                // primary solve).  No-op / byte-identical in PIN mode (0 rows).
                if (plan.clock_constraint_H.rows() > 0)
                    est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                                plan.clock_constraint_weight);
                if (!cap_priors.empty()) est.addSharedPrior(cap_priors);
                // C1: replay the dense D2 fold on the fresh arrowhead.  The layout is
                // frozen for the epoch, so the key→column mapping and the LDLT are
                // bit-reproducible — the re-solve sees the identical shared system.
                // Non-empty only when the primary dense fold succeeded (its fallback
                // routed through cap_priors above), so this cannot throw where the
                // primary did not.
                if (!cap_dense_keys.empty())
                    est.addSharedPriorDense(cap_dense_keys, cap_dense_cov);
                // Fix A: rebuild the ensemble phase-bias gauges on the fresh arrowhead
                // (beginEpoch reopened the layout but est_index_of was rebuilt above and
                // refloated stations may have dropped, so the stored gauges' station
                // indices are stale).  No-op when OFF.
                buildEnsembleGauges();
                // Card 4 (M1) C3b: rebuild the DD-prior blocks on the fresh arrowhead
                // (beginEpoch cleared them + est_index_of was rebuilt above).  The
                // predict is NOT re-run — only the fold is rebuilt.  No-op when OFF.
                buildCarryDdPriors();
                return est.solve(plan.shared_datum);
            };

            double sigma0 = 1.0;
            std::map<std::string, double> sat_eff;
            std::map<int, double> sta_eff;
            std::map<std::pair<int, std::string>, double> arc_eff;
            assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            const double sigma0_first = sigma0;
            if (trace_flag_rates) { we_t2_opp = true; we_t2_flag = (sigma0_first > trig); }  // B8/T2 (R12: committed at emit, below)
            std::string adapt_detail;
            for (int iter = 0; iter < kMaxAdapt && sigma0 > trig; ++iter) {
                // Best NEW culprit per granularity (all gated on effect/σ̂0 > raim_k).  An
                // isolated ARC is the finest, least-destructive peel; a whole STATION is the
                // coarsest (it discards good arcs too) and is de-rated by sta_bias so it is
                // only chosen when it dominates the best satellite/arc by that margin.
                // Preference on equal score is ARC > SAT > STA (surgical-first tie-break).
                std::string best_arc_sat; int best_arc_sta = -1; double best_arc_eff = 0.0;
                if (surgical)
                    for (const auto& [key, eff] : arc_eff) {
                        const int s = key.first; const std::string& sat = key.second;
                        if (drop_stations.count(s) || drop_sats.count(sat)) continue;
                        const auto it = drop_arcs.find(s);
                        if (it != drop_arcs.end() && it->second.count(sat)) continue;
                        if (eff > best_arc_eff) { best_arc_eff = eff; best_arc_sta = s; best_arc_sat = sat; }
                    }
                std::string best_sat; double best_sat_eff = 0.0;
                for (const auto& [sat, eff] : sat_eff)
                    if (!drop_sats.count(sat) && eff > best_sat_eff) { best_sat_eff = eff; best_sat = sat; }
                int best_sta = -1; double best_sta_eff = 0.0;
                for (const auto& [sta, eff] : sta_eff)
                    if (!drop_stations.count(sta) && eff > best_sta_eff) { best_sta_eff = eff; best_sta = sta; }

                // Consensus escalation: a satellite with ≥ raim_min_sta arcs currently over the
                // gate is a NETWORK-WIDE problem (its shared clock/phase-bias is corrupted), not a
                // stack of isolated arcs — reinitialize the whole satellite in ONE iteration (one
                // re-solve) rather than peeling arc by arc.  When the best arc belongs to such a
                // satellite, redirect to the satellite branch.  Reuses the RAIM cross-station
                // consensus count (§ post-fit RAIM).
                if (surgical && best_arc_sta >= 0 && !drop_sats.count(best_arc_sat)) {
                    int n_bad = 0;
                    for (const auto& [key, eff] : arc_eff) {
                        if (key.second != best_arc_sat || drop_stations.count(key.first)) continue;
                        const auto it = drop_arcs.find(key.first);
                        if (it != drop_arcs.end() && it->second.count(key.second)) continue;
                        if (eff / sigma0 > raim_k) ++n_bad;
                    }
                    if (n_bad >= raim_min_sta) {
                        if (best_arc_eff > best_sat_eff) { best_sat = best_arc_sat; best_sat_eff = best_arc_eff; }
                        best_arc_sta = -1;   // drop the whole satellite instead of this one arc
                    }
                }

                enum class Kind { None, Arc, Sat, Sta } kind = Kind::None;
                double best_score = -1.0;   // strict '>' below → earlier (more surgical) wins ties
                if (best_arc_sta >= 0 && best_arc_eff / sigma0 > raim_k && best_arc_eff > best_score) {
                    best_score = best_arc_eff; kind = Kind::Arc;
                }
                if (!best_sat.empty() && best_sat_eff / sigma0 > raim_k && best_sat_eff > best_score) {
                    best_score = best_sat_eff; kind = Kind::Sat;
                }
                if (best_sta >= 0 && best_sta_eff / sigma0 > raim_k &&
                    (best_sta_eff / sta_bias) > best_score) {
                    best_score = best_sta_eff / sta_bias; kind = Kind::Sta;
                }
                if (kind == Kind::None) break;  // nothing bad enough left to drop

                if (kind == Kind::Arc) {
                    drop_arcs[best_arc_sta].insert(best_arc_sat);
                    adapt_detail += fmt::format("{}arc:{}/{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_arc_sta].name, best_arc_sat);
                } else if (kind == Kind::Sat) {
                    drop_sats.insert(best_sat);
                    adapt_detail += fmt::format("{}sat:{}", adapt_detail.empty() ? "" : ",", best_sat);
                } else {
                    drop_stations.insert(best_sta);
                    adapt_detail += fmt::format("{}sta:{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_sta].name);
                }

                Eigen::VectorXd x_try;
                we_adapt_attempted = true;
                try { x_try = resolve_dropping(); }
                catch (const std::exception& ex) {
                    // Should be unreachable now (the per-station full-weight retry uses the
                    // known-PD original assembly).  Last resort: restore the clean no-drop solve
                    // so the emit / harvest run on a consistent estimator rather than a
                    // half-rebuilt one (the old "keep prior solve" left est mid-rebuild).
                    log->warn("Network[RAIM within-epoch]: re-solve failed ({}); restoring clean solve.",
                              ex.what());
                    drop_sats.clear(); drop_stations.clear(); drop_arcs.clear();
                    try { x_s = resolve_dropping(); } catch (...) {}
                    break;
                }
                x_s = x_try;
                ++n_adapt;
                assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            }
            if (n_adapt > 0) {
                log->info("Network[RAIM within-epoch]: TOW {:.1f} re-conditioned in {} re-solve(s) — "
                          "refloated {} sat / {} arc, {} sta excluded [{}]; σ̂0 {:.2f} → {:.2f} (trigger {:.2f}).",
                          se.time.tow, n_adapt, drop_sats.size(),
                          [&] { size_t n = 0; for (const auto& kv : drop_arcs) n += kv.second.size(); return n; }(),
                          drop_stations.size(), adapt_detail, sigma0_first, sigma0, trig);
                // T2: one event per settled culprit (whole-sat drop, per-(sta,sat) arc drop,
                // excluded station).  statistic=final σ̂0, threshold=trigger, detail=first->final.
                if (events_trace.is_open()) {
                    const std::string span = fmt::format("{:.2f}->{:.2f}", sigma0_first, sigma0);
                    for (const auto& s : drop_sats)
                        emitEvent("T2_within_epoch", "we_refloat_sat", s, "", sigma0, trig,
                                  "refloat", span);
                    for (const auto& kv : drop_arcs)
                        for (const auto& s : kv.second)
                            emitEvent("T2_within_epoch", "we_refloat_arc", s,
                                      stations[kv.first].name, sigma0, trig, "refloat", span);
                    for (int sta : drop_stations)
                        emitEvent("T2_within_epoch", "we_exclude_station", "",
                                  stations[sta].name, sigma0, trig, "exclude", span);
                }
            }

            // A within-epoch satellite refloat reinitialized that sat's AMBIGUITY at every
            // station; its coupled shared clock is just as suspect, so refloat the carried
            // clock too (the stash below skips it).  Opt-in (clock_monitor_couple_raim).
            if (clk_mon_couple)
                for (const auto& s : drop_sats) clock_refloat_sats.insert(s);
            // P0-2: a within-epoch whole-satellite drop contaminated that sat's emitted
            // correction — feed the retro-bump regardless of the (numeric) couple opt-in.
            if (retro_feed)
                for (const auto& s : drop_sats) retro_flag_sats.insert(s);

            sigma0_adapt = sigma0;   // A4: publish the final within-epoch σ̂0 for the withhold gate
        }
        // C3c §3: drop_sats/drop_arcs leave scope with the RAIM block — retire the aliases
        // so the next epoch's primary buildCarryDdPriors never dereferences a dangling set.
        carry_we_drop_sats = nullptr;
        carry_we_drop_arcs = nullptr;

        // Reduce-once invariant (design §3.5): the carried prior is injected
        // pre-gate, so the single gate reduce() must be the ONLY per-station Schur
        // recompute this epoch — UNLESS the within-epoch adaptation deliberately re-solved
        // (each drop→re-solve is another reduce()).  A second recompute otherwise means a
        // stray post-gate local-block change silently doubled the serial path.
        if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) {
            log->error("Network[phase-leveling]: reduce() recomputed {} times this epoch "
                       "(expected exactly 1) — a stray post-gate local-block change doubled "
                       "the serial Schur path.", est.reduceRecomputeCount());
        }

        // --- Within-epoch escape-hatch: withhold rather than emit a still-contaminated epoch
        // The capped adaptation (drop culprit → re-solve, kMaxAdapt passes) may not fully
        // isolate the fault; if σ̂0 STILL exceeds the withhold ceiling the shared solution
        // is too corrupted to publish.  Reuse the observability-gate disposition (no emit,
        // no carry stash, epoch skipped — the contaminated solve never enters the store or
        // the carry) instead of emitting it with only a log warning.  OFF (raim_withhold ≤ 0)
        // is byte-identical: the epoch emits exactly as today.
        //
        // P0-4: gate on σ̂0 ALONE, NOT on we_adapt_attempted.  A DIFFUSE contamination
        // that leaves NO single dominant culprit (kind == Kind::None) never launches a
        // re-solve, so we_adapt_attempted stays false — yet σ̂0 is high, and that is
        // exactly the case the withhold ceiling should catch.  sigma0_adapt is set to the
        // assessed σ̂0 whenever the within-epoch RAIM block runs (whether or not a culprit
        // was dropped) and stays 0 otherwise, so a clean/OFF epoch (sigma0_adapt == 0)
        // still can never trip it.
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION carve-out of the σ̂0
        // withhold.  When the ceiling trips, judge each constellation by its OWN σ̂0_c —
        //
        //   σ̂0_c = max(1, 1.4826 · median{ r_i : i ∈ constellation c's rows })
        //
        // over EXACTLY the rows the global σ̂0 pools (carried-ambiguity PHASE rows of
        // every present non-pivot station, r_i = |v_i|·√w_i, divided by √(redundancy r̄_i)
        // under B2 with its uncheckable-row exclusion), partitioned by the ambiguity's
        // satellite constellation.  Same statistic, same ceiling, restricted support.
        //
        // APPROXIMATION (stated, not hidden): the residuals are evaluated at the JOINT
        // solution — a contamination in constellation A perturbs shared per-station
        // nuisances (receiver clock, ZWD, iono) that leak into B's residuals, and each
        // row's B2 redundancy is its share of the JOINT system's redundancy, not of a
        // per-constellation solve.  σ̂0_c therefore under-separates cross-constellation
        // coupling; the "every constellation individually clean while the global σ̂0
        // tripped" outcome below is the honest edge of that approximation and is
        // published-with-warning + counted (n_perconst_diffuse_published), not silently
        // absorbed.  A constellation also needs >= per_const_withhold_min_rows rows to
        // be JUDGEABLE — fewer cannot certify cleanliness under a contaminated epoch.
        //
        // The RANK/OBSERVABILITY gate above keeps whole-epoch semantics untouched: a
        // rank deficiency means the declared datum itself is invalid — the product
        // would be a meaningless least-norm split, so no "healthy subset" exists to
        // carve out.  This carve-out applies ONLY to the σ̂0 (contamination) withhold.
        bool           perconst_active = false;   // a publishable subset exists this epoch
        std::set<char> perconst_publish;          // constellations to keep in the emission
        std::set<char> perconst_dec_withhold;     // constellations the decision withheld
        std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;  // σ̂0_c + rows (events/log)
        if (perconst_on && raim_withhold > 0.0 && sigma0_adapt > raim_withhold) {
            for (auto& [cst, pool] : we_sigma0_pool_c) {
                NetworkEstimator::PerConstSigma0 st;
                st.rows = static_cast<int>(pool.size());
                if (!pool.empty()) {   // same median × 1.4826, floor 1 as the global σ̂0
                    std::nth_element(pool.begin(), pool.begin() + pool.size() / 2, pool.end());
                    st.sigma0_c = std::max(raim_s0_floor, 1.4826 * pool[pool.size() / 2]);
                }
                perconst_stats[cst] = st;
            }
            const NetworkEstimator::PerConstWithholdDecision dec =
                NetworkEstimator::decidePerConstWithhold(perconst_stats, raim_withhold,
                                                         perconst_min_rows);
            if (!dec.publish.empty()) {
                perconst_active       = true;
                perconst_publish      = dec.publish;
                perconst_dec_withhold = dec.withhold;
                std::string detail;
                for (const auto& [cst, st] : perconst_stats)
                    detail += fmt::format("{}{}:{:.2f}/{}{}", detail.empty() ? "" : " ", cst,
                                          st.sigma0_c, st.rows,
                                          dec.publish.count(cst) ? "" : "(withheld)");
                log->info("Network (RTA-10): TOW {:.1f} global σ̂0={:.2f} > ceiling {:.2f} — "
                          "per-constellation carve-out publishes {} of {} judged "
                          "constellation(s) [σ̂0_c/rows: {}].",
                          se.time.tow, sigma0_adapt, raim_withhold, dec.publish.size(),
                          perconst_stats.size(), detail);
            }
            // dec.publish empty ⇒ NO constellation is individually publishable — fall
            // through to the whole-epoch withhold below, exactly as without the toggle.
        }
        if (raim_withhold > 0.0 && sigma0_adapt > raim_withhold && !perconst_active) {
            log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} > withhold ceiling {:.2f} "
                      "after {} re-solve(s) — WITHHOLDING epoch (too contaminated to publish).",
                      se.time.tow, sigma0_adapt, raim_withhold, n_adapt);
            emitEvent("SIGMA0", "withhold", "", "", sigma0_adapt, raim_withhold,
                      "skip_epoch", fmt::format("{} re-solve(s)", n_adapt));
            ++n_skipped;
            ++n_sigma0_withheld;   // R8: dedicated count of σ̂0-ceiling withholds (reported in the summary)
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",1,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.sigma0 = sigma0_adapt; es.n_adapt = n_adapt;
              es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("sigma0_withheld", es); }
            // RTA-10: product-level heartbeat for the WITHHELD epoch (H line, no E line)
            // — a live tail can tell "σ̂0 withheld" from "producer died".  Only once the
            // deferred product writer has opened (the first emit); earlier withheld
            // epochs precede the product file itself.  withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldSigma0;
                hst.sigma0    = sigma0_adapt;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        subEnd(SB_SOLVEGLUE);   // D0b: close solve glue (net removed the internal solve/reduce)
        subBegin(SB_INSTALL);   // D0b: carry_driver.installPosterior (DD posterior mean/cov readback)
        // Card 4 (M1) C3b: install the DD posterior (mean + covariance) back into the
        // carry from the FINAL accepted x_s + datum — the invariant DDs the next epoch
        // projects forward.  POST-ACCEPTANCE (after the observability + σ̂0 withhold
        // gates' skip-paths above), so a withheld/contaminated epoch never poisons the
        // carry; and OUTSIDE the within-epoch RAIM block, so it runs every accepted
        // epoch (not only RAIM-armed ones).  A failure resets the carry (safe cold
        // start).  No-op when OFF.
        if (carry_invariant) {
            // parallel_carry: thread installPosterior's posterior-cov GEMMs.  Run it
            // SERIAL-over-blocks (n_threads=1, its byte-identical-to-serial path) so Eigen's
            // threading does not nest with the seam's per-block OpenMP.
            try { CarryEigenScope es(carry_par, n_threads);
                  carry_driver.installPosterior(est, x_s, plan.shared_datum,
                                                carry_par ? 1 : seam_threads); }
            catch (const std::exception& ex) {
                log->warn("Network[Card4]: DD-carry install failed at TOW {:.1f} ({}); "
                          "carry reset (refloats next epoch).", se.time.tow, ex.what());
                carry_driver.reset();
            }
            carry_prev_tow = se.time.tow;
            // M5 card WL-1: fold the accepted build's WL eligibility tallies into the run
            // totals (this install runs only for an accepted epoch; the last buildCarryDdPriors
            // before the accepted solve set mw_wl_epoch).  Invariant #4: counters or it didn't happen.
            if (mw_wl_on) {
                mw_wl_run.cycles_seen       += mw_wl_epoch.cycles_seen;
                mw_wl_run.rows_built        += mw_wl_epoch.rows_built;
                mw_wl_run.rows_capped       += mw_wl_epoch.rows_capped;
                mw_wl_run.skip_not_primary  += mw_wl_epoch.skip_not_primary;
                mw_wl_run.skip_missing_band += mw_wl_epoch.skip_missing_band;
                mw_wl_run.skip_missing_mw   += mw_wl_epoch.skip_missing_mw;
                mw_wl_run.skip_young_arc    += mw_wl_epoch.skip_young_arc;
                mw_wl_run.skip_bad_lambda   += mw_wl_epoch.skip_bad_lambda;
                mw_wl_run.skip_mixed_frame  += mw_wl_epoch.skip_mixed_frame;
                mw_wl_run.sum_R             += mw_wl_epoch.sum_R;

                // --- F6 one-shot attribution write (log-only; docs/HANDOFF_mw_wl_card.md §6c). ---
                // The accepted epoch's x_s is final here; add reconstructed_abs (the estimator's
                // current WL DD over the same cycle) + the kernel WL-block innovation, then dump.
                if (mw_wl_f6_dump && !mw_wl_f6.written && mw_wl_f6.pending
                    && mw_wl_f6.tow == se.time.tow) {
                    std::map<int, Eigen::VectorXd> xr_of;   // est_station -> local corrections
                    for (const auto& fr : mw_wl_f6.rows)
                        for (const auto& a : fr.arcs)
                            if (a.est_station >= 0 && !xr_of.count(a.est_station)) {
                                try { xr_of[a.est_station] = est.reconstructStation(
                                          static_cast<std::size_t>(a.est_station), x_s); }
                                catch (...) { xr_of[a.est_station] = Eigen::VectorXd(); }
                            }
                    Eigen::VectorXd v_wl;                   // kernel innovation v = z_free − Gᵀx_s − ẑ
                    {
                        Eigen::MatrixXd cov_v_wl;           // (unused; v is cov-free)
                        try {
                            const Eigen::MatrixXd cov0 =
                                Eigen::MatrixXd::Zero(x_s.size(), x_s.size());
                            est.ddPriorInnovation(mw_wl_f6.wl_block_index, x_s, cov0, v_wl, cov_v_wl);
                        } catch (...) { v_wl.resize(0); }
                    }
                    std::error_code ec_f6;
                    std::filesystem::create_directories(log_dir_, ec_f6);
                    std::ofstream f6(log_dir_ / "network_mw_wl_f6.csv");
                    if (f6) {
                        f6 << "# M5 WL-1 F6 attribution dump — first arming epoch (week "
                           << mw_wl_f6.week << ", tow " << fmt::format("{:.1f}", mw_wl_f6.tow)
                           << ", loop_epoch " << mw_wl_f6.loop_epoch << ", wl_block_index "
                           << mw_wl_f6.wl_block_index << ")\n";
                        f6 << "row,n_arcs,z_mw_cyc,z_mw_frac,z_lin_cyc,z_corr_cyc,zhat_block_cyc,"
                              "reconstructed_abs_cyc,recon_minus_zmw_cyc,recon_abs_frac,innovation_v_cyc\n";
                        for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                            const auto& fr = mw_wl_f6.rows[r];
                            double recon = 0.0; bool recon_ok = true;
                            for (const auto& a : fr.arcs) {
                                const auto it = xr_of.find(a.est_station);
                                double xr1 = 0.0, xr2 = 0.0;
                                if (it != xr_of.end()
                                    && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                                    && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                                    xr1 = it->second(a.col1); xr2 = it->second(a.col2);
                                } else { recon_ok = false; }
                                if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                                    recon += (a.coeff / a.lambda1) * (a.x_lin1 + xr1)
                                           - (a.coeff / a.lambda2) * (a.x_lin2 + xr2);
                            }
                            const double zmw_frac = fr.z_mw - std::round(fr.z_mw);
                            const double vrow = (static_cast<Eigen::Index>(r) < v_wl.size())
                                                    ? v_wl(static_cast<Eigen::Index>(r))
                                                    : std::numeric_limits<double>::quiet_NaN();
                            f6 << r << ',' << fr.arcs.size() << ','
                               << fmt::format("{:.4f}", fr.z_mw) << ','
                               << fmt::format("{:.4f}", zmw_frac) << ','
                               << fmt::format("{:.4f}", fr.z_lin) << ','
                               << fmt::format("{:.4f}", fr.z_corr) << ','
                               << fmt::format("{:.4f}", fr.zhat_block) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon) : std::string("nan")) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon - fr.z_mw) : std::string("nan")) << ','
                               << (recon_ok ? fmt::format("{:.4f}", recon - std::round(recon)) : std::string("nan")) << ','
                               << fmt::format("{:.4f}", vrow) << '\n';
                        }
                        f6 << "row,arc,est_station,sat,sig1,sig2,coeff,mw_mean_m,mw_arc_cyc,mw_count,"
                              "mw_sigma_m,bia_corrected,col1,col2,lambda1,lambda2,lambda_wl,"
                              "x_lin1_m,x_lin2_m,xr1_m,xr2_m,n1_abs_cyc,n2_abs_cyc,wl_arc_kf_cyc\n";
                        for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                            const auto& fr = mw_wl_f6.rows[r];
                            for (std::size_t k = 0; k < fr.arcs.size(); ++k) {
                                const auto& a = fr.arcs[k];
                                const auto it = xr_of.find(a.est_station);
                                double xr1 = 0.0, xr2 = 0.0; bool have_xr = false;
                                if (it != xr_of.end()
                                    && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                                    && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                                    xr1 = it->second(a.col1); xr2 = it->second(a.col2); have_xr = true;
                                }
                                const double n1abs = (a.lambda1 > 0.0) ? (a.x_lin1 + xr1) / a.lambda1 : 0.0;
                                const double n2abs = (a.lambda2 > 0.0) ? (a.x_lin2 + xr2) / a.lambda2 : 0.0;
                                const double mw_arc_cyc = (a.lambda_wl > 0.0) ? a.mw_mean_m / a.lambda_wl : 0.0;
                                f6 << r << ',' << k << ',' << a.est_station << ','
                                   << a.sat << ',' << a.sig1 << ',' << a.sig2 << ','
                                   << fmt::format("{:.1f}", a.coeff) << ','
                                   << fmt::format("{:.4f}", a.mw_mean_m) << ','
                                   << fmt::format("{:.4f}", mw_arc_cyc) << ','
                                   << a.mw_count << ','
                                   << fmt::format("{:.4f}", std::sqrt(std::max(a.mw_var_m2, 0.0))) << ','
                                   << (a.bia_corrected ? 1 : 0) << ','
                                   << a.col1 << ',' << a.col2 << ','
                                   << fmt::format("{:.6f}", a.lambda1) << ','
                                   << fmt::format("{:.6f}", a.lambda2) << ','
                                   << fmt::format("{:.6f}", a.lambda_wl) << ','
                                   << fmt::format("{:.4f}", a.x_lin1) << ','
                                   << fmt::format("{:.4f}", a.x_lin2) << ','
                                   << (have_xr ? fmt::format("{:.4f}", xr1) : std::string("nan")) << ','
                                   << (have_xr ? fmt::format("{:.4f}", xr2) : std::string("nan")) << ','
                                   << fmt::format("{:.4f}", n1abs) << ','
                                   << fmt::format("{:.4f}", n2abs) << ','
                                   << fmt::format("{:.4f}", n1abs - n2abs) << '\n';
                            }
                        }
                        mw_wl_f6.written = true;
                        mw_wl_f6.pending = false;
                        log->info("Network (M5/WL-1 F6): wrote {} — first arming epoch tow {:.1f}, "
                                  "{} row(s); attribute z_mw_frac (MW integer-ness) + "
                                  "recon_minus_zmw (state-frame gap).",
                                  (log_dir_ / "network_mw_wl_f6.csv").string(), mw_wl_f6.tow,
                                  mw_wl_f6.rows.size());
                    }
                }
            }
            // M5 card WL-2 EWL lane: same accepted-install accumulation (invariant #4),
            // per lane instance (UWL-1: one per tabled constellation when armed).
            if (ewl_on) {
                for (auto& lane : ewl_lanes) {
                    lane.run.cycles_seen       += lane.epoch.cycles_seen;
                    lane.run.rows_built        += lane.epoch.rows_built;
                    lane.run.rows_fixed        += lane.epoch.rows_fixed;
                    lane.run.rows_fixed_cold   += lane.epoch.rows_fixed_cold;
                    lane.run.rows_cold_unfixed += lane.epoch.rows_cold_unfixed;
                    lane.run.rows_capped       += lane.epoch.rows_capped;
                    lane.run.skip_not_primary  += lane.epoch.skip_not_primary;
                    lane.run.skip_missing_band += lane.epoch.skip_missing_band;
                    lane.run.skip_missing_mw   += lane.epoch.skip_missing_mw;
                    lane.run.skip_young_arc    += lane.epoch.skip_young_arc;
                    lane.run.skip_bad_lambda   += lane.epoch.skip_bad_lambda;
                    lane.run.skip_mixed_frame  += lane.epoch.skip_mixed_frame;
                    lane.run.skip_quarantine   += lane.epoch.skip_quarantine;
                    lane.run.sum_R             += lane.epoch.sum_R;
                }
            }
            // M5 card WL-2 WL lane (§3.3): apply the stashed inject plan against the FRESH
            // posterior (this epoch's NL), THEN fold the run counters.  The plan was decided
            // from DETECTOR statistics (§3.0.1) in the accepted build; here we read the carried
            // lane value + NL sum and, only if the carry still disagrees with the WL integer by
            // more than inject_disagree_min_cyc (self-limiting), set the lane difference to the
            // integer while preserving the phase-determined sum.  injectCarriedChord marks each
            // moved chord D5-exempt for the NEXT install (the §3.3 storm guard) internally.
            if (wl_inject_on || lane_fix_on) {
                for (const auto& pl : wl_inject_epoch) {
                    double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                    if (pl.pair
                            ? !carry_driver.carriedPairValue(pl.group_l1, pl.chord, z1, v1, z2, v2)
                            : (!carry_driver.carriedChordValue(pl.group_l1, pl.chord, z1, v1) ||
                               !carry_driver.carriedChordValue(pl.group_l2, pl.chord, z2, v2))) {
                        ++wl_run.skip_basis;   // not 1:1 across BOTH bands' carry bases — skip (§3.3)
                        if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis;
                        continue;
                    }
                    const double A1 = (z1 + pl.z_lin1) / pl.lambda1;   // absolute band DDs (cyc)
                    const double A2 = (z2 + pl.z_lin2) / pl.lambda2;
                    const double lane = A1 - A2, s = A1 + A2;          // WL DD + preserved NL sum
                    if (std::abs(pl.w - lane) <= nmw.inject_disagree_min_cyc)
                        continue;   // already lane-consistent (hold/fix row / prior inject) ⇒ no fight
                    const double A1p = 0.5 * (pl.w + s), A2p = 0.5 * (s - pl.w);
                    const double sg1 = nmw.wl_fix_sigma_cyc * pl.lambda1;
                    const double sg2 = nmw.wl_fix_sigma_cyc * pl.lambda2;
                    bool moved = false;
                    if (pl.pair) {
                        // WL-3 fallback: one joint edit, both bands, one D5 exemption (§3.3
                        // "should fire ~never in steady state — telemetry proves it").
                        moved = carry_driver.injectCarriedChordPair(
                            pl.group_l1, pl.chord,
                            A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1,
                            A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                    } else {
                        const bool i1 = carry_driver.injectCarriedChord(
                            pl.group_l1, pl.chord, A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1);
                        const bool i2 = carry_driver.injectCarriedChord(
                            pl.group_l2, pl.chord, A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                        moved = i1 && i2;
                    }
                    if (moved) { ++wl_run.injects; wl_run.inject_abs_sum += std::abs(pl.w - lane);
                                 if (uwl_on) {
                                     auto& wcr = wl_cons_run[pl.cons ? pl.cons : '?'];
                                     ++wcr.injects; wcr.inject_abs_sum += std::abs(pl.w - lane);
                                 }
                                 if (mw_tagsets) d5_inject_chords_now.insert(pl.chord); }
                    else       { ++wl_run.skip_basis;
                                 if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis; }
                }
                // Accepted-install accumulation: the pooled epoch tallies (also the whole walk
                // when the UWL card is off) plus, under UWL-1, the per-cons walk tallies —
                // folded into BOTH the aggregate and the per-cons run totals.
                wlAccumulate(wl_run, wl_epoch);
                for (const auto& [wcons, we] : wl_cons_epoch) {
                    wlAccumulate(wl_run, we);
                    wlAccumulate(wl_cons_run[wcons], we);
                }

                // F-B lane trace: write every entry's POST-install carried lane + tags.
                if (mw_lane_trace && !lane_trace_epoch.empty()) {
                    std::map<ArcKey, double> w_of;   // decided cycles' integer targets
                    for (const auto& pl : wl_inject_epoch) w_of[pl.chord] = pl.w;
                    std::error_code ec_lt;
                    std::filesystem::create_directories(log_dir_, ec_lt);
                    std::ofstream ltf(log_dir_ / "network_wl_lane_trace.csv", std::ios::app);
                    if (ltf) {
                        if (!lane_trace_header_written && ltf.tellp() == std::streampos(0))
                            ltf << "week,tow,chord_u,chord_v,station,sat,lane_cyc,frac_cyc,"
                                   "w,decided,held,injected\n";
                        lane_trace_header_written = true;
                        for (const auto& e : lane_trace_epoch) {
                            double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                            if (lane_on
                                    ? !carry_driver.carriedPairValue(e.g1, e.chord, z1, v1, z2, v2)
                                    : (!carry_driver.carriedChordValue(e.g1, e.chord, z1, v1) ||
                                       !carry_driver.carriedChordValue(e.g2, e.chord, z2, v2)))
                                continue;   // not 1:1 carried in both bands this epoch
                            const double lane = (z1 + e.z_lin1) / e.lam1
                                              - (z2 + e.z_lin2) / e.lam2;
                            const double frac = lane - std::round(lane);
                            const auto wit = w_of.find(e.chord);
                            const std::string sta_name =
                                (e.chord.u >= 0 && e.chord.u < static_cast<int>(stations.size()))
                                    ? stations[static_cast<std::size_t>(e.chord.u)].name
                                    : std::string{};
                            ltf << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                                << ',' << e.chord.u << ',' << e.chord.v << ',' << sta_name
                                << ',' << e.sat << ',' << fmt::format("{:.4f}", lane) << ','
                                << fmt::format("{:.4f}", frac) << ',';
                            if (wit != w_of.end()) ltf << fmt::format("{:.0f}", wit->second);
                            ltf << ',' << (wit != w_of.end() ? 1 : 0) << ','
                                << (d5_hold_chords.count(e.chord) ? 1 : 0) << ','
                                << (d5_inject_chords_now.count(e.chord) ? 1 : 0) << '\n';
                        }
                    }
                }
            }
            const auto& cc = carry_driver.counters();
            carry_epochs       = cc.epochs;
            carry_born_total   = cc.chords_born;
            carry_marg_total   = cc.chords_marginalized;
            carry_merges_total = cc.merges;
            carry_splits_total = cc.splits;
            carry_swaps_total  = cc.tree_swaps;
            carry_zdim_last    = cc.z_dim;
            carry_slipkills_total = cc.slip_kills;
            carry_dropped_total   = cc.chords_dropped;
            carry_innov_flags_total  = cc.innov_flags;
            carry_innov_retaps_total = cc.innov_retaps;
            // D5: fold the post-solve innovation flags into the NEXT epoch's slip funnel.
            // Each (group, chord) maps back to its (station, ambiguity) via the reverse map
            // built with this epoch's blocks; next epoch's buildCarryDdPriors excludes it from
            // the A_old membership so the affected cycle re-taps fresh (the C3c path).
            if (carry_innov_gate) {
                for (const auto& [gk, chord] : carry_driver.lastInnovFlags()) {
                    const auto git = carry_arc_amb_key.find(gk);
                    const auto ait = (git != carry_arc_amb_key.end())
                                         ? git->second.find(chord) : std::map<ArcKey, std::string>::const_iterator{};
                    if (git == carry_arc_amb_key.end() || ait == git->second.end()) {
                        // A flagged chord that the reverse map (built with THIS epoch's blocks)
                        // cannot resolve should never happen — both come from the same
                        // buildCarryDdPriors.  Warn once so a wiring regression is not silent;
                        // the flag is dropped (safe: q_z + next-epoch detection are the backstop).
                        if (++carry_innov_map_miss == 1)
                            log->warn("Network[Card4 D5]: an innovation flag (group {}, arc {}→{}) did "
                                      "not resolve to an ambiguity in the reverse map at TOW {:.1f} — "
                                      "the next-epoch re-tap is skipped for it (reverse-map/block "
                                      "mismatch; investigate if this recurs).",
                                      gk, chord.u, chord.v, se.time.tow);
                        continue;
                    }
                    carry_innov_pending.insert({chord.u, ait->second});
                    // WL-3 §3.4: a pair chord's flag refloats BOTH bands next epoch (they
                    // share the graph edge) — feed the band-2 ambiguity into the funnel too.
                    if (lane_on) {
                        const auto g2it = carry_arc_amb_key2.find(gk);
                        if (g2it != carry_arc_amb_key2.end()) {
                            const auto a2it = g2it->second.find(chord);
                            if (a2it != g2it->second.end())
                                carry_innov_pending.insert({chord.u, a2it->second});
                        }
                    }
                }
            }
            // F-A attribution dump (log-only, network.mw_wl.d5_dump): one CSV row per D5
            // flag with adjacency tags.  Uses the SAME reverse map as the funnel above.
            if (mw_d5_dump && carry_innov_gate) {
                const auto& flags  = carry_driver.lastInnovFlags();
                const auto& sigmas = carry_driver.lastInnovFlagSigmas();
                if (!flags.empty()) {
                    std::error_code ec_d5;
                    std::filesystem::create_directories(log_dir_, ec_d5);
                    std::ofstream d5f(log_dir_ / "network_d5_flags.csv", std::ios::app);
                    if (d5f) {
                        if (!d5_dump_header_written && d5f.tellp() == std::streampos(0))
                            d5f << "week,tow,group,chord_u,chord_v,station,amb_key,std_innov,"
                                   "ewl_stasat,hold_chord,injected_prev\n";
                        d5_dump_header_written = true;
                        for (std::size_t fi = 0; fi < flags.size(); ++fi) {
                            const auto& [gk, chord] = flags[fi];
                            std::string amb, sat;
                            const auto git = carry_arc_amb_key.find(gk);
                            if (git != carry_arc_amb_key.end()) {
                                const auto ait = git->second.find(chord);
                                if (ait != git->second.end()) amb = ait->second;
                            }
                            if (!amb.empty())
                                if (const auto parts = StateKeys::parseAmbiguityKey(amb))
                                    sat = parts->satellite;
                            const std::string sta_name =
                                (chord.u >= 0 && chord.u < static_cast<int>(stations.size()))
                                    ? stations[static_cast<std::size_t>(chord.u)].name
                                    : std::string{};
                            d5f << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                                << ',' << gk << ',' << chord.u << ',' << chord.v << ','
                                << sta_name << ',' << amb << ','
                                << fmt::format("{:.2f}",
                                       fi < sigmas.size() ? sigmas[fi] : 0.0) << ','
                                << (d5_ewl_stasat.count({ chord.u, sat }) ? 1 : 0) << ','
                                << (d5_hold_chords.count(chord) ? 1 : 0) << ','
                                << (d5_inject_chords_prev.count(chord) ? 1 : 0) << '\n';
                        }
                    }
                }
            }
            if (mw_tagsets) {   // rotate: this epoch's applied injects tag NEXT install's flags
                d5_inject_chords_prev.swap(d5_inject_chords_now);
                d5_inject_chords_now.clear();
            }
        }
        subEnd(SB_INSTALL);     // D0b: close installPosterior (the Card-4 posterior readback)
        subBegin(SB_NETAR);     // D0b: producer-side network AR Phase-2 (log-only; OFF in tier)

        // --- Producer-side network AR (Phase 2: DD fixability attempt, LOG ONLY) ---
        // The between-station double-difference ambiguities are datum-INVARIANT
        // estimable quantities, so this fix attempt is independent of the phase-bias
        // datum in use.  Phase 2 is READ-ONLY (no re-pin / re-solve yet — that is
        // Phase 3): it reconstructs the per-station float ambiguities, forms an
        // independent DD basis (star topology per (const,signal)), builds the exact
        // DD covariance from est.jointConditionalCovariance, converts to cycles, and
        // runs ils::ilsFix to log how many integers the network COULD fix.  Gated on
        // network.ambiguity_resolution.enable + min_epochs; byte-identical when off.
        if (netar.enable && n_emitted >= static_cast<size_t>(std::max(0, netar.min_epochs))
            && x_s.size() > 0 && ar_ppp_utils) {
            struct ArRec { double value_m; std::size_t est_id; Eigen::Index col; };
            // (const,signal) -> sat -> station index -> absolute float ambiguity + its
            // (est id, A_r column) for the covariance request.  GPS+Galileo only.
            std::map<std::pair<std::string, std::string>,
                     std::map<std::string, std::map<std::size_t, ArRec>>> ar_pool;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                Eigen::VectorXd x_r;
                try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
                catch (...) { continue; }
                const auto& h2l = asm_res[i].h2local;
                const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                    if (cst != "G" && cst != "E") continue;   // GPS+Gal integer-capable; GLONASS FDMA stays float
                    if (stations[i].sm->isAmbiguityUnresolvable(key)) continue;
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                    const auto hit = h2l.find(amb_idx);
                    if (hit == h2l.end() || hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const double value_m = x_lin(amb_idx) + x_r(hit->second);
                    if (!std::isfinite(value_m)) continue;
                    ar_pool[{cst, parts->signal}][parts->satellite][i] =
                        ArRec{value_m, static_cast<std::size_t>(est_index_of[i]),
                              static_cast<Eigen::Index>(hit->second)};
                }
            }
            // Independent DD basis: per (const,signal), a reference sat (tracked at the
            // most stations) and a reference station (tracking ref-sat + the most sats);
            // one DD per (other station r, other sat j) that both r and ref-station track.
            std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
            std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
            auto reqOf = [&](const ArRec& r) -> int {
                const std::pair<std::size_t, Eigen::Index> k{r.est_id, r.col};
                auto it = req_idx.find(k);
                if (it != req_idx.end()) return it->second;
                const int idx = static_cast<int>(reqs.size());
                reqs.push_back(k); req_idx[k] = idx; return idx;
            };
            struct DDrow { int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda; };
            std::vector<DDrow> dds;
            int n_const_signal = 0;
            for (auto& [cs, satmap] : ar_pool) {
                std::string ref_sat; std::size_t best = 0;
                for (auto& [sat, sm] : satmap)
                    if (sm.size() > best) { best = sm.size(); ref_sat = sat; }
                if (ref_sat.empty() || satmap[ref_sat].size() < 2) continue;
                std::size_t ref_i = 0, ref_cnt = 0; bool have_ref = false;
                for (auto& [sta_i, rec] : satmap[ref_sat]) {
                    (void)rec;
                    std::size_t cnt = 0;
                    for (auto& [sat, sm] : satmap) if (sm.count(sta_i)) ++cnt;
                    if (!have_ref || cnt > ref_cnt) { ref_cnt = cnt; ref_i = sta_i; have_ref = true; }
                }
                if (!have_ref) continue;
                const System sys = (cs.first == "G") ? System::GPS : System::Galileo;
                const double lambda = ar_ppp_utils->getWavelength(sys, cs.second, ref_sat);
                if (!(std::isfinite(lambda) && lambda > 0.0)) continue;
                const ArRec& fp = satmap[ref_sat].at(ref_i);   // ref station, ref sat
                ++n_const_signal;
                for (auto& [sat, sm] : satmap) {
                    if (sat == ref_sat) continue;
                    const auto fj_it = sm.find(ref_i);          // ref station, sat j
                    if (fj_it == sm.end()) continue;
                    for (const auto& [sta_i, rj] : sm) {
                        if (sta_i == ref_i) continue;
                        const auto rp_it = satmap[ref_sat].find(sta_i);   // station r, ref sat
                        if (rp_it == satmap[ref_sat].end()) continue;
                        const double dd_m = (rj.value_m - rp_it->second.value_m)
                                          - (fj_it->second.value_m - fp.value_m);
                        dds.push_back(DDrow{ reqOf(rj), reqOf(rp_it->second),
                                             reqOf(fj_it->second), reqOf(fp), dd_m, lambda });
                    }
                }
            }
            if (!dds.empty() && reqs.size() >= 2) {
                Eigen::MatrixXd cov_undiff;
                try { cov_undiff = est.jointConditionalCovariance(reqs, plan.shared_datum); }
                catch (...) { cov_undiff = Eigen::MatrixXd(); }
                if (cov_undiff.rows() == static_cast<Eigen::Index>(reqs.size())
                    && cov_undiff.rows() > 0) {
                    const int nd = static_cast<int>(dds.size());
                    Eigen::MatrixXd D = Eigen::MatrixXd::Zero(nd, static_cast<int>(reqs.size()));
                    Eigen::VectorXd y_cyc(nd);
                    std::vector<double> lam(nd);
                    for (int a = 0; a < nd; ++a) {
                        const DDrow& d = dds[a];
                        D(a, d.i_rj) += 1.0; D(a, d.i_rp) -= 1.0;
                        D(a, d.i_fj) -= 1.0; D(a, d.i_fp) += 1.0;
                        y_cyc(a) = d.dd_m / d.lambda;
                        lam[a] = d.lambda;
                    }
                    Eigen::MatrixXd cov_dd_m = D * cov_undiff * D.transpose();
                    Eigen::MatrixXd Q_cyc(nd, nd);
                    for (int a = 0; a < nd; ++a)
                        for (int b = 0; b < nd; ++b)
                            Q_cyc(a, b) = cov_dd_m(a, b) / (lam[a] * lam[b]);
                    Q_cyc = 0.5 * (Q_cyc + Q_cyc.transpose());
                    // --- ILS dimension cap (network.ambiguity_resolution.ils_max_dim) ---
                    // MLAMBDA's search does not return on the uncapped full-network basis
                    // (~2500 DDs at 305 stations, Jul-15).  When armed (> 0), search only
                    // the ils_max_dim lowest-formal-σ DDs — the standard partial-AR
                    // subset (ascending Q_aa) — and leave the rest float.  The summary
                    // statistics (med σ / med frac / near-int) stay FULL-basis so the
                    // convergence gauge is unaffected by the cap.
                    Eigen::VectorXd y_fix = y_cyc;
                    Eigen::MatrixXd Q_fix = Q_cyc;
                    int nd_fix = nd;
                    if (netar.ils_max_dim > 0 && nd > netar.ils_max_dim) {
                        nd_fix = netar.ils_max_dim;
                        std::vector<int> order(static_cast<std::size_t>(nd));
                        std::iota(order.begin(), order.end(), 0);
                        std::sort(order.begin(), order.end(), [&](int a, int b) {
                            return Q_cyc(a, a) < Q_cyc(b, b);
                        });
                        y_fix.resize(nd_fix);
                        Q_fix.resize(nd_fix, nd_fix);
                        for (int a = 0; a < nd_fix; ++a) {
                            y_fix(a) = y_cyc(order[static_cast<std::size_t>(a)]);
                            for (int b = 0; b < nd_fix; ++b)
                                Q_fix(a, b) = Q_cyc(order[static_cast<std::size_t>(a)],
                                                    order[static_cast<std::size_t>(b)]);
                        }
                    }
                    ils::IlsGateConfig gates;
                    gates.min_success_rate     = netar.min_success_rate;
                    gates.ratio_threshold      = netar.ratio_threshold;
                    gates.ratio_upper_bound    = netar.ratio_upper_bound;
                    gates.nl_max_per_pair_frac = netar.nl_max_per_pair_frac;
                    gates.enable_partial_ar    = netar.enable_partial_ar;
                    ils::IlsResult fix;
                    try { fix = ils::ilsFix(y_fix, Q_fix, gates); }
                    catch (...) { fix = ils::IlsResult(); }
                    // Median DD formal σ (cycles) — the convergence gauge: as the
                    // network float tightens toward cm this drops and the DDs become
                    // fixable.  Also the worst float-fraction (evidence the products
                    // are integer-consistent even before they are confidently fixable).
                    std::vector<double> dsig; dsig.reserve(nd);
                    std::vector<double> dfrac; dfrac.reserve(nd);
                    int n_near = 0;   // DDs whose float is within 0.15 cyc of an integer
                    for (int a = 0; a < nd; ++a) {
                        if (Q_cyc(a, a) > 0.0) dsig.push_back(std::sqrt(Q_cyc(a, a)));
                        const double f = std::abs(y_cyc(a) - std::round(y_cyc(a)));
                        dfrac.push_back(f);
                        if (f <= 0.15) ++n_near;
                    }
                    auto med = [](std::vector<double>& v) {
                        if (v.empty()) return 0.0;
                        std::sort(v.begin(), v.end());
                        return v[v.size() / 2];
                    };
                    const double med_sig  = med(dsig);
                    const double med_frac = med(dfrac);
                    log->info("Network AR (Phase 2, log-only): TOW {:.1f} — {} DD / {} undiff-amb / "
                              "{} (const,signal); fixed {}{} of {} searched{} (ratio {:.2f}, "
                              "p_succ {:.4f}, med_DD_sigma {:.4f} cyc, med_DD_frac {:.3f}, "
                              "near_int {}/{})",
                              se.time.tow, nd, static_cast<int>(reqs.size()), n_const_signal,
                              fix.ok ? static_cast<int>(fix.kept.size()) : 0,
                              fix.partial ? " partial" : "", nd_fix,
                              nd_fix < nd ? " (ILS-capped)" : "", fix.ratio, fix.p_succ,
                              med_sig, med_frac, n_near, nd);
                }
            }
        }

        // --- One-shot full-arrowhead NEQ export (network.neq_export_epoch) -----------
        // Post-solve READ-ONLY dump of the assembled normal equations for offline
        // spectral / weak-direction attribution: border N_ss (raw + reduced) + RHS,
        // every station's N_sr / N_rr (rebuilt from its LDLᵀ) / u_r as row-major
        // little-endian double .bin, plus shared-column and per-station local-column
        // labels.  Solution is untouched; fires on exactly one epoch per run.
        if (nc.neq_export_epoch > 0
            && n_emitted + 1 == static_cast<size_t>(nc.neq_export_epoch)
            && x_s.size() > 0) {
            const std::filesystem::path dir = log_dir_ / "neq_export";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            auto appendBin = [](std::ofstream& f, const Eigen::MatrixXd& M) {
                // Explicit row-major copy so the offline reader needs no
                // storage-order convention (Eigen's default is column-major).
                const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic,
                                    Eigen::RowMajor> R = M;
                f.write(reinterpret_cast<const char*>(R.data()),
                        static_cast<std::streamsize>(sizeof(double) * R.size()));
            };
            auto sanitize = [](std::string s) {
                for (char& ch : s) if (ch == ',') ch = ';';
                return s;
            };
            const auto& kernel = est.normals();
            {
                std::ofstream f(dir / "Nss_raw.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.sharedNormals());
            }
            {
                std::ofstream f(dir / "us_raw.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.sharedRhs());
            }
            {
                std::ofstream f(dir / "Nss_red.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.reducedSharedNormals());
            }
            {
                std::ofstream f(dir / "us_red.bin", std::ios::binary | std::ios::trunc);
                appendBin(f, kernel.reducedSharedRhs());
            }
            {
                std::ofstream f(dir / "shared_labels.csv", std::ios::trunc);
                f << "col,label\n";
                for (Eigen::Index c = 0; c < est.numShared(); ++c)
                    f << c << ',' << sanitize(est.sharedColumnLabel(c)) << '\n';
            }
            std::ofstream sidx(dir / "stations_index.csv", std::ios::trunc);
            sidx << "est_slot,name,n_local\n";
            std::ofstream slab(dir / "stations_labels.csv", std::ios::trunc);
            slab << "est_slot,local_col,label\n";
            std::ofstream nsr(dir / "stations_Nsr.bin", std::ios::binary | std::ios::trunc);
            std::ofstream nrr(dir / "stations_Nrr.bin", std::ios::binary | std::ios::trunc);
            std::ofstream urf(dir / "stations_ur.bin", std::ios::binary | std::ios::trunc);
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const auto r = static_cast<std::size_t>(est_index_of[i]);
                const Eigen::Index nl = kernel.numLocal(r);
                sidx << r << ',' << stations[i].name << ',' << nl << '\n';
                appendBin(nsr, kernel.stationCrossBlock(r));
                appendBin(nrr, kernel.stationLocalNormals(r));
                appendBin(urf, kernel.stationLocalRhs(r));
                // Local col → StateManager label via the assembly's h2local inverse;
                // columns with no SM mapping (none expected) stay "?".
                std::vector<std::string> names(static_cast<std::size_t>(nl), "?");
                const auto sm_labels = stations[i].sm->getStateLabels();
                for (const auto& kv : asm_res[i].h2local) {
                    const auto lc = static_cast<Eigen::Index>(kv.second);
                    if (lc >= 0 && lc < nl
                        && static_cast<std::size_t>(kv.first) < sm_labels.size())
                        names[static_cast<std::size_t>(lc)] =
                            sm_labels[static_cast<std::size_t>(kv.first)];
                }
                for (Eigen::Index c = 0; c < nl; ++c)
                    slab << r << ',' << c << ','
                         << sanitize(names[static_cast<std::size_t>(c)]) << '\n';
            }
            {
                std::ofstream meta(dir / "meta.csv", std::ios::trunc);
                meta << "week,tow,epoch,n_shared,n_stations\n"
                     << se.time.week << ',' << se.time.tow << ','
                     << nc.neq_export_epoch << ',' << est.numShared() << ','
                     << kernel.numStations() << '\n';
            }
            log->info("Network: NEQ export written to {} (epoch {}, n_shared {}, "
                      "{} station blocks)",
                      dir.string(), nc.neq_export_epoch, est.numShared(),
                      kernel.numStations());
        }

        subEnd(SB_NETAR);      // D0b: close producer-AR / pre-emit tail
        subBegin(SB_SOLPREP);  // D0b: toSolutionEpoch + per-sat discontinuity signalling

        // --- Emit -----------------------------------------------------------
        NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++,
                                                       /*provider_id=*/0, /*solution_id=*/0,
                                                       /*ambiguity_datum_ensemble=*/phase_datum_ensemble);
        // Card 5 (RTF-2): stamp the broadcast-ephemeris VERSION the producer's clock-C0
        // baseline used, per emitted satellite, so the consumer applies C0 against the
        // SAME broadcast clock (getSatelliteClockCorrection pinned to clock_iode, Card 7)
        // and the eph-cutover re-level (Card 6) can detect a version roll.  The broadcast
        // clock is station-independent (one shared LiveEphemerisStore), so the version the
        // per-station emission path (computeEmissionState) selected this epoch is exactly
        // what computeSatelliteClock reports here — toe granularity (hours) dwarfs the
        // intra-epoch light-travel spread, so pick() lands on the same record.  Querying
        // the store directly at the emit site is a deliberate, equivalent simplification of
        // the plan's "thread the IODE out of the emission path" (no per-station out-param
        // plumbing needed).  GLONASS ('R') stays -1 (plan D8).  sp3 mode: the store is empty
        // ⇒ nullopt ⇒ clock_iode stays -1 everywhere ⇒ Card-4 writes no iode= token ⇒
        // byte-identical.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            for (auto& s : sol.sats) {
                if (!s.has_clock || s.sat.empty() || s.sat[0] == 'R') continue;
                if (auto bc = eph_store.computeSatelliteClock(s.sat, se.time))
                    s.clock_iode = bc->iode;
            }
        }
        // RT0-4: stamp the wall-clock TIME-OF-GENERATION (distinct from the content
        // epoch se.time) so a consumer can measure product latency.  Only when armed
        // — otherwise generation_time stays {0,0} and no G record is serialized
        // (byte-identical).  Non-deterministic (wall clock), hence gated.
        if (emit_gen_time) sol.generation_time = TimeUtils::nowGpsTime();
        // RTA-3: stamp the per-run solution-generation id, minted once from the
        // FIRST EMITTED epoch's GPS time (week·604800 + ⌊tow⌋).  Deterministic under
        // file replay (the id is serialized per epoch as an I record and comes back
        // out of the file); a restarted live producer first-emits at a later epoch
        // and therefore mints a DIFFERENT id — the consumer treats the change as a
        // global datum break.  0 is reserved for "unset", so an armed run clamps to
        // 1 in the (never-in-practice) week-0 corner.  OFF ⇒ sol.generation_id stays
        // 0 ⇒ no I record, store generation unset (byte-identical).
        // RTB-7 seam: an RTB-7(a) WARM RESUME restores the previous run's non-zero
        // id BEFORE the epoch loop, so the generation_id == 0 guard below suppresses
        // this mint — re-minting on a warm resume would signal a global datum break
        // and force a spurious consumer-wide hard reset.  Only a genuine cold start
        // (no/stale/mismatched snapshot) reaches the mint.
        if (emit_gen_id) {
            if (generation_id == 0) {
                generation_id = static_cast<uint32_t>(se.time.week) * 604800u
                              + static_cast<uint32_t>(se.time.tow);
                if (generation_id == 0) generation_id = 1;
                log->info("Network (RTA-3): solution-generation id {} minted from the first "
                          "emitted epoch (week {}, tow {:.1f}).",
                          generation_id, se.time.week, se.time.tow);
            }
            sol.generation_id = generation_id;
        }
        // B3b: publish the two-state clock-model drift as C1 (consumer extrapolates
        // c0 + c1·dt).  clock_osc[sat].x1 is the drift carried INTO this epoch (from the
        // previous epoch's posterior sequence — the correct extrapolation basis; the
        // clock-model update for THIS epoch runs after the emit).  0 when clock_model off.
        if (clk_model_on)
            for (auto& s : sol.sats) {
                const auto oit = clock_osc.find(s.sat);
                if (oit == clock_osc.end() || !oit->second.init) continue;
                // R6: a satellite that vanished (eclipse) and returns still carries its PRE-GAP
                // drift here — the clock-model update that reseeds x1 to 0 runs after this emit
                // — so shipping oit->second.x1 unconditionally would emit an hours-old rate as C1
                // for one epoch.  Gate on the carried state's freshness against the SAME staleness
                // bound the prediction / drift finite-difference use (R11): past the gap the drift
                // is not trustworthy → leave c1 at 0 (ZOH), matching the reseed the update will do.
                const double sat_dt = (oit->second.t_last > 0.0) ? (pl_now_s - oit->second.t_last)
                                                                 : 0.0;
                if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                    s.clock_c1_m_s = oit->second.x1;
                    c1_zoh_active.erase(s.sat);
                } else {
                    // T5/R6 (V2 blind spot): a vanish-return past the gap ships c1=0 (ZOH) for one
                    // epoch instead of an hours-old rate.  Edge-triggered (c1_zoh_active) so a
                    // persistently-stale sat logs ONCE, not every emit.
                    if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                        emitEvent("T5_clock_model", "c1_zoh_suppress", s.sat, "", sat_dt,
                                  clk_model_max_gap, "c1_zero", "vanish_return");
                }
            }
        // network.emit_clock_rate: same C1 publish, but from the DECOUPLED rate state (no carry
        // prior, no refloat — the clock_model no-go's benefit was ~1× anyway; the asset is the
        // rate, not the carry).  clock_rate_est[sat].x1 is the drift finite-differenced from the
        // PREVIOUS epoch's posterior (its update runs after this emit).  Same R6 staleness ZOH gate.
        else if (emit_clock_rate_on)
            for (auto& s : sol.sats) {
                const auto rit = clock_rate_est.find(s.sat);
                if (rit == clock_rate_est.end() || !rit->second.init) continue;
                const double sat_dt = (rit->second.t_last > 0.0) ? (pl_now_s - rit->second.t_last)
                                                                 : 0.0;
                if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                    s.clock_c1_m_s = rit->second.x1;
                    c1_zoh_active.erase(s.sat);
                } else {
                    if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                        emitEvent("T5_clock_rate", "c1_zoh_suppress", s.sat, "", sat_dt,
                                  clk_model_max_gap, "c1_zero", "vanish_return");
                }
            }
        // RTA-10: constellations withheld from THIS epoch's emission — filled by the
        // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out below;
        // feeds the H heartbeat's status (ok vs partial) and its withheld field.
        std::set<char> epoch_withheld_consts;
        // RT0-5: per-constellation clock-runaway quarantine.  Group this epoch's clock
        // solutions by constellation; if a constellation's median |clock − global median|
        // exceeds the magnitude ceiling (or median |drift| exceeds the rate ceiling), DROP
        // that constellation's satellites from the emission and flag it — a runaway (e.g.
        // the open BeiDou clock divergence) then degrades SCOPE, not the whole product.
        // OFF ⇒ sol untouched (byte-identical).  Placed AFTER the C1 drift-stamp pass so
        // the rate ceiling sees this epoch's clock_c1_m_s (0 when clock_model off ⇒ only
        // the magnitude ceiling applies), and BEFORE the URA pass so a withheld
        // constellation costs no URA work.
        //
        // HAZARD (documented, not defended): the magnitude test is referenced to the
        // GLOBAL median across ALL clock sats.  If the runaway constellation supplies
        // half or more of the epoch's clock satellites, the global median lands inside
        // the runaway cluster and the comparison INVERTS — the healthy minority trips
        // the ceiling and is quarantined while the runaway constellation passes.  The
        // check is sized for its intended regime (a minority constellation, e.g. the
        // open BeiDou divergence, walking off by ~km against a multi-constellation
        // majority); do not arm it in a run dominated by the suspect constellation.
        // The single-constellation degenerate case is warned at config load.
        if (clk_runaway_on && !sol.sats.empty()) {
            std::vector<double> all_c;
            for (const auto& s : sol.sats) if (s.has_clock) all_c.push_back(s.clock_c0_m);
            std::set<char> runaway;
            if (!all_c.empty()) {
                std::nth_element(all_c.begin(), all_c.begin() + all_c.size() / 2, all_c.end());
                const double gmed = all_c[all_c.size() / 2];   // common-mode datum
                std::map<char, std::vector<double>> dev, rate;
                for (const auto& s : sol.sats) {
                    if (s.sat.empty() || !s.has_clock) continue;
                    dev[s.sat[0]].push_back(std::abs(s.clock_c0_m - gmed));
                    if (s.clock_c1_m_s != 0.0) rate[s.sat[0]].push_back(std::abs(s.clock_c1_m_s));
                }
                auto medianOf = [](std::vector<double>& v) {
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    return v[v.size() / 2];
                };
                for (auto& [c, d] : dev) {
                    const double dm = medianOf(d);
                    double       rm = 0.0;
                    bool         bad = dm > clk_runaway_mag;
                    if (!bad) {
                        const auto rit = rate.find(c);
                        if (rit != rate.end() && !rit->second.empty()) {
                            rm  = medianOf(rit->second);
                            bad = rm > clk_runaway_rate;
                        }
                    }
                    if (bad) {
                        runaway.insert(c);
                        const auto n_drop = std::count_if(sol.sats.begin(), sol.sats.end(),
                            [c](const NetworkSatSolution& s){ return !s.sat.empty() && s.sat[0] == c; });
                        log->warn("Network (RT0-5): constellation '{}' clock runaway — median |Δclk|="
                                  "{:.1f} m (ceil {:.0f}), median |drift|={:.3f} m/s (ceil {:.2f}); "
                                  "WITHHOLDING its {} sats this epoch.",
                                  c, dm, clk_runaway_mag, rm, clk_runaway_rate, n_drop);
                        emitEvent("RT0-5_runaway", "constellation_withhold", std::string(1, c), "",
                                  dm, clk_runaway_mag, "drop_const", fmt::format("drift={:.3f}", rm));
                    }
                }
            }
            if (!runaway.empty()) {
                std::vector<NetworkSatSolution> kept;
                kept.reserve(sol.sats.size());
                for (auto& s : sol.sats)
                    if (s.sat.empty() || runaway.count(s.sat[0]) == 0) kept.push_back(std::move(s));
                sol.sats.swap(kept);
                ++n_runaway_epochs;
                epoch_withheld_consts.insert(runaway.begin(), runaway.end());  // RTA-10: H heartbeat
            }
        }
        // RTA-10 (network.per_const_withhold): apply the per-constellation σ̂0 carve-out
        // decided at the withhold site above — prune the non-publishable constellations'
        // satellites from the emission EXACTLY the way the RT0-5 runaway quarantine does
        // (adjacent code, same swap-prune pattern).  Placed AFTER the C1 drift-stamp pass
        // (unlike RT0-5 nothing here reads clock_c1_m_s — a pruned sat merely had its C1
        // stamped needlessly, which is harmless) and BEFORE the URA pass, so a withheld
        // constellation costs no URA work — the same seam RT0-5 occupies.  A satellite
        // whose constellation is NOT in the publish set is pruned; that includes any
        // constellation ABSENT from the σ̂0 pools entirely (zero rows ⇒ unjudgeable ⇒
        // not publishable under a contaminated epoch).  The downstream emitter seams —
        // presence-aware pending datum bumps, the post-prune retro split, the rise/set
        // code-datum detection — all key on the post-prune sol.sats, so they behave for
        // this prune exactly as they do for the (tested) RT0-5 quarantine.
        if (perconst_active) {
            std::set<char> pruned;
            std::vector<NetworkSatSolution> kept;
            kept.reserve(sol.sats.size());
            for (auto& s : sol.sats) {
                if (!s.sat.empty() && perconst_publish.count(s.sat[0]) == 0)
                    pruned.insert(s.sat[0]);
                else
                    kept.push_back(std::move(s));
            }
            if (!pruned.empty()) {
                sol.sats.swap(kept);
                ++n_perconst_partial_epochs;
                epoch_withheld_consts.insert(pruned.begin(), pruned.end());
                for (char cst : pruned) {
                    const auto sit    = perconst_stats.find(cst);
                    const bool judged = sit != perconst_stats.end();
                    log->warn("Network (RTA-10): TOW {:.1f} constellation '{}' WITHHELD by the "
                              "per-constellation σ̂0 carve-out — σ̂0_c={} over {} row(s) "
                              "(ceiling {:.2f}, min rows {}); its sats are pruned from this "
                              "epoch's emission.",
                              se.time.tow, cst,
                              judged ? fmt::format("{:.2f}", sit->second.sigma0_c)
                                     : std::string("n/a"),
                              judged ? sit->second.rows : 0, raim_withhold, perconst_min_rows);
                    emitEvent("RTA-10_perconst", "constellation_withhold", std::string(1, cst),
                              "",
                              judged ? std::optional<double>(sit->second.sigma0_c) : std::nullopt,
                              raim_withhold, "drop_const",
                              fmt::format("rows={};global_sigma0={:.2f}",
                                          judged ? sit->second.rows : 0, sigma0_adapt));
                }
            } else if (perconst_dec_withhold.empty()) {
                // The honest edge of the per-constellation approximation: the GLOBAL σ̂0
                // exceeded the ceiling, yet EVERY judged constellation individually passed
                // (cross-constellation coupling — shared per-station nuisances — that no
                // per-constellation partial captures).  Publish everything, but say so
                // loudly and count it; a rising count says the carve-out's separability
                // assumption is failing on this network.
                ++n_perconst_diffuse_published;
                log->warn("Network (RTA-10): TOW {:.1f} DIFFUSE contamination published — global "
                          "σ̂0={:.2f} > ceiling {:.2f} while every judged constellation passed "
                          "individually (the per-constellation statistic disagreed with the "
                          "global one).",
                          se.time.tow, sigma0_adapt, raim_withhold);
                emitEvent("RTA-10_perconst", "diffuse_published", "", "", sigma0_adapt,
                          raim_withhold, "publish_all",
                          fmt::format("{} const(s) individually clean", perconst_stats.size()));
            }
            // else: the decision withheld constellation(s) that are not in this epoch's
            // emission anyway (RT0-5 already quarantined them, or they carry no solution
            // sats) — nothing pruned here, and not the diffuse edge either.
        }
        // B1: per-sat URA = EWMA(σ̂0) × formal clock σ (shared-covariance diagonal),
        // skipping datum-pinned clock columns (their variance is the artificial pin,
        // not a physical σ) and inflating a recently-refloated sat for its warmup horizon.
        // Uses the PREVIOUS epoch's σ̂0 EWMA (post-emit QC ordering — a one-epoch lag).
        if (ura_on) {
            // RTB-7(b): evaluate the quality-ramp LIFT rule (counts through the
            // previous epoch, matching the floor's indexing below): n_emitted ≥
            // max(warmup_epochs, phase_leveling_warmup_epochs) AND the σ̂0 EWMA
            // has received ≥ warmup_epochs updates — OR the EWMA is structurally
            // unavailable (the RTB-6 sentinel condition: emit_ura+raim+
            // phase_leveling not all armed; here ura_on holds, so raim_on&&pl_on
            // decides), in which case the epoch horizon alone lifts.  Logs the
            // RTB-7(c) cold-start-to-product-grade metric at the lift.  Guarded
            // by the floor ⇒ OFF (0) is byte-identical.
            if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted) {
                const bool ewma_structural = raim_on && pl_on;   // && ura_on (this block)
                if (networkWarmupRampLifted(static_cast<long long>(n_emitted),
                                            nc.warmup_epochs, pl_warmup,
                                            static_cast<long long>(ura_ewma_updates),
                                            ewma_structural)) {
                    ramp_lifted    = true;
                    grade_metric_s = (first_emit_content_s >= 0.0)
                                         ? (pl_now_s - first_emit_content_s)
                                         : 0.0;
                    log->info("Network (RTB-7): cold-start-to-product-grade: {:.1f} s / {} "
                              "epochs — warmup URA floor {:.1f} m LIFTED (n_emitted {} >= "
                              "max(warmup_epochs {}, pl_warmup {}); sigma0-EWMA updates {}{}).",
                              grade_metric_s, n_emitted, nc.warmup_ura_floor_m, n_emitted,
                              nc.warmup_epochs, pl_warmup, ura_ewma_updates,
                              ewma_structural ? "" : " — EWMA structurally unavailable, "
                                                     "update count waived");
                }
            }
            const Eigen::MatrixXd& scov = est.sharedCovariance();
            std::set<Eigen::Index> pinned;
            for (const auto& pin : plan.shared_datum) pinned.insert(pin.column);
            const double s0 = std::max(1.0, ura_sigma0_ewma);
            for (auto& s : sol.sats) {
                if (!s.has_clock) continue;
                Eigen::Index col;
                try { col = est.clockColumn(s.sat); } catch (...) { continue; }
                if (pinned.count(col)) continue;   // datum pivot — bogus pin variance, no honest URA
                if (col < 0 || col >= scov.rows() || col >= scov.cols()) continue;
                const double var = scov(col, col);
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                double ura = s0 * std::sqrt(var);
                const auto hit = ura_refloat_horizon.find(s.sat);
                if (ura_refloat_inflate > 1.0 && hit != ura_refloat_horizon.end() && hit->second > 0)
                    ura *= ura_refloat_inflate;   // honest surrogate for the post-refloat DIA penalty
                // RTB-7(b): cold-start quality ramp — a freshly (re)started producer
                // advertises DEGRADED URA until the ramp LIFTS (the
                // networkWarmupRampLifted rule evaluated above: emitted-epoch
                // horizon AND σ̂0-EWMA stabilization, not a bare epoch count), so a
                // consumer never trusts a not-yet-converged network solution.  A
                // warm-resumed producer restores ramp_lifted from the snapshot and
                // is not degraded.  Off (floor 0) ⇒ byte-identical.
                if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted)
                    ura = std::max(ura, nc.warmup_ura_floor_m);
                s.clock_ura_m = ura;
                s.has_ura     = true;
            }
        }
        // B1b (defect-2 lever B, Tienstra §6.6): per-(sat,signal) PHASE-BIAS correction
        // σ — the phase-bias block of Qŷ_B1 the clock-only URA channel omits.  The
        // consumer currently applies phase-bias corrections as if EXACT; this transmits
        // their formal 1σ so a wandering bias (the 6–69 cm differential defect-2 driver)
        // is down-weighted instead.  Gauge-exact: a COPY of the shared covariance with
        // the folded ensemble phase-bias gauges nulled (zeroPhaseGaugeColumns) and the
        // datum-pinned columns skipped (their 1/pin_scale is a gauge, not a physical σ) —
        // exactly the URA loop's pin treatment.  Scaled by the SAME σ̂0 EWMA as the URA.
        // Off ⇒ σ stays 0 ⇒ product/store byte-identical.
        if (config_.networkEmitPhaseBiasSigma()) {
            Eigen::MatrixXd        pcov = est.sharedCovariance();   // copy; treat gauges as exact
            est.zeroPhaseGaugeColumns(pcov);
            std::set<Eigen::Index> pb_pinned;
            for (const auto& pin : plan.shared_datum) pb_pinned.insert(pin.column);
            const double        s0 = std::max(1.0, ura_sigma0_ewma);
            std::vector<double> pb_sigmas;   // premise-check diagnostic: distribution of ABSOLUTE emitted σ
            // For the B2-vs-LeverA decision: collect per-signal columns so we can also
            // measure the BETWEEN-SATELLITE-DIFFERENCED σ.  The consumer's rx clock +
            // per-arc float ambiguity absorb the common/level directions of the
            // phase-bias error (TK-2015 Eq 88), so the ABSOLUTE σ (float-ambiguity-
            // entangled level, ~1 m at 0 fixes) overstates what actually reaches the
            // consumer estimate.  The between-satellite SD σ is the honest per-epoch
            // consumer-relevant magnitude.
            std::map<std::string, std::vector<Eigen::Index>> by_signal;
            for (auto& s : sol.sats) {
                for (auto& [signal, npb] : s.phase_bias) {
                    Eigen::Index col;
                    try { col = est.phaseBiasColumn(s.sat, signal); } catch (...) { continue; }
                    if (pb_pinned.count(col)) continue;                 // anchor-pinned → exact, no honest σ
                    if (col < 0 || col >= pcov.rows() || col >= pcov.cols()) continue;
                    const double var = pcov(col, col);
                    if (!(var > 0.0) || !std::isfinite(var)) continue;  // folded gauge (zeroed) or degenerate
                    npb.sigma_m = s0 * std::sqrt(var);
                    pb_sigmas.push_back(npb.sigma_m);
                    by_signal[signal].push_back(col);
                }
            }
            // T3d: under the anchor-free ENSEMBLE datum the absolute σ above references the
            // zeroed receiver-ensemble gauge column (degenerate — the level is not observable).
            // Overwrite each emitted phase-bias σ with the honest BETWEEN-SATELLITE-SD σ per
            // signal (the quantity the consumer's rx-clock + float ambiguity leave observable),
            // referenced to the group's first eligible column; the reference sat itself takes the
            // group-median SD σ (its SD-vs-self is 0, not an honest uncertainty).  Only under
            // phase_datum_ensemble ⇒ byte-identical for the pivot/prioritized datum.
            if (phase_datum_ensemble) {
                // Under the ensemble gauge EVERY satellite phase-bias column is a FOLDED gauge
                // direction, so the zeroed pcov above gives σ=0 for all of them (by_signal is
                // empty).  Compute the honest between-sat SD σ from the RAW covariance instead:
                // the fold's common gauge-direction variance cancels in the between-sat
                // difference, leaving the relative uncertainty.  Reference each signal group to
                // its first sat; the reference sat takes the group-median SD σ (SD-vs-self is 0).
                const Eigen::MatrixXd rawcov = est.sharedCovariance();
                std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> grp;
                for (auto& s : sol.sats)
                    for (auto& [signal, npb] : s.phase_bias) {
                        (void)npb;
                        Eigen::Index col;
                        try { col = est.phaseBiasColumn(s.sat, signal); } catch (...) { continue; }
                        if (col >= 0 && col < rawcov.rows() && col < rawcov.cols())
                            grp[signal].push_back({ s.sat, col });
                    }
                for (auto& [signal, sc] : grp) {
                    if (sc.size() < 2) continue;                 // no between-sat difference
                    const Eigen::Index ref = sc.front().second;
                    std::map<std::string, double> sd_of;         // sat -> SD σ
                    std::vector<double> sds;
                    for (std::size_t i = 1; i < sc.size(); ++i) {
                        const Eigen::Index c = sc[i].second;
                        const double v = rawcov(c, c) + rawcov(ref, ref) - 2.0 * rawcov(c, ref);
                        if (v > 0.0 && std::isfinite(v)) {
                            const double sd = s0 * std::sqrt(v);
                            sd_of[sc[i].first] = sd;
                            sds.push_back(sd);
                        }
                    }
                    if (sds.empty()) continue;
                    std::sort(sds.begin(), sds.end());
                    sd_of[sc.front().first] = sds[sds.size() / 2];   // ref sat: group median
                    for (auto& s : sol.sats) {
                        auto it = s.phase_bias.find(signal);
                        if (it == s.phase_bias.end()) continue;
                        auto sit = sd_of.find(s.sat);
                        if (sit != sd_of.end() && sit->second > 0.0)
                            it->second.sigma_m = sit->second;
                    }
                }
            }
            // Lever-B premise check + the decisive ratio: ABSOLUTE σ vs between-sat-SD σ.
            if (!pb_sigmas.empty() && static_cast<long long>(n_emitted) % 120 == 0) {
                std::sort(pb_sigmas.begin(), pb_sigmas.end());
                // Between-satellite SD σ per signal, referenced to the first sat in the
                // signal group: sqrt(var_s + var_ref − 2·cov(s,ref)) × σ̂0.  If SD σ ≪
                // ABS σ, the level dominates (consumer-absorbed) ⇒ covariance carry is
                // structurally limited ⇒ the wander is temporal ⇒ pivot to Lever A.
                std::vector<double> sd_sigmas;
                for (auto& [signal, cols] : by_signal) {
                    if (cols.size() < 2) continue;
                    const Eigen::Index ref = cols.front();
                    for (std::size_t i = 1; i < cols.size(); ++i) {
                        const Eigen::Index c = cols[i];
                        const double v = pcov(c, c) + pcov(ref, ref) - 2.0 * pcov(c, ref);
                        if (v > 0.0 && std::isfinite(v)) sd_sigmas.push_back(s0 * std::sqrt(v));
                    }
                }
                double sd_lo = -1.0, sd_med = -1.0, sd_hi = -1.0;
                if (!sd_sigmas.empty()) {
                    std::sort(sd_sigmas.begin(), sd_sigmas.end());
                    sd_lo = sd_sigmas.front();
                    sd_med = sd_sigmas[sd_sigmas.size() / 2];
                    sd_hi = sd_sigmas.back();
                }
                log->info("Network (B1b): phase-bias σ for {} (sat,signal) — ABS min/med/max = "
                          "{:.3f}/{:.3f}/{:.3f} m | between-sat-SD min/med/max = {:.3f}/{:.3f}/{:.3f} m "
                          "(−1 = <2 sats/signal) (σ̂0 {:.3f}).",
                          pb_sigmas.size(), pb_sigmas.front(), pb_sigmas[pb_sigmas.size() / 2],
                          pb_sigmas.back(), sd_lo, sd_med, sd_hi, s0);
            }
        }
        // Datum bump + serialization, applied HERE (after a successful solve so a
        // withheld/failed epoch never leaks a pending bump).  The A1/A2 clock-pivot
        // handover (this epoch) is a per-CONSTELLATION datum re-reference (E-line
        // field-8); the B1 retro-bump (a post-emit QC tier flagged these SATELLITES
        // last epoch) is a per-SATELLITE discontinuity (R record).  Both must
        // reproduce on replay from the serialized flags.  A GLOBAL datum bump
        // (emit_clock_disc off) serializes an EMPTY constellation set so replay
        // re-applies the global bump.
        bool           global_bump = false;
        std::set<char> serialized_consts;
        if (datum_changed) {
            if (emit_clock_disc) {
                emitter.markDatumChange(changed_consts);
                serialized_consts.insert(changed_consts.begin(), changed_consts.end());
            } else {
                emitter.markDatumChange();   // global (pre-A2 byte-identical)
                global_bump = true;
            }
            // PIVOT (V1/V2): the datum re-reference is otherwise UNLOGGED.  One row per
            // changed constellation, sat=new pivot, detail=<const>:old->new + arm.  Arm is
            // derived here (pivotHandoverSignalled does not return it): prev_pivot still holds
            // the key ⇒ adjacent handover; else ⇒ vanish_return (old pivot from last_pivot).
            // Emitted BEFORE prev_pivot advances below.
            if (events_trace.is_open()) {
                for (char c : changed_consts) {
                    const std::string key(1, c);
                    auto np = plan.clock_pivot.find(key);
                    const std::string new_p = (np != plan.clock_pivot.end()) ? np->second
                                                                             : std::string();
                    std::string old_p;
                    const char* arm;
                    auto pp = prev_pivot.find(key);
                    if (pp != prev_pivot.end()) { old_p = pp->second; arm = "adjacent"; }
                    else {
                        auto lp = last_pivot.find(key);
                        old_p = (lp != last_pivot.end()) ? lp->second : std::string("?");
                        arm = "vanish_return";
                    }
                    emitEvent("PIVOT", "handover", new_p, "", std::nullopt, std::nullopt,
                              "datum_bump", fmt::format("{}:{}->{};{}", key, old_p, new_p, arm));
                }
            }
        }
        std::set<std::string> retro_emit_sats;   // B1 retro-bump applied this emit (serialized as an R record)
        if (ura_retro && !ura_retro_sats.empty()) {
            // RT0-5 seam (P0-1: the R record must describe bumps that LANDED in the
            // emitted product): a flagged sat ABSENT from this epoch's emission —
            // its constellation quarantined above, or the sat itself vanished —
            // must not be consumed here.  The emitter would clear its mark without
            // any of its keys stepping, and the R record would overstate a bump
            // that never reached the product (replay would then diverge from
            // intent, not from the file).  Keep it PENDING so it is marked AND
            // serialized at its next actual emit; only sats present in the
            // (post-quarantine) sol.sats are consumed now, so mark == land and
            // replay re-marks exactly the landed sats on exactly those epochs.
            // Edge-triggered semantics preserved: a pending sat is already in
            // retro_active, so it is not re-queued while it waits.
            std::set<std::string> present_sats;
            for (const auto& s : sol.sats) present_sats.insert(s.sat);
            std::set<std::string> retro_pending;
            for (const auto& sat : ura_retro_sats)
                (present_sats.count(sat) ? retro_emit_sats : retro_pending).insert(sat);
            ura_retro_sats = std::move(retro_pending);   // absent sats wait for their next emit
            if (!retro_emit_sats.empty())
                emitter.markSatDiscontinuity(retro_emit_sats);   // per-sat, not constellation-wide
            // RETRO (V4): one row per per-sat discontinuity bump APPLIED this emit.  The QC
            // tier that flagged the sat last epoch is lost at the union, so detail=union
            // (honest — do not fabricate provenance).
            if (events_trace.is_open())
                for (const auto& s : retro_emit_sats)
                    emitEvent("RETRO", "retro_bump", s, "", std::nullopt, std::nullopt,
                              "disc_bump", "union");
        }
        // Card 6: land the forced per-sat discontinuities for eph cutovers whose prior
        // version was evicted before Δ_eph could be computed (an unknown clock step → the
        // consumer must reset that sat's network-corrected ambiguity).  Independent of
        // ura_retro_bump — this is a genuine datum step, not a QC retro-bump — but reuses
        // the same present/pending split: a flagged sat ABSENT from this emission waits for
        // its next actual emit, so the bump lands with the sat present (replay-faithful).
        if (!eph_cutover_disc_pending.empty()) {
            std::set<std::string> present, mark, still_pending;
            for (const auto& s : sol.sats) present.insert(s.sat);
            for (const auto& sat : eph_cutover_disc_pending)
                (present.count(sat) ? mark : still_pending).insert(sat);
            eph_cutover_disc_pending = std::move(still_pending);
            if (!mark.empty()) {
                emitter.markSatDiscontinuity(mark);
                if (events_trace.is_open())
                    for (const auto& s : mark)
                        emitEvent("RELEVEL", "eph_cutover_disc_landed", s, "", std::nullopt,
                                  std::nullopt, "disc_bump", "prior_eph_version_evicted");
            }
        }
        // T3c: land clock_signal_basis_hold RE-LATCH disc bumps this emit (same present/pending
        // split — a re-latched sat absent from this emission waits for its next actual emit).
        // markSatDiscontinuity bumps the per-sat phase/code/clock counters; the re-latch re-splits
        // δtˢ↔dˢ so all three step.  Populated only under clock_signal_basis_hold && emit_clock_disc
        // ⇒ the set is empty (this block a no-op) otherwise = byte-identical.  (Hold-OFF re-selection
        // is landed just below, at the emit-only this_epoch_if_pin_sel comparison.)
        if (!basis_relatch_disc_pending.empty()) {
            std::set<std::string> present, mark, still_pending;
            for (const auto& s : sol.sats) present.insert(s.sat);
            for (const auto& sat : basis_relatch_disc_pending)
                (present.count(sat) ? mark : still_pending).insert(sat);
            basis_relatch_disc_pending = std::move(still_pending);
            if (!mark.empty()) {
                emitter.markSatDiscontinuity(mark);
                if (events_trace.is_open())
                    for (const auto& s : mark)
                        emitEvent("CLK_BASIS", "relatch_disc_landed", s, "", std::nullopt,
                                  std::nullopt, "disc_bump", "if_pin_reference_moved");
            }
        }
        // T3c (hold OFF): the deterministic IF-pin re-selection discontinuity, resolved
        // ONLY here at the emit site so it describes the LAST EMITTED product's pair —
        // exactly the prev_pivot / emitted_anchor discipline.  Compare THIS emitted
        // product's selected pair (this_epoch_if_pin_sel, from the main pass) against the
        // last EMITTED pair (last_if_pin_sel) for every EMITTED satellite; a genuine
        // change re-references the emitted δtˢ↔dˢ split by the sat's inter-channel bias,
        // so force its per-sat disc bump (phase/code/clock counters).  Advancing the
        // baseline only here means a transient pin move on a WITHHELD epoch (which never
        // reaches this point) is dropped, and a pair that leaves and returns to the SAME
        // emitted reference across withheld epochs raises NO bump.  Only emitted sats are
        // compared/advanced (a sat withheld this epoch keeps its last-emitted baseline and
        // is compared when it next emits).  Empty scratch ⇒ no-op (byte-identical) when
        // emit_clock_disc is off or the run is under clock_signal_basis_hold.
        if (!this_epoch_if_pin_sel.empty()) {
            std::set<std::string> present;
            for (const auto& s : sol.sats) present.insert(s.sat);
            std::set<std::string> mark;
            for (const auto& [sat, sel] : this_epoch_if_pin_sel) {
                if (!present.count(sat)) continue;   // not emitted this epoch: baseline untouched
                const auto prev = last_if_pin_sel.find(sat);
                if (prev != last_if_pin_sel.end() && prev->second != sel) {
                    mark.insert(sat);
                    if (events_trace.is_open())
                        emitEvent("CLK_BASIS", "reselect", sat, "", std::nullopt, std::nullopt,
                                  "if_pin_reference_moved",
                                  prev->second.first + "+" + prev->second.second +
                                      "->" + sel.first + "+" + sel.second);
                }
                last_if_pin_sel[sat] = sel;   // advance the emit baseline (emitted sats only)
            }
            if (!mark.empty()) emitter.markSatDiscontinuity(mark);
        }
        // Card 2 (network.code_datum_time_linked): land the time-linked datum RE-SEED
        // disc bumps — a no-veteran sum-to-zero channel or an IF-pin split forced to the
        // 0 seed stepped the emitted value, so the consumer must reset instead of
        // integrating across the unsignalled step.  This_epoch_cb_reseed (built in the
        // main pass) is merged into the persistent pending set HERE, at the emit site —
        // which every whole-epoch withhold path `continue`s before reaching — so a
        // transient re-seed on a since-withheld epoch never enters the pending set (no
        // bump without an emitted step), while a re-seed the driver DID realize is
        // retained and deferred exactly like the eph-cutover / basis-relatch bumps: a
        // re-seeded sat ABSENT from THIS emission (pruned by quarantine / per-const
        // withhold) waits for its next actual emit so the bump lands with the sat present
        // (emit-only, replay-faithful).  Both sets are only ever populated under
        // code_datum_time_linked ⇒ no-op (byte-identical) otherwise.
        for (const auto& sat : this_epoch_cb_reseed) cb_reseed_disc_pending.insert(sat);
        if (!cb_reseed_disc_pending.empty()) {
            std::set<std::string> present, mark, still_pending;
            for (const auto& s : sol.sats) present.insert(s.sat);
            for (const auto& sat : cb_reseed_disc_pending)
                (present.count(sat) ? mark : still_pending).insert(sat);
            cb_reseed_disc_pending = std::move(still_pending);
            if (!mark.empty()) {
                emitter.markSatDiscontinuity(mark);
                if (events_trace.is_open())
                    for (const auto& s : mark)
                        emitEvent("CB_DATUM", "reseed_disc_landed", s, "", std::nullopt,
                                  std::nullopt, "disc_bump", "code_datum_time_linked_reseed");
            }
        }
        // The handover-detection baseline advances ONLY here, with the bump it
        // pairs with: prev_pivot/last_pivot describe the datum of the last
        // EMITTED product.  A handover epoch that never reaches this point
        // (gate withhold, solve failure, σ̂0 withhold) leaves the baseline
        // untouched, so the change re-detects — and re-skips the old-datum
        // carries — every epoch until one emits in the new datum.
        prev_pivot = plan.clock_pivot;
        // A1: merge-update the persistent last-ever pivot map (never erase a
        // constellation) so a later vanish-return with a different pivot is caught.
        for (const auto& kv : plan.clock_pivot) last_pivot[kv.first] = kv.second;
        // Prioritized phase-bias anchor: record per-(sat,signal) anchor HANDOVERS
        // for observability, and advance the emit-baseline (emitted_anchor).  The
        // consumer-facing DISCONTINUITY is already handled — replay-safely — by the
        // emitter's per-(sat,signal) bias-jump test: a handover steps the emitted
        // bias by a different receiver's integer ambiguity lump (≥ ~1 cycle, well
        // above disc_threshold_m), so emitToStore below bumps that key's counter and
        // the consumer resets the ambiguity.  We deliberately do NOT issue a separate
        // markSatDiscontinuity here: it is not reconstructable from the serialized
        // NetworkSolutionEpoch (replay does not re-run the election), so it would
        // make the product's disc counters diverge on NetworkProductIO replay.  The
        // baseline advances ONLY at emit (a withheld epoch re-detects next emit),
        // never erasing an absent key (matches the last_pivot policy).
        if (phase_datum_prioritized) {
            int n_handover = 0;
            for (const auto& [amb, sta] : this_epoch_anchor) {
                auto it = emitted_anchor.find(amb);
                if (it != emitted_anchor.end() && it->second != sta) {
                    // eq34 (Card 3): a tree-edge REATTACH — the emitted δ̃_s steps by
                    // an EXACT integer number of cycles (chain path difference; the
                    // receiver fractional no longer leaks), usually 0 under pin-to-
                    // round.  Disc still rides the emitter's replay-safe bias-jump
                    // test (NEVER markSatDiscontinuity — not reconstructable on
                    // NetworkProductIO replay).  Trace event only.
                    emitEvent(phase_datum_eq34 ? "PHASE_DATUM" : "PHASE_ANCHOR",
                              phase_datum_eq34 ? "eq34_reattach" : "anchor_handover",
                              amb.substr(0, amb.find('_')), sta,
                              std::nullopt, std::nullopt, "reanchor",
                              it->second + "->" + sta);
                    ++n_handover;
                }
                emitted_anchor[amb] = sta;
            }
            if (n_handover > 0)
                log->debug("Network: {} per-(sat,signal) phase-bias anchor handover(s) at "
                           "TOW {:.1f} (disc handled by the emitter bias-jump test).",
                           n_handover, se.time.tow);
        }
        // B6 (P1-1): open the deferred contract writer at the FIRST emit, stamping the
        // realized per-constellation clock pivots (this epoch's plan) into the v2 header.
        if (contract_pending) {
            contract_pending = false;
            for (const auto& [cst, sat] : plan.clock_pivot)
                pending_contract.decl.clock_pivot_sat[cst] = sat;   // realized auto-picked pivots
            // Card 3 (SingleReceiverChain): stamp the realized datum receiver r1 as
            // the diagnostic pdr= token (component-0 representative).
            if (phase_datum_eq34 && !eq34_datum_rx.empty()) {
                pending_contract.decl.eq34_datum_receiver = eq34_datum_rx.begin()->second;
                eq34_emitted_pdr = pending_contract.decl.eq34_datum_receiver;
            }
            // RTA-3: the generation id (minted above at this same first emit) travels
            // in the contract as an optional gid= field — 0 (unset) writes nothing.
            pending_contract.generation_id = sol.generation_id;
            product_writer = std::make_unique<NetworkProductWriter>(prod_path.string(), pending_contract);
            if (product_writer->good()) {
                log->info("Network: product serialization open — v2 S-basis contract, "
                          "{} realized clock pivot(s).", pending_contract.decl.clock_pivot_sat.size());
                last_contract_emit_s = pl_now_s;   // RTA-5: the header S line is the first broadcast
            } else {
                log->error("Network: failed to open product_output_path '{}' for writing; "
                           "disabling on-disk serialization for this run.", prod_path.string());
                product_writer.reset();
            }
        }
        // M4 (contract v2): per-key COMBINED-correction σ (sigc phase, sigcc code) +
        // code/phase correlation (rho) from the shared posterior covariance's
        // cross-terms.  The individual clock / phase-bias / code-bias formal σ (the
        // URA and B1b channels) are cm-level during a satellite's rise-reconvergence,
        // yet the COMBINED correction the user applies (clock + bias) is mm-level
        // because the errors are strongly correlated (Zhang 2021 Figs. 2–5; Psychas
        // 2024b Eq. 12).  A consumer weighting by sigc/sigcc keeps USING a fresh
        // satellite's products through reinitialization instead of deweighting them
        // 10–100×.  Iono is EXCLUDED (network iono is a global SH-VTEC field, not a
        // co-resident column, and is supplied separately via SsrSlantIono with its own
        // σ — bundling it here would double-count).  Gauge-nulled covariance copy +
        // datum-pin skip + positive-variance guard, exactly like B1b; a folded/zeroed
        // phase gauge (ensemble datum) yields no honest phase column ⇒ no token.
        // Scaled by the SAME σ̂0 EWMA (cancels in rho).  Off ⇒ every value 0 ⇒ no wire
        // token ⇒ product/store byte-identical.
        if (config_.networkEmitCombinedSigma()) {
            Eigen::MatrixXd cbcov = est.sharedCovariance();   // copy; treat gauges as exact
            est.zeroPhaseGaugeColumns(cbcov);
            std::set<Eigen::Index> cb_pinned;
            for (const auto& pin : plan.shared_datum) cb_pinned.insert(pin.column);
            const double s0 = std::max(1.0, ura_sigma0_ewma);
            // A column is usable only when present, in range, NOT a datum pin (its
            // 1/pin_scale variance is a gauge artefact), and carrying real positive
            // variance (rejects a zeroed phase gauge under the ensemble datum).
            auto colOrNeg = [&](auto fn) -> Eigen::Index {
                try {
                    const Eigen::Index c = fn();
                    if (c < 0 || c >= cbcov.rows() || c >= cbcov.cols() || cb_pinned.count(c))
                        return -1;
                    if (!(cbcov(c, c) > 0.0) || !std::isfinite(cbcov(c, c))) return -1;
                    return c;
                } catch (...) { return -1; }
            };
            for (auto& s : sol.sats) {
                if (!s.has_clock) continue;
                const Eigen::Index c = colOrNeg([&]{ return est.clockColumn(s.sat); });
                if (c < 0) continue;   // no honest clock column ⇒ nothing to combine
                // Per-signal combined PHASE σ (sigc) for every emitted phase bias.
                for (auto& [signal, npb] : s.phase_bias) {
                    const Eigen::Index p =
                        colOrNeg([&]{ return est.phaseBiasColumn(s.sat, signal); });
                    const auto cc = computeCombinedCorrectionSigma(cbcov, c, p, -1, s0);
                    if (cc.sigc > 0.0) npb.sigc_m = cc.sigc;
                }
                // Per-sat combined CODE σ (sigcc) + code/phase corr (rho) at a REFERENCE
                // signal: prefer the first code signal whose PHASE sibling is present (so
                // rho pairs a real code/phase observable), else the first code signal (rho
                // then absent).  Code and phase biases live in DISJOINT key namespaces
                // (code "C1W" vs phase "L1W"), so the pairing needs the RINEX obs-type
                // swap selectCombinedCodeRef performs — a raw key equality never matches.
                // The C record is per-sat and the combined code σ is clock-dominated, so
                // this per-sat summary is a faithful proxy across the satellite's code
                // signals (documented approximation).
                std::vector<std::string> code_sigs;
                code_sigs.reserve(s.code_bias_m.size());
                for (const auto& [signal, bias] : s.code_bias_m) { (void)bias; code_sigs.push_back(signal); }
                std::set<std::string> phase_sigs;
                for (const auto& [signal, npb] : s.phase_bias) { (void)npb; phase_sigs.insert(signal); }
                const auto ref = selectCombinedCodeRef(code_sigs, phase_sigs);
                if (!ref.code.empty()) {
                    const Eigen::Index b =
                        colOrNeg([&]{ return est.codeBiasColumn(s.sat, ref.code); });
                    const Eigen::Index p = ref.phase.empty()
                        ? Eigen::Index(-1)
                        : colOrNeg([&]{ return est.phaseBiasColumn(s.sat, ref.phase); });
                    const auto cc = computeCombinedCorrectionSigma(cbcov, c, p, b, s0);
                    if (cc.sigcc > 0.0) { s.sigcc_m = cc.sigcc; s.rho = cc.rho; }
                }
            }
        }
        subEnd(SB_SOLPREP);     // D0b: close sol-prep + discontinuity signalling
        subBegin(SB_EMITSTORE); // D0b block 7: re-base + PB-emit filter + store push + solution CSV
        // Card 4 (M1) phase D: emit-layer frame re-base (docs/HANDOFF_card4_phaseD.md
        // §1).  Under carry_invariant the eq34 datum is re-realized freely, so the raw
        // emitted per-(sat,σ) phase bias churns every epoch by a gauge element (a common
        // per-component fractional + a per-sat integer×λ) that leaves user DDs invariant
        // but would trip the emitter's 0.05 m jump test and storm the consumer with
        // spurious resets.  Re-reference the emitted biases onto the PREVIOUS EMISSION
        // frame BEFORE the store push + on-disk write, so both the in-process consumer
        // and a NetworkProductIO replay carry the re-based values (the transform is baked
        // into the product — replay reproduces the disc counters with no re-base rerun).
        // A genuine reference-path (tree-edge) slip's integer step is left INTACT so the
        // jump test still fires (never laundered — standing invariant #1).  Gated on
        // carry_invariant && emit_rebase ⇒ OFF path (carry_invariant=false) byte-identical.
        if (emit_rebase_on && !sol.sats.empty()) {
            // (a) Reference-path slip set: (sat,σ) whose emission reference — a spanning-
            //     tree/chain (pinned_amb_targets) edge — slipped this epoch.  Only a
            //     tree-edge slip moves the emitted b_s; a chord slip refloats its DD
            //     without touching the reference, so it is NOT a re-base survivor.
            std::set<std::pair<std::string, std::string>> rb_slipped_ref;   // (sat, signal)
            for (const auto& [sta_i, amb_key] : carry_slipped_ambs) {
                if (sta_i < 0 || sta_i >= static_cast<int>(stations.size())) continue;
                if (!stations[sta_i].pinned_amb_targets.count(amb_key)) continue;   // tree edge only
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (parts && !parts->satellite.empty())
                    rb_slipped_ref.insert({parts->satellite, parts->signal});
            }

            // (b) Connected-component partition of the (receiver,satellite) bipartite
            //     graph per (const,signal): the common fractional δ is shared only within
            //     a component (each elects its OWN datum receiver r1).  Union-find over
            //     this epoch's shared arcs — the same walk buildCarryDdPriors does.
            std::map<std::pair<char, std::string>, std::map<std::string, std::string>> dsu;
            auto findRoot = [](const std::map<std::string, std::string>& p,
                               std::string v) -> std::string {
                auto it = p.find(v);
                while (it != p.end() && it->second != v) { v = it->second; it = p.find(v); }
                return v;
            };
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i] || est_index_of[i] < 0) continue;
                for (const auto& [amb, sidx] : stations[i].sm->getAmbiguities()) {
                    (void)sidx;
                    const auto parts = StateKeys::parseAmbiguityKey(amb);
                    if (!parts || parts->satellite.empty() || parts->satellite.front() == 'R')
                        continue;   // GLONASS FDMA excluded (no common-λ integer DD)
                    if (!shared_phase_keys.count(
                            StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal)))
                        continue;   // not an eq34 graph arc (gated/legacy lump)
                    auto& p = dsu[{parts->satellite.front(), parts->signal}];
                    const std::string rv = "R" + std::to_string(i);
                    const std::string sv = "S" + parts->satellite;
                    if (!p.count(rv)) p[rv] = rv;
                    if (!p.count(sv)) p[sv] = sv;
                    p[findRoot(p, rv)] = findRoot(p, sv);   // union
                }
            }
            // (sat,signal) -> global component id (1-based; 0 = unassigned sentinel).
            std::map<std::string, int> comp_of;
            int next_comp = 0;
            for (auto& [g, p] : dsu) {
                std::map<std::string, int> root2id;
                for (const auto& [v, par] : p) {
                    (void)par;
                    if (v.empty() || v[0] != 'S') continue;   // satellite vertices only
                    const std::string root = findRoot(p, v);
                    int& id = root2id[root];
                    if (id == 0) id = ++next_comp;
                    comp_of[v.substr(1) + "|" + g.second] = id;
                }
            }

            // (c) Build re-base entries from the emitted phase biases (GLONASS + non-eq34
            //     keys excluded ⇒ their raw emitted frame is untouched).  λ per (const,σ).
            std::vector<NetworkEmitRebase::Entry> rb_entries;
            std::map<std::pair<char, std::string>, double> lambda_of;
            auto sysOfChar = [](char c) -> System {
                switch (c) { case 'G': return System::GPS;    case 'E': return System::Galileo;
                             case 'C': return System::BeiDou; case 'J': return System::QZSS;
                             default:  return System::Unknown; }
            };
            for (auto& s : sol.sats) {
                if (s.sat.empty() || s.sat.front() == 'R') continue;
                const char cons = s.sat.front();
                for (auto& [signal, npb] : s.phase_bias) {
                    const auto cit = comp_of.find(s.sat + "|" + signal);
                    if (cit == comp_of.end()) continue;   // legacy/gated key: keep raw frame
                    const std::pair<char, std::string> g{cons, signal};
                    double& lam = lambda_of[g];
                    if (lam == 0.0 && eq34_ppp_utils)
                        lam = eq34_ppp_utils->getWavelength(sysOfChar(cons), signal, s.sat);
                    NetworkEmitRebase::Entry e;
                    e.sat = s.sat; e.signal = signal; e.component = cit->second;
                    e.lambda_m = lam; e.bias_m = npb.bias_m;
                    e.slipped = rb_slipped_ref.count({s.sat, signal}) != 0;
                    rb_entries.push_back(std::move(e));
                }
            }
            emit_rebase_engine.rebaseEpoch(rb_entries);
            // Write the re-based biases back into the emitted epoch (match by key; the
            // engine mutates in place but a key match is robust to any future reorder).
            std::map<std::string, double> rb_out;
            for (const auto& e : rb_entries) rb_out[e.sat + "|" + e.signal] = e.bias_m;
            for (auto& s : sol.sats)
                for (auto& [signal, npb] : s.phase_bias) {
                    const auto it = rb_out.find(s.sat + "|" + signal);
                    if (it != rb_out.end()) npb.bias_m = it->second;
                }

            // M5 prerequisite: PB emission-continuity filter on the RE-BASED values.
            // A per-key scalar KF + self-tuning k·σ̂_step jump gate smooths the per-
            // epoch estimation noise the re-base leaves, so the emitter's (unchanged)
            // jump test fires only at genuine events — clearing the threshold-in-noise
            // discontinuity floor.  Filters EXACTLY the keys the re-base put into the
            // emission frame (rb_out); legacy/gated/GLONASS keys keep their raw value
            // + the emitter's normal jump test.  A reference-path slip (rb_slipped_ref,
            // the same set the re-base used) forces a reset that PASSES THE RAW VALUE
            // THROUGH, so the genuine step survives to the emitter — never laundered
            // (invariant #1).  Baked into sol BEFORE emitToStore + the on-disk write,
            // so a NetworkProductIO replay reproduces the emitter's disc counters with
            // no filter rerun (the transform is in the product, like the re-base).
            if (emit_pb_filter_on) {
                const double pbf_epoch_s =
                    sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;
                std::vector<NetworkPbEmitFilter::Entry> pf_entries;
                pf_entries.reserve(rb_out.size());
                for (auto& s : sol.sats) {
                    if (s.sat.empty() || s.sat.front() == 'R') continue;   // GLONASS excluded (FDMA)
                    for (auto& [signal, npb] : s.phase_bias) {
                        if (!rb_out.count(s.sat + "|" + signal)) continue;  // only re-based keys
                        NetworkPbEmitFilter::Entry e;
                        e.sat = s.sat; e.signal = signal;
                        e.value_m = npb.bias_m;
                        e.slipped = rb_slipped_ref.count({s.sat, signal}) != 0;
                        pf_entries.push_back(std::move(e));
                    }
                }
                pb_filter_engine.filterEpoch(pf_entries, pbf_epoch_s);
                std::map<std::string, std::pair<double, double>> pf_out;  // key -> (filtered value, posterior σ)
                for (const auto& e : pf_entries)
                    pf_out[e.sat + "|" + e.signal] = {e.value_m, e.post_sigma_m};
                for (auto& s : sol.sats)
                    for (auto& [signal, npb] : s.phase_bias) {
                        const auto it = pf_out.find(s.sat + "|" + signal);
                        if (it == pf_out.end()) continue;
                        npb.bias_m = it->second.first;
                        if (pb_filter_emit_sigma) npb.sigma_m = it->second.second;
                    }
            }
        }
        emitter.emitToStore(sol, network_store);
        // RTA-5: periodic S-contract re-broadcast — a CURRENT S line (refreshed
        // realized pivots) every N seconds of PRODUCT time, and additionally on any
        // epoch whose emit carried a datum change (markDatumChange fired above), so a
        // late joiner sees the contract and a post-handover reader sees the pivots the
        // product is NOW realized in.  Pivots refresh from last_pivot — the merged
        // emit-baseline map that advanced just above, paired with this epoch's bump:
        // it holds this epoch's realized pivot for every present constellation and the
        // last-EMITTED pivot for an absent one, i.e. exactly the datum of the product
        // as of this emit.  pending_contract is refreshed in place so the end-of-run
        // round-trip self-check compares against the LAST written contract.  Written
        // BEFORE the epoch block so the S line positionally precedes the epochs it
        // describes; the elapsed>0 guard keeps the first-emit epoch single-S (the
        // header line just written).  OFF (0) ⇒ no extra S lines (byte-identical).
        if (contract_rebroadcast_s > 0.0 && product_writer && nc.serialize_sbasis_contract
            && last_contract_emit_s >= 0.0 && pl_now_s > last_contract_emit_s) {
            const bool due = (pl_now_s - last_contract_emit_s) >= contract_rebroadcast_s;
            // Card 3 (D4): a datum-receiver (root) handover re-references the whole
            // component's biases by a common fractional + per-sat integers — a
            // materially softer event than a clock-pivot handover, but still a
            // datum break the consumer should re-sync the contract on.
            const std::string cur_pdr = (phase_datum_eq34 && !eq34_datum_rx.empty())
                                          ? eq34_datum_rx.begin()->second : std::string{};
            const bool eq34_root_changed = phase_datum_eq34 && cur_pdr != eq34_emitted_pdr;
            if (due || datum_changed || eq34_root_changed) {
                for (const auto& [cst, sat] : last_pivot)
                    pending_contract.decl.clock_pivot_sat[cst] = sat;   // refresh realized pivots
                if (phase_datum_eq34) {
                    pending_contract.decl.eq34_datum_receiver = cur_pdr;   // refresh pdr=
                    eq34_emitted_pdr = cur_pdr;
                }
                product_writer->writeContract(pending_contract);
                last_contract_emit_s = pl_now_s;
            }
        }
        if (product_writer) {
            // RTA-10: the emitted epoch's heartbeat, written immediately BEFORE its epoch
            // block (the S-line positional precedent — an H describes what follows).
            // status=partial iff any constellation was withheld from this emission (the
            // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out);
            // sigma0 = the global within-epoch σ̂0 (0 when the within-epoch RAIM did not
            // run this epoch — "unavailable" per the H grammar); n_sats counts the
            // POST-prune emission.  Never feeds replay; flushed with the epoch below.
            if (emit_epoch_status) {
                NetworkProducerStatus hst;
                hst.time.week        = se.time.week;
                hst.time.tow         = se.time.tow;
                hst.state            = epoch_withheld_consts.empty()
                                           ? NetworkProducerStatus::State::Ok
                                           : NetworkProducerStatus::State::Partial;
                hst.sigma0           = sigma0_adapt;
                hst.n_sats_published = static_cast<int>(sol.sats.size());
                if (epoch_withheld_consts.empty()) {
                    hst.withheld = "-";
                } else {
                    hst.withheld.clear();
                    for (char cst : epoch_withheld_consts) {
                        if (!hst.withheld.empty()) hst.withheld += ';';
                        hst.withheld += cst;
                    }
                }
                product_writer->writeEpochStatus(hst);
            }
            const bool any_bump = global_bump || !serialized_consts.empty();
            product_writer->write(sol, any_bump, global_bump ? std::set<char>{} : serialized_consts,
                                  retro_emit_sats,
                                  // RTA-4: post-emit counter source — emitToStore ran
                                  // above, so the emitter's counters ARE this epoch's
                                  // store-record values.
                                  ser_disc_ctrs ? &emitter : nullptr);
            // RTA-6: per-epoch flush so a live tail consumer (NetworkProductTail)
            // sees this epoch block now, not at an ofstream-buffer boundary.
            // Covers the RTA-5 S re-broadcast written above too (same stream).
            // Changes no bytes; default-off toggle discipline.
            if (nc.product_flush_per_epoch) product_writer->flush();
        }
        if (n_emitted == 0) first_emit_content_s = pl_now_s;   // RTB-7(c): grade-metric anchor
        ++n_emitted;
        last_emit_content_s = pl_now_s;   // RTB-6: content time of this emit (health last-emit age)
        // R12: commit the B8/T2 within-epoch-trigger flag-rate now that the epoch has EMITTED
        // (withheld/skipped epochs never reach here, so T2's denominator = emitted epochs, the
        // same population T3–T6 count over).  Guarded by trace_flag_rates ⇒ OFF byte-identical.
        if (trace_flag_rates && we_t2_opp) {
            ++fr_we_trigger.opp;
            if (we_t2_flag) ++fr_we_trigger.flag;
        }

        // --- Closed-loop consume: held-out station vs the fresh network store -
        // Run the consumer ONLY now that this epoch's products are in the store,
        // so its SSR clock + phase bias reflect the just-solved network solution.
        if (closed_loop_on && consumer) {
            auto cit = se.by_station.find(cl.station.name);
            if (cit != se.by_station.end() && !cit->second.empty()) {
                // --- Supply this epoch's SH-VTEC slant ionosphere to the consumer -
                // Evaluate the just-solved global VTEC field at the consumer's own
                // pierce points and push an UNDIFFERENCED per-satellite slant-iono
                // value (metres on L1, Odijk-2016 Table 11) into the store BEFORE
                // the consumer reads it, so it applies the tight ionosphere-weighted
                // prior in place of floating its slant iono.  Reuses the producer's
                // own SH→slant machinery (appendShIonoRows) at the consumer geometry.
                // OFF (default) ⇒ nothing pushed, consumer applies nothing (byte-
                // identical).  In-process only — not part of the on-disk product.
                if (cl.consume_network_products && cl.consume_network_iono && sh_model) {
                    const auto& iw = config_.getIonosphereWeightingConfig();
                    const double Re = shc.earth_radius_km * 1000.0;
                    const double H  = shc.layer_height_km * 1000.0;
                    const double tecu_to_L1m =
                        40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
                    const double lon_sun = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
                    // SH coefficient vector from the shared solution (guard the
                    // column lookups, as elsewhere in this file).
                    const auto& coeffs = sh_model->coeffs();
                    Eigen::VectorXd coeff(static_cast<Eigen::Index>(coeffs.size()));
                    bool coeff_ok = true;
                    for (std::size_t k = 0; k < coeffs.size() && coeff_ok; ++k) {
                        try {
                            coeff(static_cast<Eigen::Index>(k)) = x_s(
                                est.vtecCoeffColumn(coeffs[k].n, coeffs[k].m, coeffs[k].trig));
                        } catch (...) { coeff_ok = false; }
                    }
                    if (coeff_ok) {
                        // The consumer's raw RINEX carries no geometry; harvest each
                        // satellite's ECEF (receiver-independent at IPP accuracy) from
                        // the network's already-modeled obs this epoch.
                        std::map<std::string, Eigen::Vector3d> sat_pos;
                        for (const auto& mv : modeled)
                            for (const auto& o : mv)
                                if (!o.satellite.empty() && o.sat_pos.allFinite())
                                    sat_pos.emplace(o.satellite, o.sat_pos);
                        // T3f: SH-coefficient covariance + column map + fresh-OSB set (built ONLY
                        // when the formal-σ supply is armed; unused/empty otherwise).
                        std::vector<Eigen::Index> sh_cols;
                        std::set<std::string>     fresh_osb;
                        const Eigen::MatrixXd*    shcov_p = nullptr;
                        if (nc.iono_supply_formal_sigma) {
                            sh_cols.assign(coeffs.size(), -1);
                            for (std::size_t k = 0; k < coeffs.size(); ++k)
                                try { sh_cols[k] = est.vtecCoeffColumn(
                                          coeffs[k].n, coeffs[k].m, coeffs[k].trig); }
                                catch (...) {}
                            shcov_p = &est.sharedCovariance();
                            if (config_.estimateNetworkSatCodeBias())
                                for (const auto& s : sol.sats)
                                    if (!s.code_bias_m.empty()) fresh_osb.insert(s.sat);
                        }
                        int n_iono = 0;
                        for (const auto& o : cit->second) {
                            if (o.satellite.empty() || o.satellite.front() == 'R')
                                continue;   // GPS/Galileo only — the SH field excludes GLONASS
                            const auto pit = sat_pos.find(o.satellite);
                            if (pit == sat_pos.end()) continue;
                            // T3f: skip the supply for a sat without FRESH OSB coverage this epoch
                            // — the SH-implied slant leans on the code-bias observability, so an
                            // OSB-less sat's field value is not trustworthy enough to inject.
                            if (nc.iono_supply_formal_sigma
                                && config_.estimateNetworkSatCodeBias()
                                && !fresh_osb.count(o.satellite)) continue;
                            const IppGeometry g = computeIppGeometry(cl_truth, pit->second, Re, H);
                            if (!g.valid) continue;
                            if (g.elevation_rad * 180.0 / Constants::PI < iw.elevation_cutoff_deg)
                                continue;
                            const double lon_sf = g.ipp_lon_rad - lon_sun;
                            const double vtec   = sh_model->vtec(coeff, g.ipp_lat_rad, lon_sf);
                            SsrSlantIono si;
                            si.sat           = o.satellite;
                            si.iono_m        = vtec * g.mapping * tecu_to_L1m;   // slant on L1 (m)
                            const double map = g.mapping * tecu_to_L1m;          // VTEC(TECU)→slant(m)
                            double sigma = std::max(iw.floor_m,
                                                    shc.representation_sigma_tecu * map);
                            if (nc.iono_supply_formal_sigma && shcov_p) {
                                // T3f: RSS the SH-coefficient FORMAL σ onto the representativeness
                                // floor.  aᵀ Q_sh a (a = ∂VTEC/∂coeff basis row, Q_sh = SH coeff
                                // covariance, TECU²) → VTEC variance; ×map → slant metres.
                                std::vector<double> a;
                                sh_model->basis(g.ipp_lat_rad, lon_sf, a);
                                double aQa = 0.0;
                                for (std::size_t j = 0; j < sh_cols.size(); ++j)
                                    for (std::size_t k = 0; k < sh_cols.size(); ++k)
                                        if (sh_cols[j] >= 0 && sh_cols[k] >= 0
                                            && sh_cols[j] < shcov_p->rows()
                                            && sh_cols[k] < shcov_p->cols())
                                            aQa += a[j] * (*shcov_p)(sh_cols[j], sh_cols[k]) * a[k];
                                aQa = std::max(0.0, aQa);   // guard tiny negatives
                                const double repr   = shc.representation_sigma_tecu * map;
                                const double formal = map * std::sqrt(aQa);
                                sigma = std::max(iw.floor_m, std::sqrt(repr * repr + formal * formal));
                            }
                            si.sigma_m       = sigma;
                            si.received_time = se.time;
                            network_store.pushSlantIono(si);
                            ++n_iono;
                        }
                        if (n_emitted == 1 || n_emitted % 60 == 0)
                            log->info("Network closed-loop: supplied SH-VTEC slant iono for {} "
                                      "consumer satellite(s) (TOW {:.1f}).", n_iono, se.time.tow);
                    }
                }
                consumer->processEpoch(cit->second);
                ++cl_epochs;

                const Eigen::Vector3d pos = consumer->getPosition();
                const Eigen::Vector3d d   = pos - cl_truth;
                const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, cl_truth_llh);
                const double err3d = d.norm();

                const bool ar_ran = consumer->lastArRan();
                const AmbiguityResolution& ar = consumer->lastArResult();
                const bool fixed = ar_ran && ar.success;
                if (ar_ran) ++cl_ar_ran;
                if (fixed) {
                    ++cl_ar_fixed;
                    cl_sum_fixed += ar.num_fixed;
                    if (cl_first_fix_tow < 0.0) {
                        cl_first_fix_tow = se.time.tow; cl_first_fix_week = se.time.week;
                    }
                }
                // Converged-segment position RMS: rolling window over the most
                // recent epochs (the float transient + any early wrong-fix
                // excursions are not a fair measure of steady-state quality).
                cl_recent_err3d.push_back(err3d);
                if (cl_recent_err3d.size() > kClRecentWindow) cl_recent_err3d.pop_front();

                if (cl_trace) {
                    // T2c: attribute WHICH gate zeroed AR this epoch.  gate_reason is set at
                    // the pre-MLAMBDA early gates; reason 0 means MLAMBDA was reached, so the
                    // outcome is read from success/ratio (fixed / ratio-battery reject / a
                    // degenerate MLAMBDA that produced no ratio).
                    std::string ar_gate;
                    if (!ar_ran) {
                        ar_gate = "not_run";
                    } else switch (ar.gate_reason) {
                        case 1:  ar_gate = "disabled";      break;
                        case 2:  ar_gate = "min_epochs";    break;
                        case 3:  ar_gate = fmt::format("sigma_gate({:.3f}m)", ar.gate_sigma_3d); break;
                        case 4:  ar_gate = "no_candidates"; break;
                        default: ar_gate = ar.success ? "fixed"
                                         : (ar.ratio > 0.0 ? "ratio_battery" : "mlambda_fail");
                                 break;
                    }
                    cl_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                             << cit->second.size() << ',' << (ar_ran ? 1 : 0) << ','
                             << (fixed ? 1 : 0) << ',' << (ar_ran ? ar.num_fixed : 0) << ','
                             << (ar_ran ? ar.num_candidates : 0) << ','
                             << fmt::format("{:.2f}", ar_ran ? ar.ratio : 0.0) << ','
                             << fmt::format("{:.4f}", err3d) << ','
                             << fmt::format("{:.4f}", enu.x()) << ','
                             << fmt::format("{:.4f}", enu.y()) << ','
                             << fmt::format("{:.4f}", enu.z()) << ','
                             << ar_gate << '\n';
                }
            }
        }

        // Per-satellite product trace (after emit, so discontinuity counters are current).
        if (soln) {
            for (const auto& s : sol.sats) {
                if (s.has_clock)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",CLK,," << fmt::format("{:.4f}", s.clock_c0_m)
                         << ',' << emitter.clockDiscontinuityCounter(s.sat) << '\n';  // A2: 0 when off (byte-identical)
                for (const auto& [signal, pb] : s.phase_bias)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",PB," << signal << ',' << fmt::format("{:.4f}", pb.bias_m)
                         << ',' << emitter.discontinuityCounter(s.sat, signal) << '\n';
                for (const auto& [signal, cb] : s.code_bias_m)   // W6 satellite code bias (OSB)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",CB," << signal << ',' << fmt::format("{:.4f}", cb)
                         << ',' << emitter.codeBiasDiscontinuityCounter(s.sat, signal) << '\n';
            }
        }
        subEnd(SB_EMITSTORE);   // D0b: close block 7 (emit/store, order-sensitive serial)
        subBegin(SB_IONODIAG);  // D0b block 4a: iono / ZWD / anchor-info viz traces

        // --- Network-estimated slant ionosphere (viz trace) -----------------
        // Back-substitute each present station's local nuisance block (the slant
        // iono lives there, an absolute per-epoch estimate with linearization
        // point 0) and emit one row per (station, satellite) at the measured
        // elevation/azimuth, so the pyviz viewer can map each value to its
        // ionospheric pierce point.  Gated by network.trace_ionosphere.
        if (iono_trace.is_open()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;   // not added to this epoch's solve
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                // Elevation/azimuth per satellite from the modeled observations
                // (first observation of each satellite carries the geometry).
                std::map<std::string, std::pair<double, double>> elaz;
                for (const auto& o : modeled[i])
                    elaz.emplace(o.satellite, std::make_pair(o.elevation, o.azimuth));
                const auto& h2l = asm_res[i].h2local;
                for (const auto& kv : stations[i].sm->getIonosphereStates()) {
                    const auto hit = h2l.find(static_cast<int>(kv.second));
                    if (hit == h2l.end()) continue;
                    if (hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size())
                        continue;
                    const auto e = elaz.find(kv.first);
                    if (e == elaz.end()) continue;
                    iono_trace << se.time.week << ','
                               << fmt::format("{:.3f}", se.time.tow) << ','
                               << stations[i].name << ',' << kv.first << ','
                               << fmt::format("{:.4f}", x_r(hit->second)) << ','
                               << fmt::format("{:.2f}", e->second.first) << ','
                               << fmt::format("{:.2f}", e->second.second) << '\n';
                }
            }
        }

        // --- Per-station ZWD trace (L7) -------------------------------------
        // One row per present station each emitted epoch: the estimated ZWD (a-priori state +
        // reconstructed correction) and its datum-conditioned formal σ (the V7 border-sharpening
        // instrument).  Independent of the carry — locate the ZWD column via getTropoIndex /
        // h2local, NOT R.pl_zwd_present (that is only set under carry_warm && zwd_on, so run A of
        // the V7 A/B and warmup would otherwise emit no rows).
        if (zwd_trace.is_open()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;               // not in this epoch's solve
                const size_t tropo_idx_sz = stations[i].sm->getTropoIndex();
                if (tropo_idx_sz == std::numeric_limits<size_t>::max()) continue;  // no ZWD state
                const int tropo_idx = static_cast<int>(tropo_idx_sz);
                const auto& h2l = asm_res[i].h2local;
                const auto zhit = h2l.find(tropo_idx);
                if (zhit == h2l.end() || zhit->second < 0) continue;   // ZWD not in A_r this epoch
                const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                Eigen::VectorXd x_r, vard;
                try {
                    x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        static_cast<std::size_t>(est_index_of[i]), {zcol}, plan.shared_datum);
                } catch (...) { continue; }
                if (zcol >= x_r.size()) continue;
                const double x_lin = stations[i].sm->getStateVector()(tropo_idx);  // ≡0.0 today; kept for robustness
                const double zwd_m = x_lin + x_r(zcol);
                const double var   = (vard.size() > 0) ? vard(0) : -1.0;
                if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(zwd_m)) continue;
                zwd_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                          << stations[i].name << ',' << fmt::format("{:.6f}", zwd_m) << ','
                          << fmt::format("{:.6e}", std::sqrt(var)) << ','
                          << (asm_res[i].pl_zwd_injected ? 1 : 0) << '\n';
            }
        }

        // --- A-priori-clock-anchor information-fraction diagnostic (B7) ------
        // Certify the estimable network clock is DATA-dominated, not defined by the
        // a-priori CODE product it is softly anchored to.  anchor_info is the constant
        // Tikhonov pull (1/σ²); total_info is the reduced-normals diagonal on the clock
        // column (data + carry prior + datum pin).  P1-4: the legacy anchor/total fraction
        // is MISLEADING for a locked-in (carry-dominated) clock — its huge carry info
        // shrinks the fraction and certifies "data-dominated" for a column that is really
        // defined by its own past.  Decompose the denominator: subtract the carried
        // clock-prior info (clk_prior_info, folded this epoch) and the anchor, leaving
        // data_info; report anchor_frac_data = anchor/(anchor+data_info) — independent of
        // carry stiffness — plus carry_frac = carry/total, which lights up on a lock-in.
        // Read-only; the anchor fold itself is unchanged.
        if (anchor_info_trace.is_open()) {
            const double clk_sigma_a = clk_anchor_sigma;  // D6-resolved (matches the fold site above)
            const bool anchor_active =
                std::isfinite(clk_sigma_a) && clk_sigma_a > 0.0 && clk_sigma_a < 100.0;
            const double anchor_info =
                anchor_active ? 1.0 / (clk_sigma_a * clk_sigma_a) : 0.0;
            const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                if (col < 0 || col >= Nss.rows()) continue;
                const double total_info = Nss(col, col);
                const auto cpi = clk_prior_info.find(sat);
                const double carry_info = (cpi != clk_prior_info.end()) ? cpi->second : 0.0;
                // Data information = everything not the carry prior or the anchor.  For a
                // pivot the datum pin is ALSO in total_info (is_pivot flags it — its
                // decomposition is pin-dominated and not a data-dominance statement).
                const double data_info = std::max(0.0, total_info - carry_info - anchor_info);
                const double denom_ad  = anchor_info + data_info;
                const double anchor_frac_data = (denom_ad > 0.0) ? (anchor_info / denom_ad) : 0.0;
                const double carry_frac = (total_info > 0.0) ? (carry_info / total_info) : 0.0;
                const double frac = (total_info > 0.0) ? (anchor_info / total_info) : 0.0;
                const auto pit = plan.clock_pivot.find(NetworkSbasis::constellationOf(sat));
                const bool is_pivot = (pit != plan.clock_pivot.end() && pit->second == sat);
                anchor_info_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << sat << ',' << fmt::format("{:.6e}", anchor_info) << ','
                    << fmt::format("{:.6e}", carry_info) << ','
                    << fmt::format("{:.6e}", data_info) << ','
                    << fmt::format("{:.6e}", total_info) << ','
                    << fmt::format("{:.6f}", anchor_frac_data) << ','
                    << fmt::format("{:.6f}", carry_frac) << ','
                    << fmt::format("{:.6f}", frac) << ',' << (is_pivot ? 1 : 0) << '\n';
            }
        }
        subEnd(SB_IONODIAG);   // D0b: close block 4a (iono/ZWD/anchor traces)
        subBegin(SB_DDFRAC);   // D0b block 6: cross-station DD fractional-residual (B5) trace

        // --- Cross-station DD fractional-residual (B5) ----------------------
        // For each present station, back-substitute its local block and read each
        // ABSOLUTE float ambiguity (x_lin + x_r(col), m) per (sat,signal).  Within a
        // (const,signal), difference each sat against a reference sat (between-sat SD),
        // then difference that across station pairs → the between-station DD.  A DD near
        // an integer #cycles is evidence the float biases support user AR.  P1-2: the
        // fractional cycle (dd_cyc = dd_m/λ, frac_cyc = dd_cyc − round) is computed HERE
        // so the CSV is a ready fractional histogram.  EXCLUDED from the pool: the pivot
        // station (its ambiguities are pinned to zero — a degenerate DD) and GLONASS
        // (FDMA: no common λ per signal, between-sat SD not integer).  Read-only; gated.
        if (dd_frac_trace.is_open()
            && ((n_loop_epochs - 1)
                % static_cast<size_t>(std::max(1, nc.trace_dd_fractional_epoch_stride)) == 0)) {  // T2e stride
            auto sysOfChar = [](char c) -> System {
                switch (c) {
                    case 'G': return System::GPS;      case 'E': return System::Galileo;
                    case 'C': return System::BeiDou;   case 'J': return System::QZSS;
                    case 'S': return System::SBAS;     default:  return System::Unknown;
                }
            };
            struct AmbAbs { double value_m; std::size_t est_id; Eigen::Index col; };  // T2a: +est_id,col
            // (const,signal) -> sat -> station index -> absolute float ambiguity (m) plus its
            // (estimator station id, local A_r column) for the T2a joint-covariance request.
            std::map<std::pair<std::string, std::string>,
                     std::map<std::string, std::map<size_t, AmbAbs>>> pool;
            const auto t_dd_pool0 = std::chrono::steady_clock::now();   // T2b timers
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                // Legacy: skip the whole pivot (all its ambiguities are pinned to
                // zero → degenerate DD).  Prioritized: keep the station and skip only
                // its per-(sat,signal) anchored (pinned) ambiguities below.
                if (!phase_datum_prioritized && stations[i].is_pivot) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                const auto& h2l = asm_res[i].h2local;
                const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    if (isGaugePinned(i, parts->satellite, parts->signal))
                        continue;   // anchored (pinned to 0) → degenerate DD
                    const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                    if (cst == "R") continue;   // GLONASS FDMA: excluded (no common λ / integer DD)
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                    const auto hit = h2l.find(amb_idx);
                    if (hit == h2l.end()) continue;   // ambiguity column not in A_r this epoch
                    if (hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const double value_m = x_lin(amb_idx) + x_r(hit->second);
                    if (!std::isfinite(value_m)) continue;
                    pool[{cst, parts->signal}][parts->satellite][i] =
                        AmbAbs{value_m, static_cast<std::size_t>(est_index_of[i]),
                               static_cast<Eigen::Index>(hit->second)};
                }
            }
            const auto t_dd_pool1 = std::chrono::steady_clock::now();
            // --- T2a/T2b: collect the between-station DDs, (optionally) subsample, then
            //     propagate the joint conditional covariance ONCE to attach a per-DD formal σ
            //     (cycles).  The DD variance is gᵀ Q g with g=[+1,−1,−1,+1] over the four undiff
            //     ambiguities (sta_a/sat, sta_a/ref, sta_b/sat, sta_b/ref); only the diagonal is
            //     needed, so no dense nd×nd differencing matrix is materialised.  dd_m / dd_cyc /
            //     frac_cyc are computed EXACTLY as before (benchmark-identical); dd_sigma_cyc is
            //     appended (−1 = covariance unavailable this epoch).
            struct DdOut {
                std::string cons, signal, sat, ref;
                std::size_t sta_a_i, sta_b_i;
                double dd_m, lambda, dd_cyc, frac_cyc;
                std::pair<std::size_t, Eigen::Index> amb[4];  // ia(sat), ra(ref)@a, ib(sat), rb(ref)@b
                int rq[4]{ -1, -1, -1, -1 };                  // request indices, filled post-subsample
            };
            std::vector<DdOut> dds;
            for (auto& [cs, satmap] : pool) {
                std::string ref;   // reference sat = lexically-lowest sat present at ≥2 stations
                for (const auto& [sat, stamap] : satmap)
                    if (stamap.size() >= 2) { ref = sat; break; }
                if (ref.empty()) continue;
                // λ for this (const,signal).  GLONASS is already excluded, so the nominal
                // per-band wavelength is satellite-independent; 0 (unknown signal) leaves the
                // cycle columns 0 so the reader falls back to the metre column.
                double lambda = 0.0;
                if (dd_ppp_utils) {
                    const System sys = sysOfChar(cs.first.empty() ? '?' : cs.first[0]);
                    if (sys != System::Unknown)
                        lambda = dd_ppp_utils->getWavelength(sys, cs.second, ref);
                }
                const auto& rmap = satmap[ref];
                for (const auto& [sat, stamap] : satmap) {
                    if (sat == ref) continue;
                    for (auto ia = stamap.begin(); ia != stamap.end(); ++ia) {
                        const auto ra = rmap.find(ia->first);
                        if (ra == rmap.end()) continue;
                        const double sd_a = ia->second.value_m - ra->second.value_m;
                        auto ib = ia; ++ib;
                        for (; ib != stamap.end(); ++ib) {
                            const auto rb = rmap.find(ib->first);
                            if (rb == rmap.end()) continue;
                            const double sd_b = ib->second.value_m - rb->second.value_m;
                            const double dd_m = sd_a - sd_b;   // between-station DD (m)
                            const double dd_cyc = (lambda > 0.0) ? (dd_m / lambda) : 0.0;
                            const double frac_cyc =
                                (lambda > 0.0) ? (dd_cyc - std::round(dd_cyc)) : 0.0;
                            DdOut d;
                            d.cons = cs.first; d.signal = cs.second; d.sat = sat; d.ref = ref;
                            d.sta_a_i = ia->first; d.sta_b_i = ib->first;
                            d.dd_m = dd_m; d.lambda = lambda; d.dd_cyc = dd_cyc; d.frac_cyc = frac_cyc;
                            d.amb[0] = { ia->second.est_id, ia->second.col };
                            d.amb[1] = { ra->second.est_id, ra->second.col };
                            d.amb[2] = { ib->second.est_id, ib->second.col };
                            d.amb[3] = { rb->second.est_id, rb->second.col };
                            dds.push_back(std::move(d));
                        }
                    }
                }
            }
            const auto t_dd_collect = std::chrono::steady_clock::now();
            const std::size_t n_all_dd = dds.size();
            // T2b: deterministic seeded subsample keyed on (week,tow) — reproducible per epoch;
            // 0 = unlimited (full-pair benchmark).  Bounds BOTH the emit and the covariance-
            // request count (the dominant 305-station tracer cost).
            const int dd_max_pairs = nc.trace_dd_fractional_max_pairs;
            if (dd_max_pairs > 0 && dds.size() > static_cast<std::size_t>(dd_max_pairs)) {
                std::mt19937 rng(static_cast<unsigned>(se.time.week) * 604800u
                                 + static_cast<unsigned>(se.time.tow));
                std::vector<std::size_t> idx(dds.size());
                for (std::size_t j = 0; j < idx.size(); ++j) idx[j] = j;
                std::shuffle(idx.begin(), idx.end(), rng);
                idx.resize(static_cast<std::size_t>(dd_max_pairs));
                std::sort(idx.begin(), idx.end());   // stable emit order
                std::vector<DdOut> kept; kept.reserve(idx.size());
                for (std::size_t j : idx) kept.push_back(std::move(dds[j]));
                dds.swap(kept);
            }
            // Deduplicated (station,col) request set over the (subsampled) DDs.
            std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
            std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
            auto reqOf = [&](const std::pair<std::size_t, Eigen::Index>& k) -> int {
                auto it = req_idx.find(k);
                if (it != req_idx.end()) return it->second;
                const int r = static_cast<int>(reqs.size());
                reqs.push_back(k); req_idx[k] = r; return r;
            };
            for (auto& d : dds)
                for (int p = 0; p < 4; ++p) d.rq[p] = reqOf(d.amb[p]);
            Eigen::MatrixXd cov;
            if (!reqs.empty()) {
                try { cov = est.jointConditionalCovariance(reqs, plan.shared_datum); }
                catch (...) { cov = Eigen::MatrixXd(); }
            }
            const bool cov_ok =
                (cov.rows() == static_cast<Eigen::Index>(reqs.size()) && cov.rows() > 0);
            const auto t_dd_cov = std::chrono::steady_clock::now();
            for (const DdOut& d : dds) {
                double dd_sigma_cyc = -1.0;   // −1 = covariance unavailable this epoch
                if (cov_ok && d.lambda > 0.0) {
                    static const double g[4] = { +1.0, -1.0, -1.0, +1.0 };
                    double var = 0.0;
                    for (int p = 0; p < 4; ++p)
                        for (int q = 0; q < 4; ++q)
                            var += g[p] * g[q] * cov(d.rq[p], d.rq[q]);
                    dd_sigma_cyc = (var > 0.0) ? std::sqrt(var) / d.lambda : 0.0;
                }
                dd_frac_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << d.cons << ',' << d.signal << ',' << d.sat << ',' << d.ref << ','
                    << stations[d.sta_a_i].name << ',' << stations[d.sta_b_i].name << ','
                    << fmt::format("{:.4f}", d.dd_m) << ','
                    << fmt::format("{:.6f}", d.lambda) << ','
                    << fmt::format("{:.4f}", d.dd_cyc) << ','
                    << fmt::format("{:.4f}", d.frac_cyc) << ','
                    << fmt::format("{:.4f}", dd_sigma_cyc) << '\n';
            }
            const auto t_dd_emit = std::chrono::steady_clock::now();
            auto ms = [](const auto& a, const auto& b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            log->info("Network (T2b) DD-frac tracer TOW {:.1f}: pool {:.0f} ms ({} sta), "
                      "collect {:.0f} ms ({} DD{}), cov {:.0f} ms ({} reqs), emit {:.0f} ms",
                      se.time.tow, ms(t_dd_pool0, t_dd_pool1), stations.size(),
                      ms(t_dd_pool1, t_dd_collect), n_all_dd,
                      (dd_max_pairs > 0 && n_all_dd > dds.size())
                          ? fmt::format(" ->{} kept", dds.size()) : std::string(),
                      ms(t_dd_collect, t_dd_cov), reqs.size(), ms(t_dd_cov, t_dd_emit));
        }
        subEnd(SB_DDFRAC);       // D0b: close block 6 (dd-frac B5)
        subBegin(SB_SHDEV_VTEC); // D0b block 4b: T5 SH-deviation carry KF + VTEC coeff trace

        // --- T5: SH-deviation carry measurement update (SERIAL) --------------
        // The joint posterior's per-LOS pin residual r̂ = ι̂ − SH(ĉ) is the epoch's
        // observation of the GM deviation: blend it in at the pin-floor noise.
        // Approximation (v1, documented): r̂ is treated as dev + ε(floor²) — its own
        // posterior covariance (correlated through the border) is neglected, which
        // slightly UNDERSTATES P; acceptable while the deviation σ is representativeness-
        // dominated.  Insertions happen only here (serial), never in the assembly.
        if (sh_dev_on) {
            const auto& iwh = config_.getIonosphereWeightingConfig();
            const double Re_h = shc.earth_radius_km * 1000.0;
            const double H_h  = shc.layer_height_km * 1000.0;
            const double tecu_to_L1m_h =
                40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
            const double lon_sun_h = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
            const int Kh = sh_model->numCoeffs();
            std::vector<Eigen::Index> shcols_h(static_cast<std::size_t>(Kh));
            bool cols_ok = true;
            for (int k = 0; k < Kh && cols_ok; ++k) {
                const auto& c = sh_model->coeffs()[static_cast<std::size_t>(k)];
                try {
                    shcols_h[static_cast<std::size_t>(k)] =
                        est.vtecCoeffColumn(c.n, c.m, c.trig);
                } catch (...) { cols_ok = false; }
            }
            if (cols_ok) {
                const double now_s = se.time.week * 604800.0 + se.time.tow;
                const double floor_var = iwh.floor_m * iwh.floor_m;
                std::vector<double> bv;
                for (std::size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0) continue;
                    Eigen::VectorXd x_r;
                    try {
                        x_r = est.reconstructStation(
                            static_cast<std::size_t>(est_index_of[i]), x_s);
                    } catch (...) { continue; }
                    const auto& h2l = asm_res[i].h2local;
                    const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                    for (const auto& o : modeled[i]) {
                        if (o.satellite.empty() || o.satellite.front() == 'R') continue;
                        if (o.elevation < iwh.elevation_cutoff_deg) continue;
                        const int iono_idx = stations[i].sm->getIonosphereIndex(o.satellite);
                        if (iono_idx < 0 || iono_idx >= static_cast<int>(x_lin.size())) continue;
                        const auto hit = h2l.find(iono_idx);
                        if (hit == h2l.end()) continue;
                        if (hit->second < 0
                            || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                        const IppGeometry g =
                            computeIppGeometry(stations[i].ecef, o.sat_pos, Re_h, H_h);
                        if (!g.valid) continue;
                        sh_model->basis(g.ipp_lat_rad, g.ipp_lon_rad - lon_sun_h, bv);
                        double sh_hat = 0.0;
                        for (int j = 0; j < Kh; ++j)
                            sh_hat += g.mapping * tecu_to_L1m_h
                                      * bv[static_cast<std::size_t>(j)]
                                      * x_s(shcols_h[static_cast<std::size_t>(j)]);
                        const double iono_hat = x_lin(iono_idx) + x_r(hit->second);
                        const double resid = iono_hat - sh_hat;
                        const double sig_ss = shc.representation_sigma_tecu
                                              * g.mapping * tecu_to_L1m_h;
                        ShDevCarry& dc = sh_dev_carry[stations[i].name + "|" + o.satellite];
                        if (dc.t_s <= 0.0) {              // cold entry: seed at steady state
                            dc.dev_m  = 0.0;
                            dc.var_m2 = sig_ss * sig_ss;
                        }
                        dc.var_ss = sig_ss * sig_ss;
                        const double Kf = dc.var_m2 / (dc.var_m2 + floor_var);
                        dc.dev_m  += Kf * (resid - dc.dev_m);
                        dc.var_m2 *= (1.0 - Kf);
                        dc.t_s     = now_s;
                    }
                }
            }
        }

        // --- Co-estimated SH-VTEC coefficient trace -------------------------
        if (vtec_sh_trace.is_open() && sh_model) {
            const Eigen::MatrixXd& shcov = est.sharedCovariance();
            for (const auto& c : sh_model->coeffs()) {
                Eigen::Index col;
                try { col = est.vtecCoeffColumn(c.n, c.m, c.trig); } catch (...) { continue; }
                const double val = x_s(col);
                const double v   = (col < shcov.rows() && col < shcov.cols())
                                       ? shcov(col, col) : 0.0;
                vtec_sh_trace << se.time.week << ','
                              << fmt::format("{:.3f}", se.time.tow) << ','
                              << c.n << ',' << c.m << ',' << c.trig << ','
                              << fmt::format("{:.4f}", val) << ','
                              << fmt::format("{:.4f}", (v > 0.0) ? std::sqrt(v) : 0.0) << '\n';
            }
        }
        subEnd(SB_SHDEV_VTEC);  // D0b: close block 4b (T5 sh-dev + VTEC trace)
        subBegin(SB_T1A);       // D0b block 3: T1a SH health probe + fused adaptive-deweight gate

        // --- T1a: SH-field health probe + a₀₀ physical-band divergence WARN ---
        // (a) Always-on a₀₀ band WARN — fires whenever the SH field is active, so a
        //     diverging mean VTEC is visible even without the trace armed.  a₀₀ IS the
        //     mean VTEC (TECU) because P̄₀₀ ≡ 1; the SH↔slant misfit that drives the
        //     divergence is otherwise absorbed silently into the ambiguities.  Read-only.
        if (sh_model && shc.a00_warn_low_tecu < shc.a00_warn_high_tecu) {
            double a00 = std::numeric_limits<double>::quiet_NaN();
            try { a00 = x_s(est.vtecCoeffColumn(0, 0, 'C')); } catch (...) {}
            if (std::isfinite(a00)) {
                const bool in_band =
                    (a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu);
                if (!in_band && (sh_a00_was_in_band || n_emitted % 60 == 0)) {
                    log->warn("Network (SH-iono): a₀₀ = {:.1f} TECU OUTSIDE physical band "
                              "[{:.1f}, {:.1f}] — global VTEC field diverging (TOW {:.1f}); the "
                              "SH↔slant misfit is being squeezed into the ambiguities.",
                              a00, shc.a00_warn_low_tecu, shc.a00_warn_high_tecu, se.time.tow);
                } else if (in_band && !sh_a00_was_in_band) {
                    log->info("Network (SH-iono): a₀₀ = {:.1f} TECU returned to physical band "
                              "(TOW {:.1f}).", a00, se.time.tow);
                }
                sh_a00_was_in_band = in_band;
            }
        }

        // (b) Full health metrics + station-adaptive deweight gate — ONE post-solve
        //     SH↔slant sweep feeding both consumers (H1 dedup).
        //     The SH health trace (shc.trace) writes one row per emitted epoch: a₀₀
        //     (+σ), coefficient RMS by degree, that RMS relative to the per-degree Kaula
        //     prior σ (σ_n = kaula/n, σ₀ = a₀₀ prior σ), and the network-wide SH→slant
        //     misfit reconstructed at POSTERIOR geometry (mean/rms/max |residual| +
        //     3σ-exceedance counts over LOS and stations).
        //     The station-adaptive deweight (v2) needs, per present station, the SAME
        //     standardized slant residual r/σ — its per-station RMS √mean((r/σ)²) is the
        //     gate's observe() input.  Both are derived from ONE reconstruct + degree-15
        //     SH evaluation per LOS: the gate accumulator is within-station (order-free),
        //     so folding it in is byte-identical to the old separate loop.  Gate-off (or
        //     a datum-change epoch) ⇒ no observe()/advance() ⇒ byte-identical; and with
        //     the trace on (the lineage recipe) the gate now rides the trace's existing
        //     sweep at ~zero marginal cov-consumer cost (the +303 reconstructStation
        //     calls/ep and duplicate SH sweep are gone).
        const bool sh_want_trace = vtec_sh_health_trace.is_open();
        const bool sh_want_gate  = iono_deweight.adaptive() && !datum_changed;
        if (sh_model && (sh_want_trace || sh_want_gate)) {
            const Eigen::MatrixXd& shcov = est.sharedCovariance();
            const auto& coeffs = sh_model->coeffs();
            Eigen::VectorXd coeff(static_cast<Eigen::Index>(coeffs.size()));
            std::vector<Eigen::Index> ccol(coeffs.size(), -1);
            bool coeff_ok = true;
            for (std::size_t k = 0; k < coeffs.size() && coeff_ok; ++k) {
                try {
                    ccol[k] = est.vtecCoeffColumn(coeffs[k].n, coeffs[k].m, coeffs[k].trig);
                    coeff(static_cast<Eigen::Index>(k)) = x_s(ccol[k]);
                } catch (...) { coeff_ok = false; }
            }
            if (coeff_ok) {
                const int N = sh_model->maxDegree();
                double a00 = 0.0, a00_std = 0.0, all_sumsq = 0.0;
                int all_cnt = 0;
                std::vector<double> deg_sumsq(static_cast<std::size_t>(N + 1), 0.0);
                std::vector<int>    deg_cnt(static_cast<std::size_t>(N + 1), 0);
                for (std::size_t k = 0; k < coeffs.size(); ++k) {
                    const std::size_t n = static_cast<std::size_t>(coeffs[k].n);
                    const double val = coeff(static_cast<Eigen::Index>(k));
                    deg_sumsq[n] += val * val;
                    ++deg_cnt[n];
                    all_sumsq += val * val;
                    ++all_cnt;
                    if (coeffs[k].n == 0 && coeffs[k].m == 0 && coeffs[k].trig == 'C') {
                        a00 = val;
                        const Eigen::Index col = ccol[k];
                        const double vv = (col >= 0 && col < shcov.rows() && col < shcov.cols())
                                              ? shcov(col, col) : 0.0;
                        a00_std = (vv > 0.0) ? std::sqrt(vv) : 0.0;
                    }
                }
                const double rms_all = (all_cnt > 0) ? std::sqrt(all_sumsq / all_cnt) : 0.0;

                // Network-wide SH→slant misfit at posterior geometry (mirrors the
                // producer's own SH row math in appendShIonoRows, but evaluated on the
                // solved coefficients + reconstructed per-station slant states).
                const auto& iw = config_.getIonosphereWeightingConfig();
                const double Re = shc.earth_radius_km * 1000.0;
                const double H  = shc.layer_height_km * 1000.0;
                const double tecu_to_L1m =
                    40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
                const double lon_sun = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
                double res_sum = 0.0, res_sumsq = 0.0, res_max = 0.0;
                int n_los = 0, n_los_gt3 = 0, n_sta = 0, n_sta_gt3 = 0;
                for (std::size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0) continue;
                    Eigen::VectorXd x_r;
                    try {
                        x_r = est.reconstructStation(
                            static_cast<std::size_t>(est_index_of[i]), x_s);
                    } catch (...) { continue; }
                    std::map<std::string, std::pair<double, Eigen::Vector3d>> geom;
                    for (const auto& o : modeled[i])
                        if (!o.satellite.empty() && o.sat_pos.allFinite())
                            geom.emplace(o.satellite,
                                         std::make_pair(o.elevation, o.sat_pos));
                    const auto& h2l = asm_res[i].h2local;
                    bool sta_present = false, sta_gt3 = false;
                    double zz_gate = 0.0; int nn_gate = 0;   // per-station Σ(r/σ)², count (deweight gate)
                    for (const auto& kv : stations[i].sm->getIonosphereStates()) {
                        if (kv.first.empty() || kv.first.front() == 'R') continue;  // SH excludes GLONASS
                        const auto gt = geom.find(kv.first);
                        if (gt == geom.end()) continue;
                        if (gt->second.first < iw.elevation_cutoff_deg) continue;
                        const auto hit = h2l.find(static_cast<int>(kv.second));
                        if (hit == h2l.end() || hit->second < 0
                            || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                        const IppGeometry g =
                            computeIppGeometry(stations[i].ecef, gt->second.second, Re, H);
                        if (!g.valid) continue;
                        const double lon_sf = g.ipp_lon_rad - lon_sun;
                        const double sh_slant =
                            sh_model->vtec(coeff, g.ipp_lat_rad, lon_sf) * g.mapping * tecu_to_L1m;
                        const double resid = x_r(hit->second) - sh_slant;
                        if (!std::isfinite(resid)) continue;
                        const double sig = std::max(
                            iw.floor_m, shc.representation_sigma_tecu * g.mapping * tecu_to_L1m);
                        // Deweight gate: this LOS's standardized residual (same sig>0
                        // guard the old separate loop used before dividing).
                        if (sh_want_gate && sig > 0.0) {
                            const double sr = resid / sig;
                            zz_gate += sr * sr; ++nn_gate;
                        }
                        if (!sh_want_trace) continue;   // gate-only: skip trace aggregates
                        const double ar = std::fabs(resid);
                        res_sum += ar;
                        res_sumsq += resid * resid;
                        if (ar > res_max) res_max = ar;
                        ++n_los;
                        sta_present = true;
                        if (sig > 0.0 && ar > 3.0 * sig) { ++n_los_gt3; sta_gt3 = true; }
                    }
                    if (sta_present) ++n_sta;
                    if (sta_gt3) ++n_sta_gt3;
                    // Feed the deweight gate this station's standardized-residual RMS
                    // (serial station order — same as the old separate loop).
                    if (sh_want_gate && nn_gate > 0)
                        iono_deweight.observe(stations[i].name, std::sqrt(zz_gate / nn_gate));
                }
                const double res_mean = (n_los > 0) ? res_sum / n_los : 0.0;
                const double res_rms  = (n_los > 0) ? std::sqrt(res_sumsq / n_los) : 0.0;
                const bool a00_in_band =
                    (a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu);

                vtec_sh_health_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << fmt::format("{:.3f}", a00) << ','
                    << fmt::format("{:.3f}", a00_std) << ','
                    << fmt::format("{:.4f}", rms_all) << ','
                    << fmt::format("{:.4f}", res_mean) << ','
                    << fmt::format("{:.4f}", res_rms) << ','
                    << fmt::format("{:.4f}", res_max) << ','
                    << n_los << ',' << n_los_gt3 << ','
                    << n_sta << ',' << n_sta_gt3 << ','
                    << (a00_in_band ? 1 : 0);
                for (int n = 0; n <= N; ++n) {
                    const std::size_t ni = static_cast<std::size_t>(n);
                    const double rms = (deg_cnt[ni] > 0)
                        ? std::sqrt(deg_sumsq[ni] / deg_cnt[ni]) : 0.0;
                    vtec_sh_health_trace << ',' << fmt::format("{:.4f}", rms);
                }
                for (int n = 0; n <= N; ++n) {
                    const std::size_t ni = static_cast<std::size_t>(n);
                    const double rms = (deg_cnt[ni] > 0)
                        ? std::sqrt(deg_sumsq[ni] / deg_cnt[ni]) : 0.0;
                    const double ksig = (n == 0)
                        ? shc.mean_vtec_prior_sigma_tecu
                        : shc.kaula_scale_tecu / static_cast<double>(n);
                    vtec_sh_health_trace
                        << ',' << fmt::format("{:.4f}", (ksig > 0.0) ? rms / ksig : 0.0);
                }
                vtec_sh_health_trace << '\n';
                // Advance the deweight gate one clean epoch (EWMA + robust z +
                // hysteresis + slew).  Gate-off ⇒ never runs ⇒ byte-identical.
                if (sh_want_gate) iono_deweight.advance(true);
            }
        }

        // NOTE (H1 dedup): the station-adaptive SH-VTEC-constraint deweight gate
        // update USED to live here as a SECOND per-station reconstruct + degree-15
        // SH sweep, identical to the health-trace sweep above (its own comment
        // conceded "the SAME resid/σ").  It added +303 reconstructStation
        // cov-consumer calls/ep plus a duplicate SH-eval sweep.  It is now FUSED
        // into the single sweep above: observe() per station + advance() once, from
        // the same per-LOS resid/σ.  See the merged block guarded on
        // (sh_want_trace || sh_want_gate).

        // M2 (LS-VCE): advance the carry-variance monitor for this emitted epoch —
        // apply exponential forgetting and set the clean-epoch gate (a datum /
        // pivot handover contaminates every carried prior, so the whole epoch's
        // rows are dropped).  On the content-hour roll, flush the just-completed
        // hour's σ̂ for every (family, constellation) to network_carry_vce.csv.
        // All gated on carry_vce ⇒ OFF is byte-identical.
        if (carry_vce) {
            if (first_vce_s < 0.0) first_vce_s = pl_now_s;
            const int cur_hour = static_cast<int>((pl_now_s - first_vce_s) / 3600.0);
            if (cur_hour != last_vce_hour) {
                if (last_vce_hour >= 0 && carry_vce_trace.is_open()) {
                    for (const auto& fc : carry_vce_est.keys()) {
                        const auto e = carry_vce_est.estimate(fc.first, fc.second);
                        const double cfg_floor = (fc.first == "pb") ? pb_var_floor
                                               : (fc.first == "cb") ? cb_var_floor
                                                                    : pl_var_floor;
                        const double rrat = (cfg_floor > 0.0) ? e.robust_floor / cfg_floor : 0.0;
                        const double mrat = (cfg_floor > 0.0)
                            ? e.sigma[NetworkCarryVce::kFloor] / cfg_floor : 0.0;
                        carry_vce_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                            << ',' << last_vce_hour << ',' << fc.first << ',' << fc.second
                            << ',' << e.n_rows << ',' << fmt::format("{:.1f}", e.n_eff)
                            << ',' << fmt::format("{:.6e}", e.robust_floor)
                            << ',' << fmt::format("{:.6e}", e.sigma[NetworkCarryVce::kFloor])
                            << ',' << fmt::format("{:.6e}", e.std[NetworkCarryVce::kFloor])
                            << ',' << fmt::format("{:.6e}", e.sigma[NetworkCarryVce::kRate])
                            << ',' << fmt::format("{:.6e}", e.std[NetworkCarryVce::kRate])
                            << ',' << fmt::format("{:.3e}", e.cond)
                            << ',' << (e.rate_frozen ? 1 : 0)
                            << ',' << fmt::format("{:.6e}", cfg_floor)
                            << ',' << fmt::format("{:.4f}", rrat)
                            << ',' << fmt::format("{:.4f}", mrat)
                            << ',' << e.floor_clamped << ',' << e.rate_clamped << '\n';
                    }
                    carry_vce_trace.flush();
                }
                last_vce_hour = cur_hour;
            }
            carry_vce_est.beginEpoch(pl_now_s, !datum_changed);
        }
        subEnd(SB_T1A);        // D0b: close block 3 (T1a health + deweight)
        subBegin(SB_LEVELING); // D0b block 5: phase-leveling carry harvest + T1d SH-convergence

        // --- Phase-leveling carry: harvest posterior + stash (Increment 1) ----
        // For each present non-pivot station, recover its ambiguities' ABSOLUTE
        // values (x_lin + back-substituted Δx) and their conditional variances
        // (the cheap per-column diagonal harvest, NOT the full per-station inverse)
        // and refresh the persistent carry.  Then drop entries unseen beyond the
        // max gap (a stale-carry / map-growth guard — distinct from the slip-driven
        // REFLOAT/DROP of later increments).
        if (carry_warm) {
            // Card 1 (carry_conditional_coupling): build, ONCE per epoch, the shared
            // column → identity descriptor map and the datum-frame marginal-variance
            // diagonal, so each carried local's regression on the FREE shared border can
            // be stashed in a layout-stable (identity-keyed) form.  Empty / unused on the
            // legacy diagonal path → byte-identical.
            std::map<Eigen::Index, CoupledShared> col2desc;
            Eigen::MatrixXd cov_df;   // shared covariance, datum-pinned + ensemble-gauge cols zeroed
            if (carry_cpl_cond) {
                auto regcol = [&](CoupledShared d, Eigen::Index col) {
                    if (col >= 0 && col < est.numShared()) col2desc.emplace(col, std::move(d));
                };
                for (const std::string& sat : registered_clock_sats) {
                    try { CoupledShared d; d.kind = SharedParamKind::Clock; d.sat = sat;
                          regcol(std::move(d), est.clockColumn(sat)); } catch (...) {}
                }
                for (const auto& pk : shared_phase_keys) {
                    const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                    if (!parts) continue;
                    try { CoupledShared d; d.kind = SharedParamKind::PhaseBias;
                          d.sat = parts->satellite; d.signal = parts->signal;
                          regcol(std::move(d),
                                 est.phaseBiasColumn(parts->satellite, parts->signal)); } catch (...) {}
                }
                for (const auto& kv : registered_code_keys) {
                    try { CoupledShared d; d.kind = SharedParamKind::CodeBias;
                          d.sat = kv.first; d.signal = kv.second;
                          regcol(std::move(d), est.codeBiasColumn(kv.first, kv.second)); } catch (...) {}
                }
                if (config_.estimateNetworkIonoSh() && sh_model)
                    for (const auto& c : sh_model->coeffs()) {
                        try { CoupledShared d; d.kind = SharedParamKind::VtecSh;
                              d.n = c.n; d.m = c.m; d.trig = c.trig;
                              regcol(std::move(d), est.vtecCoeffColumn(c.n, c.m, c.trig)); } catch (...) {}
                    }
                // P_prev,jj a departed shared column is marginalized with next epoch: the
                // datum-frame marginal variance (datum-pinned + ensemble-gauge cols EXACT = 0,
                // mirroring conditionalLocalVarianceDiag's frame).
                cov_df = est.sharedCovariance();
                for (const auto& p : plan.shared_datum)
                    if (p.column >= 0 && p.column < cov_df.rows()) {
                        cov_df.row(p.column).setZero(); cov_df.col(p.column).setZero();
                    }
                est.zeroPhaseGaugeColumns(cov_df);
            }
            // Card 1: harvest one carried column's PURE conditional variance (N_rr⁻¹)_cc
            // + its regression on the free shared border (identity-keyed, sparsified).
            // Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
            auto harvestCoupling = [&](std::size_t est_idx, Eigen::Index col)
                    -> std::pair<double, std::vector<CoupledShared>> {
                NetworkNormalEquations::StationRegression reg;
                try { reg = est.stationRegression(est_idx, col); }
                catch (...) { return {-1.0, {}}; }
                std::vector<CoupledShared> cpl;
                const Eigen::Index ns = reg.row.size();
                if (ns > 0) {
                    const double maxabs = reg.row.cwiseAbs().maxCoeff();
                    const double thr = kCouplingSparsifyRelThresh * maxabs;
                    for (Eigen::Index j = 0; j < ns; ++j) {
                        const double mj = reg.row(j);
                        if (std::abs(mj) <= thr) continue;          // sparsify near-zero entries
                        const auto dit = col2desc.find(j);
                        if (dit == col2desc.end()) continue;        // col not in the enumerated shared set
                        // Card 1e: a datum-pinned OR ensemble-gauge column is KEPT in the row
                        // (coefficient + its ŝ_prev RHS term) with p_prev = 0 ("exactly known";
                        // the zeroed cov_df diagonal mirrors conditionalLocalVarianceDiag's
                        // frame).  The FULL regression row is exactly GAUGE-INVARIANT — every
                        // station data row satisfies A_s·g_s + A_r·g_r = 0 for each structural
                        // gauge direction, so M = e_cᵀN_rr⁻¹N_rs has Mᵀg_s = −g_r(c) and the
                        // row's +1 local completes the cancellation.  The former FREE-column
                        // restriction AMPUTATED the pinned columns' coefficients, leaving
                        // M'ᵀg ≠ 0: every coupling row then observed the clock gauge, the W5
                        // null-space gate saw "0 null directions vs declared dim" and WITHHELD
                        // every epoch that carried rows — which kept the whole coupling carry
                        // structurally inert in the field (the byte-identical-A/B trap).
                        // Under the datum the pinned term is exact (Dirichlet elimination);
                        // p_prev = 0 degrades every other path correctly: the κ time update
                        // falls through unshrunk (a pinned column does not move) and a
                        // departed pinned column marginalizes to +0.
                        const double pj = (j < cov_df.rows() && cov_df(j, j) > 0.0)
                                              ? cov_df(j, j) : 0.0;
                        CoupledShared cs = dit->second;
                        cs.coeff  = mj;
                        cs.s_prev = x_s(j);
                        cs.p_prev = pj;
                        cpl.push_back(std::move(cs));
                    }
                }
                return {reg.cond_var, std::move(cpl)};
            };
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                // Prioritized: harvest the former pivot too — its non-anchored arcs
                // are in R.pl_amb; anchored (pinned) keys were excluded at build, so
                // inject and harvest partition on the SAME per-(sat,signal) predicate.
                if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
                const StationAssembly& R = asm_res[i];
                if (R.pl_amb.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                std::vector<Eigen::Index> cols;
                cols.reserve(R.pl_amb.size());
                for (const auto& a : R.pl_amb) cols.push_back(a.col);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc, not this MARGINAL
                        vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
                } catch (...) { continue; }
                for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                    const PlAmb& pa = R.pl_amb[a];
                    if (pa.col < 0 || pa.col >= xr.size()) continue;
                    const double value_abs = pa.x_lin + xr(pa.col);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, pa.col);
                        var = hc.first; coupling = std::move(hc.second);
                        // Card 1/1d observability: a failed harvest drops the carry entirely;
                        // an EMPTY regression silently degrades the row to a plain diagonal.
                        if (!(var > 0.0) || !std::isfinite(var)) ++n_cpl_harvest_fail;
                        else if (coupling.empty())               ++n_cpl_stash_empty;
                        else                                     ++n_cpl_stash;
                    } else {
                        var = (a < static_cast<size_t>(vard.size())) ? vard(a) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    // M2 (LS-VCE): fold this ambiguity carry's post-fit innovation
                    // (carried prior − freshly solved) into the monitor BEFORE the
                    // stash overwrites the prior.  Qualify only rows that actually
                    // carried a prior this epoch — mirror the inject predicate: a
                    // live prior within max-gap, not a slip/refloat (pending reseed),
                    // and not a Card-3 re-referenced arc (a frame step, not noise).
                    if (carry_vce) {
                        const auto oc = local_carry.find(
                            std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                        if (oc != local_carry.end()
                            && !stations[i].sm->isAmbiguityPendingReseed(pa.amb_key)
                            && !eq34_reref.count({pa.sat, pa.signal})) {
                            const double gapv = pl_now_s - oc->second.last_seen_s;
                            if (gapv >= 0.0 && gapv <= pl_max_gap_s) {
                                double fold_var =
                                    oc->second.var + pl_q_amb * std::max(0.0, gapv);
                                if (pl_var_floor > 0.0)
                                    fold_var = std::max(fold_var, pl_var_floor);
                                carry_vce_est.addRow(
                                    "pl", pa.sat, oc->second.value_abs - value_abs,
                                    gapv, var, fold_var);
                            }
                        }
                    }
                    local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)] =
                        LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
                    // One-shot IF-phase refloat: the fresh post-slip value is now
                    // stashed, so drop any pending-reseed mark and let the next epoch
                    // resume normal carry on the new arc.  Cleared only after a
                    // SUCCESSFUL stash — a skipped non-finite value keeps the mark so
                    // the stale pre-slip carry is never re-trusted.  No-op when unmarked.
                    stations[i].sm->clearPendingReseed(pa.amb_key);
                }
            }
            for (auto it = local_carry.begin(); it != local_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = local_carry.erase(it);
                else ++it;
            }
            // --- ZWD carry (B9): harvest posterior ZWD + stash (mirrors the ambiguity
            //     harvest; independent loop so it works regardless of ambiguity presence).
            if (zwd_on) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    // B9: harvest the pivot's ZWD too — R.pl_zwd_present is set for the
                    // pivot now that the carry inject above runs for it.
                    if (est_index_of[i] < 0) continue;
                    const StationAssembly& R = asm_res[i];
                    if (!R.pl_zwd_present) continue;
                    const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                    Eigen::VectorXd xr, vard;
                    try {
                        xr = est.reconstructStation(est_idx, x_s);
                        if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc
                            vard = est.conditionalLocalVarianceDiag(
                                est_idx, {R.pl_zwd.col}, plan.shared_datum);
                    } catch (...) { continue; }
                    if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
                    const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, R.pl_zwd.col);
                        var = hc.first; coupling = std::move(hc.second);
                    } else {
                        var = (vard.size() > 0) ? vard(0) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    zwd_carry[static_cast<int>(i)] =
                        LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
                }
                for (auto it = zwd_carry.begin(); it != zwd_carry.end();) {
                    if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = zwd_carry.erase(it);
                    else ++it;
                }
            }
            // --- Card 3 (eq34): RPB carry — harvest δ̂_{r,σ} + stash + prune ----
            // Mirrors the ZWD harvest.  DELIBERATELY no Card-1 coupling harvest
            // for RPB columns: coupling rows would hand the RPB a de-facto
            // shared-border footprint (the D6 v2 caveat zone — tuGaugeRestore
            // group exclusions); the diagonal conditional variance keeps the
            // carry strictly local and conservative.
            if (eq34_rpb_q > 0.0) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0) continue;
                    const StationAssembly& R = asm_res[i];
                    if (R.pl_rpb.empty()) continue;
                    const std::size_t est_idx =
                        static_cast<std::size_t>(est_index_of[i]);
                    Eigen::VectorXd xr, vard;
                    std::vector<Eigen::Index> cols;
                    cols.reserve(R.pl_rpb.size());
                    for (const auto& t : R.pl_rpb) cols.push_back(std::get<1>(t));
                    try {
                        xr = est.reconstructStation(est_idx, x_s);
                        vard = est.conditionalLocalVarianceDiag(
                            est_idx, cols, plan.shared_datum);
                    } catch (...) { continue; }
                    for (std::size_t a = 0; a < R.pl_rpb.size(); ++a) {
                        const auto& [rpb_key, rcol, rx_lin] = R.pl_rpb[a];
                        if (rcol < 0 || rcol >= xr.size()) continue;
                        const double value_abs = rx_lin + xr(rcol);
                        const double var = (a < static_cast<std::size_t>(vard.size()))
                                               ? vard(a) : -1.0;
                        if (!std::isfinite(value_abs) || !(var > 0.0)
                            || !std::isfinite(var)) continue;
                        rpb_carry[{static_cast<int>(i), rpb_key}] =
                            LocalCarry{value_abs, var, pl_now_s};
                    }
                }
                for (auto it = rpb_carry.begin(); it != rpb_carry.end();) {
                    if (pl_now_s - it->second.last_seen_s > pl_max_gap_s)
                        it = rpb_carry.erase(it);
                    else ++it;
                }
            }
        }

        // --- T1d: SH-field convergence detector (latch + one-shot refloat) ----
        // Judge the co-estimated SH field "stationary" once the epoch-over-epoch coefficient
        // RMS change drops below the threshold AND a₀₀ is inside the physical band, then LATCH
        // (once converged, stay converged — the split it defines is the good one).  Runs POST-
        // harvest so a refloat drops the just-stashed (possibly contaminated early) carry → a
        // true one-epoch refloat.  The latch is read pre-solve NEXT epoch by carry_warm, so the
        // convergence-gated carry warms one epoch after the field first settles.  Gated on one
        // of the two toggles ⇒ byte-identical when both are OFF (the reference vector + latch
        // are never touched, carry_warm keeps the fixed epoch-count gate).
        if ((pl_warmup_on_conv || pl_refloat_on_conv) && sh_model && !sh_converged_latch) {
            const auto& sh_coeffs = sh_model->coeffs();
            Eigen::VectorXd cur(static_cast<Eigen::Index>(sh_coeffs.size()));
            double a00 = std::numeric_limits<double>::quiet_NaN();
            bool ok = true;
            for (std::size_t k = 0; k < sh_coeffs.size() && ok; ++k) {
                try {
                    const double v = x_s(est.vtecCoeffColumn(
                        sh_coeffs[k].n, sh_coeffs[k].m, sh_coeffs[k].trig));
                    cur(static_cast<Eigen::Index>(k)) = v;
                    if (sh_coeffs[k].n == 0 && sh_coeffs[k].m == 0 && sh_coeffs[k].trig == 'C')
                        a00 = v;
                } catch (...) { ok = false; }
            }
            if (ok && cur.size() > 0 && sh_coeff_prev.size() == cur.size()) {
                const double rms_delta = std::sqrt(
                    (cur - sh_coeff_prev).squaredNorm() / static_cast<double>(cur.size()));
                const bool a00_in_band = std::isfinite(a00)
                    && a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu;
                if (rms_delta < pl_conv_rms_thresh && a00_in_band) {
                    sh_converged_latch = true;
                    log->info("Network (SH-iono): field CONVERGED at emitted epoch {} (coeff "
                              "RMS Δ {:.4f} < {:.4f} TECU, a₀₀ {:.1f} in band) — phase-leveling "
                              "carry {}.", n_emitted, rms_delta, pl_conv_rms_thresh, a00,
                              pl_warmup_on_conv ? "warm-gate released"
                                                : (pl_refloat_on_conv ? "refloated" : "unchanged"));
                    if (pl_refloat_on_conv)
                        local_carry.clear();   // one-shot flush ⇒ ambiguities refloat next epoch
                }
            }
            if (ok) sh_coeff_prev = cur;   // roll the reference forward until latched
        }

        // --- Post-fit network RAIM (Increment 3, tier T2) -------------------
        // The safety net for the zero-process-noise carry.  The per-station slip
        // pre-processors (MW/GF TurboEdit, IF-phase triple-difference) are
        // threshold/pool-limited, so a real slip on a satellite that is SETTING (few
        // tracking stations, under the pooled-MAD count) passes undetected — and with
        // zero process noise the stale carry then freezes into the products forever.
        //
        // This runs AFTER the joint solve and spends the network redundancy the
        // per-station detectors cannot see.  A bad carried ambiguity leaks its error
        // into the SHARED satellite clock/phase-bias; the guilty station HIDES it
        // there (its own residual stays small), but every OTHER station tracking that
        // satellite is pinned to its own (correct) carry and CANNOT absorb the shared
        // shift, so its post-fit phase residual spikes.  Each carried ambiguity's phase
        // residual is a Baarda-style normalized residual: v·√w divided by a ROBUST
        // a-posteriori variance factor σ̂0 = max(1, 1.4826·median|v·√w|) pooled network-wide
        // this epoch, so a satellite is judged an outlier relative to the network's CURRENT
        // residual level, not to the mm measurement noise (which would trip on ordinary cm
        // model misfit).  A cross-station consensus on the normalized residual (aggregated
        // per satellite) therefore fires exactly when the contamination reached the PRODUCTS
        // (the shared clock actually moved ⇒ many innocent stations spike) and stays quiet
        // when a slip stayed local and harmless — robust WHILE THE SLIPPED FRACTION IS SMALL
        // (an all-move-together epoch inflates σ̂0 itself and is a separate diagnosis).  It
        // refloats the flagged satellite's
        // carry at EVERY station (marks pending-reseed, so the next epoch re-estimates
        // from data — the IF-phase detector's one-shot lifecycle).  Placed after the
        // harvest so the just-completed stash's clearPendingReseed does not wipe the
        // mark.  OFF (default) = byte-identical (no marks, no state change).
        subEnd(SB_LEVELING);   // D0b: close block 5 (leveling + T1d SH-convergence)
        subBegin(SB_RAIM);     // D0b block 2: post-fit network RAIM + median-polish attribution
        if (raim_on && carry_warm) {
            // Pass 1: reconstruct each station once, form its post-fit phase residuals, and
            // record per (station, satellite) the largest v·√w (measurement-standardized),
            // pooling every phase-row value for the robust a-posteriori variance factor.
            struct AmbStat { std::string sat; int sta; double r_max; };
            std::vector<AmbStat> amb_stats;
            std::vector<double>  r_pool;
            // Prominence-attribution input (collected iff attribution is LOGGED or DRIVES the
            // action): one cell per (station, satellite, signal) carried-ambiguity residual.
            const bool attrib_on   = raim_attrib_trace.is_open();       // log the decomposition
            const bool attrib_act  = config_.networkRaimAttribution();  // let it drive the refloat
            // B8/T4 (P1-4): also build the attribution table under trace_flag_rates so the T4
            // flag-rate is MEASURABLE on a clean baseline with ONLY network.trace_flag_rates on —
            // the documented empirical-α workflow — without needing raim_attribution or the
            // attribution trace.  The median polish then runs read-only (no refloat unless
            // attrib_act, no trace unless attrib_on), feeding only the fr_attrib counter; byte-
            // identical when trace_flag_rates is off (this disjunct is then absent).
            const bool need_attrib = attrib_on || attrib_act || trace_flag_rates;
            struct Cell { int sta; std::string sat, sig; double r; };
            std::vector<Cell> cells;
            for (size_t i = 0; i < stations.size(); ++i) {
                // Prioritized: keep the former pivot — its non-anchored arcs are in
                // R.pl_amb (anchored/pinned keys already excluded at carry build).
                if (est_index_of[i] < 0
                    || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_amb.empty() || R.A_s.cols() != x_s.size()) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                if (x_r.size() != R.A_r.cols()) continue;
                const Eigen::VectorXd resid = R.y - R.A_s * x_s - R.A_r * x_r;  // post-fit O − C
                Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
                if (raim_redun) {
                    try {
                        row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                                  R.A_s, R.A_r, R.w, plan.shared_datum);
                    } catch (...) { row_r.resize(0); }
                }
                // Largest measurement-standardized post-fit phase residual PER SATELLITE at this
                // station (a carried ambiguity's phase rows carry +1 on its A_r column; uncombined).
                std::map<std::string, double> sat_rmax;
                for (const auto& pa : R.pl_amb) {
                    if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                    double r_max = 0.0;
                    for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                        if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
                        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                        double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                        if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                            constexpr double kRedFloor = 1e-6;
                            const double ri = row_r(row);
                            if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                            r /= std::sqrt(ri);
                        }
                        r_pool.push_back(r);
                        if (r > r_max) r_max = r;
                    }
                    double& cur = sat_rmax[pa.sat];
                    if (r_max > cur) cur = r_max;
                    if (need_attrib) cells.push_back({static_cast<int>(i), pa.sat, pa.signal, r_max});
                }
                for (const auto& [sat, rmax] : sat_rmax)
                    amb_stats.push_back({sat, static_cast<int>(i), rmax});
            }

            // Robust a-posteriori variance factor for this epoch, network-wide.  Floored at 1
            // so a clean network is no more sensitive than the nominal stochastic model
            // (σ̂0→1 ⇒ identical to the plain standardized residual); when the whole network
            // misfits at cm, σ̂0 rises and the normalized residuals stay O(1) — so RAIM stops
            // firing on ambient misfit and flags only what stands out ABOVE it.
            double sigma0 = 1.0;
            if (!r_pool.empty()) {
                std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
                sigma0 = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
            }
            // B1: fold the network-wide σ̂0 into the URA EWMA (scales next epoch's emitted
            // URA — the post-emit ordering means the scale lands one epoch later).
            // RTB-7(b): the update COUNT feeds the quality-ramp lift rule (the
            // "EWMA has stabilized" half); read only while the warmup floor is armed.
            if (ura_on) {
                ura_sigma0_ewma = ura_alpha * sigma0 + (1.0 - ura_alpha) * ura_sigma0_ewma;
                ++ura_ewma_updates;
            }

            // Pass 2: normalize by σ̂0 and take the cross-station consensus per satellite.
            std::map<std::string, int> sat_flag_stations;  // satellite -> #stations whose normalized residual spiked
            struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bool spiked; };
            std::vector<RaimDetail> raim_details;           // per (station, sat); built only when tracing
            for (const auto& a : amb_stats) {
                const double s_norm = a.r_max / sigma0;
                const bool   spiked = (s_norm > raim_k);
                if (trace_flag_rates) { ++fr_raim_cell.opp; if (spiked) ++fr_raim_cell.flag; }  // B8/T3 cell
                if (station_res_trace.is_open())   // L13: every cell (not just spikes)
                    station_res_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                      << ',' << stations[a.sta].name << ',' << a.sat << ','
                                      << fmt::format("{:.3f}", a.r_max) << ','
                                      << fmt::format("{:.3f}", s_norm) << ','
                                      << fmt::format("{:.3f}", sigma0) << '\n';
                if (spiked) ++sat_flag_stations[a.sat];
                if (raim_trace.is_open())
                    raim_details.push_back({a.sat, a.sta, a.r_max, s_norm, spiked});
            }
            std::vector<std::string> flagged;
            for (const auto& kv : sat_flag_stations)
                if (kv.second >= raim_min_sta) flagged.push_back(kv.first);
            const std::set<std::string> fset(flagged.begin(), flagged.end());
            if (trace_flag_rates) {   // B8/T3 consensus: rate of consensus among sats that spiked ≥1 station
                fr_raim_sat.opp  += static_cast<long>(sat_flag_stations.size());
                fr_raim_sat.flag += static_cast<long>(flagged.size());
            }

            // A consensus-flagged satellite's slip leaked into its SHARED clock (that is why the
            // innocent stations spiked) — refloat the carried clock alongside the ambiguity.
            if (clk_mon_couple)
                for (const auto& s : fset) clock_refloat_sats.insert(s);
            // P0-2: consensus-flagged sats contaminated the emitted correction — feed the
            // retro-bump independent of the couple opt-in.
            if (retro_feed)
                for (const auto& s : fset) retro_flag_sats.insert(s);

            // Trace the consensus context: every station of a refloated sat (to see the
            // guilty-hidden / innocent-spike pattern) plus any spiked near-miss.  Quiet
            // epochs emit nothing.  σ̂0 is logged so the all-move-together epochs (where σ̂0
            // itself spikes) are diagnosable.
            if (raim_trace.is_open()) {
                for (const auto& d : raim_details) {
                    if (!(fset.count(d.sat) || d.spiked)) continue;
                    raim_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << d.sat << ',' << stations[d.sta].name << ','
                        << fmt::format("{:.3f}", d.s_raw) << ',' << fmt::format("{:.3f}", d.s_norm) << ','
                        << fmt::format("{:.3f}", sigma0) << ',' << fmt::format("{:.1f}", raim_k) << ','
                        << (d.spiked ? 1 : 0) << ',' << sat_flag_stations[d.sat] << ','
                        << raim_min_sta << ',' << (fset.count(d.sat) ? 1 : 0) << '\n';
                }
            }

            // --- Prominence attribution + optional attribution-driven refloat -------------
            // Robust 2-way median polish of the (station × satellite) standardized-residual
            // table separates a bad SATELLITE (whole column lights up), a bad STATION (whole
            // row), and an isolated ARC (interaction); a per-signal marginal on the polished
            // leftover flags a bad SIGNAL.  One pass over the residual cells already in hand —
            // no re-solve, unlike the user-side iterative snoop.  Always LOGGED when the
            // attribution trace is open; when network.raim_attribution is on it also DRIVES
            // the refloat (bad sat → network-wide, bad station → that station's arcs, isolated
            // arc → just that (station,sat)) instead of the per-satellite consensus.
            bool attrib_acted = false;
            if (need_attrib && !cells.empty()) {
                constexpr double kAttribReportProm = 4.0;  // robust-z above which an entity is logged

                // (station × sat) table: cell = max standardized residual over that
                // station-sat's signals.
                std::map<std::pair<int, std::string>, double> cell_max;
                for (const auto& c : cells) {
                    const auto key = std::make_pair(c.sta, c.sat);
                    auto it = cell_max.find(key);
                    if (it == cell_max.end() || c.r > it->second) cell_max[key] = c.r;
                }
                std::vector<std::tuple<int, std::string, double>> tbl;
                tbl.reserve(cell_max.size());
                for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
                const MedianPolish mp = medianPolish2Way(tbl);

                // B8/T4 (P1-4): attribution flag-rate on the per-satellite median-polish
                // column effect, using the SAME criterion the acting path (bad_sats) uses —
                // σ̂0 above the within-epoch trigger AND effect/σ̂0 above raim_k — so the rate
                // is measurable on a clean baseline whether or not raim_attribution is acting.
                if (trace_flag_rates) {
                    const double trig_a = config_.networkRaimSigma0Trigger();
                    for (const auto& [sat, eff] : mp.col_eff) {
                        (void)sat;
                        ++fr_attrib.opp;
                        if (sigma0 > trig_a && sigma0 > 0.0 && eff / sigma0 > raim_k) ++fr_attrib.flag;
                    }
                }

                const auto sat_prom = robustProminence(mp.col_eff);
                const auto sta_prom = robustProminence(mp.row_eff);

                // Signal marginal on the polished leftover (net of overall + station + sat).
                std::map<std::string, std::vector<double>> sig_left;
                for (const auto& c : cells)
                    sig_left[c.sig].push_back(
                        c.r - mp.overall - mp.row_eff.at(c.sta) - mp.col_eff.at(c.sat));
                std::map<std::string, double> sig_eff;
                for (auto& [sig, v] : sig_left) sig_eff[sig] = medianCopy(v);
                const auto sig_prom = robustProminence(sig_eff);

                // Isolated-arc interaction scale.
                std::vector<double> inter_abs;
                inter_abs.reserve(mp.interaction.size());
                for (double v : mp.interaction) inter_abs.push_back(std::abs(v));
                const double inter_mad = std::max(1e-9, 1.4826 * medianCopy(inter_abs));

                // Attribution-driven refloat set (scale-consistent: an effect is "bad" when it
                // stands raim_k robust baselines (σ̂0) above the additive model).  A bad
                // satellite/station is caught wholesale; an isolated arc counts only if its
                // sat/station were NOT already caught (no double attribution).
                std::set<std::string> bad_sats;
                std::set<int>         bad_stations;   // deliberately never populated — see below
                std::vector<std::pair<int, std::string>> bad_arcs;
                if (attrib_act) {
                    // Wholesale SATELLITE refloat only when the network is GENUINELY contaminated
                    // (σ̂0 above the within-epoch trigger).  At a robust-clean epoch (σ̂0 ≈ 1) a high
                    // column effect is a median-polish artifact of a satellite visible across a
                    // regional station cluster, NOT a network-wide slip — refloating its carriers
                    // everywhere would wipe good accumulated precision and destabilize (the root of
                    // the 305 24h ~4 h lock-in).  The isolated-arc path below still refloats real
                    // single slips at ANY σ̂0.
                    if (sigma0 > config_.networkRaimSigma0Trigger())
                        for (const auto& [sat, eff] : mp.col_eff)
                            if (eff / sigma0 > raim_k) bad_sats.insert(sat);
                    // Refloat the carried clock of an attribution-flagged (network-wide) satellite
                    // too — an isolated ARC is a local slip and does NOT implicate the shared clock,
                    // so only whole-satellite culprits feed the clock refloat.
                    if (clk_mon_couple)
                        for (const auto& s : bad_sats) clock_refloat_sats.insert(s);
                    // P0-2: attribution whole-satellite culprits contaminated the emitted
                    // correction — feed the retro-bump independent of the couple opt-in.
                    // (Isolated arcs are LOCAL slips that do not implicate the shared per-sat
                    //  correction, so they are excluded — mirroring the clock-refloat scoping.)
                    if (retro_feed)
                        for (const auto& s : bad_sats) retro_flag_sats.insert(s);
                    // Bad-STATION wholesale refloat is DELIBERATELY DISABLED: a regional cluster of
                    // stations lighting up on ONE shared satellite event inflates their median-polish
                    // ROW effects, so this path mass-refloated dozens of innocent stations at once
                    // (78 stations / 3977 carriers at TOW 273150 in the 24 h run) → cascade →
                    // divergence.  A genuinely bad station is handled by the within-epoch station-
                    // exclude (σ̂0 > trigger) or surfaces as isolated arcs; a whole-station carrier
                    // wipe is never worth the mis-attribution risk.  bad_stations stays empty.
                    // Persistence gate (network.raim_arc_persist_epochs, K): an isolated arc is
                    // refloated only after it trips the interaction gate K CONSECUTIVE epochs.  A real
                    // slip persists as an integer step until refloated → trips every epoch → clears K
                    // and refloats; a heavy-tail / model-error spike does not repeat on the same arc →
                    // never reaches K → is starved.  K=1 (default) takes the immediate path below and is
                    // byte-identical.  For K>1 we rebuild the streak map from THIS epoch's flagged set so
                    // any arc not flagged now resets to 0 (absence ⇒ non-consecutive), and an arc that
                    // reaches K refloats and is dropped from the carry (must re-persist K epochs for the
                    // next refloat — a persistent arc refloats at most once per K epochs).
                    // T2d: filter-maturity latch for the isolated-arc refloat gate — mirrors the
                    // warmup URA ramp (emitted-epoch horizon AND σ̂0-EWMA update count, the count
                    // waived when the EWMA is structurally unavailable).  Isolated arcs are the
                    // self-sustaining warmup churn driver (2→1063/epoch); suppress them until the
                    // filter has settled.  Forced true when the gate is OFF ⇒ byte-identical.
                    // Whole-sat bad_sats (network-wide contamination) are NEVER gated.
                    // ewma_structurally_available == (emit_ura && raim && phase_leveling): only
                    // then is the σ̂0-EWMA a fed signal, so only then does the update-count term
                    // apply; without emit_ura the EWMA is never updated and the horizon alone
                    // decides (matching the RTB-6 sentinel + the URA-ramp lift rule).
                    const bool raim_mature = !raim_maturity_gate || networkWarmupRampLifted(
                        static_cast<long long>(n_emitted), raim_maturity_epochs, pl_warmup,
                        static_cast<long long>(ura_ewma_updates), ura_on && raim_on && pl_on);
                    std::map<std::pair<int, std::string>, int> next_arc_streak;  // K>1 only
                    for (std::size_t k = 0; k < tbl.size(); ++k) {
                        if (mp.interaction[k] / sigma0 <= raim_k) continue;
                        const int sta = std::get<0>(tbl[k]);
                        const std::string& sat = std::get<1>(tbl[k]);
                        if (bad_sats.count(sat)) continue;   // caught wholesale; no double attribution
                        if (!raim_mature) continue;   // T2d: suppress isolated-arc refloat pre-maturity
                        if (raim_arc_persist <= 1) {
                            bad_arcs.emplace_back(sta, sat);   // byte-identical immediate refloat
                            continue;
                        }
                        const auto akey = std::make_pair(sta, sat);
                        const auto it = arc_spike_streak.find(akey);
                        const int streak = (it != arc_spike_streak.end() ? it->second : 0) + 1;
                        if (streak >= raim_arc_persist)
                            bad_arcs.emplace_back(sta, sat);   // persisted K epochs → refloat, drop streak
                        else
                            next_arc_streak[akey] = streak;    // still building → carry forward
                    }
                    if (raim_arc_persist > 1) arc_spike_streak.swap(next_arc_streak);
                    n_arc_refloat_epoch += static_cast<int>(bad_arcs.size());  // T2d: heartbeat churn metric
                }

                if (attrib_on) {
                    std::map<std::string, int> sat_n, sig_n;
                    std::map<int, int> sta_n;
                    for (const auto& c : cells) { ++sat_n[c.sat]; ++sig_n[c.sig]; ++sta_n[c.sta]; }
                    auto emit = [&](const char* dim, const std::string& entity, double effect,
                                    double prom, int n, int acted) {
                        raim_attrib_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << dim << ',' << entity << ','
                            << fmt::format("{:.4f}", effect) << ',' << fmt::format("{:.2f}", prom) << ','
                            << n << ',' << fmt::format("{:.4f}", mp.overall) << ','
                            << fmt::format("{:.3f}", sigma0) << ',' << acted << '\n';
                    };
                    for (const auto& [sat, pr] : sat_prom)
                        if (pr > kAttribReportProm)
                            emit("SAT", sat, mp.col_eff.at(sat), pr, sat_n[sat],
                                 (attrib_act ? bad_sats.count(sat) : fset.count(sat)) ? 1 : 0);
                    for (const auto& [sta, pr] : sta_prom)
                        if (pr > kAttribReportProm)
                            emit("STA", stations[sta].name, mp.row_eff.at(sta), pr, sta_n[sta],
                                 (attrib_act && bad_stations.count(sta)) ? 1 : 0);
                    for (const auto& [sig, pr] : sig_prom)
                        if (pr > kAttribReportProm)
                            emit("SIG", sig, sig_eff.at(sig), pr, sig_n[sig], 0);
                    std::vector<std::size_t> order(mp.interaction.size());
                    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
                    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                        return std::abs(mp.interaction[a]) > std::abs(mp.interaction[b]);
                    });
                    int arc_logged = 0;
                    for (std::size_t k : order) {
                        const double pr = std::abs(mp.interaction[k]) / inter_mad;
                        if (pr <= kAttribReportProm || arc_logged >= 20) break;
                        const int sta = std::get<0>(tbl[k]);
                        const std::string& sat = std::get<1>(tbl[k]);
                        const int acted = attrib_act
                            ? ((mp.interaction[k] / sigma0 > raim_k || bad_sats.count(sat)
                                || bad_stations.count(sta)) ? 1 : 0)
                            : (fset.count(sat) ? 1 : 0);
                        emit("ARC", stations[sta].name + "/" + sat, mp.interaction[k], pr, 1, acted);
                        ++arc_logged;
                    }
                }

                // Execute the attribution-driven refloat (surgical: whole sat / whole station /
                // single arc, per the decomposition).  Erasing local_carry + pending-reseed is
                // the same one-shot refloat the consensus path uses.
                if (attrib_act && (!bad_sats.empty() || !bad_stations.empty() || !bad_arcs.empty())) {
                    auto refloat_sat_at = [&](int i, const std::string& sat) -> int {
                        int n = 0;
                        for (const auto& pa : asm_res[i].pl_amb)
                            if (pa.sat == sat) {
                                local_carry.erase(std::make_tuple(i, pa.sat, pa.signal));
                                stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                                ++n;
                            }
                        return n;
                    };
                    int n_ref = 0;
                    for (const auto& sat : bad_sats)
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (present[i] && est_index_of[i] >= 0
                                && (phase_datum_prioritized || !stations[i].is_pivot))
                                n_ref += refloat_sat_at(static_cast<int>(i), sat);
                    // (bad_stations is intentionally empty — the whole-station refloat path is
                    //  disabled to avoid the regional mis-attribution mass-refloat.)
                    for (const auto& [sta, sat] : bad_arcs) n_ref += refloat_sat_at(sta, sat);
                    attrib_acted = true;

                    std::string detail;
                    for (const auto& s : bad_sats)
                        detail += (detail.empty() ? "" : ",") + std::string("sat:") + s;
                    for (int s : bad_stations)
                        detail += (detail.empty() ? "" : ",") + std::string("sta:") + stations[s].name;
                    if (!bad_arcs.empty())
                        detail += fmt::format("{}{} arc", detail.empty() ? "" : ",", bad_arcs.size());
                    log->info("Network[RAIM-attrib]: TOW {:.1f} refloated {} carried ambiguity(ies) — "
                              "{} bad sat(s), {} bad station(s), {} isolated arc(s) "
                              "(σ̂0={:.2f}, gate {:.1f}) [{}]",
                              se.time.tow, n_ref, bad_sats.size(), bad_stations.size(),
                              bad_arcs.size(), sigma0, raim_k, detail);
                    // T4: one event per ACTED entity (SAT with its column prominence; ARC keyed
                    // by station).  Full per-entity attribution (incl. arc prominence) is in
                    // network_raim_attrib.csv; here we log only the entities that were refloated.
                    if (events_trace.is_open()) {
                        const std::string sd = fmt::format("sigma0={:.2f}", sigma0);
                        for (const auto& s : bad_sats)
                            emitEvent("T4_attrib", "attrib_sat", s, "",
                                      sat_prom.count(s) ? std::optional<double>(sat_prom.at(s))
                                                        : std::nullopt,
                                      raim_k, "refloat", sd);
                        for (const auto& [sta, sat] : bad_arcs)
                            emitEvent("T4_attrib", "attrib_arc", sat, stations[sta].name,
                                      std::nullopt, raim_k, "refloat", sd);
                    }
                }
            }

            // --- "So bad" within-epoch adaptation trigger (detection + log) ---------------
            // When σ̂0 exceeds the trigger the joint solve is contaminated enough that emitting
            // it would be wrong.  The full adaptation drops the dominant culprit(s) and RE-SOLVES
            // this epoch (a beginEpoch replay of the scatter minus the culprit) to re-condition;
            // that core-solve change is staged.  For now we DETECT + arm it so the trigger rate
            // and the drop targets can be validated against the attribution first.
            if (config_.networkRaimWithinEpoch() && sigma0 > config_.networkRaimSigma0Trigger()) {
                log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} STILL > trigger {:.2f} after "
                          "adaptation — residual contamination the culprit-drop could not isolate "
                          "(emitted anyway; inspect network_raim_attrib.csv).",
                          se.time.tow, sigma0, config_.networkRaimSigma0Trigger());
                emitEvent("T2_within_epoch", "emitted_contaminated", "", "", sigma0,
                          config_.networkRaimSigma0Trigger(), "emit_anyway", "");
            }

            // Per-satellite consensus refloat — only when attribution is NOT driving the action.
            if (!attrib_act && !flagged.empty()) {
                int n_refloat = 0;
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0
                        || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                    for (const auto& pa : asm_res[i].pl_amb)
                        if (fset.count(pa.sat)) {
                            // Drop the contaminated carry OUTRIGHT.  The harvest above
                            // just re-stashed this ambiguity's (contaminated) value into
                            // local_carry; erasing it means the next epoch finds no prior
                            // to inject (cit == end → no addStationPrior), so the ambiguity
                            // re-floats from data — a direct, unambiguous refloat that does
                            // not depend on the deferred pending-reseed skip path.  The mark
                            // is also set (belt-and-suspenders / cleared by the next stash).
                            local_carry.erase(std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                            stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                            ++n_refloat;
                        }
                }
                std::string flist;
                for (size_t e = 0; e < flagged.size(); ++e) { if (e) flist += ","; flist += flagged[e]; }
                log->info("Network[RAIM]: TOW {:.1f} refloated {} carried ambiguity(ies) on {} "
                          "satellite(s) [{}] — normalized post-fit residual > {:.1f} at >= {} "
                          "stations (σ̂0={:.2f}; a pre-processor-missed slip leaked into the shared "
                          "clock, caught by network redundancy).",
                          se.time.tow, n_refloat, flagged.size(), flist, raim_k, raim_min_sta, sigma0);
                // T3: one event per consensus-refloated sat.  statistic = the per-sat max
                // normalized post-fit residual (max r_max / σ̂0 over its stations), from amb_stats.
                if (events_trace.is_open()) {
                    std::map<std::string, double> t3_norm;
                    for (const auto& a : amb_stats) {
                        const double sn = (sigma0 > 0.0) ? a.r_max / sigma0 : a.r_max;
                        double& m = t3_norm[a.sat];
                        m = std::max(m, sn);
                    }
                    for (const auto& s : flagged)
                        emitEvent("T3_raim", "consensus_refloat", s, "",
                                  t3_norm.count(s) ? std::optional<double>(t3_norm.at(s))
                                                   : std::nullopt,
                                  raim_k, "refloat",
                                  fmt::format("sigma0={:.2f};min_sta={}", sigma0, raim_min_sta));
                }
            }
            (void)attrib_acted;
        }
        subEnd(SB_RAIM);       // D0b: close block 2 (RAIM)
        subBegin(SB_CLOCKUPD); // D0b block 8: two-state clock update + fault QC + retro-flag feed

        // --- Two-state oscillator clock: measurement update + normalized-innovation fault test
        //     (network.clock_model) ------------------------------------------------------------
        // Update each satellite's clock model with this epoch's solved offset and test the
        // normalized innovation against the model's own tight prediction band.  A healthy clock
        // tracks its prediction (|innov|/√S ~ 1); a self-consistent lock-in jumps hundreds of σ —
        // the detectable dynamics fault the scalar random-walk carry hides.  The per-constellation
        // MEDIAN innovation (the datum common-mode) is removed first, so a whole-constellation
        // datum re-reference is gauged out and only per-satellite faults flag.  A flagged sat is
        // added to clock_refloat_sats (its phase/code bias reinitialize in the stash) and its
        // clock model is loosened (large-q reinit) so it re-acquires toward the a-priori anchor
        // instead of freezing.  On a pivot handover the models are re-seeded (the datum moved).
        if (clk_model_warm) {
            if (datum_changed && !scoped_handover) {
                clock_osc.clear();   // datum moved: every carried offset is in the retired frame (t_last dropped with it)
                // T5 (V2 blind spot): the datum-change oscillator wipe is otherwise SILENT.
                emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt, std::nullopt,
                          "clear_osc", "datum re-reference");
            } else {
                // RTA-2 (network.scoped_handover_response): wipe ONLY the changed
                // constellation(s)' oscillators — their carried offsets are in the
                // retired frame; unaffected constellations' models survive and update
                // normally below.  A wiped satellite carries no prediction this epoch
                // (the scoped prior skip above), so it falls into the seed branch and
                // re-seeds from THIS epoch's already-new-datum solve — one epoch
                // earlier than the global wipe, which skips the whole update block.
                if (datum_changed) {
                    for (auto oit = clock_osc.begin(); oit != clock_osc.end();) {
                        if (datumChangeAffectsKey(oit->first, changed_consts, /*scoped=*/true))
                            oit = clock_osc.erase(oit);
                        else
                            ++oit;
                    }
                    std::string consts;
                    for (char c : changed_consts) {
                        if (!consts.empty()) consts += ',';
                        consts += c;
                    }
                    emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt,
                              std::nullopt, "clear_osc",
                              fmt::format("datum re-reference (scoped: {})", consts));
                }
                std::map<std::string, double> z_of, innov_of, S_of, p_post_of;
                std::map<char, std::vector<double>> innov_by_const;
                const Eigen::MatrixXd& shcov = est.sharedCovariance();   // posterior formal cov
                for (const std::string& sat : registered_clock_sats) {
                    Eigen::Index col;
                    try { col = est.clockColumn(sat); } catch (...) { continue; }
                    const double z = x_s(col);
                    if (!std::isfinite(z) || sat.empty()) continue;
                    z_of[sat] = z;
                    p_post_of[sat] = (col < shcov.rows() && col < shcov.cols())
                                         ? shcov(col, col) : 0.0;   // posterior formal variance
                    auto pit = clk_pred.find(sat);
                    if (pit == clk_pred.end()) continue;      // freshly seeded / gap — no prediction
                    innov_of[sat] = z - pit->second.x0;
                    // B3 (data-incest fix): the fault statistic uses the info-consistent
                    // prior-minus-posterior variance (p00_pred − p00_post), NOT the old incest
                    // R-add (pit->p00 + clk_model_r).  The posterior x_s already FOLDS the model
                    // prior, so its innovation vs the prediction is the shrunken residual; its
                    // correct variance is the information the data removed, p00_pred − p00_post.
                    // Using +R deflated S in exactly the weak-data regime the NIS is meant to police.
                    // Floor S with the measurement-noise scale (σ_floor² = clk_model_r), NOT 1e-12, so
                    // a weak-data epoch (p00_post → p00_pred ⇒ p00_pred − p00_post → 0, and dm → 0)
                    // yields NIS ≈ 0 rather than a 0/0 blow-up that would false-flag the lowest-
                    // confidence satellites.  This is a LOWER BOUND, not the incest of adding R to a
                    // posterior-consistent innovation.
                    S_of[sat]     = std::max(std::max(1e-12, clk_model_r),
                                             pit->second.p00 - p_post_of[sat]);
                    innov_by_const[sat[0]].push_back(innov_of[sat]);
                }
                std::map<char, double> med_innov;
                for (auto& [c, vv] : innov_by_const) {
                    std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
                    med_innov[c] = vv[vv.size() / 2];
                }
                int n_flag = 0;
                std::string detail;
                for (const std::string& sat : registered_clock_sats) {
                    auto zit = z_of.find(sat);
                    if (zit == z_of.end()) continue;
                    const double z   = zit->second;
                    ClockOsc&    osc = clock_osc[sat];
                    auto pit = clk_pred.find(sat);
                    if (pit == clk_pred.end() || !osc.init) {
                        // Seed (first warm epoch of this satellite, OR its carried state was
                        // dropped as stale in the prediction above — a vanish-return): start
                        // from the solved value, zero drift, loose covariance; no fault test
                        // until it has a prediction.  Stamp t_last so the NEXT epoch's drift
                        // finite-difference and prediction use the true per-sat interval.
                        osc.x0 = z; osc.x1 = 0.0;
                        osc.p00 = 1.0; osc.p01 = 0.0; osc.p11 = 1e-6;
                        osc.t_last = pl_now_s; osc.init = true;
                        continue;
                    }
                    const double S   = std::max(1e-12, S_of[sat]);
                    const double dm  = innov_of[sat] - med_innov[sat[0]];  // datum common-mode removed
                    const double nis = dm / std::sqrt(S);
                    const bool   ext = clock_refloat_sats.count(sat) > 0;  // within-epoch/RAIM (couple)
                    const bool   flagged = (std::abs(nis) > clk_model_k) || ext;
                    if (trace_flag_rates) { ++fr_clk_model.opp; if (std::abs(nis) > clk_model_k) ++fr_clk_model.flag; }  // B8/T5 own detection (excl. ext)
                    if (flagged && !ext) { ++n_flag; detail += (detail.empty() ? "" : ",") + sat;
                        emitEvent("T5_clock_model", "nis_fault", sat, "", nis, clk_model_k,
                                  "refloat", fmt::format("innov={:.4f}", dm)); }
                    if (flagged) clock_refloat_sats.insert(sat);
                    // B3 (data-incest fix): set the carried state DIRECTLY from the joint-solve
                    // posterior — NO scalar KF measurement update (that folded the model prior a
                    // second time, making the osc covariance self-confirmingly optimistic).  The
                    // offset + its variance ARE the solve posterior; the drift is finite-differenced
                    // from the offset sequence.  On a fault, reinit the variance (loosen) so a frozen
                    // clock can re-acquire rather than lock onto the bad value; the a-priori anchor
                    // pulls it back to truth.
                    const double p_post   = std::max(1e-12, p_post_of[sat]);
                    const double x0_prev  = osc.x0;
                    // B3b/P0-3: finite-difference the drift over THIS satellite's true elapsed
                    // time (pl_now_s − osc.t_last), NOT the global epoch dt.  A satellite whose
                    // last clock-model value came several epochs ago — because intervening epochs
                    // were SKIPPED (gate/σ̂0 withhold, solve failure) or the sat was absent —
                    // would otherwise divide a multi-interval Δoffset by one dt and emit a drift
                    // ~N× too large as C1 next epoch.  Past a gap bound the pair is not usable
                    // (reset drift to 0); within it the true interval gives the correct rate.
                    const double sat_dt   = (osc.t_last > 0.0) ? (pl_now_s - osc.t_last) : dt;
                    const bool contiguous = (osc.t_last > 0.0) && sat_dt > 0.0
                                            && sat_dt <= clk_model_max_gap;   // R11
                    osc.x1  = contiguous ? (z - x0_prev) / sat_dt : 0.0;
                    osc.x0  = z;
                    osc.p00 = flagged ? ((clk_mon_reinit_var > 0.0) ? clk_mon_reinit_var : (30.0 * 30.0))
                                      : p_post;                            // epoch formal variance
                    osc.p11 = flagged ? (osc.p11 + 1e-2) : osc.p11;        // reopen drift on fault
                    osc.p01 = 0.0;   // offset/drift cross term not tracked in the direct set
                    osc.t_last = pl_now_s;   // P0-3: stamp this sat's update time
                    osc.init = true;
                    // Trace (network.trace_slip_events): the clk_mon columns are remapped for the
                    // model — clk_m=solved, median_m=predicted, dev_m=de-meaned innov, prev_clk_m=
                    // drift, jump_m=raw innov, abs_max_m=band(√S), jump_max_m=k, flag_abs=(nis>k).
                    if (clk_mon_trace.is_open()
                        && (nc.trace_clock_full || flagged || std::abs(nis) > 0.3 * clk_model_k))
                        clk_mon_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << sat << ',' << fmt::format("{:.4f}", z) << ','
                            << fmt::format("{:.4f}", pit->second.x0) << ','
                            << fmt::format("{:.4f}", dm) << ','
                            << fmt::format("{:.6f}", osc.x1) << ','
                            << fmt::format("{:.4f}", innov_of[sat]) << ','
                            << fmt::format("{:.4f}", std::sqrt(S)) << ','
                            << fmt::format("{:.1f}", clk_model_k) << ','
                            << (std::abs(nis) > clk_model_k ? 1 : 0) << ",0,"
                            << (ext ? 1 : 0) << ',' << (flagged ? 1 : 0) << ','
                            << fmt::format("{:.4f}", nis) << ",model\n";   // L4: nis + mode
                }
                if (n_flag > 0)
                    log->warn("Network[clock-model]: TOW {:.1f} flagged {} satellite clock(s) [{}] — "
                              "normalized innovation > {:.1f}σ against the two-state prediction band "
                              "(dynamics fault); model loosened + carried state reinitialized.",
                              se.time.tow, n_flag, detail, clk_model_k);
                // P0-3: per-satellite osc.t_last (stamped above) now records each sat's last
                // update time directly, so the contiguity / gap decision is exact even across
                // skipped epochs — the old whole-set prev_osc_sats marker is retired.
            }
        }

        // --- network.emit_clock_rate: decoupled C1 (clock-rate) estimator ---------------------
        // Finite-difference each satellite's POSTERIOR solved clock to derive the emitted C1,
        // WITHOUT injecting any prior or driving any refloat — so the network solution is
        // byte-identical to the off state; only clock_c1_m_s (default 0) changes.  This is the
        // recalibration-supported alternative to network.clock_model: the estimable is white-FM
        // (docs/CLOCK_HADAMARD_CALIB), so the two-state CARRY prior buys ~1× yet risks a NIS
        // storm, but the deterministic RATE emitted here is the exploitable stability (a
        // low-latency consumer extrapolates c0 + c1·dt instead of ZOH — the SSR saw-tooth fix).
        // Mirrors the clock-model drift computation's datum-wipe + gap/contiguity handling; runs
        // only when clock_model is OFF (else clock_osc already supplies C1 above).
        if (emit_rate_warm) {
            // Datum re-reference: the carried offsets are in the retired frame — drop them so a
            // datum STEP is not finite-differenced into a spurious rate (mirror clock_model).
            if (datum_changed && !scoped_handover) {
                clock_rate_est.clear();
            } else if (datum_changed) {
                for (auto rit = clock_rate_est.begin(); rit != clock_rate_est.end();) {
                    if (datumChangeAffectsKey(rit->first, changed_consts, /*scoped=*/true))
                        rit = clock_rate_est.erase(rit);
                    else
                        ++rit;
                }
            }
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                const double z = x_s(col);
                if (!std::isfinite(z) || sat.empty()) continue;
                ClockOsc& r = clock_rate_est[sat];   // default-constructs (init=false) on first sight
                // Finite-difference over THIS sat's true elapsed time; past the R11 gap bound the
                // pair is not usable → drift 0 (C1 ZOH).  First sighting / stale-return seeds x1=0.
                const double sat_dt = (r.init && r.t_last > 0.0) ? (pl_now_s - r.t_last) : 0.0;
                r.x1 = (sat_dt > 0.0 && sat_dt <= clk_model_max_gap) ? (z - r.x0) / sat_dt : 0.0;
                r.x0 = z;
                r.t_last = pl_now_s;
                r.init = true;
            }
        }

        // --- Per-satellite carried-clock sanity monitor (network.clock_monitor) --------------
        // The σ̂0 RAIM above flags contamination that MOVES the shared residual level (many
        // innocent stations spike at once).  A lone satellite whose carried clock has locked
        // into a physically impossible value is INVISIBLE to it — the clock absorbs the error
        // self-consistently, σ̂0 stays ≈1, and the stiff sat_clock_q carry then FREEZES the
        // wrong value for the rest of the run (the multi-hour single-satellite lock-in).  This
        // is the missing detector for that mode: on the FINAL (post-adaptation) solution, flag
        // any satellite whose DE-MEANED clock exceeds clk_mon_abs or whose one-epoch jump
        // exceeds clk_mon_jump, and refloat its carried shared state (the stash below skips it)
        // so the next epoch injects no stale prior and the clock re-estimates freely toward its
        // data / a-priori-anchor value.  De-meaning by the network median removes the per-epoch
        // datum gauge (a few metres, not error); the jump test runs only across adjacent
        // stable-datum epochs.  clock_refloat_sats may already hold within-epoch / post-fit RAIM
        // sats (only under clock_monitor_couple_raim); those are refloated here too (ext).
        //
        // Gated on the emitted-epoch warmup (NOT carry_warm): the D2 shared clock carry runs
        // independently of phase leveling, so the monitor must too; it only needs to be past the
        // cold-start systematic (the same pl_warmup horizon the ambiguity carry uses).  Superseded
        // by the two-state normalized-innovation test above when network.clock_model is on.
        if (clk_mon_on && !clk_model_on && static_cast<int>(n_emitted) >= pl_warmup) {
            std::map<std::string, double> clk_of;
            std::map<char, std::vector<double>> clk_by_const;  // per-constellation, for the datum median
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                const double v = x_s(col);
                if (!std::isfinite(v) || sat.empty()) continue;
                clk_of[sat] = v;
                clk_by_const[sat[0]].push_back(v);
            }
            // De-mean PER CONSTELLATION.  Each constellation has its OWN clock datum (a distinct
            // pivot satellite), so a whole-constellation offset — e.g. a Galileo-wide re-reference
            // when its pivot hands over — is a GAUGE artifact, not per-satellite error.  A single
            // global median would subtract the GPS datum from the Galileo clocks and flag the
            // entire constellation; the per-constellation median cancels that and leaves only the
            // genuine per-satellite outliers within each system.
            std::map<char, double> med_by_const;
            for (auto& [c, vv] : clk_by_const) {
                std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
                med_by_const[c] = vv[vv.size() / 2];
            }
            int n_new = 0;
            std::string detail;
            for (const auto& [sat, v] : clk_of) {
                const double med      = med_by_const[sat[0]];
                const double dev      = v - med;
                const bool   flag_abs = std::abs(dev) > clk_mon_abs;
                // Jump-test reference frame.  PIN mode: the raw clock v (the pivot pin
                // anchors the raw per-constellation frame across epochs, so v−prev_v is a
                // genuine per-sat step — byte-identical to the legacy path).  SUM-TO-ZERO
                // mode: the per-constellation DE-MEANED clock dev, because the raw
                // common-mode floats each epoch (Σδtˢ=0 is re-imposed on the current
                // satellite set, so a membership change redistributes ~Δ/N onto EVERY
                // survivor's raw clock — a datum artifact, not a per-sat jump).  De-meaning
                // by the constellation median removes that float, mirroring the two-state
                // clock-model NIS path's `innov − median`.  BROADCAST-ANCHORED mode keeps
                // the raw v like PIN: every satellite is anchored independently to its own
                // broadcast a-priori, so membership changes cause no redistribution and
                // v−prev_v is a genuine per-sat step.
                const double jref     = clock_sum_to_zero ? dev : v;
                bool   have_jump  = false;
                double jump       = 0.0;
                bool   flag_jump  = false;
                const auto pit = prev_clk.find(sat);
                // Jump only across ADJACENT epochs of a stable datum: a handover shifts every
                // clock of the re-referenced constellation (common-mode, not a per-sat fault)
                // and a long gap makes the step meaningless — both fall back to the absolute
                // bound.  RTA-2: under network.scoped_handover_response the datum is stable
                // for constellations whose pivot did NOT change, so their jump test keeps
                // running through a sibling's handover; OFF, handover_affects == datum_changed
                // and the legacy all-constellation suppression is byte-identical.
                if (pit != prev_clk.end() && !handover_affects(sat) && dt > 0.0
                    && (pl_now_s - pit->second.second) <= 1.5 * dt) {
                    have_jump = true;
                    jump      = jref - pit->second.first;   // datum-invariant under sum-to-zero (jref=dev), raw under pin (jref=v)
                    flag_jump = std::abs(jump) > clk_mon_jump;
                }
                const bool ext       = clock_refloat_sats.count(sat) > 0;  // within-epoch / post-fit RAIM
                const bool refloated = flag_abs || flag_jump || ext;
                if (trace_flag_rates) { ++fr_clk_mon.opp; if (flag_abs || flag_jump) ++fr_clk_mon.flag; }  // B8/T6 own bounds (excl. ext)
                if (refloated && !ext) { ++n_new; detail += (detail.empty() ? "" : ",") + sat;
                    emitEvent("T6_clock_monitor", "clk_monitor_refloat", sat, "", dev, clk_mon_abs,
                              "refloat",
                              have_jump ? fmt::format("jump={:.4f};jmax={:.2f}", jump, clk_mon_jump)
                                        : std::string("abs")); }
                if (refloated) clock_refloat_sats.insert(sat);

                if (clk_mon_trace.is_open()
                    && (nc.trace_clock_full || refloated || std::abs(dev) > 0.5 * clk_mon_abs
                        || (have_jump && std::abs(jump) > 0.5 * clk_mon_jump))) {
                    clk_mon_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << sat << ',' << fmt::format("{:.4f}", v) << ','
                        << fmt::format("{:.4f}", med) << ',' << fmt::format("{:.4f}", dev) << ',';
                    if (have_jump)
                        clk_mon_trace << fmt::format("{:.4f}", pit->second.first) << ','
                                      << fmt::format("{:.4f}", jump) << ',';
                    else
                        clk_mon_trace << ",,";
                    clk_mon_trace
                        << fmt::format("{:.2f}", clk_mon_abs) << ','
                        << fmt::format("{:.2f}", clk_mon_jump) << ','
                        << (flag_abs ? 1 : 0) << ',' << (flag_jump ? 1 : 0) << ','
                        << (ext ? 1 : 0) << ',' << (refloated ? 1 : 0) << ",,monitor\n";  // L4: empty nis + mode
                }

                // Re-baseline the jump test: a refloated sat's clock is about to be
                // reinitialized, so drop its history (next epoch's recovery must not read as a
                // fresh jump); otherwise record this epoch's jump reference (jref) + time as the
                // next adjacent baseline (raw v in pin mode; de-meaned dev in sum-to-zero mode).
                if (refloated) prev_clk.erase(sat);
                else           prev_clk[sat] = {jref, pl_now_s};
            }
            if (n_new > 0)
                log->warn("Network[clock-monitor]: TOW {:.1f} reinitialized {} satellite clock(s) [{}] "
                          "exceeding |Δδtˢ−median| > {:.1f} m or |jump| > {:.1f} m/epoch — carried "
                          "shared state {} to break the single-satellite lock-in.",
                          se.time.tow, n_new, detail, clk_mon_abs, clk_mon_jump,
                          (clk_mon_reinit_sig > 0.0)
                              ? fmt::format("loosened to σ={:.0f} m (soft large-q reinit)", clk_mon_reinit_sig)
                              : std::string("withheld from the stash (hard refloat)"));
        }

        // B1: after all post-emit QC tiers, record the flagged sats for the NEXT emit's
        // retro-bump (per-SATELLITE disc bump) + post-refloat URA inflation horizon.
        // The flag set is the union of clock_refloat_sats (clock-model / clock-monitor,
        // plus the coupled RAIM tiers) and retro_flag_sats (the tier-independent P0-2
        // accumulator) — so the retro-bump reaches every QC tier regardless of the
        // clock_monitor_couple_raim opt-in.
        if (retro_feed) {
            std::set<std::string> flagged_now;
            for (const std::string& sat : clock_refloat_sats)
                if (!sat.empty()) flagged_now.insert(sat);
            for (const std::string& sat : retro_flag_sats)
                if (!sat.empty()) flagged_now.insert(sat);

            if (ura_refloat_inflate > 1.0 && pl_warmup > 0)
                for (const std::string& sat : flagged_now) ura_refloat_horizon[sat] = pl_warmup;

            // Retro-bump is EDGE-triggered (P1-3): queue only a sat that just became
            // flagged (not already in retro_active), so a satellite that stays flagged
            // through several recovery epochs is reset ONCE at the consumer, not every
            // emit.  A sat that clears drops out of retro_active, so a later re-flag
            // re-arms the bump.  ura_retro_sats is consumed at the next emit.
            if (ura_retro) {
                for (const std::string& sat : flagged_now)
                    if (!retro_active.count(sat)) ura_retro_sats.insert(sat);
                retro_active = std::move(flagged_now);
            }

            // Age out the post-refloat inflation horizon one epoch at a time.
            if (ura_refloat_inflate > 1.0)
                for (auto it = ura_refloat_horizon.begin(); it != ura_refloat_horizon.end();) {
                    if (--it->second <= 0) it = ura_refloat_horizon.erase(it);
                    else ++it;
                }
        }
        subEnd(SB_CLOCKUPD);       // D0b: close block 8 (clock update + retro feed)
        subBegin(SB_STASH_STATUS); // D0b: D2/C1/SH carry re-stash + VCE fold + epoch-status QC

        // M2 (LS-VCE): snapshot the D2 shared-carry priors that were FOLDED this
        // epoch, keyed by identity, BEFORE the clear — their innovations against
        // the fresh solve are formed after the re-stash below.  Mirror the fold's
        // skip predicate so only ordinary (non-reinit, non-held, non-retired,
        // non-re-referenced) carries qualify.
        std::map<std::string, std::pair<double, double>> vce_shared_prior;  // key -> (prior value, var)
        if (carry_vce && (vce_wants_pb || vce_wants_cb)) {
            for (const CarryPrior& cp : prev_carry) {
                if (cp.inflate_q) continue;                       // clock-monitor reinit, not an ordinary carry
                if (cp.kind == SharedParamKind::PhaseBias) {
                    if (!vce_wants_pb || phase_hold_on) continue;                 // hold path ≠ a q-carry
                    if (eq34_reref.count({cp.sat, cp.signal})) continue;         // Card 3 D4: marginalized
                } else if (cp.kind == SharedParamKind::CodeBias) {
                    if (!vce_wants_cb) continue;
                } else {
                    continue;                                     // Clock / VtecSh out of scope (own models)
                }
                if (handover_affects(cp.sat)) continue;           // RTA-2 retired-datum (scoped mode)
                vce_shared_prior.emplace(sharedCarryKey(cp.kind, cp.sat, cp.signal),
                                         std::make_pair(cp.value, cp.var));
            }
        }

        // --- D2 stash for the next epoch ------------------------------------
        prev_carry.clear();
        prev_carry_cov.clear();   // C1: rebuilt below when the dense carry is on
        std::vector<Eigen::Index> fc_cols;  // C1: shared-border column per dense row
        // Fix A (covariance-carry consistency — the load-bearing companion to the
        // point-solve fold).  Under the anchor-free ensemble phase-bias datum the
        // gauge is realized POST-reduce by the rank-1 correction Ñ_ss += g gᵀ/β
        // (@ref NetworkNormalEquations::foldReducedSumZeroGauges), NOT by a local
        // pre-reduce pin as in the anchor mode.  A satellite phase-bias column is the
        // gauge (null) direction of the UNfolded reduced normals — the fold makes the
        // system invertible but leaves that column's slot of the reduced covariance
        // carrying the fold's ARTIFICIAL gauge-direction variance (an over-tight,
        // epoch-by-epoch self-reinforcing value set by the sum-to-zero reference, not
        // a physical uncertainty — the ensemble reference is re-realized EXACTLY every
        // epoch) plus its phase↔clock / phase↔code cross terms.  Feeding that column
        // into the D2 carry propagates the fold into next epoch's prior: the stiff,
        // gauge-referenced phase-bias prior biases the coupled satellite clock through
        // N_ss[clk,pb], clock_model amplifies it, and the pair runs away exponentially
        // from epoch 2 (measured ~7×/epoch → 5e8 m).  Cure: treat every ensemble
        // phase-bias gauge column as an EXACT datum (variance 0) in the covariance the
        // carry harvests — the exact analogue of how conditionalLocalCovariance /
        // conditionalLocalVarianceDiag zero the datum-PINNED shared columns before
        // reconstructing a conditional covariance ("datum-pinned rows/cols zeroed →
        // treat as exact").  Zeroing the ROW AND COLUMN drops both the over-tight
        // diagonal (→ the diagonal stash's var>0 guard skips the key) and the
        // phase↔clock/code cross-covariance (→ the key is excluded from the C1 dense
        // block too), while the gauge-INVARIANT clock / code-bias variances (which
        // the S-transform leaves identical to the anchor datum) pass through
        // untouched.  Continuity of the emitted phase bias is preserved by the LOCAL
        // ambiguity carry (phase_leveling) + the stable ensemble constraint, not by
        // this shared carry (which under the fold merely double-counted that info and
        // added the contamination).  Anchor / OFF mode: cov aliases
        // est.sharedCovariance() unchanged → byte-identical.  NB: zeroing the g
        // DIRECTION (g = λ·N_ss·e_pb, which has clock components) would instead corrupt
        // the correct clock covariance — the contaminated direction is the phase-bias
        // COLUMN e_pb, not g.
        const Eigen::MatrixXd& cov_src = est.sharedCovariance();
        Eigen::MatrixXd cov_ens;   // ensemble-only: the gauge-exact copy
        const bool ens_phase_gauge = config_.networkPhaseDatumEnsemble();
        if (ens_phase_gauge) {
            cov_ens = cov_src;
            est.zeroPhaseGaugeColumns(cov_ens);   // treat each folded gauge column as exact
        }
        const Eigen::MatrixXd& cov = ens_phase_gauge ? cov_ens : cov_src;
        std::set<Eigen::Index> pinned_cols;
        for (const auto& p : plan.shared_datum) pinned_cols.insert(p.column);
        auto stash = [&](SharedParamKind kind, const std::string& sat,
                         const std::string& signal, Eigen::Index col, bool inflate) {
            // Skip the datum-pinned columns.  Their solve covariance is 1/pin_scale
            // (not ~0), but the explicit datum re-pins them every epoch (its
            // Dirichlet elimination overrides any prior on them), and carrying one
            // forward would wrongly anchor a retired pivot to its old datum value
            // after a handover.
            if (pinned_cols.count(col)) return;
            const double var = cov(col, col);
            if (!(var > 0.0)) return;
            CarryPrior cp;
            cp.kind = kind; cp.sat = sat; cp.signal = signal;
            cp.value = x_s(col); cp.var = var; cp.inflate_q = inflate;
            prev_carry.push_back(cp);
            if (fullcov_on) {   // C1: register this entry's dense row (same skip rules)
                prev_carry_cov.row.emplace(sharedCarryKey(kind, sat, signal),
                                           fc_cols.size());
                fc_cols.push_back(col);
            }
        };
        // A satellite flagged by the clock monitor (or, opt-in, the within-epoch / post-fit RAIM)
        // has its carried SHARED state REINITIALIZED rather than frozen.  Two modes: with
        // clk_mon_reinit_sig > 0 (default) it is STILL stashed but marked inflate_q, so next epoch
        // folds it with the loose reinit variance (a weak anchor that lets a frozen clock walk to
        // truth while curbing the datum drift a hard refloat causes); with clk_mon_reinit_sig ≤ 0
        // it is WITHHELD entirely (hard skip-stash refloat — no prior).  clock_refloat_sats is
        // empty unless network.clock_monitor, so the stash is byte-identical when the monitor is
        // off (nothing flagged → inflate=false → the ordinary stiff-carry stash).
        const bool full_refloat = !(clk_mon_reinit_sig > 0.0);   // reinit_sigma ≤ 0 → hard skip-stash
        // Card 1d: carry this stash epoch's clock-monitor flags across to the next
        // inject (hard-refloat mode skip-stashes the flagged sat entirely, so the
        // flag cannot ride on prev_carry.inflate_q).  Gated to keep OFF runs pristine.
        if (carry_cpl_tu) prev_clock_refloat_sats = clock_refloat_sats;
        // The satellite clock is stashed to prev_carry ONLY in scalar-carry mode.  Under
        // network.clock_model the clock is carried by clock_osc (updated above), so it is not
        // stashed here — the two-state model is its sole cross-epoch memory.
        if (!clk_model_on)
            for (const std::string& sat : registered_clock_sats) {
                const bool flagged = clock_refloat_sats.count(sat) > 0;
                if (flagged && full_refloat) continue;
                stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
            }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (!parts) continue;
            // Card 4: the DD invariant carry is the phase time-link — do NOT also stash the
            // S-dependent shared PhaseBias carry (it would double-count in next epoch's D2 /
            // hold / dense fold).  Clock / code / SH / ZWD carries are unaffected.
            if (carry_invariant) continue;
            const bool flagged = clock_refloat_sats.count(parts->satellite) > 0;
            if (flagged && full_refloat) continue;
            const Eigen::Index col =
                est.phaseBiasColumn(parts->satellite, parts->signal);
            if (phase_hold_on) {
                // Seed-once + time-lock: carry b_s toward its realized value with the
                // fixed (clean) hold-variance — the temporal datum that stops the
                // ~metre/epoch re-realization.  Deliberately NOT cov(col,col): the
                // ensemble fold leaves that column carrying an artificial gauge
                // variance (carrying it is the epoch-2 fast runaway this whole fix
                // family guards).  A stiff, hand-set variance makes the carried value
                // the datum reference next epoch (buildEnsembleGauges then skips the
                // Σz=0 fold for it).  Datum-pinned columns (none for PB) and non-finite
                // values are still excluded.
                if (pinned_cols.count(col)) continue;
                const double v = x_s(col);
                if (!std::isfinite(v)) continue;
                CarryPrior cp;
                cp.kind = SharedParamKind::PhaseBias;
                cp.sat = parts->satellite; cp.signal = parts->signal;
                // inflate_q=false always: a clock-monitor SOFT refloat of this sat
                // (the loose-reinit path the legacy stash honors) deliberately does NOT
                // loosen the phase-bias hold — the phase-ensemble datum is orthogonal to
                // a clock fault, so the bias stays time-locked.  (A HARD refloat is still
                // handled: `flagged && full_refloat` skip-stashes above, so the sat
                // re-seeds next epoch.)
                cp.value = v; cp.var = phase_hold_var; cp.inflate_q = false;
                prev_carry.push_back(cp);
            } else {
                stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal,
                      col, flagged);
            }
        }
        // Card 2: refresh the time-linked previous-value map from THIS kept epoch's
        // solved code biases.  Cleared + rebuilt (a departed satellite drops out → its
        // return re-seeds IT alone) in lock-step with the D2 carry — same disposition
        // rules (a hard-refloated sat is skipped from BOTH, so it re-seeds next epoch,
        // its re-reference disc-signalled).  NOT snapshotted.  Empty/untouched when off.
        if (cb_time_linked) {
            code_bias_prev_.clear();
            // Advance the IF-pin comparison baseline to THIS kept epoch's pairs (a pair
            // recorded on a since-withheld epoch never reaches here, so the baseline
            // tracks emitted/kept datum realizations only).
            code_bias_ifpin_pair_ = this_epoch_cb_ifpin;
        }
        for (const auto& kv : registered_code_keys) {  // W6 satellite code bias (OSB)
            const bool flagged = clock_refloat_sats.count(kv.first) > 0;
            if (flagged && full_refloat) continue;
            const Eigen::Index col = est.codeBiasColumn(kv.first, kv.second);
            stash(SharedParamKind::CodeBias, kv.first, kv.second, col, flagged);
            if (cb_time_linked) {
                const double v = x_s(col);
                if (std::isfinite(v)) {
                    code_bias_prev_[NetworkSbasis::constellationOf(kv.first) + "|" + kv.second]
                                   [kv.first] = v;
                    code_bias_sat_seen_.insert(kv.first);
                }
            }
        }
        // C1: stash the dense covariance block over the clock/phase/code entries
        // just recorded (the SH block below keeps its own diagonal stash — its
        // carry is the separate pre-gate fold).  One O(k²) copy from the solve's
        // posterior.  A single non-finite entry clears the whole block — the next
        // fold then degrades per-key to the diagonal path instead of poisoning
        // the next epoch's normals (the exact crash mode the SH guard below
        // screens per-entry).
        if (fullcov_on && !fc_cols.empty()) {
            const Eigen::Index kfc = static_cast<Eigen::Index>(fc_cols.size());
            prev_carry_cov.P.resize(kfc, kfc);
            for (Eigen::Index a = 0; a < kfc; ++a)
                for (Eigen::Index b = 0; b < kfc; ++b)
                    prev_carry_cov.P(a, b) = cov(fc_cols[static_cast<std::size_t>(a)],
                                                 fc_cols[static_cast<std::size_t>(b)]);
            if (!prev_carry_cov.P.allFinite()) {
                log->warn("Network (C1): non-finite entries in the stashed shared-"
                          "covariance block ({} keys) — dense carry cleared; the next "
                          "epoch folds the diagonal carry.", fc_cols.size());
                prev_carry_cov.clear();
            }
        }
        if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coefficients
            for (const auto& c : sh_model->coeffs()) {
                const Eigen::Index col = est.vtecCoeffColumn(c.n, c.m, c.trig);
                if (pinned_cols.count(col)) continue;   // SH cols are never datum-pinned (safety)
                const double var = cov(col, col);
                const double val = x_s(col);
                // Never carry a non-finite value/variance forward — it would poison
                // the next epoch's normals and hard-crash the solve.  The mean-
                // reverting climatology prior re-seeds the coefficient cleanly.
                if (!(var > 0.0) || !std::isfinite(val) || !std::isfinite(var)) continue;
                CarryPrior cp;
                cp.kind = SharedParamKind::VtecSh;
                cp.sh_n = c.n; cp.sh_m = c.m; cp.sh_trig = c.trig;
                cp.value = val; cp.var = var;
                prev_carry.push_back(cp);
            }
        }

        // M2 (LS-VCE): form the PB/CB shared-carry innovations against this epoch's
        // fresh solve and fold them into the monitor (the D2 analogue of the
        // ambiguity-carry feed above).  In the re-stashed prev_carry, cp.value /
        // cp.var are the SOLVED value / posterior variance; the fold prior is the
        // snapshot taken before the clear.  Gated + best-effort skip-mirroring, so
        // OFF is byte-identical and a mis-mirrored contaminated key at most nudges
        // a monitor number (never the solve).
        if (carry_vce && !vce_shared_prior.empty()) {
            for (const CarryPrior& cp : prev_carry) {
                if (cp.inflate_q) continue;
                const bool is_pb = (cp.kind == SharedParamKind::PhaseBias) && vce_wants_pb;
                const bool is_cb = (cp.kind == SharedParamKind::CodeBias)  && vce_wants_cb;
                if (!is_pb && !is_cb) continue;
                const auto it = vce_shared_prior.find(
                    sharedCarryKey(cp.kind, cp.sat, cp.signal));
                if (it == vce_shared_prior.end()) continue;      // no folded prior last epoch
                const double var_prev = it->second.second;
                const double q     = is_cb ? q_cb : q_pb;
                const double floor = is_cb ? cb_var_floor : pb_var_floor;
                double fold_var = var_prev + q * std::max(0.0, dt);
                if (floor > 0.0) fold_var = std::max(fold_var, floor);
                carry_vce_est.addRow(is_cb ? "cb" : "pb", cp.sat,
                                     it->second.first - cp.value,   // carried − solved
                                     dt, cp.var, fold_var);
            }
        }

        // --- Trace ----------------------------------------------------------
        double sumsq = 0.0, maxabs = 0.0;
        for (const std::string& sat : registered_clock_sats) {
            const double v = x_s(est.clockColumn(sat));
            sumsq += v * v;
            maxabs = std::max(maxabs, std::abs(v));
        }
        const double rms = (n_clock > 0) ? std::sqrt(sumsq / n_clock) : 0.0;
        if (trace) {
            trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << n_present << ',' << n_clock << ',' << n_pb << ','
                  << plan.declared_dimension << ",1,1,"
                  << fmt::format("{:.4f}", rms) << ',' << fmt::format("{:.4f}", maxabs)
                  << ',' << total_pins << '\n';
        }
        // L1: the emitted-epoch disposition row — the median+std+worst-sat clock spread
        // (over the SAME registered_clock_sats / x_s(clockColumn) series as the rms above)
        // plus the QC action counts.  Computed only when the trace is open.
        if (epoch_status_trace.is_open()) {
            EpochStatusRow es;
            es.sigma0          = sigma0_adapt;   // 0.0 unless within-epoch RAIM ran this epoch
            es.n_adapt         = n_adapt;
            es.n_present       = n_present;
            es.n_clock_sats    = n_clock;
            es.n_refloat_sats  = static_cast<int>(clock_refloat_sats.size());
            es.n_retro_applied = static_cast<int>(retro_emit_sats.size());   // bumps APPLIED this emit
            es.n_arc_refloat   = n_arc_refloat_epoch;   // T2d: isolated-arc RAIM refloats this epoch
            es.datum_changed   = datum_changed ? 1 : 0;
            std::vector<double> cvals; cvals.reserve(registered_clock_sats.size());
            for (const std::string& sat : registered_clock_sats)
                cvals.push_back(x_s(est.clockColumn(sat)));
            if (!cvals.empty()) {
                std::vector<double> sorted = cvals;
                std::sort(sorted.begin(), sorted.end());
                const size_t m = sorted.size();
                const double median = (m % 2) ? sorted[m / 2]
                                              : 0.5 * (sorted[m / 2 - 1] + sorted[m / 2]);
                double ss = 0.0, worst = 0.0;
                std::string worst_sat_name;
                for (size_t k = 0; k < cvals.size(); ++k) {
                    const double d = cvals[k] - median;
                    ss += d * d;
                    if (std::abs(d) > worst) { worst = std::abs(d); worst_sat_name = registered_clock_sats[k]; }
                }
                es.clock_median_m = median;
                es.clock_std_m    = std::sqrt(ss / static_cast<double>(m));
                es.worst_sat      = worst_sat_name;
                es.worst_dev_m    = worst;
            }
            emitEpochStatus("emitted", es);
        }

        if (n_emitted == 1 || n_emitted % 60 == 0) {
            log->info("Network: TOW {:.1f} emitted epoch {} — {} stations, {} sat clocks, "
                      "{} phase biases, {} code biases ({} single-station sat(s) dropped), "
                      "datum dim {}, clock-Δ rms {:.3f} m",
                      se.time.tow, n_emitted, n_present, n_clock, n_pb, n_cb, n_dropped_sats,
                      plan.declared_dimension, rms);
            if (config_.estimateNetworkIonoSh() && sh_model) {
                double a00 = 0.0;
                try { a00 = x_s(est.vtecCoeffColumn(0, 0, 'C')); } catch (...) {}
                log->info("Network[SH-iono]:   mean VTEC (a00) = {:.2f} TECU over {} coeffs",
                          a00, sh_model->numCoeffs());
            }
        }

        prev_week = se.time.week; prev_tow = se.time.tow;
        subEnd(SB_STASH_STATUS);   // D0b: close the stash + epoch-status tail (before the readout)

        // D0: per-epoch section-timer readout (emitted-epoch path).  Deltas of the
        // estimator's run-cumulative accumulators against the loop-top snapshot give
        // THIS epoch's kernel cost.  The driver assembly wall (sec_asm_ms_epoch) covers
        // the primary produce+scatter section; reduce/solve/cov-inverse/cov-consumers
        // happen after the wall, so `other` (QC decision logic + emit + I/O + any
        // within-epoch RAIM re-solve) = total − assembly − reduce − solve − cov-inverse
        // − cov-consumers.  d_estadd is the kernel est.addStation delta — a CROSS-CUTTING
        // figure (it also captures re-solve adds that fall AFTER the wall, so it is NOT a
        // strict sub-part of the assembly wall) reported separately, not nested under it.
        if (sec_timers_on) {
            const auto& stmr = est.sectionTimers();
            const double d_total  = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sec_epoch_t0).count();
            const double d_reduce = stmr.reduce_ms        - sec_t0.reduce_ms;
            const double d_solve  = stmr.solve_ms         - sec_t0.solve_ms;
            const double d_covinv = stmr.cov_inverse_ms   - sec_t0.cov_inverse_ms;
            const double d_covcon = stmr.cov_consumers_ms - sec_t0.cov_consumers_ms;
            const double d_estadd = stmr.add_station_ms   - sec_t0.add_station_ms;
            const std::size_t d_covcalls =
                stmr.cov_consumer_calls - sec_t0.cov_consumer_calls;
            const double d_other = d_total - sec_asm_ms_epoch - d_reduce - d_solve
                                   - d_covinv - d_covcon;
            ++sec_profiled_epochs;
            log->info("Network (D0) TOW {:.1f}: total {:.1f} ms | assembly-wall {:.1f} "
                      "| reduce {:.1f} | solve {:.1f} | cov-inv {:.1f} | cov-consumers {:.1f} "
                      "({} calls) | other {:.1f} | est.addStation {:.1f} (cross-cutting) | "
                      "{} shared cols, {} stations",
                      se.time.tow, d_total, sec_asm_ms_epoch, d_reduce, d_solve,
                      d_covinv, d_covcon, d_covcalls, d_other, d_estadd,
                      static_cast<long long>(est.numShared()), n_present);

            // D0b: per-epoch sub-block attribution of `other`.  net = wall − Δ(timed
            // sections) inside each block; Σ net should approach d_other, and the residual
            // is logged as "unattributed" (loop regions this instrument does not bracket).
            std::string d0b;
            double d0b_net_sum = 0.0;
            for (int b = 0; b < SB_COUNT; ++b) {
                const double net = (sec_sub[b].wall_ms  - sec_sub_ep0[b].wall_ms)
                                 - (sec_sub[b].timed_ms - sec_sub_ep0[b].timed_ms);
                d0b_net_sum += net;
                d0b += fmt::format("{}={:.1f} ", kSubNames[b], net);
            }
            log->info("Network (D0b) TOW {:.1f}: other-net ms [ {}] Σnet {:.1f} | other {:.1f} "
                      "| unattributed {:.1f}",
                      se.time.tow, d0b, d0b_net_sum, d_other, d_other - d0b_net_sum);
        }

        // --- RTB-7(a): periodic atomic state snapshot -------------------------
        // End-of-epoch, EMITTED epochs only (skipped epochs `continue` above and
        // mutate no carry state), every state_snapshot_interval_s of PRODUCT
        // time.  Written AFTER this epoch's product emit + flush, to the
        // snapshot's own temp file + rename — the product writer is never
        // flushed or reordered by this block (emission is unperturbed).  Path
        // empty (default) ⇒ byte-identical.
        if (!snap_path.empty()
            && (last_snapshot_write_s < 0.0
                || pl_now_s - last_snapshot_write_s >= nc.state_snapshot_interval_s)) {
            // RTB-7 cold-state exclusions (in-tree record of a decision previously
            // documented only in memory/report): this snapshot carries ONLY shared,
            // network-level producer state — the cross-epoch carries (phase-level /
            // iono / ZWD), clock oscillators, pivots, generation id, URA/ramp metrics
            // and the emitter's disc-counter / baseline / first-sighting bookkeeping.
            // Per-STATION filter state is DELIBERATELY NOT snapshotted: per-station
            // cycle-slip arcs and slip-detector history, per-station StateManager
            // ambiguity / iono / receiver-bias marks, and per-station covariance.  On
            // a warm resume those layers cold-start and re-warm within a few epochs,
            // and the post-fit RAIM safety net absorbs the transient — serializing the
            // large per-station covariance blocks would buy nothing the re-warm does
            // not already provide.  That is the recovery-budget trade RTB-7 accepts.
            NetworkStateSnapshot snap;
            snap.snap_week          = se.time.week;
            snap.snap_tow           = se.time.tow;
            snap.wall_utc           = NetworkStateSnapshotIO::utcNowIso();
            snap.generation_id      = generation_id;
            snap.config_fingerprint = live_fingerprint;
            snap.n_emitted          = n_emitted;
            snap.n_skipped          = n_skipped;
            snap.n_gate_withheld    = n_gate_withheld;
            snap.n_sigma0_withheld  = n_sigma0_withheld;
            snap.n_runaway_epochs   = n_runaway_epochs;
            snap.n_perconst_partial = n_perconst_partial_epochs;
            snap.n_perconst_diffuse = n_perconst_diffuse_published;
            snap.iod                = iod;
            snap.prev_week          = prev_week;
            snap.prev_tow           = prev_tow;
            snap.last_emit_content_s = last_emit_content_s;
            snap.last_health_write_s = last_health_write_s;
            snap.ura_sigma0_ewma      = ura_sigma0_ewma;
            snap.ura_ewma_updates     = ura_ewma_updates;
            snap.ramp_lifted          = ramp_lifted;
            snap.first_emit_content_s = first_emit_content_s;
            snap.shared_carry.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                NetworkStateSnapshot::SharedCarry sc;
                sc.kind      = static_cast<int>(cp.kind);
                sc.sat       = cp.sat;
                sc.signal    = cp.signal;
                sc.sh_n      = cp.sh_n;
                sc.sh_m      = cp.sh_m;
                sc.sh_trig   = static_cast<int>(cp.sh_trig);
                sc.value     = cp.value;
                sc.var       = cp.var;
                sc.inflate_q = cp.inflate_q;
                snap.shared_carry.push_back(std::move(sc));
            }
            snap.prev_pivot    = prev_pivot;
            snap.last_pivot    = last_pivot;
            snap.c1_zoh_active = c1_zoh_active;
            snap.prev_clk      = prev_clk;
            for (const auto& [sat, o] : clock_osc) {
                NetworkStateSnapshot::Osc so;
                so.x0 = o.x0; so.x1 = o.x1;
                so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
                so.t_last = o.t_last; so.init = o.init;
                snap.clock_osc[sat] = so;
            }
            for (const auto& [sat, o] : clock_rate_est) {   // network.emit_clock_rate decoupled C1 state
                NetworkStateSnapshot::Osc so;
                so.x0 = o.x0; so.x1 = o.x1;
                so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
                so.t_last = o.t_last; so.init = o.init;
                snap.clock_rate_est[sat] = so;
            }
            for (const auto& [key, lc] : local_carry)
                snap.local_carry[key] =
                    NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            for (const auto& [idx, lc] : zwd_carry)
                snap.zwd_carry[idx] =
                    NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            snap.ura_refloat_horizon = ura_refloat_horizon;
            snap.ura_retro_sats      = ura_retro_sats;
            snap.retro_active        = retro_active;
            snap.relevel_iode              = relevel_iode;              // Card 6
            snap.eph_cutover_disc_pending  = eph_cutover_disc_pending;  // Card 6
            {
                std::ostringstream es;
                emitter.serializeState(es);
                snap.emitter_state = es.str();
            }
            std::string werr;
            if (!NetworkStateSnapshotIO::write(snap_path, snap, &werr))
                log->warn("Network (RTB-7): state-snapshot write to '{}' failed ({}) — the "
                          "previous good snapshot (if any) is left intact.",
                          snap_path.string(), werr);
            last_snapshot_write_s = pl_now_s;
        }
    }

    if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);  // restore Eigen's own threading

    double loop_ms = 0.0;   // L3/L15: hoisted out of the timing block so the run-summary CSV / manifest can read it
    {  // [TIMING] per-epoch scaling measurement (loop wall only; startup excluded)
        loop_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_loop_begin).count();
        log->info("Network[timing]: epoch-loop wall {:.1f} ms over {} epochs = {:.2f} ms/epoch "
                  "| {} stations | {} | threads {}",
                  loop_ms, n_loop_epochs,
                  n_loop_epochs ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0,
                  stations.size(), parallel_on ? "PARALLEL" : "SERIAL", n_threads);
    }

    // === Tier D0: section-timer RUN TOTALS ===================================
    // The whole-run breakdown that decides which Tier-D lever matters.  The kernel
    // accumulators are run-cumulative (assembly / reduce / solve / cov-inverse /
    // cov-consumers); `other` = the epoch-loop wall minus those + the driver
    // assembly wall (QC decision logic, emit, I/O, tracers).  Only the ratios are
    // load-bearing — absolute ms depend on the machine and the parallel settings.
    //
    // On the card's "QC tiers" section: the RAIM / attribution / clock-monitor tiers
    // are not a wall-separable phase — they are interleaved with and dominated by
    // covariance-consumer calls (rowRedundancy / reconstructStation), which ARE timed
    // and attributed to cov-consumers above; their residual decision logic (median
    // polish, standardization, thresholding) is cheap and falls into `other`.  So the
    // D1-relevant QC cost is already isolated in cov-consumers — bracketing the
    // scattered QC blocks would only double-count those covariance calls.
    if (sec_timers_on) {
        const auto& stmr = est.sectionTimers();
        const double other_ms = loop_ms - sec_asm_wall_ms - stmr.reduce_ms - stmr.solve_ms
                                - stmr.cov_inverse_ms - stmr.cov_consumers_ms;
        auto pct = [&](double ms) {
            return loop_ms > 0.0 ? 100.0 * ms / loop_ms : 0.0;
        };
        log->info("Network (D0) section-timer RUN TOTALS over {} profiled / {} loop epochs "
                  "(epoch-loop wall {:.0f} ms):", sec_profiled_epochs, n_loop_epochs, loop_ms);
        log->info("Network (D0)   assembly-wall {:>9.0f} ms ({:4.1f}%)  [primary produce+scatter]",
                  sec_asm_wall_ms, pct(sec_asm_wall_ms));
        log->info("Network (D0)   reduce        {:>9.0f} ms ({:4.1f}%)  [{} recomputes]",
                  stmr.reduce_ms, pct(stmr.reduce_ms), stmr.reduce_recomputes);
        log->info("Network (D0)   solve         {:>9.0f} ms ({:4.1f}%)  [{} solves]",
                  stmr.solve_ms, pct(stmr.solve_ms), stmr.solve_calls);
        log->info("Network (D0)   cov-inverse   {:>9.0f} ms ({:4.1f}%)  [D1 selected-inverse target]",
                  stmr.cov_inverse_ms, pct(stmr.cov_inverse_ms));
        log->info("Network (D0)   cov-consumers {:>9.0f} ms ({:4.1f}%)  [{} calls; D1 per-station target]",
                  stmr.cov_consumers_ms, pct(stmr.cov_consumers_ms), stmr.cov_consumer_calls);
        log->info("Network (D0)   other         {:>9.0f} ms ({:4.1f}%)  [QC / emit / I/O / tracers]",
                  other_ms, pct(other_ms));
        // Cross-cutting: kernel est.addStation (normal-forming + N_rr factorization).  Mostly
        // inside the assembly wall, but also captures within-epoch RAIM re-solve adds that fall
        // after the wall (in `other`), so it is NOT a strict sub-part of the assembly-wall bucket.
        log->info("Network (D0)   est.addStation {:>8.0f} ms ({:4.1f}%)  [{} adds; cross-cutting — "
                  "overlaps assembly-wall + any re-solve in other]",
                  stmr.add_station_ms, pct(stmr.add_station_ms), stmr.add_station_calls);
        // P0b: foldReducedDdPriors internal breakdown (serial-fold path only, i.e. under
        // parallel_carry) — which sub-part of the 49%-of-epoch DD fold dominates.
        if (stmr.fold_loop_ms + stmr.fold_minv_ms + stmr.fold_gmg_ms > 0.0)
            log->info("Network (D0)   foldDdPriors: per-station-loop {:>8.0f} ms ({:4.1f}%) | "
                      "M-inverse {:>8.0f} ms ({:4.1f}%) | GMGᵀ {:>7.0f} ms ({:4.1f}%)  [serial-fold path]",
                      stmr.fold_loop_ms, pct(stmr.fold_loop_ms),
                      stmr.fold_minv_ms, pct(stmr.fold_minv_ms),
                      stmr.fold_gmg_ms, pct(stmr.fold_gmg_ms));

        // === INC-C0: the invariant-carry driver's per-op split + census =========
        // Splits the SB_DDBUILD / SB_INSTALL wall brackets (which lump the whole driver)
        // into the primitive ops the perf-restructure card ranks — the dense-K² change-of-
        // basis projection (buildDdPriors) and posterior read-back (installPosterior) that
        // INC-C2's factored (A + U Σ_ss Uᵀ) store removes.  The census reconciles the
        // "K≈7834 never marginalized" finding (§0): whether marginalization has nothing to
        // kill (giant live components, case (a)) or the kill path is silent (case (b)).
        if (carry_invariant) {
            const auto& dp = carry_driver.perfTimers();
            log->info("Network (D0/INC-C0) carry-driver per-op over {} build / {} serial-install "
                      "epoch(s): basis-project {:>8.0f} ms ({:4.1f}%) | post-mean {:>7.0f} ms "
                      "({:4.1f}%) | post-cov {:>8.0f} ms ({:4.1f}%) | D5-gate {:>6.0f} ms ({:4.1f}%) "
                      "| time-update {:>5.0f} ms  [basis-project + post-cov = the dense-K² ops INC-C2 factors]",
                      dp.build_epochs, dp.install_epochs,
                      dp.basis_project_ms, pct(dp.basis_project_ms),
                      dp.post_mean_ms, pct(dp.post_mean_ms),
                      dp.post_cov_ms, pct(dp.post_cov_ms),
                      dp.d5_gate_ms, pct(dp.d5_gate_ms), dp.time_update_ms);
            const auto& cc = carry_driver.counters();
            const long long marg_total = cc.chords_marginalized + cc.chords_marg_perchord;
            const long long chords_folded = cc.chords_carried + cc.chords_born;
            log->info("Network (D0/INC-C0) carry census: max K_c {} (single largest component) | "
                      "peak z-dim {} | marginalizations {} ({} per-chord + {} whole-block) | "
                      "births/tap-offs {} | slip refloats {}.",
                      cc.max_component_kc, cc.max_z_dim, marg_total,
                      cc.chords_marg_perchord, cc.chords_marginalized,
                      cc.chords_born, cc.slip_kills);
            // INC-C0 D-c viability: short fundamental cycles ⇒ sparse DD information matrix
            // J = Pᶻ⁻¹ ⇒ the exact info-form carry (SPEC_carry_factored_pz_C2 D-c) is bounded;
            // long cycles ⇒ dense J ⇒ D-c gives no bound (choose D-b/component-cap instead).
            log->info("Network (D0/INC-C0) D-c viability: max fundamental-cycle length {} | "
                      "mean {:.1f} over {} folded chord(s) — {} (short ⇒ sparse J ⇒ D-c bounded; "
                      "long ⇒ dense J ⇒ dense per-block cost is intrinsic).",
                      cc.max_cycle_len,
                      chords_folded ? static_cast<double>(cc.sum_cycle_len)
                                      / static_cast<double>(chords_folded) : 0.0,
                      chords_folded,
                      cc.max_cycle_len <= 8 ? "SPARSE-J likely" : "DENSE-J risk");
            // INC-C1 growth bound (also a 12 h robustness assert): a large, still-growing carry
            // with ZERO marginalization over a long run means dead chords are being retained —
            // the §0 case (b) kill-path bug.  A large per-chord count instead means case (a).
            if (marg_total == 0 && cc.max_z_dim > 0 && cc.epochs > 100)
                log->warn("Network (INC-C1 growth bound): {} carried DD(s) at peak with ZERO "
                          "marginalization(s) over {} driven epoch(s) — dead chords are NOT being "
                          "killed (INC-C0 §0 case (b)); K grows unbounded.  Audit the kill path "
                          "(NetworkInvariantCarryDriver::buildDdPriors projection-W column drop).",
                          cc.max_z_dim, cc.epochs);
        }

        // === Tier D0b: `other` sub-block RUN TOTALS, ranked =================
        // net = wall − Δ(reduce+solve+cov-inverse+cov-consumers) inside each block, so the
        // recon/solve already in the five D0 sections above is not double-counted.  Σ net vs
        // the D0 `other` figure closes the accounting; "unattributed" = loop regions no
        // bracket covers (pre-slip pivot pass, the trace/readout tail, small comment gaps).
        // This is the ranked table the perlos card acts on (HANDOFF_perlos_parallel_sweeps
        // §3 gate P0): P1/P2 target the measured top blocks, not §2's guesses.
        std::vector<int> d0b_order(SB_COUNT);
        for (int b = 0; b < SB_COUNT; ++b) d0b_order[b] = b;
        std::sort(d0b_order.begin(), d0b_order.end(), [&](int a, int c) {
            return (sec_sub[a].wall_ms - sec_sub[a].timed_ms)
                 > (sec_sub[c].wall_ms - sec_sub[c].timed_ms);
        });
        double d0b_sum_net = 0.0;
        for (const SubBlockTimer& s : sec_sub) d0b_sum_net += s.wall_ms - s.timed_ms;
        log->info("Network (D0b) `other` sub-block RUN TOTALS over {} profiled epoch(s) "
                  "(ranked by net; net excludes recon/solve already in the D0 sections):",
                  sec_profiled_epochs);
        for (int b : d0b_order) {
            const SubBlockTimer& s = sec_sub[b];
            const double net = s.wall_ms - s.timed_ms;
            log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  | wall {:>9.0f} ms "
                      "| cov-consumers {:>8.0f} ms ({} calls)",
                      kSubNames[b], net, pct(net), s.wall_ms, s.covcon_ms, s.covcalls);
        }
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [Σ over blocks]",
                  "TOTAL", d0b_sum_net, pct(d0b_sum_net));
        log->info("Network (D0b)   {:<13} net {:>9.0f} ms ({:4.1f}%)  [D0 `other` − Σ; loop "
                  "regions no bracket covers]", "unattributed",
                  other_ms - d0b_sum_net, pct(other_ms - d0b_sum_net));
    }

    if (trace) trace.close();
    if (soln) soln.close();
    if (iono_trace.is_open()) iono_trace.close();
    if (vtec_sh_trace.is_open()) vtec_sh_trace.close();
    if (cl_trace) cl_trace.close();
    if (if_slip_trace.is_open()) if_slip_trace.close();
    if (raim_trace.is_open()) raim_trace.close();
    if (raim_attrib_trace.is_open()) raim_attrib_trace.close();
    if (epoch_status_trace.is_open()) epoch_status_trace.close();
    if (health_trace.is_open()) health_trace.close();
    if (events_trace.is_open()) events_trace.close();
    if (zwd_trace.is_open()) zwd_trace.close();
    if (station_res_trace.is_open()) station_res_trace.close();
    if (carry_vce_trace.is_open()) carry_vce_trace.close();

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate, {} by the A4 σ̂0 ceiling) — "
              "of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, synced.size());
    if (config_.networkPhaseDatumEq34())
        log->info("Network (Card 3/eq34): {} (sat,σ) re-reference(s), {} local carry "
                  "value(s) erased, {} carry value(s) S-transformed (v2-shift), "
                  "{} gap re-pin(s) (I2 extend-only: arcs resumed to their pre-gap "
                  "integer), {} pin/gauge-mismatch epoch(s) over the run.",
                  eq34_reref_total, eq34_erase_total, eq34_shift_total,
                  eq34_gap_repin_total, eq34_mismatch_epochs);
    if (carry_invariant)
        log->info("Network (Card 4/M1 invariant carry): {} epoch(s) driven, {} carried DD(s) at "
                  "run end; {} chord birth(s), {} marginalization(s), {} slip refloat(s), "
                  "{} unresolvable-cycle drop(s), {} tree swap(s) (basis re-expression), "
                  "{} component merge(s), {} split(s) over the run "
                  "(S-system-invariant DDs; q_z={} m²/s).",
                  carry_epochs, carry_zdim_last, carry_born_total, carry_marg_total,
                  carry_slipkills_total, carry_dropped_total,
                  carry_swaps_total, carry_merges_total, carry_splits_total,
                  config_.networkCarryInvariantQz());
    if (mw_wl_on) {
        const double mean_R = mw_wl_run.rows_built > 0
            ? mw_wl_run.sum_R / static_cast<double>(mw_wl_run.rows_built) : 0.0;
        log->info("Network (M5/WL-1 MW-seeded wide-lane): {} WL row(s) folded over {} primary "
                  "cycle(s) inspected (mean R = {:.4f} cyc², i.e. σ ≈ {:.3f} cyc); {} capped by "
                  "max_rows_per_epoch.  Skips — {} non-primary group, {} missing L2 band, {} no "
                  "MW arc (channel/pivot), {} young arc (< {} ep), {} bad λ, {} mixed bias "
                  "frame.  Cross-check the KF-WL circR vs scripts/mw_census.py (Phase-A success "
                  "= circR climbing toward ~0.6 as arcs mature; SPEC §7.5).",
                  mw_wl_run.rows_built, mw_wl_run.cycles_seen, mean_R, std::sqrt(mean_R),
                  mw_wl_run.rows_capped, mw_wl_run.skip_not_primary, mw_wl_run.skip_missing_band,
                  mw_wl_run.skip_missing_mw, mw_wl_run.skip_young_arc,
                  config_.getNetworkConfig().mw_wl.min_arc_epochs, mw_wl_run.skip_bad_lambda,
                  mw_wl_run.skip_mixed_frame);
    }
    if (ewl_on) {
        // UWL-1: one line per lane instance — per-constellation when the card is armed
        // (a pooled line hides which lane is storming; an armed constellation that
        // decided 0 cycles is a LOUD 0-line, invariant #4), the legacy pooled line
        // otherwise (lane.cons == 0).
        for (const auto& lane : ewl_lanes) {
            const auto& er = lane.run;
            const double mean_R = er.rows_built > 0
                ? er.sum_R / static_cast<double>(er.rows_built) : 0.0;
            const char* label = lane.cons == '\0' ? "all-cons"
                              : lane.cons == 'G'  ? "GPS EWL (L2,L5) λ 5.861 m"
                              : lane.cons == 'E'
                                  ? (lane.top == '8'
                                         ? (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5,E5a) λ 19.537 m [preferred]"
                                                : "Galileo UWL (E5,E5a) λ 19.537 m")
                                         : (nmw.uwl_dual_top
                                                ? "Galileo UWL (E5b,E5a) λ 9.768 m [fallback]"
                                                : "Galileo UWL (E5b,E5a) λ 9.768 m"))
                                  : "?";
            log->info("Network (M5/WL-2 {} lane): {} row(s) decided ({} INTEGER-FIXED via FFR, {} "
                      "cold-fixed n<2, {} cold-unfixed; rows {} into the solve) over "
                      "{} primary cycle(s) inspected (mean R = {:.4f} cyc²); {} capped.  Skips — {} "
                      "non-primary, {} missing band, {} no MW arc, {} young arc (< {} ep), {} bad "
                      "λ, {} mixed frame, {} x_lin quarantine.  Uncarried top band; the integers feed "
                      "the WL bridge from the STATISTIC; success = fixes by ep ~{}.",
                      label, er.rows_built + er.rows_fixed_cold, er.rows_fixed,
                      er.rows_fixed_cold, er.rows_cold_unfixed,
                      config_.getNetworkConfig().mw_wl.ewl_fold_rows ? "FOLDED" : "NOT folded",
                      er.cycles_seen, mean_R,
                      er.rows_capped, er.skip_not_primary, er.skip_missing_band,
                      er.skip_missing_mw, er.skip_young_arc,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs, er.skip_bad_lambda,
                      er.skip_mixed_frame, er.skip_quarantine,
                      config_.getNetworkConfig().mw_wl.ewl_min_arc_epochs + 1);
        }
    }
    if (wl_inject_on) {
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-2 WL lane, phase bridge + inject): {} EWL-fixed row(s) bridged → "
                  "{} WL cycle(s) decided (mean bridge R = {:.4f} cyc², σ ≈ {:.3f} cyc, iono-de-biased); "
                  "{} hold row(s) folded, {} inject(s) (mean |Δlane| = {:.2f} cyc).  Skips — {} no/young "
                  "bridge arc, {} no iono estimate, {} bridge σ>{} guard, {} FFR-unfixed, {} MW(1,2) "
                  "closure, {} x_lin quarantine, {} not-1:1 basis.  Bridge = phase-only, DD-iono removed "
                  "via the network estimate; residual estimate-error gated by FFR |frac| + MW(1,2) "
                  "closure (§3.4 v2); inject moves the carried lane to the integer, NL preserved (§3.3).",
                  wl_run.rows_seen, wl_run.cycles_decided, mean_R, std::sqrt(mean_R),
                  wl_run.holds_folded, wl_run.injects, med_dl,
                  wl_run.skip_bridge_missing, wl_run.skip_no_iono, wl_run.skip_bridge_wide,
                  config_.getNetworkConfig().mw_wl.bridge_max_sigma_cyc,
                  wl_run.skip_not_fixed, wl_run.skip_closure, wl_run.skip_quarantine,
                  wl_run.skip_basis);
    }
    if (uwl_on && (wl_inject_on || lane_on)) {
        // UWL-1: the per-constellation WL-walk breakdown (never pooled-only — SPEC §2).
        for (const auto& [wcons, wr] : wl_cons_run) {
            const double mean_R = wr.cycles_decided > 0
                ? wr.sum_bridge_R / static_cast<double>(wr.cycles_decided) : 0.0;
            const double med_dl = wr.injects > 0
                ? wr.inject_abs_sum / static_cast<double>(wr.injects) : 0.0;
            log->info("Network (M5/UWL-1 '{}' WL walk): {} row(s) bridged → {} decided (mean "
                      "bridge R = {:.4f} cyc², {} via COLD bridge, {} via E6 MID hop), {} hold(s), "
                      "{} lane fix(es) moved + {} re-pinned, {} inject(s) (mean |Δlane| = {:.2f} "
                      "cyc).  Skips — {} no/young bridge, {} no-iono, {} bridge-wide, {} "
                      "FFR-unfixed, {} closure, {} quarantine, {} basis; mid path — {} FFR "
                      "fallback, {} MW(6,5) closure, {} direct-disagree (skipped).",
                      wcons, wr.rows_seen, wr.cycles_decided, mean_R, wr.cold_bridge_cycles,
                      wr.mid_decided,
                      wr.holds_folded, wr.lane_fix_folded, wr.lane_fix_unchanged,
                      wr.injects, med_dl,
                      wr.skip_bridge_missing, wr.skip_no_iono, wr.skip_bridge_wide,
                      wr.skip_not_fixed, wr.skip_closure, wr.skip_quarantine, wr.skip_basis,
                      wr.mid_fallback, wr.mid_closure, wr.mid_disagree);
            // Increment 5 (uwl_dual_top): the dual-rung + sibling-closure breakdown.
            // mean|frac| ≈ 0 ⇒ the 2·N(8,5)=N(7,5) identity holds on this data;
            // ≈ 0.5 ⇒ a receiver half-cycle convention offsets it (learn a constant
            // BEFORE arming uwl_sib_gate — SPEC reconciliation note 2).
            if (nmw.uwl_dual_top
                && (wr.top2_decided || wr.dual_dedupe || wr.sib_checked
                    || wr.sib_unavail || wr.sib_blocked))
                log->info("Network (M5/UWL-1 '{}' dual-top): {} of {} decided via the "
                          "FALLBACK (E5b,E5a) rung, {} fallback row(s) deduped (preferred "
                          "already decided).  Sibling closure: {} checked (mean |frac| = "
                          "{:.3f}), {} off-integer, {} unmeasurable, {} BLOCKED (gate {}).",
                          wcons, wr.top2_decided, wr.cycles_decided, wr.dual_dedupe,
                          wr.sib_checked,
                          wr.sib_checked > 0
                              ? wr.sib_abs_frac_sum / static_cast<double>(wr.sib_checked)
                              : 0.0,
                          wr.sib_int_off, wr.sib_unavail, wr.sib_blocked,
                          nmw.uwl_sib_gate ? "ON" : "off/measure-only");
        }
    }
    if (lane_on) {
        // WL-3 run-end line (invariant #4: counters or it didn't happen).  In the LANE
        // shape the interesting numbers are the continuous increments + fold-on-change
        // ratio + the inject-RETIREMENT count (≈0 expected — the WL-2 fallback path).
        const double mean_R = wl_run.cycles_decided > 0
            ? wl_run.sum_bridge_R / static_cast<double>(wl_run.cycles_decided) : 0.0;
        const double med_dl = wl_run.injects > 0
            ? wl_run.inject_abs_sum / static_cast<double>(wl_run.injects) : 0.0;
        log->info("Network (M5/WL-3 lane-coordinate carry): {} pair component(s) reconciled "
                  "({} pair arc(s), {} residual primary-band arc(s), {} tree-tag mismatch(es), "
                  "{} spanning-forest completion(s)); "
                  "lane increments folded — {} MW(1,2) row(s), {} bridge row(s), {} skipped "
                  "(immature/stale/mixed/slip/quarantine).  FFR lane fixing: {} decided "
                  "cycle(s) (mean bridge R = {:.4f} cyc²) → {} integer(s) MOVED/new, {} "
                  "re-pinned (idempotent lane SET — information-once-only).  Inject FALLBACK: "
                  "{} fired (mean |Δlane| = {:.2f} cyc; ≈0 expected — continuous tightening "
                  "supersedes the WL-2 injects), {} not-1:1 basis skips.",
                  carry_driver.counters().pair_components,
                  lane_pair_arcs, lane_resid_arcs, lane_pair_tree_mismatch,
                  carry_driver.counters().pair_tree_promotions,
                  wl_run.lane_mw_rows, wl_run.lane_bridge_rows, wl_run.lane_skip_meas,
                  wl_run.cycles_decided, mean_R,
                  wl_run.lane_fix_folded, wl_run.lane_fix_unchanged,
                  wl_run.injects, med_dl, wl_run.skip_basis);
    }
    if (carry_innov_gate)
        log->info("Network (Card 4/M1 D5 fine-slip innovation gate): {} carried-DD innovation "
                  "flag(s), {} fresh re-tap(s) over the run (k={}; the estimation-side reset for "
                  "the fine slips the coarse MW/LLI/IF-TD layer is relaxed against — each flagged "
                  "chord re-taps data-only + marks its arc slipped for the next epoch).",
                  carry_innov_flags_total, carry_innov_retaps_total,
                  config_.networkCarryInvariantInnovK());
    if (carry_innov_gate && config_.networkCarryInvariantInnovCalibrate())
        log->info("Network (Card 4/M1 D5 v2 LS-VCE calibration): final empirical inflation σ̂₀ = "
                  "{:.2f}× (the gate threshold is k·σ̂₀ = {:.1f} formal-σ; σ̂₀>1 quantifies how "
                  "optimistic the formal Cov(v) is — the M2 monitor's finding, now closing the loop "
                  "on the gate itself).", carry_driver.innovInflationSigma0(),
                  config_.networkCarryInvariantInnovK() * carry_driver.innovInflationSigma0());
    if (idw_on) {
        std::string idw_list;
        for (const auto& nm : iono_deweight.everDeweighted()) {
            if (!idw_list.empty()) idw_list += ' ';
            idw_list += nm;
        }
        if (idw.adaptive)
            log->info("Network (iono deweight): {} station(s) ever deweighted (peak {} "
                      "simultaneous), {} adaptive engage / {} release event(s) over the run "
                      "(static w_s={:.3f}; adaptive gate {:.2f}σ↑ / {:.2f}σ↓ → floor {:.3f}; "
                      "network-relative robust z-score, chronic >3σ background removed by the "
                      "median-centering).{}{}",
                      iono_deweight.everDeweighted().size(), iono_deweight.peakDeweighted(),
                      iono_deweight.triggerEvents(), iono_deweight.releaseEvents(),
                      iono_deweight.staticWeight(), idw.sigma_hi, idw.sigma_lo, idw.floor,
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
        else
            log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                      "w_s={:.3f} for the whole run — the SH-iono coupling was relaxed while "
                      "they kept serving the shared clocks/biases/datum/position.{}{}",
                      iono_deweight.staticCount(), iono_deweight.staticWeight(),
                      idw_list.empty() ? "" : "  Stations: ", idw_list);
    }
    if (carry_innov_gate && carry_innov_map_miss > 0)
        log->warn("Network (Card 4/M1 D5): {} innovation flag(s) over the run did not resolve in the "
                  "flag→ambiguity reverse map (next-epoch re-tap skipped for them) — a reverse-map/"
                  "block wiring inconsistency; investigate.", carry_innov_map_miss);
    if (emit_rebase_on) {
        // Card 4 (M1) phase D: the emit-layer re-base telemetry (spec §3) — the
        // integer cycles + common-fractional shift removed onto the previous emission
        // frame (an S-selection on OUTPUTS ONLY; user DDs are invariant to it).  Feeds
        // the consumer A/B: high integer/frac churn with a silent disc counter is the
        // datum-freedom the carry buys; a surviving disc step is a genuine event.
        const auto& rb = emit_rebase_engine.counters();
        log->info("Network (Card 4/M1 phase D emit re-base): {} emit epoch(s), {} component "
                  "re-base(s), {} integer cycle(s) removed across {} key(s), {} reference-path "
                  "slip step(s) preserved; mean |common-fractional δ| = {:.4f} m over the run "
                  "(OUTPUTS-only S-selection; DDs invariant).",
                  rb.epochs, rb.components, rb.integer_cycles, rb.keys_rebased,
                  rb.slips_preserved,
                  rb.frac_shift_n ? rb.frac_shift_abs_sum / static_cast<double>(rb.frac_shift_n)
                                  : 0.0);
    }
    if (emit_pb_filter_on) {
        // PB emission-continuity filter telemetry (M5 prerequisite).  A high
        // smoothed:reset ratio with a small mean posterior σ is the win — the
        // per-epoch noise absorbed into the filter, so the emitter jump test fires
        // (via the reset raw-value steps) only at genuine events.  ref-path + gap
        // resets are the structural (never-laundered) events; innovation resets are
        // the calibrated k·σ̂_step genuine steps.  ON-path counters (inv #4).
        const auto& pf = pb_filter_engine.counters();
        const long long resets = pf.resets_refpath + pf.resets_innovation + pf.resets_gap;
        log->info("Network (M5 prereq / PB emission-continuity filter): {} emit epoch(s), "
                  "{} smoothed / {} reset ({} ref-path slip, {} innovation, {} gap) key-epoch(s), "
                  "{} first-sighting init(s); mean posterior σ = {:.4f} m (self-tuning k·σ̂_step "
                  "gate, k={}; OUTPUTS-only — the solve, user DDs and dd-frac grading are "
                  "byte-unchanged; the emitter jump test stays the sole disc authority).",
                  pf.epochs, pf.smoothed, resets, pf.resets_refpath, pf.resets_innovation,
                  pf.resets_gap, pf.inits,
                  pf.smoothed ? pf.post_sigma_sum / static_cast<double>(pf.smoothed) : 0.0,
                  config_.networkEmitPbFilterJumpK());
        if (config_.networkEmitPbFilterGateV2()) {   // PB-gate-v2 merge point (run-end telemetry)
            std::string sig;
            for (const auto& kv : pb_filter_engine.stepSigmas())
                sig += fmt::format("{}{}={:.4f}", sig.empty() ? "" : " ",
                                   kv.first.empty() ? "pool" : kv.first, kv.second);
            log->info("Network (PB filter GATE v2): {} row(s) fed to σ̂_step (censoring-free: "
                      "smoothed + innovation-gated), converged σ̂_step [m] {}; model-scaled gate "
                      "k·√(Ppred+R), k={:.1f} — rate-transferable (one k for 30 s & 300 s).",
                      pf.fed_rows, sig.empty() ? std::string("(none)") : sig,
                      config_.networkEmitPbFilterJumpK());
        }
    }
    if (clk_runaway_on && n_runaway_epochs > 0)
        log->warn("Network (RT0-5): {} epoch(s) dropped at least one constellation for clock "
                  "runaway (magnitude ceiling {:.0f} m / rate ceiling {:.2f} m/s).",
                  n_runaway_epochs, clk_runaway_mag, clk_runaway_rate);
    if (n_healed_epochs > 0)
        log->info("Network: prioritized-datum drop-and-heal recovered the gate on {} epoch(s) "
                  "({} near-null phase-bias column(s) soft-anchored).",
                  n_healed_epochs, n_healed_cols);
    if (carry_cpl_joint && n_cpl_joint_fallback > 0)
        log->warn("Network (Card 1b): {} station-epoch(s) had a non-SPD joint coupling-row "
                  "covariance after the ridge retry and fell back to the Card-1 per-row scalar "
                  "marginalization for that station (the between-row DD correlation is not "
                  "recovered on those). Investigate if the count is a large fraction of "
                  "station-epochs.", n_cpl_joint_fallback);
    if (carry_cpl_cond)
        log->info("Network (Card 1/1d): coupling-carry volume — stashes: {} with a regression, "
                  "{} EMPTY-regression (diagonal degrade), {} harvest failures; injected terms: "
                  "{} present, {} departed, {} kappa-shrunk (time-update {}).",
                  n_cpl_stash, n_cpl_stash_empty, n_cpl_harvest_fail,
                  n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                  carry_cpl_tu ? "ON" : "OFF");
    if (carry_vce) {
        // M2 (LS-VCE): run-end honesty summary — the trailing-window measured
        // carry-variance FLOOR σ̂ vs the hand floor, per (family, constellation).
        // ratio > 1 ⇒ the hand floor is optimistic (real carry disagreement
        // exceeds it); ratio < 1 ⇒ the hand floor is conservative.  The per-hour
        // series is in network_carry_vce.csv.
        log->info("Network (M2/LS-VCE): carry-variance monitor summary "
                  "(components='{}', window={} ep) — ROBUST (geometric-mean, tail-safe) "
                  "carry-innovation variance vs the hand floor:",
                  config_.networkCarryVceComponents(), config_.networkCarryVceWindowEpochs());
        for (const auto& fc : carry_vce_est.keys()) {
            const auto e = carry_vce_est.estimate(fc.first, fc.second);
            const double cfg_floor = (fc.first == "pb") ? pb_var_floor
                                   : (fc.first == "cb") ? cb_var_floor
                                                        : pl_var_floor;
            const double rrat = (cfg_floor > 0.0) ? e.robust_floor / cfg_floor : 0.0;
            const double mrat = (cfg_floor > 0.0)
                ? e.sigma[NetworkCarryVce::kFloor] / cfg_floor : 0.0;
            log->info("  [{}|{}] robust σ̂ = {:.3e} m² (hand {:.3e}, ratio {:.1f}); "
                      "mean σ̂ = {:.3e} m² (ratio {:.0f}, event-incl.); {} rows.",
                      fc.first, fc.second.empty() ? std::string("*") : fc.second,
                      e.robust_floor, cfg_floor, rrat,
                      e.sigma[NetworkCarryVce::kFloor], mrat, e.n_rows);
        }
    }
    if (n_gated_epochs > 0)
        log->info("Network: anchored-union coverage gate (< {} trackers) excluded thin shared "
                  "phase-bias column(s) on {} epoch(s) ({} column-epoch(s) total; their phase "
                  "rows served the satellite clock through the local blocks).",
                  config_.networkPhaseDatumMinStations(), n_gated_epochs, n_gated_key_epochs);
    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);

    // L15: append the end-of-run manifest record (JSONL — a second object on its own line).
    // Reopen-append (never rewrite) so a crashed run keeps the startup record (itself diagnostic).
    if (nc.write_run_manifest) {
        std::ofstream mf(manifest_path, std::ios::app);
        if (mf)
            mf << "{\"record\":\"complete\","
               << "\"n_synced\":" << synced.size() << ','
               << "\"n_emitted\":" << n_emitted << ','
               << "\"n_skipped\":" << n_skipped << ','
               << "\"n_gate_withheld\":" << n_gate_withheld << ','
               << "\"n_sigma0_withheld\":" << n_sigma0_withheld << ','
               << "\"n_perconst_partial_epochs\":" << n_perconst_partial_epochs << ','
               << "\"n_perconst_diffuse_published\":" << n_perconst_diffuse_published << ','
               << fmt::format("\"loop_ms\":{:.1f}", loop_ms)
               << "}\n";
    }

    // --- B6 (P1-1): serialized-contract round-trip self-check --------------------
    // Close the writer (flush the header), read it back, and cross-check the parsed
    // S-basis contract against what we serialized.  This is a LIVE checkContract call
    // site (previously dead code) that guards the write→read→consume replay path a
    // real product consumer would take: a header serialization/parse regression fails
    // here loudly instead of silently mis-declaring the datum to a downstream user.
    if (contract_pending && !prod_path.empty()) {
        // Zero epochs emitted: the deferred writer never opened, and the round-trip
        // self-check below is skipped.  The stale previous file was removed at
        // staging — unless that removal failed (locked/read-only), in which case a
        // PREVIOUS run's products still sit at the path: check, don't assert.
        if (std::filesystem::exists(prod_path))
            log->warn("Network[B6]: no epochs were emitted AND a stale product file from a "
                      "PREVIOUS run remains at '{}' (removal failed at startup) — do NOT "
                      "consume it; it is not this run's output.", prod_path.string());
        else
            log->warn("Network[B6]: no epochs were emitted — the deferred contract writer never "
                      "opened; no product file exists at '{}' (round-trip self-check skipped).",
                      prod_path.string());
        // L14: record the distinguishable "writer never opened" outcome so an absent
        // contract-check CSV unambiguously means "the check never ran".
        if (nc.serialize_sbasis_contract) {
            std::ofstream cc(log_dir_ / "network_contract_check.csv");
            if (cc) cc << "check,status,detail\n"
                          "overall,FAIL,writer never opened (zero epochs emitted)\n";
        }
    }
    if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty()) {
        product_writer.reset();   // flush + close so the reader sees the complete header
        SbasisContract parsed;
        bool           rok = false;
        const std::vector<NetworkProductRecord> check_recs =
            NetworkProductReader::read(prod_path.string(), &parsed, &rok);
        // RTA-5: with re-broadcast armed, run checkContract on EVERY S receipt (each
        // record's contract_refresh), not once — the per-receipt behaviour a late-
        // joining consumer relies on.  The per-receipt expectation drops the pivot
        // map (pivots legitimately evolve across re-broadcasts; the FINAL pivot map
        // is checked separately below against the last-written contract) but keeps
        // every run-constant convention field AND the run's single generation id.
        // Gated on the toggle so an OFF run's check output is byte-identical.
        std::size_t s_receipts = 0, s_receipt_fails = 0;
        std::vector<std::string> s_receipt_mism;
        if (contract_rebroadcast_s > 0.0) {
            SbasisContract receipt_expect = pending_contract;
            receipt_expect.decl.clock_pivot_sat.clear();
            for (std::size_t ri = 0; ri < check_recs.size(); ++ri) {
                if (!check_recs[ri].contract_refresh.present) continue;
                ++s_receipts;
                std::vector<std::string> rm;
                if (!NetworkProductReader::checkContract(check_recs[ri].contract_refresh,
                                                         receipt_expect, &rm)) {
                    ++s_receipt_fails;
                    for (const auto& m : rm)
                        s_receipt_mism.push_back(fmt::format("receipt {} (epoch {}): {}",
                                                             s_receipts, ri, m));
                }
            }
            if (s_receipt_fails > 0) {
                log->warn("Network[RTA-5]: {} of {} re-broadcast S receipt(s) FAILED the "
                          "per-receipt contract check:", s_receipt_fails, s_receipts);
                for (const auto& m : s_receipt_mism) log->warn("    - {}", m);
            } else {
                log->info("Network[RTA-5]: {} S-contract receipt(s) parsed positionally; every "
                          "one passed the per-receipt contract check.", s_receipts);
            }
        }
        std::vector<std::string> mism;
        if (!parsed.present) {
            log->warn("Network[B6]: the serialized S-basis contract did not parse back from '{}' — "
                      "header round-trip FAILED.", prod_path.string());
        } else if (!NetworkProductReader::checkContract(parsed, pending_contract, &mism)) {
            log->warn("Network[B6]: serialized S-basis contract does not round-trip cleanly "
                      "({} mismatch(es)):", mism.size());
            for (const auto& m : mism) log->warn("    - {}", m);
        } else if (parsed.decl.clock_pivot_sat != pending_contract.decl.clock_pivot_sat) {
            log->warn("Network[B6]: the realized clock-pivot map did not round-trip through the header.");
        } else {
            log->info("Network[B6]: S-basis contract round-trips cleanly "
                      "(write → read → checkContract), {} realized pivot(s) serialized.",
                      parsed.decl.clock_pivot_sat.size());
        }
        // L14: machine-readable check result.  The pivot-map round-trip is checked SEPARATELY
        // (checkContract only compares the expected-side pivots, so a realized-pivot mismatch is
        // NOT in `mism`) and must fail 'overall' too.  Written on PASS as well — an ABSENT file
        // means the check never ran (crash / zero epochs), distinct from a recorded FAIL.
        const bool pivot_ok = parsed.present
            && (parsed.decl.clock_pivot_sat == pending_contract.decl.clock_pivot_sat);
        const bool overall = rok && parsed.present && mism.empty() && pivot_ok
            && (contract_rebroadcast_s <= 0.0 || s_receipt_fails == 0);
        const std::filesystem::path cc_path = log_dir_ / "network_contract_check.csv";
        std::ofstream cc(cc_path);
        if (cc) {
            cc << "check,status,detail\n";
            cc << "file_readable," << (rok ? "PASS" : "FAIL") << ",\n";
            cc << "contract_present," << (parsed.present ? "PASS" : "FAIL") << ",\n";
            for (const auto& m : mism)
                cc << "field_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            // RTA-5 (toggle-gated so an OFF run's CSV is byte-identical): per-receipt
            // contract-check summary across every positional S line in the file.
            if (contract_rebroadcast_s > 0.0) {
                cc << "s_receipts," << (s_receipt_fails == 0 ? "PASS" : "FAIL") << ','
                   << s_receipts << " receipt(s); " << s_receipt_fails << " failed\n";
                for (const auto& m : s_receipt_mism)
                    cc << "s_receipt_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            }
            cc << "pivot_map_roundtrip," << (pivot_ok ? "PASS" : "FAIL") << ",\n";
            cc << "overall," << (overall ? "PASS" : "FAIL") << ','
               << parsed.decl.clock_pivot_sat.size() << " realized pivot(s) serialized\n";
            cc.close();
        } else {
            log->warn("Network[L14]: failed to open contract-check CSV {}", cc_path.string());
        }
    }

    // --- B8: empirical false-alarm-rate summary per QC tier ------------------
    // Realized flag rate vs the implied one-sided Gaussian α for the configured k
    // (magnitude gates are two-sided, so α = P(|Z|>k) = erfc(k/√2)).  Run this on a
    // CLEAN baseline to read each tier's realized false-alarm rate.  Measurement only.
    if (trace_flag_rates) {
        auto gaussAlpha = [](double k) { return std::erfc(k / std::sqrt(2.0)); };  // P(|Z|>k)
        // L3: also stream a machine-readable network_flag_rates.csv alongside the log lines
        // (both sinks gated on the ONE existing trace_flag_rates toggle).  The two log format
        // strings below are byte-identical to before — the CSV row is appended after log->info.
        const std::filesystem::path fr_csv_path = log_dir_ / "network_flag_rates.csv";
        std::ofstream fr_csv(fr_csv_path);
        if (fr_csv)
            fr_csv << "tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha\n";
        else
            log->warn("Network: failed to open flag-rate CSV {}", fr_csv_path.string());
        auto emit = [&](const char* tier, const char* label, const FlagRate& fr, double k,
                        bool has_alpha) {
            const double rate = fr.opp ? static_cast<double>(fr.flag) /
                                         static_cast<double>(fr.opp) : 0.0;
            if (has_alpha)
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (k={:.1f} => implied "
                          "Gaussian a {:.3e})", label, fr.opp, fr.flag, rate, k, gaussAlpha(k));
            else
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (absolute bound; no "
                          "Gaussian a)", label, fr.opp, fr.flag, rate);
            if (fr_csv) {
                fr_csv << tier << ',' << label << ',' << fr.opp << ',' << fr.flag << ','
                       << fmt::format("{:.6e}", rate) << ',' << fmt::format("{:.1f}", k) << ',';
                if (has_alpha) fr_csv << fmt::format("{:.6e}", gaussAlpha(k));
                fr_csv << ',' << (has_alpha ? 1 : 0) << '\n';
            }
        };
        log->info("==== Network flag-rate empirical-alpha summary (network.trace_flag_rates) ====");
        emit("T1",  "IF-phase slip (T1)",      fr_if_slip,    config_.networkCycleSlipIfPhaseTdKMad(), true);
        emit("T2",  "within-ep trigger (T2)",  fr_we_trigger, config_.networkRaimSigma0Trigger(),      true);
        emit("T3",  "post-fit RAIM cell (T3)", fr_raim_cell,  config_.networkRaimKSigma(),             true);
        emit("T3c", "RAIM consensus (T3)",     fr_raim_sat,   config_.networkRaimKSigma(),             true);
        emit("T4",  "attribution SAT (T4)",    fr_attrib,     config_.networkRaimKSigma(),             true);
        emit("T5",  "clock-model NIS (T5)",    fr_clk_model,  config_.networkClockModelKSigma(),       true);
        emit("T6",  "clock-monitor (T6)",      fr_clk_mon,    0.0,                                     false);
        log->info("=============================================================================");
        if (fr_csv) fr_csv.close();

        // L3: run-summary CSV (same toggle).  ms_per_epoch uses n_loop_epochs (loop epochs,
        // NOT n_synced — the fast-forward / max-duration epochs are excluded from the loop).
        const std::filesystem::path rs_csv_path = log_dir_ / "network_run_summary.csv";
        std::ofstream rs_csv(rs_csv_path);
        if (rs_csv) {
            rs_csv << "n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,loop_ms,"
                      "ms_per_epoch,n_stations,n_threads,parallel\n";
            const double ms_per_epoch = n_loop_epochs
                ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0;
            rs_csv << synced.size() << ',' << n_emitted << ',' << n_skipped << ','
                   << n_gate_withheld << ',' << n_sigma0_withheld << ','
                   << fmt::format("{:.1f}", loop_ms) << ',' << fmt::format("{:.3f}", ms_per_epoch) << ','
                   << stations.size() << ',' << n_threads << ',' << (parallel_on ? 1 : 0) << '\n';
            rs_csv.close();
        } else {
            log->warn("Network: failed to open run-summary CSV {}", rs_csv_path.string());
        }
    }

    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());

    // --- Closed-loop integer-recovery summary -------------------------------
    if (closed_loop_on && consumer) {
        const double fix_rate = cl_ar_ran ? (100.0 * static_cast<double>(cl_ar_fixed)
                                             / static_cast<double>(cl_ar_ran)) : 0.0;
        const double mean_fixed = cl_ar_fixed ? (static_cast<double>(cl_sum_fixed)
                                                 / static_cast<double>(cl_ar_fixed)) : 0.0;
        // Converged-window robust stats: median (typical accuracy, spike-immune)
        // + max (surfaces wrong-fix excursions the RMS would smear).
        double conv_med = 0.0, conv_max = 0.0;
        if (!cl_recent_err3d.empty()) {
            std::vector<double> w(cl_recent_err3d.begin(), cl_recent_err3d.end());
            std::sort(w.begin(), w.end());
            conv_med = w[w.size() / 2];
            conv_max = w.back();
        }
        const Eigen::Vector3d final_err = consumer->getPosition() - cl_truth;
        const Eigen::Vector3d final_enu = Coordinates::ecefToEnu(final_err, cl_truth_llh);

        const char* product_label = !cl.consume_network_products
            ? "BASELINE (precise products only — network store NOT consumed)"
            : (cl.consume_network_clock ? "FULL (network clock + phase bias)"
                                        : "BIAS-ONLY (precise clock + network phase bias)");
        const std::string mode_label =
            std::string(cl.consumer_undifferenced ? "UNDIFFERENCED" : "BSSD")
            + " user / " + product_label;

        // Honest, fix-rate-led verdict.  WL-only AR does not fix the narrow lane,
        // so the post-fix position stays at the WL-float level (~m) even on a
        // healthy product — the WL fix RATE, not the position, is the recovery
        // metric.  Flag a marginal result and point at the A/B baseline knob so a
        // reader never reads a degraded run as a clean pass.
        std::string verdict;
        if (cl_ar_fixed == 0)
            verdict = "NO integers recovered — inspect closed_loop_trace.csv / SSR coverage";
        else if (fix_rate >= 50.0)
            verdict = fmt::format("RECOVERED INTEGERS — {:.0f}% WL fix rate from our own "
                                  "network products", fix_rate);
        else
            verdict = fmt::format("PARTIAL — {:.0f}% WL fix rate (marginal); compare with the "
                                  "precise-products baseline (network.closed_loop."
                                  "consume_network_products=false)", fix_rate);

        log->info("==== Closed-loop integer-recovery harness — held-out '{}' ====",
                  cl.station.name);
        log->info("  mode                      : {}", mode_label);
        log->info("  consumer epochs processed : {}", cl_epochs);
        log->info("  AR invoked / accepted     : {} / {}  ({:.1f}% fix rate)",
                  cl_ar_ran, cl_ar_fixed, fix_rate);
        log->info("  mean WL integers per fix  : {:.1f}", mean_fixed);
        if (cl_first_fix_tow >= 0.0)
            log->info("  first integer fix at      : week {} tow {:.1f}",
                      cl_first_fix_week, cl_first_fix_tow);
        else
            log->info("  first integer fix         : NONE — no epoch recovered integers");
        log->info("  position error (final)    : {:.3f} m 3D  (E {:.3f}, N {:.3f}, U {:.3f})",
                  final_err.norm(), final_enu.x(), final_enu.y(), final_enu.z());
        log->info("  position (last {} ep)      : median {:.3f} m, max {:.3f} m 3D "
                  "(max >> median ⇒ wrong-fix excursions)",
                  cl_recent_err3d.size(), conv_med, conv_max);
        log->info("  VERDICT                   : {}", verdict);
        log->info("  closed-loop trace         : {}",
                  (log_dir_ / "closed_loop_trace.csv").string());
        log->info("==============================================================");
    }
    if (n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the datum is under-specified "
                   "(check active_systems = GPS+Galileo and ≥2 overlapping stations).");
        return 1;
    }
    return 0;
}

} // namespace GnssProcessor
