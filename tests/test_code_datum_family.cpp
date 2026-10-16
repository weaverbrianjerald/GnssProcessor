/**
 * @file test_code_datum_family.cpp
 * @brief Card GFC: one clock datum per constellation across Galileo's two code families
 *        (docs/refound/GFC_galileo_family_clock_datum_2026-10-10.md).
 *
 * The card adds two default-off arms to the time-linked code-bias datum, both built in
 * @c NetworkSbasis::buildCodeBiasDatumRows and read only under its @c cb_time_linked
 * argument:
 *
 *  - pair continuity (@c network.code_datum_pair_continuity), hold then continue: at an
 *    entering edge a satellite keeps its last kept ionosphere-free pin pair for the one
 *    kept epoch the entering signal needs to get a previous solved value (unless the held
 *    pair has none, or broadcast_tgd's group delay describes the selected pair and not the
 *    held one), and at a pair change where both new signals have a previous solved value
 *    (the release of a hold, or a held signal leaving), the pin's target is the new pair's
 *    previous solved combination and the satellite is not marked for a re-seed.  Read only
 *    where no row re-asserts an absolute target at a pair change
 *    (@c NetworkSbasis::pinPairContinuityBlocker).  The emit site leaves the re-selection
 *    discontinuity undeclared only when its baseline is the pair the builder continued
 *    from and no re-seed is marked or pending; the begin and the end of each hold are
 *    declared once per kept epoch;
 *  - family difference (@c network.code_datum_family_difference, Galileo only): a
 *    secondary-family (X) channel's sum-to-zero row is replaced by Sum (d_X - d_P) over
 *    the satellites carrying it and its same-band primary partner, time-linked
 *    (@c NetworkSbasis::codeFamilyDiffTimeLink), with the sum-to-zero row kept with no
 *    partner, no common satellite, all spanned X columns pinned, or for the one epoch a
 *    family re-seed would replace a carried level;
 *  - step 2, the first-sighting family prior
 *    (@c network.code_family_birth_prior_sigma_m, read only with the family
 *    difference): one soft row d_X - d_P = mu per Galileo band-1 satellite whose X or
 *    partner column is born (the carry does not continue it), mu the veterans' mean
 *    previous difference, or 0 where the family row re-seeds, and no row without either
 *    (@c NetworkSbasis::codeFamilyBirthPriors), returned apart from the gauge stack at
 *    weight 1/sigma^2 and folded by @c NetworkSbasis::foldFamilyBirthRows.
 *
 * These cases pin: the default path unchanged; the pair hold (the held pair, its carried
 * target, the rows that follow it, its release at the next kept epoch, the fallback when
 * it breaks, its absence without previous values, its refusal under the group delay, the
 * flag a channel re-seed sets, and its absence with the arm off or blocked); the
 * continuity target, marks, outcomes and inert postures; the
 * family row's coefficients, target, re-seed marks and every
 * sum-to-zero outcome; the datum's reach over the code-bias null directions; the
 * kept-epoch bookkeeping (counts once, declares hold edges and row switches); the emit-site
 * declaration rule and its attribution of a dropped cbpin= to a hold; the loader; the
 * run-end readout; and step 2's prior (its planner, its row, coefficients, target and
 * weight, one fold per birth, both directions of a birth, its absence with no partner
 * or with the prior inert, its kept-epoch counts and events, its knob and its run-end
 * line; its birth test on the carry, through the stash and the fold decision across a
 * READMIT park and seed and a clock-monitor reinit; no target on a bridged channel; the
 * carry map; and the fold at its own weight and kind); and the channel family key the
 * family row reads (@c NetworkSbasis::channelFamilyOf).
 *
 * Suite name @c CodeDatumFamilyTest is owned by this file alone (one suite name, one
 * file).
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ostream_sink.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents
#include "network/driver/SetupResult.hpp"
#include "common/GnssIdentity.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "network/state/CarryDisposition.hpp"   // networkCarryDecide (step 2: the birth test reads the carry)
#include "network/assembly/ContributionKind.hpp"
#include "network/assembly/Contributions.hpp"       // contributionSpec (step 2: CbFamilyBirth)
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/emit/SolPrep.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "support/NetworkTestRun.hpp"   // the run and epoch networkSolPrep's epoch form takes
#include "math/TimeUtils.hpp"                   // continuousGpsSeconds (the READMIT clock)
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// The value behind an entity-table lookup, throwing like std::map::at when it is absent
/// (card NOM step N4b moved the map the test read into the constellation table).
template <class V>
const V& netAt(const V* p) {
    if (p == nullptr) throw std::out_of_range("netAt: absent");
    return *p;
}

using Pair    = std::pair<std::string, std::string>;
using Outcome = NetworkSbasis::FamilyRowOutcome;
using PinOut  = NetworkSbasis::PinPairOutcome;

constexpr double kE1  = 1575.42e6;   // Galileo E1 and GPS L1
constexpr double kE5a = 1176.45e6;   // Galileo E5a
constexpr double kE5b = 1207.14e6;   // Galileo E5b
constexpr double kL2  = 1227.60e6;   // GPS L2

/// Carrier frequency of a (satellite, code signal) by constellation and RINEX band, in
/// the builder's oracle shape.  Unknown -> 0.
double freqOf(const std::string& sat, const std::string& sig) {
    const System sys = systemOfSat(sat);
    const std::optional<SignalId> id = SignalId::fromObsCode(sys, sig);
    if (!id) return 0.0;
    if (sys == System::Galileo) {
        switch (id->digit) {
            case '1': return kE1;
            case '5': return kE5a;
            case '7': return kE5b;
            default:  return 0.0;
        }
    }
    if (sys == System::GPS) {
        switch (id->digit) {
            case '1': return kE1;
            case '2': return kL2;
            default:  return 0.0;
        }
    }
    return 0.0;
}

/// The ionosphere-free weights of an (E1, E5a) pin.
double a1() { return kE1 * kE1 / (kE1 * kE1 - kE5a * kE5a); }
double a2() { return -kE5a * kE5a / (kE1 * kE1 - kE5a * kE5a); }

/// One event row a callback received.
struct Event {
    std::string tier, event, sat, action, detail;
};

/// Owns the builder's side channels, cleared before each call as the driver contract
/// requires.  Default posture: the time-linked code datum with no geometry-free datum,
/// so every channel keeps its level row.
struct Harness {
    bool cb_time_linked{true};
    bool emit_clock_disc{false};
    NetworkSatGfDatum sat_gf_datum{NetworkSatGfDatum::NoneMode};
    std::map<std::string, double> sat_gf_target_m;
    bool sat_gf_datum_time_linked{false};
    bool sat_gf_extra_band_free{false};
    std::set<std::string> extra_band_determined;
    NetworkSbasis::CodeDatumFamilyArms arms;
    NetworkSbasis::CodeBiasCarryMap    carry;   ///< The carry's fold per code-bias column (step 2's birth test); empty: the map alone.

    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;

    std::map<std::string, Pair> if_pin_sel, if_pin_real, cb_ifpin;
    std::set<std::string>       cb_reseed;

    NetworkSbasis::CodeBiasDatumRows build(const NetworkEstimator& est,
                                           const std::vector<Pair>& keys) {
        if_pin_sel.clear(); if_pin_real.clear(); cb_ifpin.clear(); cb_reseed.clear();
        const std::set<std::string> none;
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, keys, freqOf, cb_time_linked, emit_clock_disc,
            sat_gf_datum, sat_gf_target_m,
            sat_gf_datum_time_linked, /*sat_isc_state=*/false, none,
            sat_gf_extra_band_free, extra_band_determined,
            code_bias_prev, code_bias_ifpin_pair,
            code_bias_sat_seen, /*code_bias_datum_resumed=*/false,
            if_pin_sel, if_pin_real, cb_ifpin, cb_reseed,
            arms, carry);
    }
};

/// Register a roster of (satellite, signals) and return its keys in registration order.
std::vector<Pair> registerRoster(
    NetworkEstimator& est,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& roster) {
    std::vector<Pair> keys;
    for (const auto& [sat, sigs] : roster)
        for (const auto& s : sigs) {
            est.registerCodeBias(sat, s);
            keys.emplace_back(sat, s);
        }
    return keys;
}

/// The two-family roster of the card: three satellites tracked by both families, one by
/// the combined family only, one by the pilot family only.
const std::vector<std::pair<std::string, std::vector<std::string>>> kTwoFamily = {
    {"E01", {"C1C", "C5Q", "C1X", "C5X"}},
    {"E02", {"C1C", "C5Q", "C1X", "C5X"}},
    {"E03", {"C1C", "C5Q", "C1X", "C5X"}},
    {"E04", {"C1X", "C5X"}},
    {"E05", {"C1C", "C5Q"}},
};

/// The ionosphere-free pin row touching column @p col, or -1.
Eigen::Index ifPinRow(const NetworkSbasis::CodeBiasDatumRows& rows, Eigen::Index col) {
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r)
        if (rows.row_kinds[static_cast<std::size_t>(r)] == ContributionKind::CbIfPin
            && std::abs(rows.H(r, col)) > 1e-12)
            return r;
    return -1;
}

/// The level row (the sum-to-zero slot, kind CbSumToZero) touching column @p col, or -1.
Eigen::Index levelRowOn(const NetworkSbasis::CodeBiasDatumRows& rows, Eigen::Index col) {
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r)
        if (rows.row_kinds[static_cast<std::size_t>(r)] == ContributionKind::CbSumToZero
            && std::abs(rows.H(r, col)) > 1e-12)
            return r;
    return -1;
}

/// Whether two builds produced the same rows, targets, kinds and side channels, the
/// second with no card GFC record.
void expectSameOutput(const NetworkSbasis::CodeBiasDatumRows& a, const Harness& ha,
                      const NetworkSbasis::CodeBiasDatumRows& b, const Harness& hb) {
    ASSERT_EQ(a.H.rows(), b.H.rows());
    ASSERT_EQ(a.H.cols(), b.H.cols());
    EXPECT_TRUE(a.H == b.H);
    EXPECT_TRUE(a.z == b.z);
    EXPECT_EQ(a.row_kinds, b.row_kinds);
    EXPECT_EQ(ha.cb_reseed, hb.cb_reseed);
    EXPECT_EQ(ha.cb_ifpin, hb.cb_ifpin);
    EXPECT_EQ(ha.if_pin_real, hb.if_pin_real);
    EXPECT_EQ(a.n_s2z_channels, b.n_s2z_channels);
    EXPECT_EQ(a.n_s2z_reseed_rows, b.n_s2z_reseed_rows);
    EXPECT_EQ(a.n_s2z_reseed_sats, b.n_s2z_reseed_sats);
    EXPECT_TRUE(b.if_pin_pair_changes.empty());
    EXPECT_TRUE(b.if_pin_pair_holds.empty());
    EXPECT_TRUE(b.family_rows.empty());
}

/// Fill the previous-epoch maps so that every satellite of kTwoFamily is a veteran on
/// every channel it carries, with its pin pair unchanged.
void allVeterans(Harness& h) {
    double v = 0.1;
    for (const auto& [sat, sigs] : kTwoFamily) {
        for (const auto& s : sigs) {
            h.code_bias_prev["E|" + s][sat] = v;
            v += 0.37;
        }
        h.code_bias_sat_seen.insert(sat);
    }
    h.code_bias_ifpin_pair = {{"E01", {"C1C", "C5Q"}}, {"E02", {"C1C", "C5Q"}},
                              {"E03", {"C1C", "C5Q"}}, {"E04", {"C1X", "C5X"}},
                              {"E05", {"C1C", "C5Q"}}};
}

/// The E11 leaving edge of the card: pinned on (C1C, C5Q) at the last kept epoch, C5Q
/// gone this epoch, so the pin falls to (C1C, C5X); previous values on C1C and C5X.
struct LeavingEdge {
    NetworkEstimator  est;
    std::vector<Pair> keys;
    Harness           h;
    double            v1{0.84}, v2{-1.37};
    LeavingEdge() {
        keys = registerRoster(est, {{"E11", {"C1C", "C5X"}}, {"E12", {"C1C", "C5Q", "C5X"}}});
        h.arms.pair_continuity = true;
        h.code_bias_prev["E|C1C"] = {{"E11", v1}, {"E12", 0.1}};
        h.code_bias_prev["E|C5Q"] = {{"E11", 0.55}, {"E12", 0.2}};
        h.code_bias_prev["E|C5X"] = {{"E11", v2}, {"E12", 0.3}};
        h.code_bias_ifpin_pair = {{"E11", {"C1C", "C5Q"}}, {"E12", {"C1C", "C5Q"}}};
        h.code_bias_sat_seen = {"E11", "E12"};
    }
};

/// The entering edge of the card: E11 pinned on (C1C, C5X) at the last kept epoch, and
/// C5Q, lower-ordered, registered on it for the first time this epoch, so the selection
/// names (C1C, C5Q).  E12 keeps (C1C, C5Q) and is the C5Q channel's veteran, so no channel
/// level re-seeds.  Pair continuity armed, the clock-disc scratch recorded.
struct EnteringEdge {
    NetworkEstimator  est;
    std::vector<Pair> keys;
    Harness           h;
    double            v1{0.84}, v2{-1.37};
    EnteringEdge() {
        keys = registerRoster(est, {{"E11", {"C1C", "C5Q", "C5X"}}, {"E12", {"C1C", "C5Q"}}});
        h.arms.pair_continuity = true;
        h.emit_clock_disc = true;
        h.code_bias_prev["E|C1C"] = {{"E11", v1}, {"E12", 0.1}};
        h.code_bias_prev["E|C5Q"] = {{"E12", 0.2}};   // no E11 value: C5Q enters on E11
        h.code_bias_prev["E|C5X"] = {{"E11", v2}};
        h.code_bias_ifpin_pair = {{"E11", {"C1C", "C5X"}}, {"E12", {"C1C", "C5Q"}}};
        h.code_bias_sat_seen = {"E11", "E12"};
    }
};

