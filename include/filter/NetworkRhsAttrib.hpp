#pragma once
/**
 * @file NetworkRhsAttrib.hpp
 * @brief In-assembly reduced-RHS class attribution: which row class loads the
 *        shared columns at a chosen epoch (the era-2 injector probe).
 *
 * Diagnostics-only, off by default, and not fingerprinted: it is a forward-only
 * readout and leaves the meaning of stored state unchanged (as for
 * adaptive_iono_q).  A shared-snapshot replay leg with only this armed is
 * therefore A/B-comparable against an unarmed control.
 *
 * ## What it computes
 *
 * At an armed target epoch the solve's reduced shared RHS
 * @f$\tilde u_s = u_s - \sum_r N_{sr} N_{rr}^{-1} u_r\ (+\ \text{DD folds})@f$
 * is decomposed exactly by class (card NOM step N2b: every class is read from a
 * contribution tag or kind, filter/NetworkContributions.hpp):
 *
 *  - a station's rows by their tags' row class (@ref classifyRow, @ref rowClassName):
 *    @c obs_code, @c obs_phase, @c iono_sh_row, @c pivot_pin, @c local_pin_row,
 *    @c cpl_row, and @c obs_other for an unclassified row;
 *  - a station's prior entries by their kinds' names (@ref contributionKindName), from
 *    both channels the kernel folds: the pre-PD prior folded with the block (for
 *    example @c prior_rx_cb_datum_pre_pd) and the post-PD diagonal prior when the
 *    consume folded it (@c prior_ambiguity_carry, @c prior_zwd_carry,
 *    @c prior_iono_carry, @c prior_rx_cb_carry, @c prior_rx_cb_datum,
 *    @c prior_ambiguity_oracle_pin);
 *  - each shared fold the estimator captured, by its kind: @c "shared:" and the kind's
 *    name (@ref rhsAttribSharedClass; for example @c shared:border_carry_dense,
 *    @c shared:cb_if_pin);
 *  - the invariant-carry DD folds (@c dd_fold).  The dump reports each class's load on the target
 * shared columns (the two-layer SH means @c SH:C:0:0 / @c SH2:C:0:0), and the
 * dumped per-class full vectors allow projection on any other direction
 * offline (for example the NEQ-export weak-3).
 *
 * The split is exact because every term is linear in the RHS given the epoch's
 * final normal blocks: per station and class @f$c@f$,
 * @f$\tilde u_s^c = u_s^c - N_{sr}\,N_{rr}^{-1} u_r^c@f$ with the same
 * @f$N_{sr},N_{rr}@f$ for every class (all classes' information folded), and
 * the class vectors sum to the kernel's reduced RHS.  The dump carries an
 * explicit CLOSURE row (max |Σ_classes − kernel|).  A violated closure marks the
 * epoch's attribution as untrusted (for example when a within-epoch RAIM replay
 * rebuilt rows after the captures), so a wrong decomposition is reported
 * rather than dumped as valid (invariant #4).
 *
 * ## Outputs (one set per dumped epoch, under `<log_dir>/rhs_attrib/`)
 *
 *  - `attrib_<tow>.csv`: per (class, station) reduced-RHS contributions to the
 *    target columns and the per-family norms (sh/pb/cb/clk); per-pin datum rows;
 *    per-block DD rows; the closure row.  The 8 stations with the largest
 *    observation-class load on the target columns also get per-satellite
 *    sub-rows (a second pass over their rows).
 *  - `rows_<tow>.csv`: one line per row of every station (w, y, target-column
 *    coefficients), with the row's class and, in the last column @c kind, the
 *    contribution kind its tag declares (card NOM step N2a; card ICF instrument A').
 *  - `vec_<tow>.csv`: per-class total reduced-RHS vectors over all shared
 *    columns (col, label, value) for offline projections.
 *  - `cols_<tow>.csv` (only when @c network.rhs_attrib_columns names columns): for
 *    every (station, contribution kind), every (station, kind, satellite, signal),
 *    every shared fold kind, the DD folds and the datum pins, the reduced-RHS
 *    contribution at each named column and its solution share there
 *    (@ref networkRhsAttribColumnShares), with the kind totals, the full solution and
 *    a closure line per column (@ref networkRhsAttribDump documents the records).
 *  - `loc_<tow>.csv` (only when @c network.rhs_attrib_columns names station-local
 *    columns, `LOC:<station>:<key>`, and only at the dumps whose frame holds the solve's
 *    @c x_s: the armed dumps and a trip's current-epoch dump, not a retained previous
 *    frame): the decomposition of each named local column's solved increment
 *    @f$x_r(k)@f$ into one share per data row of the station, one per local fold kind,
 *    the shared feedback (split by station and satellite, with each station's net) and
 *    the constraint corrections, with closure lines and two checks of the single parts
 *    (@ref networkRhsAttribLocalShares, card L5B).  The station's geometry-free level
 *    direction is decomposed the same way when the station's data-only block has a
 *    one-dimensional null space.
 *
 * ## Knobs (network section; loader guards)
 *
 *  - @c network.rhs_attrib_tow   (double, default −1 = off): first target TOW.
 *  - @c network.rhs_attrib_n_ep  (int, default 2): number of target epochs.
 *  - @c network.rhs_attrib_columns (list of shared-column labels as `vec_<tow>.csv`
 *    prints them, e.g. `["CLK:G07", "CB:G07:C1W"]`, and station-local labels
 *    `LOC:<station>:<key>`, e.g. `"LOC:MAJU00MHL:GPS_DCB"`; default empty = no `cols_`
 *    or `loc_` file): the named columns of the solution-share readouts (card ICF
 *    instrument A' for the shared labels, card L5B for the local ones; the key
 *    spellings are those of @ref rhsAttribResolveLocalKey).
 *    Diagnostics only, not fingerprinted, read at every dump (armed or trip).
 *  - @c network.rhs_attrib_on_trip (bool, default false): the INC-5 flight
 *    recorder (docs/SPEC_prefit_census_observability.md §7).  Arms the capture
 *    recorder every epoch and dumps automatically at the first trip of a QC era
 *    (SH-gate engage, A4 σ̂₀ withhold, clock runaway, a₀₀-band exit) instead of
 *    at a hand-picked TOW.  With @c rhs_attrib_on_trip_prev (default true) the
 *    retained frame of the previous epoch is dumped too, since the pre-trip
 *    frame is the one that shows the cause.
 *
 * Cost when armed: at a dumped epoch, one LDLᵀ factorization per station
 * (reused by the DD and per-satellite passes) plus the per-station @f$N_{rr}@f$
 * rebuild at capture.  Disarmed, the cost is one double compare per epoch.  The
 * INC-5 retention channel also copies one epoch's assemblies and kernel blocks
 * (tens to hundreds of MB; see @ref NetworkAttribFrame) and disables itself
 * above @c network.rhs_attrib_prev_max_stations.
 */

