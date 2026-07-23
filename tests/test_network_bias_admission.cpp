/**
 * @file test_network_bias_admission.cpp
 * @brief Unit tests for the CB-ADM W6 code-bias admission census
 *        (filter/NetworkBiasAdmission.hpp).
 *
 * The census is a pure scan over a synthetic std::vector<SyncedEpoch>: it
 * classifies each (satellite,signal) raw-code pair into ADMITTED (shared
 * column) or QUARANTINED (station-local sponge) using a sustained-support
 * count floor (tier 1) and an IF-pinnability band-distinctness rule (tier 2).
 * These tests assert the classification directly on the returned POD — no
 * estimator, no modeling pipeline, fully deterministic.
 */

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "filter/NetworkBiasAdmission.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/StateKeySchema.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"

namespace GnssProcessor {
namespace {

using Verdict = NetworkBiasAdmission::Verdict;

/// @brief One satellite's raw code observations at a single station/epoch.
RinexObservation makeObs(const std::string& sat,
                         const std::vector<std::pair<std::string, double>>& codes) {
    RinexObservation o;
    o.satellite = sat;
    for (const auto& [code, val] : codes) o.measurements[code] = val;
    return o;
}

/// @brief Look up a pair's verdict in the census table (returns nullptr if absent).
const NetworkBiasAdmission::Pair* find(const NetworkBiasAdmission& a,
                                       const std::string& sat, const std::string& sig) {
    const std::string key = StateKeys::makeSatBiasKey(sat, sig);
    for (const auto& p : a.census)
        if (p.key == key) return &p;
    return nullptr;
}

/// @brief Fixture builder: a run of @p n_epochs, each carrying the SAME set of
///        (station -> [obs...]) observations (a fully-tracked steady roster).
///        Individual tests mutate the returned vector to introduce gaps.
struct CensusBuilder {
    std::vector<SyncedEpoch> synced;

    void addEpochs(int n, const std::map<std::string, std::vector<RinexObservation>>& by_station) {
        for (int i = 0; i < n; ++i) {
            SyncedEpoch se;
            se.by_station = by_station;
            synced.push_back(std::move(se));
        }
    }
};

// A GPS satellite tracked on two distinct bands (L1 + L2) at two stations, over
// a steady window, is ADMITTED on both pairs (both tiers pass).
TEST(NetworkBiasAdmissionTest, SteadyTwoBandTwoStationAdmitted) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, /*i0=*/0,
                                                   /*census_epochs=*/15, /*min_stations=*/2);
    EXPECT_TRUE(a.active);
    EXPECT_EQ(a.census_epochs_used, 6);       // K_eff clamped to available span
    EXPECT_EQ(a.census_stations, 2);
    EXPECT_EQ(a.effective_min, 2);            // max(2, min(2,2))
    EXPECT_EQ(a.sustained_floor, 3);          // ceil(6/2)
    EXPECT_EQ(a.n_admitted, 2);
    EXPECT_EQ(a.n_quarantined_census, 0);
    EXPECT_TRUE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C1C")));
    EXPECT_TRUE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C2W")));
    const auto* p = find(a, "G01", "C1C");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->support, 2);
    EXPECT_EQ(p->verdict, Verdict::Admitted);
}

// Tier-2 demotion: a satellite tracked at enough stations but on a SINGLE band
// cannot form an IF pin — ALL its candidates are quarantined (NoIfPin).
TEST(NetworkBiasAdmissionTest, SingleBandSatelliteDemotedNoIfPin) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}};  // L1 only
    b.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    EXPECT_TRUE(a.active);
    EXPECT_EQ(a.n_admitted, 0);
    EXPECT_EQ(a.n_quarantined_census, 1);
    EXPECT_FALSE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C1C")));
    const auto* p = find(a, "G01", "C1C");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->support, 2);                 // tier-1 PASSES (2 stations)...
    EXPECT_EQ(p->verdict, Verdict::NoIfPin);  // ...but tier-2 (IF pin) fails.
}

// Two same-band co-channels (C1C + C1W, both L1) are NOT distinct frequencies,
// so the satellite still fails tier-2 (mirrors selectIfPinPair skipping a
// same-band co-channel).
TEST(NetworkBiasAdmissionTest, SameBandCoChannelsNotDistinct) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C1W", 2.4e7}};
    b.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    EXPECT_EQ(a.n_admitted, 0);
    EXPECT_EQ(find(a, "G01", "C1C")->verdict, Verdict::NoIfPin);
    EXPECT_EQ(find(a, "G01", "C1W")->verdict, Verdict::NoIfPin);
}

