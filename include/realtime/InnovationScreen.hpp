/**
 * @file InnovationScreen.hpp
 * @brief Pre-update screen of the consumer's measurement innovations against the
 *        predicted state: the statistic, the sequential removal, the exclusion
 *        episodes and their counters.
 *
 * ## Why a screen before the update
 *
 * The Kalman update spreads one gross row over every state the row touches. A
 * 92 m phase step on one satellite moved the first update pass 704 m and pushed all
 * 36 standardized residuals over the post-fit threshold, so the post-fit peel then
 * reset innocent satellites and the epoch committed nothing. The post-fit residuals
 * are computed after the state has absorbed the error, which is what makes them
 * poor witnesses. The innovations are computed against the PREVIOUS state, before
 * the update, so a gross row shows only in its own innovation. This class tests
 * those innovations while the previous state is still unspoiled.
 *
 * ## The statistic
 *
 * With H the design matrix, P the predicted covariance and R the measurement noise,
 * the innovation vector nu = z - h(x) has covariance
 *
 *     S = H P H^T + R
 *
 * under the model. Row i is tested against what all the other active rows imply for
 * it:
 *
 *     w_i = (S^-1 nu)_i / sqrt((S^-1)_ii)
 *
 * which is the innovation of row i minus its prediction from the other rows, divided
 * by the standard deviation of that difference. Without a fault w_i is standard
 * normal. A gross error of size d on row i alone shifts w_i by d / sigma_i, with
 * sigma_i = 1 / sqrt((S^-1)_ii) the conditional standard deviation of row i.
 *
 * The marginal form nu_i / sqrt(S_ii) is not used. A receiver clock with random-walk
 * noise of hundreds of square metres per epoch, and a position that is allowed to
 * move, put tens of metres into every S_ii, so a marginal test cannot see a fault
 * below that size. The conditional form lets the other rows determine the common
 * clock, geometry and troposphere first, and tests only the part of row i they do
 * not explain. A row whose own state is loose (a new ambiguity) has a large
 * conditional standard deviation and cannot trip, which is correct: it carries no
 * prior to contradict.
 *
 * ## Sequential removal
 *
 * Two gross rows can hide each other in the one-pass statistic. The screen removes
 * the eligible row with the largest |w| when it exceeds the threshold k, updates
 * S^-1 and S^-1 nu by the rank-one downdate for the removed row, and repeats until
 * no eligible row exceeds k. This is the usual single-outlier snooping applied
 * one row at a time, on the prior rather than on the post-fit residuals.
 *
 * ## When the screen abstains
 *
 * A screen that excludes most of an epoch has decided that the prior is wrong, not
 * the rows. It then abstains: nothing is excluded, the epoch goes to the update and
 * the post-fit machinery as it would without the screen, and the epoch is counted
 * with its reason (too many rows flagged, too few rows to test, numerical failure).
 *
 * ## Exclusion episodes (the two edges of an exclusion)
 *
 * An exclusion decouples a row's information from the states. The decoupling edge is
 * the event Exclude (key, |w|, conditional sigma, innovation, streak). Every epoch
 * the screen tests every row again, including those it excluded the epoch before,
 * so the re-coupling edge is a re-validation, not a timer:
 *
 *  - Readmit: the key passes the test again; its row enters the update.
 *  - Refloat: a phase key has been flagged on @c refloat_after_ep consecutive epochs.
 *    A persistent step cannot heal while its ambiguity is held out of the update,
 *    so the ambiguity is re-floated (the same reset path as a first sighting) and the
 *    row enters the update that same epoch with a fresh ambiguity. A code row has no
 *    state to heal; it stays excluded for as long as it fails.
 *  - Lost: the key left the epoch's row set while excluded.
 *  - Released: the screen abstained, so every open exclusion ends with the rows in
 *    the update.
 *
 * Closure (a run-end identity): episodes opened = readmitted + refloated + lost +
 * released + still open; and every screened epoch with eligible rows is exactly one
 * of clean, flagged or abstained.
 *
 * ## Modes
 *
 * @c Mode::Act applies the decisions. @c Mode::Observe computes the same statistics,
 * episodes and counters and changes nothing, so the false-flag rate on a healthy
 * run and the distribution of the epoch's largest |w| can be read before the
 * screen is allowed to act.
 */
#pragma once

