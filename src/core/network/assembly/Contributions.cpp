/**
 * @file Contributions.cpp
 * @brief The contribution declarations, the row tags' helpers and checks, the tallies and
 *        the "contributions as armed" table (card NOM step N2a).  See
 *        network/assembly/Contributions.hpp.
 */

#include "network/assembly/Contributions.hpp"

#include "network/state/RunContext.hpp"        // NetworkRunContext::contributions
#include "config/GnssConfig.hpp"            // NetworkConfig (the manifest switch)
#include "network/solve/Estimator.hpp"      // NetworkEstimator::sharedFoldLog
#include "network/assembly/ParamRegistry.hpp"  // NetworkParamKind, paramKindName
#include "network/assembly/RowClassify.hpp"    // rowClassOfKind (the unclassified rows)
#include "network/driver/RunTypes.hpp"       // StationAssembly
#include "filter/StateKeySchema.hpp"
#include "network/diag/RunTraces.hpp"     // appendRunManifestRecord

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

namespace K = StateKeys;

// A parameter-kind set is one 32-bit word, one bit per registry kind.
static_assert(kNetworkParamKindCount <= 32,
              "ParamKindSet holds one bit per NetworkParamKind: widen it before adding a 33rd kind");

namespace {

using CK = ContributionKind;
using CC = ContributionChannel;
using CG = ContributionStage;
using CS = ContributionSubject;
using CU = ContributionUnit;
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

constexpr const char* kSplit = "MeasurementModel::computeMeasurementUpdate, networkSplitStation (StationSplit.cpp), station step data_rows";
constexpr const char* kPinFold = "networkPhaseDatumPinRows (PhaseDatum.cpp), station step phase_datum_pins";
constexpr const char* kGloFold = "networkGlonassAnchorRows (StationAnchors.cpp), station step glonass_anchors";
constexpr const char* kLgmRows = "networkPhaseColourRows (PhaseColourCarry.cpp), station step phase_colour_rows";
// The builder column of the station kinds, from the step rows of card NOM3 step PB1b
// (scripts/nom_pb1/rows_pb1b.py, decision 3): the function and file that create the
// kind's rows, and the station step that calls it.
constexpr const char* kSimZeroIono = "networkSimZeroIonoRows (StationAnchors.cpp), station step sim_zero_iono";
constexpr const char* kShIonoRows = "networkAppendShIonoRows (IonoConstraintRows.cpp), station step iono_constraint";
constexpr const char* kGimIonoRows = "networkAppendGimIonoRows (IonoConstraintRows.cpp), station step iono_constraint";
constexpr const char* kSdEwlRows = "networkAppendSdEwlRows (SdEwlAr.cpp), station step sd_ewl";
constexpr const char* kAmbCarryRow = "networkAmbCarryEmitRow (LevelingCarry.cpp), station step ambiguity";
constexpr const char* kZwdCarryRow = "networkZwdCarryPrior (ZwdCarry.cpp), station step zwd";
constexpr const char* kRxCbRows = "networkRxCbPriors (RxCodeBiasCarry.cpp), station step rx_cb";
constexpr const char* kOraclePinRows = "networkOraclePinPriors (OraclePin.cpp), station step oracle_pin";
constexpr const char* kCplRows = "CouplingRowBuilder::buildCplRow, appendCouplingRows "
                                 "(Coupling.cpp)";
constexpr const char* kBlock   = "CouplingRowBuilder::buildLocalBlock, appendCouplingRows "
                                 "(Coupling.cpp)";
constexpr const char* kCbDatum = "NetworkSbasis::buildCodeBiasDatumRows (Sbasis.cpp), "
                                 "folded by networkSolveGlue";
constexpr const char* kD2      = "networkInjectD2CarryPrior (BorderPrior.cpp)";
constexpr const char* kQ3      = "networkClockAnchorGuard (ClockAnchor.cpp), "
                                 "networkClockCloneRefloat (ClockClone.cpp)";

constexpr const char* kBlockW = "unit (whitened local block; Sigma = the carried variances and the "
                                "harvested cross covariances; network.phase_leveling_local_block)";
constexpr const char* kCbW    = "NetworkSbasis::kCodeBiasGaugeWeight = 1e3";
constexpr const char* kQ3Note = "keys handed to each call: the whole captured block on a dense "
                                "refold, the clock group on a gauge-free refold, the dropped "
                                "entries on a diagonal withdrawal, one per walk on the clone path; "
                                "a refold withdraws the tripped clocks' share and re-folds the "
                                "rest in the same call";

// The declaration table, one entry per kind in enum order (checked below at compile time).
// Fields: kind, name, channel, stage, subject, unit, parameter kinds, weight, builder,
// observation, note.
constexpr std::array<ContributionSpec, kContributionKinds> kSpecs = {{
    // --- Station data rows ---
    {CK::DataCode, "data_code", CC::StationRows, CG::BeforeGate, CS::StationLosSignal, CU::Rows,
     kCodeRow,
     "1/sigma^2 of the stochastic model (R_v); / network.gross_los_deweight^2 on a declared gross "
     "line of sight; / network.extra_band_code_sigma_scale^2 outside the IF pin pair",
     kSplit, true, nullptr},
    {CK::DataCodeCompanion, "data_code_companion", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, CU::Rows, kCodeRow,
     "as data_code, / network.second_code_sigma_scale^2", kSplit, true,
     "the second code channel's companion rows and its fallback rows"},
    {CK::DataPhase, "data_phase", CC::StationRows, CG::BeforeGate, CS::StationLosSignal, CU::Rows,
     kPhaseRow,
     "1/sigma^2 of the stochastic model (R_v); / network.signal_integrity.unvetted_sigma_scale^2 "
     "on an unvetted line of sight",
     kSplit, true, nullptr},
    {CK::DataPhaseSecond, "data_phase_second", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, CU::Rows, kPhaseRow,
     "as data_phase, / network.second_phase_sigma_scale^2", kSplit, true, nullptr},
    // --- Station constraint rows ---
    {CK::AmbiguityPin, "ambiguity_pin", CC::StationRows, CG::BeforeGate, CS::StationLosSignal,
     CU::Rows, bit(PK::Ambiguity),
     "1e10 (literal, an exact gauge); / the eq34 fresh-pin ramp scale^2 "
     "(network.eq34_fresh_pin_sigma_scale)",
     kPinFold, false, nullptr},
    {CK::Eq34RpbPin, "eq34_rpb_pin", CC::StationRows, CG::BeforeGate, CS::Station, CU::Rows,
     bit(PK::RxPhaseBias), "1e10 (literal, an exact gauge)", kPinFold, false, nullptr},
    {CK::Eq34RpbRankFloor, "eq34_rpb_rank_floor", CC::StationRows, CG::BeforeGate, CS::Station,
     CU::Rows, bit(PK::RxPhaseBias), "1/network.phase_datum_eq34_rpb_prior_sigma_m^2", kPinFold,
     false, nullptr},
    {CK::Eq34RpbTimeLink, "eq34_rpb_time_link", CC::StationRows, CG::BeforeGate, CS::Station,
     CU::Rows, bit(PK::RxPhaseBias),
     "1/(carried var + network.phase_datum_eq34_rpb_q_m2s * gap)", kPinFold, false, nullptr},
    {CK::GloIfbAnchor, "glonass_ifb_anchor", CC::StationRows, CG::BeforeGate, CS::Station,
     CU::Rows, bit(PK::RxCodeBias),
     "kGloAnchorWeight = 1.0, 1/(1 m)^2 (function-local constant)", kGloFold, false, nullptr},
    {CK::GloIonoAnchor, "glonass_iono_anchor", CC::StationRows, CG::BeforeGate, CS::StationLos,
     CU::Rows, bit(PK::SlantIono),
     "kGloAnchorWeight = 1.0, 1/(1 m)^2 (function-local constant)", kGloFold, false, nullptr},
    {CK::SimZeroIono, "sim_zero_iono", CC::StationRows, CG::BeforeGate, CS::StationLos,
     CU::Rows, bit(PK::SlantIono),
     "1/network.sim_zero_iono_sigma_m^2 (test-only; a fixture simulated without ionosphere)",
     kSimZeroIono, false, nullptr},
    {CK::ShIonoTie, "sh_iono_tie", CC::StationRows, CG::BeforeGate, CS::StationLos, CU::Rows,
     bit(PK::SlantIono) | bit(PK::VtecSh),
     "unit (whitened); R from estimation.global_vtec_sh.representation_sigma_tecu, floored at "
     "estimation.ionosphere_weighting.floor_m, with the common mode "
     "estimation.ionosphere_weighting.common_mode_sigma_m, x "
     "estimation.global_vtec_sh.row_decimation_epochs",
     kShIonoRows, false, nullptr},
    {CK::GimIono, "gim_iono", CC::StationRows, CG::BeforeGate, CS::StationLos, CU::Rows,
     bit(PK::SlantIono),
     "unit (whitened); R from estimation.ionosphere_weighting.std_scale x the GIM sigma, floored "
     "at estimation.ionosphere_weighting.floor_m, with the common mode "
     "estimation.ionosphere_weighting.common_mode_sigma_m",
     kGimIonoRows, false, nullptr},
    {CK::SdEwl, "sd_ewl", CC::StationRows, CG::BeforeGate, CS::StationLos, CU::Rows,
     bit(PK::Ambiguity) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "unit (whitened); R = diag(arc variance) + the pivot arc's variance (network.sd_ewl)",
     kSdEwlRows, false, nullptr},
    // --- Station coupling and block rows (the coupling region) ---
    {CK::CouplingScalar, "coupling_scalar", CC::StationRows, CG::BeforeGate, CS::StationLosSignal,
     CU::Rows, kBorder | bit(PK::Ambiguity) | bit(PK::Zwd) | bit(PK::PhaseColour),
     "1/sigma^2: carried variance + departed and time-update terms "
     "(network.carry_conditional_coupling)",
     kCplRows, false, nullptr},
    {CK::BlockAmbiguityCarry, "block_ambiguity_carry", CC::StationRows, CG::BeforeGate,
     CS::StationLosSignal, CU::Rows, kBlkAmb, kBlockW, kBlock, false, nullptr},
    {CK::BlockZwdCarry, "block_zwd_carry", CC::StationRows, CG::BeforeGate, CS::Station, CU::Rows,
     kBlkZwd, kBlockW, kBlock, false, nullptr},
    {CK::BlockIonoCarry, "block_iono_carry", CC::StationRows, CG::BeforeGate, CS::StationLos,
     CU::Rows, kBlkIono, kBlockW, kBlock, false, nullptr},
    {CK::BlockRxClockCarry, "block_rx_clock_carry", CC::StationRows, CG::BeforeGate, CS::Station,
     CU::Rows, kBlkRxClk, kBlockW, kBlock, false, nullptr},
    {CK::BlockRxCbCarry, "block_rx_cb_carry", CC::StationRows, CG::BeforeGate, CS::Station,
     CU::Rows, kBlkRxCb, kBlockW, kBlock, false, nullptr},
    {CK::BlockPhaseColourCarry, "block_phase_colour_carry", CC::StationRows, CG::BeforeGate,
     CS::StationLos, CU::Rows, kBlkPc, kBlockW, kBlock, false, nullptr},
    // --- Station phase-colour prior rows ---
    {CK::PhaseColourBirth, "phase_colour_birth", CC::StationRows, CG::BeforeGate, CS::StationLos,
     CU::Rows, bit(PK::PhaseColour),
     "1/sigma(e)^2, the stationary variance (network.phase_colour_sigma_zen_m, elevation floor)",
     kLgmRows, false, nullptr},
    {CK::PhaseColourBackstop, "phase_colour_backstop", CC::StationRows, CG::BeforeGate,
     CS::StationLos, CU::Rows, bit(PK::PhaseColour),
     "1/sigma(e)^2, the stationary variance (network.phase_colour_sigma_zen_m, elevation floor)",
     kLgmRows, false, nullptr},
    // --- Station priors ---
    {CK::PriorAmbiguityCarry, "prior_ambiguity_carry", CC::StationPriorPostPd, CG::BeforeGate,
     CS::StationLosSignal, CU::Entries, bit(PK::Ambiguity),
     "1/(var + network.phase_leveling_q_amb * gap), floored by "
     "network.phase_leveling_var_floor_m2, x the carry VCE factor",
     kAmbCarryRow, false, nullptr},
    {CK::PriorZwdCarry, "prior_zwd_carry", CC::StationPriorPostPd, CG::BeforeGate, CS::Station,
     CU::Entries, bit(PK::Zwd), "1/(var + network.zwd_carry_q * gap)", kZwdCarryRow, false, nullptr},
    {CK::PriorRxCbCarry, "prior_rx_cb_carry", CC::StationPriorPostPd, CG::BeforeGate, CS::Station,
     CU::Entries, bit(PK::RxCodeBias),
     "1/(var + network.rx_cb_carry_q * gap); inject gate k = 10, guard 0.25 (literals)",
     kRxCbRows, false, nullptr},
    {CK::PriorAmbiguityOraclePin, "prior_ambiguity_oracle_pin", CC::StationPriorPostPd,
     CG::BeforeGate, CS::StationLosSignal, CU::Entries, bit(PK::Ambiguity),
     "1/sigma^2 of the oracle_pin event (simulation only)", kOraclePinRows, false, nullptr},
    {CK::PriorRxCbDatum, "prior_rx_cb_datum", CC::StationPriorPostPd, CG::BeforeGate,
     CS::Station, CU::Entries, bit(PK::RxCodeBias), "1/network.rx_code_bias_datum_sigma_m^2",
     kRxCbRows, false, nullptr},
    {CK::PriorRxCbDatumPrePd, "prior_rx_cb_datum_pre_pd", CC::StationPriorPrePd, CG::BeforeGate,
     CS::Station, CU::Entries, bit(PK::RxCodeBias),
     "1/network.rx_code_bias_datum_sigma_m^2 (network.station_prior_before_pd)", kRxCbRows,
     false, nullptr},
    // --- Shared rows ---
    {CK::CbIfPin, "cb_if_pin", CC::SharedRows, CG::BeforeGate, CS::Satellite, CU::Rows,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false, nullptr},
    {CK::CbSatGfPair, "cb_sat_gf_pair", CC::SharedRows, CG::BeforeGate, CS::Satellite, CU::Rows,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false, nullptr},
    {CK::CbExtraBandHeld, "cb_extra_band_held", CC::SharedRows, CG::BeforeGate,
     CS::SatelliteSignal, CU::Rows, bit(PK::SatCodeBias), kCbW, kCbDatum, false, nullptr},
    {CK::CbExtraGf, "cb_extra_gf", CC::SharedRows, CG::BeforeGate, CS::SatelliteSignal, CU::Rows,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false, nullptr},
    {CK::CbSumToZero, "cb_sum_to_zero", CC::SharedRows, CG::BeforeGate, CS::None, CU::Rows,
     bit(PK::SatCodeBias), kCbW, kCbDatum, false, nullptr},
    {CK::CbFamilyBirth, "cb_family_birth", CC::SharedRows, CG::BeforeGate, CS::Satellite,
     CU::Rows, bit(PK::SatCodeBias),
     "1/network.code_family_birth_prior_sigma_m^2 (a soft prior, not the gauge weight)",
     "NetworkSbasis::buildCodeBiasDatumRows via codeFamilyBirthPriors (Sbasis.cpp), "
     "folded by NetworkSbasis::foldFamilyBirthRows from networkSolveGlue",
     false,
     "card GFC step 2: one row d_X - d_P = mu per (satellite, birth) on Galileo band 1, "
     "folded once"},
    {CK::ClockConstraintRows, "clock_constraint_rows", CC::SharedRows, CG::BeforeGate, CS::None,
     CU::Rows, bit(PK::SatClock),
     "NetworkSbasis::kClockGaugeWeight = 1e3 (sum-to-zero) or 1/sigma^2 (broadcast-anchored)",
     "NetworkSbasis::buildSharedDatum (Sbasis.cpp), folded by networkSolveGlue", false,
     nullptr},
    {CK::SbasisCompletion, "sbasis_completion", CC::SharedRows, CG::BeforeGate, CS::None, CU::Rows,
     kBorder, "NetworkSbasis::kSbasisCompleteWeight = 1e3",
     "NetworkSbasis::completeSbasisDimension (Sbasis.cpp), folded by networkSolveGlue",
     false, nullptr},
    // --- Shared priors ---
    {CK::CbKaula, "cb_kaula", CC::SharedPrior, CG::BeforeGate, CS::SatelliteSignal, CU::Keys,
     bit(PK::SatCodeBias), "1/network.cb_kaula_sigma_m^2 (network.cb_kaula_prior)",
     "NetworkSbasis::buildCodeBiasKaulaPriors (Sbasis.cpp)", false, nullptr},
    {CK::PbHeal, "pb_heal", CC::SharedPrior, CG::AtGate, CS::SatelliteSignal, CU::Keys,
     bit(PK::SatPhaseBias), "info = 1.0 (literal)",
     "healNearNullPhaseBias (PhaseDatum.cpp)", false, nullptr},
    {CK::ShPrior, "sh_prior", CC::SharedPrior, CG::BeforeGate, CS::None, CU::Keys, bit(PK::VtecSh),
     "the carried SH information, aged by its process noise",
     "networkInjectShVtecPrior (BorderPrior.cpp)", false, nullptr},
    {CK::PinBridge, "pin_bridge", CC::SharedPrior, CG::BeforeGate, CS::SatelliteSignal, CU::Keys,
     bit(PK::SatPhaseBias), "1/network.phase_datum_bridge_sigma_m^2",
     "networkInjectPinBridgePrior (BorderPrior.cpp)", false, nullptr},
    {CK::BorderCarryDiagonal, "border_carry_diagonal", CC::SharedPrior, CG::AfterGate,
     CS::SatelliteSignal, CU::Keys, kBorder,
     "1/(var + q*dt) per key (the registry's q; card Q3's anchored q on the clocks)", kD2, false,
     nullptr},
    {CK::BorderCarryGaugeFree, "border_carry_gauge_free", CC::SharedPrior, CG::AfterGate,
     CS::SatelliteSignal, CU::Keys, kBorder,
     "as border_carry_diagonal, the per-constellation clock common mode downdated "
     "(network.clock_anchor_gauge_free)",
     kD2, false, nullptr},
    {CK::BorderCarryDense, "border_carry_dense", CC::SharedDensePrior, CG::AfterGate, CS::None,
     CU::Keys, kBorder, "(P_prev + Q)^-1 (network.shared_carry_full_cov)",
     "networkInjectD2CarryPrior (BorderPrior.cpp), networkClockCloneDenseFold "
     "(ClockClone.cpp)",
     false, "keys of the dense block, one call per epoch (a retry without the time t-1 states is a second call)"},
    {CK::Q3Withdraw, "q3_withdraw", CC::SharedDensePrior, CG::AfterSolve, CS::Satellite, CU::Keys,
     bit(PK::SatClock),
     "the withdrawn priors' own information, negated (network.clock_anchor_resolve)", kQ3, false,
     kQ3Note},
    {CK::Q3Restore, "q3_restore", CC::SharedDensePrior, CG::AfterSolve, CS::Satellite, CU::Keys,
     bit(PK::SatClock),
     "the withdrawn priors' own information, folded back (network.clock_anchor_resolve)", kQ3,
     false, kQ3Note},
    {CK::ClonePrior, "clone_prior", CC::SharedPrior, CG::AfterGate, CS::SatelliteSignal, CU::Keys,
     bit(PK::SatClock) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "1/var of the idle clone's carried value (network.carry_coupling_clock_clone)",
     "networkClockCloneFoldEnd (ClockClone.cpp)", false, nullptr},
    {CK::CloneWalk, "clone_walk", CC::SharedDensePrior, CG::AfterGate, CS::SatelliteSignal,
     CU::Walks, bit(PK::SatClock) | bit(PK::SatPhaseBias) | bit(PK::SatCodeBias),
     "the walk covariance (network.carry_coupling_clock_clone)",
     "networkClockCloneDenseFold (ClockClone.cpp)", false, nullptr},
    {CK::SharedHistory, "shared_history", CC::SharedPrior, CG::AfterGate, CS::None, CU::Keys, 0,
     "N(0, 1) (literal; the history columns have no registry kind)",
     "networkSharedHistoryFold (SharedHistory.cpp)", false, nullptr},
    {CK::ClockOscillator, "clock_oscillator", CC::SharedPrior, CG::AfterGate, CS::Satellite,
     CU::Keys, bit(PK::SatClock), "1/p00 of the two-state clock model (network.clock_model)",
     "networkInjectClockOscPrior (BorderPrior.cpp)", false, nullptr},
    {CK::ColdClockAnchor, "cold_clock_anchor", CC::SharedPrior, CG::AfterGate, CS::Satellite,
     CU::Keys, bit(PK::SatClock),
     "1/sigma^2, sigma = network.apriori_clock_anchor_sigma_m when set, else 3 m on the "
     "brdc_rts a priori or filter.initial_std.sat_clock; folded when sigma < 100 m, not on the "
     "broadcast-anchored clock datum",
     "networkFoldColdClockAnchor (ClockAnchor.cpp)", false, nullptr},
    // --- Pins, gauges, DD blocks ---
    {CK::ClockPivotPin, "clock_pivot_pin", CC::Pins, CG::InSolve, CS::Satellite, CU::Pins,
     bit(PK::SatClock), "exact (Dirichlet elimination at the reduced normals' scale)",
     "NetworkSbasis::buildSharedDatum (Sbasis.cpp), NetworkEstimator::solve", false,
     "the last primary solve's pins, one fold per epoch"},
    {CK::DdPriorBlock, "dd_prior_block", CC::DdBlocks, CG::InSolve, CS::None, CU::Blocks,
     bit(PK::Ambiguity), "the carried DD covariance P^z (network.carry_invariant)",
     "networkBuildCarryDdPriors (DdPriorBuild.cpp)", false, nullptr},
}};

// The table must list every kind once, in enum order, with its texts: a std::array with a
// missing initializer zero-fills silently (an entry of kind 0 and no name), which this
// rejects at compile time.
constexpr bool specsInEnumOrder() {
    for (std::size_t k = 0; k < kContributionKinds; ++k) {
        const ContributionSpec& s = kSpecs[k];
        if (static_cast<std::size_t>(s.kind) != k) return false;
        if (s.name == nullptr || s.name[0] == '\0') return false;
        if (s.weight == nullptr || s.builder == nullptr) return false;
    }
    return true;
}
static_assert(kSpecs.size() == kContributionKinds,
              "the declaration table has one entry per ContributionKind before Unset");
static_assert(specsInEnumOrder(),
              "kSpecs must list every ContributionKind once, in enum order, with a name, a "
              "weight and a builder");

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

bool isFamily(CK k) { return idx(k) < kContributionKinds; }

void logTable(const std::shared_ptr<spdlog::logger>& log, const char* prefix,
              const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (line.rfind("WARNING", 0) == 0) log->warn("{}: {}", prefix, line);
        else                              log->info("{}: {}", prefix, line);
    }
}

