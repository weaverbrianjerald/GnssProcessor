/**
 * @file test_ncq_leveling_response.cpp
 * @brief Responses of the leveling carry (the carried float ambiguities of the network engine)
 *        to a slip and to a return after a gap, read at the station build's ambiguity step and
 *        at the harvest's gap prune, with the counters each response writes.
 *
 * A carried ambiguity is a near-constant quantity held by a stiff prior: its walk is a few
 * micrometres per second at most.  If a slip or a long absence does not reach that prior, the
 * stale level folds at converged weight and the misfit is pushed into the receiver clock and the
 * ionosphere.  The cases pin what the ambiguity step does today:
 *   - a steady arc folds its carried value at its variance plus the walk over the gap;
 *   - on the leveling vehicle an arc in the free-epoch latch folds no prior at all, keeps its
 *     carried entry for the harvest to re-level, and is counted as a slip or, when only a
 *     re-entry response latched it, as a re-entry;
 *   - a return inside the horizon that nothing latched folds the carry inflated by the walk
 *     over the gap; a return past the horizon folds nothing and is counted at the cliff; the
 *     horizon is strict;
 *   - a pending re-seed mark folds nothing and is counted only for an arc that had a carry;
 *   - the harvest's gap prune erases exactly the entries past the horizon and counts them;
 *   - on the float vehicle the latched arc and the arc past the horizon are not skipped: each
 *     folds at the birth variance its drop restored, never at its converged variance.
 *
 * The station, its parameter set and the epoch are synthetic (one GPS station, three phase
 * arcs, columns in slot order); the step is called directly with the frame the station build
 * gives it.
 */
#include <gtest/gtest.h>

#include "atmosphere/GlobalIonosphereMap.hpp"
#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "network/assembly/Responses.hpp"
#include "network/assembly/StationBuild.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/LevelingCarry.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "../support/CarriedFamilyStates.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kQAmb    = 1e-6;    ///< m^2/s: the carried ambiguity's walk in these configurations
constexpr double kHorizon = 900.0;   ///< s: network.phase_leveling_max_gap_s
constexpr double kVar     = 1e-4;    ///< m^2: a converged carried ambiguity (1 cm)
constexpr double kBirth   = 4.0;     ///< m^2: the variance an arc was born with (2 m, one epoch of code)

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("ncq_leveling_null", sink);
}

/// The leveling vehicle with the free-epoch latch, the walk and the horizon stated.
const char* const kLevelVehicle = R"(,
    "phase_leveling": true, "phase_leveling_q_amb": 1e-6, "phase_leveling_max_gap_s": 900.0,
    "cycle_slip": {"relevel_free_epoch": true})";

/// The float vehicle (the reference configurations' vehicle) with the same walk and horizon.
const char* const kFloatVehicle = R"(,
    "phase_leveling": true, "phase_leveling_q_amb": 1e-6, "phase_leveling_max_gap_s": 900.0,
    "cycle_slip": {"relevel_free_epoch": true},
    "ambiguity_vehicle": "float_kf", "carry_conditional_coupling": true,
    "shared_carry_full_cov": true)";

std::string networkConfig(const std::string& network_extra) {
    return std::string(R"({
        "processing": {"mode": "ppp-rtk-network", "active_systems": ["GPS"],
                       "signal_priorities": {"GPS": "WC"}},
        "network": {"clock_datum_station": "AAAA")") + network_extra + R"(,
                    "stations": [{"name": "AAAA", "position_ecef": [6378137.0, 0.0, 0.0]},
                                 {"name": "BBBB", "position_ecef": [0.0, 6378137.0, 0.0]}]}
    })";
}

/// Loads @p body as a configuration; @p tag names the temporary file.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    const fs::path p = gnss_test::tempDir() / ("ncq_leveling_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    LogManager log;
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The station's three phase arcs; slot k is column k.
struct ArcName { const char* sat; const char* signal; };
constexpr ArcName kArcs[] = {{"G05", "L1C"}, {"G05", "L2W"}, {"G07", "L1C"}};
constexpr int kNArcs = 3;

