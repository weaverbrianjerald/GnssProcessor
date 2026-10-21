#pragma once
/**
 * @file PrefitCensus.hpp
 * @brief Prefit innovation census, structural fuse, seed-birth tripwire and
 *        estimation-topology ledger of the network producer.
 *
 * ## Why a prefit instrument
 *
 * The producer monitors the post-fit σ̂₀ (unit-weight standard deviation).  Free
 * ambiguities and free local states absorb inconsistency, so it never reaches the
 * post-fit residuals, and the post-fit phase pool can be identically zero.  A
 * persistent large code innovation on one cell, folded at full weight every epoch,
 * is then invisible to the producer.  The within-epoch RAIM tier
 * (`network.raim_within_epoch`, receiver autonomous integrity monitoring) also
 * screens rows, but it runs only once the phase-leveling carry has warmed up and is
 * inert with phase leveling off.
 *
 * ## The metric: common mode removed, not raw
 *
 * A raw |y| of order 10⁵ m is not by itself a fault.  Receiver clock offsets and
 * steps are millisecond-scale: a 1 ms step puts |y| ≈ 3·10⁵ m on every row of the
 * station for one epoch, and the white receiver-clock state absorbs it in the
 * solve.  A raw-|y| screen would fire on every such step.
 *
 * The statistic is therefore computed per station and per row type, robustly
 * centred.  Here y is a row's prefit innovation and w its information weight:
 * @f[
 *   m_t = \operatorname{median}_{i \in t}\, y_i, \qquad
 *   s_i = |y_i - m_t|\,\sqrt{w_i},
 * @f]
 * which is in effect a between-satellite single difference and is free of the
 * receiver clock.  A whole-station clock step cancels to about 0, while a single
 * bad code row keeps its full deviation.
 *
 * A cell is the rows of one satellite (one line of sight, LOS) at one station.
 * Rows of class @c iono_sh_row tie the spherical-harmonic (SH) ionosphere model to
 * the slant ionosphere states.  They carry no receiver clock and are not centred
 * (m ≡ 0).  A type with fewer than @ref kNetPrefitMinTypeRows rows has no
 * trustworthy centre: the type is skipped for that station-epoch and the skip is
 * counted (it is never treated as m = 0).
 *
 * ## Scope
 *
 * The census flags magnitude.  It cannot by itself separate a parked bad cell
 * from a storm extreme on a well-checked cell; that discrimination is the
 * cross-flag of the companion redundancy census (network/qc/RedundancyCensus.hpp).
 * The prefit census is the cheap trigger; the redundancy census gives the targeted
 * verdict.
 *
 * ## Births are not exceedances
 *
 * Centring removes a station-wide level such as an unabsorbed receiver-clock
 * offset on the phase rows.  It does not handle a cell born this epoch inside
 * such a station.  The birth seed gives a fresh ambiguity the value that makes the
 * cell's own innovation about 0, which against an established station level of
 * 10⁵ m reads as s ≈ 10⁷.  Counted as exceedances, healthy births would dominate
 * the event stream.
 *
 * The census statistic and events therefore cover established cells only.  Every
 * born cell is handed to the seed-birth tripwire, which tests births at a tighter
 * threshold and puts the seed values in its event detail.  Both halves are counted
 * (@c rows_born_deferred, @c cells_born_deferred), so the hand-off is visible.
 * Births are detected whenever the walk runs (that is, when any of
 * `network.trace_prefit_census`, `network.seed_birth_check_sigma` or
 * `network.trace_topology` is on), not only when the tripwire is armed.  The
 * deferral and the census numbers thus do not depend on the tripwire's
 * configuration.
 *
 * ## Components
 *
 *  - Census, @c network.trace_prefit_census: per-class epoch statistics in
 *    `network_epoch_status.csv` and PREFIT-tier `prefit_exceedance` events.
 *    Measure-only.
 *  - Structural fuse, @c network.prefit_fuse_sigma: the only one of these four
 *    that changes the solve.  A row over @f$S_\text{fuse}@f$ has its weight capped
 *    to @f$w\,(S_\text{fuse}/s)^2@f$, so it contributes at most an
 *    @f$S_\text{fuse}@f$-equivalent standardized load.  It is a cap, not a drop:
 *    order-preserving, smooth, with the sign retained.  It is not robustness
 *    tuning: Huber weighting covers the 3 to 10σ regime, while the fuse targets the
 *    10³σ and larger regime, where the "measurement" is a wiring, seed or model
 *    fault.
 *  - Seed-birth tripwire, @c network.seed_birth_check_sigma: the same statistic at
 *    a tighter threshold, applied only to LOS whose per-LOS states were born this
 *    epoch.  A correctly self-seeded birth has s ≈ 0 by construction, so any fire
 *    localizes a seed-path defect to one epoch and site.  Measure-only.
 *  - Topology ledger, @c network.trace_topology: estimation-topology changes as
 *    events with identities.  These are per-station SH-tie row-count deltas,
 *    per-(station, satellite) row-kind changes while the satellite stays present,
 *    and shared-column registry deltas.  Measure-only.
 */

