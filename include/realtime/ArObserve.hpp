/**
 * @file ArObserve.hpp
 * @brief The consumer's observe-mode integer search: what it forms, what it reports and how
 *        a report is written to a trace.  Plain value types and free functions; the engine
 *        side (which candidates, which gate, where the output is kept) is
 *        @c RealTimeEngine::observeIntegerSearch_ (RealTimeEngineArObserve.cpp).
 *
 * ## What the search is for
 *
 * A held-out consumer that floats every ambiguity has a float position and nothing to say
 * about whether the network's phase biases let it fix integers.  The observe mode answers
 * that without changing the consumer: each epoch, from the float it already has, it forms
 * the between-satellite single-difference (SD) ambiguities, searches them for integers
 * and reports what it found, one row per SD ambiguity and one per satellite pair's wide
 * lane, plus the position the fixed integers would give.  It writes nothing back to the
 * filter.
 *
 * ## The SD ambiguities
 *
 * Candidates are grouped by (constellation, signal), and by chain component as well when
 * the wire's components are used (@ref GnssProcessor::ArObserveOptions::split_by_component "ArObserveOptions::split_by_component").  The
 * highest-elevation member of a group is its reference and every other member forms one SD
 * with it.  With D the SD rows on the state (+1 on the satellite, -1 on the reference) and
 * lambda the carrier wavelength of the satellite's signal:
 *
 *     a_hat = D x / lambda            (cycles)
 *     Q     = D P D^T / (lambda lambda^T)   (cycles^2)
 *     Q_xa  = P_pos,. D^T / lambda     (m cycles^-1, position rows only)
 *
 * One joint search runs over every SD of the epoch.
 *
 * ## The two searches
 *
 *  - Full: @c ils::ilsFix on (a_hat, Q).  The accepted fix (the whole set, or the
 *    subset K of partial AR) is conditioned on exactly:
 *
 *        x_c = x_pos - Q_xK Q_KK^-1 (a_K - afix_K)
 *
 *    A row is determined when its SD is kept; kept entries have conditional sigma 0.
 *  - Decorrelated: @c ils::ilsFixDecorrelatedPartial.  It fixes the best-determined
 *    decorrelated components and returns the floats conditioned on them (a_cond, Q_cond).
 *    The position conditioned on those components is
 *
 *        x_c = x_pos + Q_xa Q_aa^-1 (a_cond - a_hat)
 *
 *    A row is determined when its conditional sigma is at most the bar
 *    (@ref GnssProcessor::ArObserveOptions::determined_sigma_cyc "ArObserveOptions::determined_sigma_cyc") and kept when the search lists it.
 *
 * The gates are the five of the producer's search, built by the same assignment, so the
 * same numbers make the same call.
 *
 * ## The wide-lane rows
 *
 * A satellite pair that has an SD in each of its constellation's two lowest bands (for
 * example GPS L1C and L2W, against one reference) also gets a row with the signal "WL":
 * its float is SD_b1 - SD_b2 and its conditional value and sigma come from the same
 * conditional floats and covariance.  It is determined at a conditional sigma at or below
 * the bar and kept when its value lies within the fraction cap of an integer.  The row is
 * judged on its own conditional sigma, so a wide lane that is precise in the float can be
 * determined when the search fixed nothing.
 *
 * ## When the search is not run
 *
 * The integer search of an unconverged float can take unbounded time (the ambiguity
 * covariance is wide in many directions and the enumeration grows with it), which is why the
 * resolver waits for a 3D position sigma below a bar.  The observe mode waits for a bar too
 * (@ref GnssProcessor::ArObserveOptions::max_position_sigma_m "ArObserveOptions::max_position_sigma_m"): above it an epoch records its float position,
 * forms no rows and runs no search, and is counted as gated.
 *
 * ## Measure-only
 *
 * Nothing here touches a filter.  The caller hands in a copy of the state and covariance
 * and keeps the output.
 */
