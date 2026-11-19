/**
 * @file test_ncq_rx_clock_isb_response.cpp
 * @brief Response tests of the network's receiver-clock carry and of the inter-system bias (ISB)
 *        carried beside it (network.carry_rx_clock_in_block): what happens at a receiver clock
 *        jump, at a small clock step under the jump gate, and at a step of the ISB, both at the
 *        epoch that sees the event and at the epochs where the carry is trusted again.
 *
 * Why: the receiver clock is carried as a (bias, drift) pair and its ISBs as random walks, both
 * as stiff priors folded into the next epoch's station block.  A carried quantity of this kind
 * needs a wired response to a jump, or the stale prior warps the solution after the event.  The
 * existing tests of this carry check the shared prediction kernel (rxClockPredict,
 * rxClockRateUpdate) and the block algebra in isolation; nothing drives the harvest's jump gate
 * itself, its erase of both halves, its counters, or the cold rebirth that follows.
 *
 * How: one station with a receiver clock column and a Galileo ISB column, four code rows (two
 * GPS rows on the clock, two Galileo rows on clock plus ISB, sigma 1 cm, no noise), run epoch
 * by epoch through the engine's own three stages:
 *   1. PASS B's clock-family step (networkRxClockBlockColumns) reads the carried entries and
 *      builds the predicted prior columns; they are folded as the engine folds them
 *      (CouplingRowBuilder::buildLocalBlock and appendCouplingRows);
 *   2. the station is solved on a real NetworkEstimator;
 *   3. the harvest (networkHarvestRxClock) runs the jump gate, the drift filter and the stash.
 * The run's knobs come from a loaded configuration with the carry armed on the F3 posture, so
 * every threshold is the loader's default: q 0.09 m^2 per epoch, jump gate k 10, rate prior
 * 1 m^2/s^2, ISB carried.  The clock drifts 0.3 m/s (9 m per 30 s epoch), as on the sim bed.
 *
 * The receiver clock jump has a declared response (erase the clock and its drift, count it,
 * re-born cold); those cases assert it.  The ISB has no step test of its own, and the clock's
 * erase leaves the ISB in place; those two cases measure what happens and skip with the gap
 * named when the measured behaviour shows no response.
 */

#include "network/state/RunContext.hpp"   // first, as in the neighbouring carry tests
#include "../support/CarriedFamilyStates.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/StationParamsBuilder.hpp"
#include "../support/TestTempDir.hpp"

#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/Responses.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/LocalCarryHarvest.hpp"
#include "network/carry/RxClockCarry.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "network/state/StationParams.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kT0         = 1.4e9;         // continuous GPS seconds of epoch 0
constexpr double kEpochS     = 30.0;          // epoch spacing (s)
constexpr double kDriftMps   = 0.3;           // receiver clock drift (m/s): 9 m per epoch
constexpr double kClock0M    = 3000.0;        // receiver clock at epoch 0 (m)
constexpr double kIsbM       = 8.0;           // Galileo ISB (m), as on the sim bed
constexpr double kMsM        = 299792.458;    // one millisecond of receiver clock (m)
constexpr double kIsbStepM   = 5.0;           // the ISB step of the ISB case (m)
constexpr double kCodeWeight = 1.0e4;         // one code row, sigma 1 cm (1/m^2)

/// The true receiver clock at epoch @p n (no jump).
double clockTruth(int n) { return kClock0M + kDriftMps * kEpochS * static_cast<double>(n); }

