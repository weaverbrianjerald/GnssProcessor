/**
 * @file test_network_layout_flags.cpp
 * @brief The single-receiver layout flags in a network run, and the network's own troposphere
 *        gradient columns with their zero-mean prior.
 *
 * A network station's layout is its own parameter set.  The single-receiver flags
 * (estimation.estimate_velocity, estimate_phase_clock, estimate_tropo_gradients, imu.enabled,
 * state_ambiguity_basis nl_wl, between_satellite_sd.enabled) set only the consumer's layout,
 * because the consumer is configured by a copy of the network configuration.  With no consumer
 * a flag reaches no parameter, so the loader refuses it and names the network key where one
 * exists.  network.estimate_tropo_gradients gives every station two gradient columns, which the
 * shared observation equation fills, and network.tropo_gradient_prior_sigma_m one zero-mean
 * prior row per column and epoch.
 */

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/Constants.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/assembly/LocalBirth.hpp"
#include "network/assembly/SharedAdmission.hpp"
#include "network/assembly/StationAnchors.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/RunContext.hpp"   // the snapshot fingerprint
#include "network/state/StationParams.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/StationBirthFixture.hpp"
#include "../support/TestTempDir.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;
using gnss_test::BirthFixture;
using gnss_test::BirthOptions;

namespace {

/// A closed-loop consumer that the loader accepts (name, RINEX path and coordinate); the run
/// never reads the file.
const std::string kConsumer =
    R"(, "closed_loop": {"enable": true, "station": {"name": "CCCC", "rinex_path": "consumer.rnx",
                                                    "position_ecef": [0.0, 0.0, 6356752.3]}})";

/// A two-station network configuration with extra members of the estimation, imu and network
/// objects.
std::string networkConfig(const std::string& estimation_extra, const std::string& network_extra,
                          const std::string& top_extra = "") {
    return std::string(R"({
        "processing": {"mode": "ppp-rtk-network", "active_systems": ["GPS", "Galileo"]},
        "estimation": {"estimate_ifb": true)") + estimation_extra + R"(},
        "network": {"clock_datum_station": "AAAA",
                    "stations": [{"name": "AAAA", "position_ecef": [6378137.0, 0.0, 0.0]},
                                 {"name": "BBBB", "position_ecef": [0.0, 6378137.0, 0.0]}])"
        + network_extra + "}" + top_extra + "\n}";
}

/// What a load returned and what the loader's two loggers printed.
struct Loaded {
    bool        ok{false};
    std::string text;
    GnssConfig  cfg;
};

/// Loads @p text with the "gnss_config" and default loggers captured.  @p tag must be unique.
Loaded loadCapturing(const std::string& tag, const std::string& text) {
    Loaded out;
    const fs::path p = gnss_test::tempDir() / (tag + ".json");
    { std::ofstream f(p); f << text; }
    const fs::path dir = gnss_test::tempDir() / (tag + "_log");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    LogManager log;
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto cfg_log = log.getLogger("gnss_config");
    auto main_log = log.getLogger();
    cfg_log->sinks().push_back(sink);
    main_log->sinks().push_back(sink);
    out.ok = out.cfg.loadFromJson(p, log);
    cfg_log->flush();
    main_log->flush();
    out.text = oss.str();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    fs::remove(p, ec);
    return out;
}

/// One single-receiver layout flag: how a configuration sets it and the message's network key.
struct Flag {
    const char* name;
    std::string estimation_extra;
    std::string top_extra;
    const char* network_key;   ///< Named in the refusal; null when the network has none.
};

std::vector<Flag> layoutFlags() {
    return {
        {"velocity", R"(, "estimate_velocity": true)", "", nullptr},
        {"phase_clock", R"(, "estimate_phase_clock": true)", "", nullptr},
        {"gradients", R"(, "estimate_tropo_gradients": true)", "", "network.estimate_tropo_gradients"},
        {"imu", "", R"(, "imu": {"enabled": true})", nullptr},
        {"nl_wl", R"(, "state_ambiguity_basis": "nl_wl")", "", nullptr},
        {"bssd", R"(, "between_satellite_sd": {"enabled": true})", "", nullptr},
    };
}

}  // namespace