/// One station over a loaded configuration, its arcs at known linearization points, and the
/// epoch the ambiguity step reads.
struct LevelingFixture {
    GnssConfig                           cfg;
    std::unique_ptr<NetworkTestRun>      run;
    std::unique_ptr<GlobalIonosphereMap> gim;
    std::vector<StationAssembly>         slots;
    Eigen::VectorXd                      x_lin;
    GlobalVtecShConfig                   shc{};
    double                               now_s{0.0};
    /// What the step folded inline (column, information, information times innovation, tag).
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>> inj;
    /// What the step left for the station's joint block.
    std::vector<PlPend>                  pend;

    LevelingFixture(const std::string& vehicle, const std::string& tag) {
        EXPECT_TRUE(loadInto(tag, networkConfig(vehicle), cfg));
        run = std::make_unique<NetworkTestRun>(cfg, nullLogger());
        run->setup().stations.resize(1);
        station().name = "AAAA";
        x_lin = Eigen::VectorXd(kNArcs);
        for (int k = 0; k < kNArcs; ++k) {
            LocalSlot s;
            s.ref = ambiguityRef(sat(k), signal(k));
            s.x_lin = 10.0 * static_cast<double>(k + 1);
            EXPECT_TRUE(addParam(station().params, s));
            x_lin(k) = s.x_lin;
        }
        slots.resize(1);
        for (int k = 0; k < kNArcs; ++k) slot().h2local[k] = k;
        NetworkEpoch& e = run->newEpoch(GnssTime{2313, 259230.0});
        now_s = TimeUtils::continuousGpsSeconds(e.se.time);
        e.clock = EpochClock{30.0, now_s, true};
        run->ctx().counters.n_loop_epochs = 7;
    }

    static SatId sat(int k) { return *SatId::parse(kArcs[k].sat); }
    static SignalId signal(int k) { return *SignalId::fromObsCode(System::GPS, kArcs[k].signal); }
    static ArcId arcId(int k) { return ArcId{StationId{0}, sat(k), signal(k)}; }
    /// The arc's key, as the free-epoch latch and the re-seed marks name it.
    static std::string key(int k) { return keyText(ambiguityRef(sat(k), signal(k))); }

    NetStation&      station() { return run->setup().stations[0]; }
    StationAssembly& slot()    { return slots[0]; }
    NetworkRunContext& ctx()   { return run->ctx(); }

    /// Writes arc @p k's carried entry, last seen @p age_s before this epoch.
    void carry(int k, double value, double var, double age_s) {
        LocalCarry lc;
        lc.value_abs   = value;
        lc.var         = var;
        lc.last_seen_s = now_s - age_s;
        lc.var_first   = var;
        lc.var_birth   = var;
        net::put(ctx().entities.arcs, arcId(k), &net::ArcState::amb) = lc;
    }

    const LocalCarry* carried(int k) {
        return net::get(ctx().entities.arcs, arcId(k), &net::ArcState::amb);
    }

    /// Puts arc @p k in this epoch's free-epoch latch with the cause bits @p cause (0: none
    /// recorded), as the slip responses of the slip scan leave it.
    void latch(int k, unsigned cause) {
        RelevelEpochState& rl = run->ep().events.slip.relevel;
        rl.free_ambs.insert({0, key(k)});
        if (cause != 0u) rl.free_cause[{0, key(k)}] = static_cast<std::uint8_t>(cause);
    }

    /// The ambiguity step of the station build, with the carry frame the station build gives it.
    void inject() {
        NetworkEpoch& e = run->ep();
        StationBuild b{ctx(), run->config(), run->est(), run->setup(), e.se, e.clock,
                       e.pass_a, e.events, gim, 0, station(), slot()};
        StationCarryFrame carry{b, /*legacy_local_inject=*/true, /*do_amb_carry=*/true, x_lin,
                                CouplingRowBuilder{run->est(), shc, /*q_clock=*/1e-4, /*q_cb=*/1e-4,
                                                   /*q_pb=*/1e-4, /*tu=*/false, /*gauge=*/false,
                                                   /*reinit=*/false, ctx().entities.sat_signals,
                                                   slot()}};
        networkAmbiguityCarryColumns(carry);
        inj  = carry.inj;
        pend = carry.pl_pend;
    }

