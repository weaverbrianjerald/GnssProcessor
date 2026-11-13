#pragma once
/**
 * @file SdEwlRows.hpp
 * @brief The SD pre-reduce EWL row builder as named stages: the ambiguity candidates, the arc
 *        resolution, and the whitened groups.
 *
 * `networkAppendSdEwlRows` (SdEwlAr.cpp, declared in SdEwlAr.hpp) builds a
 * @ref GnssProcessor::SdEwlRowView "SdEwlRowView" once per station-epoch and calls the stages in order. The algebra, the
 * correction frame and the thread-safety contract are documented at that declaration; a
 * stage here only owns one step of it. Everything written is the station's own block.
 */

#include "network/driver/RunTypes.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "config/GnssConfig.hpp"
#include "common/GnssTypes.hpp"
#include "slip/MwSlipDetector.hpp"   // MwSlipDetector::MwArcZdMean

#include <Eigen/Core>

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Trace row sink, full form: the status, the satellites and signals, the arc statistics and
/// the three trailing integer-path columns.
using SdEwlTraceFull = std::function<void(
    const char*, const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, std::size_t, std::size_t, bool, double, double, double,
    double, double, long long, bool, bool)>;

/// Trace row sink, float-path form (the integer-path columns are zero).
using SdEwlTraceShort = std::function<void(
    const char*, const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, std::size_t, std::size_t, bool, double, double, double,
    double, double)>;

/// One ambiguity candidate of a (satellite, band): the signal, its A_r local column, and its
/// global state index.
using SdEwlCand = std::tuple<std::string, int, int>;

/// The station's candidates by (satellite, band digit), each list ascending by signal name.
using SdEwlCandidates = std::map<std::pair<std::string, char>, std::vector<SdEwlCand>>;

/// One resolved arc: the zero-difference-consistent EWL statistic plus the six columns the row
/// needs (two local ambiguities, and the shared PB/CB pair per band).
struct SdEwlArc {
    std::string sat;                 ///< the satellite id
    double      mean_m{ 0.0 };       ///< the arc's EWL statistic: the mean of the zero-difference-consistent MW observable (m)
    double      var_m2{ 0.0 };       ///< the variance of that mean after the configured sigma floor (m^2)
    std::size_t count{ 0 };          ///< samples in the arc's accumulator
    bool        bia{ false };        ///< the statistic has both the satellite (BIA) and the receiver (BSSD) biases removed
    /// The wavelengths (m) of the EWL combination, of the first band and of the second band.
    double      lam_ewl{ 0.0 }, lam_b1{ 0.0 }, /**< Wavelength of the first band (m). */ lam_b2{ 0.0 };   /**< Wavelength of the second band (m). */
    /// The carrier frequencies (Hz) of the first and the second band.
    double      f1{ 0.0 }, /**< Carrier frequency of the first band (Hz). */ f2{ 0.0 };   /**< Carrier frequency of the second band (Hz). */
    /// The A_r local ambiguity columns of the first and the second band (-1: none).
    int         amb_col_b1{ -1 }, /**< A_r local ambiguity column of the first band (-1: none). */ amb_col_b2{ -1 };   /**< A_r local ambiguity column of the second band (-1: none). */
    /// The shared columns of this satellite: the phase bias of each band (pb) and the code bias of
    /// each band (cb), by band 1 and band 2 (-1: not registered).
    Eigen::Index pb_b1{ -1 }, /**< Shared phase-bias column of the first band (-1: not registered). */ pb_b2{ -1 }, /**< Shared phase-bias column of the second band (-1: not registered). */ cb_b1{ -1 }, /**< Shared code-bias column of the first band (-1: not registered). */ cb_b2{ -1 };   /**< Shared code-bias column of the second band (-1: not registered). */
    /// This arc's linearized prediction of the MW observable, i.e. its half of
    /// @f$H\,x_{lin}@f$ (see the correction-frame note at the row build).  Split in
    /// two because increment 2b's fixed row drops its local ambiguity coefficients,
    /// and the linearization point must lose exactly the same terms in the same
    /// place, or the row is folded in a mixed frame.
    double      pred_amb_m{ 0.0 };   ///< the two local ambiguity terms of H·x_lin
    double      pred_bias_m{ 0.0 };  ///< the four shared PB/CB terms of H·x_lin
    double      pred_m{ 0.0 };       ///< pred_amb_m + pred_bias_m (the float row's frame)
    // Trace-only provenance (§6): the elected phase pair, the code channels the
    // MW actually consumed, and how many candidate pairs were searched.
    /// The elected phase pair's signals and the code channels the MW consumed (trace only).
    std::string sig1, sig2, /**< Phase signal of the second band in the elected pair (trace only). */ code1, /**< Code channel of the first band consumed by the MW (trace only). */ code2;   /**< Code channel of the second band consumed by the MW (trace only). */
    std::size_t n_cand{ 0 };   ///< the number of candidate pairs searched (trace only)
    // The published integer for this arc, if any (increment 2b).  `n_cyc` is in the
    // r0-anchored frame, so only the between-satellite difference against the
    // group's pivot is meaningful.
    bool        has_fix{ false };   ///< a published integer is attached to this arc
    long long   n_fix_cyc{ 0 };     ///< that integer (cycles), in the r0-anchored frame; meaningful only when @c has_fix
    /// The integer came from a rounded double difference rather than from a frame
    /// choice.  A fixed group must contain at least one of these (see the group
    /// split below), or it asserts a hard constraint with no measurement behind it.
    bool        fix_measured{ false };
};

