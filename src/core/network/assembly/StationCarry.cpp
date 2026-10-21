/**
 * @file StationCarry.cpp
 * @brief The station's carried-prior section: the frame, the walk of kStationCarrySteps, and the diagonal prior finalize.
 */

#include "network/assembly/StationCarry.hpp"
#include "network/assembly/StationBuild.hpp"   // StationBuild, StationCarryFrame
#include "network/driver/Epoch.hpp"
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/carry/Coupling.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "filter/StateKeySchema.hpp"
#include "rinex/MultiRinexSync.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "rinex/MultiRinexSync.hpp"
#include "network/solve/Estimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR: ils::ilsFix
#include "network/solve/Sbasis.hpp"
#include "network/assembly/ExtraBandScope.hpp"  // the extra-band / primary-pair predicates
#include "network/assembly/SecondCodeScope.hpp" // secondCodeCellEdges, the two cell edges
#include "network/assembly/SecondPhaseScope.hpp" // the cell edges + the witness
#include "network/solve/PivotHandover.hpp"   // pivot-handover datum-change detector
#include "network/carry/CarryVce.hpp"        // LS-VCE carry-row variance-component monitor
#include "network/carry/InvariantCarryDriver.hpp"  // DD cycle-basis invariant carry
#include "network/ar/SdEwlAr.hpp"               // the shared EWL band table (sdEwlBands) and the SD EWL integer decision
#include "network/carry/Coupling.hpp"       // conditional coupling-row builder
#include "network/assembly/ParamInventory.hpp" // the parameter inventory and the receiver IFB rule
#include "network/assembly/Contributions.hpp" // row tags and the contribution tallies
#include "network/qc/PrefitCensus.hpp"   // structural fuse
#include "network/diag/ShDevVtec.hpp"     // networkShDevCarryAge
#include "network/assembly/StationBuild.hpp"  // networkAssemblyTally
#include "network/solve/SolveStations.hpp" // the consume and the in-solve track
#include "network/solve/ChainDatum.hpp"    // networkEq34PinAudit
#include "network/ar/ArSuspect.hpp"      // arSuspectNote: the AR search's suspect-corner stamps
#include "filter/StateKeySchema.hpp"
#include "network/driver/RunTypes.hpp"      // run-loop record types: NetStation / StationAssembly / carry PODs
#include "network/qc/RobustStats.hpp"   // median/MAD robust attribution helpers
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs
#include "network/diag/RunTraces.hpp" // trace-stream ownership + open logic + roster + FlagRate
#include "network/state/RunContext.hpp"    // ctx: knobs + carry state + service counters
#include "network/driver/SetupResult.hpp"   // setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair
#include "slip/TurboEditResolver.hpp"   // per-signal MW+GF integer repair
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "network/slip/SlipQc.hpp"       // slip-QC pure helpers: ifElevMap / parseIfPairResetKey
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // identity reads (systemOfSat, ObservableId, ...), carriers
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to 1 thread inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun

namespace GnssProcessor {

// ---- PASS B station steps ----

StationStep networkStationCarriedPriors(StationBuild& build) {
    // ---- From the station build ----
    const NetworkRunContext& ctx = build.ctx;
    const GnssConfig& config_ = build.config;
    const NetworkEstimator& est = build.est;
    bool carry_warm = build.clock.carry_warm;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // The border kinds' process noise, from the parameter registry.
    const double q_clock = ctx.params.q(SharedParamKind::Clock);     // m²/s
    const double q_pb    = ctx.params.q(SharedParamKind::PhaseBias); // m²/s
    const double q_cb    = ctx.params.q(SharedParamKind::CodeBias);  // m²/s
    // Simulation-only oracle ambiguity pin (network/slip/OraclePin): this epoch's
    // pin table, built serially before PASS B and only read here.  A const pointer,
    // null on every run with no `oracle_pin` event armed, so the produce loop's pin
    // block is unreachable when off.
    const std::map<std::tuple<int, std::string, std::string>,
                   NetworkOraclePin::Pin>* opin_table =
        ctx.carry.oracle_pin ? &ctx.carry.oracle_pin->table : nullptr;
    const bool   opin_on = (opin_table != nullptr) && !opin_table->empty();
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // border time update of the coupling rows at injection, κ shrink + M²κq inflation; requires carry_cpl_cond (enforced at load)
    const bool   zwd_on        = ctx.knobs.zwd_on;                 // carry per-station ZWD across epochs
    // zwd_seam_live: the post-resume inject window is open; run the exact ZWD fold.
    // Stable across the parallel produce phase (the countdown decrements
    // serially at the post-solve harvest).
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    const bool   rx_cb_on      = ctx.knobs.rx_cb_on;             // rx-CB carry
    const bool   rx_cb_datum  = ctx.knobs.rx_cb_datum;           // zero-mean-over-receivers DCB datum; shares the pl_cb walk with the carry
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const net::SatSignalTable& sat_signals = ctx.entities.sat_signals;   // tu_reinit history
    // ---- End of the bindings ----

    // --- Phase-leveling carry: record present ambiguities and build the
    //     diagonal carried prior ----------------------
    // For every present non-pivot ambiguity: record it for the post-solve
    // stash and, if a converged carry exists and is not stale, build its
    // diagonal prior (Λ=1/var_eff, η=Λ·(value_abs−x_lin)) to inject into this
    // station's local block after addStation and before reduce.  Pivot
    // ambiguities are skipped under the legacy single-pivot datum (the prioritized
    // phase datum has no special pivot): their 1e10 pins define the phase-bias gauge
    // exactly, so a carried diagonal there is redundant.  Pure per-station
    // compute and a read-only local_carry lookup, no shared write, safe in
    // the parallel produce phase.
    // rx-CB carry: the legacy predicate is hoisted so the
    // receiver code-bias carry can enter this block without the carry_warm
    // (phase-leveling) gate; the legacy interior (the carry steps below) keeps its
    // original condition, so rx_cb_on=false is byte-identical.
    // zwd_seam_live joins the pivot-inclusion OR for the same reason zwd_on
    // is there: the ZWD carry stays out of any pivot/anchor exclusion, and
    // the seam window is the same fold on the same column.
    const bool legacy_local_inject =
        carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on
                       || zwd_seam_live);
    // The gate admits rx_cb_on and rx_cb_datum alongside
    // legacy_local_inject because their row builds run in the carry steps below
    // (the receiver code-bias datum rows in the pl_cb walk)
    // and their knobs are independent of both the legacy leveling carry and
    // the rx-CB carry.  Without them each would be inert wherever
    // legacy_local_inject is false (phase_leveling off, carry_invariant, and
    // the phase_leveling_warmup_epochs cold window at the head of every
    // leveled run) while the loader and the run-end line report it armed:
    // the armed-and-inert failure, and a contradiction of
    // the pl_cb walk's contract (pl_cb is filled whenever either knob is
    // armed).  opin_on is also admitted, but the oracle pin step itself
    // still requires legacy_local_inject.  opin_on is false when no oracle
    // pin is armed (null table pointer), so the gate is byte-identical there.
    if (!(legacy_local_inject || rx_cb_on || rx_cb_datum || opin_on)) return StationStep::Next;
    // The frame (network/assembly/StationBuild.hpp): the values the carry steps hand
    // each other (do_amb_carry: the ambiguity carry injects, off under carry_invariant).
    StationCarryFrame carry{build, legacy_local_inject,
                            (phase_datum_prioritized || !st.is_pivot) && !carry_invariant,
                            build.postUpdateState(),
                            CouplingRowBuilder{est, shc, q_clock, q_cb, q_pb,
                                               carry_cpl_tu,
                                               ctx.knobs.carry_cpl_gauge, ctx.knobs.carry_cpl_reinit,
                                               sat_signals, R}};
    const Eigen::VectorXd& x_lin = carry.x_lin;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;  // (col, Λ, η, tag): legacy diagonal path (the tag)
    // The pre-PD (before the positive-definiteness test) half of the same channel.  Rows land here
    // only under network.station_prior_before_pd, and then never in `inj`:
    // the two are folded by different calls that do not sum.  Empty when
    // off, so the finalize below is a no-op and R.prior_* stays empty.
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj_prior = carry.inj_prior;  // (col, Λ, η, tag): pre-PD datum rows
    // Conditional coupling (network.carry_conditional_coupling): each
    // carried local column becomes one pseudo-observation row coupling it to
    // the free shared border (Odijk S-conditional carry) instead of an
    // independent diagonal prior; see network/carry/Coupling.hpp.  The scratch
    // state and helpers are CouplingRowBuilder members, built fresh per
    // station (no shared write, safe in the parallel produce phase); the rows
    // are appended after the carry loops.
    CouplingRowBuilder& cb = carry.cb;
    // Re-admission re-coupling edge (network.readmit_coupling_marg_ep): the
    // open marginalization windows (written serially at the stash, read-only
    // here).
    if (ctx.knobs.readmit_marg_ep > 0 && net::present(ctx.entities.sats, &net::SatelliteState::readmit_marg) > 0) {
        cb.sats = &ctx.entities.sats;   // readmit_marg
        cb.loop_ep      = static_cast<long long>(ctx.counters.n_loop_epochs);
    }
    // The exact time update's epoch inputs (built serially before this
    // loop by networkCouplingExactPrepare; read-only here).
    if (ctx.knobs.carry_cpl_exact) {
        // The rows read the copy with each cloned clock on its clone
        // when it was built this epoch (rows_on is false off and on any epoch it
        // was not); the exact time update's own epoch otherwise.
        cb.cxu       = ctx.carry.ccl.rows_on ? &ctx.carry.ccl.rows_epoch
                                             : &ctx.carry.cxu_epoch;
        cb.cxu_cross = ctx.knobs.carry_cpl_exact_cross;
        if (ctx.knobs.carry_cpl_shx) cb.shx = &ctx.carry.shx;
    }
    // The carry steps, in table order (kStationCarrySteps).
    for (const StationCarryStep& s : kStationCarrySteps) s.build(carry);
    return StationStep::Next;
}

