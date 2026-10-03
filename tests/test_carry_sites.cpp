/**
 * @file test_carry_sites.cpp
 * @brief Card PRM phase 2: the carried references of the border (filter/NetworkCarrySites),
 *        the one traversal over them, and each re-base mechanism's walk through it.
 *
 * The section 5 test: a carried state with every site populated for one satellite, a shift of
 * 1 m applied through the traversal, and every site moved by 1 m; a scan of the carried
 * state's declaration that fails when a new local carry map (or a new holder of regression
 * terms) is added without a site.  Then each mechanism's walk through the traversal against
 * its rule written out (step 1 checked each against its hand-written walk; step 2 removed
 * the hand walks).  The byte-identity A/B of the phase (scripts/prm_ab.py) is the
 * end-to-end gate.  Phase 3b step 2 (network.carry_rebase_complete): the owed exemptions,
 * each of the four walks with its owed sites moved by its own rule (the phase hold's held
 * value excepted) and without them equal to the phase 2 walk, and the knob's loader rules.
 */
#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAmbFloat.hpp"
#include "filter/NetworkCarrySites.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkClockLevel.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkRelevel.hpp"
#include "logging/LogManager.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

CarryPrior prior(SharedParamKind kind, const std::string& sat, const std::string& sig,
                 double value, double var = 1e-4) {
    CarryPrior cp;
    cp.kind = kind; cp.sat = sat; cp.signal = sig; cp.value = value; cp.var = var;
    return cp;
}

CoupledShared term(SharedParamKind kind, const std::string& sat, const std::string& sig,
                   double coeff, double s_prev, double p_prev) {
    CoupledShared cs;
    cs.kind = kind; cs.sat = sat; cs.signal = sig;
    cs.coeff = coeff; cs.s_prev = s_prev; cs.p_prev = p_prev;
    return cs;
}

LocalCarry local(double value, std::vector<CoupledShared> coupling) {
    LocalCarry lc{value, 1e-4, 100.0};
    lc.coupling = std::move(coupling);
    return lc;
}

/// One reference in every site, each with its own value, for the satellite G01 (signal
/// L1C, station 0); the receiver-side local values on their own keys.  The carried border
/// value and every coupling term are of @p kind (a clock, or the phase bias of G01 L1C).
void populateEverySite(NetworkCarryState& c, SharedParamKind kind = SharedParamKind::Clock) {
    double v = 1.0;
    const auto next = [&v]() { v += 1.0; return v; };
    const std::string sig = (kind == SharedParamKind::Clock) ? "" : "L1C";
    c.prev_carry = {prior(kind, "G01", sig, next())};
    c.cxu_pivot['G'] = {"G01", next()};
    c.clock_osc["G01"].x0 = next();
    c.clock_osc["G01"].init = true;
    c.prev_clk["G01"] = {next(), 100.0};
    c.cb_exc_prev_clk["G01"] = next();
    c.pb_hold_prev_clock_m["G01"] = next();
    c.pb_level_prev["G01_L1C"] = next();
    c.pin_bridge["G01_L1C"].value = next();
    c.pb_hold["G01|L1C"].held_m = next();
    c.code_bias_prev_["G|C1C"]["G01"] = next();
    const auto t = [&]() { return std::vector<CoupledShared>{
        term(kind, "G01", sig, 1.0, next(), 1e-5)}; };
    c.local_carry[{0, "G01", "L1C"}] = local(next(), t());
    c.zwd_carry[0] = local(next(), t());
    c.iono_carry[{0, "G01"}] = local(next(), t());
    c.iono_blk_carry[{0, "G01"}] = local(next(), t());
    c.gl_hold_amb[{0, "G01", "L1C"}] = local(next(), t());
    c.gl_hold_iono[{0, "G01"}] = local(next(), t());
    c.gl_hold_iono_blk[{0, "G01"}] = local(next(), t());
    c.rx_clk_carry[{0, ""}] = local(next(), t());
    c.rpb_carry[{0, "GPS_RPB_L1C"}] = local(next(), t());
    c.rx_cb_carry[{0, "GPS_DCB"}] = local(next(), t());
    c.pc_carry[{0, "G01"}] = local(next(), t());
}

/// Every referenced double (and its variance) in the traversal's order: the bitwise
/// comparison of two carried states.
std::vector<double> referencedDoubles(NetworkCarryState& c) {
    std::vector<double> out;
    forEachCarriedReference(c, kAllCarrySites, [&](const CarriedRef& r) {
        out.push_back(*r.value);
        if (r.var) out.push_back(*r.var);
        return false;
    });
    return out;
}

bool bitwiseEqual(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The body of `struct <name> {` in a header, up to the matching close brace.
std::string structBody(const std::string& text, const std::string& name) {
    const std::size_t at = text.find("struct " + name + " {");
    if (at == std::string::npos) return {};
    std::size_t i = text.find('{', at);
    int depth = 0;
    const std::size_t from = i;
    for (; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) break;
    }
    return text.substr(from, i - from);
}

}  // namespace

// The table is in the order of the enumeration, every site has a declaration, and the names
// are unique.
TEST(CarrySites, TableIsInOrderAndComplete) {
    std::set<std::string> names;
    for (int i = 0; i < kCarrySites; ++i) {
        const CarrySiteSpec& sp = carrySiteSpec(static_cast<CarrySite>(i));
        EXPECT_EQ(static_cast<int>(sp.site), i);
        ASSERT_NE(sp.name, nullptr);
        ASSERT_NE(sp.container, nullptr);
        ASSERT_NE(sp.key, nullptr);
        ASSERT_NE(sp.holds, nullptr);
        ASSERT_NE(sp.shift, nullptr);
        ASSERT_NE(sp.snapshot_note, nullptr);
        EXPECT_TRUE(names.insert(sp.name).second) << sp.name;
    }
    EXPECT_EQ(kAllCarrySites, (std::uint32_t{1} << kCarrySites) - 1u);
}

