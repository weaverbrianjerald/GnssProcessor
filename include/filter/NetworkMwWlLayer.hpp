#pragma once
/**
 * @file NetworkMwWlLayer.hpp
 * @brief MW-seeded wide-lane pseudo-observation layer (M5 card WL-1).
 *        See docs/SPEC_mw_wl_card.md.
 *
 * The WL-1 float lane on the primary (1,2) pair (network.mw_wl.enable) is retired; the
 * class now serves only the WL-2 EWL lane instances (@ref Config::ffr_fix true), whose
 * blocks are decided and exposed but never folded.  The design notes below describe the
 * original WL-1 rows.
 *
 * The network producer is a per-epoch estimator whose cross-epoch ambiguity
 * information travels in the invariant DD carry (card 4), which is phase-side.
 * Long-horizon code coherence, which is what determines the wide-lane integer N_WL,
 * has no container there, so the WL implied by the filter shows no integer structure
 * while an estimator-free MW census of the same raw data does.  See
 * `docs/CONFIG_promotion_ledger.md §A1`.
 *
 * This layer supplies that accumulator.  The per-station MwSlipDetector maintains a
 * per-arc Welford mean of the Melbourne-Wubbena observable.  For each carried
 * fundamental cycle (a station-satellite DD, owned by the carry driver's chord
 * basis), the signed sum of the per-arc MW means is a gauge-free estimate of the
 * double-differenced wide-lane integer combination
 * @f$\nabla\Delta N_{WL}=\nabla\Delta N_1-\nabla\Delta N_2@f$: the receiver and
 * satellite hardware-bias lumps cancel exactly, because each receiver and each
 * satellite appears with balanced +/- signs around a closed cycle.  The layer folds
 * this as a rank-K pseudo-observation block on the cross-band ambiguity columns the
 * cycle spans (@f$+c/\lambda_1@f$ on the L1 column, @f$-c/\lambda_2@f$ on the L2
 * column, with c the arc's signed cycle coefficient), which gives the joint solve the
 * WL direction it otherwise lacks.
 *
 * ## Scope
 *
 * Pure numeric leaf: it consumes the carry driver's DD-prior blocks (each row a
 * fundamental cycle expressed as signed Terms on the L1-band ambiguity columns) plus
 * a caller-supplied @ref ArcResolver that maps each Term to its cross-band and MW
 * data, and returns WL @ref NetworkNormalEquations::DdPriorBlock instances.  It owns
 * no estimator state, no StateManager and no detector; those belong to the network
 * producer's wiring (the resolver is built in NetworkMwWlGlue.cpp).  GLONASS is
 * excluded upstream (the carry driver never emits FDMA arcs), so no FDMA handling is
 * needed here.
 *
 * ## Frame note (SPEC §2 correction)
 *
 * SPEC §2 assumes the per-arc MW means are raw undifferenced (ZD).  On the network
 * path the detector stores BIA-corrected arcs single-differenced against a
 * per-station MW pivot, while the pivot's own arc (and any arc without BIA) is stored
 * ZD.  A closed fundamental
 * cycle balances each station's signs, so a per-station pivot term cancels within the
 * cycle whichever pivot the station picked, provided every arc is in the same frame.
 * @ref MwSlipDetector::arcZdMean reconstructs a ZD-consistent per-(station,sat) mean
 * (SD mean plus the pivot mean) so every arc enters this layer in one frame; this
 * leaf sees only the reconstructed mean and variance and is frame-agnostic.
 *
 * ## Composition caveat (SPEC §3.2)
 *
 * The WL blocks are appended to the same `foldReducedDdPriors` call as the carry
 * blocks, and they share the L1 ambiguity columns.  `foldReducedDdPriors` folds each
 * block against the unmodified per-station N_rr, so two blocks that share local
 * columns compose inexactly: the cross term is dropped (two post-reduce Woodbury
 * folds do not compose).  Within a single WL block the rank-K fold is joint and
 * exact, so grouping the WL rows of one carry component into one block keeps the
 * WL-WL coupling exact; only the WL-carry coupling is approximate.  The layer is a
 * Phase-A float pre-conditioner.  The emitted product stays datum-invariant, because
 * the WL rows are gauge-free and leave the W5 null space untouched; the size of the
 * approximation is a Phase-A grading question (SPEC §7.6).  See
 * docs/SPEC_mw_wl_card.md §13.2 (this fold-composition question) and §15.
 */