#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRowClassify.hpp"   // NetColFam
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Everything one epoch's reduced-RHS decomposition reads, detached from
 *        the live estimator (INC-5).
 *
 * The attribution is a pure function of (row stacks, per-station kernel blocks,
 * shared-info captures, DD-prior inputs, reduced system, datum pins).  Bundling
 * them lets one code path serve both the live epoch and a retained previous
 * epoch.  The flight recorder depends on the latter: the frame before a
 * discharge identifies the injector, while the frame of the discharge shows
 * only its effect.
 *
 * Assembly ownership is optional.  On the live path @ref asm_ptr points into
 * the caller's `asm_res` (no copy); on the retention path @ref asm_own holds
 * deep copies and @ref asm_ptr points into it.  Kernel blocks are always owned:
 * @c stationLocalNormals returns by value (rebuilt from the cached LDLᵀ), and
 * the rest are caches the next epoch overwrites.
 *
 * Size at 63 stations / ~250 shared columns / ~200 local columns is of order
 * 100 MB, so @c network.rhs_attrib_prev_max_stations (default 128) turns
 * retention off for large networks instead of letting memory grow unnoticed.
 */
struct NetworkAttribFrame {
    bool                     valid{false};   ///< A usable reduced system was captured.
    int                      week{0};
    double                   tow{0.0};
    Eigen::Index             n{0};           ///< shared-column count.
    std::vector<std::string> label;          ///< shared-column labels (size @ref n).
    std::vector<NetColFam>   fam;            ///< shared-column families (size @ref n), from the column identity.
    std::vector<std::string> col_sat;        ///< shared-column satellites (CLK / CB / PB columns), size @ref n.
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
    std::vector<NetworkEstimator::AttribCapture>        captures;   ///< shared-info folds, each with the kind its site named.
    std::vector<NetworkNormalEquations::DdPriorBlock>   dd_blocks;  ///< DD-prior input blocks.
    Eigen::VectorXd            b_red;   ///< kernel reduced shared RHS.
    Eigen::MatrixXd            N_red;   ///< kernel reduced shared normals (datum-pin section).
    NetworkEstimator::Datum    datum;   ///< the epoch's minimum-constraint pins.
    /// The solve's shared estimate @c x_s as the driver holds it after the epoch's last
    /// solve, when the caller supplied it (size @ref n); empty otherwise (the trip path,
    /// a retained frame).  Read only by the solution-share readout, which reports it
    /// beside its own recomputation as a check that the operator is the solve's.
    Eigen::VectorXd            x_s;

    // --- per work item, station-local readout (card L5B) -----------------------
    // Filled only when the caller supplied x_s and asked for the local readout (a LOC:
    // label is named; the armed dumps and a trip's current-epoch dump); empty otherwise, so
    // every other capture, the flight recorder's per-epoch retention among them, pays
    // nothing for them.
    /// Per item, one name per local column (A_r column): the station's state label
    /// (@ref rhsAttribLocalColumnNames); an empty name where no state claims the column.
    /// Read from the state manager as it stands at the capture: on the network path states
    /// are removed only inside the measurement update, before the split, so the split's
    /// state indices (@c h2local) still name the same states after the solve (the
    /// station-block audit reads them the same way).
    std::vector<std::vector<std::string>> local_names;
    /// Per item, the kernel's reconstruction of the station's locals at @ref x_s with its
    /// terms apart (@c NetworkNormalEquations::reconstructStationParts).
    std::vector<NetworkNormalEquations::StationReconstructionParts> local_parts;

    /// Approximate retained heap footprint in bytes (run-end telemetry).
    std::size_t bytes() const;
    /// Release everything (used when retention is disabled mid-run).
    void clear();
};

/**
 * @brief Capture one epoch into a @ref NetworkAttribFrame.
 *
 * @param out            [out] frame, overwritten wholesale.
 * @param se             this epoch (week/tow stamp).
 * @param stations       station roster (names).
 * @param est            the solved estimator (kernel and captures must be current).
 * @param plan_datum     the epoch's minimum-constraint pins.
 * @param asm_res        PASS A/B station assemblies.
 * @param est_index_of   station → kernel slot (−1 = not folded).
 * @param own_assemblies deep-copy the assemblies into the frame (retention);
 *                       false = point at @p asm_res, valid only until it changes.
 * @param x_s            optional: the solve's shared estimate, copied into
 *                       @ref NetworkAttribFrame::x_s when its size matches the shared
 *                       width; nullptr (the default) leaves it empty.
 * @param local_readout  also record, when @p x_s was copied, every item's local-column
 *                       names (from the station's state manager, when it has one) and the
 *                       kernel's reconstruction parts at @p x_s
 *                       (@ref NetworkAttribFrame::local_names,
 *                       @ref NetworkAttribFrame::local_parts), which the station-local
 *                       readout reads.  The callers pass
 *                       @ref rhsAttribNamesLocalColumns of @c network.rhs_attrib_columns;
 *                       false (the default) records nothing for it.
 */
void networkAttribCaptureFrame(NetworkAttribFrame& out, const SyncedEpoch& se,
                               const std::vector<NetStation>& stations,
                               const NetworkEstimator& est,
                               const NetworkEstimator::Datum& plan_datum,
                               const std::vector<StationAssembly>& asm_res,
                               const std::vector<int>& est_index_of,
                               bool own_assemblies,
                               const Eigen::VectorXd* x_s = nullptr,
                               bool local_readout = false);

/**
 * @brief The attribution class of a shared fold: the one map from a contribution kind
 *        to the class the dump writes (card NOM step N2b).
 *
 * The class is @c "shared:" followed by the kind's name (@ref contributionKindName), so
 * each fold site names one thing: the kind it hands the estimator.  A fold whose site
 * named no kind reads @c shared:unset (the within-epoch replay's single re-fold of the
 * captured priors, which are of several kinds).  These strings are written to the
 * attribution's @c attrib_<tow>.csv (the TOTAL rows' class) and @c vec_<tow>.csv (the
 * class column) only; no event carries them.
 *
 * @param k The fold's kind.
 * @return The class.
 */
std::string rhsAttribSharedClass(ContributionKind k);

