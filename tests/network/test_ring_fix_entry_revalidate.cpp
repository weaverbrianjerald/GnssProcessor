// Unit tests of the ring fix's verdict on a cut's entry link (include/network/ar/RingFix.hpp, ringFixEntryVerdict,
// network.ring_fix.entry_revalidate): off, a usable held entry is a known step as before; on, an entry that came back
// across a gap needs the gap re-entry test's clean measurement of its own held pair, and a near-null signature at a
// whole step with a small ionosphere-free deviation refuses it.

#include "network/ar/RingFix.hpp"
#include "network/slip/RawObservables.hpp"   // GapResumption

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace GnssProcessor;

namespace {

constexpr double kMaxIf = 0.30;

RingFixEntryEvidence usableEntry() {
    RingFixEntryEvidence e;
    e.usable = true;
    return e;
}

}  // namespace

TEST(RingFixEntryRevalidate, AnEntryThatIsNotUsableIsNotHeld) {
    RingFixEntryEvidence e;   // not usable
    EXPECT_EQ(ringFixEntryVerdict(e, false, kMaxIf), RingFixEntryVerdict::NotHeld);
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::NotHeld);
}

TEST(RingFixEntryRevalidate, OffAUsableEntryIsKnownWhateverTheEvidence) {
    RingFixEntryEvidence e = usableEntry();
    e.resumed = true;
    e.gap_measured = false;
    e.signature = true;
    e.signature_k = 1;
    EXPECT_EQ(ringFixEntryVerdict(e, false, kMaxIf), RingFixEntryVerdict::Known);
    e.gap_measured = true;
    e.gap_shifted = true;
    EXPECT_EQ(ringFixEntryVerdict(e, false, kMaxIf), RingFixEntryVerdict::Known);
}

TEST(RingFixEntryRevalidate, AnEntryBackAcrossAGapWithItsPairUnmeasuredIsRefused) {
    // Section 107.6: the slip layer measured another pair of the link (E5b/E5), so the held E1/E5a pair has no
    // measurement across the satellite's absence.
    RingFixEntryEvidence e = usableEntry();
    e.resumed = true;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::GapUnchecked);
}

TEST(RingFixEntryRevalidate, AnEntryWhosePairShiftedAcrossTheGapIsRefused) {
    RingFixEntryEvidence e = usableEntry();
    e.resumed = true;
    e.gap_measured = true;
    e.gap_shifted = true;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::GapShifted);
}

TEST(RingFixEntryRevalidate, AnEntryMeasuredCleanAcrossTheGapIsKnown) {
    RingFixEntryEvidence e = usableEntry();
    e.resumed = true;
    e.gap_measured = true;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::Known);
}

TEST(RingFixEntryRevalidate, ANearNullSignatureAtAStepRefusesTheEntry) {
    RingFixEntryEvidence e = usableEntry();
    e.signature = true;
    e.signature_k = -1;
    e.signature_if_m = 0.10;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::Signature);
    // A large ionosphere-free deviation is the link's own code bias, not a step.
    e.signature_if_m = 0.50;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::Known);
    // Under half a step.
    e.signature_k = 0;
    e.signature_if_m = 0.0;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::Known);
    // The gap's verdict comes first.
    e.signature_k = 2;
    e.resumed = true;
    EXPECT_EQ(ringFixEntryVerdict(e, true, kMaxIf), RingFixEntryVerdict::GapUnchecked);
}

TEST(RingFixEntryRevalidate, AnEntryWithoutAGapOrASignatureIsKnown) {
    EXPECT_EQ(ringFixEntryVerdict(usableEntry(), true, kMaxIf), RingFixEntryVerdict::Known);
}

TEST(RingFixEntryRevalidate, EveryVerdictHasItsOwnName) {
    const std::set<std::string> names{ringFixEntryVerdictName(RingFixEntryVerdict::NotHeld),
                                      ringFixEntryVerdictName(RingFixEntryVerdict::Known),
                                      ringFixEntryVerdictName(RingFixEntryVerdict::GapUnchecked),
                                      ringFixEntryVerdictName(RingFixEntryVerdict::GapShifted),
                                      ringFixEntryVerdictName(RingFixEntryVerdict::Signature),
                                      ringFixEntryVerdictName(RingFixEntryVerdict::RowUnknown)};
    EXPECT_EQ(names.size(), 6u);
}