/// A minimal loadable network config with @p extra spliced into the network block.
std::string networkConfig(const std::string& extra) {
    return std::string(
        "{\n"
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"trace_events\": true,\n")
        + extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load @p extra into @p cfg through a temporary file.
bool loadWith(const std::string& extra, GnssConfig& cfg, LogManager& log,
              const std::string& tag) {
    const fs::path p = fs::temp_directory_path() / ("gfc_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << networkConfig(extra);
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// The time link and the satellite code bias the arms refine, plus both arms.
const char* kArmedEffective =
    "    \"estimate_sat_code_bias\": true,\n"
    "    \"code_datum_time_linked\": true,\n"
    "    \"code_datum_pair_continuity\": true,\n"
    "    \"code_datum_family_difference\": true,\n";

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("gfc_test_null", sink);
}

std::shared_ptr<spdlog::logger> captureLogger(std::ostringstream& oss) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    return std::make_shared<spdlog::logger>("gfc_test_capture", sink);
}

/// Everything one networkSolPrep call needs (the emit-site harness of
/// test_network_solprep_disc.cpp): a four-satellite clock-only shared estimate
/// (G01..G04), the clock-disc channel live, and one warm-up epoch emitted.
struct EmitHarness {
    LogManager   logman;
    GnssConfig   cfg;
    fs::path     trace_dir;
    std::unique_ptr<NetworkTestRun> fx;   ///< The run networkSolPrep's epoch form takes.
    NetworkRunContext*  ctx;              ///< fx->ctx()
    NetworkRunTraces*   traces;           ///< fx->traces(), opened in trace_dir
    EpochEvents         ev;               ///< The events the seam reads (copied into the epoch).
    NetworkSetupResult& setup;            ///< fx->setup()
    NetworkEstimator&   est;              ///< fx->est()
    Eigen::VectorXd     x_s;
    NetworkSbasis::Plan plan;
    SyncedEpoch         se;

    std::unique_ptr<NetworkTestRun> makeRun(const std::string& tag) {
        EXPECT_TRUE(loadWith("", cfg, logman, "emit_" + tag));
        trace_dir = fs::temp_directory_path() / ("gfc_emit_traces_" + tag);
        fs::create_directories(trace_dir);
        return std::make_unique<NetworkTestRun>(cfg, nullLogger(), trace_dir);
    }

    explicit EmitHarness(const std::string& tag)
        : fx(makeRun(tag)), ctx(&fx->ctx()), traces(&fx->traces()), setup(fx->setup()),
          est(fx->est()) {
        traces->openAll(cfg, cfg.getNetworkConfig(), cfg.getGlobalVtecShConfig(),
                        /*sh_model=*/nullptr, /*cl_trace_armed=*/false, nullLogger());
        setup.emitter.emplace(0.05, /*emit_clock_disc=*/true);
        for (const char* s : {"G01", "G02", "G03", "G04"}) est.registerClock(s);
        x_s = Eigen::VectorXd::Zero(est.numShared());
        se.time = GnssTime{2313, 259200.0};
        const NetworkSolutionEpoch warm =
            est.toSolutionEpoch(x_s, GnssTime{2313, 259170.0}, /*iod_ssr=*/0);
        setup.emitter->emitToStore(warm, setup.network_store);
    }

    ~EmitHarness() {
        std::error_code ec;
        traces->closeAll();
        fs::remove_all(trace_dir, ec);
    }

    NetworkSolPrepResult run() {
        // The solve's hand-off as the solve glue leaves it on a plain epoch: the datum
        // unchanged, no sigma0 adaptation, no per-constellation publication (the defaults).
        NetworkEpoch& e = fx->newEpoch(se.time);
        e.solve.x_s = x_s;
        e.solve.plan = plan;
        e.clock.pl_now_s = 1000.0;
        e.events = ev;
        return networkSolPrep(fx->run(), e, e.events);
    }

    /// Destroy the traces unit (flush + close) and return the events CSV body.
    std::string eventsCsv() {
        traces->closeAll();
        std::ifstream f(trace_dir / "network_events.csv");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /// A pair change of @p sat from @p from to @p to with outcome @p o, selected
    /// this epoch as @p to and last emitted as @p emitted.
    void change(const std::string& sat, const Pair& emitted, const Pair& from, const Pair& to,
                PinOut o = PinOut::Continued) {
        net::putSat(ctx->entities, sat, &net::SatelliteState::last_if_pin_sel) = emitted;
        ev.code_datum.this_epoch_if_pin_sel[sat] = to;
        ev.code_datum.this_epoch_if_pin_changes[sat] = NetworkSbasis::PinPairChange{from, to, o};
    }
};

}  // namespace

// ===========================================================================
// (a) The default path
// ===========================================================================

// Arms off: the two-family roster gets the pre-card rows.  Every channel keeps its own
// level row, and the combined channel's row is a plain +1 over its satellites (here time
// linked on veterans, so the row spans all four C1X satellites with RHS Sum b_prev).
// Arms on without the time link, and the default argument, give the same output.
TEST(CodeDatumFamilyTest, ArmsOffLeaveTheRowsUnchanged) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);

    Harness off;
    allVeterans(off);
    // A pair change with previous values on the new pair, to show that the off path still
    // re-seeds it: E03 was last pinned on (C1X, C5X) and is pinned on (C1C, C5Q) now.
    off.code_bias_ifpin_pair["E03"] = {"C1X", "C5X"};
    const auto r_off = off.build(est, keys);

    // The combined channel's level row: +1 on each C1X column, nothing elsewhere.
    const Eigen::Index row = levelRowOn(r_off, est.codeBiasColumn("E04", "C1X"));
    ASSERT_GE(row, 0);
    double rhs = 0.0;
    for (const std::string sat : {"E01", "E02", "E03", "E04"}) {
        EXPECT_DOUBLE_EQ(r_off.H(row, est.codeBiasColumn(sat, "C1X")), 1.0) << sat;
        rhs += off.code_bias_prev["E|C1X"][sat];
    }
    EXPECT_EQ(r_off.H.row(row).cwiseAbs().sum(), 4.0);
    EXPECT_NEAR(r_off.z(row), rhs, 1e-12);
    // The pair change re-seeds: E03's IF row targets 0 and E03 is marked.
    const Eigen::Index if03 = ifPinRow(r_off, est.codeBiasColumn("E03", "C1C"));
    ASSERT_GE(if03, 0);
    EXPECT_DOUBLE_EQ(r_off.z(if03), 0.0);
    EXPECT_TRUE(off.cb_reseed.count("E03"));
    EXPECT_EQ(r_off.n_s2z_channels, 4);

    // Arms on, time link off: the arms are not read.
    Harness untimed = off;
    untimed.cb_time_linked = false;
    Harness untimed_on = untimed;
    untimed_on.arms = {true, true};
    const auto r_u  = untimed.build(est, keys);
    const auto r_uo = untimed_on.build(est, keys);
    expectSameOutput(r_u, untimed, r_uo, untimed_on);

    // The default argument and an explicit {false, false} are the same call.
    Harness explicit_off = off;
    explicit_off.arms = {false, false};
    const auto r_eo = explicit_off.build(est, keys);
    expectSameOutput(r_off, off, r_eo, explicit_off);
}

// Arms on, on a roster with nothing to act on (one family, no pair change): the output is
// the arms-off output.
TEST(CodeDatumFamilyTest, ArmsOnWithNothingToActOnChangeNothing) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E01", {"C1C", "C5Q"}}, {"E02", {"C1C", "C5Q"}},
                                           {"E03", {"C1C", "C5Q"}}});
    Harness off;
    off.code_bias_prev["E|C1C"] = {{"E01", 0.2}, {"E02", -0.4}, {"E03", 0.9}};
    off.code_bias_prev["E|C5Q"] = {{"E01", 1.2}, {"E02", 0.6}, {"E03", -0.3}};
    off.code_bias_ifpin_pair = {{"E01", {"C1C", "C5Q"}}, {"E02", {"C1C", "C5Q"}},
                                {"E03", {"C1C", "C5Q"}}};
    off.code_bias_sat_seen = {"E01", "E02", "E03"};
    Harness on = off;
    on.arms = {true, true};
    const auto r_off = off.build(est, keys);
    const auto r_on  = on.build(est, keys);
    expectSameOutput(r_off, off, r_on, on);
}

// ===========================================================================
// (b) Pair continuity
// ===========================================================================

