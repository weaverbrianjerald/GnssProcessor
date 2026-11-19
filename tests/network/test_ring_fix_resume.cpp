/**
 * @file test_ring_fix_resume.cpp
 * @brief A held link that answers a tracking break of its own (network.ring_fix.resume_response).
 *
 * The class of a resumption read from the epoch's records; the release and the suspension at the resumption epoch;
 * the re-hold, contradiction, expiry and end of a suspension; the off identity of the release step; the record
 * through the memory write and read; and the five resumptions of the adopted days on which a hold was folded across
 * the break (BOGT00COL G09 on DOY 143, POTS00DEU G13 on DOY 144, KIT300UZB G29 on DOY 145, KIT300UZB G29 and G10 on
 * DOY 147), each with its resumption record and link state as the run folders show them.
 */
#include "network/ar/RingFix.hpp"
#include "network/slip/RawObservables.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kL1 = 0.19029367279836487;   // GPS L1 (m)
constexpr double kL2 = 0.24421021342456825;   // GPS L2 (m)
const RingFixLinkId kId{3, "G09", "L1C", "L2W"};

/// A run state holding @p id at (n1, n2), decided long before @p now_s, with the key at @p r.
NetworkRingFix heldState(RingFixResumeResponse r, const RingFixLinkId& id = kId, long long n1 = -71,
                         long long n2 = -130, double now_s = 572730.0) {
    NetworkRingFix rf;
    rf.resume_response = r;
    RingFixLink& L = rf.links[id];
    L.lambda1 = kL1;
    L.lambda2 = kL2;
    L.held = true;
    L.n1 = n1;
    L.n2 = n2;
    L.ring = 5;
    L.decided_s = now_s - 3690.0;
    L.stamp1 = 1000.0;
    L.stamp2 = 1000.0;
    L.root = "S30000SIM";
    return rf;
}

/// The release step's view of a quiet held link (present, carried, not latched).
RingFixHoldObs quietObs() {
    RingFixHoldObs o;
    o.present = true;
    o.root = "S30000SIM";
    o.carry_ok = true;
    o.stamp1 = 1000.0;
    o.stamp2 = 1000.0;
    return o;
}

/// A row-level resumption record of (station, satellite) on the pair (a, b).
GapResumption rowResumption(int station, const std::string& sat, const std::string& a, const std::string& b,
                            bool measurable, bool shifted, long long gap_ep, double d_gf, int lli = 0) {
    GapResumption g;
    g.station = station;
    g.satellite = sat;
    g.sig1 = a;
    g.sig2 = b;
    g.gap_epochs = gap_ep;
    g.measurable = measurable;
    g.shifted = shifted;
    g.d_gf = d_gf;
    g.lli1 = lli;
    g.lli2 = lli;
    return g;
}

/// A one-signal resumption record under a surviving row.
SigResumption sigResumption(int station, const std::string& sat, const std::string& sig, bool measurable,
                            bool shifted, long long gap_ep, double d_gf) {
    SigResumption s;
    s.station = station;
    s.satellite = sat;
    s.signal = sig;
    s.partner = measurable ? "L1C" : "";
    s.measurable = measurable;
    s.shifted = shifted;
    s.gap_epochs = gap_ep;
    s.d_gf = d_gf;
    return s;
}

/// The decision's view of a link with a two-band float near (n1, n2) and a small variance.
RingFixDecideObs floatNear(const RingFixLinkId& id, long long n1, long long n2, double var = 1e-5) {
    RingFixDecideObs o;
    o.id = id;
    o.lambda1 = kL1;
    o.lambda2 = kL2;
    o.have_float = true;
    o.f1 = static_cast<double>(n1);
    o.f2 = static_cast<double>(n2);
    o.q11 = var;
    o.q12 = var * 0.9;
    o.q22 = var;
    o.ring = 5;
    o.candidate = true;
    o.root = "S30000SIM";
    o.carry_ok = true;
    o.relevel1_s = 572730.0;   // re-opened at the suspension
    o.relevel2_s = 572730.0;
    return o;
}

