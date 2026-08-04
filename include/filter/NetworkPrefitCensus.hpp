#pragma once
/**
 * @file NetworkPrefitCensus.hpp
 * @brief INC-1..4 of docs/SPEC_prefit_census_observability.md — the PREFIT
 *        innovation census, the structural fuse, the seed-birth tripwire and
 *        the estimation-topology ledger.
 *
 * ## Why a PREFIT instrument at all
 *
 * The producer monitors POST-fit σ̂₀ (P1-4).  Free ambiguities and free local
 * states launder inconsistency out of the post-fit residual by construction —
 * the P1-4 line says so itself (the phase pool is identically zero on the FT63
 * stack).  Era-2's injector was a STEADY +1.88e5 m prefit code innovation on
 * (BSHM00ISR, G07), folded at w ≈ 2 every epoch for minutes, and nothing in the
 * producer could see it: `raim_within_epoch` — the one pre-solve row screen —
 * is carry_warm-gated and inert with leveling OFF.
 *
 * ## The metric: common-mode REMOVED, not raw
 *
 * A raw |y| of 188 km is not inherently absurd.  Receiver clock offsets and
 * steps are ms-scale — URUM00CHN and INEG00MEX take clean 1 ms steps on this
 * very leg, putting |y| ≈ 3·10⁵ m on EVERY row of the station for one epoch,
 * absorbed exactly by the white receiver-clock state in the solve.  A raw-|y|
 * screen would fire on those and miss nothing else; it would be a
 * false-positive generator with a fuse attached.
 *
 * So the census statistic is per station, per row TYPE, robust-centred:
 * @f[
 *   m_t = \operatorname{median}_{i \in t}\, y_i, \qquad
 *   s_i = |y_i - m_t|\,\sqrt{w_i},
 * @f]
 * which is a between-satellite single difference in all but name and is
 * receiver-clock-free by construction.  A whole-station clock step cancels to
 * ~0; BSHM's G07 code row stays at s ≈ 2.6·10⁵ because its station's code
 * median is ~10 m.  @c iono_sh_row rows carry no receiver clock and are NOT
 * centred (m ≡ 0).  A type with fewer than @ref kNetPrefitMinTypeRows rows has
 * no trustworthy centre: the type is SKIPPED for that station-epoch and the
 * skip is counted (never silently treated as m = 0).
 *
 * ## Scope
 *
 * The census flags MAGNITUDE.  It cannot by itself separate "parked garbage"
 * from "storm extreme on a well-checked cell" — that discrimination is the
 * companion redundancy card's cross-flag (docs/SPEC_redundancy_census.md).
 * The census is the free trigger; redundancy is the targeted verdict.
 *
 * ## Births are not exceedances (INC-1 yields to INC-3)
 *
 * The centring works — measured on the FT63 leg-19 replay, CAS100ATA's sixteen
 * phase rows all sit at 177037.1…177037.6 m (a 0.59 ms receiver-clock offset
 * the phase model has not absorbed) and every one of them centres to ~0; the
 * median |station phase level| across the network is 0.46 m.  What does NOT
 * centre is a cell BORN this epoch inside such a station: the state initializer
 * seeds a fresh ambiguity so its OWN innovation is ~0, which against an
 * established station level of 115 km reads as s ≈ 1e7.  Measured on a first
 * cut: 10077 exceedances and 666 events in one hour, essentially all of them
 * healthy births.  A tripwire that cries wolf 666 times an hour is not a
 * tripwire.
 *
 * So INC-1's statistic and its events cover ESTABLISHED cells only, and every
 * born cell is handed to INC-3 — which exists for exactly this question, at a
 * tighter threshold, with the seed values in its event detail.  Both halves are
 * counted (@c rows_born_deferred, @c cells_born_deferred) so the hand-off is
 * visible rather than a silent filter, and births are detected whenever the
 * walk runs — not only when INC-3 is armed, because the deferral must not
 * depend on whether anyone is listening on the other side.
 *
 * ## Increments
 *
 *  - INC-1 @c network.trace_prefit_census — per-class epoch statistics into
 *    `network_epoch_status.csv` + PREFIT-tier `prefit_exceedance` events.
 *    MEASURE-ONLY.
 *  - INC-2 @c network.prefit_fuse_sigma — the structural fuse.  The ONLY
 *    solve-affecting knob in this card: a row over @f$S_\text{fuse}@f$ has its
 *    weight capped to @f$w\,(S_\text{fuse}/s)^2@f$ so it contributes at most an
 *    @f$S_\text{fuse}@f$-equivalent standardized load.  A CAP, not a drop (the
 *    prefer-fallback directive): order-preserving, smooth, sign retained.  It
 *    is NOT robustness tuning — Huber and the deweight gate own the 3–10σ
 *    regime; the fuse exists for the 10³σ+ regime where a "measurement" is
 *    actually a wiring / seed / model fault.
 *  - INC-3 @c network.seed_birth_check_sigma — the same statistic at a tighter
 *    threshold, applied only to LOS whose per-LOS states were BORN this epoch.
 *    A correctly self-seeded birth has s ≈ 0 by construction, so ANY fire
 *    localizes a seed-path defect to an exact epoch and site.  MEASURE-ONLY.
 *  - INC-4 @c network.trace_topology — estimation-topology changes as EVENTS
 *    with identities: SH-tie row-set presence deltas per station (the deweight
 *    valve — era-2's whole story was one such event that nothing logged),
 *    per-(station, satellite) row-KIND changes while the satellite stays
 *    present, and shared-column registry deltas.  MEASURE-ONLY.
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
/// common-mode centre.  Below it the type is skipped for that station-epoch
/// (and counted) — a 2-row "median" would BE the outlier it is meant to remove.
inline constexpr int kNetPrefitMinTypeRows = 4;

/// Weight regime above which a row is a pin / hard constraint, never data.
/// The fuse skips these outright: a healthy gauge pin runs s ≈ 1e7 by design.
inline constexpr double kNetPrefitPinWeight = 1e6;

/// Bit of one DATA class in the fuse-domain mask (@c network.prefit_fuse_classes).
inline constexpr unsigned dataClassBit(NetRowClass c) {
    return isDataRowClass(c) ? (1u << static_cast<unsigned>(c)) : 0u;
}

/**
 * @brief Parse a DATA-class token list into a class mask.
 *
 * Tokens (comma or space separated, case-insensitive): @c code, @c phase,
 * @c other, @c sh, plus @c all and @c none.  Unknown tokens are IGNORED here and
 * reported to @p unknown so the loader can warn with the offending text —
 * silently dropping a misspelt class would silently widen or narrow a
 * solve-affecting domain.
 *
 * @param spec     the raw config string.
 * @param unknown  [out, optional] appended with each unrecognized token.
 * @return the mask (0 = fuse nothing).
 */