// A leaving edge with previous values on both new signals: the target is
// a1 v(C1C) + a2 v(C5X), the satellite is not marked for a re-seed, and the change is
// returned as continued from the last kept pair.  The builder emits no event (the emit
// site counts the change once per kept epoch).
TEST(CodeDatumFamilyTest, PairChangeWithPreviousValuesContinuesTheClock) {
    LeavingEdge e;
    const auto rows = e.h.build(e.est, e.keys);

    const Eigen::Index r11 = ifPinRow(rows, e.est.codeBiasColumn("E11", "C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, e.est.codeBiasColumn("E11", "C5X")), a2(), 1e-12);
    EXPECT_NEAR(rows.z(r11), a1() * e.v1 + a2() * e.v2, 1e-12);
    EXPECT_FALSE(e.h.cb_reseed.count("E11"));
    ASSERT_EQ(rows.if_pin_pair_changes.size(), 1u);
    const NetworkSbasis::PinPairChange& c = rows.if_pin_pair_changes.at("E11");
    EXPECT_EQ(c.from, (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(c.to, (Pair{"C1C", "C5X"}));
    EXPECT_EQ(c.outcome, PinOut::Continued);
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5X"}));

    // The unchanged satellite is carried as before and is not a pair change.
    const Eigen::Index r12 = ifPinRow(rows, e.est.codeBiasColumn("E12", "C1C"));
    ASSERT_GE(r12, 0);
    EXPECT_NEAR(rows.z(r12), a1() * 0.1 + a2() * 0.2, 1e-12);

    // The same epoch with the arm off re-seeds E11 at 0 and marks it.
    Harness off = e.h;
    off.arms = {};
    const auto r_off = off.build(e.est, e.keys);
    EXPECT_DOUBLE_EQ(r_off.z(ifPinRow(r_off, e.est.codeBiasColumn("E11", "C1C"))), 0.0);
    EXPECT_TRUE(off.cb_reseed.count("E11"));
    EXPECT_TRUE(r_off.if_pin_pair_changes.empty());
}

// Without a previous solved value on one of the new pair's signals the re-seed applies.
// Under the arm this needs a change no hold covers; here a held signal leaves in the same
// epoch a lower-ordered one enters: C5X leaves and C5Q, never registered before, takes its
// place, so the fallback pair (C1C, C5Q) has no previous C5Q value.  (The other cases:
// NoHoldWithoutPreviousValuesOnTheHeldPair, GroupDelayRefusesAHoldThatWouldMisanchor.)  A
// return after a gap (no pair at the last kept epoch) is not a pair change.
TEST(CodeDatumFamilyTest, PairChangeWithoutPreviousValuesReseeds) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E11", {"C1C", "C5Q"}}});
    Harness h;
    h.arms.pair_continuity = true;
    h.code_bias_prev["E|C1C"] = {{"E11", 0.84}};
    h.code_bias_prev["E|C5X"] = {{"E11", 0.55}};   // no C5Q value: the pilot channel is new
    h.code_bias_ifpin_pair = {{"E11", {"C1C", "C5X"}}};
    h.code_bias_sat_seen = {"E11"};
    const auto rows = h.build(est, keys);
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());

    const Eigen::Index r11 = ifPinRow(rows, est.codeBiasColumn("E11", "C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_DOUBLE_EQ(rows.z(r11), 0.0);
    EXPECT_TRUE(h.cb_reseed.count("E11"));
    ASSERT_EQ(rows.if_pin_pair_changes.count("E11"), 1u);
    EXPECT_EQ(rows.if_pin_pair_changes.at("E11").outcome, PinOut::Reseeded);

    Harness ret = h;
    ret.code_bias_ifpin_pair.clear();
    const auto r_ret = ret.build(est, keys);
    EXPECT_TRUE(r_ret.if_pin_pair_changes.empty());
    EXPECT_TRUE(ret.cb_reseed.count("E11"));
}

// A continued pin holds the clock only when no other row on the satellite's columns
// re-asserts an absolute target.  The blocking posture: the satellite GF datum without
// its time link.  Under the time-linked satellite GF datum (the field posture) the arm
// acts.
TEST(CodeDatumFamilyTest, ContinuityIsInertWhereARowReassertsAnAbsoluteTarget) {
    using NS = NetworkSbasis;
    EXPECT_EQ(NS::pinPairContinuityBlocker(NetworkSatGfDatum::NoneMode, false), nullptr);
    EXPECT_EQ(NS::pinPairContinuityBlocker(NetworkSatGfDatum::BroadcastTgd, true), nullptr);
    EXPECT_NE(NS::pinPairContinuityBlocker(NetworkSatGfDatum::BroadcastTgd, false), nullptr);
    EXPECT_NE(NS::pinPairContinuityBlocker(NetworkSatGfDatum::Zero, false), nullptr);

    // Absolute GF rows: the leaving edge re-seeds exactly as with the arm off.
    LeavingEdge abs_gf;
    abs_gf.h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    abs_gf.h.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    Harness abs_off = abs_gf.h;
    abs_off.arms = {};
    const auto r_abs = abs_gf.h.build(abs_gf.est, abs_gf.keys);
    const auto r_abs_off = abs_off.build(abs_gf.est, abs_gf.keys);
    expectSameOutput(r_abs_off, abs_off, r_abs, abs_gf.h);
    EXPECT_TRUE(abs_gf.h.cb_reseed.count("E11"));

    // Time-linked GF rows: continued.
    LeavingEdge tl_gf;
    tl_gf.h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    tl_gf.h.sat_gf_datum_time_linked = true;
    tl_gf.h.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    const auto r_tl = tl_gf.h.build(tl_gf.est, tl_gf.keys);
    ASSERT_EQ(r_tl.if_pin_pair_changes.count("E11"), 1u);
    EXPECT_EQ(r_tl.if_pin_pair_changes.at("E11").outcome, PinOut::Continued);
    EXPECT_FALSE(tl_gf.h.cb_reseed.count("E11"));
}

// A continued satellite that a channel-level re-seed marks in the same call had its datum
// moved by that re-seed: its change is returned as level-re-seeded (declared at the emit
// site), and the mark stands.  Here E11 gains C7Q, a channel with no veteran at all.
TEST(CodeDatumFamilyTest, ContinuedSatelliteMarkedByALevelReseedIsNotContinued) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E11", {"C1C", "C5X", "C7Q"}}});
    Harness h;
    h.arms.pair_continuity = true;
    h.code_bias_prev["E|C1C"] = {{"E11", 0.84}};
    h.code_bias_prev["E|C5Q"] = {{"E11", 0.55}};
    h.code_bias_prev["E|C5X"] = {{"E11", -1.37}};
    h.code_bias_ifpin_pair = {{"E11", {"C1C", "C5Q"}}};
    h.code_bias_sat_seen = {"E11"};
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_s2z_reseed_rows, 1);   // E|C7Q, no veteran
    EXPECT_TRUE(h.cb_reseed.count("E11"));
    ASSERT_EQ(rows.if_pin_pair_changes.count("E11"), 1u);
    EXPECT_EQ(rows.if_pin_pair_changes.at("E11").outcome, PinOut::LevelReseeded);
    // The pin's own target is still the continued one.
    const Eigen::Index r11 = ifPinRow(rows, est.codeBiasColumn("E11", "C1C"));
    EXPECT_NEAR(rows.z(r11), a1() * 0.84 + a2() * -1.37, 1e-12);
}

// (a) An entering change: the pin holds.  C5Q reaches E11 beside its pinned (C1C, C5X),
// so the selection names (C1C, C5Q), which cannot be continued (C5Q has no previous
// value), while both held signals are registered and have previous values: the pin stays
// on (C1C, C5X) and carries that pair's own previous combination, as for an unchanged
// pair.  Nothing is marked, no pair change is recorded (so the emit site has no
// re-selection to declare, its scratch holding the held pair), and the hold is returned
// with the selection it overrode.  Every pair-keyed record and row follows the held
// pair: the geometry-free pair row is on (C1C, C5X) and the entering C5Q gets its own
// extra row, anchored at the group delay because (C1C, C5X) is a pair the Galileo group
// delay describes (so broadcast_tgd does not refuse the hold).
TEST(CodeDatumFamilyTest, EnteringSignalKeepsTheHeldPair) {
    EnteringEdge e;
    const auto rows = e.h.build(e.est, e.keys);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };

    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C1C")), a1(), 1e-12);
    EXPECT_NEAR(rows.H(r11, col("C5X")), a2(), 1e-12);
    EXPECT_DOUBLE_EQ(rows.H(r11, col("C5Q")), 0.0);
    EXPECT_NEAR(rows.z(r11), a1() * e.v1 + a2() * e.v2, 1e-12);
    EXPECT_TRUE(e.h.cb_reseed.empty());
    EXPECT_TRUE(rows.if_pin_pair_changes.empty());
    ASSERT_EQ(rows.if_pin_pair_holds.size(), 1u);
    EXPECT_EQ(rows.if_pin_pair_holds.at("E11").selected, (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(rows.if_pin_pair_holds.at("E11").held, (Pair{"C1C", "C5X"}));
    EXPECT_FALSE(rows.if_pin_pair_holds.at("E11").reseeded);
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5X"}));
    EXPECT_EQ(e.h.if_pin_sel.at("E11"), (Pair{"C1C", "C5X"}));
    EXPECT_EQ(e.h.if_pin_real.at("E11"), (Pair{"C1C", "C5X"}));
    EXPECT_EQ(e.h.cb_ifpin.at("E12"), (Pair{"C1C", "C5Q"}));   // its selection is its pair

    // The geometry-free datum (time-linked, the field posture) follows the pinned pair.
    Harness g = e.h;
    g.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    g.sat_gf_datum_time_linked = true;
    g.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    const auto rg = g.build(e.est, e.keys);
    ASSERT_EQ(rg.if_pin_pair_holds.count("E11"), 1u);
    const double mu5 = (kE1 * kE1) / (kE5a * kE5a);
    int pair_rows = 0, extra_rows = 0;
    for (Eigen::Index r = 0; r < rg.H.rows(); ++r) {
        if (std::abs(rg.H(r, col("C1C"))) < 1e-12) continue;   // another satellite's row
        const ContributionKind k = rg.row_kinds[static_cast<std::size_t>(r)];
        if (k == ContributionKind::CbSatGfPair) {
            ++pair_rows;
            EXPECT_NEAR(rg.H(r, col("C5X")), 1.0 / (mu5 - 1.0), 1e-12);
            EXPECT_DOUBLE_EQ(rg.H(r, col("C5Q")), 0.0);
            EXPECT_NEAR(rg.z(r), (e.v2 - e.v1) / (mu5 - 1.0), 1e-12);   // carried
        } else if (k == ContributionKind::CbExtraGf) {
            ++extra_rows;
            EXPECT_NEAR(rg.H(r, col("C5Q")), 1.0 / (mu5 - 1.0), 1e-12);
            EXPECT_DOUBLE_EQ(rg.H(r, col("C5X")), 0.0);
            EXPECT_NEAR(rg.z(r), 0.4, 1e-12);   // a first sighting: the pair's own target
        }
    }
    EXPECT_EQ(pair_rows, 1);
    EXPECT_EQ(extra_rows, 1);
    EXPECT_TRUE(g.cb_reseed.empty());
}

// (b) The hold breaks.  After the held epoch of (a) is kept (its pin pair and every
// registered signal's solved value harvested, C5Q's included), C5X leaves: the selection
// (C1C, C5Q) stands, and as C5Q has a previous value from the held epoch, the leaving
// change continues from the held pair (target the new pair's previous combination, no
// mark).  Had the entering signal left instead, the selection would name the held pair
// again: neither a hold nor a change.  (With both still registered the hold is released
// the same way: HoldIsReleasedAtTheNextKeptEpoch.)
TEST(CodeDatumFamilyTest, BrokenHoldFallsBackToTheSelectionAndContinues) {
    EnteringEdge e;
    const auto held = e.h.build(e.est, e.keys);
    ASSERT_EQ(held.if_pin_pair_holds.count("E11"), 1u);

    // The harvest of the held epoch.
    e.h.code_bias_ifpin_pair = e.h.cb_ifpin;
    e.h.code_bias_prev["E|C1C"]["E11"] = 0.81;
    e.h.code_bias_prev["E|C5Q"]["E11"] = 0.62;
    e.h.code_bias_prev["E|C5X"]["E11"] = -1.35;

    const std::vector<Pair> keys_left = {{"E11", "C1C"}, {"E11", "C5Q"},
                                         {"E12", "C1C"}, {"E12", "C5Q"}};
    const auto rows = e.h.build(e.est, keys_left);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());
    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C5Q")), a2(), 1e-12);
    EXPECT_DOUBLE_EQ(rows.H(r11, col("C5X")), 0.0);
    EXPECT_NEAR(rows.z(r11), a1() * 0.81 + a2() * 0.62, 1e-12);
    EXPECT_TRUE(e.h.cb_reseed.empty());
    ASSERT_EQ(rows.if_pin_pair_changes.size(), 1u);
    const NetworkSbasis::PinPairChange& c = rows.if_pin_pair_changes.at("E11");
    EXPECT_EQ(c.from, (Pair{"C1C", "C5X"}));
    EXPECT_EQ(c.to, (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(c.outcome, PinOut::Continued);
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(e.h.if_pin_sel.at("E11"), (Pair{"C1C", "C5Q"}));

    // The entering signal leaves instead.
    const std::vector<Pair> keys_back = {{"E11", "C1C"}, {"E11", "C5X"},
                                         {"E12", "C1C"}, {"E12", "C5Q"}};
    const auto back = e.h.build(e.est, keys_back);
    EXPECT_TRUE(back.if_pin_pair_holds.empty());
    EXPECT_TRUE(back.if_pin_pair_changes.empty());
    const Eigen::Index rb = ifPinRow(back, col("C1C"));
    ASSERT_GE(rb, 0);
    EXPECT_NEAR(back.z(rb), a1() * 0.81 + a2() * -1.35, 1e-12);
    EXPECT_TRUE(e.h.cb_reseed.empty());
}

// The hold is released at the next kept epoch.  After the held epoch of (a) is kept, both
// held signals are still registered, but the selected pair (C1C, C5Q) now has previous
// values on both signals (C5Q was solved while held), so there is no hold: the pin moves to
// the selection by a continued change (target the new pair's previous combination, no
// mark).  The hold lasted one kept epoch, and the pin is the selection's again, so a
// constellation's satellites do not stay split across pin pairs.  Under the time-linked
// broadcast_tgd datum the released pair's geometry-free row carries the values of the
// held epoch.
TEST(CodeDatumFamilyTest, HoldIsReleasedAtTheNextKeptEpoch) {
    EnteringEdge e;
    e.h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    e.h.sat_gf_datum_time_linked = true;
    e.h.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    const auto held = e.h.build(e.est, e.keys);
    ASSERT_EQ(held.if_pin_pair_holds.count("E11"), 1u);

    // The harvest of the held epoch.
    e.h.code_bias_ifpin_pair = e.h.cb_ifpin;
    e.h.code_bias_prev["E|C1C"]["E11"] = 0.81;
    e.h.code_bias_prev["E|C5Q"]["E11"] = 0.62;
    e.h.code_bias_prev["E|C5X"]["E11"] = -1.35;

    const auto rows = e.h.build(e.est, e.keys);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());
    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C5Q")), a2(), 1e-12);
    EXPECT_DOUBLE_EQ(rows.H(r11, col("C5X")), 0.0);
    EXPECT_NEAR(rows.z(r11), a1() * 0.81 + a2() * 0.62, 1e-12);
    EXPECT_TRUE(e.h.cb_reseed.empty());
    ASSERT_EQ(rows.if_pin_pair_changes.size(), 1u);
    const NetworkSbasis::PinPairChange& c = rows.if_pin_pair_changes.at("E11");
    EXPECT_EQ(c.from, (Pair{"C1C", "C5X"}));
    EXPECT_EQ(c.to, (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(c.outcome, PinOut::Continued);
    EXPECT_FALSE(c.hold_refused_gd);
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(e.h.if_pin_sel.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(e.h.if_pin_real.at("E11"), e.h.if_pin_real.at("E12"));   // unanimous again

    const double mu5 = (kE1 * kE1) / (kE5a * kE5a);
    int pair_rows = 0;
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r) {
        if (rows.row_kinds[static_cast<std::size_t>(r)] != ContributionKind::CbSatGfPair
            || std::abs(rows.H(r, col("C1C"))) < 1e-12)
            continue;
        ++pair_rows;
        EXPECT_NEAR(rows.H(r, col("C5Q")), 1.0 / (mu5 - 1.0), 1e-12);
        EXPECT_NEAR(rows.z(r), (0.62 - 0.81) / (mu5 - 1.0), 1e-12);   // carried
    }
    EXPECT_EQ(pair_rows, 1);
}

// No hold where it buys nothing.  A hard refloat at the last harvest left E11 without any
// previous value but kept its pin pair, so a held pin would re-seed as the selection's
// does.  The selection stands, the change is re-seeded and marked as without the hold, and
// there is no hold record (the run end cannot count a re-seeded satellite as held).
TEST(CodeDatumFamilyTest, NoHoldWithoutPreviousValuesOnTheHeldPair) {
    EnteringEdge e;
    for (auto& [chan, per_sat] : e.h.code_bias_prev) per_sat.erase("E11");
    const auto rows = e.h.build(e.est, e.keys);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());
    ASSERT_EQ(rows.if_pin_pair_changes.count("E11"), 1u);
    EXPECT_EQ(rows.if_pin_pair_changes.at("E11").outcome, PinOut::Reseeded);
    EXPECT_FALSE(rows.if_pin_pair_changes.at("E11").hold_refused_gd);
    EXPECT_TRUE(e.h.cb_reseed.count("E11"));
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C5Q")), a2(), 1e-12);
    EXPECT_DOUBLE_EQ(rows.z(r11), 0.0);
}

// Under broadcast_tgd a hold is refused where the group delay describes the selected pair
// and not the held one.  E11 is pinned on (C1C, C7Q) (E1/E5b, which the Galileo group
// delay BGD(E1, E5a) does not describe) and C5Q enters.  Held, C5Q's first-sighting row
// would anchor at the held pair's zero and the release would carry that zero onto
// (C1C, C5Q).  Refused, the selection is pinned at once, re-seeded and marked, its
// geometry-free pair row anchors at the group delay, and the change records why it was not
// held.  Under the zero datum the same edge is held.
TEST(CodeDatumFamilyTest, GroupDelayRefusesAHoldThatWouldMisanchor) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E11", {"C1C", "C5Q", "C7Q"}},
                                           {"E12", {"C1C", "C5Q"}}});
    Harness h;
    h.arms.pair_continuity = true;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_datum_time_linked = true;
    h.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    h.code_bias_prev["E|C1C"] = {{"E11", 0.84}, {"E12", 0.1}};
    h.code_bias_prev["E|C5Q"] = {{"E12", 0.2}};   // no E11 value: C5Q enters on E11
    h.code_bias_prev["E|C7Q"] = {{"E11", -1.1}};
    h.code_bias_ifpin_pair = {{"E11", {"C1C", "C7Q"}}, {"E12", {"C1C", "C5Q"}}};
    h.code_bias_sat_seen = {"E11", "E12"};
    Harness zero = h;
    zero.sat_gf_datum = NetworkSatGfDatum::Zero;
    zero.sat_gf_target_m.clear();

    const auto rows = h.build(est, keys);
    const auto col = [&](const std::string& sig) { return est.codeBiasColumn("E11", sig); };
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());
    ASSERT_EQ(rows.if_pin_pair_changes.count("E11"), 1u);
    const NetworkSbasis::PinPairChange& c = rows.if_pin_pair_changes.at("E11");
    EXPECT_EQ(c.from, (Pair{"C1C", "C7Q"}));
    EXPECT_EQ(c.to, (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(c.outcome, PinOut::Reseeded);
    EXPECT_TRUE(c.hold_refused_gd);
    EXPECT_TRUE(h.cb_reseed.count("E11"));
    EXPECT_EQ(h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    int pair_rows = 0;
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r) {
        if (rows.row_kinds[static_cast<std::size_t>(r)] != ContributionKind::CbSatGfPair
            || std::abs(rows.H(r, col("C1C"))) < 1e-12)
            continue;
        ++pair_rows;
        EXPECT_GT(std::abs(rows.H(r, col("C5Q"))), 1e-12);
        EXPECT_NEAR(rows.z(r), 0.4, 1e-12);   // anchored at the group delay
    }
    EXPECT_EQ(pair_rows, 1);

    const auto rz = zero.build(est, keys);
    ASSERT_EQ(rz.if_pin_pair_holds.count("E11"), 1u);
    EXPECT_EQ(rz.if_pin_pair_holds.at("E11").held, (Pair{"C1C", "C7Q"}));
    EXPECT_TRUE(rz.if_pin_pair_changes.empty());
    EXPECT_TRUE(zero.cb_reseed.empty());
}

// A held satellite that a channel-level re-seed marks anyway is flagged.  The entering
// C5Q channel has no veteran (E12's C5Q value is missing too), so its sum-to-zero row
// re-seeds and marks every present satellite seen before, held E11 included.  The hold
// still keeps E11's pin on its own previous combination, and the record says it was marked,
// so the run end does not count it as a clean hold.
TEST(CodeDatumFamilyTest, HeldSatelliteMarkedByAChannelReseedIsFlagged) {
    EnteringEdge e;
    e.h.code_bias_prev.erase("E|C5Q");
    const auto rows = e.h.build(e.est, e.keys);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };
    ASSERT_EQ(rows.if_pin_pair_holds.count("E11"), 1u);
    EXPECT_TRUE(rows.if_pin_pair_holds.at("E11").reseeded);
    EXPECT_GE(rows.n_s2z_reseed_rows, 1);
    EXPECT_TRUE(e.h.cb_reseed.count("E11"));
    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C5X")), a2(), 1e-12);
    EXPECT_NEAR(rows.z(r11), a1() * e.v1 + a2() * e.v2, 1e-12);

    // The entering edge of (a), with the C5Q veteran: not flagged.
    EnteringEdge clean;
    const auto rc = clean.h.build(clean.est, clean.keys);
    ASSERT_EQ(rc.if_pin_pair_holds.count("E11"), 1u);
    EXPECT_FALSE(rc.if_pin_pair_holds.at("E11").reseeded);
}

// (c) Arm off: the entering change re-selects the pin exactly as before the card, onto
// (C1C, C5Q) at a re-seed target with E11 marked, and nothing is recorded.  The
// family-difference arm alone does not hold the pin either.
TEST(CodeDatumFamilyTest, ArmOffKeepsTheSelection) {
    EnteringEdge e;
    e.h.arms = {};
    const auto rows = e.h.build(e.est, e.keys);
    const auto col = [&](const std::string& sig) { return e.est.codeBiasColumn("E11", sig); };
    const Eigen::Index r11 = ifPinRow(rows, col("C1C"));
    ASSERT_GE(r11, 0);
    EXPECT_NEAR(rows.H(r11, col("C5Q")), a2(), 1e-12);
    EXPECT_DOUBLE_EQ(rows.H(r11, col("C5X")), 0.0);
    EXPECT_DOUBLE_EQ(rows.z(r11), 0.0);
    EXPECT_TRUE(e.h.cb_reseed.count("E11"));
    EXPECT_TRUE(rows.if_pin_pair_holds.empty());
    EXPECT_TRUE(rows.if_pin_pair_changes.empty());
    EXPECT_EQ(e.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_EQ(e.h.if_pin_sel.at("E11"), (Pair{"C1C", "C5Q"}));

    Harness fd = e.h;
    fd.arms.family_difference = true;
    const auto r_fd = fd.build(e.est, e.keys);
    EXPECT_TRUE(r_fd.if_pin_pair_holds.empty());
    EXPECT_EQ(fd.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_TRUE(fd.cb_reseed.count("E11"));
}

// (d) Under a blocking posture (a row re-asserting an absolute target) the arm is not
// read: the pin follows the selection, and the output is the arm-off output.
TEST(CodeDatumFamilyTest, BlockedArmKeepsTheSelection) {
    EnteringEdge on;
    on.h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;   // without its time link
    on.h.sat_gf_target_m = {{"E11", 0.4}, {"E12", -0.2}};
    ASSERT_NE(NetworkSbasis::pinPairContinuityBlocker(on.h.sat_gf_datum,
                                                      on.h.sat_gf_datum_time_linked),
              nullptr);
    Harness off = on.h;
    off.arms = {};
    const auto r_on  = on.h.build(on.est, on.keys);
    const auto r_off = off.build(on.est, on.keys);
    expectSameOutput(r_off, off, r_on, on.h);
    EXPECT_EQ(on.h.if_pin_sel, off.if_pin_sel);
    EXPECT_EQ(on.h.cb_ifpin.at("E11"), (Pair{"C1C", "C5Q"}));
    EXPECT_TRUE(on.h.cb_reseed.count("E11"));
}

// ===========================================================================
// (c) The family-difference row
// ===========================================================================

// With veterans: the combined channels' level rows become Sum (d_X - d_P) over the
// veterans among the satellites tracking both families, +1 on the X column and -1 on the
// partner's, RHS the sum of their previous differences.  A common satellite without a
// previous value on both columns, and a satellite of one family only, are not in the row.
// No combined channel keeps a sum-to-zero row; the primary channels keep theirs.
TEST(CodeDatumFamilyTest, FamilyDifferenceRowOnVeterans) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1X"].erase("E03");   // E03 is common but not a band-1 veteran
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);

    const auto col = [&](const std::string& sat, const std::string& sig) {
        return est.codeBiasColumn(sat, sig);
    };
    const Eigen::Index r1 = levelRowOn(rows, col("E01", "C1X"));
    ASSERT_GE(r1, 0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E01", "C1X")), 1.0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E01", "C1C")), -1.0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E02", "C1X")), 1.0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E02", "C1C")), -1.0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E03", "C1X")), 0.0);   // not a veteran
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E03", "C1C")), 0.0);
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E04", "C1X")), 0.0);   // combined family only
    EXPECT_DOUBLE_EQ(rows.H(r1, col("E05", "C1C")), 0.0);   // pilot family only
    EXPECT_EQ(rows.H.row(r1).cwiseAbs().sum(), 4.0);
    const double rhs1 = (h.code_bias_prev["E|C1X"]["E01"] - h.code_bias_prev["E|C1C"]["E01"])
                      + (h.code_bias_prev["E|C1X"]["E02"] - h.code_bias_prev["E|C1C"]["E02"]);
    EXPECT_NEAR(rows.z(r1), rhs1, 1e-12);

    // Band 5: all three common satellites are veterans.
    const Eigen::Index r5 = levelRowOn(rows, col("E01", "C5X"));
    ASSERT_GE(r5, 0);
    double rhs5 = 0.0;
    for (const std::string sat : {"E01", "E02", "E03"}) {
        EXPECT_DOUBLE_EQ(rows.H(r5, col(sat, "C5X")), 1.0) << sat;
        EXPECT_DOUBLE_EQ(rows.H(r5, col(sat, "C5Q")), -1.0) << sat;
        rhs5 += h.code_bias_prev["E|C5X"][sat] - h.code_bias_prev["E|C5Q"][sat];
    }
    EXPECT_NEAR(rows.z(r5), rhs5, 1e-12);

    // No row spans E04's X columns, and the primary channels keep their sum-to-zero rows.
    EXPECT_EQ(levelRowOn(rows, col("E04", "C1X")), -1);
    EXPECT_EQ(levelRowOn(rows, col("E04", "C5X")), -1);
    const Eigen::Index rc = levelRowOn(rows, col("E05", "C1C"));
    ASSERT_GE(rc, 0);
    EXPECT_DOUBLE_EQ(rows.H(rc, col("E05", "C1C")), 1.0);
    // One row for one row: the row count and kinds are those of the arms-off build.
    Harness off = h;
    off.arms = {};
    const auto r_off = off.build(est, keys);
    EXPECT_EQ(rows.H.rows(), r_off.H.rows());
    EXPECT_EQ(rows.row_kinds, r_off.row_kinds);

    ASSERT_EQ(rows.family_rows.size(), 2u);
    const auto& f1 = rows.family_rows.at("E|C1X");
    EXPECT_EQ(f1.outcome, Outcome::FamilyVeteran);
    EXPECT_EQ(f1.partner, "C1C");
    EXPECT_EQ(f1.channel.sys, System::Galileo);
    EXPECT_EQ(f1.channel.digit, '1');
    EXPECT_EQ(rows.family_rows.at("E|C5X").outcome, Outcome::FamilyVeteran);
    EXPECT_EQ(rows.family_rows.at("E|C5X").partner, "C5Q");
    EXPECT_TRUE(h.cb_reseed.empty());
}

// No veteran anywhere on band 1 (neither the family row nor the channel's sum-to-zero):
// the row spans every common satellite at RHS 0 and marks the combined channel's
// satellites seen before (E04 included: its X columns are referenced to the re-seeded
// offset too).  First appearances are not marked.
TEST(CodeDatumFamilyTest, FamilyRowWithoutAnyVeteranReseeds) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev.erase("E|C1X");
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);

    const Eigen::Index r1 = levelRowOn(rows, est.codeBiasColumn("E01", "C1X"));
    ASSERT_GE(r1, 0);
    for (const std::string sat : {"E01", "E02", "E03"}) {
        EXPECT_DOUBLE_EQ(rows.H(r1, est.codeBiasColumn(sat, "C1X")), 1.0) << sat;
        EXPECT_DOUBLE_EQ(rows.H(r1, est.codeBiasColumn(sat, "C1C")), -1.0) << sat;
    }
    EXPECT_DOUBLE_EQ(rows.z(r1), 0.0);
    EXPECT_EQ(h.cb_reseed, (std::set<std::string>{"E01", "E02", "E03", "E04"}));
    const auto& f1 = rows.family_rows.at("E|C1X");
    EXPECT_EQ(f1.outcome, Outcome::FamilyReseed);
    EXPECT_EQ(f1.reseed_sats, 4);
    EXPECT_EQ(rows.family_rows.at("E|C5X").outcome, Outcome::FamilyVeteran);
    EXPECT_EQ(rows.n_s2z_reseed_rows, 0);   // the family re-seed is counted as its own

    Harness fresh = h;
    fresh.code_bias_sat_seen.clear();
    fresh.code_bias_ifpin_pair.clear();
    const auto r_fresh = fresh.build(est, keys);
    EXPECT_EQ(r_fresh.family_rows.at("E|C1X").reseed_sats, 0);
}

