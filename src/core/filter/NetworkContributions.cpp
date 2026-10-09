/**
 * @file NetworkContributions.cpp
 * @brief The contribution declarations, the row tags' helpers, the tallies and the
 *        "contributions as armed" table (card NOM step N2a).  See
 *        filter/NetworkContributions.hpp.
 */

#include "filter/NetworkContributions.hpp"

#include "app/NetworkRunContext.hpp"        // NetworkRunContext::contributions
#include "config/GnssConfig.hpp"            // NetworkConfig (the manifest switch)
#include "filter/NetworkEstimator.hpp"      // NetworkEstimator::sharedFoldLog
#include "filter/NetworkParamRegistry.hpp"  // NetworkParamKind, paramKindName
#include "filter/NetworkRunTypes.hpp"       // StationAssembly, CplRowRaw
#include "filter/StateKeySchema.hpp"
#include "logging/NetworkRunTraces.hpp"     // appendRunManifestRecord

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace GnssProcessor {

namespace K = StateKeys;

namespace {

using CK = ContributionKind;
using CC = ContributionChannel;
using CG = ContributionStage;
using CS = ContributionSubject;
using PK = NetworkParamKind;

constexpr ParamKindSet bit(PK k) { return ParamKindSet{1} << static_cast<unsigned>(k); }

// Parameter sets shared by several families.
constexpr ParamKindSet kBorder = bit(PK::SatClock) | bit(PK::SatPhaseBias)
                               | bit(PK::SatCodeBias) | bit(PK::VtecSh);
constexpr ParamKindSet kCodeRow = bit(PK::SatClock) | bit(PK::SatCodeBias) | bit(PK::RxClock)
                                | bit(PK::RxCodeBias) | bit(PK::SlantIono) | bit(PK::Zwd);
constexpr ParamKindSet kPhaseRow = bit(PK::SatClock) | bit(PK::SatPhaseBias) | bit(PK::Ambiguity)
                                 | bit(PK::SlantIono) | bit(PK::Zwd) | bit(PK::RxClock)
                                 | bit(PK::RxPhaseBias) | bit(PK::PhaseColour);
// A whitened local block row spans its own column and every block column pushed before it
// (L⁻¹ is lower triangular); the push order is ambiguity, wet delay, ionosphere, receiver
// clock, receiver code bias, phase colour (ApplicationNetworkAssembly.cpp, PASS B).
constexpr ParamKindSet kBlkAmb   = kBorder | bit(PK::Ambiguity);
constexpr ParamKindSet kBlkZwd   = kBlkAmb | bit(PK::Zwd);
constexpr ParamKindSet kBlkIono  = kBlkZwd | bit(PK::SlantIono);
constexpr ParamKindSet kBlkRxClk = kBlkIono | bit(PK::RxClock);
constexpr ParamKindSet kBlkRxCb  = kBlkRxClk | bit(PK::RxCodeBias);
constexpr ParamKindSet kBlkPc    = kBlkRxCb | bit(PK::PhaseColour);

constexpr const char* kSplit   = "MeasurementModel::computeMeasurementUpdate, "
                                 "Application::networkSplitStation_ (ApplicationNetworkAssembly.cpp)";
constexpr const char* kPinFold = "Application::networkPassB_, phase-datum pin fold "
                                 "(ApplicationNetworkAssembly.cpp)";
constexpr const char* kGloFold = "Application::networkPassB_, GLONASS anchors "
                                 "(ApplicationNetworkAssembly.cpp)";
constexpr const char* kCarryWalk = "Application::networkPassB_, carry walk "
                                   "(ApplicationNetworkAssembly.cpp)";
constexpr const char* kLgmRows = "Application::networkPassB_, card LGM rows "
                                 "(ApplicationNetworkAssembly.cpp)";
constexpr const char* kCplRows = "CouplingRowBuilder::buildCplRow, appendCouplingRows "
                                 "(NetworkCoupling.cpp)";
constexpr const char* kBlock   = "CouplingRowBuilder::buildLocalBlock, appendCouplingRows "
                                 "(NetworkCoupling.cpp)";
constexpr const char* kCbDatum = "NetworkSbasis::buildCodeBiasDatumRows (NetworkSbasis.cpp), "
                                 "folded by networkSolveGlue";
constexpr const char* kD2      = "networkInjectD2CarryPrior (NetworkRunContext.cpp)";

constexpr const char* kBlockW = "unit (whitened local block; Sigma = the carried variances and the "
                                "harvested cross covariances; network.phase_leveling_local_block)";
constexpr const char* kCbW    = "NetworkSbasis::kCodeBiasGaugeWeight = 1e3";

// The declaration table, one entry per kind in enum order (a unit test checks the order).
constexpr std::array<ContributionSpec, kContributionKinds> kSpecs = {{
    // --- Station data rows ---
    {CK::DataCode, "data_code", CC::StationRows, CG::BeforeGate, CS::StationLosSignal, kCodeRow,
     "1/sigma^2 of the stochastic model (R_v); / network.gross_los_deweight^2 on a declared gross "
     "line of sight; / network.extra_band_code_sigma_scale^2 outside the IF pin pair",
     kSplit, true},
    {CK::DataCodeCompanion, "data_code_companion", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, kCodeRow,
     "as data_code, / network.second_code_sigma_scale^2", kSplit, true},
    {CK::DataPhase, "data_phase", CC::StationRows, CG::BeforeGate, CS::StationLosSignal, kPhaseRow,
     "1/sigma^2 of the stochastic model (R_v); / network.signal_integrity.unvetted_sigma_scale^2 "
     "on an unvetted line of sight",
     kSplit, true},
    {CK::DataPhaseSecond, "data_phase_second", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, kPhaseRow,
     "as data_phase, / network.second_phase_sigma_scale^2", kSplit, true},
    // --- Station constraint rows ---
    {CK::AmbiguityPin, "ambiguity_pin", CC::StationRows, CG::BeforeGate, CS::StationLosSignal,
     bit(PK::Ambiguity),
     "1e10 (literal, an exact gauge); / the eq34 fresh-pin ramp scale^2 "
     "(network.eq34_fresh_pin_sigma_scale)",
     kPinFold, false},
    {CK::Eq34RpbPin, "eq34_rpb_pin", CC::StationRows, CG::BeforeGate, CS::Station,
     bit(PK::RxPhaseBias), "1e10 (literal, an exact gauge)", kPinFold, false},
    {CK::Eq34RpbRankFloor, "eq34_rpb_rank_floor", CC::StationRows, CG::BeforeGate, CS::Station,
     bit(PK::RxPhaseBias), "1/network.phase_datum_eq34_rpb_prior_sigma_m^2", kPinFold, false},
    {CK::Eq34RpbTimeLink, "eq34_rpb_time_link", CC::StationRows, CG::BeforeGate, CS::Station,
     bit(PK::RxPhaseBias),
     "1/(carried var + network.phase_datum_eq34_rpb_q_m2s * gap)", kPinFold, false},
    {CK::GloIfbAnchor, "glonass_ifb_anchor", CC::StationRows, CG::BeforeGate, CS::Station,
     bit(PK::RxCodeBias), "kGloAnchorWeight = 1.0, 1/(1 m)^2 (function-local constant)",
     kGloFold, false},
    {CK::GloIonoAnchor, "glonass_iono_anchor", CC::StationRows, CG::BeforeGate, CS::StationLos,
     bit(PK::SlantIono), "kGloAnchorWeight = 1.0, 1/(1 m)^2 (function-local constant)",
     kGloFold, false},
    {CK::ShIonoTie, "sh_iono_tie", CC::StationRows, CG::BeforeGate, CS::StationLos,
     bit(PK::SlantIono) | bit(PK::VtecSh),
     "unit (whitened); R from global_vtec_sh.representation_sigma_tecu, the "
     "ionosphere_weighting floor and common-mode sigma, x row_decimation_epochs, / the "
     "station's iono deweight",
     "Application::networkAppendShIonoRows_ (ApplicationNetworkAssembly.cpp)", false},
    {CK::GimIono, "gim_iono", CC::StationRows, CG::BeforeGate, CS::StationLos,
     bit(PK::SlantIono),
     "unit (whitened); R from ionosphere_weighting std_scale x the GIM sigma (floor_m) and "
     "common_mode_sigma_m",
     "Application::networkAppendGimIonoRows_ (ApplicationNetworkAssembly.cpp)", false},
    {CK::SdEwl, "sd_ewl", CC::StationRows, CG::BeforeGate, CS::StationLos,
     bit(PK::Ambiguity) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "unit (whitened); R = diag(arc variance) + the pivot arc's variance (network.sd_ewl)",
     "Application::networkAppendSdEwlRows_ (ApplicationNetworkAssembly.cpp)", false},
    // --- Station block rows ---
    {CK::CouplingScalar, "coupling_row", CC::StationRows, CG::BeforeGate, CS::StationLosSignal,
     kBorder | bit(PK::Ambiguity) | bit(PK::Zwd) | bit(PK::PhaseColour),
     "1/sigma^2: carried variance + departed and time-update terms "
     "(network.carry_conditional_coupling)",
     kCplRows, false},
    {CK::CouplingJoint, "coupling_row_joint", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, kBorder | bit(PK::Ambiguity) | bit(PK::Zwd),
     "unit (whitened jointly; network.carry_coupling_joint_whiten)", kCplRows, false},
    {CK::BlockAmbiguity, "block_ambiguity", CC::StationRows, CG::BeforeGate, CS::StationLosSignal,
     kBlkAmb, kBlockW, kBlock, false},
    {CK::BlockZwd, "block_zwd", CC::StationRows, CG::BeforeGate, CS::Station, kBlkZwd, kBlockW,
     kBlock, false},
    {CK::BlockIono, "block_iono", CC::StationRows, CG::BeforeGate, CS::StationLos, kBlkIono,
     kBlockW, kBlock, false},
    {CK::BlockRxClock, "block_rx_clock", CC::StationRows, CG::BeforeGate, CS::Station, kBlkRxClk,
     kBlockW, kBlock, false},
    {CK::BlockRxCodeBias, "block_rx_code_bias", CC::StationRows, CG::BeforeGate, CS::Station,
     kBlkRxCb, kBlockW, kBlock, false},
    {CK::BlockPhaseColour, "block_phase_colour", CC::StationRows, CG::BeforeGate, CS::StationLos,
     kBlkPc, kBlockW, kBlock, false},
    {CK::PhaseColourBirth, "phase_colour_birth", CC::StationRows, CG::BeforeGate, CS::StationLos,
     bit(PK::PhaseColour),
     "1/sigma(e)^2, the stationary variance (network.phase_colour_sigma_zen_m, elevation floor)",
     kLgmRows, false},
    {CK::PhaseColourBackstop, "phase_colour_backstop", CC::StationRows, CG::BeforeGate,
     CS::StationLos, bit(PK::PhaseColour),
     "1/sigma(e)^2, the stationary variance (network.phase_colour_sigma_zen_m, elevation floor)",
     kLgmRows, false},
    // --- Station priors ---
    {CK::PriorAmbiguity, "prior_ambiguity", CC::StationPriorPostPd, CG::BeforeGate,
     CS::StationLosSignal, bit(PK::Ambiguity),
     "1/(var + network.phase_leveling_q_amb * gap), floored by "
     "network.phase_leveling_var_floor_m2, x the carry VCE factor",
     kCarryWalk, false},
    {CK::PriorZwd, "prior_zwd", CC::StationPriorPostPd, CG::BeforeGate, CS::Station, bit(PK::Zwd),
     "1/(var + network.zwd_carry_q * gap)", kCarryWalk, false},
    {CK::PriorIono, "prior_iono", CC::StationPriorPostPd, CG::BeforeGate, CS::StationLos,
     bit(PK::SlantIono),
     "1/max(var + network.iono_carry_q * gap, network.iono_carry_var_floor)", kCarryWalk, false},
    {CK::PriorRxCodeBias, "prior_rx_code_bias", CC::StationPriorPostPd, CG::BeforeGate,
     CS::Station, bit(PK::RxCodeBias),
     "1/(var + network.rx_cb_carry_q * gap); inject gate k = 10, guard 0.25 (literals)",
     kCarryWalk, false},
    {CK::PriorOraclePin, "prior_oracle_pin", CC::StationPriorPostPd, CG::BeforeGate,
     CS::StationLosSignal, bit(PK::Ambiguity),
     "1/sigma^2 of the oracle_pin event (simulation only)", kCarryWalk, false},
    {CK::PriorRxcbDatum, "prior_rxcb_datum", CC::StationPriorPostPd, CG::BeforeGate, CS::Station,
     bit(PK::RxCodeBias), "1/network.rx_code_bias_datum_sigma_m^2", kCarryWalk, false},
    {CK::PriorRxcbDatumPrePd, "prior_rxcb_datum_pre_pd", CC::StationPriorPrePd, CG::BeforeGate,
     CS::Station, bit(PK::RxCodeBias),
     "1/network.rx_code_bias_datum_sigma_m^2 (network.station_prior_before_pd)", kCarryWalk,
     false},
    // --- Shared rows ---
    {CK::CbIfPin, "cb_if_pin", CC::SharedRows, CG::BeforeGate, CS::Satellite,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbSatGfPair, "cb_sat_gf_pair", CC::SharedRows, CG::BeforeGate, CS::Satellite,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbExtraBandHeld, "cb_extra_band_held", CC::SharedRows, CG::BeforeGate,
     CS::SatelliteSignal, bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbExtraGf, "cb_extra_gf", CC::SharedRows, CG::BeforeGate, CS::SatelliteSignal,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbGfPin, "cb_gf_pin", CC::SharedRows, CG::BeforeGate, CS::Satellite,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbFamilyGf, "cb_family_gf", CC::SharedRows, CG::BeforeGate, CS::Satellite,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbIfLevel, "cb_if_level", CC::SharedRows, CG::BeforeGate, CS::None,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::CbSumToZero, "cb_sum_to_zero", CC::SharedRows, CG::BeforeGate, CS::None,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false},
    {CK::ClockConstraintRows, "clock_constraint_rows", CC::SharedRows, CG::BeforeGate, CS::None,
     bit(PK::SatClock),
     "NetworkSbasis::kClockGaugeWeight = 1e3 (sum-to-zero) or 1/sigma^2 (broadcast-anchored)",
     "NetworkSbasis::buildSharedDatum (NetworkSbasis.cpp), folded by networkSolveGlue", false},
    {CK::SbasisCompletion, "sbasis_completion", CC::SharedRows, CG::BeforeGate, CS::None,
     kBorder, "NetworkSbasis::kSbasisCompleteWeight = 1e3",
     "NetworkSbasis::completeSbasisDimension (NetworkSbasis.cpp), folded by networkSolveGlue",
     false},
    // --- Shared priors ---
    {CK::CbKaula, "cb_kaula", CC::SharedPrior, CG::BeforeGate, CS::SatelliteSignal,
     bit(PK::SatCodeBias), "1/network.cb_kaula_sigma_m^2",
     "NetworkSbasis::buildCodeBiasKaulaPriors (NetworkSbasis.cpp)", false},
    {CK::PbHeal, "pb_heal", CC::SharedPrior, CG::AtGate, CS::SatelliteSignal,
     bit(PK::SatPhaseBias), "info = 1.0 (literal)",
     "healNearNullPhaseBias (NetworkPhaseDatum.cpp)", false},
    {CK::ShPrior, "sh_prior", CC::SharedPrior, CG::BeforeGate, CS::None, bit(PK::VtecSh),
     "the carried SH information, aged by its process noise",
     "networkInjectShVtecPrior (NetworkRunContext.cpp)", false},
    {CK::PhaseHold, "phase_hold", CC::SharedPrior, CG::BeforeGate, CS::SatelliteSignal,
     bit(PK::SatPhaseBias), "1/var of the held phase bias (network.phase_datum_hold_sigma_m)",
     "networkInjectPhaseHoldPrior (NetworkRunContext.cpp)", false},
    {CK::PinBridge, "pin_bridge", CC::SharedPrior, CG::BeforeGate, CS::SatelliteSignal,
     bit(PK::SatPhaseBias), "1/network.phase_datum_bridge_sigma_m^2",
     "networkInjectPinBridgePrior (NetworkRunContext.cpp)", false},
    {CK::BorderCarryDiagonal, "border_carry_diagonal", CC::SharedPrior, CG::AfterGate,
     CS::SatelliteSignal, kBorder,
     "1/(var + q*dt) per key (the registry's q; card Q3's anchored q on the clocks)", kD2, false},
    {CK::BorderCarryGaugeFree, "border_carry_gauge_free", CC::SharedPrior, CG::AfterGate,
     CS::SatelliteSignal, kBorder,
     "as border_carry_diagonal, the per-constellation clock common mode downdated "
     "(network.clock_anchor_gauge_free)",
     kD2, false},
    {CK::BorderCarryDense, "border_carry_dense", CC::SharedDensePrior, CG::AfterGate, CS::None,
     kBorder, "(P_prev + Q)^-1 (network.shared_carry_full_cov)",
     "networkInjectD2CarryPrior (NetworkRunContext.cpp), networkClockCloneDenseFold "
     "(NetworkClockClone.cpp)",
     false},
    {CK::Q3Refold, "q3_refold", CC::SharedDensePrior, CG::AfterSolve, CS::Satellite,
     bit(PK::SatClock),
     "the withdrawn priors' own information, negated or re-folded (network.clock_anchor_resolve)",
     "networkClockAnchorGuard (NetworkClockAnchor.cpp), networkClockCloneRefloat "
     "(NetworkClockClone.cpp)",
     false},
    {CK::ClonePrior, "clone_prior", CC::SharedPrior, CG::AfterGate, CS::SatelliteSignal,
     bit(PK::SatClock) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "1/var of the idle clone's carried value (network.carry_coupling_clock_clone)",
     "networkClockCloneFoldEnd (NetworkClockClone.cpp)", false},
    {CK::CloneWalk, "clone_walk", CC::SharedDensePrior, CG::AfterGate, CS::SatelliteSignal,
     bit(PK::SatClock) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "the walk covariance (network.carry_coupling_clock_clone)",
     "networkClockCloneDenseFold (NetworkClockClone.cpp)", false},
    {CK::SharedHistory, "shared_history", CC::SharedPrior, CG::AfterGate, CS::None, 0,
     "N(0, 1) (literal; the history columns have no registry kind)",
     "networkSharedHistoryFold (NetworkSharedHistory.cpp)", false},
    {CK::ClockOscillator, "clock_oscillator", CC::SharedPrior, CG::AfterGate, CS::Satellite,
     bit(PK::SatClock), "1/p00 of the two-state clock model (network.clock_model)",
     "networkInjectClockOscPrior (NetworkRunContext.cpp)", false},
    {CK::ColdClockAnchor, "cold_clock_anchor", CC::SharedPrior, CG::AfterGate, CS::Satellite,
     bit(PK::SatClock),
     "1/sigma^2, sigma = the a-priori clock sigma when below 100 m (initial_std.sat_clock)",
     "networkSolveGlue, a-priori clock anchor (NetworkSolveGlue.cpp)", false},
    // --- Pins, gauges, DD blocks ---
    {CK::ClockPivotPin, "clock_pivot_pin", CC::Pins, CG::InSolve, CS::Satellite,
     bit(PK::SatClock), "exact (Dirichlet elimination at the reduced normals' scale)",
     "NetworkSbasis::buildSharedDatum (NetworkSbasis.cpp), NetworkEstimator::solve", false},
    {CK::EnsemblePhaseGauge, "ensemble_phase_gauge", CC::Gauges, CG::BeforeGate,
     CS::SatelliteSignal, bit(PK::SatPhaseBias) | bit(PK::Ambiguity),
     "exact (post-reduce Woodbury fold; network.phase_datum_ensemble)",
     "networkBuildEnsembleGauges (NetworkDdPriorBuild.cpp)", false},
    {CK::DdPriorBlock, "dd_prior_block", CC::DdBlocks, CG::InSolve, CS::None, bit(PK::Ambiguity),
     "the carried DD covariance P^z (network.carry_invariant)",
     "networkBuildCarryDdPriors (NetworkDdPriorBuild.cpp)", false},
}};

std::string jsonEscape(const std::string& v) {
    std::string o;
    o.reserve(v.size() + 4);
    for (char ch : v) {
        if (ch == '\\' || ch == '"') o.push_back('\\');
        o.push_back(ch);
    }
    return o;
}

std::string fmtTime(int week, double tow) { return fmt::format("week {} TOW {:.1f}", week, tow); }

std::size_t idx(CK k) { return static_cast<std::size_t>(k); }

void logTable(const std::shared_ptr<spdlog::logger>& log, const char* prefix,
              const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (line.rfind("WARNING", 0) == 0) log->warn("{}: {}", prefix, line);
        else                              log->info("{}: {}", prefix, line);
    }
}

// The run tally as printed: the integrity counters cover every assembled epoch, tallied
// or not (an epoch withheld after PASS B is still checked).
ContributionTally runAsPrinted(const NetworkContributionLedger& L) {
    ContributionTally run = L.whole_run;
    run.untagged_rows          = L.untagged_rows_all;
    run.station_epochs_checked = L.checked_all;
    return run;
}

}  // namespace

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