#include "filter/NetworkNormalEquations.hpp"   // DdPriorBlock

#include <Eigen/Dense>

#include <cstddef>
#include <functional>
#include <vector>

namespace GnssProcessor {

/**
 * @class NetworkMwWlLayer
 * @brief Builds the epoch's wide-lane pseudo-observation blocks from the carry
 *        driver's chord basis and the per-arc MW Welford means.
 */
class NetworkMwWlLayer {
public:
    /// Tuning (`network.mw_wl.*`; SPEC §4).  With the default `enable=false`, @ref build
    /// returns an empty vector and the producer output is byte-identical.
    struct Config {
        bool   enable{false};            ///< Phase-A master switch.  Off: no blocks.
        int    min_arc_epochs{10};       ///< Welford n per arc before its cycles are eligible.
        double sigma_floor_cyc{0.05};    ///< per-row R floor (cycles): R = max(Σ c²·var/λ_WL², floor²).
        int    max_rows_per_epoch{0};    ///< 0 = unlimited; otherwise keep the lowest-R rows (drops are counted in @c rows_capped).
        // FFR statistic-gated fixing and the station-1 quarantine (WL-2,
        // SPEC_ewl_wl_cascade_card.md).  These knobs are per lane; the WL-2 EWL (2,5) lane
        // instances set ffr_fix=true (the retired WL-1 (1,2) float lane was the false case).
        bool   ffr_fix{false};           ///< When a row's statistic passes FFR, fold it as an integer-hard row (zhat = round(z_abs) in the correction frame, Pz = fix_weight_sigma_cyc²) instead of a float pseudo-obs (WL-2).  Off: float rows.
        double fix_sigma_cyc{0.10};      ///< FFR gate: fix when the row's formal σ (√R before the floor) < this (cyc).
        double fix_frac_cyc{0.20};       ///< FFR gate: and |frac(z_abs)| < this (cyc).
        double fix_weight_sigma_cyc{0.01};///< Fixed-row σ (soft-hard, so a fix can be rolled back): a fixed row's Pz = this² (diagonal, no shared-arc covariance).
        double max_abs_xlin_m{0.0};      ///< Station-1 quarantine (SPEC §7 pre-flight): skip a cycle if any arc's |x_lin| exceeds this (m).  0 = disabled.  Guards against the ~256 km receiver-clock-seed linearization, which ill-conditions the correction-frame rows.
        // Per-constellation lane instances (UWL-1, docs/SPEC_uwl_card.md).
        char   cons_filter{'\0'};        ///< Per-constellation instance: when non-zero, only cycles of this constellation (taken from the first arc) are processed; other cycles are left, uncounted, to their own instance (UWL-1).  0 (default) = the single all-constellation lane.
    };

