/**
 * @file test_shdev_carry.cpp
 * @brief Iono-2 / T5v2 unit gates for the EXACT per-LOS SH-deviation update
 *        (@ref shDevExactUpdate) — the closed-form scalar posterior of the
 *        Gauss–Markov representativeness deviation from the solved pin
 *        functional.
 *
 * The T5 v1 scalar filter was falsified as CIRCULAR (Jul-10 2026): it treated
 * the pin-regularized residual as an independent floor-noise observation, so
 * its variance collapsed to floor² after one update regardless of how little
 * information the epoch actually carried.  These gates pin the v2 closed form
 * to its drop-in-derivable limits and PROVE the non-collapse property the v1
 * lacked — the falsifier the memory called for ("an exact-state version would
 * differ").
 *
 * Model under test (all one unit frame):
 *   prior d ~ N(d_pred, P);  pin row u − d = ε, ε ~ N(0, f²);  data-only
 *   likelihood u ~ N(u₀, U).  The solve returns û, V̂ = U·a/(U+a), a = P+f².
 *   Exact: d̂ = d_pred + (P/a)(û − d_pred);  P⁺ = P·(U+f²)/(U+a).
 */

#include <gtest/gtest.h>

#include "filter/NetworkRunTypes.hpp"

#include <cmath>

using namespace GnssProcessor;

namespace {

// Forward-model helper: the solved posterior (û, V̂) of u given the data-only
// estimate (u0, U) and the RHS-shifted pin row (d_pred, P, f²) — the exact
// two-Gaussian fusion the network solve performs after Schur-eliminating d.
struct SolvedPin { double u_hat; double v_hat; };
SolvedPin solvePin(double u0, double U, double d_pred, double P, double f2) {
    const double a = P + f2;
    const double S = U + a;
    return {u0 - (U / S) * (u0 - d_pred), U * a / S};
}

// The exact update reproduces the closed-form limits.
TEST(ShDevExactUpdate, ClosedFormLimits) {
    const double P = 0.64, f2 = 0.04, d_pred = 0.3;
    const double a = P + f2;

    // U → ∞ (uninformative epoch): the solve returns û = d_pred (the row is the
    // only handle on u), V̂ = a.  The update must be a NO-OP on the variance.
    {
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, d_pred, a, f2);
        EXPECT_DOUBLE_EQ(d, d_pred);
        EXPECT_DOUBLE_EQ(Pp, P);
    }
    // V̂ ≥ a numerically (degenerate) — same no-information treatment.
    {
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, 1.7, a * 1.01, f2);
        EXPECT_DOUBLE_EQ(Pp, P);
        // Mean still blends toward the reported û with the predicted gain.
        EXPECT_NEAR(d, d_pred + (P / a) * (1.7 - d_pred), 1e-15);
    }
    // U → 0 (data pins u exactly at u_true): P⁺ = P·f²/a — the classic scalar
    // KF update of d against a floor-noise observation of its true value.
    {
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, 1.0, 0.0, f2);
        EXPECT_NEAR(Pp, P * f2 / a, 1e-15);
        EXPECT_NEAR(d, d_pred + (P / a) * (1.0 - d_pred), 1e-15);
    }
    // Intermediate U: recover exactly P⁺ = P(U+f²)/(U+a) through the V̂ round-trip.
    for (const double U : {1e-3, 0.05, 0.3, 2.0, 50.0}) {
        const auto sp = solvePin(0.9, U, d_pred, P, f2);
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, sp.u_hat, sp.v_hat, f2);
        EXPECT_NEAR(Pp, P * (U + f2) / (U + a), 1e-12) << "U=" << U;
        EXPECT_NEAR(d, d_pred + (P / a) * (sp.u_hat - d_pred), 1e-12);
        EXPECT_GT(Pp, P * f2 / a);   // strictly above the perfect-data limit
        EXPECT_LT(Pp, P);            // strictly below the prior (some info arrived)
    }
}

// THE non-collapse falsifier: under a weak-data stream (per-epoch data-only
// variance U ≫ f²) the v1 circular recursion collapses its variance to ~f²
// after one update, while the exact v2 recursion holds P near the honest
// steady state set by U, τ, and σ_ss — information accumulates at the true
// rate instead of the incestuous one.
TEST(ShDevExactUpdate, NonCollapseVsCircularV1) {
    const double f2 = 0.0025;        // floor² (e.g. 0.05² m² mapped)
    const double sig_ss2 = 0.64;     // GM steady-state variance
    const double U = 0.36;           // per-epoch data-only variance ≫ f²
    const double lam = std::exp(-30.0 / 900.0);   // dt=30 s, τ=900 s aging
    const double lam2 = lam * lam;

    double P_v2 = sig_ss2, P_v1 = sig_ss2;
    for (int ep = 0; ep < 60; ++ep) {
        // GM aging (identical for both recursions).
        P_v2 = lam2 * P_v2 + sig_ss2 * (1.0 - lam2);
        P_v1 = lam2 * P_v1 + sig_ss2 * (1.0 - lam2);
        // v2: exact update through the solved posterior of u.
        const auto sp = solvePin(0.0, U, 0.0, P_v2, f2);
        P_v2 = shDevExactUpdate(0.0, P_v2, sp.u_hat, sp.v_hat, f2).second;
        // v1 (falsified): treat the solved residual as an independent
        // observation of d at floor noise: Kf = P/(P+f²), P *= (1−Kf).
        const double Kf = P_v1 / (P_v1 + f2);
        P_v1 *= (1.0 - Kf);
    }
    // v1 collapse: P ≈ f²-scale after the very first update (the σ-incest).
    EXPECT_LT(P_v1, 3.0 * f2);
    // v2 honesty: P stays far above the floor — bounded below by the exact
    // single-epoch information rate (P⁺ ≥ P·(U+f²)/(U+P+f²) with the GM leak
    // re-inflating each epoch).  With U = 144·f² the steady state must sit
    // orders of magnitude above the collapsed v1.
    EXPECT_GT(P_v2, 30.0 * f2);
    EXPECT_GT(P_v2, 20.0 * P_v1);
    // And it still ACCUMULATES: the steady state is below the no-update GM
    // steady state (information does flow, at the honest rate).
    EXPECT_LT(P_v2, sig_ss2);
}

// Bayesian consistency: iterating {solve → exact update} with no GM leak
// reproduces the batch information sum EXACTLY — the recursion's information
// form is 1/P⁺ = 1/P + 1/(U+f²) (each epoch is one independent observation of
// d at variance U+f², the pin noise plus the data-only noise), so after k
// epochs 1/P = 1/P₀ + k/(U+f²) to machine precision.
TEST(ShDevExactUpdate, InformationAddsAtBatchRateWithoutLeak) {
    const double f2 = 0.0025, U = 0.36, P0 = 0.64;
    double P = P0;
    const int k = 40;
    for (int ep = 0; ep < k; ++ep) {
        const auto sp = solvePin(0.0, U, 0.0, P, f2);
        P = shDevExactUpdate(0.0, P, sp.u_hat, sp.v_hat, f2).second;
    }
    const double info_batch = 1.0 / P0 + static_cast<double>(k) / (U + f2);
    EXPECT_NEAR(1.0 / P, info_batch, 1e-6 * info_batch);
}

}  // namespace
