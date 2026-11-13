#pragma once
/**
 * @file RaimPriorChannel.hpp
 * @brief The post-fit RAIM's prior channel (@c network.raim_prior_channel,
 *        docs/record/refound/RPC_raim_prior_channel_2026-10-30.md): each carried-prior row of a
 *        station block tested once, in the cell of its own column, with the T4 isolated-arc
 *        test.
 *
 * ## Why a channel of its own
 *
 * On the float vehicle a station block holds, after its observation rows, the rows of the
 * carried local block: rows of @f$L^{-1}[I \mid M \mid y]@f$ with @f$\Sigma = LL^\top@f$ the
 * joint prior covariance of the carried columns in push order.  Row @c k is lower-triangular:
 * its diagonal sits on block column @c k, its primary column, which
 * @c StationAssembly::cpl_row_local_cols records and whose identity its tag carries, and it
 * has coefficients on every column pushed before it.  The carried-phase RAIM pools admit a row
 * to the pool of every ambiguity column its coefficient clears 0.5 on, so one prior residual
 * entered several pools at full weight, and the prior rows were most of the pool's samples.
 * With the key on, the pools keep the observation phase rows only (@c raimPoolAdmitsRow's
 * second setting, card RPA), and the prior rows are tested here instead:
 *
 *  - every row of the coupling region (@c contributionInCouplingRegion) is one sample, with
 *    the statistic @f$|v|\sqrt{w}@f$ (divided by @f$\sqrt{r_i}@f$ under
 *    @c network.raim_redundancy_normalize, the pools' convention);
 *  - it belongs to its own column (@c cpl_row_local_cols), whose subject is a line-of-sight
 *    carry, a receiver code bias or a station carry with no response here
 *    (@ref GnssProcessor::RaimPriorSubjectKind "RaimPriorSubjectKind");
 *  - the table has one cell per (station, subject), the largest statistic of the subject's
 *    rows, and the T4 isolated-arc test runs on it (@ref GnssProcessor::raimSelectIsolatedArcs "raimSelectIsolatedArcs") against a
 *    robust scale of the channel's own.
 *
 * The residual of row @c k is the standardized innovation of column @c k given the columns
 * pushed before it (the Cholesky-order decomposition), so a fault in column @c j also reaches
 * every later row in proportion to @f$(L^{-1})_{kj}@f$.  The cells are per line of sight, so a
 * spread between the bands of one satellite stays in its cell; the coefficient check
 * (@ref GnssProcessor::RaimPriorRow::own_coef "RaimPriorRow::own_coef" against @ref GnssProcessor::RaimPriorRow::max_other_coef "RaimPriorRow::max_other_coef") counts the rows
 * whose largest coefficient is on another column, the size of the remaining spread.
 *
 * The pieces here are pure functions of their arguments, unit-tested on synthetic blocks
 * (tests/network/test_raim_prior_channel.cpp).  The pass that calls them, the responses and
 * the event rows are in @c networkPostFitRaim (network/qc/Raim.cpp).
 */

#include "network/assembly/ContributionKind.hpp"   // ContributionKind
#include "network/qc/Raim.hpp"                     // RaimIsolatedArcs, raimSelectIsolatedArcs, RaimPriorChannelTally

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct StationAssembly;     // network/driver/RunTypes.hpp
struct NetworkRunEndView;   // network/runend/RunEnd.hpp

/// What the own column of a carried-prior row carries, and so which response a flag takes.
enum class RaimPriorSubjectKind : std::uint8_t {
    LineOfSight,  ///< An ambiguity, a slant ionosphere or a phase colour of one (station, satellite): the T4 arc response.
    RxCodeBias,   ///< A receiver code bias of the station: the carried entry is erased (reset).
    Unwired,      ///< A station carry with no response in the channel (the wet delay, the receiver clock, or a row whose subject does not resolve): counted only.
};