TEST(NetworkLayoutFlagsTest, AFlagWithNoConsumerIsRefusedNamingTheNetworkKey) {
    for (const Flag& f : layoutFlags()) {
        const Loaded l = loadCapturing(std::string("refused_") + f.name,
                                       networkConfig(f.estimation_extra, "", f.top_extra));
        EXPECT_FALSE(l.ok) << f.name;
        EXPECT_NE(l.text.find("reaches no parameter"), std::string::npos) << f.name << "\n" << l.text;
        if (f.network_key != nullptr)
            EXPECT_NE(l.text.find(f.network_key), std::string::npos) << f.name << "\n" << l.text;
    }
}

TEST(NetworkLayoutFlagsTest, WithAConsumerAFlagSetsTheConsumersLayoutAndLoads) {
    for (const Flag& f : layoutFlags()) {
        const Loaded l = loadCapturing(std::string("consumer_") + f.name,
                                       networkConfig(f.estimation_extra, kConsumer, f.top_extra));
        EXPECT_TRUE(l.ok) << f.name << "\n" << l.text;
        EXPECT_TRUE(l.cfg.getNetworkConfig().closed_loop.enable) << f.name;
    }
}

TEST(NetworkLayoutFlagsTest, WithoutAFlagANetworkRunLoadsWithOrWithoutAConsumer) {
    EXPECT_TRUE(loadCapturing("plain", networkConfig("", "")).ok);
    EXPECT_TRUE(loadCapturing("plain_consumer", networkConfig("", kConsumer)).ok);
}

TEST(NetworkLayoutFlagsTest, TheNetworkGradientKeyLoadsAloneAndIsNotTheSingleReceiverFlag) {
    const Loaded l = loadCapturing("network_key",
                                   networkConfig("", R"(, "estimate_tropo_gradients": true)"));
    ASSERT_TRUE(l.ok) << l.text;
    EXPECT_TRUE(l.cfg.getNetworkConfig().estimate_tropo_gradients);
    EXPECT_FALSE(l.cfg.estimateTropoGradients());
    // A top-level "estimate_tropo_gradients" is the single-receiver flag's legacy spelling,
    // never the network key's.
    const Loaded top = loadCapturing("top_level",
                                     networkConfig("", kConsumer, R"(, "estimate_tropo_gradients": true)"));
    ASSERT_TRUE(top.ok) << top.text;
    EXPECT_FALSE(top.cfg.getNetworkConfig().estimate_tropo_gradients);
    EXPECT_TRUE(top.cfg.estimateTropoGradients());
}

TEST(NetworkLayoutFlagsTest, TheGradientKeyGivesEveryStationTwoColumnsAfterTheWetDelay) {
    const BirthFixture plain;
    EXPECT_EQ(plain.set.gradient[0], -1);
    EXPECT_EQ(plain.set.gradient[1], -1);

    BirthOptions o;
    o.network_extra = R"(, "estimate_tropo_gradients": true)";
    const BirthFixture f(o);
    ASSERT_TRUE(f.stack.config.getNetworkConfig().estimate_tropo_gradients);
    EXPECT_EQ(f.set.gradient[0], f.set.zwd + 1);
    EXPECT_EQ(f.set.gradient[1], f.set.zwd + 2);
    EXPECT_EQ(f.set.slots.size(), plain.set.slots.size() + 2);
    EXPECT_EQ(slotAt(f.set, f.set.gradient[0]).ref, gradientRef(true));
    EXPECT_EQ(slotAt(f.set, f.set.gradient[1]).ref, gradientRef(false));
    EXPECT_EQ(findParam(f.set, gradientRef(true)), f.set.gradient[0]);
    EXPECT_EQ(findParam(f.set, gradientRef(false)), f.set.gradient[1]);
    EXPECT_EQ(stateLabel(slotAt(f.set, f.set.gradient[0]), 0), "TGRAD_N");
    EXPECT_EQ(stateLabel(slotAt(f.set, f.set.gradient[1]), 0), "TGRAD_E");
}