// The run tally as printed: the tag checks cover every assembled epoch, tallied or not (an
// epoch withheld after PASS B is still checked).
ContributionTally runAsPrinted(const NetworkContributionLedger& L) {
    ContributionTally run = L.whole_run;
    run.untagged_rows          = L.untagged_rows_all;
    run.unset_tags             = L.unset_tags_all;
    run.misplaced_rows         = L.misplaced_rows_all;
    run.counter_mismatches     = L.mismatches_all;
    run.unclassified_rows      = L.unclassified_all;
    run.station_epochs_checked = L.checked_all;
    return run;
}

}  // namespace

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

const std::array<ContributionSpec, kContributionKinds>& contributionSpecs() { return kSpecs; }

const ContributionSpec& contributionSpec(ContributionKind k) {
    return kSpecs[isFamily(k) ? idx(k) : 0];
}

const char* contributionKindName(ContributionKind k) {
    return isFamily(k) ? kSpecs[idx(k)].name : "unset";
}

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

const char* contributionUnitName(ContributionUnit u) {
    switch (u) {
        case CU::Rows:    return "rows";
        case CU::Entries: return "entries";
        case CU::Keys:    return "keys";
        case CU::Walks:   return "walks";
        case CU::Pins:    return "pins";
        case CU::Gauges:  return "gauges";
        case CU::Blocks:  return "blocks";
    }
    return "?";
}