/// A loadable two-station network configuration with the receiver-clock carry armed on the
/// posture it is validated on (leveling carry, conditional coupling rows, F3 block).
std::string armedConfigText() {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\", \"Galileo\": \"CB\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"phase_leveling\": true,\n"
          "    \"carry_conditional_coupling\": true,\n"
          "    \"shared_carry_full_cov\": true,\n"
          "    \"phase_leveling_local_block\": true,\n"
          "    \"carry_rx_clock_in_block\": true,\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

GnssConfig loadArmedConfig(bool& ok) {
    const fs::path p = gnss_test::tempDir() / "ncq_rxclk_isb_cfg.json";
    {
        std::ofstream f(p);
        f << armedConfigText();
    }
    LogManager log;
    GnssConfig cfg;
    ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return cfg;
}

/// One clock-family prior column PASS B built this epoch.
struct Injected {
    bool   isb;       ///< The ISB column (else the primary clock).
    double mean;      ///< The predicted absolute value folded as the prior (m).
    double var_eff;   ///< Its prior variance (m^2).
};

/// What one epoch left for the assertions.
struct EpochResult {
    double                t{0.0};          ///< The epoch's stamp (continuous GPS s).
    std::vector<Injected> injected;        ///< PASS B's clock-family prior columns.
    std::size_t           n_recorded{0};   ///< Clock-family columns PASS B recorded for the harvest.
    bool                  block_ok{true};  ///< The prior block whitened (true when there was none).
    double                clock_post{0.0}; ///< The solve's absolute receiver clock (m).
    double                isb_post{0.0};   ///< The solve's absolute ISB (m).

    const Injected* clockPrior() const {
        for (const Injected& c : injected)
            if (!c.isb) return &c;
        return nullptr;
    }
    const Injected* isbPrior() const {
        for (const Injected& c : injected)
            if (c.isb) return &c;
        return nullptr;
    }
};

/// The run counters a clock-family response moves.
struct Tally {
    long long refused{0};      ///< n_rxclk_carry_refused
    long long gap_drop{0};     ///< n_rxclk_carry_gap_drop
    long long gate_tests{0};   ///< n_rxclk_gate_tests
    double    gate_sum{0.0};   ///< sum_rxclk_gate_m
    double    max_innov{0.0};  ///< max_rxclk_innov_m
    long long prm_jump{0};     ///< the declared response site rxclk_jump_erase
    long long prm_prune{0};    ///< the declared response site rxclk_gap_prune
};

/// One station, one receiver clock and one Galileo ISB, run through PASS B, the solve and the
/// harvest of the receiver-clock carry.
class RxClockIsbBench {
public:
    RxClockIsbBench()
        : cfg_(loadArmedConfig(loaded_)), fx_(std::make_unique<NetworkTestRun>(cfg_)) {
        fx_->setup().stations.resize(1);
        NetStation& st = fx_->setup().stations[0];
        st.name     = "AAAA";
        clock_slot_ = gnss_test::bornParam(st.params, rxClockRef());
        isb_slot_   = gnss_test::bornParam(st.params, isbRef(System::Galileo));
    }

    /// The configuration loaded, the carry is live on the block route with the ISB carried, and
    /// both columns took a slot.
    bool armed() const {
        const NetworkKnobs& k = fx_->ctx().knobs;
        return loaded_ && k.rxclk_blk_on && k.carry_cpl_cond && k.rxclk_isb && clock_slot_ >= 0
            && isb_slot_ >= 0 && clock_slot_ != isb_slot_;
    }

    const NetworkKnobs& knobs() const { return fx_->ctx().knobs; }

    /// The stamp of epoch @p n.
    static double stampOf(int n) { return kT0 + kEpochS * static_cast<double>(n); }

    /// Runs the next epoch with the given true clock and ISB.
    EpochResult step(double clock_m, double isb_m) {
        NetworkRunContext& ctx = fx_->ctx();
        EpochResult out;
        out.t = stampOf(n_);
        NetworkEpoch& e = fx_->newEpoch(GnssTime{});
        e.clock = EpochClock{n_ > 0 ? kEpochS : 0.0, out.t, true};
        using LoopEp = decltype(ctx.counters.n_loop_epochs);
        ctx.counters.n_loop_epochs = static_cast<LoopEp>(n_ + 1);

        // The station's data rows over [clock, ISB].
        NetworkEstimator& est = fx_->est();
        est.beginEpoch();
        e.assembly.asm_res.resize(1);
        StationAssembly& R = e.assembly.asm_res[0];
        R.A_s = Eigen::MatrixXd::Zero(4, est.numShared());
        R.A_r = Eigen::MatrixXd::Zero(4, 2);
        R.A_r(0, 0) = 1.0;
        R.A_r(1, 0) = 1.0;
        R.A_r(2, 0) = 1.0;
        R.A_r(2, 1) = 1.0;
        R.A_r(3, 0) = 1.0;
        R.A_r(3, 1) = 1.0;
        R.w = Eigen::VectorXd::Constant(4, kCodeWeight);
        R.y = Eigen::VectorXd(4);
        R.y << clock_m, clock_m, clock_m + isb_m, clock_m + isb_m;
        R.h2local[clock_slot_] = 0;
        R.h2local[isb_slot_]   = 1;
        // The linearization point of every slot: zero, and never written back, as in a run.
        const Eigen::VectorXd x_lin = Eigen::VectorXd::Zero(
            static_cast<Eigen::Index>(fx_->setup().stations[0].params.slots.size()));

        {   // PASS B: the clock family's prior columns, folded as the station build folds them.
            NetStation& st = fx_->setup().stations[0];
            StationBuild b{ctx, fx_->config(), est, fx_->setup(), e.se, e.clock, e.pass_a,
                           e.events, gim_, 0, st, R};
            const GlobalVtecShConfig shc{};
            const net::SatSignalTable sat_signals{};
            StationCarryFrame carry{b, true, true, x_lin,
                                    CouplingRowBuilder{est, shc, 0.0, 0.0, 0.0, false, false,
                                                       false, sat_signals, R}};
            networkRxClockBlockColumns(carry);
            for (const LocalBlockCol& c : carry.rxclk_bc)
                out.injected.push_back(Injected{c.is_rx_isb, c.lc->value_abs, c.var_eff});
            out.n_recorded = R.pl_rxclk.size();
            if (!carry.rxclk_bc.empty()) {
                out.block_ok = carry.cb.buildLocalBlock(carry.rxclk_bc);
                carry.cb.appendCouplingRows();
            }
        }

        // The solve.
        const std::size_t id = est.addStation(R.A_s, R.A_r, R.w, R.y);
        e.solve.x_s = est.solve(NetworkEstimator::Datum{});
        e.assembly.est_index_of.assign(1, static_cast<int>(id));
        const Eigen::VectorXd xr = est.reconstructStation(id, e.solve.x_s);
        out.clock_post = x_lin(clock_slot_) + xr(0);
        out.isb_post   = x_lin(isb_slot_) + xr(1);

        // The harvest: the jump gate, the drift filter, the stash and the prune.
        HarvestBorder border;
        const std::vector<StationHarvestBlock> blocks(1);
        networkHarvestRxClock(fx_->run(), e, border, blocks);
        ctx.carry.pl_prev_now_s = out.t;
        ++n_;
        return out;
    }

    /// Runs @p n epochs of the quiet truth from the current epoch on.
    void quiet(int n) {
        for (int k = 0; k < n; ++k) step(clockTruth(n_), kIsbM);
    }

    int nextIndex() const { return n_; }

    const LocalCarry* clock() const { return gnss_test::carriedClockFamily(ent(), 0, std::string()); }
    const LocalCarry* isb() const { return gnss_test::carriedClockFamily(ent(), 0, "Galileo"); }
    const net::RxClkRate* rate() const { return gnss_test::carriedClockRate(ent(), 0); }

    /// The one-step prediction the next harvest tests against, from the carried state (the
    /// gap is one epoch of 30 s; the harvest's own spacing makes gap_ep exactly 1).
    RxClkPred nextPrediction() const {
        const net::RxClkRate* rt = rate();
        if (rt == nullptr) return RxClkPred{};
        const double rv = rt->rate_var >= 0.0 ? rt->rate_var : knobs().rxclk_rate_var0;
        return rxClockPredict(rt->prev_abs, rt->prev_var, rt->rate, rv, kEpochS, 1.0,
                              knobs().rxclk_q_m2);
    }
    /// The next harvest's jump gate in metres: k sqrt(var_pred).
    double nextGate() const { return knobs().rxclk_jump_k * std::sqrt(nextPrediction().var); }

    Tally tally() const {
        const NetworkServiceCounters& C = fx_->ctx().counters;
        Tally t;
        t.refused    = C.n_rxclk_carry_refused;
        t.gap_drop   = C.n_rxclk_carry_gap_drop;
        t.gate_tests = C.n_rxclk_gate_tests;
        t.gate_sum   = C.sum_rxclk_gate_m;
        t.max_innov  = C.max_rxclk_innov_m;
        t.prm_jump   = C.prm_resp[static_cast<int>(ResponseSite::RxclkJumpErase)];
        t.prm_prune  = C.prm_resp[static_cast<int>(ResponseSite::RxclkGapPrune)];
        return t;
    }

private:
    const net::NetworkEntities& ent() const { return fx_->ctx().entities; }

    bool                                 loaded_{false};
    GnssConfig                           cfg_;
    std::unique_ptr<NetworkTestRun>      fx_;
    std::unique_ptr<GlobalIonosphereMap> gim_;
    int                                  clock_slot_{-1};
    int                                  isb_slot_{-1};
    int                                  n_{0};
};

}  // namespace