/// @brief The name of a prior-subject kind, as written in the run-end text and the event detail.
/// @param k  The subject kind.
/// @return The subject kind's name in the run-end text and the event detail ("los", "rxcb", "unwired").
const char* raimPriorSubjectKindName(RaimPriorSubjectKind k);

/// The subject of one carried-prior row: what its own column carries.
struct RaimPriorSubject {
    RaimPriorSubjectKind kind{RaimPriorSubjectKind::Unwired};  ///< Which response a flag takes.
    std::string sat;     ///< @c LineOfSight: the satellite ("G07"); empty otherwise.
    std::string cb_key;  ///< @c RxCodeBias: the code-bias state key of the own column; empty otherwise.

    /**
     * @brief The column label of the prior table: the satellite of a line of sight,
     *        "rxcb:<key>" for a receiver code bias, the row kind's name for an unwired carry.
     * @param row_kind The kind of the row the subject belongs to (named for @c Unwired).
     * @return The label; one cell per (station, label).
     */
    std::string label(ContributionKind row_kind) const;
};

/**
 * @brief The subject of one row of a station's coupling region.
 *
 *  - @c block_ambiguity_carry, @c block_iono_carry, @c block_phase_colour_carry, and a
 *    @c coupling_scalar row whose tag names a satellite: @c LineOfSight, the tag's satellite;
 *  - @c block_rx_cb_carry, and a @c coupling_scalar row with no satellite, whose own column is
 *    one of the station's receiver code-bias columns (@c StationAssembly::pl_cb): @c RxCodeBias,
 *    that column's state key;
 *  - everything else (@c block_zwd_carry, @c block_rx_clock_carry, a line-of-sight kind whose
 *    tag names no satellite, a code-bias kind whose column is not in @c pl_cb): @c Unwired.
 *
 * @param R       The station's assembly (row tags and @c pl_cb are read).
 * @param row     A row of the coupling region.
 * @param own_col The row's own column (@c cpl_row_local_cols of the row).
 * @return The subject.
 */
RaimPriorSubject raimPriorSubjectOf(const StationAssembly& R, Eigen::Index row,
                                    Eigen::Index own_col);

/// One carried-prior row as the channel tests it.
struct RaimPriorRow {
    int              station{0};                       ///< Roster index of the station.
    Eigen::Index     row{0};                           ///< The row of the station block (A_s / A_r).
    Eigen::Index     own_col{0};                       ///< Its own column (@c cpl_row_local_cols).
    ContributionKind kind{ContributionKind::Unset};    ///< The row's kind, from its tag.
    RaimPriorSubject subject;                          ///< What its own column carries.
    double           statistic{0.0};                   ///< @f$|v|\sqrt{w}@f$, over @f$\sqrt{r_i}@f$ under the redundancy normalization.
    double           own_coef{0.0};                    ///< |A_r(row, own_col)|: the row's coefficient on its own column.
    double           max_other_coef{0.0};              ///< The largest |A_r(row, c)| over the station's other carried columns (0 when none).
    Eigen::Index     max_other_col{-1};                ///< That column (-1 when none).

    /// @brief Whether the row is dominated by a carried column other than its own.
    /// @return Whether the row's largest coefficient is on another carried column than its own.
    bool otherDominant() const { return max_other_coef > own_coef; }
};

/**
 * @brief Collect one station's carried-prior rows: every row of its coupling region, with
 *        its statistic, own column, subject and coefficient check.
 *
 * A row at or below the redundancy floor under the normalization is left out and counted in
 * @c tally.rows_floor, as the observation pools leave such a row out.  A non-finite statistic
 * is left out.  Tested rows are appended to @p out and counted (@c RaimPriorChannelTally::noteRow,
 * network/qc/Raim.hpp).
 *
 * @param R            The station's assembly: @c A_r, @c w, @c row_tags, @c cpl_row_start,
 *                     @c cpl_row_local_cols and @c pl_cb are read.
 * @param station      The station's roster index.
 * @param station_name Its name (for the tally's identity of the largest row).
 * @param resid        The post-fit residuals of the station's rows (y - A_s x_s - A_r x_r).
 * @param row_r        The rows' redundancy numbers, or empty when not computed.
 * @param normalize    Whether the statistic is divided by @f$\sqrt{r_i}@f$
 *                     (@c network.raim_redundancy_normalize, with @p row_r available).
 * @param r_floor      The redundancy floor (@c kBaardaRedundancyFloor).
 * @param tow          The epoch, for the tally.
 * @param out          [out] The tested rows, appended.
 * @param tally        [in/out] The run-scope tally.
 * @return The number of rows appended.
 */
