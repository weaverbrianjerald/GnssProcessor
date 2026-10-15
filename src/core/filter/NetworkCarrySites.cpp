/**
 * @file NetworkCarrySites.cpp
 * @brief The site table, the mechanism table and the traversal of carried references
 *        (card PRM phase 2).  The design is in the header's file comment.
 */

#include "filter/NetworkCarrySites.hpp"

#include "app/NetworkRunContext.hpp"
#include "filter/StateKeySchema.hpp"   // StateKeys::parseAmbiguityKey ("sat_signal" keys)

#include <array>
#include <tuple>
#include <utility>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "logging/NetworkRunTraces.hpp"
#include <fmt/format.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace GnssProcessor {

namespace {

using S = CarrySite;
using ES = net::StationState;         // the station carries (card NOM step N4a)
using ESS = net::StationSignalState;
using EC = net::ConstellationState;   // the constellation carries (card NOM step N4b)
using ECh = net::ChannelState;
using EL = net::LosState;             // the line-of-sight carries (card NOM step N4c)
using EG = net::SatSignalState;       // the satellite-signal carries (card NOM step N4d)
using EA = net::SatelliteState;       // the satellite carries (card NOM step N4e1)
using R = CarryRefRole;

// One row per site, in the order of CarrySite (checked by the unit test).
const std::array<CarrySiteSpec, kCarrySites> kSites{{
    {S::PrevCarry, "prev_carry", "prev_carry", "(kind, sat, signal)", R::Value,
     "the carried border value and its variance (READMIT parked entries included)",
     "value moves; a re-expression also moves the variance", "D",
     "live entries (state_snapshot_path); a parked entry is cold-started (READMIT restarts cold)"},
    {S::CxuPivot, "cxu_pivot", "cxu_pivot", "constellation -> (sat, value)", R::PinTarget,
     "the stash's pinned clock and its value (the exact coupling update's gauge deficit)",
     "value moves with the pivot's clock", nullptr,
     "derived at restore from the QK/QP border with value 0 (card CTU)"},
    {S::ClockOsc, "clock_osc", "clock_osc", "sat", R::Value,
     "the two-state oscillator's carried clock offset x0 (network.clock_model)",
     "x0 moves when the oscillator is initialized", "K", "always"},
    {S::PrevClk, "prev_clk", "prev_clk", "sat", R::Baseline,
     "the clock monitor's jump baseline (first)", "moves (a re-base is not a clock step)",
     "J", "always (filled under network.clock_monitor)"},
    {S::CbExcPrevClk, "cb_exc_prev_clk", "cb_exc_prev_clk", "sat", R::Baseline,
     "the code-bias excursion test's previous solved clock", "moves", nullptr,
     "cold-started"},
    {S::PbHoldPrevClock, "pb_hold_prev_clock", "pb_hold_prev_clock_m", "sat", R::Baseline,
     "the emitted clock at the phase hold's previous emission", "moves", nullptr,
     "cold-started (the hold restarts with a new generation)"},
    {S::PbLevelPrev, "pb_level_prev", "pb_level_prev", "SAT_SIGNAL", R::Baseline,
     "the last solved shared phase bias (the pin bridge's source, the S-basis completion)",
     "moves", nullptr, "cold-started (re-harvested at the next stash)"},
    {S::PinBridge, "pin_bridge", "pin_bridge", "SAT_SIGNAL", R::Held,
     "an open pin bridge's held phase bias (value)", "moves", nullptr, "cold-started"},
    {S::PbHold, "pb_hold", "pb_hold", "SAT|SIGNAL", R::Held,
     "the phase hold's held value (held_m, emit frame)",
     "not moved: held_m is an emitted value in the emit frame",
     nullptr, "cold-started (the generation advances)"},
    {S::CodeBiasPrev, "code_bias_prev", "code_bias_prev_", "<const>|<signal> -> sat",
     R::Baseline, "the time-linked code-bias datum's previous solved value", "moves", "CBP",
     "state_snapshot_datum_capture"},
    {S::AmbiguityValue, "ambiguity_value", "local_carry", "(station, sat, signal)",
     R::LocalValue, "the carried ambiguity (value_abs), in its satellite phase bias's frame",
     "moves opposite to its phase bias and receiver phase bias", "L", "always"},
    {S::AmbiguityCoupling, "ambiguity_coupling", "local_carry", "(station, sat, signal)",
     R::Regression, "the ambiguity's regression terms on the border (s_prev, p_prev)",
     "s_prev moves with its column; a re-expression also moves p_prev", "QL",
     "state_snapshot_coupling_capture (QS dictionary)"},
    {S::ZwdCoupling, "zwd_coupling", "zwd_carry", "station", R::Regression,
     "the wet delay's regression terms on the border", "s_prev moves with its column", "QZ",
     "state_snapshot_coupling_capture (QS dictionary)"},
    {S::IonoCoupling, "iono_coupling", "iono_carry", "(station, sat)", R::Regression,
     "the diagonal slant ionosphere carry's regression terms (none are harvested)",
     "s_prev moves with its column", nullptr, "cold-started (station-local, not snapshotted)"},
    {S::IonoBlockCoupling, "iono_block_coupling", "iono_blk_carry", "(station, sat)",
     R::Regression, "the in-block slant ionosphere's regression terms",
     "s_prev moves with its column", nullptr, "cold-started (station-local, not snapshotted)"},
    {S::HoldAmbiguityValue, "hold_ambiguity_value", "gl_hold_amb", "(station, sat, signal)",
     R::LocalValue, "the gross screen's held ambiguity (re-applied over local_carry)",
     "moves like the ambiguity", nullptr, "cold-started"},
    {S::HoldAmbiguityCoupling, "hold_ambiguity_coupling", "gl_hold_amb",
     "(station, sat, signal)", R::Regression,
     "the held ambiguity's regression terms (cleared at the declaration)",
     "s_prev moves with its column", nullptr, "cold-started"},
    {S::HoldIonoCoupling, "hold_iono_coupling", "gl_hold_iono", "(station, sat)",
     R::Regression, "the held slant ionosphere's regression terms (cleared at the declaration)",
     "s_prev moves with its column", nullptr, "cold-started"},
    {S::HoldIonoBlockCoupling, "hold_iono_block_coupling", "gl_hold_iono_blk",
     "(station, sat)", R::Regression,
     "the held in-block ionosphere's regression terms (cleared at the declaration)",
     "s_prev moves with its column", nullptr, "cold-started"},
    {S::RxClockValue, "rx_clock_value", "rx_clk_carry", "(station, system)", R::LocalValue,
     "the carried receiver clock or ISB (value_abs), in the clock datum's frame",
     "moves with the clock datum", nullptr, "cold-started (station-local, not snapshotted)"},
    {S::RxClockCoupling, "rx_clock_coupling", "rx_clk_carry", "(station, system)",
     R::Regression, "the receiver clock's regression terms on the border",
     "s_prev moves with its column", nullptr, "cold-started (station-local, not snapshotted)"},
    {S::RxPhaseBiasValue, "rx_phase_bias_value", "rpb_carry", "(station, sys_RPB_signal)",
     R::LocalValue, "the eq34 chain's carried receiver phase bias (value_abs)",
     "moves with the receiver's chain frame", nullptr, "cold-started"},
    {S::RxPhaseBiasCoupling, "rx_phase_bias_coupling", "rpb_carry",
     "(station, sys_RPB_signal)", R::Regression,
     "the receiver phase bias's regression terms (none are harvested)",
     "s_prev moves with its column", nullptr, "cold-started"},
    {S::RxCodeBiasValue, "rx_code_bias_value", "rx_cb_carry", "(station, code-bias key)",
     R::LocalValue, "the carried receiver code bias (value_abs), in the code-bias datum's frame",
     "moves with the code-bias datum", "RC", "state_snapshot_station_capture"},
    {S::RxCodeBiasCoupling, "rx_code_bias_coupling", "rx_cb_carry",
     "(station, code-bias key)", R::Regression,
     "the receiver code bias's regression terms (the in-block route)",
     "s_prev moves with its column", nullptr,
     "cold-started (RC restores the value only; the in-block route re-harvests)"},
    {S::PhaseColourCoupling, "phase_colour_coupling", "pc_carry", "(station, sat)",
     R::Regression, "the phase-colour nuisance state's regression terms (card LGM)",
     "s_prev moves with its column", nullptr, "cold-started (not snapshotted)"},
}};

// The reasons an exemption gives.  "Owed" exemptions are the gaps of the hand lists this
// card replaced (card PRM section 15 and phase 3b), which network.carry_rebase_complete moves
// (owedReason below); the others are by design.
constexpr const char* kOwed =
    "not moved before card PRM phase 2 (a gap of the hand-written list; owed to phase 3b)";
constexpr const char* kOwedClv64 =
    "not moved before card PRM phase 2 (card CLV section 6.4 item 6; owed to phase 3b)";
constexpr const char* kStsRowsBefore =
    "the handover epoch's rows are built before the transform; from the next epoch every "
    "term is re-harvested in the incoming datum";
constexpr const char* kStsCxuPivot =
    "the rows of the handover epoch read the outgoing pivot's stash before the transform";
constexpr const char* kStsOsc =
    "erased for the changed constellation at the datum change (NetworkClockQc)";
constexpr const char* kStsPrevClk =
    "the jump test skips the changed constellation and re-seeds after the solve";
constexpr const char* kStsCbExc = "the excursion test is suppressed at a datum change";
constexpr const char* kStsHoldClock =
    "the emitted clock steps by the datum delta at a handover, by design";
constexpr const char* kStsRxClock =
    "the transform's direction spans the satellite clocks; the receiver clock is on the "
    "datum's direction too, so the transform before the rows is refused with "
    "network.carry_rx_clock_in_block (owed)";
constexpr const char* kD4BridgeRoot =
    "a bridged satellite is a root of the chain with a known zero shift (by construction)";
constexpr const char* kPbHoldEmit =
    "held_m is a held emitted value in the emit frame: moving it needs its own design, so "
    "it is exempt from network.carry_rebase_complete too";

/// Whether an exemption's reason is an owed gap (moved under network.carry_rebase_complete,
/// card PRM phase 3b step 2).  The handover's reasons, the receiver clock's included, are its
/// own design and its walks take no knob.
bool owedReason(const char* reason) {
    return reason == kOwed || reason == kOwedClv64;
}

/// One mechanism's row: the sites it moves and the sites it exempts with a reason.
struct MechRow {
    std::uint32_t moves;
    std::array<const char*, kCarrySites> exempt;   // null: moves or does not apply
};

constexpr std::uint32_t bit(S s) { return carrySiteBit(s); }

MechRow ephRelevelRow() {
    MechRow r{};
    r.moves = bit(S::PrevCarry) | bit(S::ClockOsc) | bit(S::PrevClk)
            | bit(S::AmbiguityCoupling) | bit(S::ZwdCoupling);
    for (S s : {S::CxuPivot, S::CbExcPrevClk, S::PbHoldPrevClock})
        r.exempt[static_cast<int>(s)] = (s == S::CxuPivot) ? kOwed : kOwedClv64;
    for (S s : {S::IonoCoupling, S::IonoBlockCoupling, S::HoldAmbiguityCoupling,
                S::HoldIonoCoupling, S::HoldIonoBlockCoupling, S::RxClockCoupling,
                S::RxPhaseBiasCoupling, S::RxCodeBiasCoupling, S::PhaseColourCoupling})
        r.exempt[static_cast<int>(s)] = kOwedClv64;
    return r;
}

MechRow handoverRow(bool before_rows) {
    MechRow r{};
    r.moves = bit(S::PrevCarry);
    if (before_rows)
        r.moves |= bit(S::CxuPivot) | bit(S::AmbiguityCoupling) | bit(S::ZwdCoupling)
                 | bit(S::IonoCoupling) | bit(S::IonoBlockCoupling)
                 | bit(S::HoldAmbiguityCoupling) | bit(S::HoldIonoCoupling)
                 | bit(S::HoldIonoBlockCoupling) | bit(S::RxClockCoupling)
                 | bit(S::RxPhaseBiasCoupling) | bit(S::RxCodeBiasCoupling)
                 | bit(S::PhaseColourCoupling);
    else {
        r.exempt[static_cast<int>(S::CxuPivot)] = kStsCxuPivot;
        for (S s : {S::AmbiguityCoupling, S::ZwdCoupling, S::IonoCoupling,
                    S::IonoBlockCoupling, S::HoldAmbiguityCoupling, S::HoldIonoCoupling,
                    S::HoldIonoBlockCoupling, S::RxClockCoupling, S::RxPhaseBiasCoupling,
                    S::RxCodeBiasCoupling, S::PhaseColourCoupling})
            r.exempt[static_cast<int>(s)] = kStsRowsBefore;
    }
    r.exempt[static_cast<int>(S::ClockOsc)]        = kStsOsc;
    r.exempt[static_cast<int>(S::PrevClk)]         = kStsPrevClk;
    r.exempt[static_cast<int>(S::CbExcPrevClk)]    = kStsCbExc;
    r.exempt[static_cast<int>(S::PbHoldPrevClock)] = kStsHoldClock;
    r.exempt[static_cast<int>(S::RxClockValue)]    = kStsRxClock;
    return r;
}

MechRow eq34ShiftRow() {
    MechRow r{};
    r.moves = bit(S::PrevCarry) | bit(S::AmbiguityValue) | bit(S::RxPhaseBiasValue)
            | bit(S::AmbiguityCoupling) | bit(S::RxPhaseBiasCoupling) | bit(S::ZwdCoupling);
    for (S s : {S::PbLevelPrev, S::HoldAmbiguityValue, S::IonoCoupling,
                S::IonoBlockCoupling, S::HoldAmbiguityCoupling, S::HoldIonoCoupling,
                S::HoldIonoBlockCoupling, S::RxClockCoupling, S::RxCodeBiasCoupling,
                S::PhaseColourCoupling})
        r.exempt[static_cast<int>(s)] = kOwed;
    r.exempt[static_cast<int>(S::PbHold)]    = kPbHoldEmit;
    r.exempt[static_cast<int>(S::PinBridge)] = kD4BridgeRoot;
    return r;
}

MechRow declaredHandoverRow() {
    MechRow r{};
    r.moves = bit(S::PrevCarry) | bit(S::AmbiguityValue) | bit(S::AmbiguityCoupling);
    for (S s : {S::PbLevelPrev, S::PinBridge, S::HoldAmbiguityValue,
                S::ZwdCoupling, S::IonoCoupling, S::IonoBlockCoupling,
                S::HoldAmbiguityCoupling, S::HoldIonoCoupling, S::HoldIonoBlockCoupling,
                S::RxClockCoupling, S::RxPhaseBiasCoupling, S::RxCodeBiasCoupling,
                S::PhaseColourCoupling})
        r.exempt[static_cast<int>(s)] = kOwed;
    r.exempt[static_cast<int>(S::PbHold)] = kPbHoldEmit;
    return r;
}

const MechRow& mechRow(CarryMechanism m) {
    static const std::array<MechRow, kCarryMechanisms> rows{{
        ephRelevelRow(), handoverRow(false), handoverRow(true), eq34ShiftRow(),
        declaredHandoverRow()}};
    return rows[static_cast<std::size_t>(m)];
}

}  // namespace