unsigned parseDataClassMask(const std::string& spec,
                            std::vector<std::string>* unknown = nullptr);

/// Human-readable rendering of a DATA-class mask ("code+sh", "none", …).
std::string dataClassMaskName(unsigned mask);

/// Per-epoch, per-run tallies of every increment in this card (standing
/// invariant #4 — inert wiring is only visible through the denominators).
struct NetworkPrefitCensusTallies {
    // --- INC-1 census walk ---------------------------------------------------
    long long epochs = 0;              ///< census/topology walk passes (the denominator).
    long long station_epochs = 0;      ///< (station, epoch) blocks walked.
    long long rows = 0;                ///< DATA rows walked.
    long long type_skips = 0;          ///< (station, epoch, type) medians skipped for n < 4.
    long long nonfinite_rows = 0;      ///< rows skipped for a non-finite y or w.
    long long exceedances = 0;         ///< DATA rows over prefit_event_sigma.
    long long events = 0;              ///< PREFIT exceedance events emitted.
    long long events_held = 0;         ///< …suppressed by the per-cell holdoff.
    long long events_class_skipped = 0;///< exceedances on a class OUTSIDE network.prefit_event_classes — suppressed, but counted, so a narrowed event domain is never a silent filter.
    long long rows_born_deferred = 0;  ///< DATA rows withheld from the INC-1 statistic because their cell was BORN this epoch — INC-3's business, at INC-3's threshold (see the header's "births are not exceedances").
    long long cells_born_deferred = 0; ///< (cell, class) exceedances INC-1 handed to INC-3 rather than evented.
    double    max_s = 0.0;             ///< run maximum standardized deviation.
    double    max_s_tow = -1.0;        ///< its epoch.
    std::string max_s_id;              ///< its "STATION|SAT|class".

    // --- INC-2 structural fuse ----------------------------------------------
    long long fuse_epochs = 0;         ///< assembly passes with the fuse armed.
    long long fuse_station_epochs = 0; ///< (station, epoch) blocks screened.
    long long fuse_rows = 0;           ///< DATA rows screened.
    long long fuse_capped = 0;         ///< rows whose weight was capped.
    long long fuse_events = 0;         ///< prefit_fused events emitted.
    long long fuse_events_held = 0;    ///< …suppressed by the per-cell holdoff.
    long long fuse_skip_pin_w = 0;     ///< rows skipped: w >= kNetPrefitPinWeight.
    long long fuse_skip_thin = 0;      ///< rows skipped: their type had n < 4.
    long long fuse_skip_class = 0;     ///< rows skipped: their DATA class is outside network.prefit_fuse_classes.
    long long fuse_would_class = 0;    ///< …of those, how many WOULD have been capped — the measured cost of the domain restriction, so narrowing it is never a silent choice.
    double    fuse_max_s = 0.0;        ///< largest PRE-cap s the fuse acted on.
    double    fuse_min_ratio = 1.0;    ///< smallest w_eff/w applied (the deepest cut).

