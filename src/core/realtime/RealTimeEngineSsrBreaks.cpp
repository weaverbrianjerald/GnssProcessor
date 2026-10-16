/**
 * @file RealTimeEngineSsrBreaks.cpp
 * @brief SSR datum-break reactions for RealTimeEngine::processEpoch.
 *
 * Five relocated handlers react when the network producer moves its datum
 * (reactToGenerationChange, logProducerStatus, resetOnPhaseBiasDisc,
 * reinflateOnCodeBiasDisc, reinflateOnClockDisc); the file also holds the
 * declared-roster consumer and the hold, carry and translate variants that
 * resetOnPhaseBiasDisc dispatches.
 *
 * Order matters: reactToGenerationChange applies the union of the
 * finer-grained reactions globally (a producer restart re-realizes every
 * bias and level at once, with disc counters re-seeded to 0), so it must
 * run before the per-signal handlers, which would otherwise see 0->0 as "no
 * change".  All five are no-ops without SSR content, so live RTCM streams
 * stay byte-identical.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"
#include "measurement/StateInitializer.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "common/Constants.hpp"           // the L1 reference frequency
#include "common/SignalBand.hpp"          // a phase signal's carrier
#include "filter/StateKeySchema.hpp"      // the ambiguity keys
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <map>
#include <fmt/format.h>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {
/// Cap on the ARC rows one consumer writes for freshness flips, so a pathological
/// wire cannot flood the log.  The counters record every flip regardless of the
/// cap.
constexpr long long kRosterFlipEventCap = 400;
}  // namespace

// ---------------------------------------------------------------------------
// The declared signal roster at the consumer
// ---------------------------------------------------------------------------

void RealTimeEngine::refreshDeclaredRoster_() {
    ++roster_consumer_.epochs;
    NetworkSignalDeclaration decl = ssrStore().declaredRoster();
    const bool changed = (decl != ssr_declared_roster_);
    if (decl.present) ++roster_consumer_.epochs_declared;
    if (changed) {
        if (ssr_declared_roster_.present) ++roster_consumer_.decl_changes;
        ssr_declared_roster_ = std::move(decl);
    }
    // The latch is decided inside computeModels at a constellation's first epoch,
    // so the declaration must reach the model before that call.  It is handed
    // over on every change and on the first epoch, so a mid-run change also
    // reaches a constellation whose latch is not yet decided.
    if (changed || roster_consumer_.epochs == 1)
        ppp_model_->setDeclaredSignalRoster(ssr_declared_roster_,
                                            config_.consumerRosterLatch());

    if (declared_roster_logged_) return;
    // Log one line, once, in either direction: an armed knob on a wire that
    // carries no declaration is inert and reports it (standing invariant 4), and
    // a declaration arriving at an unarmed consumer is logged as the phase-5a
    // observatory.
    auto log = log_manager_.getLogger("rt_engine");
    if (ssr_declared_roster_.present) {
        declared_roster_logged_ = true;
        log->info("[RT] Card SID phase 5: the producer DECLARES its signal roster on this "
                  "wire: {}.  The consumer's band latch {} it (network.closed_loop."
                  "roster_latch {}), and every freshness flip is attributed against it.",
                  signalDeclarationSummary(ssr_declared_roster_),
                  config_.consumerRosterLatch() ? "FOLLOWS" : "is only COMPARED with",
                  config_.consumerRosterLatch() ? "armed" : "off");
    } else if (config_.consumerRosterLatch() && roster_consumer_.epochs > 1) {
        declared_roster_logged_ = true;
        log->warn("[RT] Card SID phase 5b: network.closed_loop.roster_latch is armed but this "
                  "wire carries NO declared signal roster (the producer did not arm "
                  "network.ledger_emit), so the knob is INERT: the latch keeps the "
                  "consumer's own rule and every freshness flip keeps today's response.");
    }
}

bool RealTimeEngine::noteFreshnessFlip_(const std::string& key, const std::string& sat,
                                        const std::string& signal, const std::string& cons,
                                        const char* domain, bool was_fresh, bool now_fresh,
                                        bool on_wire, const GnssTime& t) {
    (void)key;
    ++roster_consumer_.flips;
    if (domain[0] == 'p') ++roster_consumer_.flips_phase; else ++roster_consumer_.flips_code;
    if (now_fresh) ++roster_consumer_.flip_to_fresh; else ++roster_consumer_.flip_to_stale;

    // The three-way split of E3, read in this order because each test is only
    // meaningful when the one before it failed:
    //   1. the declaration names no family for this signal at all: the network
    //      structurally does not carry it, so its absence is not a staleness;
    //   2. it does, but the store holds no record for this satellite: the family
    //      exists and is absent here (the dropouts of the multi-site consumer);
    //   3. a record exists and the age gate refused it: a genuine staleness.
    const bool have_decl = ssr_declared_roster_.present;
    const FlipAttribution att = signalDeclarationAttributeFlip(
        ssr_declared_roster_, cons, signal, domain[0] == 'p', on_wire);
    const char* cls = flipAttributionName(att);
    switch (att) {
    case FlipAttribution::NoDeclaration: ++roster_consumer_.flip_no_decl;    break;
    case FlipAttribution::Undeclared:    ++roster_consumer_.flip_undeclared; break;
    case FlipAttribution::OffWire:       ++roster_consumer_.flip_off_wire;   break;
    case FlipAttribution::Stale:         ++roster_consumer_.flip_stale;      break;
    }
    const bool undeclared = (att == FlipAttribution::Undeclared);

    if (roster_consumer_.flip_events < kRosterFlipEventCap) {
        ++roster_consumer_.flip_events;
        // Record the identity of the flip (satellite, signal, from, to, epoch)
        // on the ARC tier the other lifecycle edges use, for both edges
        // of the gate.
        if (consumer_sat_trace_)
            noteConsumerSatEvent_(sat + "|" + signal,
                                  now_fresh ? "flip_to_fresh" : "flip_to_stale");
        auto log = log_manager_.getLogger("rt_engine");
        log->info("[RT] ARC ssr_freshness_flip {} {} {} ({} -> {}) class={} on_wire={} "
                  "declared={} tow={:.1f}",
                  domain, sat, signal, was_fresh ? "fresh" : "stale",
                  now_fresh ? "fresh" : "stale", cls, on_wire ? 1 : 0,
                  have_decl ? 1 : 0, t.tow);
    } else {
        ++roster_consumer_.flip_events_suppressed;
    }
    // Only the Undeclared class is suppressed, and only with the knob armed (C2).
    return undeclared && config_.consumerRosterLatch();
}

const PppModel::DeclaredLatchCounters& RealTimeEngine::declaredLatchCounters() const {
    return ppp_model_->declaredLatchCounters();
}

void RealTimeEngine::logRosterConsumerSummary(const char* tag) const {
    const auto& r = roster_consumer_;
    const auto& d = ppp_model_->declaredLatchCounters();
    // Silent when no declaration was ever seen and the knob is off: a consumer of
    // an earlier wire prints nothing, so its log is byte-identical.
    if (r.epochs_declared == 0 && !config_.consumerRosterLatch()) return;
    auto log = log_manager_.getLogger("rt_engine");
    log->info("[RT] Card SID phase 5, declared roster ({}): {} of {} epoch(s) held a "
              "declaration ({} change(s)) -> latch compared {} cells: agree {} / differ {} "
              "(of which the consumer's own signal is NOT declared at all: {}) / none of "
              "the declared signals tracked {}; latch taken FROM the declaration {} / left "
              "on the consumer's own rule {} (roster_latch {})",
              tag, r.epochs_declared, r.epochs, r.decl_changes, d.compared, d.agree,
              d.differ, d.own_not_declared, d.none_tracked, d.from_roster, d.own_rule,
              config_.consumerRosterLatch() ? "ARMED" : "off");
    log->info("[RT] Card SID phase 5, freshness flips ({}): {} total (phase {} / code {}; "
              "to stale {} / to fresh {}) -> UNDECLARED {} (the network carries no family "
              "for the signal) / OFF-WIRE {} (it does, and this satellite has no record) / "
              "STALE {} (a record exists and the age gate refused it) / unattributable {} "
              "(no declaration held); responses SUPPRESSED by the declaration {}; ARC rows "
              "{} ({} suppressed by the cap of {})",
              tag, r.flips, r.flips_phase, r.flips_code, r.flip_to_stale, r.flip_to_fresh,
              r.flip_undeclared, r.flip_off_wire, r.flip_stale, r.flip_no_decl,
              r.flip_suppressed, r.flip_events, r.flip_events_suppressed,
              kRosterFlipEventCap);
    if (config_.ssrPhaseBiasStepTranslate() || config_.ssrPhaseBiasDiscCarry())
        log->warn("[RT] Card SID phase 5: this consumer runs the card-PBT translation or the "
                  "card-CC carry as its phase-bias reaction, and the flip census above is "
                  "wired on the RESET path only: the PHASE half of the count is therefore "
                  "dead wiring on this posture.  The CODE half is unaffected.");
}

/**
 * @brief Solution-generation id change -> global datum break (RTA-3).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1648-1670); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::reactToGenerationChange(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;

    const auto gid = ssrStore().getGenerationId();
    if (gid) {
        if (last_ssr_generation_id_ != 0 && *gid != last_ssr_generation_id_) {
            log->warn("{}SSR solution-generation id changed ({} -> {}): producer restart / "
                      "global datum re-realization — resetting ALL network-corrected "
                      "ambiguities and re-inflating receiver code-bias states.",
                      epoch_prefix, last_ssr_generation_id_, *gid);
            const double reset_std = config_.getAmbiguityStd();
            std::vector<std::string> keys;   // copy: resetAmbiguity may touch the map
            keys.reserve(state_manager_->getAmbiguities().size());
            for (const auto& kv : state_manager_->getAmbiguities()) keys.push_back(kv.first);
            for (const auto& k : keys) state_manager_->resetAmbiguity(k, reset_std);
            // Record the global reset per ambiguity key, so the per-satellite rows
            // of that epoch show it and a mass reset is not mistaken for an
            // undeclared one.  No-op unarmed.
            if (consumer_sat_trace_)
                for (const auto& k : keys) {
                    const auto sep = k.find('_');
                    if (sep == std::string::npos || sep == 0 || sep + 1 >= k.size()) continue;
                    noteConsumerSatEvent_(k.substr(0, sep) + "|" + k.substr(sep + 1),
                                          "reset_gen");
                }
            const double cb_var =
                config_.codeBiasReinflateStd() * config_.codeBiasReinflateStd();
            std::vector<std::string> cb_keys;
            cb_keys.reserve(state_manager_->getCodeBiasStates().size());
            for (const auto& kv : state_manager_->getCodeBiasStates())
                cb_keys.push_back(kv.first);
            for (const auto& k : cb_keys)
                state_manager_->reinflateCodeBiasVariance(k, cb_var);
        }
        last_ssr_generation_id_ = *gid;
    }
}

/**
 * @brief Producer epoch-status heartbeat -> one log line per transition (RTA-10).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1684-1699); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::logProducerStatus(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;

    const auto pst = ssrStore().getProducerStatus();
    if (pst && (!last_producer_status_ || *last_producer_status_ != pst->state)) {
        const char* from = last_producer_status_
                               ? NetworkProducerStatus::stateName(*last_producer_status_)
                               : "(none)";
        const char* to = NetworkProducerStatus::stateName(pst->state);
        if (pst->state == NetworkProducerStatus::State::Ok)
            log->info("{}Network producer status: {} -> {} (heartbeat tow {:.1f}).",
                      epoch_prefix, from, to, pst->time.tow);
        else
            log->warn("{}Network producer status: {} -> {} (heartbeat tow {:.1f}, "
                      "sigma0 {:.2f}, withheld [{}], {} sat(s) published).",
                      epoch_prefix, from, to, pst->time.tow, pst->sigma0,
                      pst->withheld, pst->n_sats_published);
        last_producer_status_ = pst->state;
    }
}

/**
 * @brief SSR phase-bias discontinuity / freshness flip -> ambiguity reset.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1708-1751); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::resetOnPhaseBiasDisc(RtEpochContext& ctx) {
    // Held keys are handled first, keyed on the data rather than on a consumer
    // knob: the consumer cannot know that the producer armed network.emit_pb_hold,
    // it can only read SsrPhaseBiasEntry::hold_generation.  Every key handled
    // there lands in ctx.pb_hold_handled and is skipped by whichever of the three
    // reactions below runs, so each key is visited once per epoch (the same
    // guarantee the PBT-before-CC dispatch gives).  With an unheld product the set
    // stays empty and all three bodies are unchanged.
    reactToPbHold_(ctx);
    // With the step translation armed, it replaces the whole reaction for declared
    // and undeclared steps alike, so it is dispatched before the declared-step carry.
    // That precedence is what gives the once-only guarantee when both knobs are
    // on: one seam reacts per epoch, it visits each (sat, signal) key once, and
    // PbStepTranslateLedger::react makes at most one shiftAmbiguity call per cell.
    // carryOnPhaseBiasDisc_ is then never entered, so a declared step cannot be
    // translated twice.
    if (config_.ssrPhaseBiasStepTranslate()) { translatePhaseBiasStep_(ctx); return; }
    // With the declared-step carry armed, the whole reaction is the carry seam;
    // everything below is the original reset path, verbatim.
    if (config_.ssrPhaseBiasDiscCarry()) { carryOnPhaseBiasDisc_(ctx); return; }

    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto& ssr = ssrStore();
    const double reset_std = config_.getAmbiguityStd();
    const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
    // Stale hold (A5): probe with the same effective limit the apply site uses
    // (hold_factor × max_age), so the flip detector fires when the applied
    // ssr_bias_term steps: once per real outage, not once per provider-update
    // cycle hovering near max_age.
    const double bias_max_age  = config_.getSsrBiasMaxAge()
                               * config_.getSsrBiasStaleHoldFactor();
    const bool   presence_on_hold = config_.ssrPbHoldPresenceFlip();
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key = obs.satellite + "|" + code;
            // A key the hold seam claimed skips the discontinuity-counter reaction,
            // which the hold owns.  With processing.ssr_pb_hold_presence_flip
            // off it skips the A5 edge below as well; armed, that edge runs as on an
            // unheld key.
            const bool claimed = ctx.pb_hold_handled.count(key) > 0;
            if (claimed && !presence_on_hold) continue;
            if (!claimed) {
                if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            key, pb->discontinuity_counter)) {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        noteConsumerSatEvent_(key, "reset_disc");   // consumer satellite trace (no-op unarmed)
                        noteAmbLevelDisturbed_(obs.satellite);      // ambiguity level check (no-op unarmed)
                        log->info("{}SSR phase-bias discontinuity: reset {} {}",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
            }
            // With the bias staleness gate on, a phase bias that goes stale (age-gated
            // out) mid-arc and later returns steps the applied ssr_bias_term by the
            // full bias with no producer disc-counter change; the ambiguity would
            // absorb the jump unannounced and the next AR would fix a wrong integer.
            // Track whether the bias is currently applied (fresh within the gate) and
            // reset the ambiguity on any fresh↔stale flip, so the step re-floats.
            // With the gate off there are no flips, so the output is byte-identical
            // (A5, P1-4).
            if (bias_age_gate) {
                const bool applied_now =
                    static_cast<bool>(ssr.getPhaseBias(obs.satellite, code, obs.time, bias_max_age));
                const auto it = ssr_phase_bias_applied_.find(key);
                if (it != ssr_phase_bias_applied_.end() && it->second != applied_now) {
                    // The flip is recorded as an ARC row naming the satellite, the
                    // signal, both sides of the edge and the epoch, and attributed
                    // against the producer's declared roster and the family's
                    // presence on the wire.  The attribution also decides the
                    // response: a signal the network declares no family for is
                    // structurally absent, not stale, so resetting its ambiguity
                    // would re-float on a non-event.  Suppression needs
                    // network.closed_loop.roster_latch and a declaration, so every
                    // earlier wire keeps the existing reaction unchanged.
                    const bool suppress = noteFreshnessFlip_(
                        key, obs.satellite, code, PppUtils::getSystemString(obs.system),
                        "phase", it->second, applied_now,
                        static_cast<bool>(ssr.getPhaseBias(obs.satellite, code)),
                        obs.time);
                    countPbHoldPresenceEdge_(key, applied_now, suppress);
                    if (suppress) {
                        ++roster_consumer_.flip_suppressed;
                        log->info("{}SSR phase-bias freshness flip ({} -> {}) on {} {}: NOT a "
                                  "staleness: the producer declares no phase-bias family for "
                                  "this signal, so the ambiguity is left alone (card SID 5b)",
                                  epoch_prefix, it->second ? "fresh" : "stale",
                                  applied_now ? "fresh" : "stale", obs.satellite, code);
                    } else {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        noteConsumerSatEvent_(key, "reset_flip");   // consumer satellite trace (no-op unarmed)
                        noteAmbLevelDisturbed_(obs.satellite);      // ambiguity level check (no-op unarmed)
                        log->info("{}SSR phase-bias freshness flip ({} -> {}): reset {} {}",
                                  epoch_prefix, it->second ? "fresh" : "stale",
                                  applied_now ? "fresh" : "stale", obs.satellite, code);
                    }
                }
                ssr_phase_bias_applied_[key] = applied_now;
            }
        }
    }
}

/**
 * @brief SSR phase-bias discontinuity / freshness flip -> ambiguity carry
 *        (amb -= Δ), the knob-on counterpart of resetOnPhaseBiasDisc.
 *
 * New code, not a relocation.  The per-key sweep matches the reset reaction
 * (the raw phase signals of every modeled satellite, the same ungated read for
 * the disc counter and the same gated (hold_factor × max_age) read for the A5
 * applied flag) and adds the value the measurement model applies this epoch,
 * taken with the same read the model makes (MeasurementModel.cpp ssr_bias_term:
 * gated ⇒ getPhaseBias(sat, code, obs.time, max_age); ungated otherwise; 0 when
 * it yields nothing).  Δ = applied_now − applied_prev is one translation per
 * event, whether the event is a counter bump, a flip, or both in the same epoch.
 * PbDiscCarryLedger::react decides and mutates; a refusal takes the existing reset.
 *
 * Update site: the ledger's record() runs at the tail of each key's iteration,
 * after the reaction.  This epoch's Δ is therefore taken against the value the
 * model applied at the previous update, and the next epoch's Δ against the
 * value the update after this reaction is about to apply.  The A5 flag map is
 * refreshed at the same site with the same value the reset path would write.
 */