const CarrySiteSpec& carrySiteSpec(CarrySite s) {
    return kSites[static_cast<std::size_t>(s)];
}

const char* carryRefRoleName(CarryRefRole r) {
    switch (r) {
        case R::Value:      return "value";
        case R::Regression: return "regression";
        case R::Baseline:   return "baseline";
        case R::Held:       return "held";
        case R::PinTarget:  return "pin_target";
        case R::LocalValue: return "local_value";
    }
    return "?";
}

const char* carryMechanismName(CarryMechanism m) {
    switch (m) {
        case CarryMechanism::EphRelevel:         return "eph_relevel";
        case CarryMechanism::Handover:           return "handover";
        case CarryMechanism::HandoverBeforeRows: return "handover_before_rows";
        case CarryMechanism::Eq34Shift:          return "eq34_shift";
        case CarryMechanism::DeclaredHandover:   return "declared_handover";
    }
    return "?";
}

CarryMechanismSite carryMechanismSite(CarryMechanism m, CarrySite s) {
    const MechRow& r = mechRow(m);
    CarryMechanismSite out;
    out.moves  = (r.moves & carrySiteBit(s)) != 0;
    out.reason = out.moves ? nullptr : r.exempt[static_cast<std::size_t>(s)];
    return out;
}

std::uint32_t carryMechanismSites(CarryMechanism m) { return mechRow(m).moves; }

