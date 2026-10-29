/**
 * @file test_network_mw_reflag.cpp
 * @brief MW re-flag skip (`network.cycle_slip.mw_reflag_skip`, card MRF): one slip frees
 *        a carried arc once.
 *
 * ### The defect
 *
 * At an epoch whose phase carries a receiver loss-of-lock bit (LLI), the MW drain frees
 * the flagged bands of the arc.  The MW detector withholds that epoch's sample and keeps
 * the arc's statistics (the uncombined LLI policy), so the next sample is compared with
 * the pre-slip level.  An unlocked arc flags the step at that sample, one epoch after the
 * LLI; a locked arc absorbs it until its mean has drifted past the lock bound and flags it
 * a few samples later.  Either flag reaches the drain as a new slip, and the drain frees
 * the arc a second time, discarding the re-convergence since the LLI.  The same holds when
 * another response (an IF-TD flag, a gap re-entry) freed the arc at the step.
 *
 * ### The fix
 *
 * The detector reports the epoch each flagged step entered the data
 * (MwSlipResult::step_epoch_s): the first sample outside the gate, or the LLI epoch when
 * that epoch's withheld sample already sat at the new level.  The drain does not free a
 * band again when its last re-level (LocalCarry::relevel_s) is at or after that epoch
 * (networkMwReflagCovers); a band the earlier response did not free is freed as before.
 *
 * ### What is gated here (suite NetworkMwReflag)
 *
 * - The rule, pure: a first detection is never covered; a band is covered only when
 *   re-levelled at or after the step epoch.
 * - The sequence, through the real MW detector (a five-satellite GPS L1C/L2W bed with a
 *   BIA product, so the arcs are single-differenced and lock as on the field), the PASS-A
 *   admission rule and the real MW drain, with the harvest's re-level stamp emulated:
 *   unarmed, the flag of an LLI epoch and the MW flag of the same step each free the
 *   arc (the defect, pinned); armed, the arc is freed once and the census names the skip.
 *   Both the both-band and the one-band LLI shapes of the field, an unlocked and a locked
 *   arc, and an IF-TD-like free without LLI.
 * - The safety edges: a step that enters after the LLI epoch, and a second step inside an
 *   open excursion, are new slips and are freed armed or not.
 * - The loader: default off, parsed, disarmed without mw_detect.
 *
 * Pure unit gates: a hand-fed detector and drain, no estimator.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/slip/SlipQc.hpp"
#include "network/state/Entities.hpp"
#include "network/state/StationParams.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"
#include "slip/MwSlipDetector.hpp"
#include "../support/StationParamsBuilder.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <Eigen/Dense>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

// GPS L1 / L2 wide lane, formed the way the detector forms it (c / |f1 - f2|).
constexpr double kF1Hz  = 1575.42e6;
constexpr double kF2Hz  = 1227.60e6;
constexpr double kLamWl = Constants::SPEED_OF_LIGHT / (kF1Hz - kF2Hz);   // 0.8619 m

constexpr int    kWeek        = 2313;
constexpr double kTow0        = 259200.0;
constexpr double kDtS         = 30.0;    // the field rate
constexpr double kRxWlBiasCyc = 0.37;    // receiver WL bias, common to every satellite

double towOf(int k)     { return kTow0 + kDtS * k; }
double secondsOf(int k) { return kWeek * 604800.0 + towOf(k); }

struct Sat {
    const char* id;
    int    n_wl;         // wide-lane integer
    double sat_bias_m;   // satellite MW bias carried by the BIA product
    double elev_deg;     // constant, so the MW pivot never changes
};

// G01 is the highest satellite, so it is the MW pivot of the group throughout.  G03, a
// non-pivot, is the satellite that slips.
const std::vector<Sat> kSats = {
    { "G01",  3,  0.21, 72.0 },
    { "G02", -2, -0.33, 64.0 },
    { "G03",  7,  0.12, 50.0 },
    { "G04", 11,  0.45, 35.0 },
    { "G05", -4, -0.18, 28.0 },
};
const std::string kSlipSat = "G03";

/// One slip of G03: the epoch it enters the data, the whole cycles each band moves by,
/// and the bands the receiver flags (LLI) at that epoch.
struct Slip {
    int epoch;
    double d1_cyc;
    double d2_cyc;
    std::set<std::string> lli;
};

/// One bed run: when G03 rises, its slips, the epochs at which another drain (an IF-TD
/// flag, for example) frees both of its bands, the run length, the MW noise, and single
/// noise draws of G03's MW at chosen epochs.
struct Scenario {
    int g03_first{ 0 };
    std::vector<Slip> slips;
    std::set<int> other_frees;
    int n_epochs{ 0 };
    /// Half-width of every satellite's uniform MW noise (m).  The default 1 cm sits below
    /// the detector's 8 cm gate floor, so nothing but a slip leaves the gate.
    double noise_half_m{ 0.01 };
    /// Extra MW of G03 at an epoch (m), carried by its code (both codes move by the
    /// opposite amount, so the phases, and with them the IF test, see nothing).
    std::map<int, double> g03_mw_extra_m;
};

// Deterministic bounded noise: splitmix64 of (satellite, epoch), uniform in +-half_m.
double noiseM(std::size_t sat_idx, int epoch, double half_m) {
    std::uint64_t z = (static_cast<std::uint64_t>(sat_idx) << 32)
                    ^ static_cast<std::uint64_t>(epoch);
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= (z >> 31);
    const double u = static_cast<double>(z >> 11) / 9007199254740992.0;  // [0, 1)
    return half_m * (2.0 * u - 1.0);
}

// One satellite at one epoch: phase in cycles, code in metres, built so that the
// detector's MW combination, lambda_WL * (L1 - L2) - (f1*C1 + f2*C2) / (f1 + f2),
// equals lambda_WL * (N_WL + receiver bias) + satellite bias + noise, plus
// lambda_WL * (d1 - d2) after a slip of (d1, d2) cycles, plus G03's extra draw.
PppObservation obsAt(std::size_t i, int k, const Scenario& sc) {
    const Sat&   s   = kSats[i];
    const double rho = 2.2e7 + 1.0e5 * static_cast<double>(i);
    const double mw  = kLamWl * (s.n_wl + kRxWlBiasCyc) + s.sat_bias_m
                     + noiseM(i, k, sc.noise_half_m);
    double l2 = 0.8e7 + 1.0e4 * static_cast<double>(i);
    double l1 = l2 + (mw + rho) / kLamWl;
    double code = rho;

    PppObservation o{};
    o.satellite = s.id;
    o.system    = System::GPS;
    o.time      = GnssTime{ kWeek, towOf(k) };
    o.elevation = s.elev_deg;
    o.azimuth   = 0.0;
    if (s.id == kSlipSat) {
        for (const Slip& sl : sc.slips) {
            if (k < sl.epoch) continue;
            l1 += sl.d1_cyc;
            l2 += sl.d2_cyc;
            if (k == sl.epoch)
                for (const auto& band : sl.lli) o.lli_flags[band] = 1;
        }
        if (const auto it = sc.g03_mw_extra_m.find(k); it != sc.g03_mw_extra_m.end())
            code -= it->second;
    }
    const std::map<std::string, double> m = {
        { "L1C", l1 }, { "L2W", l2 }, { "C1C", code }, { "C2W", code } };
    o.raw_observed = m;
    o.observed     = m;
    return o;
}

// MW satellite bias = (f1*bL1 - f2*bL2)/(f1 - f2) - (f1*bC1 + f2*bC2)/(f1 + f2).  Zero
// phase biases and both code biases at -b give +b; the code entries are non-zero because
// the bias index treats a zero code bias as missing.
std::vector<BiasData> biaProduct() {
    const GnssTime t0{ kWeek, 0.0 };
    const GnssTime t1{ kWeek, 604799.0 };
    std::vector<BiasData> v;
    for (const auto& s : kSats) {
        v.push_back({ s.id, "L1C", 0.0, 0.0, t0, t1 });
        v.push_back({ s.id, "L2W", 0.0, 0.0, t0, t1 });
        v.push_back({ s.id, "C1C", -s.sat_bias_m, 0.0, t0, t1 });
        v.push_back({ s.id, "C2W", -s.sat_bias_m, 0.0, t0, t1 });
    }
    return v;
}

std::shared_ptr<spdlog::logger> nullLogger() {
    return std::make_shared<spdlog::logger>(
        "test_network_mw_reflag", std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// A minimal loadable network config with the MW detector armed and the skip optional.
std::string reflagConfig(bool mw_detect, bool skip) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"cycle_slip\": { \"mw_detect\": " + (mw_detect ? "true" : "false")
        + (skip ? ", \"mw_reflag_skip\": true" : "")
        + " },\n"
          "    \"stations\": [\n"
          "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
          "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
          "    ]\n"
          "  }\n"
          "}\n";
}

bool loadConfig(const std::string& tag, const std::string& body, GnssConfig& cfg,
                LogManager& log) {
    const fs::path p = gnss_test::tempDir() / ("mw_reflag_cfg_" + tag + ".json");
    { std::ofstream f(p); f << body; }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// One station's MW detector and MW drain, epoch by epoch, with the harvest's re-level
/// stamp emulated: every arc the drain (or an emulated other drain) frees at an epoch is
/// re-levelled at that epoch.  The detector's results go through the PASS-A admission rule
/// exactly as passAMwAdmit applies it.  The traces object's streams stay closed, so the
/// event rows are no-ops; the counters are the census.
struct ReflagBed {
    LogManager log_manager;
    GnssConfig config;
    bool loaded{ false };
    std::unique_ptr<ProductParser>   pp;
    std::unique_ptr<RinexParser>     rp;
    std::unique_ptr<GnssCorrections> corr;
    std::unique_ptr<MwSlipDetector>  det;
    std::vector<NetStation> stations;
    std::vector<bool> present = std::vector<bool>(1, true);
    net::ArcTable arcs;
    std::set<std::pair<int, std::string>> funnel;
    RawSlipEpoch raw;                     // not computed: the LLI stays decisive
    SlipQcParams qc;
    std::vector<RawSlipHistory> hist = std::vector<RawSlipHistory>(1);
    SlipQcCounters cnt;
    SyncedEpoch se;
    NetworkRunTraces traces{ gnss_test::tempDir() / "gnss_mw_reflag_test_unused" };
    std::shared_ptr<spdlog::logger> log{ nullLogger() };
    Scenario sc;
    std::map<std::string, std::vector<int>> frees;    ///< amb key -> epochs the arc was freed
    std::map<int, std::vector<MwSlipResult>> flags;   ///< epoch -> G03's flagged-pair results

    ReflagBed(bool armed, Scenario scenario) : sc(std::move(scenario)) {
        loaded = loadConfig(armed ? "on" : "off", reflagConfig(true, armed), config,
                            log_manager);
        pp   = std::make_unique<ProductParser>(config, log_manager);
        rp   = std::make_unique<RinexParser>(config, log_manager, *pp);
        corr = std::make_unique<GnssCorrections>(
            log_manager, config, nullptr, nullptr,
            std::make_shared<const std::vector<BiasData>>(biaProduct()), *rp);
        det  = std::make_unique<MwSlipDetector>(config, log_manager);
        det->setCorrections(corr.get());

        stations.emplace_back();
        NetStation& st = stations.back();
        st.name   = "AAAA";
        st.params = startStationParams(config, Eigen::Vector3d::Zero());
        for (const auto& s : kSats)
            for (const char* band : { "L1C", "L2W" }) {
                gnss_test::bornAmbiguity(st.params, s.id, band);
                // The carry is born at the satellite's first epoch.
                const int first = (s.id == kSlipSat) ? sc.g03_first : 0;
                net::putArcIn(arcs, { 0, s.id, band }, &net::ArcState::amb).relevel_s =
                    secondsOf(first);
            }
    }
    ReflagBed(const ReflagBed&) = delete;
    ReflagBed& operator=(const ReflagBed&) = delete;

    void epoch(int k) {
        std::vector<PppObservation> obs;
        for (std::size_t i = 0; i < kSats.size(); ++i)
            if (kSats[i].id != kSlipSat || k >= sc.g03_first) obs.push_back(obsAt(i, k, sc));
        const std::vector<MwSlipResult> results = det->detectLargeMwResets(obs);

        PassABuffers pa(stations.size());
        for (const auto& r : results) {
            if (r.sig1.empty() || r.sig2.empty()) continue;
            if (r.satellite == kSlipSat) flags[k].push_back(r);
            const MwAdmitDecision d = networkMwAdmitCandidate(
                r.slip_wl_known, r.slip_wl_cycles, r.slip_frac,
                config.networkCycleSlipTurboEditFracMax(), r.evidence_ratio, r.arc_epochs,
                r.bia_corrected, config.networkCycleSlipMwEvidenceRefloat(),
                config.networkCycleSlipMwEvidenceRatioMin(),
                config.networkCycleSlipMwEvidenceMinArcEpochs());
            if (d.admitted)
                pa.sta_mw_refloat[0].push_back(PassABuffers::MwRefloatCand{
                    r.satellite, r.sig1, r.sig2, r.slip_wl_cycles, r.slip_frac, r.lambda_wl,
                    r.slip_wl_known, r.evidence_ratio, r.arc_epochs, d.via_evidence,
                    r.step_epoch_s });
        }

        RelevelEpochState relevel;
        const std::vector<std::vector<PppObservation>> modeled{ obs };
        se.time = GnssTime{ kWeek, towOf(k) };
        networkSlipDrainMwRefloat(pa, config, stations, present, modeled, /*i_lo=*/0,
                                  /*n_sta_total=*/1, /*carry_invariant=*/false,
                                  /*carry_warm=*/true, funnel, raw, qc, hist, cnt, se,
                                  traces, log, nullptr, nullptr, nullptr, nullptr,
                                  &relevel, /*relevel_pend=*/nullptr,
                                  /*bridge_arcs=*/nullptr, /*oracle=*/nullptr,
                                  /*common_band=*/nullptr, &arcs);
        // Another drain's response at this epoch (an IF-TD flag): both bands freed.
        if (sc.other_frees.count(k)) {
            relevel.free_ambs.insert({ 0, kSlipSat + "_L1C" });
            relevel.free_ambs.insert({ 0, kSlipSat + "_L2W" });
        }
        // The harvest's re-level stamp: an arc freed at this epoch is re-levelled at it.
        for (const auto& [i, key] : relevel.free_ambs) {
            frees[key].push_back(k);
            const auto us = key.find('_');
            net::putArcIn(arcs, { i, key.substr(0, us), key.substr(us + 1) },
                          &net::ArcState::amb).relevel_s = secondsOf(k);
        }
    }

    void run() {
        for (int k = 0; k < sc.n_epochs; ++k) epoch(k);
    }

    std::vector<int> freed(const std::string& band) const {
        const auto it = frees.find(kSlipSat + "_" + band);
        return it == frees.end() ? std::vector<int>{} : it->second;
    }
};