void RealTimeEngine::carryOnPhaseBiasDisc_(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto& ssr = ssrStore();
    const double reset_std = config_.getAmbiguityStd();
    const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
    const double bias_max_age  = config_.getSsrBiasMaxAge()
                               * config_.getSsrBiasStaleHoldFactor();
    const bool basis_nlwl = (config_.getStateAmbiguityBasis() == StateAmbiguityBasis::NlWl);
    const bool bssd       = config_.enableBetweenSatelliteSd();
    const bool presence_on_hold = config_.ssrPbHoldPresenceFlip();
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key     = obs.satellite + "|" + code;
            // A claimed key: the hold owns its counter; armed, its A5 edge
            // is carried here as an unheld key's is.
            const bool claimed = ctx.pb_hold_handled.count(key) > 0;
            if (claimed && !presence_on_hold) continue;
            const std::string amb_key = obs.satellite + "_" + code;

            // The disc-counter bump: the ungated read, exactly as the reset path.
            bool bump = false;
            if (!claimed)
                if (auto pb = ssr.getPhaseBias(obs.satellite, code))
                    bump = state_manager_->noteSsrDiscontinuity(key, pb->discontinuity_counter);

            // The value the model applies this epoch (the model's own read).
            const auto applied = bias_age_gate
                ? ssr.getPhaseBias(obs.satellite, code, obs.time, bias_max_age)
                : ssr.getPhaseBias(obs.satellite, code);
            const bool   applied_now   = static_cast<bool>(applied);
            const double applied_now_m = applied ? applied->bias_m : 0.0;

            // The A5 freshness flip, on the same flag memory the reset path keeps
            // (and the RT snapshot restores); only tracked under the gate.
            bool flip = false;
            if (bias_age_gate) {
                const auto it = ssr_phase_bias_applied_.find(key);
                flip = (it != ssr_phase_bias_applied_.end() && it->second != applied_now);
            }
            if (flip) countPbHoldPresenceEdge_(key, applied_now, false);

            const auto d = pb_disc_carry_.react(*state_manager_, key, amb_key, bump, flip,
                                                applied_now_m, basis_nlwl, bssd, reset_std);
            // Any action other than None wrote this key (a carry shifts the value, a
            // refusal resets it), so the level window for this satellite restarts
            //.
            if (d.action != PbDiscCarryLedger::Action::None)
                noteAmbLevelDisturbed_(obs.satellite);          // ambiguity level check (no-op unarmed)
            using A = PbDiscCarryLedger::Action;
            switch (d.action) {
                case A::None: break;
                case A::CarriedDisc:
                    noteConsumerSatEvent_(key, "carry_disc");   // consumer satellite trace (no-op unarmed)
                    log->info("{}SSR phase-bias discontinuity: carried {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::CarriedFlip:
                    noteConsumerSatEvent_(key, "carry_flip");   // consumer satellite trace (no-op unarmed)
                    log->info("{}SSR phase-bias freshness flip ({} -> {}): carried {} {} by "
                              "{:+.3f} m", epoch_prefix, applied_now ? "stale" : "fresh",
                              applied_now ? "fresh" : "stale", obs.satellite, code,
                              -d.delta_m);
                    break;
                default:
                    // A refused carry took the reset; the event names the refusal so
                    // the row differs from a plain declared reset.
                    noteConsumerSatEvent_(key, PbDiscCarryLedger::actionName(d.action));
                    log->info("{}SSR phase-bias {}: reset {} {} ({} — carry refused)",
                              epoch_prefix, bump ? "discontinuity" : "freshness flip",
                              obs.satellite, code, PbDiscCarryLedger::actionName(d.action));
                    break;
            }

            // Update site (after the reaction): the value the coming update applies.
            pb_disc_carry_.record(key, applied_now_m);
            if (bias_age_gate) ssr_phase_bias_applied_[key] = applied_now;
        }
    }
}

