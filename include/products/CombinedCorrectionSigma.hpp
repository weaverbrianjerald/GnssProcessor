#pragma once
/**
 * @file CombinedCorrectionSigma.hpp
 * @brief M4 (product contract v2): combined-correction formal-σ arithmetic.
 *
 * The pure covariance→σ math behind the wire tokens @c sigc / @c sigcc / @c rho,
 * extracted from the producer's per-epoch emit path so it can be unit-tested
 * against a hand-built covariance with KNOWN cross-terms (the emitter unit in
 * @c tests/test_network_combined_sigma.cpp) while production
 * (@c ApplicationNetwork) and the test share one code path.
 *
 * The estimated satellite-side correction a PPP-RTK user applies to one
 * observation is a SUM of shared-parameter estimates: the satellite clock C0
 * plus, per signal, the satellite phase bias (carrier) or code bias
 * (pseudorange).  Those estimates are strongly CORRELATED — during a
 * satellite's rise-reconvergence the individual clock / bias formal σ are
 * cm-level yet the COMBINED correction is mm-level, because the errors move
 * together (Zhang/Hou/Zha/Liu 2021 J Geod 95:91 Figs. 2–5; Psychas/Khodabandeh/
 * Teunissen 2024b J Geod 98:15 Eq. 12).  A consumer that weights by the
 * individual σ RSS over-deweights a fresh satellite's products by 10–100×; the
 * COMBINED σ lets it keep using them during reconvergence.  The cross-covariance
 * that makes the combined σ small is a direct element of the network's dense
 * shared posterior covariance (@c NetworkEstimator::sharedCovariance) whenever
 * both terms are co-resident columns (clock, code bias and phase bias all are;
 * see @c clockColumn / @c codeBiasColumn / @c phaseBiasColumn).
 *
 * IONO is deliberately EXCLUDED from the combined σ here: the network iono is a
 * GLOBAL SH-VTEC field (@c vtecCoeffColumn), not a per-satellite column, and it
 * is delivered to the consumer on its own channel (@c SsrSlantIono) carrying its
 * own per-LOS σ.  Folding it into @c sigc would double-count against that
 * channel.  So the combined phase/code σ is the honest quality of the
 * satellite-side clock+bias correction; the iono quality rides its own supply.
 */

#include <Eigen/Dense>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/// The per-satellite REFERENCE (code, phase) signal pair for the C-record combined
/// code σ (@c sigcc) + code/phase correlation (@c rho).  @c code is empty when the
/// satellite carries no code bias; @c phase is empty when no code signal has a
/// matching phase sibling (then @c rho is left absent — a combined code σ still emits).
struct CombinedCodeRef {
    std::string code;
    std::string phase;
};

/// Select the reference (code, phase) pair for @ref computeCombinedCorrectionSigma's
/// per-sat combined-code channel.  Prefers the first code signal whose PHASE sibling is
/// present so @c rho pairs a real code/phase observable; falls back to the first code
/// signal alone (no phase pair ⇒ rho absent).  The phase sibling is the RINEX
/// observation-type swap of the leading char (code @c C1W → phase @c L1W): a RINEX 3 obs
/// code is @c \<type\>\<band\>\<attribute\> with type ∈ {C,L,D,S}, so a code and its phase
/// share band+attribute and differ only in the leading letter.  Comparing the raw code
/// key against the phase-key set (disjoint 'C…' vs 'L…' namespaces) never matches — the
/// swap is mandatory.  @p code_signals is scanned in order (pass a sorted container for
/// deterministic selection); @p phase_signals is the set of the satellite's phase keys.
inline CombinedCodeRef selectCombinedCodeRef(const std::vector<std::string>& code_signals,
                                             const std::set<std::string>&    phase_signals) {
    CombinedCodeRef ref;
    for (const auto& code : code_signals) {
        if (code.empty()) continue;
        if (ref.code.empty()) ref.code = code;   // fallback: first code signal
        std::string phase_sib = code;
        phase_sib[0] = 'L';                        // RINEX obs-type swap: C1W → L1W
        if (phase_signals.count(phase_sib)) {      // prefer a signal with a phase pair
            ref.code  = code;
            ref.phase = phase_sib;
            break;
        }
    }
    return ref;
}

