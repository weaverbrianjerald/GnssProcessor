/**
 * @file test_if_phase_ref_gate_defaults.cpp
 * @brief The network.slip_qc defaults table of IfPhaseSlipDetector::RefGateParams
 *        (card UOM step U5t-c).
 *
 * RefGateParams mirrors the reference-gate members of the network's slip-QC
 * configuration (SlipQcConfig), and network setup copies every field from that
 * configuration, so a run never uses the struct's own defaults.  The unit tests
 * of the detector do: they default-construct the parameters.  The header
 * documents the two sets of defaults as one table; this suite holds the table to
 * the code, field by field, so a default changed on one side alone fails here
 * instead of making the tests certify a gate the field runs do not use.
 */

#include "slip/IfPhaseSlipDetector.hpp"
#include "config/GnssConfigTypes.hpp"
#include <gtest/gtest.h>

using namespace GnssProcessor;

namespace {

// Every RefGateParams field equals the SlipQcConfig member network setup copies
// into it (ApplicationNetworkSetup.cpp), at their defaults.
TEST(IfPhaseRefGateDefaultsTest, EveryFieldEqualsTheSlipQcConfigDefault) {
    const IfPhaseSlipDetector::RefGateParams p;
    const SlipQcConfig sq;
    EXPECT_EQ(p.enable,          sq.ref_gate);
    EXPECT_EQ(p.k,               sq.ref_k);
    EXPECT_EQ(p.min_n,           sq.ref_min_n);
    EXPECT_EQ(p.thin_widen,      sq.ref_thin_widen);
    EXPECT_EQ(p.coherence,       sq.ref_coherence);
    EXPECT_EQ(p.max_consecutive, sq.ref_max_consecutive);
    EXPECT_EQ(p.ewma_alpha,      sq.ref_ewma_alpha);
    EXPECT_EQ(p.shrink_kappa,    sq.ref_shrink_kappa);
    EXPECT_EQ(p.floor_frac,      sq.ref_floor_frac);
    EXPECT_EQ(p.warmup_abstain,  sq.ref_warmup_abstain);
    EXPECT_EQ(p.elev_norm,       sq.ref_elev_norm);
    EXPECT_EQ(p.elev_clamp_deg,  sq.ref_elev_clamp_deg);
}

// The values the header's table states (the legacy gate: off, a 0.5 λ_IF floor).
TEST(IfPhaseRefGateDefaultsTest, TheDocumentedDefaults) {
    const IfPhaseSlipDetector::RefGateParams p;
    EXPECT_FALSE(p.enable);
    EXPECT_DOUBLE_EQ(p.k, 6.0);
    EXPECT_EQ(p.min_n, 4);
    EXPECT_DOUBLE_EQ(p.thin_widen, 3.0);
    EXPECT_DOUBLE_EQ(p.coherence, 0.5);
    EXPECT_EQ(p.max_consecutive, 1);
    EXPECT_DOUBLE_EQ(p.ewma_alpha, 0.15);
    EXPECT_DOUBLE_EQ(p.shrink_kappa, 20.0);
    EXPECT_DOUBLE_EQ(p.floor_frac, 0.5);
    EXPECT_FALSE(p.warmup_abstain);
    EXPECT_FALSE(p.elev_norm);
    EXPECT_DOUBLE_EQ(p.elev_clamp_deg, 5.0);
}

}  // namespace