const std::array<ContributionSpec, kContributionKinds>& contributionSpecs() { return kSpecs; }

const ContributionSpec& contributionSpec(ContributionKind k) {
    const std::size_t i = idx(k);
    return kSpecs[i < kContributionKinds ? i : 0];
}

const char* contributionKindName(ContributionKind k) { return contributionSpec(k).name; }

const char* contributionChannelName(ContributionChannel c) {
    switch (c) {
        case CC::StationRows:        return "station_rows";
        case CC::StationPriorPrePd:  return "station_prior_pre_pd";
        case CC::StationPriorPostPd: return "station_prior_post_pd";
        case CC::SharedRows:         return "shared_rows";
        case CC::SharedPrior:        return "shared_prior";
        case CC::SharedDensePrior:   return "shared_dense_prior";
        case CC::Pins:               return "pins";
        case CC::Gauges:             return "gauges";
        case CC::DdBlocks:           return "dd_blocks";
    }
    return "?";
}

const char* contributionStageName(ContributionStage s) {
    switch (s) {
        case CG::BeforeGate: return "before_gate";
        case CG::AtGate:     return "at_gate";
        case CG::AfterGate:  return "after_gate";
        case CG::InSolve:    return "in_solve";
        case CG::AfterSolve: return "after_solve";
    }
    return "?";
}