    /// The cross-band and MW data a single fundamental-cycle arc contributes, resolved
    /// from a carry Term (est_station, band-1 local col).  Filled by @ref ArcResolver.
    struct ArcData {
        Eigen::Index col1{-1};         ///< band-1 (sig1) ambiguity local column (== the carry Term's local_col).
        Eigen::Index col2{-1};         ///< band-2 (sig2) ambiguity local column at the same (station,sat); -1 = absent.
        double lambda1{0.0};           ///< band-1 wavelength (m).
        double lambda2{0.0};           ///< band-2 wavelength (m).
        double lambda_wl{0.0};         ///< wide-lane wavelength c/(f1-f2) (m).
        double mw_zd_mean_m{0.0};      ///< ZD-consistent MW arc mean (m); an absolute measurement.
        double mw_var_m2{0.0};         ///< variance of that mean (m²).
        double x_lin1{0.0};            ///< band-1 ambiguity linearization value (m).  The kernel solves
                                       ///< corrections about a fixed x_lin (no KF write-back), so the
                                       ///< absolute MW measurement is re-expressed in the correction
                                       ///< frame: z_corr = z_MW − Σ[(c/λ1)·x_lin1 − (c/λ2)·x_lin2].
                                       ///< Without this term the solve is displaced by the absolute
                                       ///< WL DD, of order 10² cycles (finding F5).
        double x_lin2{0.0};            ///< band-2 ambiguity linearization value (m).
        long long mw_count{0};         ///< Welford sample count of the arc.
        bool   is_primary_sig1{false}; ///< the carry Term's signal is this arc's estimator primary sig1 (the first version restricts WL rows to the primary L1/L2 pair).
        bool   have_mw{false};         ///< a ZD-consistent MW arc exists on the (sig1,sig2) primary pair.
        bool   bia_corrected{false};   ///< satellite-bias frame of the arc (from @ref MwSlipDetector::MwArcZdMean).  Satellite-bias cancellation needs one frame per cycle; mixed-frame cycles are skipped (@c skip_mixed_frame).
        char   cons{'?'};              ///< The arc's constellation letter, for @ref Config::cons_filter routing (UWL-1).
        bool   mw_cold{false};         ///< The served statistic is the unlagged cold read (n below the lagged maturity; a-priori or infant Welford variance).  Under UWL-1 §3.3 a cold arc bypasses the young-arc gate, and a cycle with any cold arc is decision-only: its FFR integer is exposed via @ref FixedEwlRow, but the row is never emitted into the returned blocks, because it carries the current epoch's data.
    };

    /// Resolve one carry Term (est_station, local_col) → its @ref ArcData.
    /// Returns false only when the term cannot be reverse-mapped at all (unexpected);
    /// a resolvable but ineligible arc returns true with the eligibility flags cleared.
    using ArcResolver =
        std::function<bool(std::size_t est_station, Eigen::Index local_col, ArcData& out)>;

    /// Eligibility and skip tallies (invariant #4: run-end counters show whether the
    /// path is live).  Accumulated across the epoch's carry blocks; the caller sums them
    /// into run totals.
    struct Counters {
        long long cycles_seen{0};       ///< primary-sig1 fundamental cycles inspected.
        long long rows_built{0};        ///< WL rows emitted (after the max-rows cap).
        long long rows_capped{0};       ///< eligible rows dropped by @c max_rows_per_epoch.
        long long skip_not_primary{0};  ///< cycle skipped: a term is not on its arc's primary sig1 (an L1W / L2 / L5 carry group).
        long long skip_missing_band{0}; ///< cycle skipped: an arc lacks a band-2 ambiguity column (or failed to reverse-map; that case is counted before the @c cons_filter test, so by every instance).
        long long skip_missing_mw{0};   ///< cycle skipped: an arc has no reconstructable ZD MW mean (channel mismatch / pivot missing).
        long long skip_young_arc{0};    ///< cycle skipped: a warm arc's MW count < @c min_arc_epochs.
        long long skip_bad_lambda{0};   ///< cycle skipped: a non-finite / non-positive wavelength.
        long long skip_mixed_frame{0};  ///< cycle skipped: arcs mix BIA-corrected and raw satellite-bias frames, so the satellite bias would not cancel.  Never fires on the self-contained path (every arc raw); guards runs with BIA.
        long long skip_quarantine{0};   ///< cycle skipped: an arc's |x_lin| exceeds @c max_abs_xlin_m (the station-1 ~256 km linearization).
        long long rows_fixed{0};        ///< Rows folded integer-hard (passed FFR); the rest are floats (WL-2).
        long long rows_fixed_cold{0};   ///< Cold cycles whose FFR passed; the integer is exposed via @ref FixedEwlRow and the row is not emitted (decision-only; UWL-1 §3.3).
        long long rows_cold_unfixed{0}; ///< Cold cycles whose FFR failed; dropped entirely, since a cold float row would fold same-epoch data (UWL-1 §3.3).
        double    sum_R{0.0};           ///< Σ of the emitted rows' R (cyc²), for the mean-R report.
    };

