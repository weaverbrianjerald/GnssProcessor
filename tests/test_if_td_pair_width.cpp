/**
 * @file test_if_td_pair_width.cpp
 * @brief IF-TD pair width (`network.cycle_slip.if_phase_td_pairs`,
 *        docs/SPEC_event_harness.md §14.10): one pair per satellite against every
 *        pair, and the independence that makes the widening a pure addition.
 *
 * ### The defect
 *
 * The iono-free triple difference is formed over a pair of phase bands, and with
 * the default the detector forms one pair per satellite: the first two distinct
 * bands it finds (L1C+L2W on GPS).  Every other pair is unwatched, so the
 * equal-count case that the common-band scope's IF-TD veto exists to catch is
 * uncaught on all of them.  Two bands slipping by the same integer leave their
 * wide lane silent while the iono-free combination moves 0.107 m per cycle, and a
 * silent MW lane with no IF-TD pair behind it clears two bands that both moved.
 * On the storm bed this made the response free the one band that had not slipped
 * (§14.10).
 *
 * ### What is gated here
 *
 * - @c IfTdPairWidth.First…: the default forms one pair per satellite, and it is
 *   (L1C, L2W) whichever bands the arc carries.
 * - @c IfTdPairWidth.All…: every distinct pair is formed (3 on a triple-band arc,
 *   1 on a dual-band one), each with its own chain key.
 * - The first pair's numbers are identical in both modes, sample for sample and
 *   reset key for reset key: the new pairs do not perturb the pair the default
 *   path already decides on.
 * - Independence: an L5-only step moves the two pairs that contain L5 and leaves
 *   (L1C, L2W) bit-identical; restarting one pair's chain
 *   (@c resetSatelliteInState) restarts that pair alone; and the driver's shared
 *   pool and threshold helpers see only their own pair's samples.
 *
 * The tests drive the public API with synthetic uncombined (PPP-RTK)
 * observations.  Because the IF coefficients satisfy c1 + c2 == 1 exactly, giving
 * every band the same observed-minus-modeled value makes the IF OMC of every pair
 * equal that value.  The quiet stream is therefore a designed quantity for all
 * three pairs at once, and a per-band offset is the only thing that separates
 * them.  Detectors run with setDeferFlagging(true), the network wiring: in that
 * mode only the reference-slip branch pushes reset keys, so a nonempty return
 * from detect() is unambiguous evidence about one chain's reference arm.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/IfPhaseSlipDetector.hpp"
#include "slip/NetworkSlipQc.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

constexpr double kF1 = Constants::GPS_L1_FREQ;
constexpr double kF2 = Constants::GPS_L2_FREQ;
constexpr double kF5 = Constants::GPS_L5_FREQ;

/// The three band pairs of a GPS L1/L2/L5 arc, in the detector's own order
/// (lexical over the observable keys, so (L1C, L2W) is the first pair).
const std::string kP12 = "L1C_L2W";
const std::string kP15 = "L1C_L5Q";
const std::string kP25 = "L2W_L5Q";

/// The low-band IF coefficient c2 = −f_b²/(f_a² − f_b²): what a step on band b
/// alone does to the (a, b) iono-free combination.  |c2| is 1.26 on (1,5) and
/// 11.26 on (2,5), so an L5-only step is large on both and exactly zero on (1,2),
/// which does not contain L5; the independence assertions below rely on this.
double c2Of(double fa, double fb) { return -(fb * fb) / (fa * fa - fb * fb); }

/// Deterministic sub-millimetre wobble (m) so the per-pair MADs are never
/// degenerate.  At most 0.6 mm, more than two orders of magnitude below the
/// ~10 cm physical floor, so it cannot move a verdict.
constexpr double kWobbleM[8] = { 0.0004, -0.0003, 0.0002,  0.0005,
                                -0.0006,  0.0001, -0.0002, 0.0003 };
double wobble(std::size_t epoch, std::size_t sat) {
    return kWobbleM[(epoch * 3u + sat) % 8u];
}

using OmcState  = std::map<std::string, double>;          ///< satellite → common IF OMC (m)
using ElevState = std::map<std::string, double>;          ///< satellite → elevation (deg)
/// satellite → (signal → extra OMC on that band alone), the per-band injector.
using BandOffset = std::map<std::string, std::map<std::string, double>>;

/**
 * @brief One epoch of uncombined triple-band (or dual-band) observations.
 *
 * The model value is zero on every band and the observed value carries the whole
 * OMC.  A satellite's bands all take the same value unless @p extra names one,
 * so the quiet stream gives every pair the identical IF OMC.
 */
