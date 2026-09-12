#pragma once
/**
 * @file NetworkSlipOracle.hpp
 * @brief SIM-ONLY ORACLE slip response — the measured CEILING of a
 *        level-preserving re-level (`network.cycle_slip.oracle_relevel`).
 *
 * ## Why this exists
 *
 * The truth-graded slip census on the slipped 60-station bed
 * (`runs/golden_sim_campaign` against the fixture `runs/simnet60_campaign_wu93`,
 * 2 452 truth slips / 6 h) established the shape of the producer's CURRENT slip
 * response: a detector fires, the arc is FREED, and the PASS-B leveling inject
 * re-levels it from that one epoch's own — already corrupted — states.  The
 * level error injected by the response is ~2.5x the bed's ambient DD error
 * (|e| 5.6 -> 6.7 cyc), it takes ~20 min to walk back, and the T4 attribution
 * tier then re-erases the damage (48 % of T4 events land within 15 min of a
 * previous re-level; chain pairs get WORSE 64 % of the time).  The metre-class
 * ZWD plateau (truth rms 2.10 m against 7 mm on the quiet twin) is the sink
 * those injections drain into.  This is standing invariant #1's RE-COUPLING
 * edge, measured: the response's own re-level is the defect.
 *
 * Building the engineering answer (EWL / WL-MW integer estimation of the jump,
 * SPEC_lane_slip_response) is expensive.  Before paying for it we measure its
 * CEILING.  When a detector actuates a refloat on a CARRIED arc, the oracle
 * looks the TRUE integer jump(s) up in the fixture's `truth_slips.csv` and
 * SHIFTS the carried level by exactly the sum d*lambda instead of freeing the
 * arc.  Nothing else changes: the detector population, the epoch ordering, the
 * events and the rest of the QC stack are untouched.  If the ZWD plateau does
 * NOT collapse under a perfect repair, level preservation is FALSIFIED as the
 * lever and the engineering card should not be built.
 *
 * ## It is FIELD-INERT by construction
 *
 * The knob needs a truth root; a field lineage has none, so the loader
 * HARD-ERRORS when `oracle_relevel` is armed and
 * `network.cycle_slip.oracle_truth_root` does not exist, and setup hard-errors
 * when any ROSTER station is missing its `truth_slips.csv`.  This is a
 * DIAGNOSTIC knob: it must never be promoted, and it must never appear in a
 * golden.
 *
 * ## Where it actuates, and where it deliberately does NOT
 *
 * ONE helper (@ref networkSlipOracleHandle) is called from each of the four
 * DETECTOR refloat actuation sites in slip/NetworkSlipQc.cpp:
 *   - `refloat_signal` (the MW confident-wide-lane drain AND the RINEX-LLI
 *     drain both funnel through it),
 *   - `refloat_pair`   (the IF-phase triple-difference driver, both bands),
 *   - `networkSlipDrainWindupRestart` (whole-satellite, every signal).
 * `networkSlipDrainGapReentry` is a DIFFERENT response (a break the detectors
 * structurally cannot cover) and is deliberately left alone, as is the T4
 * RAIM-attribution erase path in filter/NetworkRaim.cpp — the census question
 * is what a perfect DETECTOR response would buy, with every other mechanism
 * held fixed.
 *
 * When the helper returns true the caller SKIPS every freeing action for that
 * arc: no `free_ambs` latch, no `carry_slipped_ambs` funnel insert, no
 * pending-reseed mark, no bridge-pend void, no erase.  The site's event row is
 * still emitted, with action `oracle_repair` / `oracle_keep`, so the trace
 * population is comparable to the control arm's.
 *
 * ## The window: a detector-LATENCY bound, floored by the carry's BIRTH
 *
 * A repair must apply exactly the jumps the carried level does NOT already
 * contain.  The window is
 *
 *     ( max(last_applied_s, carry.birth_s, now_s − window_s), now_s ]
 *
 * lower bound EXCLUSIVE, upper bound INCLUSIVE.  Each term earns its place:
 *
 * - `now_s − window_s` (@ref NetworkSlipOracle::window_s,
 *   `network.cycle_slip.oracle_window_s`, default 90 s = three 30 s epochs) is
 *   the DETECTOR-LATENCY bound and the load-bearing one.  Detectors normally
 *   fire 1–3 epochs after the slip — MW needs its window to fill, IF-TD needs
 *   the triple difference — which is exactly the 90 s the truth census calls
 *   DETECTED.  ⚠ @ref LocalCarry::last_seen_s is NOT usable as the lower bound
 *   even though it looks like the natural "already absorbed" mark: it is the
 *   PREVIOUS epoch's stash time, and a zero-q carried level is not re-set by an
 *   intervening solve (the stiff prior moves only slightly against one epoch of
 *   phase), so the jump is still un-absorbed when the late detector arrives.
 *   Using it made the normal population read as false alarms — 701 "false-alarm
 *   kept" against 88 repaired on a 60-epoch smoke — and the ceiling that arm
 *   would have reported was a badly biased lower bound.
 * - `carry.birth_s` (@ref LocalCarry::birth_s) is the HARD floor: the
 *   continuous-GPS second at which the CURRENT carry entry was CREATED.  A T4
 *   attribution erase deletes the entry, and the next harvest re-creates it from
 *   a fresh solve whose level ALREADY contains any earlier jump — so a truth
 *   slip stamped before the entry's birth is absorbed by construction and
 *   re-applying it would inject the very error this card exists to remove.  It
 *   is stamped once at creation and preserved across re-stashes, exactly like
 *   @ref LocalCarry::birth_ep (which is NOT usable here: it is a loop-epoch
 *   INDEX, not seconds).  ⚠ It is deliberately not snapshotted, so a
 *   warm-resume-restored entry carries @ref LocalCarry::kBirthSRestored (−1,
 *   SPEC_event_harness §15.4) and the window falls back to the latency bound
 *   alone — the conservative direction for a sim-only diagnostic — and keeps
 *   doing so for the arc's whole restored life.  Before the sentinel the first
 *   post-resume harvest re-stamped it with the RESUME second, which floored the
 *   window there and reported the arc as `bound_birth`.
 * - `last_applied_s` (@ref ArcState) guards the same arc being handled twice
 *   before a re-harvest: two detectors firing in one epoch, or on consecutive
 *   epochs, must apply the jump ONCE.  It advances on every repair.
 *
 * A window that holds no truth slip is a genuine FALSE ALARM — the detector
 * fired on an arc that did not jump within the latency it is capable of seeing —
 * and is answered by @ref NetworkSlipOracle::false_alarm.
 *
 * ## The two policies are independent, and that is what makes the census additive
 *
 * @ref NetworkSlipOracle::true_slip answers the OTHER half — an actuation whose
 * window DOES hold a truth slip.  `repair` is the ceiling this card was built
 * for; `free` declines the arc so the caller runs today's ordinary response for
 * it.  The pair spans the 2x2 the census needs:
 *
 * | `true_slip` | `false_alarm` | what the arm measures |
 * |---|---|---|
 * | `repair` | `keep`   | THE CEILING — perfect repair, no false-alarm cost |
 * | `repair` | `free`   | the repair alone, false-alarm cost KEPT |
 * | `free`   | `keep`   | the FALSE ALARMS alone — real slips pay today's response |
 * | `free`   | `free`   | a pure NO-OP (both halves decline): a byte-identity control |
 *
 * The third row is the isolation arm: if the ZWD sink and the consumer move on
 * it, the detector's false alarms are a material injection source in their own
 * right and the detector thresholds become a card; if they do not, the
 * true-slip re-level is the whole lever and the engineering build (EWL / WL-MW
 * integer estimation) is the only way to buy it.  ⚠ On `free` the run-end
 * `applied` count is 0 BY CONSTRUCTION — the armed-and-inert check reads
 * @ref NetworkSlipOracle::fa_kept there instead.
 */