bool contributionTouches(const ContributionSpec& spec, NetworkParamKind k) {
    return (spec.params & bit(k)) != 0;
}

bool contributionInCouplingRegion(ContributionKind k) {
    switch (k) {
        case CK::CouplingScalar:
        case CK::BlockAmbiguityCarry:
        case CK::BlockZwdCarry:
        case CK::BlockIonoCarry:
        case CK::BlockRxClockCarry:
        case CK::BlockRxCbCarry:
        case CK::BlockPhaseColourCarry:
            return true;
        default:
            return false;
    }
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

SignalId rxPhaseBiasSignal(std::string_view key) {
    if (const auto p = K::parseReceiverPhaseBiasKey(key))
        if (const auto g = SignalId::fromObsCode(systemFromName(p->system), p->signal)) return *g;
    return SignalId{};
}

SignalId rxCodeBiasSignal(std::string_view key) {
    if (const auto p = K::parseReceiverIscKey(key))
        if (const auto g = SignalId::fromObsCode(systemFromName(p->system), p->signal)) return *g;
    return SignalId{};
}

RowTag rxPhaseBiasTag(ContributionKind kind, std::string_view key) {
    return RowTag{kind, SatId{}, rxPhaseBiasSignal(key)};
}

RowTag rxCodeBiasTag(ContributionKind kind, std::string_view key) {
    return RowTag{kind, SatId{}, rxCodeBiasSignal(key)};
}

void tagDataRows(std::vector<RowTag>& tags, Eigen::Index m,
                 const std::vector<std::string>& sats,
                 const std::vector<std::string>& sigs) {
    tags.clear();
    tags.reserve(static_cast<std::size_t>(std::max<Eigen::Index>(m, 0)));
    for (Eigen::Index r = 0; r < m; ++r) {
        const std::size_t ru = static_cast<std::size_t>(r);
        if (ru >= sats.size() || ru >= sigs.size()) {
            tags.push_back(RowTag{CK::DataCode, {}, {}});
            continue;
        }
        const CK kind = obsTypeOf(sigs[ru]) == ObsType::Phase ? CK::DataPhase : CK::DataCode;
        tags.push_back(makeRowTag(kind, sats[ru], sigs[ru]));
    }
}

long long untaggedRows(const StationAssembly& R) {
    const auto gap = [](std::size_t a, std::size_t b) {
        return static_cast<long long>(a > b ? a - b : b - a);
    };
    return gap(static_cast<std::size_t>(R.A_r.rows()), R.row_tags.size())
         + gap(R.carry_cols.size(), R.carry_tags.size())
         + gap(R.prior_cols.size(), R.prior_tags.size());
}

StationTagCheck checkStationTags(const StationAssembly& R) {
    StationTagCheck c;
    c.untagged = untaggedRows(R);
    std::array<long long, kContributionKinds> n{};   // tags per kind, rows and entries built
    // A diagonal channel: every tag set, of the channel's kind, and counted.
    const auto channel = [&](const std::vector<RowTag>& tags, ContributionChannel want) {
        for (const RowTag& g : tags) {
            if (!isFamily(g.kind)) { ++c.unset; continue; }
            ++n[idx(g.kind)];
            if (kSpecs[idx(g.kind)].channel != want) ++c.misplaced;
        }
    };
    // The rows: the split's data rows carry the observation kinds and no other row does;
    // the coupling region carries its kinds and no other row does; every kind is a row kind.
    const Eigen::Index nd = R.n_data_rows;
    const Eigen::Index cs = R.cpl_row_start;
    const Eigen::Index ce = cs >= 0 ? cs + static_cast<Eigen::Index>(R.cpl_row_local_cols.size())
                                    : -1;
    for (std::size_t r = 0; r < R.row_tags.size(); ++r) {
        const RowTag& g = R.row_tags[r];
        if (!isFamily(g.kind)) { ++c.unset; continue; }
        ++n[idx(g.kind)];
        const ContributionSpec& s = kSpecs[idx(g.kind)];
        const Eigen::Index row = static_cast<Eigen::Index>(r);
        const bool data_row   = row < nd;
        const bool cpl_region = cs >= 0 && row >= cs && row < ce;
        if (s.channel != CC::StationRows || s.is_observation != data_row
            || contributionInCouplingRegion(g.kind) != cpl_region)
            ++c.misplaced;
    }
    // The rows the row classifier files as unclassified (card NOM N2b): a row past the
    // tags, or a tag whose kind maps to obs_other.
    for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const CK k = (static_cast<std::size_t>(row) < R.row_tags.size())
                         ? R.row_tags[static_cast<std::size_t>(row)].kind : CK::Unset;
        if (rowClassOfKind(k, w) == NetRowClass::ObsOther) ++c.unclassified;
    }
    channel(R.carry_tags, CC::StationPriorPostPd);
    channel(R.carry_merged, CC::StationPriorPostPd);
    channel(R.prior_tags, CC::StationPriorPrePd);
    channel(R.prior_merged, CC::StationPriorPrePd);
    // The builders' own counters.
    const auto at = [&](CK k) { return n[idx(k)]; };
    const auto differ = [&](long long tags, long long counter) {
        if (tags != counter) ++c.mismatches;
    };
    differ(at(CK::AmbiguityPin) + at(CK::Eq34RpbPin), R.np);
    differ(at(CK::PhaseColourBirth), R.pc_birth_rows);
    differ(at(CK::PhaseColourBackstop), R.pc_backstop_rows);
    differ(at(CK::BlockAmbiguityCarry) + at(CK::BlockZwdCarry) + at(CK::BlockIonoCarry)
               + at(CK::BlockRxClockCarry) + at(CK::BlockRxCbCarry)
               + at(CK::BlockPhaseColourCarry),
           R.pl_blk_rows);
    differ(at(CK::SdEwl), R.sd_ewl_rows);
    differ(at(CK::PriorAmbiguityOraclePin), R.opin_rows);
    differ(at(CK::PriorRxCbDatum) + at(CK::PriorRxCbDatumPrePd), R.pl_cb_datum_rows);
    return c;
}