std::uint32_t carryMechanismOwedSites(CarryMechanism m) {
    const MechRow& r = mechRow(m);
    std::uint32_t owed = 0;
    for (int i = 0; i < kCarrySites; ++i) {
        const auto s = static_cast<S>(i);
        if (s == S::PbHold) continue;   // a held emitted value: its own design (kPbHoldEmit)
        if ((r.moves & carrySiteBit(s)) == 0 && owedReason(r.exempt[static_cast<std::size_t>(i)]))
            owed |= carrySiteBit(s);
    }
    return owed;
}

std::uint32_t carryMechanismSiteMask(CarryMechanism m, bool complete) {
    return carryMechanismSites(m) | (complete ? carryMechanismOwedSites(m) : 0u);
}

bool carryMechanismMoves(CarryMechanism m, CarrySite s, bool complete) {
    return (carryMechanismSiteMask(m, complete) & carrySiteBit(s)) != 0;
}

long long forEachCarriedReference(NetworkCarryState& carry, net::NetworkEntities& entities,
                                  std::uint32_t sites,
                                  const CarryRefVisitor& visit, CarrySiteCounts* counts) {
    static const std::string kNone;
    long long moved = 0;
    const auto on = [&](S s) { return (sites & carrySiteBit(s)) != 0; };
    const auto hand = [&](const CarriedRef& r) {
        const auto i = static_cast<std::size_t>(r.site);
        if (counts) ++counts->visited[i];
        if (visit(r)) {
            ++moved;
            if (counts) {
                ++counts->moved[i];
                if (r.site == S::PrevCarry || r.site == S::ClockOsc)
                    ++counts->moved_border[static_cast<int>(r.kind)];
            }
        }
    };
    if (on(S::PrevCarry))
        for (CarryPrior& cp : carry.prev_carry)
            hand(CarriedRef{S::PrevCarry, cp.kind, cp.sat, cp.signal, -1, &cp.value, &cp.var,
                            cp.t_park_s > 0.0, &cp, nullptr, nullptr, nullptr});
    if (on(S::CxuPivot))
        net::forEach(entities.constellations, &EC::cxu_pivot,
                     [&](System, std::pair<std::string, double>& pv) {
                         hand(CarriedRef{S::CxuPivot, SharedParamKind::Clock, pv.first, kNone, -1,
                                         &pv.second, nullptr, false, nullptr, nullptr, nullptr, nullptr});
                     });
    // The satellite carries (card NOM step N4e1), in satellite order: the order of their old
    // satellite-string keys (O1).
    if (on(S::ClockOsc))
        net::forEach(entities.sats, &EA::clock_osc, [&](const SatId& id, ClockOsc& o) {
            hand(CarriedRef{S::ClockOsc, SharedParamKind::Clock, net::satKey(id), kNone, -1, &o.x0,
                            nullptr, false, nullptr, nullptr, nullptr, &o});
        });
    if (on(S::PrevClk))
        net::forEach(entities.sats, &EA::prev_clk, [&](const SatId& id, std::pair<double, double>& b) {
            hand(CarriedRef{S::PrevClk, SharedParamKind::Clock, net::satKey(id), kNone, -1, &b.first,
                            nullptr, false, nullptr, nullptr, nullptr, nullptr});
        });
    if (on(S::CbExcPrevClk))
        net::forEach(entities.sats, &EA::cb_exc_prev_clk, [&](const SatId& id, double& v) {
            hand(CarriedRef{S::CbExcPrevClk, SharedParamKind::Clock, net::satKey(id), kNone, -1, &v,
                            nullptr, false, nullptr, nullptr, nullptr, nullptr});
        });
    if (on(S::PbHoldPrevClock))
        net::forEach(entities.sats, &EA::pb_hold_prev_clock_m, [&](const SatId& id, double& v) {
            hand(CarriedRef{S::PbHoldPrevClock, SharedParamKind::Clock, net::satKey(id), kNone, -1, &v,
                            nullptr, false, nullptr, nullptr, nullptr, nullptr});
        });
    // The satellite-signal carries (card NOM step N4d), in (satellite, signal) order: the
    // order of their old "sat_signal" / "sat|signal" keys (O1).
    if (on(S::PbLevelPrev))
        net::forEach(entities.sat_signals, &EG::pb_level_prev, [&](const SatSignalId& id, double& v) {
            hand(CarriedRef{S::PbLevelPrev, SharedParamKind::PhaseBias, net::satKey(id.sat),
                            net::signalKey(id.signal), -1, &v, nullptr, false, nullptr, nullptr,
                            nullptr, nullptr});
        });
    if (on(S::PinBridge))
        net::forEach(entities.sat_signals, &EG::pin_bridge, [&](const SatSignalId& id, net::PinBridge& br) {
            hand(CarriedRef{S::PinBridge, SharedParamKind::PhaseBias, net::satKey(id.sat),
                            net::signalKey(id.signal), -1, &br.value, nullptr, false, nullptr,
                            nullptr, nullptr, nullptr});
        });
    if (on(S::PbHold))
        net::forEach(entities.sat_signals, &EG::pb_hold, [&](const SatSignalId& id, net::PbHoldKey& st) {
            hand(CarriedRef{S::PbHold, SharedParamKind::PhaseBias, net::satKey(id.sat),
                            net::signalKey(id.signal), -1, &st.held_m, nullptr, false, nullptr,
                            nullptr, nullptr, nullptr});
        });
    if (on(S::CodeBiasPrev))
        net::forEach(entities.constellations, &EC::channels, &ECh::code_bias_prev,
                     [&](System, const std::string& sig, std::map<std::string, double>& per_sat) {
                         for (auto& [sat, v] : per_sat)
                             hand(CarriedRef{S::CodeBiasPrev, SharedParamKind::CodeBias, sat, sig, -1, &v,
                                             nullptr, false, nullptr, nullptr, nullptr, nullptr});
                     });
    // A local carry map: its value in a border frame (when the site exists) and its
    // regression terms.
    const auto terms = [&](S site, int station, LocalCarry& lc) {
        for (CoupledShared& cs : lc.coupling)
            hand(CarriedRef{site, cs.kind, cs.sat, cs.signal, station, &cs.s_prev, &cs.p_prev,
                            false, nullptr, &cs, &lc, nullptr});
    };
    if (on(S::AmbiguityValue))
        for (auto& [key, lc] : carry.local_carry)
            hand(CarriedRef{S::AmbiguityValue, SharedParamKind::PhaseBias, std::get<1>(key),
                            std::get<2>(key), std::get<0>(key), &lc.value_abs, nullptr, false,
                            nullptr, nullptr, &lc, nullptr});
    if (on(S::AmbiguityCoupling))
        for (auto& [key, lc] : carry.local_carry) terms(S::AmbiguityCoupling, std::get<0>(key), lc);
    if (on(S::ZwdCoupling))
        net::forEach(entities.stations, &ES::zwd, [&](const StationId& id, LocalCarry& lc) {
            terms(S::ZwdCoupling, id.index, lc);
        });
    if (on(S::IonoCoupling))
        net::forEach(entities.los, &EL::iono,
                     [&](const LosId& id, LocalCarry& lc) { terms(S::IonoCoupling, id.station.index, lc); });
    if (on(S::IonoBlockCoupling))
        net::forEach(entities.los, &EL::iono_blk, [&](const LosId& id, LocalCarry& lc) {
            terms(S::IonoBlockCoupling, id.station.index, lc);
        });
    if (on(S::HoldAmbiguityValue))
        for (auto& [key, lc] : carry.gl_hold_amb)
            hand(CarriedRef{S::HoldAmbiguityValue, SharedParamKind::PhaseBias, std::get<1>(key),
                            std::get<2>(key), std::get<0>(key), &lc.value_abs, nullptr, false,
                            nullptr, nullptr, &lc, nullptr});
    if (on(S::HoldAmbiguityCoupling))
        for (auto& [key, lc] : carry.gl_hold_amb)
            terms(S::HoldAmbiguityCoupling, std::get<0>(key), lc);
    if (on(S::HoldIonoCoupling))
        net::forEach(entities.los, &EL::gl_hold_iono, [&](const LosId& id, LocalCarry& lc) {
            terms(S::HoldIonoCoupling, id.station.index, lc);
        });
    if (on(S::HoldIonoBlockCoupling))
        net::forEach(entities.los, &EL::gl_hold_iono_blk, [&](const LosId& id, LocalCarry& lc) {
            terms(S::HoldIonoBlockCoupling, id.station.index, lc);
        });
    if (on(S::RxClockValue))
        net::forEach(entities.stations, &ES::signals, &ESS::rx_clk,
                     [&](const StationId& id, const std::string& key, LocalCarry& lc) {
                         hand(CarriedRef{S::RxClockValue, SharedParamKind::Clock, kNone, key, id.index,
                                         &lc.value_abs, nullptr, false, nullptr, nullptr, &lc, nullptr});
                     });
    if (on(S::RxClockCoupling))
        net::forEach(entities.stations, &ES::signals, &ESS::rx_clk,
                     [&](const StationId& id, const std::string&, LocalCarry& lc) {
                         terms(S::RxClockCoupling, id.index, lc);
                     });
    if (on(S::RxPhaseBiasValue))
        net::forEach(entities.stations, &ES::signals, &ESS::rpb,
                     [&](const StationId& id, const std::string& key, LocalCarry& lc) {
                         hand(CarriedRef{S::RxPhaseBiasValue, SharedParamKind::PhaseBias, kNone, key,
                                                 id.index, &lc.value_abs, nullptr, false, nullptr, nullptr, &lc,
                                                 nullptr});
                     });
    if (on(S::RxPhaseBiasCoupling))
        net::forEach(entities.stations, &ES::signals, &ESS::rpb,
                     [&](const StationId& id, const std::string&, LocalCarry& lc) {
                         terms(S::RxPhaseBiasCoupling, id.index, lc);
                     });
    if (on(S::RxCodeBiasValue))
        net::forEach(entities.stations, &ES::signals, &ESS::rx_cb,
                     [&](const StationId& id, const std::string& key, LocalCarry& lc) {
                         hand(CarriedRef{S::RxCodeBiasValue, SharedParamKind::CodeBias, kNone, key,
                                                 id.index, &lc.value_abs, nullptr, false, nullptr, nullptr, &lc,
                                                 nullptr});
                     });
    if (on(S::RxCodeBiasCoupling))
        net::forEach(entities.stations, &ES::signals, &ESS::rx_cb,
                     [&](const StationId& id, const std::string&, LocalCarry& lc) {
                         terms(S::RxCodeBiasCoupling, id.index, lc);
                     });
    if (on(S::PhaseColourCoupling))
        net::forEach(entities.los, &EL::pc, [&](const LosId& id, LocalCarry& lc) {
            terms(S::PhaseColourCoupling, id.station.index, lc);
        });
    return moved;
}

