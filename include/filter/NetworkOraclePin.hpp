#pragma once
/**
 * @file NetworkOraclePin.hpp
 * @brief SIM-ONLY ORACLE AMBIGUITY PIN — hold some or all of the producer's
 *        float ambiguities at the fixture's TRUE integers and read where the
 *        remaining error goes (`network.inject.events[] kind "oracle_pin"`).
 *
 * ## The question this answers
 *
 * The synthetic bed knows every true integer ambiguity
 * (`<truth_root>/<STATION>/truth_ambiguities.csv`, one `N_int` per
 * (satellite, signal) arc for the whole day, PRE-slip) and every applied slip
 * (`truth_slips.csv`).  Holding the ambiguities at truth inside the estimator
 * removes the ambiguity/ionosphere degeneracy: whatever error REMAINS in the
 * slant ionosphere, the ZWD, the clocks, the code and phase biases and the
 * consumer position is the model's own.  Partial pins (one band, one station,
 * one satellite) are the COUPLING MAP — they show which parameter absorbs the
 * error a pinned arc no longer carries.
 *
 * `all` is therefore the CEILING of what integer information buys on this bed,
 * and a direct test of the standing root-cause claim (memory
 * `project_why_non_integer_rootcause`: 99.3 % ionosphere).  Slant iono snapping
 * to the phase floor CONFIRMS it; iono staying wrong FALSIFIES it and the other
 * rows name the sink it went to instead.
 *
 * ## It is FIELD-INERT by construction
 *
 * Exactly like `network.cycle_slip.oracle_relevel`: the pin needs a truth root,
 * a field lineage has none, so the loader HARD-ERRORS when any `oracle_pin`
 * event is armed and `network.cycle_slip.oracle_truth_root` is empty or absent,
 * and @ref networkOraclePinLoad hard-errors when any ROSTER station is missing
 * `truth_ambiguities.csv` or `truth_slips.csv`.  This is a DIAGNOSTIC: it must
 * never be promoted, never appear in a golden, and never enter the config
 * promotion ledger.
 *
 * With no `oracle_pin` event in `network.inject.events[]` the object is never
 * constructed, every call site tests a null pointer first, and not one branch
 * below is reachable — standing invariant #6's structural byte-identity.
 *
 * ## THE GAUGE — why the pin is a two-way fit and not `lambda * N_true`
 *
 * A raw pin `value_abs = λ·N_true` fights the datum.  The network fixes ONE arc
 * per (satellite, band) family — the ANCHOR — at whatever level it carried when
 * it was elected, and station-level phase biases ride INSIDE the ambiguities.
 * In the run's own frame a non-anchor arc's ambiguity column is
 *
 *     α_rsj = β_sj + u_rj + λ_j · N_rsj
 *
 * where
 *   - `N_rsj` is the TRUE integer of arc (station r, satellite s, band j) at
 *     this epoch — `N_int` plus every truth slip delta stamped at or before now;
 *   - `u_rj` is station r's band-j PHASE OFFSET: the receiver phase bias plus
 *     whatever code-datum lump the receiver clock absorbed.  Station-common
 *     across satellites, NOT an integer, and DATA-DETERMINED — the pin must not
 *     overwrite it, so it is FITTED from the run's own carried values;
 *   - `β_sj` is the family's frame term, fixed by the anchor a(s,j) and its
 *     enforced pin target `t_sj` (`NetStation::pinned_amb_targets`; 0 on the
 *     prioritized default, the CARRIED value under a continuous handover — it is
 *     READ, never assumed):
 *
 *         β_sj = t_sj − λ_j · N_{a s j} − u_{a j}.
 *
 * Both terms are solved together, once per epoch, over EVERY carried non-anchor
 * arc (@ref networkOraclePinFit).  Eliminating `β` between an arc and its own
 * family's anchor leaves a pure STATION-OFFSET DIFFERENCE, with every frame term
 * gone:
 *
 *     g(r,s,j) = value_abs(r,s,j) − t_sj − λ_j·(N_rsj − N_{a s j})
 *              = u_rj − u_{a(s,j) j},
 *
 * so the fit is a LEVELLING NETWORK over the per-(station, band) offsets,
 * determined up to one constant per band — the same free constant the pinned
 * values are invariant to.  It is solved by DETERMINATION, not iteration: a root
 * anchor per band is declared 0, and each remaining station's offset is estimated
 * ONCE, as a MEDIAN over >= 3 arcs whose family anchors already have a value, and
 * then frozen.
 *
 * ⚠ The obvious alternative — alternate "β from the anchors, u from the medians"
 * to a tolerance — does NOT converge.  Its error recursion is
 * `e_r = median over the station's families of e_{anchor}`, and a median is a
 * SELECTION, not an average, so the anchors' errors cycle among their own values
 * forever (reproduced on the unit suite's 6×8×2 bed: 20/20 iterations,
 * oscillating between two values).  The properties that make this the right pin
 * — and that the unit suite asserts:
 *
 *   1. every pinned DOUBLE DIFFERENCE equals λ·DD(N_true) EXACTLY (β and u
 *      cancel in a DD), so the pin asserts INTEGER information and nothing else;
 *   2. anchor arcs are never in the table (they are gauge-pinned at 1e10 and a
 *      second row there would fight the datum);
 *   3. the free per-band constant of (β, u) — β−c together with u+c — leaves
 *      every pinned value unchanged, i.e. the pin adds no gauge of its own.
 *
 * The current epoch's anchors are used EVERY epoch, so a handover moves the
 * family's β and the pin follows it.  On the slipped bed `N_rsj(t)` steps at a
 * truth slip and the pinned value steps with the phase — the intended
 * "follows truth" behaviour.
 *
 * ## Where it actuates — a real prior, never an `x_lin` shift
 *
 * The pin table is built SERIALLY once per epoch (before PASS B) and read
 * READ-ONLY inside the possibly-parallel PASS B produce loop, where each pinned
 * ambiguity column takes ONE DIAGONAL LOCAL PRIOR row of the same shape as the
 * phase-leveling carry row:
 *
 *     Λ = 1 / σ², η = Λ · (pin.value_m − x_lin(amb_idx)).
 *
 * ⚠ THE x_lin TRAP (docs/SPEC_lane_slip_response.md §7.2): shifting `x_lin`
 * alone is a proven EXACT solve no-op — the carried prior's RHS absorbs it.
 * The row above is a real (Λ, η) pair, which is why it actuates, and the pin
 * deliberately does NOT also write @ref LocalCarry::value_abs.
 *
 * σ defaults to 1e-4 m (≈ 0.0005 cyc, weight 1e8) — stiff, but two decades
 * BELOW the 1e10 datum pins, so the gauge stays the anchors'.
 *
 * The row is INDEPENDENT of the leveling carry: it fires whether or not the arc
 * has a @ref LocalCarry entry, whether or not the slip response freed the arc
 * this epoch, and regardless of the F3 local block.
 */