#include "filter/NetworkRunTypes.hpp"   // NetStation, LocalCarry (PppUtils fwd via PppModel)

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
    NotHandled,   ///< The caller must proceed with today's response (free the arc).
    Repaired,     ///< The carried level was shifted by the true sum d*lambda.
    Kept          ///< No truth slip in the window; the carry was left untouched.
};

/**
 * @brief Run-scope state for the sim-only oracle slip response.
 *
 * Owns the truth table, the per-arc application ledger, the wavelength provider
 * and the run-end counters.  Constructed ONLY when the knob is armed
 * (@ref networkSlipOracleLoad); the drains receive a `NetworkSlipOracle*` that
 * is null on every other run, so the OFF path never reaches a call and is
 * structurally byte-identical (standing invariant #6).
 *
 * Non-copyable: a value copy would silently reset the per-arc ledger and let the
 * same jump be applied twice.
 */
struct NetworkSlipOracle {
    NetworkSlipOracle() = default;
    NetworkSlipOracle(const NetworkSlipOracle&) = delete;
    NetworkSlipOracle& operator=(const NetworkSlipOracle&) = delete;
    ~NetworkSlipOracle();

    /// What to do when a detector fires on a carried arc with NO truth slip in
    /// the window.  `Keep` is THE CEILING (an oracle knows it was a false alarm
    /// and leaves the level alone); `Free` falls through to today's response, so
    /// the arm measures repair-only and keeps the false-alarm cost.
    enum class FalseAlarm { Keep, Free };