// The one owner of a held entry's step keeps each asking mechanism's rule: the cut and keep-carry ask the verdict (off,
// a usable link is known), and the returning row never makes a step known.
TEST(RingFixEntryRevalidate, ChainEntryStepKeepsEachMechanismsRule) {
    RingFixEntryEvidence refused = usableEntry();
    refused.resumed = true;   // back across a gap, its pair unmeasured
    for (const bool revalidate : {false, true}) {
        EXPECT_EQ(chainEntryStep(ChainEntryKind::CutEntry, refused, revalidate, kMaxIf),
                  ringFixEntryVerdict(refused, revalidate, kMaxIf));
        EXPECT_EQ(chainEntryStep(ChainEntryKind::ReturningRow, usableEntry(), revalidate, kMaxIf),
                  RingFixEntryVerdict::RowUnknown);
        EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, refused, revalidate, kMaxIf),
                  ringFixEntryVerdict(refused, revalidate, kMaxIf));
        EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, RingFixEntryEvidence{}, revalidate, kMaxIf),
                  RingFixEntryVerdict::NotHeld);
    }
    EXPECT_EQ(chainEntryStep(ChainEntryKind::CutEntry, refused, true, kMaxIf), RingFixEntryVerdict::GapUnchecked);
    EXPECT_EQ(chainEntryStep(ChainEntryKind::CutEntry, refused, false, kMaxIf), RingFixEntryVerdict::Known);
    EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, refused, true, kMaxIf), RingFixEntryVerdict::GapUnchecked);
    EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, refused, false, kMaxIf), RingFixEntryVerdict::Known);
}

// The keep-carry path's evidence, gathered before the election: a held link whose arc came back across a gap carries
// the gap re-entry test's facts for its own pair; a link not held, or with the check off, carries none.
TEST(RingFixEntryRevalidate, KeepEvidenceTakesTheGapResumptionsOfTheHeldLinks) {
    NetworkRingFix rf;
    rf.rehost_keep_carry = true;
    rf.entry_revalidate = true;
    const RingFixLinkId held_pair{3, "E05", "L1C", "L5Q"}, other{4, "E05", "L1C", "L5Q"}, idle{5, "E07", "L1C", "L5Q"};
    rf.links[held_pair].held = true;
    rf.links[other].held = true;
    rf.links[idle].held = false;
    std::vector<GapResumption> res(2);
    res[0].station = 3; res[0].satellite = "E05"; res[0].sig1 = "L5Q"; res[0].sig2 = "L1C";   // its pair, either order
    res[0].measurable = true; res[0].shifted = true;
    res[1].station = 4; res[1].satellite = "E05"; res[1].sig1 = "L1C"; res[1].sig2 = "L7Q";   // another pair
    ringFixGatherKeepEvidence(rf, res);
    ASSERT_EQ(rf.keep_evidence.size(), 2u);   // the held links only
    const RingFixEntryEvidence& a = rf.keep_evidence.at(held_pair);
    EXPECT_TRUE(a.resumed && a.gap_measured && a.gap_shifted);
    const RingFixEntryEvidence& b = rf.keep_evidence.at(other);
    EXPECT_TRUE(b.resumed);
    EXPECT_FALSE(b.gap_measured || b.gap_shifted);   // came back, its own pair not measured: unchecked
    RingFixEntryEvidence eb = b;
    eb.usable = true;
    EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, eb, true, kMaxIf), RingFixEntryVerdict::GapUnchecked);
    RingFixEntryEvidence ea = a;
    ea.usable = true;
    EXPECT_EQ(chainEntryStep(ChainEntryKind::KeepCarry, ea, true, kMaxIf), RingFixEntryVerdict::GapShifted);
    rf.entry_revalidate = false;
    ringFixGatherKeepEvidence(rf, res);
    EXPECT_TRUE(rf.keep_evidence.empty());
}
