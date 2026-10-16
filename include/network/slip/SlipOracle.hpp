#pragma once
/**
 * @file SlipOracle.hpp
 * @brief Sim-only oracle slip response: the measured ceiling of a
 *        level-preserving re-level (`network.cycle_slip.oracle_relevel`).
 *
 * ## Purpose
 *
 * In the producer's ordinary slip response a detector fires, the arc is freed,
 * and the PASS-B leveling inject re-levels it from that one epoch's states, which
 * the slip has already corrupted.  The truth-graded slip census on the slipped
 * 60-station sim bed (fixture `runs/simnet60_campaign_wu93`, 2 452 truth slips in
 * 6 h) attributes a level error to this re-level of about 2.5x the bed's ambient
 * DD error (|e| 5.6 -> 6.7 cyc), decaying over about 20 min.  The T4 attribution
 * tier then erases the affected arcs again, and the injections drain into a
 * metre-class ZWD plateau (truth rms 2.10 m against 7 mm on the quiet twin).
 * This is the re-coupling edge of standing invariant #1: the response's own
 * re-level is the defect.
 *
 * The engineering answer (EWL / WL-MW integer estimation of the jump,
 * SPEC_lane_slip_response) is expensive to build, so this oracle measures its
 * ceiling first.  When a detector actuates a refloat on a carried arc, the oracle
 * looks up the true integer jump(s) in the fixture's `truth_slips.csv` and shifts
 * the carried level by the sum d*lambda (plus the optional injected noise)
 * instead of freeing the arc.  The detector population, the epoch ordering, the
 * events and the rest of the QC stack are unchanged.  If the ZWD plateau does not
 * collapse under a perfect repair, level preservation is falsified as the lever
 * and the engineering card is not worth building.
 *
 * ## Field-inert by construction
 *
 * The knob needs a truth root, which a field lineage does not have.  The loader
 * raises a hard error when `oracle_relevel` is armed and
 * `network.cycle_slip.oracle_truth_root` does not exist, and setup raises one
 * when any roster station lacks its `truth_slips.csv`.  This is a diagnostic
 * knob: it is not for promotion and does not belong in a golden config.
 *
 * ## Where it actuates, and where it does not
 *
 * One helper (@ref networkSlipOracleHandle) serves the detector refloat
 * actuation sites in network/slip/SlipQc.cpp, which cover the four detector tiers:
 *   - `refloat_signal` (both the MW confident-wide-lane drain and the RINEX-LLI
 *     drain go through it),
 *   - `refloat_pair`   (the IF-phase triple-difference driver, both bands),
 *   - `networkSlipDrainWindupRestart` (whole satellite, every signal).
 * `networkSlipDrainGapReentry` is a different response (a break the detectors
 * cannot cover) and is left alone, as is the T4 RAIM-attribution erase path in
 * network/qc/Raim.cpp: the census question is what a perfect detector
 * response would buy with every other mechanism held fixed.
 *
 * When the helper returns true the caller skips every freeing action for that
 * arc: no `free_ambs` latch, no `carry_slipped_ambs` funnel insert, no
 * pending-reseed mark, no bridge-pend void, no erase.  The site's event row is
 * still emitted, with action `oracle_repair` / `oracle_keep`, so the trace
 * population is comparable to the control arm's.
 *
 * ## The window: a detector-latency bound, floored at the carry's birth
 *
 * A repair applies the jumps that the carried level does not already contain.
 * The window is
 *
 *     ( max(last_applied_s, carry.birth_s, now_s − window_s), now_s ]
 *
 * with the lower bound exclusive and the upper bound inclusive.  The three terms:
 *
 * - `now_s − window_s` (@ref NetworkSlipOracle::window_s,
 *   `network.cycle_slip.oracle_window_s`, default 90 s = three 30 s epochs) is
 *   the detector-latency bound, and the one that normally binds.  Detectors fire
 *   1–3 epochs after the slip (MW needs its window to fill, IF-TD needs the
 *   triple difference), which is the 90 s within which the truth census counts a
 *   slip as detected.  @ref LocalCarry::last_seen_s is not usable as the lower
 *   bound, although it looks like an "already absorbed" mark: it is the previous
 *   epoch's stash time, and an intervening solve does not re-set a zero-q carried
 *   level (the stiff prior moves little against one epoch of phase), so the jump
 *   is still unabsorbed when a late detector fires.  With that bound the normal
 *   population reads as false alarms (701 kept against 88 repaired on a 60-epoch
 *   smoke), and the measured ceiling would be biased low.
 * - `carry.birth_s` (@ref LocalCarry::birth_s) is the hard floor: the
 *   continuous-GPS second at which the current carry entry was created.  A T4
 *   attribution erase deletes the entry, and the next harvest re-creates it from
 *   a fresh solve whose level already contains any earlier jump, so a truth slip
 *   stamped before the entry's birth is already absorbed, and re-applying it
 *   would inject the error this card is meant to remove.  It is stamped once at
 *   creation and kept across re-stashes, like @ref LocalCarry::birth_ep (which is
 *   not usable here: it is a loop-epoch index, not seconds).  It is not
 *   snapshotted, so a warm-resume-restored entry carries
 *   @ref LocalCarry::kBirthSRestored (−1, SPEC_event_harness §15.4) and the
 *   window falls back to the latency bound alone for the arc's whole restored
 *   life, the conservative direction for a sim-only diagnostic.
 * - `last_applied_s` (@ref ArcState) keeps one arc from being handled twice
 *   before a re-harvest: two detectors firing in one epoch, or on consecutive
 *   epochs, apply the jump once.  It advances on every repair.
 *
 * A window that holds no truth slip is a false alarm (the detector fired on an
 * arc that did not jump within the latency it can see), and
 * @ref NetworkSlipOracle::false_alarm decides the response to it.
 *
 * ## Two independent policies make the census additive
 *
 * @ref NetworkSlipOracle::true_slip decides the other half: an actuation whose
 * window holds a truth slip.  `repair` is the ceiling this card measures; `free`
 * declines the arc so the caller runs the ordinary response for it.  The pair
 * spans the 2x2 the census needs:
 *
 * | `true_slip` | `false_alarm` | what the arm measures |
 * |---|---|---|
 * | `repair` | `keep`   | the ceiling: perfect repair, no false-alarm cost |
 * | `repair` | `free`   | the repair alone, false-alarm cost kept |
 * | `free`   | `keep`   | the false alarms alone: real slips get the ordinary response |
 * | `free`   | `free`   | a no-op (both halves decline): a byte-identity control |
 *
 * The third row is the isolation arm.  If the ZWD sink and the consumer move on
 * it, the detector's false alarms are a material injection source on their own
 * and the detector thresholds need a card of their own; if they do not, the
 * true-slip re-level is the whole lever and only the engineering build (EWL /
 * WL-MW integer estimation) can deliver it.  With `true_slip = free` the run-end
 * `applied` count is 0 by construction, so the armed-and-inert check reads
 * @ref NetworkSlipOracle::fa_kept instead.
 */

