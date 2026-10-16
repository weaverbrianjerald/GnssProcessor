#pragma once
/**
 * @file SdEwlRows.hpp
 * @brief The SD pre-reduce EWL row builder as named stages: the correction-frame state
 *        indices, the ambiguity candidates, the arc resolution, and the whitened groups.
 *
 * `networkAppendSdEwlRows` (SdEwlAr.cpp, declared in SdEwlAr.hpp) builds a
 * @ref SdEwlRowView once per station-epoch and calls the stages in order. The algebra, the
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
    std::string sat;
    double      mean_m{ 0.0 };
    double      var_m2{ 0.0 };
    std::size_t count{ 0 };
    bool        bia{ false };
    double      lam_ewl{ 0.0 }, lam_b1{ 0.0 }, lam_b2{ 0.0 };
    double      f1{ 0.0 }, f2{ 0.0 };
    int         amb_col_b1{ -1 }, amb_col_b2{ -1 };
    Eigen::Index pb_b1{ -1 }, pb_b2{ -1 }, cb_b1{ -1 }, cb_b2{ -1 };
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
    std::string sig1, sig2, code1, code2;
    std::size_t n_cand{ 0 };
    // The published integer for this arc, if any (increment 2b).  `n_cyc` is in the
    // r0-anchored frame, so only the between-satellite difference against the
    // group's pivot is meaningful.
    bool        has_fix{ false };
    long long   n_fix_cyc{ 0 };
    /// The integer came from a rounded double difference rather than from a frame
    /// choice.  A fixed group must contain at least one of these (see the group
    /// split below), or it asserts a hard constraint with no measurement behind it.
    bool        fix_measured{ false };
};

/// One gated row of a group: the satellite arc, its correction-frame right-hand side and variance.
struct SdEwlRow { const SdEwlArc* s; double z; double var; };

/**
 * @brief The state of one satellite's phase-pair search: the winning pair and its arc, the
 *        near-miss bookkeeping that attributes a skip to a cause, and the published integer.
 *
 * One per satellite, declared by @ref sdEwlResolveSat, filled by @ref sdEwlElectPair and read
 * by the stages after it. The members are the search's former locals, unchanged.
 */
struct SdEwlPairSearch {
    MwSlipDetector::MwArcZdMean zd;                 ///< the winner
    std::string sig1, sig2;                         ///< the winning pair's phase signals (empty: none)
    int col1 = -1, col2 = -1, sidx1 = -1, sidx2 = -1;   ///< their local columns and state indices
    std::size_t n_cand = 0, best_n = 0;             ///< pairs searched; sample count of the best near-miss
    bool any_exists = false, any_recon_fail = false;   ///< a pair existed; one could not be framed
    std::string near_s1, near_s2;                   ///< the best near-miss pair
    const SdEwlFix* my_fix = nullptr;               ///< the published integer, if usable
};

/**
 * @brief Handles on one station-epoch of the SD-EWL row builder that every stage reads.
 *
 * References and copies only; owns nothing. The members are the parameters of
 * `networkAppendSdEwlRows` and the values its prelude derived from them.
 */
struct SdEwlRowView {
    const GnssConfig& config_;
    NetStation& st;
    const NetworkEstimator& est;
    const std::vector<PppObservation>& sta_obs;
    const std::map<int, int>& h2local;   ///< state index -> local A_r column of this epoch's phase rows
    const GnssTime& t;
    Eigen::MatrixXd& A_s;
    Eigen::MatrixXd& A_r;
    Eigen::VectorXd& w;
    Eigen::VectorXd& y;
    std::vector<RowTag>& tags;
    std::vector<std::string>& trace_rows;
    const net::StationTable* fix;
    std::size_t station_index;
    const SdEwlConfig& cfg;
    NetStation::SdEwlCounters& ctr;
    bool fix_on;
    SdEwlTraceFull tr2;   ///< trace row, full form (with the integer-path columns)
    SdEwlTraceShort tr;   ///< trace row, float-path form
};

/**
 * @brief Fills the station's shared phase-bias and code-bias state indices, keyed by
 *        (satellite, signal). A column another station registered has no state here and
 *        reads as absent, which matches the real rows (the innovations omit it).
 */