/// The resume step at @p now_s for the one link @p id with the resumption class of @p records.
std::vector<RingFixResumeEdge> resumeAt(NetworkRingFix& rf, RingFixHoldObs o, const std::vector<GapResumption>& rows,
                                        const std::vector<SigResumption>& sigs, double now_s = 572730.0,
                                        const RingFixLinkId& id = kId) {
    o.resume = ringFixResumeClassOf(id, rows, sigs);
    return ringFixResumeStep(rf, {{id, o}}, now_s);
}

}  // namespace

// ---- The class of a resumption ----

TEST(RingFixResume, TheClassOfAResumptionIsReadFromTheLinksOwnRecords) {
    const auto cls = [](const std::vector<GapResumption>& rows, const std::vector<SigResumption>& sigs) {
        return ringFixResumeClassOf(kId, rows, sigs).cls;
    };
    EXPECT_EQ(cls({}, {}), RingFixResumeClass::None);
    EXPECT_EQ(cls({rowResumption(3, "G09", "L1C", "L2W", true, false, 4, 0.01)}, {}), RingFixResumeClass::Clean);
    EXPECT_EQ(cls({rowResumption(3, "G09", "L2W", "L1C", true, true, 15, -1.02)}, {}), RingFixResumeClass::Shifted);
    EXPECT_EQ(cls({rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {}), RingFixResumeClass::Unmeasured);
    // A pair other than the link's measured nothing of the link's pair.
    EXPECT_EQ(cls({rowResumption(3, "G09", "L1C", "L5Q", true, false, 4, 0.0)}, {}), RingFixResumeClass::Unmeasured);
    // An off-pair return of the same satellite (signals the link does not use) is not the link's break.
    EXPECT_EQ(cls({rowResumption(3, "G09", "L2L", "L5Q", false, true, 3, 0.9)}, {}), RingFixResumeClass::None);
    // Another station's or another satellite's record is not the link's.
    EXPECT_EQ(cls({rowResumption(4, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {}), RingFixResumeClass::None);
    EXPECT_EQ(cls({rowResumption(3, "G10", "L1C", "L2W", false, false, 1, 0.0)}, {}), RingFixResumeClass::None);
    // A signal record of one of the link's signals.
    EXPECT_EQ(cls({}, {sigResumption(3, "G09", "L2W", true, false, 1, 0.002)}), RingFixResumeClass::Clean);
    EXPECT_EQ(cls({}, {sigResumption(3, "G09", "L2W", true, true, 1, 0.4)}), RingFixResumeClass::Shifted);
    EXPECT_EQ(cls({}, {sigResumption(3, "G09", "L2W", false, false, 1, 0.0)}), RingFixResumeClass::Unmeasured);
    EXPECT_EQ(cls({}, {sigResumption(3, "G09", "L5Q", false, false, 1, 0.0)}), RingFixResumeClass::None);
    // First match wins in the order shifted, unmeasured, clean.
    EXPECT_EQ(cls({rowResumption(3, "G09", "L1C", "L2W", true, false, 4, 0.0)},
                  {sigResumption(3, "G09", "L2W", false, false, 1, 0.0)}),
              RingFixResumeClass::Unmeasured);
}

// ---- Each class under each value ----

TEST(RingFixResume, EachClassUnderEachValue) {
    struct Case {
        RingFixResumeClass cls;
        GapResumption row;
    };
    const Case cases[] = {
        {RingFixResumeClass::Clean, rowResumption(3, "G09", "L1C", "L2W", true, false, 4, 0.01)},
        {RingFixResumeClass::Shifted, rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.02)},
        {RingFixResumeClass::Unmeasured, rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)},
    };
    for (const Case& c : cases) {
        for (RingFixResumeResponse r :
             {RingFixResumeResponse::Off, RingFixResumeResponse::Release, RingFixResumeResponse::Revalidate}) {
            NetworkRingFix rf = heldState(r);
            const auto edges = resumeAt(rf, quietObs(), {c.row}, {});
            const RingFixLink& L = rf.links.at(kId);
            const std::string what =
                std::string(ringFixResumeClassName(c.cls)) + " under " + ringFixResumeResponseName(r);
            const bool responds = r != RingFixResumeResponse::Off && c.cls != RingFixResumeClass::Clean;
            EXPECT_EQ(edges.size(), responds ? 1u : 0u) << what;
            EXPECT_EQ(L.held, !responds) << what;
            if (!responds) {
                EXPECT_FALSE(L.crossing.active) << what;
                continue;
            }
            EXPECT_EQ(edges[0].cls, c.cls) << what;
            if (r == RingFixResumeResponse::Release) {
                EXPECT_EQ(edges[0].kind, RingFixResumeEdge::Kind::Release) << what;
                EXPECT_EQ(rf.counters.released[static_cast<std::size_t>(RingFixRelease::Resume)], 1) << what;
                EXPECT_FALSE(L.crossing.active) << what;
                EXPECT_EQ(L.released_s, 572730.0) << what;
            } else {
                EXPECT_EQ(edges[0].kind, RingFixResumeEdge::Kind::Suspend) << what;
                EXPECT_EQ(rf.counters.released[static_cast<std::size_t>(RingFixRelease::Resume)], 0) << what;
                EXPECT_TRUE(L.crossing.active) << what;
                EXPECT_EQ(L.crossing.n1, -71) << what;
                EXPECT_EQ(L.crossing.n2, -130) << what;
                EXPECT_EQ(L.crossing.cls, c.cls) << what;
            }
        }
    }
}

TEST(RingFixResume, ACleanResumptionKeepsTheHoldUnderBothValuesAndIsCounted) {
    for (RingFixResumeResponse r : {RingFixResumeResponse::Release, RingFixResumeResponse::Revalidate}) {
        NetworkRingFix rf = heldState(r);
        EXPECT_TRUE(resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", true, false, 4, 0.0)}, {}).empty());
        EXPECT_TRUE(rf.links.at(kId).held);
        EXPECT_EQ(rf.counters.resume_kept_clean, 1);
    }
}

TEST(RingFixResume, SlipAndRootChangeKeepTheirOwnCauses) {
    for (RingFixResumeResponse r : {RingFixResumeResponse::Release, RingFixResumeResponse::Revalidate}) {
        NetworkRingFix rf = heldState(r);
        RingFixHoldObs o = quietObs();
        o.latched = true;
        EXPECT_TRUE(resumeAt(rf, o, {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {}).empty());
        const auto ev = ringFixReleaseStep(rf, {{kId, o}}, 572730.0);
        ASSERT_EQ(ev.size(), 1u);
        EXPECT_EQ(ev[0].release, RingFixRelease::Slip);
    }
}

// ---- The off identity ----

TEST(RingFixResume, OffLeavesTheStateAndTheReleaseStepAsWithoutTheKey) {
    NetworkRingFix with_key = heldState(RingFixResumeResponse::Off);
    NetworkRingFix without = heldState(RingFixResumeResponse::Off);
    RingFixHoldObs o = quietObs();
    o.present = false;
    o.carry_ok = false;   // BRV R3's branch: not present, no carry, no latch
    EXPECT_TRUE(resumeAt(with_key, o, {rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.02)}, {}).empty());
    const auto a = ringFixReleaseStep(with_key, {{kId, o}}, 572730.0);
    RingFixHoldObs plain = o;
    plain.resume = {};
    const auto b = ringFixReleaseStep(without, {{kId, plain}}, 572730.0);
    ASSERT_EQ(a.size(), b.size());
    const RingFixLink &La = with_key.links.at(kId), &Lb = without.links.at(kId);
    EXPECT_EQ(La.held, Lb.held);
    EXPECT_EQ(La.absent_since_s, Lb.absent_since_s);
    EXPECT_EQ(La.released_s, Lb.released_s);
    EXPECT_FALSE(La.crossing.active);
    for (std::size_t k = 0; k < kRingFixReleaseCount; ++k)
        EXPECT_EQ(with_key.counters.released[k], without.counters.released[k]);
    // Today's behaviour on this branch: the link stays held and only the absence clock starts.
    EXPECT_TRUE(La.held);
    EXPECT_EQ(La.absent_since_s, 572730.0);
}

// ---- BRV R3 and R2 ----

TEST(RingFixResume, TheDoy143ReBaseRouteRespondsPresentOrNot) {
    // BOGT00COL G09 on DOY 143: a chain re-host 30 s before the return erased the carry and set rebase_stamps, which
    // blocks the refloat test; the link may be present at the return or not.  Both respond in the resumption epoch.
    for (bool present : {true, false}) {
        for (RingFixResumeResponse r : {RingFixResumeResponse::Release, RingFixResumeResponse::Revalidate}) {
            NetworkRingFix rf = heldState(r);
            rf.links.at(kId).rebase_stamps = true;
            RingFixHoldObs o = quietObs();
            o.present = present;
            o.carry_ok = false;
            o.latched = false;
            const std::vector<GapResumption> rec{rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.0204, 1)};
            // Today the release step returns nothing for this link in the resumption epoch.
            NetworkRingFix today = heldState(RingFixResumeResponse::Off);
            today.links.at(kId).rebase_stamps = true;
            EXPECT_TRUE(ringFixReleaseStep(today, {{kId, o}}, 572730.0).empty());
            const auto edges = resumeAt(rf, o, rec, {});
            ASSERT_EQ(edges.size(), 1u) << "present " << present;
            EXPECT_EQ(edges[0].cls, RingFixResumeClass::Shifted);
            EXPECT_TRUE(edges[0].carry_fresh);
            EXPECT_FALSE(rf.links.at(kId).held);
        }
    }
}

TEST(RingFixResume, BrvR3TheResumedLinkWithoutCarryIsReleasedAtTheResumption) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Release);
    RingFixHoldObs o = quietObs();
    o.present = false;
    o.carry_ok = false;
    const auto edges = resumeAt(rf, o, {rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.0204, 1)}, {});
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].kind, RingFixResumeEdge::Kind::Release);
    EXPECT_TRUE(edges[0].carry_fresh);
    EXPECT_EQ(edges[0].lli, 1);
    EXPECT_NEAR(edges[0].d_gf_m, -1.0204, 1e-12);
    EXPECT_FALSE(rf.links.at(kId).held);
    EXPECT_EQ(rf.counters.resume_fresh, 1);
}