#include "network/state/Entities.hpp"   // net::ArcTable (card NOM step N4f1)
#include "network/driver/RunTypes.hpp"   // NetStation, LocalCarry (PppUtils fwd via PppModel)

#include <spdlog/spdlog.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppUtils;

/// What @ref networkSlipOracleHandle did with an arc, for the caller's event row.
enum class OracleOutcome {
    NotHandled,   ///< The caller proceeds with the ordinary response (frees the arc).
    Repaired,     ///< The carried level was shifted by the true sum d*lambda (plus the injected noise).
    Kept          ///< No truth slip in the window; the carry was left untouched.
};

/**
 * @brief Run-scope state for the sim-only oracle slip response.
 *
 * Owns the truth table, the per-arc application ledger, the wavelength provider
 * and the run-end counters.  Constructed only when the knob is armed
 * (@ref networkSlipOracleLoad); the drains receive a `NetworkSlipOracle*` that
 * is null on every other run, so with the knob off no call is reached and the
 * output is byte-identical by construction (standing invariant #6).
 *
 * Non-copyable: a value copy would reset the per-arc ledger and let the same
 * jump be applied twice.
 */
struct NetworkSlipOracle {
    NetworkSlipOracle() = default;
    NetworkSlipOracle(const NetworkSlipOracle&) = delete;
    NetworkSlipOracle& operator=(const NetworkSlipOracle&) = delete;
    ~NetworkSlipOracle();

    /// Response when a detector fires on a carried arc with no truth slip in the
    /// window.  `Keep` is the ceiling (an oracle knows it was a false alarm and
    /// leaves the level alone); `Free` falls through to the ordinary response, so
    /// the arm measures the repair alone and keeps the false-alarm cost.
    enum class FalseAlarm { Keep, Free };

    /// Response when the window holds a truth slip.  `Repair` is the ceiling
    /// (shift the level by the true sum d*lambda); `Free` declines the arc so the
    /// caller runs the ordinary free/re-level response for it, which, paired with
    /// @ref FalseAlarm::Keep, isolates the false-alarm cost.  See the 2x2 in the
    /// file banner.
    enum class TrueSlip { Repair, Free };