using Epochs = std::vector<int>;

}  // namespace

// ---------------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------------

// A step of this epoch is a first detection: its own responses free it, so no earlier
// re-level can cover it.  An unknown step epoch (0) is never covered.
TEST(NetworkMwReflag, RuleNeverCoversAFirstDetection) {
    const double now = secondsOf(10);
    EXPECT_FALSE(networkMwStepIsEarlier(now, now));
    EXPECT_FALSE(networkMwStepIsEarlier(0.0, now));
    EXPECT_TRUE(networkMwStepIsEarlier(secondsOf(9), now));
    EXPECT_FALSE(networkMwReflagCovers(now, now, secondsOf(9)));
    EXPECT_FALSE(networkMwReflagCovers(0.0, now, secondsOf(9)));
}

// A step of an earlier epoch is covered only by a re-level at or after that epoch; a
// re-level before it (the arc's own birth, an older slip) and an unstamped carry (0, a
// warm-resume-restored entry) are not.
TEST(NetworkMwReflag, RuleCoversOnlyAReLevelAtOrAfterTheStep) {
    const double now  = secondsOf(10);
    const double step = secondsOf(7);
    EXPECT_TRUE(networkMwReflagCovers(step, now, secondsOf(7)));
    EXPECT_TRUE(networkMwReflagCovers(step, now, secondsOf(9)));
    EXPECT_FALSE(networkMwReflagCovers(step, now, secondsOf(6)));
    EXPECT_FALSE(networkMwReflagCovers(step, now, 0.0));
    // Two stamps of one epoch compare equal within the tolerance.
    EXPECT_TRUE(networkMwReflagCovers(step + 0.3, now, step));
    EXPECT_FALSE(networkMwReflagCovers(step + 0.7, now, step));
}

