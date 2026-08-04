#pragma once
/**
 * @file NetworkRowClassify.hpp
 * @brief INC-0 of docs/SPEC_prefit_census_observability.md — the ONE shared
 *        row/column classifier for every assembled network row.
 *
 * `classifyRow` and the shared-column family/label pass used to live in an
 * anonymous namespace inside `src/core/filter/NetworkRhsAttrib.cpp`.  The
 * prefit census (INC-1), the structural fuse (INC-2), the seed-birth tripwire
 * (INC-3) and the topology ledger (INC-4) all need the SAME verdict about what
 * a row is; a second copy would let the census and the attribution disagree
 * about whether a given row is code, phase or a pin — the one thing that would
 * make both instruments untrustworthy at once.  One classifier, one truth.
 *
 * ## What a class means
 *
 *  - @c obs_code / @c obs_phase / @c obs_other — DATA rows: an actual
 *    observation equation.  The phase discriminator is the satellite
 *    PHASE-bias partial (`PB:` column) with the ambiguity-column touch as the
 *    secondary signal for runs that estimate no satellite phase bias — a
 *    property of the measurement model, so it holds whatever the QC
 *    configuration is (the frame-diag convention, NetworkFrameDiag.cpp).
 *    @c obs_other is a data row touching a shared column of neither family
 *    (e.g. a clock-only pseudo-observation): counted, never misfiled.
 *  - @c iono_sh_row — the SH-VTEC ↔ slant-iono tie pseudo-observation.  DATA
 *    class, but it carries no receiver clock, so the census does NOT
 *    common-mode-centre it.
 *  - @c pivot_pin / @c local_pin_row — rows with NO shared partial: a hard
 *    gauge pin (w ≥ 1e9) or a soft local prior/anchor row.
 *  - @c cpl_row — Card-1 conditional coupling rows: statements about the
 *    CARRY, not data.
 *
 * Only the four DATA classes are census/fuse material (@ref isDataRowClass);
 * a healthy pivot pin runs a standardized deviation of ~1e7 by design and
 * would drown any statistic it was allowed into.
 */

#include <string>
#include <vector>

#include <Eigen/Dense>

namespace GnssProcessor {

class NetworkEstimator;
struct StationAssembly;

/// Family tag of a shared column, parsed once per epoch from its label
/// ("CLK:" / "CB:" / "PB:" / "SH"…, NetworkEstimator.cpp:138-153).
enum class NetColFam : char { Sh, Pb, Cb, Clk, Other };

/// Row class.  The enumerator ORDER of the four DATA classes is the census's
/// column order (@ref dataClassIndex) — do not reorder without the trace header.
enum class NetRowClass : unsigned char {
    ObsCode = 0,     ///< @c obs_code
    ObsPhase = 1,    ///< @c obs_phase
    ObsOther = 2,    ///< @c obs_other
    IonoShRow = 3,   ///< @c iono_sh_row
    PivotPin = 4,    ///< @c pivot_pin
    LocalPinRow = 5, ///< @c local_pin_row
    CplRow = 6       ///< @c cpl_row
};

/// Number of DATA classes (the census/fuse domain).
inline constexpr int kNetDataClasses = 4;

/// @return Whether @p c is a DATA class — an observation equation or an
///         SH↔slant tie row.  Pins and coupling rows are excluded.
inline constexpr bool isDataRowClass(NetRowClass c) {
    return static_cast<unsigned char>(c) < static_cast<unsigned char>(kNetDataClasses);
}

/// @return Census column index of a DATA class (0..3), or −1 for pins/coupling.
inline constexpr int dataClassIndex(NetRowClass c) {
    return isDataRowClass(c) ? static_cast<int>(c) : -1;
}

/// @return The class's stable wire name (the string the attribution dumps and
///         the census events have always used).  Never null.
const char* rowClassName(NetRowClass c);

/**
 * @brief One epoch's shared-column labels, families, per-column satellite, and
 *        the two SH mean columns — built ONCE per epoch and shared by every
 *        consumer of @ref classifyRow.
 *
 * The shared layout is rebuilt every epoch, so this cannot be cached across
 * epochs.  A label lookup that throws falls back to @c "col<index>" (family
 * @c Other) rather than aborting the walk: a diagnostic must degrade, never
 * take the run down.
 */
struct SharedColumnMap {
    Eigen::Index             n{0};        ///< number of shared columns.
    std::vector<std::string> label;       ///< per-column label (size @ref n).
    std::vector<NetColFam>   fam;         ///< per-column family (size @ref n).
    std::vector<std::string> col_sat;     ///< per-column satellite id, empty for SH / unparsed.
    Eigen::Index             c00_l1{-1};  ///< column of @c SH:C:0:0  (−1 = absent).
    Eigen::Index             c00_l2{-1};  ///< column of @c SH2:C:0:0 (−1 = absent).
};

/**
 * @brief Build the epoch's shared-column map from the estimator's labels.
 * @param est The estimator whose CURRENT epoch layout is being classified.
 * @param n   Shared-column count (@c est.numShared()); a non-positive value
 *            yields an empty map.
 */
SharedColumnMap buildSharedColumnMap(const NetworkEstimator& est, Eigen::Index n);

/**
 * @brief Classify one assembled row of one station.
 *
 * @param cm      This epoch's shared-column map (@ref buildSharedColumnMap).
 *                Its @c fam / @c col_sat must be sized to @c R.A_s.cols().
 * @param R       The station's assembled block.
 * @param row     Row index into @c R.A_s / @c R.A_r / @c R.w / @c R.y.
 * @param row_sat Optional out: the satellite of the FIRST labelled shared
 *                column the row touches (only written while still empty, so a
 *                caller may pre-seed it).  Pass nullptr to skip.
 */
NetRowClass classifyRow(const SharedColumnMap& cm, const StationAssembly& R,
                        Eigen::Index row, std::string* row_sat);

} // namespace GnssProcessor
