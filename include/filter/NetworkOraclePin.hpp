#pragma once
/**
 * @file NetworkOraclePin.hpp
 * @brief Simulation-only oracle ambiguity pin: holds some or all of the producer's
 *        float ambiguities at the fixture's true integers so that the remaining
 *        error can be located (`network.inject.events[] kind "oracle_pin"`).
 *
 * ## Purpose
 *
 * The synthetic bed records every true integer ambiguity
 * (`<truth_root>/<STATION>/truth_ambiguities.csv`, one `N_int` per
 * (satellite, signal) arc for the whole day, before any slip) and every applied slip
 * (`truth_slips.csv`).  Holding the ambiguities at truth inside the estimator
 * removes the ambiguity/ionosphere degeneracy: the error that remains in the slant
 * ionosphere, the ZWD, the clocks, the code and phase biases and the consumer
 * position is the model's own.  Partial pins (one band, one station, one satellite)
 * give the coupling map: they show which parameter absorbs the error that a pinned
 * arc no longer carries.
 *
 * Scope `all` is therefore the upper bound on what integer information buys on this
 * bed, and a direct test of the root-cause claim that the non-integer error is
 * 99.3 % ionosphere (memory `project_why_non_integer_rootcause`).  Slant ionosphere
 * that drops to the phase noise floor confirms the claim; ionosphere that stays wrong
 * refutes it, and the other parameters show where the error went instead.
 *
 * ## Field-inert by construction
 *
 * As with `network.cycle_slip.oracle_relevel`, the pin needs a truth root and a field
 * lineage has none.  The loader throws when network.inject is enabled, an
 * `oracle_pin` event is armed, and `network.cycle_slip.oracle_truth_root` is empty or
 * does not exist; @ref networkOraclePinLoad throws when any roster station lacks
 * `truth_ambiguities.csv` or `truth_slips.csv`.  The pin is a diagnostic: it is never
 * promoted, never appears in a golden, and never enters the config promotion ledger.
 *
 * With no `oracle_pin` event in `network.inject.events[]` (or with network.inject
 * disabled) the object is never constructed and every call site tests a null pointer
 * first, so no branch below is reachable and the output is structurally
 * byte-identical (standing invariant #6).
 *
 * ## The gauge: why the pin is a two-way fit and not `lambda * N_true`
 *
 * A raw pin `value_abs = λ·N_true` conflicts with the datum.  The network fixes one
 * arc per (satellite, band) family, the anchor, at the level it carried when it was
 * elected, and station-level phase biases are contained in the ambiguities.  In the
 * run's own frame a non-anchor arc's ambiguity column is
 *
 *     α_rsj = β_sj + u_rj + λ_j · N_rsj
 *
 * where
 *   - `N_rsj` is the true integer of arc (station r, satellite s, band j) at this
 *     epoch: `N_int` plus every truth slip delta stamped at or before now;
 *   - `u_rj` is station r's band-j phase offset: the receiver phase bias plus any
 *     code-datum lump the receiver clock absorbed.  It is common to all satellites at
 *     the station, is not an integer, and is determined by the data, so the pin does
 *     not overwrite it; it is fitted from the run's own carried values;
 *   - `β_sj` is the family's frame term, fixed by the anchor a(s,j) and its enforced
 *     pin target `t_sj` (`NetStation::pinned_amb_targets`; 0 on the prioritized
 *     default, the carried value under a continuous handover; the fit reads it and
 *     does not assume 0):
 *
 *         β_sj = t_sj − λ_j · N_{a s j} − u_{a j}.
 *
 * Both terms are solved together, once per epoch, over every carried non-anchor arc
 * (@ref networkOraclePinFit).  Eliminating `β` between an arc and its own family's
 * anchor leaves a pure station-offset difference, with every frame term gone:
 *
 *     g(r,s,j) = value_abs(r,s,j) − t_sj − λ_j·(N_rsj − N_{a s j})
 *              = u_rj − u_{a(s,j) j},
 *
 * so the fit is a levelling network over the per-(station, band) offsets, determined
 * up to one constant per band; the pinned values are invariant to that constant.  It
 * is solved by determination, not iteration: one root anchor per band is set to 0,
 * and each remaining station's offset is estimated once, as the median over >= 3
 * arcs whose family anchors already have a value, and then frozen.
 *
 * Alternating "β from the anchors, u from the medians" to a tolerance does not
 * converge.  Its error recursion is `e_r = median over the station's families of
 * e_{anchor}`; a median selects one sample instead of averaging, so the anchors'
 * errors cycle among their own values indefinitely (on the unit suite's 6×8×2 bed it
 * ran 20 of 20 iterations, oscillating between two values).  The unit suite asserts
 * the properties that make the determination the correct pin:
 *
 *   1. every pinned double difference equals λ·DD(N_true) exactly (β and u cancel in
 *      a DD), so the pin asserts integer information and nothing else;
 *   2. anchor arcs are never in the table (they are gauge-pinned at 1e10, and a
 *      second row there would conflict with the datum);
 *   3. the free per-band constant of (β, u), β−c together with u+c, leaves every
 *      pinned value unchanged, so the pin adds no gauge of its own.
 *
 * The current epoch's anchors are used every epoch, so a handover moves the family's
 * β and the pin follows it.  On the slipped bed `N_rsj(t)` steps at a truth slip and
 * the pinned value steps with the phase, which is the intended behaviour.
 *
 * ## Where it actuates: a real prior, never an `x_lin` shift
 *
 * The pin table is built serially once per epoch (before PASS B) and is read-only
 * inside the possibly parallel PASS B produce loop, where each pinned ambiguity
 * column takes one diagonal local prior row of the same shape as the phase-leveling
 * carry row:
 *
 *     Λ = 1 / σ², η = Λ · (pin.value_m − x_lin(amb_idx)).
 *
 * Shifting `x_lin` alone is an exact solve no-op (docs/SPEC_lane_slip_response.md
 * §7.2), because the carried prior's RHS absorbs the shift.  The row above is a real
 * (Λ, η) pair and therefore actuates; the pin does not also write
 * @ref LocalCarry::value_abs.
 *
 * σ defaults to 1e-4 m (about 0.0005 cycles, weight 1e8): stiff, but two decades
 * below the 1e10 datum pins, so the gauge stays with the anchors.
 *
 * The row is independent of the leveling carry: it is applied whether or not the arc
 * has a @ref LocalCarry entry, whether or not the slip response freed the arc this
 * epoch, and regardless of the F3 local block.
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
#include <cstdint>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppUtils;
class NetworkRunTraces;
struct NetworkRunContext;


/**
 * @brief Run-scope state for the simulation-only oracle ambiguity pin.
 *
 * Constructed only when at least one `oracle_pin` event is parsed
 * (@ref networkOraclePinLoad); null on every other run, so the off path is
 * structurally byte-identical.  Non-copyable: a value copy would duplicate the
 * `ever_pinned` ledger and misreport the first-pin and release edges.
 */