    /// The inline fold of column @p col, if any: its information and its information-weighted
    /// innovation.
    bool folded(Eigen::Index col, double* lambda = nullptr, double* eta = nullptr) const {
        for (const auto& row : inj) {
            if (std::get<0>(row) != col) continue;
            if (lambda) *lambda = std::get<1>(row);
            if (eta) *eta = std::get<2>(row);
            return true;
        }
        return false;
    }

    long long resp(ResponseSite s) { return ctx().counters.prm_resp[static_cast<int>(s)]; }
};

/// The relative tolerance of a fold's information: the step forms 1/(var + q gap) exactly as
/// the expectation does, so only the last bits may differ.
constexpr double kRel = 1e-12;

}  // namespace

// ---- The steady arc: the control -------------------------------------------------------------

// Every carried arc seen one epoch ago folds its carried value as a diagonal prior of information
// 1/(var + q gap) aimed at the carried value; nothing is skipped and no response counter moves.
TEST(NcqLevelingResponse, ASteadyArcFoldsItsCarriedValueAtItsVariancePlusTheWalk) {
    LevelingFixture f(kLevelVehicle, "steady");
    const NetworkKnobs& k = f.ctx().knobs;
    ASSERT_TRUE(k.pl_on);
    ASSERT_FALSE(k.amb_float_kf);
    ASSERT_TRUE(k.relevel_free_on);
    ASSERT_FALSE(k.pl_blk_on) << "the leveling vehicle without the joint block folds inline";
    ASSERT_DOUBLE_EQ(k.pl_q_amb, kQAmb);
    ASSERT_DOUBLE_EQ(k.pl_max_gap_s, kHorizon);
    for (int a = 0; a < kNArcs; ++a) f.carry(a, f.x_lin(a) + 0.05, kVar, 30.0);
    f.inject();
    ASSERT_EQ(f.inj.size(), 3u);
    EXPECT_TRUE(f.pend.empty());
    const double want = 1.0 / (kVar + kQAmb * 30.0);
    for (int a = 0; a < kNArcs; ++a) {
        double lambda = 0.0, eta = 0.0;
        ASSERT_TRUE(f.folded(a, &lambda, &eta)) << a;
        EXPECT_NEAR(lambda, want, kRel * want) << a;
        EXPECT_NEAR(eta, lambda * 0.05, 1e-9 * lambda) << a;
    }
    EXPECT_EQ(f.slot().pl_amb.size(), 3u);
    EXPECT_EQ(f.slot().relevel_free_skips, 0);
    EXPECT_EQ(f.slot().prm_amb_free_latch, 0);
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 0);
    EXPECT_EQ(f.slot().prm_amb_pending_reseed_skip, 0);
    EXPECT_EQ(f.slot().inject_stale_skips, 0);
    EXPECT_EQ(f.slot().prm_amb_gap_skip, 0);
}

// ---- A slip on the leveling vehicle ----------------------------------------------------------

