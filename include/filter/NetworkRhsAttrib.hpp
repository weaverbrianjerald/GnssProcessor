#pragma once
/**
 * @file NetworkRhsAttrib.hpp
 * @brief In-assembly reduced-RHS class attribution — the era-2 injector probe
 *        (project_ft63_fast_tier: "NEXT INSTRUMENT").
 *
 * Diagnostics-only, default-OFF, NOT fingerprinted (forward-only readout; stored
 * state meaning unchanged — the adaptive_iono_q precedent), so a shared-snapshot
 * replay leg with only this armed is A/B-legal against an unarmed control.
 *
 * ## What it answers
 *
 * At an armed target epoch the solve's reduced shared RHS
 * @f$\tilde u_s = u_s - \sum_r N_{sr} N_{rr}^{-1} u_r\ (+\ \text{DD folds})@f$
 * is decomposed EXACTLY by row class: which class of information — observation
 * rows (code / phase / slant↔SH ties), pivot pins, coupling rows, station-local
 * carry priors (rx-CB / RPB / ZWD / leveling), shared-info priors (D2 carries,
 * dense C1 block, Kaula, W6 code-bias gauge, clock-datum rows, phase-datum
 * prior), or the invariant-carry DD folds — loads a target shared column (the
 * two-layer SH means @c SH:C:0:0 / @c SH2:C:0:0) and, via the dumped per-class
 * full vectors, any offline direction (e.g. the NEQ-export weak-3).
 *
 * The split is exact because every term is linear in the RHS given the epoch's
 * final normal blocks: per station and class @f$c@f$,
 * @f$\tilde u_s^c = u_s^c - N_{sr}\,N_{rr}^{-1} u_r^c@f$ with the SAME
 * @f$N_{sr},N_{rr}@f$ (all classes' information folded), and the class vectors
 * sum to the kernel's reduced RHS.  The dump carries an explicit CLOSURE row
 * (max |Σ_classes − kernel|); a violated closure marks the epoch's attribution
 * as untrusted (e.g. a within-epoch RAIM replay rebuilt rows after the
 * captures) rather than silently lying — invariant-#4 discipline.
 *
 * ## Outputs (one set per armed epoch, under `<log_dir>/rhs_attrib/`)
 *
 *  - `attrib_<tow>.csv`  — per (class, station) reduced-RHS contributions to the
 *    target columns + family norms; per-pin datum rows; per-block DD rows;
 *    closure row.  Stations with |target contribution| in the top tier also get
 *    per-satellite sub-rows (second pass over just their rows).
 *  - `vec_<tow>.csv`     — per-class TOTAL reduced-RHS vectors over all shared
 *    columns (col, label, value) for offline projections.
 *
 * ## Knobs (network section; loader guards)
 *
 *  - @c network.rhs_attrib_tow   (double, default −1 = OFF): first target TOW.
 *  - @c network.rhs_attrib_n_ep  (int, default 2): consecutive target epochs.
 *  - @c network.rhs_attrib_on_trip (bool, default false): INC-5 FLIGHT RECORDER
 *    (docs/SPEC_prefit_census_observability.md §7) — arm the capture recorder
 *    every epoch and dump automatically at the first trip of a QC era (SH-gate
 *    engage, A4 σ̂₀ withhold, clock-runaway, a₀₀-band exit) instead of at a
 *    hand-picked TOW.  With @c rhs_attrib_on_trip_prev, the PREVIOUS epoch's
 *    retained frame is dumped too — the pre-trip frame is the informative one.
 *
 * Cost when armed: two extra per-station LDLᵀ factorization sweeps at the
 * target epochs only (~63 × O(n_r³) ≈ negligible); zero cost disarmed beyond
 * one double compare per epoch.  The INC-5 retention channel additionally
 * copies one epoch's assemblies + kernel blocks (tens to hundreds of MB — see
 * @ref NetworkAttribFrame) and is auto-disabled above
 * @c network.rhs_attrib_prev_max_stations.
 */

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Everything ONE epoch's reduced-RHS decomposition reads, detached from
 *        the live estimator (INC-5).
 *
 * The attribution is a pure function of (row stacks, per-station kernel blocks,
 * shared-info captures, DD-prior inputs, reduced system, datum pins).  Bundling
 * them makes the SAME code path serve the live epoch and a RETAINED previous
 * epoch — which is the whole point of the flight recorder: the frame BEFORE a
 * discharge is what names the injector (era-2's 329130 "before" frame is what
 * exposed the weight flip; the 329160 frame alone only showed the aftermath).
 *
 * Assembly ownership is deliberately optional.  On the live path @ref asm_ptr
 * points straight into the caller's `asm_res` (no copy at all); on the
 * retention path @ref asm_own holds deep copies and @ref asm_ptr points into
 * it.  Kernel blocks are ALWAYS owned: @c stationLocalNormals returns by value
 * (rebuilt from the cached LDLᵀ) and the rest are caches the next epoch
 * overwrites.
 *
 * Size at 63 stations / ~250 shared columns / ~200 local columns is order
 * 100 MB; hence @c network.rhs_attrib_prev_max_stations (default 128) turning
 * retention off for large networks rather than silently eating memory.
 */