/**
 * @brief The exact reduced-RHS decomposition of one captured frame, before it is
 *        written.
 */
struct NetworkRhsAttribDecomposition {
    /// One class's reduced contribution at one station.
    struct StationPart {
        std::string     cls;  ///< The class (a row class name or a prior kind's name).
        std::string     sta;  ///< The station.
        Eigen::VectorXd v;    ///< Its reduced shared-RHS contribution (n_shared).
    };
    /// One DD prior block's recomputed fold.
    struct DdPart {
        std::size_t     block{0};  ///< The block's index in the frame's DD inputs.
        std::string     label;     ///< Up to three of its terms ("sat:signal" or "loc<col>").
        Eigen::VectorXd v;         ///< Its reduced shared-RHS contribution.
    };
    bool valid{false};                                    ///< The frame had a reduced system.
    std::map<std::string, Eigen::VectorXd> class_total;  ///< Per class, the sum over stations and folds.
    std::vector<StationPart> station_parts;               ///< Per (station, class), in station order.
    std::vector<DdPart>      dd_parts;                    ///< Per DD block that factored.
    double closure_abs{0.0};  ///< max |sum of the classes - the kernel's reduced RHS|.
    double closure_rel{0.0};  ///< ||sum - kernel|| / max(1, ||kernel||); the dump warns above 1e-4.
};

/**
 * @brief Decompose one captured frame by class, exactly (no file is written).
 *
 * Iterates the channels the kernel folds, in its order: per station the block's rows
 * (classed by @ref classifyRow), the pre-PD prior entries (@c prior_cols, folded with the
 * block) and, when @c carry_prior_folded, the post-PD entries (@c carry_cols), each
 * entry classed by its tag's kind; each part is reduced through the station's final
 * @f$N_{sr}N_{rr}^{-1}@f$.  Then the captured shared folds (@ref rhsAttribSharedClass)
 * and the DD folds.  The classes sum to the kernel's reduced RHS up to rounding unless a
 * within-epoch replay rebuilt the kernel after the captures, which the closure reports.
 *
 * @param f A frame captured by @ref networkAttribCaptureFrame.
 * @return The decomposition; @c valid false for a frame without a reduced system.
 */
NetworkRhsAttribDecomposition networkRhsAttribDecompose(const NetworkAttribFrame& f);

/**
 * @brief The load of every contribution at a few named shared columns, as reduced
 *        right-hand side and as solution share (card ICF instrument A',
 *        @c network.rhs_attrib_columns).
 *
 * A right-hand-side share says how hard a contribution pushes on a column; the solve
 * moves the column by @f$\tilde N^{-1}\tilde u@f$, so a contribution's effect on the
 * estimate is its solution share, which can differ in size and sign (card ICF section 5).
 *
 * ## The operator
 *
 * The solution share is computed with the operator @c NetworkEstimator::solve applies,
 * rebuilt from the frame: the kernel's reduced shared normals @f$\tilde N@f$
 * (@ref NetworkAttribFrame::N_red, after the shared folds, the ensemble phase gauges and
 * the DD folds; the datum is NOT in it) with the epoch's minimum-constraint pins applied
 * by the same Dirichlet elimination: each pinned row and column zeroed, the pinned
 * diagonal set to @f$s=\max(1,\max_i|\tilde N_{ii}|)@f$, factorized by
 * @c Eigen::LDLT as @c NetworkNormalEquations::solveSharedConstrained does.  Call the
 * result @f$N_c@f$, with free columns @f$F@f$ and pinned columns @f$P@f$.  The solve is
 * @f[ x_F = \tilde N_{FF}^{-1}\,(\tilde u_F - \tilde N_{FP} v_P),\qquad x_P = v_P, @f]
 * so for a named free column @f$c@f$ with @f$g = N_c^{-1} e_c@f$ (its entries on
 * @f$P@f$ are zero by construction and are zeroed explicitly), a reduced-RHS part
 * @f$\tilde u^{(k)}@f$ has the solution share @f$g^\top \tilde u^{(k)}@f$, and the
 * datum's own share is @f$g^\top(-\tilde N_{:,P} v_P)@f$.  The shares sum to @f$x_c@f$
 * exactly, up to the right-hand-side closure of @ref networkRhsAttribDecompose.  At a
 * pinned named column every share is zero and the datum's share is the pin value.
 *
 * A station part never forms its reduced vector: with
 * @f$r = N_{rr}^{-1} N_{sr}^\top e_c@f$ and @f$t = N_{rr}^{-1} N_{sr}^\top g@f$ per
 * station, a row with weight @f$w@f$ and prefit @f$y@f$ contributes
 * @f$w y\,(A_s[c] - A_r\!\cdot r)@f$ to the reduced RHS at @f$c@f$ and
 * @f$w y\,(A_s\!\cdot g - A_r\!\cdot t)@f$ to the solution share, and a diagonal
 * prior entry @f$\delta u@f$ on local column @f$j@f$ contributes @f$-\delta u\,r_j@f$
 * and @f$-\delta u\,t_j@f$.  These are the station reduction of
 * @ref networkRhsAttribDecompose (@f$u_s - N_{sr}N_{rr}^{-1}u_r@f$) restricted to the
 * part's rows and read at one column, so any grouping of rows is exact.
 *
 * ## What the solved quantity is
 *
 * The shared columns are linearized at the a priori product each epoch (the clock
 * column is @f$\Delta\delta t^s@f$ against the a priori satellite clock, which
 * @c NetworkEstimator::toSolutionEpoch copies into the emitted clock), so the solution
 * share is a share of the column's estimate itself, not of a step from the previous
 * epoch; the epoch-to-epoch change of a share is the change of that part's
 * contribution.  The carried border and station priors enter as contributions like any
 * other (their kinds name them).
 */