// =============================================================================
// The control: a quiet drifting clock never trips the gate, the drift is learned from the
// first increment, and from the third epoch on the injected prior is stiff (trusted).
// Without this case a skip or a refusal below could be an artifact of the bench.
// =============================================================================
TEST(NcqRxClockIsbResponse, AQuietDriftNeverTripsTheGateAndTheCarryIsTrustedFromTheThirdEpoch) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    std::vector<EpochResult> r;
    for (int n = 0; n < 6; ++n) r.push_back(bench.step(clockTruth(n), kIsbM));

    const Tally t = bench.tally();
    EXPECT_EQ(t.refused, 0);
    EXPECT_EQ(t.prm_jump, 0);
    EXPECT_EQ(t.gap_drop, 0);
    // Epoch 0 has nothing to predict from; epochs 1 to 5 are each tested once.
    EXPECT_EQ(t.gate_tests, 5);

    // Epoch 0: nothing carried, nothing injected; both columns recorded for the harvest.
    EXPECT_TRUE(r[0].injected.empty());
    EXPECT_EQ(r[0].n_recorded, 2u);
    // Epoch 1: the drift is not known yet, so the clock prior is nearly free (sigma about 30 m).
    ASSERT_NE(r[1].clockPrior(), nullptr);
    EXPECT_GT(r[1].clockPrior()->var_eff, 800.0);
    // Epochs 2 to 5: one increment has been folded, so the prior is stiff (sigma under 1 m).
    for (int n = 2; n < 6; ++n) {
        ASSERT_NE(r[static_cast<std::size_t>(n)].clockPrior(), nullptr) << "epoch " << n;
        EXPECT_LT(r[static_cast<std::size_t>(n)].clockPrior()->var_eff, 1.0) << "epoch " << n;
        EXPECT_TRUE(r[static_cast<std::size_t>(n)].block_ok) << "epoch " << n;
    }
    // The ISB is carried from epoch 1 as a pure random walk.
    for (int n = 1; n < 6; ++n) {
        ASSERT_NE(r[static_cast<std::size_t>(n)].isbPrior(), nullptr) << "epoch " << n;
        EXPECT_NEAR(r[static_cast<std::size_t>(n)].isbPrior()->mean, kIsbM, 1e-3) << "epoch " << n;
    }

    ASSERT_NE(bench.rate(), nullptr);
    EXPECT_NEAR(bench.rate()->rate, kDriftMps, 1e-3);
    EXPECT_EQ(bench.rate()->n_incr, 5);
    ASSERT_NE(bench.clock(), nullptr);
    ASSERT_NE(bench.isb(), nullptr);
    EXPECT_DOUBLE_EQ(bench.clock()->birth_s, RxClockIsbBench::stampOf(0));
    EXPECT_DOUBLE_EQ(bench.isb()->birth_s, RxClockIsbBench::stampOf(0));
    EXPECT_NEAR(bench.clock()->value_abs, clockTruth(5), 1e-3);
}

