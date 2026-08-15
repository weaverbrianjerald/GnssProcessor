#pragma once
/**
 * @file NetworkMwWlLayer.hpp
 * @brief M5 card "WL-1" — the MW-seeded wide-lane pseudo-observation layer.
 *        See docs/SPEC_mw_wl_card.md.
 *
 * The network producer is a per-epoch estimator whose only cross-epoch information
 * channel is the Card-4 invariant DD carry (phase-side).  Long-horizon CODE
 * coherence — which is what fixes the wide-lane integer N_WL — has no container, so
 * the KF-implied WL is uniform (circR ~ 0.001-0.11) in every configuration even
 * though the estimator-free MW census of the same raw data shows the WL integers
 * plainly present (circR ~ 0.62).  See memory `project_wl_structural_gap_jul15`
 * and `docs/CONFIG_promotion_ledger.md §A1`.
 *
 * This layer supplies the missing accumulator.  The per-station MwSlipDetector
 * already maintains a per-arc Welford mean of the Melbourne-Wubbena observable.
 * For each carried fundamental cycle (a station-satellite DD, owned by the Card-4
 * carry driver's chord basis), the sign-weighted sum of the per-arc MW means is a
 * gauge-free estimate of the double-differenced wide-lane integer combination
 * @f$\nabla\Delta N_{WL}=\nabla\Delta N_1-\nabla\Delta N_2@f$ — both the receiver and
 * satellite hardware-bias lumps cancel identically because each receiver and each
 * satellite appears with a balanced +/- sign around a closed cycle.  We fold that
 * as a rank-K pseudo-observation block on the SAME cross-band ambiguity columns the
 * cycle spans (@f$+c/\lambda_1@f$ on the L1 column, @f$-c/\lambda_2@f$ on the L2
 * column), giving the joint solve the WL direction it structurally lacks.
 *
 * ## Scope
 *
 * Pure numeric leaf: it consumes the carry driver's already-built DD-prior blocks
 * (each row a fundamental cycle expressed as signed Terms on the L1-band ambiguity
 * columns) plus a caller-supplied @ref ArcResolver that maps each Term to its
 * cross-band + MW data, and returns WL @ref NetworkNormalEquations::DdPriorBlock
 * instances.  It owns NO estimator state, no StateManager, no detector — those live
 * in the ApplicationNetwork wiring.  GLONASS is excluded upstream (the carry driver
 * never emits FDMA arcs), so no FDMA handling is needed here.
 *
 * ## Frame note (SPEC §2 correction)
 *
 * SPEC §2 assumed the per-arc MW means are raw undifferenced (ZD).  On the network
 * path the detector actually runs BIA-corrected and single-differenced against a
 * PER-STATION MW pivot.  A closed fundamental cycle balances each station's signs,
 * so a per-station pivot term cancels within the cycle regardless of which pivot a
 * station picked — EXCEPT that a cycle arc that IS a station's own pivot is stored
 * ZD while its siblings are SD.  The @ref MwSlipDetector::arcZdMean accessor
 * reconstructs a ZD-consistent per-(station,sat) mean (SD + pivot re-added) so every
 * arc enters this layer in one consistent frame; this leaf then only sees the
 * reconstructed mean/variance and is frame-agnostic.
 *
 * ## Composition caveat (SPEC §3.2)
 *
 * The WL blocks are appended to the SAME `foldReducedDdPriors` call as the Card-4
 * carry blocks, and they share the L1 ambiguity columns.  `foldReducedDdPriors`
 * folds each block against the PRISTINE per-station N_rr, so two blocks that share
 * local columns compose INEXACTLY (the cross term is dropped — the documented
 * "two post-reduce Woodbury folds don't compose" property).  Within a single WL
 * block the rank-K fold is joint (exact), so grouping the WL rows of one carry
 * component into ONE block keeps the WL<->WL coupling exact; only the WL<->carry
 * coupling is approximate.  This is a Phase-A float pre-conditioner (the emitted
 * product stays datum-invariant — the WL rows are gauge-free, so the W5 null space
 * is untouched); the empirical size of the approximation is a Phase-A grading
 * question (SPEC §7.6).  See docs/SPEC_mw_wl_card.md §13.2 (this exact fold-composition question) and §15.
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
 *        driver's chord basis + the per-arc MW Welford means.
 */