#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "network/assembly/RowClassify.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

class GnssConfig;
class NetworkEstimator;
struct GnssTime;
struct NetworkRunContext;
struct NetworkRunTraces;

/// Minimum rows a station's DATA type needs before its median is trusted as a
/// common-mode centre.  Below it the type is skipped for that station-epoch and
/// counted: the median of two rows would be the outlier it is meant to remove.
inline constexpr int kNetPrefitMinTypeRows = 4;

/// Weight at or above which a row is a pin or hard constraint rather than data.
/// The fuse skips these rows: a healthy gauge pin runs at s ≈ 1e7 by design.
inline constexpr double kNetPrefitPinWeight = 1e6;

/// @brief Bit of one DATA class in a class mask, as used by
///        @c network.prefit_fuse_classes, @c network.prefit_event_classes and
///        @c network.h_sigma0_classes.
/// @param c The row class.
/// @return `1 << class index` for a DATA class, 0 for a pin or coupling class.
inline constexpr unsigned dataClassBit(NetRowClass c) {
    return isDataRowClass(c) ? (1u << static_cast<unsigned>(c)) : 0u;
}

/**
 * @brief Parse a DATA-class token list into a class mask.
 *
 * Tokens (case-insensitive, separated by comma, space, tab, `+` or `|`):
 * @c code, @c phase, @c other, @c sh (also @c iono_sh_row, @c iono), plus
 * @c all and @c none.  Unknown tokens are ignored and appended to @p unknown so
 * the loader can warn with the offending text; a misspelt class dropped without
 * a warning would silently widen or narrow the domain of a fuse, an event channel
 * or the H sigma0 pool.
 *
 * @param spec     the raw config string.
 * @param unknown  [out, optional] appended with each unrecognized token.
 * @return the mask (0 = empty domain: no class selected).
 */
unsigned parseDataClassMask(const std::string& spec,
                            std::vector<std::string>* unknown = nullptr);

/// @brief Human-readable rendering of a DATA-class mask, as the class names joined
///        by `+` ("obs_code+iono_sh_row"), or "none" for an empty mask.
/// @param mask A class mask (see @ref dataClassBit).
/// @return The rendering.
std::string dataClassMaskName(unsigned mask);

/// Run-total tallies of the census, fuse, seed-birth tripwire, topology ledger and
/// flight recorder.  A zero finding means something only next to its denominator
/// (@c epochs, @c rows, @c rec_consults): inert wiring shows only through these.
struct NetworkPrefitCensusTallies {
    // --- census walk ---------------------------------------------------------
    long long epochs = 0;              ///< census/topology walk passes (the denominator).
    long long station_epochs = 0;      ///< (station, epoch) blocks walked.
    long long rows = 0;                ///< DATA rows walked.
    long long type_skips = 0;          ///< (station, epoch, type) medians skipped for fewer than kNetPrefitMinTypeRows rows.
    long long nonfinite_rows = 0;      ///< rows skipped for a non-finite y or w.
    long long exceedances = 0;         ///< DATA rows over prefit_event_sigma.
    long long events = 0;              ///< PREFIT exceedance events emitted.
    long long events_held = 0;         ///< events suppressed by the per-cell holdoff.
    /// Exceedances (birth exceedances included) on a class outside
    /// network.prefit_event_classes.  They are suppressed and counted, so a narrowed
    /// event domain stays visible.
    long long events_class_skipped = 0;
    /// DATA rows kept out of the census statistic because their cell was born this
    /// epoch.  The seed-birth tripwire tests them instead (see "Births are not
    /// exceedances" in the file header).
    long long rows_born_deferred = 0;
    long long cells_born_deferred = 0; ///< (cell, class) exceedances handed to the seed-birth tripwire instead of emitting an event.
    double    max_s = 0.0;             ///< run maximum standardized deviation.
    double    max_s_tow = -1.0;        ///< its epoch.
    std::string max_s_id;              ///< its "STATION|SAT|class".