TEST(RingFixResume, BrvR2ASuspendedLinkIsNotAHeldEntryAtALaterCut) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {});
    ASSERT_TRUE(rf.links.at(kId).crossing.active);
    // Ten epochs later the link enters the tree through a cut: the entry is not a held one, so the step is unknown.
    RingFixEntryEvidence ev;
    ev.usable = rf.links.at(kId).held;   // the cut stage asks for a held, unlatched, untripped link
    EXPECT_EQ(chainEntryStep(ChainEntryKind::CutEntry, ev, true, 0.30), RingFixEntryVerdict::NotHeld);
    EXPECT_EQ(chainEntryStep(ChainEntryKind::CutEntry, ev, false, 0.30), RingFixEntryVerdict::NotHeld);
    // A cut across the link ends the record: its integers are no longer in the frame they were decided in.
    RingFixCut cut;
    cut.sys = "G";
    cut.signal = "L1C";
    cut.stations = {3};
    const auto edges = ringFixEndCrossingsAtCuts(rf, {cut}, 573030.0);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].kind, RingFixResumeEdge::Kind::Clear);
    EXPECT_EQ(edges[0].verdict, "ended:cut");
    EXPECT_FALSE(rf.links.at(kId).crossing.active);
}

// ---- The suspension's second edge ----