// The family row has no veteran (no common satellite has a previous C1X value) but the
// channel's sum-to-zero time link has one (E04): the sum-to-zero row carries the level
// for this epoch instead of a re-seed, nothing is marked, and the outcome is "bridged".
TEST(CodeDatumFamilyTest, FamilyRowBridgesThroughTheSumToZeroCarry) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    for (const std::string sat : {"E01", "E02", "E03"}) h.code_bias_prev["E|C1X"].erase(sat);
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    Harness off = h;
    off.arms = {};
    const auto r_off = off.build(est, keys);

    const Eigen::Index r1 = levelRowOn(rows, est.codeBiasColumn("E04", "C1X"));
    const Eigen::Index o1 = levelRowOn(r_off, est.codeBiasColumn("E04", "C1X"));
    ASSERT_GE(r1, 0);
    ASSERT_GE(o1, 0);
    EXPECT_TRUE(rows.H.row(r1) == r_off.H.row(o1));
    EXPECT_DOUBLE_EQ(rows.z(r1), r_off.z(o1));
    EXPECT_EQ(rows.family_rows.at("E|C1X").outcome, Outcome::Bridged);
    EXPECT_EQ(rows.family_rows.at("E|C1X").partner, "C1C");
    EXPECT_TRUE(h.cb_reseed.empty());
}

// No satellite tracks both families, or no same-band primary channel is registered: the
// combined channels keep their sum-to-zero rows, with the reason recorded.
TEST(CodeDatumFamilyTest, NoCommonSatelliteOrNoPartnerKeepsTheSumToZeroRow) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E01", {"C1C", "C5Q"}}, {"E02", {"C1C", "C5Q"}},
                                           {"E03", {"C1X", "C5X"}}, {"E04", {"C1X", "C5X"}}});
    Harness h;
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    Harness off;
    const auto r_off = off.build(est, keys);
    EXPECT_TRUE(rows.H == r_off.H);
    EXPECT_TRUE(rows.z == r_off.z);
    EXPECT_EQ(rows.family_rows.at("E|C1X").outcome, Outcome::NoCommon);
    EXPECT_EQ(rows.family_rows.at("E|C5X").outcome, Outcome::NoCommon);

    NetworkEstimator est2;
    const auto keys2 = registerRoster(est2, {{"E01", {"C1X", "C5Q"}}, {"E02", {"C1X", "C5Q"}}});
    Harness h2;
    h2.arms.family_difference = true;
    const auto rows2 = h2.build(est2, keys2);
    Harness off2;
    const auto r_off2 = off2.build(est2, keys2);
    EXPECT_TRUE(rows2.H == r_off2.H);
    ASSERT_EQ(rows2.family_rows.size(), 1u);
    EXPECT_EQ(rows2.family_rows.at("E|C1X").outcome, Outcome::NoPartner);
    EXPECT_TRUE(rows2.family_rows.at("E|C1X").partner.empty());
}

// The partner is the same-band primary channel with the most satellites in common; a tie
// goes to the lexically first signal.
TEST(CodeDatumFamilyTest, PartnerIsTheBestCoveredSameBandPrimaryChannel) {
    NetworkEstimator est;
    // C1C shares three satellites with C1X, C1B two: C1C is the partner.
    const auto keys = registerRoster(est, {{"E01", {"C1B", "C1C", "C1X", "C5Q"}},
                                           {"E02", {"C1B", "C1C", "C1X", "C5Q"}},
                                           {"E03", {"C1C", "C1X", "C5Q"}}});
    Harness h;
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    const Eigen::Index r = levelRowOn(rows, est.codeBiasColumn("E03", "C1X"));
    ASSERT_GE(r, 0);
    EXPECT_DOUBLE_EQ(rows.H(r, est.codeBiasColumn("E03", "C1C")), -1.0);
    EXPECT_DOUBLE_EQ(rows.H(r, est.codeBiasColumn("E01", "C1B")), 0.0);
    EXPECT_EQ(rows.family_rows.at("E|C1X").partner, "C1C");

    // A tie (two satellites each): C1B, the lexically first.
    NetworkEstimator est2;
    const auto keys2 = registerRoster(est2, {{"E01", {"C1B", "C1X", "C5Q"}},
                                             {"E02", {"C1B", "C1X", "C5Q"}},
                                             {"E03", {"C1C", "C1X", "C5Q"}},
                                             {"E04", {"C1C", "C1X", "C5Q"}}});
    Harness h2;
    h2.arms.family_difference = true;
    const auto rows2 = h2.build(est2, keys2);
    const Eigen::Index r2 = levelRowOn(rows2, est2.codeBiasColumn("E01", "C1X"));
    ASSERT_GE(r2, 0);
    EXPECT_DOUBLE_EQ(rows2.H(r2, est2.codeBiasColumn("E01", "C1B")), -1.0);
    EXPECT_DOUBLE_EQ(rows2.H(r2, est2.codeBiasColumn("E03", "C1X")), 0.0);
    EXPECT_DOUBLE_EQ(rows2.H(r2, est2.codeBiasColumn("E03", "C1C")), 0.0);
}

// Under the field posture's geometry-free datum (broadcast_tgd, time-linked), the channels
// whose every column a geometry-free row pins keep their RF-0d suppression and get no
// record; only the combined band-1 channel, whose C1X columns are same-band extras and
// stay free, has a level row, and the arm turns it into the family-difference row.
TEST(CodeDatumFamilyTest, SuppressedChannelsAreNotTouched) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E01", {"C1C", "C5Q", "C1X", "C5X"}},
                                           {"E02", {"C1C", "C5Q", "C1X", "C5X"}},
                                           {"E03", {"C1C", "C5Q", "C1X", "C5X"}}});
    Harness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_datum_time_linked = true;
    h.sat_gf_target_m = {{"E01", 0.5}, {"E02", -0.3}, {"E03", 0.8}};
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.n_s2z_suppressed, 3);   // C1C, C5Q, C5X
    EXPECT_EQ(rows.family_rows.count("E|C5X"), 0u);
    ASSERT_EQ(rows.family_rows.count("E|C1X"), 1u);
    EXPECT_TRUE(rows.family_rows.at("E|C1X").familyRow());
    const Eigen::Index r1 = levelRowOn(rows, est.codeBiasColumn("E02", "C1X"));
    ASSERT_GE(r1, 0);
    EXPECT_DOUBLE_EQ(rows.H(r1, est.codeBiasColumn("E02", "C1C")), -1.0);
}

// The rank guard.  Triple-frequency posture with the extra band freed where the data
// determine it: satellites A and B carry C7Q and C7X, both pinned by carried
// geometry-free extra rows; F carries C7X alone, free (data-determined).  E|C7Q is fully
// pinned (suppressed); E|C7X survives because of F.  The family row would span A and B
// only, whose C7X columns are pinned, so it would add no rank: the sum-to-zero row stays.
TEST(CodeDatumFamilyTest, DependentFamilyRowKeepsTheSumToZeroRow) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"E01", {"C1C", "C5Q", "C7Q", "C7X"}},
                                           {"E02", {"C1C", "C5Q", "C7Q", "C7X"}},
                                           {"E03", {"C1C", "C5Q", "C7X"}}});
    Harness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_datum_time_linked = true;
    h.sat_gf_target_m = {{"E01", 0.5}, {"E02", -0.3}, {"E03", 0.8}};
    h.sat_gf_extra_band_free = true;
    h.extra_band_determined = {"E03|C7X|15"};
    double v = 0.2;
    for (const std::string sat : {"E01", "E02", "E03"}) {
        for (const std::string sig : {"C1C", "C5Q", "C7Q", "C7X"}) {
            h.code_bias_prev["E|" + sig][sat] = v;
            v += 0.31;
        }
        h.code_bias_sat_seen.insert(sat);
        h.code_bias_ifpin_pair[sat] = {"C1C", "C5Q"};
    }
    h.code_bias_prev["E|C7Q"].erase("E03");
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    Harness off = h;
    off.arms = {};
    const auto r_off = off.build(est, keys);

    ASSERT_EQ(rows.family_rows.count("E|C7X"), 1u);
    EXPECT_EQ(rows.family_rows.at("E|C7X").outcome, Outcome::Dependent);
    EXPECT_EQ(rows.family_rows.at("E|C7X").partner, "C7Q");
    EXPECT_TRUE(rows.H == r_off.H);
    EXPECT_TRUE(rows.z == r_off.z);
    const Eigen::Index r7 = levelRowOn(rows, est.codeBiasColumn("E03", "C7X"));
    ASSERT_GE(r7, 0);
    EXPECT_DOUBLE_EQ(rows.H(r7, est.codeBiasColumn("E03", "C7X")), 1.0);
}

// Galileo only: a GPS combined channel beside a primary one of its band keeps its
// sum-to-zero row and gets no record.
TEST(CodeDatumFamilyTest, OnlyGalileoChannelsAreTouched) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, {{"G01", {"C1C", "C2L", "C2X"}},
                                           {"G02", {"C1C", "C2L", "C2X"}},
                                           {"G03", {"C1C", "C2X"}}});
    Harness h;
    h.arms.family_difference = true;
    const auto rows = h.build(est, keys);
    Harness off;
    const auto r_off = off.build(est, keys);
    expectSameOutput(r_off, off, rows, h);
}

// The time link of the family row, alone: veterans carry the sum of their previous
// differences; without one every common satellite is spanned at 0, flagged as a re-seed.
TEST(CodeDatumFamilyTest, FamilyTimeLinkCarriesVeteransOrReseeds) {
    const std::vector<std::string> common = {"E01", "E02", "E03"};
    const auto tl = NetworkSbasis::codeFamilyDiffTimeLink(
        common, {{"E01", 1.5}, {"E02", 0.4}, {"E03", 2.0}}, {{"E01", 0.5}, {"E03", 1.25}});
    EXPECT_EQ(tl.jac_sats, (std::vector<std::string>{"E01", "E03"}));
    EXPECT_NEAR(tl.rhs, (1.5 - 0.5) + (2.0 - 1.25), 1e-12);
    EXPECT_FALSE(tl.reseed);

    const auto seed = NetworkSbasis::codeFamilyDiffTimeLink(common, {{"E02", 0.4}}, {});
    EXPECT_EQ(seed.jac_sats, common);
    EXPECT_DOUBLE_EQ(seed.rhs, 0.0);
    EXPECT_TRUE(seed.reseed);
}

// ===========================================================================
// (d) The datum's reach over the code-bias null directions
// ===========================================================================

// The code-bias directions the data cannot see are the per-channel levels (a receiver
// code bias absorbs them) and the per-satellite common modes (the satellite clock absorbs
// them).  On the two-family roster they span 8 dimensions (the sum of the channel levels
// equals the sum of the satellite modes).  The datum rows must reach all of them: the
// rank of H restricted to their span equals the span's dimension, with the sum-to-zero
// rows and with the family-difference rows alike.  Dropping a family-difference row
// leaves one direction unreached, so the row carries the combined channel's level.
TEST(CodeDatumFamilyTest, NullDirectionsStayFullyPinned) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    const Eigen::Index n = est.numShared();

    std::vector<Eigen::VectorXd> basis;
    for (const std::string sig : {"C1C", "C5Q", "C1X", "C5X"}) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
        for (const auto& [sat, sigs] : kTwoFamily)
            for (const auto& s : sigs)
                if (s == sig) v(est.codeBiasColumn(sat, s)) = 1.0;
        basis.push_back(v);
    }
    for (const auto& [sat, sigs] : kTwoFamily) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
        for (const auto& s : sigs) v(est.codeBiasColumn(sat, s)) = 1.0;
        basis.push_back(v);
    }
    Eigen::MatrixXd B(n, static_cast<Eigen::Index>(basis.size()));
    for (std::size_t k = 0; k < basis.size(); ++k) B.col(static_cast<Eigen::Index>(k)) = basis[k];
    const auto rankOf = [](const Eigen::MatrixXd& M) {
        Eigen::FullPivLU<Eigen::MatrixXd> lu(M);
        lu.setThreshold(1e-9);
        return static_cast<int>(lu.rank());
    };
    const int dim = rankOf(B);
    ASSERT_EQ(dim, 8);

    for (const bool fd : {false, true}) {
        Harness h;
        allVeterans(h);
        h.arms.family_difference = fd;
        const auto rows = h.build(est, keys);
        EXPECT_EQ(rankOf(rows.H * B), dim) << "family_difference " << fd;
        if (!fd) continue;
        ASSERT_EQ(rows.family_rows.at("E|C1X").outcome, Outcome::FamilyVeteran);
        // Drop the band-1 family-difference row: the combined band-1 level is unreached.
        const Eigen::Index r1 = levelRowOn(rows, est.codeBiasColumn("E01", "C1X"));
        ASSERT_GE(r1, 0);
        Eigen::MatrixXd H2(rows.H.rows() - 1, n);
        for (Eigen::Index r = 0, k = 0; r < rows.H.rows(); ++r)
            if (r != r1) H2.row(k++) = rows.H.row(r);
        EXPECT_EQ(rankOf(H2 * B), dim - 1);
    }
}

// ===========================================================================
// (e) The kept-epoch bookkeeping and the emit site
// ===========================================================================