// Card PRM section 5: every site populated for one satellite, a shift of 1 m applied through
// the traversal, and every site moved by 1 m (one reference each, each visited once).
TEST(CarrySites, EverySiteMovesByOneMetre) {
    NetworkCarryState c;
    populateEverySite(c);
    const std::vector<double> before = referencedDoubles(c);
    CarrySiteCounts counts;
    const long long n = forEachCarriedReference(c, kAllCarrySites, [](const CarriedRef& r) {
        *r.value += 1.0;
        return true;
    }, &counts);
    EXPECT_EQ(n, kCarrySites);
    for (int i = 0; i < kCarrySites; ++i) {
        EXPECT_EQ(counts.visited[i], 1) << carrySiteSpec(static_cast<CarrySite>(i)).name;
        EXPECT_EQ(counts.moved[i], 1) << carrySiteSpec(static_cast<CarrySite>(i)).name;
    }
    // Each reference visited once, in the traversal's order: compare value by value.
    const std::vector<double> after = referencedDoubles(c);
    ASSERT_EQ(after.size(), before.size());
    std::size_t k = 0;
    forEachCarriedReference(c, kAllCarrySites, [&](const CarriedRef& r) {
        EXPECT_EQ(after[k], before[k] + 1.0) << carrySiteSpec(r.site).name;
        ++k;
        if (r.var) {
            EXPECT_EQ(after[k], before[k]) << carrySiteSpec(r.site).name << " variance";
            ++k;
        }
        return false;
    });
    // The walk itself writes nothing: a second counting walk sees the same doubles.
    EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), after));
}

// The walk reads only the sites it is given, and a site mask selects exactly them.
TEST(CarrySites, MaskSelectsSites) {
    NetworkCarryState c;
    populateEverySite(c);
    for (int i = 0; i < kCarrySites; ++i) {
        CarrySiteCounts counts;
        forEachCarriedReference(c, carrySiteBit(static_cast<CarrySite>(i)),
                                [](const CarriedRef&) { return false; }, &counts);
        for (int j = 0; j < kCarrySites; ++j)
            EXPECT_EQ(counts.visited[j], j == i ? 1 : 0)
                << carrySiteSpec(static_cast<CarrySite>(i)).name;
    }
    long long held[kCarrySites]{};
    carrySiteHeld(c, held);
    for (int i = 0; i < kCarrySites; ++i) EXPECT_EQ(held[i], 1);
}

// The scan of section 5: every LocalCarry map of the carried state is a site (a map added
// without a table row fails here), and the only holder of regression terms is LocalCarry.
TEST(CarrySites, EveryLocalCarryMapIsASite) {
    const std::filesystem::path root(GNSS_PROJECT_ROOT);
    const std::string ctx_h  = readFile(root / "include" / "app" / "NetworkRunContext.hpp");
    const std::string types_h = readFile(root / "include" / "filter" / "NetworkRunTypes.hpp");
    ASSERT_FALSE(ctx_h.empty());
    ASSERT_FALSE(types_h.empty());
    const std::string body = structBody(ctx_h, "NetworkCarryState");
    ASSERT_FALSE(body.empty());

    std::set<std::string> containers;
    for (int i = 0; i < kCarrySites; ++i)
        containers.insert(carrySiteSpec(static_cast<CarrySite>(i)).container);
    const std::regex decl(R"(LocalCarry>\s+(\w+)\s*;)");
    int maps = 0;
    for (std::sregex_iterator it(body.begin(), body.end(), decl), end; it != end; ++it) {
        ++maps;
        EXPECT_TRUE(containers.count((*it)[1].str()))
            << "the local carry map " << (*it)[1].str() << " has no site in NetworkCarrySites";
    }
    EXPECT_EQ(maps, 11) << "the eleven local carry maps of 2026-10-02 (card PRM section 16)";
    // A carried container of LocalCarry by any other shape (a vector, a pointer) is a site
    // the scan above cannot see.
    EXPECT_EQ(body.find("std::vector<LocalCarry>"), std::string::npos);
    // Regression terms on the border: held only by LocalCarry::coupling in the run types, and
    // by no container of the carried state directly.
    EXPECT_EQ(body.find("CoupledShared>"), std::string::npos);
    const std::regex holders(R"(std::vector<CoupledShared>\s+(\w+))");
    std::vector<std::string> fields;
    for (std::sregex_iterator it(types_h.begin(), types_h.end(), holders), end; it != end; ++it)
        fields.push_back((*it)[1].str());
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0], "coupling");
    EXPECT_NE(structBody(types_h, "LocalCarry").find("std::vector<CoupledShared> coupling"),
              std::string::npos);
}