    // ---- knobs (snapshot taken at construction) ----------------------------
    std::string truth_root;              ///< `network.cycle_slip.oracle_truth_root`.
    double      noise_cyc{0.0};          ///< `network.cycle_slip.oracle_noise_cyc`: 1-sigma of the deterministic Gaussian added to each applied sum d (cycles).
    FalseAlarm  false_alarm{FalseAlarm::Keep}; ///< `network.cycle_slip.oracle_false_alarm`.
    TrueSlip    true_slip{TrueSlip::Repair};   ///< `network.cycle_slip.oracle_true_slip`.
    double      window_s{90.0};          ///< `network.cycle_slip.oracle_window_s`: detector-latency bound (s) on how far back a truth slip may be repaired; see the file banner.  The lower bound is exclusive, so 0 admits nothing and every actuation reads as a false alarm.

    /// One truth jump: continuous-GPS seconds and the integer jump in that
    /// signal's own cycles (the fixture writes one row per applied jump).
    struct TruthSlip {
        double s{0.0};
        int    delta_cyc{0};
    };
    /// (station index, satellite, signal) -> the arc's jumps, ascending in `s`.
    std::map<std::tuple<int, std::string, std::string>, std::vector<TruthSlip>> truth;

    /// Per-arc application ledger: one of the three terms of the exclusive lower bound.
    struct ArcState { double last_applied_s{-1.0e18}; };
    std::map<std::tuple<int, std::string, std::string>, ArcState> arcs;

    /// The leveling carry this oracle actuates on: `ctx.entities.arcs`.
    /// A bare pointer because the map is a sibling member of the same
    /// @ref NetworkRunContext that owns this object, so it outlives the oracle
    /// and is never reallocated.  The oracle writes @ref LocalCarry::value_abs
    /// and nothing else: an `x_lin` shift is an exact solve no-op
    /// (docs/SPEC_lane_slip_response.md §7.2).
    net::ArcTable* carry{nullptr};   ///< The arc table (its @c ArcState::amb, card NOM step N4f1).

    /// Wavelength provider for the cycles->metres conversion (FDMA-correct via
    /// the satellite id).  Owned here so the oracle does not depend on whether
    /// another card armed the shared raw-observable provider.
    std::unique_ptr<PppUtils> utils;

    // ---- run-end counters (standing invariant #4) --------------------------
    /// Closure:
    /// `seen == applied + true_slip_freed + fa_kept + fa_freed + no_carry + no_lambda`.
    long long seen{0};          ///< (station, sat, signal) actuations offered to the hook.
    long long applied{0};       ///< Arcs repaired (carry shifted by the true sum d*lambda).
    long long multi{0};         ///< …of which the window held >= 2 truth jumps.
    long long true_slip_freed{0}; ///< Truth slip in the window, policy `free`: declined, so the caller ran the ordinary response (the false-alarm-isolation arm).
    long long fa_kept{0};       ///< No truth slip in the window, policy `keep`: carry untouched, arc not freed.
    long long fa_freed{0};      ///< No truth slip in the window, policy `free`: fell through to the ordinary response.
    long long no_carry{0};      ///< No leveling carry entry for the arc: nothing to preserve, fell through.
    long long no_lambda{0};     ///< Wavelength unavailable for the signal: counted and fell through, never skipped.
    double    sum_abs_cyc{0.0}; ///< Sum over applied arcs of |sum d| (cycles).
    double    max_abs_cyc{0.0}; ///< Max over applied arcs of |sum d| (cycles).

    /// Split of @ref fa_kept by detector tier, keyed on the `kCause*` bit the
    /// calling site passes.  A single total cannot say which detector's
    /// thresholds to examine; the four tiers use four different statistics (an
    /// MW window integer, the receiver's own LLI bit, an IF triple difference, a
    /// wind-up accumulator restart), so only a per-tier split can turn "false
    /// alarms cost X" into a card.  Closure: `fa_kept == mw + lli + iftd + wusat +
    /// other`; `other` is expected to be 0 (an unrecognised cause bit is a
    /// mis-wired call site).
    long long fa_kept_mw{0};    ///< …from the MW confident-wide-lane drain (`kCauseMw`).
    long long fa_kept_lli{0};   ///< …from the RINEX-LLI loss-of-lock drain (`kCauseLli`).
    long long fa_kept_iftd{0};  ///< …from the IF-phase triple-difference driver (`kCauseIfTd`).
    long long fa_kept_wusat{0}; ///< …from the whole-satellite wind-up restart (`kCauseWindup`).
    long long fa_kept_other{0}; ///< …from a cause bit this split does not name; expected to be 0.

    /// Which of the three lower-bound terms was binding, over every actuation
    /// that reached the window scan (i.e. `seen - no_carry`).  Without this split
    /// the only outcome is "N repaired, M false alarms", which cannot tell a
    /// window that is too narrow from a population that did not jump, and the two
    /// need opposite fixes.  On the 60-epoch campaign smoke the latency term binds
    /// on nearly every actuation, and widening it to 1e5 s moves `applied` only
    /// from 88 to 90, so on that bed late detection does not explain the
    /// false-alarm population.
    long long bound_window{0};  ///< …`now_s - window_s` was the max (the normal warm case).
    long long bound_birth{0};   ///< …@ref LocalCarry::birth_s was the max: the arc is younger than the window (born, or erased and re-born, inside it).
    long long bound_applied{0}; ///< …@ref ArcState::last_applied_s was the max: a second detector on an arc this oracle already repaired.
};