// Sustained-support floor: a station that tracks the pair in FEWER than
// ceil(K_eff/2) epochs does not "support" it, so a 2-station pair where one
// station is gappy drops to support=1 and quarantines (LowCount).
TEST(NetworkBiasAdmissionTest, SustainedSupportFloorRejectsGappyStation) {
    // K_eff = 6 -> floor = 3.  STA0 tracks G01 in all 6 epochs; STA1 only in 2.
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> both = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(6, {{"STA0", {makeObs("G01", both)}},
                    {"STA1", {makeObs("G01", both)}}});
    // Knock STA1 out of 4 of the 6 epochs (leave it present in epochs 0,1 only).
    for (int i = 2; i < 6; ++i) b.synced[i].by_station.erase("STA1");
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    // STA1 supports (2 epochs) < floor(3) => support=1 < effective_min(2).
    const auto* p = find(a, "G01", "C1C");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->support, 1);
    EXPECT_EQ(p->verdict, Verdict::LowCount);
    EXPECT_EQ(a.n_admitted, 0);
    // census_stations still counts STA1 (it appeared in the window).
    EXPECT_EQ(a.census_stations, 2);
}

// effective_min clamp keeps a 3-station dev roster runnable: with
// min_stations=4 but only 3 census stations, effective_min = max(2,min(4,3)) = 3.
TEST(NetworkBiasAdmissionTest, EffectiveMinClampThreeStationRoster) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}},
                    {"STA2", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, 15, /*min_stations=*/4);
    EXPECT_EQ(a.census_stations, 3);
    EXPECT_EQ(a.effective_min, 3);
    EXPECT_EQ(a.n_admitted, 2);               // all 3 stations support => >= 3
    EXPECT_TRUE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C1C")));

    // Drop STA2 entirely => only 2 stations support => 2 < effective_min(3).
    CensusBuilder b2;
    b2.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                     {"STA1", {makeObs("G01", g01)}}});
    const auto a2 = NetworkBiasAdmission::runCensus(b2.synced, 0, 15, 4);
    EXPECT_EQ(a2.effective_min, 2);           // clamp: max(2, min(4,2))
    EXPECT_EQ(a2.n_admitted, 2);              // 2 supporting == effective_min(2) => admit
}

// Late-seen default: a pair appearing only AFTER the census window is never
// classified (quarantined by default at the registration site later).
TEST(NetworkBiasAdmissionTest, LateSeenPairNotInCensus) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(4, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    // A late epoch introduces G05 — beyond census_epochs=4, so it is unseen.
    SyncedEpoch late;
    late.by_station = {{"STA0", {makeObs("G05", g01)}}, {"STA1", {makeObs("G05", g01)}}};
    b.synced.push_back(std::move(late));
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, /*census_epochs=*/4, 2);
    EXPECT_EQ(a.census_epochs_used, 4);
    EXPECT_FALSE(a.isAdmitted(StateKeys::makeSatBiasKey("G05", "C1C")));
    EXPECT_EQ(find(a, "G05", "C1C"), nullptr);  // never entered the census table
    EXPECT_TRUE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C1C")));
}

// K_eff clamp: census_epochs larger than the available span classifies over the
// available count (a short smoke run still works).
TEST(NetworkBiasAdmissionTest, KEffClampToAvailableSpan) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(3, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, /*census_epochs=*/15, 2);
    EXPECT_TRUE(a.active);
    EXPECT_EQ(a.census_epochs_used, 3);
    EXPECT_EQ(a.sustained_floor, 2);          // ceil(3/2)
    EXPECT_EQ(a.n_admitted, 2);
}

// Exclusions: GLONASS satellites, phase/SNR observations, non-3-char codes,
// composite '_' keys, and present-but-zero values never enter the census.
TEST(NetworkBiasAdmissionTest, ExcludesGlonassPhaseAndZeroValues) {
    CensusBuilder b;
    std::vector<std::pair<std::string, double>> mixed = {
        {"C2W", 2.4e7},   // usable GPS code -> counted
        {"L2W", 1.2e8},   // phase -> excluded (type != 'C')
        {"S2W", 45.0},    // SNR   -> excluded
        {"C1C", 0.0},     // present-but-zero -> excluded
    };
    b.addEpochs(6, {
        {"STA0", {makeObs("G10", mixed), makeObs("R07", {{"C1C", 2.4e7}, {"C2C", 2.4e7}})}},
        {"STA1", {makeObs("G10", mixed), makeObs("R07", {{"C1C", 2.4e7}, {"C2C", 2.4e7}})}},
    });
    const auto a = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    // GLONASS R07 excluded entirely.
    EXPECT_EQ(find(a, "R07", "C1C"), nullptr);
    // Only the single usable GPS code pair appears; L2W/S2W/zero-C1C excluded.
    EXPECT_NE(find(a, "G10", "C2W"), nullptr);
    EXPECT_EQ(find(a, "G10", "C1C"), nullptr);  // zero value excluded
    EXPECT_EQ(find(a, "G10", "L2W"), nullptr);
    EXPECT_EQ(static_cast<int>(a.census.size()), 1);
    // G10 has a single usable band => tier-2 fails => quarantined.
    EXPECT_EQ(find(a, "G10", "C2W")->verdict, Verdict::NoIfPin);
}