#pragma once

#include "filter/IlsCore.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkSolutionEpoch;   // products/NetworkProductEmitter.hpp

// ===========================================================================
// What the wire declares about its integer frame
// ===========================================================================

/// The S line's amb_datum value of the single-receiver chain datum
/// (@c SbasisDeclaration::AmbDatum::SingleReceiverChain), the only phase datum whose
/// satellite phase biases share an integer frame within a component.
inline constexpr int kWireAmbDatumChain = 2;

/**
 * @brief The chain declarations of the wire the consumer reads: the phase datum and, per
 *        (satellite, signal), the chain component.
 *
 * The consumer's gate (@c network.require_signal_integer_for_ar) admits an ambiguity into the
 * observe-mode search only when the wire declares the chain datum and gives the
 * ambiguity's phase bias a component, and the search pairs satellites within one component.
 * The context is set by whoever feeds the consumer's store: the replay driver from the product
 * file, the in-process run from the producer's contract.
 */
struct WireChainContext {
    /// The S line's amb_datum (0 pivot receiver, 1 sum to zero, @ref kWireAmbDatumChain);
    /// -1 when the wire declares none (a product file without an S line, a consumer that
    /// was never told).
    int amb_datum{ -1 };
    /// "<sat>|<signal>" -> the P record's @c cmp= component; a key without an entry has no token.
    std::map<std::string, std::uint32_t> cmp;
};

/**
 * @brief Fold one product epoch's chain components into a context.
 *
 * For every satellite of @p epoch the context's entries of that satellite are replaced by the
 * epoch's phase-bias records that carry a component (a record without a token leaves no entry),
 * which mirrors the wholesale replacement of a satellite's phase biases in the store.  A
 * satellite the epoch does not name keeps its entries.  @c amb_datum is untouched.
 *
 * @param ctx   The context to update.
 * @param epoch The product epoch the store was just filled from.
 */
void absorbWireChain(WireChainContext& ctx, const NetworkSolutionEpoch& epoch);

// ===========================================================================
// The search
// ===========================================================================

/// Which integer search the observe mode runs (@c network.closed_loop.ar_search).
enum class ArSearchMode {
    Full,                ///< @c ils::ilsFix, one accepted fix, conditioned on the kept set.
    DecorrelatedPartial  ///< @c ils::ilsFixDecorrelatedPartial, conditioned on the fixed components.
};

/// @brief The mode a search name selects.
/// @param name "full" or "decorrelated_partial"; anything else reads as Full (the loader refuses it).
/// @return The mode.
ArSearchMode arSearchModeOf(const std::string& name);

/// @brief The name of a mode, as the configuration spells it.
/// @param mode The mode.
/// @return "full" or "decorrelated_partial".
const char* arSearchModeName(ArSearchMode mode);

/// One ambiguity state that may enter the search (after the caller's exclusions and gate).
struct ArCandidate {
    std::string   sat;               ///< RINEX satellite id, e.g. "G05".
    std::string   signal;            ///< Raw phase signal, e.g. "L1C" (never an iono-free key).
    int           state_idx{ -1 };   ///< Index of the ambiguity in the state vector (metres).
    double        elevation_deg{ 0.0 };  ///< Satellite elevation; picks the reference of a group.
    double        lambda_m{ 0.0 };   ///< Carrier wavelength of the signal as the measurement model uses it (m).
    std::uint32_t cmp{ 0 };          ///< Chain component of the (satellite, signal) phase bias; 0 = none.
    std::uint32_t hold_generation{ 0 };  ///< The store entry's hold generation (@c gen=); 0 = unheld.
};