// The declared moves and exemptions of each mechanism: the gaps of the hand lists, named.
TEST(CarrySites, MechanismTableStatesTheHandListsAndTheirGaps) {
    using S = CarrySite;
    using M = CarryMechanism;
    const auto moves = [](M m, S s) { return carryMechanismSite(m, s).moves; };
    const auto exempt = [](M m, S s) { return carryMechanismSite(m, s).reason != nullptr; };
    const S couplings[] = {S::AmbiguityCoupling, S::ZwdCoupling, S::IonoCoupling,
                           S::IonoBlockCoupling, S::HoldAmbiguityCoupling, S::HoldIonoCoupling,
                           S::HoldIonoBlockCoupling, S::RxClockCoupling,
                           S::RxPhaseBiasCoupling, S::RxCodeBiasCoupling};
    // Card CLV: the ten local carries of its list, not card LGM's later one.
    for (S s : couplings) EXPECT_TRUE(moves(M::ClockLevel, s));
    EXPECT_FALSE(moves(M::ClockLevel, S::PhaseColourCoupling));
    EXPECT_TRUE(exempt(M::ClockLevel, S::PhaseColourCoupling));
    EXPECT_TRUE(moves(M::ClockLevel, S::PinBridge));
    EXPECT_TRUE(exempt(M::ClockLevel, S::PbHold));
    // The ephemeris re-level: two of the local carries (card CLV section 6.4 item 6).
    EXPECT_TRUE(moves(M::EphRelevel, S::AmbiguityCoupling));
    EXPECT_TRUE(moves(M::EphRelevel, S::ZwdCoupling));
    for (S s : couplings)
        if (s != S::AmbiguityCoupling && s != S::ZwdCoupling) EXPECT_TRUE(exempt(M::EphRelevel, s));
    EXPECT_TRUE(exempt(M::EphRelevel, S::CbExcPrevClk));
    EXPECT_TRUE(exempt(M::EphRelevel, S::PbHoldPrevClock));
    EXPECT_TRUE(moves(M::EphRelevel, S::ClockOsc));
    // The handover after the rows: the carried clocks only.  Before the rows: every clock
    // reference the rows read.
    EXPECT_EQ(carryMechanismSites(M::Handover), carrySiteBit(S::PrevCarry));
    for (S s : couplings) {
        EXPECT_TRUE(exempt(M::Handover, s));
        EXPECT_TRUE(moves(M::HandoverBeforeRows, s));
    }
    EXPECT_TRUE(moves(M::HandoverBeforeRows, S::PhaseColourCoupling));
    EXPECT_TRUE(moves(M::HandoverBeforeRows, S::CxuPivot));
    for (M m : {M::Handover, M::HandoverBeforeRows})
        for (S s : {S::ClockOsc, S::PrevClk, S::CbExcPrevClk, S::PbHoldPrevClock, S::RxClockValue})
            EXPECT_TRUE(exempt(m, s));
    // The phase-bias datum shifts.
    for (S s : {S::PrevCarry, S::AmbiguityValue, S::RxPhaseBiasValue, S::AmbiguityCoupling,
                S::RxPhaseBiasCoupling, S::ZwdCoupling})
        EXPECT_TRUE(moves(M::Eq34Shift, s));
    EXPECT_EQ(carryMechanismSites(M::DeclaredHandover),
              carrySiteBit(S::PrevCarry) | carrySiteBit(S::AmbiguityValue)
                  | carrySiteBit(S::AmbiguityCoupling));
    // A site is never both moved and exempted, and every exemption has a reason.
    for (int m = 0; m < kCarryMechanisms; ++m)
        for (int i = 0; i < kCarrySites; ++i) {
            const CarryMechanismSite ms =
                carryMechanismSite(static_cast<M>(m), static_cast<S>(i));
            if (ms.moves) EXPECT_EQ(ms.reason, nullptr);
            if (ms.reason) EXPECT_GT(std::strlen(ms.reason), 10u);
        }
}

// Card CLV's re-base through the traversal: every site of its row moves by the satellite's
// level shift, every other site stays, and the transfer and declaration follow (the rule
// written out over a state with every site populated; step 1 checked the walk against the
// hand-written one bit for bit, card PRM section 16).
TEST(CarrySites, ClockLevelReBaseMovesItsSitesAndNothingElse) {
    NetworkCarryState c, e;   // not copyable: built twice
    populateEverySite(c);
    populateEverySite(e);
    CarrySiteCounts counts;
    const long long n = clockLevelShiftCarry(c, {{"G01", 1.0}}, nullptr, nullptr, nullptr,
                                             nullptr, &counts);
    // Expected: the sites of the row hold G01 clock or phase-bias references (moved by 1);
    // the code-bias datum reference moves only with the follow or a direct step; the hold's
    // held value is read for its signal and not moved.
    long long n_expect = 0;
    std::vector<double> want;
    forEachCarriedReference(e, kAllCarrySites, [&](const CarriedRef& r) {
        const bool moves = carryMechanismSite(CarryMechanism::ClockLevel, r.site).moves
                        && r.site != CarrySite::CodeBiasPrev
                        && (r.kind == SharedParamKind::Clock || r.kind == SharedParamKind::PhaseBias);
        want.push_back(*r.value + (moves ? 1.0 : 0.0));
        if (r.var) want.push_back(*r.var);
        if (moves) ++n_expect;
        return false;
    });
    EXPECT_EQ(n, n_expect);
    EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), want));
    EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::PhaseColourCoupling)], 0);
    EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::CxuPivot)], 0);
    EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::PbHold)], 0);
    EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::PbHold)], 1);
    // The held signal's emit-side transfer and the declared step.
    EXPECT_EQ(c.clk_level.sat.at("G01").xfer.at("L1C"), 1.0);
    EXPECT_EQ(c.clk_level.sat.at("G01").pending_step, 1.0);
    EXPECT_EQ(clockLevelShiftCarry(c, {}), 0);
}

// The ephemeris re-level through the traversal: the rule written out (the oscillator's offset
// when initialized, every carried clock entry of the satellite, parked included, the jump
// baseline, and the clock terms of the ambiguity and wet-delay carries under the coupling).
TEST(CarrySites, EphRelevelThroughSitesFollowsTheRule) {
    for (const bool cpl : {true, false}) {
        const auto build = [](NetworkCarryState& c) {
            populateEverySite(c);
            c.prev_carry.push_back(prior(SharedParamKind::Clock, "G01", "", 0.4));
            c.prev_carry.back().t_park_s = 1000.0;              // parked: moves too
            c.prev_carry.push_back(prior(SharedParamKind::Clock, "G02", "", -0.9));
            c.clock_osc["G02"].x0 = 0.5;                        // not initialized: stays
        };
        NetworkCarryState c, e;   // not copyable: built twice
        build(c);
        build(e);
        const double d = 0.25;
        e.clock_osc["G01"].x0 -= d;
        for (CarryPrior& cp : e.prev_carry)
            if (cp.kind == SharedParamKind::Clock && cp.sat == "G01") cp.value -= d;
        e.prev_clk["G01"].first -= d;
        long long n_expect = 4;   // the oscillator, two carried entries, the baseline
        if (cpl) {
            e.local_carry[std::make_tuple(0, std::string("G01"), std::string("L1C"))]
                .coupling[0].s_prev -= d;
            e.zwd_carry[0].coupling[0].s_prev -= d;
            n_expect += 2;
        }
        CarrySiteCounts counts;
        EXPECT_EQ(ephCutoverShiftCarry(c, "G01", d, cpl, &counts), n_expect);
        EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::ClockOsc)], 1);
        EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), referencedDoubles(e))) << "cpl=" << cpl;
        EXPECT_EQ(c.clock_osc["G02"].x0, 0.5);
    }
}