// ---------------------------------------------------------------------------
// Tallies
// ---------------------------------------------------------------------------

void ContributionTally::merge(const ContributionTally& o) {
    const bool empty_self = epochs == 0 && station_epochs_checked == 0;
    const bool empty_other = o.epochs == 0 && o.station_epochs_checked == 0;
    for (std::size_t k = 0; k < kContributionKinds; ++k) {
        kinds[k].count += o.kinds[k].count;
        kinds[k].folds += o.kinds[k].folds;
    }
    epochs += o.epochs;
    station_blocks += o.station_blocks;
    station_epochs_checked += o.station_epochs_checked;
    untagged_rows += o.untagged_rows;
    unset_tags += o.unset_tags;
    misplaced_rows += o.misplaced_rows;
    counter_mismatches += o.counter_mismatches;
    unclassified_rows += o.unclassified_rows;
    coalesced_entries += o.coalesced_entries;
    folds_without_kind += o.folds_without_kind;
    replay_builds += o.replay_builds;
    replay_folds += o.replay_folds;
    replay_dim += o.replay_dim;
    gim_calls += o.gim_calls;
    gim_no_gim += o.gim_no_gim;
    gim_weighting_off += o.gim_weighting_off;
    if (empty_other) return;
    if (empty_self) {
        week_first = o.week_first;
        tow_first = o.tow_first;
    }
    week_last = o.week_last;
    tow_last = o.tow_last;
}