// ---------------------------------------------------------------------------------
// The sequence: an LLI epoch and the MW flag of the same step
// ---------------------------------------------------------------------------------

// Field shape of BSHM00ISR / G19 at 05:30 (TOW 279000): a young arc (8 samples after a
// gap), the receiver flags L2W only, L2W moves by 9 cycles.  The LLI frees L2W at its
// epoch; the first sample after it flags the 9-cycle step against the pre-slip level,
// one epoch later.  Unarmed, that flag frees L2W again (and L1C, which MW cannot clear).
// Armed, L2W is freed once and L1C is still freed at the MW flag.
TEST(NetworkMwReflag, OneBandLliOnAYoungArcFreesTheFlaggedBandOnce) {
    Scenario sc;
    sc.g03_first = 120;
    sc.slips     = { Slip{ 128, 0.0, 9.0, { "L2W" } } };
    sc.n_epochs  = 140;

    ReflagBed off(false, sc), on(true, sc);
    ASSERT_TRUE(off.loaded);
    ASSERT_TRUE(on.loaded);
    off.run();
    on.run();

    // The detector: one flag, at the first sample after the LLI epoch, which reports the
    // LLI epoch as the epoch the step entered.
    ASSERT_EQ(off.flags.size(), 1u);
    ASSERT_EQ(off.flags.begin()->first, 129);
    for (const auto& r : off.flags.at(129)) {
        EXPECT_TRUE(r.slip_wl_known);
        EXPECT_EQ(r.slip_wl_cycles, -9);
        EXPECT_DOUBLE_EQ(r.step_epoch_s, secondsOf(128));
    }

    // Unarmed: L2W freed by the LLI and again by the MW flag (the defect).
    EXPECT_EQ(off.freed("L2W"), (Epochs{ 128, 129 }));
    EXPECT_EQ(off.freed("L1C"), (Epochs{ 129 }));
    EXPECT_EQ(off.cnt.mw_reflag_candidates, 1);
    EXPECT_EQ(off.cnt.mw_reflag_bands_covered, 1);
    EXPECT_EQ(off.cnt.mw_reflag_bands_open, 1);

    // Armed: L2W freed once; L1C, not freed at the LLI epoch, still freed by the flag.
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 128 }));
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 129 }));
    EXPECT_EQ(on.cnt.mw_reflag_candidates, 1);
    EXPECT_EQ(on.cnt.mw_reflag_bands, 2);
    EXPECT_EQ(on.cnt.mw_reflag_bands_covered, 1);
    EXPECT_EQ(on.cnt.mw_reflag_bands_open, 1);
    EXPECT_EQ(on.cnt.mw_reflag_all_covered, 0);
    EXPECT_DOUBLE_EQ(on.cnt.mw_reflag_delay_max_s, kDtS);
}

