#pragma once
/**
 * @file Coupling.hpp
 * @brief Conditional local-carry coupling-row builder for the PPP-RTK network
 *        producer: the per-station assembly of the pseudo-observation rows that
 *        couple each carried phase-leveling ambiguity or ZWD to the free shared
 *        border (Odijk S-conditional carry).
 *
 * The legacy phase-leveling carry folds an independent diagonal prior on each
 * carried local.  The conditional carry (network.carry_conditional_coupling)
 * instead injects one row per carried column, @f$A_r{:}{+}1@f$ / @f$A_s{:}M_{rj}@f$, with
 * @f$y=(\hat r_{\text{prev}}-r_{\text{lin}})+\sum_{\text{present}}M_{rj}\hat s_{\text{prev},j}@f$
 * and @f$\sigma^2=\text{var\_eff}+\sum_{\text{departed}}M_{rj}^2 P_{\text{prev},jj}@f$.
 * This removes the double count of shared information, so the recursion equals the
 * batch solve.  The time update (carry_coupling_time_update) shrinks each present column by
 * @f$\kappa_j=P_{\text{prev}}/(P_{\text{prev}}+q_j\Delta t)@f$ and re-projects the
 * shrink gauge-orthogonally (@ref CouplingRowBuilder::tuGaugeRestore).  The gauge projection
 * (carry_coupling_gauge_project) extends the same re-projection to the truncated
 * columns, whose deletion breaks the row's gauge invariance.
 *
 * The coupling code was moved unchanged out of Application::networkPassB_
 * (ApplicationNetworkAssembly.cpp) at decomposition step INC-7b; see
 * docs/network_decomposition_plan.md.  @ref CouplingRowBuilder is constructed fresh
 * per station inside the PASS-B OpenMP produce loop.  It holds the per-station
 * scratch (the pending-row container @c crows) and exposes the former @c [&] lambdas
 * as member functions over explicit const references, so the function bodies are
 * unchanged; it owns no cross-epoch state.  The coupling type vocabulary
 * (@ref CoupledShared, @ref CplRowRaw, @ref kCouplingSparsifyRelThresh) and the
 * per-station slot @ref StationAssembly are defined in network/driver/RunTypes.hpp,
 * which this header @c \#includes.
 */

#include "network/driver/RunTypes.hpp"   // CoupledShared / CplRowRaw / LocalCarry / StationAssembly / SharedParamKind (via Estimator.hpp)
#include "network/carry/CouplingExact.hpp"  // CouplingExactEpoch / CouplingExactRow
#include "network/state/Entities.hpp"   // net::SatSignalTable (the reinit history)

#include <Eigen/Dense>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <cstdint>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct GlobalVtecShConfig;   // config/GnssConfigTypes.hpp; reference member only (q_per_degree read in the .cpp)
struct SharedHistoryState;   // network/carry/SharedHistory.hpp; pointer member only

/// One pending scalar-path conditional coupling row: the present shared
/// terms are kept as ordinary @f$A_s@f$ coefficients, and the departed terms are
/// already marginalized into @c w.  Buffered per station in @ref CouplingRowBuilder and
/// batch-appended by @ref CouplingRowBuilder::appendCouplingRows.
struct PendCplRow {
    Eigen::Index local_col{0};
    std::vector<std::pair<Eigen::Index, double>> shared;  ///< (present free shared col, M_rj)
    double y{0.0};
    double w{0.0};
    /// Local coefficients of a whitened block row (F3, network.phase_leveling_local_block).
    /// When non-empty, these (A_r column, coefficient) pairs replace the scalar path's
    /// single +1 at @c local_col, and @c local_col then names the row's primary column
    /// for the refloat bookkeeping (@c StationAssembly::cpl_row_local_cols).  A whitened
    /// block row is one row of L⁻¹, dense (lower-triangular) over the block's local
    /// columns.  Empty on every scalar-path row, so the append writes the single +1
    /// (off = byte-identical).
    std::vector<std::pair<Eigen::Index, double>> local{};
    /// The row's contribution tag: @c CouplingScalar on a scalar row, the
    /// primary column's block kind on a whitened block row, with that column's identity.
    /// @ref CouplingRowBuilder::appendCouplingRows appends it to @c StationAssembly::row_tags
    /// in lockstep with the row.  Default @c Unset: a row no builder tagged is caught by
    /// the tag check (@c checkStationTags).
    RowTag tag{};
};