/**
 * @brief Every phase-bias product step, declared or not -> ambiguity translation.
 *        Replaces resetOnPhaseBiasDisc / carryOnPhaseBiasDisc_.
 *
 * New code, not a relocation.  The per-key sweep matches the reset and carry
 * reactions (the raw phase signals of every modeled satellite, the same ungated
 * read for the disc counter and the same gated (hold_factor × max_age) read for
 * the A5 applied flag), with one difference: the reaction keys on the step in
 * the applied value, not on the declaration.
 *
 * Applied-value guard: Δ is formed from the value the measurement model
 * applies, taken here with the model's own read (the `ssr_bias_term` block in
 * MeasurementModel.cpp buildJacobianRow: gated ⇒ getPhaseBias(sat, code,
 * obs.time, max_age), ungated otherwise, 0 when it yields nothing).  The
 * previous side of the difference is this ledger's record of that same read at
 * the last epoch, never a fresh re-read of the product, so an interpolation
 * change, a provider update landing mid-epoch or a staleness hold cannot make
 * the translation disagree with the step the phase row took.
 *
 * Update site: the ledger's record() runs at the tail of each key's iteration,
 * after the reaction.  This epoch's Δ is therefore taken against the value the
 * model applied at the previous update, and the next epoch's Δ against the
 * value the update after this reaction is about to apply.  The A5 flag map is
 * refreshed at the same site with the same value the reset path would write,
 * and the disc counter is advanced by the same noteSsrDiscontinuity call, so
 * flipping the knob mid-lineage cannot leave either memory stale.
 */
