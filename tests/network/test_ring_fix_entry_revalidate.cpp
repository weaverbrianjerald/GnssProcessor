// Unit tests of the ring fix's verdict on a cut's entry link (include/network/ar/RingFix.hpp, ringFixEntryVerdict,
// network.ring_fix.entry_revalidate): off, a usable held entry is a known step as before; on, an entry that came back
// across a gap needs the gap re-entry test's clean measurement of its own held pair, and a near-null signature at a
// whole step with a small ionosphere-free deviation refuses it.

#include "network/ar/RingFix.hpp"

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
                                      ringFixEntryVerdictName(RingFixEntryVerdict::Signature)};
    EXPECT_EQ(names.size(), 5u);
}
