#pragma once
/**
 * @file NetworkPrefitCensus.hpp
 * @brief Prefit innovation census, structural fuse, seed-birth tripwire and
 *        estimation-topology ledger (INC-1..4 of
 *        docs/SPEC_prefit_census_observability.md).
 *
 * ## Purpose of a prefit instrument
 *
 * The producer monitors post-fit σ̂₀ (P1-4).  Free ambiguities and free local
 * states absorb inconsistency, so it does not reach the post-fit residual; the
 * post-fit phase pool can be identically zero.  A persistent large code
 * innovation on one cell, folded at full weight every epoch, is then invisible
 * to the producer.  The only other pre-solve row screen, `raim_within_epoch`, is
 * gated on carry_warm and is inert with phase leveling off.
 *
 * ## The metric: common mode removed, not raw
 *
 * A raw |y| of order 10⁵ m is not by itself a fault.  Receiver clock offsets and
 * steps are millisecond-scale: a 1 ms step puts |y| ≈ 3·10⁵ m on every row of the
 * station for one epoch, and the white receiver-clock state absorbs it in the
 * solve.  A raw-|y| screen would fire on every such step.
 *
 * The census statistic is therefore computed per station and per row type,
 * robustly centred:
 * @f[
 *   m_t = \operatorname{median}_{i \in t}\, y_i, \qquad
 *   s_i = |y_i - m_t|\,\sqrt{w_i},
 * @f]
 * which is in effect a between-satellite single difference and is free of the
 * receiver clock.  A whole-station clock step cancels to about 0, while a single
 * bad code row keeps its full deviation.  @c iono_sh_row rows carry no receiver
 * clock and are not centred (m ≡ 0).  A type with fewer than
 * @ref kNetPrefitMinTypeRows rows has no trustworthy centre: the type is skipped
 * for that station-epoch and the skip is counted (it is never treated as m = 0).
 *
 * ## Scope
 *
 * The census flags magnitude.  It cannot by itself separate a parked bad cell
 * from a storm extreme on a well-checked cell; that discrimination is the
 * cross-flag of the companion redundancy census (docs/SPEC_redundancy_census.md).
 * The census is the cheap trigger; the redundancy census gives the targeted
 * verdict.
 *
 * ## Births are not exceedances (INC-1 defers to INC-3)
 *
 * Centring removes a station-wide level such as an unabsorbed receiver-clock
 * offset on the phase rows.  It does not handle a cell born this epoch inside
 * such a station: the state initializer seeds a fresh ambiguity so that the
 * cell's own innovation is about 0, which against an established station level
 * of 10⁵ m reads as s ≈ 10⁷.  Counted as exceedances, healthy births would
 * dominate the event stream.
 *
 * INC-1's statistic and events therefore cover established cells only, and every
 * born cell is handed to INC-3, which tests births at a tighter threshold with
 * the seed values in its event detail.  Both halves are counted
 * (@c rows_born_deferred, @c cells_born_deferred), so the hand-off is visible.
 * Births are detected whenever the walk runs, not only when INC-3 is armed, so
 * the deferral and the census numbers do not depend on INC-3's configuration.
 *
 * ## Increments
 *
 *  - INC-1 @c network.trace_prefit_census: per-class epoch statistics into
 *    `network_epoch_status.csv` and PREFIT-tier `prefit_exceedance` events.
 *    Measure-only.
 *  - INC-2 @c network.prefit_fuse_sigma: the structural fuse, the only
 *    solve-affecting knob among INC-1..4.  A row over @f$S_\text{fuse}@f$ has its
 *    weight capped to @f$w\,(S_\text{fuse}/s)^2@f$, so it contributes at most an
 *    @f$S_\text{fuse}@f$-equivalent standardized load.  It is a cap, not a drop:
 *    order-preserving, smooth, with the sign retained.  It is not robustness
 *    tuning: Huber and the deweight gate cover the 3–10σ regime; the fuse targets
 *    the 10³σ and larger regime, where the "measurement" is a wiring, seed or
 *    model fault.
 *  - INC-3 @c network.seed_birth_check_sigma: the same statistic at a tighter
 *    threshold, applied only to LOS whose per-LOS states were born this epoch.
 *    A correctly self-seeded birth has s ≈ 0 by construction, so any fire
 *    localizes a seed-path defect to one epoch and site.  Measure-only.
 *  - INC-4 @c network.trace_topology: estimation-topology changes as events with
 *    identities: per-station SH-tie row-count deltas (the edges of the iono
 *    deweight valve), per-(station, satellite) row-kind changes while the
 *    satellite stays present, and shared-column registry deltas.  Measure-only.
 */

#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "filter/NetworkRowClassify.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

class GnssConfig;
class NetworkEstimator;
class NetworkIonoDeweightGate;
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

/// Bit of one DATA class in the fuse-domain mask (@c network.prefit_fuse_classes).
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
 * a warning would silently widen or narrow a solve-affecting domain.
 *
 * @param spec     the raw config string.
 * @param unknown  [out, optional] appended with each unrecognized token.
 * @return the mask (0 = fuse nothing).
 */