struct NetworkOraclePin {
    NetworkOraclePin() = default;
    NetworkOraclePin(const NetworkOraclePin&) = delete;
    NetworkOraclePin& operator=(const NetworkOraclePin&) = delete;
    ~NetworkOraclePin();

    /// One arc's truth: the day's pre-slip integer, the slips applied to it, and the
    /// wavelength.  @ref lambda_m is the run's wavelength (from @ref utils, correct
    /// for FDMA through the satellite id), not the CSV's column; the CSV value is
    /// stored but not used.
    struct Truth {
        int    n_int{0};
        double lambda_m{0.0};        ///< The run's wavelength (from @ref utils); the pin's metres.
        double lambda_csv_m{0.0};    ///< The fixture's own column; stored, not compared or used.
        std::vector<NetworkSlipOracle::TruthSlip> slips;   ///< Ascending in @c s.
    };

    std::string truth_root;   ///< `network.cycle_slip.oracle_truth_root`.

    /// (station index, satellite, signal) -> that arc's truth.
    std::map<std::tuple<int, std::string, std::string>, Truth> truth;

    /// Wavelength provider (correct for FDMA).  Owned here so the pin does not
    /// depend on whether another card armed a shared provider.
    std::unique_ptr<PppUtils> utils;

    /// Every armed `oracle_pin` event's (index into `network.inject.events`, parsed
    /// scope, σ, tow).  An arc is pinned when any scope whose event has started covers
    /// it; the first such event (lowest event index) owns the row's σ and event index.
    struct Armed {
        std::size_t    ev_index{0};
        OraclePinScope scope;
        double         sigma_m{1.0e-4};
        double         tow{-1.0};
    };
    std::vector<Armed> scopes;