/**
 * @brief Sorts one diagonal-prior channel by column, coalesces duplicate columns and unpacks the
 *        result into the station's slot.
 *
 * Ascending column order gives a deterministic fold sequence. A column can receive two rows
 * (the receiver code-bias datum prior and its carry prior, an oracle pin on a carried
 * ambiguity column, or two datum rows on one receiver code-bias column), and information is
 * additive, so duplicates are summed into one entry rather than handed to the estimator
 * twice (its diagonal fold does not sum). Without a duplicate the vector is unchanged. The
 * tag of an entry merged into another goes to @p merged_tags, in merge order. An empty
 * channel leaves every output empty.
 *
 * @param entries     [in/out] The channel's (column, Lambda, eta, tag) entries; sorted and
 *                    coalesced in place.
 * @param merged_tags [out] Appended with the tag of each merged-away entry.
 * @param cols        [out] The surviving columns, in column order.
 * @param dNrr        [out] Their information increments.
 * @param dur         [out] Their right-hand-side increments.
 * @param tags        [out] Their tags.
 */
static void finalizeDiagonalPriors(
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& entries,
    std::vector<RowTag>& merged_tags, std::vector<Eigen::Index>& cols,
    Eigen::VectorXd& dNrr, Eigen::VectorXd& dur, std::vector<RowTag>& tags) {
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    if (entries.size() > 1) {
        std::size_t w = 0;
        for (std::size_t r = 1; r < entries.size(); ++r) {
            if (std::get<0>(entries[r]) == std::get<0>(entries[w])) {
                std::get<1>(entries[w]) += std::get<1>(entries[r]);
                std::get<2>(entries[w]) += std::get<2>(entries[r]);
                merged_tags.push_back(std::get<3>(entries[r]));
            } else {
                entries[++w] = entries[r];
            }
        }
        entries.resize(w + 1);
    }
    const Eigen::Index n = static_cast<Eigen::Index>(entries.size());
    cols.resize(static_cast<size_t>(n));
    dNrr.resize(n);
    dur.resize(n);
    tags.resize(static_cast<size_t>(n));
    for (Eigen::Index k = 0; k < n; ++k) {
        cols[static_cast<size_t>(k)] = std::get<0>(entries[static_cast<size_t>(k)]);
        dNrr(k) = std::get<1>(entries[static_cast<size_t>(k)]);
        dur(k)  = std::get<2>(entries[static_cast<size_t>(k)]);
        tags[static_cast<size_t>(k)] = std::get<3>(entries[static_cast<size_t>(k)]);
    }
}

void networkStationPriorFinalize(StationCarryFrame& carry) {
    // ---- From the station build ----
    StationBuild& build = carry.build;
    StationAssembly& R = build.R;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj_prior = carry.inj_prior;
    // ---- End of the bindings ----

    // Diagonal fold. Under carry_cpl_cond the ambiguity and ZWD carries
    // route through buildCplRow, so inj then holds only the diagonal-only
    // families (rx-CB carry, rx-CB datum, oracle pin);
    // with all of them off it is empty, and the slot's R.carry_* stay empty
    // (byte-identical).
    finalizeDiagonalPriors(inj, R.carry_merged, R.carry_cols, R.carry_dNrr, R.carry_dur,
                           R.carry_tags);
    // The same finalize for the pre-PD datum rows. Empty when off, so R.prior_*
    // is untouched.
    finalizeDiagonalPriors(inj_prior, R.prior_merged, R.prior_cols, R.prior_dNrr, R.prior_dur,
                           R.prior_tags);
}

// ---- End of the PASS B station steps ----

} // namespace GnssProcessor
