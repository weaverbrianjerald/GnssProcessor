/**
 * @file test_pb_jump_decommon.cpp
 * @brief Card JT — the DE-COMMONED phase-bias discontinuity test
 *        (@c network.disc_jump_test_decommon).
 *
 * The emitter's phase jump test screens the RAW epoch-to-epoch @f$\Delta PB@f$ of
 * each (sat, signal) against @c 0.25\,\lambda.  It is FRAME-BLIND: a
 * satellite-COMMON phase-bias move is absorbed by a consumer's own receiver phase
 * clock, so the quantity that lands in a consumer ambiguity is the step MINUS the
 * network's median step on that signal (standing invariant #5).  CST §4 measured
 * the consequence on DOY-129 — a @f$-1.451@f$ m L1W frame move at TOW 276810 hid
 * G32 L1W's real @f$+1.409@f$ m step as a raw @f$-0.042@f$ m, 29.6x under its own
 * bar, and it landed UNDECLARED in a live consumer ambiguity.
 *
 * Pinned here, on a synthetic sky with no estimator in the loop:
 *  - a PURE common-mode move (every satellite steps by the same amount) declares
 *    NOTHING with the knob ON and every cell with it OFF — the RE-COUPLING edge of
 *    invariant #7, the declaration this test withholds;
 *  - one satellite's real step HIDDEN by an opposite common move is declared ON and
 *    missed OFF — the DECOUPLING edge, the CST §4 population reproduced exactly;
 *  - below @c NetworkProductEmitter::kDiscDecommonMinSats the armed test reproduces
 *    the RAW result cell for cell (a thin signal's median is not a frame), and says
 *    so with a @c FallbackRaw class on every cell;
 *  - OFF is byte-identical to a pre-JT emitter (counters AND the event vector);
 *  - the median is taken WITHIN a signal, never across a mixed L1/L2 population;
 *  - the S contract round-trips the knob (@c discdecom=) and an OFF contract writes
 *    the pre-JT S line byte-for-byte.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace fs = std::filesystem;
namespace {

const GnssTime kT0{2313, 259200.0};

/// GPS L1 / L2 nominal wavelengths and the 0.25-cycle bars they imply — the exact
/// numbers CST §4 quotes, so a threshold regression shows up here as a bar change.
constexpr double kBarL1 = 0.25 * 0.1902936727983649;   // ≈ 0.04757 m
constexpr double kBarL2 = 0.25 * 0.2442102134245683;   // ≈ 0.06105 m

/// Build one epoch from (sat -> (signal -> phase bias)).  Clock-only satellites are
/// not interesting here; every satellite carries a clock so the record shape matches
/// a real emission.
NetworkSolutionEpoch makeEpoch(
    const GnssTime& t,
    const std::vector<std::pair<std::string, std::map<std::string, double>>>& sats) {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = t;
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;
    for (const auto& [sat, biases] : sats) {
        NetworkSatSolution s;
        s.sat        = sat;
        s.has_clock  = true;
        s.clock_c0_m = 0.0;
        for (const auto& [sig, b] : biases)
            s.phase_bias[sig] = NetworkPhaseBias{b, true, 1};
        sol.sats.push_back(s);
    }
    return sol;
}

/// A single-signal (L1W) sky of @p n satellites whose biases are @p base + per-sat
/// offset (index * @p spread), so no two satellites share a value.
std::vector<std::pair<std::string, std::map<std::string, double>>>
makeSky(int n, double base, double spread) {
    std::vector<std::pair<std::string, std::map<std::string, double>>> out;
    for (int i = 0; i < n; ++i) {
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", i + 1);
        out.push_back({std::string(id), {{"L1W", base + i * spread}}});
    }
    return out;
}

/// The 0.25-cycle per-λ emitter both arms use (the RF-0hv production setting).
NetworkProductEmitter makeEmitter(bool decommon) {
    return NetworkProductEmitter(NetworkProductEmitter::kDefaultDiscThresholdM,
                                 /*emit_clock_disc=*/false,
                                 /*staleness_horizon_s=*/0.0,
                                 /*code_bias_jump_test=*/false,
                                 /*clock_disc_on_set_change=*/false,
                                 /*disc_threshold_frac_cycles=*/0.25,
                                 /*disc_jump_test_decommon=*/decommon);
}