// Counts once per call (a kept epoch), one CLK_BASIS/continued event per continued change
// and none for the others; a channel's first realization is recorded silently and every
// later switch of its row is one CB_DATUM/family_row event; empty scratch does nothing.
TEST(CodeDatumFamilyTest, KeptEpochCountsOnceAndDeclaresRowSwitches) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "kept"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents epoch_events;
    std::vector<Event> events;
    const auto sink = [&](const char* tier, const char* event, const std::string& sat,
                          const std::string&, std::optional<double>, std::optional<double>,
                          const char* action, const std::string& detail) {
        events.push_back({tier, event, sat, action, detail});
    };
    const SignalId e1x = *SignalId::fromObsCode(System::Galileo, "C1X");
    const SignalId e5x = *SignalId::fromObsCode(System::Galileo, "C5X");

    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);   // empty: nothing
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(ctx.counters.n_cb_family_epochs, 0u);

    epoch_events.code_datum.this_epoch_if_pin_changes = {
        {"E11", {{"C1C", "C5Q"}, {"C1C", "C5X"}, PinOut::Continued}},
        {"E12", {{"C1C", "C5X"}, {"C1C", "C5Q"}, PinOut::Reseeded}},
        {"E13", {{"C1C", "C5Q"}, {"C1X", "C5X"}, PinOut::LevelReseeded}}};
    epoch_events.code_datum.this_epoch_cb_family_rows = {
        {"E|C1X", {e1x, "C1C", Outcome::FamilyVeteran, 0}},
        {"E|C5X", {e5x, "C5Q", Outcome::NoCommon, 0}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    const auto& pc = ctx.counters.cb_pair_by_const.at(System::Galileo);
    EXPECT_EQ(pc.continued, 1);
    EXPECT_EQ(pc.reseeded, 1);
    EXPECT_EQ(pc.level_reseeded, 1);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].tier, "CLK_BASIS");
    EXPECT_EQ(events[0].event, "continued");
    EXPECT_EQ(events[0].sat, "E11");
    EXPECT_EQ(events[0].detail, "C1C+C5Q->C1C+C5X");
    EXPECT_EQ(ctx.counters.n_cb_family_epochs, 1u);
    EXPECT_EQ(ctx.counters.cb_family_by_band.at({System::Galileo, '1'}).family_veteran, 1);
    EXPECT_EQ(ctx.counters.cb_family_by_band.at({System::Galileo, '5'}).no_common, 1);
    EXPECT_EQ(ctx.counters.n_cb_family_switches, 0u);

    // Band 1 falls back to the sum-to-zero row, then returns to the family row.
    events.clear();
    epoch_events.code_datum.this_epoch_if_pin_changes.clear();
    epoch_events.code_datum.this_epoch_cb_family_rows = {{"E|C1X", {e1x, "C1C", Outcome::Bridged, 0}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    epoch_events.code_datum.this_epoch_cb_family_rows = {{"E|C1X", {e1x, "C1C", Outcome::FamilyVeteran, 0}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].tier, "CB_DATUM");
    EXPECT_EQ(events[0].event, "family_row");
    EXPECT_EQ(events[0].action, "to_sum_to_zero");
    EXPECT_EQ(events[0].detail, "E|C1X: family_difference/C1C->sum_to_zero (bridged)");
    EXPECT_EQ(events[1].action, "to_family_difference");
    EXPECT_EQ(ctx.counters.n_cb_family_switches, 2u);
    EXPECT_EQ(ctx.counters.n_cb_family_epochs, 3u);
    EXPECT_EQ(ctx.counters.cb_family_by_band.at({System::Galileo, '1'}).bridged, 1);
}

// The holds of a kept epoch: each held satellite-epoch is counted (those a channel-level
// re-seed marked apart); a hold is declared once when it begins (CLK_BASIS/held,
// hold_begin, the selection against the held pair) and once when it ends (hold_end, with
// the pair pinned now and what moved it there), against the last kept epoch's holds; a
// kept epoch repeating the same holds declares nothing.
TEST(CodeDatumFamilyTest, KeptEpochCountsHoldsAndDeclaresTheirEdges) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "kept_hold"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents epoch_events;
    std::vector<Event> events;
    const auto sink = [&](const char* tier, const char* event, const std::string& sat,
                          const std::string&, std::optional<double>, std::optional<double>,
                          const char* action, const std::string& detail) {
        events.push_back({tier, event, sat, action, detail});
    };
    using Hold = NetworkSbasis::PinPairHold;
    const Hold h11{{"C1C", "C5Q"}, {"C1C", "C5X"}};
    const Hold h12{{"C1C", "C5Q"}, {"C1X", "C5X"}, /*reseeded=*/true};

    epoch_events.code_datum.this_epoch_if_pin_holds = {{"E11", h11}, {"E12", h12}};
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E11", h11.held}, {"E12", h12.held}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].tier, "CLK_BASIS");
    EXPECT_EQ(events[0].event, "held");
    EXPECT_EQ(events[0].sat, "E11");
    EXPECT_EQ(events[0].action, "hold_begin");
    EXPECT_EQ(events[0].detail, "selected C1C+C5Q vs held C1C+C5X");
    EXPECT_EQ(events[1].sat, "E12");
    EXPECT_EQ(events[1].detail,
              "selected C1C+C5Q vs held C1X+C5X; marked by a channel-level re-seed");

    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);   // the same holds: counted, not declared
    EXPECT_EQ(events.size(), 2u);
    const NetworkSbasis::CodeDatumPairCounts& c =
        ctx.counters.cb_pair_by_const.at(System::Galileo);
    EXPECT_EQ(c.held, 4);
    EXPECT_EQ(c.held_reseeded, 2);
    EXPECT_EQ(c.hold_begun, 2);
    EXPECT_EQ(c.hold_ended, 0);
    EXPECT_EQ(c.hold_resumed, 0);
    EXPECT_EQ(c.continued + c.reseeded + c.level_reseeded, 0);

    // E11's hold is released (the pin continued onto the selection); E12's entering
    // signals leave (the selection names its held pair again).  The release is also the
    // continued change's own event.
    events.clear();
    epoch_events.code_datum.this_epoch_if_pin_holds.clear();
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E11", {"C1C", "C5Q"}}, {"E12", h12.held}};
    epoch_events.code_datum.this_epoch_if_pin_changes = {
        {"E11", {h11.held, {"C1C", "C5Q"}, PinOut::Continued}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].action, "hold_end");
    EXPECT_EQ(events[0].detail,
              "selected C1C+C5Q vs held C1C+C5X; pinned C1C+C5Q (continued)");
    EXPECT_EQ(events[1].detail,
              "selected C1C+C5Q vs held C1X+C5X; pinned C1X+C5X (the selection named it again)");
    EXPECT_EQ(events[2].event, "continued");
    EXPECT_EQ(c.hold_ended, 2);
    EXPECT_EQ(c.held, 4);
    EXPECT_EQ(c.continued, 1);

    // A hold that ends with the satellite unpinned, and one that ends at a re-seeded change.
    events.clear();
    epoch_events.code_datum.this_epoch_if_pin_changes.clear();
    epoch_events.code_datum.this_epoch_if_pin_holds = {{"E13", h11}, {"E14", h11}};
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E13", h11.held}, {"E14", h11.held}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    epoch_events.code_datum.this_epoch_if_pin_holds.clear();
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E14", {"C1C", "C5Q"}}};
    epoch_events.code_datum.this_epoch_if_pin_changes = {
        {"E14", {h11.held, {"C1C", "C5Q"}, PinOut::Reseeded}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].action, "hold_begin");
    EXPECT_EQ(events[2].detail, "selected C1C+C5Q vs held C1C+C5X; not pinned now");
    EXPECT_EQ(events[3].detail,
              "selected C1C+C5Q vs held C1C+C5X; pinned C1C+C5Q (re-seeded)");
    EXPECT_EQ(net::present(ctx.entities.sats, &net::SatelliteState::last_if_pin_hold), 0u);
    EXPECT_EQ(c.reseeded, 1);
    EXPECT_EQ(c.hold_refused_gd, 0);

    // A re-seeded change whose hold the group-delay test refused is counted as such.
    epoch_events.code_datum.this_epoch_cb_ifpin.clear();
    epoch_events.code_datum.this_epoch_if_pin_changes = {
        {"E15", {{"C1C", "C7Q"}, {"C1C", "C5Q"}, PinOut::Reseeded, /*hold_refused_gd=*/true}}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    EXPECT_EQ(c.reseeded, 2);
    EXPECT_EQ(c.hold_refused_gd, 1);
    epoch_events.code_datum.this_epoch_if_pin_changes.clear();

    // The first kept epoch after a warm resume: the hold baseline was not restored, so a
    // hold found there is declared "hold_resumed" and not counted as begun; the latch is
    // consumed, and a hold that begins at the next kept epoch is a begin again.
    events.clear();
    const int begun_before = c.hold_begun;
    ctx.carry.resume.if_pin_holds = true;
    epoch_events.code_datum.this_epoch_if_pin_holds = {{"E11", h11}};
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E11", h11.held}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    EXPECT_FALSE(ctx.carry.resume.if_pin_holds);
    epoch_events.code_datum.this_epoch_if_pin_holds = {{"E11", h11}, {"E16", h11}};
    epoch_events.code_datum.this_epoch_cb_ifpin = {{"E11", h11.held}, {"E16", h11.held}};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].sat, "E11");
    EXPECT_EQ(events[0].action, "hold_resumed");
    EXPECT_EQ(events[0].detail,
              "selected C1C+C5Q vs held C1C+C5X; first kept epoch after a warm resume");
    EXPECT_EQ(events[1].sat, "E16");
    EXPECT_EQ(events[1].action, "hold_begin");
    EXPECT_EQ(c.hold_resumed, 1);
    EXPECT_EQ(c.hold_begun, begun_before + 1);
}

// A continued satellite whose last emitted pair is the pair the builder continued from:
// the re-selection is left undeclared (no mark, no R-record entry, no clock disc step),
// the emit baseline advances, and the change is counted and evented once.
TEST(CodeDatumFamilyTest, EmitSiteLeavesAMatchingContinuedReselectionUndeclared) {
    EmitHarness h("match");
    h.change("G02", {"C1W", "C2W"}, {"C1W", "C2W"}, {"C1C", "C2W"});

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 0u);
    EXPECT_EQ(h.ctx->counters.n_if_pin_continued_undeclared, 1u);
    EXPECT_EQ(h.ctx->counters.n_if_pin_continued_declared, 0u);
    EXPECT_EQ(*net::getSat(h.ctx->entities.sats, "G02", &net::SatelliteState::last_if_pin_sel), (Pair{"C1C", "C2W"}));
    EXPECT_EQ(h.ctx->counters.cb_pair_by_const.at(System::GPS).continued, 1);
    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 0);
    const std::string csv = h.eventsCsv();
    EXPECT_NE(csv.find("reselect_continued"), std::string::npos);
    EXPECT_NE(csv.find("if_pin_pair_continued"), std::string::npos);
}

// The emitted baseline is not the pair the builder continued from (the satellite was
// pruned from an earlier kept epoch's emission, which moved its kept pair), or a re-seed
// is pending for it: the re-selection is declared as without the arm.
TEST(CodeDatumFamilyTest, EmitSiteDeclaresAContinuedReselectionOtherwise) {
    {
        EmitHarness h("baseline");
        h.change("G02", {"C1W", "C2W"}, {"C1W", "C5Q"}, {"C1C", "C5Q"});
        const NetworkSolPrepResult r = h.run();
        EXPECT_EQ(r.retro_emit_sats.count("G02"), 1u);
        EXPECT_EQ(h.ctx->counters.n_if_pin_continued_declared, 1u);
        EXPECT_EQ(h.ctx->counters.n_if_pin_continued_undeclared, 0u);
        h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
        EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 1);
    }
    {
        EmitHarness h("pending");
        h.change("G03", {"C1W", "C2W"}, {"C1W", "C2W"}, {"C1C", "C2W"});
        net::assignSatFlags(h.ctx->entities, net::Disc::CbReseed, {"G03"});
        const NetworkSolPrepResult r = h.run();
        EXPECT_EQ(r.retro_emit_sats.count("G03"), 1u);
        EXPECT_EQ(h.ctx->counters.n_if_pin_continued_declared, 1u);
        EXPECT_EQ(h.ctx->counters.n_if_pin_continued_undeclared, 0u);
    }
}

// A continued satellite outside this emission: counted as deferred, its emit baseline
// untouched, so its re-selection is compared when it next emits.
TEST(CodeDatumFamilyTest, EmitSiteDefersAContinuedSatelliteOutsideTheEmission) {
    EmitHarness h("absent");
    h.change("G09", {"C1W", "C2W"}, {"C1W", "C2W"}, {"C1C", "C2W"});
    const NetworkSolPrepResult r = h.run();
    EXPECT_TRUE(r.retro_emit_sats.empty());
    EXPECT_EQ(h.ctx->counters.n_if_pin_continued_unemitted, 1u);
    EXPECT_EQ(h.ctx->counters.n_if_pin_continued_undeclared, 0u);
    EXPECT_EQ(*net::getSat(h.ctx->entities.sats, "G09", &net::SatelliteState::last_if_pin_sel), (Pair{"C1W", "C2W"}));
}

// A held satellite at the emit site: its recorded selection is the held pair, which is
// its last emitted pair, so no re-selection is declared and the clock disc does not step;
// the hold's begin reaches network_events.csv.
TEST(CodeDatumFamilyTest, EmitSiteDeclaresNoReselectionForAHeldPair) {
    EmitHarness h("held");
    const Pair held{"C1W", "C2W"};
    net::putSat(h.ctx->entities, "G02", &net::SatelliteState::last_if_pin_sel) = held;
    h.ev.code_datum.this_epoch_if_pin_sel["G02"]   = held;
    h.ev.code_datum.this_epoch_if_pin_holds["G02"] = NetworkSbasis::PinPairHold{{"C1C", "C2W"}, held};
    h.ev.code_datum.this_epoch_cb_ifpin["G02"]     = held;

    const NetworkSolPrepResult r = h.run();

    EXPECT_EQ(r.retro_emit_sats.count("G02"), 0u);
    EXPECT_EQ(*net::getSat(h.ctx->entities.sats, "G02", &net::SatelliteState::last_if_pin_sel), held);
    EXPECT_EQ(h.ctx->counters.cb_pair_by_const.at(System::GPS).held, 1);
    EXPECT_EQ(h.ctx->counters.cb_pair_by_const.at(System::GPS).hold_begun, 1);
    h.setup.emitter->emitToStore(r.sol, h.setup.network_store);
    EXPECT_EQ(h.setup.emitter->clockDiscontinuityCounter("G02"), 0);
    const std::string csv = h.eventsCsv();
    EXPECT_NE(csv.find("hold_begin"), std::string::npos);
    EXPECT_NE(csv.find("selected C1C+C2W vs held C1W+C2W"), std::string::npos);
    EXPECT_EQ(csv.find("reselect"), std::string::npos);
}

// A held satellite in a split constellation: the emitted satellites of GPS are not
// unanimous on their pin pair (G02 held on (C1W, C2W), G03 on the selection (C1C, C2W)),
// so no cbpin=G declaration is made, and the dropped declaration is attributed to the hold.
// A split with no held satellite among the emitted ones is not.
TEST(CodeDatumFamilyTest, EmitSiteAttributesADroppedCbpinToAHold) {
    const Pair held{"C1W", "C2W"};
    const Pair sel{"C1C", "C2W"};
    {
        EmitHarness h("split_held");
        h.ev.code_datum.this_epoch_if_pin_holds["G02"] = NetworkSbasis::PinPairHold{sel, held};
        h.ev.code_datum.this_epoch_if_pin_real = {{"G02", held}, {"G03", sel}};
        h.run();
        EXPECT_EQ(h.ctx->counters.cb_pair_by_const.at(System::GPS).hold_split_decl, 1);
    }
    {
        EmitHarness h("split_plain");
        h.ev.code_datum.this_epoch_if_pin_real = {{"G02", held}, {"G03", sel}};
        h.run();
        EXPECT_EQ(h.ctx->counters.cb_pair_by_const.count(System::GPS), 0u);
    }
    {
        EmitHarness h("unanimous_held");
        h.ev.code_datum.this_epoch_if_pin_holds["G02"] = NetworkSbasis::PinPairHold{sel, held};
        h.ev.code_datum.this_epoch_if_pin_real = {{"G02", held}, {"G03", held}};
        h.run();
        EXPECT_EQ(h.ctx->counters.cb_pair_by_const.at(System::GPS).hold_split_decl, 0);
    }
}

// ===========================================================================
// (f) The loader and the run-end readout
// ===========================================================================

TEST(CodeDatumFamilyTest, KnobsDefaultOffAndLoad) {
    {
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(loadWith("", cfg, log, "load_off"));
        EXPECT_FALSE(cfg.networkCodeDatumPairContinuity());
        EXPECT_FALSE(cfg.networkCodeDatumFamilyDifference());
    }
    {
        LogManager log;
        GnssConfig cfg;
        ASSERT_TRUE(loadWith(kArmedEffective, cfg, log, "load_on"));
        EXPECT_TRUE(cfg.networkCodeDatumPairContinuity());
        EXPECT_TRUE(cfg.networkCodeDatumFamilyDifference());
        EXPECT_TRUE(cfg.networkCodeDatumTimeLinked());
    }
}

// Off: nothing.  Armed without a prerequisite: INERT naming only what is missing.  Pair
// continuity under absolute geometry-free rows: INERT naming the posture.  Effective with
// counters: the counts, no INERT.
TEST(CodeDatumFamilyTest, RunEndReadoutSaysInertOrCounts) {
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("", cfg, log, "re_off"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EpochEvents epoch_events;
    EpochEvents events;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkCodeDatumFamilyRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_TRUE(oss.str().empty());
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("    \"estimate_sat_code_bias\": true,\n"
                             "    \"code_datum_pair_continuity\": true,\n",
                             cfg, log, "re_inert"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EpochEvents epoch_events;
    EpochEvents events;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkCodeDatumFamilyRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("INERT without network.code_datum_time_linked."),
                  std::string::npos);
        EXPECT_EQ(oss.str().find("network.estimate_sat_code_bias"), std::string::npos);
        EXPECT_EQ(oss.str().find("family_difference"), std::string::npos);
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith(std::string(kArmedEffective)
                                 + "    \"w6_iono_constraint\": \"none\",\n"
                                   "    \"sat_gf_datum\": \"zero\",\n",
                             cfg, log, "re_absgf"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EpochEvents epoch_events;
    EpochEvents events;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkCodeDatumFamilyRunEnd(ctx, cfg, lg);
        lg->flush();
        EXPECT_NE(oss.str().find("GFC pair continuity): ARMED but INERT under "
                                 "network.sat_gf_datum without"),
                  std::string::npos);
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith(kArmedEffective, cfg, log, "re_on"));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EpochEvents epoch_events;
    EpochEvents events;
        ctx.counters.cb_pair_by_const[System::Galileo] = {17, 3, 1, 40, 2, 1, 5, 0, 4, 6};
        ctx.counters.n_if_pin_continued_undeclared = 15;
        ctx.counters.n_cb_family_epochs = 720;
        NetworkSbasis::CodeDatumFamilyCounts c;
        c.family_veteran = 718;
        c.family_reseed = 1;
        c.reseed_sats = 22;
        c.bridged = 1;
        ctx.counters.cb_family_by_band[{System::Galileo, '1'}] = c;
        ctx.counters.n_cb_family_switches = 2;
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkCodeDatumFamilyRunEnd(ctx, cfg, lg);
        lg->flush();
        const std::string s = oss.str();
        EXPECT_EQ(s.find("INERT"), std::string::npos);
        EXPECT_NE(s.find("17 ionosphere-free pin pair change(s) CONTINUED"), std::string::npos);
        EXPECT_NE(s.find("E 17/3/1"), std::string::npos);
        EXPECT_NE(s.find("40 satellite-epoch(s) HELD"), std::string::npos);
        EXPECT_NE(s.find("5 of them marked anyway by a channel-level re-seed"),
                  std::string::npos);
        EXPECT_NE(s.find("2 hold(s) begun, 1 ended and 0 found at the first kept epoch "
                         "after a warm resume"),
                  std::string::npos);
        EXPECT_NE(s.find("4 hold(s) refused because the broadcast group delay"),
                  std::string::npos);
        EXPECT_NE(s.find("6 emitted epoch(s) dropped a constellation's cbpin= declaration"),
                  std::string::npos);
        EXPECT_NE(s.find("held/re-seeded/begun/ended/resumed/refused/cbpin dropped: "
                         "E 40/5/2/1/0/4/6"),
                  std::string::npos);
        EXPECT_EQ(s.find("no pair change, re-seed or discontinuity"), std::string::npos);
        EXPECT_NE(s.find("15 pair re-selection discontinuity(ies) were left undeclared"),
                  std::string::npos);
        EXPECT_NE(s.find("over 720 kept epoch(s)"), std::string::npos);
        EXPECT_NE(s.find("E band 1: 718 family row(s) on veterans, 1 re-seeded (marking 22"),
                  std::string::npos);
        EXPECT_NE(s.find("2 switch(es)"), std::string::npos);
    }
}