std::vector<PppObservation> makeEpoch(double tow, std::size_t epoch_idx,
                                      const OmcState& omc_m,
                                      const ElevState& elev_deg,
                                      const std::vector<std::string>& bands,
                                      const BandOffset& extra = {}) {
    std::vector<PppObservation> epoch;
    epoch.reserve(omc_m.size());
    std::size_t k = 0;
    for (const auto& [sat, omc] : omc_m) {
        const double v = omc + wobble(epoch_idx, k++);
        PppObservation obs{};
        obs.satellite = sat;
        obs.system    = System::GPS;
        obs.time      = GnssTime{ 2400, tow };
        obs.elevation = elev_deg.at(sat);
        obs.azimuth   = 0.0;
        obs.los       = Eigen::Vector3d(0.0, 0.0, 1.0);
        obs.sat_pos   = Eigen::Vector3d::Zero();
        obs.rx_pos_linearization = Eigen::Vector3d::Zero();
        const auto eit = extra.find(sat);
        for (const auto& sig : bands) {
            double add = 0.0;
            if (eit != extra.end()) {
                const auto sit = eit->second.find(sig);
                if (sit != eit->second.end()) add = sit->second;
            }
            obs.modeled[sig]  = 0.0;
            obs.observed[sig] = v + add;
        }
        epoch.push_back(std::move(obs));
    }
    return epoch;
}

/// Detector under test plus the config/log it borrows.  Member order keeps the
/// references alive for the detector.
struct PairHarness {
    LogManager log_manager;
    GnssConfig config;                      // defaults: 10 deg elevation mask
    IfPhaseSlipDetector det{ config, log_manager };
    std::vector<std::string> bands{ "L1C", "L2W", "L5Q" };

    explicit PairHarness(bool all_pairs) {
        det.setDeferFlagging(true);         // the network wiring
        det.setAllPairs(all_pairs);
    }

    std::vector<std::string> step(std::size_t epoch_idx, const OmcState& omc,
                                  const ElevState& elev,
                                  const BandOffset& extra = {}) {
        return det.detect(makeEpoch(30.0 * static_cast<double>(epoch_idx),
                                    epoch_idx, omc, elev, bands, extra));
    }

    /// The cached double difference of one (satellite, pair), or NaN when the
    /// pair produced nothing this epoch.
    double dd(const std::string& sat, const std::string& pair) const {
        const auto it = det.getLastIfJumpsAllPairs().find({ sat, pair });
        return (it == det.getLastIfJumpsAllPairs().end())
                   ? std::numeric_limits<double>::quiet_NaN()
                   : it->second.delta_grad_m;
    }
};

/// Six GPS satellites; G01 is the highest, so every chain elects it, and at 80° it
/// stays above the 45° reference-retention threshold, so every chain keeps it.
/// The reference identity is then a controlled variable rather than an outcome.
ElevState sixSatElevations() {
    return { { "G01", 80.0 }, { "G02", 70.0 }, { "G03", 40.0 },
             { "G04", 35.0 }, { "G05", 30.0 }, { "G06", 25.0 } };
}

OmcState sixSatOmc() {
    return { { "G01", 0.00 }, { "G02", 0.12 }, { "G03", -0.07 },
             { "G04", 0.21 }, { "G05", -0.15 }, { "G06", 0.05 } };
}

/// Bit-identity for a double difference, NaN included.  The first epoch of an
/// arc has no previous gradient and is a designed NaN, so "identical" has to
/// mean identical rather than equal: `nan == nan` is false.
::testing::AssertionResult SameDd(const char* ea, const char* eb,
                                  double a, double b) {
    if ((std::isnan(a) && std::isnan(b)) || a == b) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << ea << " = " << a << " but " << eb << " = " << b;
}