std::size_t raimPriorChannelCollect(const StationAssembly& R, int station,
                                    const std::string& station_name,
                                    const Eigen::VectorXd& resid, const Eigen::VectorXd& row_r,
                                    bool normalize, double r_floor, double tow,
                                    std::vector<RaimPriorRow>& out, RaimPriorChannelTally& tally);

/**
 * @brief The channel's verdict on one pass: the prior table, its median polish and scale,
 *        the isolated-arc flags, and the responses they take.
 */
struct RaimPriorSelection {
    double scale{1.0};  ///< The channel's robust scale: max(floor, 1.4826 x the median row statistic).
    /// The prior table, (station, label, value): one cell per (station, subject), the largest
    /// statistic of the subject's rows.  In (station, label) order.
    std::vector<std::tuple<int, std::string, double>> cells;
    std::vector<std::size_t> cell_row;   ///< Per cell, the index (into the pass's rows) of its largest row.
    std::vector<double>      interaction;///< Per cell, the median polish's interaction.
    double                   overall{0.0};  ///< The polish's overall effect.
    /// The isolated-arc test's result on the table (@ref raimSelectIsolatedArcs): @c refloat holds
    /// the (station, label) cells flagged that passed the maturity gate, @c held the cells it held.
    RaimIsolatedArcs isolated;
    /// The line-of-sight flags the channel refloats: (station, satellite), in table order, without
    /// the arcs the observation channel refloats in the same pass.
    std::vector<std::pair<int, std::string>> arcs;
    std::vector<std::size_t> arc_cells;   ///< Per entry of @c arcs, its cell.
    std::vector<std::size_t> arcs_both;   ///< Cells of the line-of-sight flags the observation channel also refloats.
    std::vector<std::size_t> rxcb;        ///< Cells of the receiver code-bias flags (their carries are reset).
    std::vector<std::size_t> unwired;     ///< Cells of the flags on an unwired subject.
    std::vector<std::size_t> zwd;         ///< Cells of the flags on the station wet delay's block row that take the wet delay's event response (empty unless @ref raimPriorWireZwd moved them out of @c unwired).
    std::vector<std::size_t> held_cells;  ///< Per entry of @c isolated.held, its cell.
    /// The flagged cells that take a response: a line of sight refloated (by this channel, or by
    /// the observation channel in the same pass) and a receiver code bias reset.
    std::set<std::pair<int, std::string>> acted_cells;

    /// @brief Flag statistic of one prior-table cell, the quantity compared with the bar.
    /// @param cell Index into @c cells (and @c interaction); not range-checked.
    /// @return The flag statistic of a cell: its interaction over the scale.
    ///         Dimensionless, in units of the channel's robust scale.
    double flagStatistic(std::size_t cell) const { return interaction[cell] / scale; }

    /**
     * @brief Whether a cell was acted on: a refloat (by this channel or, for a line of sight,
     *        the observation channel) or a reset.  An unwired or held cell reads false.
     * @param station The cell's station.
     * @param label   The cell's label.
     * @return Membership in @c acted_cells.
     */
    bool acted(int station, const std::string& label) const;
};