// ===========================================================================
// (g) Step 2: the first-sighting family prior (network.code_family_birth_prior_sigma_m)
// ===========================================================================

namespace {

using Side = NetworkSbasis::FamilyBirthSide;

/// The family-difference arm with the first-sighting prior at @p sigma.
void armBirthPrior(Harness& h, double sigma) {
    h.arms.family_difference   = true;
    h.arms.birth_prior_sigma_m = sigma;
}

/// The gauge stack and every step-1 side channel of two builds agree.
void expectSameGaugeStack(const NetworkSbasis::CodeBiasDatumRows& a, const Harness& ha,
                          const NetworkSbasis::CodeBiasDatumRows& b, const Harness& hb) {
    ASSERT_EQ(a.H.rows(), b.H.rows());
    EXPECT_TRUE(a.H == b.H);
    EXPECT_TRUE(a.z == b.z);
    EXPECT_EQ(a.row_kinds, b.row_kinds);
    EXPECT_EQ(ha.cb_reseed, hb.cb_reseed);
    ASSERT_EQ(a.family_rows.size(), b.family_rows.size());
    for (const auto& [chan, r] : a.family_rows) {
        EXPECT_EQ(r.outcome, b.family_rows.at(chan).outcome) << chan;
        EXPECT_EQ(r.partner, b.family_rows.at(chan).partner) << chan;
    }
}

}  // namespace

// The pure planner with no carry map (the previous-value map alone): mu is the mean
// previous difference over the veterans (a previous value on both columns) and nothing
// else; a common satellite missing either value is born, tagged with the column(s) born,
// in the order of the common set; a veteran gets no row.  With no veteran mu is 0 and
// defined only where the family row re-seeds the offset at 0.
TEST(CodeDatumFamilyTest, BirthPlannerTakesMuOverVeteransOnly) {
    const std::vector<std::string> common = {"E01", "E02", "E03", "E04", "E05"};
    const std::map<std::string, NetworkSbasis::CodeBiasCarry> no_carry;
    const auto bp = NetworkSbasis::codeFamilyBirthPriors(
        common,
        /*prev_x=*/{{"E01", 1.5}, {"E02", 0.4}, {"E03", 2.0}, {"E09", 7.0}},
        /*prev_p=*/{{"E01", 0.5}, {"E02", 0.1}, {"E04", 3.0}, {"E09", -7.0}}, no_carry,
        no_carry, /*family_reseed=*/false);
    EXPECT_EQ(bp.n_veterans, 2);   // E01, E02; E09 is not common this epoch
    EXPECT_NEAR(bp.mu_m, ((1.5 - 0.5) + (0.4 - 0.1)) / 2.0, 1e-15);
    EXPECT_TRUE(bp.mu_defined);
    const std::vector<NetworkSbasis::FamilyBirth> want = {
        {"E03", Side::Partner, false}, {"E04", Side::Secondary, false},
        {"E05", Side::Both, false}};
    EXPECT_EQ(bp.births, want);
    EXPECT_TRUE(bp.carried.empty());

    const auto none = NetworkSbasis::codeFamilyBirthPriors(common, {}, {{"E02", 0.3}},
                                                           no_carry, no_carry, false);
    EXPECT_EQ(none.n_veterans, 0);
    EXPECT_DOUBLE_EQ(none.mu_m, 0.0);
    EXPECT_FALSE(none.mu_defined);   // no veteran and no re-seed: no realized family mean
    ASSERT_EQ(none.births.size(), common.size());
    EXPECT_EQ(none.births[1], (NetworkSbasis::FamilyBirth{"E02", Side::Secondary, false}));
    EXPECT_EQ(none.births[0].side, Side::Both);
    const auto reseed = NetworkSbasis::codeFamilyBirthPriors(common, {}, {{"E02", 0.3}},
                                                             no_carry, no_carry, true);
    EXPECT_TRUE(reseed.mu_defined);   // the family row re-seeds the offset at 0
    EXPECT_DOUBLE_EQ(reseed.mu_m, 0.0);

    const auto all_vet = NetworkSbasis::codeFamilyBirthPriors(
        {"E01"}, {{"E01", 1.0}}, {{"E01", 0.25}}, no_carry, no_carry, false);
    EXPECT_TRUE(all_vet.births.empty());
    EXPECT_NEAR(all_vet.mu_m, 0.75, 1e-15);

    EXPECT_STREQ(NetworkSbasis::familyBirthSideName(Side::Secondary), "x_born");
    EXPECT_STREQ(NetworkSbasis::familyBirthSideName(Side::Partner), "partner_born");
    EXPECT_STREQ(NetworkSbasis::familyBirthSideName(Side::Both), "both_born");
}

// Sigma 0 (the default) with the family difference armed: no prior row, no record, the
// step-1 output unchanged, although E03's C1X column is at its first sighting.
TEST(CodeDatumFamilyTest, BirthPriorOffFoldsNoRow) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1X"].erase("E03");
    armBirthPrior(h, 0.0);
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.family_birth_H.rows(), 0);
    EXPECT_EQ(rows.family_birth_z.size(), 0);
    EXPECT_DOUBLE_EQ(rows.family_birth_weight, 0.0);
    EXPECT_TRUE(rows.family_birth_priors.empty());

    Harness step1 = h;
    step1.arms = {};
    step1.arms.family_difference = true;   // the step-1 arms, no prior field set
    const auto r1 = step1.build(est, keys);
    expectSameGaugeStack(rows, h, r1, step1);
}

// E03's C1X column is new beside its veteran C1C (E01 and E02 veterans on both): one soft
// row, +1 on E03|C1X and -1 on E03|C1C only, target the veterans' mean previous
// difference (the family row's RHS over its two veterans), weight 1/sigma^2, apart from the
// gauge stack, which is the sigma-0 stack.  A band-5 first sighting gets none.
TEST(CodeDatumFamilyTest, BirthPriorTiesANewSecondaryColumnToItsVeteranPartner) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1X"].erase("E03");
    armBirthPrior(h, 0.10);
    const auto rows = h.build(est, keys);

    ASSERT_EQ(rows.family_birth_H.rows(), 1);
    ASSERT_EQ(rows.family_birth_z.size(), 1);
    const Eigen::Index cx = est.codeBiasColumn("E03", "C1X");
    const Eigen::Index cp = est.codeBiasColumn("E03", "C1C");
    EXPECT_DOUBLE_EQ(rows.family_birth_H(0, cx), 1.0);
    EXPECT_DOUBLE_EQ(rows.family_birth_H(0, cp), -1.0);
    EXPECT_DOUBLE_EQ(rows.family_birth_H.row(0).cwiseAbs().sum(), 2.0);
    const auto& px = h.code_bias_prev["E|C1X"];
    const auto& pp = h.code_bias_prev["E|C1C"];
    const double mu = ((px.at("E01") - pp.at("E01")) + (px.at("E02") - pp.at("E02"))) / 2.0;
    EXPECT_NEAR(rows.family_birth_z(0), mu, 1e-12);
    EXPECT_NEAR(rows.family_birth_weight, 1.0 / (0.10 * 0.10), 1e-9);
    // The target is the family mean the family row holds this epoch: its RHS over its count.
    const Eigen::Index r1 = levelRowOn(rows, est.codeBiasColumn("E01", "C1X"));
    ASSERT_GE(r1, 0);
    EXPECT_NEAR(rows.family_birth_z(0) * 2.0, rows.z(r1), 1e-12);

    ASSERT_EQ(rows.family_birth_priors.size(), 1u);
    const auto& b = rows.family_birth_priors[0];
    EXPECT_EQ(b.sat, "E03");
    EXPECT_EQ(b.channel.sys, System::Galileo);
    EXPECT_EQ(b.channel.digit, '1');
    EXPECT_EQ(b.x_signal, "C1X");
    EXPECT_EQ(b.partner, "C1C");
    EXPECT_EQ(b.born, Side::Secondary);
    EXPECT_NEAR(b.mu_m, mu, 1e-12);
    EXPECT_EQ(b.n_veterans, 2);
    EXPECT_DOUBLE_EQ(b.sigma_m, 0.10);
    EXPECT_TRUE(b.seen_before);

    Harness off = h;
    off.arms.birth_prior_sigma_m = 0.0;
    const auto r_off = off.build(est, keys);
    expectSameGaugeStack(rows, h, r_off, off);

    // A band-5 first sighting gets no prior row: band 1 only.
    Harness b5 = h;
    allVeterans(b5);
    b5.code_bias_prev["E|C5X"].erase("E03");
    const auto r5 = b5.build(est, keys);
    EXPECT_EQ(r5.family_birth_H.rows(), 0);
    EXPECT_TRUE(r5.family_birth_priors.empty());
}

// Folded once: at the next kept epoch the harvest has given E03|C1X a previous value, so
// the column is a veteran and no row is folded again.
TEST(CodeDatumFamilyTest, BirthPriorIsFoldedOncePerBirth) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    const double born_value = h.code_bias_prev["E|C1X"].at("E03");
    h.code_bias_prev["E|C1X"].erase("E03");
    armBirthPrior(h, 0.10);
    ASSERT_EQ(h.build(est, keys).family_birth_H.rows(), 1);

    h.code_bias_prev["E|C1X"]["E03"] = born_value;   // the kept epoch's harvest
    const auto next = h.build(est, keys);
    EXPECT_EQ(next.family_birth_H.rows(), 0);
    EXPECT_TRUE(next.family_birth_priors.empty());
    EXPECT_EQ(next.family_rows.at("E|C1X").outcome, Outcome::FamilyVeteran);
}

// Symmetric: E03's C1C column is new beside its veteran C1X.  The same row (+1 on the X
// column, -1 on the partner's, target the veteran mean of E01 and E02), tagged partner
// born.  With both columns new (a rising satellite tracked by both families) one row,
// tagged both.
TEST(CodeDatumFamilyTest, BirthPriorTiesANewPartnerColumnToItsVeteranSecondary) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1C"].erase("E03");
    armBirthPrior(h, 0.10);
    const auto rows = h.build(est, keys);
    ASSERT_EQ(rows.family_birth_H.rows(), 1);
    EXPECT_DOUBLE_EQ(rows.family_birth_H(0, est.codeBiasColumn("E03", "C1X")), 1.0);
    EXPECT_DOUBLE_EQ(rows.family_birth_H(0, est.codeBiasColumn("E03", "C1C")), -1.0);
    const auto& px = h.code_bias_prev["E|C1X"];
    const auto& pp = h.code_bias_prev["E|C1C"];
    EXPECT_NEAR(rows.family_birth_z(0),
                ((px.at("E01") - pp.at("E01")) + (px.at("E02") - pp.at("E02"))) / 2.0, 1e-12);
    ASSERT_EQ(rows.family_birth_priors.size(), 1u);
    EXPECT_EQ(rows.family_birth_priors[0].born, Side::Partner);

    Harness both = h;
    allVeterans(both);
    both.code_bias_prev["E|C1C"].erase("E03");
    both.code_bias_prev["E|C1X"].erase("E03");
    const auto rb = both.build(est, keys);
    ASSERT_EQ(rb.family_birth_priors.size(), 1u);
    EXPECT_EQ(rb.family_birth_priors[0].born, Side::Both);
    EXPECT_EQ(rb.family_birth_priors[0].n_veterans, 2);
}

// No partner on the satellite: E04 (combined family only) with a new C1X column gets no
// row, and a roster with no same-band primary channel gets none either.  At the run's first
// epoch (no veteran anywhere) every common satellite is born on both columns and its row
// targets 0, the convention the family row re-seeds at.
TEST(CodeDatumFamilyTest, BirthPriorNeedsThePartnerOnTheSatellite) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1X"].erase("E04");
    armBirthPrior(h, 0.10);
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.family_birth_H.rows(), 0);
    EXPECT_TRUE(rows.family_birth_priors.empty());

    NetworkEstimator est2;
    const auto keys2 = registerRoster(est2, {{"E01", {"C1X", "C5Q"}}, {"E02", {"C1X", "C5Q"}}});
    Harness h2;
    armBirthPrior(h2, 0.10);
    const auto rows2 = h2.build(est2, keys2);
    EXPECT_EQ(rows2.family_rows.at("E|C1X").outcome, Outcome::NoPartner);
    EXPECT_EQ(rows2.family_birth_H.rows(), 0);

    Harness first;   // empty previous-value maps: the run's first epoch
    armBirthPrior(first, 0.10);
    const auto r_first = first.build(est, keys);
    EXPECT_EQ(r_first.family_rows.at("E|C1X").outcome, Outcome::FamilyReseed);
    ASSERT_EQ(r_first.family_birth_priors.size(), 3u);   // E01, E02, E03
    for (Eigen::Index r = 0; r < r_first.family_birth_z.size(); ++r)
        EXPECT_DOUBLE_EQ(r_first.family_birth_z(r), 0.0);
    for (const auto& b : r_first.family_birth_priors) {
        EXPECT_EQ(b.born, Side::Both) << b.sat;
        EXPECT_EQ(b.n_veterans, 0) << b.sat;
        EXPECT_FALSE(b.seen_before) << b.sat;
    }
}

// The prior refines the time-linked family difference: without the family difference, or
// without the time link, a positive sigma folds nothing.
TEST(CodeDatumFamilyTest, BirthPriorIsInertWithoutTheFamilyDifference) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1X"].erase("E03");
    h.arms.birth_prior_sigma_m = 0.10;   // family difference off
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.family_birth_H.rows(), 0);
    EXPECT_TRUE(rows.family_birth_priors.empty());
    EXPECT_TRUE(rows.family_rows.empty());

    Harness untimed = h;
    armBirthPrior(untimed, 0.10);
    untimed.cb_time_linked = false;
    const auto r_untimed = untimed.build(est, keys);
    EXPECT_EQ(r_untimed.family_birth_H.rows(), 0);
    EXPECT_TRUE(r_untimed.family_birth_priors.empty());
}

// The kept-epoch bookkeeping counts each prior row per (constellation, band), with its
// satellite, the column born and the zero-convention rows, and declares each once as a
// CB_DATUM/family_birth_prior event (statistic mu, threshold sigma, action the column
// born, detail the satellite, the column born, mu and sigma).
TEST(CodeDatumFamilyTest, KeptEpochCountsAndDeclaresBirthPriors) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "kept_birth"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents epoch_events;
    struct Row {
        std::string tier, event, sat, action, detail;
        std::optional<double> stat, thr;
    };
    std::vector<Row> events;
    const auto sink = [&](const char* tier, const char* event, const std::string& sat,
                          const std::string&, std::optional<double> stat,
                          std::optional<double> thr, const char* action,
                          const std::string& detail) {
        events.push_back({tier, event, sat, action, detail, stat, thr});
    };
    const SignalId e1x = *SignalId::fromObsCode(System::Galileo, "C1X");
    NetworkSbasis::FamilyBirthPrior a;
    a.sat = "E03"; a.channel = e1x; a.x_signal = "C1X"; a.partner = "C1C";
    a.born = Side::Secondary; a.mu_m = 0.0123; a.n_veterans = 17; a.sigma_m = 0.1;
    a.seen_before = true;
    NetworkSbasis::FamilyBirthPrior b = a;
    b.sat = "E07"; b.born = Side::Both; b.mu_m = 0.0; b.n_veterans = 0; b.seen_before = false;
    epoch_events.code_datum.this_epoch_cb_family_births = {a, b};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);

    const auto& c = ctx.counters.cb_family_birth_by_band.at({System::Galileo, '1'});
    EXPECT_EQ(c.rows, 2);
    EXPECT_EQ(c.x_born, 1);
    EXPECT_EQ(c.partner_born, 0);
    EXPECT_EQ(c.both_born, 1);
    EXPECT_EQ(c.no_veteran, 1);
    EXPECT_EQ(c.seen_before, 1);
    EXPECT_DOUBLE_EQ(c.max_abs_mu_m, 0.0123);
    EXPECT_EQ(c.sats, (std::set<std::string>{"E03", "E07"}));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].tier, "CB_DATUM");
    EXPECT_EQ(events[0].event, "family_birth_prior");
    EXPECT_EQ(events[0].sat, "E03");
    EXPECT_EQ(events[0].action, "x_born");
    EXPECT_EQ(events[0].stat, std::optional<double>(0.0123));
    EXPECT_EQ(events[0].thr, std::optional<double>(0.1));
    EXPECT_EQ(events[0].detail,
              "E03: C1X-C1C = mu, the secondary column born (C1X, satellite seen before); "
              "mu +0.0123 m over 17 veteran(s); sigma 0.100 m");
    EXPECT_EQ(events[1].action, "both_born");
    EXPECT_EQ(events[1].detail,
              "E07: C1X-C1C = mu, both columns born (C1X+C1C); mu +0.0000 m over 0 "
              "veteran(s) (none: the family row's re-seed at 0); sigma 0.100 m");

    // A kept epoch without prior rows adds nothing.
    events.clear();
    epoch_events.code_datum.this_epoch_cb_family_births.clear();
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(ctx.counters.cb_family_birth_by_band.at({System::Galileo, '1'}).rows, 2);
}