#include "filter/NetworkRunTypes.hpp"   // NetStation, LocalCarry
#include "filter/NetworkOraclePinScope.hpp"  // OraclePinScope + its grammar (shared with the loader)
#include "slip/NetworkSlipOracle.hpp"   // NetworkSlipOracle::TruthSlip + its CSV parser

#include <spdlog/spdlog.h>

#include <cstddef>
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
class NetworkRunTraces;
struct NetworkRunContext;


/**
 * @brief Run-scope state for the sim-only oracle ambiguity pin.
 *
 * Constructed ONLY when at least one `oracle_pin` event is parsed
 * (@ref networkOraclePinLoad); null on every other run, so the OFF path is
 * structurally byte-identical.  Non-copyable: a value copy would duplicate the
 * `ever_pinned` ledger and mis-report the first-pin / release edges.
 */
struct NetworkOraclePin {
    NetworkOraclePin() = default;
    NetworkOraclePin(const NetworkOraclePin&) = delete;
    NetworkOraclePin& operator=(const NetworkOraclePin&) = delete;
    ~NetworkOraclePin();

    /// One arc's truth: the day's PRE-slip integer, the jumps applied to it, and
    /// the wavelength.  ⚠ @ref lambda_m is the RUN's wavelength (from
    /// @ref utils, FDMA-correct via the satellite id), NOT the CSV's column —
    /// the CSV value is read only to cross-check and is otherwise unused.
    struct Truth {
        int    n_int{0};
        double lambda_m{0.0};        ///< The RUN's wavelength (from @ref utils) — the pin's metres.
        double lambda_csv_m{0.0};    ///< The fixture's own column, kept for the cross-check only.
        std::vector<NetworkSlipOracle::TruthSlip> slips;   ///< ASCENDING in @c s.
    };