void RealTimeEngine::translatePhaseBiasStep_(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto& ssr = ssrStore();
    const double reset_std = config_.getAmbiguityStd();
    const double eps_m     = config_.getSsrPhaseBiasStepTranslateEpsM();
    const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
    const double bias_max_age  = config_.getSsrBiasMaxAge()
                               * config_.getSsrBiasStaleHoldFactor();
    const bool basis_nlwl = (config_.getStateAmbiguityBasis() == StateAmbiguityBasis::NlWl);
    const bool bssd       = config_.enableBetweenSatelliteSd();
    const bool presence_on_hold = config_.ssrPbHoldPresenceFlip();
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key     = obs.satellite + "|" + code;
            // A claimed key: the hold owns its counter; armed, its A5 edge
            // is translated here as an unheld key's step is.
            const bool claimed = ctx.pb_hold_handled.count(key) > 0;
            if (claimed && !presence_on_hold) continue;
            const std::string amb_key = obs.satellite + "_" + code;

            // The disc-counter bump: the ungated read, exactly as the reset path.
            bool bump = false;
            if (!claimed)
                if (auto pb = ssr.getPhaseBias(obs.satellite, code))
                    bump = state_manager_->noteSsrDiscontinuity(key, pb->discontinuity_counter);

            // The value the model applies this epoch (the model's own read).
            const auto applied = bias_age_gate
                ? ssr.getPhaseBias(obs.satellite, code, obs.time, bias_max_age)
                : ssr.getPhaseBias(obs.satellite, code);
            const bool   applied_now   = static_cast<bool>(applied);
            const double applied_now_m = applied ? applied->bias_m : 0.0;

            // The A5 freshness flip, on the same flag memory the reset path keeps
            // (and the RT snapshot restores); only tracked under the gate.
            bool flip = false;
            if (bias_age_gate) {
                const auto it = ssr_phase_bias_applied_.find(key);
                flip = (it != ssr_phase_bias_applied_.end() && it->second != applied_now);
            }
            // Under a hold the applied value is constant within a generation, and the
            // hold seam already re-floated a generation change, so for a claimed key
            // only the presence edge is translated; any other epoch is recorded only
            //.
            if (claimed && !flip) {
                pb_step_translate_.record(key, applied_now, applied_now_m);
                if (bias_age_gate) ssr_phase_bias_applied_[key] = applied_now;
                continue;
            }
            if (flip) countPbHoldPresenceEdge_(key, applied_now, false);

            const auto d = pb_step_translate_.react(*state_manager_, key, amb_key,
                                                    bump || flip, applied_now, applied_now_m,
                                                    basis_nlwl, bssd, eps_m, reset_std);
            // Every action except None, SkippedEps and RefusedUndeclared wrote this
            // key (a translation shifts the value, a declared refusal resets it), so
            // the level window for this satellite restarts.  The three that write
            // nothing leave the window alone, so an empty declaration does not cost
            // the check a whole window.
            if (d.action != PbStepTranslateLedger::Action::None
                && d.action != PbStepTranslateLedger::Action::SkippedEps
                && d.action != PbStepTranslateLedger::Action::RefusedUndeclared)
                noteAmbLevelDisturbed_(obs.satellite);          // ambiguity level check (no-op unarmed)
            using A = PbStepTranslateLedger::Action;
            switch (d.action) {
                case A::None:
                case A::SkippedEps:
                    break;                                       // nothing written
                case A::TranslateDeclared:
                    noteConsumerSatEvent_(key, "translate_disc");   // consumer satellite trace (no-op unarmed)
                    log->info("{}SSR phase-bias DECLARED step: translated {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::TranslateUndeclared:
                    noteConsumerSatEvent_(key, "translate_undeclared");  // consumer satellite trace (no-op unarmed)
                    log->info("{}SSR phase-bias UNDECLARED step: translated {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::RefusedUndeclared:
                    // An undeclared step blocked by a refusal class: as on the
                    // original path, nothing is written, but the refusal is recorded
                    // as an event so it is not silent (invariant #4).
                    noteConsumerSatEvent_(key, PbStepTranslateLedger::actionName(d.action));
                    break;
                default:
                    // A declared cell the translation could not reach took the
                    // existing reset; the event names the refusal so the CST row
                    // differs from a plain declared reset.
                    noteConsumerSatEvent_(key, PbStepTranslateLedger::actionName(d.action));
                    log->info("{}SSR phase-bias declared step: reset {} {} ({} — translate "
                              "refused)", epoch_prefix, obs.satellite, code,
                              PbStepTranslateLedger::actionName(d.action));
                    break;
            }

            // Update site (after the reaction): the value the coming update applies.
            pb_step_translate_.record(key, applied_now, applied_now_m);
            if (bias_age_gate) ssr_phase_bias_applied_[key] = applied_now;
        }
    }
}

