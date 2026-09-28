/**
 * @file test_sim_ambiguity_regime.cpp
 * @brief The integer-ambiguity draw and its two magnitude regimes.
 *
 * The suite names here (`SimAmbiguityRegime`, `SimAmbiguityRegimeLoader`) are
 * owned by this file alone; the repository keeps one suite per file with no
 * cross-file overlap.
 *
 * Several cases name the silent failure they guard: each failure produces a
 * fixture that looks normal but grades an estimator wrongly.
 *
 *  - a lost station salt  => every between-station DD is exactly 0, so ambiguity
 *    resolution reports a 100 % fix rate while measuring nothing;
 *  - a lost per-band independence => the wide-lane becomes one network-wide
 *    constant and one MW solve generalizes to the whole constellation;
 *  - a legacy regime kept under a field-looking config => the fixture is 4
 *    decades wider on disk than the field and defeats every zero-target cold
 *    start.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "sim/AmbiguityDraw.hpp"
#include "sim/SimConfig.hpp"

using namespace GnssProcessor;
using namespace GnssProcessor::sim;

namespace {

AmbiguityDrawConfig legacyCfg() {
    AmbiguityDrawConfig c;
    c.regime = AmbiguityRegime::Legacy;
    return c;
}

AmbiguityDrawConfig fieldCfg(int half_width) {
    AmbiguityDrawConfig c;
    c.regime = AmbiguityRegime::Field;
    c.half_width_cycles = half_width;
    return c;
}

/// A representative sweep for one station (one salt): 2 systems x 32 PRN x 3 bands.
std::vector<int> sweep(const AmbiguityDrawConfig& cfg, std::uint64_t salt) {
    std::vector<int> out;
    for (const char* sys : { "G", "E" })
        for (int prn = 1; prn <= 32; ++prn)
            for (int bi = 0; bi < 3; ++bi)
                out.push_back(drawAmbiguity(cfg, std::string(sys) + "01", prn, bi, salt));
    return out;
}

/// Write a temp sim-config JSON and load it; returns loadSimConfig's verdict.
bool tryLoad(const std::string& body, std::string& err, SimConfig& out) {
    static int seq = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("gnss_amb_regime_" + std::to_string(++seq) + ".json");
    { std::ofstream f(path); f << body; }
    const bool ok = loadSimConfig(path, out, &err);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// The draw
// ---------------------------------------------------------------------------

/// The default stays legacy; otherwise every archived fixture would change
/// meaning, without notice, the next time it is regenerated.
TEST(SimAmbiguityRegime, DefaultsToLegacy) {
    const AmbiguityDrawConfig c;
    EXPECT_EQ(c.regime, AmbiguityRegime::Legacy);
    for (int prn = 1; prn <= 32; ++prn) {
        const int n = drawAmbiguity(c, "G01", prn, 0, 0);
        EXPECT_GE(n, 100000);
        EXPECT_LE(n, 999999);
    }
}

/// Byte-identity anchor: the legacy path reproduces the exact pre-extraction
/// expression.  If it drifts, every existing fixture regenerates differently and
/// no archived A/B remains comparable.
TEST(SimAmbiguityRegime, LegacyReproducesTheHistoricalExpression) {
    const auto historical = [](const std::string& sat, int prn, int band_idx,
                               std::uint64_t seed_salt) -> int {
        unsigned long long h = 0x9E3779B97F4A7C15ull
            ^ (static_cast<unsigned long long>(static_cast<unsigned char>(sat[0])) << 56)
            ^ (static_cast<unsigned long long>(prn) << 32)
            ^ (static_cast<unsigned long long>(band_idx) * 0xD1B54A32D192ED03ull)
            ^ (seed_salt * 0xA24BAED4963EE407ull);
        h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27; h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<int>(100000ull + h % 900000ull);
    };
    const auto c = legacyCfg();
    for (const char* sys : { "G", "E" })
        for (int prn = 1; prn <= 32; ++prn)
            for (int bi = 0; bi < 3; ++bi)
                for (std::uint64_t salt : { 0ull, 1ull, 0x9E3779B9ull, 1234567890123ull })
                    EXPECT_EQ(drawAmbiguity(c, std::string(sys) + "01", prn, bi, salt),
                              historical(std::string(sys) + "01", prn, bi, salt))
                        << "sys=" << sys << " prn=" << prn << " bi=" << bi << " salt=" << salt;
}

/// The field regime stays inside [-W, +W] and spans the range: a draw collapsed
/// onto a few values would make the DD degenerate without leaving the declared
/// bounds.
TEST(SimAmbiguityRegime, FieldRegimeStaysInRangeAndSpansIt) {
    for (int W : { 1, 8, 48, 250, 100000 }) {
        const auto c = fieldCfg(W);
        std::set<int> seen;
        for (int prn = 1; prn <= 32; ++prn)
            for (int bi = 0; bi < 3; ++bi)
                for (std::uint64_t salt = 1; salt <= 40; ++salt) {
                    const int n = drawAmbiguity(c, "G01", prn, bi, salt);
                    ASSERT_GE(n, -W) << "W=" << W;
                    ASSERT_LE(n, W) << "W=" << W;
                    seen.insert(n);
                }
        if (W >= 48) {
            // 3840 draws over 2W+1 slots: both signs must appear, and the spread
            // must be a real fraction of the range rather than a cluster.
            EXPECT_LT(*seen.begin(), 0) << "W=" << W;
            EXPECT_GT(*seen.rbegin(), 0) << "W=" << W;
            EXPECT_GT(*seen.rbegin() - *seen.begin(), W) << "W=" << W;
        }
    }
}

/// The field regime lands in the measured field decade, which is the purpose of
/// the knob: eight bundled IGS stations (DOY-129, Melbourne-Wubbena, iono-free)
/// give median |N_WL| 28.5 cyc and median |DD N_WL| 29.7 cyc, against the legacy
/// draw's 268127 / 358244.  A regression that restored a wide draw would still
/// pass every range check above.
TEST(SimAmbiguityRegime, FieldRegimeMatchesTheMeasuredFieldDecade) {
    const auto c = fieldCfg(48);
    std::vector<int> wl, dd;
    for (int prn = 1; prn <= 32; ++prn)
        for (std::uint64_t sa = 1; sa <= 24; ++sa) {
            const int n1a = drawAmbiguity(c, "G01", prn, 0, sa);
            const int n2a = drawAmbiguity(c, "G01", prn, 1, sa);
            wl.push_back(n1a - n2a);
            const std::uint64_t sb = sa + 100;
            const int ref = 1 + (prn % 32);
            dd.push_back((n1a - drawAmbiguity(c, "G01", ref, 0, sa)) -
                         (drawAmbiguity(c, "G01", prn, 0, sb) -
                          drawAmbiguity(c, "G01", ref, 0, sb)));
        }
    const auto medAbs = [](std::vector<int> v) {
        for (auto& x : v) x = std::abs(x);
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // Field medians are 28.5 (WL) and 29.7 (DD).  The band is wide because the
    // claim under test is the decade, not a tuned match.
    EXPECT_GE(medAbs(wl), 5);
    EXPECT_LE(medAbs(wl), 150);
    EXPECT_GE(medAbs(dd), 5);
    EXPECT_LE(medAbs(dd), 200);
}

/// Silent failure guarded: without the station term every station gets identical
/// integers, so every between-station SD and DD is exactly zero and ambiguity
/// resolution reports a perfect fix rate while measuring nothing.  Pinned in both
/// regimes; the field regime's narrow range makes accidental collisions more
/// likely, so a broken salt is hardest to spot there.
TEST(SimAmbiguityRegime, StationSaltIsLoadBearingInBothRegimes) {
    for (const auto& c : { legacyCfg(), fieldCfg(48) }) {
        int n_zero_sd = 0, n_total = 0;
        for (int prn = 1; prn <= 32; ++prn)
            for (int bi = 0; bi < 3; ++bi) {
                const int a = drawAmbiguity(c, "G01", prn, bi, 11111ull);
                const int b = drawAmbiguity(c, "G01", prn, bi, 22222ull);
                if (a == b) ++n_zero_sd;
                ++n_total;
            }
        // A degenerate salt gives n_zero_sd == n_total.  Under the field regime a
        // few genuine collisions are expected (2W+1 = 97 slots), so the assertion
        // is "almost never", not "never".
        EXPECT_LT(n_zero_sd, n_total / 4) << "salt is not separating stations";
    }
}

/// Salt 0 is the single-station legacy value and must make its term vanish, so a
/// standalone synth reproduces the historical integers exactly.
TEST(SimAmbiguityRegime, SaltZeroIsTheHistoricalSingleStationDraw) {
    const auto c = legacyCfg();
    for (int prn = 1; prn <= 32; ++prn)
        EXPECT_EQ(drawAmbiguity(c, "G01", prn, 0, 0),
                  static_cast<int>(100000ull + ambiguityHash("G01", prn, 0, 0) % 900000ull));
}

/// Silent failure guarded: a formula without per-band independence (the pre-2026
/// one) makes the wide-lane N1-N2 the constant -137 on every satellite, so one
/// Melbourne-Wubbena solve generalizes to the whole constellation.  Per-band
/// draws stay independent in both regimes.
TEST(SimAmbiguityRegime, WideLaneIsNotConstantAcrossSatellites) {
    for (const auto& c : { legacyCfg(), fieldCfg(48) }) {
        std::set<int> wl;
        for (int prn = 1; prn <= 32; ++prn)
            wl.insert(drawAmbiguity(c, "G01", prn, 0, 7ull) -
                      drawAmbiguity(c, "G01", prn, 1, 7ull));
        EXPECT_GT(wl.size(), 8u) << "wide-lane collapsed toward a network-wide constant";
    }
}

/// Cross-process determinism: the network driver synthesizes each station in its
/// own child process, so a draw that depended on call order or on any shared
/// stream would desynchronize the roster without any error.
TEST(SimAmbiguityRegime, DrawIsPureAndOrderIndependent) {
    const auto c = fieldCfg(48);
    const auto a = sweep(c, 4242ull);
    auto b = sweep(c, 4242ull);
    EXPECT_EQ(a, b);
    // Interleaving a different station's draws must not perturb ours.
    std::vector<int> interleaved;
    for (const char* sys : { "G", "E" })
        for (int prn = 1; prn <= 32; ++prn)
            for (int bi = 0; bi < 3; ++bi) {
                (void)drawAmbiguity(c, std::string(sys) + "01", prn, bi, 999ull);
                interleaved.push_back(drawAmbiguity(c, std::string(sys) + "01", prn, bi, 4242ull));
            }
    EXPECT_EQ(a, interleaved);
}

/// The two regimes share one entropy source and differ only in the range
/// mapping, so an A/B over the regime moves the ambiguities and nothing else.
TEST(SimAmbiguityRegime, RegimesShareTheSameHash) {
    for (int prn = 1; prn <= 32; ++prn) {
        const std::uint64_t h = ambiguityHash("G01", prn, 1, 55ull);
        EXPECT_EQ(drawAmbiguity(legacyCfg(), "G01", prn, 1, 55ull),
                  static_cast<int>(100000ull + h % 900000ull));
        EXPECT_EQ(drawAmbiguity(fieldCfg(48), "G01", prn, 1, 55ull),
                  static_cast<int>(static_cast<std::int64_t>(h % 97ull) - 48));
    }
}

// ---------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------

/// An absent "ambiguity" block resolves to legacy, the byte-identity path.
TEST(SimAmbiguityRegimeLoader, AbsentBlockIsLegacy) {
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(tryLoad(R"({"noise": {"code_sigma_zen_m": 0.3}})", err, cfg)) << err;
    EXPECT_EQ(cfg.ambiguity.regime, AmbiguityRegime::Legacy);
}

TEST(SimAmbiguityRegimeLoader, AcceptsFieldRegimeAndHalfWidth) {
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(tryLoad(R"({"ambiguity": {"regime": "field", "half_width_cycles": 250}})",
                        err, cfg)) << err;
    EXPECT_EQ(cfg.ambiguity.regime, AmbiguityRegime::Field);
    EXPECT_EQ(cfg.ambiguity.half_width_cycles, 250);
}

TEST(SimAmbiguityRegimeLoader, FieldRegimeWithoutHalfWidthUsesTheCalibratedDefault) {
    SimConfig cfg;
    std::string err;
    ASSERT_TRUE(tryLoad(R"({"ambiguity": {"regime": "field"}})", err, cfg)) << err;
    EXPECT_EQ(cfg.ambiguity.regime, AmbiguityRegime::Field);
    EXPECT_EQ(cfg.ambiguity.half_width_cycles, 48);
}

/// Rejects an armed-but-inert config: one that reads as field-realistic while
/// the fixture on disk is still 1e5 cycles wide.
TEST(SimAmbiguityRegimeLoader, RejectsHalfWidthWithoutFieldRegime) {
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(tryLoad(R"({"ambiguity": {"half_width_cycles": 48}})", err, cfg));
    EXPECT_NE(err.find("half_width_cycles"), std::string::npos) << err;
    EXPECT_NE(err.find("inert"), std::string::npos) << err;
}

TEST(SimAmbiguityRegimeLoader, RejectsUnknownRegime) {
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(tryLoad(R"({"ambiguity": {"regime": "realistic"}})", err, cfg));
    EXPECT_NE(err.find("realistic"), std::string::npos) << err;
}

TEST(SimAmbiguityRegimeLoader, RejectsNonStringRegime) {
    SimConfig cfg;
    std::string err;
    EXPECT_FALSE(tryLoad(R"({"ambiguity": {"regime": 3}})", err, cfg));
}

TEST(SimAmbiguityRegimeLoader, RejectsOutOfRangeHalfWidth) {
    SimConfig cfg;
    std::string err;
    for (const char* body : { R"({"ambiguity": {"regime": "field", "half_width_cycles": 0}})",
                              R"({"ambiguity": {"regime": "field", "half_width_cycles": -5}})",
                              R"({"ambiguity": {"regime": "field", "half_width_cycles": 100001}})" }) {
        EXPECT_FALSE(tryLoad(body, err, cfg)) << body;
        EXPECT_NE(err.find("half_width_cycles"), std::string::npos) << err;
    }
}