    std::string truth_root;   ///< `network.cycle_slip.oracle_truth_root`.

    /// (station index, satellite, signal) -> that arc's truth.
    std::map<std::tuple<int, std::string, std::string>, Truth> truth;

    /// Wavelength provider (FDMA-correct).  Owned here so the pin does not
    /// depend on whether any OTHER card armed a shared provider.
    std::unique_ptr<PppUtils> utils;

    /// Every armed `oracle_pin` event's (index into `network.inject.events`,
    /// parsed scope, σ, tow).  An arc is pinned when ANY scope whose event has
    /// STARTED covers it; the FIRST such event owns the row's σ and event index.
    struct Armed {
        std::size_t    ev_index{0};
        OraclePinScope scope;
        double         sigma_m{1.0e-4};
        double         tow{-1.0};
    };
    std::vector<Armed> scopes;

    // ---- THIS EPOCH's pin table (rebuilt by networkOraclePinEpoch) ----------
    /// One pinned arc: the gauge-consistent absolute value, its prior σ, and the
    /// event that owns it.  Read-only inside PASS B.
    struct Pin {
        double      value_m{0.0};
        double      sigma_m{1.0e-4};
        std::size_t ev_index{0};
        int         n_true{0};      ///< The true integer this value encodes (telemetry).
    };
    std::map<std::tuple<int, std::string, std::string>, Pin> table;

    // ---- counters (standing invariant #4) ----------------------------------
    long long epochs_active{0};        ///< Epochs where >= 1 event had started (table rebuilt).
    long long rows_injected{0};        ///< ARC-EPOCHS actually folded as a prior row.
    long long arcs_first_pinned{0};    ///< Distinct arcs entering the table for the first time.
    long long arcs_released{0};        ///< Arcs pinned last epoch that are absent this epoch.
    long long no_anchor{0};            ///< In scope, but the family had NO anchor this epoch (arc-epochs).
    long long no_truth{0};             ///< In scope, but no truth row for the arc (arc-epochs).
    long long no_station_term{0};      ///< In scope, but station r's band-j offset u could not be fitted (< 3 arcs).
    long long no_column{0};            ///< Table hit at PASS B, but the ambiguity column was not in this epoch's A_r.
    long long gauge_pinned_skipped{0}; ///< Table hit at PASS B on a GAUGE-PINNED (anchor) arc — never folded.
    long long fit_iters_total{0};      ///< Sum of determination ROUNDS over active epochs.
    long long fit_iters_last{0};       ///< Determination rounds of the LAST active epoch (1 = every station resolved straight off the root anchor).
    double    fit_resid_rms_cyc{0.0};  ///< LAST epoch: rms over pinned arcs of (value_abs − pinned)/λ.
    double    fit_resid_max_cyc{0.0};  ///< LAST epoch: max |…| of the same.
    long long fit_resid_n{0};          ///< LAST epoch: arcs the residual was measurable on.

    /// Arcs ever in the table — drives @ref arcs_first_pinned.
    std::set<std::tuple<int, std::string, std::string>> ever_pinned;
    /// The PREVIOUS epoch's table keys.  BOTH edges of invariant #7 are taken
    /// against it and BOTH emit a per-arc `network_events.csv` row (action
    /// `pinned` / `released`), so the pinned set at any epoch is exactly
    /// (entries − exits) up to that epoch.  The counters alone were measured to be
    /// insufficient: a grader reconstructing membership from the ONSET rows
    /// under-counts by every arc that joined later (7 828 at onset vs 9 381 at the
    /// run end on the first quiet arm) and reads a working pin as a failure.
    std::set<std::tuple<int, std::string, std::string>> prev_keys;
};

/**
 * @brief Parse one `truth_ambiguities.csv`.
 *
 * Header contract (src/sim/main.cpp): `satellite,signal,N_int,wavelength_m`.
 * Pinned by NAME rather than by position so a fixture that grows a column fails
 * loudly here instead of silently mis-reading `N_int`.
 *
 * @param path  File to read.
 * @return (satellite, signal) -> (N_int, wavelength_m from the CSV).
 * @throws std::runtime_error on a malformed header or row, or a duplicate arc.
 */