// The knob: default 0 (off), loads a positive sigma, refuses a negative one.
TEST(CodeDatumFamilyTest, BirthPriorKnobDefaultsOffAndLoads) {
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith("", cfg, log, "bp_off"));
        EXPECT_DOUBLE_EQ(cfg.networkCodeFamilyBirthPriorSigmaM(), 0.0);
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith(std::string(kArmedEffective)
                                 + "    \"code_family_birth_prior_sigma_m\": 0.1,\n",
                             cfg, log, "bp_on"));
        EXPECT_DOUBLE_EQ(cfg.networkCodeFamilyBirthPriorSigmaM(), 0.1);
    }
    {
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadWith(std::string(kArmedEffective)
                                  + "    \"code_family_birth_prior_sigma_m\": -0.1,\n",
                              cfg, log, "bp_neg"));
    }
    // Below 0.01 m (information above ten times the gauge weight) the prior would override
    // the datum rows it refines: refused.  At 0.02 m (information 2500, above the gauge
    // weight 1e3) it loads, with a warning.
    {
        LogManager log; GnssConfig cfg;
        EXPECT_FALSE(loadWith(std::string(kArmedEffective)
                                  + "    \"code_family_birth_prior_sigma_m\": 0.005,\n",
                              cfg, log, "bp_stiff"));
    }
    {
        LogManager log; GnssConfig cfg;
        ASSERT_TRUE(loadWith(std::string(kArmedEffective)
                                 + "    \"code_family_birth_prior_sigma_m\": 0.02,\n",
                             cfg, log, "bp_gauge"));
        EXPECT_DOUBLE_EQ(cfg.networkCodeFamilyBirthPriorSigmaM(), 0.02);
    }
}

// The run-end readout of the prior: nothing at sigma 0; INERT without the family
// difference or the time link, naming what is missing; ZERO rows warned; the counts read
// back when it folded.
TEST(CodeDatumFamilyTest, RunEndReadsTheBirthPrior) {
    const auto readout = [](const std::string& extra, const std::string& tag,
                            const std::function<void(NetworkRunContext&)>& fill) {
        LogManager log; GnssConfig cfg;
        EXPECT_TRUE(loadWith(extra, cfg, log, tag));
        NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
        EpochEvents epoch_events;
    EpochEvents events;
        if (fill) fill(ctx);
        std::ostringstream oss;
        auto lg = captureLogger(oss);
        networkCodeDatumFamilyRunEnd(ctx, cfg, lg);
        lg->flush();
        return oss.str();
    };
    const std::string fd_only = "    \"estimate_sat_code_bias\": true,\n"
                                "    \"code_datum_time_linked\": true,\n"
                                "    \"code_datum_family_difference\": true,\n";
    EXPECT_EQ(readout(fd_only, "re_bp_zero_sigma", nullptr).find("birth prior"),
              std::string::npos);

    const std::string no_fd = readout("    \"estimate_sat_code_bias\": true,\n"
                                      "    \"code_datum_time_linked\": true,\n"
                                      "    \"code_family_birth_prior_sigma_m\": 0.1,\n",
                                      "re_bp_nofd", nullptr);
    EXPECT_NE(no_fd.find("GFC family birth prior): network.code_family_birth_prior_sigma_m "
                         "= 0.1 m ARMED but INERT without network.code_datum_family_difference."),
              std::string::npos);

    const std::string no_tl = readout("    \"estimate_sat_code_bias\": true,\n"
                                      "    \"code_datum_family_difference\": true,\n"
                                      "    \"code_family_birth_prior_sigma_m\": 0.1,\n",
                                      "re_bp_notl", nullptr);
    EXPECT_NE(no_tl.find("= 0.1 m ARMED but INERT without network.code_datum_time_linked."),
              std::string::npos);

    // Neither the family difference nor the time link: both named.
    const std::string neither = readout("    \"estimate_sat_code_bias\": true,\n"
                                        "    \"code_family_birth_prior_sigma_m\": 0.1,\n",
                                        "re_bp_neither", nullptr);
    EXPECT_NE(neither.find("= 0.1 m ARMED but INERT without "
                           "network.code_datum_family_difference and "
                           "network.code_datum_time_linked."),
              std::string::npos);

    const std::string armed = fd_only + "    \"code_family_birth_prior_sigma_m\": 0.1,\n";
    EXPECT_NE(readout(armed, "re_bp_none", nullptr).find("ARMED but ZERO soft rows folded"),
              std::string::npos);
    // Only skips counted: still zero rows, with the skips named.
    const std::string skips_only = readout(armed, "re_bp_skips", [](NetworkRunContext& ctx) {
        NetworkSbasis::CodeFamilyBirthCounts c;
        c.seeded = 2; c.no_reference = 1;
        ctx.counters.cb_family_birth_by_band[{System::Galileo, '1'}] = c;
    });
    EXPECT_NE(skips_only.find("ARMED but ZERO soft rows folded"), std::string::npos);
    EXPECT_NE(skips_only.find("no row on 2 READMIT seed(s), 0 carried column(s) without a "
                              "previous value, 1 birth(s) with no realized family mean"),
              std::string::npos);

    const std::string s = readout(armed, "re_bp_on", [](NetworkRunContext& ctx) {
        NetworkSbasis::CodeFamilyBirthCounts c;
        c.rows = 25; c.x_born = 20; c.partner_born = 3; c.both_born = 2; c.no_veteran = 2;
        c.seen_before = 4; c.loosened = 1; c.max_abs_mu_m = 0.0425;
        c.sats = {"E01", "E02", "E03"};
        c.seeded = 6; c.carried = 0; c.no_reference = 3;
        ctx.counters.cb_family_birth_by_band[{System::Galileo, '1'}] = c;
    });
    EXPECT_EQ(s.find("INERT"), std::string::npos);
    EXPECT_NE(s.find("sigma 0.1 m (information 100); over the kept epochs 25 soft row(s) "
                     "d_X - d_P = mu folded, once per (satellite, birth), on 3 satellite(s): "
                     "E band 1: 25 row(s) on 3 satellite(s) (secondary column born 20, "
                     "partner born 3, both 2; 4 on satellites seen before; 1 on a loosened "
                     "carry; 2 at the family row's re-seed target 0; max |mu| 0.0425 m); no "
                     "row on 6 READMIT seed(s), 0 carried column(s) without a previous value, "
                     "3 birth(s) with no realized family mean"),
              std::string::npos);
}

// ===========================================================================
// (h) Step 2's birth test reads the carry, and the fold path (the review of 2026-10-10)
// ===========================================================================

namespace {

using Fold = NetworkSbasis::CodeBiasCarry;
using Skip = NetworkSbasis::FamilyBirthSkip;

/// A solved shared estimate over @p keys, epoch open and closed: a prior on every
/// code-bias column at a value of its own, enough for the stash, which reads the solve's
/// values and covariance.  No station: the priors make the shared system full rank.
Eigen::VectorXd solveOver(NetworkEstimator& est, const std::vector<Pair>& keys) {
    est.beginEpoch();
    std::vector<NetworkEstimator::SharedPrior> pri;
    double v = 0.05;
    for (const auto& [sat, sig] : keys) {
        NetworkEstimator::SharedPrior pr;
        pr.kind   = SharedParamKind::CodeBias;
        pr.sat    = sat;
        pr.signal = sig;
        pr.value  = v;
        pr.info   = 100.0;
        pri.push_back(pr);
        v += 0.11;
    }
    est.addSharedPrior(pri);
    return est.solve({});
}

}  // namespace

// The planner reads the carry.  A column the previous-value map lacks continues when the
// carry folds it (a READMIT seed, or a plain fold the map missed): no row, recorded with
// the column(s) the carry continues.  A column the clock monitor's reinit loosens is born
// although the map has its value, flagged loosened.  The mean is taken over the map's
// veterans as before (E06, loosened, is one).
TEST(CodeDatumFamilyTest, BirthPlannerReadsTheCarry) {
    const std::vector<std::string> common = {"E01", "E02", "E03", "E04",
                                             "E05", "E06", "E07", "E08"};
    const std::map<std::string, double> prev_x = {
        {"E01", 1.0}, {"E02", 0.6}, {"E03", 0.2}, {"E06", 0.9}, {"E07", 0.3}, {"E08", 0.4}};
    const std::map<std::string, double> prev_p = {
        {"E01", 0.5}, {"E02", 0.4}, {"E05", 0.1}, {"E06", 0.3}};
    const std::map<std::string, Fold> carry_x = {
        {"E01", Fold::Carried}, {"E02", Fold::Carried}, {"E03", Fold::Carried},
        {"E04", Fold::Seeded},  {"E05", Fold::Carried}, {"E06", Fold::Loosened},
        {"E07", Fold::Carried}, {"E08", Fold::Loosened}};
    const std::map<std::string, Fold> carry_p = {
        {"E01", Fold::Carried}, {"E02", Fold::Carried}, {"E03", Fold::Seeded},
        {"E04", Fold::Seeded},  {"E05", Fold::Carried}, {"E06", Fold::Carried}};
    const auto bp = NetworkSbasis::codeFamilyBirthPriors(common, prev_x, prev_p, carry_x,
                                                         carry_p, false);
    EXPECT_EQ(bp.n_veterans, 3);   // E01, E02, E06: a previous value on both columns
    EXPECT_NEAR(bp.mu_m, ((1.0 - 0.5) + (0.6 - 0.4) + (0.9 - 0.3)) / 3.0, 1e-15);
    EXPECT_TRUE(bp.mu_defined);
    const std::vector<NetworkSbasis::FamilyBirth> births = {
        {"E06", Side::Secondary, true},   // the reinit gave its X carry up
        {"E07", Side::Partner, false},    // no partner value, no partner carry
        {"E08", Side::Both, true}};       // X loosened, no partner at all
    EXPECT_EQ(bp.births, births);
    using Carried = std::tuple<std::string, Skip, Side>;
    const std::vector<Carried> carried = {
        {"E03", Skip::Seeded, Side::Partner},    // a return inside the seed horizon
        {"E04", Skip::Seeded, Side::Both},       // the whole satellite returned
        {"E05", Skip::Carried, Side::Secondary}};   // a carry the map missed
    EXPECT_EQ(bp.carried, carried);
    EXPECT_STREQ(NetworkSbasis::familyBirthSkipName(Skip::Seeded), "seeded");
    EXPECT_STREQ(NetworkSbasis::familyBirthSkipName(Skip::Carried), "carried");
    EXPECT_STREQ(NetworkSbasis::familyBirthSkipName(Skip::NoReference), "no_reference");
}

// The builder with the carry map: E03's C1C column has no previous value, but a READMIT
// seed carries it, so no row is folded and the skip is recorded.  Without the carry map
// (the previous-value map alone, the defect of the first build) the same epoch folds a
// second row; with the park past the horizon (no carry entry) the return is a birth.
TEST(CodeDatumFamilyTest, BirthPriorSkipsAReturnTheSeedCarries) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    h.code_bias_prev["E|C1C"].erase("E03");
    armBirthPrior(h, 0.10);
    for (const auto& [sat, sig] : keys) h.carry[{sat, sig}] = Fold::Carried;
    h.carry[{"E03", "C1C"}] = Fold::Seeded;
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.family_birth_H.rows(), 0);
    EXPECT_TRUE(rows.family_birth_priors.empty());
    ASSERT_EQ(rows.family_birth_skips.size(), 1u);
    const auto& s = rows.family_birth_skips[0];
    EXPECT_EQ(s.sat, "E03");
    EXPECT_EQ(s.why, Skip::Seeded);
    EXPECT_EQ(s.side, Side::Partner);
    EXPECT_EQ(s.x_signal, "C1X");
    EXPECT_EQ(s.partner, "C1C");
    EXPECT_EQ(s.outcome, Outcome::FamilyVeteran);

    Harness map_only = h;
    map_only.carry.clear();
    const auto r_map = map_only.build(est, keys);
    ASSERT_EQ(r_map.family_birth_priors.size(), 1u);
    EXPECT_EQ(r_map.family_birth_priors[0].sat, "E03");
    EXPECT_TRUE(r_map.family_birth_skips.empty());

    Harness expired = h;
    expired.carry.erase({"E03", "C1C"});   // parked past the horizon: no carry entry
    const auto r_exp = expired.build(est, keys);
    ASSERT_EQ(r_exp.family_birth_priors.size(), 1u);
    EXPECT_EQ(r_exp.family_birth_priors[0].born, Side::Partner);
    EXPECT_FALSE(r_exp.family_birth_priors[0].loosened);
    EXPECT_TRUE(r_exp.family_birth_skips.empty());
    expectSameGaugeStack(rows, h, r_exp, expired);   // the prior never touches the gauge rows
}

// The mirror: the map has E03's C1X value, but the clock monitor's reinit loosens its carry,
// so the column is born again and folds one row, flagged loosened, target the veterans'
// mean (E03 still one of them: the family row spans it).
TEST(CodeDatumFamilyTest, BirthPriorFoldsOnALoosenedCarry) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    armBirthPrior(h, 0.10);
    for (const auto& [sat, sig] : keys) h.carry[{sat, sig}] = Fold::Carried;
    h.carry[{"E03", "C1X"}] = Fold::Loosened;
    const auto rows = h.build(est, keys);
    ASSERT_EQ(rows.family_birth_priors.size(), 1u);
    const auto& b = rows.family_birth_priors[0];
    EXPECT_EQ(b.sat, "E03");
    EXPECT_EQ(b.born, Side::Secondary);
    EXPECT_TRUE(b.loosened);
    EXPECT_EQ(b.n_veterans, 3);
    const auto& px = h.code_bias_prev["E|C1X"];
    const auto& pp = h.code_bias_prev["E|C1C"];
    double sum = 0.0;
    for (const char* sat : {"E01", "E02", "E03"}) sum += px.at(sat) - pp.at(sat);
    EXPECT_NEAR(rows.family_birth_z(0), sum / 3.0, 1e-12);
    EXPECT_TRUE(rows.family_birth_skips.empty());
}

// A bridged channel (no family veteran, the sum-to-zero time link carries the X level):
// the offset this epoch is what the sum-to-zero frame holds, so the births have no realized
// family mean to target.  No row; each birth recorded as no_reference with the outcome.
TEST(CodeDatumFamilyTest, BirthPriorHasNoTargetOnABridgedChannel) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, kTwoFamily);
    Harness h;
    allVeterans(h);
    for (const std::string sat : {"E01", "E02", "E03"}) h.code_bias_prev["E|C1X"].erase(sat);
    armBirthPrior(h, 0.10);
    const auto rows = h.build(est, keys);
    ASSERT_EQ(rows.family_rows.at("E|C1X").outcome, Outcome::Bridged);
    EXPECT_EQ(rows.family_birth_H.rows(), 0);
    EXPECT_TRUE(rows.family_birth_priors.empty());
    ASSERT_EQ(rows.family_birth_skips.size(), 3u);
    for (const auto& s : rows.family_birth_skips) {
        EXPECT_EQ(s.why, Skip::NoReference) << s.sat;
        EXPECT_EQ(s.side, Side::Secondary) << s.sat;
        EXPECT_EQ(s.outcome, Outcome::Bridged) << s.sat;
    }
    Harness off = h;
    off.arms.birth_prior_sigma_m = 0.0;
    const auto r_off = off.build(est, keys);
    expectSameGaugeStack(rows, h, r_off, off);
}

// The carry map the driver hands the builder: Folded is carried, Seeded seeded, Reinit
// loosened, Parked and every non-code-bias entry absent; an undecided entry is read from
// its own record.
TEST(CodeDatumFamilyTest, CodeBiasCarryMapReadsTheFoldDecisions) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "carry_map"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents epoch_events;
    const auto entry = [](SharedParamKind kind, const std::string& sat, const std::string& sig,
                          CarryDisposition d) {
        CarryPrior cp;
        cp.kind = kind; cp.sat = sat; cp.signal = sig; cp.value = 0.1; cp.var = 1e-3;
        cp.fold.disposition = d;
        return cp;
    };
    CarryPrior undecided_live = entry(SharedParamKind::CodeBias, "E06", "C1C",
                                      CarryDisposition::Undecided);
    CarryPrior undecided_flag = entry(SharedParamKind::CodeBias, "E07", "C1C",
                                      CarryDisposition::Undecided);
    undecided_flag.inflate_q = true;
    CarryPrior undecided_park = entry(SharedParamKind::CodeBias, "E08", "C1C",
                                      CarryDisposition::Undecided);
    undecided_park.t_park_s = 1.0;
    ctx.carry.prev_carry = {
        entry(SharedParamKind::CodeBias, "E01", "C1X", CarryDisposition::Folded),
        entry(SharedParamKind::CodeBias, "E02", "C1C", CarryDisposition::Seeded),
        entry(SharedParamKind::CodeBias, "E03", "C1X", CarryDisposition::Reinit),
        entry(SharedParamKind::CodeBias, "E04", "C1C", CarryDisposition::Parked),
        entry(SharedParamKind::Clock, "E05", "", CarryDisposition::Folded),
        entry(SharedParamKind::PhaseBias, "E05", "L1C", CarryDisposition::Folded),
        undecided_live, undecided_flag, undecided_park};
    const NetworkSbasis::CodeBiasCarryMap m = networkCodeBiasCarryMap(ctx);
    const NetworkSbasis::CodeBiasCarryMap want = {
        {{"E01", "C1X"}, Fold::Carried}, {{"E02", "C1C"}, Fold::Seeded},
        {{"E03", "C1X"}, Fold::Loosened}, {{"E06", "C1C"}, Fold::Carried},
        {{"E07", "C1C"}, Fold::Loosened}};
    EXPECT_EQ(m, want);
}