// Both bands flagged and moved, on a young arc: armed, the MW flag frees nothing.
TEST(NetworkMwReflag, BothBandLliOnAYoungArcFreesTheArcOnce) {
    Scenario sc;
    sc.g03_first = 120;
    sc.slips     = { Slip{ 128, 20.0, 5.0, { "L1C", "L2W" } } };
    sc.n_epochs  = 140;

    ReflagBed off(false, sc), on(true, sc);
    ASSERT_TRUE(off.loaded);
    ASSERT_TRUE(on.loaded);
    off.run();
    on.run();

    EXPECT_EQ(off.freed("L1C"), (Epochs{ 128, 129 }));
    EXPECT_EQ(off.freed("L2W"), (Epochs{ 128, 129 }));
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 128 }));
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 128 }));
    EXPECT_EQ(on.cnt.mw_reflag_all_covered, 1);
    // A fully covered candidate is not a wide-lane detection of the armed drain.
    EXPECT_EQ(on.cnt.mw_detections, off.cnt.mw_detections - 1);
    EXPECT_EQ(on.cnt.mw_refloats, off.cnt.mw_refloats - 1);
}

// ---------------------------------------------------------------------------------
// The locked arc: the step is absorbed, demoted, then flagged four samples late
// ---------------------------------------------------------------------------------