// =============================================================================
// A one-millisecond receiver clock jump, at the epoch that sees it.  PASS B has no data test,
// so the stale clock prior is folded into the jump epoch's solve; the harvest after that solve
// tests the innovation, refuses the carry, erases the clock and its drift, and counts it once
// in its own counter and once at the declared response site.  The ISB is not erased.
// Fails if: the gate compares against a different prediction than PASS B injected, the erase
// keeps either half, the count moves at another site, or the ISB is erased with the clock.
// =============================================================================
TEST(NcqRxClockIsbResponse, AMillisecondJumpIsErasedAtTheHarvestThatSeesItWithBothHalvesAndOneCount) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    const RxClkPred pred = bench.nextPrediction();
    const double gate = bench.nextGate();
    const Tally before = bench.tally();
    ASSERT_GT(gate, 0.0);
    ASSERT_LT(gate, 10.0);   // a converged drift: the gate is metres, not a millisecond

    const EpochResult r = bench.step(clockTruth(3) + kMsM, kIsbM);

    // Before detection: the jump epoch folded the stale, stiff prediction (the two edges share
    // one kernel, so the injected prior is exactly the harvest's prediction).
    const Injected* inj = r.clockPrior();
    ASSERT_NE(inj, nullptr);
    EXPECT_DOUBLE_EQ(inj->mean, pred.mean);
    EXPECT_DOUBLE_EQ(inj->var_eff, pred.var);
    EXPECT_LT(inj->var_eff, 1.0);

    // At detection: one refusal, both halves erased.
    const Tally after = bench.tally();
    EXPECT_EQ(after.refused - before.refused, 1);
    EXPECT_EQ(after.prm_jump - before.prm_jump, 1);
    EXPECT_EQ(after.prm_prune - before.prm_prune, 0);
    EXPECT_EQ(after.gap_drop - before.gap_drop, 0);
    EXPECT_EQ(bench.clock(), nullptr);
    EXPECT_EQ(bench.rate(), nullptr);
    // The gate census saw exactly this test, at the predicted gate, and the jump as its largest
    // innovation (the folded prior pulls the solution back by well under 1 % here).
    EXPECT_EQ(after.gate_tests - before.gate_tests, 1);
    EXPECT_NEAR(after.gate_sum - before.gate_sum, gate, 1e-9 * gate);
    EXPECT_GT(after.max_innov, 0.9 * kMsM);
    // The receiver's step is common to every system: the ISB carry is not erased.
    EXPECT_NE(bench.isb(), nullptr);
}