/**
 * @brief Build the oracle for this run: read every roster station's
 *        `truth_slips.csv` and index it by (station index, satellite, signal).
 *
 * The fixture writes `<truth_root>/<STATION>/truth_slips.csv` with header
 * `week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg` and one row per
 * applied jump (src/sim/main.cpp).  The station directory name equals the roster
 * station name, and a sim fixture has one file per station, so a missing file
 * means a mis-pointed root, not a quiet station, and is a hard error.  (The
 * held-out closed-loop consumer is not in @p stations, so its file is not read.)
 *
 * @param config_      The run config (reads the `network.cycle_slip.oracle_*` block).
 * @param stations     The joint-solve roster, in station-index order.
 * @param log_manager  For the wavelength provider.
 * @param log          Run logger.
 * @return The constructed oracle; never null on success.
 * @throws std::runtime_error if the root or any station's truth file is missing
 *         or unreadable, or a row is malformed.
 */
std::unique_ptr<NetworkSlipOracle> networkSlipOracleLoad(
    const GnssConfig& config_,
    const std::vector<NetStation>& stations,
    LogManager& log_manager,
    const std::shared_ptr<spdlog::logger>& log);

/**
 * @brief The hook: decide, and when it decides, actuate, for one arc that a
 *        detector site is about to free.
 *
 * @param oracle     Run-scope oracle (never null at a call site: the sites test
 *                   the pointer first, so with the knob off no call is reached).
 * @param sta        Station index (the `local_carry` key's first element).
 * @param sat        Satellite id.
 * @param signal     RINEX signal code of the arc (e.g. "L1C").
 * @param now_s      This epoch's continuous-GPS seconds (`week*604800 + tow`).
 * @param cause      The calling site's `RelevelEpochState::kCause*` bit (the
 *                   detector tier), used only to attribute a false-alarm keep
 *                   (@ref NetworkSlipOracle::fa_kept_mw and the other tiers).  It
 *                   never changes a decision; an unrecognised bit lands in
 *                   @ref NetworkSlipOracle::fa_kept_other, expected to be 0.
 * @param[out] detail  Filled with `delta=<sum d>;n=<slips>;noise=<x>` when handled
 *                     (the caller puts it in the event row's detail column); left
 *                     untouched otherwise.
 * @param[out] outcome Filled with what happened (event action selector).
 * @return true when the oracle handled the arc: the caller then skips every
 *         freeing action for it (no free_ambs, no carry_slipped_ambs, no
 *         pending-reseed, no pend void, no erase) and emits its event with
 *         `oracle_repair` / `oracle_keep`.  false: proceed with the ordinary
 *         response (a false alarm under policy `free`, a truth slip under
 *         `true_slip` `free`, or an arc the oracle could not measure).
 */
bool networkSlipOracleHandle(NetworkSlipOracle& oracle, int sta,
                             const std::string& sat, const std::string& signal,
                             double now_s, std::uint8_t cause,
                             std::string& detail, OracleOutcome& outcome);

/// Deterministic zero-mean Gaussian in cycles, seeded from
/// (station, satellite, signal, epoch), so two calls with the same key in the
/// same run, or in a re-run, produce the same draw.  Exposed for the unit
/// suite's determinism case.  Returns exactly 0 when @p sigma_cyc <= 0.
double networkSlipOracleNoise(double sigma_cyc, int sta, const std::string& sat,
                              const std::string& signal, double now_s);

/// Parse one `truth_slips.csv` into ascending-by-time per-(sat, signal) jump
/// lists.  Split out for the unit suite (the loader wraps it per station).
/// @throws std::runtime_error on a malformed header or row.
std::map<std::pair<std::string, std::string>,
         std::vector<NetworkSlipOracle::TruthSlip>>
networkSlipOracleParseCsv(const std::string& path);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the sim-only oracle slip response.
 *
 * The sim-only oracle slip response: detector actuations on carried arcs and the counts
 * that grade them.
 *
 * Gate: returns 0 at once unless `ctx.carry.slip_oracle`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSlipOracleRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;
class LogManager;

/**
 * @brief Loads the oracle slip response (simulation only) when network.cycle_slip.oracle_relevel is armed.
 *
 * Throws on a missing or mis-pointed fixture, before any trace file opens.  Setup builder (card
 * NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.ctx.carry.slip_oracle.
 * @param log_manager_ The application's logging manager (Application::log_manager_).
 */
void networkSlipOracleArm(NetworkRun& run, LogManager& log_manager_);

} // namespace GnssProcessor