// An arc the MW detector latched folds no prior this epoch (the other arcs still fold), keeps its
// carried entry untouched for the harvest to re-level from this epoch's solve, stays on the
// harvest's list, and is counted once as a slip; the serial tallies carry the count into the run
// counter and the response table.
TEST(NcqLevelingResponse, ASlipLatchedArcFoldsNoPriorKeepsItsCarryAndIsCountedAsASlip) {
    LevelingFixture f(kLevelVehicle, "slip");
    for (int a = 0; a < kNArcs; ++a) f.carry(a, f.x_lin(a) + 0.05, kVar, 30.0);
    f.latch(1, RelevelEpochState::kCauseMw);
    f.inject();
    EXPECT_FALSE(f.folded(1)) << "the stale pre-slip level must not fold";
    EXPECT_TRUE(f.folded(0));
    EXPECT_TRUE(f.folded(2));
    EXPECT_EQ(f.inj.size(), 2u);
    EXPECT_EQ(f.slot().pl_amb.size(), 3u) << "the freed arc is still harvested (re-levelled)";
    EXPECT_EQ(f.slot().relevel_free_skips, 1);
    EXPECT_EQ(f.slot().prm_amb_free_latch, 1);
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 0);
    const LocalCarry* lc = f.carried(1);
    ASSERT_NE(lc, nullptr) << "the skip is not an erase";
    EXPECT_DOUBLE_EQ(lc->value_abs, f.x_lin(1) + 0.05);
    EXPECT_DOUBLE_EQ(lc->var, kVar);
    EXPECT_DOUBLE_EQ(lc->last_seen_s, f.now_s - 30.0);

    networkAmbiguityFreeEpochTally(f.ctx(), f.slots);
    networkResponsesStationTally(f.ctx(), f.slots);
    EXPECT_EQ(f.ctx().counters.n_relevel_free_skips, 1);
    EXPECT_EQ(f.resp(ResponseSite::AmbFreeEpochLatch), 1);
    EXPECT_EQ(f.resp(ResponseSite::AmbFreeEpochLatchReentry), 0);
}

// The latch's cause decides the row: any cause with a bit outside the re-entry responses (the
// gap re-entry and the wind-up restart), and a latch with no recorded cause, is a slip; a cause
// made only of re-entry bits is a re-entry.  Either way the arc folds nothing.
TEST(NcqLevelingResponse, EveryCauseButAReentryAloneIsCountedAsASlip) {
    using RL = RelevelEpochState;
    struct Cause { unsigned bits; bool reentry; const char* name; };
    const Cause causes[] = {
        {0u, false, "no recorded cause"},
        {RL::kCauseLli, false, "loss of lock"},
        {RL::kCauseIfTd, false, "ionosphere-free time difference"},
        {RL::kCauseGfJump, false, "geometry-free jump"},
        {RL::kCauseT4Attrib, false, "RAIM attribution"},
        {static_cast<unsigned>(RL::kCauseGap | RL::kCauseMw), false, "gap and MW"},
        {RL::kCauseGap, true, "gap re-entry"},
        {RL::kCauseWindup, true, "wind-up restart"},
        {static_cast<unsigned>(RL::kCauseGap | RL::kCauseWindup), true, "gap and wind-up"},
    };
    int n = 0;
    for (const Cause& c : causes) {
        LevelingFixture f(kLevelVehicle, "cause" + std::to_string(n++));
        f.carry(0, f.x_lin(0) + 0.05, kVar, 30.0);
        f.latch(0, c.bits);
        f.inject();
        EXPECT_FALSE(f.folded(0)) << c.name;
        EXPECT_EQ(f.slot().relevel_free_skips, 1) << c.name;
        EXPECT_EQ(f.slot().prm_amb_free_latch, c.reentry ? 0 : 1) << c.name;
        EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, c.reentry ? 1 : 0) << c.name;
    }
}

// A latched arc with no carried entry (a new arc) has no prior to skip: it is in the latch's
// population count but in neither response row.
TEST(NcqLevelingResponse, ALatchedArcWithoutACarryIsNotAResponse) {
    LevelingFixture f(kLevelVehicle, "latch_no_carry");
    f.carry(0, f.x_lin(0), kVar, 30.0);
    f.latch(2, RelevelEpochState::kCauseMw);
    f.inject();
    EXPECT_TRUE(f.folded(0));
    EXPECT_FALSE(f.folded(2));
    EXPECT_EQ(f.slot().relevel_free_skips, 1);
    EXPECT_EQ(f.slot().prm_amb_free_latch, 0);
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 0);
}

// ---- A return after a gap --------------------------------------------------------------------