#include <Eigen/Dense>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Which rows the screen may exclude. Rows of other kinds take no part in the screen.
enum class InnovationScreenRows {
    All,     ///< Code and carrier phase rows.
    Phase,   ///< Carrier phase rows only.
    Code     ///< Code rows only.
};

/// Screens the innovations of one consumer epoch and keeps the exclusion episodes.
class InnovationScreen {
public:
    /// What a flagged row does to the epoch.
    enum class Mode {
        Act,       ///< Excluded rows leave the update; refloats are applied.
        Observe    ///< Everything is computed and counted; nothing is written.
    };

    /// Why an epoch was not screened.
    enum class Abstain {
        None,        ///< The epoch was screened.
        TooMany,     ///< More rows were flagged than the allowed fraction of the epoch.
        TooFew,      ///< Too few rows would remain for the statistic to mean anything.
        Numerical    ///< S was not positive definite or not finite.
    };

    /// The resolved knobs.
    struct Config {
        double k{ 6.0 };                 ///< A row is flagged when |w| exceeds k.
        Mode   mode{ Mode::Act };        ///< Act or Observe.
        InnovationScreenRows rows{ InnovationScreenRows::All };  ///< Which rows may be excluded.
        double max_fraction{ 0.25 };     ///< Abstain when more than this fraction of the epoch's rows is flagged.
        int    refloat_after_ep{ 3 };    ///< Consecutive flagged epochs after which a phase key is re-floated.
        int    min_rows{ 6 };            ///< Abstain when fewer rows than this would remain active.
    };

    /// One row as the screen sees it.
    struct Row {
        std::string sat;        ///< Satellite of the row.
        std::string signal;     ///< Observation code of the row (for example "L1W" or "C1C").
        bool        eligible{ true };   ///< Takes part in the screen and may be excluded; a false row is marginalized out of S, so a fault in it cannot make an eligible row look wrong.
        /// True for a carrier phase row.
        bool phase() const { return !signal.empty() && signal[0] == 'L'; }
        /// The exclusion key the measurement model reads: "<sat>_<signal>".
        std::string key() const { return sat + "_" + signal; }
    };

    /// What happened to one key.
    enum class EventKind {
        Exclude,    ///< Flagged: the row leaves the update (act) or would (observe).
        Readmit,    ///< An excluded key passed the test again.
        Refloat,    ///< A phase key reached the streak limit and its ambiguity is re-floated.
        Lost,       ///< An excluded key left the epoch's row set.
        Released    ///< The screen abstained; the open exclusion ended with the row in the update.
    };

    /// One decoupling or re-coupling event with its identity.
    struct Event {
        EventKind   kind{ EventKind::Exclude };
        std::string sat;
        std::string signal;
        double      w{ 0.0 };            ///< Conditional statistic at the event (0 for Lost and Released).
        double      sigma{ 0.0 };        ///< Conditional standard deviation of the row (m).
        double      innovation{ 0.0 };   ///< Innovation of the row (m).
        int         streak{ 0 };         ///< Consecutive flagged epochs including this one; for Readmit, Lost and Released the length that just ended.
    };

    /// The decisions for one epoch.
    struct Outcome {
        Abstain abstain{ Abstain::None };
        bool    acted{ false };              ///< Mode was Act, so @c exclude and @c refloat are to be applied.
        int     flagged_before_abstain{ 0 }; ///< Rows removed before the screen gave up (TooMany and TooFew).
        double  max_abs_w{ 0.0 };            ///< Largest |w| over the eligible rows of the first, full-set pass.
        std::vector<std::string> exclude;                            ///< Keys ("<sat>_<signal>") to hold out of the update; empty unless acted.
        std::vector<std::pair<std::string, std::string>> refloat;    ///< (sat, signal) of the ambiguities to re-float; empty unless acted.
        std::vector<Event> events;                                   ///< Every event of the epoch, in the order they were decided.
        /// True when the update must be built again.
        bool changesUpdate() const { return !exclude.empty() || !refloat.empty(); }
    };

    /// Leave-one-out statistics of the full row set.
    struct Statistics {
        bool            ok{ false };   ///< S was symmetric positive definite and finite.
        Eigen::MatrixXd T;             ///< S^-1.
        Eigen::VectorXd g;             ///< S^-1 nu.
        Eigen::VectorXd w;             ///< w_i = g_i / sqrt(T_ii).
        Eigen::VectorXd sigma;         ///< Conditional standard deviation 1 / sqrt(T_ii) (m).
    };

    /// Number of buckets of the epoch-maximum histogram.
    static constexpr int kHistBuckets = 8;