/// One gated row of a group: the satellite arc, its correction-frame right-hand side and variance.
struct SdEwlRow { const SdEwlArc* s; /**< The non-pivot satellite arc of the single difference (not owned; points into the group's arc list). */ double z; /**< The right-hand side in the correction frame (m): the single-difference statistic minus its linearized prediction. */ double var; /**< The variance of this arc's own statistic (m^2); the pivot arc's variance is added when the row covariance is built. */ };

/**
 * @brief The state of one satellite's phase-pair search: the winning pair and its arc, the
 *        near-miss bookkeeping that attributes a skip to a cause, and the published integer.
 *
 * One per satellite, declared by @ref sdEwlResolveSat, filled by @ref sdEwlElectPair and read
 * by the stages after it. The members are the search's former locals, unchanged.
 */
struct SdEwlPairSearch {
    MwSlipDetector::MwArcZdMean zd;                 ///< the winner
    std::string sig1, /**< Phase signal of the first band of the winning pair (empty: none). */ sig2;   ///< the winning pair's phase signals (empty: none)
    int col1 = -1, /**< A_r local column of the first-band signal (-1: none). */ col2 = -1, /**< A_r local column of the second-band signal (-1: none). */ sidx1 = -1, /**< Global state index of the first-band ambiguity (-1: none). */ sidx2 = -1;   ///< their local columns and state indices
    std::size_t n_cand = 0, /**< Number of candidate pairs searched, counted even after a winner is chosen. */ best_n = 0;   ///< pairs searched; sample count of the best near-miss
    bool any_exists = false, /**< At least one candidate pair had a formed MW arc. */ any_recon_fail = false;   ///< a pair existed; one could not be framed
    std::string near_s1, /**< Phase signal of the first band of the best near-miss pair. */ near_s2;   ///< the best near-miss pair
    const SdEwlFix* my_fix = nullptr;               ///< the published integer, if usable
};

/**
 * @brief Handles on one station-epoch of the SD-EWL row builder that every stage reads.
 *
 * References and copies only; owns nothing. The members are the parameters of
 * `networkAppendSdEwlRows` and the values its prelude derived from them.
 */