// A return 600 s after the arc was last seen, inside the 900 s horizon, that no response latched:
// the carry bridges the gap and folds at its variance inflated by the walk over the whole gap.
TEST(NcqLevelingResponse, AReturnInsideTheHorizonFoldsTheCarryInflatedByTheWalkOverTheGap) {
    LevelingFixture f(kLevelVehicle, "return_inside");
    f.carry(0, f.x_lin(0) + 0.05, kVar, 600.0);
    f.inject();
    double lambda = 0.0, eta = 0.0;
    ASSERT_TRUE(f.folded(0, &lambda, &eta));
    const double want = 1.0 / (kVar + kQAmb * 600.0);
    EXPECT_NEAR(lambda, want, kRel * want);
    EXPECT_NEAR(eta, lambda * 0.05, 1e-9 * lambda);
    EXPECT_EQ(f.slot().inject_stale_skips, 0);
    EXPECT_EQ(f.slot().prm_amb_gap_skip, 0);
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 0);
}

// The same return, latched by the gap re-entry response (the level did not survive the break):
// nothing folds and the skip is counted as a re-entry, in the station's slot and in the response
// table.
TEST(NcqLevelingResponse, AReentryLatchInsideTheHorizonFoldsNothingAndIsCountedAsAReentry) {
    LevelingFixture f(kLevelVehicle, "reentry_inside");
    f.carry(0, f.x_lin(0) + 0.05, kVar, 600.0);
    f.latch(0, RelevelEpochState::kCauseGap);
    f.inject();
    EXPECT_FALSE(f.folded(0));
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 1);
    EXPECT_EQ(f.slot().prm_amb_free_latch, 0);
    EXPECT_EQ(f.slot().inject_stale_skips, 0) << "the latch is read before the horizon";
    ASSERT_NE(f.carried(0), nullptr);
    networkResponsesStationTally(f.ctx(), f.slots);
    EXPECT_EQ(f.resp(ResponseSite::AmbFreeEpochLatchReentry), 1);
    EXPECT_EQ(f.resp(ResponseSite::AmbFreeEpochLatch), 0);
}

// A return 900.5 s after the arc was last seen, past the horizon: the leveling vehicle folds
// nothing (the cliff), leaves the entry for the harvest to re-stash, and counts the skip in both
// of its counters; the serial tally records the worst epoch and the response table the skip.
TEST(NcqLevelingResponse, AReturnPastTheHorizonFoldsNothingAndIsCountedAtTheCliff) {
    LevelingFixture f(kLevelVehicle, "return_past");
    f.carry(0, f.x_lin(0) + 0.05, kVar, kHorizon + 0.5);
    f.carry(1, f.x_lin(1) + 0.05, kVar, 30.0);
    f.inject();
    EXPECT_FALSE(f.folded(0));
    EXPECT_TRUE(f.folded(1));
    EXPECT_EQ(f.slot().inject_stale_skips, 1);
    EXPECT_EQ(f.slot().prm_amb_gap_skip, 1);
    EXPECT_NE(f.carried(0), nullptr) << "the inject never erases; the prune does";

    networkAmbiguityStaleSkipTally(f.ctx(), f.slots, f.run->ep().se);
    networkResponsesStationTally(f.ctx(), f.slots);
    const auto& C = f.ctx().counters;
    EXPECT_EQ(C.n_cr_inject_stale, 1);
    EXPECT_EQ(C.n_cr_inject_stale_epochs, 1);
    EXPECT_EQ(C.n_cr_inject_stale_max, 1);
    EXPECT_EQ(C.n_cr_inject_stale_max_sta, 1);
    EXPECT_EQ(C.n_cr_inject_stale_max_week, 2313);
    EXPECT_DOUBLE_EQ(C.cr_inject_stale_max_tow, 259230.0);
    EXPECT_EQ(f.resp(ResponseSite::AmbGapSkip), 1);
}

// The horizon is strict: a return exactly 900 s after the arc was last seen still bridges, at the
// variance plus the walk over 900 s.
TEST(NcqLevelingResponse, TheHorizonIsStrict) {
    LevelingFixture f(kLevelVehicle, "horizon_edge");
    f.carry(0, f.x_lin(0), kVar, kHorizon);
    f.inject();
    double lambda = 0.0;
    ASSERT_TRUE(f.folded(0, &lambda));
    const double want = 1.0 / (kVar + kQAmb * kHorizon);
    EXPECT_NEAR(lambda, want, kRel * want);
    EXPECT_EQ(f.slot().inject_stale_skips, 0);
}