/// Sum of every (sat, L1W) phase counter over an @p n-satellite sky.
int totalCounters(const NetworkProductEmitter& e, int n, const char* sig = "L1W") {
    int total = 0;
    for (int i = 0; i < n; ++i) {
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", i + 1);
        total += e.discontinuityCounter(id, sig);
    }
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) A PURE common-mode move: ON declares nothing, OFF declares every cell.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, PureCommonModeMoveDeclaresNothingArmedAndEverythingRaw) {
    constexpr int    kN     = 10;
    constexpr double kShift = -1.451;   // the CST §4 L1W frame move, to the metre

    SsrCorrectionStore s_on, s_off;
    auto e_on  = makeEmitter(true);
    auto e_off = makeEmitter(false);

    const GnssTime t1{kT0.week, kT0.tow + 30.0};
    // Epoch 0 seeds the baselines (first sighting never declares).
    e_on.emitToStore(makeEpoch(kT0, makeSky(kN, 100.0, 0.7)), s_on);
    e_off.emitToStore(makeEpoch(kT0, makeSky(kN, 100.0, 0.7)), s_off);
    ASSERT_EQ(totalCounters(e_on, kN), 0);
    ASSERT_EQ(totalCounters(e_off, kN), 0);

    // Epoch 1: EVERY satellite moves by exactly the same amount — a frame move, and
    // nothing a consumer's ambiguity can feel.
    e_on.emitToStore(makeEpoch(t1, makeSky(kN, 100.0 + kShift, 0.7)), s_on);
    e_off.emitToStore(makeEpoch(t1, makeSky(kN, 100.0 + kShift, 0.7)), s_off);

    EXPECT_EQ(totalCounters(e_on, kN), 0) << "the frame move must not be declared";
    EXPECT_EQ(totalCounters(e_off, kN), kN)
        << "the raw test declares every cell of a pure frame move";

    // Invariant #7's identity for the withheld edge: one raw_only_suppressed row per
    // cell, each carrying the frame it de-commoned against.
    const auto& ev = e_on.pbJumpDecommonEvents();
    ASSERT_EQ(ev.size(), static_cast<std::size_t>(kN));
    for (const auto& e : ev) {
        EXPECT_EQ(e.cls, NetworkProductEmitter::PbJumpDecommonClass::RawOnlySuppressed);
        EXPECT_FALSE(e.declared);
        EXPECT_EQ(e.population, kN);
        EXPECT_NEAR(e.raw_delta_m, kShift, 1e-9);
        EXPECT_NEAR(e.common_m,    kShift, 1e-9);
        EXPECT_NEAR(e.decommon_m,  0.0,    1e-9);
        EXPECT_NEAR(e.threshold_m, kBarL1, 1e-9);
    }
    // The OFF emitter records nothing at all (no allocation, no drain).
    EXPECT_TRUE(e_off.pbJumpDecommonEvents().empty());
}

// ---------------------------------------------------------------------------
// (2) One satellite's step HIDDEN by an opposite common move — the CST §4 cell.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, StepHiddenByOppositeFrameMoveIsDeclaredArmedAndMissedRaw) {
    constexpr int    kN     = 10;
    constexpr double kShift = -1.451;   // the frame
    constexpr double kReal  = +1.409;   // G10's own de-commoned step (CST §4's G32 L1W)

    SsrCorrectionStore s_on, s_off;
    auto e_on  = makeEmitter(true);
    auto e_off = makeEmitter(false);

    const GnssTime t1{kT0.week, kT0.tow + 30.0};
    e_on.emitToStore(makeEpoch(kT0, makeSky(kN, 100.0, 0.7)), s_on);
    e_off.emitToStore(makeEpoch(kT0, makeSky(kN, 100.0, 0.7)), s_off);

    // Every satellite takes the frame move; the LAST one additionally takes a real
    // +1.409 m step, so its RAW delta is -1.451 + 1.409 = -0.042 m — under the
    // 0.0476 m bar, exactly as the field measured.
    auto sky1 = makeSky(kN, 100.0 + kShift, 0.7);
    sky1.back().second["L1W"] += kReal;
    const std::string hidden_sat = sky1.back().first;

    e_on.emitToStore(makeEpoch(t1, sky1), s_on);
    e_off.emitToStore(makeEpoch(t1, sky1), s_off);

    EXPECT_EQ(e_off.discontinuityCounter(hidden_sat, "L1W"), 0)
        << "the raw test misses the hidden step (|-0.042| < 0.0476)";
    EXPECT_EQ(e_on.discontinuityCounter(hidden_sat, "L1W"), 1)
        << "the de-commoned test declares it (|+1.409| > 0.0476)";
    // ...and exactly ONE cell is declared armed: the other nine are pure frame.
    EXPECT_EQ(totalCounters(e_on, kN), 1);
    // The raw test, meanwhile, declared all NINE frame-only cells and missed the one
    // that mattered — the inversion this card is about.
    EXPECT_EQ(totalCounters(e_off, kN), kN - 1);

    // The identity row for the previously MISSED class.
    const auto& ev = e_on.pbJumpDecommonEvents();
    int n_decommon_only = 0, n_raw_only = 0, n_both = 0;
    for (const auto& e : ev) {
        switch (e.cls) {
            case NetworkProductEmitter::PbJumpDecommonClass::DecommonOnly:
                ++n_decommon_only;
                EXPECT_EQ(e.sat, hidden_sat);
                EXPECT_EQ(e.signal, "L1W");
                EXPECT_TRUE(e.declared);
                EXPECT_NEAR(e.raw_delta_m, kShift + kReal, 1e-9);
                EXPECT_NEAR(e.common_m,    kShift,         1e-9);
                EXPECT_NEAR(e.decommon_m,  kReal,          1e-9);
                break;
            case NetworkProductEmitter::PbJumpDecommonClass::RawOnlySuppressed:
                ++n_raw_only; EXPECT_FALSE(e.declared); break;
            case NetworkProductEmitter::PbJumpDecommonClass::Both: ++n_both; break;
            case NetworkProductEmitter::PbJumpDecommonClass::FallbackRaw:
                ADD_FAILURE() << "population " << e.population << " should not fall back";
                break;
        }
    }
    EXPECT_EQ(n_decommon_only, 1);
    EXPECT_EQ(n_raw_only, kN - 1);
    EXPECT_EQ(n_both, 0);
}