/**
 * @brief The channel's test on one pass (pure).
 *
 * Builds the prior table from @p rows, its median polish and robust scale, runs the T4
 * isolated-arc test on it (@ref GnssProcessor::raimSelectIsolatedArcs "raimSelectIsolatedArcs", with no persistence gate) and sorts the
 * flags into responses by subject.
 *
 * @param rows        The pass's tested rows (@ref GnssProcessor::raimPriorChannelCollect "raimPriorChannelCollect").
 * @param s0_floor    The floor of the scale (@c network.raim_s0_floor).
 * @param k_sigma     The bar, in the scale (@c network.raim_k_sigma).
 * @param caught_sats Satellites the observation channel caught wholesale this pass: their
 *                    line-of-sight cells are not flagged again.
 * @param obs_arcs    The arcs the observation channel refloats this pass: a line-of-sight flag on
 *                    one of them is recorded in @c arcs_both and not refloated twice.
 * @param mature      The maturity gate's verdict (true with the gate off).
 * @return The selection; empty with no rows.
 */
RaimPriorSelection raimPriorChannelSelect(const std::vector<RaimPriorRow>& rows, double s0_floor,
                                          double k_sigma, const std::set<std::string>& caught_sats,
                                          const std::vector<std::pair<int, std::string>>& obs_arcs,
                                          bool mature);

/**
 * @brief Moves the flags on the station wet delay's own block row (@c block_zwd_carry) out of
 *        @c unwired into @c zwd and marks them acted.
 *
 * Called only where the wet delay's event response is armed (NetworkKnobs::zwd_event_on, the
 * in-block route under the float vehicle): those flags then take the response of a RAIM per-arc
 * attribution (the station's carried wet delay is erased), so they are neither unwired nor
 * counted as such.  Every other unwired flag stays where it is.
 *
 * @param sel  [in/out] The selection (@ref GnssProcessor::raimPriorChannelSelect "raimPriorChannelSelect").
 * @param rows The pass's tested rows the selection indexes.
 */
void raimPriorWireZwd(RaimPriorSelection& sel, const std::vector<RaimPriorRow>& rows);

/**
 * @brief Fold one pass's selection into the tally: the scale, the cells, the flags and held
 *        cells, the arcs both channels flagged, the unwired flags.  The responses' outcomes
 *        (@c arcs, @c arcs_ambiguities, @c arcs_empty, @c rxcb_resets, @c rxcb_no_entry) are
 *        counted by the caller, which takes them.
 * @param sel       The selection.
 * @param any_rows  Whether the pass tested at least one row.
 * @param tally     [in/out] The tally.
 */
void raimPriorChannelNoteSelection(const RaimPriorSelection& sel, bool any_rows,
                                   RaimPriorChannelTally& tally);

/**
 * @brief The event detail of one flagged cell: the scale, the row's kind, own column,
 *        statistic and coefficient check, and its subject.
 * @param r     The cell's largest row.
 * @param scale The channel's scale this pass.
 * @return "sigma0_p=..;row_kind=..;own_col=..;row_stat=..;own_coef=..;max_other_coef=..;max_other_col=..;subject=.."
 */
std::string raimPriorEventDetail(const RaimPriorRow& r, double scale);

/// One line of the channel's run-end section.
struct RaimPriorReportLine {
    bool        warn{false};  ///< Printed at warning level.
    std::string text;         ///< The line, without the "Network (RAIM prior channel" prefix.
};

/**
 * @brief The run-end section's lines (pure, so the text is unit-tested).
 * @param t       The tally.
 * @param k_sigma The bar (@c network.raim_k_sigma).
 * @return The lines in print order: the denominators, the rows by kind, the flags and
 *         responses, the coefficient check; the outcomes INERT and NO FLAG in words.
 */
std::vector<RaimPriorReportLine> raimPriorChannelReport(const RaimPriorChannelTally& t,
                                                        double k_sigma);

/**
 * @brief Run-end section: the prior channel (@c network.raim_prior_channel).
 *
 * A section of the RAIM subsystem's report (@c networkRaimRunEnd); prints
 * @ref GnssProcessor::raimPriorChannelReport "raimPriorChannelReport".  Gate: @c ctx.knobs.raim_prior_channel.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkRaimPriorChannelRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