// ---- The pending re-seed mark ----------------------------------------------------------------

// An arc marked for a re-seed folds nothing, whatever its carry; the skip is counted only for an
// arc that had a carried entry (a new arc has no prior to skip).
TEST(NcqLevelingResponse, APendingReseedFoldsNothingAndIsCountedOnlyForACarriedArc) {
    LevelingFixture f(kLevelVehicle, "reseed");
    f.carry(0, f.x_lin(0) + 0.05, kVar, 30.0);
    f.carry(1, f.x_lin(1) + 0.05, kVar, 30.0);
    markPendingReseed(f.station().params, SatSignalId{LevelingFixture::sat(0), LevelingFixture::signal(0)});
    markPendingReseed(f.station().params, SatSignalId{LevelingFixture::sat(2), LevelingFixture::signal(2)});
    f.inject();
    EXPECT_FALSE(f.folded(0));
    EXPECT_TRUE(f.folded(1));
    EXPECT_FALSE(f.folded(2));
    EXPECT_EQ(f.slot().prm_amb_pending_reseed_skip, 1);
    networkResponsesStationTally(f.ctx(), f.slots);
    EXPECT_EQ(f.resp(ResponseSite::AmbPendingReseedSkip), 1);
}

// ---- The gap prune ---------------------------------------------------------------------------

// The prune after the harvest erases the entries unseen past the horizon and keeps the one at the
// horizon; it counts the removals, the epoch, the largest removal with the carry size before it,
// the response row, and the entry the float drop had dropped this same epoch.  A second prune in
// the same epoch finds nothing more.
TEST(NcqLevelingResponse, TheGapPruneErasesOnlyTheEntriesPastTheHorizonAndCountsThem) {
    LevelingFixture f(kLevelVehicle, "prune");
    ASSERT_DOUBLE_EQ(f.ctx().params.spec(NetworkParamKind::Ambiguity).gap_horizon_s, kHorizon);
    f.carry(0, f.x_lin(0), kVar, kHorizon);
    f.carry(1, f.x_lin(1), kVar, kHorizon + 0.5);
    f.carry(2, f.x_lin(2), kVar, 2000.0);
    net::get(f.ctx().entities.arcs, LevelingFixture::arcId(2), &net::ArcState::amb)->dropped_ep = 7;

    networkPruneAmbiguities(f.run->run(), f.run->ep(), f.run->ep().events);
    EXPECT_NE(f.carried(0), nullptr);
    EXPECT_EQ(f.carried(1), nullptr);
    EXPECT_EQ(f.carried(2), nullptr);
    const auto& C = f.ctx().counters;
    EXPECT_EQ(C.n_cr_gap_prune, 2);
    EXPECT_EQ(C.n_cr_gap_prune_epochs, 1);
    EXPECT_EQ(C.n_cr_gap_prune_max, 2);
    EXPECT_EQ(C.n_cr_gap_prune_max_fleet, 3);
    EXPECT_EQ(C.prm_pruned_after_drop, 1);
    EXPECT_EQ(f.resp(ResponseSite::AmbGapPrune), 2);

    networkPruneAmbiguities(f.run->run(), f.run->ep(), f.run->ep().events);
    EXPECT_EQ(C.n_cr_gap_prune, 2);
    EXPECT_EQ(C.n_cr_gap_prune_epochs, 1);
    EXPECT_NE(f.carried(0), nullptr);
}

// ---- The float vehicle -----------------------------------------------------------------------