    /// What to do when the window DOES hold a truth slip.  `Repair` is the
    /// ceiling (shift the level by the true sum d*lambda); `Free` declines the
    /// arc so the caller runs TODAY's ordinary free/re-level response for it —
    /// which, paired with @ref FalseAlarm::Keep, isolates the false-alarm cost
    /// on its own.  See the header banner's 2x2.
    enum class TrueSlip { Repair, Free };

    // ---- knobs (snapshot taken at construction) ----------------------------
    std::string truth_root;              ///< `network.cycle_slip.oracle_truth_root`.
    double      noise_cyc{0.0};          ///< `network.cycle_slip.oracle_noise_cyc` — 1-sigma of the deterministic Gaussian added to each applied sum d (cycles).
    FalseAlarm  false_alarm{FalseAlarm::Keep}; ///< `network.cycle_slip.oracle_false_alarm`.
    TrueSlip    true_slip{TrueSlip::Repair};   ///< `network.cycle_slip.oracle_true_slip`.
    double      window_s{90.0};          ///< `network.cycle_slip.oracle_window_s` — detector-latency bound (s) on how far back a truth slip may be repaired; see the header banner.  0 = same-epoch only.

    /// One truth jump: continuous-GPS seconds and the integer jump in THAT
    /// signal's own cycles (the fixture writes one row per applied jump).
    struct TruthSlip {
        double s{0.0};
        int    delta_cyc{0};
    };
    /// (station index, satellite, signal) -> the arc's jumps, ASCENDING in `s`.
    std::map<std::tuple<int, std::string, std::string>, std::vector<TruthSlip>> truth;

    /// Per-arc application ledger — one of the three exclusive lower bounds.
    struct ArcState { double last_applied_s{-1.0e18}; };
    std::map<std::tuple<int, std::string, std::string>, ArcState> arcs;

    /// The leveling carry this oracle actuates on: `ctx.carry.local_carry`.
    /// A bare pointer because the map is a sibling member of the same
    /// @ref NetworkRunContext that owns this object — it outlives the oracle and
    /// is never reallocated.  ⚠ The oracle actuates on @ref LocalCarry::value_abs
    /// and NOTHING else: an `x_lin` shift is a proven EXACT solve no-op
    /// (docs/SPEC_lane_slip_response.md §7.2).
    std::map<std::tuple<int, std::string, std::string>, LocalCarry>* carry{nullptr};

    /// Wavelength provider for the cycles->metres conversion (FDMA-correct via
    /// the satellite id).  Owned here so the oracle does not depend on whether
    /// any OTHER card armed the shared raw-observable provider.
    std::unique_ptr<PppUtils> utils;

    // ---- run-end counters (standing invariant #4) --------------------------
    /// Closure:
    /// `seen == applied + true_slip_freed + fa_kept + fa_freed + no_carry + no_lambda`.
    long long seen{0};          ///< (station, sat, signal) actuations offered to the hook.
    long long applied{0};       ///< Arcs REPAIRED (carry shifted by the true sum d*lambda).
    long long multi{0};         ///< …of which the window held >= 2 truth jumps.
    long long true_slip_freed{0}; ///< Truth slip IN window, policy `free` — declined, so the caller ran today's response (the false-alarm-isolation arm).
    long long fa_kept{0};       ///< No truth slip in window, policy `keep` — carry untouched, arc NOT freed.
    long long fa_freed{0};      ///< No truth slip in window, policy `free` — fell through to today's response.
    long long no_carry{0};      ///< No leveling carry entry for the arc — nothing to preserve, fell through.
    long long no_lambda{0};     ///< Wavelength unavailable for the signal — fell through (never silently skipped).
    double    sum_abs_cyc{0.0}; ///< Sum over applied arcs of |sum d| (cycles).
    double    max_abs_cyc{0.0}; ///< Max over applied arcs of |sum d| (cycles).

    /// Per-DETECTOR-TIER split of @ref fa_kept, keyed on the `kCause*` bit the
    /// calling site already carries.  Without it a false-alarm-isolation arm
    /// reports one lump number and cannot say WHICH detector's thresholds to
    /// look at — the four sites have four different statistics (an MW window
    /// integer, the receiver's own LLI bit, an IF triple difference, a wind-up
    /// accumulator restart) and only a per-tier attribution turns "false alarms
    /// cost X" into a card.  Closure: `fa_kept == mw + lli + iftd + wusat + other`;
    /// `other` must be 0 (an unrecognised cause bit is a mis-wired call site).
    long long fa_kept_mw{0};    ///< …from the MW confident-wide-lane drain (`kCauseMw`).
    long long fa_kept_lli{0};   ///< …from the RINEX-LLI loss-of-lock drain (`kCauseLli`).
    long long fa_kept_iftd{0};  ///< …from the IF-phase triple-difference driver (`kCauseIfTd`).
    long long fa_kept_wusat{0}; ///< …from the whole-satellite wind-up restart (`kCauseWindup`).
    long long fa_kept_other{0}; ///< …from a cause bit this split does not name — MUST be 0.