struct NetworkAttribFrame {
    bool                     valid{false};   ///< A usable reduced system was captured.
    int                      week{0};
    double                   tow{0.0};
    Eigen::Index             n{0};           ///< shared-column count.
    std::vector<std::string> label;          ///< shared-column labels (size @ref n).
    Eigen::Index             c00_l1{-1};     ///< column of @c SH:C:0:0.
    Eigen::Index             c00_l2{-1};     ///< column of @c SH2:C:0:0.

    // --- per work item (one entry per station folded into the kernel) -------
    std::vector<std::string>      sta_name;  ///< station name.
    std::vector<std::size_t>      slot;      ///< kernel slot (@c est.addStation index).
    std::vector<StationAssembly>  asm_own;   ///< deep copies (retention path only).
    std::vector<const StationAssembly*> asm_ptr;  ///< the block each item reads.
    std::vector<Eigen::MatrixXd>  Nsr;       ///< @f$N_{sr}@f$ per item.
    std::vector<Eigen::MatrixXd>  Nrr;       ///< @f$N_{rr}@f$ per item (rebuilt from LDLᵀ).
    std::vector<Eigen::VectorXd>  ur;        ///< @f$u_r@f$ per item.

    // --- epoch-level ---------------------------------------------------------
    std::vector<NetworkEstimator::AttribCapture>        captures;   ///< shared-info folds (tags are literals).
    std::vector<NetworkNormalEquations::DdPriorBlock>   dd_blocks;  ///< DD-prior INPUT blocks.
    Eigen::VectorXd            b_red;   ///< kernel reduced shared RHS.
    Eigen::MatrixXd            N_red;   ///< kernel reduced shared normals (datum-pin section).
    NetworkEstimator::Datum    datum;   ///< the epoch's minimum-constraint pins.

    /// Approximate retained heap footprint in bytes (run-end telemetry).
    std::size_t bytes() const;
    /// Release everything (used when retention is disabled mid-run).
    void clear();
};

/**
 * @brief Capture ONE epoch into a @ref NetworkAttribFrame.
 *
 * @param out            [out] frame, overwritten wholesale.
 * @param se             this epoch (week/tow stamp).
 * @param stations       station roster (names).
 * @param est            the SOLVED estimator (kernel + captures must be current).
 * @param plan_datum     the epoch's minimum-constraint pins.
 * @param asm_res        PASS A/B station assemblies.
 * @param est_index_of   station → kernel slot (−1 = not folded).
 * @param own_assemblies deep-copy the assemblies into the frame (retention);
 *                       false = point at @p asm_res, valid only until it changes.
 */
void networkAttribCaptureFrame(NetworkAttribFrame& out, const SyncedEpoch& se,
                               const std::vector<NetStation>& stations,
                               const NetworkEstimator& est,
                               const NetworkEstimator::Datum& plan_datum,
                               const std::vector<StationAssembly>& asm_res,
                               const std::vector<int>& est_index_of,
                               bool own_assemblies);