struct SdEwlRowView {
    const GnssConfig& config_;           ///< the run's configuration
    NetStation& st;                      ///< this station (its MW detector, parameter set, name and counters)
    const NetworkEstimator& est;         ///< the estimator, for the const shared-column lookups only
    const std::vector<PppObservation>& sta_obs;   ///< this station's modeled observations this epoch
    const std::map<int, int>& h2local;   ///< state index -> local A_r column of this epoch's phase rows
    const GnssTime& t;                   ///< the epoch time (trace stamp only)
    Eigen::MatrixXd& A_s;                ///< the station's shared-column design block, extended in place
    Eigen::MatrixXd& A_r;                ///< the station's local-column design block, extended in place
    Eigen::VectorXd& w;                  ///< the station's row weights, extended in place
    Eigen::VectorXd& y;                  ///< the station's right-hand side (correction frame), extended in place
    std::vector<RowTag>& tags;           ///< the station's row tags: one SD-EWL tag per appended row, in row order
    std::vector<std::string>& trace_rows;   ///< pre-formatted trace lines of this station-epoch (untouched unless the trace is armed)
    const net::StationTable* fix;        ///< the published-integer station table, or null (every row then takes the float path)
    std::size_t station_index;           ///< this station's index into @c fix
    const SdEwlConfig& cfg;              ///< the SD-EWL configuration (@c network.sd_ewl)
    NetStation::SdEwlCounters& ctr;      ///< this station's SD-EWL counters
    bool fix_on;                         ///< the integer path is armed and a fix table was passed
    SdEwlTraceFull tr2;   ///< trace row, full form (with the integer-path columns)
    SdEwlTraceShort tr;   ///< trace row, float-path form
};

/**
 * @brief Fills the station's per-(satellite, band) ambiguity candidates from the ambiguities
 *        of its parameter set that have a phase row this epoch, each list sorted ascending by
 *        signal name so the pair search that follows is a strict total order (byte identity).
 * @param v     Station-epoch handles (the parameter set and the state index to local column
 *              map are read).
 * @param[out] cand  Candidates by (satellite, band digit), appended to; the call is made
 *              on an empty table once per station-epoch.
 */
void sdEwlFillCandidates(const SdEwlRowView& v, SdEwlCandidates& cand);

/**
 * @brief Resolves every eligible satellite of the station into an arc, grouped by
 *        constellation: one call of @ref GnssProcessor::sdEwlResolveSat "sdEwlResolveSat" per observation, in order.
 * @param v          Station-epoch handles (the station's observations are read).
 * @param cand       Ambiguity candidates by (satellite, band), from @ref GnssProcessor::sdEwlFillCandidates "sdEwlFillCandidates".
 * @param[in,out] by_const  Resolved arcs by constellation letter (appended to).
 */
void sdEwlResolveArcs(const SdEwlRowView& v, const SdEwlCandidates& cand,
                      std::map<char, std::vector<SdEwlArc>>& by_const);

/**
 * @brief Resolves one satellite into an arc and appends it to its constellation's list.
 *        Screens the satellite (band pair, ambiguity candidates), then runs the three
 *        stages in order; every skip is counted and traced with its cause and ends the
 *        satellite.
 * @param v          Station-epoch handles.
 * @param cand       Ambiguity candidates by (satellite, band).
 * @param o          The satellite's observation.
 * @param by_const   Resolved arcs by constellation letter (appended to).
 */
void sdEwlResolveSat(const SdEwlRowView& v, const SdEwlCandidates& cand, const PppObservation& o,
                     std::map<char, std::vector<SdEwlArc>>& by_const);

/**
 * @brief Looks up the published integer and elects the satellite's phase pair: the first
 *        candidate pair, in ascending signal order, whose MW arc is mature. Without a
 *        winner the skip is attributed to its cause (no arc, frame reconstruction, young).
 * @param v      Station-epoch handles.
 * @param o      The satellite's observation.
 * @param cons   The satellite's constellation letter.
 * @param c1it   Candidates of the lower band.
 * @param c2it   Candidates of the upper band.
 * @param ps     The pair-search state (filled).
 * @return false when no pair was elected (the skip is already counted and traced).
 */