// The clock-pivot handover's S-transform through the traversal: after PASS B the carried
// clocks of the registered set (the kernel's rule, written out; step 1 checked it against the
// vector kernel bit for bit, card PRM section 16); before the rows it also re-expresses every
// clock reference of the constellation the rows read.
TEST(CarrySites, HandoverThroughSitesFollowsTheRule) {
    const std::vector<std::pair<std::string, double>> v_c{{"G01", 1.0}, {"G02", 1.0}, {"G03", 1.0}};
    NetworkCarryState b;
    b.prev_carry = {
        prior(SharedParamKind::Clock, "G01", "", 1.2, 2e-4),
        prior(SharedParamKind::Clock, "G02", "", -0.5, 3e-4),
        prior(SharedParamKind::PhaseBias, "G02", "L1C", 0.3, 1e-4),
        prior(SharedParamKind::Clock, "G03", "", 0.7, 4e-4),
        prior(SharedParamKind::Clock, "G05", "", 0.9, 5e-4),   // not registered (parked)
        prior(SharedParamKind::Clock, "E11", "", 0.1, 1e-4),
    };
    b.prev_carry[4].t_park_s = 500.0;
    b.local_carry[std::make_tuple(0, std::string("G01"), std::string("L1C"))] = local(3.0, {
        term(SharedParamKind::Clock, "G01", "", 0.5, 1.2, 2e-4)});
    CarrySiteCounts counts;
    const ClockStransformResult rb =
        applyClockStransform(b, v_c, "G02", CarryMechanism::Handover, &counts);
    ASSERT_TRUE(rb.applied);
    const double c0 = -0.5 / 1.0, vq = 3e-4;
    EXPECT_EQ(rb.c0, c0);
    EXPECT_EQ(rb.var_q, vq);
    EXPECT_EQ(rb.rows, 3);
    EXPECT_EQ(rb.x_q_post, 0.0);
    EXPECT_TRUE(rb.selfcheck_ok);
    EXPECT_EQ(b.prev_carry[0].value, 1.2 - 1.0 * c0);
    EXPECT_EQ(b.prev_carry[0].var, 2e-4 + 1.0 * 1.0 * vq);
    EXPECT_EQ(b.prev_carry[1].value, -0.5 - 1.0 * c0);
    EXPECT_EQ(b.prev_carry[1].var, 3e-4 + 1.0 * 1.0 * vq);
    EXPECT_EQ(b.prev_carry[2].value, 0.3);
    EXPECT_EQ(b.prev_carry[3].value, 0.7 - 1.0 * c0);
    EXPECT_EQ(b.prev_carry[4].value, 0.9) << "after the rows, an unregistered clock stays";
    EXPECT_EQ(b.prev_carry[5].value, 0.1);
    EXPECT_EQ(b.local_carry[std::make_tuple(0, std::string("G01"), std::string("L1C"))]
                  .coupling[0].s_prev, 1.2) << "after the rows, the terms are re-harvested";
    EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::PrevCarry)], 3);
    EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::AmbiguityCoupling)], 0);

    // Before the rows: the unregistered clock of the constellation, the coupling terms on its
    // clocks (s_prev; p_prev of a free term), and the stash's pivot move; E11 does not.
    NetworkCarryState w;
    w.prev_carry = {prior(SharedParamKind::Clock, "G01", "", 1.2, 2e-4),
                    prior(SharedParamKind::Clock, "G02", "", -0.5, 3e-4),
                    prior(SharedParamKind::Clock, "G05", "", 0.9, 5e-4),
                    prior(SharedParamKind::Clock, "E11", "", 0.1, 1e-4)};
    w.prev_carry[2].t_park_s = 500.0;
    w.cxu_pivot['G'] = {"G04", 0.0};
    w.local_carry[std::make_tuple(0, std::string("G01"), std::string("L1C"))] = local(3.0, {
        term(SharedParamKind::Clock, "G01", "", 0.5, 1.2, 2e-4),
        term(SharedParamKind::Clock, "G04", "", -0.5, 0.0, 0.0),     // the pinned pivot
        term(SharedParamKind::Clock, "E11", "", 0.2, 0.1, 1e-4),
        term(SharedParamKind::PhaseBias, "G01", "L1C", 1.0, 0.3, 1e-4)});
    const ClockStransformResult rw =
        applyClockStransform(w, v_c, "G02", CarryMechanism::HandoverBeforeRows);
    ASSERT_TRUE(rw.applied);
    EXPECT_EQ(rw.c0, c0);
    EXPECT_EQ(rw.rows, 3);
    EXPECT_EQ(w.prev_carry[0].value, 1.2 - c0);
    EXPECT_EQ(w.prev_carry[0].var, 2e-4 + vq);
    EXPECT_EQ(w.prev_carry[1].value, 0.0);
    EXPECT_EQ(w.prev_carry[2].value, 0.9 - c0);
    EXPECT_EQ(w.prev_carry[2].var, 5e-4 + vq);
    EXPECT_EQ(w.prev_carry[3].value, 0.1);
    EXPECT_EQ(w.cxu_pivot['G'].second, -c0);
    const auto& cpl =
        w.local_carry[std::make_tuple(0, std::string("G01"), std::string("L1C"))].coupling;
    EXPECT_EQ(cpl[0].s_prev, 1.2 - c0);
    EXPECT_EQ(cpl[0].p_prev, 2e-4 + vq);
    EXPECT_EQ(cpl[1].s_prev, -c0);
    EXPECT_EQ(cpl[1].p_prev, 0.0) << "a pinned term keeps its pinned route";
    EXPECT_EQ(cpl[2].s_prev, 0.1);
    EXPECT_EQ(cpl[3].s_prev, 0.3);
}

