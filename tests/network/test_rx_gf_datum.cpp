/**
 * @file test_rx_gf_datum.cpp
 * @brief The receiver geometry-free datum (network.rx_gf_datum): its loader, its row in each
 *        mode, the serial tally's edges and counters, the run-end line, and a rank check of the
 *        datum on a toy network in the engine's own receiver code-bias parameterization.
 *
 * Each (receiver, constellation) has one direction the data cannot see: its slant ionospheres
 * shifted by iota, the receiver DCB by -(1 - mu_2) iota, an extra-band IFB by -mu_k iota and its
 * phase biases by +mu_j iota.  network.rx_code_bias_datum holds it with a loose prior folded on
 * every epoch (prior_refold, the default); birth_prior folds that prior only where the DCB has no
 * carried row; hard holds the DCB with a row of the chain pins' weight.  The third-band datum
 * record (XBD) proves by exact rank that one DCB row per (receiver, constellation), at every
 * receiver, closes the direction as a pure datum.
 *
 * What is pinned here:
 *
 *  1. **Loader.**  prior_refold by default; birth_prior and hard parse; any other value refuses
 *     the configuration; the knob reads prior_refold without network.rx_code_bias_datum.
 *  2. **The row, mode by mode.**  prior_refold folds today's row and writes no record;
 *     birth_prior folds the prior on an uncarried DCB and nothing on a carried one (both carry
 *     routes); hard folds a 1e10 row at 0 on an uncarried DCB and at the carried value on a
 *     carried one.  Every row goes through the datum's own channel and is counted.
 *  3. **The tally.**  A first record is a placement at a birth; carried epochs place nothing; an
 *     uncarried epoch within the carry's horizon is a re-seed, its edge counted once; a station
 *     out of the solve releases; a column back after more than the horizon is a placement at a
 *     return.  Nothing is counted under prior_refold.
 *  4. **The run-end line** prints only when armed.
 *  5. **Rank.**  On a toy GPS triple-frequency network whose receiver code columns and
 *     coefficients come from the engine's own resolver (ReceiverBiasResolver), the chain-style
 *     hard rows leave one free direction per receiver; one DCB row per receiver closes them and is
 *     a pure datum (it constrains no estimable function); the analytic N5 direction is in the null
 *     space at a non-root receiver.
 */

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "measurement/ReceiverBias.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/RxCodeBiasCarry.hpp"
#include "network/carry/RxCodeBiasCarryRunEnd.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "network/state/StationParams.hpp"
#include "../support/ModelStack.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/StationParamsBuilder.hpp"
#include "../support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kT0     = 1.4e9;   // continuous GPS seconds of the first epoch
constexpr double kEpochS = 30.0;    // epoch spacing (s)

// ---------------------------------------------------------------------------
// Configurations
// ---------------------------------------------------------------------------

/// A two-station GPS network configuration with the receiver DCB datum armed and @p extra
/// network members (each ending with ",\n").
std::string configText(const std::string& extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"estimation\": { \"estimate_ifb\": true },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"rx_code_bias_datum\": true,\n"
          "    \"station_prior_before_pd\": true,\n"
          "    \"station_pd_pin_aware\": true,\n"
        + extra +
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

/// The receiver code-bias carry on the in-block route (the posture of the integer-fixing runs).
const char* const kBlockCarry =
    "    \"phase_leveling\": true,\n"
    "    \"carry_conditional_coupling\": true,\n"
    "    \"shared_carry_full_cov\": true,\n"
    "    \"phase_leveling_local_block\": true,\n"
    "    \"code_datum_time_linked\": true,\n"
    "    \"carry_rx_code_bias\": true,\n"
    "    \"carry_rx_code_bias_in_block\": true,\n";

/// The receiver code-bias carry on the diagonal route.
const char* const kDiagonalCarry =
    "    \"code_datum_time_linked\": true,\n"
    "    \"carry_rx_code_bias\": true,\n";

/// A loaded configuration (the file removed after the load).
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    bool       ok{false};
    Loaded(const std::string& tag, const std::string& extra) {
        const fs::path p = gnss_test::tempDir() / ("rxgf_cfg_" + tag + ".json");
        {
            std::ofstream f(p);
            f << configText(extra);
        }
        ok = cfg.loadFromJson(p, log);
        std::error_code ec;
        fs::remove(p, ec);
    }
};

