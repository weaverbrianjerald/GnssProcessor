#pragma once
/**
 * @file NetworkCouplingExact.hpp
 * @brief The exact conditional time update of the carried coupling rows (card CTU,
 *        docs/refound/CTU_coupling_time_update_2026-09-28.md, sections 4 and 9.1).
 *
 * A carried local (a float ambiguity, a ZWD, a slant ionosphere) is stashed at the
 * harvest as a conditional row on the shared border s, @f$l = r + M^\top s + e@f$ with
 * @f$e\sim N(0,C)@f$ (card 1).  The border then moves, @f$s^+ = s + w@f$,
 * @f$w\sim N(0,Q)@f$.  With @f$P@f$ the stashed border posterior, the row's regression
 * on the moved border is
 *
 * @f[ l \mid s^+:\quad M_{new,S} = (P_{SS}+Q_S)^{-1}P_{S,K}M_K,\qquad
 *     \mathrm{Var}\,e' = C + M_K^\top\big[P_{KK} - P_{K,S}(P_{SS}+Q_S)^{-1}P_{S,K}\big]M_K @f]
 *
 * over the row's stashed columns @f$K = S\cup D@f$: @f$S@f$ the columns present this
 * epoch that the border fold carries with a finite inflation @f$Q_S@f$, @f$D@f$ the
 * columns the fold does not carry (departed, decoupled, re-realized or skipped).  The
 * coefficients of @f$D@f$ are re-attributed to @f$S@f$ through @f$P@f$, which is the
 * difference between this rule and the scalar kappa shrink of card 1d: a clock
 * coefficient re-attributed to the correlated phase-bias columns keeps the row's
 * co-movement with the satellite's level instead of stating it at the row's precision.
 * With @f$D@f$ empty and @f$P@f$ diagonal the rule is card 1d's kappa exactly.
 *
 * Both halves are evaluated in forms free of cancellation (with
 * @f$F=(P_{SS}+Q_S)^{-1}@f$, @f$g=Q_SM_S@f$, @f$u=P_{S,D}M_D@f$, @f$z=Fu@f$): the
 * coefficients as @f$m_{new} = M_S - Fg + z@f$ (from @f$FP_{SS} = I - FQ_S@f$, so a small
 * inflation moves the row by a small, accurately computed correction, and @f$Q=0@f$
 * returns the harvested row exactly), and the variance as
 * @f$M^\top BM = m_{new}^\top g + g^\top z + (M_D^\top P_{DD}M_D - u^\top z)@f$, using
 * @f$P_{SS}-P_{SS}FP_{SS} = P_{SS}FQ_S@f$.  The producer applies the per-row form (card
 * section 9.1 item 3): each row gets its own @f$M^\top BM@f$, and the time-update
 * covariance between rows is dropped, across stations and within a station alike.  The
 * same split gives that covariance (@ref couplingExactCross); the unit gate uses it for
 * its joint reference recursion, which equals the batch.  Adding it inside the station
 * blocks was measured on the sim bed (2026-09-28) and drove a level drift through the
 * S-basis completion, so the producer does not use it.
 *
 * The functions here are pure (no run context), so the unit gate
 * (tests/test_coupling_exact_update.cpp) drives the same code as the producer.
 */

#include <Eigen/Dense>

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Why a stashed border column is outside @c S this epoch (card CTU, section 9.1 item 5).
enum class CouplingExactDRoute : int {
    Present   = 0,   ///< in S: present and folded with a finite inflation
    Departed  = 1,   ///< absent from this epoch's layout
    Reinit    = 2,   ///< decorrelated by the fold (a clock-monitor reinit, inflate_q)
    Reref     = 3,   ///< skipped by the fold for a frame re-reference (phase-anchor or eq34 handover)
    Skipped   = 4,   ///< skipped by the fold for another rule (no prior entry, clock model, var_eff <= 0)
};

/// Number of @ref CouplingExactDRoute values (array sizing of the route counters).
inline constexpr int kCouplingExactRoutes = 5;