// The eq34 D4 shift through the traversal: the rule written out.
TEST(CarrySites, Eq34ShiftThroughSitesFollowsTheRule) {
    NetworkCarryState c;
    c.prev_carry = {prior(SharedParamKind::PhaseBias, "G01", "L1C", 0.4),
                    prior(SharedParamKind::PhaseBias, "G02", "L1C", -0.2),
                    prior(SharedParamKind::Clock, "G01", "", 1.0)};
    const auto pbTerm = [](const std::string& sat, double s) {
        return term(SharedParamKind::PhaseBias, sat, "L1C", 1.0, s, 1e-4);
    };
    const auto amb = [](int sta) {
        return std::make_tuple(sta, std::string("G01"), std::string("L1C"));
    };
    c.local_carry[amb(0)] = local(5.0, {pbTerm("G01", 0.4), pbTerm("G02", -0.2)});
    c.local_carry[amb(1)] = local(7.0, {pbTerm("G01", 0.4)});
    c.rpb_carry[{0, "G_RPB_L1C"}] = local(0.6, {});
    c.zwd_carry[0] = local(0.1, {pbTerm("G01", 0.4)});
    c.iono_blk_carry[{0, "G01"}] = local(9.0, {pbTerm("G01", 0.4)});   // exempt: stays
    const std::map<std::pair<std::string, std::string>, double> shift_sat{{{"G01", "L1C"}, 0.3}};
    const std::map<std::tuple<int, char, std::string>, double> shift_rxg{
        {std::make_tuple(0, 'G', std::string("L1C")), 0.1}};
    const std::map<std::pair<int, std::string>, double> shift_rpb{{{0, "G_RPB_L1C"}, 0.1}};
    long long nv = 0, nt = 0;
    eq34ShiftCarry(c, shift_sat, shift_rxg, shift_rpb, nv, nt);
    EXPECT_EQ(nv, 4);   // two ambiguities, the receiver phase bias, the G01 phase bias
    EXPECT_EQ(nt, 3);   // the G01 terms of the two ambiguities and of the wet delay
    EXPECT_EQ(c.local_carry[amb(0)].value_abs, 5.0 + ((0.0 - 0.3) - 0.1));
    EXPECT_EQ(c.local_carry[amb(1)].value_abs, 7.0 + (0.0 - 0.3));
    const std::pair<int, std::string> rpb_key{0, "G_RPB_L1C"};
    EXPECT_EQ(c.rpb_carry[rpb_key].value_abs, 0.6 + 0.1);
    EXPECT_EQ(c.prev_carry[0].value, 0.4 + 0.3);
    EXPECT_EQ(c.prev_carry[1].value, -0.2);
    EXPECT_EQ(c.prev_carry[2].value, 1.0);
    EXPECT_EQ(c.local_carry[amb(0)].coupling[0].s_prev, 0.4 + 0.3);
    EXPECT_EQ(c.local_carry[amb(0)].coupling[1].s_prev, -0.2);
    EXPECT_EQ(c.zwd_carry[0].coupling[0].s_prev, 0.4 + 0.3);
    const std::pair<int, std::string> iono_key{0, "G01"};
    EXPECT_EQ(c.iono_blk_carry[iono_key].coupling[0].s_prev, 0.4);
    // No satellite or receiver step: nothing moves.
    eq34ShiftCarry(c, {}, {}, shift_rpb, nv, nt);
    EXPECT_EQ(nv + nt, 0);
}

// The declared anchor handover through the traversal: the rule written out.
TEST(CarrySites, DeclaredHandoverThroughSitesFollowsTheRule) {
    NetworkCarryState c;
    const auto pbTerm = [](const std::string& sig, double s) {
        return term(SharedParamKind::PhaseBias, "G23", sig, 1.0, s, 1e-4);
    };
    const auto amb = [](int sta, const char* sig) {
        return std::make_tuple(sta, std::string("G23"), std::string(sig));
    };
    c.prev_carry = {prior(SharedParamKind::PhaseBias, "G23", "L2W", 0.4),
                    prior(SharedParamKind::PhaseBias, "G23", "L1C", -0.2)};
    c.local_carry[amb(0, "L2W")] = local(2.5, {pbTerm("L2W", 0.4)});
    c.local_carry[amb(1, "L2W")] = local(4.0, {pbTerm("L1C", -0.2)});
    c.local_carry[amb(1, "L1C")] = local(6.0, {});
    c.zwd_carry[0] = local(0.1, {pbTerm("L2W", 0.4)});   // exempt: stays
    const double delta = 2.5;
    long long a = 0, b = 0, t = 0;
    declaredHandoverShiftCarry(c, "G23", "L2W", delta, a, b, t);
    EXPECT_EQ(a, 2);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(t, 1);
    EXPECT_EQ(c.local_carry[amb(0, "L2W")].value_abs, 0.0);
    EXPECT_EQ(c.local_carry[amb(1, "L2W")].value_abs, 4.0 - delta);
    EXPECT_EQ(c.local_carry[amb(1, "L1C")].value_abs, 6.0);
    EXPECT_EQ(c.prev_carry[0].value, 0.4 + delta);
    EXPECT_EQ(c.prev_carry[1].value, -0.2);
    EXPECT_EQ(c.local_carry[amb(0, "L2W")].coupling[0].s_prev, 0.4 + delta);
    EXPECT_EQ(c.local_carry[amb(1, "L2W")].coupling[0].s_prev, -0.2);
    EXPECT_EQ(c.zwd_carry[0].coupling[0].s_prev, 0.4);
}

// ===========================================================================
// Card PRM phase 3b step 2: network.carry_rebase_complete
// ===========================================================================

