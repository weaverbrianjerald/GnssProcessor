#pragma once
/**
 * @file NetworkCoupling.hpp
 * @brief Card-1/1b/1d CONDITIONAL local-carry coupling-row builder for the
 *        PPP-RTK network producer — the per-station assembly of the
 *        pseudo-observation rows that couple each carried phase-leveling
 *        ambiguity / ZWD to the FREE shared border (Odijk S-conditional carry).
 *
 * Where the legacy phase-leveling carry folds an INDEPENDENT diagonal prior on
 * each carried local, the conditional carry (network.carry_conditional_coupling)
 * injects, per carried column, ONE row @f$A_r{:}{+}1@f$ / @f$A_s{:}M_{rj}@f$ with
 * @f$y=(\hat r_{\text{prev}}-r_{\text{lin}})+\sum_{\text{present}}M_{rj}\hat s_{\text{prev},j}@f$
 * and @f$\sigma^2=\text{var\_eff}+\sum_{\text{departed}}M_{rj}^2 P_{\text{prev},jj}@f$ —
 * removing the shared-info double count so the recursion equals the batch solve.
 * Card 1b (carry_coupling_joint_whiten) marginalizes the DEPARTED columns
 * JOINTLY (an @f$M_w@f$ side-block, whitened via @ref whitenCouplingRows) so the
 * between-row DD information the scalar path drops is preserved; Card 1d
 * (carry_coupling_time_update) shrinks each PRESENT column by
 * @f$\kappa_j=P_{\text{prev}}/(P_{\text{prev}}+q_j\Delta t)@f$ and re-projects the
 * shrink gauge-orthogonally (@ref CouplingRowBuilder::tuGaugeRestore); Card 1f
 * (carry_coupling_gauge_project) extends that same re-projection to cover the
 * TRUNCATED columns, whose deletion is what actually breaks the row's gauge
 * invariance — the departed-column defect this card exists to close.
 *
 * Seeded at decomposition INC-7b (2026-07-17), which relocated the coupling
 * cluster VERBATIM out of Application::networkPassB_
 * (ApplicationNetworkAssembly.cpp; see
 * docs/network_decomposition_plan.md).  @ref CouplingRowBuilder
 * is constructed FRESH per station inside the PASS-B OpenMP produce loop — it
 * models the former loop-body scratch (the four @c crows / @c craw / @c white_pw
 * / @c white_map containers) and the six former @c [&] lambdas as members over
 * explicit const references, so the bodies relocate byte-for-byte; it owns NO
 * cross-epoch state.  The coupling type vocabulary (@ref CoupledShared,
 * @ref CplRowRaw, @ref kCouplingSparsifyRelThresh, @ref whitenCouplingRows) and
 * the per-station slot @ref StationAssembly stay in filter/NetworkRunTypes.hpp
 * (this header @c \#includes it) — @ref whitenCouplingRows in particular stays
 * header-inline so its two callers (the builder here and the run-TU RAIM
 * refloat re-whiten) keep identical codegen.
 */

#include "filter/NetworkRunTypes.hpp"   // CoupledShared / CplRowRaw / LocalCarry / StationAssembly / whitenCouplingRows / SharedParamKind (via NetworkEstimator.hpp)

#include <Eigen/Dense>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct GlobalVtecShConfig;   // config/GnssConfigTypes.hpp — reference member only (q_per_degree read in the .cpp)

/// Card 1: one pending SCALAR-path conditional coupling row (present shared
/// terms kept as ordinary @f$A_s@f$ coefficients, departed terms already
/// marginalized into @c w).  Buffered per station in @ref CouplingRowBuilder and
/// batch-appended by @ref CouplingRowBuilder::appendCouplingRows.
struct PendCplRow {
    Eigen::Index local_col{0};
    std::vector<std::pair<Eigen::Index, double>> shared;  ///< (present free shared col, M_rj)
    double y{0.0};
    double w{0.0};
    /// F3 (network.phase_leveling_local_block): when NON-EMPTY, the row's local
    /// coefficients — (A_r column, coefficient) — REPLACE the scalar path's single
    /// +1 at @c local_col, which then names the row's PRIMARY column for the refloat
    /// bookkeeping (@c StationAssembly::cpl_row_local_cols).  A whitened block row
    /// is one row of L⁻¹, dense (lower-triangular) over the block's local columns.
    /// Empty on every scalar-path row, so the append writes exactly the +1 it
    /// always did (OFF = byte-identical).
    std::vector<std::pair<Eigen::Index, double>> local{};
};

