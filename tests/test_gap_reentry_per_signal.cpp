/**
 * @file test_gap_reentry_per_signal.cpp
 * @brief Card GRS: the gap re-entry tier keyed on the SIGNAL
 *        (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md §2.6).
 *
 * The defect under test is a BLIND SPOT, not a wrong number: the row-level
 * Card-G instrument keys on the raw row, the row is rebuilt every epoch from
 * whichever phase signals carry a matching code, and a satellite that keeps ONE
 * signal through a dropout keeps its row.  A receiver losing lock on the
 * semi-codeless L1/L2 phases while the civil L5 tracks through therefore
 * re-enters with an ADJACENT predecessor: no resumption, no detector on the
 * return epoch, and the carried ambiguities fold across the break.
 *
 * Everything here exercises the PURE compute/qualify pass over synthetic
 * @c PppObservation vectors: no station, no state manager, no solve.  The
 * geometry is the one @c test_network_slip_qc.cpp uses (same file-local
 * helpers, deliberately duplicated rather than shared so neither file's fixture
 * can be changed under the other):
 *
 *   L_i = (R − I_i)/λ_i + N_i  [cycles],   C_i = R + I_i  [m],
 *   I_i = I_1 (λ_i/λ_1)²
 *
 * under which the geometry-free PHASE difference between two signals is
 *   φ_a − φ_b = (I_b − I_a) + λ_a N_a − λ_b N_b   [m]
 * and is CONSTANT while both ambiguities and the ionosphere are.  An injected
 * step of ΔN cycles on signal a therefore moves it by exactly ΔN·λ_a metres,
 * a designed margin against the empirical GF gate, not a derived one.
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "slip/NetworkRawObservables.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

// ---------------------------------------------------------------------------
// Synthetic geometry (GPS L1 / L2 / L5)
// ---------------------------------------------------------------------------

const double kL1 = Constants::SPEED_OF_LIGHT / Constants::GPS_L1_FREQ;  // ≈ 0.190294 m
const double kL2 = Constants::SPEED_OF_LIGHT / Constants::GPS_L2_FREQ;  // ≈ 0.244210 m
const double kL5 = Constants::SPEED_OF_LIGHT / Constants::GPS_L5_FREQ;  // ≈ 0.254828 m

constexpr double kRangeM  = 22'000'000.0;   ///< Nominal geometric range.
constexpr double kIonoL1M = 5.0;            ///< First-order L1 ionospheric delay.

/// First-order ionospheric delay at a band, in metres.
double ionoAt(double lambda_m) {
    const double ratio = lambda_m / kL1;
    return kIonoL1M * ratio * ratio;
}

/// Append one band's raw phase (cycles) and matching code (metres).
/// @p code_bias_m shifts the code only, which moves MW without touching GF.
void addBand(PppObservation& obs, const std::string& phase_sig, double lambda_m,
             double amb_cyc, double code_bias_m = 0.0) {
    const double iono = ionoAt(lambda_m);
    obs.raw_observed[phase_sig] = (kRangeM - iono) / lambda_m + amb_cyc;
    obs.raw_observed["C" + phase_sig.substr(1)] = kRangeM + iono + code_bias_m;
}

/// The bands one epoch of one satellite carries, and their ambiguities.
struct Band {
    std::string sig;        ///< Phase signal name.
    double      lambda_m;   ///< Its wavelength.
    double      amb_cyc;    ///< Its ambiguity (cycles).
    bool        lli{false}; ///< Assert RINEX LLI bit 0 on this signal.
};

/// A GPS satellite carrying exactly @p bands, with matching codes.
PppObservation makeObs(const std::string& sat, const std::vector<Band>& bands,
                       double code_bias_m = 0.0) {
    PppObservation obs{};
    obs.satellite = sat;
    obs.system    = System::GPS;
    obs.time      = GnssTime{ 2400, 0.0 };
    obs.elevation = 45.0;
    for (const auto& b : bands) {
        addBand(obs, b.sig, b.lambda_m, b.amb_cyc, code_bias_m);
        if (b.lli) obs.lli_flags[b.sig] = 1;
    }
    return obs;
}

/// The three-band GPS satellite the field case is built on.
std::vector<Band> triple(double n1, double n2, double n5) {
    return { { "L1C", kL1, n1 }, { "L2W", kL2, n2 }, { "L5Q", kL5, n5 } };
}

/// One station's compute + qualify passes, exactly as the run loop drives them.
struct PerSigHarness {
    LogManager                  log_manager;
    PppUtils                    utils{ log_manager };
    SlipQcParams                params;
    std::vector<RawSlipHistory> hist;
    RawSlipEpoch                epoch;
    SlipQcCounters              counters;
    long long                   epoch_idx{ 0 };

    PerSigHarness() {
        // The real posture: the rider is a rider, so the row-level card is
        // armed with it.  Individual tests turn the rider back off.
        params.gap_reentry             = true;
        params.gap_reentry_per_signal  = true;
    }

    void run(const std::vector<PppObservation>& obs) {
        const std::vector<std::vector<PppObservation>> modeled{ obs };
        epoch.reset(1);
        const std::vector<bool> present{ true };
        networkComputeRawObservables(modeled, present, 1, utils, epoch,
                                     params.gap_reentry_per_signal);
        networkQualifyRawObservables(modeled, present, 1, epoch_idx, params,
                                     hist, epoch, counters);
        ++epoch_idx;
    }

    /// This epoch's per-signal resumption for one (satellite, signal), or null.
    const SigResumption* sig(const std::string& sat, const std::string& signal) const {
        for (const auto& g : epoch.sig_resumptions)
            if (g.satellite == sat && g.signal == signal) return &g;
        return nullptr;
    }
};

/// A satellite continuously tracked on all three bands.  The station's
/// empirical GF scale has to come from somewhere, and a single-satellite epoch
/// would let the event under test set its own gate.
PppObservation ballast(const std::string& sat, double code_bias_m) {
    return makeObs(sat, triple(500.0, 380.0, 360.0), code_bias_m);
}

// ===========================================================================
// The rider is a rider: OFF changes nothing
// ===========================================================================

// Rule 1 / invariant #6: with the knob off the qualify pass produces exactly
// what it produced before the card: the row-level resumptions, no per-signal
// list on any row, and no per-signal counters.  The SAME stream that fires the
// tier in the next test is used, so this is a real control and not a quiet one.
TEST(GapReentryPerSignalQualify, RiderOffLeavesTheEpochUnchanged) {
    PerSigHarness on;
    PerSigHarness off;
    off.params.gap_reentry_per_signal = false;

    // G01 loses L1C and L2W for three epochs while L5Q tracks through.
    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        triple(100.0, 60.0, 40.0) };
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        const std::vector<PppObservation> ep{ makeObs("G01", stream[k], wobble),
                                              ballast("G03", wobble),
                                              ballast("G04", wobble) };
        on.run(ep);
        off.run(ep);
    }

    // The armed arm sees the break; the control sees nothing at all.
    EXPECT_GT(on.counters.gap_sig_resumptions, 0);
    EXPECT_TRUE(off.epoch.sig_resumptions.empty());
    EXPECT_EQ(off.counters.gap_sig_resumptions, 0);
    EXPECT_EQ(off.counters.gap_sig_shifted, 0);
    EXPECT_EQ(off.counters.gap_sig_unmeasurable, 0);
    EXPECT_EQ(off.counters.gap_sig.arcs, 0);

    // …and the ROW is byte-identical in the parts every other consumer reads:
    // the per-signal list is the only thing the knob adds.
    ASSERT_EQ(on.epoch.rows.at(0).size(), off.epoch.rows.at(0).size());
    for (std::size_t r = 0; r < off.epoch.rows.at(0).size(); ++r) {
        const auto& a = on.epoch.rows.at(0)[r];
        const auto& b = off.epoch.rows.at(0)[r];
        EXPECT_EQ(a.satellite, b.satellite);
        EXPECT_EQ(a.sig1, b.sig1);
        EXPECT_EQ(a.sig2, b.sig2);
        EXPECT_EQ(a.dual, b.dual);
        EXPECT_DOUBLE_EQ(a.mw_m, b.mw_m);
        EXPECT_DOUBLE_EQ(a.gf_m, b.gf_m);
        EXPECT_DOUBLE_EQ(a.l1_c1_m, b.l1_c1_m);
        EXPECT_TRUE(b.sigs.empty());     // nothing new is filled when OFF
        EXPECT_FALSE(a.sigs.empty());    // …and it IS filled when ON
    }
    // Both arms agree about the row-level tier, which the rider must not touch.
    EXPECT_EQ(on.counters.gap_resumptions, off.counters.gap_resumptions);
    EXPECT_EQ(on.counters.gap_shifted, off.counters.gap_shifted);
}

// ===========================================================================
// The field case: a primary-pair dropout with the third band tracking through
// ===========================================================================

// THE DEFECT, reproduced: L1C and L2W are absent for three processed epochs
// while L5Q keeps the row alive, so the row-level tier sees an adjacent
// predecessor and reports NOTHING.  The per-signal tier reports both returning
// signals, measures each against the partner that tracked through, and grades
// the injected 301-cycle L1C step as SHIFTED while the unslipped L2W stays
// clean.  (BSHM00ISR / G14, TOW 268770 to 268860 is this shape.)
TEST(GapReentryPerSignalQualify, PrimaryPairDropoutWithThirdBandThroughFiresBothSignals) {
    PerSigHarness h;
    constexpr double kStepCyc = 301.0;     // the field's measured L1C step

    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        triple(100.0 + kStepCyc, 60.0, 40.0) };   // L1C returns stepped, L2W clean
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        h.run({ makeObs("G01", stream[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    // The row-level tier is BLIND here, which is the whole card.
    EXPECT_EQ(h.counters.gap_resumptions, 0);
    EXPECT_TRUE(h.epoch.resumptions.empty());

    ASSERT_EQ(h.epoch.sig_resumptions.size(), 2u);
    const SigResumption* l1 = h.sig("G01", "L1C");
    const SigResumption* l2 = h.sig("G01", "L2W");
    ASSERT_NE(l1, nullptr);
    ASSERT_NE(l2, nullptr);

    // Identity and the gap, in PROCESSED epochs: last seen at index 0, back at
    // index 4, so three epochs were missed.
    EXPECT_EQ(l1->station, 0);
    EXPECT_EQ(l1->gap_epochs, 3);
    EXPECT_EQ(l2->gap_epochs, 3);

    // The witness is the band that tracked through, on both.
    EXPECT_EQ(l1->partner, "L5Q");
    EXPECT_EQ(l2->partner, "L5Q");
    EXPECT_TRUE(l1->partner_through);
    EXPECT_TRUE(l2->partner_through);
    EXPECT_TRUE(l1->measurable);
    EXPECT_TRUE(l2->measurable);

    // The level evidence: the GF difference against L5Q moved by exactly the
    // injected step in metres on L1C, and not at all on L2W.  The ionosphere
    // and the geometry are common to the difference and cancel, so this is an
    // equality to numerical precision, not a tolerance band.
    EXPECT_NEAR(l1->d_gf, kStepCyc * kL1, 1e-6);
    EXPECT_NEAR(l2->d_gf, 0.0, 1e-9);
    EXPECT_TRUE(l1->shifted);
    EXPECT_FALSE(l2->shifted);

    EXPECT_EQ(h.counters.gap_sig_resumptions, 2);
    EXPECT_EQ(h.counters.gap_sig_shifted, 1);
    EXPECT_EQ(h.counters.gap_sig_unmeasurable, 0);
    EXPECT_EQ(h.counters.gap_sig_partner_broken, 0);
}

// The same dropout with NO step: both signals still re-enter (the response's
// population is the RE-ENTRY, and an unshifted one is bridged by design), and
// neither is graded shifted.  Without this control the test above would pass on
// a tier that flags every re-entry.
TEST(GapReentryPerSignalQualify, UnsteppedPrimaryPairDropoutIsMeasuredAndClean) {
    PerSigHarness h;
    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        triple(100.0, 60.0, 40.0) };
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        h.run({ makeObs("G01", stream[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    ASSERT_EQ(h.epoch.sig_resumptions.size(), 2u);
    for (const auto& g : h.epoch.sig_resumptions) {
        EXPECT_TRUE(g.measurable) << g.signal;
        EXPECT_TRUE(g.partner_through) << g.signal;
        EXPECT_NEAR(g.d_gf, 0.0, 1e-9) << g.signal;
        EXPECT_FALSE(g.shifted) << g.signal;
    }
    EXPECT_EQ(h.counters.gap_sig_resumptions, 2);
    EXPECT_EQ(h.counters.gap_sig_shifted, 0);
}

// The mirror image: only the THIRD band drops out while the primary pair tracks
// through.  Exactly one per-signal re-entry, witnessed by a primary-pair band.
// This is the case a band-scoped response must NOT widen to the whole
// satellite, because the two bands that tracked through keep their levels.
TEST(GapReentryPerSignalQualify, ThirdBandOnlyDropoutFiresExactlyOneSignal) {
    PerSigHarness h;
    constexpr double kStepCyc = 7.0;

    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } },
        { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } },
        triple(100.0, 60.0, 40.0 + kStepCyc) };
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        h.run({ makeObs("G01", stream[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    EXPECT_EQ(h.counters.gap_resumptions, 0);        // the row never went absent
    ASSERT_EQ(h.epoch.sig_resumptions.size(), 1u);
    const SigResumption& g = h.epoch.sig_resumptions.front();
    EXPECT_EQ(g.signal, "L5Q");
    EXPECT_EQ(g.gap_epochs, 2);
    EXPECT_TRUE(g.measurable);
    EXPECT_TRUE(g.partner_through);
    // std::map order picks the lexically lowest qualifying partner.
    EXPECT_EQ(g.partner, "L1C");
    EXPECT_NEAR(g.d_gf, kStepCyc * kL5, 1e-6);
    EXPECT_TRUE(g.shifted);
    EXPECT_EQ(h.counters.gap_sig_resumptions, 1);
    EXPECT_EQ(h.counters.gap_sig_shifted, 1);
}

// ===========================================================================
// The two populations are disjoint
// ===========================================================================

// A WHOLE-satellite gap is the row-level tier's case, and it must stay there:
// the per-signal tier skips any satellite that produced a resumption in the
// same epoch, so one break is answered once.  Without the skip this stream
// would be freed twice (once per band, and again for the row).
TEST(GapReentryPerSignalQualify, FullSatelliteGapStaysWithTheRowLevelTier) {
    PerSigHarness h;
    constexpr double kStepCyc = 2.0;

    const std::vector<std::vector<Band>> g01 = {
        triple(100.0, 60.0, 40.0),
        {},                                          // the satellite is gone
        {},
        triple(100.0 + kStepCyc, 60.0, 40.0) };
    for (std::size_t k = 0; k < g01.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        std::vector<PppObservation> ep{ ballast("G03", wobble), ballast("G04", wobble) };
        if (!g01[k].empty()) ep.insert(ep.begin(), makeObs("G01", g01[k], wobble));
        h.run(ep);
    }

    ASSERT_EQ(h.epoch.resumptions.size(), 1u);
    EXPECT_EQ(h.epoch.resumptions.front().satellite, "G01");
    EXPECT_EQ(h.counters.gap_resumptions, 1);
    // …and NOTHING from the per-signal tier for that satellite.
    EXPECT_TRUE(h.epoch.sig_resumptions.empty());
    EXPECT_EQ(h.counters.gap_sig_resumptions, 0);
}

// ===========================================================================
// Absence of evidence
// ===========================================================================

// A signal seen for the FIRST time is not a re-entry: there is no pre-gap level
// to difference and nothing carried to protect.  Covers both shapes: a fresh
// satellite, and a band that appears later on a satellite already tracked.
TEST(GapReentryPerSignalQualify, FirstSightingIsNeverAReEntry) {
    PerSigHarness h;

    h.run({ makeObs("G01", { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } }, 0.001),
            ballast("G03", 0.001), ballast("G04", 0.001) });
    EXPECT_TRUE(h.epoch.sig_resumptions.empty());

    // L5Q appears for the first time two epochs in: a new band on a satellite
    // that never lost it, so it has no last-seen epoch and no snapshot.
    h.run({ makeObs("G01", { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } }, 0.002),
            ballast("G03", 0.002), ballast("G04", 0.002) });
    h.run({ makeObs("G01", triple(100.0, 60.0, 40.0), 0.003),
            ballast("G03", 0.003), ballast("G04", 0.003) });

    EXPECT_TRUE(h.epoch.sig_resumptions.empty());
    EXPECT_EQ(h.counters.gap_sig_resumptions, 0);
    EXPECT_EQ(h.counters.gap_sig_unmeasurable, 0);

    // A one-epoch absence is a STEP, not a re-entry: the tier's bar is the same
    // "≥ 1 missed processed epoch" the row-level one uses, so an adjacent
    // predecessor is never a gap.
    h.run({ makeObs("G01", triple(100.0, 60.0, 40.0), 0.004),
            ballast("G03", 0.004), ballast("G04", 0.004) });
    EXPECT_TRUE(h.epoch.sig_resumptions.empty());
}

// No partner on both sides of the break ⇒ UNMEASURABLE, and the tier says so
// rather than fabricating a level.  Here the satellite is single-band
// throughout, so the returning signal has nothing to difference against.  This
// is the population the rider frees BY DEFAULT
// (gap_reentry_per_signal_unmeasurable), which is why it must be visible.
TEST(GapReentryPerSignalQualify, NoPartnerOnBothSidesIsUnmeasurable) {
    PerSigHarness h;

    const std::vector<std::vector<Band>> stream = {
        { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } },
        { { "L2W", kL2, 60.0 } },     // L1C gone AND L2W is the only witness…
        {},                            // …which then goes too: no partner survives
        { { "L1C", kL1, 100.0 } } };   // L1C returns alone
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        std::vector<PppObservation> ep{ ballast("G03", wobble), ballast("G04", wobble) };
        if (!stream[k].empty()) ep.insert(ep.begin(), makeObs("G01", stream[k], wobble));
        h.run(ep);
    }

    // The ROW went absent at index 2, so the row-level tier owns THAT break and
    // the per-signal tier stays silent for G01 (the disjointness rule again).
    EXPECT_EQ(h.counters.gap_resumptions, 1);
    EXPECT_TRUE(h.epoch.sig_resumptions.empty());

    // Now the same shape WITHOUT the row ever going absent: L1C drops for two
    // epochs while L2W carries the row, and L2W is itself absent at the return
    // epoch, so the snapshot has no partner present now.
    PerSigHarness u;
    const std::vector<std::vector<Band>> stream2 = {
        { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 } },
        { { "L2W", kL2, 60.0 } },
        { { "L2W", kL2, 60.0 } },
        { { "L1C", kL1, 100.0 } } };
    for (std::size_t k = 0; k < stream2.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        u.run({ makeObs("G01", stream2[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    EXPECT_EQ(u.counters.gap_resumptions, 0);         // the row survived on L2W
    ASSERT_EQ(u.epoch.sig_resumptions.size(), 1u);
    const SigResumption& g = u.epoch.sig_resumptions.front();
    EXPECT_EQ(g.signal, "L1C");
    EXPECT_EQ(g.gap_epochs, 2);
    EXPECT_FALSE(g.measurable);
    EXPECT_TRUE(g.partner.empty());
    EXPECT_FALSE(g.shifted);
    EXPECT_DOUBLE_EQ(g.d_gf, 0.0);   // never fabricated
    EXPECT_EQ(u.counters.gap_sig_unmeasurable, 1);
    EXPECT_EQ(u.counters.gap_sig_shifted, 0);
}

// The LLI of the RETURNING signal is carried, not the primary pair's.  On the
// field case only the band that never dropped asserted loss of lock, so reading
// the pair would attribute the receiver's verdict to the wrong band, and the
// response's second gate reads exactly this field.
TEST(GapReentryPerSignalQualify, TheReturningSignalCarriesItsOwnLli) {
    PerSigHarness h;

    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L5Q", kL5, 40.0 } },
        { { "L5Q", kL5, 40.0 } },
        // L1C returns flagged; L2W returns with no flag at all.
        { { "L1C", kL1, 100.0, true }, { "L2W", kL2, 60.0 }, { "L5Q", kL5, 40.0 } } };
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        h.run({ makeObs("G01", stream[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    ASSERT_EQ(h.epoch.sig_resumptions.size(), 2u);
    const SigResumption* l1 = h.sig("G01", "L1C");
    const SigResumption* l2 = h.sig("G01", "L2W");
    ASSERT_NE(l1, nullptr);
    ASSERT_NE(l2, nullptr);
    EXPECT_EQ(l1->lli & 1, 1);
    EXPECT_EQ(l2->lli & 1, 0);
}

// ===========================================================================
// The witness rule
// ===========================================================================

// A partner that did NOT track through the gap is the SECOND choice, and the
// tier records that the level test rests on it: the partner's own break sits
// inside the difference, so a shifted verdict there names the PAIR rather than
// this signal.  Built so the ROW never goes absent (L5Q carries it through the
// L1C/L2W dropout) while the only partner each returning signal has is the
// other returning signal, because L5Q is gone by the time they come back.
TEST(GapReentryPerSignalQualify, SecondChoicePartnerIsRecordedAsNotThrough) {
    PerSigHarness h;
    constexpr double kStepCyc = 11.0;

    const std::vector<std::vector<Band>> stream = {
        triple(100.0, 60.0, 40.0),
        { { "L5Q", kL5, 40.0 } },                 // the row survives on L5Q alone
        // L1C and L2W return together; L5Q is gone, so neither has a witness
        // that tracked through.  L2W returns stepped.
        { { "L1C", kL1, 100.0 }, { "L2W", kL2, 60.0 + kStepCyc } } };
    for (std::size_t k = 0; k < stream.size(); ++k) {
        const double wobble = 0.001 * static_cast<double>(k + 1);
        h.run({ makeObs("G01", stream[k], wobble),
                ballast("G03", wobble), ballast("G04", wobble) });
    }

    EXPECT_EQ(h.counters.gap_resumptions, 0);     // the row never went absent
    ASSERT_EQ(h.epoch.sig_resumptions.size(), 2u);
    const SigResumption* l1 = h.sig("G01", "L1C");
    const SigResumption* l2 = h.sig("G01", "L2W");
    ASSERT_NE(l1, nullptr);
    ASSERT_NE(l2, nullptr);

    EXPECT_EQ(l1->partner, "L2W");
    EXPECT_EQ(l2->partner, "L1C");
    EXPECT_FALSE(l1->partner_through);
    EXPECT_FALSE(l2->partner_through);
    EXPECT_TRUE(l1->measurable);
    EXPECT_TRUE(l2->measurable);
    EXPECT_EQ(h.counters.gap_sig_partner_broken, 2);

    // The step is L2W's, but the pair-differenced level moves by the same
    // amount with opposite signs, so BOTH read shifted, which is exactly why
    // the second-choice case is counted separately instead of being graded as
    // if it had named the guilty band.
    EXPECT_NEAR(l1->d_gf, -kStepCyc * kL2, 1e-6);
    EXPECT_NEAR(l2->d_gf,  kStepCyc * kL2, 1e-6);
    EXPECT_TRUE(l1->shifted);
    EXPECT_TRUE(l2->shifted);
    EXPECT_EQ(h.counters.gap_sig_shifted, 2);
}

} // namespace
} // namespace GnssProcessor