void RealTimeEngine::logPbStepTranslateSummary_(const char* tag) {
    if (!config_.ssrPhaseBiasStepTranslate()) return;   // knob off: never printed (byte-identical)
    const auto& c = pb_step_translate_.counters();
    log_manager_.getLogger("rt_engine")->info(
        "SSR phase-bias STEP translation ({}): cells {} (declared {}) -> "
        "n_pb_translate_declared {} / n_pb_translate_undeclared {}; "
        "sum_abs_translate_m {:.3f} (max |d| {:.3f} m); "
        "n_pb_translate_skipped_eps {} (of which declared-but-EMPTY {}); "
        "declared resets first_sight {} / basis {} / bssd {} / unknown_key {}; "
        "undeclared refused {}; no_pair {}; closure {}; {} keys recorded",
        tag, c.cells, c.declared_seen, c.translate_declared, c.translate_undeclared,
        c.sum_abs_translate_m, c.max_abs_translate_m, c.skipped_eps,
        c.skipped_eps_declared, c.reset_first_sight, c.reset_basis, c.reset_bssd,
        c.reset_unknown_key, c.refused_undeclared, c.no_pair,
        c.closureHolds() ? "HOLDS" : "BROKEN", pb_step_translate_.size());
}

/**
 * @brief Reaction to a held phase bias.
 *
 * New code, not a relocation.  Dispatched first inside @ref resetOnPhaseBiasDisc,
 * and keyed on the wire rather than on a knob: a consumer cannot know that the
 * producer armed @c network.emit_pb_hold, it can only see a nonzero
 * @c SsrPhaseBiasEntry::hold_generation.  Zero there means an unheld product (every
 * RTCM stream, every pre-PBH file, every run with the hold off) and this seam
 * handles nothing, so the three reactions after it are byte-identical.
 *
 * Under a hold the emitted value is constant within a generation, which changes
 * the meaning of both signals the original reactions key on:
 *
 *  - The discontinuity counter no longer implies a value step.  It is still read,
 *    so the per-key memory in @c StateManager advances and a key that later leaves
 *    the hold does not produce a spurious reset from a stale comparison, but the
 *    reaction is suppressed and counted: the hold exists so that the consumer
 *    does not react to the producer's discontinuity marks.
 *  - The generation becomes the declaration.  A change of a previously seen nonzero
 *    generation means the producer re-levelled this key, so the ambiguity re-floats
 *    through the existing reset path (@c StateManager::resetAmbiguity at
 *    @c getAmbiguityStd, the same call @c reset_disc uses) and notifies the ambiguity level check
 *    as that reset does.  A first sighting is not a change: there is no prior
 *    state that could have drifted against it.
 *
 * The A5 freshness flip is not handled or suppressed here: it is a step in the
 * applied correction (a bias gated out and back in), which the hold does not
 * touch.  Keys this seam handled are recorded in @c ctx.pb_hold_handled and skipped
 * downstream, so a held key is never visited twice in one epoch.
 *
 * The presence-edge fix corrects two premises above.  A first sighting has no prior state only
 * when the row is new: a held bias that appears on a phase row the consumer was
 * already using without it steps the applied correction by the whole bias, and so
 * does one that leaves the row.  And the A5 edge is reachable on the closed-loop
 * postures, because the harness arms the staleness gate on its derived consumer;
 * the downstream skip of a claimed key is what left both edges unanswered (the
 * wire's L1W family flips at the stations that track it, e.g. G32 L1W appearing at
 * 92.4 m).  With @c processing.ssr_pb_hold_presence_flip armed the downstream
 * reactions skip only a claimed key's counter reaction and run its A5 edge.
 */
