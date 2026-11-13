#pragma once
/**
 * @file AnchorLevelHold.hpp
 * @brief An anchor station's level directions held as part of the phase datum (card ALD).
 *
 * ### What the hold is
 *
 * Each station's data-only local normals have one null direction per constellation, its
 * geometry-free level: a shift of every slant ionosphere of the constellation, of every
 * ambiguity by the band's ionospheric factor, and of the receiver code biases that keeps
 * every code and phase row unchanged.  The direction carries no receiver clock, inter-system
 * bias or wet-delay loading.  At a station that anchors phase-bias families, the pins forbid
 * the move on the pinned ambiguities, and the station's level then moves together with the
 * phase biases its pins define: that joint direction is invisible to all data and is closed
 * only by the station's carried coupling, conditionally on the shared state.  A push on the
 * anchor's level is a push on the phase biases it defines (card R1D, the anchor loop).
 *
 * The hold closes that direction in the datum itself.  While a station anchors at least one
 * family of a constellation, one local row per such constellation is added beside its pins:
 *
 *     a_r = c restricted to the columns that carry a previous posterior,   a_s = 0,
 *     y   = a_r^T (x_prev - x_lin),   w = the pins' weight,
 *
 * where @c c is this epoch's unit level direction from the station's data rows, @c x_prev the
 * carried values of those columns and @c x_lin this epoch's linearization point.  The row
 * holds the carried part of the station's level at its carried value; a column with no carry
 * (a returning line of sight, an arc the slip response freed or dropped this epoch, a pinned
 * ambiguity) is free in it.  At a handover the successor's hold starts at its own carried
 * level, as its pin value does, and the outgoing station's hold ends when it anchors no family
 * of the constellation.  With @c network.phase_datum_anchor_level_hold_all_stations the row is
 * built at every station with a level direction, the minimum-constraint completion of every
 * station level.
 *
 * ### Where the pieces live
 *
 *  - @ref GnssProcessor::anchorLevelDirections "anchorLevelDirections": the data-only null space of a block and its split into one
 *    direction per constellation (pure linear algebra, unit-tested).
 *  - @ref GnssProcessor::anchorLevelHoldRow "anchorLevelHoldRow": the row from a direction, the carried mask and the two values.
 *  - @ref GnssProcessor::networkAnchorLevelHoldRows "networkAnchorLevelHoldRows": the station step (kStationBuildSteps, right after the
 *    pins): classifies the block's columns, reads the carried values through the entity tables
 *    (read only, the parallel produce), appends the rows and records what it did in the
 *    station's slot.
 *  - @ref GnssProcessor::networkAnchorLevelHoldTally "networkAnchorLevelHoldTally": the serial tally after the produce: the counters, the
 *    per-station memo and the hold's edges as events (standing invariant 7): @c hold_start,
 *    @c hold_end and, when one station's hold of a constellation ends in the epoch another's
 *    starts, @c hold_handover, all on the @c ANCHOR_LEVEL tier of network_events.csv.
 *  - @ref GnssProcessor::networkAnchorLevelHoldRunEnd "networkAnchorLevelHoldRunEnd": the run-end report.
 *
 * Off (@c network.phase_datum_anchor_level_hold false): no row, no record, no event, no line.
 */

#include "common/GnssIdentity.hpp"   // System
#include "network/driver/RunTypes.hpp"   // LevelHoldRecord, LevelHoldSkip, StationAssembly

#include <Eigen/Dense>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRunContext;
struct NetworkRunTraces;
struct NetworkRunEndView;
struct StationBuild;
struct NetStation;
struct SyncedEpoch;
enum class StationStep : std::uint8_t;

/// The weight of a hold row: the pins' (an exact gauge, the literal of the ambiguity pins).
inline constexpr double kAnchorLevelHoldWeight = 1e10;

/// The data-only null-space threshold: an eigenvalue below this fraction of the largest is
/// null (the threshold of the RHS attribution's level readout, card R1D).
inline constexpr double kAnchorLevelNullTol = 1e-11;

/// The split threshold: a singular value of the null basis restricted to the columns outside
/// a constellation below this is zero (the basis is orthonormal, so its entries are at most 1).
inline constexpr double kAnchorLevelSplitTol = 1e-6;

/**
 * @brief The level directions of one station block: one unit direction per constellation,
 *        zero outside the constellation's own columns.
 */