/// One injectable carried column of a station as collected by the PASS-B inject
/// pre-pass (F3, network.phase_leveling_local_block); the input of
/// @ref CouplingRowBuilder::buildLocalBlock.  Every admission predicate (gauge pin,
/// pending reseed, relevel free-epoch latch, finite x_lin, live carry, gap ≤ max) has
/// already run, so a column absent from this list is marginalized out of the block.
struct LocalBlockCol {
    Eigen::Index      col;      ///< A_r column of the carried ambiguity
    double            x_lin;    ///< this epoch's linearization point of that state (m)
    const LocalCarry* lc;       ///< the carry entry (value_abs, regression, harvest block + index)
    double            var_eff;  ///< final per-column prior variance: var + q·gap, floored, times the VCE factor (m²)
    double            gap;      ///< clamped inter-epoch gap (s); the Δt of the κ time update
    double            vce_fac;  ///< the M2 v2 VCE factor α² already in @c var_eff (1 when off); the cross terms scale by √(α_i²·α_j²)
    /// The entry is the station's carried tropospheric ZWD column, not an ambiguity
    /// (B9-in-block, @c network.carry_zwd_in_block).  The builder treats it like any
    /// other carried local column; the flag only splits its telemetry
    /// (@c pl_blk_zwd_cols, @c pl_blk_zwd_missing_cross) out of the ambiguity
    /// population.  Default false, so an existing 6-element brace-init keeps its meaning.
    bool              is_zwd{false};
    /// The entry is one of the station's carried per-LOS slant-iono columns, not an
    /// ambiguity (I-in-block, @c network.carry_iono_in_block).  Like @c is_zwd it is
    /// telemetry only (the builder's algebra never reads it); it splits
    /// @c pl_blk_iono_cols / @c pl_blk_iono_missing_cross out of the ambiguity
    /// population.  Default false, so an existing 6- or 7-element brace-init keeps its
    /// meaning.
    bool              is_iono{false};
    /// The entry is one of the station's carried receiver-clock-family columns: the
    /// primary clock, or an ISB under @c network.carry_rx_clock_isb (RX-CLK-in-block,
    /// @c network.carry_rx_clock_in_block).  Like @c is_zwd / @c is_iono it is telemetry
    /// only (the builder's algebra never reads it); it splits @c pl_blk_rxclk_cols /
    /// @c pl_blk_rxclk_missing_cross out of the ambiguity population.  The clock's
    /// carried mean is the drift-extrapolated prediction; the injector supplies it by
    /// pointing @c lc at a predicted copy of the carry entry, so the builder needs no
    /// special case.  Default false, so an existing brace-init keeps its meaning.
    bool              is_rx_clk{false};
    /// With @c is_rx_clk set: the clock-family entry is one of the station's per-system
    /// ISB columns rather than its primary receiver clock (RX-CLK-in-block).  Telemetry
    /// only (read by @c pl_blk_rxclk_isb_cols alone).  The algebraic difference lives in
    /// the injector (an ISB is carried as a pure random walk, the clock about a carried
    /// drift), not here.
    bool              is_rx_isb{false};
    /// The entry is one of the station's carried receiver code-bias columns
    /// (RXCB-IN-BLOCK, @c network.carry_rx_code_bias_in_block).  Telemetry only, like
    /// @c is_rx_clk: it splits @c pl_blk_rxcb_cols / @c pl_blk_rxcb_missing_cross out of
    /// the ambiguity population.  Default false, so an existing brace-init keeps its
    /// meaning.
    bool              is_rx_cb{false};
    /// The column's Gauss-Markov transition coefficient (the phase-colour
    /// state): the injector passes a predicted copy of the carry entry (value and border
    /// coefficients already times phi) and the transitioned variance, and the builder
    /// scales the column's harvested cross covariances by phi as well
    /// (cov(phi x, y) = phi cov(x, y)).  1 (default) leaves every existing column
    /// unchanged: the scaling is skipped, so an existing brace-init keeps its meaning.
    double            phi{1.0};
    /// The entry is a phase-colour column.  Telemetry only: a refused column
    /// is recorded in @c R.pc_refused so the injector can give it a birth row.
    bool              is_pc{false};
    /// The column's satellite, where it has one (an ambiguity, a slant ionosphere, a phase
    /// colour): the identity of the block row whose primary column this is.
    /// The row's kind follows from the flags above (@ref localBlockColKind).
    SatId             sat{};
    /// The column's signal, where it has one (an ambiguity's phase signal, a receiver code
    /// bias's code signal).
    SignalId          signal{};
};