class NetworkMwWlLayer {
public:
    /// Tuning (`network.mw_wl.*`; SPEC §4).  Default `enable=false` ⇒ @ref build
    /// returns an empty vector, so the producer is byte-identical.
    struct Config {
        bool   enable{false};            ///< Phase-A master.  OFF ⇒ no blocks.
        int    min_arc_epochs{10};       ///< Welford n per arc before its cycles are eligible.
        double sigma_floor_cyc{0.05};    ///< per-row R floor (cycles): R = max(Σ var/λ_WL², floor²).
        int    max_rows_per_epoch{0};    ///< 0 = unlimited; else keep the lowest-R rows first (count drops).
        double debug_r_scale{1.0};       ///< DIAGNOSTIC ONLY (`network.mw_wl.debug_r_scale`): multiply every row's Pz (R + shared-arc cov) by this.  1.0 = honest (default).  <<1 makes the WL rows dominate the stiff carry — the F6 probe for "is separate-Woodbury composition fatal, or is authority enough?".  NOT a production knob.
        // --- WL-2 (SPEC_ewl_wl_cascade_card.md): FFR statistic-gated fixing + station-1 quarantine.
        // These are per-LANE knobs; the same @ref build serves the WL-1 (1,2) lane (ffr_fix=false ⇒
        // pure float, byte-identical) and the WL-2 EWL (2,5) lane (ffr_fix=true).
        bool   ffr_fix{false};           ///< WL-2: when a row's statistic passes FFR, fold it as an INTEGER-hard row (zhat=round(z_abs) in the correction frame, Pz=fix_weight²) instead of a float pseudo-obs.  OFF ⇒ WL-1 float behaviour.
        double fix_sigma_cyc{0.10};      ///< FFR gate: fix when the row's formal σ (√R, pre-floor) < this (cyc).
        double fix_frac_cyc{0.20};       ///< FFR gate: and |frac(z_abs)| < this (cyc).
        double fix_weight_sigma_cyc{0.01};///< Fixed-row σ (soft-hard, rollback-able): a fixed row's Pz = this² (diagonal, no shared-arc cov).
        double max_abs_xlin_m{0.0};      ///< Station-1 quarantine (SPEC §7 pre-flight): skip a cycle if ANY arc's |x_lin| exceeds this (m).  0 = disabled.  Guards the ~256 km receiver-clock-seed pathology that ill-conditions the correction-frame rows.
        // --- UWL-1 (docs/SPEC_uwl_card.md): per-constellation lane instances. ---
        char   cons_filter{'\0'};        ///< UWL-1 per-cons telemetry: when non-zero, only cycles whose satellites belong to this constellation are processed (others are silently left to their own instance).  0 (default) = the legacy single all-constellation lane.
    };

    /// The cross-band + MW data a single fundamental-cycle arc contributes, resolved
    /// from a carry Term (est_station, band-1 local col).  Filled by @ref ArcResolver.
    struct ArcData {
        Eigen::Index col1{-1};         ///< band-1 (sig1) ambiguity local column (== the carry Term's local_col).
        Eigen::Index col2{-1};         ///< band-2 (sig2) ambiguity local column at the SAME (station,sat); -1 ⇒ absent.
        double lambda1{0.0};           ///< band-1 wavelength (m).
        double lambda2{0.0};           ///< band-2 wavelength (m).
        double lambda_wl{0.0};         ///< wide-lane wavelength c/(f1-f2) (m).
        double mw_zd_mean_m{0.0};      ///< ZD-consistent MW arc mean (m) — an ABSOLUTE measurement.
        double mw_var_m2{0.0};         ///< variance of that mean (m²).
        double x_lin1{0.0};            ///< band-1 ambiguity LINEARIZATION value (m).  The kernel solves
                                       ///< CORRECTIONS about a fixed x_lin (no KF write-back), so the
                                       ///< absolute MW measurement must be re-expressed in the correction
                                       ///< frame: z_corr = z_MW − Σ[(c/λ1)·x_lin1 − (c/λ2)·x_lin2].
                                       ///< Omitting this displaces the solve by the absolute WL DD
                                       ///< (~10²  cyc) — the Jul-15 verification finding F5.
        double x_lin2{0.0};            ///< band-2 ambiguity linearization value (m).
        long long mw_count{0};         ///< Welford sample count of the arc.
        bool   is_primary_sig1{false}; ///< the carry Term's signal == this arc's estimator PRIMARY sig1 (v1 restricts WL rows to the primary L1/L2 pair).
        bool   have_mw{false};         ///< a ZD-consistent MW arc exists on the (sig1,sig2) primary pair.
        bool   bia_corrected{false};   ///< satellite-bias frame of the arc (from @ref MwSlipDetector::MwArcZdMean).  Satellite-bias cancellation needs ONE frame per cycle — mixed-frame cycles are skipped (@c skip_mixed_frame).
        char   cons{'?'};              ///< UWL-1: the arc's constellation letter (for @ref Config::cons_filter routing).
        bool   mw_cold{false};         ///< UWL-1 §3.3: the served statistic is the UNLAGGED cold read (n < the lagged maturity; a-priori or infant Welford variance).  A cycle with ANY cold arc bypasses the young gate but is DECISION-ONLY: its FFR integer is exposed via @ref FixedEwlRow, yet the row is NEVER emitted into the returned blocks (fold discipline).
    };