namespace {

/// The expected doubles of a walk over a state built like the walked one: every reference in
/// the traversal's order, moved by @p delta (0 = not moved), with its variance unchanged.
/// @p n_moved counts the references the rule moves.
template <class Rule>
std::vector<double> expectedDoubles(NetworkCarryState& e, Rule delta, long long& n_moved) {
    std::vector<double> want;
    n_moved = 0;
    forEachCarriedReference(e, kAllCarrySites, [&](const CarriedRef& r) {
        const double d = delta(r);
        want.push_back(*r.value + d);
        if (r.var) want.push_back(*r.var);
        if (d != 0.0) ++n_moved;
        return false;
    });
    return want;
}

bool inMask(std::uint32_t mask, CarrySite s) { return (mask & carrySiteBit(s)) != 0; }

long long movedTotal(const CarrySiteCounts& c) {
    long long n = 0;
    for (int i = 0; i < kCarrySites; ++i) n += c.moved[i];
    return n;
}

/// Each owed site of @p m moved once with the knob and never without it (the populated state
/// holds one reference per site, each in the rule's reach unless @p unreached says otherwise).
template <class Unreached>
void expectOwedMoved(CarryMechanism m, const CarrySiteCounts& counts, bool complete,
                     Unreached unreached) {
    const std::uint32_t owed = carryMechanismOwedSites(m);
    for (int i = 0; i < kCarrySites; ++i) {
        const auto s = static_cast<CarrySite>(i);
        if (!(owed & carrySiteBit(s))) continue;
        EXPECT_EQ(counts.moved[i], (complete && !unreached(s)) ? 1 : 0)
            << carryMechanismName(m) << " " << carrySiteSpec(s).name << " complete=" << complete;
    }
}

}  // namespace

// The owed exemptions are the gaps card PRM sections 16.2 and 16.7 name: disjoint from the
// moved sites, never the phase hold's held value, none for the two handover transforms, and the
// by-design exemptions stay exempt with the knob.
TEST(CarrySites, OwedSitesAreTheHandListsGaps) {
    using S = CarrySite;
    using M = CarryMechanism;
    const auto mask = [](std::initializer_list<S> sites) {
        std::uint32_t bits = 0;
        for (S s : sites) bits |= carrySiteBit(s);
        return bits;
    };
    EXPECT_EQ(carryMechanismOwedSites(M::ClockLevel),
              mask({S::CxuPivot, S::PhaseColourCoupling}));
    EXPECT_EQ(carryMechanismOwedSites(M::EphRelevel),
              mask({S::CxuPivot, S::CbExcPrevClk, S::PbHoldPrevClock, S::IonoCoupling,
                    S::IonoBlockCoupling, S::HoldAmbiguityCoupling, S::HoldIonoCoupling,
                    S::HoldIonoBlockCoupling, S::RxClockCoupling, S::RxPhaseBiasCoupling,
                    S::RxCodeBiasCoupling, S::PhaseColourCoupling}));
    EXPECT_EQ(carryMechanismOwedSites(M::Handover), 0u);
    EXPECT_EQ(carryMechanismOwedSites(M::HandoverBeforeRows), 0u);
    EXPECT_EQ(carryMechanismOwedSites(M::Eq34Shift),
              mask({S::PbLevelPrev, S::HoldAmbiguityValue, S::IonoCoupling, S::IonoBlockCoupling,
                    S::HoldAmbiguityCoupling, S::HoldIonoCoupling, S::HoldIonoBlockCoupling,
                    S::RxClockCoupling, S::RxCodeBiasCoupling, S::PhaseColourCoupling}));
    EXPECT_EQ(carryMechanismOwedSites(M::DeclaredHandover),
              mask({S::PbLevelPrev, S::PinBridge, S::HoldAmbiguityValue, S::ZwdCoupling,
                    S::IonoCoupling, S::IonoBlockCoupling, S::HoldAmbiguityCoupling,
                    S::HoldIonoCoupling, S::HoldIonoBlockCoupling, S::RxClockCoupling,
                    S::RxPhaseBiasCoupling, S::RxCodeBiasCoupling, S::PhaseColourCoupling}));
    for (int m = 0; m < kCarryMechanisms; ++m) {
        const auto mech = static_cast<M>(m);
        const std::uint32_t owed = carryMechanismOwedSites(mech);
        EXPECT_EQ(owed & carryMechanismSites(mech), 0u) << carryMechanismName(mech);
        EXPECT_EQ(owed & carrySiteBit(S::PbHold), 0u) << carryMechanismName(mech);
        EXPECT_EQ(carryMechanismSiteMask(mech, false), carryMechanismSites(mech));
        EXPECT_EQ(carryMechanismSiteMask(mech, true), carryMechanismSites(mech) | owed);
        for (int i = 0; i < kCarrySites; ++i) {
            const auto s = static_cast<S>(i);
            const CarryMechanismSite ms = carryMechanismSite(mech, s);
            if (owed & carrySiteBit(s)) EXPECT_NE(ms.reason, nullptr) << carrySiteSpec(s).name;
            EXPECT_EQ(carryMechanismMoves(mech, s, false), ms.moves);
            EXPECT_EQ(carryMechanismMoves(mech, s, true),
                      ms.moves || (owed & carrySiteBit(s)) != 0);
        }
    }
    // The phase hold's held value stays exempt, with its reason, wherever a direction reaches
    // it; the two phase-bias shifts state that the knob leaves it.
    for (M m : {M::ClockLevel, M::Eq34Shift, M::DeclaredHandover})
        EXPECT_NE(carryMechanismSite(m, S::PbHold).reason, nullptr) << carryMechanismName(m);
    for (M m : {M::Eq34Shift, M::DeclaredHandover})
        EXPECT_NE(std::string(carryMechanismSite(m, S::PbHold).reason).find("carry_rebase_complete"),
                  std::string::npos) << carryMechanismName(m);
    // By design, with the knob too: the oscillator under card CLV, the D4 shift's bridged root,
    // and every handover exemption (the receiver clock's included).
    EXPECT_FALSE(carryMechanismMoves(M::ClockLevel, S::ClockOsc, true));
    EXPECT_FALSE(carryMechanismMoves(M::Eq34Shift, S::PinBridge, true));
    for (M m : {M::Handover, M::HandoverBeforeRows})
        for (S s : {S::ClockOsc, S::PrevClk, S::CbExcPrevClk, S::PbHoldPrevClock, S::RxClockValue})
            EXPECT_FALSE(carryMechanismMoves(m, s, true)) << carryMechanismName(m);
}