/**
 * @brief The contribution kind of the whitened block row whose primary column is @p c
 *        (from the column's family flags, the ones the builder's telemetry
 *        already reads).
 * @param c The block column.
 * @return @c BlockZwdCarry, @c BlockIonoCarry, @c BlockRxClockCarry (an ISB included),
 *         @c BlockRxCbCarry, @c BlockPhaseColourCarry, or @c BlockAmbiguityCarry for a
 *         column with no flag.
 */
ContributionKind localBlockColKind(const LocalBlockCol& c);

/// One buffered present-column term of a coupling row under the border time update.
/// The κ-shrunk coefficient is held back so that the row can be
/// gauge-re-projected before it is finalized.
struct TuEnt {
    Eigen::Index c;   ///< shared column
    double m_shr;     ///< κ-shrunk coefficient (post-restore = final)
    double m_un;      ///< unshrunk harvested coefficient
    double s_prev;    ///< last epoch's solved value (RHS term)
    char   cc;        ///< constellation letter ('\0' = none/SH)
    SharedParamKind kind{SharedParamKind::Clock};  ///< coupling kind
};

/// Per-station conditional coupling-row builder.  Constructed fresh for
/// each present carry-eligible station inside the PASS-B OpenMP produce loop and never
/// hoisted: fresh construction is the per-station scratch reset.  It holds the
/// per-station scratch (@c crows), the shared slot @ref StationAssembly @c R, and the
/// const inputs that the member functions read.  The driver calls @ref buildCplRow once
/// per carried column (ambiguity and ZWD); @ref appendCouplingRows then batch-appends
/// the accumulated rows into @c R.A_s / @c R.A_r / @c R.w / @c R.y.
///
/// An aggregate (no user-declared constructor): the 10 leading const, value and
/// reference members (@c est through @c R) are set by brace-init at the construction
/// site; the scratch container @c crows default-initializes empty, and @c readmit_marg /
/// @c loop_ep are assigned after construction when the re-admission window is armed.
/// Owns no cross-epoch state: the carry map it reads (@c tu_reinit_hist) lives in
/// NetworkRunContext and is passed by const reference.
struct CouplingRowBuilder {
    const NetworkEstimator&    est;               ///< column layout (const queries only)
    const GlobalVtecShConfig&  shc;               ///< SH q ladder (couplingQ)
    double                     q_clock;           ///< Clock per-epoch process noise (m²/s)
    double                     q_cb;              ///< CodeBias process noise
    double                     q_pb;              ///< PhaseBias process noise
    bool                       carry_cpl_tu;      ///< border time update, κ shrink
    /// Hold each (constellation, kind) coefficient sum at its harvest value when the row
    /// is truncated, instead of at the surviving-present value (knob
    /// network.carry_coupling_gauge_project).  See @ref CouplingRowBuilder::tuGaugeRestore.
    bool                       carry_cpl_gauge;
    /// Consult @c tu_reinit_hist even with the κ time update off, and probe the
    /// (kind, sat, signal) key as well as the sat-wide wildcard (knob
    /// network.carry_coupling_reinit_sources).  See @ref CouplingRowBuilder::tuReinitSince.
    bool                       carry_cpl_reinit;
    /// The satellite signals, for their reinit history (see
    /// @c net::SatSignalState::tu_reinit): kind → content time of the last reinit fold;
    /// read-only here (uncounted lookups, safe in the parallel produce).  The
    /// satellite-wide @c SignalId{} is the wildcard: the clock-QC and datum-slip feeds
    /// mark a whole satellite, and the realization-step feeds mark one (sat, signal)
    /// family.
    const net::SatSignalTable& sat_signals;
    StationAssembly&           R;                 ///< this thread's own PASS-B slot (= asm_res[i])