// End to end through the stash and the decision (the review's double fold): a column that
// leaves the layout is parked by READMIT and dropped from the previous-value map; back
// 20 min later it is seeded from the park and is NOT a birth (the map alone would fold a
// second row).  Back past the 65 min horizon it is.  Once the seeded epoch is kept the
// column is an ordinary veteran.  A clock-monitor reinit at a later stash loosens a
// satellite's carry, and its columns are born again.
TEST(CodeDatumFamilyTest, BirthPriorIsFoldedOnceAcrossAReadmitSeed) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("    \"estimate_sat_code_bias\": true,\n"
                         "    \"code_datum_time_linked\": true,\n"
                         "    \"code_datum_family_difference\": true,\n"
                         "    \"code_family_birth_prior_sigma_m\": 0.1,\n"
                         "    \"readmit_seed_horizon_s\": 3900.0,\n",
                         cfg, logman, "readmit_once"));
    NetworkTestRun fx(cfg);   // networkCarryDecide's epoch form reads the run's estimator
    NetworkRunContext& ctx = fx.ctx();
    const auto decide = [&](const GnssTime& t, double dt) {
        NetworkEpoch& e = fx.newEpoch(t);
        e.clock.dt = dt;
        networkCarryDecide(fx.run(), e);
    };
    ASSERT_TRUE(ctx.knobs.cb_time_linked);
    ASSERT_TRUE(ctx.knobs.readmit_seed_cb);
    ASSERT_GT(ctx.knobs.readmit_seed_horizon_s, 0.0);
    ASSERT_GT(ctx.knobs.clk_mon_reinit_sig, 0.0);   // a flagged satellite is loosened, not dropped
    const fs::path dir = fs::temp_directory_path() / "gfc_readmit_once_traces";
    fs::create_directories(dir);
    auto traces = std::make_unique<NetworkRunTraces>(dir);
    traces->openAll(cfg, cfg.getNetworkConfig(), cfg.getGlobalVtecShConfig(),
                    /*sh_model=*/nullptr, /*cl_trace_armed=*/false, nullLogger());
    const NetworkSbasis::Plan plan;

    std::vector<std::pair<std::string, std::vector<std::string>>> gone_roster = kTwoFamily;
    gone_roster[2].second = {"C5Q", "C1X", "C5X"};   // E03 without C1C
    const auto at = [](double tow) { return GnssTime{2313, tow}; };
    // Keep an epoch: the solve over the layout, then the stash (with the READMIT clock the
    // D2 inject would set).
    const auto keep = [&](NetworkEstimator& est, const std::vector<Pair>& keys, double tow,
                          const std::set<std::string>& refloat = {}) {
        const Eigen::VectorXd x = solveOver(est, keys);
        ctx.carry.readmit_now_s = TimeUtils::continuousGpsSeconds(at(tow));
        networkStashCarry(ctx, cfg, *traces, at(tow), est, x, plan, nullptr, nullptr, refloat,
                          /*registered_clock_sats=*/{}, /*shared_phase_keys=*/{}, keys,
                          EpochEvents{}, nullLogger());
    };
    // The builder at this epoch, from the context's maps and (optionally) the carry.
    const auto build = [&](const NetworkEstimator& est, const std::vector<Pair>& keys,
                           bool with_carry) {
        Harness h;
        armBirthPrior(h, 0.10);
        h.code_bias_prev       = net::channelMap(ctx.entities.constellations,
                                                 &net::ChannelState::code_bias_prev);
        h.code_bias_sat_seen   = net::satSet(ctx.entities.sats, &net::SatelliteState::code_bias_seen);
        h.code_bias_ifpin_pair = net::satMap(ctx.entities.sats, &net::SatelliteState::code_bias_ifpin_pair);
        if (with_carry) h.carry = networkCodeBiasCarryMap(ctx);
        return h.build(est, keys);
    };
    const auto rowsOn = [](const NetworkSbasis::CodeBiasDatumRows& r, const std::string& sat) {
        int n = 0;
        for (const auto& b : r.family_birth_priors) n += b.sat == sat ? 1 : 0;
        return n;
    };

    // t1: every column registered; kept.
    const double t1 = 259200.0;
    NetworkEstimator& e1 = fx.renewEstimator();
    const auto keys_all = registerRoster(e1, kTwoFamily);
    keep(e1, keys_all, t1);
    ASSERT_EQ(netAt(net::get(ctx.entities.constellations, System::Galileo, &net::ConstellationState::channels, "C1C", &net::ChannelState::code_bias_prev)).count("E03"), 1u);

    // t2: E03's C1C column leaves the layout; kept, so READMIT parks its carry and the
    // previous-value map drops it.
    NetworkEstimator& e2 = fx.renewEstimator();
    const auto keys_gone = registerRoster(e2, gone_roster);
    decide(at(t1 + 30.0), 30.0);
    keep(e2, keys_gone, t1 + 30.0);
    EXPECT_EQ(ctx.counters.n_readmit_parked, 1);
    EXPECT_EQ(netAt(net::get(ctx.entities.constellations, System::Galileo, &net::ConstellationState::channels, "C1C", &net::ChannelState::code_bias_prev)).count("E03"), 0u);

    // t3 = t2 + 20 min: back, seeded from the park.  No birth; a second row without the carry.
    NetworkEstimator& e3 = fx.renewEstimator();
    registerRoster(e3, kTwoFamily);
    const double t3 = t1 + 30.0 + 1200.0;
    decide(at(t3), 1200.0);
    EXPECT_EQ(networkCodeBiasCarryMap(ctx).at({"E03", "C1C"}), Fold::Seeded);
    const auto r3 = build(e3, keys_all, true);
    EXPECT_EQ(rowsOn(r3, "E03"), 0);
    ASSERT_EQ(r3.family_birth_skips.size(), 1u);
    EXPECT_EQ(r3.family_birth_skips[0].sat, "E03");
    EXPECT_EQ(r3.family_birth_skips[0].why, Skip::Seeded);
    EXPECT_EQ(rowsOn(build(e3, keys_all, false), "E03"), 1);   // the map alone: a double fold

    // The same return past the horizon (t1 + 5000 s since the park): parked, a birth.
    decide(at(t1 + 5000.0), 3770.0);
    EXPECT_EQ(networkCodeBiasCarryMap(ctx).count({"E03", "C1C"}), 0u);
    const auto r3b = build(e3, keys_all, true);
    EXPECT_EQ(rowsOn(r3b, "E03"), 1);
    EXPECT_TRUE(r3b.family_birth_skips.empty());

    // Keep the seeded epoch: the column is a veteran of the map again, and nothing folds.
    decide(at(t3), 1200.0);
    keep(e3, keys_all, t3);
    NetworkEstimator& e4 = fx.renewEstimator();
    registerRoster(e4, kTwoFamily);
    decide(at(t3 + 30.0), 30.0);
    const auto r4 = build(e4, keys_all, true);
    EXPECT_EQ(r4.family_birth_H.rows(), 0);
    EXPECT_TRUE(r4.family_birth_skips.empty());

    // A clock-monitor reinit of E01 at the next stash: its carry is loosened, and its
    // columns are born again (one row, flagged loosened), though the map has their values.
    keep(e4, keys_all, t3 + 30.0, {"E01"});
    NetworkEstimator& e5 = fx.renewEstimator();
    registerRoster(e5, kTwoFamily);
    decide(at(t3 + 60.0), 30.0);
    const auto r5 = build(e5, keys_all, true);
    ASSERT_EQ(r5.family_birth_priors.size(), 1u);
    EXPECT_EQ(r5.family_birth_priors[0].sat, "E01");
    EXPECT_EQ(r5.family_birth_priors[0].born, Side::Both);
    EXPECT_TRUE(r5.family_birth_priors[0].loosened);

    traces.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The fold path: the rows fold at their own weight 1/sigma^2 under CbFamilyBirth, one
// fold record of their dimension; with two weak column priors at 0 the solved difference
// is the closed form 2 w mu / (eps + 2 w).  The RAIM replay folds its captured copy
// through the same helper and solves identically.  No rows: no fold record.
TEST(CodeDatumFamilyTest, FamilyBirthRowsFoldAtTheirOwnWeightAndKind) {
    const double mu = 0.3, w = 1.0 / (0.1 * 0.1), eps = 1e-4;
    const auto solveWith = [&](const Eigen::MatrixXd& H, const Eigen::VectorXd& z,
                               double weight, std::vector<SharedFoldRecord>* folds) {
        NetworkEstimator est;
        est.registerCodeBias("E03", "C1X");
        est.registerCodeBias("E03", "C1C");
        est.beginEpoch();
        std::vector<NetworkEstimator::SharedPrior> pri(2);
        pri[0].kind = SharedParamKind::CodeBias; pri[0].sat = "E03"; pri[0].signal = "C1X";
        pri[1].kind = SharedParamKind::CodeBias; pri[1].sat = "E03"; pri[1].signal = "C1C";
        pri[0].info = pri[1].info = eps;
        est.addSharedPrior(pri);
        NetworkSbasis::foldFamilyBirthRows(est, H, z, weight);
        if (folds) *folds = est.sharedFoldLog().folds;
        const Eigen::VectorXd x = est.solve({});
        return Eigen::Vector2d(x(est.codeBiasColumn("E03", "C1X")),
                               x(est.codeBiasColumn("E03", "C1C")));
    };
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(1, 2);
    {
        NetworkEstimator layout;   // the column order of the layout above
        H(0, layout.registerCodeBias("E03", "C1X")) = 1.0;
        H(0, layout.registerCodeBias("E03", "C1C")) = -1.0;
    }
    const Eigen::VectorXd z = Eigen::VectorXd::Constant(1, mu);
    std::vector<SharedFoldRecord> folds;
    const Eigen::Vector2d x = solveWith(H, z, w, &folds);
    ASSERT_FALSE(folds.empty());
    EXPECT_EQ(folds.back().kind, ContributionKind::CbFamilyBirth);
    EXPECT_EQ(folds.back().dim, 1);
    EXPECT_NEAR(x(0) - x(1), 2.0 * w * mu / (eps + 2.0 * w), 1e-12);

    // The replay's re-fold of the captured copy: the same solution, bit for bit.
    const Eigen::MatrixXd cap_H = H;
    const Eigen::VectorXd cap_z = z;
    const Eigen::Vector2d xr = solveWith(cap_H, cap_z, w, nullptr);
    EXPECT_EQ(xr(0), x(0));
    EXPECT_EQ(xr(1), x(1));

    std::vector<SharedFoldRecord> none;
    solveWith(Eigen::MatrixXd(0, 2), Eigen::VectorXd(0), 0.0, &none);
    for (const auto& f : none) EXPECT_NE(f.kind, ContributionKind::CbFamilyBirth);

    const ContributionSpec& spec = contributionSpec(ContributionKind::CbFamilyBirth);
    EXPECT_STREQ(spec.name, "cb_family_birth");
    EXPECT_EQ(spec.channel, ContributionChannel::SharedRows);
    EXPECT_EQ(spec.unit, ContributionUnit::Rows);
    EXPECT_EQ(spec.subject, ContributionSubject::Satellite);
}

// The kept-epoch bookkeeping of the skips: each counted per (constellation, band); a birth
// with no target declared as a "no_reference" event, a seeded or carried satellite not
// declared (READMIT declares the seed).  A loosened row says so in its detail.
TEST(CodeDatumFamilyTest, KeptEpochCountsBirthSkipsAndDeclaresNoReference) {
    LogManager logman;
    GnssConfig cfg;
    ASSERT_TRUE(loadWith("", cfg, logman, "kept_skip"));
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());
    EpochEvents epoch_events;
    std::vector<Event> events;
    const auto sink = [&](const char* tier, const char* event, const std::string& sat,
                          const std::string&, std::optional<double>, std::optional<double>,
                          const char* action, const std::string& detail) {
        events.push_back({tier, event, sat, action, detail});
    };
    const SignalId e1x = *SignalId::fromObsCode(System::Galileo, "C1X");
    NetworkSbasis::FamilyBirthSkipRecord seeded;
    seeded.sat = "E03"; seeded.channel = e1x; seeded.x_signal = "C1X"; seeded.partner = "C1C";
    seeded.why = Skip::Seeded; seeded.side = Side::Partner;
    seeded.outcome = Outcome::FamilyVeteran;
    NetworkSbasis::FamilyBirthSkipRecord carried = seeded;
    carried.sat = "E04"; carried.why = Skip::Carried;
    NetworkSbasis::FamilyBirthSkipRecord noref = seeded;
    noref.sat = "E05"; noref.why = Skip::NoReference; noref.side = Side::Secondary;
    noref.outcome = Outcome::Bridged;
    epoch_events.code_datum.this_epoch_cb_family_birth_skips = {seeded, carried, noref};
    NetworkSbasis::FamilyBirthPrior loose;
    loose.sat = "E06"; loose.channel = e1x; loose.x_signal = "C1X"; loose.partner = "C1C";
    loose.born = Side::Both; loose.loosened = true; loose.mu_m = 0.02; loose.n_veterans = 3;
    loose.sigma_m = 0.1; loose.seen_before = true;
    epoch_events.code_datum.this_epoch_cb_family_births = {loose};
    networkCodeDatumFamilyKeptEpoch(ctx, sink, epoch_events);

    const auto& c = ctx.counters.cb_family_birth_by_band.at({System::Galileo, '1'});
    EXPECT_EQ(c.rows, 1);
    EXPECT_EQ(c.loosened, 1);
    EXPECT_EQ(c.seeded, 1);
    EXPECT_EQ(c.carried, 1);
    EXPECT_EQ(c.no_reference, 1);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].action, "both_born");
    EXPECT_EQ(events[0].detail,
              "E06: C1X-C1C = mu, both columns born (C1X+C1C, a loosened carry, satellite "
              "seen before); mu +0.0200 m over 3 veteran(s); sigma 0.100 m");
    EXPECT_EQ(events[1].tier, "CB_DATUM");
    EXPECT_EQ(events[1].event, "family_birth_prior");
    EXPECT_EQ(events[1].sat, "E05");
    EXPECT_EQ(events[1].action, "no_reference");
    EXPECT_EQ(events[1].detail,
              "E05: C1X-C1C, C1X born with no realized family mean to target (channel "
              "bridged, no veteran): no row");
}

// ===========================================================================
// (i) The channel family key (NetworkSbasis::channelFamilyOf, SPEC_family_gf_datum
//     section 3.1), which the family-difference row reads twice (its secondary-channel
//     test and its partner selection): it is not the RINEX attribute character, since
//     the Galileo primary family is C1C/C5Q/C7Q, whose attributes are 'C','Q','Q'.
// ===========================================================================

TEST(CodeDatumFamilyTest, ChannelFamilyIsNotTheAttributeCharacter) {
    // The Galileo primary family spans different attribute characters.  Keying on the
    // character would split it in two and declare C1C a family of its own.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1C"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5Q"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C7Q"),
              NetworkSbasis::ChannelFamily::Primary);
    // Other discrete components belong to the same family: a receiver tracking the E1
    // data component or the E5a in-phase component is not a third population.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1B"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5I"),
              NetworkSbasis::ChannelFamily::Primary);
    // Only combined I+Q tracking is the secondary family.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1X"),
              NetworkSbasis::ChannelFamily::Secondary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5X"),
              NetworkSbasis::ChannelFamily::Secondary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C7X"),
              NetworkSbasis::ChannelFamily::Secondary);
}

TEST(CodeDatumFamilyTest, GpsCodelessAndCivilChannelsAreOneFamily) {
    // Regression guard.  An attribute-character key would place C2W and C2L in different
    // families and add rows on the GPS postures, which have no family split.  Every GPS
    // channel the field arms register belongs to the primary family.
    for (const char* sig : {"C1C", "C1W", "C1P", "C1S", "C1L",
                            "C2W", "C2L", "C2S", "C2P",
                            "C5Q", "C5I"})
        EXPECT_EQ(NetworkSbasis::channelFamilyOf("G", sig),
                  NetworkSbasis::ChannelFamily::Primary)
            << "GPS " << sig << " must not be a secondary family";
}

TEST(CodeDatumFamilyTest, UnrecognisedConstellationNeverPartitions) {
    // The allow-list of SPEC_family_gf_datum section 7 is enforced in the mapper rather
    // than at the call site: a constellation whose family split has not been validated
    // reports no secondary family, so its census cannot partition and no row is built
    // for it.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("R", "C1X"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("J", "C5X"),
              NetworkSbasis::ChannelFamily::Primary);
    // BeiDou is on the list (the expected next case), so it maps normally.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("C", "C2X"),
              NetworkSbasis::ChannelFamily::Secondary);
}
