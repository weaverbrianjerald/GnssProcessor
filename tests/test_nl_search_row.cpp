/**
 * @file test_nl_search_row.cpp
 * @brief Unit tests for the WL-constrained N_1 search functional
 *        (@c AmbiguityResolver::nlRowCoefficients).
 *
 * Two rows of the (L1,L2) ambiguity plane can observe the narrow-lane integer
 * N_1 once the widelane WL = N_1 − N_2 is known:
 *
 *   - the narrowlane row @f$(f_1,f_2)/(f_1+f_2)@f$ (legacy default), and
 *   - the exact iono-free row @f$(f_1^2,-f_2^2)/(f_1^2-f_2^2)@f$
 *     (@c estimation.ambiguity_resolution.nl_iono_free_row).
 *
 * The reference here is exact algebra, not an external vector: the iono-free
 * row's defining property @f$c_{IF}\cdot\alpha = 0@f$ is an identity, so these
 * assertions hold to machine precision and a sign slip cannot hide.
 *
 * Suite name @c NlSearchRowTest is owned solely by this file (suite-hygiene
 * rule: one suite, one file).
 */

#include <gtest/gtest.h>

#include "filter/AmbiguityResolver.hpp"
#include "common/Constants.hpp"

#include <cmath>

namespace GnssProcessor {

/// Test seam: nlRowCoefficients is a private static (see the friend
/// declaration in AmbiguityResolver.hpp).  Defined only in the test binary.
struct NlRowTestAccess {
    using Coefs = AmbiguityResolver::NlRowCoefficients;
    static Coefs row(double f1, double f2, double lam1, double lam2, bool iono_free) {
        return AmbiguityResolver::nlRowCoefficients(f1, f2, lam1, lam2, iono_free);
    }
};

} // namespace GnssProcessor

using namespace GnssProcessor;

namespace {

// GPS L1 / L2.
constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;

double lam(double f) { return Constants::SPEED_OF_LIGHT / f; }

/// Reduced observable for a given row, from a noise-free (N_1, N_2) pair with
/// an optional slant-iono error injected on the DD ambiguities (metres of L1
/// delay; band 2 scales by alpha_2 = (f1/f2)^2).
double reduced(const NlRowTestAccess::Coefs& c,
               double n1, double n2, double iono_l1_m = 0.0) {
    const double alpha2 = (kF1 / kF2) * (kF1 / kF2);
    // DD ambiguity states are carried in metres; an unmodelled slant-iono error
    // is absorbed into them with the ionospheric signature (+1, +alpha2).
    const double dd1 = lam(kF1) * n1 + iono_l1_m;
    const double dd2 = lam(kF2) * n2 + iono_l1_m * alpha2;
    return c.t_1 * dd1 + c.t_2 * dd2;
}

} // namespace

// ---------------------------------------------------------------------------
// Identity: both rows recover the same integer from a clean float
// ---------------------------------------------------------------------------

TEST(NlSearchRowTest, BothRowsRecoverN1FromNoiseFreeFloat) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    for (const bool iono_free : {false, true}) {
        const auto c = NlRowTestAccess::row(kF1, kF2, l1, l2, iono_free);
        for (const int n1 : {-7, 0, 3, 1234}) {
            for (const int wl : {-2, 0, 1, 5}) {
                const int    n2 = n1 - wl;
                const double y  = reduced(c, n1, n2);
                // The search variable is y + shift·WL and must land on N_1.
                EXPECT_NEAR(y + c.shift * wl, static_cast<double>(n1), 1e-6)
                    << "iono_free=" << iono_free << " n1=" << n1 << " wl=" << wl;
            }
        }
    }
}

TEST(NlSearchRowTest, BackTransformRecoversBothBandAmbiguities) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    for (const bool iono_free : {false, true}) {
        const auto c = NlRowTestAccess::row(kF1, kF2, l1, l2, iono_free);
        const int n1 = 42, wl = 3, n2 = n1 - wl;
        // y_reduced as the resolver stores it after back-shifting the fix.
        const double y_reduced_fixed = static_cast<double>(n1) - c.shift * wl;
        EXPECT_NEAR((y_reduced_fixed + c.shift * wl) * l1, n1 * l1, 1e-9);
        EXPECT_NEAR((y_reduced_fixed + c.bt_2  * wl) * l2, n2 * l2, 1e-9);
    }
}

TEST(NlSearchRowTest, NarrowlaneRowIsBitIdenticalToLegacyExpressions) {
    // Standing invariant #6: with nl_iono_free_row off the path is bit-identical,
    // not only algebraically equal.  The expected values are the expressions that
    // resolveWlConstrained used inline before the search functional was factored
    // out, compared with EXPECT_EQ rather than EXPECT_NEAR.
    const double f1 = kF1, f2 = kF2;
    const double lam1 = lam(f1), lam2 = lam(f2);
    const double f_sum = f1 + f2;
    const auto c = NlRowTestAccess::row(f1, f2, lam1, lam2, false);
    EXPECT_EQ(c.t_1,    f1 / (lam1 * f_sum));
    EXPECT_EQ(c.t_2,    f2 / (lam2 * f_sum));
    EXPECT_EQ(c.shift,  f2 / (f1 + f2));
    EXPECT_EQ(c.bt_2,  -f1 / f_sum);
}

TEST(NlSearchRowTest, BtCoefIsShiftMinusOne) {
    // N_2 = N_1 − WL forces this relation for any valid row.
    const double l1 = lam(kF1), l2 = lam(kF2);
    for (const bool iono_free : {false, true}) {
        const auto c = NlRowTestAccess::row(kF1, kF2, l1, l2, iono_free);
        EXPECT_NEAR(c.bt_2, c.shift - 1.0, 1e-12) << "iono_free=" << iono_free;
    }
}