/// Per-station (and, summed, per-run) telemetry of the exact update (standing invariant 4).
struct CouplingExactCounters {
    long long rows_exact{0};         ///< rows updated by the one rule
    long long rows_stale{0};         ///< rows harvested before the stash that P belongs to: card 1d route
    long long rows_unfactored{0};    ///< rows met on an epoch whose P_SS + Q_S did not factor: card 1d route
    long long terms_s{0};            ///< harvested terms on an S column, updated exactly
    long long terms_d[kCouplingExactRoutes]{};   ///< harvested terms on a D column, re-attributed, by route
    long long terms_pinned{0};       ///< datum-pinned terms kept outside S and D with their coefficient
    long long terms_out_scalar{0};   ///< present terms with no dense row: scalar kappa at the fold's inflation
    long long terms_out_departed{0}; ///< terms with no dense row whose column departed or was re-realized: marginalized
    long long terms_out_sh{0};       ///< SH-VTEC terms (never in the dense block): the card 1d treatment
    long long terms_emitted{0};      ///< coefficients written to the rows (the dense spread over S included)
    long long gauge_held{0};         ///< clock-family deficits moved onto the constellation's pinned pivot
    long long gauge_orphan{0};       ///< clock-family deficits with no known pivot and no present clock of the constellation (the row stays gauge-dirty)
    long long gauge_spread{0};       ///< clock-family deficits whose pivot left the layout, spread over the constellation's present clocks
    long long sprev_mismatch{0};     ///< S terms whose stashed s_prev differs from the carried mean by > 1e-9 m
    long long var_negative{0};       ///< rows whose bracket term came out below 0 (rounding) and was clamped
    double    var_add_sum{0.0};      ///< summed per-row inflation M'BM (m^2), the mass the per-row form adds
    double    shrink_abs_sum{0.0};   ///< summed |M_S - m_new| over the S terms (how much of each row moved)

    void add(const CouplingExactCounters& o) {
        rows_exact += o.rows_exact; rows_stale += o.rows_stale;
        rows_unfactored += o.rows_unfactored; terms_s += o.terms_s;
        for (int k = 0; k < kCouplingExactRoutes; ++k) terms_d[k] += o.terms_d[k];
        terms_pinned += o.terms_pinned; terms_out_scalar += o.terms_out_scalar;
        terms_out_departed += o.terms_out_departed; terms_out_sh += o.terms_out_sh;
        terms_emitted += o.terms_emitted; gauge_held += o.gauge_held;
        gauge_orphan += o.gauge_orphan; gauge_spread += o.gauge_spread;
        sprev_mismatch += o.sprev_mismatch;
        var_negative += o.var_negative;
        var_add_sum += o.var_add_sum; shrink_abs_sum += o.shrink_abs_sum;
    }
};

/// The per-epoch inputs of the exact update: built serially once per epoch, before the
/// parallel PASS-B produce, and read-only inside it (the factor's solve is const).
///
/// The index space "P row" is the stashed dense border block (@c prev_carry_cov, the
/// clock, phase-bias and code-bias entries of the previous kept epoch, in the datum frame
/// of that epoch); "S index" is the subset carried into this epoch with a finite
/// inflation.  A default-constructed epoch has @c ok false, and every row then takes the
/// card 1d route.
struct CouplingExactEpoch {
    bool   ok{false};                    ///< built and factored this epoch
    double stash_s{0.0};                 ///< content time (continuous GPS s) of the stash @c P belongs to
    Eigen::MatrixXd P;                   ///< the stashed border posterior (m^2)
    std::map<std::string, int> p_of_key; ///< sharedCarryKey identity -> P row
    std::vector<int>  s_of_p;            ///< P row -> S index, or -1 (D)
    std::vector<int>  route_p;           ///< P row -> CouplingExactDRoute (as int)
    std::vector<char> clk_p;             ///< P row -> 1 for a satellite-clock key
    std::vector<char> cc_p;              ///< P row -> constellation letter ('\0' if none)
    std::vector<int>  p_of_s;            ///< S index -> P row
    std::vector<Eigen::Index> col_of_s;  ///< S index -> this epoch's shared column
    Eigen::VectorXd   q_s;               ///< S index -> Q_j, the fold's inflation of that key this epoch (m^2)
    Eigen::VectorXd   shat_s;            ///< S index -> the carried mean the fold centres its prior on (m)
    /// @f$F=(P_{SS}+Q_S)^{-1}@f$, formed once per epoch through an LDLT factor in
    /// correlation scaling.  Held explicitly (not as a factor object) because it is only
    /// ever applied to the small corrections @f$Q_SM_S@f$ and @f$P_{S,D}M_D@f$, and so
    /// that this header, which every network translation unit includes, instantiates no
    /// decomposition template.
    Eigen::MatrixXd   F;
    /// Pinned clock per constellation in the stash's datum (the clock pivot): its column
    /// this epoch and its pinned value.  The pivot takes each row's clock-family deficit,
    /// which keeps the row's identity along the declared clock gauge (the only null
    /// direction the rank gate declares on this posture); see @ref couplingExactRow.
    std::map<char, std::pair<Eigen::Index, double>> pivot;
    /// The fold's inflation of carried keys that have no dense row (they fold through the
    /// diagonal leftovers), keyed like @c p_of_key: the scalar fallback of those terms.
    std::map<std::string, double> q_outside;
    /// Off-diagonal process noise between S columns, (S index, S index, m^2), both orders
    /// listed (card CTU G5: the level noise of a switch-on seam, V u u' over a satellite's
    /// clock and phase-bias columns, whose diagonal is in @c q_s).  Empty on every other
    /// epoch, which leaves Q_S diagonal as before.
    std::vector<std::tuple<int, int, double>> q_off;