std::string mode(const char* m) { return std::string("    \"rx_gf_datum\": \"") + m + "\",\n"; }

// ---------------------------------------------------------------------------
// The row bench: one station with one GPS DCB column through networkRxCbPriors
// ---------------------------------------------------------------------------

/// One prior row the step folded.
struct Folded {
    Eigen::Index col;
    double       lam;
    double       eta;
    bool         pre_pd;
};

/// What one call of networkRxCbPriors left.
struct RowResult {
    std::vector<Folded>          rows;      ///< The datum rows (pre-PD and post-PD).
    std::vector<RxGfDatumRecord> records;   ///< R.rx_gf.
    int                          counted{0};///< R.pl_cb_datum_rows.
};

class RowBench {
public:
    explicit RowBench(const GnssConfig& cfg) : fx_(std::make_unique<NetworkTestRun>(cfg)) {
        fx_->setup().stations.resize(1);
        NetStation& st = fx_->setup().stations[0];
        st.name = "AAAA";
        dcb_slot_ = gnss_test::bornRxCodeBias(st.params, gnss_test::dcbKey(System::GPS), kXlin);
    }

    static constexpr double kXlin = 0.7;   ///< The DCB's linearization point (m).

    const NetworkKnobs& knobs() const { return fx_->ctx().knobs; }
    bool slotted() const { return dcb_slot_ >= 0; }
    NetworkRunContext& ctx() { return fx_->ctx(); }

    /// Calls the step at time @p now_s; @p carried_block, when set, is pushed as the DCB's
    /// carried block column first (the in-block carry's decision, made by the earlier step).
    RowResult run(double now_s, const LocalCarry* carried_block = nullptr) {
        NetworkRunContext& ctx = fx_->ctx();
        NetworkEpoch& e = fx_->newEpoch(GnssTime{});
        e.clock = EpochClock{kEpochS, now_s, true};
        e.assembly.asm_res.resize(1);
        StationAssembly& R = e.assembly.asm_res[0];
        R.h2local[dcb_slot_] = 0;
        NetStation& st = fx_->setup().stations[0];
        const Eigen::VectorXd x_lin =
            Eigen::VectorXd::Constant(static_cast<Eigen::Index>(st.params.slots.size()), kXlin);
        StationBuild b{ctx, fx_->config(), fx_->est(), fx_->setup(), e.se, e.clock, e.pass_a,
                       e.events, gim_, 0, st, R};
        const GlobalVtecShConfig shc{};
        const net::SatSignalTable sat_signals{};
        StationCarryFrame carry{b, true, true, x_lin,
                                CouplingRowBuilder{fx_->est(), shc, 0.0, 0.0, 0.0, false, false,
                                                   false, sat_signals, R}};
        if (carried_block != nullptr)
            carry.rxcb_bc.push_back(LocalBlockCol{0, kXlin, carried_block, 1e-4, kEpochS,
                                                  /*vce_fac=*/1.0, false, false, false, false,
                                                  /*is_rx_cb=*/true});
        networkRxCbPriors(carry);
        RowResult out;
        for (const auto& [col, lam, eta, tag] : carry.inj_prior)
            out.rows.push_back(Folded{col, lam, eta, true});
        for (const auto& [col, lam, eta, tag] : carry.inj)
            if (tag.kind == ContributionKind::PriorRxCbDatum)
                out.rows.push_back(Folded{col, lam, eta, false});
        out.records = R.rx_gf;
        out.counted = R.pl_cb_datum_rows;
        return out;
    }

private:
    std::unique_ptr<NetworkTestRun>      fx_;
    std::unique_ptr<GlobalIonosphereMap> gim_;
    int                                  dcb_slot_{-1};
};

}  // namespace

// ===========================================================================
// 1. Loader
// ===========================================================================

TEST(RxGfDatumLoader, DefaultIsPriorRefold) {
    Loaded L("default", "");
    ASSERT_TRUE(L.ok);
    EXPECT_EQ(L.cfg.networkRxGfDatum(), NetworkRxGfDatum::PriorRefold);
}

TEST(RxGfDatumLoader, ParsesBothNewValues) {
    Loaded b("birth", mode("birth_prior"));
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(b.cfg.networkRxGfDatum(), NetworkRxGfDatum::BirthPrior);
    Loaded h("hard", mode("hard"));
    ASSERT_TRUE(h.ok);
    EXPECT_EQ(h.cfg.networkRxGfDatum(), NetworkRxGfDatum::Hard);
}