    std::vector<PendCplRow>    crows{};    ///< scalar-path pending rows
    /// Re-admission re-coupling edge (network.readmit_coupling_marg_ep): satellite →
    /// loop-epoch index at which its post-re-admission marginalization expires.  While
    /// @c loop_ep < expiry, every coupling term on that satellite's shared columns takes
    /// the departed route (σ² += M²·P_prev, coefficient dropped), so no station block or
    /// row is conditioned on a column whose realization is still settling from its cold
    /// re-registration.  This edge is the source of the network-wide DD episodes after
    /// eclipse re-admission clusters; see SPEC_amb_iono_split_overcount §6.9 for the
    /// measurement.  The windows are @c net::SatelliteState::readmit_marg of this table
    /// (uncounted lookups, safe in the parallel produce).  Null = off.
    const net::SatTable* sats{nullptr};
    long long                                loop_ep{0};   ///< this epoch's n_loop_epochs (the expiry basis)
    /// The exact conditional time update (network.carry_coupling_exact_update):
    /// this epoch's inputs, built serially before the produce loop.  Null = off (the plain
    /// time-update route, byte-identical).  A row takes the exact route when the epoch is @c ok and
    /// the row was harvested at the stash the epoch's dense block belongs to.
    const CouplingExactEpoch*                cxu{nullptr};
    /// network.carry_coupling_exact_cross: with @c cxu, add the exact update's
    /// bracket between two rows of a station's local block (@ref buildLocalBlock).  False
    /// = the per-row form (byte-identical).
    bool                                     cxu_cross{false};
    /// network.carry_coupling_shared_history: the held compression and this
    /// epoch's history columns (filter/NetworkSharedHistory), read-only here.  A row on the
    /// exact route emits its history terms on the registered columns; every other row
    /// (the time-update route, a stale generation) marginalizes them into its variance.  Null =
    /// off (byte-identical).
    const SharedHistoryState*                shx{nullptr};

