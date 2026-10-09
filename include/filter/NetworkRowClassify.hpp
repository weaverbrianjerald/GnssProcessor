#pragma once
/**
 * @file NetworkRowClassify.hpp
 * @brief The shared row/column classifier for every assembled network row
 *        (INC-0 of docs/SPEC_prefit_census_observability.md).
 *
 * `classifyRow` and the shared-column family/label pass live here so that the
 * attribution (`src/core/filter/NetworkRhsAttrib.cpp`), the prefit census
 * (INC-1), the structural fuse (INC-2), the seed-birth tripwire (INC-3) and the
 * topology ledger (INC-4) all use one verdict about what a row is.  Separate
 * copies could let the census and the attribution disagree about whether a
 * given row is code, phase or a pin.
 *
 * ## What a class means
 *
 *  - @c obs_code / @c obs_phase / @c obs_other: data rows, each an actual
 *    observation equation.  A row is phase when it has a satellite phase-bias
 *    partial (`PB:` column), which is a property of the measurement model and
 *    does not depend on the QC configuration.  For runs that estimate no
 *    satellite phase bias, the secondary signal is a touch (|A_r| > 0.5) on an
 *    ambiguity column listed in @c R.pl_amb (the frame-diag convention,
 *    NetworkFrameDiag.cpp); that list is filled only while the phase-leveling
 *    carry is the live vehicle.  A row with a `CB:` partial and neither phase
 *    signal is code.  @c obs_other is a data row touching a shared column of
 *    neither family (for example a clock-only pseudo-observation); it is
 *    counted, not misfiled.
 *  - @c iono_sh_row: the SH-VTEC ↔ slant-iono tie pseudo-observation (any row
 *    touching an `SH` column).  A data class, but it carries no receiver
 *    clock, so the census does not common-mode-centre it.
 *  - @c pivot_pin / @c local_pin_row: rows with no shared partial, either a
 *    hard gauge pin (w ≥ 1e9) or a soft local prior/anchor row.
 *  - @c cpl_row: the conditional coupling rows (card 1), from
 *    @c R.cpl_row_start on; they are statements about the carry, not data.
 *
 * Only the four data classes feed the census and the fuse
 * (@ref isDataRowClass).  A healthy pivot pin has a standardized deviation of
 * about 1e7 by design and would dominate any statistic it entered.
 */

#include <string>
#include <vector>

#include <Eigen/Dense>

