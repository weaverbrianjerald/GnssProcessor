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
 *
 * ## Knobs (network section; loader guards)
 *
 *  - @c network.rhs_attrib_tow   (double, default −1 = off): first target TOW.
 *  - @c network.rhs_attrib_n_ep  (int, default 2): number of target epochs.
 *  - @c network.rhs_attrib_columns (list of shared-column labels as `vec_<tow>.csv`
 *    prints them, e.g. `["CLK:G07", "CB:G07:C1W"]`; default empty = no `cols_` file):
 *    the named columns of the solution-share readout (card ICF instrument A').
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
#include <map>
#include <memory>
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
 */
void networkAttribCaptureFrame(NetworkAttribFrame& out, const SyncedEpoch& se,
                               const std::vector<NetStation>& stations,
                               const NetworkEstimator& est,
                               const NetworkEstimator::Datum& plan_datum,
                               const std::vector<StationAssembly>& asm_res,
                               const std::vector<int>& est_index_of,
                               bool own_assemblies,
                               const Eigen::VectorXd* x_s = nullptr);

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
 * nothing beyond the three class-based files, whose bytes do not depend on it.
 *
 * @param f       A frame captured by @ref networkAttribCaptureFrame.
 * @param log     Run logger (closure-violation warning and the dump line).
 * @param dir     Output directory (created if absent).
 * @param kind    Short tag naming the reason for the dump ("armed",
 *                "trip:sigma0", "trip-prev:sh_gate", …).  It appears in the log
 *                text only, so the CSVs stay byte-comparable across dump reasons.
 * @param columns The named columns (@c network.rhs_attrib_columns); empty = no
 *                `cols_` file.
 */
void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind,
                          const std::vector<std::string>& columns = {});

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
 *                     written beside the recomputed one in `cols_<tow>.csv`
 *                     (@c network.rhs_attrib_columns); nullptr = not reported.
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