/// Pair labels present for one satellite in the all-pairs cache.
std::vector<std::string> pairsOf(const IfPhaseSlipDetector& det,
                                 const std::string& sat) {
    std::vector<std::string> out;
    for (const auto& [key, jp] : det.getLastIfJumpsAllPairs())
        if (key.first == sat) out.push_back(key.second);
    (void)det;
    return out;
}

}  // namespace

// ------------------------------------------------------- `first`: one pair

/// The default forms one pair per satellite, from the first two distinct bands:
/// (L1C, L2W) on a GPS L1/L2/L5 arc.  The detector does not look at the L5 band
/// at all, which is the defect.
TEST(IfTdPairWidth, FirstFormsExactlyOnePairPerSatellite) {
    PairHarness h(/*all_pairs=*/false);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);

    EXPECT_EQ(h.det.getLastIfJumps().size(), 6u);
    EXPECT_EQ(h.det.getLastIfJumpsAllPairs().size(), 6u);
    for (const auto& [sat, jp] : h.det.getLastIfJumps()) {
        EXPECT_EQ(jp.sig1, "L1C") << sat;
        EXPECT_EQ(jp.sig2, "L2W") << sat;
        // The first pair uses the bare-system chain, so its decisions are those
        // without the knob and a driver reset targets the same chain as before.
        EXPECT_EQ(jp.state_key, "GPS") << sat;
    }
    EXPECT_EQ(pairsOf(h.det, "G03"), (std::vector<std::string>{ kP12 }));
}

/// A band pair the detector never forms cannot flag, whatever the band does: an
/// L5-only step of 40 cycles leaves the one watched pair's double difference
/// exactly where the quiet stream put it.  This is the field case (an unwatched
/// lane) stated as an identity.
TEST(IfTdPairWidth, FirstIsBlindToAnL5OnlyStep) {
    PairHarness quiet(/*all_pairs=*/false), stepped(/*all_pairs=*/false);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    const BandOffset l5_step{ { "G03", { { "L5Q", 40.0 * Constants::GPS_L5_WAVELENGTH } } } };

    for (std::size_t e = 0; e < 3; ++e) {
        quiet.step(e, omc, elev);
        stepped.step(e, omc, elev, e >= 2 ? l5_step : BandOffset{});
    }
    EXPECT_PRED_FORMAT2(SameDd, stepped.dd("G03", kP12), quiet.dd("G03", kP12));
}

// -------------------------------------------------------- `all`: every pair

/// Every distinct pair is formed, each under its own chain key, and the first
/// pair keeps the bare-system key so its chain is untouched.
TEST(IfTdPairWidth, AllFormsEveryPairOnItsOwnChain) {
    PairHarness h(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);

    EXPECT_EQ(h.det.getLastIfJumpsAllPairs().size(), 18u);   // 6 satellites x 3 pairs
    EXPECT_EQ(pairsOf(h.det, "G03"),
              (std::vector<std::string>{ kP12, kP15, kP25 }));
    // getLastIfJumps keeps its contract from before the knob: the first pair only.
    EXPECT_EQ(h.det.getLastIfJumps().size(), 6u);
    EXPECT_EQ(h.det.getLastIfJumps().at("G03").sig2, "L2W");

    const auto& allp = h.det.getLastIfJumpsAllPairs();
    EXPECT_EQ(allp.at({ "G03", kP12 }).state_key, "GPS");
    EXPECT_EQ(allp.at({ "G03", kP15 }).state_key, "GPS|" + kP15);
    EXPECT_EQ(allp.at({ "G03", kP25 }).state_key, "GPS|" + kP25);
}

/// A dual-band arc has one pair however the knob is set: the widening adds pairs,
/// it does not invent bands.
TEST(IfTdPairWidth, AllOnADualBandArcIsStillOnePair) {
    PairHarness h(/*all_pairs=*/true);
    h.bands = { "L1C", "L2W" };
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);

    EXPECT_EQ(h.det.getLastIfJumpsAllPairs().size(), 6u);
    EXPECT_EQ(pairsOf(h.det, "G03"), (std::vector<std::string>{ kP12 }));
    EXPECT_EQ(h.det.getLastIfJumpsAllPairs().at({ "G03", kP12 }).state_key, "GPS");
}