struct NetworkRhsColumnShares {
    /// One contribution's load at every named column.
    struct Part {
        std::string     record;  ///< @c STAKIND, @c STALOS, @c SHARED, @c DDFOLD, @c DATUMPIN or @c KIND.
        std::string     sta;     ///< The station (@c STAKIND, @c STALOS); empty otherwise.
        std::string     kind;    ///< The kind's name (@ref contributionKindName); @c "shared:<kind>" for a shared fold (@ref rhsAttribSharedClass), @c dd_fold, @c datum_pin.
        std::string     sat;     ///< @c STALOS: the satellite the tag names, @c "?" for none; empty otherwise.
        std::string     sig;     ///< @c STALOS: the signal the tag names ("1C"), empty for none.
        long long       n{0};    ///< Terms in the part: rows and prior entries, fold captures, DD blocks or pins.
        Eigen::VectorXd rhs;     ///< Per named column: the part's reduced-RHS contribution (0 at an absent column).
        Eigen::VectorXd sol;     ///< Per named column: the part's solution share (0 at an absent column).
    };
    bool valid{false};                  ///< The frame had a reduced system and the constrained factorization succeeded.
    std::vector<std::string>  labels;   ///< The named columns, as requested.
    std::vector<Eigen::Index> col;      ///< Their shared columns this epoch; -1 = no such column.
    std::vector<char>         pinned;   ///< 1 where the epoch's datum pins the column.
    std::vector<Part> station_kind;     ///< @c STAKIND: per (station, kind), stations in frame order, kinds by name.
    std::vector<Part> station_los;      ///< @c STALOS: per (station, kind, satellite, signal); sums to @ref station_kind.
    std::vector<Part> folds;            ///< @c SHARED per shared-fold class, then @c DDFOLD and @c DATUMPIN.
    std::vector<Part> kind_total;       ///< @c KIND: per kind over every station, then each fold; sums to the totals.
    Eigen::VectorXd rhs_sum;   ///< Sum of @c rhs over @ref station_kind and the @c SHARED and @c DDFOLD folds (the datum pin acts on the solve's copy and is excluded, as in the class closure).
    Eigen::VectorXd sol_sum;   ///< Sum of @c sol over @ref station_kind and every fold, the datum pin included.
    Eigen::VectorXd b_red;     ///< The kernel's reduced RHS at each column.
    Eigen::VectorXd x_full;    ///< @f$(N_c^{-1}u_c)_c@f$: the datum-constrained solution recomputed from the frame.
    Eigen::VectorXd x_solve;   ///< The driver's @c x_s at each column (NaN when the frame holds none).
    Eigen::VectorXd rhs_closure_abs;  ///< @f$|\text{rhs\_sum}-\tilde u_c|@f$.
    Eigen::VectorXd rhs_closure_rel;  ///< @ref rhs_closure_abs over @f$|\tilde u_c|@f$ (0 when both are 0).
    Eigen::VectorXd sol_closure_abs;  ///< @f$|\text{sol\_sum}-x_c|@f$ with @f$x_c@f$ = @ref x_full.
    Eigen::VectorXd sol_closure_rel;  ///< @ref sol_closure_abs over @f$|x_c|@f$ (0 when both are 0).
};

/**
 * @brief The solution share and reduced-RHS contribution of every (station, kind) part,
 *        every shared fold kind, the DD folds and the datum at the named columns (no
 *        file is written).
 *
 * See @ref NetworkRhsColumnShares for the operator.  The station parts follow the
 * channels @ref networkRhsAttribDecompose iterates (rows by their tags' kinds, the
 * pre-PD prior entries, the post-PD entries when folded), keyed by kind instead of
 * class; the shared folds are @ref NetworkAttribFrame::captures grouped by
 * @ref rhsAttribSharedClass; the DD folds are @p dec's recomputed blocks.  A label that
 * names no shared column this epoch keeps its slot with @c col -1 and zeros.
 *
 * @param f      A frame captured by @ref networkAttribCaptureFrame.
 * @param dec    @ref networkRhsAttribDecompose of the same frame (its DD parts are reused).
 * @param labels The named columns (shared-column labels, "CLK:G07").
 * @return The shares; @c valid false for a frame without a reduced system or a
 *         constrained system that does not factorize.
 */
NetworkRhsColumnShares networkRhsAttribColumnShares(const NetworkAttribFrame& f,
                                                    const NetworkRhsAttribDecomposition& dec,
                                                    const std::vector<std::string>& labels);

// =========================================================================================
// The station-local readout (card L5B)
// =========================================================================================

/// A station-local column named in @c network.rhs_attrib_columns: `LOC:<station>:<key>`.
struct RhsAttribLocalLabel {
    std::string label;    ///< The label as requested.
    std::string station;  ///< The station name (the text between the first and second ':'); empty when malformed.
    std::string key;      ///< The column's key (everything after the second ':', which may hold ':'); empty when malformed.
};

/**
 * @brief Whether a label of @c network.rhs_attrib_columns names a station-local column.
 * @param label A label.
 * @return true when it starts with `LOC:`; such a label is never read as a shared column.
 */
bool isRhsAttribLocalLabel(const std::string& label);

/**
 * @brief Whether a label list names any station-local column (the capture's
 *        @c local_readout switch, @ref networkAttribCaptureFrame).
 * @param labels The labels of @c network.rhs_attrib_columns.
 * @return true when @ref isRhsAttribLocalLabel holds for one of them.
 */
bool rhsAttribNamesLocalColumns(const std::vector<std::string>& labels);

/**
 * @brief Split a station-local label into station and key.
 * @param label A label for which @ref isRhsAttribLocalLabel holds.
 * @return The parts; station and key empty when either part is missing
 *         (`LOC:`, `LOC:MAJU00MHL`, `LOC::GPS_DCB`), which the readout reports as
 *         unresolved.
 */
RhsAttribLocalLabel parseRhsAttribLocalLabel(const std::string& label);

/**
 * @brief The names of a station's local columns: for each column of @c R.A_r, the
 *        station's state label (@c StateManager::getStateLabels) of the state the
 *        assembly's @c h2local map puts there.
 *
 * The labels are the state manager's: the fixed core by name (@c CLK, @c ZWD,
 * @c ISB_<system>, @c PHASE_CLK, ...) and the dynamic maps with a category prefix
 * (@c CBIAS:<key> for the receiver code biases, @c IONO:<sat>, @c AMB:<key>,
 * @c RPB:<key>, @c PCOL:<sat>, ...).  A column no state claims keeps an empty name.
 *
 * @param R  The station's assembly (its @c h2local map and local width).
 * @param sm The station's state manager, at the epoch of the assembly.
 * @return One name per local column.
 */
std::vector<std::string> rhsAttribLocalColumnNames(const StationAssembly& R,
                                                   const StateManager& sm);

/**
 * @brief Resolve the key of a `LOC:` label against a station's local-column names.
 *
 * The key matches a column when it equals the column's name (@c "CLK", @c "ZWD",
 * @c "CBIAS:GPS_DCB", @c "IONO:G08", @c "AMB:G08_L1C") or, failing that, the name's
 * part after its category prefix, when exactly one column has that part (@c "GPS_DCB",
 * @c "GPS_IFB5", @c "GPS_ISC_C1W" for the receiver code biases, @c "G08_L1C" for an
 * ambiguity).  So the receiver code-bias keys are written as the state manager keys
 * them, and every other column as @ref rhsAttribLocalColumnNames names it.
 *
 * @param names The station's local-column names.
 * @param key   The label's key.
 * @return The local column; -1 when no column matches; -2 when the bare key matches
 *         more than one column.
 */