/**
 * @brief Decompose and DUMP one captured frame (the attribution proper).
 *
 * Writes `attrib_<tow>.csv`, `rows_<tow>.csv` and `vec_<tow>.csv` under @p dir
 * exactly as the hand-armed path always has, plus the closure row.
 *
 * @param f    A frame captured by @ref networkAttribCaptureFrame.
 * @param log  Run logger (closure violation warn + the dump line).
 * @param dir  Output directory (created if absent).
 * @param kind Short tag naming WHY this frame is being dumped ("armed",
 *             "trip:sigma0", "trip-prev:sh_gate", …) — log text only, so the
 *             CSVs stay byte-comparable across dump reasons.
 */
void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind);

/**
 * @brief Post-solve reduced-RHS class attribution at the armed target epochs.
 *
 * Call AFTER the epoch's solve (the kernel's reduced system and the station
 * LDLᵀ blocks must be current) and after the postfit sweep, from the driver's
 * SB_FRAMEDIAG block.  No-op unless @c network.rhs_attrib_tow ≥ 0 and this
 * epoch's TOW lies in the armed window.
 *
 * @param ctx          Run context (counters only; owns no attribution state).
 * @param config_      Full configuration (reads @c network.rhs_attrib_*).
 * @param log          Main run logger (arming + closure lines).
 * @param traces       Run traces (provides the log directory for the dump files).
 * @param se           This synced epoch (week/tow stamp).
 * @param stations     Network station list (names for the dump).
 * @param est          The solved estimator (kernel accessors + capture recorder).
 * @param plan_datum   The epoch's minimum-constraint datum pins (from
 *                     @c plan.shared_datum) — attributed per pin.
 * @param asm_res      PASS A/B station assemblies (the row stacks to classify).
 * @param est_index_of Station → kernel slot map (−1 = not folded this epoch).
 */
void networkRhsAttribution(NetworkRunContext& ctx, const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           const NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations,
                           NetworkEstimator& est,
                           const NetworkEstimator::Datum& plan_datum,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of);

/**
 * @brief INC-5 flight recorder: consume a pending trip, then refresh the
 *        retained previous-epoch frame.
 *
 * Called from the SAME SB_FRAMEDIAG slot as @ref networkRhsAttribution, right
 * after it.  Two independent jobs, in this order:
 *
 *  1. If a trigger site raised @c ctx.carry.rhs_trip_pending this epoch, dump
 *     THIS epoch (bypassing the @c rhs_attrib_tow window) and — when
 *     @c rhs_attrib_on_trip_prev and a frame was retained — the PREVIOUS epoch.
 *  2. Retain THIS epoch for the next one (unless retention is off or the
 *     network is larger than @c rhs_attrib_prev_max_stations).
 *
 * No-op unless @c network.rhs_attrib_on_trip.  Measure-only: nothing here is
 * read back by the solve.
 */
void networkRhsAttribTrip(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          const NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          const NetworkEstimator& est,
                          const NetworkEstimator::Datum& plan_datum,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of);

/**
 * @brief INC-5 trigger-site helper — record a QC condition and report its
 *        RISING edge (the "first trip per era, one-shot until the era clears"
 *        rule of docs/SPEC_prefit_census_observability.md §7).
 *
 * Trigger sites call this UNCONDITIONALLY every epoch they evaluate their
 * condition — passing @p active false is what clears the era and re-arms the
 * latch.  Every call is counted (invariant #4: "armed but no trigger site ever
 * consulted the latch" must be distinguishable from "armed and nothing
 * happened"), so a site that only calls on the true branch would be a miswire.
 *
 * @param ctx    Run context (latch set + counters).
 * @param kind   Stable era name ("sh_gate", "sigma0", "clk_runaway", "a00_band").
 *               A string literal — stored by value, so any lifetime is fine.
 * @param active Whether the condition holds THIS epoch.
 * @return       true on the rising edge (inactive → active) while the recorder
 *               is armed; false otherwise.  A true return has already raised
 *               @c ctx.carry.rhs_trip_pending.
 */
bool networkRhsTripEdge(NetworkRunContext& ctx, const char* kind, bool active);

} // namespace GnssProcessor