struct LevelDirections {
    /// One constellation's direction.
    struct Entry {
        System          sys{System::Unknown};  ///< The constellation.
        Eigen::VectorXd c;                     ///< Unit direction over the block's local columns; exactly zero outside the constellation's columns.
        double          off_sys_max{0.0};      ///< Largest |entry| outside the constellation's columns before it was zeroed (the clock-gauge premise: about 1e-11 on a sound block).
    };
    int                 n_null{0};       ///< Dimension of the data-only null space of the whole block.
    double              lambda_max{0.0}; ///< Largest eigenvalue of the data-only normals.
    std::vector<Entry>  found;           ///< The directions found, in the order of @p want.
    /// The constellations asked for and refused, with the reason.
    std::vector<std::pair<System, LevelHoldSkip>> refused;
};

/**
 * @brief The data-only level directions of a station block.
 *
 * Forms the weighted normals of the data rows, takes their null space (eigenvalues below
 * @ref GnssProcessor::kAnchorLevelNullTol "kAnchorLevelNullTol" times the largest) and, for each wanted constellation, the one
 * direction of that space that vanishes outside the constellation's level columns (its slant
 * ionospheres, ambiguities and receiver code biases): the right singular vector of the basis
 * restricted to the other columns at a singular value below @ref GnssProcessor::kAnchorLevelSplitTol "kAnchorLevelSplitTol".  A
 * constellation with none of those singular values has no level direction in the null space;
 * one with more than one has no unique direction.  The direction is normalized, its entries
 * outside the constellation set to exactly zero, and its sign chosen so that the mean of its
 * slant-ionosphere entries is positive (the first non-zero entry when it has none).
 *
 * @param A_data   The block's data rows over its local columns (rows [0, n_data_rows) of A_r).
 * @param w_data   Their row weights (a non-finite or zero weight drops the row).
 * @param col_sys  Per local column: the constellation of a level column (ambiguity, slant
 *                 ionosphere, receiver code bias), or System::Unknown for every other column.
 * @param col_iono Per local column: true for a slant ionosphere (the sign convention).
 * @param want     The constellations to split out, in the order the result keeps.
 * @return The directions found and the constellations refused.
 */
LevelDirections anchorLevelDirections(const Eigen::MatrixXd& A_data, const Eigen::VectorXd& w_data,
                                      const std::vector<System>& col_sys,
                                      const std::vector<char>& col_iono,
                                      const std::vector<System>& want);

/// A hold row before it is appended to the block.
struct LevelHoldRow {
    Eigen::VectorXd a_r;              ///< The direction restricted to the carried columns (zero elsewhere).
    double          level_prev{0.0};  ///< a_r^T x_prev: the carried level.
    double          level_lin{0.0};   ///< a_r^T x_lin: the level at the linearization point.
    double          y{0.0};           ///< level_prev - level_lin: the row's innovation.
    int             n_cols{0};        ///< Carried columns in the row.
};

/**
 * @brief The hold row of one direction.
 * @param c       The unit level direction (@ref GnssProcessor::LevelDirections::Entry::c "LevelDirections::Entry::c").
 * @param carried Per local column: non-zero when the column carries a previous posterior.
 * @param x_prev  Per local column: the carried value (read only where @p carried).
 * @param x_lin   Per local column: this epoch's linearization point.
 * @return The row; @c n_cols is 0 when no column of the direction is carried.
 */
LevelHoldRow anchorLevelHoldRow(const Eigen::VectorXd& c, const std::vector<char>& carried,
                                const Eigen::VectorXd& x_prev, const Eigen::VectorXd& x_lin);

/**
 * @brief The name of a skip reason, for the event rows and the run-end report.
 * @param s  The skip reason.
 * @return A static string: "none", "no_columns", "no_data", "no_null_direction", "not_unique",
 *         "no_carried_column" or "non_finite"; "?" for a value outside the enum.  Never null.
 */
const char* levelHoldSkipName(LevelHoldSkip s);

// ---- PASS B station step ----