namespace GnssProcessor {

class NetworkEstimator;
struct StationAssembly;

/// Family tag of a shared column, read once per epoch from the estimator's column
/// identity (@ref sharedColumnFamily).  The families match the label prefixes of
/// NetworkEstimator::sharedColumnLabel ("SH" / "PB:" / "CB:" / "CLK:"); a clone of any
/// kind (a clock clone "CLKC:", card SCL; a bias clone "PBC:" or "CBC:", card BCL) and a
/// shared-history column ("HIST:") are @c Other.
enum class NetColFam : char { Sh, Pb, Cb, Clk, Other };

/**
 * @brief The family of one shared column, and its satellite, from the estimator's
 *        column identity (@c NetworkEstimator::sharedColumnIdentity).
 *
 * A satellite column (clock, code bias, phase bias) gives @c Clk, @c Cb or @c Pb and
 * its satellite.  A column with no satellite identity is an SH-VTEC coefficient
 * (@c Sh, either shell) unless it is a clone (a clock clone, card SCL, or a phase- or
 * code-bias clone, card BCL) or a shared-history column (card SCL2 X2), which are
 * @c Other, as is a column out of range.  A bias clone is not a bias of its family: its
 * label ("PBC:", "CBC:") never matched the "PB:" and "CB:" prefixes this function
 * replaced, and its column identity is withheld (@c sharedColumnIdentity is false).
 *
 * @param est The estimator whose current epoch layout is being classified.
 * @param col The shared column.
 * @param sat Optional out: the column's satellite, written for @c Clk, @c Cb and
 *            @c Pb only.  Pass nullptr to skip.
 * @return The family.
 */
NetColFam sharedColumnFamily(const NetworkEstimator& est, Eigen::Index col,
                             std::string* sat = nullptr);

/// Row class.  The enumerator order of the four data classes is the census's
/// column order (@ref dataClassIndex); reordering them changes the trace header.
enum class NetRowClass : unsigned char {
    ObsCode = 0,     ///< @c obs_code
    ObsPhase = 1,    ///< @c obs_phase
    ObsOther = 2,    ///< @c obs_other
    IonoShRow = 3,   ///< @c iono_sh_row
    PivotPin = 4,    ///< @c pivot_pin
    LocalPinRow = 5, ///< @c local_pin_row
    CplRow = 6       ///< @c cpl_row
};

/// Number of data classes (the census/fuse domain).
inline constexpr int kNetDataClasses = 4;

/// @return Whether @p c is a data class: an observation equation or an
///         SH↔slant tie row.  Pins and coupling rows are excluded.
inline constexpr bool isDataRowClass(NetRowClass c) {
    return static_cast<unsigned char>(c) < static_cast<unsigned char>(kNetDataClasses);
}

/// @return Census column index of a data class (0..3), or −1 for pins/coupling.
inline constexpr int dataClassIndex(NetRowClass c) {
    return isDataRowClass(c) ? static_cast<int>(c) : -1;
}

/// @return The class's stable wire name (the string the attribution dumps and
///         the census events have always used).  Never null.
const char* rowClassName(NetRowClass c);

/**
 * @brief One epoch's shared-column labels, families, per-column satellite, and
 *        the two SH mean columns, built once per epoch and shared by every
 *        consumer of @ref classifyRow.
 *
 * The shared layout is rebuilt every epoch, so this map is not valid across
 * epochs.  A label lookup that throws falls back to @c "col<index>" (family
 * @c Other) instead of aborting the walk, so a diagnostic degrades instead of
 * stopping the run.
 */
struct SharedColumnMap {
    Eigen::Index             n{0};        ///< number of shared columns.
    std::vector<std::string> label;       ///< per-column label (size @ref n).
    std::vector<NetColFam>   fam;         ///< per-column family (size @ref n).
    std::vector<std::string> col_sat;     ///< per-column satellite id (CLK / CB / PB columns), empty for SH / unparsed.
    Eigen::Index             c00_l1{-1};  ///< column of @c SH:C:0:0  (−1 = absent).
    Eigen::Index             c00_l2{-1};  ///< column of @c SH2:C:0:0 (−1 = absent).
};

/**
 * @brief Build the epoch's shared-column map from the estimator's labels (kept for the
 *        traces) and column identities (the families, the satellites and the two SH
 *        mean columns).
 * @param est The estimator whose current epoch layout is being classified.
 * @param n   Shared-column count (@c est.numShared()); a non-positive value
 *            yields an empty map.
 */
SharedColumnMap buildSharedColumnMap(const NetworkEstimator& est, Eigen::Index n);

/**
 * @brief Classify one assembled row of one station.
 *
 * @param cm      This epoch's shared-column map (@ref buildSharedColumnMap).
 *                Only the first min(@c R.A_s.cols(), @c cm.n) columns are
 *                examined, so @c cm is expected to cover @c R.A_s.cols().
 * @param R       The station's assembled block.
 * @param row     Row index into @c R.A_s / @c R.A_r / @c R.w / @c R.y.  A row
 *                beyond @c R.w is treated as weight 1.
 * @param row_sat Optional out: the satellite of the first touched shared
 *                column that carries one (written only while still empty, so a
 *                caller may pre-seed it; the scan stops at an SH column).
 *                Pass nullptr to skip.
 */
NetRowClass classifyRow(const SharedColumnMap& cm, const StationAssembly& R,
                        Eigen::Index row, std::string* row_sat);

} // namespace GnssProcessor