// i0 window offset: the census scans from i0, ignoring earlier epochs.
TEST(NetworkBiasAdmissionTest, RespectsI0Offset) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    // Epoch 0 carries a decoy satellite G09 that appears NOWHERE else.
    SyncedEpoch e0;
    e0.by_station = {{"STA0", {makeObs("G09", g01)}}, {"STA1", {makeObs("G09", g01)}}};
    b.synced.push_back(std::move(e0));
    b.addEpochs(6, {{"STA0", {makeObs("G01", g01)}},
                    {"STA1", {makeObs("G01", g01)}}});
    const auto a = NetworkBiasAdmission::runCensus(b.synced, /*i0=*/1, 15, 2);
    EXPECT_EQ(a.census_epochs_used, 6);
    EXPECT_EQ(find(a, "G09", "C1C"), nullptr);  // epoch 0 not scanned
    EXPECT_TRUE(a.isAdmitted(StateKeys::makeSatBiasKey("G01", "C1C")));
}

// Determinism: identical inputs yield identical results run-to-run.
TEST(NetworkBiasAdmissionTest, Deterministic) {
    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    const std::vector<std::pair<std::string, double>> c21 = {{"C2I", 2.4e7}, {"C6I", 2.4e7}};
    b.addEpochs(8, {{"STA0", {makeObs("G01", g01), makeObs("C21", c21)}},
                    {"STA1", {makeObs("G01", g01), makeObs("C21", c21)}}});
    const auto a1 = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    const auto a2 = NetworkBiasAdmission::runCensus(b.synced, 0, 15, 2);
    EXPECT_EQ(a1.admitted, a2.admitted);
    ASSERT_EQ(a1.census.size(), a2.census.size());
    for (size_t i = 0; i < a1.census.size(); ++i) {
        EXPECT_EQ(a1.census[i].key, a2.census[i].key);
        EXPECT_EQ(a1.census[i].verdict, a2.census[i].verdict);
    }
    // BeiDou two-band pair admitted alongside GPS — the card's target case.
    EXPECT_TRUE(a1.isAdmitted(StateKeys::makeSatBiasKey("C21", "C2I")));
    EXPECT_TRUE(a1.isAdmitted(StateKeys::makeSatBiasKey("C21", "C6I")));
}

// Inactive / empty-window guards: no classification => active=false (the
// registration site then falls back to the legacy per-epoch gate).
TEST(NetworkBiasAdmissionTest, EmptyWindowInactive) {
    std::vector<SyncedEpoch> empty;
    EXPECT_FALSE(NetworkBiasAdmission::runCensus(empty, 0, 15, 2).active);

    CensusBuilder b;
    const std::vector<std::pair<std::string, double>> g01 = {{"C1C", 2.4e7}, {"C2W", 2.4e7}};
    b.addEpochs(3, {{"STA0", {makeObs("G01", g01)}}});
    EXPECT_FALSE(NetworkBiasAdmission::runCensus(b.synced, /*i0=*/5, 15, 2).active);   // i0 past end
    EXPECT_FALSE(NetworkBiasAdmission::runCensus(b.synced, 0, /*census_epochs=*/0, 2).active);
    // A default-constructed result is the OFF state.
    EXPECT_FALSE(NetworkBiasAdmission{}.active);
    EXPECT_FALSE(NetworkBiasAdmission{}.isAdmitted("G01|C1C"));
}

// INC-2 registration-swap contract (mirrors ApplicationNetworkAssembly's ON
// branch): an ADMITTED (sat,signal) registers a shared column that
// codeBiasColumn resolves; a QUARANTINED (unregistered) pair makes
// codeBiasColumn THROW — the exact signal the scatter's catch{continue} uses to
// leave that state a station-local column.  Also exercises the late-quarantine
// dedup used to drive n_cb_late_quarantined.
TEST(NetworkBiasAdmissionTest, RegistrationSwapContract) {
    NetworkBiasAdmission adm;              // simulate: census admitted C21|C2I only
    adm.active   = true;
    adm.admitted = {"C21|C2I"};
    adm.seen     = {"C21|C2I", "C21|C6I"};  // C6I was seen but quarantined at census

    NetworkEstimator est;
    // Mirror the reg-site swap: register ONLY admitted keys.
    for (const std::string& key : {std::string("C21|C2I"), std::string("C21|C6I")}) {
        const auto parts = StateKeys::parseSatBiasKey(key);
        ASSERT_TRUE(parts.has_value());
        if (adm.isAdmitted(key))
            est.registerCodeBias(parts->satellite, parts->signal);
    }
    est.beginEpoch();
    EXPECT_NO_THROW((void)est.codeBiasColumn("C21", "C2I"));   // admitted -> shared column
    EXPECT_ANY_THROW((void)est.codeBiasColumn("C21", "C6I"));  // quarantined -> throws -> stays local

    // Late-quarantine dedup: a key the census never classified (neither admitted
    // nor seen) counts exactly once.
    EXPECT_EQ(adm.seen.count("C21|C7I"), 0u);
    EXPECT_TRUE(adm.late_tracked.insert("C21|C7I").second);   // first sighting
    EXPECT_FALSE(adm.late_tracked.insert("C21|C7I").second);  // subsequent epochs no-op
}

}  // namespace
}  // namespace GnssProcessor