    // ---- This epoch's pin table (rebuilt by networkOraclePinEpoch) -----------
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
    long long rows_injected{0};        ///< Arc-epochs folded as a prior row.
    long long arcs_first_pinned{0};    ///< Distinct arcs entering the table for the first time.
    long long arcs_released{0};        ///< Arcs pinned last epoch that are absent this epoch.
    long long no_anchor{0};            ///< In scope, but the family had no usable anchor this epoch (arc-epochs).
    long long no_truth{0};             ///< In scope, but the arc's truth wavelength is not positive (arc-epochs); arcs with no truth row are never visited.
    long long no_station_term{0};      ///< In scope, but station r's band-j offset u could not be fitted (< 3 arcs).
    long long no_column{0};            ///< Table hit at PASS B, but the ambiguity column was not in this epoch's A_r.
    long long gauge_pinned_skipped{0}; ///< Table hit at PASS B on a gauge-pinned (anchor) arc; never folded.
    long long fit_iters_total{0};      ///< Sum of determination rounds over active epochs.
    long long fit_iters_last{0};       ///< Determination rounds of the last active epoch (1 = every station resolved directly from the root anchor).
    double    fit_resid_rms_cyc{0.0};  ///< Last active epoch: rms over pinned arcs of (value_abs − pinned)/λ.
    double    fit_resid_max_cyc{0.0};  ///< Last active epoch: max |…| of the same.
    long long fit_resid_n{0};          ///< Last active epoch: arcs the residual was measurable on.

    /// Arcs ever in the table; drives @ref arcs_first_pinned.
    std::set<std::tuple<int, std::string, std::string>> ever_pinned;
    /// The previous epoch's table keys.  Both edges of invariant #7 are taken against
    /// it, and both emit a per-arc `network_events.csv` row (action `pinned` /
    /// `released`), so the pinned set at any epoch is (entries − exits) up to that
    /// epoch.  The counters alone are not enough: membership reconstructed from the
    /// onset rows misses every arc that joined later.
    std::set<std::tuple<int, std::string, std::string>> prev_keys;
};

/**
 * @brief Parse one `truth_ambiguities.csv`.
 *
 * Header contract (src/sim/main.cpp): `satellite,signal,N_int,wavelength_m`.  The
 * header line must begin with these four names in this order, so a column inserted
 * before or among them fails here instead of being misread as `N_int`.  A column
 * appended after `wavelength_m` passes the check and is ignored.
 *
 * @param path  File to read.
 * @return (satellite, signal) -> (N_int, wavelength_m from the CSV).
 * @throws std::runtime_error on a malformed header or row, or a duplicate arc.
 */
std::map<std::pair<std::string, std::string>, std::pair<int, double>>
networkOraclePinParseAmbCsv(const std::string& path);

/**
 * @brief The true integer of an arc at continuous-GPS second @p now_s.
 *
 * `N(t) = N_int + Σ delta_cycles over every truth slip with s <= t`.  The bound is
 * inclusive, so a slip stamped exactly at @p now_s counts (the observation at that
 * epoch already carries it).
 */
int networkOraclePinTrueN(const NetworkOraclePin::Truth& t, double now_s);

/**
 * @brief Build the pin for this run: read every roster station's
 *        `truth_ambiguities.csv` + `truth_slips.csv` and index them by
 *        (station index, satellite, signal).
 *
 * Call only when `network.inject.events[]` holds at least one `oracle_pin` event.  A
 * missing per-station file means a mis-pointed root, not a quiet station, and is a
 * hard error; this is the field-inertness mechanism.  An arc whose wavelength the
 * provider cannot resolve is dropped and counted in the arming log line.
 *
 * @param config_      Run config (reads `network.inject` + `network.cycle_slip.oracle_truth_root`).
 * @param stations     Joint-solve roster, in station-index order.
 * @param log_manager  For the wavelength provider.
 * @param log          Run logger.
 * @return The constructed pin; never null on success.
 * @throws std::runtime_error when no `oracle_pin` event is armed, on a missing root,
 *         a missing or unreadable station file, a malformed row, an unparseable scope,
 *         or a σ that is not positive and finite.
 */