TEST(NetworkLayoutFlagsTest, EveryRowCarriesTheGradientPartialsOfTheSharedEquation) {
    BirthOptions o;
    o.network_extra = R"(, "estimate_tropo_gradients": true)";
    BirthFixture f(o);
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    networkBirthLocalStates(c, f.set, obs);
    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);
    const StationDataRows rows = networkStationRows(c, f.set, obs);
    ASSERT_GT(rows.rows(), 0);
    for (Eigen::Index r = 0; r < rows.rows(); ++r) {
        const std::string& sat = rows.satellite_ids[static_cast<std::size_t>(r)];
        const PppObservation* o_r = nullptr;
        for (const PppObservation& x : obs)
            if (x.satellite == sat) o_r = &x;
        ASSERT_NE(o_r, nullptr) << sat;
        // mf_grad = 1 / (sin(e) tan(e) + 0.0032), times cos(az) and sin(az).
        const double el = o_r->elevation * Constants::PI / 180.0;
        const double az = o_r->azimuth * Constants::PI / 180.0;
        const double mf = 1.0 / (std::sin(el) * std::tan(el) + 0.0032);
        EXPECT_NEAR(rowPartial(rows, r, f.set.gradient[0]), mf * std::cos(az), 1e-12) << sat;
        EXPECT_NEAR(rowPartial(rows, r, f.set.gradient[1]), mf * std::sin(az), 1e-12) << sat;
    }
}

TEST(NetworkLayoutFlagsTest, TheGradientColumnsSurviveAPruneAndKeepTheirSlots) {
    BirthOptions o;
    o.network_extra = R"(, "estimate_tropo_gradients": true)";
    BirthFixture f(o);
    const StationRowContext c = f.context();
    networkBirthLocalStates(c, f.set, f.epoch(0));
    ASSERT_GT(f.set.amb.size(), 0u);
    const int gn = f.set.gradient[0], ge = f.set.gradient[1];
    // Removing every ambiguity (slots born after the gradients) leaves the gradients in place.
    std::vector<int> amb_slots;
    for (const auto& kv : f.set.amb) amb_slots.push_back(kv.second);
    removeSlots(f.set, amb_slots);
    EXPECT_EQ(f.set.gradient[0], gn);
    EXPECT_EQ(f.set.gradient[1], ge);
    // Asking to remove a gradient slot removes nothing: it is a core column.
    const std::size_t n = f.set.slots.size();
    removeSlots(f.set, {gn, ge});
    EXPECT_EQ(f.set.slots.size(), n);
    EXPECT_EQ(findParam(f.set, gradientRef(true)), gn);
}