Eigen::Index rhsAttribResolveLocalKey(const std::vector<std::string>& names,
                                      const std::string& key);

/**
 * @brief The decomposition of named station-local solved increments into the rows and
 *        folds that move them (card L5B; @c network.rhs_attrib_columns with `LOC:` labels).
 *
 * ## The algebra
 *
 * For a station @f$r@f$ the solve's local increment is (@c reconstructStation)
 * @f[ x_r = N_{rr}^{-1}\,(u_r - N_{rs}x_s) + c_{\text{sz}} + c_{\text{dd}}, @f]
 * with @f$c_{\text{sz}}, c_{\text{dd}}@f$ the sum-to-zero gauge and DD-prior corrections
 * (zero unless those folds name the station).  For a direction @f$p@f$ over the local
 * columns (a unit vector @f$e_k@f$ for a named column) let @f$g = N_{rr}^{-1}p@f$.  Since
 * @f$u_r = \sum_i w_i y_i a_i + \sum_e \eta_e e_{l_e}@f$ (the station's rows of @c A_r and
 * the diagonal prior entries folded into it),
 * @f[ p^\top x_r = \sum_i w_i y_i\,(a_i^\top g) + \sum_e \eta_e\,g_{l_e}
 *                 - (N_{sr}g)^\top x_s + p^\top c_{\text{sz}} + p^\top c_{\text{dd}}, @f]
 * one share per row, per prior entry, the shared feedback and the two corrections.  The
 * rows and the prior entries are the channels @ref networkRhsAttribDecompose iterates:
 * every row of @c A_r, the pre-PD prior (@c prior_cols, folded with the block) and the
 * post-PD prior (@c carry_cols) when the consume folded it.  @f$N_{rr}@f$ and
 * @f$N_{sr}@f$ are the frame's (the kernel's, rebuilt from its factor), @f$x_s@f$ the
 * solve's; @f$x_r@f$ and the corrections are the kernel's own
 * (@c NetworkNormalEquations::reconstructStationParts).
 *
 * Two checks are reported.  @c unattributed is the kernel's @f$p^\top N_{rr}^{-1}u_r@f$
 * minus the attributed rows and prior entries: a channel that reached the kernel's
 * @f$u_r@f$ without being recorded in the assembly shows here, as does the round-off
 * between the kernel's factor and the frame's.  @c closure_abs is
 * @f$|\text{total} - p^\top x_r|@f$ with total the sum of every share (the unattributed
 * term excluded), so the shares account for the solve's increment to within it.
 *
 * Data rows (a @c ROW part each) are the rows whose tag kind is one of the four data
 * kinds (code, code companion, phase, second phase), and any untagged row of the split's
 * data region.  Every other row is a fold, grouped by kind (@c FOLD, channel @c row),
 * as are the prior entries (channels @c pre_pd and @c post_pd); each fold group is also
 * split by its tags' satellite and signal (@c FOLDLOS), and every share is summed by kind
 * (@c KIND).
 *
 * ## The shared feedback, split
 *
 * The feedback is a linear functional of the shared solution,
 * @f$-(N_{sr}g)^\top x_s = h^\top x_s@f$ with @f$h = -N_{sr}g@f$, and is split two ways.
 * By shared column (@c FBCOL, one per column with @f$h_c \ne 0@f$: the column's label in
 * @c kind, its estimate @f$x_s(c)@f$ in @c y, @f$h_c@f$ in @c a_col and @f$h_c x_s(c)@f$ in
 * @c share).  A single shared estimate depends on the datum, so a single @c FBCOL term is
 * not datum-invariant and the terms cancel in pairs; only their sum, the feedback, is a
 * quantity to grade.  The @c FBCOL lines are an inventory, not a ranking.  And by what the
 * shared solution itself is made of: with the solve's
 * constrained operator @f$N_c@f$ of @ref NetworkRhsColumnShares and @f$g_s = N_c^{-1}h@f$,
 * every part of every station has the share @f$g_s^\top\tilde u^{(k)}@f$ of
 * @f$h^\top x_c@f$, as do the shared folds, the DD folds and the datum (the solution-share
 * algebra of @ref networkRhsAttribColumnShares at the functional @f$h@f$): @c FBSTA per
 * (station, kind), @c FBLOS per (station, kind, satellite, signal), @c FBFOLD per fold
 * class and the datum, @c FBTOTAL their sum @f$h^\top x_c@f$ and @c FBCLOSURE its distance
 * to the feedback at the solve's @f$x_s@f$.  For the target station's own rows and prior
 * entries the shared-path share is also written beside the direct one (@c share_shared on
 * @c ROW, @c FOLD, @c FOLDLOS and @c KIND).  When the constrained operator does not
 * factorize the split does not run: every shared-path share is then NaN (an empty cell),
 * never a zero.
 *
 * ## Station nets: the ranking unit
 *
 * The per-kind parts of one station cancel heavily (a station's phase rows and its carried
 * ambiguity block can move the target by hundreds of metres in opposite directions and net
 * a few), so a single @c FOLD, @c KIND, @c FBSTA or @c FBLOS part says little about what
 * moves the target.  The ranking unit is the station's net, the effect of all its rows and
 * prior entries together: @c NETSTA per station (@c share the net: for the target station
 * its direct shares plus its shared-path part, for another station its shared-path part
 * alone; @c share_shared the shared-path part), and @c NETSAT the same per (station,
 * satellite) across kinds and signals (satellite @c "?" for the terms whose tags name
 * none).  Both are sorted by |net|.  The target's increment is the sum of the @c NETSTA
 * nets, the @c FBFOLD folds and the two constraint corrections, to within @c CLOSURE_ABS
 * and @c FBCLOSURE, and the change of a station's net between two dumps is the change of
 * that station's contribution.  The satellite nets of one station still cancel against
 * each other through the station's common columns (receiver clock, code biases), by
 * thousands of metres at some stations, so @c NETSAT is detail within a station and the
 * run log ranks @c NETSTA only.
 *
 * What a net counts depends on the folds the posture arms.  With no DD-prior block and
 * no ensemble sum-to-zero gauge (@c DDPRIOR and @c SUMZERO zero, no @c dd_fold), a row's
 * whole effect on the target is @c share + @c share_shared (its @c FBLOS part for a row of
 * another station), and the target's increment is the sum of the whole effects of every
 * station's rows and prior entries, the shared folds and the datum.  With them, a row also
 * acts through the folds' own right-hand sides (@f$z_{\text{free}}@f$ of a DD block,
 * @f$\gamma@f$ of a gauge) and through the two reconstruction corrections; that part of
 * its effect is attributed whole, to the @c dd_fold fold, @c SUMZERO and @c DDPRIOR, and
 * not to the row.  The ensemble gauges' @f$\gamma g/\beta@f$ in the reduced right-hand
 * side is in no channel of the class decomposition (@c network.phase_datum_ensemble) and
 * shows in @c FBCLOSURE, as it shows in the class closure.
 *
 * ## Checks of the single parts
 *
 * @c CLOSURE_ABS and @c FBCLOSURE bound sums, not parts, and the parts cancel.  Two checks
 * bound single parts.
 *
 *  - @c REFINE: each of the three solves (@f$g@f$, @f$g_s@f$ and
 *    @f$T = N_{rr}^{-1}N_{sr}^\top g_s@f$) gets one step of iterative refinement, the
 *    residual formed with the frame's normals (@f$N_c@f$ applied from @f$\tilde N@f$
 *    without forming it).  The written parts are the refined ones; @c REFINE gives in
 *    @c share the largest change any part took and in @c kind which part, an estimate of
 *    the rounding of the unrefined parts.
 *  - @c PERTURB: the reduced system the frame holds is solved again with the prefits of
 *    one group of rows and prior entries removed (their reduced right-hand side
 *    subtracted, the factor of @f$N_c@f$ reused since a prefit moves no normal, the
 *    target's locals formed again), and the change of @f$p^\top x_r@f$ is written in
 *    @c check beside the attributed whole effect in @c share.  Four groups: the target
 *    station's data row of largest whole effect (@c channel @c row), the other-station
 *    @c FBLOS group of largest |share| (@c los), the target station whole and the other
 *    station of largest |net| (@c station).  The re-solve holds the DD folds and the
 *    gauges at their solved right-hand sides and leaves the constraint corrections out,
 *    so on a posture with those folds it checks the arithmetic of the shares, not the
 *    whole effect.
 *
 * ## The geometry-free level direction
 *
 * For each station with a resolved `LOC:` label a target @c GFDIR:<station> decomposes
 * the station's geometry-free level direction the same way.  The direction is the null
 * space of the station's data-only local normals @f$\sum_{\text{data}} w_i a_i a_i^\top@f$
 * (the slant ionospheres, receiver code biases, receiver clock and ambiguities moving
 * together so that no data row changes), taken when it is one-dimensional (eigenvalues
 * below @f$10^{-11}@f$ of the largest); otherwise the target is unresolved and its note
 * gives the null count.  The vector @f$d@f$ is scaled so that its loadings on the
 * @c IONO: columns average 1 (unit norm with the largest loading positive when there is
 * none), and @f$p = d/(d^\top d)@f$, so @f$p^\top x_r@f$ is the coefficient of the
 * orthogonal projection of @f$x_r@f$ on @f$d@f$: the station-common slant-ionosphere move
 * along the level direction, in metres.  The target's name lists @f$d@f$'s loadings on the
 * station-level columns and the mean of its ionosphere and ambiguity loadings.
 */