TEST(RxGfDatumLoader, RefusesAnUnknownValue) {
    Loaded L("unknown", mode("hard_row"));
    EXPECT_FALSE(L.ok);
}

TEST(RxGfDatumLoader, KnobIsPriorRefoldWithoutTheDatum) {
    Loaded L("nodatum", mode("hard") + "    \"rx_code_bias_datum\": false,\n");
    ASSERT_TRUE(L.ok);
    EXPECT_EQ(L.cfg.networkRxGfDatum(), NetworkRxGfDatum::Hard);   // parsed as written
    NetworkTestRun fx(L.cfg);
    EXPECT_EQ(fx.ctx().knobs.rx_gf_datum, NetworkRxGfDatum::PriorRefold);   // read only with the datum
}

// ===========================================================================
// 2. The row, mode by mode
// ===========================================================================

TEST(RxGfDatumRow, PriorRefoldFoldsTodaysRowAndWritesNoRecord) {
    Loaded L("row_refold", kBlockCarry);
    ASSERT_TRUE(L.ok);
    RowBench B(L.cfg);
    ASSERT_TRUE(B.slotted());
    ASSERT_EQ(B.knobs().rx_gf_datum, NetworkRxGfDatum::PriorRefold);
    const LocalCarry carried{};
    for (const LocalCarry* c : {static_cast<const LocalCarry*>(nullptr), &carried}) {
        const RowResult r = B.run(kT0, c);
        ASSERT_EQ(r.rows.size(), 1u) << "the prior is folded whether carried or not";
        EXPECT_TRUE(r.rows[0].pre_pd);
        EXPECT_DOUBLE_EQ(r.rows[0].lam, 1.0 / (10.0 * 10.0));
        EXPECT_DOUBLE_EQ(r.rows[0].eta, r.rows[0].lam * (0.0 - RowBench::kXlin));
        EXPECT_EQ(r.counted, 1);
        EXPECT_TRUE(r.records.empty());
    }
}