void carrySiteHeld(const NetworkCarryState& carry, const net::NetworkEntities& entities,
                   long long (&held)[kCarrySites]) {
    CarrySiteCounts c;
    // The walk writes nothing, creates no entry and reads no value (its contract), and this
    // visitor moves nothing, so counting through the mutable walk leaves the carry as it was.
    forEachCarriedReference(const_cast<NetworkCarryState&>(carry),
                            const_cast<net::NetworkEntities&>(entities), kAllCarrySites,
                            [](const CarriedRef&) { return false; }, &c);
    for (int i = 0; i < kCarrySites; ++i) held[i] = c.visited[i];
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the carried references per site (PRM 2).
int networkCarrySitesRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;

    // Card PRM phase 2 (sections 5 and 6): the carried references of the border per site,
    // held at the run's end and moved by each mechanism over the run through the one
    // traversal; then, per mechanism, the sites it does not move with the reason.  The
    // same rows go to the run manifest as a `param_sites` record.
    const auto& C = ctx.counters;
    long long held[kCarrySites]{};
    carrySiteHeld(ctx.carry, ctx.entities, held);
    std::string js = "{\"record\":\"param_sites\",\"sites\":[";
    for (int i = 0; i < kCarrySites; ++i) {
        const CarrySiteSpec& sp = carrySiteSpec(static_cast<CarrySite>(i));
        std::string moved, jm;
        for (int m = 0; m < kCarryMechanisms; ++m) {
            const auto mech = static_cast<CarryMechanism>(m);
            const long long n = C.prm_ref_moved.site[i][m];
            if (carryMechanismMoves(mech, sp.site, ctx.knobs.rebase_complete) || n > 0)
                moved += fmt::format("{}{} {}", moved.empty() ? "" : ", ",
                                     carryMechanismName(mech), n);
            jm += fmt::format("{}\"{}\":{}", m ? "," : "", carryMechanismName(mech), n);
        }
        log->info("Network (param model, run end): site {} ({}, {}): {} held; moved: {}; "
                  "snapshot {}.",
                  sp.name, sp.container, carryRefRoleName(sp.role), held[i],
                  moved.empty() ? std::string("by no mechanism") : moved,
                  sp.record ? sp.record : "none (cold-started)");
        js += fmt::format("{}{{\"site\":\"{}\",\"container\":\"{}\",\"role\":\"{}\","
                          "\"held\":{},\"moved\":{{{}}},\"snapshot\":\"{}\"}}",
                          i ? "," : "", sp.name, sp.container, carryRefRoleName(sp.role),
                          held[i], jm, sp.record ? sp.record : "");
    }
    js += "],\"mechanisms\":[";
    // Card PRM phase 3b: under network.carry_rebase_complete a mechanism's owed sites are
    // moved, so they leave its "not moved" list for a list of their own (the line and the
    // record are unchanged with the knob off).
    const bool complete = ctx.knobs.rebase_complete;
    for (int m = 0; m < kCarryMechanisms; ++m) {
        const auto mech = static_cast<CarryMechanism>(m);
        const std::uint32_t owed =
            (complete && carryMechanismArmed(mech, ctx.knobs, config_))
                ? carryMechanismOwedSites(mech) : 0u;
        std::string exempt, jx, now_moved, jo;
        for (int i = 0; i < kCarrySites; ++i) {
            const auto s = static_cast<CarrySite>(i);
            const CarryMechanismSite ms = carryMechanismSite(mech, s);
            if (!ms.reason) continue;
            if (owed & carrySiteBit(s)) {
                now_moved += fmt::format("{}{}", now_moved.empty() ? "" : ", ",
                                         carrySiteSpec(s).name);
                jo += fmt::format("{}\"{}\"", jo.empty() ? "" : ",", carrySiteSpec(s).name);
                continue;
            }
            exempt += fmt::format("{}{}", exempt.empty() ? "" : ", ", carrySiteSpec(s).name);
            jx += fmt::format("{}{{\"site\":\"{}\",\"reason\":\"{}\"}}", jx.empty() ? "" : ",",
                              carrySiteSpec(s).name, ms.reason);
        }
        if (owed == 0u) {
            log->info("Network (param model, run end): mechanism {}: {} call(s); not moved: {}.",
                      carryMechanismName(mech), C.prm_ref_calls[m],
                      exempt.empty() ? std::string("none") : exempt);
            js += fmt::format("{}{{\"mechanism\":\"{}\",\"calls\":{},\"exempt\":[{}]}}",
                              m ? "," : "", carryMechanismName(mech), C.prm_ref_calls[m], jx);
        } else {
            log->info("Network (param model, run end): mechanism {}: {} call(s); not moved: {}; "
                      "the owed sites moved under network.carry_rebase_complete: {}.",
                      carryMechanismName(mech), C.prm_ref_calls[m],
                      exempt.empty() ? std::string("none") : exempt, now_moved);
            js += fmt::format("{}{{\"mechanism\":\"{}\",\"calls\":{},\"exempt\":[{}],"
                              "\"rebase_complete\":[{}]}}",
                              m ? "," : "", carryMechanismName(mech), C.prm_ref_calls[m], jx,
                              jo);
        }
    }
    js += "]}";
    traces.appendRunManifestRecord(config_.getNetworkConfig(), js);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