    // --- INC-3 seed-birth tripwire ------------------------------------------
    long long births = 0;              ///< (station, satellite) LOS born this epoch.
    long long birth_rows = 0;          ///< DATA rows belonging to a born LOS.
    long long birth_events = 0;        ///< seed_birth_exceedance events emitted.
    long long birth_seed_suppressed = 0;///< births ignored on the FIRST walked epoch (no prior presence map).

    // --- INC-4 topology ledger ----------------------------------------------
    long long topo_tie_events = 0;     ///< per-station SH-tie row-count deltas.
    long long topo_kind_events = 0;    ///< per-(station, sat) row-KIND changes.
    long long topo_col_add = 0;        ///< shared columns added vs the previous epoch.
    long long topo_col_rm = 0;         ///< …removed.
    long long topo_truncated = 0;      ///< column-delta events dropped by the per-epoch cap (LOGGED, never silent).

    // --- INC-5 flight recorder (filter/NetworkRhsAttrib) --------------------
    long long rec_consults = 0;        ///< trigger-site latch consultations (0 ⇒ NO site ever asked).
    long long rec_trips = 0;           ///< rising edges that armed a dump.
    long long rec_dumps = 0;           ///< attribution dumps written.
    long long rec_prev_dumps = 0;      ///< …of which retained PREVIOUS-epoch frames.
    long long rec_frames_retained = 0; ///< epochs captured into the retention slot.
    long long rec_prev_bytes_last = 0; ///< bytes of the most recent retained frame.
};

/// The per-DATA-type robust centres of ONE station's assembled rows.
struct PrefitStationCentres {
    double    m[kNetDataClasses]{};      ///< the centre (0 for @c iono_sh_row).
    bool      have[kNetDataClasses]{};   ///< the type has a usable centre this epoch.
    long long n[kNetDataClasses]{};      ///< finite rows seen per type.
};

/**
 * @brief Per-type robust centres of one station's DATA rows.
 *
 * Centred types are @c obs_code / @c obs_phase / @c obs_other (median of @c y,
 * requiring @ref kNetPrefitMinTypeRows rows); @c iono_sh_row is NOT centred
 * (m ≡ 0) because it carries no receiver clock.  Rows with a non-finite y or w
 * never enter a median.
 */
PrefitStationCentres prefitStationCentres(const SharedColumnMap& cm,
                                          const StationAssembly& R);

/// @return @f$s = |y - m|\sqrt{\max(w,0)}@f$ — the card's standardized
///         prefit deviation.  Non-finite inputs yield 0 (the caller counts
///         those separately rather than propagating a NaN into a max).
double prefitStandardizedDeviation(double y, double w, double m);

/**
 * @brief INC-2: cap the fused weight of one station's over-threshold DATA rows.
 *
 * Call from the SERIAL pre-@c addStation site — the single point where every
 * appender has finished touching @c R.w — so the event order is deterministic
 * and thread-count independent.
 *
 * Exclusions, all counted: rows with @c w >= @ref kNetPrefitPinWeight (pin /
 * constraint regime) and rows whose type had no usable centre.  Single-frequency
 * cells are deliberately NOT excluded as a class: a 10³σ common-removed
 * excursion is ≳1800 TECU/30 s even at code noise — physically impossible
 * ionosphere — so the cap cannot bite genuine single-frequency storm dynamics.
 *
 * @param ctx     Run context (counters + the per-cell event holdoff).
 * @param traces  PREFIT-tier `prefit_fused` events.
 * @param t       This epoch (event stamp).
 * @param cm      This epoch's shared-column map.
 * @param station Station name (event identity).
 * @param R       [in/out] the station's assembled block; only @c w is written.
 * @param s_fuse  The threshold @f$S_\text{fuse}@f$ (> 0; the caller gates on it).
 * @return        Rows capped.
 */
int networkPrefitFuseStation(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, const SharedColumnMap& cm,
                             const std::string& station, StationAssembly& R,
                             double s_fuse);

/**
 * @brief INC-1 / INC-3 / INC-4: the post-solve census, seed-birth tripwire and
 *        topology ledger — one walk over the epoch's assembled row stacks.
 *
 * MEASURE-ONLY: it reads @c asm_res and writes only trace rows, events, run
 * counters and the per-epoch census scalars the epoch-status writer picks up.
 * Placed in SB_FRAMEDIAG BEFORE the stash/status block for exactly that reason.
 *
 * Note on composition with INC-2: when the fuse is armed it has ALREADY capped
 * the weights this walk reads, so the census reports the POST-cap statistic and
 * the pre-cap magnitude lives in the `prefit_fused` events.  That is the honest
 * split — the fuse leg and the census leg are separate validation arms.
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
 * @param iono_deweight The SH-constraint deweight gate, or nullptr — read-only,
 *                      supplies the per-station weight the INC-4 tie-row event
 *                      names ("BSHM +1 tie row, w 0 → 0.25").
 */
void networkPrefitCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         const NetworkEstimator& est,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of,
                         const NetworkIonoDeweightGate* iono_deweight);

} // namespace GnssProcessor