void networkContributionsStationDrain(NetworkContributionLedger& led,
                                      const std::vector<StationAssembly>& asm_res,
                                      const std::vector<int>& est_index_of,
                                      int week, double tow) {
    ContributionTally t;
    t.week_first = t.week_last = week;
    t.tow_first = t.tow_last = tow;
    // Kinds present in one station's block (its fold count is one per kind).
    std::array<bool, kContributionKinds> seen{};
    const auto count = [&](const std::vector<RowTag>& tags) {
        for (const RowTag& g : tags) {
            if (!isFamily(g.kind)) continue;   // an unset tag is on the check's line
            ++t.kinds[idx(g.kind)].count;
            seen[idx(g.kind)] = true;
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
        const StationTagCheck chk = checkStationTags(R);
        t.untagged_rows      += chk.untagged;
        t.unset_tags         += chk.unset;
        t.misplaced_rows     += chk.misplaced;
        t.counter_mismatches += chk.mismatches;
        t.unclassified_rows  += chk.unclassified;
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
    led.untagged_rows_all  += t.untagged_rows;
    led.unset_tags_all     += t.unset_tags;
    led.misplaced_rows_all += t.misplaced_rows;
    led.mismatches_all     += t.counter_mismatches;
    led.unclassified_all   += t.unclassified_rows;
    led.checked_all        += t.station_epochs_checked;
    led.pending = std::move(t);
}

void networkContributionsEpochTail(NetworkRun& nrun, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    // The frozen body declares or uses `run`, so this definition names the parameter nrun
    // (specification 3.2).
    NetworkRunContext& ctx = nrun.ctx;
    const NetworkEstimator& est = nrun.engines.est;
    const std::shared_ptr<spdlog::logger>& log = nrun.log;
    NetworkRunTraces& traces = nrun.traces;
    const NetworkConfig& nc = nrun.nc;
    bool emitted = ep.emitted;
    // ---- End of the bindings ----
    NetworkContributionLedger& L = ctx.contributions;
    ContributionTally t = std::move(L.pending);
    L.pending = ContributionTally{};
    const SharedFoldLog& f = est.sharedFoldLog();
    for (const SharedFoldRecord& r : f.folds) {
        if (!isFamily(r.kind)) { ++t.folds_without_kind; continue; }
        t.kinds[idx(r.kind)].count += r.dim;
        ++t.kinds[idx(r.kind)].folds;
    }
    if (f.solved && f.pins > 0) {
        t.kinds[idx(CK::ClockPivotPin)].count += f.pins;
        ++t.kinds[idx(CK::ClockPivotPin)].folds;
    }
    t.replay_builds     = f.replay_builds;
    t.replay_folds      = f.replay_folds;
    t.replay_dim        = f.replay_dim;
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
    const char* epoch_scope = run_scope ? "the last tallied epoch" : "this epoch";
    out.push_back(fmt::format(
        "{}: epoch columns = {} ({}), {} station block(s) taken; run columns = {} epoch(s) "
        "tallied, {} to {}. count: in the kind's unit (rows of a station block or of a shared "
        "constraint fold, entries of a diagonal station prior, keys handed to a shared prior "
        "fold, walks, pins, gauges or DD blocks); folds: station blocks carrying the kind on "
        "the station channels, fold calls on the shared ones; the primary build (a "
        "within-epoch replay is counted apart)",
        scope, epoch_scope, fmtTime(epoch.week_first, epoch.tow_first), epoch.station_blocks,
        run.epochs, fmtTime(run.week_first, run.tow_first), fmtTime(run.week_last, run.tow_last)));
    const auto line = [](const std::string& kind, const std::string& channel,
                         const std::string& stage, const std::string& unit, const std::string& ce,
                         const std::string& fe, const std::string& cr, const std::string& fr,
                         const std::string& tail) {
        std::string s = fmt::format("{:<26} {:<21} {:<11} {:<7} {:>11} {:>11} {:>12} {:>10} {}",
                                    kind, channel, stage, unit, ce, fe, cr, fr, tail);
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    };
    out.push_back(line("kind", "channel", "stage", "unit", "count_epoch", "folds_epoch",
                       "count_run", "folds_run", "weight | builder | note"));
    for (const ContributionSpec& s : kSpecs) {
        const ContributionCount& ce = epoch.kinds[idx(s.kind)];
        const ContributionCount& cr = run.kinds[idx(s.kind)];
        std::string tail = fmt::format("{} | {}", s.weight, s.builder);
        if (s.note) tail += fmt::format(" | note: {}", s.note);
        if (s.kind == CK::GimIono && cr.count == 0 && run.gim_calls > 0) {
            tail += fmt::format(
                " | note: 0 rows; the builder ran on {} station-epoch(s) of the run and returned "
                "with nothing appended ({} with no GIM loaded, {} with the ionosphere "
                "weighting off)",
                run.gim_calls, run.gim_no_gim, run.gim_weighting_off);
        }
        out.push_back(line(s.name, contributionChannelName(s.channel),
                           contributionStageName(s.stage), contributionUnitName(s.unit),
                           std::to_string(ce.count), std::to_string(ce.folds),
                           std::to_string(cr.count), std::to_string(cr.folds), tail));
    }
    out.push_back(fmt::format(
        "station rows without a tag: {} ({} station block(s) checked, {}); over the run {} "
        "({} station block(s) checked, every assembled epoch)",
        epoch.untagged_rows, epoch.station_epochs_checked, epoch_scope, run.untagged_rows,
        run.station_epochs_checked));
    out.push_back(fmt::format(
        "station rows and diagonal entries tagged unset: {} ({}); over the run {}",
        epoch.unset_tags, epoch_scope, run.unset_tags));
    out.push_back(fmt::format(
        "station rows the readers file as unclassified (obs_other: no tag, an unset kind, or "
        "a kind that is not a station row): {} ({}); over the run {}",
        epoch.unclassified_rows, epoch_scope, run.unclassified_rows));
    out.push_back(fmt::format(
        "station rows and entries out of place: {} ({}); over the run {} (the split's data "
        "rows carry the data kinds and no other row does; the coupling region, cpl_row_start "
        "and its coupling rows, carries the coupling and block kinds and no other row does; "
        "every tag's kind belongs to its channel)",
        epoch.misplaced_rows, epoch_scope, run.misplaced_rows));
    out.push_back(fmt::format(
        "tag counts against the builders' counters: {} mismatch(es) ({}); over the run {} "
        "(ambiguity_pin + eq34_rpb_pin = np, phase_colour_birth = pc_birth_rows, "
        "phase_colour_backstop = pc_backstop_rows, the block_*_carry kinds = pl_blk_rows, "
        "sd_ewl = the SD-EWL builder's rows_built, prior_ambiguity_oracle_pin = opin_rows, "
        "prior_rx_cb_datum + prior_rx_cb_datum_pre_pd = pl_cb_datum_rows; per station block)",
        epoch.counter_mismatches, epoch_scope, run.counter_mismatches));
    out.push_back(fmt::format("shared folds without a kind: {} ({}); over the run {}",
                              epoch.folds_without_kind, epoch_scope, run.folds_without_kind));
    out.push_back(fmt::format(
        "diagonal prior entries merged into another entry of the same column: {} ({}); over "
        "the run {} (each counted under its own kind above)",
        epoch.coalesced_entries, epoch_scope, run.coalesced_entries));
    out.push_back(fmt::format(
        "within-epoch replay (counted apart from the kinds): {} build(s), {} shared re-fold(s) "
        "of dimension {} ({}); over the run {} build(s), {} re-fold(s) of dimension {}",
        epoch.replay_builds, epoch.replay_folds, epoch.replay_dim, epoch_scope,
        run.replay_builds, run.replay_folds, run.replay_dim));
    return out;
}

std::string contributionsJson(const ContributionTally& epoch, const ContributionTally& run,
                              const std::string& scope) {
    std::string j = fmt::format(
        "{{\"record\":\"contributions\",\"scope\":\"{}\",\"epoch\":{{\"week\":{},\"tow\":{:.3f},"
        "\"station_blocks\":{}}},\"run\":{{\"first\":{{\"week\":{},\"tow\":{:.3f}}},"
        "\"last\":{{\"week\":{},\"tow\":{:.3f}}},\"epochs\":{},\"station_blocks\":{}}},"
        "\"columns\":[\"count\",\"folds\"],\"kinds\":[",
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
            "\"unit\":\"{}\",\"params\":[{}],\"observation\":{},\"weight\":\"{}\","
            "\"builder\":\"{}\",\"note\":\"{}\",\"epoch\":[{},{}],\"run\":[{},{}]}}",
            first ? "" : ",", s.name, contributionChannelName(s.channel),
            contributionStageName(s.stage), contributionSubjectName(s.subject),
            contributionUnitName(s.unit), params, s.is_observation ? "true" : "false",
            jsonEscape(s.weight), jsonEscape(s.builder), jsonEscape(s.note ? s.note : ""),
            ce.count, ce.folds, cr.count, cr.folds);
        first = false;
    }
    j += fmt::format(
        "],\"integrity\":{{\"untagged_rows\":[{},{}],\"unset_tags\":[{},{}],"
        "\"misplaced_rows\":[{},{}],\"counter_mismatches\":[{},{}],"
        "\"unclassified_rows\":[{},{}],"
        "\"station_blocks_checked\":[{},{}],\"folds_without_kind\":[{},{}],"
        "\"coalesced_entries\":[{},{}]}},"
        "\"replay\":{{\"builds\":[{},{}],\"folds\":[{},{}],\"dim\":[{},{}]}},"
        "\"gim\":{{\"calls\":[{},{}],\"no_gim\":[{},{}],"
        "\"weighting_off\":[{},{}]}}}}",
        epoch.untagged_rows, run.untagged_rows, epoch.unset_tags, run.unset_tags,
        epoch.misplaced_rows, run.misplaced_rows, epoch.counter_mismatches,
        run.counter_mismatches, epoch.unclassified_rows, run.unclassified_rows,
        epoch.station_epochs_checked, run.station_epochs_checked,
        epoch.folds_without_kind, run.folds_without_kind, epoch.coalesced_entries,
        run.coalesced_entries, epoch.replay_builds, run.replay_builds, epoch.replay_folds,
        run.replay_folds, epoch.replay_dim, run.replay_dim, epoch.gim_calls,
        run.gim_calls, epoch.gim_no_gim, run.gim_no_gim, epoch.gim_weighting_off,
        run.gim_weighting_off);
    return j;
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: contributions as armed (NOM N2a).
int networkContributionsRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;

    // Card NOM N2a: every contribution to the normal equations by its declared kind over
    // the run ("contributions as armed", the run's table) and its manifest record.
    networkContributionsRunEnd(ctx, log, traces, config_.getNetworkConfig());
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