TEST(RxGfDatumRow, BirthPriorFoldsOnlyOnAnUncarriedColumn) {
    Loaded L("row_birth", std::string(kBlockCarry) + mode("birth_prior"));
    ASSERT_TRUE(L.ok);
    RowBench B(L.cfg);
    ASSERT_TRUE(B.slotted());
    ASSERT_EQ(B.knobs().rx_gf_datum, NetworkRxGfDatum::BirthPrior);
    ASSERT_TRUE(B.knobs().rxcb_blk_on) << "the in-block route must be live for this case";

    const RowResult cold = B.run(kT0);
    ASSERT_EQ(cold.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(cold.rows[0].lam, 1.0 / (10.0 * 10.0));
    EXPECT_DOUBLE_EQ(cold.rows[0].eta, cold.rows[0].lam * (0.0 - RowBench::kXlin));
    ASSERT_EQ(cold.records.size(), 1u);
    EXPECT_FALSE(cold.records[0].carried);
    EXPECT_TRUE(cold.records[0].prior);
    EXPECT_FALSE(cold.records[0].hard);
    EXPECT_EQ(cold.counted, 1);

    LocalCarry carried{};
    carried.value_abs = 0.25;
    const RowResult warm = B.run(kT0 + kEpochS, &carried);
    EXPECT_TRUE(warm.rows.empty()) << "a carried DCB keeps its posterior: no refold";
    ASSERT_EQ(warm.records.size(), 1u);
    EXPECT_TRUE(warm.records[0].carried);
    EXPECT_FALSE(warm.records[0].prior);
    EXPECT_EQ(warm.counted, 0);
}

TEST(RxGfDatumRow, HardHoldsAtZeroUncarriedAndAtTheCarriedValueCarried) {
    Loaded L("row_hard", std::string(kBlockCarry) + mode("hard"));
    ASSERT_TRUE(L.ok);
    RowBench B(L.cfg);
    ASSERT_TRUE(B.slotted());
    ASSERT_EQ(B.knobs().rx_gf_datum, NetworkRxGfDatum::Hard);
    ASSERT_TRUE(B.knobs().rxcb_blk_on);

    const RowResult cold = B.run(kT0);
    ASSERT_EQ(cold.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(cold.rows[0].lam, kRxGfHardWeight);
    EXPECT_DOUBLE_EQ(cold.rows[0].eta, kRxGfHardWeight * (0.0 - RowBench::kXlin));
    ASSERT_EQ(cold.records.size(), 1u);
    EXPECT_TRUE(cold.records[0].hard);
    EXPECT_DOUBLE_EQ(cold.records[0].target, 0.0);

    LocalCarry carried{};
    carried.value_abs = 0.25;
    const RowResult warm = B.run(kT0 + kEpochS, &carried);
    ASSERT_EQ(warm.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(warm.rows[0].lam, kRxGfHardWeight);
    EXPECT_DOUBLE_EQ(warm.rows[0].eta, kRxGfHardWeight * (0.25 - RowBench::kXlin));
    ASSERT_EQ(warm.records.size(), 1u);
    EXPECT_TRUE(warm.records[0].carried);
    EXPECT_DOUBLE_EQ(warm.records[0].target, 0.25);
    EXPECT_EQ(warm.counted, 1);
}

TEST(RxGfDatumRow, DiagonalRouteReadsTheInjectGate) {
    Loaded L("row_diag", std::string(kDiagonalCarry) + mode("birth_prior"));
    ASSERT_TRUE(L.ok);
    RowBench B(L.cfg);
    ASSERT_TRUE(B.slotted());
    ASSERT_TRUE(B.knobs().rx_cb_on);
    ASSERT_FALSE(B.knobs().rxcb_blk_on);

    // A carried entry inside the horizon folds (no prior); past the horizon it does not (prior).
    LocalCarry entry{};
    entry.value_abs   = RowBench::kXlin;   // innovation 0: the innovation gate passes
    entry.var         = 1e-4;
    entry.last_seen_s = kT0;
    net::put(B.ctx().entities.stations, StationId{0}, &net::StationState::signals,
             std::string("GPS_DCB"), &net::StationSignalState::rx_cb) = entry;

    const RowResult inside = B.run(kT0 + kEpochS);
    ASSERT_EQ(inside.records.size(), 1u);
    EXPECT_TRUE(inside.records[0].carried);
    EXPECT_TRUE(inside.rows.empty());

    const RowResult past = B.run(kT0 + kRxCbMaxGapS + kEpochS);
    ASSERT_EQ(past.records.size(), 1u);
    EXPECT_FALSE(past.records[0].carried);
    EXPECT_EQ(past.rows.size(), 1u);
}

// ===========================================================================
// 3. The tally
// ===========================================================================

namespace {

/// Drives networkRxGfDatumTally with one station and one GPS DCB record per epoch.
class TallyBench {
public:
    explicit TallyBench(const GnssConfig& cfg) : fx_(std::make_unique<NetworkTestRun>(cfg)) {
        fx_->setup().stations.resize(1);
        fx_->setup().stations[0].name = "AAAA";
    }
    /// One epoch at @p now_s: @p present false leaves the station out of the solve.
    void epoch(double now_s, bool present, bool carried) {
        NetworkEpoch& e = fx_->newEpoch(GnssTime{});
        using LoopEp = decltype(fx_->ctx().counters.n_loop_epochs);
        fx_->ctx().counters.n_loop_epochs = static_cast<LoopEp>(++n_);
        std::vector<StationAssembly> asm_res(1);
        asm_res[0].ok = present;
        if (present) {
            RxGfDatumRecord r;
            r.sys = System::GPS;
            r.carried = carried;
            r.hard = true;
            r.target = carried ? 1e-11 : 0.0;
            r.now_s = now_s;
            asm_res[0].rx_gf.push_back(r);
        }
        networkRxGfDatumTally(fx_->ctx(), fx_->traces(), e.se, fx_->setup().stations, asm_res);
    }
    const NetworkServiceCounters& C() const { return fx_->ctx().counters; }
    const net::RxGfDatumMemo* memo() const {
        const auto it = fx_->ctx().entities.stations.find(StationId{0});
        if (it == fx_->ctx().entities.stations.end()) return nullptr;
        const auto m = it->second.rx_gf.find(System::GPS);
        return m == it->second.rx_gf.end() ? nullptr : &m->second;
    }

private:
    std::unique_ptr<NetworkTestRun> fx_;
    long long                       n_{0};
};

}  // namespace

TEST(RxGfDatumTally, EdgesAndCountersOverAStationsLife) {
    Loaded L("tally", mode("hard"));
    ASSERT_TRUE(L.ok);
    TallyBench T(L.cfg);
    double t = kT0;
    T.epoch(t, true, false);                    // birth
    EXPECT_EQ(T.C().rxgf_place_birth, 1);
    ASSERT_NE(T.memo(), nullptr);
    EXPECT_TRUE(T.memo()->active);
    T.epoch(t += kEpochS, true, true);          // carried
    T.epoch(t += kEpochS, true, true);          // carried
    EXPECT_EQ(T.C().rxgf_carried, 2);
    T.epoch(t += kEpochS, true, false);         // the carry erased within the horizon: a re-seed edge
    T.epoch(t += kEpochS, true, false);         // still uncarried: counted, no second edge
    EXPECT_EQ(T.C().rxgf_reseed_uncarried, 2);
    EXPECT_EQ(T.C().rxgf_reseed_edges, 1);
    T.epoch(t += kEpochS, false, false);        // out of the solve: the release edge
    EXPECT_EQ(T.C().rxgf_releases, 1);
    EXPECT_FALSE(T.memo()->active);
    T.epoch(t += kRxCbMaxGapS + kEpochS, true, false);   // back after more than the horizon: a return
    EXPECT_EQ(T.C().rxgf_place_return, 1);
    EXPECT_EQ(T.C().rxgf_place_birth, 1);
    EXPECT_EQ(T.C().rxgf_records, 6);
    EXPECT_EQ(T.C().rxgf_hard_rows, 6);
    EXPECT_LE(T.C().rxgf_max_abs_target, 1e-10);
}

TEST(RxGfDatumTally, NothingUnderPriorRefold) {
    Loaded L("tally_off", "");
    ASSERT_TRUE(L.ok);
    TallyBench T(L.cfg);
    T.epoch(kT0, true, false);
    EXPECT_EQ(T.C().rxgf_records, 0);
    EXPECT_EQ(T.C().rxgf_place_birth, 0);
    EXPECT_EQ(T.memo(), nullptr);
}

// ===========================================================================
// 4. The run-end line
// ===========================================================================

namespace {

std::string runEndText(const GnssConfig& cfg, long long records) {
    std::ostringstream text;
    auto log = std::make_shared<spdlog::logger>(
        "rxgf_runend", std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
    log->set_level(spdlog::level::info);
    NetworkTestRun fx(cfg, log);
    fx.ctx().counters.rxgf_records = records;
    NetworkRun& run = fx.run();
    const NetworkRunEndInputs inputs{run.engines, run.emit, run.timing, run.closed_loop};
    const NetworkRunEndView view{fx.ctx(), fx.config(), log, fx.traces(), fx.setup(), inputs, 0.0};
    EXPECT_EQ(networkRxCodeBiasCarryRunEnd(view), 0);
    log->flush();
    return text.str();
}

}  // namespace

TEST(RxGfDatumRunEnd, PrintsOnlyWhenArmed) {
    Loaded off("re_off", "");
    ASSERT_TRUE(off.ok);
    EXPECT_EQ(runEndText(off.cfg, 0).find("RX_GF_DATUM"), std::string::npos);
    Loaded on("re_on", mode("hard"));
    ASSERT_TRUE(on.ok);
    const std::string armed = runEndText(on.cfg, 5);
    EXPECT_NE(armed.find("Network (RX_GF_DATUM): hard"), std::string::npos) << armed;
    EXPECT_EQ(armed.find("is inert"), std::string::npos);
    EXPECT_NE(runEndText(on.cfg, 0).find("is inert"), std::string::npos);
}

// ===========================================================================
// 5. Rank, in the engine's own receiver code-bias parameterization
// ===========================================================================

namespace {

/// A toy GPS triple-frequency network: R receivers, S satellites, K epochs, every receiver
/// tracking C1C/C2W/C5Q and L1C/L2W/L5Q.  The receiver code columns and coefficients are the
/// engine resolver's (DCB on the clock-reference pair, IFB on band 5); the rest follows the
/// observation equation.  Columns are built on demand by name.
class ToyNetwork {
public:
    ToyNetwork(const ReceiverBiasResolver& rb, int R, int S, int K) : R_(R), S_(S), K_(K) {
        const ClockRefMap refs{{'G', ClockRefSignals{"C1C", "C2W"}}};
        const double f1 = nominalCarrierFrequencyHz(FrequencyBand::GpsL1);
        const std::map<std::string, FrequencyBand> band{
            {"1", FrequencyBand::GpsL1}, {"2", FrequencyBand::GpsL2}, {"5", FrequencyBand::GpsL5}};
        for (const std::string code : {"C1C", "C2W", "C5Q"}) {
            const ReceiverBiasEntry e = rb.resolveReceiverBias(code, System::GPS, "G01", refs);
            entry_[code] = e;
            const double f = nominalCarrierFrequencyHz(band.at(code.substr(1, 1)));
            mu_[code.substr(1, 1)] = (f1 * f1) / (f * f);
            lam_[code.substr(1, 1)] = 299792458.0 / f;
        }
        const char* codes[] = {"C1C", "C2W", "C5Q"};
        const char* phases[] = {"L1C", "L2W", "L5Q"};
        for (int k = 0; k < K_; ++k)
            for (int r = 0; r < R_; ++r)
                for (int s = 0; s < S_; ++s) {
                    for (const char* c : codes) {
                        Row row;
                        row[col("clk", r, k)] += 1.0;
                        row[col("satclk", s, k)] -= 1.0;
                        row[col("iono", r * 100 + s, k)] += mu_[std::string(1, c[1])];
                        const ReceiverBiasEntry& e = entry_[c];
                        row[col(e.state_key, r)] += e.coefficient;
                        row[col(std::string("satcb_") + c, s)] -= 1.0;
                        obs_.push_back(row);
                    }
                    for (const char* p : phases) {
                        const std::string d(1, p[1]);
                        Row row;
                        row[col("clk", r, k)] += 1.0;
                        row[col("satclk", s, k)] -= 1.0;
                        row[col("iono", r * 100 + s, k)] -= mu_[d];
                        row[col(std::string("amb_") + p, r * 100 + s)] += lam_[d];
                        row[col(std::string("rpb_") + p, r)] += 1.0;
                        row[col(std::string("satpb_") + p, s)] -= 1.0;
                        obs_.push_back(row);
                    }
                }
        // The engine's hard rows: a clock pin per epoch; per satellite the pair's
        // ionosphere-free and geometry-free pins; one level row for the extra signal C5Q; the
        // chain datum per phase signal (the root's phase bias, a spanning tree of ambiguities).
        const double m2 = mu_["2"];
        for (int k = 0; k < K_; ++k) hard_.push_back(Row{{col("satclk", 0, k), 1.0}});
        for (int s = 0; s < S_; ++s) {
            hard_.push_back(Row{{col("satcb_C1C", s), m2 / (m2 - 1.0)}, {col("satcb_C2W", s), -1.0 / (m2 - 1.0)}});
            hard_.push_back(Row{{col("satcb_C1C", s), -1.0 / (m2 - 1.0)}, {col("satcb_C2W", s), 1.0 / (m2 - 1.0)}});
        }
        Row level;
        for (int s = 0; s < S_; ++s) level[col("satcb_C5Q", s)] = 1.0;
        hard_.push_back(level);
        for (const char* p : phases) {
            hard_.push_back(Row{{col(std::string("rpb_") + p, 0), 1.0}});   // root receiver 0
            for (int s = 0; s < S_; ++s) hard_.push_back(Row{{col(std::string("amb_") + p, s), 1.0}});
            for (int r = 1; r < R_; ++r) hard_.push_back(Row{{col(std::string("amb_") + p, r * 100), 1.0}});
        }
    }

    using Row = std::map<int, double>;

    /// The DCB row of every receiver (the hard mode's rows).
    std::vector<Row> dcbRows() {
        std::vector<Row> rows;
        for (int r = 0; r < R_; ++r) rows.push_back(Row{{col(entry_["C1C"].state_key, r), 1.0}});
        return rows;
    }

    /// The analytic N5 direction of receiver @p r.
    Eigen::VectorXd n5(int r) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(ncols());
        const double dmu = mu_["1"] - mu_["2"];
        for (int k = 0; k < K_; ++k)
            for (int s = 0; s < S_; ++s) v(col("iono", r * 100 + s, k)) = 1.0;
        v(col(entry_["C1C"].state_key, r)) = -dmu;
        v(col(entry_["C5Q"].state_key, r)) = -mu_["5"];
        for (const char* p : {"L1C", "L2W", "L5Q"}) v(col(std::string("rpb_") + p, r)) = mu_[std::string(1, p[1])];
        return v;
    }

    Eigen::MatrixXd matrix(const std::vector<Row>& rows) {
        Eigen::MatrixXd M = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(rows.size()), ncols());
        for (std::size_t i = 0; i < rows.size(); ++i)
            for (const auto& [c, v] : rows[i]) M(static_cast<Eigen::Index>(i), c) = v;
        return M;
    }

    const std::vector<Row>& obs() const { return obs_; }
    const std::vector<Row>& hard() const { return hard_; }
    Eigen::Index ncols() const { return static_cast<Eigen::Index>(cols_.size()); }
    const std::map<std::string, ReceiverBiasEntry>& entries() const { return entry_; }

private:
    int col(const std::string& name, int a, int b = -1) {
        const auto key = std::make_tuple(name, a, b);
        const auto it = cols_.find(key);
        if (it != cols_.end()) return it->second;
        const int c = static_cast<int>(cols_.size());
        cols_.emplace(key, c);
        return c;
    }

    int R_, S_, K_;
    std::map<std::tuple<std::string, int, int>, int> cols_;
    std::map<std::string, ReceiverBiasEntry>          entry_;
    std::map<std::string, double>                     mu_, lam_;
    std::vector<Row>                                  obs_, hard_;
};

Eigen::Index rankOf(const Eigen::MatrixXd& M) {
    Eigen::FullPivLU<Eigen::MatrixXd> lu(M);
    lu.setThreshold(1e-10);
    return lu.rank();
}

Eigen::MatrixXd stack(const Eigen::MatrixXd& A, const Eigen::MatrixXd& B) {
    Eigen::MatrixXd M(A.rows() + B.rows(), A.cols());
    M << A, B;
    return M;
}

}  // namespace

TEST(RxGfDatumRank, OneDcbRowPerReceiverClosesTheDirectionAsAPureDatum) {
    LogManager log;
    gnss_test::ModelStack ms;
    ASSERT_TRUE(ms.load(log, "rxgf_rank", "ppp-rtk", "", "", false));
    const ReceiverBiasResolver rb(ms.config, *ms.ppp, log);
    constexpr int R = 4, S = 3, K = 2;
    ToyNetwork net(rb, R, S, K);

    // The engine maps the pair's codes onto one DCB column and band 5 onto an IFB column.
    const auto& e = net.entries();
    ASSERT_EQ(e.at("C1C").state_key, e.at("C2W").state_key);
    ASSERT_NE(e.at("C1C").state_key, e.at("C5Q").state_key);
    EXPECT_NEAR(e.at("C1C").coefficient - e.at("C2W").coefficient, 1.0, 1e-12);
    EXPECT_DOUBLE_EQ(e.at("C5Q").coefficient, 1.0);

    const Eigen::MatrixXd A = net.matrix(net.obs());
    const Eigen::MatrixXd H = net.matrix(net.hard());
    const Eigen::MatrixXd D = net.matrix(net.dcbRows());
    const Eigen::Index n = net.ncols();
    const Eigen::Index rA = rankOf(A);
    const Eigen::Index rAH = rankOf(stack(A, H));
    EXPECT_EQ(rAH, rA + rankOf(H)) << "the engine's hard rows are a pure datum";
    EXPECT_EQ(n - rAH, R) << "one free direction per receiver after the engine's hard rows";

    const Eigen::MatrixXd AHD = stack(stack(A, H), D);
    EXPECT_EQ(rankOf(AHD), n) << "one DCB row per receiver closes the null space";
    EXPECT_EQ(rankOf(stack(A, D)), rA + R) << "the DCB rows constrain no estimable function";

    // The analytic direction at a non-root receiver: invisible to the data and the engine's rows,
    // and moved by its DCB row.
    const Eigen::VectorXd v = net.n5(2);
    EXPECT_LT((A * v).cwiseAbs().maxCoeff(), 1e-9);
    EXPECT_LT((H * v).cwiseAbs().maxCoeff(), 1e-9);
    EXPECT_GT((D * v).cwiseAbs().maxCoeff(), 0.1);
}
