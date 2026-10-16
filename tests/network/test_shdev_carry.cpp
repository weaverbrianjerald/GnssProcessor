/**
 * @file test_shdev_carry.cpp
 * @brief Unit gates for the exact per-LOS SH-deviation update
 *        (@ref shDevExactUpdate), the closed-form scalar posterior of the
 *        Gauss–Markov representativeness deviation given the solved pin
 *        functional (Iono-2, T5v2).
 *
 * The T5 v1 scalar filter was falsified as circular: it treated the
 * pin-regularized residual as an independent floor-noise observation, so its
 * variance collapsed to floor² after one update regardless of how little
 * information the epoch carried.  These gates check the v2 closed form against
 * its analytically derivable limits and show that its variance does not
 * collapse under weak data, unlike v1.
 *
 * Model under test (all one unit frame):
 *   prior d ~ N(d_pred, P);  pin row u − d = ε, ε ~ N(0, f²);  data-only
 *   likelihood u ~ N(u₀, U).  The solve returns û, V̂ = U·a/(U+a), a = P+f².
 *   Exact: d̂ = d_pred + (P/a)(û − d_pred);  P⁺ = P·(U+f²)/(U+a).
 */

#include <gtest/gtest.h>

#include "network/driver/RunTypes.hpp"

#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace GnssProcessor;

namespace {

// Forward-model helper: the solved posterior (û, V̂) of u given the data-only
// estimate (u0, U) and the RHS-shifted pin row (d_pred, P, f²).  This is the
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
    // only handle on u), V̂ = a.  The update leaves the variance unchanged.
    {
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, d_pred, a, f2);
        EXPECT_DOUBLE_EQ(d, d_pred);
        EXPECT_DOUBLE_EQ(Pp, P);
    }
    // V̂ ≥ a numerically (degenerate): the same no-information treatment.
    {
        const auto [d, Pp] = shDevExactUpdate(d_pred, P, 1.7, a * 1.01, f2);
        EXPECT_DOUBLE_EQ(Pp, P);
        // Mean still blends toward the reported û with the predicted gain.
        EXPECT_NEAR(d, d_pred + (P / a) * (1.7 - d_pred), 1e-15);
    }
    // U → 0 (data pins u exactly at u_true): P⁺ = P·f²/a, the standard scalar
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

// Non-collapse test: under a weak-data stream (per-epoch data-only variance
// U ≫ f²) the circular v1 recursion collapses its variance to ~f² after one
// update, while the exact v2 recursion holds P near the steady state set by U,
// τ and σ_ss.  Information accumulates at the rate the data supports instead of
// being counted again on every epoch.
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
    // v1 collapse: P is at f² scale after the first update.
    EXPECT_LT(P_v1, 3.0 * f2);
    // v2: P stays well above the floor, bounded below by the exact single-epoch
    // information rate (P⁺ = P·(U+f²)/(U+P+f²), with the GM leak re-inflating P
    // each epoch).  With U = 144·f² the steady state is about 39·f² (≈0.097),
    // roughly 40 times the collapsed v1 value.
    EXPECT_GT(P_v2, 30.0 * f2);
    EXPECT_GT(P_v2, 20.0 * P_v1);
    // Information still accumulates: the steady state is below the no-update GM
    // steady state.
    EXPECT_LT(P_v2, sig_ss2);
}

// Bayesian consistency: iterating {solve → exact update} with no GM leak
// reproduces the batch information sum.  The recursion's information form is
// 1/P⁺ = 1/P + 1/(U+f²) (each epoch is one independent observation of d at
// variance U+f², the pin noise plus the data-only noise), so after k epochs
// 1/P = 1/P₀ + k/(U+f²) up to rounding (the test allows 1e-6 relative).
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

// ---------------------------------------------------------------------------
// Contract-enforcing de-mean of the carried deviation priors (Iono-2 v3,
// SPEC_iono2_mechB_v3).  It keeps the common vertical deviation and a00, an
// exact quasi-null pair, from ratcheting against each other.  These gates check
// the projection properties.
// ---------------------------------------------------------------------------

namespace {

std::map<std::string, ShDevCarry> makeCarry(
    const std::vector<std::tuple<std::string, double, double, double>>& rows) {
    std::map<std::string, ShDevCarry> m;
    for (const auto& [key, dev, var, t_s] : rows) {
        ShDevCarry c;
        c.dev = dev; c.var = var; c.var_ss = 25.0; c.t_s = t_s;
        m[key] = c;
    }
    return m;
}

double weightedMean(const std::map<std::string, ShDevCarry>& m,
                    const std::string& sta_prefix = "") {
    double sw = 0.0, swd = 0.0;
    for (const auto& kv : m) {
        if (kv.second.t_s <= 0.0) continue;
        if (!sta_prefix.empty()
            && kv.first.substr(0, kv.first.find('|')) != sta_prefix) continue;
        const double w = 1.0 / kv.second.var;
        sw += w; swd += w * kv.second.dev;
    }
    return sw > 0.0 ? swd / sw : 0.0;
}

}  // namespace