    /// Resolve one carry Term (est_station, local_col) → its @ref ArcData.
    /// Returns false only when the term cannot be reverse-mapped at all (unexpected);
    /// a resolvable-but-ineligible arc returns true with the eligibility flags cleared.
    using ArcResolver =
        std::function<bool(std::size_t est_station, Eigen::Index local_col, ArcData& out)>;

    /// Eligibility / skip tallies (invariant #4: counters or it didn't happen).
    /// Accumulated across the epoch's carry blocks; the caller sums them into run totals.
    struct Counters {
        long long cycles_seen{0};       ///< primary-sig1 fundamental cycles inspected.
        long long rows_built{0};        ///< WL rows emitted (after the max-rows cap).
        long long rows_capped{0};       ///< eligible rows dropped by @c max_rows_per_epoch.
        long long skip_not_primary{0};  ///< cycle skipped: a term is not on its arc's primary sig1 (an L1W / L2 / L5 carry group).
        long long skip_missing_band{0}; ///< cycle skipped: an arc lacks a band-2 ambiguity column (or failed to reverse-map).
        long long skip_missing_mw{0};   ///< cycle skipped: an arc has no reconstructable ZD MW mean (channel mismatch / pivot missing).
        long long skip_young_arc{0};    ///< cycle skipped: an arc's MW count < @c min_arc_epochs.
        long long skip_bad_lambda{0};   ///< cycle skipped: a non-finite / non-positive wavelength.
        long long skip_mixed_frame{0};  ///< cycle skipped: arcs mix BIA-corrected and raw satellite-bias frames (cancellation would break; dormant on the self-contained path, guarded for the BIA era).
        long long skip_quarantine{0};   ///< cycle skipped: an arc's |x_lin| exceeds @c max_abs_xlin_m (the station-1 256 km pathology).
        long long rows_fixed{0};        ///< WL-2: rows folded INTEGER-hard (passed FFR); the rest are floats.
        long long rows_fixed_cold{0};   ///< UWL-1 §3.3: COLD cycles whose FFR passed — integer exposed via @ref FixedEwlRow, row NOT emitted (decision-only).
        long long rows_cold_unfixed{0}; ///< UWL-1 §3.3: COLD cycles whose FFR failed — dropped entirely (a cold float row would fold same-epoch data).
        double    sum_R{0.0};           ///< Σ of the emitted rows' R (cyc²) — for the mean-R report.
    };

    /// WL-2 (SPEC_ewl_wl_cascade_card.md §3.4) — one FFR-fixed EWL row exposed for the WL
    /// phase bridge.  The bridge (in @c ApplicationNetwork) maps @c (block,row) → the source
    /// carry chord's @c ArcKey via @c NetworkInvariantCarryDriver::blockChordsForTest and
    /// assembles @f$z_{WL}^{\text{bridge}}=[\sum c\,GF_b+\lambda_{EWL}\cdot N_{EWL}]/\lambda_{WL}@f$.
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
     *        @ref NetworkInvariantCarryDriver::buildDdPriors — each row is a
     *        fundamental cycle expressed as signed Terms on ONE band's ambiguity
     *        columns.  Only primary-sig1 cycles (every arc on its estimator primary
     *        L1 signal) produce WL rows; L2 / L5 / secondary-L1 group blocks are
     *        skipped (counted @c skip_not_primary), so a cycle is never double-folded.
     * @param resolve Maps each Term to its @ref ArcData.
     * @param[in,out] counters Eligibility tallies (accumulated, not reset here).
     * @param[out] fixed_rows  WL-2 (§3.4): optional; when non-null, receives one
     *        @ref FixedEwlRow per FFR-fixed row (source carry block + row index + the
     *        integer @f$N_{EWL}@f$) so the WL phase bridge can key the EWL integers back to
     *        their carry chords.  Only emitted for the rows that pass FFR (@c ffr_fix);
     *        cleared at entry.  Ignored for the WL-1 float lane (@c ffr_fix=false ⇒ none).
     * @return One WL block per carry block that yielded ≥1 eligible row, with
     *         @c zhat = measured WL (cycles) and the EXACT @c Pz: diagonal = per-row
     *         R (floored), off-diagonals = the shared-arc covariance
     *         @f$P^z_{ij}=\sum_{a\in i\cap j} c_i(a)c_j(a)\,\mathrm{var}_a/\lambda_{WL,a}^2@f$
     *         (cycles of one component share tree-path arcs, so their measured sums
     *         share those arcs' MW noise — a diagonal Pz would double-count it).
     *         Rows of the SAME carry component group into one block so both this
     *         covariance and the shared-column coupling fold jointly (exact).
     *         Empty when @c Config::enable is false.
     */
    std::vector<NetworkNormalEquations::DdPriorBlock>
    build(const std::vector<NetworkNormalEquations::DdPriorBlock>& carry_blocks,
          const ArcResolver& resolve, Counters& counters,
          std::vector<FixedEwlRow>* fixed_rows = nullptr) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace GnssProcessor