TEST(RingFixResume, TheReValidationIsATestInTheFloatsOwnScaleNotAFixedSize) {
    // The same float offset from the held integers is accepted for a noisy float and rejected for a precise one: the
    // verdict follows the float's own covariance, as it must for a much noisier receiver class.
    const RingFixDecideObs precise = [] { RingFixDecideObs o = floatNear(kId, -71, -130, 1e-4); o.f1 += 0.05; return o; }();
    const RingFixDecideObs noisy = [] { RingFixDecideObs o = floatNear(kId, -71, -130, 4e-3); o.f1 += 0.05; return o; }();
    double Tp = 0.0, Tn = 0.0;
    const auto gate = [](const RingFixDecideObs& o) {
        return ringFixGate(o.f1, o.f2, o.q11, o.q12, o.q22, o.lambda1, o.lambda2, 1.0);
    };
    ringFixResumeTest(gate(precise), precise, -71, -130, 1.0, &Tp);
    ringFixResumeTest(gate(noisy), noisy, -71, -130, 1.0, &Tn);
    const double q = -2.0 * std::log(kRingFixResumeLevel);
    EXPECT_GT(Tp, q);
    EXPECT_LT(Tn, q);
    // The run's factor on the formal sigmas widens the test as it widens the odds.
    double Ts = 0.0;
    ringFixResumeTest(gate(precise), precise, -71, -130, 3.0, &Ts);
    EXPECT_NEAR(Ts, Tp / 9.0, 1e-9 * Tp);
    // No usable covariance: no evidence.
    RingFixDecideObs flat = precise;
    flat.q12 = flat.q11;   // singular
    EXPECT_EQ(ringFixResumeTest(gate(flat), flat, -71, -130, 1.0), RingFixResumeTest::None);
}