/// What the search is run with.
struct ArObserveOptions {
    ArSearchMode       mode{ ArSearchMode::Full };  ///< The integer search.
    ils::IlsGateConfig gates;                       ///< The acceptance gates (ratio, upper bound, bootstrap rate, fraction cap, partial AR).
    double             determined_sigma_cyc{ 0.05 };///< Conditional sigma at or below which a row is determined.
    bool               split_by_component{ false }; ///< Group by chain component as well as by (constellation, signal).
    int                position_index{ 0 };         ///< Index of the first of the three position states.
    /// Skip the integer search when the 3D position sigma, sqrt(P_xx + P_yy + P_zz), exceeds
    /// this (m); 0 disables the gate.  The engine passes the larger of the resolver's bar and 0.20 m.
    double             max_position_sigma_m{ 0.0 };
};

/// The single-difference problem of an epoch before it is searched.
struct ArSdProblem {
    std::vector<int> cand;   ///< Per SD: index (into the candidates passed in) of the differenced satellite.
    std::vector<int> ref;    ///< Per SD: index of its group's reference.
    Eigen::VectorXd  a;      ///< The SD floats, D x / lambda (cycles).
    Eigen::MatrixXd  Q;      ///< Their covariance, D P D^T / (lambda lambda^T) (cycles^2), symmetrized.
    Eigen::MatrixXd  Qxa;    ///< Position-to-SD cross-covariance, P_pos,. D^T / lambda (3 x n, m per cycle); zero without a position.
};

/**
 * @brief Form the SD ambiguities of an epoch: group, pick the references, build the float
 *        and its covariance.
 *
 * Candidates with an out-of-range state index or a non-positive wavelength are left out.
 * The groups are (constellation letter, signal), and also the chain component when
 * @ref GnssProcessor::ArObserveOptions::split_by_component "ArObserveOptions::split_by_component" is set; a group of one forms nothing.  The
 * reference of a group is its highest-elevation member (the first of equals in satellite
 * order); the SDs run in group order, then satellite order, so the output is deterministic.
 *
 * @param x          State vector.
 * @param P          State covariance.
 * @param candidates The ambiguity states that may enter.
 * @param opt        Search settings (grouping and the position index are read).
 * @return The problem; empty (zero SDs) when no group has two members.
 */
ArSdProblem formArSdProblem(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                            const std::vector<ArCandidate>& candidates,
                            const ArObserveOptions& opt);

/// One row of the search: a single-difference ambiguity of one signal, or a satellite
/// pair's wide lane (signal "WL").
struct ArFixRow {
    char          system{ '?' };    ///< RINEX constellation letter of the satellite.
    std::string   signal;           ///< "L1C", "L2W", ...; "WL" for a pair's wide lane.
    std::string   sat;              ///< The satellite of the difference.
    std::string   ref_sat;          ///< The reference it is differenced against.
    std::uint32_t cmp{ 0 };         ///< Chain component of the satellite's phase bias (0 = none).
    double        float_cyc{ 0.0 };         ///< The float value (cycles); for "WL" the difference of the two bands.
    double        cond_sigma_cyc{ 0.0 };    ///< Conditional sigma (cycles) given what the search fixed; 0 for a kept SD of the full search.
    long long     fixed_int{ 0 };           ///< Nearest integer to the conditional value.
    bool          determined{ false };      ///< The integer is determined (see the file banner).
    bool          kept{ false };            ///< The search accepted this integer (for "WL": determined and within the fraction cap).
    std::uint32_t gen_sat{ 0 };     ///< Hold generation of the satellite's phase bias (the lowest band for "WL"); 0 = unheld.
    std::uint32_t gen_ref{ 0 };     ///< Hold generation of the reference's phase bias.
};