    /// True (and counted) when @p cs sits on a satellite inside its re-admission window.
    bool readmitDiverted(const CoupledShared& cs);
    /// Resolve @p cs to its shared-border column this epoch, or −1 if it has departed.
    Eigen::Index resolveSharedCol(const CoupledShared& cs) const;
    /// Per-kind border process noise (m²/s): the dense-fold q ladder, and the per-degree
    /// SH value.
    double couplingQ(const CoupledShared& cs) const;
    /// Gauge re-projection: hold each (constellation, kind) coefficient
    /// sum at the value the harvest regression had, so that the row observes the datum
    /// gauge exactly as much as its @f$A_r=+1@f$ local requires, and no more.
    ///
    /// The harvest identity is @f$M^{\mathsf T}g_s=-g_r(c)@f$, not @f$M^{\mathsf T}g_s=0@f$
    /// (a Galileo ambiguity row measures @f$\Sigma_E=-1@f$ exactly, because the E gauge
    /// moves E ambiguities by +1; `docs/SPEC_card1_coupling_carry.md` §1).  The target is
    /// therefore the harvested sum; forcing the sum to zero would itself inject a gauge
    /// observation on every row whose local carries gauge weight.
    ///
    /// @param ents      buffered present terms; @c m_shr is updated in place.
    /// @param trunc_sum per (constellation, kind) sum of the coefficients of the
    ///        truncated columns (departed, or forced onto the departed route by a
    ///        reinit).  @c nullptr reproduces the plain time-update behaviour
    ///        byte-for-byte: the deficit is then the κ shrink alone, which is correct
    ///        only when nothing was truncated.
    ///
    /// The time update restores @f$\Sigma_{\text{present}}(m_{un}-m_{shr})@f$, which does not see a
    /// departed column: deleting its coefficient leaves @f$\Sigma_{\text{family}}M=-M_{dep}@f$,
    /// and the row becomes a high-weight observation of the current gauge pinned at the
    /// old realization.  The null-space gate then reads the system as over-specified and
    /// withholds.  Folding the truncated coefficients into the same deficit corrects
    /// departures and the κ shrink in one expression, and reduces exactly to the plain time update
    /// when nothing departed.  A family with truncated mass and no present recipient is
    /// counted (@c R.cpl_gauge_orphan) and left uncorrected.
    void tuGaugeRestore(std::vector<TuEnt>& ents,
                        const std::map<std::pair<char, int>, double>* trunc_sum) const;
    /// True when the (kind, sat[, signal]) column was reinit-folded after @p since_s
    /// (q = ∞, marginalize).  Always false with both @c carry_cpl_tu and
    /// @c carry_cpl_reinit off.
    bool tuReinitSince(const CoupledShared& cs, double since_s) const;
    /// Build one coupling row for carried local column @p col (buffered, not yet appended).
    /// @p sat and @p signal name the column (the row's tag carries them, with
    /// the kind @c CouplingScalar, which the builder sets); the defaults name none (a column
    /// with no satellite, the wet delay).
    void buildCplRow(Eigen::Index col, double x_lin_val,
                     const LocalCarry& lc, double var_eff, double gap,
                     const SatId& sat = SatId{}, const SignalId& signal = SignalId{});
    /// Scalar-path ingredients of one coupling row: the loop body of
    /// @ref buildCplRow's scalar path, factored out so that the F3 block path shares the
    /// same predicate chain.  Fills @p row's @c local_col / @c shared / @c y and returns
    /// through @p sigma2 the row's σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt.
    /// Returns false when σ² ≤ 0 or the RHS is non-finite (the row is not emitted).
    /// @c row.w is not set: the scalar caller writes 1/σ², and the block caller puts σ²
    /// on Σ's diagonal.  Increments the same @c R.cpl_terms_* counters in both cases.
    ///
    /// With @c cxu armed and the row harvested at its stash, the row takes the exact
    /// route (@ref exactRowIngredients) instead.  A row the exact route cannot take is
    /// counted (@c R.cxu.rows_stale / @c rows_unfactored) and built as before.
    /// @param xr_keep  when non-null and the row took the exact route, receives that route's
    ///        @ref CouplingExactRow (its pair term); left untouched (@c ok false)
    ///        on any other route.
    bool scalarRowIngredients(Eigen::Index col, double x_lin_val,
                              const LocalCarry& lc, double var_eff, double gap,
                              PendCplRow& row, double& sigma2,
                              CouplingExactRow* xr_keep = nullptr);
    /// The exact route of one row.  Terms on a stashed dense key go
    /// through @ref couplingExactRow (S updated, D re-attributed through P); a datum-pinned
    /// term (@c p_prev = 0) is kept as harvested; a present term with no dense row takes
    /// the time update's kappa at the fold's own inflation (@c cxu->q_outside); a departed or
    /// re-realized term with no dense row is marginalized; SH terms keep the plain time-update
    /// treatment.  The clock-family deficit goes to the constellation's pinned pivot.
    /// σ² = var_eff + Σ_departed M²P_prev + Σ_scalar M²κqΔt + M'BM, the per-row form (the
    /// time-update covariance between rows is added only by the block term, from
    /// @p xr_keep).  Same return and counter contract as @ref scalarRowIngredients.
    bool exactRowIngredients(Eigen::Index col, double x_lin_val, const LocalCarry& lc,
                             double var_eff, double gap, PendCplRow& row, double& sigma2,
                             CouplingExactRow* xr_keep = nullptr);
    /// Build this station's injectable carried columns as one jointly whitened local
    /// block instead of independent rows (F3, network.phase_leveling_local_block,
    /// SPEC_amb_iono_split_overcount).  Per column, the scalar-path ingredients
    /// (@ref scalarRowIngredients) supply the +1 local, the present-shared regressor
    /// row, the RHS and σ² (the diagonal of Σ).  The off-diagonals are the harvested
    /// conditional cross covariances (N_rr⁻¹)_{c_i c_j}, read from the two entries'
    /// shared @ref PlHarvestBlock and scaled by the VCE factor; a pair harvested at
    /// different epochs gets 0 and is counted.  With Σ = LLᵀ, the n rows
    /// L⁻¹·[I | M | y] are buffered as unit-weight multi-local @ref PendCplRow entries
    /// (appended by the scalar branch of @ref appendCouplingRows), and the
    /// pre-whitening ingredients are retained in @c R.pl_blk for the within-epoch RAIM
    /// refloat re-whiten.  The result is exact in every local-local direction given the
    /// border: the per-LOS iono direction, and also the station-common rx clock, ZWD
    /// and rx-DCB directions.
    /// Returns false when the factor is non-SPD after the ridge retry; nothing is then
    /// buffered, and the caller falls back to per-column rows and counts the fallback.
    /// Returns true with nothing buffered when no column survives.  Columns refused by
    /// the scalar screen are dropped from the block and counted.
    bool buildLocalBlock(const std::vector<LocalBlockCol>& cols);
    /// Batch-append the buffered rows into @c R (scalar per-row rows and whitened block rows).
    void appendCouplingRows();
};