    /// Run-end counters (the ON-path telemetry).
    struct Counters {
        uint64_t epochs{ 0 };            ///< Epochs with at least one eligible row.
        uint64_t rows{ 0 };              ///< Eligible rows tested over those epochs.
        uint64_t epochs_flagged{ 0 };    ///< Epochs with at least one flagged row.
        uint64_t flagged{ 0 };           ///< Flagged (row, epoch) pairs; excluded in act mode, would-be in observe.
        uint64_t flagged_phase{ 0 };     ///< Of those, carrier phase rows.
        uint64_t flagged_code{ 0 };      ///< Of those, code rows.
        uint64_t episodes{ 0 };          ///< Exclusion episodes opened.
        uint64_t readmitted{ 0 };        ///< Episodes closed by the key passing again.
        uint64_t refloated{ 0 };         ///< Episodes closed by a refloat.
        uint64_t lost{ 0 };              ///< Episodes closed by the key leaving the row set.
        uint64_t released{ 0 };          ///< Episodes closed by an abstaining epoch.
        uint64_t abstain_many{ 0 };      ///< Epochs abstained for Abstain::TooMany.
        uint64_t abstain_few{ 0 };       ///< Epochs abstained for Abstain::TooFew.
        uint64_t abstain_numeric{ 0 };   ///< Epochs abstained for Abstain::Numerical.
        uint64_t skipped{ 0 };           ///< Calls with no eligible row, or inputs of inconsistent size.
        double   max_abs_w{ 0.0 };       ///< Largest |w| of any flagged row.
        int      max_streak{ 0 };        ///< Longest run of consecutive flagged epochs of one key.
        /// Epochs by their largest |w| over the eligible rows:
        /// below 2, 2 to 3, 3 to 4, 4 to 5, 5 to 6, 6 to 10, 10 to 100, 100 and above.
        std::array<uint64_t, kHistBuckets> hist{};

        /// Epochs that abstained, all reasons.
        uint64_t abstained() const { return abstain_many + abstain_few + abstain_numeric; }
        /// Exclusion episodes still open.
        uint64_t open() const { return episodes - readmitted - refloated - lost - released; }
        /// Episode and epoch closure; false only after a bookkeeping bug.
        bool closureHolds() const {
            return episodes >= readmitted + refloated + lost + released
                && epochs >= epochs_flagged + abstained();
        }
    };

    /// Set the knobs. Does not clear the counters or the open episodes.
    void configure(const Config& c) { cfg_ = c; }
    /// The knobs in force.
    const Config& config() const { return cfg_; }
    /// Run-end counters.
    const Counters& counters() const { return counters_; }
    /// Number of exclusion episodes currently open.
    size_t openEpisodes() const { return open_.size(); }

    /**
     * @brief Screen one epoch.
     *
     * @param S     Innovation covariance H P H^T + R of all @p rows (m by m).
     * @param nu    Innovation vector (m).
     * @param rows  Identity and eligibility of each row, aligned with @p nu. Only the
     *              eligible rows are tested; the others are dropped from S and nu first.
     * @return The epoch's decisions. Updates the counters and the exclusion episodes.
     */
    Outcome evaluate(const Eigen::MatrixXd& S, const Eigen::VectorXd& nu,
                     const std::vector<Row>& rows);

    /**
     * @brief The leave-one-out statistics of a full row set.
     *
     * Exposed for the unit tests and for read-only diagnostics; @ref evaluate calls
     * it once per epoch and then works on the downdated copies.
     *
     * @param S   Innovation covariance (m by m); symmetrized internally.
     * @param nu  Innovation vector (m).
     * @return The statistics; @c ok is false when S is not usable.
     */
    static Statistics leaveOneOut(const Eigen::MatrixXd& S, const Eigen::VectorXd& nu);

    /// Name of an abstain reason, for logs.
    static const char* abstainName(Abstain a);
    /// Name of an event kind, for logs.
    static const char* eventName(EventKind k);

private:
    /// An open exclusion episode.
    struct Open {
        int         streak{ 0 };   ///< Consecutive flagged epochs so far.
        std::string sat;
        std::string signal;
    };

    Config   cfg_;
    Counters counters_;
    std::map<std::string, Open> open_;   ///< Keyed by the exclusion key.

    /// Close every open episode because the epoch abstained.
    void releaseAll(Outcome& out);
};

} // namespace GnssProcessor