void RealTimeEngine::reactToPbHold_(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto&  ssr       = ssrStore();
    const double reset_std = config_.getAmbiguityStd();
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            // The ungated read, exactly as the three reactions below make it.
            const auto pb = ssr.getPhaseBias(obs.satellite, code);
            if (!pb || pb->hold_generation == 0) continue;        // not held: unchanged path
            const std::string key = obs.satellite + "|" + code;
            ctx.pb_hold_handled.insert(key);

            // Consume the counter so StateManager's per-key memory tracks, then let
            // the ledger record that it was ignored.
            const bool disc_changed =
                state_manager_->noteSsrDiscontinuity(key, pb->discontinuity_counter);
            const auto d = pb_hold_.react(key, pb->hold_generation, disc_changed);
            if (d.action == PbHoldLedger::Action::Refloat) {
                state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                noteConsumerSatEvent_(key, "reset_hold_gen");   // consumer satellite trace (no-op unarmed)
                noteAmbLevelDisturbed_(obs.satellite);          // ambiguity level check (no-op unarmed)
                log->info("{}SSR phase-bias HOLD generation {} -> {}: reset {} {}",
                          epoch_prefix, d.previous_generation, pb->hold_generation,
                          obs.satellite, code);
            }
        }
    }
}

void RealTimeEngine::logPbHoldSummary_(const char* tag) {
    // Printed only when a held entry was seen, so a consumer reading an unheld
    // product is byte-identical.  A run that reports 0 cells here had an inert
    // hold on the producer side, however clean the wire reads.
    const auto& c = pb_hold_.counters();
    if (c.cells == 0) return;
    log_manager_.getLogger("rt_engine")->info(
        "SSR phase-bias HOLD ({}): cells {} over {} held key(s) -> first_sight {} / held {} / "
        "refloats {} (generation changes re-floated through the reset path); "
        "disc-counter changes IGNORED {} (no value step under a hold); closure {}",
        tag, c.cells, c.keys, c.first_sight, c.held, c.refloats, c.disc_ignored,
        c.closureHolds() ? "HOLDS" : "BROKEN");
}

void RealTimeEngine::countPbHoldPresenceEdge_(const std::string& key, bool applied_now,
                                              bool suppressed) {
    if (!config_.ssrPbHoldPresenceFlip() || !pb_hold_.recorded(key)) return;
    if (applied_now) ++pb_hold_presence_.appear; else ++pb_hold_presence_.vanish;
    if (suppressed) ++pb_hold_presence_.suppressed;
}

void RealTimeEngine::logPbHoldPresenceSummary(const char* tag) const {
    // Printed only when armed and a held entry was seen, so every other consumer
    // keeps a byte-identical log.  Armed with held keys and 0 edges means no held
    // bias appeared on, or left, a row in use: an inert knob on this wire.
    if (!config_.ssrPbHoldPresenceFlip() || pb_hold_.counters().cells == 0) return;
    const auto& c = pb_hold_presence_;
    log_manager_.getLogger("rt_engine")->info(
        "[RT] SSR phase-bias HOLD presence edges ({}, card PBF): {} (appear {} / vanish {}) "
        "answered by the A5 reaction ({}), {} left alone by the declared roster",
        tag, c.appear + c.vanish, c.appear, c.vanish,
        config_.ssrPhaseBiasStepTranslate() ? "translate"
        : config_.ssrPhaseBiasDiscCarry()   ? "carry" : "reset",
        c.suppressed);
}

void RealTimeEngine::logPbDiscCarrySummary_(const char* tag) {
    if (!config_.ssrPhaseBiasDiscCarry()) return;   // knob off: never printed (byte-identical)
    const auto& c = pb_disc_carry_.counters();
    log_manager_.getLogger("rt_engine")->info(
        "SSR phase-bias declared-step carry ({}): events {} (disc bumps {} + freshness flips {}) "
        "-> pb_disc_carried {} / pb_flip_carried {}; pb_disc_reset_first_sight {} / "
        "pb_disc_reset_basis {} / pb_disc_reset_bssd {} / pb_disc_reset_unknown_key {}; "
        "closure carried + resets == events: {}; {} keys recorded",
        tag, c.events, c.disc_bumps, c.flips, c.disc_carried, c.flip_carried,
        c.reset_first_sight, c.reset_basis, c.reset_bssd, c.reset_unknown_key,
        c.closureHolds() ? "HOLDS" : "BROKEN", pb_disc_carry_.size());
}