// ---------------------------------------------------------------------------
// (3) Below the population minimum the ARMED test reproduces the RAW result.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, BelowMinimumPopulationReproducesRawTest) {
    ASSERT_EQ(NetworkProductEmitter::kDiscDecommonMinSats, 3);
    const int kThin = NetworkProductEmitter::kDiscDecommonMinSats - 1;   // 2 satellites
    constexpr double kShift = -1.451;

    SsrCorrectionStore s_on, s_off;
    auto e_on  = makeEmitter(true);
    auto e_off = makeEmitter(false);

    const GnssTime t1{kT0.week, kT0.tow + 30.0};
    e_on.emitToStore(makeEpoch(kT0, makeSky(kThin, 100.0, 0.7)), s_on);
    e_off.emitToStore(makeEpoch(kT0, makeSky(kThin, 100.0, 0.7)), s_off);

    // A pure common-mode move again — which a 2-satellite "median" cannot tell from a
    // real one, so the armed test must NOT pretend it can: it falls back to raw.
    e_on.emitToStore(makeEpoch(t1, makeSky(kThin, 100.0 + kShift, 0.7)), s_on);
    e_off.emitToStore(makeEpoch(t1, makeSky(kThin, 100.0 + kShift, 0.7)), s_off);

    for (int i = 0; i < kThin; ++i) {
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", i + 1);
        EXPECT_EQ(e_on.discontinuityCounter(id, "L1W"),
                  e_off.discontinuityCounter(id, "L1W"))
            << "thin signal " << id << ": armed must reproduce raw cell for cell";
    }
    EXPECT_EQ(totalCounters(e_on, kThin), kThin);   // = the raw result

    // ...and it SAYS so, on every cell: a silent fallback would read as a working
    // de-common (invariant #4 — the counter is the telemetry, not the absence of a
    // crash).
    const auto& ev = e_on.pbJumpDecommonEvents();
    ASSERT_EQ(ev.size(), static_cast<std::size_t>(kThin));
    for (const auto& e : ev) {
        EXPECT_EQ(e.cls, NetworkProductEmitter::PbJumpDecommonClass::FallbackRaw);
        EXPECT_EQ(e.population, kThin);
        EXPECT_TRUE(e.declared);
        EXPECT_NEAR(e.decommon_m, e.raw_delta_m, 1e-12);   // raw stands, undivided
        EXPECT_NEAR(e.common_m, 0.0, 1e-12);
    }
}

// ---------------------------------------------------------------------------
// (4) OFF is byte-identical to a pre-JT emitter.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, OffPathMatchesPreCardEmitterExactly) {
    constexpr int kN = 6;
    SsrCorrectionStore s_new, s_old;
    auto e_new = makeEmitter(false);
    // The pre-JT construction: the six-parameter overload, no seventh argument.
    NetworkProductEmitter e_old(NetworkProductEmitter::kDefaultDiscThresholdM, false, 0.0,
                                false, false, 0.25);

    GnssTime t = kT0;
    for (int k = 0; k < 4; ++k) {
        auto sky = makeSky(kN, 100.0 - 0.9 * k, 0.7);
        if (k == 2) sky[1].second["L1W"] += 3.0;          // one real jump
        e_new.emitToStore(makeEpoch(t, sky), s_new);
        e_old.emitToStore(makeEpoch(t, sky), s_old);
        t.tow += 30.0;
    }
    for (int i = 0; i < kN; ++i) {
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", i + 1);
        EXPECT_EQ(e_new.discontinuityCounter(id, "L1W"),
                  e_old.discontinuityCounter(id, "L1W")) << id;
    }
    EXPECT_TRUE(e_new.pbJumpDecommonEvents().empty());
    EXPECT_FALSE(e_new.discJumpTestDecommon());

    std::ostringstream a, b;
    e_new.serializeState(a);
    e_old.serializeState(b);
    EXPECT_EQ(a.str(), b.str()) << "the knob adds no serialized emitter state";
}