/// The epoch's search outcome and positions.
struct ArEpochSummary {
    int    n_entries{ 0 };   ///< SD band rows formed (searched, unless @ref gated).
    int    n_pairs{ 0 };     ///< Distinct satellite pairs searched (a pair with two bands counts once); 0 when gated.
    bool   gated{ false };   ///< The position-sigma gate closed: no rows, no search, @ref fix_ecef equals @ref float_ecef.
    int    n_wl_det{ 0 };    ///< Wide-lane rows determined and kept.
    int    n_band_det{ 0 };  ///< Band rows determined and kept.
    int    z_fixed{ 0 };     ///< Components the search fixed (the full search: the kept count).
    int    z_total{ 0 };     ///< Components searched (the SD count).
    double ratio{ 0.0 };     ///< Ratio of the accepted fix; the attempt's ratio when none was accepted.
    double p_s{ 0.0 };       ///< Bootstrap success rate of the accepted fix; the attempt's when none was accepted.
    bool   accepted{ false };     ///< The search fixed something (a kept SD, or a fixed component).
    bool   conditioned{ false };  ///< @ref fix_ecef is the position conditioned on the fix (false = equal to the float).
    bool   cond_failed{ false };  ///< A conditioning solve failed; the position was left at the float.
    Eigen::Vector3d float_ecef{ Eigen::Vector3d::Zero() };  ///< The float ECEF position (m).
    Eigen::Vector3d fix_ecef{ Eigen::Vector3d::Zero() };    ///< The position conditioned on the fix (m).
};

/// One epoch of the observe-mode search.
struct ArObserveEpoch {
    GnssTime              time{ 0, 0.0 };  ///< The consumer epoch it describes.
    bool                  valid{ false };  ///< The search stage ran (an update was applied), even if it formed no SD or the position-sigma gate closed.
    std::vector<ArFixRow> rows;            ///< Band rows in group order, then the wide-lane rows.
    ArEpochSummary        summary;         ///< Counts, ratio and positions.
    ils::IlsResult        fix;             ///< The integer search's own result, for audits.
};

/**
 * @brief Form the SD ambiguities of an epoch, search them and report.
 *
 * @param x          State vector (metres for the ambiguities, ECEF metres for the position).
 * @param P          State covariance.
 * @param candidates The ambiguity states that may enter, after the caller's exclusions and gate.
 * @param opt        Search settings.
 * @return The epoch: a row per SD and per wide lane, the summary and the integer search's own
 *         result.  With no SD to form, @c valid is true, @c rows is empty and
 *         @c summary.fix_ecef equals @c summary.float_ecef.  Never throws.
 */
ArObserveEpoch observeIntegerSearch(const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                    const std::vector<ArCandidate>& candidates,
                                    const ArObserveOptions& opt);

// ===========================================================================
// Run totals
// ===========================================================================

/// Totals of a run, the on-path telemetry of the instrument.  All zero, and never printed,
/// with the mode off.
struct ArObserveCounters {
    long long epochs{ 0 };           ///< Epochs whose search stage ran (an update was applied).
    long long epochs_gated{ 0 };     ///< ...of which the position-sigma gate closed on (formed SDs, ran no search).
    long long epochs_searched{ 0 };  ///< ...of which ran the integer search (at least one SD, gate open).
    long long epochs_accepted{ 0 };  ///< ...of which the search fixed something.
    long long candidates{ 0 };       ///< Ambiguity states that reached the gate.
    long long eligible{ 0 };         ///< ...that the gate admitted (all of them with the gate off).
    long long gated_out{ 0 };        ///< ...that the gate refused.
    long long sd_pairs{ 0 };         ///< Satellite pairs formed.
    long long band_rows{ 0 };        ///< SD band rows searched.
    long long wl_rows{ 0 };          ///< Wide-lane rows formed.
    long long determined{ 0 };       ///< Rows (band and wide lane) determined.
    long long kept{ 0 };             ///< Rows (band and wide lane) determined and kept.
    long long band_kept{ 0 };        ///< ...band rows among them.
    long long wl_kept{ 0 };          ///< ...wide-lane rows among them.
    long long cond_failed{ 0 };      ///< Epochs whose conditioning solve failed.