/// F3 (network.phase_leveling_local_block): one injectable carried ambiguity
/// column of a station as the PASS-B inject pre-pass collected it — the input of
/// @ref CouplingRowBuilder::buildLocalBlock.  Every admission predicate (gauge pin,
/// pending reseed, relevel free-epoch latch, finite x_lin, live carry, gap ≤ max)
/// has already run: a column absent from this list is MARGINALIZED out of the
/// block by construction.
struct LocalBlockCol {
    Eigen::Index      col;      ///< A_r column of the carried ambiguity
    double            x_lin;    ///< this epoch's linearization point of that state (m)
    const LocalCarry* lc;       ///< the carry entry (value_abs, regression, harvest block + index)
    double            var_eff;  ///< FINISHED per-column prior variance: var + q·gap, floored, ×VCE factor (m²)
    double            gap;      ///< clamped inter-epoch gap (s) — the Card-1d κ time update's Δt
    double            vce_fac;  ///< the M2 v2 VCE factor α² already in @c var_eff (1 when off): the cross terms scale by √(α_i²·α_j²)
};

/// Card 1d: one buffered PRESENT-column term of a coupling row under the
/// border time update — the κ-shrunk coefficient is held back so the row
/// can be gauge-RE-PROJECTED before it is finalized.
struct TuEnt {
    Eigen::Index c;   ///< shared column
    double m_shr;     ///< κ-shrunk coefficient (post-restore = final)
    double m_un;      ///< unshrunk harvested coefficient
    double s_prev;    ///< last epoch's solved value (RHS term)
    char   cc;        ///< constellation letter ('\0' = none/SH)
    SharedParamKind kind{SharedParamKind::Clock};  ///< coupling kind
};

/// Card 1/1b/1d: the per-station conditional coupling-row builder.  Constructed
/// FRESH for each present carry-eligible station inside the PASS-B OpenMP produce
/// loop (never hoisted — fresh construction IS the per-station scratch reset), it
/// holds the former loop-body scratch (@c crows / @c craw / @c white_pw /
/// @c white_map) plus the shared slot @ref StationAssembly @c R and the const
/// inputs the six former @c [&] lambdas read.  @ref buildCplRow is called once per
/// carried column (ambiguity + ZWD, driver-side), then @ref appendCouplingRows
/// batch-appends the accumulated rows into @c R.A_s / @c R.A_r / @c R.w / @c R.y.
///
/// An aggregate (no user-declared constructor): the 10 leading const/reference
/// members are set by brace-init at the construction site; the four trailing
/// scratch containers default-initialize empty.  Owns NO cross-epoch state — the
/// carry maps it reads (@c tu_reinit_hist) live in NetworkRunContext and are
/// passed by const reference.
struct CouplingRowBuilder {
    const NetworkEstimator&    est;               ///< column layout (const queries only)
    const GlobalVtecShConfig&  shc;               ///< SH q ladder (couplingQ)
    double                     q_clock;           ///< Clock per-epoch process noise (m²/s)
    double                     q_cb;              ///< CodeBias process noise
    double                     q_pb;              ///< PhaseBias process noise
    bool                       carry_cpl_joint;   ///< Card 1b: joint-whiten departed cols
    bool                       carry_cpl_tu;      ///< Card 1d: border time update (κ shrink)
    bool                       carry_cpl_shmarg;  ///< Card 1d-SH: marginalize VtecSh terms
    /// Card 1f (network.carry_coupling_gauge_project): hold each (constellation,kind)
    /// coefficient sum at its HARVEST value when the row is truncated, instead of at the
    /// surviving-present value.  See @ref CouplingRowBuilder::tuGaugeRestore.
    bool                       carry_cpl_gauge;
    /// Card 1g (network.carry_coupling_reinit_sources): honour @c tu_reinit_hist even with
    /// the κ time update OFF, and probe the (kind,sat,signal) key as well as the sat-wide
    /// wildcard.  See @ref CouplingRowBuilder::tuReinitSince.
    bool                       carry_cpl_reinit;
    /// Card 1d/1g reinit history (kind,sat,signal)→last reinit-fold content time; read-only
    /// here.  An EMPTY signal is the SAT-WIDE wildcard (the clock-QC and datum-slip feeds
    /// mark a whole satellite; the realization-step feeds mark one (sat,signal) family).
    const std::map<std::tuple<SharedParamKind, std::string, std::string>, double>& tu_reinit_hist;
    StationAssembly&           R;                 ///< this thread's own PASS-B slot (= asm_res[i])