/**
 * @brief SSR code-bias datum re-reference -> receiver code-bias reinflation.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1766-1868); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::reinflateOnCodeBiasDisc(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (config_.estimateIfb()) {
        const auto& ssr = ssrStore();
        const double cb_var =
            config_.codeBiasReinflateStd() * config_.codeBiasReinflateStd();
        const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
        const double bias_max_age  = config_.getSsrBiasMaxAge()
                                   * config_.getSsrBiasStaleHoldFactor();
        std::set<std::string> reref_systems;   // system-name prefixes whose datum stepped
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.modeled) {
                const std::string& code = kv.first;
                if (code.empty() || code[0] != 'C') continue;          // code obs only
                if (code.find('_') != std::string::npos) continue;      // raw signal only
                if (auto c = ssr.getCodeBiasDiscontinuity(obs.satellite, code)) {
                    if (state_manager_->noteSsrDiscontinuity(
                            "CB|" + obs.satellite + "|" + code, *c)) {
                        reref_systems.insert(PppUtils::getSystemString(obs.system));
                        // The rx code-bias re-inflation is constellation-scoped, but
                        // the declaration that triggered it belongs to this (sat,
                        // signal), so the event is recorded there (no-op
                        // unarmed).
                        noteConsumerSatEvent_(obs.satellite + "|" + code, "reinflate_disc");
                        log->info("{}SSR code-bias discontinuity: {} {} (datum re-reference)",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
                // Code analogue of the phase flip (A5, P1-4): with the bias
                // staleness gate on, a code bias that goes stale, or vanishes from
                // a wholesale-replacing producer push (no disc counter travels for
                // an absent signal), steps the applied ssr_bias_term by the full
                // bias with no counter change.  The near-frozen receiver code-bias
                // state would carry that step for many epochs, so a fresh↔absent
                // flip is treated like a datum re-reference and re-inflates.  With
                // the gate off there are no flips, so the output is byte-identical.
                if (bias_age_gate) {
                    const bool applied_now =
                        ssr.hasCodeBias(obs.satellite, code, obs.time, bias_max_age);
                    const std::string fkey = "CB|" + obs.satellite + "|" + code;
                    const auto it = ssr_code_bias_applied_.find(fkey);
                    if (it != ssr_code_bias_applied_.end() && it->second != applied_now) {
                        // Record and attribute the flip.  The code-bias dropouts of
                        // the multi-site consumer arrive through this edge, and the re-inflation it
                        // drives is an unbounded decoupling edge.  A signal the
                        // producer declares no family for is structurally absent, not
                        // stale, so its disappearance is not a datum re-reference.
                        const bool suppress = noteFreshnessFlip_(
                            fkey, obs.satellite, code,
                            PppUtils::getSystemString(obs.system), "code",
                            it->second, applied_now,
                            ssr.hasCodeBias(obs.satellite, code), obs.time);
                        if (suppress) {
                            ++roster_consumer_.flip_suppressed;
                            log->info("{}SSR code-bias freshness flip ({} -> {}) on {} {}: NOT a "
                                      "datum re-reference: the producer declares no code-bias "
                                      "family for this signal, so the receiver code-bias states "
                                      "are left alone (card SID 5b)",
                                      epoch_prefix, it->second ? "applied" : "absent",
                                      applied_now ? "applied" : "absent", obs.satellite, code);
                        } else {
                            reref_systems.insert(PppUtils::getSystemString(obs.system));
                            noteConsumerSatEvent_(obs.satellite + "|" + code,
                                                  "reinflate_flip");   // consumer satellite trace (no-op unarmed)
                            log->info("{}SSR code-bias freshness flip ({} -> {}): {} {} "
                                      "(re-inflating rx code-bias states)",
                                      epoch_prefix, it->second ? "applied" : "absent",
                                      applied_now ? "applied" : "absent",
                                      obs.satellite, code);
                        }
                    }
                    ssr_code_bias_applied_[fkey] = applied_now;
                }
            }
        }
        // Re-inflate every receiver code-bias state of each re-referenced
        // constellation (keys "<sys>_DCB" / "<sys>_IFB<band>").  The scope is the
        // constellation rather than the band so that it reaches the reference-band
        // DCB without a fragile signal->band->state-key mapping.  The receiver
        // clock cannot absorb that DCB step: the clock is shared across
        // constellations and across code and phase, so steering it injects a
        // transient into every other residual.  The few extra IFB/DCB states the
        // wider scope touches re-converge within an epoch or two from informative
        // code under the modest re-inflation variance.
        // Per-satellite scoping is not possible here (T3g): the receiver code-bias
        // states are per (system, band), and there is no per-satellite receiver
        // code-bias state.  A satellite's code bias sits on the satellite side and is
        // already per satellite in the SSR product; this reaction re-inflates the
        // receiver states that absorb the datum move.  A per-band scope could miss
        // the reference-band DCB described above, so it is not used.  The
        // per-satellite scoping that applies (the emitted satellite discontinuity)
        // lives on the producer side (markSatDiscontinuity).
        for (const auto& [key, idx] : state_manager_->getCodeBiasStates()) {
            (void)idx;
            for (const std::string& sys : reref_systems) {
                if (key.rfind(sys + "_", 0) == 0) {
                    state_manager_->reinflateCodeBiasVariance(key, cb_var);
                    break;
                }
            }
        }
    } else {
        // With estimate_ifb off there are no receiver code-bias states to absorb a
        // network code-bias datum re-reference.  For a standalone consumer of a network
        // product that emits satellite code biases this is a misconfiguration that would
        // otherwise go unreported (the closed-loop consumer inherits estimate_ifb=true
        // from the producer, so it is not affected).  Warn once if the stream carries a
        // network code-bias discontinuity counter (A5 part b).
        if (!warned_ssr_cb_no_ifb_) {
            const auto& ssr = ssrStore();
            for (const auto& obs : modeled_obs) {
                bool hit = false;
                for (const auto& kv : obs.modeled) {
                    const std::string& code = kv.first;
                    if (code.empty() || code[0] != 'C' || code.find('_') != std::string::npos) continue;
                    if (ssr.getCodeBiasDiscontinuity(obs.satellite, code)) { hit = true; break; }
                }
                if (hit) {
                    log->warn("{}Consuming NETWORK satellite code biases but estimation.estimate_ifb "
                              "is OFF — no receiver code-bias state exists to absorb a code-bias datum "
                              "re-reference; enable estimate_ifb for a standalone network-code-bias "
                              "consumer (the closed-loop consumer inherits it automatically).",
                              epoch_prefix);
                    warned_ssr_cb_no_ifb_ = true;
                    break;
                }
            }
        }
    }
}

/**
 * @brief SSR clock datum re-reference -> receiver-clock note + scoped ISB re-inflation (A2).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1889-1943); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::reinflateOnClockDisc(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto& ssr = ssrStore();
    const double isb_std = config_.clockDiscIsbReinflateStd();
    std::set<std::string> reref_isb;   // ISB system names whose clock datum stepped
    std::set<std::string> present_isb; // all non-reference ISBs observed this epoch
    bool reference_stepped = false;    // a reference-constellation (GPS) clock re-referenced
    for (const auto& obs : modeled_obs) {
        if (isb_std > 0.0) {
            const std::string sn = StateInitializer::systemToIsbName(obs.system);
            if (!sn.empty()) present_isb.insert(sn);
        }
        if (auto c = ssr.getClockDiscontinuity(obs.satellite)) {
            if (state_manager_->noteSsrDiscontinuity("SC|" + obs.satellite, *c)) {
                // The clock declaration applies to this satellite.  The reaction
                // below is necessarily constellation-scoped (one receiver clock, one
                // ISB per constellation; see the T3g note below), so the row records
                // that the declaration arrived and that the consumer's only possible
                // response fired.
                noteConsumerSatEvent_(obs.satellite + "|CLK", "reinflate_isb");
                log->info("{}SSR clock discontinuity: {} (datum re-reference)",
                          epoch_prefix, obs.satellite);
                // A receiver clock estimated fresh each epoch (white noise)
                // re-absorbs the common-mode part with no action; the ISB
                // re-inflation below handles the differential part.
                // The clock discontinuity is detected per satellite
                // (getClockDiscontinuity(obs.satellite)), but the receiver-side
                // reaction cannot be per satellite: there is one ISB per
                // constellation and one shared receiver clock, with no per-satellite
                // receiver clock or ISB state.  The constellation scope below is the
                // finest granularity available (T3g).
                if (isb_std > 0.0) {
                    const std::string sys_name =
                        StateInitializer::systemToIsbName(obs.system);
                    if (!sys_name.empty()) {
                        // Non-reference constellation stepped: the differential
                        // lands on its own ISB, so only that one is re-inflated.
                        reref_isb.insert(sys_name);
                    } else {
                        // Reference constellation (GPS) re-referenced: the receiver
                        // clock absorbs the common mode, but the step lands
                        // differentially on every non-reference ISB (each is defined
                        // relative to the GPS datum).  systemToIsbName(GPS) is "", so
                        // scoping to it would re-inflate nothing and leave every
                        // Galileo/GLONASS/BeiDou ISB carrying the step for many
                        // epochs, which is the GPS-to-non-reference offset this
                        // handler exists to correct.  Mark the reference step; the
                        // ISBs are re-inflated below.
                        reference_stepped = true;
                    }
                }
            }
        }
    }
    // On a reference re-reference, every allocated non-reference ISB is re-inflated,
    // observed this epoch or not.  The ISB is a difference of two clock datums, and the
    // declaration is consumed once per satellite, so an ISB whose constellation is absent
    // from the step epoch would otherwise return stale by the whole step.
    const std::set<std::string> isbs =
        isbReinflationSet(reference_stepped, reref_isb,
                          state_manager_->getSystemTimeOffsetStates());
    if (reference_stepped && !isbs.empty())
        log->info("{}Reference (GPS) clock re-reference — re-inflating {} "
                  "non-reference ISB variance(s) for the differential step ({} observed "
                  "this epoch; every allocated ISB is re-inflated).",
                  epoch_prefix, isbs.size(), present_isb.size());
    for (const std::string& sys : isbs)
        state_manager_->reinflateSystemTimeOffsetVariance(sys, isb_std * isb_std);
}

void RealTimeEngine::logReinflateSummary(const char* tag) const {
    if (!state_manager_) return;
    log_manager_.getLogger("rt_engine")->info(
        "[RT] card OWD re-inflations ({}): {}", tag,
        StateManager::reinflateSummary(state_manager_->reinflateCounters()));
    if (config_.ssrLevelStepInflate())
        log_manager_.getLogger("rt_engine")->info(
            "[RT] card CLV2 declared level steps ({}): {} (satellite, epoch) above the dead "
            "band, {} answered along the phase-blind direction, {} refused; mean sigma "
            "{:.4f} m, max {:.4f} m, scale k {:.2f}.",
            tag, level_step_events_, level_step_inflated_, level_step_refused_,
            level_step_inflated_ > 0
                ? level_step_sigma_sum_ / static_cast<double>(level_step_inflated_) : 0.0,
            level_step_sigma_max_, config_.ssrLevelStepInflateK());
}

void RealTimeEngine::reactToDeclaredLevelSteps_(RtEpochContext& ctx) {
    if (!config_.ssrLevelStepInflate() || !state_manager_) return;
    constexpr double kDeadBandM = 1e-4;
    const auto& ssr = ssrStore();
    // This epoch's raw code and phase signals per satellite (combinations skipped).
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> sigs;
    for (const auto& obs : ctx.modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.size() != 3) continue;
            if (code[0] == 'C')      sigs[obs.satellite].first.insert(code);
            else if (code[0] == 'L') sigs[obs.satellite].second.insert(code);
        }
    }
    std::vector<std::pair<std::vector<std::pair<int, double>>, double>> inflations;
    for (const auto& [sat, cp] : sigs) {
        const auto cur = ssr.getDeclaredLevelSteps(sat);
        const SsrCorrectionStore::DeclaredLevelSteps now =
            cur ? *cur : SsrCorrectionStore::DeclaredLevelSteps{};
        auto it = level_steps_seen_.find(sat);
        if (it == level_steps_seen_.end()) {   // the first reading only records
            level_steps_seen_.emplace(sat, now);
            continue;
        }
        const double d_clock = now.clock_m - it->second.clock_m;
        double sigma = 0.0;
        for (const std::string& c : cp.first) {
            double d_bias = 0.0;
            if (const auto a = now.bias_m.find(c); a != now.bias_m.end()) d_bias += a->second;
            if (const auto b = it->second.bias_m.find(c); b != it->second.bias_m.end())
                d_bias -= b->second;
            sigma = std::max(sigma, std::abs(-d_clock + d_bias));
        }
        it->second = now;
        if (!(sigma > kDeadBandM)) continue;
        ++level_step_events_;
        const int iono_idx = state_manager_->getIonosphereIndex(sat);
        if (iono_idx < 0 || !(state_manager_->getStateVariance(iono_idx) > 0.0)) {
            ++level_step_refused_;   // no ionosphere state, or a frozen (pivot) row
            continue;
        }
        std::vector<std::pair<int, double>> v;
        v.emplace_back(iono_idx, 1.0);
        for (const std::string& ph : cp.second) {
            const int ai = state_manager_->getAmbiguityIndex(StateKeys::makeAmbiguityKey(sat, ph));
            if (ai < 0) continue;
            const double lam = bandNominalWavelengthMeters(classifySignalForSat(sat, ph).band);
            if (!(lam > 0.0)) continue;
            const double f = Constants::SPEED_OF_LIGHT / lam;
            v.emplace_back(ai, (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ) / (f * f));
        }
        if (v.size() < 2) {   // without an ambiguity the direction is not phase-blind
            ++level_step_refused_;
            continue;
        }
        const double s = config_.ssrLevelStepInflateK() * sigma;
        inflations.emplace_back(std::move(v), s * s);
        ++level_step_inflated_;
        level_step_sigma_sum_ += sigma;
        level_step_sigma_max_  = std::max(level_step_sigma_max_, sigma);
        noteConsumerSatEvent_(sat + "|LVL", "level_step_inflate");
    }
    if (inflations.empty()) return;
    Eigen::MatrixXd P = state_manager_->getCovariance();
    for (const auto& [v, var] : inflations)
        for (const auto& [i, ci] : v)
            for (const auto& [j, cj] : v) P(i, j) += var * ci * cj;
    state_manager_->setCovariance(P);
}

std::set<std::string> RealTimeEngine::isbReinflationSet(
        bool reference_stepped, const std::set<std::string>& stepped,
        const std::map<std::string, size_t>& allocated) {
    std::set<std::string> out = stepped;   // a set: one entry per ISB, once per epoch
    if (reference_stepped)
        for (const auto& [sys, idx] : allocated) {
            (void)idx;
            out.insert(sys);
        }
    return out;
}

} // namespace GnssProcessor