    /// One FFR-fixed EWL row exposed for the WL phase bridge (WL-2,
    /// SPEC_ewl_wl_cascade_card.md §3.4).  The bridge (in NetworkMwWlGlue.cpp) maps
    /// @c (block,row) → the source carry chord's @c ArcKey via
    /// @c NetworkInvariantCarryDriver::blockChordsForTest and assembles
    /// @f$z_{WL}^{\text{bridge}}=[\sum c\,GF_b+\lambda_{EWL}\cdot N_{EWL}]/\lambda_{WL}@f$.
    struct FixedEwlRow {
        std::size_t block{0};    ///< source carry-block index (into the @p carry_blocks passed to @ref build).
        std::size_t row{0};      ///< source row (== fundamental-cycle / chord) index within that carry block.
        long long   integer{0};  ///< the FFR-fixed EWL DD integer @f$\nabla\Delta N_{EWL}@f$ = round(z_EWL^abs).
    };

    explicit NetworkMwWlLayer(Config cfg) : cfg_(cfg) {}

    /**
     * @brief Build the WL pseudo-observation blocks for this epoch.
     *
     * @param carry_blocks The blocks returned by
     *        @ref NetworkInvariantCarryDriver::buildDdPriors; each row is a
     *        fundamental cycle expressed as signed Terms on one band's ambiguity
     *        columns.  Only primary-sig1 cycles (every arc on its estimator primary
     *        L1 signal) produce WL rows; L2 / L5 / secondary-L1 group blocks are
     *        skipped (counted @c skip_not_primary), so a cycle is never folded twice.
     * @param resolve Maps each Term to its @ref ArcData.
     * @param[in,out] counters Eligibility tallies (accumulated, not reset here).
     * @param[out] fixed_rows  WL-2 (§3.4): optional; when non-null, cleared at entry and
     *        then receives one @ref FixedEwlRow per FFR-fixed row (source carry block,
     *        row index and the integer @f$N_{EWL}@f$) so the WL phase bridge can key
     *        the EWL integers back to their carry chords.  Warm fixed rows come first in
     *        emission order, then the cold decisions.  Only rows that pass FFR
     *        (@c ffr_fix) are reported, so a float lane (@c ffr_fix=false) reports none.
     * @return One WL block per carry block that yielded ≥1 eligible row.  @c zhat is
     *         the measured lane DD in the correction frame (cycles, z_abs − z_lin).
     *         For float rows, @c Pz has diagonal = per-row R (floored) and
     *         off-diagonals = the shared-arc covariance
     *         @f$P^z_{ij}=\sum_{a\in i\cap j} c_i(a)c_j(a)\,\mathrm{var}_a/\lambda_{WL,a}^2@f$
     *         (cycles of one component share tree-path arcs, so their measured sums
     *         share those arcs' MW noise, which a diagonal Pz would treat as
     *         independent in each row).  A fixed row has zhat = round(z_abs) − z_lin,
     *         diagonal fix_weight_sigma_cyc² and no off-diagonal terms.  Rows of one
     *         carry component group into one block so this covariance and the
     *         shared-column coupling fold jointly (exact).  Empty when
     *         @c Config::enable is false.
     */
    std::vector<NetworkNormalEquations::DdPriorBlock>
    build(const std::vector<NetworkNormalEquations::DdPriorBlock>& carry_blocks,
          const ArcResolver& resolve, Counters& counters,
          std::vector<FixedEwlRow>* fixed_rows = nullptr) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the MW/WL lanes: EWL, the WL inject, UWL.
 *
 * The MW/WL lanes: the EWL lanes, the WL inject and the UWL walk.
 *
 * Gate: each section under its own condition, as in the teardown: `ewl_on`;
 * `wl_inject_on`; `uwl_on && wl_inject_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkLanesRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