// ---------------------------------------------------------------------------
// The defining property: c_IF · alpha = 0 exactly
// ---------------------------------------------------------------------------

TEST(NlSearchRowTest, IonoFreeRowIsInsensitiveToSlantIono) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    const auto c_if = NlRowTestAccess::row(kF1, kF2, l1, l2, true);
    const int n1 = 11, wl = -4, n2 = n1 - wl;

    const double clean = reduced(c_if, n1, n2, 0.0);
    // 1 TECU = 0.16237 m of L1 delay.  The injected 0.5, 5 and 50 TECU reach far
    // beyond anything a fix would tolerate on the narrowlane row.
    for (const double tecu : {0.5, 5.0, 50.0}) {
        const double perturbed = reduced(c_if, n1, n2, tecu * 0.16237);
        EXPECT_NEAR(perturbed, clean, 1e-9)
            << "iono-free row moved under " << tecu << " TECU";
    }
}

TEST(NlSearchRowTest, NarrowlaneRowCarriesKnownIonoSensitivity) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    const auto c_nl = NlRowTestAccess::row(kF1, kF2, l1, l2, false);
    const int n1 = 11, wl = -4, n2 = n1 - wl;

    const double clean     = reduced(c_nl, n1, n2, 0.0);
    const double perturbed = reduced(c_nl, n1, n2, 0.16237);  // exactly 1 TECU
    // Documented sensitivity: 0.959 NL cycles per TECU of slant iono.
    EXPECT_NEAR(std::abs(perturbed - clean), 0.959, 5e-3);
}

// ---------------------------------------------------------------------------
// The price: both amplifications are exactly (f1+f2)/(f1-f2)
// ---------------------------------------------------------------------------

TEST(NlSearchRowTest, IonoFreeRowAmplifiesWidelaneErrorByFrequencyRatio) {
    const double l1 = lam(kF1), l2 = lam(kF2);
    const auto c_nl = NlRowTestAccess::row(kF1, kF2, l1, l2, false);
    const auto c_if = NlRowTestAccess::row(kF1, kF2, l1, l2, true);

    const double expected = (kF1 + kF2) / (kF1 - kF2);   // 8.06 for GPS L1/L2
    EXPECT_NEAR(expected, 8.0587, 1e-3);

    // A 1-cycle widelane error displaces the N_1 target by |shift|.
    EXPECT_NEAR(std::abs(c_nl.shift), kF2 / (kF1 + kF2), 1e-12);   // 0.4380
    EXPECT_NEAR(std::abs(c_if.shift), kF2 / (kF1 - kF2), 1e-12);   // 3.5294
    EXPECT_NEAR(std::abs(c_if.shift) / std::abs(c_nl.shift), expected, 1e-9);

    // The row norm, and with it the searched sigma, scales by the same factor.
    const double norm_nl = std::hypot(c_nl.t_1, c_nl.t_2);
    const double norm_if = std::hypot(c_if.t_1, c_if.t_2);
    EXPECT_NEAR(norm_if / norm_nl, expected, 1e-9);
}

TEST(NlSearchRowTest, IonoFreeRowSumsToNarrowlaneWavelength) {
    // c_IF1·lam1 + c_IF2·lam2 = c/(f1+f2) = lambda_NL, exactly.  For this reason
    // the WL-conditioned N_1 step has a 10.70 cm wavelength rather than the 6.3 mm
    // step of the unconditioned iono-free combination of integer (N_1, N_2).
    const double l1 = lam(kF1), l2 = lam(kF2);
    const auto c_if = NlRowTestAccess::row(kF1, kF2, l1, l2, true);
    const double lambda_nl = Constants::SPEED_OF_LIGHT / (kF1 + kF2);
    EXPECT_NEAR(lambda_nl, 0.106953, 1e-6);
    // t = c_IF / lambda_NL, so (t_1·lam1 + t_2·lam2) must be exactly 1.
    EXPECT_NEAR(c_if.t_1 * l1 + c_if.t_2 * l2, 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Cross-constellation check: the guard threshold separates real widelane pairs
// from the intra-E5 pair, whose amplification is about 78.
// ---------------------------------------------------------------------------

TEST(NlSearchRowTest, AmplificationSeparatesWidelanePairsFromIntraE5) {
    struct Pair { const char* name; double f1, f2; double expect; };
    const Pair pairs[] = {
        { "GPS L1/L2",       1575.42e6, 1227.60e6,  8.06 },
        { "GPS L1/L5",       1575.42e6, 1176.45e6,  6.90 },
        { "Galileo E1/E5b",  1575.42e6, 1207.14e6,  7.56 },
        { "Galileo E5b/E5a", 1207.14e6, 1176.45e6, 77.67 },  // degenerate
    };
    for (const auto& p : pairs) {
        const auto c = NlRowTestAccess::row(p.f1, p.f2, lam(p.f1), lam(p.f2), true);
        const double amplification = (p.f1 + p.f2) / (p.f1 - p.f2);
        EXPECT_NEAR(amplification, p.expect, 0.02) << p.name;
        // shift is the WL displacement; the resolver's 20x guard must admit the
        // first three and reject the last.
        EXPECT_NEAR(std::abs(c.shift), p.f2 / (p.f1 - p.f2), 1e-9) << p.name;
        EXPECT_EQ(amplification < 20.0, std::string(p.name) != "Galileo E5b/E5a")
            << p.name << " sits on the wrong side of the guard";
    }
}