// On the float vehicle a slip drops the arc to its birth variance before the station build (value
// kept), and the latched arc is then not skipped: it goes to the joint block at that birth
// variance plus the walk, never at its converged variance.  It is in the latch's population count
// and in neither leveling-vehicle row.
TEST(NcqLevelingResponse, OnTheFloatVehicleALatchedArcFoldsAtItsDroppedBirthVariance) {
    LevelingFixture f(kFloatVehicle, "float_latch");
    const NetworkKnobs& k = f.ctx().knobs;
    ASSERT_TRUE(k.amb_float_kf);
    ASSERT_TRUE(k.pl_blk_on) << "the vehicle's fold is the joint block";
    ASSERT_DOUBLE_EQ(k.pl_q_amb, kQAmb);

    // Born loose one harvest, converged the next: the harvest's first-variance rule keeps the
    // birth variance on the entry.
    gnss_test::CarriedFamilyState s;
    s.station  = 0;
    s.in_block = false;
    s.stamp_s  = f.now_s - 60.0;
    s.epoch    = 5;
    s.arcs     = {{"G05", "L1C", 10.3, kBirth}};
    gnss_test::stashCarriedFamilies(f.ctx().entities, s);
    s.stamp_s  = f.now_s - 30.0;
    s.epoch    = 6;
    s.arcs     = {{"G05", "L1C", 10.05, kVar}};
    gnss_test::stashCarriedFamilies(f.ctx().entities, s);
    ASSERT_NE(f.carried(0), nullptr);
    ASSERT_DOUBLE_EQ(f.carried(0)->var, kVar);
    ASSERT_DOUBLE_EQ(f.carried(0)->var_birth, kBirth);

    const auto out = gnss_test::applyArcSlipDrop(f.ctx().entities, 0, "G05", "L1C",
                                                 /*birth_floor_m2=*/0.0, /*drop_floor_m2=*/0.0,
                                                 f.now_s, 7);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->acted);
    EXPECT_DOUBLE_EQ(f.carried(0)->var, kBirth);
    EXPECT_DOUBLE_EQ(f.carried(0)->value_abs, 10.05) << "the drop keeps the value";

    f.latch(0, RelevelEpochState::kCauseMw);
    f.inject();
    EXPECT_TRUE(f.inj.empty()) << "the block route folds nothing inline";
    ASSERT_EQ(f.pend.size(), 1u);
    EXPECT_EQ(f.pend[0].col, 0);
    EXPECT_EQ(f.pend[0].lc, f.carried(0));
    EXPECT_NEAR(f.pend[0].var_eff, kBirth + kQAmb * 30.0, 1e-12);
    EXPECT_DOUBLE_EQ(f.pend[0].gap, 30.0);
    EXPECT_EQ(f.slot().relevel_free_skips, 1);
    EXPECT_EQ(f.slot().prm_amb_free_latch, 0);
    EXPECT_EQ(f.slot().prm_amb_free_latch_reentry, 0);
}

// On the float vehicle an arc returning past the horizon, dropped before the station build, is not
// skipped either: it folds at the birth variance plus the walk over the whole gap.  The cliff is
// counted in the population counter only, not in the leveling vehicle's skip row.
TEST(NcqLevelingResponse, OnTheFloatVehicleAReturnPastTheHorizonFoldsAtItsDroppedBirthVariance) {
    LevelingFixture f(kFloatVehicle, "float_past");
    const double age = 1000.0;
    gnss_test::CarriedFamilyState s;
    s.station  = 0;
    s.in_block = false;
    s.stamp_s  = f.now_s - age - 30.0;
    s.epoch    = 3;
    s.arcs     = {{"G05", "L1C", 10.3, kBirth}};
    gnss_test::stashCarriedFamilies(f.ctx().entities, s);
    s.stamp_s  = f.now_s - age;
    s.epoch    = 4;
    s.arcs     = {{"G05", "L1C", 10.05, kVar}};
    gnss_test::stashCarriedFamilies(f.ctx().entities, s);
    const auto out = gnss_test::applyArcSlipDrop(f.ctx().entities, 0, "G05", "L1C", 0.0, 0.0,
                                                 f.now_s, 7);
    ASSERT_TRUE(out.has_value());
    ASSERT_DOUBLE_EQ(f.carried(0)->var, kBirth);

    f.inject();
    ASSERT_EQ(f.pend.size(), 1u);
    EXPECT_EQ(f.pend[0].col, 0);
    EXPECT_NEAR(f.pend[0].var_eff, kBirth + kQAmb * age, 1e-12);
    EXPECT_EQ(f.slot().inject_stale_skips, 1);
    EXPECT_EQ(f.slot().prm_amb_gap_skip, 0);
}