unsigned parseDataClassMask(const std::string& spec,
                            std::vector<std::string>* unknown = nullptr);

/// Human-readable rendering of a DATA-class mask ("code+sh", "none", …).
std::string dataClassMaskName(unsigned mask);

/// Run-total tallies of every prefit-census increment (INC-1..5).  Inert wiring is visible
/// only through these denominators (invariant #4).
struct NetworkPrefitCensusTallies {
    // --- census walk (INC-1) -------------------------------------------------
    long long epochs = 0;              ///< census/topology walk passes (the denominator).
    long long station_epochs = 0;      ///< (station, epoch) blocks walked.
    long long rows = 0;                ///< DATA rows walked.
    long long type_skips = 0;          ///< (station, epoch, type) medians skipped for n < 4.
    long long nonfinite_rows = 0;      ///< rows skipped for a non-finite y or w.
    long long exceedances = 0;         ///< DATA rows over prefit_event_sigma.
    long long events = 0;              ///< PREFIT exceedance events emitted.
    long long events_held = 0;         ///< …suppressed by the per-cell holdoff.
    long long events_class_skipped = 0;///< exceedances on a class outside network.prefit_event_classes: suppressed and counted, so a narrowed event domain stays visible.
    long long rows_born_deferred = 0;  ///< DATA rows withheld from the INC-1 statistic because their cell was born this epoch; INC-3 tests them at its own threshold (see "Births are not exceedances" in the file header).
    long long cells_born_deferred = 0; ///< (cell, class) exceedances INC-1 handed to INC-3 instead of emitting an event.
    double    max_s = 0.0;             ///< run maximum standardized deviation.
    double    max_s_tow = -1.0;        ///< its epoch.
    std::string max_s_id;              ///< its "STATION|SAT|class".

    // --- structural fuse (INC-2) --------------------------------------------
    long long fuse_epochs = 0;         ///< assembly passes with the fuse armed.
    long long fuse_station_epochs = 0; ///< (station, epoch) blocks screened.
    long long fuse_rows = 0;           ///< DATA rows screened.
    long long fuse_capped = 0;         ///< rows whose weight was capped.
    long long fuse_events = 0;         ///< prefit_fused events emitted.
    long long fuse_events_held = 0;    ///< …suppressed by the per-cell holdoff.
    long long fuse_skip_pin_w = 0;     ///< rows skipped: w >= kNetPrefitPinWeight.
    long long fuse_skip_thin = 0;      ///< rows skipped: their type had n < 4.
    long long fuse_skip_class = 0;     ///< rows skipped: their DATA class is outside network.prefit_fuse_classes.
    long long fuse_would_class = 0;    ///< …of those, the rows that would have been capped: the measured cost of the domain restriction.
    double    fuse_max_s = 0.0;        ///< largest pre-cap s the fuse acted on.
    double    fuse_min_ratio = 1.0;    ///< smallest w_eff/w applied (the deepest cut).

    // --- seed-birth tripwire (INC-3) ----------------------------------------
    long long births = 0;              ///< (station, satellite) LOS born, summed over epochs.
    long long birth_rows = 0;          ///< DATA rows belonging to a born LOS.
    long long birth_events = 0;        ///< seed_birth_exceedance events emitted.
    long long birth_seed_suppressed = 0;///< (station, satellite) cells present on the first walked epoch, not treated as births (no prior presence map).

    // --- topology ledger (INC-4) --------------------------------------------
    long long topo_tie_events = 0;     ///< per-station SH-tie row-count deltas.
    long long topo_kind_events = 0;    ///< per-(station, sat) row-kind changes.
    long long topo_col_add = 0;        ///< shared columns added vs the previous epoch.
    long long topo_col_rm = 0;         ///< …removed.
    long long topo_truncated = 0;      ///< column-delta events dropped by the per-epoch cap (counted and logged).