TEST(RingFixResume, TenAgreeingEpochsHoldTheLinkAgainAtItsIntegersWithoutTheWindow) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {});
    const long long decided = rf.counters.decided;
    std::vector<RingFixResumeEdge> edges;
    double now = 572760.0;
    for (int k = 0; k < kRingFixPersistEpochs; ++k, now += 30.0) {
        edges.clear();
        ringFixDecideStep(rf, {floatNear(kId, -71, -130)}, now, &edges);
        if (k + 1 < kRingFixPersistEpochs) EXPECT_TRUE(edges.empty()) << k;
    }
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].verdict, "revalidated");
    const RingFixLink& L = rf.links.at(kId);
    EXPECT_TRUE(L.held);
    EXPECT_EQ(L.n1, -71);
    EXPECT_EQ(L.n2, -130);
    EXPECT_FALSE(L.crossing.active);
    EXPECT_EQ(rf.counters.decided, decided);   // a re-hold is not a new decision
    EXPECT_EQ(rf.counters.resume_revalidated[static_cast<std::size_t>(RingFixResumeClass::Unmeasured)], 1);
}

TEST(RingFixResume, AGatePassAtOtherIntegersContradictsTheRecord) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.02)}, {});
    std::vector<RingFixResumeEdge> edges;
    ringFixDecideStep(rf, {floatNear(kId, -70, -129)}, 572760.0, &edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].verdict, "contradicted");
    EXPECT_EQ(edges[0].m1, -70);
    EXPECT_EQ(edges[0].m2, -129);
    EXPECT_FALSE(rf.links.at(kId).crossing.active);
    EXPECT_FALSE(rf.links.at(kId).held);
    EXPECT_EQ(rf.counters.released[static_cast<std::size_t>(RingFixRelease::Resume)], 1);
}