/// The pair the single-pair detector was blind to now reads the step, and reads
/// it at the designed size: a 40-cycle L5 step enters the (a, L5Q) iono-free
/// combination scaled by c2 = −f5²/(fa² − f5²).  On (1,5) that is −1.26 and on
/// (2,5) −11.26, so an unwatched (1,5) lane can hide a step of metres.
TEST(IfTdPairWidth, AllSeesTheL5StepOnBothPairsThatContainL5) {
    PairHarness quiet(/*all_pairs=*/true), stepped(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    const double step_m = 40.0 * Constants::GPS_L5_WAVELENGTH;
    const BandOffset l5_step{ { "G03", { { "L5Q", step_m } } } };

    for (std::size_t e = 0; e < 3; ++e) {
        quiet.step(e, omc, elev);
        stepped.step(e, omc, elev, e >= 2 ? l5_step : BandOffset{});
    }
    EXPECT_NEAR(stepped.dd("G03", kP15) - quiet.dd("G03", kP15),
                c2Of(kF1, kF5) * step_m, 1e-6);
    EXPECT_NEAR(stepped.dd("G03", kP25) - quiet.dd("G03", kP25),
                c2Of(kF2, kF5) * step_m, 1e-6);
    // Both are metres (about 12.9 m and 115 m), over a hundred times the ~10 cm
    // (1,1) physical floor.
    EXPECT_GT(std::abs(stepped.dd("G03", kP15) - quiet.dd("G03", kP15)), 1.0);
}

// ------------------------------- the first pair is unchanged by the widening

/// The main contract: with the knob at `all`, the first pair's samples are
/// identical to what `first` produced (value, elevation, wavelength, reference
/// identity and validity), epoch after epoch.  The extra pairs are an addition,
/// not a perturbation, which is what lets an `all` arm be compared with an `off`
/// one on the first pair's decisions.
TEST(IfTdPairWidth, FirstPairSamplesAreIdenticalInBothModes) {
    PairHarness a(/*all_pairs=*/false), b(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    auto omc = sixSatOmc();

    for (std::size_t e = 0; e < 6; ++e) {
        omc["G04"] += 0.013;                       // a slow, harmless drift
        const auto ka = a.step(e, omc, elev);
        const auto kb = b.step(e, omc, elev);

        ASSERT_EQ(a.det.getLastIfJumps().size(), b.det.getLastIfJumps().size());
        for (const auto& [sat, ja] : a.det.getLastIfJumps()) {
            const auto& jb = b.det.getLastIfJumps().at(sat);
            EXPECT_PRED_FORMAT2(SameDd, ja.delta_grad_m, jb.delta_grad_m)
                << sat << " ep" << e;
            EXPECT_DOUBLE_EQ(ja.lambda_if_m, jb.lambda_if_m)   << sat << " ep" << e;
            EXPECT_DOUBLE_EQ(ja.elevation_deg, jb.elevation_deg) << sat << " ep" << e;
            EXPECT_EQ(ja.is_reference, jb.is_reference)        << sat << " ep" << e;
            EXPECT_EQ(ja.valid, jb.valid)                      << sat << " ep" << e;
            EXPECT_EQ(ja.state_key, "GPS");
            EXPECT_EQ(jb.state_key, "GPS");
        }
        // `all` may push reset keys for the added chains; the first pair's keys
        // must match one for one.
        auto firstPairKeys = [](const std::vector<std::string>& keys) {
            std::vector<std::string> out;
            for (const auto& k : keys)
                if (k.size() > kP12.size()
                    && k.compare(k.size() - kP12.size(), kP12.size(), kP12) == 0)
                    out.push_back(k);
            return out;
        };
        EXPECT_EQ(firstPairKeys(ka), firstPairKeys(kb)) << "ep" << e;
        EXPECT_EQ(firstPairKeys(ka), ka) << "ep" << e;   // `first` makes no others
    }
}

/// One reference arm per (station, constellation) (§14.10.4).  A common-mode
/// step on the reference satellite fires the first chain's reference test and
/// pushes its one reset key, in both modes: the additional chains share that
/// reference and do not re-run the test.
///
/// Running it per chain would not be a pure addition: it would be three tests on
/// three different iono-free combinations of the same arcs, and the extra-wide
/// ones are ill-conditioned ((L2W,L5Q) amplifies noise 16.6× against 2.98× on
/// (L1C,L2W) while its λ_IF is 0.1248 m, so the legacy candidate condition
/// |median| > 0.5·λ_IF is met by noise).  On a quiet 60-station null, per-chain
/// reference tests multiplied the reference fires with no extra flags from the
/// pooled arm; §14.10.4 of docs/SPEC_event_harness.md has the figures.
TEST(IfTdPairWidth, ReferenceArmRunsOnceWhateverThePairWidth) {
    PairHarness a(/*all_pairs=*/false), b(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    auto omc = sixSatOmc();

    a.step(0, omc, elev);  b.step(0, omc, elev);
    a.step(1, omc, elev);  b.step(1, omc, elev);
    // The reference moves; every non-reference gradient therefore steps by −Δ,
    // and the first chain's median non-ref DD clears its 0.5·λ_IF floor.
    omc["G01"] += 0.60;
    const auto ka = a.step(2, omc, elev);
    const auto kb = b.step(2, omc, elev);

    EXPECT_EQ(ka, (std::vector<std::string>{ "G01_" + kP12 }));
    EXPECT_EQ(kb, ka);
}

/// The additional chains adopt that reference, so every pair of a satellite sits
/// in one between-satellite frame.  G01 is the highest satellite, so it is the
/// first chain's reference; the (1,5) and (2,5) chains must report it as theirs
/// too.
TEST(IfTdPairWidth, AdditionalChainsAdoptTheFirstChainsReference) {
    PairHarness h(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);

    const auto& allp = h.det.getLastIfJumpsAllPairs();
    for (const auto& pair : { kP12, kP15, kP25 }) {
        EXPECT_TRUE(allp.at({ "G01", pair }).is_reference) << pair;
        for (const char* sat : { "G02", "G03", "G06" })
            EXPECT_FALSE(allp.at({ sat, pair }).is_reference) << sat << " " << pair;
    }
}

// ------------------------------------------------- per-pair-kind independence

/// An L5-only step moves the two pairs that contain L5 and leaves (L1C, L2W)
/// bit-identical to the quiet stream.  One pair's evidence cannot leak into
/// another's, which is the precondition for the per-pair-kind pools to be
/// independent decisions rather than one decision reported three times.
TEST(IfTdPairWidth, AnL5StepLeavesTheL1L2PairBitIdentical) {
    PairHarness quiet(/*all_pairs=*/true), stepped(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    const BandOffset l5_step{ { "G03", { { "L5Q", 40.0 * Constants::GPS_L5_WAVELENGTH } } } };

    for (std::size_t e = 0; e < 4; ++e) {
        quiet.step(e, omc, elev);
        stepped.step(e, omc, elev, e >= 2 ? l5_step : BandOffset{});
        for (const auto& [sat, el] : elev) {
            (void)el;
            EXPECT_PRED_FORMAT2(SameDd, stepped.dd(sat, kP12), quiet.dd(sat, kP12))
                << sat << " ep" << e;
        }
    }
}

/// Restarting one pair's chain restarts that pair alone.  A driver that flagged
/// (L1C, L5Q) must not discard the (L1C, L2W) gradient history that did not flag:
/// that would lose a clean arc and make one pair's decision depend on another's,
/// which is what the whole-satellite reset does.
TEST(IfTdPairWidth, ResetSatelliteInStateRestartsOnlyThatPair) {
    PairHarness h(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);
    ASSERT_TRUE(std::isfinite(h.dd("G03", kP12)));
    ASSERT_TRUE(std::isfinite(h.dd("G03", kP15)));

    h.det.resetSatelliteInState("GPS|" + kP15, "G03");
    h.step(2, omc, elev);

    EXPECT_FALSE(std::isfinite(h.dd("G03", kP15)));   // fresh arc, no previous gradient
    EXPECT_TRUE(std::isfinite(h.dd("G03", kP12)));    // untouched
    EXPECT_TRUE(std::isfinite(h.dd("G03", kP25)));    // untouched
    EXPECT_TRUE(std::isfinite(h.dd("G04", kP15)));    // and only that satellite
}

/// The whole-satellite reset still clears every chain of that satellite, so the
/// `first` path (one chain) is unchanged and the difference between the two
/// resets is explicit.
TEST(IfTdPairWidth, ResetSatelliteStillClearsEveryPair) {
    PairHarness h(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);
    h.det.resetSatellite("G03");
    h.step(2, omc, elev);

    for (const auto& pair : { kP12, kP15, kP25 })
        EXPECT_FALSE(std::isfinite(h.dd("G03", pair))) << pair;
    EXPECT_TRUE(std::isfinite(h.dd("G04", kP12)));
}

/// A chain key that does not exist is a silent no-op, so a stale or malformed
/// key can never clear a chain it does not name.
TEST(IfTdPairWidth, ResetSatelliteInStateIgnoresAnUnknownChain) {
    PairHarness h(/*all_pairs=*/true);
    const auto elev = sixSatElevations();
    const auto omc  = sixSatOmc();
    h.step(0, omc, elev);
    h.step(1, omc, elev);
    const double before = h.dd("G03", kP12);

    h.det.resetSatelliteInState("GPS|L1C_L9X", "G03");
    h.step(2, omc, elev);
    EXPECT_TRUE(std::isfinite(h.dd("G03", kP12)));
    EXPECT_TRUE(std::isfinite(before));
}

// ------------------------- the driver's pooled decision is per pair kind too

/// The driver thresholds each sample against its own pair kind's pooled centre
/// and scale (`by_pair` is keyed by pair label, and both helpers are called once
/// per kind).  Stated as an identity on the shared helpers: a very different
/// second pair kind cannot move the first kind's centre, scale or threshold,
/// which keeps the first pair's flags the same under `all` as under `first`.
TEST(IfTdPairWidth, PooledScaleAndThresholdAreScopedToOnePairKind) {
    // Pair kind A: a quiet zenith-ish family with one real outlier.
    const std::vector<double> dd_a{ 0.001, -0.002, 0.000, 0.003, -0.001, 0.240 };
    const std::vector<double> el_a{ 80.0, 70.0, 60.0, 50.0, 40.0, 30.0 };
    const std::vector<char>   rf_a{ 1, 0, 0, 0, 0, 0 };
    // Pair kind B: the same satellites on an extra-wide lane, three orders of
    // magnitude noisier: the population that would swamp a shared pool.
    const std::vector<double> dd_b{ 1.7, -2.4, 3.1, -0.9, 2.2, -3.6 };

    const IfTdPairPool a_alone = ifTdPoolStats(dd_a, el_a, rf_a);

    std::vector<double> dd_mixed = dd_a, el_mixed = el_a;
    std::vector<char>   rf_mixed = rf_a;
    dd_mixed.insert(dd_mixed.end(), dd_b.begin(), dd_b.end());
    el_mixed.insert(el_mixed.end(), el_a.begin(), el_a.end());
    rf_mixed.insert(rf_mixed.end(), rf_a.begin(), rf_a.end());
    const IfTdPairPool mixed = ifTdPoolStats(dd_mixed, el_mixed, rf_mixed);

    // Pooling the two kinds together gives a different decision; this guards
    // against a vacuous test.
    EXPECT_GT(mixed.s_pool, 10.0 * a_alone.s_pool);

    // The threshold the driver forms for one of kind A's samples depends only on
    // kind A: identical station EWMA, identical shrinkage, identical floor.
    constexpr double k_mad = 8.0, kappa = 20.0, floor_frac = 1.0, lambda = 0.10695;
    auto thresholdFor = [&](const IfTdPairPool& pool, long count, double ewma,
                            double el_deg) {
        const double s_eff = ifTdShrunkScale(count, ewma, pool.s_pool, kappa);
        return std::max(k_mad * s_eff * ifElevMap(el_deg), floor_frac * lambda);
    };
    const double thr_a     = thresholdFor(a_alone, /*count=*/0, /*ewma=*/0.0, 30.0);
    const double thr_a_warm = thresholdFor(a_alone, /*count=*/60, /*ewma=*/0.004, 30.0);
    EXPECT_GT(thresholdFor(mixed, 0, 0.0, 30.0), 10.0 * thr_a);
    // The outlier clears kind A's own gate; it is only invisible when the pool is
    // shared with the noisy kind.
    EXPECT_GT(0.240 - a_alone.med, thr_a_warm);
}

}  // namespace GnssProcessor