    // --- flight recorder (INC-5, filter/NetworkRhsAttrib) -------------------
    long long rec_consults = 0;        ///< trigger-site latch consultations (0 means no site ever consulted it).
    long long rec_trips = 0;           ///< rising edges that armed a dump.
    long long rec_dumps = 0;           ///< attribution dumps written.
    long long rec_prev_dumps = 0;      ///< …of which retained previous-epoch frames.
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

/// @return @f$s = |y - m|\sqrt{\max(w,0)}@f$, the card's standardized prefit
///         deviation.  Non-finite inputs yield 0 (the caller counts those
///         separately instead of propagating a NaN into a maximum).
double prefitStandardizedDeviation(double y, double w, double m);

/**
 * @brief Pooled robust prefit σ̂₀ for the sigma0 field of the H heartbeat
 *        (@c network.h_sigma0_prefit_census; H-rider, edge-queue §1.6 item 3a).
 *
 * One light pass over this epoch's assembled rows with the statistic, centres
 * and class taxonomy of the INC-1 census (per-station per-type robust centres;
 * @f$s = |y-m|\sqrt{w}@f$), pooled across stations over the classes in
 * @p class_mask and reduced to @f$\hat\sigma_0 = 1.4826\cdot\mathrm{median}(s)@f$.
 * It follows the census in these respects, so the wire statistic matches the
 * event population in `network_events.csv`:
 *
 *  - @c obs_phase should stay out of the mask by default (census deviation 2c:
 *    phase prefit innovations are arc-specific, not a common-mode population);
 *  - pin-regime rows (w ≥ @ref kNetPrefitPinWeight) are excluded;
 *  - rows of a type with no usable centre are excluded (the census walk counts
 *    these);
 *  - cells born this epoch are excluded where the row's tag names the
 *    satellite ("Births are not exceedances"), read from the lag-1 presence map
 *    the census walk maintains; for this reason the loader forces this knob off
 *    unless @c network.trace_prefit_census is armed.  SH tie rows are never
 *    deferred, although their tags name their lines of sight: they are structural
 *    rows, not per-cell births.
 *
 * Computed before the solve, at the top of @c networkSolveGlue, so every H site,
 * including the pre-solve observability-gate withhold, reads the same per-epoch
 * value.  Returns 0.0 on an empty pool, which is the H grammar's "unavailable"
 * sentinel.  Measure-only: reads @c asm_res, writes nothing.
 */
double networkPrefitSigma0(const NetworkRunContext& ctx,
                           const std::vector<NetStation>& stations,
                           const NetworkEstimator& est,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           unsigned class_mask);

/**
 * @brief Structural fuse (INC-2): cap the weight of one station's over-threshold
 *        DATA rows.
 *
 * Call from the serial site before @c addStation, the one point where every
 * appender has finished writing @c R.w, so the event order is deterministic and
 * independent of the thread count.
 *
 * Exclusions, all counted: rows with @c w >= @ref kNetPrefitPinWeight (pin /
 * constraint regime), rows whose type had no usable centre, and rows whose class
 * is outside @c network.prefit_fuse_classes.  Single-frequency cells are not
 * excluded as a class: a 10³σ common-removed excursion is ≳1800 TECU per 30 s
 * even at code noise, which is not physical ionosphere, so the cap does not
 * affect genuine single-frequency storm dynamics.
 *
 * @param ctx     Run context (counters + the per-cell event holdoff).
 * @param traces  PREFIT-tier `prefit_fused` events.
 * @param t       This epoch (event stamp).
 * @param n_shared This epoch's shared-column count (@c est.numShared()); a block of
 *                another width is skipped.
 * @param station Station name (event identity).
 * @param R       [in/out] the station's assembled block; only @c w is written.  Its
 *                rows are classified from their tags (@ref classifyRow).
 * @param s_fuse  The threshold @f$S_\text{fuse}@f$ (> 0; the caller gates on it).
 * @return        Rows capped.
 */
int networkPrefitFuseStation(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, Eigen::Index n_shared,
                             const std::string& station, StationAssembly& R,
                             double s_fuse);

/**
 * @brief Post-solve census, seed-birth tripwire and topology ledger
 *        (INC-1 / INC-3 / INC-4), in one walk over the epoch's assembled row stacks.
 *
 * Measure-only: it reads @c asm_res and writes only trace rows, events, run
 * counters, the per-epoch census scalars the epoch-status writer picks up, and
 * the carried presence and code-flag maps.  It is placed in SB_FRAMEDIAG, before
 * the stash/status block, so those scalars are ready when the block runs.
 *
 * Composition with INC-2: when the fuse is armed it has already capped the
 * weights this walk reads, so the census reports the post-cap statistic and the
 * pre-cap magnitude is in the `prefit_fused` events.  The fuse and the census
 * are validated as separate arms.
 *
 * @param ctx           Run context (knobs, carry maps, counters).
 * @param config_       Full configuration (the @c network.prefit_* / topology knobs).
 * @param log           Run logger (invariant-#4 dead-wiring warns).
 * @param traces        PREFIT / TOPOLOGY events.
 * @param se            This synced epoch.
 * @param stations      Station roster (names + state managers for the birth detail).
 * @param est           The solved estimator (shared-column labels).
 * @param asm_res       PASS A/B station assemblies.
 * @param est_index_of  Station → kernel slot (−1 = not folded this epoch).
 * @param iono_deweight The SH-constraint deweight gate, or nullptr.  Read-only;
 *                      supplies the per-station weight named in the INC-4
 *                      tie-row event (w_prev, w_now).
 */
void networkPrefitCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         const NetworkEstimator& est,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of,
                         const NetworkIonoDeweightGate* iono_deweight);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the prefit tier: census, fuse, seed births, topology, recorder, attribution.
 *
 * The prefit tier: the census walk, the fuse, seed births, the topology ledger, the
 * flight recorder and the right-hand-side attribution, each line under its own knob.
 *
 * Gate: prints always (a block).
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkPrefitCensusRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