struct NetworkRhsLocalShares {
    /// One share of a target.
    struct Part {
        std::string  record;   ///< @c ROW, @c FOLD, @c FOLDLOS, @c KIND, @c FEEDBACK, @c SUMZERO, @c DDPRIOR, @c FBCOL, @c FBSTA, @c FBLOS, @c FBFOLD, @c NETSTA, @c NETSAT, @c REFINE, @c PERTURB.
        std::string  channel;  ///< @c row (a row of @c A_r), @c pre_pd, @c post_pd; @c PERTURB: the group (@c row, @c los, @c station); empty for the whole-station terms, @c KIND and the feedback split.
        std::string  from;     ///< @c FBSTA, @c FBLOS, @c NETSTA, @c NETSAT, @c PERTURB: the station whose part it is; empty otherwise.
        std::string  kind;     ///< The contribution kind's name (@ref contributionKindName); the shared column's label (@c FBCOL); the fold class (@c FBFOLD); @c REFINE: the part that moved most; empty for the whole-station terms and the nets.
        std::string  sat;      ///< The tag's satellite (@c ROW, @c FOLDLOS, @c FBLOS, @c NETSAT, @c PERTURB); @c "?" for none.
        std::string  signal;   ///< The tag's signal ("1C"; @c ROW, @c FOLDLOS, @c FBLOS, @c PERTURB); empty for none.
        std::string  obs;      ///< @c ROW (and its @c PERTURB): the observation code ("C5Q", "L1C"); empty otherwise.
        Eigen::Index row{-1};  ///< @c ROW (and its @c PERTURB): the row of @c A_r.
        long long    n{0};     ///< Rows and prior entries in the part (@c REFINE: the parts compared).
        double w{0.0};         ///< @c ROW: the row's weight.
        double y{0.0};         ///< @c ROW: the row's prefit residual.
        double v{0.0};         ///< @c ROW: its post-fit residual @f$y - A_s x_s - A_r x_r@f$.
        double a{0.0};         ///< @c ROW: its coefficient along the target (@f$A_r(i,k)@f$ for a named column, @f$d^\top a_i@f$ for @c GFDIR); @c FBCOL: @f$h_c@f$.
        /// The share of the target's solved increment (m): direct for @c ROW, @c FOLD,
        /// @c FOLDLOS, @c KIND; through the shared solution for @c FBSTA, @c FBLOS, @c FBFOLD;
        /// the net (direct plus through the shared solution) for @c NETSTA and @c NETSAT;
        /// @c REFINE: the largest change of any part; @c PERTURB: the attributed whole
        /// effect of the group.
        double share{0.0};
        /// The share through the shared solution: @c ROW, @c FOLD, @c FOLDLOS, @c KIND (the
        /// target station's own rows and entries) and @c NETSTA, @c NETSAT; NaN otherwise
        /// and whenever the split did not run.
        double share_shared{std::numeric_limits<double>::quiet_NaN()};
        /// @c PERTURB: the group's whole effect measured by re-solving without its
        /// prefits; NaN otherwise.
        double check{std::numeric_limits<double>::quiet_NaN()};
    };
    /// One named local column, or one station's level direction.
    struct Target {
        std::string  label;      ///< The requested label (@c LOC:...), or @c GFDIR:<station>.
        std::string  station;    ///< The station.
        std::string  key;        ///< The label's key (@c GFDIR for the level direction).
        std::string  name;       ///< The resolved column's name, or the direction's loadings; empty when unresolved.
        std::string  note;       ///< Why the target is unresolved; empty when resolved.
        bool         resolved{false};
        Eigen::Index local_col{-1};   ///< The local column (-1 for @c GFDIR and when unresolved).
        Eigen::Index n_local{0};      ///< The station's local width (0 when the station is not in the frame).
        std::vector<Part> parts;      ///< @c ROW, @c FOLD, @c FOLDLOS, @c KIND, the whole-station terms, then @c FBCOL, @c FBSTA, @c FBLOS, @c FBFOLD, @c NETSTA, @c NETSAT, @c REFINE, @c PERTURB.
        double attributed{0.0};   ///< Sum of the row and prior-entry shares.
        double kernel_free{0.0};  ///< The kernel's @f$p^\top N_{rr}^{-1}u_r@f$.
        double unattributed{0.0}; ///< @ref kernel_free minus @ref attributed.
        double feedback{0.0};     ///< @f$-(N_{sr}g)^\top x_s@f$.
        double sumzero{0.0};      ///< @f$p^\top c_{\text{sz}}@f$.
        double ddprior{0.0};      ///< @f$p^\top c_{\text{dd}}@f$.
        double total{0.0};        ///< @ref attributed + @ref feedback + @ref sumzero + @ref ddprior.
        double solved{0.0};       ///< The kernel's @f$p^\top x_r@f$.
        double closure_abs{0.0};  ///< @f$|\text{total} - \text{solved}|@f$.
        double closure_rel{0.0};  ///< @ref closure_abs over @f$|\text{solved}|@f$ (0 when both are 0).
        double scale{0.0};        ///< The largest |share| of any row, fold or whole-station term.
        bool   fb_split{false};   ///< The feedback's network split ran (the constrained operator factorized).
        double fb_network{0.0};   ///< @f$h^\top x_c@f$: the sum of the feedback's network split (@c FBTOTAL).
        double fb_closure{0.0};   ///< @f$|h^\top x_c - \text{feedback}|@f$ (@c FBCLOSURE).
        double refine_max{0.0};   ///< @c REFINE: the largest change of any part under one refinement step (m).
        std::string refine_part;  ///< @c REFINE: the part (record, channel, station, kind, satellite, signal, row).
        int    n_perturb{0};      ///< The @c PERTURB checks run (0 without the split).
        double perturb_max_err{0.0};  ///< The largest |check - share| of the @c PERTURB parts (m).
    };
    bool valid{false};             ///< The frame held the solve's @c x_s and the local parts.
    std::vector<Target> targets;   ///< The requested labels in order, then one @c GFDIR per station.
};