    /// Which of the three lower-bound terms was BINDING, over every actuation
    /// that reached the window scan (i.e. `seen - no_carry`).  Without this the
    /// only readable outcome is "N repaired, M false alarm", which cannot tell a
    /// window that is too narrow from a population that genuinely did not jump —
    /// the two have opposite fixes.  Measured on the 60-epoch campaign smoke:
    /// the LATENCY term binds on essentially every actuation and widening it to
    /// 1e5 s moves `applied` 88 -> 90, so on that bed late detection is NOT what
    /// the false-alarm population is made of.
    long long bound_window{0};  ///< …`now_s - window_s` was the max (the normal warm case).
    long long bound_birth{0};   ///< …@ref LocalCarry::birth_s was the max — the arc is YOUNGER than the window (born, or erased-and-reborn, inside it).
    long long bound_applied{0}; ///< …@ref ArcState::last_applied_s was the max — a second detector on an arc this oracle already repaired.
};

/**
 * @brief Build the oracle for this run: read every ROSTER station's
 *        `truth_slips.csv` and index it by (station index, satellite, signal).
 *
 * The fixture writes `<truth_root>/<STATION>/truth_slips.csv` with header
 * `week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg` and one row per
 * APPLIED jump (src/sim/main.cpp).  Station directory name == roster station
 * name, and a sim fixture has one file per station — so a MISSING file is a
 * mis-pointed root, not a quiet station, and is a hard error.  (The held-out
 * closed-loop consumer is not in @p stations, so its absence is not consulted.)
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
 * @brief THE HOOK — decide and (when it decides) actuate, for ONE arc a detector
 *        site is about to free.
 *
 * @param oracle     Run-scope oracle (never null at a call site — the sites test
 *                   the pointer first so the OFF path is unreachable).
 * @param sta        Station index (the `local_carry` key's first element).
 * @param sat        Satellite id.
 * @param signal     RINEX signal code of the arc (e.g. "L1C").
 * @param now_s      This epoch's continuous-GPS seconds (`week*604800 + tow`).
 * @param cause      The calling site's `RelevelEpochState::kCause*` bit — the
 *                   DETECTOR TIER, used only to attribute a false-alarm KEEP
 *                   (@ref NetworkSlipOracle::fa_kept_mw and friends).  It never
 *                   changes a decision; an unrecognised bit lands in
 *                   @ref NetworkSlipOracle::fa_kept_other, which must be 0.
 * @param[out] detail  Filled with `delta=<sum d>;n=<slips>;noise=<x>` when handled
 *                     (the caller puts it in the event row's detail column); left
 *                     untouched otherwise.
 * @param[out] outcome Filled with what happened (event action selector).
 * @return true when the oracle HANDLED the arc — the caller must skip EVERY
 *         freeing action for it (no free_ambs, no carry_slipped_ambs, no
 *         pending-reseed, no pend void, no erase) and emit its event with
 *         `oracle_repair` / `oracle_keep`.  false ⇒ proceed exactly as today
 *         (a false alarm under policy `free`, a truth slip under `true_slip`
 *         `free`, or an arc the oracle could not measure at all).
 */
bool networkSlipOracleHandle(NetworkSlipOracle& oracle, int sta,
                             const std::string& sat, const std::string& signal,
                             double now_s, std::uint8_t cause,
                             std::string& detail, OracleOutcome& outcome);

/// Deterministic zero-mean Gaussian in CYCLES, seeded from
/// (station, satellite, signal, epoch) so two calls with the same key in the
/// same run — or in a re-run — produce the same draw.  Exposed for the unit
/// suite's determinism case.  Returns 0 exactly when @p sigma_cyc <= 0.
double networkSlipOracleNoise(double sigma_cyc, int sta, const std::string& sat,
                              const std::string& signal, double now_s);

/// Parse one `truth_slips.csv` into ascending-by-time per-(sat, signal) jump
/// lists.  Split out for the unit suite (the loader wraps it per station).
/// @throws std::runtime_error on a malformed header or row.
std::map<std::pair<std::string, std::string>,
         std::vector<NetworkSlipOracle::TruthSlip>>
networkSlipOracleParseCsv(const std::string& path);

} // namespace GnssProcessor