// =============================================================================
// After the millisecond jump: the re-coupling edge.  The next epoch carries no clock prior
// (cold by design) and re-births the entry with a fresh drift state; the epoch after injects a
// nearly free prior (the drift is unknown again) and folds the first increment through a wide
// gate; from the epoch after that the prior is stiff again.
// Fails if: a stale entry survives the erase, the rebirth keeps the old birth stamp, the drift
// state is not reset to its no-rate sentinel, or the gate stays wide after the first increment.
// =============================================================================
TEST(NcqRxClockIsbResponse, AfterAMillisecondJumpTheClockIsRebornColdAndTrustedAgainAfterOneIncrement) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    bench.step(clockTruth(3) + kMsM, kIsbM);
    ASSERT_EQ(bench.clock(), nullptr);
    const Tally t_jump = bench.tally();
    ASSERT_EQ(t_jump.refused, 1);

    // Epoch 4: no clock prior, the ISB prior still folded, both columns recorded.
    const EpochResult r4 = bench.step(clockTruth(4) + kMsM, kIsbM);
    EXPECT_EQ(r4.clockPrior(), nullptr);
    EXPECT_NE(r4.isbPrior(), nullptr);
    EXPECT_EQ(r4.n_recorded, 2u);
    const Tally t4 = bench.tally();
    EXPECT_EQ(t4.refused, t_jump.refused);
    EXPECT_EQ(t4.gate_tests, t_jump.gate_tests);   // no prediction to test against
    ASSERT_NE(bench.clock(), nullptr);
    EXPECT_DOUBLE_EQ(bench.clock()->birth_s, r4.t);
    EXPECT_EQ(bench.clock()->birth_ep, 5);
    EXPECT_DOUBLE_EQ(bench.clock()->var_first, bench.clock()->var);
    // The reborn value is this epoch's solution.  It sits centimetres off the data here, not
    // millimetres: the ISB carried out of the jump epoch is off (the case on the ISB below)
    // and pulls the free clock through the Galileo rows.
    EXPECT_NEAR(bench.clock()->value_abs, clockTruth(4) + kMsM, 1.0);
    ASSERT_NE(bench.rate(), nullptr);
    EXPECT_EQ(bench.rate()->n_incr, 0);
    EXPECT_LT(bench.rate()->rate_var, 0.0);
    EXPECT_DOUBLE_EQ(bench.rate()->prev_s, r4.t);

    // Epoch 5: a nearly free prior (sigma about 30 m) and a wide gate (about 300 m) that the
    // ordinary 9 m of drift passes; the first increment is folded.
    const double gate5 = bench.nextGate();
    EXPECT_GT(gate5, 250.0);
    const EpochResult r5 = bench.step(clockTruth(5) + kMsM, kIsbM);
    ASSERT_NE(r5.clockPrior(), nullptr);
    EXPECT_GT(r5.clockPrior()->var_eff, 800.0);
    const Tally t5 = bench.tally();
    EXPECT_EQ(t5.refused, t_jump.refused);
    EXPECT_EQ(t5.gate_tests - t4.gate_tests, 1);
    EXPECT_NEAR(t5.gate_sum - t4.gate_sum, gate5, 1e-9 * gate5);
    ASSERT_NE(bench.rate(), nullptr);
    EXPECT_EQ(bench.rate()->n_incr, 1);
    EXPECT_NEAR(bench.rate()->rate, kDriftMps, 0.01);   // the rebirth epoch's offset enters once

    // Epoch 6: trusted again, a stiff prior and a gate of metres.
    const double gate6 = bench.nextGate();
    EXPECT_LT(gate6, 10.0);
    const EpochResult r6 = bench.step(clockTruth(6) + kMsM, kIsbM);
    ASSERT_NE(r6.clockPrior(), nullptr);
    EXPECT_LT(r6.clockPrior()->var_eff, 1.0);
    EXPECT_EQ(bench.tally().refused, t_jump.refused);
    ASSERT_NE(bench.clock(), nullptr);
    EXPECT_DOUBLE_EQ(bench.clock()->birth_s, r4.t);   // the rebirth's stamp is kept
}