    /**
     * @brief The run-end identity: every candidate was admitted or refused, and a kept row is a
     *        determined row of a searched epoch.
     * @return True when the counters satisfy the identity (the epoch, candidate and row
     *         counts are mutually consistent); false flags an accounting defect.
     */
    bool closureHolds() const {
        return candidates == eligible + gated_out && kept <= determined
            && kept == band_kept + wl_kept && epochs_accepted <= epochs_searched
            && epochs_searched + epochs_gated <= epochs;
    }
};

/**
 * @brief Add one epoch to the run totals.
 *
 * @param counters     The totals.
 * @param epoch        The epoch's output (ignored unless @c valid).
 * @param n_candidates Ambiguity states that reached the gate this epoch.
 * @param n_eligible   ...that the gate admitted.
 */
void accumulateArObserve(ArObserveCounters& counters, const ArObserveEpoch& epoch,
                         long long n_candidates, long long n_eligible);

/**
 * @brief The run-end line of the instrument.
 *
 * @param counters The run's totals.
 * @param tag      Where the line comes from ("closed loop", "end of replay").
 * @param search   The search's name.
 * @param sigma    The determined bar (cycles).
 * @param gate     Whether the chain gate was on.
 * @return One line, without a newline.
 */
std::string formatArObserveSummary(const ArObserveCounters& counters, const std::string& tag,
                                   const std::string& search, double sigma, bool gate);

// ===========================================================================
// The two trace files
// ===========================================================================

/// Header of @c logs/closed_loop_ar_fixes.csv.  The consumer grader checks these columns
/// (scripts/consumer_ar_grade.py, FIX_HEADER).
inline constexpr const char* kArFixTraceHeader =
    "week,tow,system,signal,sat,ref_sat,cmp,float_cyc,cond_sigma_cyc,fixed_int,determined,kept,"
    "z_fixed,z_total,ratio,p_s,gen_sat,gen_ref\n";

/// Header of @c logs/closed_loop_ar_epoch.csv: the epoch's counts and the ENU errors of
/// the conditioned (fix_) and the float (float_) position.
inline constexpr const char* kArEpochTraceHeader =
    "week,tow,n_pairs,n_wl_det,n_band_det,fix_e,fix_n,fix_u,float_e,float_n,float_u\n";

/// East, north and up errors (m) of an epoch's two positions against a truth position.
struct ArEpochErrors {
    Eigen::Vector3d fix_enu{ Eigen::Vector3d::Zero() };    ///< Conditioned position minus truth.
    Eigen::Vector3d float_enu{ Eigen::Vector3d::Zero() };  ///< Float position minus truth.
};

/**
 * @brief The ENU errors of an epoch's positions.
 *
 * @param summary   The epoch's summary.
 * @param truth     Truth ECEF position (the antenna reference point the consumer estimates).
 * @param truth_llh The truth in geodetic coordinates (the ENU frame).
 * @return The errors.
 */
ArEpochErrors arEpochErrors(const ArEpochSummary& summary, const Eigen::Vector3d& truth,
                            const Coordinates::Llh& truth_llh);

/**
 * @brief One row of @c closed_loop_ar_fixes.csv.
 *
 * The epoch-wide columns (z_fixed, z_total, ratio, p_s) repeat on every row of the epoch.
 *
 * @param week GPS week of the consumer epoch.
 * @param tow  Time of week (s), written to 3 dp.
 * @param row  The row.
 * @param s    The epoch's summary.
 * @return The CSV line, newline-terminated.
 */
std::string formatArFixRow(int week, double tow, const ArFixRow& row, const ArEpochSummary& s);

/**
 * @brief The row of @c closed_loop_ar_epoch.csv.
 *
 * @param week GPS week of the consumer epoch.
 * @param tow  Time of week (s), written to 3 dp.
 * @param s    The epoch's summary.
 * @param err  The ENU errors of its two positions (written to 4 dp).
 * @return The CSV line, newline-terminated.
 */
std::string formatArEpochRow(int week, double tow, const ArEpochSummary& s,
                             const ArEpochErrors& err);

} // namespace GnssProcessor