    std::vector<PendCplRow>    crows{};    ///< Card 1 scalar-path pending rows
    std::vector<CplRowRaw>     craw{};     ///< Card 1b joint-path pre-whitening ingredients
    std::vector<double>        white_pw{}; ///< P_w per M_w column (departed marginal var)
    std::map<std::string, int> white_map{};///< departed identity → M_w column
    /// READMIT re-coupling edge (network.readmit_coupling_marg_ep): satellite → loop-epoch
    /// index at which its post-re-admission marginalization EXPIRES.  While
    /// @c loop_ep < expiry every coupling term on that satellite's shared columns takes the
    /// DEPARTED route (σ² += M²·P_prev, coefficient dropped): no station block or row
    /// conditions on a column whose realization is still settling from its cold
    /// re-registration — the edge SPEC_amb_iono_split_overcount §6.9 measured as two ~4 h
    /// network-wide DD episodes after each eclipse re-admission cluster.  Null = off.
    const std::map<std::string, long long>* readmit_marg{nullptr};
    long long                                loop_ep{0};   ///< this epoch's n_loop_epochs (the expiry basis)

    /// True (and counted) when @p cs sits on a satellite inside its re-admission window.
    bool readmitDiverted(const CoupledShared& cs);
    /// Resolve @p cs to its shared-border column this epoch, or −1 if it has DEPARTED.
    Eigen::Index resolveSharedCol(const CoupledShared& cs) const;
    /// Stable identity key of a departed shared column (rows share one M_w column).
    static std::string couplingId(const CoupledShared& cs);
    /// Per-kind border process noise (m²/s) — the dense-fold q ladder + SH per-degree.
    double couplingQ(const CoupledShared& cs) const;
    /// Card 1d/1f gauge re-projection: hold each (constellation,kind) coefficient sum at
    /// the value the HARVEST regression had, so the row keeps observing the datum gauge
    /// exactly as much as its @f$A_r=+1@f$ local requires — and no more.
    ///
    /// The harvest identity is @f$M^{\mathsf T}g_s=-g_r(c)@f$, NOT @f$M^{\mathsf T}g_s=0@f$
    /// (a Galileo ambiguity row measures @f$\Sigma_E=-1@f$ exactly — the E gauge moves E
    /// ambiguities by +1; `docs/SPEC_card1_coupling_carry.md` §1).  So the correct target
    /// is the harvested sum, and "annihilate the sum" would itself inject a gauge
    /// observation on every row whose local carries gauge weight.
    ///
    /// @param ents      buffered PRESENT terms; @c m_shr is updated in place.
    /// @param trunc_sum Card 1f: per (constellation,kind) sum of the coefficients of the
    ///        TRUNCATED columns (departed, or reinit-forced onto the departed route).
    ///        @c nullptr reproduces the Card-1d behaviour byte-for-byte — the deficit is
    ///        then the κ shrink alone, which is correct only when nothing was truncated.
    ///
    /// Card 1d restored @f$\Sigma_{\text{present}}(m_{un}-m_{shr})@f$, which is blind to a
    /// DEPARTED column: deleting its coefficient leaves @f$\Sigma_{\text{family}}M=-M_{dep}@f$
    /// and the row becomes a high-weight observation of the CURRENT gauge pinned at the OLD
    /// realization (measured: 7 departures drove the smallest null eigenvalue 1e-11 → 9.43e-3,
    /// five decades over the gate cutoff, for 4 spurious OVER-specified withholds).  Folding
    /// the truncated coefficients into the SAME deficit fixes departures and the κ shrink in
    /// one expression, and reduces EXACTLY to Card 1d when nothing departed.
    void tuGaugeRestore(std::vector<TuEnt>& ents,
                        const std::map<std::pair<char, int>, double>* trunc_sum) const;
    /// True when the (kind,sat[,signal]) column reinit-folded after @p since_s (q=∞ marginalize).
    bool tuReinitSince(const CoupledShared& cs, double since_s) const;
    /// Build ONE coupling row for carried local column @p col (buffered, not yet appended).
    void buildCplRow(Eigen::Index col, double x_lin_val,
                     const LocalCarry& lc, double var_eff, double gap);
    /// Card 1 scalar-path INGREDIENTS of one coupling row — the loop body of
    /// @ref buildCplRow's scalar path, factored out so the F3 block path shares it
    /// verbatim (one predicate chain, no drift): fills @p row's @c local_col /
    /// @c shared / @c y and returns through @p sigma2 the row's
    /// σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt.  Returns false on the
    /// scalar path's own "never poison" screen (σ² ≤ 0 or non-finite RHS).  @c row.w
    /// is NOT set: the scalar caller writes 1/σ², the block caller puts σ² on Σ's
    /// diagonal.  Increments the same @c R.cpl_terms_* counters either way.
    bool scalarRowIngredients(Eigen::Index col, double x_lin_val,
                              const LocalCarry& lc, double var_eff, double gap,
                              PendCplRow& row, double& sigma2);
    /// F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount): build
    /// this station's injectable carried ambiguity set as ONE jointly-whitened local
    /// block instead of independent rows.  Per column the scalar-path ingredients
    /// (@ref scalarRowIngredients) supply +1/local, the present-shared regressor row,
    /// the RHS and σ² (Σ's diagonal); the off-diagonals are the HARVESTED conditional
    /// cross covariances (N_rr⁻¹)_{c_i c_j} read from the two entries' shared
    /// @ref PlHarvestBlock (VCE-factor scaled; 0 + counted when the two columns were
    /// harvested at different epochs).  Σ = LLᵀ, and the n rows L⁻¹·[I | M | y] are
    /// buffered as unit-weight multi-local @ref PendCplRow entries (appended by the
    /// scalar branch of @ref appendCouplingRows), with the pre-whitening ingredients
    /// retained in @c R.pl_blk for the within-epoch RAIM refloat re-whiten.  Exact
    /// for EVERY local-local direction given the border — the per-LOS iono direction
    /// F1 corrects AND the station-common rx clock / ZWD / rx-DCB directions it
    /// leaves over-counted.  Returns false when the factor is non-SPD after the
    /// ridge retry: nothing is buffered and the caller falls back to per-column rows
    /// (F1-scaled if that knob is also armed) and COUNTS it.  Columns refused by the
    /// scalar screen are dropped from the block and counted.  Not valid under
    /// @c carry_cpl_joint (the caller's predicate excludes it).
    bool buildLocalBlock(const std::vector<LocalBlockCol>& cols);
    /// Batch-append the buffered rows into @c R (joint-whitened, or scalar per-row).
    void appendCouplingRows();
};