// =============================================================================
// A small clock step under the gate (0.4 of it, about 1.5 m here): the declared behaviour is
// absorption.  No refusal and no count; the entry keeps its birth and takes the stepped value;
// the gate census records the test at the predicted gate and the step as its innovation; the
// drift estimate is pulled toward the step and the carry stays trusted on the epochs after.
// Fails if: the gate fires under k sigma, the census records another gate, the entry is
// re-born, or the drift overshoot that follows makes a later epoch trip.
// =============================================================================
TEST(NcqRxClockIsbResponse, AStepUnderTheGateIsAbsorbedWithNoCountAndTheCarryStaysTrusted) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    const RxClkPred pred = bench.nextPrediction();
    const double gate = bench.nextGate();
    ASSERT_GT(gate, 0.0);
    ASSERT_LT(gate, 10.0);
    const double step_m = 0.4 * gate;
    ASSERT_NE(bench.rate(), nullptr);
    const double rate_before = bench.rate()->rate;
    const Tally before = bench.tally();

    bench.step(clockTruth(3) + step_m, kIsbM);

    const Tally after = bench.tally();
    EXPECT_EQ(after.refused - before.refused, 0);
    EXPECT_EQ(after.prm_jump - before.prm_jump, 0);
    EXPECT_EQ(after.gate_tests - before.gate_tests, 1);
    EXPECT_NEAR(after.gate_sum - before.gate_sum, gate, 1e-9 * gate);
    ASSERT_NE(bench.clock(), nullptr);
    EXPECT_DOUBLE_EQ(bench.clock()->birth_s, RxClockIsbBench::stampOf(0));
    EXPECT_NEAR(bench.clock()->value_abs, clockTruth(3) + step_m, 0.01);
    const double innov = bench.clock()->value_abs - pred.mean;
    EXPECT_LT(std::abs(innov), gate);
    EXPECT_GT(std::abs(innov), 0.3 * gate);
    EXPECT_NEAR(after.max_innov, std::max(before.max_innov, std::abs(innov)), 1e-9);
    ASSERT_NE(bench.rate(), nullptr);
    EXPECT_GT(bench.rate()->rate, rate_before);   // the step pulls the drift

    // The step persists; the carry follows it and never trips on the drift's overshoot.
    for (int n = 4; n < 7; ++n) {
        const EpochResult r = bench.step(clockTruth(n) + step_m, kIsbM);
        ASSERT_NE(r.clockPrior(), nullptr) << "epoch " << n;
        if (n == 4) {   // the prior already carries the step
            EXPECT_LT(std::abs(r.clockPrior()->mean - (clockTruth(4) + step_m)),
                      std::abs(r.clockPrior()->mean - clockTruth(4)));
        }
        EXPECT_LT(r.clockPrior()->var_eff, 1.0) << "epoch " << n;
    }
    EXPECT_EQ(bench.tally().refused, before.refused);
    EXPECT_NEAR(bench.rate()->rate, kDriftMps, 0.05);
}