std::map<std::pair<std::string, std::string>, std::pair<int, double>>
networkOraclePinParseAmbCsv(const std::string& path);

/**
 * @brief The TRUE integer of an arc at continuous-GPS second @p now_s.
 *
 * `N(t) = N_int + Σ delta_cycles over every truth slip with s <= t` — the bound
 * is INCLUSIVE, so a slip stamped exactly at @p now_s counts (the observation at
 * that epoch already carries it).
 */
int networkOraclePinTrueN(const NetworkOraclePin::Truth& t, double now_s);

/**
 * @brief Build the pin for this run: read every ROSTER station's
 *        `truth_ambiguities.csv` + `truth_slips.csv` and index them by
 *        (station index, satellite, signal).
 *
 * Called ONLY when `network.inject.events[]` holds at least one `oracle_pin`
 * event.  A MISSING per-station file is a mis-pointed root, not a quiet station,
 * and is a hard error — the field-inertness mechanism.
 *
 * @param config_      Run config (reads `network.inject` + `network.cycle_slip.oracle_truth_root`).
 * @param stations     Joint-solve roster, in station-index order.
 * @param log_manager  For the wavelength provider.
 * @param log          Run logger.
 * @return The constructed pin; never null on success.
 * @throws std::runtime_error on a missing root, a missing/unreadable station
 *         file, a malformed row, or an unparseable scope.
 */
std::unique_ptr<NetworkOraclePin> networkOraclePinLoad(
    const GnssConfig& config_,
    const std::vector<NetStation>& stations,
    LogManager& log_manager,
    const std::shared_ptr<spdlog::logger>& log);

/**
 * @brief THE FIT — rebuild this epoch's pin table from truth + the run's own frame.
 *
 * Split out from @ref networkOraclePinEpoch so the unit suite can drive it on a
 * synthetic carry with no estimator.  Resolves the per-(station, band) offsets by
 * the rooted determination described in the header banner, then writes the
 * in-scope non-anchor arcs into @c pin.table.
 *
 * @param pin        Run-scope state (table + counters are written).
 * @param stations   Roster (names, `pinned_amb_targets`).
 * @param carry      `ctx.carry.local_carry` — the fit's observations
 *                   (@ref LocalCarry::value_abs, never `x_lin`).
 * @param anchor_of  `ctx.carry.this_epoch_anchor`: "<sat>_<sig>" -> anchor STATION NAME.
 * @param now_s      Continuous-GPS seconds of this epoch.
 * @param active     Indices into @c pin.scopes whose event has STARTED.
 * @return Determination ROUNDS the fit ran (0 when nothing beyond the roots was
 *         resolvable; terminates in at most one round per station by
 *         construction — a station's offset is estimated once and frozen).
 */
int networkOraclePinFit(
    NetworkOraclePin& pin,
    const std::vector<NetStation>& stations,
    const std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    const std::map<std::string, std::string>& anchor_of,
    double now_s,
    const std::vector<std::size_t>& active);

/**
 * @brief Per-epoch driver: decide whether any event has started, rebuild the
 *        table, move the first-pin / release edges, and emit the trace rows.
 *
 * SERIAL, once per epoch, AFTER the phase-anchor election and AFTER column
 * registration, BEFORE PASS B.  With no event yet due the table is CLEARED and
 * no counter moves.
 */
void networkOraclePinEpoch(NetworkRunContext& ctx,
                           const GnssConfig& config_,
                           const std::vector<NetStation>& stations,
                           const GnssTime& t,
                           const std::shared_ptr<spdlog::logger>& log,
                           NetworkRunTraces& traces);

/**
 * @brief Run-end clause (one log line, every counter of @ref NetworkOraclePin).
 *
 * Standing invariant #4: a run reporting `rows_injected == 0` has measured
 * NOTHING, and the line says so in-band.
 *
 * @return 0 always (the never-fired hard error belongs to the injector's own
 *         run-end check, which this kind participates in unchanged).
 */
int networkOraclePinRunEnd(const NetworkOraclePin* pin,
                           const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