TEST(NetworkLayoutFlagsTest, TheGradientPriorDefaultsToOneMillimetreAndARefusedValueRestoresIt) {
    const Loaded def = loadCapturing("prior_default",
                                     networkConfig("", R"(, "estimate_tropo_gradients": true)"));
    ASSERT_TRUE(def.ok) << def.text;
    EXPECT_DOUBLE_EQ(def.cfg.getNetworkConfig().tropo_gradient_prior_sigma_m, 0.001);
    const Loaded zero = loadCapturing(
        "prior_zero", networkConfig("", R"(, "estimate_tropo_gradients": true,
                                         "tropo_gradient_prior_sigma_m": 0.0)"));
    ASSERT_TRUE(zero.ok) << zero.text;
    EXPECT_EQ(zero.cfg.getNetworkConfig().tropo_gradient_prior_sigma_m, 0.0);
    // A negative sigma names no bound: refused, and the physical default restored.
    const Loaded bad = loadCapturing(
        "prior_negative", networkConfig("", R"(, "estimate_tropo_gradients": true,
                                             "tropo_gradient_prior_sigma_m": -0.002)"));
    ASSERT_TRUE(bad.ok) << bad.text;
    EXPECT_DOUBLE_EQ(bad.cfg.getNetworkConfig().tropo_gradient_prior_sigma_m, 0.001);
    EXPECT_NE(bad.text.find("tropo_gradient_prior_sigma_m"), std::string::npos) << bad.text;
}

namespace {

/// One station over a gradient-armed configuration, and its build at the first epoch with a
/// slot that holds one data row and the two gradient columns at local columns 0 and 1.
struct GradientPriorFixture {
    BirthFixture                         birth;
    NetworkTestRun                       run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    StationAssembly                      slot;

    explicit GradientPriorFixture(const std::string& network_extra)
        : birth(options(network_extra)), run(birth.stack.config) {
        run.setup().stations.resize(1);
        station().name   = "AAAA";
        station().params = birth.set;
        run.newEpoch(GnssTime{2313, 259230.0});
        slot.h2local = {{birth.set.gradient[0], 0}, {birth.set.gradient[1], 1}};
        slot.A_s = Eigen::MatrixXd::Zero(1, 3);
        slot.A_r = Eigen::MatrixXd::Zero(1, 2);
        slot.w = Eigen::VectorXd::Ones(1);
        slot.y = Eigen::VectorXd::Zero(1);
        slot.row_tags = {RowTag{ContributionKind::DataCode, {}, {}}};
    }

    static BirthOptions options(const std::string& network_extra) {
        BirthOptions o;
        o.network_extra = network_extra;
        return o;
    }

    NetStation& station() { return run.setup().stations[0]; }

    StationBuild build() {
        NetworkEpoch& e = run.ep();
        return StationBuild{run.ctx(), run.config(), run.est(), run.setup(), e.se, e.clock,
                            e.pass_a, e.events, gim, 0, station(), slot};
    }
};

}  // namespace

TEST(NetworkLayoutFlagsTest, EachGradientColumnTakesAZeroMeanPriorRow) {
    GradientPriorFixture f(R"(, "estimate_tropo_gradients": true,
                              "tropo_gradient_prior_sigma_m": 0.002)");
    // A linearization value away from zero: the row aims at absolute zero, so its target is
    // minus that value.
    f.station().params.slots[static_cast<std::size_t>(f.birth.set.gradient[1])].x_lin = 0.0004;
    StationBuild b = f.build();
    EXPECT_EQ(networkTropoGradientPriorRows(b), StationStep::Next);
    ASSERT_EQ(f.slot.A_r.rows(), 3);
    ASSERT_EQ(f.slot.A_s.rows(), 3);
    ASSERT_EQ(f.slot.row_tags.size(), 3u);
    for (Eigen::Index r = 1; r < 3; ++r) {
        EXPECT_EQ(f.slot.row_tags[static_cast<std::size_t>(r)].kind,
                  ContributionKind::TropoGradientPrior);
        EXPECT_DOUBLE_EQ(f.slot.w(r), 1.0 / (0.002 * 0.002));
        EXPECT_EQ(f.slot.A_s.row(r).squaredNorm(), 0.0);
    }
    EXPECT_EQ(f.slot.A_r(1, 0), 1.0);   // north
    EXPECT_EQ(f.slot.A_r(1, 1), 0.0);
    EXPECT_EQ(f.slot.A_r(2, 0), 0.0);   // east
    EXPECT_EQ(f.slot.A_r(2, 1), 1.0);
    EXPECT_DOUBLE_EQ(f.slot.y(1), 0.0);
    EXPECT_DOUBLE_EQ(f.slot.y(2), -0.0004);
    // The data row is untouched.
    EXPECT_EQ(f.slot.row_tags[0].kind, ContributionKind::DataCode);
    EXPECT_EQ(f.slot.w(0), 1.0);
}

TEST(NetworkLayoutFlagsTest, NoPriorRowWithSigmaZeroOrForAColumnOutOfTheBlock) {
    {
        GradientPriorFixture f(R"(, "estimate_tropo_gradients": true,
                                  "tropo_gradient_prior_sigma_m": 0.0)");
        StationBuild b = f.build();
        EXPECT_EQ(networkTropoGradientPriorRows(b), StationStep::Next);
        EXPECT_EQ(f.slot.A_r.rows(), 1);
        EXPECT_EQ(f.slot.row_tags.size(), 1u);
    }
    {
        // The east column left out of this epoch's block: the north column alone gets a row.
        GradientPriorFixture f(R"(, "estimate_tropo_gradients": true)");
        f.slot.h2local.erase(f.birth.set.gradient[1]);
        StationBuild b = f.build();
        EXPECT_EQ(networkTropoGradientPriorRows(b), StationStep::Next);
        ASSERT_EQ(f.slot.A_r.rows(), 2);
        EXPECT_EQ(f.slot.A_r(1, 0), 1.0);
        EXPECT_DOUBLE_EQ(f.slot.w(1), 1.0 / (0.001 * 0.001));
    }
}

namespace {

/// The field golden with @p edit applied, loaded into @p cfg.  @p tag must be unique.
bool loadFieldGolden(const std::function<void(nlohmann::json&)>& edit, GnssConfig& cfg,
                     const std::string& tag) {
    const fs::path src =
        fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / "golden_field_refound.json";
    std::ifstream in(src);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, true, /*ignore_comments=*/true);
    edit(j);
    const fs::path p = gnss_test::tempDir() / ("golden_" + tag + ".json");
    { std::ofstream out(p); out << j.dump(2); }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The live snapshot fingerprint a run of @p cfg would compute.
std::uint64_t liveFingerprint(const GnssConfig& cfg) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    ctx.carry.snap_path = "state_snapshot.snap";   // the builder is a no-op without a path
    std::vector<NetStation> st(2);
    st[0].name = "AAAA"; st[0].is_pivot = true;
    st[1].name = "BBBB"; st[1].is_pivot = false;
    networkSnapshotFingerprint(ctx, cfg, st);
    return ctx.carry.live_fingerprint;
}

}  // namespace

TEST(NetworkLayoutFlagsTest, TheGradientPriorJoinsTheFingerprintOnlyWithTheGradients) {
    const auto grad = [](nlohmann::json& j) { j["network"]["estimate_tropo_gradients"] = true; };
    const auto sigma = [](double s) {
        return [s](nlohmann::json& j) { j["network"]["tropo_gradient_prior_sigma_m"] = s; };
    };
    GnssConfig off, off_sigma, on, on_sigma, on_zero;
    ASSERT_TRUE(loadFieldGolden([](nlohmann::json&) {}, off, "fp_off"));
    ASSERT_TRUE(loadFieldGolden(sigma(0.002), off_sigma, "fp_off_sigma"));
    ASSERT_TRUE(loadFieldGolden(grad, on, "fp_on"));
    ASSERT_TRUE(loadFieldGolden([&](nlohmann::json& j) { grad(j); sigma(0.002)(j); }, on_sigma,
                                "fp_on_sigma"));
    ASSERT_TRUE(loadFieldGolden([&](nlohmann::json& j) { grad(j); sigma(0.0)(j); }, on_zero,
                                "fp_on_zero"));
    // Without the gradients the prior is not read, so every existing fingerprint is unchanged.
    EXPECT_NE(liveFingerprint(off), 0u);
    EXPECT_EQ(liveFingerprint(off_sigma), liveFingerprint(off));
    // With them, the gradients and each prior sigma give their own fingerprint.
    EXPECT_NE(liveFingerprint(on), liveFingerprint(off));
    EXPECT_NE(liveFingerprint(on_sigma), liveFingerprint(on));
    EXPECT_NE(liveFingerprint(on_zero), liveFingerprint(on));
}