// ---------------------------------------------------------------------------
// (5) The median is per SIGNAL, never across a mixed L1/L2 population.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, CommonModeIsTakenWithinEachSignal) {
    constexpr int    kN  = 9;
    constexpr double kC1 = -1.451;   // the L1W frame
    constexpr double kC2 = +0.900;   // an INDEPENDENT L2W frame (band-differential)

    SsrCorrectionStore store;
    auto e = makeEmitter(true);

    std::vector<std::pair<std::string, std::map<std::string, double>>> sky0, sky1;
    for (int i = 0; i < kN; ++i) {
        char id[8];
        std::snprintf(id, sizeof(id), "G%02d", i + 1);
        sky0.push_back({id, {{"L1W", 100.0 + i * 0.7}, {"L2W", 200.0 - i * 0.4}}});
        sky1.push_back({id, {{"L1W", 100.0 + i * 0.7 + kC1},
                             {"L2W", 200.0 - i * 0.4 + kC2}}});
    }
    const GnssTime t1{kT0.week, kT0.tow + 30.0};
    e.emitToStore(makeEpoch(kT0, sky0), store);
    e.emitToStore(makeEpoch(t1, sky1), store);

    // Both frames are absorbed by their OWN signal's median; a single mixed median
    // (here ≈ -0.28 m) would leave ±1.17 m on every cell and declare all 18.
    EXPECT_EQ(totalCounters(e, kN, "L1W"), 0);
    EXPECT_EQ(totalCounters(e, kN, "L2W"), 0);
    const auto& ev = e.pbJumpDecommonEvents();
    ASSERT_EQ(ev.size(), static_cast<std::size_t>(2 * kN));
    for (const auto& x : ev) {
        EXPECT_EQ(x.cls, NetworkProductEmitter::PbJumpDecommonClass::RawOnlySuppressed);
        EXPECT_NEAR(x.common_m, (x.signal == "L1W") ? kC1 : kC2, 1e-9);
        EXPECT_NEAR(x.threshold_m, (x.signal == "L1W") ? kBarL1 : kBarL2, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// (6) The S contract carries the knob, and an OFF contract is byte-identical.
// ---------------------------------------------------------------------------
TEST(PbJumpDecommonTest, SLineDeclaresTheKnobAndOffIsByteIdentical) {
    const fs::path with_p    = fs::temp_directory_path() / "jt_discdecom_with.txt";
    const fs::path without_p = fs::temp_directory_path() / "jt_discdecom_without.txt";
    std::error_code ec; fs::remove(with_p, ec); fs::remove(without_p, ec);

    SbasisContract base;
    base.decl.reference_receiver_id = "MBAR00UGA";
    base.disc_threshold_m           = 0.05;
    base.present                    = true;
    SbasisContract armed = base;
    armed.disc_jump_test_decommon = true;

    { NetworkProductWriter w(without_p.string(), base);  ASSERT_TRUE(w.good()); }
    { NetworkProductWriter w(with_p.string(),    armed); ASSERT_TRUE(w.good()); }

    // Compare the S LINES, not the whole files: the '#' legend is grammar-complete by
    // contract, so it names discdecom= in BOTH files by design.
    const auto sLine = [](const fs::path& p) {
        std::ifstream f(p);
        std::string ln;
        while (std::getline(f, ln))
            if (ln.rfind("S,", 0) == 0) return ln;
        return std::string{};
    };
    const std::string a = sLine(without_p), b = sLine(with_p);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_EQ(a.find("discdecom="), std::string::npos)
        << "an OFF producer writes the pre-JT S line byte-for-byte";
    EXPECT_EQ(b, a + ",discdecom=1");

    // A replay-emitter PARAMETER, not an interpretation hint: the round-trip must
    // reach makeReplayEmitter, or a file replay mis-counts exactly the cells a frame
    // move hid (the whole point of the card).
    bool ok = false;
    SbasisContract parsed_on, parsed_off;
    NetworkProductReader::read(with_p.string(),    &parsed_on,  &ok);  ASSERT_TRUE(ok);
    NetworkProductReader::read(without_p.string(), &parsed_off, &ok);  ASSERT_TRUE(ok);
    EXPECT_TRUE(parsed_on.disc_jump_test_decommon);
    EXPECT_FALSE(parsed_off.disc_jump_test_decommon);
    EXPECT_TRUE(NetworkProductReader::makeReplayEmitter(parsed_on).discJumpTestDecommon());
    EXPECT_FALSE(NetworkProductReader::makeReplayEmitter(parsed_off).discJumpTestDecommon());
    fs::remove(with_p, ec); fs::remove(without_p, ec);
}

} // namespace GnssProcessor