/**
 * @brief Decompose the named station-local solved increments of one frame (no file is
 *        written).  See @ref NetworkRhsLocalShares.
 *
 * @param f      A frame captured by @ref networkAttribCaptureFrame with the solve's
 *               @c x_s and @c local_readout (the armed dumps and a trip's current-epoch
 *               dump when a `LOC:` label is named); a frame without that capture yields
 *               @c valid false and every target unresolved.
 * @param dec    @ref networkRhsAttribDecompose of the same frame (its DD parts feed the
 *               feedback's network split).
 * @param labels The `LOC:` labels, in the order the readout keeps them; a label that does
 *               not resolve keeps its slot with a note.
 * @param gf_direction Also decompose each named station's geometry-free level direction.
 * @return The targets.
 */
NetworkRhsLocalShares networkRhsAttribLocalShares(const NetworkAttribFrame& f,
                                                  const NetworkRhsAttribDecomposition& dec,
                                                  const std::vector<std::string>& labels,
                                                  bool gf_direction = true);

/**
 * @brief Decompose and dump one captured frame (the attribution proper).
 *
 * Writes `attrib_<tow>.csv`, `rows_<tow>.csv` and `vec_<tow>.csv` under @p dir,
 * in the same format for hand-armed and trip dumps, including the closure row.
 * `rows_<tow>.csv` ends with the column @c kind, the row's contribution kind
 * (@ref contributionKindName of its tag; @c unset for a row without a tag).
 *
 * With @p columns non-empty it also writes `cols_<tow>.csv`
 * (@ref networkRhsAttribColumnShares): header
 * `week,tow,record,station,kind,sat,signal,n` followed by `rhs[<label>],sol[<label>]`
 * for each named column, one line per record:
 *
 *  - @c COLUMN: the field pair holds the shared column index (empty when absent) and 1
 *    when the datum pins it, else 0;
 *  - @c STAKIND, @c STALOS, @c SHARED, @c DDFOLD, @c DATUMPIN, @c KIND: the parts
 *    (@ref NetworkRhsColumnShares::Part);
 *  - @c TOTAL: the sums (@c rhs without, @c sol with the datum pin);
 *  - @c FULL: the kernel's reduced RHS and the recomputed constrained solution;
 *  - @c SOLVE: the driver's solution (the @c sol field; empty when the frame holds none);
 *  - @c CLOSURE_ABS and @c CLOSURE_REL: the closures of @c TOTAL against @c FULL.
 *
 * Every value is written with ten significant digits.  An empty @p columns writes
 * nothing beyond the three class-based files, whose bytes do not depend on it.  The
 * `cols_` file holds the shared labels of @p columns only; the `LOC:` labels go to
 * `loc_<tow>.csv` (@ref networkRhsAttribLocalShares), written when the frame holds the
 * station-local capture (the armed dumps and a trip's current-epoch dump; a retained
 * previous frame does not, and the labels are then named once in the log), with the header
 * `week,tow,label,station,name,local_col,record,channel,from_station,row,kind,sat,signal,obs,n,w,y,v_post,a_col,share,share_shared,check`
 * and, per target in label order (then each @c GFDIR target), the records:
 *
 *  - @c TARGET: @c name and @c local_col of the resolved column (the direction's loadings
 *    for @c GFDIR), @c n the station's local width, @c share the solved increment; an
 *    unresolved target has this line only, with its note in @c kind;
 *  - @c ROW (one per data row, with @c w, @c y, @c v_post and @c a_col), @c FOLD,
 *    @c FOLDLOS, @c KIND, @c FEEDBACK, @c SUMZERO, @c DDPRIOR: the parts
 *    (@ref NetworkRhsLocalShares::Part);
 *  - @c UNATTRIBUTED, @c TOTAL, @c SOLVED, @c CLOSURE_ABS, @c CLOSURE_REL in @c share;
 *  - the feedback's split (@ref NetworkRhsLocalShares): @c FBCOL, @c FBSTA, @c FBLOS,
 *    @c FBFOLD, then @c FBTOTAL and @c FBCLOSURE in @c share;
 *  - the station nets, each sorted by |net|: @c NETSTA, @c NETSAT (@c share the net,
 *    @c share_shared its shared-path part);
 *  - the checks of the single parts: @c REFINE, then the @c PERTURB groups with the
 *    attributed whole effect in @c share and the re-solved one in @c check.
 *
 * A value the readout does not have (a shared-path share when the constrained operator did
 * not factorize, a post-fit residual without @c x_s) is an empty cell.
 *
 * @param f       A frame captured by @ref networkAttribCaptureFrame.
 * @param log     Run logger (closure-violation warning and the dump line).
 * @param dir     Output directory (created if absent).
 * @param kind    Short tag naming the reason for the dump ("armed",
 *                "trip:sigma0", "trip-prev:sh_gate", …).  It appears in the log
 *                text only, so the CSVs stay byte-comparable across dump reasons.
 * @param columns The named columns (@c network.rhs_attrib_columns); empty = no
 *                `cols_` or `loc_` file.
 * @param warned  Optional: the `LOC:` labels (and the no-x_s condition) already named in
 *                the log this run, so that each is named once; nullptr names them at every
 *                dump.
 */