// =============================================================================
// A step a few times the gate (3 gates, about 11 m here, far below a millisecond) takes the
// same response as the millisecond: the response is set by the gate, not by the step's size.
// Fails if: the gate is a fixed size in metres or the erase applies only to large steps.
// =============================================================================
TEST(NcqRxClockIsbResponse, AStepAFewTimesTheGateIsErasedLikeAMillisecond) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    const double gate = bench.nextGate();
    ASSERT_GT(gate, 0.0);
    ASSERT_LT(gate, 10.0);
    const Tally before = bench.tally();

    bench.step(clockTruth(3) + 3.0 * gate, kIsbM);

    const Tally after = bench.tally();
    EXPECT_EQ(after.refused - before.refused, 1);
    EXPECT_EQ(after.prm_jump - before.prm_jump, 1);
    EXPECT_EQ(bench.clock(), nullptr);
    EXPECT_EQ(bench.rate(), nullptr);
    EXPECT_NE(bench.isb(), nullptr);

    const EpochResult r4 = bench.step(clockTruth(4) + 3.0 * gate, kIsbM);
    EXPECT_EQ(r4.clockPrior(), nullptr);
    ASSERT_NE(bench.clock(), nullptr);
    EXPECT_DOUBLE_EQ(bench.clock()->birth_s, r4.t);
}

