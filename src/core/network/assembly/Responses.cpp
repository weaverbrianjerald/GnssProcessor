/**
 * @file Responses.cpp
 * @brief The declared responses of the carried instances (card PRM phase 3a; see
 *        Responses.hpp).
 */
#include "network/assembly/Responses.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents

#include "network/state/RunContext.hpp"   // NetworkKnobs, NetworkServiceCounters, the carry
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"

#include <array>
#include <cstddef>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "network/runend/RunEnd.hpp"
#include "network/diag/RunTraces.hpp"
#include <fmt/format.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace GnssProcessor {

namespace {

using K = NetworkParamKind;
using E = ParamEvent;
using P = ResponsePolicy;
using R = ResponseSite;
using Src = ResponseCountSource;

constexpr int kNoDisp = -1;
constexpr int dispOf(CarryDisposition d) { return static_cast<int>(d); }

/// The table, in @ref ResponseSite order (checked by the unit test).  The PASS B skip rows
/// (free epoch, gap skip, the jump skip) count the refusals of the fold offered to a solve,
/// including an epoch the A4 ceiling later withholds.
const std::array<ResponseSiteSpec, kResponseSites> kTable{{
    // --- Ambiguity ---------------------------------------------------------------------
    {R::AmbDropLatchSlip, "amb_drop_latch_slip", K::Ambiguity, E::Slip, P::Drop, Src::Increment, kNoDisp,
     "AmbFloat.cpp networkAmbFloatApplyDrops (1)", "arcs whose prior the drop changed: the variance raised to birth strength, or the regression and block row cleared at birth strength (latched by anything but a re-entry response alone)"},
    {R::AmbDropLatchReentry, "amb_drop_latch_reentry", K::Ambiguity, E::Gap, P::Drop, Src::Increment, kNoDisp,
     "AmbFloat.cpp networkAmbFloatApplyDrops (1)", "arcs whose prior the drop changed (latched by gap re-entry or wind-up restart only)"},
    {R::AmbDropStaleGap, "amb_drop_stale_gap", K::Ambiguity, E::Gap, P::Drop, Src::Increment, kNoDisp,
     "AmbFloat.cpp networkAmbFloatApplyDrops (2)", "arcs past the gap horizon whose prior the drop changed"},
    {R::AmbFreeEpochLatch, "amb_free_epoch_latch", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "LevelingCarry.cpp PASS B inject", "carried arcs not folded (latched by anything but a re-entry response alone)"},
    {R::AmbFreeEpochLatchReentry, "amb_free_epoch_latch_reentry", K::Ambiguity, E::Gap, P::FreeEpoch, Src::Increment, kNoDisp,
     "LevelingCarry.cpp PASS B inject", "carried arcs not folded (latched by gap re-entry or wind-up restart only)"},
    {R::AmbPendingReseedSkip, "amb_pending_reseed_skip", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "LevelingCarry.cpp PASS B inject", "carried arcs not folded (a pending-reseed mark at the inject)"},
    {R::AmbT2ReplayFree, "amb_t2_replay_free", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "RaimWithinEpoch.cpp within-epoch replay", "carried arcs whose prior the replay withheld"},
    {R::AmbGapSkip, "amb_gap_skip", K::Ambiguity, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "LevelingCarry.cpp PASS B inject", "carried arcs past the gap horizon not folded"},
    {R::AmbGapPrune, "amb_gap_prune", K::Ambiguity, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "LevelingCarry.cpp harvest prune", "arcs erased"},
    {R::AmbRaimDrop, "amb_raim_drop", K::Ambiguity, E::Slip, P::Drop, Src::Increment, kNoDisp,
     "Raim.cpp attribution (T4) and consensus (T3)", "arcs whose prior the drop changed"},
    {R::AmbRaimErase, "amb_raim_erase", K::Ambiguity, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "Raim.cpp attribution (T4) and consensus (T3)", "arcs erased"},
    {R::AmbDatumSlipErase, "amb_datum_slip_erase", K::Ambiguity, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "SlipQc.cpp datum-station slip re-reference", "arcs erased"},
    {R::AmbGrossErase, "amb_gross_erase", K::Ambiguity, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "GrossLos.cpp the declaration's response", "arcs erased"},
    {R::AmbGrossHold, "amb_gross_hold", K::Ambiguity, E::Decouple, P::Hold, Src::Increment, kNoDisp,
     "GrossLos.cpp the declaration's response", "arcs held"},
    {R::AmbWithholdErase, "amb_withhold_erase", K::Ambiguity, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "RaimWithinEpoch.cpp a withheld epoch's free response", "arcs erased"},
    {R::AmbAnchorRerefErase, "amb_anchor_reref_erase", K::Ambiguity, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "PhaseDatum.cpp networkApplyPhaseAnchorCarryReref", "arcs erased"},
    {R::AmbEq34Erase, "amb_eq34_erase", K::Ambiguity, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "ChainDatum.cpp eq34 re-reference", "arcs erased"},
    // --- Slant ionosphere --------------------------------------------------------------
    {R::IonoRaimErase, "iono_raim_erase", K::SlantIono, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "Raim.cpp attribution (T4)", "carried ionospheres erased"},
    {R::IonoGrossErase, "iono_gross_erase", K::SlantIono, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "GrossLos.cpp the declaration's response", "carried ionospheres erased"},
    {R::IonoGrossHold, "iono_gross_hold", K::SlantIono, E::Decouple, P::Hold, Src::Increment, kNoDisp,
     "GrossLos.cpp the declaration's response (bounded)", "hold declarations (a diagonal carry's hold acts only while a carried level is held)"},
    {R::IonoGapSkip, "iono_gap_skip", K::SlantIono, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "SlantIonoCarry.cpp PASS B in-block inject", "carried ionospheres not folded"},
    {R::IonoGapPrune, "iono_gap_prune", K::SlantIono, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "SlantIonoCarry.cpp harvest prune (block carry)", "carried ionospheres erased"},
    // --- Phase colour ------------------------------------------------------------------
    {R::PcRaimErase, "pc_raim_erase", K::PhaseColour, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "Raim.cpp attribution (T4)", "phase colours erased"},
    {R::PcGrossErase, "pc_gross_erase", K::PhaseColour, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "GrossLos.cpp the declaration's response", "phase colours erased"},
    {R::PcGapPrune, "pc_gap_prune", K::PhaseColour, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "PhaseColourCarry.cpp harvest prune", "phase colours erased"},
    // --- Receiver code bias ------------------------------------------------------------
    {R::RxcbStepErase, "rxcb_step_erase", K::RxCodeBias, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "RxCodeBiasCarry.cpp in-block harvest step test", "receiver code biases erased"},
    {R::RxcbJumpSkip, "rxcb_jump_skip", K::RxCodeBias, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "RxCodeBiasCarry.cpp diagonal route innovation test", "receiver code biases not folded"},
    {R::RxcbGapSkip, "rxcb_gap_skip", K::RxCodeBias, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "RxCodeBiasCarry.cpp PASS B inject gap gates (block and diagonal)", "receiver code biases not folded"},
    {R::RxcbGapPrune, "rxcb_gap_prune", K::RxCodeBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "RxCodeBiasCarry.cpp harvest prune", "receiver code biases erased"},
    // --- Receiver clock ----------------------------------------------------------------
    {R::RxclkJumpErase, "rxclk_jump_erase", K::RxClock, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "RxClockCarry.cpp in-block harvest prediction test", "receiver clocks erased"},
    {R::RxclkGapSkip, "rxclk_gap_skip", K::RxClock, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "RxClockCarry.cpp PASS B inject (block)", "receiver clocks not folded"},
    {R::RxclkGapPrune, "rxclk_gap_prune", K::RxClock, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "RxClockCarry.cpp harvest prune", "receiver clocks erased"},
    // --- Receiver phase bias -----------------------------------------------------------
    {R::RpbEq34Erase, "rpb_eq34_erase", K::RxPhaseBias, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "ChainDatum.cpp eq34 re-reference", "receiver phase biases erased"},
    {R::RpbGapSkip, "rpb_gap_skip", K::RxPhaseBias, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "PhaseDatum.cpp the eq34 receiver phase-bias gap gate", "receiver phase biases not folded"},
    {R::RpbGapPrune, "rpb_gap_prune", K::RxPhaseBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "ChainDatum.cpp harvest prune", "receiver phase biases erased"},
    // --- Wet delay ---------------------------------------------------------------------
    {R::ZwdGapSkip, "zwd_gap_skip", K::Zwd, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ZwdCarry.cpp PASS B inject gap gates", "wet delays not folded"},
    {R::ZwdGapPrune, "zwd_gap_prune", K::Zwd, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "ZwdCarry.cpp harvest prune", "wet delays erased"},
    // --- Satellite clock ---------------------------------------------------------------
    {R::ClkDeparted, "clk_departed", K::SatClock, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "BorderPrior.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::ClkPark, "clk_park", K::SatClock, E::Gap, P::Park, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT park", "clocks parked"},
    {R::ClkParkExpire, "clk_park_expire", K::SatClock, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT expiry", "parked clocks dropped"},
    {R::ClkSeed, "clk_seed", K::SatClock, E::Readmit, P::SeedAtVarPlusQGap, Src::Disposition,
     dispOf(CarryDisposition::Seeded), "CarryDisposition.cpp carryFoldDecide (Seeded)", "clocks seeded"},
    {R::ClkParkDiscard, "clk_park_discard", K::SatClock, E::Readmit, P::Discard, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT (column back, not consumed)", "parked clocks dropped"},
    {R::ClkParkDiscardDatum, "clk_park_discard_datum", K::SatClock, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "BorderPrior.cpp READMIT frame drop at a clock-datum change", "parked clocks dropped"},
    {R::ClkReinit, "clk_reinit", K::SatClock, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "CarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::ClkRetire, "clk_retire", K::SatClock, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "BorderPrior.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::ClkAnchorDecouple, "clk_anchor_decouple", K::SatClock, E::Decouple, P::Loosen, Src::Increment, kNoDisp,
     "ClockAnchor.cpp the guard", "anchored-to-loose edges"},
    {R::ClkAnchorRecouple, "clk_anchor_recouple", K::SatClock, E::Readmit, P::Recouple, Src::Increment, kNoDisp,
     "ClockAnchor.cpp the guard", "loose-to-anchored edges after a decouple (first engagements excluded)"},
    {R::ClkOscWipe, "clk_osc_wipe", K::SatClock, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "ClockQc.cpp the clock oscillator at a clock-datum change", "oscillator states erased"},
    {R::ClkOscReinit, "clk_osc_reinit", K::SatClock, E::Slip, P::Reinit, Src::Increment, kNoDisp,
     "ClockQc.cpp the clock oscillator's fault response", "oscillator states re-initialized"},
    {R::ClkOscGapReset, "clk_osc_gap_reset", K::SatClock, E::Gap, P::Reinit, Src::Increment, kNoDisp,
     "ClockQc.cpp the clock oscillator past its maximum gap", "oscillator drifts reset"},
    // --- Satellite code bias -----------------------------------------------------------
    {R::CbDeparted, "cb_departed", K::SatCodeBias, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "BorderPrior.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::CbPark, "cb_park", K::SatCodeBias, E::Gap, P::Park, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT park", "code biases parked"},
    {R::CbParkExpire, "cb_park_expire", K::SatCodeBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT expiry", "parked code biases dropped"},
    {R::CbSeed, "cb_seed", K::SatCodeBias, E::Readmit, P::SeedAtVarPlusQGap, Src::Disposition,
     dispOf(CarryDisposition::Seeded), "CarryDisposition.cpp carryFoldDecide (Seeded)", "code biases seeded"},
    {R::CbParkDiscard, "cb_park_discard", K::SatCodeBias, E::Readmit, P::Discard, Src::Increment, kNoDisp,
     "BorderStash.cpp READMIT (column back, not consumed)", "parked code biases dropped"},
    {R::CbParkDiscardDatum, "cb_park_discard_datum", K::SatCodeBias, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "BorderPrior.cpp READMIT frame drop at a clock-datum change", "parked code biases dropped"},
    {R::CbReinit, "cb_reinit", K::SatCodeBias, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "CarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::CbRetire, "cb_retire", K::SatCodeBias, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "BorderPrior.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::CbTimeLinkReanchor, "cb_time_link_reanchor", K::SatCodeBias, E::Readmit, P::ReanchorAtTarget, Src::Increment, kNoDisp,
     "Sbasis.cpp the geometry-free time link", "GF datum rows re-anchored at a satellite absent at the last kept epoch (row-epochs)"},
    // --- Satellite phase bias ----------------------------------------------------------
    {R::PbDeparted, "pb_departed", K::SatPhaseBias, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "BorderPrior.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::PbReref, "pb_reref", K::SatPhaseBias, E::Handover, P::Rereference, Src::Disposition,
     dispOf(CarryDisposition::Reref), "CarryDisposition.cpp carryFoldDecide (Reref)", "key-epochs not folded"},
    {R::PbReinit, "pb_reinit", K::SatPhaseBias, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "CarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::PbRetire, "pb_retire", K::SatPhaseBias, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "BorderPrior.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::PbPinBridgeDiscard, "pb_pin_bridge_discard", K::SatPhaseBias, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "AnchorHygiene.cpp a pin bridge at a datum change", "bridges dropped"},
    // --- SH coefficient ----------------------------------------------------------------
    {R::ShRetire, "sh_retire", K::VtecSh, E::Handover, P::Retire, Src::Increment, kNoDisp,
     "BorderPrior.cpp the SH prior at a clock-datum change", "SH key-epochs not folded"},
}};

}  // namespace

const char* paramEventName(ParamEvent e) {
    switch (e) {
        case ParamEvent::Slip:     return "slip";
        case ParamEvent::Gap:      return "gap";
        case ParamEvent::Handover: return "handover";
        case ParamEvent::Decouple: return "decouple";
        case ParamEvent::Readmit:  return "readmit";
    }
    return "?";
}

const char* responsePolicyName(ResponsePolicy p) {
    switch (p) {
        case ResponsePolicy::Drop:              return "drop";
        case ResponsePolicy::EraseAndRebirth:   return "erase_and_rebirth";
        case ResponsePolicy::FreeEpoch:         return "free_epoch";
        case ResponsePolicy::GapSkip:           return "gap_skip";
        case ResponsePolicy::GapPrune:          return "gap_prune";
        case ResponsePolicy::Marginalize:       return "marginalize";
        case ResponsePolicy::Rereference:       return "rereference";
        case ResponsePolicy::Retire:            return "retire";
        case ResponsePolicy::Stransform:        return "stransform";
        case ResponsePolicy::Park:              return "park";
        case ResponsePolicy::SeedAtVarPlusQGap: return "seed_at_var_plus_q_gap";
        case ResponsePolicy::Discard:           return "discard";
        case ResponsePolicy::Loosen:            return "loosen";
        case ResponsePolicy::Recouple:          return "recouple";
        case ResponsePolicy::Reinit:            return "reinit";
        case ResponsePolicy::Hold:              return "hold";
        case ResponsePolicy::ReanchorAtTarget:  return "reanchor_at_target";
    }
    return "?";
}

const ResponseSiteSpec& responseSiteSpec(ResponseSite s) {
    return kTable[static_cast<std::size_t>(s)];
}

bool responseSiteArmed(ResponseSite s, const NetworkKnobs& k, const GnssConfig& config) {
    const NetworkConfig& nc = config.getNetworkConfig();
    const bool readmit = k.readmit_seed_horizon_s > 0.0;
    // inflate_q (a soft reinit) needs a positive reinit sigma and a clock refloat flag: the
    // clock monitor (its RAIM couple implies it) or the clock model's fault test.  The
    // clock itself is stashed only off the clock model.
    const bool reinitSoft = k.clk_mon_reinit_sig > 0.0;
    const bool reinitFlags = k.clk_mon_on || k.clk_model_on;
    const bool cbEst = config.estimateNetworkSatCodeBias();
    const bool clkCarried = !k.clk_model_on;
    const bool pbCarried = !k.carry_invariant;   // not stashed under the DD carry
    // A clock-pivot datum change is possible only with a pivot clock datum.
    const bool pivotDatum = !k.clock_sum_to_zero && !k.clock_brdc_anchored;
    const bool relevelFree = config.networkCycleSlipRelevelFreeEpoch();
    const bool reentry = nc.slip_qc.gap_reentry || config.networkCycleSlipWindupRestartRefloat();
    // The leveling vehicle's local inject (carry_warm implies phase_leveling; the column is
    // carried unless carry_invariant).
    const bool levelInject = k.pl_on && !k.carry_invariant && !k.amb_float_kf;
    // RAIM's T4 block: entered on the attribution's action, or on an event-harness arc erase
    // in an erase mode (network.inject, arc_erase).
    bool injectArcErase = false, injectFreeEpoch = false;
    if (nc.inject.enable)
        for (const auto& ev : nc.inject.events) {
            if (ev.kind != "arc_erase") continue;
            if (ev.mode == "free_epoch") injectFreeEpoch = true;
            else                         injectArcErase = true;
        }
    // A latch with a slip-detector cause (MW, LLI, IF-TD) or the harness's free-epoch tag.
    const bool slipLatch = config.networkCycleSlipMwDetect() || config.networkCycleSlipIfPhaseTd()
                           || injectFreeEpoch;
    // local_carry is structurally empty under carry_invariant (the free-ambiguity lineage).
    const bool ambCarried = k.pl_on && !k.carry_invariant;
    const bool t4 = config.networkRaimAttribution() || injectArcErase;
    switch (s) {
        // The border's fold dispositions (carryFoldDecide and the fold's Retired clause).
        case R::ClkDeparted: return clkCarried;                 // a column can always leave
        case R::CbDeparted:  return cbEst;
        case R::PbDeparted:  return pbCarried;
        case R::ClkSeed:     return readmit && clkCarried;
        case R::CbSeed:      return readmit && k.readmit_seed_cb && cbEst;
        // inflate_q: a clock refloat flag (the clock monitor, or RAIM's and the within-epoch
        // replay's satellite culprits under clock_monitor_couple_raim) on all three kinds,
        // stashed soft only with a positive reinit sigma; and the code-bias excursion guard.
        case R::ClkReinit:   return k.clk_mon_on && clkCarried && reinitSoft;
        case R::PbReinit:    return reinitFlags && reinitSoft && pbCarried;
        case R::CbReinit:
            return cbEst && ((reinitFlags && reinitSoft) || nc.cb_excursion_reinit);
        // The retired datum: a clock-pivot handover whose carry the S-transform did not
        // re-express (scoped mode), or every entry at a handover in legacy (unscoped) mode.
        case R::ClkRetire:   return pivotDatum && clkCarried;
        case R::CbRetire:    return pivotDatum && cbEst;
        case R::PbRetire:    return pivotDatum && pbCarried;
        case R::PbReref:
            return pbCarried
                && ((k.pa_carry_reref && (!k.pd_held_basis || k.pd_held_cold_release))
                    || k.phase_datum_eq34);

        // --- The ambiguity: the float vehicle's drop, the leveling vehicle's skips.  The
        // latch exists only under relevel_free_epoch (the loader disarms it without
        // phase_leveling and under carry_invariant); float_kf implies phase_leveling.
        case R::AmbDropLatchSlip:     return k.amb_float_kf && relevelFree && slipLatch;
        case R::AmbDropLatchReentry:  return k.amb_float_kf && relevelFree && reentry;
        case R::AmbDropStaleGap:      return k.amb_float_kf;
        case R::AmbFreeEpochLatch:    return levelInject && relevelFree && slipLatch;
        case R::AmbFreeEpochLatchReentry: return levelInject && relevelFree && reentry;
        case R::AmbPendingReseedSkip: return k.pl_on && !k.carry_invariant;
        case R::AmbT2ReplayFree:
            return k.pl_on && !k.carry_invariant && config.networkRaimWithinEpoch();
        case R::AmbGapSkip:           return levelInject;
        case R::AmbGapPrune:          return ambCarried;
        case R::AmbRaimDrop:          return k.raim_on && k.pl_on && k.amb_float_kf;
        case R::AmbRaimErase:         return k.raim_on && ambCarried && !k.amb_float_kf;
        case R::AmbDatumSlipErase:    return k.datum_station_detect && k.pl_on;   // forced off without mw_detect and under the prioritized datum
        case R::AmbGrossErase:        return k.gl_on && !k.gl_bound && ambCarried;
        case R::AmbGrossHold:         return k.gl_on && k.gl_bound && ambCarried;
        case R::AmbWithholdErase:     return k.wh_free_erase && k.raim_withhold > 0.0 && k.pl_on;
        case R::AmbAnchorRerefErase:
            return k.pl_on && k.pa_carry_reref && (!k.pd_held_basis || k.pd_held_cold_release);
        case R::AmbEq34Erase:         return k.phase_datum_eq34 && ambCarried;
        // --- The slant ionosphere and the phase colour (erased with their line of sight).
        case R::IonoRaimErase:        return k.raim_on && k.pl_on && t4 && k.iono_blk;
        case R::IonoGrossErase:       return k.gl_on && !k.gl_bound && k.iono_blk;
        case R::IonoGrossHold:        return k.gl_bound && k.iono_blk;
        case R::IonoGapSkip:
            return k.pl_on && k.iono_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::IonoGapPrune:
            return k.pl_on && k.iono_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::PcRaimErase:          return k.raim_on && k.pl_on && t4 && k.pc_on;
        case R::PcGrossErase:         return k.gl_on && !k.gl_bound && k.pc_on;
        case R::PcGapPrune:           return k.pl_on && k.pc_on;
        // --- The receiver-side carries.
        case R::RxcbStepErase:
            return k.pl_on && k.rx_cb_on && k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::RxcbJumpSkip:
            return k.rx_cb_on
                && !(k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond);
        case R::RxcbGapSkip:   // the in-block route, or the diagonal route when the block is not armed
            return k.rx_cb_on
                && ((k.pl_on && !k.carry_invariant)
                    || !(k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond));
        case R::RxcbGapPrune:         return k.rx_cb_on;
        case R::RxclkJumpErase:
        case R::RxclkGapPrune:
            return k.pl_on && k.rxclk_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::RxclkGapSkip:
            return k.pl_on && k.rxclk_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::RpbEq34Erase:
            return k.phase_datum_eq34 && k.pl_on && k.eq34_rpb_q > 0.0;
        case R::RpbGapSkip:           return k.pl_on && k.eq34_rpb_q > 0.0;
        case R::RpbGapPrune:          return k.pl_on && k.eq34_rpb_q > 0.0;
        case R::ZwdGapSkip:           return k.pl_on && (k.zwd_on || k.zwd_seam_on);
        case R::ZwdGapPrune:          return k.pl_on && (k.zwd_on || k.zwd_capture || k.zwd_seam_on);
        // --- The border's READMIT, anchor and oscillator responses.
        case R::ClkPark:
        case R::ClkParkExpire:
        case R::ClkParkDiscard:       return readmit && clkCarried;
        case R::ClkParkDiscardDatum:  return readmit && clkCarried && pivotDatum;
        case R::CbPark:
        case R::CbParkExpire:
        case R::CbParkDiscard:        return readmit && k.readmit_seed_cb && cbEst;
        case R::CbParkDiscardDatum:   return readmit && k.readmit_seed_cb && cbEst && pivotDatum;
        case R::ClkAnchorDecouple:
        case R::ClkAnchorRecouple:    return k.cta_on;
        case R::ClkOscWipe:           return k.clk_model_on && pivotDatum;
        case R::ClkOscReinit:
        case R::ClkOscGapReset:       return k.clk_model_on;
        case R::CbTimeLinkReanchor:
            return config.estimateNetworkSatCodeBias() && k.cb_time_linked
                && config.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode
                && config.getNetworkConfig().sat_gf_datum_time_linked;
        case R::PbPinBridgeDiscard:   return k.pin_bridge_on && pivotDatum;
        case R::ShRetire:             return config.estimateNetworkIonoSh() && pivotDatum;
    }
    return false;
}

bool carryMechanismArmed(CarryMechanism m, const NetworkKnobs& k, const GnssConfig& config) {
    switch (m) {
        case CarryMechanism::EphRelevel:
            return config.networkAprioriSource() == NetworkAprioriSource::BrdcRts;
        case CarryMechanism::Handover:           return k.handover_stransform;
        case CarryMechanism::HandoverBeforeRows: return k.handover_stransform_before_rows;
        // The D4 shift also runs for the seed-round re-host's frame-shiftable edges.
        case CarryMechanism::Eq34Shift:
            return k.phase_datum_eq34 && (k.eq34_shift_on || k.eq34_seed_round_rehost);
        // Called only for a continuous handover of a re-referenced family.
        case CarryMechanism::DeclaredHandover:
            return k.amb_float_kf && k.amb_float_declared_handover && k.pa_carry_reref
                && k.pd_handover_continuous;
    }
    return false;
}

std::uint32_t carryMechanismBorderKinds(CarryMechanism m, const NetworkKnobs& k) {
    const auto bit = [](SharedParamKind x) { return std::uint32_t{1} << static_cast<int>(x); };
    switch (m) {
        case CarryMechanism::EphRelevel:
        case CarryMechanism::Handover:
        case CarryMechanism::HandoverBeforeRows: return bit(SharedParamKind::Clock);
        case CarryMechanism::Eq34Shift:
        case CarryMechanism::DeclaredHandover:   return bit(SharedParamKind::PhaseBias);
    }
    return 0u;
}

bool carrySiteLocalKind(CarrySite s, NetworkParamKind& kind) {
    switch (s) {
        case CarrySite::AmbiguityValue:
        case CarrySite::HoldAmbiguityValue: kind = NetworkParamKind::Ambiguity;   return true;
        case CarrySite::RxClockValue:       kind = NetworkParamKind::RxClock;     return true;
        case CarrySite::RxPhaseBiasValue:   kind = NetworkParamKind::RxPhaseBias; return true;
        case CarrySite::RxCodeBiasValue:    kind = NetworkParamKind::RxCodeBias;  return true;
        default:                            return false;
    }
}

int responseBorderIndex(NetworkParamKind kind) {
    switch (kind) {
        case NetworkParamKind::SatClock:     return static_cast<int>(SharedParamKind::Clock);
        case NetworkParamKind::SatCodeBias:  return static_cast<int>(SharedParamKind::CodeBias);
        case NetworkParamKind::SatPhaseBias: return static_cast<int>(SharedParamKind::PhaseBias);
        case NetworkParamKind::VtecSh:       return static_cast<int>(SharedParamKind::VtecSh);
        default:                             return -1;
    }
}

long long responseSiteCount(const NetworkServiceCounters& C, ResponseSite s) {
    const ResponseSiteSpec& rs = responseSiteSpec(s);
    const int b = responseBorderIndex(rs.kind);
    switch (rs.source) {
        case ResponseCountSource::Increment:   return C.prm_resp[static_cast<int>(s)];
        case ResponseCountSource::Disposition:
            return (b >= 0 && rs.disposition >= 0) ? C.prm_disp[b][rs.disposition] : 0;
        case ResponseCountSource::Departed:    return b >= 0 ? C.prm_departed[b] : 0;
    }
    return 0;
}

void networkResponseLatchTally(NetworkRunContext& ctx, const EpochEvents& events) {
    const RelevelEpochState& lat = events.slip.relevel;
    if (lat.free_ambs.empty()) return;
    auto& C = ctx.counters;
    ++C.prm_latch_epochs;
    for (const auto& key : lat.free_ambs) {
        const auto parts = StateKeys::parseAmbiguityKey(key.second);
        const bool carried = parts
            && (net::getArc(ctx.entities.arcs, std::make_tuple(key.first, parts->satellite,
                                                           parts->signal), &net::ArcState::amb) != nullptr) > 0;
        const auto c = lat.free_cause.find(key);
        const unsigned cause = (c != lat.free_cause.end()) ? c->second : 0u;
        ++C.prm_latch_unique;
        if (carried) ++C.prm_latch_unique_carried;
        int nbits = 0;
        for (int b = 0; b < kLatchCauses - 1; ++b) {
            if (!(cause & (1u << b))) continue;
            ++nbits;
            ++C.prm_latch_keys[b];
            if (carried) ++C.prm_latch_carried[b];
        }
        if (nbits == 0) {
            ++C.prm_latch_keys[kLatchCauses - 1];
            if (carried) ++C.prm_latch_carried[kLatchCauses - 1];
        }
        if (nbits > 1) ++C.prm_latch_multi;
    }
}

const char* latchCauseName(int i) {
    static const char* const kNames[kLatchCauses] = {
        "mw", "lli", "if_td", "t4_attrib", "t3_consensus", "windup", "gap_reentry", "gf_jump",
        "none"};
    return (i >= 0 && i < kLatchCauses) ? kNames[i] : "?";
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the declared responses of the carried instances (PRM 3a).
int networkResponsesRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;

    // Card PRM phase 3a (section 4.5): the responses of the carried instances over the run,
    // one row per declared site (kind, event, policy) with its count; the Stransform
    // responses of the datum mechanisms, from the traversal's moved values per kind; the
    // free-epoch latch per cause (card RF-1 section 1.5's untraced latches); and the
    // declaration check: a site that responded on a run where it is not declared armed.  The
    // same rows go to the run manifest as a `param_responses` record.
    const auto& C = ctx.counters;
    long long n_undeclared = 0, n_armed = 0, n_quiet = 0;
    std::string jr;
    for (int i = 0; i < kResponseSites; ++i) {
        const auto site = static_cast<ResponseSite>(i);
        const ResponseSiteSpec& rs = responseSiteSpec(site);
        const bool armed = responseSiteArmed(site, ctx.knobs, config_);
        const long long n = responseSiteCount(C, site);
        n_armed += armed ? 1 : 0;
        jr += fmt::format("{}{{\"site\":\"{}\",\"kind\":\"{}\",\"event\":\"{}\","
                          "\"policy\":\"{}\",\"armed\":{},\"count\":{}}}",
                          jr.empty() ? "" : ",", rs.name, paramKindName(rs.kind),
                          paramEventName(rs.event), responsePolicyName(rs.policy),
                          armed ? "true" : "false", n);
        if (!armed && n == 0) { ++n_quiet; continue; }
        log->info("Network (param responses, run end): {} {} -> {} at {}: {} {} ({}; {}).",
                  paramKindName(rs.kind), paramEventName(rs.event),
                  responsePolicyName(rs.policy), rs.name, n, rs.unit,
                  armed ? "armed" : "NOT ARMED", rs.where);
        if (!armed && n > 0) {
            ++n_undeclared;
            log->warn("Network (param responses, run end): site {} responded {} time(s) on a "
                      "run where it is not declared armed (filter/Responses.cpp "
                      "responseSiteArmed): the declaration is wrong.", rs.name, n);
        }
    }
    std::string jm;
    for (int m = 0; m < kCarryMechanisms; ++m) {
        const auto mech = static_cast<CarryMechanism>(m);
        const bool armed = carryMechanismArmed(mech, ctx.knobs, config_);
        std::string moved;
        long long total = 0;
        for (int b = 0; b < 4; ++b) {
            const long long n = C.prm_ref_moved.border[b][m];
            if (n == 0 && !(armed && (carryMechanismBorderKinds(mech, ctx.knobs) >> b & 1u)))
                continue;
            moved += fmt::format("{}{} {}", moved.empty() ? "" : ", ",
                                 paramKindName(paramKindOf(static_cast<SharedParamKind>(b))), n);
            total += n;
            if (n > 0 && !((carryMechanismBorderKinds(mech, ctx.knobs) >> b) & 1u)) {
                ++n_undeclared;
                log->warn("Network (param responses, run end): mechanism {} moved {} {} "
                          "value(s), a kind it does not declare (carryMechanismBorderKinds).",
                          carryMechanismName(mech), n,
                          paramKindName(paramKindOf(static_cast<SharedParamKind>(b))));
            }
        }
        for (int s = 0; s < kCarrySites; ++s) {
            NetworkParamKind lk;
            if (!carrySiteLocalKind(static_cast<CarrySite>(s), lk)) continue;
            const long long n = C.prm_ref_moved.site[s][m];
            if (n == 0 && !(armed && carryMechanismMoves(mech, static_cast<CarrySite>(s),
                                                         ctx.knobs.rebase_complete)))
                continue;
            moved += fmt::format("{}{} ({}) {}", moved.empty() ? "" : ", ", paramKindName(lk),
                                 carrySiteSpec(static_cast<CarrySite>(s)).name, n);
            total += n;
        }
        jm += fmt::format("{}{{\"mechanism\":\"{}\",\"armed\":{},\"calls\":{},\"values\":{}}}",
                          m ? "," : "", carryMechanismName(mech), armed ? "true" : "false",
                          C.prm_ref_calls[m], total);
        if (!armed && total == 0 && C.prm_ref_calls[m] == 0) continue;
        log->info("Network (param responses, run end): handover -> stransform by {}: {} "
                  "call(s), carried values moved: {} ({}).",
                  carryMechanismName(mech), C.prm_ref_calls[m],
                  moved.empty() ? std::string("none") : moved,
                  armed ? "armed" : "NOT ARMED");
        if (!armed && (total > 0 || C.prm_ref_calls[m] > 0)) {
            ++n_undeclared;
            log->warn("Network (param responses, run end): mechanism {} ran on a run where "
                      "it is not declared armed (carryMechanismArmed): the declaration is "
                      "wrong.", carryMechanismName(mech));
        }
    }
    std::string latch, jl;
    for (int c = 0; c < kLatchCauses; ++c) {
        jl += fmt::format("{}{{\"cause\":\"{}\",\"keys\":{},\"carried\":{}}}", c ? "," : "",
                          latchCauseName(c), C.prm_latch_keys[c], C.prm_latch_carried[c]);
        if (C.prm_latch_keys[c] == 0) continue;
        latch += fmt::format("{}{} {} ({} carried)", latch.empty() ? "" : ", ",
                             latchCauseName(c), C.prm_latch_keys[c], C.prm_latch_carried[c]);
    }
    log->info("Network (param responses, run end): free-epoch latch over {} epoch(s): {} "
              "(station, ambiguity) key-epoch(s) latched, {} holding a carried entry at the "
              "response, {} with more than one cause; by cause: {}.",
              C.prm_latch_epochs, C.prm_latch_unique, C.prm_latch_unique_carried,
              C.prm_latch_multi, latch.empty() ? std::string("none") : latch);
    if (C.prm_resp[static_cast<int>(ResponseSite::AmbDropStaleGap)] > 0)
        log->info("Network (param responses, run end): {} stale-gap drop(s) changed a "
                  "prior; {} arc(s) dropped in an epoch (any outcome) were erased by the gap "
                  "prune in the same epoch (absent arcs: the drop preceded the erase).",
                  C.prm_resp[static_cast<int>(ResponseSite::AmbDropStaleGap)],
                  C.prm_pruned_after_drop);
    if (C.prm_pbhold_unmoved_at_shift > 0)
        log->info("Network (param responses, run end): {} held phase-bias emission(s) of a "
                  "shifted family stayed in the old frame at a phase-bias shift (pb_hold is "
                  "exempt from the eq34 shift and the declared handover).",
                  C.prm_pbhold_unmoved_at_shift);
    log->info("Network (param responses, run end): {} of {} declared site(s) armed on this "
              "run, {} not armed and quiet; {} declaration error(s).",
              n_armed, kResponseSites, n_quiet, n_undeclared);
    traces.appendRunManifestRecord(
        config_.getNetworkConfig(),
        fmt::format("{{\"record\":\"param_responses\",\"sites\":[{}],\"mechanisms\":[{}],"
                    "\"latch\":{{\"epochs\":{},\"unique\":{},\"carried\":{},\"multi\":{},"
                    "\"causes\":[{}]}},\"pruned_after_drop\":{},\"declaration_errors\":{}}}",
                    jr, jm, C.prm_latch_epochs, C.prm_latch_unique,
                    C.prm_latch_unique_carried, C.prm_latch_multi, jl,
                    C.prm_pruned_after_drop, n_undeclared));
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