TEST(RingFixResume, ARecordNotRevalidatedInTimeExpires) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {});
    std::vector<RingFixResumeEdge> edges;
    RingFixDecideObs noisy = floatNear(kId, -71, -130, 1e-1);   // the odds rule decides nothing yet
    for (double now = 572760.0; now - 572730.0 < kRingFixWindowS; now += 30.0) {
        ringFixDecideStep(rf, {noisy}, now, &edges);
        EXPECT_TRUE(edges.empty()) << now;   // within the decision window the record stands
    }
    ringFixDecideStep(rf, {noisy}, 572730.0 + kRingFixWindowS, &edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].verdict, "expired");
    EXPECT_FALSE(rf.links.at(kId).crossing.active);
    EXPECT_EQ(rf.counters.resume_expired[static_cast<std::size_t>(RingFixResumeClass::Unmeasured)], 1);
}

TEST(RingFixResume, ALatchWhileSuspendedEndsTheRecord) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {});
    RingFixHoldObs o = quietObs();
    o.latched = true;
    const auto edges = resumeAt(rf, o, {}, {}, 572790.0);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].verdict, "ended:slip");
    EXPECT_FALSE(rf.links.at(kId).crossing.active);
}

TEST(RingFixResume, TheClosureOfTheSuspensionsHolds) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    resumeAt(rf, quietObs(), {rowResumption(3, "G09", "L1C", "L2W", false, false, 1, 0.0)}, {});
    const RingFixCounters& C = rf.counters;
    long long suspended = 0, closed = 0;
    for (std::size_t k = 0; k < kRingFixResumeClassCount; ++k) {
        suspended += C.resume_suspended[k];
        closed += C.resume_revalidated[k] + C.resume_contradicted[k] + C.resume_expired[k] + C.resume_ended[k];
    }
    EXPECT_EQ(suspended, closed + ringFixActiveCrossings(rf));
}

// ---- The saved state ----

TEST(RingFixResume, TheRecordSurvivesTheMemoryWriteAndRead) {
    NetworkRingFix rf = heldState(RingFixResumeResponse::Revalidate);
    std::ostringstream none;
    ringFixWriteMemory(rf, none);
    EXPECT_EQ(none.str().find("\nRX,"), std::string::npos);   // no record, no line
    RingFixHoldObs fresh = quietObs();
    fresh.carry_ok = false;   // carry_fresh true: every field of the record away from its default
    resumeAt(rf, fresh, {rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.0204)}, {});
    ASSERT_TRUE(rf.links.at(kId).crossing.carry_fresh);
    rf.links.at(kId).crossing.agree = 3;
    rf.links.at(kId).crossing.epochs = 4;
    std::ostringstream os;
    ringFixWriteMemory(rf, os);
    NetworkRingFix back;
    back.resume_response = RingFixResumeResponse::Revalidate;
    std::istringstream is(os.str());
    for (std::string line; std::getline(is, line);) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, ',');) f.push_back(c);
        ASSERT_TRUE(ringFixReadMemoryLine(back, f)) << line;
    }
    const RingFixCrossing& a = rf.links.at(kId).crossing;
    const RingFixCrossing& b = back.links.at(kId).crossing;
    EXPECT_TRUE(b.active);
    EXPECT_EQ(b.since_s, a.since_s);
    EXPECT_EQ(b.cls, a.cls);
    EXPECT_EQ(b.carry_fresh, a.carry_fresh);
    EXPECT_EQ(b.n1, a.n1);
    EXPECT_EQ(b.n2, a.n2);
    EXPECT_EQ(b.agree, 3);
    EXPECT_EQ(b.epochs, 4);
    EXPECT_NEAR(b.d_gf_m, a.d_gf_m, 1e-9);
    EXPECT_FALSE(back.links.at(kId).held);
}

// ---- The five resumptions of the adopted days ----