namespace {

/// The extra MW (m) that puts G03's single difference (against the pivot G01) at epoch
/// @p k exactly @p cycles above the bed's level plus its noise: the noise draw of a test.
double drawToCycles(const Scenario& sc, int k, double cycles) {
    const std::size_t i_g03 = 2, i_g01 = 0;
    return kLamWl * cycles
        - (noiseM(i_g03, k, sc.noise_half_m) - noiseM(i_g01, k, sc.noise_half_m));
}

/// The locked-arc shape: G03 locked for 130 epochs (count past the 100-sample Hatch
/// cap), a 15 wide-lane cycle step at epoch 130.  The arc absorbs the step (a locked
/// arc treats an exceedance as an outlier candidate), its mean drifts 0.15, 0.30 and
/// 0.45 cycles over three samples, the third passes the 0.4-cycle lock bound and demotes
/// the lock with an unknown integer (refused by the admission rule), and the fourth
/// sample flags on the unlocked arc against the partly drifted mean.  Its residual is
/// about 0.45 cycle, so it is admitted only on a noise draw that brings it inside the
/// repair gate: the draw below, which cancels the mean's drift, 15 (1 - 0.99^3) cycles.
/// The bed noise is 8 cm, so the draw stays inside the 0.74 m gate of the arc's own
/// time difference and is not a new step (the field's gate at BSHM00ISR / G19 was 1.4 m).
Scenario lockedArcScenario(bool lli) {
    Scenario sc;
    sc.noise_half_m = 0.08;
    sc.slips = { Slip{ 130, 20.0, 5.0,
                       lli ? std::set<std::string>{ "L1C", "L2W" } : std::set<std::string>{} } };
    // The first sample that flags on the unlocked arc: 134 after an LLI epoch (the LLI
    // epoch's sample is withheld), 133 without one.
    const int k_flag = lli ? 134 : 133;
    sc.g03_mw_extra_m[k_flag] = drawToCycles(sc, k_flag, 15.0 * (1.0 - 0.99 * 0.99 * 0.99));
    if (!lli) sc.other_frees = { 130 };   // the IF-TD flag frees both bands at the step
    sc.n_epochs = 140;
    return sc;
}

/// Epochs at which G03's lane flagged, with whether each flag carried a usable integer.
std::map<int, bool> flagEpochs(const ReflagBed& bed) {
    std::map<int, bool> out;
    for (const auto& [k, rows] : bed.flags)
        for (const auto& r : rows) out[k] = r.slip_wl_known;
    return out;
}

}  // namespace