// After the pass, the information-weighted global mean is exactly zero and the
// per-station means of qualifying stations are exactly zero.
TEST(ShDevDemean, RemovesGlobalAndStationMeans) {
    std::vector<std::tuple<std::string, double, double, double>> rows;
    // Two stations × 5 LOS each with distinct offsets + per-LOS structure.
    for (int i = 0; i < 5; ++i) {
        rows.emplace_back("AAAA|G0" + std::to_string(i), 3.0 + 0.1 * i, 4.0 + i, 100.0);
        rows.emplace_back("BBBB|G0" + std::to_string(i), -1.0 - 0.2 * i, 2.0 + i, 100.0);
    }
    auto m = makeCarry(rows);
    const auto st = shDevDemean(m, 4, 4);
    EXPECT_TRUE(st.applied);
    EXPECT_EQ(st.n_used, 10);
    EXPECT_EQ(st.n_sta, 2);
    EXPECT_NEAR(weightedMean(m), 0.0, 1e-12);
    EXPECT_NEAR(weightedMean(m, "AAAA"), 0.0, 1e-12);
    EXPECT_NEAR(weightedMean(m, "BBBB"), 0.0, 1e-12);
}

// Below the global threshold nothing is modified at all.
TEST(ShDevDemean, BelowGlobalThresholdUntouched) {
    auto m = makeCarry({{"AAAA|G01", 3.0, 4.0, 100.0},
                        {"AAAA|G02", 5.0, 4.0, 100.0}});
    const auto before = m;
    const auto st = shDevDemean(m, 30, 4);
    EXPECT_FALSE(st.applied);
    EXPECT_EQ(st.n_sta, 0);
    for (const auto& kv : before) {
        EXPECT_DOUBLE_EQ(m.at(kv.first).dev, kv.second.dev);
        EXPECT_DOUBLE_EQ(m.at(kv.first).var, kv.second.var);
    }
}

// A station below the per-station threshold keeps its globally de-meaned values
// (no station mean is removed for it), while qualifying stations go to zero mean.
TEST(ShDevDemean, ThinStationKeepsGlobalOnlyResidual) {
    std::vector<std::tuple<std::string, double, double, double>> rows;
    for (int i = 0; i < 6; ++i)
        rows.emplace_back("AAAA|G0" + std::to_string(i), 2.0 + 0.1 * i, 4.0, 100.0);
    rows.emplace_back("THIN|G07", 9.0, 4.0, 100.0);  // 1 LOS, below min_station 4
    auto m = makeCarry(rows);
    auto ref = m;
    const auto st = shDevDemean(m, 4, 4);
    EXPECT_TRUE(st.applied);
    EXPECT_EQ(st.n_sta, 1);  // only AAAA qualifies
    EXPECT_NEAR(weightedMean(m, "AAAA"), 0.0, 1e-12);
    // THIN got exactly the global subtraction, nothing else.
    EXPECT_NEAR(m.at("THIN|G07").dev, ref.at("THIN|G07").dev - st.g_tecu, 1e-12);
}

// Cold entries (t_s = 0) neither participate in the means nor get modified.
TEST(ShDevDemean, ColdEntriesExcludedAndUnmodified) {
    std::vector<std::tuple<std::string, double, double, double>> rows;
    for (int i = 0; i < 5; ++i)
        rows.emplace_back("AAAA|G0" + std::to_string(i), 1.0, 4.0, 100.0);
    rows.emplace_back("AAAA|G09", 500.0, 4.0, 0.0);  // cold; its large dev must not bias g
    auto m = makeCarry(rows);
    const auto st = shDevDemean(m, 4, 4);
    EXPECT_TRUE(st.applied);
    EXPECT_EQ(st.n_used, 5);
    EXPECT_NEAR(st.g_tecu, 1.0, 1e-12);          // cold 500 excluded from the mean
    EXPECT_DOUBLE_EQ(m.at("AAAA|G09").dev, 500.0);  // and unmodified
}

// v3k erase-policy split: the D5-only response keeps the accumulated level and
// re-taps the variance to steady state; the subsequent GM aging step is a no-op
// on that variance (λ²σ_ss² + (1−λ²)σ_ss² = σ_ss²) while the mean decays
// normally.  The retap discards confidence, not the level.
TEST(ShDevD5Retap, KeepsLevelRetapsVarianceAgingIdempotent) {
    ShDevCarry dc;
    dc.dev = 3.7; dc.var = 0.9; dc.var_ss = 25.0; dc.t_s = 100.0;
    shDevD5Retap(dc);
    EXPECT_DOUBLE_EQ(dc.dev, 3.7);        // level held
    EXPECT_DOUBLE_EQ(dc.var, 25.0);       // confidence dropped to steady state
    const double lam = std::exp(-30.0 / 900.0);
    const double var_aged = lam * lam * dc.var + dc.var_ss * (1.0 - lam * lam);
    EXPECT_NEAR(var_aged, dc.var_ss, 1e-12);  // aging is idempotent on the retap
}

// Variances are never touched (conservative contract), and an empty map is safe.
TEST(ShDevDemean, VariancesUntouchedAndEmptySafe) {
    std::map<std::string, ShDevCarry> empty;
    const auto st0 = shDevDemean(empty, 30, 4);
    EXPECT_FALSE(st0.applied);

    std::vector<std::tuple<std::string, double, double, double>> rows;
    for (int i = 0; i < 8; ++i)
        rows.emplace_back("AAAA|G0" + std::to_string(i), 2.0 + i, 1.0 + 0.5 * i, 100.0);
    auto m = makeCarry(rows);
    const auto ref = m;
    const auto st = shDevDemean(m, 4, 4);
    EXPECT_TRUE(st.applied);
    for (const auto& kv : ref)
        EXPECT_DOUBLE_EQ(m.at(kv.first).var, kv.second.var);
}

}  // namespace