void sdEwlFillBiasStates(const SdEwlRowView& v,
                         std::map<std::pair<std::string, std::string>, int>& pb_state,
                         std::map<std::pair<std::string, std::string>, int>& cb_state);

/**
 * @brief Fills the station's per-(satellite, band) ambiguity candidates from
 *        StateManager's own key map, each list sorted ascending by signal name so the pair
 *        search that follows is a strict total order (byte identity).
 */
void sdEwlFillCandidates(const SdEwlRowView& v, SdEwlCandidates& cand);

/**
 * @brief Resolves every eligible satellite of the station into an arc, grouped by
 *        constellation: one call of @ref sdEwlResolveSat per observation, in order.
 */
void sdEwlResolveArcs(const SdEwlRowView& v, const Eigen::VectorXd& x_lin,
                      const std::map<std::pair<std::string, std::string>, int>& pb_state,
                      const std::map<std::pair<std::string, std::string>, int>& cb_state,
                      const SdEwlCandidates& cand,
                      std::map<char, std::vector<SdEwlArc>>& by_const);

/**
 * @brief Resolves one satellite into an arc and appends it to its constellation's list.
 *        Screens the satellite (band pair, ambiguity candidates), then runs the three
 *        stages in order; every skip is counted and traced with its cause and ends the
 *        satellite.
 * @param v          Station-epoch handles.
 * @param x_lin      The station's linearization point.
 * @param pb_state   Phase-bias state indices by (satellite, signal).
 * @param cb_state   Code-bias state indices by (satellite, signal).
 * @param cand       Ambiguity candidates by (satellite, band).
 * @param o          The satellite's observation.
 * @param by_const   Resolved arcs by constellation letter (appended to).
 */
void sdEwlResolveSat(const SdEwlRowView& v, const Eigen::VectorXd& x_lin,
                     const std::map<std::pair<std::string, std::string>, int>& pb_state,
                     const std::map<std::pair<std::string, std::string>, int>& cb_state,
                     const SdEwlCandidates& cand, const PppObservation& o,
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
 *        the four shared bias columns, and the arc's half of the linearization. A missing
 *        shared column skips the satellite; otherwise the arc is appended to @p by_const.
 * @param v          Station-epoch handles.
 * @param x_lin      The station's linearization point.
 * @param pb_state   Phase-bias state indices by (satellite, signal).
 * @param cb_state   Code-bias state indices by (satellite, signal).
 * @param o          The satellite's observation.
 * @param cons       The satellite's constellation letter.
 * @param f1         Lower band frequency, Hz.
 * @param f2         Upper band frequency, Hz.
 * @param ps         The pair-search state.
 * @param by_const   Resolved arcs by constellation letter (appended to).
 */
void sdEwlBuildArc(const SdEwlRowView& v, const Eigen::VectorXd& x_lin,
                   const std::map<std::pair<std::string, std::string>, int>& pb_state,
                   const std::map<std::pair<std::string, std::string>, int>& cb_state,
                   const PppObservation& o, char cons, double f1, double f2,
                   const SdEwlPairSearch& ps, std::map<char, std::vector<SdEwlArc>>& by_const);

/**
 * @brief Builds one whitened group of rows against the group's pivot arc: gates each
 *        row, caps the count, appends the design, right-hand side and tags to the block,
 *        and traces every outcome. Unwinds the append if the factorization fails.
 */
void sdEwlEmitGroup(const SdEwlRowView& v, const std::vector<const SdEwlArc*>& g,
                    bool fixed_group);

/**
 * @brief Appends one group's rows to the station block. The rows share the pivot arc's
 *        noise, so R = diag(var_s) + var_p * 11^T; they are whitened by L^-1 into
 *        unit-information rows and the append is unwound if the factorization fails.
 */
void sdEwlAppendRows(const SdEwlRowView& v, const SdEwlArc& p, const std::vector<SdEwlRow>& rows,
                     bool fixed_group);

/**
 * @brief Partitions each constellation's arcs into the fixed and float groups (one group
 *        when the integer path is off) and emits them. The pivot is the most mature arc,
 *        ties by satellite name, so the row set is deterministic.
 */
void sdEwlBuildGroups(const SdEwlRowView& v, std::map<char, std::vector<SdEwlArc>>& by_const);

} // namespace GnssProcessor
