/**
 * @file test_mw_detector_steps.cpp
 * @brief Step-level pieces of the MW slip detector (card UOM step U5m-d): the verdict's
 *        defaults, the one function that forms a flagged lane's results, and the MW pivot
 *        election.
 *
 * The detector's drivers (detectLargeMwResets, processMwSample) are ordered steps over
 * working values; the end-to-end behaviour is pinned by the existing MW suites and by the
 * same-build identical-output gate.  These tests pin the pieces the cut named: the values a
 * flagged sample reports when its path sets nothing (the former out-parameter resets), the
 * result shape of a combined lane (its one ionosphere-free ambiguity) and of an uncombined
 * lane (both signals, in order), and the pivot rule (highest elevation, a deprioritized
 * satellite only as the last resort).
 */
#include <gtest/gtest.h>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwDetectorWork.hpp"
#include "slip/MwSlipDetector.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Befriended in MwSlipDetector.hpp; forwards to the private step functions and names the
/// private value types.  Declared in this namespace, as the friend declaration names
/// GnssProcessor::MwDetectorStepsTestAccess.
struct MwDetectorStepsTestAccess {
    using Lane = MwSlipDetector::MwLane;          ///< The lane value type.
    using Verdict = MwSlipDetector::MwVerdict;    ///< The verdict value type.
    using Entry = MwSlipDetector::SatMwEntry;     ///< An SD group member.

    /// Forwards to MwSlipDetector::emitLaneSlip_.
    static void emit(std::vector<MwSlipResult>& results, const Lane& lane, const Verdict& v,
                     double lambda_wl, bool bia_corrected) {
        MwSlipDetector::emitLaneSlip_(results, lane, v, lambda_wl, bia_corrected);
    }

    /// Forwards to MwSlipDetector::electPivot_.
    static std::string elect(const MwSlipDetector& det, const std::map<std::string, Entry>& g) {
        return det.electPivot_(g);
    }
};

}  // namespace GnssProcessor

using namespace GnssProcessor;
using Access = MwDetectorStepsTestAccess;

namespace {

/// A verdict whose every field differs from its default.
Access::Verdict flagged() {
    Access::Verdict v;
    v.slip_cycles = 3;
    v.wl_known = true;
    v.slip_frac = 0.1;
    v.slip_wl_float = 3.05;
    v.evidence_ratio = 2.5;
    v.arc_epochs = 42;
    return v;
}

void expectVerdictFields(const MwSlipResult& r, const Access::Verdict& v, double lambda_wl) {
    EXPECT_EQ(r.slip_wl_cycles, v.slip_cycles);
    EXPECT_EQ(r.slip_wl_known, v.wl_known);
    EXPECT_DOUBLE_EQ(r.lambda_wl, lambda_wl);
    EXPECT_DOUBLE_EQ(r.slip_frac, v.slip_frac);
    EXPECT_DOUBLE_EQ(r.slip_wl_float, v.slip_wl_float);
    EXPECT_DOUBLE_EQ(r.evidence_ratio, v.evidence_ratio);
    EXPECT_EQ(r.arc_epochs, v.arc_epochs);
}

PppObservation observationAt(const std::string& sat, double elevation_deg) {
    PppObservation obs;
    obs.satellite = sat;
    obs.elevation = elevation_deg;
    return obs;
}

}  // namespace

// The values a flagged path reports for a field it does not set: those the former
// out-parameter resets wrote at the top of processMwSample.
TEST(MwDetectorSteps, VerdictDefaultsAreTheFormerOutParameterResets) {
    const Access::Verdict v;
    EXPECT_EQ(v.slip_cycles, 0);
    EXPECT_FALSE(v.wl_known);
    EXPECT_DOUBLE_EQ(v.slip_frac, 0.5);
    EXPECT_TRUE(std::isnan(v.slip_wl_float));
    EXPECT_TRUE(std::isnan(v.evidence_ratio));
    EXPECT_EQ(v.arc_epochs, 0);
}

// A combined (ionosphere-free) lane resets its one IF ambiguity.
TEST(MwDetectorSteps, CombinedLaneEmitsItsIonoFreeAmbiguity) {
    const Access::Lane lane{ "G01", "L1C", "L2W", "C1C", "C2W", "L1C_L2W", "G01_L1C_L2W" };
    const Access::Verdict v = flagged();
    std::vector<MwSlipResult> results;
    Access::emit(results, lane, v, 0.862, /*bia_corrected=*/false);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].amb_key, "G01_L1C_L2W");
    EXPECT_EQ(results[0].sig1, "L1C");
    EXPECT_EQ(results[0].sig2, "L2W");
    EXPECT_EQ(results[0].satellite, "G01");
    EXPECT_FALSE(results[0].bia_corrected);
    expectVerdictFields(results[0], v, 0.862);
}

// An uncombined lane resets both signals' ambiguities, sig1 first; results append.
TEST(MwDetectorSteps, UncombinedLaneEmitsBothSignalsInOrder) {
    const Access::Lane lane{ "E07", "L1C", "L5Q", "C1C", "C5Q", "", "E07_L1C_L5Q" };
    const Access::Verdict v = flagged();
    std::vector<MwSlipResult> results(1);   // one earlier result stays first
    Access::emit(results, lane, v, 0.751, /*bia_corrected=*/true);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[1].amb_key, "E07_L1C");
    EXPECT_EQ(results[2].amb_key, "E07_L5Q");
    for (std::size_t k = 1; k < 3; ++k) {
        EXPECT_EQ(results[k].sig1, "L1C");
        EXPECT_EQ(results[k].sig2, "L5Q");
        EXPECT_EQ(results[k].satellite, "E07");
        EXPECT_TRUE(results[k].bia_corrected);
        expectVerdictFields(results[k], v, 0.751);
    }
}

// The MW pivot is the highest satellite; a deprioritized one is skipped while another
// member exists, and elected only when every member is deprioritized.
TEST(MwDetectorSteps, PivotElectionSkipsDeprioritizedSatellites) {
    LogManager log;
    GnssConfig cfg;
    MwSlipDetector det(cfg, log);
    const PppObservation g01 = observationAt("G01", 30.0);
    const PppObservation g05 = observationAt("G05", 70.0);
    const PppObservation g09 = observationAt("G09", 50.0);
    std::map<std::string, Access::Entry> group;
    group["G01"] = Access::Entry{ &g01, {}, 0.0, 0.0, 0.0, true, 0.862, false, {} };
    group["G05"] = Access::Entry{ &g05, {}, 0.0, 0.0, 0.0, true, 0.862, false, {} };
    group["G09"] = Access::Entry{ &g09, {}, 0.0, 0.0, 0.0, true, 0.862, false, {} };

    EXPECT_EQ(Access::elect(det, group), "G05");
    det.notifyPivotForcedHandover("G05", "test");
    EXPECT_EQ(Access::elect(det, group), "G09");
    det.notifyPivotForcedHandover("G09", "test");
    det.notifyPivotForcedHandover("G01", "test");
    EXPECT_EQ(Access::elect(det, group), "G05");   // all deprioritized: the highest
}