/// Serial post-produce coupling observability, called once per epoch after the
/// PASS-B produce loop, unconditionally (the @c carry_cpl_cond guards are internal).
/// (1) Fold each station's @c cpl_terms_* and gauge/diversion counts into the run
/// totals; log one line per epoch that has gauge-projection orphans, naming the
/// (constellation, kind) families, and add them to @p cpl_orphan_fam_total when it is
/// non-null.  (2) The time-update debug dump of the first @c n_cpl_row_dump assembled
/// coupling rows of the run (weight, RHS, A_s sparsity, gauge sums: what
/// @c addStation receives, for A/B-arm comparison); the budget is decremented across
/// epochs.  Log-only; reads @c est and @c asm_res without modifying them.
void foldAndDumpCouplingRows(bool carry_cpl_cond,
                             const std::vector<StationAssembly>& asm_res,
                             const NetworkEstimator& est,
                             const std::vector<std::string>& registered_clock_sats,
                             const std::set<std::string>& shared_phase_keys,
                             const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                             double tow,
                             const std::shared_ptr<spdlog::logger>& log,
                             long long& n_cpl_terms_present,
                             long long& n_cpl_terms_departed,
                             long long& n_cpl_terms_shrunk,
                             long long& n_cpl_gauge_held,
                             long long& n_cpl_gauge_orphan,
                             long long& n_cpl_reinit_diverted,
                             long long& n_cpl_readmit_diverted,
                             int& n_cpl_row_dump,
                             std::map<std::pair<char,int>, std::pair<long long,double>>*
                                 cpl_orphan_fam_total = nullptr);

