/**
 * @file PassASlipDrains.cpp
 * @brief The PASS A response drain stages declared in PassASlipDrains.hpp.
 *
 * The bodies were cut verbatim from `Application::networkPassASlipScan_`; only the
 * bindings at the head of each stage and the calls between stages are new.
 */

#include "network/slip/PassASlipDrains.hpp"

#include "network/slip/GfArcJump.hpp"          // networkSlipDrainGfArcJump
#include "network/slip/SlipQc.hpp"             // the drain seams
#include "network/assembly/Responses.hpp"     // prmRespond, ResponseSite
#include "network/assembly/SignalRoster.hpp"  // signalRosterModelledArcs
#include "network/state/ArcLedger.hpp"        // ArcReentryEpoch, the re-entry population
#include "network/state/Entities.hpp"         // net::relevelPendMap, net::assignSatFlags
#include "network/ar/ArSuspect.hpp"           // arSuspectNote
#include "network/carry/Inject.hpp"           // the injected free_epoch directive
#include "config/GnssConfig.hpp"

#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

std::map<net::ArcTuple, RelevelPend> passAMwRefloatStage(const PassAView& v, bool mw_carry_warm,
                                                         const bool datum_detect_on,
                                                         RelevelEpochState* relevel_ptr,
                                                         const CommonBandEpoch* common_band_ptr) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    net::NetworkEntities& entities = ctx.entities;   // written: sats
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto log = v.log;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;
    net::ArcTable& arc_rows = v.arc_rows;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;

    // The datum-station slip re-reference's carry erases, read off the
    // drain's own counter around the call (serial: between the two parallel regions).
    const long long dse0 = qc.counters.datum_slip_carry_erases;
    // The datum re-reference queues as the drain takes them: copied in
    // from the satellite table and written back when the drain may have changed them.
    std::set<std::string> datum_reref_pb_sats =
        net::satSet(ctx.entities.sats, &net::SatelliteState::datum_reref_pb);
    std::set<std::string> datum_reref_amb_sats =
        net::satSet(ctx.entities.sats, &net::SatelliteState::datum_reref_amb);
    // The bridge v2 pends as the drains below take them: copied in from
    // the lines of sight here and written back after the last drain.
    std::map<net::ArcTuple, RelevelPend> relevel_pend = net::relevelPendMap(ctx.entities.los);
    networkSlipDrainMwRefloat(pa, config_, stations, present, modeled, i_lo,
                              n_sta_total, carry_invariant, mw_carry_warm,
                              carry_slipped_ambs, qc.epoch, qc.params, qc.hist,
                              qc.counters, se, traces, log,
                              datum_detect_on ? &datum_reref_pb_sats : nullptr,
                              datum_detect_on ? &datum_reref_amb_sats : nullptr,
                              datum_detect_on ? &ctx.carry.refsat_mark_sats : nullptr,
                              datum_detect_on ? &arc_rows : nullptr,
                              relevel_ptr,
                              relevel_ptr ? &relevel_pend : nullptr,
                              relevel_ptr ? &arc_rows : nullptr,
                              // Simulation-only oracle slip response: null unless
                              // network.cycle_slip.oracle_relevel armed the run,
                              // so the off path never reaches a hook call
                              // (structurally byte-identical, invariant #6).
                              ctx.carry.slip_oracle.get(),
                              common_band_ptr);
    if (datum_detect_on) {
        net::assignSatFlags(entities, &net::SatelliteState::datum_reref_pb, datum_reref_pb_sats);
        net::assignSatFlags(entities, &net::SatelliteState::datum_reref_amb, datum_reref_amb_sats);
    }
    prmRespond(ctx.counters.prm_resp, ResponseSite::AmbDatumSlipErase,
               qc.counters.datum_slip_carry_erases - dse0);
    return relevel_pend;
}

void passAIfTdStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                    std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                    const CommonBandEpoch* common_band_ptr) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto log = v.log;
    bool carry_warm = v.carry_warm;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;
    const bool trace_flag_rates = v.trace_flag_rates;
    net::ArcTable& arc_rows = v.arc_rows;
    auto& fr_if_slip = v.fr_if_slip;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;

    // The mark sink is passed only when armed (Edge-1 piece 2), so the off path
    // passes nullptr and the driver's insert is never reached (byte-identical).
    networkSlipIfTdDriver(pa, config_, stations, present, i_lo, n_sta_total,
                          carry_invariant, carry_warm, carry_slipped_ambs,
                          trace_flag_rates, fr_if_slip, se, traces, log,
                          config_.networkMarkRefsatResetDisc()
                              ? &ctx.carry.refsat_mark_sats : nullptr,
                          relevel_ptr,
                          relevel_ptr ? &arc_rows : nullptr,
                          relevel_ptr ? &qc.counters : nullptr,
                          relevel_ptr ? &relevel_pend : nullptr,
                          // The qualified raw verdicts (d_gf) and per-station
                          // empirical step scales (bridge v3 E4), only when the
                          // GF row is armed (nullptr keeps the off path inert).
                          config_.networkCycleSlipRelevelBridgeGf() ? &qc.epoch : nullptr,
                          config_.networkCycleSlipRelevelBridgeGf() ? &qc.hist : nullptr,
                          ctx.carry.slip_oracle.get(),    // simulation-only oracle
                          common_band_ptr,                // common-band scope (§14)
                          // Pair width (§14.10): always passed, because the
                          // per-pair-kind tally is knob-independent telemetry
                          // (invariant #4), so a `first` run's run-end clause
                          // already sizes what the one watched pair sees.
                          // Counters only; the solve, the products and the event
                          // rows are untouched while the knob is `first`.
                          &ctx.counters.n_iftd_pair);
}

void passAGfArcJumpStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                         std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                         const CommonBandEpoch* common_band_ptr) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    auto log = v.log;
    bool carry_warm = v.carry_warm;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;
    const bool ar_suspect_armed = v.ar_suspect_armed;
    const int i_lo = v.i_lo;

    // ---- Arc-scoped GF jump trigger ---------------------------------
    // (include/network/slip/GfArcJump.hpp.)  The pre-solve witness of the slips the MW
    // drain and the IF-TD driver above both miss: the equal count (1,1), the 60/77
    // family, and a slip repeated in the epoch after an IF-TD flag.  Sited after the
    // IF-TD driver, so a band that driver already freed takes only the cause bit (its
    // response and bridge pend stay as they were), and before the wind-up, gap
    // re-entry and injected-free drains, like every freeing
    // site, so this epoch's PASS-B inject and the anchor-slip handover read the latch
    // it fills.  No-op unless network.cycle_slip.gf_arc_jump.
    networkSlipDrainGfArcJump(qc, stations, present, i_lo, n_sta_total,
                              carry_invariant, carry_warm, carry_slipped_ambs,
                              se, traces, log, relevel_ptr,
                              relevel_ptr ? &relevel_pend : nullptr,
                              common_band_ptr);
    // The producer AR search's suspect-corner mask (
    // network/ar/ArSuspect.hpp): every GF flag stamps its corner, whatever the drain
    // did with it (a cold, pinned or common-band-kept flag still names a doubtful arc).
    if (ar_suspect_armed)
        for (const LosId& los : qc.epoch.gf_jump_flagged)
            arSuspectNote(ctx.carry.ar_suspect, los,
                          static_cast<long long>(ctx.counters.n_loop_epochs),
                          kArSuspectGfJump, &ctx.counters.netar_suspect);
}

void passAWindupRestartStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                             std::map<net::ArcTuple, RelevelPend>& relevel_pend) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto log = v.log;
    double pl_now_s = v.pl_now_s;
    bool carry_warm = v.carry_warm;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;
    net::ArcTable& arc_rows = v.arc_rows;
    const int i_lo = v.i_lo;

    // ---- Wind-up accumulator-restart response (§10.4) ------------------------
    // (SPEC_phase_windup_sign §10.4.)  Drains the per-station restart ledgers that
    // the model evaluations above just filled and refloats the stepped arcs: the
    // wired invariant-#1 response that any accumulate_phase_windup arm requires.
    // A restart-freed pair enters this epoch's freed set like every other.  No-op unless
    // network.cycle_slip.windup_restart_refloat (disarmed by the loader outside
    // accumulated mode).  It receives the leveling carry for the reason the gap
    // re-entry drain below does: a restart is a return after a gap, so the arc
    // usually has no ambiguity column yet at this point and its carry entry is the
    // target.
    networkSlipDrainWindupRestart(config_, stations, present, i_lo, n_sta_total,
                                  carry_invariant, carry_warm,
                                  carry_slipped_ambs, qc.counters, se, traces,
                                  log, relevel_ptr, &relevel_pend,
                                  ctx.carry.slip_oracle.get(),    // simulation-only oracle
                                  &arc_rows, pl_now_s, ctx.knobs.pl_max_gap_s,
                                  // The arc identity from the arc ledger.
                                  ctx.roster.active ? &ctx.arc_ledger : nullptr,
                                  &ctx.counters.sid.n3b);
}

void passAGapReentryStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                          std::map<net::ArcTuple, RelevelPend>& relevel_pend) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    auto log = v.log;
    double pl_now_s = v.pl_now_s;
    bool carry_warm = v.carry_warm;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    SlipQcChannel& qc = v.qc;
    EpochEvents::Slip& slip_ev = v.slip_ev;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;
    net::ArcTable& arc_rows = v.arc_rows;
    const int i_lo = v.i_lo;

    // ---- Gap re-entry refloat (gap response) ------------------------------
    // (docs/SPEC_slip_qc_hardening.md §G3/W1.)  An arc that re-appears within
    // network.phase_leveling_max_gap_s has no detector covering its return epoch
    // (MW needs an adjacent predecessor and IF-TD three consecutive epochs), so its
    // pre-gap zero-q leveling level would be re-injected across a break that the raw
    // MW/GF evidence (or the receiver's own LLI) says it did not survive.  This drain
    // is the wired invariant-#1 response: it marks both bands of a slipped re-entry
    // exactly as a detected slip.
    //
    // Sited here because it must run in the same epoch as the re-entry (the PASS-B
    // leveling inject in networkPassB_ would fold the stale level, and it reads
    // `relevel.free_ambs`), so a gap-freed pair enters this epoch's freed set like
    // every other response.
    //
    // It receives `local_carry` and (pl_now_s, pl_max_gap_s), not only the
    // StateManager, because at this point in the epoch a re-entering arc has no
    // ambiguity column (PASS B's ensureRequiredStates re-creates it later).  The
    // carry entry is what survives the gap and what the inject would bridge, so the
    // carry is the drain's target and its oracle; the ambiguity key is synthesised
    // in the inject site's own format.  A drain that walked the live ambiguity map
    // instead would find no re-entering arc and do nothing.
    // No-op unless network.cycle_slip.gap_reentry_relevel.
    //
    // Ledger re-entry population (network.ledger_reentry): the
    // population of both drains below becomes the arc ledger's admitted
    // Gap-to-Reentered set.  It is built here because this is the first point at
    // which the epoch's modelled rows exist (computeModels ran in PASS A above) and
    // the last before the response needs them.  It is a query on the ledger's
    // admitted timestamps, not a step: the admitted machine is advanced once per
    // epoch in the roster's audit pass, far downstream, and advancing it twice
    // would make every arc look continuous.  Disarmed, `sid` stays empty and both
    // drains take their raw-driven path, byte for byte.
    ArcReentryEpoch sid;
    sid.armed = qc.params.ledger_reentry && ctx.roster.active;
    sid.refloat_no_verdict = qc.params.ledger_reentry_refloat;
    if (sid.armed) {
        std::set<ArcLedger::Key> admitted;
        signalRosterModelledArcs(ctx.roster, modeled, present, admitted,
                                 &sid.not_modelled);
        arcLedgerReentryPopulation(ctx.arc_ledger, admitted,
                                   ctx.arc_ledger.epochIndex(), pl_now_s,
                                   ctx.knobs.pl_max_gap_s, sid);
    }
    // The per-arc outcome sink of the unified "answered" census (R4) is written
    // in both states of network.ledger_reentry on every run (the roster is built
    // on every run), so the four-way split is comparable
    // across the flip.  A record only: nothing in the solve reads it.
    ArcReentryOutcomeMap* sid_out =
        ctx.roster.active ? &slip_ev.sid_reentry_outcome : nullptr;
    networkSlipDrainGapReentry(stations, present, i_lo, n_sta_total,
                               carry_invariant, carry_warm, carry_slipped_ambs,
                               qc.epoch, qc.params,
                               arc_rows, pl_now_s, ctx.knobs.pl_max_gap_s,
                               qc.counters, se, traces, log,
                               relevel_ptr,
                               relevel_ptr ? &relevel_pend : nullptr,
                               sid.armed ? &sid : nullptr, sid_out,
                               ctx.roster.active ? &ctx.arc_ledger : nullptr);

    // ---- The same response, keyed on the signal ------------------
    // (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md.)  The drain above
    // answers a raw row that went absent.  A satellite that keeps one tracking
    // signal through a primary-pair loss of lock keeps its row, so that tier does
    // not see it, and the returning phases' carried ambiguities would fold across
    // the break.  Sited immediately after it, for the same reason (same epoch as the
    // re-entry), with the same arguments.
    // No-op unless network.cycle_slip.gap_reentry_per_signal (a rider of
    // gap_reentry_relevel).
    networkSlipDrainGapReentryPerSignal(stations, present, i_lo, n_sta_total,
                                        carry_invariant, carry_warm, carry_slipped_ambs,
                                        qc.epoch, qc.params,
                                        arc_rows, pl_now_s, ctx.knobs.pl_max_gap_s,
                                        qc.counters, se, traces, log,
                                        relevel_ptr,
                                        relevel_ptr ? &relevel_pend : nullptr,
                                        sid.armed ? &sid : nullptr, sid_out);
}

void passAInjectedFreeStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                            std::map<net::ArcTuple, RelevelPend>& relevel_pend) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    auto log = v.log;
    bool carry_warm = v.carry_warm;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = v.carry_slipped_ambs;
    const int n_sta_total = v.n_sta_total;
    const bool carry_invariant = v.carry_invariant;

    // ---- Injected free_epoch arc response (event harness, Study 1) ----------
    // (docs/SPEC_event_harness.md §9.)  An `arc_erase` event carrying
    // "mode": "free_epoch" is answered here, as a detector refloat, instead of by
    // the T4 attribution block: the A/B that prices the erase path's four extra
    // actions.  Sited with the other PASS-A drains so that this epoch's PASS-B
    // leveling inject reads the free-epoch latch it fills (post-fit RAIM is too
    // late: relevel.clear() above would run first, in the next epoch).
    // Unreachable unless the injector staged a free_epoch directive.
    if (ctx.inject.armed && !ctx.inject.arc_erase_now.empty())
        networkSlipDrainInjectedFree(ctx.inject.arc_erase_now, stations, present,
                                     n_sta_total, carry_invariant, carry_warm,
                                     carry_slipped_ambs, ctx.inject, se, traces, log,
                                     relevel_ptr,
                                     relevel_ptr ? &relevel_pend : nullptr);
}

} // namespace GnssProcessor