void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind,
                          const std::vector<std::string>& columns = {},
                          std::set<std::string>* warned = nullptr);

/**
 * @brief Post-solve reduced-RHS class attribution at the armed target epochs.
 *
 * Call after the epoch's solve (the kernel's reduced system and the station
 * LDLᵀ blocks must be current) and after the postfit sweep, from the driver's
 * SB_FRAMEDIAG block.  No-op unless @c network.rhs_attrib_tow ≥ 0 and this
 * epoch's TOW lies in the armed window.
 *
 * @param ctx          Run context (the dumped-epoch count and counters; owns no
 *                     attribution state).
 * @param config_      Full configuration (reads @c network.rhs_attrib_*).
 * @param log          Main run logger (arming and closure lines).
 * @param traces       Run traces (provides the log directory for the dump files).
 * @param se           This synced epoch (week/tow stamp).
 * @param stations     Network station list (names for the dump).
 * @param est          The solved estimator (kernel accessors and capture recorder).
 * @param plan_datum   The epoch's minimum-constraint datum pins (from
 *                     @c plan.shared_datum), attributed per pin.
 * @param asm_res      PASS A/B station assemblies (the row stacks to classify).
 * @param est_index_of Station → kernel slot map (−1 = not folded this epoch).
 * @param x_s          Optional: the epoch's shared solution as the driver holds it,
 *                     written beside the recomputed one in `cols_<tow>.csv` and read by
 *                     the station-local readout `loc_<tow>.csv`
 *                     (@c network.rhs_attrib_columns); nullptr = neither.
 */
void networkRhsAttribution(NetworkRunContext& ctx, const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           const NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations,
                           NetworkEstimator& est,
                           const NetworkEstimator::Datum& plan_datum,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           const Eigen::VectorXd* x_s = nullptr);

/**
 * @brief INC-5 flight recorder: consume a pending trip, then refresh the
 *        retained previous-epoch frame.
 *
 * Called from the SB_FRAMEDIAG slot right after @ref networkRhsAttribution, and
 * from the solve glue on the A4 withhold path, whose epoch never reaches
 * SB_FRAMEDIAG.  Two independent jobs, in this order:
 *
 *  1. If a trigger site raised @c ctx.carry.rhs_trip_pending this epoch, dump
 *     this epoch (bypassing the @c rhs_attrib_tow window) and, when
 *     @c rhs_attrib_on_trip_prev is set and a frame was retained, the previous
 *     epoch.
 *  2. Retain this epoch for the next one, unless @c rhs_attrib_on_trip_prev is
 *     off or the network has more stations than
 *     @c rhs_attrib_prev_max_stations.
 *
 * No-op unless @c network.rhs_attrib_on_trip.  Measure-only: the solve reads
 * nothing written here.
 *
 * @param ctx          Run context (the trip latch, the retained frame and counters).
 * @param config_      Full configuration (reads @c network.rhs_attrib_*).
 * @param log          Main run logger.
 * @param traces       Run traces (provides the log directory for the dump files).
 * @param se           This synced epoch (week/tow stamp).
 * @param stations     Network station list.
 * @param est          The solved estimator.
 * @param plan_datum   The epoch's minimum-constraint datum pins.
 * @param asm_res      PASS A/B station assemblies.
 * @param est_index_of Station → kernel slot map (−1 = not folded this epoch).
 * @param x_s          Optional: the epoch's shared solution as the driver holds it, read
 *                     by the current-epoch dump only (the `cols_` @c SOLVE line and the
 *                     station-local readout); the retained previous frame never holds one.
 *                     nullptr (the A4 withhold path) = neither.
 */
void networkRhsAttribTrip(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          const NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          const NetworkEstimator& est,
                          const NetworkEstimator::Datum& plan_datum,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of,
                          const Eigen::VectorXd* x_s = nullptr);

/**
 * @brief INC-5 trigger-site helper: record a QC condition and report its
 *        rising edge (the "first trip per era, one-shot until the era clears"
 *        rule of docs/SPEC_prefit_census_observability.md §7).
 *
 * Trigger sites call this on every epoch they evaluate their condition,
 * whatever its value: a call with @p active false clears the era and re-arms
 * the latch.  Every call is counted, so "armed, but no trigger site consulted
 * the latch" is distinguishable from "armed, and nothing happened"
 * (invariant #4).  A site that calls only on the true branch is miswired.
 *
 * @param ctx    Run context (latch set and counters).
 * @param kind   Stable era name ("sh_gate", "sigma0", "clk_runaway", "a00_band"),
 *               passed as a string literal.  It is copied into a std::string,
 *               so its lifetime does not matter.
 * @param active Whether the condition holds this epoch.
 * @return       true on the rising edge (inactive → active) while the recorder
 *               is armed; false otherwise.  After a true return
 *               @c ctx.carry.rhs_trip_pending is set; the first era to trip in
 *               an epoch names the dump.
 */
bool networkRhsTripEdge(NetworkRunContext& ctx, const char* kind, bool active);

} // namespace GnssProcessor