// Card CLV's re-base with the owed sites: the stash's pinned pivot value and card LGM's
// phase-colour terms move by the satellite's level shift like the rest of its row; the hold's
// held value is read for the transfer and not moved; without the knob the walk is phase 2's.
TEST(CarrySites, ClockLevelReBaseCompleteMovesTheOwedSites) {
    for (const bool complete : {false, true}) {
        NetworkCarryState c, e;   // not copyable: built twice
        populateEverySite(c);
        populateEverySite(e);
        CarrySiteCounts counts;
        const long long n = clockLevelShiftCarry(c, {{"G01", 1.0}}, nullptr, nullptr, nullptr,
                                                 nullptr, &counts, complete);
        const std::uint32_t mask = carryMechanismSiteMask(CarryMechanism::ClockLevel, complete);
        long long n_expect = 0;
        const std::vector<double> want = expectedDoubles(e, [&](const CarriedRef& r) {
            const bool moves = inMask(mask, r.site) && r.site != CarrySite::CodeBiasPrev
                && (r.kind == SharedParamKind::Clock || r.kind == SharedParamKind::PhaseBias);
            return moves ? 1.0 : 0.0;
        }, n_expect);
        EXPECT_EQ(n, n_expect) << "complete=" << complete;
        EXPECT_EQ(movedTotal(counts), n_expect) << "complete=" << complete;
        EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), want)) << "complete=" << complete;
        expectOwedMoved(CarryMechanism::ClockLevel, counts, complete,
                        [](CarrySite) { return false; });
        EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::PbHold)], 1);
        EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::PbHold)], 0);
        EXPECT_EQ(c.pb_hold.at("G01|L1C").held_m, e.pb_hold.at("G01|L1C").held_m);
        EXPECT_EQ(counts.moved[static_cast<int>(CarrySite::ClockOsc)], 0);
        EXPECT_EQ(c.clk_level.sat.at("G01").xfer.at("L1C"), 1.0);   // the transfer, either way
    }
    // Off is the default: no knob argument is complete = false.
    NetworkCarryState a, b;
    populateEverySite(a);
    populateEverySite(b);
    clockLevelShiftCarry(a, {{"G01", 0.5}});
    clockLevelShiftCarry(b, {{"G01", 0.5}}, nullptr, nullptr, nullptr, nullptr, nullptr, false);
    EXPECT_TRUE(bitwiseEqual(referencedDoubles(a), referencedDoubles(b)));
}

// The ephemeris re-level with the owed sites (card CLV section 6.4 item 6): the stash's pivot
// value and the excursion and hold clock baselines move by -d, and every other coupling map's
// clock terms move under the conditional coupling as the ambiguity's and wet delay's do;
// without the knob the walk is phase 2's, and no knob argument is complete = false.
TEST(CarrySites, EphRelevelCompleteMovesTheOwedSites) {
    for (const bool complete : {false, true})
        for (const bool cpl : {true, false}) {
            NetworkCarryState c, e;   // not copyable: built twice
            populateEverySite(c);
            populateEverySite(e);
            const double d = 0.25;
            CarrySiteCounts counts;
            const long long n = ephCutoverShiftCarry(c, "G01", d, cpl, &counts, complete);
            const std::uint32_t mask =
                carryMechanismSiteMask(CarryMechanism::EphRelevel, complete);
            long long n_expect = 0;
            const std::vector<double> want = expectedDoubles(e, [&](const CarriedRef& r) {
                if (!inMask(mask, r.site) || r.kind != SharedParamKind::Clock || r.sat != "G01")
                    return 0.0;
                if (carrySiteSpec(r.site).role == CarryRefRole::Regression && !cpl) return 0.0;
                return -d;
            }, n_expect);
            EXPECT_EQ(n, n_expect) << "complete=" << complete << " cpl=" << cpl;
            EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), want))
                << "complete=" << complete << " cpl=" << cpl;
            expectOwedMoved(CarryMechanism::EphRelevel, counts, complete, [&](CarrySite s) {
                return carrySiteSpec(s).role == CarryRefRole::Regression && !cpl;
            });
            EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::PbHold)], 0);
            EXPECT_EQ(c.pb_hold.at("G01|L1C").held_m, e.pb_hold.at("G01|L1C").held_m);
            if (!complete) {
                NetworkCarryState f;
                populateEverySite(f);
                ephCutoverShiftCarry(f, "G01", d, cpl);
                EXPECT_TRUE(bitwiseEqual(referencedDoubles(f), referencedDoubles(c)));
            }
        }
}

// The eq34 D4 shift with the owed sites, on a state whose border value and coupling terms are
// G01 L1C's phase bias: the gross screen's held ambiguity moves like the ambiguity
// (-db - dr), the last solved phase bias like the carried phase bias (+db), and every other
// coupling map's terms by +db; the phase hold's held value and the bridged root do not.
TEST(CarrySites, Eq34ShiftCompleteMovesTheOwedSites) {
    const double db = 0.25, dr = 0.125, drpb = 0.0625;
    const std::map<std::pair<std::string, std::string>, double> shift_sat{{{"G01", "L1C"}, db}};
    const std::map<std::tuple<int, char, std::string>, double> shift_rxg{
        {std::make_tuple(0, 'G', std::string("L1C")), dr}};
    const std::map<std::pair<int, std::string>, double> shift_rpb{{{0, "GPS_RPB_L1C"}, drpb}};
    for (const bool complete : {false, true}) {
        NetworkCarryState c, e;   // not copyable: built twice
        populateEverySite(c, SharedParamKind::PhaseBias);
        populateEverySite(e, SharedParamKind::PhaseBias);
        long long nv = 0, nt = 0;
        CarrySiteCounts counts;
        eq34ShiftCarry(c, shift_sat, shift_rxg, shift_rpb, nv, nt, &counts, complete);
        const std::uint32_t mask = carryMechanismSiteMask(CarryMechanism::Eq34Shift, complete);
        long long n_expect = 0;
        const std::vector<double> want = expectedDoubles(e, [&](const CarriedRef& r) {
            if (!inMask(mask, r.site) || r.kind != SharedParamKind::PhaseBias) return 0.0;
            switch (r.site) {
                case CarrySite::AmbiguityValue:
                case CarrySite::HoldAmbiguityValue: return (0.0 - db) - dr;
                case CarrySite::RxPhaseBiasValue:   return drpb;
                default:                            return db;   // the phase bias, its baseline, a term
            }
        }, n_expect);
        EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), want)) << "complete=" << complete;
        EXPECT_EQ(movedTotal(counts), n_expect) << "complete=" << complete;
        // The legacy counters keep their meaning: the carried values, and every coupling term.
        EXPECT_EQ(nv, 3) << "complete=" << complete;
        EXPECT_EQ(nt, complete ? 11 : 3);
        expectOwedMoved(CarryMechanism::Eq34Shift, counts, complete,
                        [](CarrySite) { return false; });
        EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::PbHold)], 0);
        EXPECT_EQ(c.pb_hold.at("G01|L1C").held_m, e.pb_hold.at("G01|L1C").held_m);
        EXPECT_EQ(c.pin_bridge.at("G01_L1C").value, e.pin_bridge.at("G01_L1C").value);
        if (!complete) {
            NetworkCarryState f;
            populateEverySite(f, SharedParamKind::PhaseBias);
            eq34ShiftCarry(f, shift_sat, shift_rxg, shift_rpb, nv, nt);
            EXPECT_TRUE(bitwiseEqual(referencedDoubles(f), referencedDoubles(c)));
        }
    }
}

