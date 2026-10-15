/**
 * @file test_param_responses.cpp
 * @brief Card PRM phase 3a: the declared responses of the carried instances
 *        (filter/NetworkResponses), the registry's per-event policies built from them, the
 *        free-epoch latch's per-cause tally, and the traversal's per-kind tally of the border
 *        moves (the Stransform responses).
 *
 * The phase changes no number; the byte-identity A/B (scripts/prm_ab.py) is the end-to-end
 * gate, and the run end's declaration check (a site that responded on a run where it is not
 * declared armed) is the run-time one.  These cases pin the table's structure, the goldens'
 * declared posture, and the two tallies the table reads.  Phase 3b step 1 adds the gap
 * policies' one implementation (carryPastHorizon, carryGapPrune) and the registry's declared
 * gap horizon; step 2 the Stransform declaration of the owed value sites under
 * network.carry_rebase_complete.
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkCarrySites.hpp"
#include "filter/NetworkParamRegistry.hpp"
#include "filter/NetworkResponses.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

bool loadGolden(const std::string& bed, GnssConfig& cfg) {
    LogManager log;
    const std::filesystem::path p =
        std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / "golden" /
        ("golden_" + bed + "_refound.json");
    return cfg.loadFromJson(p, log);
}

bool has(std::uint32_t mask, ResponsePolicy p) { return (mask & responsePolicyBit(p)) != 0; }

/// The registry's per-event policies rebuilt from the tables (the armed response sites, and
/// the armed mechanisms' Stransform on the kinds whose values they move, the owed value sites
/// included under network.carry_rebase_complete), compared with @p r both ways.
void expectMasksFromTheTables(const NetworkParamRegistry& r, const NetworkKnobs& k,
                              const GnssConfig& cfg, const std::string& tag) {
    std::uint32_t expect[kNetworkParamKindCount][kParamEvents]{};
    for (int i = 0; i < kResponseSites; ++i) {
        const auto site = static_cast<ResponseSite>(i);
        if (!responseSiteArmed(site, k, cfg)) continue;
        const ResponseSiteSpec& rs = responseSiteSpec(site);
        expect[static_cast<int>(rs.kind)][static_cast<int>(rs.event)] |=
            responsePolicyBit(rs.policy);
    }
    const auto ho = static_cast<int>(ParamEvent::Handover);
    const std::uint32_t st = responsePolicyBit(ResponsePolicy::Stransform);
    for (int m = 0; m < kCarryMechanisms; ++m) {
        const auto mech = static_cast<CarryMechanism>(m);
        if (!carryMechanismArmed(mech, k, cfg)) continue;
        for (int b = 0; b < 4; ++b)
            if ((carryMechanismBorderKinds(mech, k) >> b) & 1u)
                expect[static_cast<int>(paramKindOf(static_cast<SharedParamKind>(b)))][ho] |= st;
        for (int c = 0; c < kCarrySites; ++c) {
            NetworkParamKind lk;
            if (carryMechanismMoves(mech, static_cast<CarrySite>(c), k.rebase_complete)
                && carrySiteLocalKind(static_cast<CarrySite>(c), lk))
                expect[static_cast<int>(lk)][ho] |= st;
        }
    }
    for (std::size_t kind = 0; kind < kNetworkParamKindCount; ++kind)
        for (std::size_t e = 0; e < kParamEvents; ++e)
            EXPECT_EQ(r.spec(static_cast<NetworkParamKind>(kind)).on[e], expect[kind][e])
                << tag << " " << paramKindName(static_cast<NetworkParamKind>(kind)) << " "
                << paramEventName(static_cast<ParamEvent>(e));
}

std::uint32_t on(const NetworkParamRegistry& r, NetworkParamKind kind, ParamEvent e) {
    return r.spec(kind).on[static_cast<std::size_t>(e)];
}

CarryPrior prior(SharedParamKind kind, const std::string& sat, const std::string& sig) {
    CarryPrior cp;
    cp.kind = kind;
    cp.sat = sat;
    cp.signal = sig;
    cp.value = 1.0;
    cp.var = 1e-4;
    return cp;
}

}  // namespace

// The table: one row per enumerator, in enumerator order, with unique names, and a count
// source that fits the kind (the fold's tallies hold border kinds only).
TEST(ParamResponses, TableIsInEnumOrderWithUniqueNamesAndValidSources) {
    std::set<std::string> names;
    for (int i = 0; i < kResponseSites; ++i) {
        const ResponseSiteSpec& rs = responseSiteSpec(static_cast<ResponseSite>(i));
        EXPECT_EQ(static_cast<int>(rs.site), i) << rs.name;
        EXPECT_TRUE(names.insert(rs.name).second) << "duplicate name " << rs.name;
        EXPECT_NE(std::string(rs.where), "") << rs.name;
        EXPECT_NE(std::string(rs.unit), "") << rs.name;
        switch (rs.source) {
            case ResponseCountSource::Increment:
                EXPECT_EQ(rs.disposition, -1) << rs.name;
                break;
            case ResponseCountSource::Disposition:
                EXPECT_GE(responseBorderIndex(rs.kind), 0) << rs.name;
                EXPECT_GT(rs.disposition, 0) << rs.name;
                EXPECT_LT(rs.disposition, kCarryDispositions) << rs.name;
                break;
            case ResponseCountSource::Departed:
                EXPECT_GE(responseBorderIndex(rs.kind), 0) << rs.name;
                EXPECT_EQ(rs.disposition, -1) << rs.name;
                break;
        }
    }
    EXPECT_EQ(static_cast<int>(names.size()), kResponseSites);
}

// The registry's per-event policies are exactly the union of the armed sites' policies and
// the armed mechanisms' Stransform: rebuilt here from the table and compared both ways.
TEST(ParamResponses, RegistryMasksAreExactlyTheArmedSitesAndMechanisms) {
    for (const char* bed : {"field", "sim"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        expectMasksFromTheTables(r, k, cfg, bed);
    }
}

// Arming cases the goldens do not exercise: the clock model's fault test flags satellites
// with no clock monitor, so the phase-bias and code-bias reinit rows are armed (the clock
// itself is on the oscillator, not stashed); the seed-round re-host runs the eq34 shift with
// the shift knob off.
TEST(ParamResponses, ArmingFollowsTheCodesOwnGates) {
    GnssConfig cfg;
    NetworkKnobs k{};
    k.clk_model_on = true;
    k.clk_mon_reinit_sig = 30.0;
    EXPECT_TRUE(responseSiteArmed(ResponseSite::PbReinit, k, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkReinit, k, cfg));   // no clock in prev_carry
    EXPECT_FALSE(responseSiteArmed(ResponseSite::ClkDeparted, k, cfg));
    EXPECT_TRUE(responseSiteArmed(ResponseSite::ClkOscReinit, k, cfg));
    NetworkKnobs q{};
    q.phase_datum_eq34 = true;
    q.eq34_seed_round_rehost = true;
    EXPECT_TRUE(carryMechanismArmed(CarryMechanism::Eq34Shift, q, cfg));
    q.eq34_seed_round_rehost = false;
    EXPECT_FALSE(carryMechanismArmed(CarryMechanism::Eq34Shift, q, cfg));
    // The free-ambiguity lineage carries no local ambiguity: nothing to prune or erase.
    NetworkKnobs ci{};
    ci.pl_on = true;
    ci.carry_invariant = true;
    ci.raim_on = true;
    EXPECT_FALSE(responseSiteArmed(ResponseSite::AmbGapPrune, ci, cfg));
    EXPECT_FALSE(responseSiteArmed(ResponseSite::AmbRaimErase, ci, cfg));
}

// The goldens' declared posture (the float vehicle with RAIM's attribution, the gross screen,
// the phase-anchor re-reference, READMIT, card Q3's anchor and the handover S-transform):
// what each kind does on each event, as the inventory of 2026-10-02 read it in the code.
TEST(ParamResponses, GoldensDeclareTheFloatVehiclesResponses) {
    for (const char* bed : {"field", "sim"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        using K = NetworkParamKind;
        using E = ParamEvent;
        using P = ResponsePolicy;
        // The ambiguity: a slip drops (the latch and RAIM's attribution), never erases.
        EXPECT_TRUE(has(on(r, K::Ambiguity, E::Slip), P::Drop)) << bed;
        EXPECT_FALSE(has(on(r, K::Ambiguity, E::Slip), P::EraseAndRebirth)) << bed;
        EXPECT_TRUE(has(on(r, K::Ambiguity, E::Gap), P::Drop)) << bed;
        EXPECT_TRUE(has(on(r, K::Ambiguity, E::Gap), P::GapPrune)) << bed;
        EXPECT_FALSE(has(on(r, K::Ambiguity, E::Gap), P::GapSkip)) << bed;
        EXPECT_TRUE(has(on(r, K::Ambiguity, E::Handover), P::EraseAndRebirth)) << bed;
        EXPECT_TRUE(has(on(r, K::Ambiguity, E::Decouple), P::EraseAndRebirth)) << bed;
        EXPECT_FALSE(has(on(r, K::Ambiguity, E::Decouple), P::Hold)) << bed;
        // The satellite clock: parked and seeded across a departure, loosened and re-coupled
        // by the anchor's guard, re-expressed at a pivot handover.
        EXPECT_TRUE(has(on(r, K::SatClock, E::Gap), P::Marginalize)) << bed;
        EXPECT_FALSE(has(on(r, K::SatClock, E::Slip), P::Reinit)) << bed;   // no clock monitor
        EXPECT_TRUE(has(on(r, K::SatClock, E::Gap), P::Park)) << bed;
        EXPECT_TRUE(has(on(r, K::SatClock, E::Readmit), P::SeedAtVarPlusQGap)) << bed;
        EXPECT_TRUE(has(on(r, K::SatClock, E::Readmit), P::Recouple)) << bed;
        EXPECT_TRUE(has(on(r, K::SatClock, E::Decouple), P::Loosen)) << bed;
        EXPECT_TRUE(has(on(r, K::SatClock, E::Handover), P::Stransform)) << bed;
        EXPECT_TRUE(has(on(r, K::SatClock, E::Handover), P::Retire)) << bed;
        // The phase bias: a re-referenced frame is not folded for one epoch; it is not parked.
        EXPECT_TRUE(has(on(r, K::SatPhaseBias, E::Handover), P::Rereference)) << bed;
        EXPECT_FALSE(has(on(r, K::SatPhaseBias, E::Slip), P::Reinit)) << bed;
        EXPECT_TRUE(has(on(r, K::SatCodeBias, E::Readmit), P::SeedAtVarPlusQGap)) << bed;
        // The receiver code bias in the block: erased on a step, skipped and pruned on a gap.
        EXPECT_TRUE(has(on(r, K::RxCodeBias, E::Slip), P::EraseAndRebirth)) << bed;
        EXPECT_TRUE(has(on(r, K::RxCodeBias, E::Gap), P::GapPrune)) << bed;
        // Not carried on this posture: no response declared.
        for (const K kind : {K::Zwd, K::SlantIono, K::RxClock, K::RxPhaseBias, K::PhaseColour})
            for (std::size_t e = 0; e < kParamEvents; ++e)
                EXPECT_EQ(r.spec(kind).on[e], 0u) << bed << " " << paramKindName(kind)
                                                  << " " << paramEventName(static_cast<E>(e));
        // The response table names every kind once.
        EXPECT_EQ(r.responseTable().size(), kNetworkParamKindCount) << bed;
    }
}

// An armed datum mechanism declares Stransform on the handover of every kind whose carried
// value it moves: the clock-pivot S-transform the satellite clock; the eq34 D4 shift the
// phase bias, the ambiguity and the receiver phase bias.
TEST(ParamResponses, ArmedMechanismsDeclareStransformOnTheKindsTheyMove) {
    GnssConfig cfg;
    NetworkKnobs k{};
    k.handover_stransform = true;
    k.phase_datum_eq34 = true;
    k.eq34_shift_on = true;
    const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
    using K = NetworkParamKind;
    const auto ho = [&](K kind) { return on(r, kind, ParamEvent::Handover); };
    EXPECT_TRUE(has(ho(K::SatClock), ResponsePolicy::Stransform));
    EXPECT_TRUE(has(ho(K::SatPhaseBias), ResponsePolicy::Stransform));
    EXPECT_TRUE(has(ho(K::Ambiguity), ResponsePolicy::Stransform));
    EXPECT_TRUE(has(ho(K::RxPhaseBias), ResponsePolicy::Stransform));
    EXPECT_FALSE(has(ho(K::SatCodeBias), ResponsePolicy::Stransform));
    EXPECT_FALSE(has(ho(K::RxCodeBias), ResponsePolicy::Stransform));
}

// Card PRM phase 3b step 2: the one owed station-local value is the gross screen's held
// ambiguity (owed by the eq34 shift and the declared anchor handover), whose kind the two
// mechanisms already declare through the ambiguity value; so network.carry_rebase_complete adds
// no kind to the declaration today, and the masks are the same with and without it.
TEST(ParamResponses, RebaseCompleteAddsNoDeclaredKindToday) {
    using K = NetworkParamKind;
    using S = CarrySite;
    using M = CarryMechanism;
    // The owed local values of every mechanism: the held ambiguity, under the two phase-bias
    // shifts, and nothing else.
    for (int m = 0; m < kCarryMechanisms; ++m)
        for (int c = 0; c < kCarrySites; ++c) {
            NetworkParamKind lk;
            const auto s = static_cast<S>(c);
            if (!(carryMechanismOwedSites(static_cast<M>(m)) & carrySiteBit(s))
                || !carrySiteLocalKind(s, lk))
                continue;
            EXPECT_EQ(static_cast<int>(s), static_cast<int>(S::HoldAmbiguityValue))
                << carryMechanismName(static_cast<M>(m));
            EXPECT_EQ(static_cast<int>(lk), static_cast<int>(K::Ambiguity));
            EXPECT_TRUE(m == static_cast<int>(M::Eq34Shift)
                        || m == static_cast<int>(M::DeclaredHandover))
                << carryMechanismName(static_cast<M>(m));
        }
    EXPECT_FALSE(carryMechanismMoves(M::DeclaredHandover, S::HoldAmbiguityValue, false));
    EXPECT_TRUE(carryMechanismMoves(M::DeclaredHandover, S::HoldAmbiguityValue, true));

    GnssConfig cfg;
    NetworkKnobs k{};
    k.amb_float_kf = true;
    k.amb_float_declared_handover = true;
    k.pa_carry_reref = true;
    k.pd_handover_continuous = true;
    ASSERT_TRUE(carryMechanismArmed(M::DeclaredHandover, k, cfg));
    for (const bool complete : {false, true}) {
        k.rebase_complete = complete;
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        const auto ho = [&](K kind) { return on(r, kind, ParamEvent::Handover); };
        EXPECT_TRUE(has(ho(K::Ambiguity), ResponsePolicy::Stransform)) << complete;
        EXPECT_TRUE(has(ho(K::SatPhaseBias), ResponsePolicy::Stransform)) << complete;
        expectMasksFromTheTables(r, k, cfg, complete ? "dho complete" : "dho");
        k.rebase_complete = false;
        const NetworkParamRegistry r_off = NetworkParamRegistry::build(k, cfg);
        k.rebase_complete = complete;
        for (std::size_t kind = 0; kind < kNetworkParamKindCount; ++kind)
            EXPECT_EQ(r.spec(static_cast<K>(kind)).on, r_off.spec(static_cast<K>(kind)).on)
                << complete << " " << paramKindName(static_cast<K>(kind));
        // The held ambiguity is among the sites the knob moves.
        bool via_hold = false;
        for (int c = 0; c < kCarrySites; ++c) {
            NetworkParamKind lk;
            const auto s = static_cast<S>(c);
            if (s != S::AmbiguityValue && carrySiteLocalKind(s, lk) && lk == K::Ambiguity
                && carryMechanismMoves(M::DeclaredHandover, s, k.rebase_complete))
                via_hold = true;
        }
        EXPECT_EQ(via_hold, complete);
    }
    // The eq34 shift with the knob: the same masks as the tables, the ambiguity, the phase bias
    // and the receiver phase bias declared.
    NetworkKnobs q{};
    q.phase_datum_eq34 = true;
    q.eq34_shift_on = true;
    q.rebase_complete = true;
    const NetworkParamRegistry rq = NetworkParamRegistry::build(q, cfg);
    EXPECT_TRUE(has(on(rq, K::Ambiguity, ParamEvent::Handover), ResponsePolicy::Stransform));
    EXPECT_TRUE(has(on(rq, K::RxPhaseBias, ParamEvent::Handover), ResponsePolicy::Stransform));
    expectMasksFromTheTables(rq, q, cfg, "eq34 complete");
}

// The traversal tallies the prev_carry moves per border kind, and the run-scope tally adds
// them per mechanism (the Stransform rows of the run-end table).
TEST(ParamResponses, TraversalTalliesBorderMovesPerKind) {
    NetworkCarryState c; net::NetworkEntities c_ent;
    c.prev_carry = {prior(SharedParamKind::Clock, "G01", ""),
                    prior(SharedParamKind::Clock, "G02", ""),
                    prior(SharedParamKind::PhaseBias, "G01", "L1C"),
                    prior(SharedParamKind::CodeBias, "G01", "C1C")};
    CarrySiteCounts cc;
    const long long n = forEachCarriedReference(
        c, c_ent, carrySiteBit(CarrySite::PrevCarry),
        [](const CarriedRef& ref) { return ref.kind != SharedParamKind::CodeBias; }, &cc);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(cc.moved_border[static_cast<int>(SharedParamKind::Clock)], 2);
    EXPECT_EQ(cc.moved_border[static_cast<int>(SharedParamKind::PhaseBias)], 1);
    EXPECT_EQ(cc.moved_border[static_cast<int>(SharedParamKind::CodeBias)], 0);
    CarryMovedTally t;
    carryTallyMoved(t, CarryMechanism::EphRelevel, cc);
    carryTallyMoved(t, CarryMechanism::EphRelevel, cc);
    const int m = static_cast<int>(CarryMechanism::EphRelevel);
    EXPECT_EQ(t.border[static_cast<int>(SharedParamKind::Clock)][m], 4);
    EXPECT_EQ(t.site[static_cast<int>(CarrySite::PrevCarry)][m], 6);
}

// The latch tally: each latched (station, ambiguity) key counted once per cause bit it
// carries ("none" when no cause is recorded), and as carried when the response finds an
// entry; a key with two causes counts once as unique and once as multi-cause.
TEST(ParamResponses, LatchTallyCountsEachCauseAndTheCarriedKeys) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGolden("sim", cfg));
    auto ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
    auto& lat = ctx->carry.relevel;
    const std::string a = StateKeys::makeAmbiguityKey("G05", "L1C");
    const std::string b = StateKeys::makeAmbiguityKey("G07", "L2W");
    const std::string c = StateKeys::makeAmbiguityKey("G09", "L1C");
    lat.free_ambs = {{0, a}, {1, b}, {2, c}};
    lat.free_cause[{0, a}] = RelevelEpochState::kCauseMw | RelevelEpochState::kCauseLli;
    lat.free_cause[{1, b}] = RelevelEpochState::kCauseGap;
    ctx->carry.local_carry[std::make_tuple(0, std::string("G05"), std::string("L1C"))] = LocalCarry{};
    ctx->carry.local_carry[std::make_tuple(2, std::string("G09"), std::string("L1C"))] = LocalCarry{};
    networkResponseLatchTally(*ctx);
    const auto& C = ctx->counters;
    EXPECT_EQ(C.prm_latch_epochs, 1);
    EXPECT_EQ(C.prm_latch_unique, 3);
    EXPECT_EQ(C.prm_latch_unique_carried, 2);
    EXPECT_EQ(C.prm_latch_multi, 1);
    EXPECT_EQ(C.prm_latch_keys[0], 1);      // mw
    EXPECT_EQ(C.prm_latch_carried[0], 1);
    EXPECT_EQ(C.prm_latch_keys[1], 1);      // lli
    EXPECT_EQ(C.prm_latch_keys[6], 1);      // gap re-entry, no carry
    EXPECT_EQ(C.prm_latch_carried[6], 0);
    EXPECT_EQ(C.prm_latch_keys[kLatchCauses - 1], 1);   // no cause recorded
    EXPECT_EQ(C.prm_latch_carried[kLatchCauses - 1], 1);
    EXPECT_STREQ(latchCauseName(6), "gap_reentry");
    // An empty latch is not an epoch of the tally.
    lat.free_ambs.clear();
    networkResponseLatchTally(*ctx);
    EXPECT_EQ(C.prm_latch_epochs, 1);
}

// The count sources: an Increment row reads prm_resp, a Disposition row the fold's tally of
// its kind and disposition, a Departed row the fold's departed tally.
TEST(ParamResponses, SiteCountReadsItsDeclaredSource) {
    GnssConfig cfg;
    ASSERT_TRUE(loadGolden("sim", cfg));
    auto ctx = std::make_unique<NetworkRunContext>(cfg, cfg.getNetworkConfig());
    auto& C = ctx->counters;
    prmRespond(C.prm_resp, ResponseSite::AmbGapPrune, 7);
    C.prm_disp[static_cast<int>(SharedParamKind::Clock)][static_cast<int>(CarryDisposition::Seeded)] = 4;
    C.prm_departed[static_cast<int>(SharedParamKind::PhaseBias)] = 12;
    EXPECT_EQ(responseSiteCount(C, ResponseSite::AmbGapPrune), 7);
    EXPECT_EQ(responseSiteCount(C, ResponseSite::ClkSeed), 4);
    EXPECT_EQ(responseSiteCount(C, ResponseSite::CbSeed), 0);
    EXPECT_EQ(responseSiteCount(C, ResponseSite::PbDeparted), 12);
}

// Card PRM phase 3b: the GapPrune policy's one implementation erases exactly the entries past
// the horizon (a gap equal to the horizon is inside it), calls on_erase once for each while the
// entry is still in the map, in the map's order, and returns the count.  A NaN horizon prunes
// nothing (the comparison is false).
TEST(ParamResponses, CarryGapPruneErasesExactlyTheEntriesPastTheHorizon) {
    const double now = 10000.0;
    const double horizon = 900.0;
    const auto seenAt = [](double t) {
        LocalCarry lc{};
        lc.last_seen_s = t;
        return lc;
    };
    std::map<int, LocalCarry> m;
    m[1] = seenAt(now - 30.0);      // inside
    m[2] = seenAt(now - 900.0);     // on the horizon: inside (strict >)
    m[3] = seenAt(now - 900.5);     // past
    m[4] = seenAt(now);             // seen this epoch
    m[5] = seenAt(now - 5000.0);    // past
    m[6] = seenAt(now + 30.0);      // a negative gap: never past
    std::vector<int> erased;
    std::size_t expect_size = m.size();   // a call after its erase would see one fewer
    const long long n = carryGapPrune(m, now, horizon, [&](const auto& it) {
        EXPECT_EQ(m.size(), expect_size) << "on_erase runs before the erase";
        --expect_size;
        EXPECT_TRUE(carryPastHorizon(now, it->second.last_seen_s, horizon));
        erased.push_back(it->first);
    });
    EXPECT_EQ(n, 2);
    EXPECT_EQ(erased, (std::vector<int>{3, 5}));
    EXPECT_EQ(m.size(), 4u);
    for (const int key : {1, 2, 4, 6}) EXPECT_EQ(m.count(key), 1u) << key;
    for (const int key : {3, 5}) EXPECT_EQ(m.count(key), 0u) << key;

    // The predicate is the sites' comparison: strict, gap = now - last_seen.
    EXPECT_FALSE(carryPastHorizon(now, now - 900.0, horizon));
    EXPECT_TRUE(carryPastHorizon(now, now - 900.5, horizon));
    EXPECT_FALSE(carryPastHorizon(now, now + 30.0, horizon));

    // A NaN horizon (a kind with no gap horizon) erases nothing and calls nothing.
    int calls = 0;
    const long long n_nan = carryGapPrune(m, now, std::numeric_limits<double>::quiet_NaN(),
                                          [&](const auto&) { ++calls; });
    EXPECT_EQ(n_nan, 0);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(m.size(), 4u);
}

// Card PRM phase 3b: the goldens' registry declares the ambiguity's gap horizon at 900 s, the
// knob snapshot's own double (the prune reads the registry in place of the knob, so the two
// must be the same double), the receiver code bias at the fixed kRxCbMaxGapS, and no gap
// horizon for a border kind.
TEST(ParamResponses, GoldensDeclareTheAmbiguityGapHorizon) {
    for (const char* bed : {"field", "sim"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(bed, cfg)) << bed;
        const NetworkKnobs k = NetworkKnobs::snapshot(cfg, cfg.getNetworkConfig());
        const NetworkParamRegistry r = NetworkParamRegistry::build(k, cfg);
        using K = NetworkParamKind;
        EXPECT_EQ(r.spec(K::Ambiguity).gap_horizon_s, 900.0) << bed;
        EXPECT_EQ(r.spec(K::Ambiguity).gap_horizon_s, k.pl_max_gap_s) << bed;
        EXPECT_EQ(r.spec(K::Zwd).gap_horizon_s, k.pl_max_gap_s) << bed;
        EXPECT_EQ(r.spec(K::RxPhaseBias).gap_horizon_s, k.pl_max_gap_s) << bed;
        EXPECT_EQ(r.spec(K::RxClock).gap_horizon_s, k.rxclk_max_gap_s) << bed;
        EXPECT_EQ(r.spec(K::PhaseColour).gap_horizon_s, k.pc_max_gap_s) << bed;
        EXPECT_EQ(r.spec(K::RxCodeBias).gap_horizon_s, kRxCbMaxGapS) << bed;
        for (const K kind : {K::SatClock, K::SatPhaseBias, K::SatCodeBias, K::VtecSh})
            EXPECT_TRUE(std::isnan(r.spec(kind).gap_horizon_s)) << bed << " " << paramKindName(kind);
        // The in-block slant-ionosphere carry is not armed on the goldens: no horizon.
        EXPECT_TRUE(std::isnan(r.spec(K::SlantIono).gap_horizon_s)) << bed;
        // The in-block route declares its own horizon (its prune reads it).
        NetworkKnobs kb = k;
        kb.iono_blk = true;
        EXPECT_EQ(NetworkParamRegistry::build(kb, cfg).spec(K::SlantIono).gap_horizon_s,
                  k.iono_blk_max_gap_s) << bed;
    }
}