bool sdEwlElectPair(const SdEwlRowView& v, const PppObservation& o, char cons,
                    SdEwlCandidates::const_iterator c1it,
                    SdEwlCandidates::const_iterator c2it, SdEwlPairSearch& ps);

/**
 * @brief Checks the elected pair: the detector's wavelength against the tier's nominal one,
 *        and that the arc carries the code channels the bias columns are keyed on.
 * @param v    Station-epoch handles.
 * @param o    The satellite's observation.
 * @param f1   Lower band frequency, Hz.
 * @param f2   Upper band frequency, Hz.
 * @param ps   The pair-search state.
 * @return false when a check failed (the skip is already counted and traced).
 */
bool sdEwlCheckPair(const SdEwlRowView& v, const PppObservation& o, double f1, double f2,
                    const SdEwlPairSearch& ps);

/**
 * @brief Builds the arc of the elected pair: statistics and floors, the attached integer,
 *        the four shared bias columns, and the arc's half of the linearization, which is read
 *        from the station's parameter set (a bias the station holds no column for reads as
 *        zero). A missing shared column skips the satellite; otherwise the arc is appended to
 *        @p by_const.
 * @param v          Station-epoch handles.
 * @param o          The satellite's observation.
 * @param cons       The satellite's constellation letter.
 * @param f1         Lower band frequency, Hz.
 * @param f2         Upper band frequency, Hz.
 * @param ps         The pair-search state.
 * @param by_const   Resolved arcs by constellation letter (appended to).
 */
void sdEwlBuildArc(const SdEwlRowView& v, const PppObservation& o, char cons, double f1, double f2,
                   const SdEwlPairSearch& ps, std::map<char, std::vector<SdEwlArc>>& by_const);

/**
 * @brief Builds one whitened group of rows against the group's pivot arc: gates each
 *        row, caps the count, appends the design, right-hand side and tags to the block,
 *        and traces every outcome. Unwinds the append if the factorization fails.
 * @param v            Station-epoch handles (the design blocks, tags and counters are
 *                     written through it).
 * @param g            The group's arcs, pivot first (the most mature arc).  Fewer than two
 *                     arcs is counted as no pivot and emits nothing.
 * @param fixed_group  True for a group whose arcs carry a published integer (the rows fold
 *                     the integer out of the right-hand side and drop their local
 *                     ambiguity coefficients); false for the float group.
 */
void sdEwlEmitGroup(const SdEwlRowView& v, const std::vector<const SdEwlArc*>& g,
                    bool fixed_group);

/**
 * @brief Appends one group's rows to the station block. The rows share the pivot arc's
 *        noise, so R = diag(var_s) + var_p * 11^T; they are whitened by L^-1 into
 *        unit-information rows and the append is unwound if the factorization fails.
 * @param v            Station-epoch handles (the design blocks, row weights, right-hand
 *                     side, tags and counters are extended in place through it).
 * @param p            The group's pivot arc, whose ambiguity columns and noise every row
 *                     shares.
 * @param rows         The gated rows (arc, correction-frame right-hand side in m, variance
 *                     in m^2), one per appended design row.
 * @param fixed_group  True when the rows are of the fixed-integer group, so their local
 *                     ambiguity coefficients stay zero; false for the float group.
 */
void sdEwlAppendRows(const SdEwlRowView& v, const SdEwlArc& p, const std::vector<SdEwlRow>& rows,
                     bool fixed_group);

/**
 * @brief Partitions each constellation's arcs into the fixed and float groups (one group
 *        when the integer path is off) and emits them. The pivot is the most mature arc,
 *        ties by satellite name, so the row set is deterministic.
 * @param v          Station-epoch handles.
 * @param[in,out] by_const  Resolved arcs by constellation letter; each constellation's list
 *                   is re-ordered (most mature first, then ascending satellite name).
 */
void sdEwlBuildGroups(const SdEwlRowView& v, std::map<char, std::vector<SdEwlArc>>& by_const);

} // namespace GnssProcessor