    // --- structural fuse -----------------------------------------------------
    long long fuse_epochs = 0;         ///< assembly passes with the fuse armed.
    long long fuse_station_epochs = 0; ///< (station, epoch) blocks screened.
    long long fuse_rows = 0;           ///< DATA rows screened.
    long long fuse_capped = 0;         ///< rows whose weight was capped.
    long long fuse_events = 0;         ///< prefit_fused events emitted.
    long long fuse_events_held = 0;    ///< events suppressed by the per-cell holdoff.
    long long fuse_skip_pin_w = 0;     ///< rows skipped: w >= kNetPrefitPinWeight.
    long long fuse_skip_thin = 0;      ///< rows skipped: fewer than kNetPrefitMinTypeRows rows of their type.
    long long fuse_skip_class = 0;     ///< rows skipped: their DATA class is outside network.prefit_fuse_classes.
    long long fuse_would_class = 0;    ///< of those, the rows that would have been capped: the measured cost of the domain restriction.
    double    fuse_max_s = 0.0;        ///< largest pre-cap s the fuse acted on.
    double    fuse_min_ratio = 1.0;    ///< smallest w_eff/w applied (the deepest cut), with w_eff the capped weight.

    // --- seed-birth tripwire -------------------------------------------------
    long long births = 0;              ///< (station, satellite) LOS born, summed over epochs.
    long long birth_rows = 0;          ///< DATA rows belonging to a born LOS.
    long long birth_events = 0;        ///< seed_birth_exceedance events emitted.
    /// (station, satellite) cells present on the first walked epoch.  They are not
    /// treated as births because there is no earlier presence map to compare with.
    long long birth_seed_suppressed = 0;

    // --- topology ledger -----------------------------------------------------
    long long topo_tie_events = 0;     ///< per-station SH-tie row-count deltas.
    long long topo_kind_events = 0;    ///< per-(station, sat) row-kind changes.
    long long topo_col_add = 0;        ///< shared columns added vs the previous epoch.
    long long topo_col_rm = 0;         ///< shared columns removed vs the previous epoch.
    long long topo_truncated = 0;      ///< column-delta events dropped by the per-epoch cap (counted and logged).

    // --- flight recorder (network/qc/RhsAttrib.hpp) --------------------------
    long long rec_consults = 0;        ///< trigger-site latch consultations (0 means no site ever consulted it).
    long long rec_trips = 0;           ///< rising edges that armed a dump.
    long long rec_dumps = 0;           ///< attribution dumps written.
    long long rec_prev_dumps = 0;      ///< dumps that used retained previous-epoch frames.
    long long rec_frames_retained = 0; ///< epochs captured into the retention slot.
    long long rec_prev_bytes_last = 0; ///< bytes of the most recent retained frame.
};

/// The per-DATA-type robust centres of one station's assembled rows.
struct PrefitStationCentres {
    double    m[kNetDataClasses]{};      ///< the centre (0 for @c iono_sh_row).
    bool      have[kNetDataClasses]{};   ///< the type has a usable centre this epoch.
    long long n[kNetDataClasses]{};      ///< finite rows seen per type.
};

/**
 * @brief Per-type robust centres of one station's DATA rows.
 *
 * Centred types are @c obs_code / @c obs_phase / @c obs_other (median of @c y,
 * requiring @ref kNetPrefitMinTypeRows rows); @c iono_sh_row is not centred
 * (m ≡ 0) because it carries no receiver clock, and it has a usable centre
 * whenever it has one finite row.  Rows with a non-finite y or w never enter a
 * median.  The rows are classified from their tags (@ref classifyRow).
 *
 * @param R The station's assembled block, with its row tags.
 * @return The per-type centres.
 */
PrefitStationCentres prefitStationCentres(const StationAssembly& R);

/// @brief Standardized prefit deviation @f$s = |y - m|\sqrt{\max(w,0)}@f$.
/// @param y Prefit innovation of the row.
/// @param w Information weight of the row.
/// @param m Robust centre of the row's type (see @ref prefitStationCentres).
/// @return s.  Non-finite inputs yield 0 (the caller counts those separately
///         instead of propagating a NaN into a maximum).
double prefitStandardizedDeviation(double y, double w, double m);