// The declared anchor handover with the owed sites, on a state whose border value and coupling
// terms are G01 L1C's phase bias: the gross screen's held ambiguity moves by -delta like the
// ambiguity, the last solved phase bias and the open pin bridge by +delta like the carried
// phase bias, and every other coupling map's terms by +delta; the phase hold's held value does
// not.
TEST(CarrySites, DeclaredHandoverCompleteMovesTheOwedSites) {
    const double delta = 0.5;
    for (const bool complete : {false, true}) {
        NetworkCarryState c, e;   // not copyable: built twice
        populateEverySite(c, SharedParamKind::PhaseBias);
        populateEverySite(e, SharedParamKind::PhaseBias);
        long long a = 0, b = 0, t = 0;
        CarrySiteCounts counts;
        declaredHandoverShiftCarry(c, "G01", "L1C", delta, a, b, t, &counts, complete);
        const std::uint32_t mask =
            carryMechanismSiteMask(CarryMechanism::DeclaredHandover, complete);
        long long n_expect = 0;
        const std::vector<double> want = expectedDoubles(e, [&](const CarriedRef& r) {
            if (!inMask(mask, r.site) || r.kind != SharedParamKind::PhaseBias || r.sat != "G01"
                || r.signal != "L1C")
                return 0.0;
            return (r.site == CarrySite::AmbiguityValue || r.site == CarrySite::HoldAmbiguityValue)
                       ? -delta : delta;
        }, n_expect);
        EXPECT_TRUE(bitwiseEqual(referencedDoubles(c), want)) << "complete=" << complete;
        EXPECT_EQ(movedTotal(counts), n_expect) << "complete=" << complete;
        // The legacy counters keep their meaning: the live ambiguities, the carried phase
        // biases, and every coupling term.
        EXPECT_EQ(a, 1);
        EXPECT_EQ(b, 1);
        EXPECT_EQ(t, complete ? 11 : 1);
        expectOwedMoved(CarryMechanism::DeclaredHandover, counts, complete,
                        [](CarrySite) { return false; });
        EXPECT_EQ(counts.visited[static_cast<int>(CarrySite::PbHold)], 0);
        EXPECT_EQ(c.pb_hold.at("G01|L1C").held_m, e.pb_hold.at("G01|L1C").held_m);
        if (!complete) {
            NetworkCarryState f;
            populateEverySite(f, SharedParamKind::PhaseBias);
            declaredHandoverShiftCarry(f, "G01", "L1C", delta, a, b, t);
            EXPECT_TRUE(bitwiseEqual(referencedDoubles(f), referencedDoubles(c)));
        }
    }
}

namespace {

namespace fs = std::filesystem;

/// A minimal two-station GPS network configuration with @p network_extra in its network
/// section, written to a temporary file and loaded (the HandoverStransformConfig pattern).
struct RebaseLoaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    RebaseLoaded(const std::string& tag, const std::string& network_extra) {
        const std::string body = std::string("{\n") +
            "  \"processing\": {\n"
            "    \"mode\": \"ppp-rtk-network\",\n"
            "    \"active_systems\": [ \"GPS\" ],\n"
            "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
            "  },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            + network_extra +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = fs::temp_directory_path() / ("carry_rebase_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << body;
        f.close();
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~RebaseLoaded() { std::error_code ec; fs::remove(path, ec); }
};

}  // namespace

// The loader: the key is read, defaults false, and is disarmed (a structural zero) when no
// re-base mechanism with owed sites is armed; the knob snapshot carries it.
TEST(CarryRebaseCompleteConfig, DefaultsOffReadWithAMechanismAndInertWithout) {
    {
        RebaseLoaded off("off", "");
        EXPECT_FALSE(off.cfg.networkCarryRebaseComplete());
        EXPECT_FALSE(NetworkKnobs::snapshot(off.cfg, off.cfg.getNetworkConfig()).rebase_complete);
    }
    {
        RebaseLoaded inert("inert", "    \"carry_rebase_complete\": true,\n");
        EXPECT_FALSE(inert.cfg.networkCarryRebaseComplete());
        EXPECT_FALSE(
            NetworkKnobs::snapshot(inert.cfg, inert.cfg.getNetworkConfig()).rebase_complete);
    }
    {
        // The broadcast a priori arms the ephemeris cut-over re-level.
        RebaseLoaded on("on",
                        "    \"apriori_source\": \"brdc_rts\",\n"
                        "    \"carry_rebase_complete\": true,\n");
        EXPECT_TRUE(on.cfg.networkCarryRebaseComplete());
        EXPECT_TRUE(NetworkKnobs::snapshot(on.cfg, on.cfg.getNetworkConfig()).rebase_complete);
    }
    {
        // The re-level alone, without the knob: off.
        RebaseLoaded brdc("brdc", "    \"apriori_source\": \"brdc_rts\",\n");
        EXPECT_FALSE(brdc.cfg.networkCarryRebaseComplete());
    }
}