// Field shape of BSHM00ISR / G19 at 04:24 (TOW 275040): both bands flagged and moved on a
// locked arc.  The LLI frees the arc at 130; the arc demotes at 133 (a band-scope flag,
// refused) and flags at 134 (admitted), which frees it again unarmed.  Armed, once.
TEST(NetworkMwReflag, LockedArcLliFlagFourSamplesLateFreesTheArcOnce) {
    const Scenario sc = lockedArcScenario(/*lli=*/true);
    ReflagBed off(false, sc), on(true, sc);
    ASSERT_TRUE(off.loaded);
    ASSERT_TRUE(on.loaded);
    off.run();
    on.run();

    // The detector: the demotion at 133 (unknown integer) and the unlocked flag at 134
    // (known), both reporting the LLI epoch as the step's.
    EXPECT_EQ(flagEpochs(off), (std::map<int, bool>{ { 133, false }, { 134, true } }));
    for (const auto& [k, rows] : off.flags)
        for (const auto& r : rows) EXPECT_DOUBLE_EQ(r.step_epoch_s, secondsOf(130)) << k;

    EXPECT_EQ(off.freed("L1C"), (Epochs{ 130, 134 }));
    EXPECT_EQ(off.freed("L2W"), (Epochs{ 130, 134 }));
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 130 }));
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 130 }));
    EXPECT_EQ(on.cnt.mw_reflag_candidates, 1);
    EXPECT_EQ(on.cnt.mw_reflag_all_covered, 1);
    EXPECT_DOUBLE_EQ(on.cnt.mw_reflag_delay_max_s, 4.0 * kDtS);
}