std::unique_ptr<NetworkOraclePin> networkOraclePinLoad(
    const GnssConfig& config_,
    const std::vector<NetStation>& stations,
    LogManager& log_manager,
    const std::shared_ptr<spdlog::logger>& log);

/**
 * @brief The fit: rebuild this epoch's pin table from truth and the run's own frame.
 *
 * Separate from @ref networkOraclePinEpoch so the unit suite can drive it on a
 * synthetic carry with no estimator.  Resolves the per-(station, band) offsets by the
 * rooted determination described in the file header, then writes the in-scope
 * non-anchor arcs into @c pin.table.
 *
 * @param pin        Run-scope state (table and counters are written).
 * @param stations   Roster (names, `pinned_amb_targets`).
 * @param carry      `ctx.carry.local_carry`: the fit's observations
 *                   (@ref LocalCarry::value_abs, never `x_lin`).
 * @param anchor_of  `ctx.carry.this_epoch_anchor`: "<sat>_<sig>" -> anchor station name.
 * @param now_s      Continuous-GPS seconds of this epoch.
 * @param active     Indices into @c pin.scopes whose event has started.
 * @return Determination rounds the fit ran (0 when nothing beyond the roots was
 *         resolvable).  At most one round per station, because each station's offset
 *         is estimated once and then frozen.
 */
int networkOraclePinFit(
    NetworkOraclePin& pin,
    const std::vector<NetStation>& stations,
    const std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
    const std::map<std::string, std::string>& anchor_of,
    double now_s,
    const std::vector<std::size_t>& active);

struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief Per-epoch driver: decide whether any event has started, rebuild the table,
 *        update the first-pin and release edges, and emit the trace rows.
 *
 * Call serially, once per epoch, after the phase-anchor election and after column
 * registration, and before PASS B.  While no event is due, the table and the
 * previous-epoch keys are cleared and no counter moves.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.setup.stations (stations),
 *            run.log (log).
 * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se.time (t).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx.
 *   - @p config_ = run.config.
 *   - @p stations = run.setup.stations.
 *   - @p t = ep.se.time.
 *   - @p log = run.log.
 *   - @p traces = run.traces.
 */
void networkOraclePinEpoch(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Run-end clause (one log line, every counter of @ref NetworkOraclePin).
 *
 * Standing invariant #4: a run with `rows_injected == 0` has measured nothing, and
 * the line states that in-band.
 *
 * @return 0 always.  An event that never fired is reported by the injector's own
 *         run-end check, in which this kind participates unchanged.
 */
int networkOraclePinRunEnd(const NetworkOraclePin* pin,
                           const std::shared_ptr<spdlog::logger>& log);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the sim-only oracle ambiguity pin.
 *
 * The table's row for the sim-only oracle ambiguity pin; it returns the report's result.
 *
 * Gate: none of its own: the row calls the report above on every run, as the teardown
 * did, and that report applies its own.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return the oracle pin report's result, 0 always (it enters the exit-code combination
 * and leaves it unchanged).
 */
int networkOraclePinRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py) ----

struct StationBuild;                       // filter/NetworkStationBuild.hpp (card NOM3 step PB1b)
struct StationCarryFrame;                  // filter/NetworkStationBuild.hpp
enum class StationStep : std::uint8_t;     // filter/NetworkStationBuild.hpp

/**
 * @brief The simulation-only oracle ambiguity pin's diagonal prior rows.
 *
 * Step `oracle_pin` of kStationCarrySteps (filter/NetworkStationBuild.hpp); its kinds:
 * PriorAmbiguityOraclePin. It returns at once unless the frozen enclosing conditions
 * legacy_local_inject and `opin_on`. Records: opin_*. The frozen text of networkPassB_'s
 * produce loop below its bindings (card NOM3 step PB1b).
 * @param carry The station's carry frame (the build, the priors, the coupling builder, the
 *              block columns).
 */
void networkOraclePinPriors(StationCarryFrame& carry);

// ---- End of the PASS B station steps, declarations (card NOM3 step PB1b) ----

} // namespace GnssProcessor