    /// Number of S columns.
    Eigen::Index nS() const { return static_cast<Eigen::Index>(p_of_s.size()); }
};

/// Factor @f$P_{SS}+Q_S@f$ of an epoch whose index maps, @c q_s and @c P are filled, in
/// correlation scaling (as the dense fold does, @c NetworkEstimator::addSharedPriorDense),
/// and set @c ok.  An empty S is a valid epoch (every stashed column re-attributes or
/// departs).
/// @return false (and @c ok false) when the scaled matrix is not positive definite or a
///         diagonal is not positive.
bool couplingExactFactor(CouplingExactEpoch& ep);

/// The exact update of one row's stashed terms (the one rule of card CTU section 4).
struct CouplingExactRow {
    bool ok{false};
    Eigen::VectorXd m_new;                    ///< new coefficients over S
    Eigen::VectorXd z;                        ///< F P_{S,D} M_D (the re-attributed part of m_new)
    Eigen::VectorXd u_d;                      ///< P_{S,D} M_D
    Eigen::VectorXd m_s;                      ///< the harvested coefficients over S (dense)
    std::vector<std::pair<int, double>> g;    ///< (S index, Q_j M_j) over the row's own S terms with Q_j > 0
    std::vector<std::pair<int, double>> m_d;  ///< (P row, M_j) over the row's D terms
    double var_add{0.0};                      ///< M'BM, the row's inflation (m^2), clamped at 0
    double var_raw{0.0};                      ///< the same before the clamp (telemetry)
    /// Per constellation: the harvested clock coefficients over the row's stashed clock
    /// terms (S and D) minus the new clock coefficients over S.  Adding it to the
    /// constellation's pinned pivot keeps the clock-family sum at its harvest value.
    std::map<char, double> clk_deficit;
    int n_s{0};                               ///< the row's stashed terms in S
    int n_d_route[kCouplingExactRoutes]{};    ///< the row's stashed terms in D, by route
};

/// Update one row.  @p terms are (P row, harvested coefficient) pairs, one per stashed
/// term of the row (a repeated P row is summed).  @p ep must be @c ok.
CouplingExactRow couplingExactRow(const CouplingExactEpoch& ep,
                                  const std::vector<std::pair<int, double>>& terms);

/// The time-update covariance @f$M_a^\top B M_b@f$ of two rows updated against the same
/// epoch.  Equals @c a.var_raw for @p a == @p b.  Not used by the producer (per-row form);
/// the unit gate's joint reference recursion uses it.
double couplingExactCross(const CouplingExactEpoch& ep, const CouplingExactRow& a,
                          const CouplingExactRow& b);

}  // namespace GnssProcessor