// No LLI: the IF-TD flag frees the arc at the step (emulated), the locked arc flags the
// same step three samples later.  Unarmed, a second free; armed, none.
TEST(NetworkMwReflag, LockedArcAfterAnIfTdFreeFreesTheArcOnce) {
    const Scenario sc = lockedArcScenario(/*lli=*/false);
    ReflagBed off(false, sc), on(true, sc);
    ASSERT_TRUE(off.loaded);
    ASSERT_TRUE(on.loaded);
    off.run();
    on.run();

    EXPECT_EQ(flagEpochs(off), (std::map<int, bool>{ { 132, false }, { 133, true } }));
    for (const auto& [k, rows] : off.flags)
        for (const auto& r : rows) EXPECT_DOUBLE_EQ(r.step_epoch_s, secondsOf(130)) << k;

    EXPECT_EQ(off.freed("L1C"), (Epochs{ 130, 133 }));
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 130 }));
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 130 }));
    EXPECT_EQ(on.cnt.mw_reflag_all_covered, 1);
}

// A second step inside the open excursion (one epoch after the first, freed by nobody)
// moves the step epoch to it: the late flag is a new slip and is freed, armed or not.
// The steps are 15 and 4 cycles; the mean drifts 0.15, 0.34 and 0.52 cycles, demotes at
// 132, and the draw at 133 cancels that drift so the flag rounds to 19 cycles.
TEST(NetworkMwReflag, ASecondStepInsideTheExcursionIsANewSlip) {
    Scenario sc = lockedArcScenario(/*lli=*/false);
    sc.slips.push_back(Slip{ 131, 4.0, 0.0, {} });
    const double m1 = 0.15, m2 = m1 + (19.0 - m1) / 100.0, m3 = m2 + (19.0 - m2) / 100.0;
    sc.g03_mw_extra_m = { { 133, drawToCycles(sc, 133, m3) } };
    ReflagBed on(true, sc);
    ASSERT_TRUE(on.loaded);
    on.run();

    EXPECT_EQ(flagEpochs(on), (std::map<int, bool>{ { 132, false }, { 133, true } }));
    for (const auto& [k, rows] : on.flags)
        for (const auto& r : rows) EXPECT_DOUBLE_EQ(r.step_epoch_s, secondsOf(131)) << k;
    // The IF-TD free at 130 predates the second step, so the flag at 133 frees again.
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 130, 133 }));
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 130, 133 }));
    EXPECT_EQ(on.cnt.mw_reflag_candidates, 1);
    EXPECT_EQ(on.cnt.mw_reflag_bands_covered, 0);
    EXPECT_EQ(on.cnt.mw_reflag_bands_open, 2);
}

// ---------------------------------------------------------------------------------
// The safety edges: new slips are freed, armed or not
// ---------------------------------------------------------------------------------

// The receiver flags an epoch whose sample has not moved, and the arc steps at the next
// epoch: the step entered after the LLI epoch, so the LLI's free does not cover it.
TEST(NetworkMwReflag, AStepAfterTheLliEpochIsANewSlip) {
    Scenario sc;
    sc.g03_first = 120;
    sc.slips     = { Slip{ 128, 0.0, 0.0, { "L1C", "L2W" } },
                     Slip{ 129, 12.0, 0.0, {} } };
    sc.n_epochs  = 140;

    ReflagBed on(true, sc);
    ASSERT_TRUE(on.loaded);
    on.run();

    ASSERT_TRUE(on.flags.count(129));
    for (const auto& r : on.flags.at(129)) EXPECT_DOUBLE_EQ(r.step_epoch_s, secondsOf(129));
    EXPECT_EQ(on.freed("L1C"), (Epochs{ 128, 129 }));
    EXPECT_EQ(on.freed("L2W"), (Epochs{ 128, 129 }));
    EXPECT_EQ(on.cnt.mw_reflag_candidates, 0);
}

// ---------------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------------

TEST(NetworkMwReflag, LoaderDefaultsOffParsesAndNeedsTheDetector) {
    LogManager log;
    GnssConfig def, on, orphan;
    ASSERT_TRUE(loadConfig("def", reflagConfig(true, false), def, log));
    ASSERT_TRUE(loadConfig("on", reflagConfig(true, true), on, log));
    ASSERT_TRUE(loadConfig("orphan", reflagConfig(false, true), orphan, log));
    EXPECT_FALSE(def.networkCycleSlipMwReflagSkip());
    EXPECT_TRUE(on.networkCycleSlipMwReflagSkip());
    EXPECT_FALSE(orphan.networkCycleSlipMwReflagSkip());   // disarmed: no detector
}
