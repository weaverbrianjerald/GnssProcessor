/**
 * @file test_mw_pivot_reset.cpp
 * @brief MwSlipDetector: an external reset of the single-difference MW pivot,
 *        followed by the election of a different pivot.
 *
 * With a satellite-bias (BIA) product loaded, @c detectLargeMwResets single-
 * differences every BIA-corrected arc of a (constellation, signal pair) group
 * against one pivot satellite.  A non-pivot arc then carries
 * (N_WL(sat) - N_WL(pivot)) * lambda_WL, and the pivot arc carries its own
 * undifferenced value, receiver bias included.  When the elected pivot changes,
 * the pivot-change block re-references the surviving arcs to the new pivot
 * (S-transform) or erases them, and drops the old pivot's undifferenced arc.
 * That block runs only when a previous pivot is stored for the group.
 *
 * @c resetSatellite(pivot) rolls the pivot's own arc back and erases the stored
 * pivot entry.  It leaves the group's other arcs as they are.  When the next
 * pass elects a different pivot, the pivot-change block is skipped: every
 * surviving arc keeps a mean in the old pivot's frame while its samples arrive
 * in the new pivot's frame, a step of (N_WL(old) - N_WL(new)) * lambda_WL.
 *
 * The bed is six GPS L1C/L2W satellites with a BIA product built in memory, a
 * receiver wide-lane bias of 0.37 cycles, and bounded MW noise (|n| <= 1 cm,
 * below the detector's absolute threshold floor of 8 x 1 cm, so no reference
 * mode can flag during warm-up).  The pivot changes by an elevation crossing at
 * epoch kSwitch, G01 setting below G02.  G06 rises five epochs before the
 * change, so it is the one survivor whose arc is not yet integer-locked.  No
 * satellite slips at any epoch.  The arms differ in one call:
 * @c resetSatellite("G01") after the pass of epoch kSwitch - 1, the position of
 * the KF post-fit handler's reset in the epoch.
 *
 *   - ControlHandoverIsSilent: without the reset the S-transform runs.  No
 *     satellite flags, the locks move to the G02 frame at the change, and the
 *     detector's zero-difference reconstruction of a survivor stays continuous.
 *   - ResetThenNewPivotFlagsTheWholeGroup: with the reset, the current
 *     behaviour, epoch by epoch.  This case pins the defect; a fix lands
 *     default-OFF and adds its own arm next to it.
 *   - EveryReferenceModeFlagsTheWholeGroup: the two arms under each of the
 *     three MW reference modes.
 *   - OldPivotUnobservedAtTheChangeFlagsOnReturn: a related gap on the ordinary
 *     handover path, no reset involved.  The pivot-change block drops the old
 *     pivot's undifferenced arc only when that satellite is observed at the
 *     change; unobserved, the arc survives and meets a single-difference sample
 *     when the satellite returns as a non-pivot.
 *
 * Pure unit gates: a hand-fed detector, no estimator, no network.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "products/ProductParser.hpp"
#include "products/ProductTypes.hpp"
#include "rinex/RinexParser.hpp"
#include "slip/MwSlipDetector.hpp"

using namespace GnssProcessor;

namespace {

// GPS L1 / L2 wide lane, formed the way the detector forms it (c / |f1 - f2|).
constexpr double kF1Hz  = 1575.42e6;
constexpr double kF2Hz  = 1227.60e6;
constexpr double kLamWl = Constants::SPEED_OF_LIGHT / (kF1Hz - kF2Hz);  // 0.8619 m

constexpr int    kWeek        = 2400;
constexpr double kTow0        = 345600.0;
constexpr int    kSwitch      = 40;     // first epoch at which G02 out-ranks G01
constexpr int    kPost        = 30;     // epochs observed from kSwitch on
constexpr double kRxWlBiasCyc = 0.37;   // receiver WL bias, common to every satellite
constexpr double kNoiseHalfM  = 0.01;   // MW noise on each satellite, uniform in +-1 cm

struct Sat {
    const char* id;
    int    n_wl;         // wide-lane integer
    double sat_bias_m;   // satellite MW bias carried by the BIA product
    double elev0_deg;    // elevation at epoch 0
    double elev_rate;    // degrees per epoch
    int    first_epoch;  // first epoch the satellite is observed
};

// G01 sets through G02 between epochs 39 and 40 (68.10 > 67.95, then 68.00 < 68.05).
// Single-difference integers against G01: G02 -5, G03 4, G04 8, G05 -7, G06 -9.
// Against G02: G01 5, G03 9, G04 13, G05 -2, G06 -4.  The frame step is
// N_WL(G01) - N_WL(G02) = +5 cycles on every survivor.
const std::vector<Sat> kSats = {
    { "G01",  3,  0.21, 72.00, -0.10, 0 },
    { "G02", -2, -0.33, 64.05,  0.10, 0 },
    { "G03",  7,  0.12, 50.00,  0.00, 0 },
    { "G04", 11,  0.45, 35.00,  0.00, 0 },
    { "G05", -4, -0.18, 28.00,  0.00, 0 },
    { "G06", -6,  0.07, 15.00,  0.00, kSwitch - 5 },
};

std::string arcKey(const std::string& sat) { return sat + "_L1C_L2W"; }

std::size_t satIndex(const std::string& sat) {
    for (std::size_t i = 0; i < kSats.size(); ++i)
        if (sat == kSats[i].id) return i;
    return kSats.size();
}

// Deterministic bounded noise: splitmix64 of (satellite, epoch), mapped to a
// uniform value in [-kNoiseHalfM, +kNoiseHalfM).
double noiseM(std::size_t sat_idx, int epoch) {
    std::uint64_t z = (static_cast<std::uint64_t>(sat_idx) << 32)
                    ^ static_cast<std::uint64_t>(epoch);
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= (z >> 31);
    const double u = static_cast<double>(z >> 11) / 9007199254740992.0;  // [0, 1)
    return kNoiseHalfM * (2.0 * u - 1.0);
}

// One satellite at one epoch: phase in cycles, code in metres, built so that the
// detector's MW combination, lambda_WL * (L1 - L2) - (f1*C1 + f2*C2) / (f1 + f2),
// equals lambda_WL * (N_WL + receiver bias) + satellite bias + noise.
PppObservation obsAt(std::size_t i, int k) {
    const Sat&   s   = kSats[i];
    const double rho = 2.2e7 + 1.0e5 * static_cast<double>(i);
    const double mw  = kLamWl * (s.n_wl + kRxWlBiasCyc) + s.sat_bias_m + noiseM(i, k);
    const double l2  = 0.8e7 + 1.0e4 * static_cast<double>(i);
    const double l1  = l2 + (mw + rho) / kLamWl;

    PppObservation o{};
    o.satellite = s.id;
    o.system    = System::GPS;
    o.time      = GnssTime{ kWeek, kTow0 + k };
    o.elevation = s.elev0_deg + s.elev_rate * k;
    o.azimuth   = 0.0;
    const std::map<std::string, double> m = {
        { "L1C", l1 }, { "L2W", l2 }, { "C1C", rho }, { "C2W", rho } };
    o.raw_observed = m;
    o.observed     = m;
    return o;
}

// MW satellite bias = (f1*bL1 - f2*bL2)/(f1 - f2) - (f1*bC1 + f2*bC2)/(f1 + f2).
// Zero phase biases and both code biases at -b give +b.  The code entries are
// non-zero on purpose: the GnssCorrections bias index treats a zero code bias as
// missing, and a missing entry takes the satellite out of the SD group.
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

// A detector wired to the in-memory BIA product.  GnssCorrections keeps pointers
// to the config and the RINEX parser, so the bed owns everything and is never
// copied.  The LogManager stays bare: initialize() would clash with the spdlog
// registry of other suites.
struct Bed {
    LogManager log;
    GnssConfig cfg;
    bool loaded{ true };
    std::unique_ptr<ProductParser>   pp;
    std::unique_ptr<RinexParser>     rp;
    std::unique_ptr<GnssCorrections> corr;
    std::unique_ptr<MwSlipDetector>  det;

    explicit Bed(const std::string& detect_reference = {}) {
        if (!detect_reference.empty()) {
            const auto path = std::filesystem::temp_directory_path()
                / ("gnss_mw_pivot_reset_" + detect_reference + ".json");
            {
                std::ofstream f(path);
                f << R"({ "estimation": { "cycle_slip": { "mw_detect_reference": ")"
                  << detect_reference << R"(" } } })";
            }
            loaded = cfg.loadFromJson(path, log);
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        pp   = std::make_unique<ProductParser>(cfg, log);
        rp   = std::make_unique<RinexParser>(cfg, log, *pp);
        corr = std::make_unique<GnssCorrections>(
            log, cfg, nullptr, nullptr,
            std::make_shared<const std::vector<BiasData>>(biaProduct()), *rp);
        det  = std::make_unique<MwSlipDetector>(cfg, log);
        det->setCorrections(corr.get());
    }
    Bed(const Bed&) = delete;
    Bed& operator=(const Bed&) = delete;

    /// One epoch; @p unobserved names a satellite left out of it.
    std::vector<MwSlipResult> step(int k, const std::string& unobserved = {}) {
        std::vector<PppObservation> obs;
        for (std::size_t i = 0; i < kSats.size(); ++i)
            if (k >= kSats[i].first_epoch && unobserved != kSats[i].id)
                obs.push_back(obsAt(i, k));
        return det->detectLargeMwResets(obs);
    }
};

// A flagged satellite, collapsed from its result rows (the uncombined path emits
// one row per band of the pair, both carrying the same integer).
struct Flag {
    int  epoch;   // offset from kSwitch
    bool known;
    int  cycles;
};
using FlagLog = std::map<std::string, std::vector<Flag>>;   // satellite -> flags

void record(FlagLog& log, const std::vector<MwSlipResult>& rows, int k,
            const char* arm) {
    std::map<std::string, Flag> by_sat;
    for (const auto& r : rows)
        by_sat[r.satellite] = Flag{ k - kSwitch, r.slip_wl_known, r.slip_wl_cycles };
    for (const auto& [sat, f] : by_sat) {
        log[sat].push_back(f);
        std::cout << "[mw-pivot-reset] " << arm << " epoch=+" << f.epoch
                  << " sat=" << sat << " known=" << f.known
                  << " dN_WL=" << f.cycles << std::endl;
    }
}

std::optional<int> wlOf(const std::map<std::string, int>& wl, const std::string& sat) {
    const auto it = wl.find(arcKey(sat));
    if (it == wl.end()) return std::nullopt;
    return it->second;
}

// |zero-difference reconstruction of the arc - truth|, in WL cycles; NaN while the
// reconstruction is unavailable (young arc, or a pivot arc with < 2 lagged samples).
// The truth is the BIA-corrected undifferenced MW, lambda_WL * (N_WL + receiver bias).
double zdErrorCyc(const MwSlipDetector& det, const std::string& sat) {
    const auto z = det.arcZdMean(sat, "L1C", "L2W");
    if (!z.valid) return std::numeric_limits<double>::quiet_NaN();
    const double truth = kLamWl * (kSats[satIndex(sat)].n_wl + kRxWlBiasCyc);
    return std::abs(z.mean_m - truth) / kLamWl;
}

void warmUp(Bed& bed) {
    for (int k = 0; k < kSwitch; ++k) {
        const auto rows = bed.step(k);
        ASSERT_TRUE(rows.empty()) << "flag during warm-up at epoch " << k
                                  << " (" << rows.front().satellite << ")";
    }
}

}  // namespace

// ---------------------------------------------------------------------------------
// Control: the pivot changes with the previous pivot stored, and the S-transform
// carries every survivor into the G02 frame without a flag.
// ---------------------------------------------------------------------------------
TEST(MwPivotReset, ControlHandoverIsSilent) {
    Bed bed;
    MwSlipDetector& det = *bed.det;
    warmUp(bed);
    if (HasFatalFailure()) return;

    // Settled in the G01 frame; G06 has five samples and no lock yet.
    auto wl = det.getResolvedWlAmbiguities();
    EXPECT_EQ(wlOf(wl, "G01"), 0);   // the pivot row
    EXPECT_EQ(wlOf(wl, "G02"), -5);
    EXPECT_EQ(wlOf(wl, "G03"), 4);
    EXPECT_EQ(wlOf(wl, "G04"), 8);
    EXPECT_EQ(wlOf(wl, "G05"), -7);
    EXPECT_FALSE(wlOf(wl, "G06").has_value());

    FlagLog flags;
    double max_zd_err = 0.0;
    int    n_zd       = 0;
    for (int k = kSwitch; k < kSwitch + kPost; ++k) {
        record(flags, bed.step(k), k, "control");
        if (k == kSwitch) {
            const auto& st = det.getEpochPairStatus();
            for (const char* s : { "G03", "G04", "G05", "G06" }) {
                ASSERT_TRUE(st.count(arcKey(s))) << s;
                EXPECT_FALSE(st.at(arcKey(s)).flagged) << s;
            }
            EXPECT_LT(det.getWorstMwScore("G03"), 8.0)
                << "a re-referenced survivor sits at noise level";
            // The locks moved with the transform: G02 is the pivot row, the
            // survivors carry N - N_G02 at once.
            wl = det.getResolvedWlAmbiguities();
            EXPECT_EQ(wlOf(wl, "G02"), 0);
            EXPECT_EQ(wlOf(wl, "G03"), 9);
            EXPECT_EQ(wlOf(wl, "G04"), 13);
            EXPECT_EQ(wlOf(wl, "G05"), -2);
            EXPECT_TRUE(det.getResolvedWlQuality().at(arcKey("G02")).exact);
        }
        for (const char* s : { "G03", "G04", "G05" }) {
            const double e = zdErrorCyc(det, s);
            if (std::isfinite(e)) { max_zd_err = std::max(max_zd_err, e); ++n_zd; }
        }
    }
    EXPECT_TRUE(flags.empty()) << flags.size() << " satellite(s) flagged";
    EXPECT_GT(n_zd, 0) << "the zero-difference reconstruction was never available";
    EXPECT_LT(max_zd_err, 0.1) << "the reconstruction is continuous across the change";

    wl = det.getResolvedWlAmbiguities();
    EXPECT_EQ(wlOf(wl, "G02"), 0);
    EXPECT_EQ(wlOf(wl, "G03"), 9);
    EXPECT_EQ(wlOf(wl, "G04"), 13);
    EXPECT_EQ(wlOf(wl, "G05"), -2);
    EXPECT_EQ(wlOf(wl, "G06"), -4);   // locked after the change, in the G02 frame
    EXPECT_EQ(wlOf(wl, "G01"), 5);    // the old pivot restarted as an SD arc
}

// ---------------------------------------------------------------------------------
// The reset arm, current behaviour.  Every expectation below describes a
// satellite that did not slip.
// ---------------------------------------------------------------------------------
TEST(MwPivotReset, ResetThenNewPivotFlagsTheWholeGroup) {
    Bed bed;
    MwSlipDetector& det = *bed.det;
    warmUp(bed);
    if (HasFatalFailure()) return;

    det.resetSatellite("G01");

    // The reset removed the pivot row and re-referenced nothing: the survivors
    // still carry their G01-frame integers.
    auto wl = det.getResolvedWlAmbiguities();
    EXPECT_FALSE(wlOf(wl, "G01").has_value());
    EXPECT_EQ(wlOf(wl, "G02"), -5);
    EXPECT_EQ(wlOf(wl, "G03"), 4);
    EXPECT_EQ(wlOf(wl, "G04"), 8);
    EXPECT_EQ(wlOf(wl, "G05"), -7);

    FlagLog flags;
    for (int k = kSwitch; k < kSwitch + kPost; ++k) {
        record(flags, bed.step(k), k, "reset");
        if (k != kSwitch) continue;

        // G02 is elected with no stored previous pivot, so nothing is
        // re-referenced: every survivor's gate fires on a sample 5 cycles
        // (N_G01 - N_G02) from its mean.
        const auto& st = det.getEpochPairStatus();
        for (const char* s : { "G03", "G04", "G05", "G06" }) {
            ASSERT_TRUE(st.count(arcKey(s))) << s;
            EXPECT_TRUE(st.at(arcKey(s)).flagged) << s;
        }
        EXPECT_GT(det.getWorstMwScore("G03"), 50.0);

        // The locked survivors absorb the step silently this epoch, so their
        // G01-frame integers stay exposed.  The new pivot's row is its old
        // single-difference integer, not the pivot's 0: its arc was neither
        // dropped nor re-initialised as the pivot.
        wl = det.getResolvedWlAmbiguities();
        EXPECT_EQ(wlOf(wl, "G02"), -5);
        EXPECT_EQ(wlOf(wl, "G03"), 4);
        EXPECT_EQ(wlOf(wl, "G04"), 8);
        EXPECT_EQ(wlOf(wl, "G05"), -7);
        EXPECT_FALSE(det.getResolvedWlQuality().at(arcKey("G02")).exact);
        // The detector's zero-difference reconstruction of G03 adds the new
        // pivot's single-difference mean to G03's G01-frame mean.
        EXPECT_GT(zdErrorCyc(det, "G03"), 1.0);
    }

    // Every satellite of the group flags inside the window.
    for (const auto& s : kSats)
        EXPECT_TRUE(flags.count(s.id)) << s.id << " never flagged";

    // The unlocked survivor publishes the frame step as its own known ZD slip.
    ASSERT_TRUE(flags.count("G06"));
    EXPECT_EQ(flags["G06"].front().epoch, 0);
    EXPECT_TRUE(flags["G06"].front().known);
    EXPECT_EQ(flags["G06"].front().cycles, 5);

    // The old pivot's rolled-back undifferenced arc receives an SD sample.
    ASSERT_TRUE(flags.count("G01"));
    EXPECT_EQ(flags["G01"].front().epoch, 0);

    // The locked survivors drift-demote together, in the same epoch, each with
    // an unknown integer, and restart.
    ASSERT_TRUE(flags.count("G03") && flags.count("G04") && flags.count("G05"));
    const int first = flags["G03"].front().epoch;
    EXPECT_GT(first, 0);
    EXPECT_LE(first, 5);
    for (const char* s : { "G03", "G04", "G05" }) {
        EXPECT_EQ(flags[s].front().epoch, first) << s;
        for (const auto& f : flags[s])
            EXPECT_FALSE(f.known) << s << " at +" << f.epoch;
    }

    // The new pivot's arc is fed undifferenced samples against a single-
    // difference mean: it demotes, then publishes a known ZD slip of its own.
    // At that epoch the undifferenced sample, N_G02 + 0.37 = -1.63 cycles, sits
    // 2.93 cycles above the partly migrated mean (-4.56 cycles), which rounds to
    // a known 3.  Whether the jump rounds depends on the receiver-bias fraction.
    ASSERT_TRUE(flags.count("G02"));
    bool g02_known = false;
    for (const auto& f : flags["G02"])
        if (f.known && f.cycles != 0) g02_known = true;
    EXPECT_TRUE(g02_known) << "the new pivot published no known integer";

    // The group re-converges in the G02 frame once every arc has restarted.
    wl = det.getResolvedWlAmbiguities();
    EXPECT_EQ(wlOf(wl, "G02"), 0);
    EXPECT_EQ(wlOf(wl, "G03"), 9);
    EXPECT_EQ(wlOf(wl, "G04"), 13);
    EXPECT_EQ(wlOf(wl, "G05"), -2);
    EXPECT_EQ(wlOf(wl, "G06"), -4);
    EXPECT_EQ(wlOf(wl, "G01"), 5);
}

// ---------------------------------------------------------------------------------
// Both arms under each MW reference mode: the control stays silent, and after the
// reset every satellite of the group flags.
// ---------------------------------------------------------------------------------
TEST(MwPivotReset, EveryReferenceModeFlagsTheWholeGroup) {
    for (const std::string mode : { "hatch_mean", "local_median", "time_difference" }) {
        for (const bool reset : { false, true }) {
            SCOPED_TRACE(mode + (reset ? " reset" : " control"));
            Bed bed(mode);
            ASSERT_TRUE(bed.loaded);
            warmUp(bed);
            if (HasFatalFailure()) return;
            if (reset) bed.det->resetSatellite("G01");

            FlagLog flags;
            const std::string arm = mode + (reset ? "/reset" : "/control");
            for (int k = kSwitch; k < kSwitch + kPost; ++k)
                record(flags, bed.step(k), k, arm.c_str());

            if (!reset) {
                EXPECT_TRUE(flags.empty()) << flags.size() << " satellite(s) flagged";
            } else {
                for (const auto& s : kSats)
                    EXPECT_TRUE(flags.count(s.id)) << s.id << " never flagged";
            }
        }
    }
}

// ---------------------------------------------------------------------------------
// The ordinary handover, no reset: G01 is unobserved at the change and the next
// epoch (a two-epoch outage, far inside the 120 s gap tolerance) and returns below
// G02.  The change runs against the stored G01 and S-transforms the survivors, but
// G01's undifferenced arc is not dropped, because the drop sits inside the loop
// over the satellites observed at the change.  On its return G01 is a non-pivot,
// and its first single-difference sample, 5 cycles, meets its undifferenced mean,
// 3.37 cycles.  The 1.63-cycle jump rounds to no integer with this bed's 0.37-cycle
// receiver bias; with a receiver-bias fraction under 0.25 cycles the jump estimate
// would publish a known integer instead.
// ---------------------------------------------------------------------------------
TEST(MwPivotReset, OldPivotUnobservedAtTheChangeFlagsOnReturn) {
    Bed bed;
    warmUp(bed);
    if (HasFatalFailure()) return;

    FlagLog flags;
    for (int k = kSwitch; k < kSwitch + kPost; ++k)
        record(flags, bed.step(k, k < kSwitch + 2 ? "G01" : ""), k, "old-pivot-unobserved");

    EXPECT_EQ(flags.size(), 1u) << "only the returning old pivot flags";
    ASSERT_TRUE(flags.count("G01"));
    ASSERT_EQ(flags["G01"].size(), 1u);
    EXPECT_EQ(flags["G01"].front().epoch, 2) << "the first epoch G01 is observed again";
    EXPECT_FALSE(flags["G01"].front().known);

    // Every survivor kept its lock through the change, in the G02 frame.
    const auto wl = bed.det->getResolvedWlAmbiguities();
    EXPECT_EQ(wlOf(wl, "G02"), 0);
    EXPECT_EQ(wlOf(wl, "G03"), 9);
    EXPECT_EQ(wlOf(wl, "G04"), 13);
    EXPECT_EQ(wlOf(wl, "G05"), -2);
}