namespace {

/// One observed case: the link, its held integers, its resumption record and its carry at the return.
struct ObservedCase {
    const char*   what;
    RingFixLinkId id;
    long long     n1, n2;
    double        tow;
    GapResumption row;
    bool          carry_ok;
    bool          present;
};

std::vector<ObservedCase> observedCases() {
    return {
        // _rap_ad_n143: shifted_fresh, gap 15, dGF -1.0204, LLI 11, no carry, no slip mark; folded at -0.65 m.
        {"BOGT00COL G09 DOY 143", {3, "G09", "L1C", "L2W"}, -71, -130, 572730.0,
         rowResumption(3, "G09", "L1C", "L2W", true, true, 15, -1.0204, 1), false, true},
        // _rap_ad_n144: unmeas_bridged, gap 1 (a one-epoch loss of L2W); folded at -27.36 m.
        {"POTS00DEU G13 DOY 144", {7, "G13", "L1C", "L2W"}, 8, -10, 18930.0,
         rowResumption(7, "G13", "L1C", "L2W", false, false, 1, 0.0), true, true},
        // _rap_ad_n145: unmeas_bridged, gap 6; folded at 5.04 m.
        {"KIT300UZB G29 DOY 145", {11, "G29", "L1C", "L2W"}, -51, 217, 119220.0,
         rowResumption(11, "G29", "L1C", "L2W", false, false, 6, 0.0), true, true},
        // _rap_ad_n147: unmeas_bridged, gap 3; folded at -0.010 and -0.033 m.
        {"KIT300UZB G29 DOY 147", {11, "G29", "L1C", "L2W"}, 61, 44, 291450.0,
         rowResumption(11, "G29", "L1C", "L2W", false, false, 3, 0.0), true, true},
        // _rap_ad_n147: unmeas_bridged, gap 3; folded at -0.137 and -0.133 m.
        {"KIT300UZB G10 DOY 147", {11, "G10", "L1C", "L2W"}, -128, -169, 309600.0,
         rowResumption(11, "G10", "L1C", "L2W", false, false, 3, 0.0), true, true},
    };
}

}  // namespace

TEST(RingFixResume, TheFiveObservedCasesRespondInTheResumptionEpoch) {
    for (const ObservedCase& c : observedCases()) {
        for (RingFixResumeResponse r :
             {RingFixResumeResponse::Off, RingFixResumeResponse::Release, RingFixResumeResponse::Revalidate}) {
            NetworkRingFix rf = heldState(r, c.id, c.n1, c.n2, c.tow);
            rf.links.at(c.id).rebase_stamps = !c.carry_ok;   // BOGT00COL: the re-host before the return erased the carry
            RingFixHoldObs o = quietObs();
            o.present = c.present;
            o.carry_ok = c.carry_ok;
            const auto edges = resumeAt(rf, o, {c.row}, {}, c.tow, c.id);
            const std::string what = std::string(c.what) + " under " + ringFixResumeResponseName(r);
            if (r == RingFixResumeResponse::Off) {
                // Today: no response in the resumption epoch, and the release step keeps the hold.
                EXPECT_TRUE(edges.empty()) << what;
                EXPECT_TRUE(ringFixReleaseStep(rf, {{c.id, o}}, c.tow).empty()) << what;
                EXPECT_TRUE(rf.links.at(c.id).held) << what;
                continue;
            }
            ASSERT_EQ(edges.size(), 1u) << what;
            EXPECT_EQ(edges[0].id.sat, c.id.sat) << what;
            EXPECT_EQ(edges[0].kind, r == RingFixResumeResponse::Release ? RingFixResumeEdge::Kind::Release
                                                                         : RingFixResumeEdge::Kind::Suspend)
                << what;
            EXPECT_FALSE(rf.links.at(c.id).held) << what;   // no hold row in this epoch's PASS B
            EXPECT_EQ(edges[0].carry_fresh, !c.carry_ok) << what;
        }
    }
}
