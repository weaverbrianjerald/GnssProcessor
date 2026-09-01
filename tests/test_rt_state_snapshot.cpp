/**
 * @file test_rt_state_snapshot.cpp
 * @brief Unit gates for the RT filter-state snapshot (the consumer-side warm
 *        start): RtStateSnapshotIO round-trip + strict-parse rejections, and
 *        the StateManager::restoreDynamicState exact-layout replay primitive.
 *
 * The engine-level halves (stage / age + fingerprint gates / apply / cadence
 * write) are validated end-to-end on the sim closed-loop ladder — the
 * SPEC_synthetic_minimum_network §17 record — not here: they need a full
 * engine + product stream.  What this file locks is the FORMAT contract (a
 * corrupt or truncated file can never half-restore) and the layout-replay
 * contract (a restored index landing anywhere but its serialized slot is a
 * hard refusal, never a silent shift).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "realtime/RtStateSnapshot.hpp"

#include <sstream>
#include <string>

using namespace GnssProcessor;

namespace {

/// A small, fully-populated synthetic snapshot: 4 core + 3 dynamic states.
RtStateSnapshot makeSnap() {
    RtStateSnapshot s;
    s.snap_week  = 2313;
    s.snap_tow   = 345570.0;
    s.wall_utc   = "2026-09-01T20:00:00Z";
    s.fingerprint = 0x0123456789abcdefULL;
    s.core_size  = 4;
    s.state_size = 7;
    s.generation_id = 1399204800u;
    s.epoch_count   = 720;
    s.dyn = { { 4, "ION", "G01" },
              { 5, "AMB", "G01_L1C" },
              { 6, "RXCB", "DCB_G_C2W" } };
    s.x = Eigen::VectorXd::Zero(7);
    for (int i = 0; i < 7; ++i) s.x(i) = 0.5 * i - 1.25;
    s.P = Eigen::MatrixXd::Zero(7, 7);
    for (int i = 0; i < 7; ++i)
        for (int j = 0; j <= i; ++j) {
            const double v = (i == j) ? 1.0 + 0.1 * i : 0.01 * (i + j);
            s.P(i, j) = v;
            s.P(j, i) = v;
        }
    s.ssr_disc = { { "G01|L1C", 3 }, { "E11|L5Q", 0 } };
    s.phase_bias_applied = { { "G01|L1C", true }, { "E11|L5Q", false } };
    s.code_bias_applied  = { { "CB|G01|C1C", true } };
    return s;
}

std::string serialized(const RtStateSnapshot& s) {
    std::ostringstream os;
    RtStateSnapshotIO::serialize(os, s);
    return os.str();
}

bool parses(const std::string& text, RtStateSnapshot& out, std::string* err = nullptr) {
    std::istringstream is(text);
    return RtStateSnapshotIO::parse(is, out, err);
}

}  // namespace

TEST(RtStateSnapshotTest, RoundTripIsExact) {
    const RtStateSnapshot s = makeSnap();
    RtStateSnapshot r;
    std::string err;
    ASSERT_TRUE(parses(serialized(s), r, &err)) << err;
    EXPECT_EQ(r.snap_week, s.snap_week);
    EXPECT_DOUBLE_EQ(r.snap_tow, s.snap_tow);
    EXPECT_EQ(r.fingerprint, s.fingerprint);
    EXPECT_EQ(r.core_size, s.core_size);
    EXPECT_EQ(r.state_size, s.state_size);
    EXPECT_EQ(r.generation_id, s.generation_id);
    EXPECT_EQ(r.epoch_count, s.epoch_count);
    ASSERT_EQ(r.dyn.size(), s.dyn.size());
    for (std::size_t k = 0; k < s.dyn.size(); ++k) {
        EXPECT_EQ(r.dyn[k].idx, s.dyn[k].idx);
        EXPECT_EQ(r.dyn[k].kind, s.dyn[k].kind);
        EXPECT_EQ(r.dyn[k].key, s.dyn[k].key);
    }
    // "{:.17g}" round-trips doubles exactly — bitwise state/covariance equality.
    ASSERT_EQ(r.x.size(), s.x.size());
    for (Eigen::Index i = 0; i < s.x.size(); ++i) EXPECT_EQ(r.x(i), s.x(i));
    ASSERT_EQ(r.P.rows(), s.P.rows());
    for (Eigen::Index i = 0; i < s.P.rows(); ++i)
        for (Eigen::Index j = 0; j < s.P.cols(); ++j) EXPECT_EQ(r.P(i, j), s.P(i, j));
    EXPECT_EQ(r.ssr_disc, s.ssr_disc);
    EXPECT_EQ(r.phase_bias_applied, s.phase_bias_applied);
    EXPECT_EQ(r.code_bias_applied, s.code_bias_applied);
}

TEST(RtStateSnapshotTest, TruncationAndCorruptionAreRejected) {
    const std::string good = serialized(makeSnap());
    RtStateSnapshot r;

    // Missing END (simulates a torn write the atomic replace exists to prevent).
    {
        const std::string t = good.substr(0, good.rfind("END,"));
        EXPECT_FALSE(parses(t, r));
    }
    // END count mismatch.
    {
        std::string t = good;
        const auto pos = t.rfind("END,");
        t = t.substr(0, pos) + "END,9999\n";
        EXPECT_FALSE(parses(t, r));
    }
    // A dropped X row breaks the per-section count check even though END is
    // regenerated to match (content checks are independent of the line count).
    {
        std::istringstream is(good);
        std::string line, rebuilt;
        bool dropped = false;
        int n = 0;
        std::vector<std::string> body;
        std::getline(is, line);  // header
        rebuilt = line + "\n";
        while (std::getline(is, line)) {
            if (line.rfind("END,", 0) == 0) break;
            if (!dropped && line.rfind("X,3,", 0) == 0) { dropped = true; continue; }
            body.push_back(line);
            ++n;
        }
        for (const auto& b : body) rebuilt += b + "\n";
        rebuilt += "END," + std::to_string(n) + "\n";
        ASSERT_TRUE(dropped);
        EXPECT_FALSE(parses(rebuilt, r));
    }
    // Out-of-order K index (the gapless ascending replay contract).
    {
        std::string t = good;
        const auto pos = t.find("K,5,");
        ASSERT_NE(pos, std::string::npos);
        t.replace(pos, 4, "K,6,");
        EXPECT_FALSE(parses(t, r));
    }
    // Unknown record tag.
    {
        std::string t = good;
        t.insert(t.find("G,"), "Q,junk\n");
        EXPECT_FALSE(parses(t, r));
    }
    // Wrong P-row arity.
    {
        std::string t = good;
        const auto pos = t.find("P,2,");
        ASSERT_NE(pos, std::string::npos);
        const auto eol = t.find('\n', pos);
        t.replace(pos, eol - pos, "P,2,1.0,2.0");  // needs 3 values for row 2
        EXPECT_FALSE(parses(t, r));
    }
    // Sanity: the unmodified text still parses after all that surgery above.
    EXPECT_TRUE(parses(good, r));
}

TEST(RtStateSnapshotTest, RestoreDynamicStateReplaysExactLayout) {
    LogManager lm;
    GnssConfig cfg;
    // The ctor's size parameter is a base hint: the manager derives its actual
    // core block from the config (base + clock + ISB offsets + ...), so read
    // the realized core back — exactly what the engine's write-side harvest
    // does (core = state size − Σ dynamic map sizes).
    StateManager sm{ 5, lm, cfg };
    const std::size_t core = sm.getStateSize();
    ASSERT_GE(core, 5u);

    using K = StateManager::RestoredStateKind;
    // Interleaved kinds — the layout a semantic add* replay could NOT rebuild
    // (addIonosphereState pairs a rate row when enableIonoRate, elevation-
    // scales seeds, and short-circuits duplicates).
    ASSERT_TRUE(sm.restoreDynamicState(K::Ionosphere, "G01", 3.25, core + 0));
    ASSERT_TRUE(sm.restoreDynamicState(K::Ambiguity, "G01_L1C", -7.5, core + 1));
    ASSERT_TRUE(sm.restoreDynamicState(K::Ionosphere, "E11", 1.5, core + 2));
    ASSERT_TRUE(sm.restoreDynamicState(K::ReceiverCodeBias, "DCB_G_C2W", 0.75, core + 3));
    EXPECT_EQ(sm.getStateSize(), core + 4);
    EXPECT_EQ(sm.getIonosphereStates().at("G01"), core + 0);
    EXPECT_EQ(sm.getAmbiguities().at("G01_L1C"), core + 1);
    EXPECT_EQ(sm.getIonosphereStates().at("E11"), core + 2);
    EXPECT_EQ(sm.getCodeBiasStates().at("DCB_G_C2W"), core + 3);
    EXPECT_EQ(sm.getStateVector()(static_cast<Eigen::Index>(core + 1)), -7.5);

    // Both refusal paths are MUTATION-FREE: getStateSize() is map-derived, so
    // an appended-but-unregistered row would desynchronize the vector from the
    // reported size — the primitive pre-checks the landing index and the key.
    const std::size_t n_before = sm.getStateSize();
    EXPECT_FALSE(sm.restoreDynamicState(K::Ambiguity, "E11_L1C", 0.0, 42));      // wrong index
    EXPECT_FALSE(sm.restoreDynamicState(K::Ionosphere, "G01", 9.9, n_before));   // duplicate key
    EXPECT_EQ(sm.getStateSize(), n_before);
    EXPECT_EQ(sm.getStateVector().size(), static_cast<Eigen::Index>(n_before));

    // Wholesale x/P replace at the rebuilt size — the resume's final step.
    const std::size_t n = sm.getStateSize();
    Eigen::VectorXd x = Eigen::VectorXd::Constant(static_cast<Eigen::Index>(n), 2.5);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(static_cast<Eigen::Index>(n),
                                                  static_cast<Eigen::Index>(n)) * 0.04;
    sm.setStateVector(x);
    sm.setCovariance(P);
    EXPECT_EQ(sm.getStateVector()(static_cast<Eigen::Index>(core)), 2.5);
    EXPECT_EQ(sm.getCovariance()(static_cast<Eigen::Index>(core + 1),
                                 static_cast<Eigen::Index>(core + 1)), 0.04);

    // The SSR disc-counter memory round-trips through the new accessors.
    sm.setSsrDiscontinuityMemory({ { "G01|L1C", 2 } });
    EXPECT_EQ(sm.getSsrDiscontinuityMemory().at("G01|L1C"), 2);
    EXPECT_TRUE(sm.noteSsrDiscontinuity("G01|L1C", 3));   // restored baseline → bump fires
    EXPECT_FALSE(sm.noteSsrDiscontinuity("E11|L5Q", 1));  // first sighting → record only
}
