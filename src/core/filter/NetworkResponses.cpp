/**
 * @file NetworkResponses.cpp
 * @brief The declared responses of the carried instances (card PRM phase 3a; see
 *        NetworkResponses.hpp).
 */
#include "filter/NetworkResponses.hpp"

#include "app/NetworkRunContext.hpp"   // NetworkKnobs, NetworkServiceCounters, the carry
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"

#include <array>
#include <cstddef>

namespace GnssProcessor {

namespace {

using K = NetworkParamKind;
using E = ParamEvent;
using P = ResponsePolicy;
using R = ResponseSite;
using Src = ResponseCountSource;

constexpr int kNoDisp = -1;
constexpr int dispOf(CarryDisposition d) { return static_cast<int>(d); }

/// The table, in @ref ResponseSite order (checked at first use and by the unit test).
const std::array<ResponseSiteSpec, kResponseSites> kTable{{
    // --- Ambiguity ---------------------------------------------------------------------
    {R::AmbDropLatchSlip, "amb_drop_latch_slip", K::Ambiguity, E::Slip, P::Drop, Src::Increment, kNoDisp,
     "NetworkAmbFloat.cpp networkAmbFloatApplyDrops (1)", "arcs whose prior the drop changed: the variance raised to birth strength, or the regression and block row cleared at birth strength (latched by anything but a re-entry response alone)"},
    {R::AmbDropLatchReentry, "amb_drop_latch_reentry", K::Ambiguity, E::Gap, P::Drop, Src::Increment, kNoDisp,
     "NetworkAmbFloat.cpp networkAmbFloatApplyDrops (1)", "arcs whose prior the drop changed (latched by gap re-entry or wind-up restart only)"},
    {R::AmbDropStaleGap, "amb_drop_stale_gap", K::Ambiguity, E::Gap, P::Drop, Src::Increment, kNoDisp,
     "NetworkAmbFloat.cpp networkAmbFloatApplyDrops (2)", "arcs past the gap horizon whose prior the drop changed"},
    {R::AmbFreeEpochLatch, "amb_free_epoch_latch", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject", "carried arcs not folded (latched by anything but a re-entry response alone)"},
    {R::AmbFreeEpochLatchReentry, "amb_free_epoch_latch_reentry", K::Ambiguity, E::Gap, P::FreeEpoch, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject", "carried arcs not folded (latched by gap re-entry or wind-up restart only)"},
    {R::AmbPendingReseedSkip, "amb_pending_reseed_skip", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject", "carried arcs not folded (a pending-reseed mark at the inject)"},
    {R::AmbT2ReplayFree, "amb_t2_replay_free", K::Ambiguity, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "NetworkSolveGlue.cpp within-epoch replay", "carried arcs whose prior the replay withheld"},
    {R::AmbGapSkip, "amb_gap_skip", K::Ambiguity, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject", "carried arcs past the gap horizon not folded"},
    {R::AmbGapPrune, "amb_gap_prune", K::Ambiguity, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "arcs erased"},
    {R::AmbRaimDrop, "amb_raim_drop", K::Ambiguity, E::Slip, P::Drop, Src::Increment, kNoDisp,
     "NetworkRaim.cpp attribution (T4) and consensus (T3)", "arcs whose prior the drop changed"},
    {R::AmbRaimErase, "amb_raim_erase", K::Ambiguity, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRaim.cpp attribution (T4) and consensus (T3)", "arcs erased"},
    {R::AmbRaimRetap, "amb_raim_retap", K::Ambiguity, E::Slip, P::Retap, Src::Increment, kNoDisp,
     "NetworkRaim.cpp attribution (T4)", "arcs whose variance the retap raised or whose regression it cleared"},
    {R::AmbDatumSlipErase, "amb_datum_slip_erase", K::Ambiguity, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkSlipQc.cpp datum-station slip re-reference", "arcs erased"},
    {R::AmbGrossErase, "amb_gross_erase", K::Ambiguity, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkGrossLos.cpp the declaration's response", "arcs erased"},
    {R::AmbGrossHold, "amb_gross_hold", K::Ambiguity, E::Decouple, P::Hold, Src::Increment, kNoDisp,
     "NetworkGrossLos.cpp the declaration's response", "arcs held"},
    {R::AmbWithholdErase, "amb_withhold_erase", K::Ambiguity, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkSolveGlue.cpp a withheld epoch's free response", "arcs erased"},
    {R::AmbAnchorRerefErase, "amb_anchor_reref_erase", K::Ambiguity, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkPhaseDatum.cpp networkApplyPhaseAnchorCarryReref", "arcs erased"},
    {R::AmbEq34Erase, "amb_eq34_erase", K::Ambiguity, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkChainDatum.cpp eq34 re-reference", "arcs erased"},
    {R::AmbFleetFlush, "amb_fleet_flush", K::Ambiguity, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkLeveling.cpp SH convergence flush", "arcs erased"},
    // --- Slant ionosphere --------------------------------------------------------------
    {R::IonoSlipErase, "iono_slip_erase", K::SlantIono, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp the iono carry's slip scope", "carried ionospheres erased"},
    {R::IonoRaimErase, "iono_raim_erase", K::SlantIono, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRaim.cpp attribution (T4)", "carried ionospheres erased"},
    {R::IonoGrossErase, "iono_gross_erase", K::SlantIono, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkGrossLos.cpp the declaration's response", "carried ionospheres erased"},
    {R::IonoGrossHold, "iono_gross_hold", K::SlantIono, E::Decouple, P::Hold, Src::Increment, kNoDisp,
     "NetworkGrossLos.cpp the declaration's response (bounded)", "hold declarations (a diagonal carry's hold acts only while a carried level is held)"},
    {R::IonoGapSkip, "iono_gap_skip", K::SlantIono, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject (block and diagonal)", "carried ionospheres not folded"},
    {R::IonoGapPrune, "iono_gap_prune", K::SlantIono, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune (diagonal and block carries)", "carried ionospheres erased"},
    // --- Phase colour ------------------------------------------------------------------
    {R::PcRaimErase, "pc_raim_erase", K::PhaseColour, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRaim.cpp attribution (T4)", "phase colours erased"},
    {R::PcGrossErase, "pc_gross_erase", K::PhaseColour, E::Decouple, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkGrossLos.cpp the declaration's response", "phase colours erased"},
    {R::PcGapPrune, "pc_gap_prune", K::PhaseColour, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "phase colours erased"},
    // --- Receiver code bias ------------------------------------------------------------
    {R::RxcbStepErase, "rxcb_step_erase", K::RxCodeBias, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp in-block harvest step test", "receiver code biases erased"},
    {R::RxcbPeerStepErase, "rxcb_peer_step_erase", K::RxCodeBias, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp peer-relative step gate (LV-4)", "receiver code biases erased"},
    {R::RxcbJumpSkip, "rxcb_jump_skip", K::RxCodeBias, E::Slip, P::FreeEpoch, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp diagonal route innovation test", "receiver code biases not folded"},
    {R::RxcbGapSkip, "rxcb_gap_skip", K::RxCodeBias, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject gap gates (block and diagonal)", "receiver code biases not folded"},
    {R::RxcbGapPrune, "rxcb_gap_prune", K::RxCodeBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "receiver code biases erased"},
    // --- Receiver clock ----------------------------------------------------------------
    {R::RxclkJumpErase, "rxclk_jump_erase", K::RxClock, E::Slip, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp in-block harvest prediction test", "receiver clocks erased"},
    {R::RxclkGapSkip, "rxclk_gap_skip", K::RxClock, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject (block)", "receiver clocks not folded"},
    {R::RxclkGapPrune, "rxclk_gap_prune", K::RxClock, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "receiver clocks erased"},
    // --- Receiver phase bias -----------------------------------------------------------
    {R::RpbEq34Erase, "rpb_eq34_erase", K::RxPhaseBias, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkChainDatum.cpp eq34 re-reference", "receiver phase biases erased"},
    {R::RpbGapSkip, "rpb_gap_skip", K::RxPhaseBias, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp the eq34 receiver phase-bias gap gate", "receiver phase biases not folded"},
    {R::RpbGapPrune, "rpb_gap_prune", K::RxPhaseBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "receiver phase biases erased"},
    // --- Wet delay ---------------------------------------------------------------------
    {R::ZwdGapSkip, "zwd_gap_skip", K::Zwd, E::Gap, P::GapSkip, Src::Increment, kNoDisp,
     "ApplicationNetworkAssembly.cpp PASS B inject gap gates", "wet delays not folded"},
    {R::ZwdGapPrune, "zwd_gap_prune", K::Zwd, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp harvest prune", "wet delays erased"},
    // --- Satellite clock ---------------------------------------------------------------
    {R::ClkDeparted, "clk_departed", K::SatClock, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "NetworkRunContext.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::ClkPark, "clk_park", K::SatClock, E::Gap, P::Park, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT park", "clocks parked"},
    {R::ClkParkExpire, "clk_park_expire", K::SatClock, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT expiry", "parked clocks dropped"},
    {R::ClkSeed, "clk_seed", K::SatClock, E::Readmit, P::SeedAtVarPlusQGap, Src::Disposition,
     dispOf(CarryDisposition::Seeded), "NetworkCarryDisposition.cpp carryFoldDecide (Seeded)", "clocks seeded"},
    {R::ClkParkDiscard, "clk_park_discard", K::SatClock, E::Readmit, P::Discard, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT (column back, not consumed)", "parked clocks dropped"},
    {R::ClkParkDiscardDatum, "clk_park_discard_datum", K::SatClock, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT frame drop at a clock-datum change", "parked clocks dropped"},
    {R::ClkReinit, "clk_reinit", K::SatClock, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "NetworkCarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::ClkRetire, "clk_retire", K::SatClock, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "NetworkRunContext.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::ClkAnchorDecouple, "clk_anchor_decouple", K::SatClock, E::Decouple, P::Loosen, Src::Increment, kNoDisp,
     "NetworkClockAnchor.cpp the guard", "anchored-to-loose edges"},
    {R::ClkAnchorRecouple, "clk_anchor_recouple", K::SatClock, E::Readmit, P::Recouple, Src::Increment, kNoDisp,
     "NetworkClockAnchor.cpp the guard", "loose-to-anchored edges after a decouple (first engagements excluded)"},
    {R::ClkOscWipe, "clk_osc_wipe", K::SatClock, E::Handover, P::EraseAndRebirth, Src::Increment, kNoDisp,
     "NetworkClockQc.cpp the clock oscillator at a clock-datum change", "oscillator states erased"},
    {R::ClkOscReinit, "clk_osc_reinit", K::SatClock, E::Slip, P::Reinit, Src::Increment, kNoDisp,
     "NetworkClockQc.cpp the clock oscillator's fault response", "oscillator states re-initialized"},
    {R::ClkOscGapReset, "clk_osc_gap_reset", K::SatClock, E::Gap, P::Reinit, Src::Increment, kNoDisp,
     "NetworkClockQc.cpp the clock oscillator past its maximum gap", "oscillator drifts reset"},
    // --- Satellite code bias -----------------------------------------------------------
    {R::CbDeparted, "cb_departed", K::SatCodeBias, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "NetworkRunContext.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::CbPark, "cb_park", K::SatCodeBias, E::Gap, P::Park, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT park", "code biases parked"},
    {R::CbParkExpire, "cb_park_expire", K::SatCodeBias, E::Gap, P::GapPrune, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT expiry", "parked code biases dropped"},
    {R::CbSeed, "cb_seed", K::SatCodeBias, E::Readmit, P::SeedAtVarPlusQGap, Src::Disposition,
     dispOf(CarryDisposition::Seeded), "NetworkCarryDisposition.cpp carryFoldDecide (Seeded)", "code biases seeded"},
    {R::CbParkDiscard, "cb_park_discard", K::SatCodeBias, E::Readmit, P::Discard, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT (column back, not consumed)", "parked code biases dropped"},
    {R::CbParkDiscardDatum, "cb_park_discard_datum", K::SatCodeBias, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp READMIT frame drop at a clock-datum change", "parked code biases dropped"},
    {R::CbReinit, "cb_reinit", K::SatCodeBias, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "NetworkCarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::CbRetire, "cb_retire", K::SatCodeBias, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "NetworkRunContext.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::CbTimeLinkReanchor, "cb_time_link_reanchor", K::SatCodeBias, E::Readmit, P::ReanchorAtTarget, Src::Increment, kNoDisp,
     "NetworkSbasis.cpp the geometry-free time link", "GF datum rows re-anchored at a satellite absent at the last kept epoch (row-epochs)"},
    // --- Satellite phase bias ----------------------------------------------------------
    {R::PbDeparted, "pb_departed", K::SatPhaseBias, E::Gap, P::Marginalize, Src::Departed, kNoDisp,
     "NetworkRunContext.cpp the D2 fold (departed flag)", "key-epochs folded with the column departed"},
    {R::PbReref, "pb_reref", K::SatPhaseBias, E::Handover, P::Rereference, Src::Disposition,
     dispOf(CarryDisposition::Reref), "NetworkCarryDisposition.cpp carryFoldDecide (Reref)", "key-epochs not folded"},
    {R::PbReinit, "pb_reinit", K::SatPhaseBias, E::Slip, P::Reinit, Src::Disposition,
     dispOf(CarryDisposition::Reinit), "NetworkCarryDisposition.cpp carryFoldDecide (Reinit)", "key-epochs folded at the reinit variance"},
    {R::PbRetire, "pb_retire", K::SatPhaseBias, E::Handover, P::Retire, Src::Disposition,
     dispOf(CarryDisposition::Retired), "NetworkRunContext.cpp the D2 fold (Retired)", "key-epochs not folded"},
    {R::PbPinBridgeDiscard, "pb_pin_bridge_discard", K::SatPhaseBias, E::Handover, P::Discard, Src::Increment, kNoDisp,
     "NetworkAnchorHygiene.cpp a pin bridge at a datum change", "bridges dropped"},
    // --- SH coefficient ----------------------------------------------------------------
    {R::ShRetire, "sh_retire", K::VtecSh, E::Handover, P::Retire, Src::Increment, kNoDisp,
     "NetworkRunContext.cpp the SH prior at a clock-datum change", "SH key-epochs not folded"},
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
        case ResponsePolicy::Retap:             return "retap";
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
    const bool clockReinit = (k.clk_mon_on || config.networkClockMonitorCoupleRaim())
                             && k.clk_mon_reinit_sig > 0.0;
    const bool relevelFree = config.networkCycleSlipRelevelFreeEpoch();
    const bool reentry = nc.slip_qc.gap_reentry || config.networkCycleSlipWindupRestartRefloat();
    // The leveling vehicle's local inject (carry_warm implies phase_leveling; the column is
    // carried unless carry_invariant).
    const bool levelInject = k.pl_on && !k.carry_invariant && !k.amb_float_kf;
    // RAIM's T4 block: entered on the attribution's action, or on an event-harness arc erase
    // in an erase mode (network.inject, arc_erase).
    bool injectArcErase = false;
    if (nc.inject.enable)
        for (const auto& ev : nc.inject.events)
            if (ev.kind == "arc_erase" && ev.mode != "free_epoch") injectArcErase = true;
    const bool t4 = config.networkRaimAttribution() || injectArcErase;
    switch (s) {
        // The border's fold dispositions (carryFoldDecide and the fold's Retired clause).
        case R::ClkDeparted:
        case R::CbDeparted:
        case R::PbDeparted:  return true;                       // a column can always leave
        case R::ClkSeed:     return readmit;
        case R::CbSeed:      return readmit && k.readmit_seed_cb;
        // inflate_q: a clock refloat flag (the clock monitor, or RAIM's and the within-epoch
        // replay's satellite culprits under clock_monitor_couple_raim) on all three kinds,
        // stashed soft only with a positive reinit sigma; and the code-bias excursion guard.
        case R::ClkReinit:
        case R::PbReinit:    return clockReinit;
        case R::CbReinit:    return clockReinit || config.getNetworkConfig().cb_excursion_reinit;
        // The retired-datum clause: a clock-pivot handover in scoped mode whose carry the
        // S-transform did not re-express.
        case R::ClkRetire:
        case R::CbRetire:
        case R::PbRetire:    return k.scoped_handover;
        case R::PbReref:     return config.networkPhaseAnchorCarryReref() || k.phase_datum_eq34;

        // --- The ambiguity: the float vehicle's drop, the leveling vehicle's skips.  The
        // latch exists only under relevel_free_epoch (the loader disarms it without
        // phase_leveling and under carry_invariant); float_kf implies phase_leveling.
        case R::AmbDropLatchSlip:     return k.amb_float_kf && relevelFree;
        case R::AmbDropLatchReentry:  return k.amb_float_kf && relevelFree && reentry;
        case R::AmbDropStaleGap:      return k.amb_float_kf;
        case R::AmbFreeEpochLatch:    return levelInject && relevelFree;
        case R::AmbFreeEpochLatchReentry: return levelInject && relevelFree && reentry;
        case R::AmbPendingReseedSkip: return k.pl_on && !k.carry_invariant;
        case R::AmbT2ReplayFree:
            return k.pl_on && !k.carry_invariant && config.networkRaimWithinEpoch();
        case R::AmbGapSkip:           return levelInject;
        case R::AmbGapPrune:          return k.pl_on;
        case R::AmbRaimDrop:          return k.raim_on && k.pl_on && k.amb_float_kf;
        case R::AmbRaimErase:         return k.raim_on && k.pl_on && !k.amb_float_kf;
        case R::AmbRaimRetap:
            return k.raim_on && k.pl_on && t4 && config.networkCycleSlipRelevelRetapStatistical();
        case R::AmbDatumSlipErase:    return k.datum_station_detect && k.pl_on;   // forced off without mw_detect and under the prioritized datum
        case R::AmbGrossErase:        return k.gl_on && !k.gl_bound && k.pl_on;
        case R::AmbGrossHold:         return k.gl_on && k.gl_bound && k.pl_on;
        case R::AmbWithholdErase:     return k.wh_free_erase && k.raim_withhold > 0.0 && k.pl_on;
        case R::AmbAnchorRerefErase:
            return k.pl_on && k.pa_carry_reref && (!k.pd_held_basis || k.pd_held_cold_release);
        case R::AmbEq34Erase:         return k.phase_datum_eq34 && !k.phase_datum_ensemble && k.pl_on;
        case R::AmbFleetFlush:
            return k.pl_on && k.pl_refloat_on_conv && config.estimateNetworkIonoSh();
        // --- The slant ionosphere and the phase colour (erased with their line of sight).
        case R::IonoSlipErase:        return k.iono_on;
        case R::IonoRaimErase:        return k.raim_on && k.pl_on && t4 && (k.iono_on || k.iono_blk);
        case R::IonoGrossErase:       return k.gl_on && !k.gl_bound && (k.iono_on || k.iono_blk);
        case R::IonoGrossHold:        return k.gl_bound && (k.iono_on || k.iono_blk);
        case R::IonoGapSkip:
            return k.pl_on && (k.iono_on || (k.iono_blk && k.pl_local_block && k.carry_cpl_cond
                                             && !k.carry_cpl_joint));
        case R::IonoGapPrune:
            return k.pl_on && (k.iono_on || (k.iono_blk && k.pl_local_block && k.carry_cpl_cond));
        case R::PcRaimErase:          return k.raim_on && k.pl_on && t4 && k.pc_on;
        case R::PcGrossErase:         return k.gl_on && !k.gl_bound && k.pc_on;
        case R::PcGapPrune:           return k.pl_on && k.pc_on;
        // --- The receiver-side carries.
        case R::RxcbStepErase:
            return k.pl_on && k.rx_cb_on && k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::RxcbPeerStepErase:    return k.rx_cb_on && k.rx_cb_step_gate && !k.rxcb_blk;
        case R::RxcbJumpSkip:
            return k.rx_cb_on && !k.rx_cb_step_gate
                && !(k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond && !k.carry_cpl_joint);
        case R::RxcbGapSkip:   // the in-block route, or the diagonal route when the block is not armed
            return k.rx_cb_on
                && ((k.pl_on && !k.carry_invariant)
                    || !(k.rxcb_blk && k.pl_local_block && k.carry_cpl_cond && !k.carry_cpl_joint));
        case R::RxcbGapPrune:         return k.rx_cb_on;
        case R::RxclkJumpErase:
        case R::RxclkGapPrune:
            return k.pl_on && k.rxclk_blk && k.pl_local_block && k.carry_cpl_cond;
        case R::RxclkGapSkip:
            return k.pl_on && k.rxclk_blk && k.pl_local_block && k.carry_cpl_cond && !k.carry_cpl_joint;
        case R::RpbEq34Erase:
            return k.phase_datum_eq34 && !k.phase_datum_ensemble && k.pl_on && k.eq34_rpb_q > 0.0;
        case R::RpbGapSkip:           return k.phase_datum_eq34 && k.eq34_rpb_q > 0.0;
        case R::RpbGapPrune:          return k.pl_on && k.eq34_rpb_q > 0.0;
        case R::ZwdGapSkip:           return k.pl_on && (k.zwd_on || k.zwd_seam_on);
        case R::ZwdGapPrune:          return k.pl_on && (k.zwd_on || k.zwd_capture || k.zwd_seam_on);
        // --- The border's READMIT, anchor and oscillator responses.
        case R::ClkPark:
        case R::ClkParkExpire:
        case R::ClkParkDiscard:
        case R::ClkParkDiscardDatum:  return readmit;
        case R::CbPark:
        case R::CbParkExpire:
        case R::CbParkDiscard:
        case R::CbParkDiscardDatum:   return readmit && k.readmit_seed_cb;
        case R::ClkAnchorDecouple:
        case R::ClkAnchorRecouple:    return k.cta_on;
        case R::ClkOscWipe:
        case R::ClkOscReinit:
        case R::ClkOscGapReset:       return k.clk_model_on;
        case R::CbTimeLinkReanchor:
            return config.estimateNetworkSatCodeBias() && k.cb_time_linked
                && config.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode
                && config.getNetworkConfig().sat_gf_datum_time_linked;
        case R::PbPinBridgeDiscard:   return k.pin_bridge_on;
        case R::ShRetire:             return config.estimateNetworkIonoSh();
    }
    return false;
}

bool carryMechanismArmed(CarryMechanism m, const NetworkKnobs& k, const GnssConfig& config) {
    switch (m) {
        case CarryMechanism::ClockLevel:         return k.clv_on;
        case CarryMechanism::EphRelevel:
            return config.networkAprioriSource() == NetworkAprioriSource::BrdcRts;
        case CarryMechanism::Handover:           return k.handover_stransform;
        case CarryMechanism::HandoverBeforeRows: return k.handover_stransform_before_rows;
        case CarryMechanism::Eq34Shift:          return k.phase_datum_eq34 && k.eq34_shift_on;
        case CarryMechanism::DeclaredHandover:   return k.amb_float_declared_handover;
    }
    return false;
}

std::uint32_t carryMechanismBorderKinds(CarryMechanism m, const NetworkKnobs& k) {
    const auto bit = [](SharedParamKind x) { return std::uint32_t{1} << static_cast<int>(x); };
    switch (m) {
        case CarryMechanism::ClockLevel:
            return bit(SharedParamKind::Clock) | bit(SharedParamKind::PhaseBias)
                 | ((k.clv_cb_follow > 0 || k.clv_free_cb_rebase) ? bit(SharedParamKind::CodeBias) : 0u);
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

void networkResponseLatchTally(NetworkRunContext& ctx) {
    const RelevelEpochState& lat = ctx.carry.relevel;
    if (lat.free_ambs.empty()) return;
    auto& C = ctx.counters;
    ++C.prm_latch_epochs;
    for (const auto& key : lat.free_ambs) {
        const auto parts = StateKeys::parseAmbiguityKey(key.second);
        const bool carried = parts
            && ctx.carry.local_carry.count(std::make_tuple(key.first, parts->satellite,
                                                           parts->signal)) > 0;
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
        "mw", "lli", "if_td", "t4_attrib", "t3_consensus", "windup", "gap_reentry", "none"};
    return (i >= 0 && i < kLatchCauses) ? kNames[i] : "?";
}

}  // namespace GnssProcessor