const char* contributionSubjectName(ContributionSubject s) {
    switch (s) {
        case CS::StationLosSignal: return "station_los_signal";
        case CS::StationLos:       return "station_los";
        case CS::Station:          return "station";
        case CS::Satellite:        return "satellite";
        case CS::SatelliteSignal:  return "satellite_signal";
        case CS::None:             return "none";
    }
    return "?";
}

bool contributionTouches(const ContributionSpec& spec, NetworkParamKind k) {
    return (spec.params & bit(k)) != 0;
}

std::string paramKindSetText(ParamKindSet set) {
    std::string out;
    for (std::size_t k = 0; k < kNetworkParamKindCount; ++k) {
        if ((set & (ParamKindSet{1} << k)) == 0) continue;
        if (!out.empty()) out += ',';
        out += paramKindName(static_cast<NetworkParamKind>(k));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Row tags
// ---------------------------------------------------------------------------

RowTag makeRowTag(ContributionKind kind, std::string_view sat, std::string_view code) {
    RowTag t;
    t.kind = kind;
    if (!sat.empty()) {
        if (const auto s = SatId::parse(sat)) {
            t.sat = *s;
            if (!code.empty())
                if (const auto g = SignalId::fromObsCode(s->sys, code)) t.signal = *g;
        }
    }
    return t;
}

RowTag makeRowTag(ContributionKind kind, System sys, std::string_view code) {
    RowTag t;
    t.kind = kind;
    if (sys != System::Unknown && !code.empty())
        if (const auto g = SignalId::fromObsCode(sys, code)) t.signal = *g;
    return t;
}

RowTag rxPhaseBiasTag(ContributionKind kind, std::string_view key) {
    if (const auto p = K::parseReceiverPhaseBiasKey(key))
        return makeRowTag(kind, systemFromName(p->system), p->signal);
    return RowTag{kind, {}, {}};
}

RowTag rxCodeBiasTag(ContributionKind kind, std::string_view key) {
    if (const auto p = K::parseReceiverIscKey(key))
        return makeRowTag(kind, systemFromName(p->system), p->signal);
    return RowTag{kind, {}, {}};
}

std::set<std::string> receiverIscSignals(const StateManager& sm) {
    std::set<std::string> isc_sigs;
    for (const auto& [key, idx_] : sm.getCodeBiasStates()) {
        (void)idx_;
        if (const auto p = K::parseReceiverIscKey(key)) isc_sigs.insert(p->signal);
    }
    return isc_sigs;
}

void tagDataRows(std::vector<RowTag>& tags, Eigen::Index m,
                 const std::vector<std::string>& sats,
                 const std::vector<std::string>& sigs,
                 const std::set<std::string>* isc_sigs,
                 const std::set<std::string>& spc_cells) {
    tags.clear();
    tags.reserve(static_cast<std::size_t>(std::max<Eigen::Index>(m, 0)));
    for (Eigen::Index r = 0; r < m; ++r) {
        const std::size_t ru = static_cast<std::size_t>(r);
        if (ru >= sats.size() || ru >= sigs.size()) {
            tags.push_back(RowTag{CK::DataCode, {}, {}});
            continue;
        }
        const std::string& sat = sats[ru];
        const std::string& sig = sigs[ru];
        CK kind = CK::DataCode;
        if (obsTypeOf(sig) == ObsType::Phase) {
            kind = (!sat.empty() && !spc_cells.empty()
                    && spc_cells.count(K::makeSatPhaseBiasKey(sat, sig)))
                       ? CK::DataPhaseSecond : CK::DataPhase;
        } else if (isc_sigs && !sig.empty() && !sat.empty() && isc_sigs->count(sig)) {
            kind = CK::DataCodeCompanion;
        }
        tags.push_back(makeRowTag(kind, sat, sig));
    }
}

std::vector<RowTag> rowTagsAfterJointRewhiten(const std::vector<RowTag>& tags, Eigen::Index nd,
                                              const std::vector<CplRowRaw>& raw,
                                              const std::set<Eigen::Index>& skip,
                                              Eigen::Index nk) {
    std::vector<RowTag> out;
    const std::size_t head = static_cast<std::size_t>(std::max<Eigen::Index>(nd, 0));
    out.reserve(head + static_cast<std::size_t>(std::max<Eigen::Index>(nk, 0)));
    for (std::size_t r = 0; r < head && r < tags.size(); ++r) out.push_back(tags[r]);
    if (nk <= 0) return out;
    // whitenCouplingRows's keep rule: a row survives when its local column is not skipped.
    for (const CplRowRaw& row : raw)
        if (skip.find(row.local_col) == skip.end()) out.push_back(row.tag);
    return out;
}

long long untaggedRows(const StationAssembly& R) {
    const auto gap = [](std::size_t a, std::size_t b) {
        return static_cast<long long>(a > b ? a - b : b - a);
    };
    return gap(static_cast<std::size_t>(R.A_r.rows()), R.row_tags.size())
         + gap(R.carry_cols.size(), R.carry_tags.size())
         + gap(R.prior_cols.size(), R.prior_tags.size());
}

// ---------------------------------------------------------------------------
// Tallies
// ---------------------------------------------------------------------------

void ContributionTally::merge(const ContributionTally& o) {
    const bool empty_self  = epochs == 0 && station_epochs_checked == 0;
    const bool empty_other = o.epochs == 0 && o.station_epochs_checked == 0;
    for (std::size_t k = 0; k < kContributionKinds; ++k) {
        kinds[k].rows  += o.kinds[k].rows;
        kinds[k].folds += o.kinds[k].folds;
    }
    epochs                 += o.epochs;
    station_blocks         += o.station_blocks;
    station_epochs_checked += o.station_epochs_checked;
    untagged_rows          += o.untagged_rows;
    coalesced_entries      += o.coalesced_entries;
    folds_without_kind     += o.folds_without_kind;
    replay_builds          += o.replay_builds;
    replay_folds           += o.replay_folds;
    replay_dim             += o.replay_dim;
    replay_realigned       += o.replay_realigned;
    replay_misaligned      += o.replay_misaligned;
    gim_calls              += o.gim_calls;
    gim_no_gim             += o.gim_no_gim;
    gim_weighting_off      += o.gim_weighting_off;
    if (empty_other) return;
    if (empty_self) {
        week_first = o.week_first;
        tow_first  = o.tow_first;
    }
    week_last = o.week_last;
    tow_last  = o.tow_last;
}

void networkContributionsStationDrain(NetworkContributionLedger& led,
                                      const std::vector<StationAssembly>& asm_res,
                                      const std::vector<int>& est_index_of,
                                      int week, double tow) {
    ContributionTally t;
    t.week_first = t.week_last = week;
    t.tow_first  = t.tow_last  = tow;
    // Kinds present in one station's block (its fold count is one per kind).
    std::array<bool, kContributionKinds> seen{};
    const auto count = [&](const std::vector<RowTag>& tags) {
        for (const RowTag& g : tags) {
            const std::size_t k = idx(g.kind);
            if (k >= kContributionKinds) continue;
            ++t.kinds[k].rows;
            seen[k] = true;
        }
    };
    for (std::size_t i = 0; i < asm_res.size(); ++i) {
        const StationAssembly& R = asm_res[i];
        switch (R.gim_rows) {
            case GimRowsOutcome::NotCalled:    break;
            case GimRowsOutcome::NoGim:        ++t.gim_calls; ++t.gim_no_gim; break;
            case GimRowsOutcome::WeightingOff: ++t.gim_calls; ++t.gim_weighting_off; break;
            case GimRowsOutcome::NoRows:
            case GimRowsOutcome::Appended:     ++t.gim_calls; break;
        }
        if (!R.ok || R.threw) continue;   // never assembled: nothing to check
        ++t.station_epochs_checked;
        t.untagged_rows += untaggedRows(R);
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;   // not taken
        ++t.station_blocks;
        seen.fill(false);
        count(R.row_tags);
        count(R.prior_tags);   // the pre-PD channel folds with addStation
        count(R.prior_merged);
        t.coalesced_entries += static_cast<long long>(R.prior_merged.size());
        if (R.carry_prior_folded) {
            count(R.carry_tags);
            count(R.carry_merged);
            t.coalesced_entries += static_cast<long long>(R.carry_merged.size());
        }
        for (std::size_t k = 0; k < kContributionKinds; ++k)
            if (seen[k]) ++t.kinds[k].folds;
    }
    led.untagged_rows_all += t.untagged_rows;
    led.checked_all       += t.station_epochs_checked;
    led.replay_realigned  = 0;
    led.replay_misaligned = 0;
    led.pending = std::move(t);
}

void networkContributionsEpochTail(NetworkRunContext& ctx, const NetworkEstimator& est,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   NetworkRunTraces& traces, const NetworkConfig& nc,
                                   bool emitted) {
    NetworkContributionLedger& L = ctx.contributions;
    ContributionTally t = std::move(L.pending);
    L.pending = ContributionTally{};
    const SharedFoldLog& f = est.sharedFoldLog();
    for (const SharedFoldRecord& r : f.folds) {
        if (!r.has_kind) { ++t.folds_without_kind; continue; }
        const std::size_t k = idx(r.kind);
        if (k >= kContributionKinds) { ++t.folds_without_kind; continue; }
        t.kinds[k].rows += r.dim;
        ++t.kinds[k].folds;
    }
    if (f.solved && f.pins > 0) {
        t.kinds[idx(CK::ClockPivotPin)].rows += f.pins;
        ++t.kinds[idx(CK::ClockPivotPin)].folds;
    }
    t.replay_builds     = f.replay_builds;
    t.replay_folds      = f.replay_folds;
    t.replay_dim        = f.replay_dim;
    t.replay_realigned  = L.replay_realigned;
    t.replay_misaligned = L.replay_misaligned;
    t.epochs = 1;
    L.whole_run.merge(t);
    L.last_epoch = std::move(t);
    if (L.first_table_written || !emitted || L.last_epoch.station_blocks == 0) return;
    L.first_table_written = true;
    const ContributionTally run = runAsPrinted(L);
    logTable(log, "Network (contributions as armed)",
             contributionsTable(L.last_epoch, run, "first emitted epoch"));
    traces.appendRunManifestRecord(nc, contributionsJson(L.last_epoch, run, "first_emitted_epoch"));
}

void networkContributionsRunEnd(const NetworkRunContext& ctx,
                                const std::shared_ptr<spdlog::logger>& log,
                                NetworkRunTraces& traces, const NetworkConfig& nc) {
    const NetworkContributionLedger& L = ctx.contributions;
    const ContributionTally run = runAsPrinted(L);
    logTable(log, "Network (contributions as armed, run end)",
             contributionsTable(L.last_epoch, run, "run"));
    traces.appendRunManifestRecord(nc, contributionsJson(L.last_epoch, run, "run"));
}

// ---------------------------------------------------------------------------
// The table and the manifest record
// ---------------------------------------------------------------------------

std::vector<std::string> contributionsTable(const ContributionTally& epoch,
                                            const ContributionTally& run,
                                            const std::string& scope) {
    std::vector<std::string> out;
    const bool run_scope = scope == "run";
    out.push_back(fmt::format(
        "{}: epoch columns = {} ({}), {} station block(s) taken; run columns = {} epoch(s) "
        "tallied, {} to {}.  rows: rows of a station block, entries of a diagonal prior, or "
        "the summed dimension of a shared fold; folds: station blocks carrying the kind, or "
        "shared fold calls; the primary build (a within-epoch replay is counted apart)",
        scope, run_scope ? "the last tallied epoch" : "this epoch",
        fmtTime(epoch.week_first, epoch.tow_first), epoch.station_blocks, run.epochs,
        fmtTime(run.week_first, run.tow_first), fmtTime(run.week_last, run.tow_last)));
    const auto line = [](const std::string& kind, const std::string& channel,
                         const std::string& stage, const std::string& re, const std::string& fe,
                         const std::string& rr, const std::string& fr, const std::string& tail) {
        std::string s = fmt::format("{:<24} {:<21} {:<11} {:>10} {:>11} {:>12} {:>10}  {}", kind,
                                    channel, stage, re, fe, rr, fr, tail);
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    };
    out.push_back(line("kind", "channel", "stage", "rows_epoch", "folds_epoch", "rows_run",
                       "folds_run", "weight | builder"));
    for (const ContributionSpec& s : kSpecs) {
        const ContributionCount& ce = epoch.kinds[idx(s.kind)];
        const ContributionCount& cr = run.kinds[idx(s.kind)];
        std::string tail = fmt::format("{} | {}", s.weight, s.builder);
        if (s.kind == CK::GimIono && cr.rows == 0 && run.gim_calls > 0) {
            tail += fmt::format(
                " | note: 0 rows; the builder ran on {} station-epoch(s) of the run and returned "
                "with nothing appended ({} with no GIM loaded, {} with the ionosphere "
                "weighting off)",
                run.gim_calls, run.gim_no_gim, run.gim_weighting_off);
        }
        out.push_back(line(s.name, contributionChannelName(s.channel),
                           contributionStageName(s.stage), std::to_string(ce.rows),
                           std::to_string(ce.folds), std::to_string(cr.rows),
                           std::to_string(cr.folds), tail));
    }
    out.push_back(fmt::format(
        "station rows without a tag: {} ({} station block(s) checked, {}); over the run {} "
        "({} station block(s) checked, every assembled epoch)",
        epoch.untagged_rows, epoch.station_epochs_checked,
        run_scope ? "the last tallied epoch" : "this epoch", run.untagged_rows,
        run.station_epochs_checked));
    out.push_back(fmt::format("shared folds without a kind: {} ({}); over the run {}",
                              epoch.folds_without_kind,
                              run_scope ? "the last tallied epoch" : "this epoch",
                              run.folds_without_kind));
    out.push_back(fmt::format(
        "diagonal prior entries merged into another entry of the same column: {} ({}); over "
        "the run {} (each counted under its own kind above)",
        epoch.coalesced_entries, run_scope ? "the last tallied epoch" : "this epoch",
        run.coalesced_entries));
    out.push_back(fmt::format(
        "within-epoch replay (counted apart from the kinds): {} build(s), {} shared re-fold(s) "
        "of dimension {}, {} station block(s) re-aligned after a joint re-whiten, {} "
        "misaligned ({}); over the run {} build(s), {} re-fold(s) of dimension {}, {} "
        "re-aligned, {} misaligned",
        epoch.replay_builds, epoch.replay_folds, epoch.replay_dim, epoch.replay_realigned,
        epoch.replay_misaligned, run_scope ? "the last tallied epoch" : "this epoch",
        run.replay_builds, run.replay_folds, run.replay_dim, run.replay_realigned,
        run.replay_misaligned));
    return out;
}

std::string contributionsJson(const ContributionTally& epoch, const ContributionTally& run,
                              const std::string& scope) {
    std::string j = fmt::format(
        "{{\"record\":\"contributions\",\"scope\":\"{}\",\"epoch\":{{\"week\":{},\"tow\":{:.3f},"
        "\"station_blocks\":{}}},\"run\":{{\"first\":{{\"week\":{},\"tow\":{:.3f}}},"
        "\"last\":{{\"week\":{},\"tow\":{:.3f}}},\"epochs\":{},\"station_blocks\":{}}},"
        "\"columns\":[\"rows\",\"folds\"],\"kinds\":[",
        jsonEscape(scope), epoch.week_first, epoch.tow_first, epoch.station_blocks,
        run.week_first, run.tow_first, run.week_last, run.tow_last, run.epochs,
        run.station_blocks);
    bool first = true;
    for (const ContributionSpec& s : kSpecs) {
        const ContributionCount& ce = epoch.kinds[idx(s.kind)];
        const ContributionCount& cr = run.kinds[idx(s.kind)];
        std::string params;
        {
            const std::string text = paramKindSetText(s.params);
            std::size_t p = 0;
            while (p < text.size()) {
                const std::size_t q = text.find(',', p);
                const std::string one = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
                params += fmt::format("{}\"{}\"", params.empty() ? "" : ",", one);
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        j += fmt::format(
            "{}{{\"kind\":\"{}\",\"channel\":\"{}\",\"stage\":\"{}\",\"subject\":\"{}\","
            "\"params\":[{}],\"observation\":{},\"weight\":\"{}\",\"builder\":\"{}\","
            "\"epoch\":[{},{}],\"run\":[{},{}]}}",
            first ? "" : ",", s.name, contributionChannelName(s.channel),
            contributionStageName(s.stage), contributionSubjectName(s.subject), params,
            s.is_observation ? "true" : "false", jsonEscape(s.weight), jsonEscape(s.builder),
            ce.rows, ce.folds, cr.rows, cr.folds);
        first = false;
    }
    j += fmt::format(
        "],\"integrity\":{{\"untagged_rows\":[{},{}],\"station_blocks_checked\":[{},{}],"
        "\"folds_without_kind\":[{},{}],\"coalesced_entries\":[{},{}]}},"
        "\"replay\":{{\"builds\":[{},{}],\"folds\":[{},{}],\"dim\":[{},{}],\"realigned\":[{},{}],"
        "\"misaligned\":[{},{}]}},\"gim\":{{\"calls\":[{},{}],\"no_gim\":[{},{}],"
        "\"weighting_off\":[{},{}]}}}}",
        epoch.untagged_rows, run.untagged_rows, epoch.station_epochs_checked,
        run.station_epochs_checked, epoch.folds_without_kind, run.folds_without_kind,
        epoch.coalesced_entries, run.coalesced_entries, epoch.replay_builds, run.replay_builds,
        epoch.replay_folds, run.replay_folds, epoch.replay_dim, run.replay_dim,
        epoch.replay_realigned, run.replay_realigned, epoch.replay_misaligned,
        run.replay_misaligned, epoch.gim_calls, run.gim_calls, epoch.gim_no_gim, run.gim_no_gim,
        epoch.gim_weighting_off, run.gim_weighting_off);
    return j;
}

}  // namespace GnssProcessor