/// SERIAL post-produce coupling observability (called once per epoch after the
/// PASS-B produce loop, unconditionally — the @c carry_cpl_cond guards are
/// internal): (1) fold each station's @c cpl_terms_* counts into the run totals,
/// then (2) the Card-1d one-shot debug dump of the first @c n_cpl_row_dump
/// assembled coupling rows (weight / RHS / A_s sparsity / gauge sums — the ground
/// truth of what @c addStation receives, for A/B-arm comparison; budget
/// decremented across epochs).  Log-only; touches @c est / @c asm_res read-only.
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

/// Card 3 (eq34) × Card 1 — SPEC_card1_coupling_carry §14: re-base the coupling
/// references that live on D4-S-transformed satellite phase-bias columns.
/// @c CoupledShared::s_prev and @c CarryPrior::value are literally the same
/// number for the same identity (both are @f$x_s(j)@f$ at harvest), so the D4
/// v2-shift that moves the carry values MUST move the matching coupling
/// @c s_prev by the SAME amount — else the row's two carried references land in
/// different frames and the coupling row injects exactly the frame step as row
/// bias (the row's functional @f$u = r + M^\top s@f$ is frame-consistent only if
/// ALL carried references move together; NetworkRelevel.cpp's Card-6 eph cutover
/// states the same principle for clock columns).  Touches @c PhaseBias entries
/// whose (sat, signal) is in @p shift_sat only — Clock/CodeBias/VtecSh columns
/// are not moved by the D4 shift, and receiver-side columns have no
/// @c SharedParamKind image at all.  Zero and non-finite shifts are skipped with
/// the same guards as the prev_carry walk.
/// @param coupling  one carried local's regression stash, mutated in place.
/// @param shift_sat the D4 v2-shift set, (sat, signal-group) → shift (m).
/// @return number of coupling terms re-based (counter fodder; 0 = none matched).
long long rebaseCouplingPhaseBiasSprev(
    std::vector<CoupledShared>& coupling,
    const std::map<std::pair<std::string, std::string>, double>& shift_sat);

} // namespace GnssProcessor