/// The step a satellite phase-bias shift applies to one carried reference of the column
/// (sat, signal): its carried value or a coupling term's @c s_prev (SPEC_card1_coupling_carry
/// §14; the eq34 chain datum, combined with the coupling carry; and the float vehicle's declared anchor
/// handover).  @c CoupledShared::s_prev and @c CarryPrior::value are the same number for the
/// same identity (both are @f$x_s(j)@f$ at harvest), so a shift that moves the carried value
/// must move the matching coupling @c s_prev by the same amount; otherwise the row's two
/// carried references are in different frames and the coupling row injects the frame step
/// as row bias (the row's functional @f$u = r + M^\top s@f$ is frame-consistent only if all
/// carried references move together).  The one rule of both phase-bias shifts, applied to
/// every reference they move through the traversal of carried references.
/// @param shift_sat the shift set, (sat, signal) -> shift (m).
/// @param sat       the reference's satellite.
/// @param sig       the reference's signal.
/// @return the step (m); 0 when the column is not in the set or its shift is zero or not
///         finite (the reference does not move).
double phaseBiasShiftOf(const std::map<std::pair<std::string, std::string>, double>& shift_sat,
                        const std::string& sat, const std::string& sig);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the coupling carry, reinit sources and gauge projection.
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in Coupling.cpp, print in this order, each gated on its own knob:
 *   - networkCouplingCarryRunEnd: the coupling carry.
 *   - networkCouplingProjectionRunEnd: coupling reinit sources and gauge projection.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkCouplingRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochPassA;              // network/driver/Epoch.hpp
struct EpochAssembly;           // network/driver/Epoch.hpp

// ---- PASS B serial stages, declarations (generated by scripts/nom_pb1_cut.py) ----

/**
 * @brief Records this epoch's reinit-history entries the coupling rows marginalize on.
 *
 * The inflate-q stash flags and the clock refloats (under 1d or 1g), and under 1g the anchor
 * handovers, eq34 re-references and datum-slip re-realizations. Serial, before PASS B's
 * parallel produce reads the history; called by networkPassB_.
 *
 * Epoch form (generated by scripts/nom_pb1_cut.py): a serial stage of
 * Application::networkPassB_, cut with the alias lines of networkPassB_ it reads.  Below its
 * bindings is the frozen text; a piece wholly inside one `if` returns early instead
 * (scripts/nom_pb1/declared.py).
 *
 * @param run The run (network/driver/Run.hpp). Bindings: run.ctx (ctx).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.clock.pl_now_s
 *           (pl_now_s).
 */
void networkCouplingReinitHistory(NetworkRun& run, const NetworkEpoch& ep);

// ---- End of the PASS B serial stages, declarations ----

// ---- PASS B station steps, declarations (generated by scripts/nom_pb1b_cut.py) ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp

/**
 * @brief F3: the station's local carry block over the collected columns, or its per-column
 *        fallback.
 *
 * Step `local_block` of kStationCarrySteps (network/assembly/StationBuild.hpp); its kinds:
 * BlockAmbiguityCarry, BlockZwdCarry, BlockIonoCarry, BlockRxClockCarry, BlockRxCbCarry,
 * BlockPhaseColourCarry, CouplingScalar. It returns at once unless the frozen enclosing
 * conditions legacy_local_inject && do_amb_carry. Records: pl_blk_*, pl_zwd*, pl_cb_injected,
 * pc_folded, pc_*_folds; st.l5b.blk_folds. The frozen text of networkPassB_'s produce loop
 * below its bindings.
 * @param carry The station's carry frame (the build, the priors, the coupling builder, the
 *              block columns).
 */
void networkStationLocalBlock(StationCarryFrame& carry);