/**
 * @brief The anchor-level hold rows of one station (card ALD).
 *
 * Step `anchor_level_hold` of kStationBuildSteps (network/assembly/StationBuild.hpp), right
 * after `phase_datum_pins`; its kind: AnchorLevelHold.  Gated on
 * @c ctx.knobs.pd_anchor_level_hold.  For every constellation the station anchors a family of
 * (every constellation with level columns under @c pd_anchor_level_hold_all) it takes the
 * level direction of the block's data rows, restricts it to the columns whose carry is live
 * this epoch (an ambiguity with a leveling-carry entry that is not gauge-pinned, not freed,
 * not pending a re-seed and not dropped this epoch; a slant ionosphere with its in-block carry
 * inside its horizon; a receiver code bias with its carry inside its horizon) and appends one
 * row at the pins' weight aimed at the carried level.  Records one @ref GnssProcessor::LevelHoldRecord "LevelHoldRecord" per
 * constellation considered in the station's slot (@c StationAssembly::level_hold) and counts
 * the rows in @c level_hold_rows.
 *
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkAnchorLevelHoldRows(StationBuild& build);

// ---- PASS B serial tally ----

/**
 * @brief The serial tally of the hold after the produce: the run counters, each station's
 *        memo (@c net::StationState::level_hold) and the hold's edges as events.
 *
 * Walks the stations in roster order.  A (station, constellation) whose row was built this
 * epoch and not last epoch raises @c hold_start; one that was held last epoch and not this
 * epoch raises @c hold_end (the reason names the tenure's end, the skip of this epoch, or the
 * station's absence from the assembly); a constellation with at least one end and one start
 * in the same epoch raises @c hold_handover naming both sides.  All on the @c ANCHOR_LEVEL
 * tier of network_events.csv.  Returns at once with the knob off.
 *
 * @param ctx      The run context (the counters and the station memos are written).
 * @param traces   The event stream.
 * @param se       The epoch (its time stamps the events).
 * @param stations The roster (names).
 * @param asm_res  The stations' slots after the produce.
 */
void networkAnchorLevelHoldTally(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                 const SyncedEpoch& se, const std::vector<NetStation>& stations,
                                 const std::vector<StationAssembly>& asm_res);

// ---- Run-end report ----

/**
 * @brief Run-end report: the anchor-level hold of the phase datum (card ALD).
 *
 * The subsystem's one row of kNetworkRunEndReports.  Gated on
 * @c ctx.knobs.pd_anchor_level_hold.  Prints the rows built and the station-epochs held by
 * constellation, the columns a row holds by kind and the columns it leaves free by cause, the
 * tenure records that built no row by cause, the edges (starts, ends, handover epochs), the
 * largest innovation and the largest off-constellation loading (the clock-gauge premise).  An
 * armed run that built no row is reported as armed and inert (standing invariant 4).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkAnchorLevelHoldRunEnd(const NetworkRunEndView& v);

/// @brief Run-scope counters of the anchor-level hold, a base of NetworkServiceCounters
///        (network/state/RunContext.hpp).  Written by the serial tally only.
struct AnchorLevelHoldCounters {
    long long alh_epochs{0};              ///< Epochs the tally ran (the knob armed).
    long long alh_rows{0};                ///< Hold rows built.
    long long alh_station_epochs{0};      ///< Station-epochs with at least one row.
    long long alh_tenure_records{0};      ///< (station, constellation, epoch) records where the station anchored a family of the constellation.
    long long alh_all_records{0};         ///< Records without tenure (the all-stations scope only).
    long long alh_cols{0};                ///< Carried columns held, summed over the rows.
    long long alh_cols_amb{0};            ///< ... of which ambiguities.
    long long alh_cols_iono{0};           ///< ... of which slant ionospheres.
    long long alh_cols_rxcb{0};           ///< ... of which receiver code biases.
    long long alh_excl_pinned{0};         ///< Level columns left free because they are gauge-pinned (the pins hold them), summed over the rows.
    long long alh_excl_dropped{0};        ///< Level columns left free because the slip response freed, dropped or marked them this epoch.
    long long alh_excl_uncarried{0};      ///< Level columns left free because they carry no previous posterior (new, stale or non-finite).
    long long alh_skip_no_data{0};        ///< Records with no row: the block has no data information.
    long long alh_skip_no_null{0};        ///< Records with no row: the data-only null space holds no direction of the constellation.
    long long alh_skip_not_unique{0};     ///< Records with no row: more than one direction of the constellation.
    long long alh_skip_no_carried{0};     ///< Records with no row: no column of the direction is carried.
    long long alh_skip_non_finite{0};     ///< Records with no row: a non-finite innovation.
    long long alh_skip_no_columns{0};     ///< Records with no row: the block has no level column of the constellation.
    long long alh_starts{0};              ///< hold_start events.
    long long alh_ends{0};                ///< hold_end events.
    long long alh_handover_epochs{0};     ///< hold_handover events (one per constellation and epoch).
    long long alh_n_null_max{0};          ///< Largest data-only null dimension seen at a station.
    double    alh_y_max{0.0};             ///< Largest |innovation| of a row (m).
    double    alh_y_max_tow{0.0};         ///< Its TOW.
    char      alh_y_max_sys{'?'};         ///< Its constellation letter.
    std::string alh_y_max_station;        ///< Its station.
    double    alh_off_sys_max{0.0};       ///< Largest off-constellation |loading| of a direction before it was zeroed.
    std::map<char, long long> alh_rows_by_sys;             ///< Rows by constellation letter.
    std::map<char, long long> alh_station_epochs_by_sys;   ///< Station-epochs held by constellation letter.
};

} // namespace GnssProcessor