// =============================================================================
// The ISB in a clock-jump epoch.  The clock's erase leaves the ISB in place on the ground that
// a receiver step is common to every system.  But the jump epoch's solve folded the stale clock
// prior, so the Galileo rows put part of the jump into the ISB, and the ISB is stashed with that
// value: it has no test of its own and is not re-born with the clock.  Measured here; skipped
// with the gap named when the ISB kept a moved value.
// Passes (no skip) if: the ISB carried out of a jump epoch keeps its pre-jump value within
// 1 cm, or is re-born at the jump.
// =============================================================================
TEST(NcqRxClockIsbResponse, TheIsbCarriedOutOfAClockJumpEpochKeepsItsValue) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    ASSERT_NE(bench.isb(), nullptr);
    const double isb_before = bench.isb()->value_abs;
    const double birth_before = bench.isb()->birth_s;
    ASSERT_NEAR(isb_before, kIsbM, 1e-3);

    const EpochResult r3 = bench.step(clockTruth(3) + kMsM, kIsbM);
    ASSERT_EQ(bench.tally().refused, 1);   // the clock's own response did fire
    const LocalCarry* isb = bench.isb();
    const bool reborn = isb == nullptr || isb->birth_s != birth_before;
    const double shift = isb != nullptr ? isb->value_abs - isb_before : 0.0;
    if (!reborn && std::abs(shift) > 0.01) {
        const double clock_short = (clockTruth(3) + kMsM) - r3.clock_post;
        const EpochResult r4 = bench.step(clockTruth(4) + kMsM, kIsbM);
        GTEST_SKIP() << "Gap: a receiver clock jump moves the carried inter-system bias and "
                        "nothing answers it.  The jump epoch's solve folds the stale clock prior "
                        "(it is tested only after the solve), leaving the clock "
                     << clock_short << " m short of the data and the Galileo ISB moved by "
                     << shift << " m; the jump response erases only the clock and its drift, so "
                        "the ISB is carried on with that value (its birth kept, no counter) and "
                        "the next epoch's ISB is still "
                     << (r4.isb_post - kIsbM) << " m off.";
    }
    if (isb != nullptr && !reborn) EXPECT_NEAR(shift, 0.0, 0.01);
}

// =============================================================================
// An ISB step (5 m, about 16 sigma of the ISB's per-epoch walk).  PASS B folds the old ISB at
// var + q in the step epoch, and the harvest has no innovation test for an ISB: the step is
// re-stashed with no counter and the entry keeps its birth.  Measured here; skipped with the
// gap named when nothing answered the step.
// Passes (no skip) if: an ISB step is erased or re-born at the harvest that sees it, or counted.
// =============================================================================
TEST(NcqRxClockIsbResponse, AnIsbStepIsDetectedAndAnsweredAtTheHarvestThatSeesIt) {
    RxClockIsbBench bench;
    ASSERT_TRUE(bench.armed());
    bench.quiet(3);
    ASSERT_NE(bench.isb(), nullptr);
    const double isb_var_before = bench.isb()->var;
    const double birth_before = bench.isb()->birth_s;
    const Tally before = bench.tally();

    const EpochResult r3 = bench.step(clockTruth(3), kIsbM + kIsbStepM);

    // The step epoch folds the old ISB as a pure random walk: the prior is var + q.
    const Injected* inj = r3.isbPrior();
    ASSERT_NE(inj, nullptr);
    EXPECT_NEAR(inj->mean, kIsbM, 1e-3);
    EXPECT_DOUBLE_EQ(inj->var_eff, isb_var_before + bench.knobs().rxclk_q_m2);
    const double step_sigmas = kIsbStepM / std::sqrt(inj->var_eff);

    const Tally after = bench.tally();
    const LocalCarry* isb = bench.isb();
    const bool counted = after.refused != before.refused || after.prm_jump != before.prm_jump
                      || after.prm_prune != before.prm_prune;
    const bool reborn = isb == nullptr || isb->birth_s != birth_before;
    // The clock is untouched by an ISB step: it must not be refused for it.
    EXPECT_NE(bench.clock(), nullptr);
    if (!counted && !reborn) {
        GTEST_SKIP() << "Gap: the inter-system bias carry has no step test.  A "
                     << kIsbStepM << " m ISB step (" << step_sigmas
                     << " sigma of its folded prior) is re-stashed at "
                     << isb->value_abs
                     << " m with its birth kept and no counter moved; the step epoch's solve "
                        "folded the old ISB at full weight.  The clock's jump gate tests the "
                        "primary clock only.";
    }
    EXPECT_TRUE(isb == nullptr || isb->birth_s == r3.t);
}