/**
 * @brief Pooled robust prefit σ̂₀ for the sigma0 field of the H record
 *        (@c network.h_sigma0_prefit_census).
 *
 * The H record is the per-epoch status record of the product file
 * (@c network.emit_epoch_status_record).  This function makes one light pass over
 * the epoch's assembled rows with the statistic, centres and row classes of the
 * census (per-station per-type robust centres, @f$s = |y-m|\sqrt{w}@f$).  It pools
 * s across stations over the classes in @p class_mask and reduces the pool to
 * @f$\hat\sigma_0 = 1.4826\cdot\mathrm{median}(s)@f$.  The rules for the pool:
 *
 *  - @c obs_phase should stay out of the mask (the default of
 *    @c network.h_sigma0_classes is "code,sh,other"): phase prefit innovations are
 *    arc-specific, not a common-mode population;
 *  - pin-regime rows (w ≥ @ref kNetPrefitPinWeight) are excluded;
 *  - rows of a type with no usable centre are excluded (the census walk counts
 *    these);
 *  - cells born this epoch are excluded where the row's tag names the satellite
 *    (see "Births are not exceedances" in the file header).  Births are read from
 *    the previous epoch's presence map, which the census walk maintains.  For this
 *    reason the loader forces this knob off unless @c network.trace_prefit_census
 *    (and @c network.emit_epoch_status_record) is armed.  SH tie rows are never
 *    excluded this way, although their tags name their lines of sight: they are
 *    structural rows, not per-cell births.
 *
 * It runs before the solve (see @ref networkCensusSigma0), so every H site,
 * including the pre-solve observability-gate withhold, reads the same per-epoch
 * value.  Returns 0.0 on an empty pool, which the H record reads as "unavailable".
 * Measure-only: reads @p asm_res, writes nothing.
 *
 * @param ctx          Run context (the previous epoch's presence map).
 * @param stations     Station roster.
 * @param est          The estimator (shared-column count).
 * @param asm_res      The epoch's station assemblies.
 * @param est_index_of Station index to estimator slot (-1 = not folded this epoch).
 * @param class_mask   DATA classes to pool (see @ref parseDataClassMask).
 * @return The pooled σ̂₀, or 0.0 when no row qualifies.
 */
double networkPrefitSigma0(const NetworkRunContext& ctx,
                           const std::vector<NetStation>& stations,
                           const NetworkEstimator& est,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           unsigned class_mask);

/**
 * @brief Structural fuse: cap the weight of one station's over-threshold DATA rows.
 *
 * Called from the serial stage @ref networkPrefitFuseEpoch, the one point where
 * every row appender has finished writing @c R.w and the station is not yet added
 * to the estimator.  The event order is therefore deterministic and independent of
 * the thread count.
 *
 * Exclusions, all counted: rows with @c w >= @ref kNetPrefitPinWeight (pin /
 * constraint regime), rows whose type had no usable centre, and rows whose class
 * is outside @c network.prefit_fuse_classes.  Single-frequency cells are not
 * excluded as a class: a 10³σ common-removed excursion is ≳1800 TECU per 30 s
 * even at code noise, which is not physical ionosphere, so the cap does not
 * affect genuine single-frequency storm dynamics.
 *
 * @param ctx      Run context (counters and the per-cell event holdoff).
 * @param traces   Receives the PREFIT-tier `prefit_fused` events.
 * @param t        This epoch (event stamp).
 * @param n_shared This epoch's shared-column count (@c est.numShared()); a block of
 *                 another width is skipped.
 * @param station  Station name (event identity).
 * @param R        [in/out] the station's assembled block; only @c w is written.  Its
 *                 rows are classified from their tags (@ref classifyRow).
 * @param s_fuse   The threshold @f$S_\text{fuse}@f$ (> 0; the caller gates on it).
 * @return         Rows capped.
 */
int networkPrefitFuseStation(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, Eigen::Index n_shared,
                             const std::string& station, StationAssembly& R,
                             double s_fuse);

/**
 * @brief Post-solve census, seed-birth tripwire and topology ledger, in one walk
 *        over the epoch's assembled row stacks.
 *
 * Measure-only: it reads @p asm_res and writes only trace rows, events, run
 * counters, the per-epoch census scalars the epoch-status writer picks up, and
 * the carried presence and code-flag maps.  It runs in the frame-diagnostics slot
 * (SB_FRAMEDIAG), before the stash/status block, so those scalars are ready when
 * that block reads them.  The walk is skipped unless one of
 * `network.trace_prefit_census`, `network.seed_birth_check_sigma` or
 * `network.trace_topology` is on.
 *
 * When the fuse is armed it has already capped the weights this walk reads, so the
 * census reports the post-cap statistic and the pre-cap magnitude is in the
 * `prefit_fused` events.
 *
 * @param ctx           Run context (knobs, carry maps, counters).
 * @param config_       Full configuration (unused: arming is read from the knob
 *                      snapshot in @p ctx).
 * @param log           Run logger (warns when the walk is armed but classifies no
 *                      rows).
 * @param traces        PREFIT / TOPOLOGY events.
 * @param se            This synced epoch.
 * @param stations      Station roster (names + parameter sets for the birth detail).
 * @param est           The solved estimator (shared-column labels).
 * @param asm_res       PASS A/B station assemblies.
 * @param est_index_of  Station index to estimator slot (-1 = not folded this epoch).
 * @param events The epoch's events; writes census (the epoch-status scalars).
 */
void networkPrefitCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         const NetworkEstimator& est,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of, EpochEvents& events);

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Epoch form of networkPrefitCensus: forwards to the explicit overload above.
 *
 * The explicit overload stays for callers that have no NetworkRun
 * (tests/test_prefit_census.cpp).
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings:
 *            run.ctx (ctx), run.traces (traces). Read: run.config (config_),
 *            run.log (log), run.setup.stations (stations), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
 *           ep.assembly.asm_res (asm_res), ep.assembly.est_index_of (est_index_of).
 * @param events The epoch's events; writes census (the epoch-status scalars).
 */
void networkPrefitCensus(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report of the prefit tier: census, fuse, seed births, topology and
 *        flight recorder.
 *
 * Each section is printed only when its own knob is armed.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkPrefitCensusRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- Solve stage 1 ----

/**
 * @brief First stage of the network solve: computes this epoch's census-fed sigma0
 *        for the H record (ctx.carry.ep_h_sigma0_census).
 *
 * Runs once, before any H record is written, so every H site of the epoch (the
 * pre-solve withhold included) reads the same value.  Measure-only.  The value is
 * 0.0 when the feed (@c network.h_sigma0_prefit_census) is off or the pool is empty.
 *
 * @param run          The run: ctx.carry.ep_h_sigma0_census and
 *                     ctx.counters.h_sig0_census_nonzero are written.
 * @param ep           The epoch, read: assembly (asm_res).
 * @param est_index_of The solve's station-slot map (read).
 */
void networkCensusSigma0(NetworkRun& run, const NetworkEpoch& ep,
                         const std::vector<int>& est_index_of);

struct EpochPassA;              // network/driver/Epoch.hpp
struct EpochAssembly;           // network/driver/Epoch.hpp

// ---- PASS B serial stages, declarations ----

/**
 * @brief Apply the structural fuse to every assembled station of the epoch.
 *
 * This is the last writer of each data row's weight.  No-op unless
 * network.prefit_fuse_sigma > 0.  Called by networkPassB_ after the assembly tally
 * and before the assembly consume.
 *
 * @param run The run (network/driver/Run.hpp). Used: run.ctx (knobs and counters), run.setup
 *            (stations), run.traces (events), run.engines.est (shared-column count).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Used: ep.se (time),
 *           ep.pass_a.present (stations present this epoch).
 * @param assembly [in/out] The station blocks; only the row weights R.w are written.
 */
void networkPrefitFuseEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochAssembly& assembly);

// ---- End of the PASS B serial stages, declarations ----

/// @brief Run-scope counters of PrefitCensus, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct PrefitCensusCounters {

    /// Tallies of the census, fuse, seed-birth tripwire, topology ledger and flight
    /// recorder, reported by networkPrefitCensusRunEnd (network/qc/PrefitCensus.cpp).
    /// A tripwire that never fires looks like one that was never connected; only
    /// `epochs`, `rows` and `rec_consults` tell them apart.
    NetworkPrefitCensusTallies pc;

    // Counter of the census-fed H sigma0 (incremented by networkCensusSigma0).
    long long h_sig0_census_nonzero = 0;   ///< epochs with a usable census σ̂₀ on the H record (0 on a run with the feed armed means the feed is inert).

    // Prefit census warn latch.
    bool prefit_empty_logged = false;  ///< one-shot "armed but classified zero data rows" warn latch.
};

} // namespace GnssProcessor