/// Result of @ref computeCombinedCorrectionSigma: the three M4 wire quantities.
/// Any quantity is left 0 when its inputs are unavailable / degenerate (a missing
/// column, a non-positive or non-finite variance) — 0 is the "absent" wire value,
/// so an unresolved key simply emits no token and the consumer falls back to v1.
struct CombinedCorrectionSigma {
    double sigc{0.0};   ///< Combined PHASE-correction 1σ (m): σ0·√Var(clk + phase_bias).
    double sigcc{0.0};  ///< Combined CODE-correction 1σ (m): σ0·√Var(clk + code_bias).
    double rho{0.0};    ///< Correlation of the combined code & combined phase corrections (σ0 cancels).
};

/// Variance of the SUM of two shared estimates from a covariance matrix:
/// Var(x_i + x_j) = cov(i,i) + cov(j,j) + 2·cov(i,j).  Caller guards the indices.
inline double combinedSumVariance(const Eigen::MatrixXd& cov,
                                  Eigen::Index i, Eigen::Index j) {
    return cov(i, i) + cov(j, j) + 2.0 * cov(i, j);
}

/**
 * @brief Compute the combined phase/code σ and their correlation for one
 *        satellite key from the shared posterior covariance.
 *
 * @param cov Dense shared posterior covariance (symmetric).  Pass the gauge-treated
 *            copy the caller already builds for the phase-bias channel (folded
 *            ensemble gauges nulled); datum-pinned columns should be excluded by the
 *            caller (a pin's 1/pin_scale variance is a gauge artefact, not a physical
 *            σ) — pass such a key with the offending column set to a negative index.
 * @param clk_col   Clock C0 column (@c clockColumn).  Negative / out-of-range ⇒ all
 *                  three outputs are 0 (the clock is the shared term of every combined
 *                  correction; without it there is nothing to combine).
 * @param pb_col    Phase-bias column (@c phaseBiasColumn) for this (sat, signal), or
 *                  < 0 to skip the combined-phase term (@c sigc / @c rho stay 0).
 * @param cb_col    Code-bias column (@c codeBiasColumn) for the reference signal, or
 *                  < 0 to skip the combined-code term (@c sigcc / @c rho stay 0).
 * @param sigma0    √-scaling on the formal covariance (the EWMA(σ̂0) the URA/B1b
 *                  channels apply); multiplies @c sigc / @c sigcc and CANCELS in
 *                  @c rho.  Pass @c std::max(1.0, ura_sigma0_ewma) to match B1/B1b.
 * @return The three quantities; each 0 when unavailable or numerically degenerate.
 */
inline CombinedCorrectionSigma computeCombinedCorrectionSigma(
    const Eigen::MatrixXd& cov, Eigen::Index clk_col, Eigen::Index pb_col,
    Eigen::Index cb_col, double sigma0) {
    CombinedCorrectionSigma out;
    const Eigen::Index n = cov.rows();
    auto valid = [&](Eigen::Index c) { return c >= 0 && c < n && c < cov.cols(); };
    if (!valid(clk_col)) return out;   // no shared clock term ⇒ nothing to combine
    if (!std::isfinite(sigma0) || sigma0 <= 0.0) sigma0 = 1.0;

    double var_phase = 0.0, var_code = 0.0;
    bool have_phase = false, have_code = false;
    if (valid(pb_col)) {
        var_phase = combinedSumVariance(cov, clk_col, pb_col);
        if (var_phase > 0.0 && std::isfinite(var_phase)) {
            out.sigc  = sigma0 * std::sqrt(var_phase);
            have_phase = true;
        }
    }
    if (valid(cb_col)) {
        var_code = combinedSumVariance(cov, clk_col, cb_col);
        if (var_code > 0.0 && std::isfinite(var_code)) {
            out.sigcc = sigma0 * std::sqrt(var_code);
            have_code = true;
        }
    }
    if (have_phase && have_code) {
        // Cov(clk+cb, clk+pb) = cov(c,c)+cov(c,p)+cov(b,c)+cov(b,p).
        const double cov_cross = cov(clk_col, clk_col) + cov(clk_col, pb_col)
                               + cov(cb_col, clk_col)  + cov(cb_col, pb_col);
        const double denom = std::sqrt(var_code * var_phase);
        if (denom > 0.0 && std::isfinite(denom) && std::isfinite(cov_cross)) {
            double r = cov_cross / denom;
            if (r < -1.0) r = -1.0;   // clamp against tiny numerical overshoot
            if (r >  1.0) r =  1.0;
            out.rho = r;
        }
    }
    return out;
}

} // namespace GnssProcessor