/**
 * @brief Appends the station's pending coupling rows and block rows (the coupling region).
 *
 * Step `coupling_region` of kStationCarrySteps (network/assembly/StationBuild.hpp); its kinds:
 * CouplingScalar, BlockAmbiguityCarry, BlockZwdCarry, BlockIonoCarry, BlockRxClockCarry,
 * BlockRxCbCarry, BlockPhaseColourCarry. It returns at once unless `carry_cpl_cond`. Records:
 * cpl_row_start, the coupling census. The frozen text of networkPassB_'s produce loop below its
 * bindings.
 * @param carry The station's carry frame (the build, the priors, the coupling builder, the
 *              block columns).
 */
void networkCouplingRegionRows(StationCarryFrame& carry);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of Coupling, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct CouplingCounters {
    long long n_cpl_terms_present = 0, n_cpl_terms_departed = 0, n_cpl_terms_shrunk = 0;
    // On-path telemetry for the departed-column gauge defect.  `held` counts
    // (constellation, kind) families whose coefficient sum was restored to its harvest
    // value against a truncated column; `orphan` counts families whose truncated mass
    // had no present recipient, i.e. the rows the projection could not make
    // gauge-invariant.  Standing invariant #4: armed with both at 0 means no row was
    // ever truncated, so the projection is inert, not clean.
    long long n_cpl_gauge_held = 0, n_cpl_gauge_orphan = 0;
    /// Orphan attribution: run totals per (constellation, SharedParamKind), as the
    /// orphan count and the summed |truncated mass| left gauge-dirty.  Structurally a
    /// channel of two or more constellations (a row's minority family can lose all its
    /// present columns while the majority family survives); GPS-only arms measure 0.
    /// Fed by foldAndDumpCouplingRows, read at teardown.
    std::map<std::pair<char,int>, std::pair<long long,double>> cpl_orphan_fam_total;
    /// The denominator of the reinit-sources probe: terms diverted to the departed route by the widened
    /// (kind, sat, signal) probe specifically.  Armed with this at 0 means the
    /// realization-step feeds never matched a live coupling term: inert, not clean.
    long long n_cpl_reinit_diverted = 0;
};

struct SyncedEpoch;   // rinex/MultiRinexSync.hpp

/**
 * @brief Folds the stations' local-block telemetry (block counts and sizes, the per-family
 *        admitted, unfolded and missing-cross columns, the dimension histogram) into the run
 *        counters.
 *
 * Serial, after the parallel produce. Keyed on the local-block knob, not on whether a block was
 * built, so an armed but inert posture still prints its zeros at run end. Does nothing when
 * the knob is off.
 *
 * @param ctx     The run context: the counters are written.
 * @param asm_res The stations' assembled blocks, read.
 */
void networkLocalBlockTally(NetworkRunContext& ctx,
                            const std::vector<StationAssembly>& asm_res);

/**
 * @brief Folds the stations' coupling-term counts into the run totals and dumps the first
 *        assembled coupling rows (foldAndDumpCouplingRows), then latches the first warm
 *        epoch's present and departed split for the resume census.
 *
 * Serial, after the parallel produce. The census value is a difference of the two run totals
 * bracketing the fold, because the fold updates them in place and returns nothing.
 *
 * @param ctx                   The run context: the coupling totals and the resume census.
 * @param est                   The estimator (its column layout is read by the dump).
 * @param asm_res               The stations' assembled blocks, read.
 * @param registered_clock_sats The epoch's registered clock satellites.
 * @param shared_phase_keys     The epoch's shared phase-bias keys.
 * @param registered_code_keys  The epoch's registered code-bias (satellite, signal) pairs.
 * @param se                    The epoch (its time of week stamps the dump).
 * @param log                   The run logger.
 */
void networkCouplingTermTally(NetworkRunContext& ctx, const NetworkEstimator& est,
                              const std::vector<StationAssembly>& asm_res,
                              const std::vector<std::string>& registered_clock_sats,
                              const std::set<std::string>& shared_phase_keys,
                              const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                              const SyncedEpoch& se,
                              const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor
