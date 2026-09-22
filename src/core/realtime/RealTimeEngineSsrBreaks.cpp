/**
 * @file RealTimeEngineSsrBreaks.cpp
 * @brief SSR datum-break reactions for RealTimeEngine::processEpoch.
 *
 * Five handlers for "the network producer moved its datum under us".
 *
 * ORDER IS LOAD-BEARING: reactToGenerationChange applies the UNION of the
 * finer-grained reactions globally (a producer restart re-realizes every
 * bias and level at once, with disc counters re-seeded to 0), so it must
 * run before the per-signal handlers that would otherwise see 0->0 = "no
 * change".  All five are no-ops without SSR content, which is what keeps
 * live RTCM streams byte-identical.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"
#include "measurement/StateInitializer.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <fmt/format.h>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {
/// Card SID phase 5 (E3): cap on the ARC rows one consumer writes for freshness
/// flips, so a pathological wire cannot flood the log.  The counters carry the
/// full population either way.
constexpr long long kRosterFlipEventCap = 400;
}  // namespace

// ---------------------------------------------------------------------------
// Card SID phase 5: the DECLARED SIGNAL ROSTER at the consumer
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
    // The latch is decided INSIDE computeModels at a constellation's first epoch,
    // so the declaration has to reach the model before that call or it is too
    // late.  Handed over on every change (and on the first epoch), which also
    // means a mid-run change reaches a constellation that is not yet decided.
    if (changed || roster_consumer_.epochs == 1)
        ppp_model_->setDeclaredSignalRoster(ssr_declared_roster_,
                                            config_.consumerRosterLatch());

    if (declared_roster_logged_) return;
    // One line, once, in BOTH directions: an armed knob on a wire that carries no
    // declaration is INERT and has to say so (standing invariant 4), and a
    // declaration arriving at an unarmed consumer is the phase-5a observatory.
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
    //   2. it does, but the store holds no record for THIS satellite: the family
    //      exists and is absent here (card MSC's dropouts);
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
        // The identity the audit found missing at BOTH edges of this gate
        // (satellite, signal, from, to, epoch), on the ARC tier the card's other
        // lifecycle edges use.
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
    // C2: only the UNDECLARED class is suppressed, and only with the knob armed.
    return undeclared && config_.consumerRosterLatch();
}

const PppModel::DeclaredLatchCounters& RealTimeEngine::declaredLatchCounters() const {
    return ppp_model_->declaredLatchCounters();
}

void RealTimeEngine::logRosterConsumerSummary(const char* tag) const {
    const auto& r = roster_consumer_;
    const auto& d = ppp_model_->declaredLatchCounters();
    // Silent when no declaration was ever seen AND the knob is off: a consumer of
    // a pre-card wire prints nothing, so its log is byte-identical.
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
 * @brief RTA-3: solution-generation id change -> GLOBAL datum break.
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
            // Card CST: a GLOBAL reset — record it per ambiguity key so the rows
            // of that epoch carry it too (an undeclared-looking mass reset is
            // exactly what this instrument must not hide).  No-op unarmed.
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
 * @brief RTA-10: producer epoch-status heartbeat -> one-shot transition log.
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
    // Card PBH: the HELD keys are decided first, and on the DATA rather than on a
    // consumer knob — the consumer cannot know the producer armed
    // network.emit_pb_hold, it can only read SsrPhaseBiasEntry::hold_generation.
    // Every key it handles lands in ctx.pb_hold_handled and is skipped by whichever
    // of the three reactions below runs, so each key is visited exactly once per
    // epoch (the same once-only guarantee the PBT-before-CC dispatch gives).  With
    // an unheld product the set stays empty and all three bodies are unchanged.
    reactToPbHold_(ctx);
    // Card PBT (A): with the STEP translation armed it REPLACES the whole
    // reaction — declared and undeclared steps alike — so it is dispatched
    // before the card-CC carry.  That precedence IS the once-only guarantee
    // when both knobs are on: exactly one seam reacts per epoch, it visits each
    // (sat, signal) key exactly once, and PbStepTranslateLedger::react makes at
    // most one shiftAmbiguity call per cell.  carryOnPhaseBiasDisc_ is then
    // never entered, so a declared step cannot be translated twice.
    if (config_.ssrPhaseBiasStepTranslate()) { translatePhaseBiasStep_(ctx); return; }
    // Card CC (A): with the declared-step carry armed the whole reaction is the
    // carry seam below; everything from here on is the pre-card path, verbatim.
    if (config_.ssrPhaseBiasDiscCarry()) { carryOnPhaseBiasDisc_(ctx); return; }

    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    const auto& ssr = ssrStore();
    const double reset_std = config_.getAmbiguityStd();
    const bool   bias_age_gate = config_.enableSsrBiasMaxAge();
    // A5 stale-hold: probe with the SAME effective limit the apply site uses
    // (hold_factor × max_age), so the flip detector fires exactly when the
    // applied ssr_bias_term actually steps — once per genuine outage, not per
    // provider-update cycle hovering near max_age.
    const double bias_max_age  = config_.getSsrBiasMaxAge()
                               * config_.getSsrBiasStaleHoldFactor();
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key = obs.satellite + "|" + code;
            if (ctx.pb_hold_handled.count(key)) continue;         // card PBH reacted
            if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                if (state_manager_->noteSsrDiscontinuity(
                        key, pb->discontinuity_counter)) {
                    state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                    noteConsumerSatEvent_(key, "reset_disc");   // card CST (no-op unarmed)
                    noteAmbLevelDisturbed_(obs.satellite);      // card ALC (no-op unarmed)
                    log->info("{}SSR phase-bias discontinuity: reset {} {}",
                              epoch_prefix, obs.satellite, code);
                }
            }
            // A5 (P1-4): with the bias staleness gate ON, a phase bias that goes stale
            // (age-gated out) mid-arc and later returns steps the APPLIED ssr_bias_term
            // by a full bias — with NO producer disc-counter change — so the ambiguity
            // silently absorbs the jump and the next AR wrong-fixes.  Track whether the
            // bias is currently APPLIED (fresh within the gate); reset the ambiguity on
            // any fresh↔stale flip so the step re-floats instead of corrupting the fix.
            // Only when the gate is on ⇒ no flips otherwise ⇒ byte-identical.
            if (bias_age_gate) {
                const bool applied_now =
                    static_cast<bool>(ssr.getPhaseBias(obs.satellite, code, obs.time, bias_max_age));
                const auto it = ssr_phase_bias_applied_.find(key);
                if (it != ssr_phase_bias_applied_.end() && it->second != applied_now) {
                    // Card SID phase 5 (E3 / C2): the flip gets its identity here ,
                    // an ARC row naming the satellite, the signal, both sides of the
                    // edge and the epoch, and an attribution against the producer's
                    // DECLARED roster and the family's presence on the wire.  The
                    // answer also decides the response: a signal the network declares
                    // no family for is structurally absent, not stale, so resetting
                    // the ambiguity for it re-floats on a non-event.  Suppression
                    // needs network.closed_loop.roster_latch AND a declaration, so
                    // every pre-card wire keeps today's reaction exactly.
                    const bool suppress = noteFreshnessFlip_(
                        key, obs.satellite, code, PppUtils::getSystemString(obs.system),
                        "phase", it->second, applied_now,
                        static_cast<bool>(ssr.getPhaseBias(obs.satellite, code)),
                        obs.time);
                    if (suppress) {
                        ++roster_consumer_.flip_suppressed;
                        log->info("{}SSR phase-bias freshness flip ({} -> {}) on {} {}: NOT a "
                                  "staleness: the producer declares no phase-bias family for "
                                  "this signal, so the ambiguity is left alone (card SID 5b)",
                                  epoch_prefix, it->second ? "fresh" : "stale",
                                  applied_now ? "fresh" : "stale", obs.satellite, code);
                    } else {
                        state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                        noteConsumerSatEvent_(key, "reset_flip");   // card CST (no-op unarmed)
                        noteAmbLevelDisturbed_(obs.satellite);      // card ALC (no-op unarmed)
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
 * @brief Card CC (A): SSR phase-bias discontinuity / freshness flip -> ambiguity
 *        CARRY (amb -= Δ), the knob-ON twin of resetOnPhaseBiasDisc.
 *
 * New code (not a relocation).  Same per-key sweep as the reset reaction —
 * the raw phase signals of every modeled satellite, the same ungated read for
 * the disc counter and the same gated (hold_factor × max_age) read for the A5
 * applied flag — plus the one thing the carry needs: the VALUE the measurement
 * model applies this epoch, taken with the SAME read the model makes
 * (MeasurementModel.cpp ssr_bias_term: gated ⇒ getPhaseBias(sat, code, obs.time,
 * max_age); ungated otherwise; 0 when it yields nothing).  Δ = applied_now −
 * applied_prev is one translation per event whether the event is a counter
 * bump, a flip, or both in the same epoch.  PbDiscCarryLedger::react decides
 * and mutates; the refusals take the existing reset.
 *
 * UPDATE SITE: the ledger's record() runs at the tail of each key's iteration,
 * AFTER the reaction — so this epoch's Δ was against the value the model
 * applied at the previous update, and the next epoch's Δ will be against the
 * value the update below this reaction is about to apply.  The A5 flag map is
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
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key     = obs.satellite + "|" + code;
            if (ctx.pb_hold_handled.count(key)) continue;         // card PBH reacted
            const std::string amb_key = obs.satellite + "_" + code;

            // The disc-counter bump: the ungated read, exactly as the reset path.
            bool bump = false;
            if (auto pb = ssr.getPhaseBias(obs.satellite, code))
                bump = state_manager_->noteSsrDiscontinuity(key, pb->discontinuity_counter);

            // The value the model APPLIES this epoch (the model's own read).
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

            const auto d = pb_disc_carry_.react(*state_manager_, key, amb_key, bump, flip,
                                                applied_now_m, basis_nlwl, bssd, reset_std);
            // Card ALC: any non-None action WROTE this key (a carry shifts the value,
            // a refusal resets it), so the level window for this satellite restarts.
            if (d.action != PbDiscCarryLedger::Action::None)
                noteAmbLevelDisturbed_(obs.satellite);          // card ALC (no-op unarmed)
            using A = PbDiscCarryLedger::Action;
            switch (d.action) {
                case A::None: break;
                case A::CarriedDisc:
                    noteConsumerSatEvent_(key, "carry_disc");   // card CST (no-op unarmed)
                    log->info("{}SSR phase-bias discontinuity: carried {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::CarriedFlip:
                    noteConsumerSatEvent_(key, "carry_flip");   // card CST (no-op unarmed)
                    log->info("{}SSR phase-bias freshness flip ({} -> {}): carried {} {} by "
                              "{:+.3f} m", epoch_prefix, applied_now ? "stale" : "fresh",
                              applied_now ? "fresh" : "stale", obs.satellite, code,
                              -d.delta_m);
                    break;
                default:
                    // Card CST: a REFUSED carry took the reset; name the refusal so
                    // the row distinguishes it from a plain declared reset.
                    noteConsumerSatEvent_(key, PbDiscCarryLedger::actionName(d.action));
                    log->info("{}SSR phase-bias {}: reset {} {} ({} — carry refused)",
                              epoch_prefix, bump ? "discontinuity" : "freshness flip",
                              obs.satellite, code, PbDiscCarryLedger::actionName(d.action));
                    break;
            }

            // UPDATE SITE (after the reaction): what the update below will apply.
            pb_disc_carry_.record(key, applied_now_m);
            if (bias_age_gate) ssr_phase_bias_applied_[key] = applied_now;
        }
    }
}

/**
 * @brief Card PBT (A): EVERY phase-bias product step -> ambiguity TRANSLATION,
 *        declared or not.  Replaces resetOnPhaseBiasDisc / carryOnPhaseBiasDisc_.
 *
 * New code (not a relocation).  Same per-key sweep as the reset and carry
 * reactions — the raw phase signals of every modeled satellite, the same
 * ungated read for the disc counter and the same gated (hold_factor × max_age)
 * read for the A5 applied flag — with one change of subject: the reaction keys
 * on the STEP in the applied value, not on the declaration.
 *
 * ⚠ THE APPLIED-VALUE GUARD.  Δ is formed from the value the measurement model
 * ACTUALLY applies, taken here with the model's own read (MeasurementModel.cpp:
 * 1622-1624 `ssr_bias_term`: gated ⇒ getPhaseBias(sat, code, obs.time,
 * max_age), ungated otherwise, 0 when it yields nothing), and the previous side
 * of the difference is this ledger's RECORD of that same read at the last
 * epoch — never a fresh re-read of the product.  So an interpolation change, a
 * provider update landing mid-epoch or a staleness hold cannot make the
 * translation disagree with the step the phase row actually took.
 *
 * UPDATE SITE: the ledger's record() runs at the tail of each key's iteration,
 * AFTER the reaction — so this epoch's Δ was against the value the model
 * applied at the previous update, and the next epoch's Δ will be against the
 * value the update below this reaction is about to apply.  The A5 flag map is
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
    for (const auto& obs : modeled_obs) {
        for (const auto& kv : obs.modeled) {
            const std::string& code = kv.first;
            if (code.empty() || code[0] != 'L') continue;       // phase only
            if (code.find('_') != std::string::npos) continue;   // raw signal only
            const std::string key     = obs.satellite + "|" + code;
            if (ctx.pb_hold_handled.count(key)) continue;         // card PBH reacted
            const std::string amb_key = obs.satellite + "_" + code;

            // The disc-counter bump: the ungated read, exactly as the reset path.
            bool bump = false;
            if (auto pb = ssr.getPhaseBias(obs.satellite, code))
                bump = state_manager_->noteSsrDiscontinuity(key, pb->discontinuity_counter);

            // The value the model APPLIES this epoch (the model's own read).
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

            const auto d = pb_step_translate_.react(*state_manager_, key, amb_key,
                                                    bump || flip, applied_now, applied_now_m,
                                                    basis_nlwl, bssd, eps_m, reset_std);
            // Card ALC: every action but None / SkippedEps / RefusedUndeclared WROTE
            // this key (a translation shifts the value, a declared refusal resets it),
            // so the level window for this satellite restarts.  The three that write
            // nothing leave the window alone, which is what keeps an EMPTY declaration
            // from costing the check a whole window.
            if (d.action != PbStepTranslateLedger::Action::None
                && d.action != PbStepTranslateLedger::Action::SkippedEps
                && d.action != PbStepTranslateLedger::Action::RefusedUndeclared)
                noteAmbLevelDisturbed_(obs.satellite);          // card ALC (no-op unarmed)
            using A = PbStepTranslateLedger::Action;
            switch (d.action) {
                case A::None:
                case A::SkippedEps:
                    break;                                       // nothing written
                case A::TranslateDeclared:
                    noteConsumerSatEvent_(key, "translate_disc");   // card CST (no-op unarmed)
                    log->info("{}SSR phase-bias DECLARED step: translated {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::TranslateUndeclared:
                    noteConsumerSatEvent_(key, "translate_undeclared");  // card CST (no-op unarmed)
                    log->info("{}SSR phase-bias UNDECLARED step: translated {} {} by {:+.3f} m",
                              epoch_prefix, obs.satellite, code, -d.delta_m);
                    break;
                case A::RefusedUndeclared:
                    // An undeclared step a refusal class blocked: the pre-card path
                    // did nothing here and neither do we — but it is named, because
                    // a silent refusal is exactly what invariant #4 forbids.
                    noteConsumerSatEvent_(key, PbStepTranslateLedger::actionName(d.action));
                    break;
                default:
                    // A DECLARED cell the translation could not reach took the
                    // EXISTING reset; name the refusal so the CST row distinguishes
                    // it from a plain declared reset.
                    noteConsumerSatEvent_(key, PbStepTranslateLedger::actionName(d.action));
                    log->info("{}SSR phase-bias declared step: reset {} {} ({} — translate "
                              "refused)", epoch_prefix, obs.satellite, code,
                              PbStepTranslateLedger::actionName(d.action));
                    break;
            }

            // UPDATE SITE (after the reaction): what the update below will apply.
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
 * @brief Card PBH: the reaction to a HELD phase bias.
 *
 * New code (not a relocation).  Dispatched FIRST inside @ref resetOnPhaseBiasDisc,
 * and keyed on the wire rather than on a knob: a consumer cannot know the producer
 * armed @c network.emit_pb_hold, it can only see a nonzero
 * @c SsrPhaseBiasEntry::hold_generation.  Zero there means an unheld product (every
 * RTCM stream, every pre-PBH file, every OFF run) and this seam handles nothing, so
 * the three reactions below it are byte-identical.
 *
 * Under a hold the emitted value is a GENERATION CONSTANT, which inverts the
 * meaning of both signals the pre-card reactions key on:
 *
 *  - the DISCONTINUITY COUNTER no longer implies a value step.  It is still read,
 *    so @c StateManager's per-key memory advances and a key that later leaves the
 *    hold does not mint a spurious reset from a stale comparison — but the reaction
 *    is suppressed and COUNTED, which is the whole point of the contract: the
 *    producer's marks are what the hold was built to stop paying for.
 *  - the GENERATION becomes the declaration.  A change of a previously-seen nonzero
 *    generation means the producer re-levelled this key, so the ambiguity re-floats
 *    through the EXISTING reset path (@c StateManager::resetAmbiguity at
 *    @c getAmbiguityStd, the same call @c reset_disc uses) and notifies card ALC
 *    exactly as that reset does.  A FIRST sighting is not a change: there is no
 *    prior state that could have drifted against it.
 *
 * The A5 freshness flip is deliberately NOT handled here and NOT suppressed: it is
 * a step in the APPLIED correction (a bias gated out and back in), which the hold
 * does not touch.  It is also unreachable on the closed-loop postures, where
 * @c processing.enable_ssr_bias_max_age is off.  Keys this seam handled are recorded
 * in @c ctx.pb_hold_handled and skipped downstream, so a held key is never visited
 * twice in one epoch.
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
                noteConsumerSatEvent_(key, "reset_hold_gen");   // card CST (no-op unarmed)
                noteAmbLevelDisturbed_(obs.satellite);          // card ALC (no-op unarmed)
                log->info("{}SSR phase-bias HOLD generation {} -> {}: reset {} {}",
                          epoch_prefix, d.previous_generation, pb->hold_generation,
                          obs.satellite, code);
            }
        }
    }
}

void RealTimeEngine::logPbHoldSummary_(const char* tag) {
    // Printed only when a held entry was actually seen — a consumer reading an
    // unheld product is byte-identical, and a run that reports 0 cells here had an
    // INERT hold on the producer side no matter how clean the wire reads.
    const auto& c = pb_hold_.counters();
    if (c.cells == 0) return;
    log_manager_.getLogger("rt_engine")->info(
        "SSR phase-bias HOLD ({}): cells {} over {} held key(s) -> first_sight {} / held {} / "
        "refloats {} (generation changes re-floated through the reset path); "
        "disc-counter changes IGNORED {} (no value step under a hold); closure {}",
        tag, c.cells, c.keys, c.first_sight, c.held, c.refloats, c.disc_ignored,
        c.closureHolds() ? "HOLDS" : "BROKEN");
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
                        // Card CST: the rx code-bias re-inflation is CONSTELLATION-
                        // scoped, but the DECLARATION that triggered it is this
                        // (sat, signal) — record it there (no-op unarmed).
                        noteConsumerSatEvent_(obs.satellite + "|" + code, "reinflate_disc");
                        log->info("{}SSR code-bias discontinuity: {} {} (datum re-reference)",
                                  epoch_prefix, obs.satellite, code);
                    }
                }
                // A5 (P1-4) code analog: with the bias staleness gate ON, a code
                // bias that goes stale — or VANISHES from a wholesale-replacing
                // producer push (no disc counter travels for an absent signal) —
                // flips the applied ssr_bias_term by the full bias with no
                // counter change.  The near-frozen receiver code-bias state drags
                // that step for many epochs; treat a fresh↔absent flip exactly
                // like a datum re-reference and re-inflate.  Only when the gate
                // is on ⇒ no flips otherwise ⇒ byte-identical.
                if (bias_age_gate) {
                    const bool applied_now =
                        ssr.hasCodeBias(obs.satellite, code, obs.time, bias_max_age);
                    const std::string fkey = "CB|" + obs.satellite + "|" + code;
                    const auto it = ssr_code_bias_applied_.find(fkey);
                    if (it != ssr_code_bias_applied_.end() && it->second != applied_now) {
                        // Card SID phase 5 (E3 / C2): the identity and the attribution
                        // of this flip: card MSC's 50 code-bias dropouts are exactly
                        // this edge, and the re-inflation it drives is the unbounded
                        // decoupling edge that cost QUIN00USA 70 m.  A signal the
                        // producer declares no family for is structurally absent, not
                        // stale, so its "disappearance" is not a datum re-reference.
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
                                                  "reinflate_flip");   // card CST (no-op unarmed)
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
        // constellation (keys "<sys>_DCB" / "<sys>_IFB<band>").  Constellation-
        // scoped (not per-band) for robustness: it reaches the reference-band
        // DCB — which the receiver clock CANNOT self-heal (the clock is shared
        // across constellations and across code+phase, so steering it injects a
        // transient into every other residual) — without a fragile
        // signal->band->state-key mapping; the handful of extra IFB/DCB states
        // it touches re-converge within an epoch or two from informative code
        // under the modest re-inflation variance.
        // T3g FINDING (per-satellite scoping is not achievable here): the receiver
        // code-bias states this reaction touches are per-(system,band) — there is NO
        // per-satellite receiver code-bias state to narrow to (a satellite's code bias
        // is on the SATELLITE side, already per-sat in the SSR product; the reaction
        // re-inflates the RECEIVER states that absorb the datum move).  The finest
        // in-principle scope is per-band, but a per-band narrowing can MISS the
        // reference-band DCB the shared clock cannot self-heal (the very failure this
        // constellation scope avoids), so it is deliberately NOT taken.  The card's
        // "per-satellite" ask is therefore structurally inapplicable to the receiver
        // reaction; the per-sat scoping that IS meaningful (the emitted SATELLITE
        // discontinuity) already lives on the producer side (markSatDiscontinuity).
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
        // A5 part (b): estimate_ifb is off, so there are NO receiver code-bias states to
        // absorb a network code-bias datum re-reference.  For a STANDALONE consumer of a
        // network product that emits satellite code biases this is a silent misconfiguration
        // (the closed-loop consumer inherits estimate_ifb=true from the producer, so it is
        // safe).  Warn ONCE if the stream actually carries a network code-bias discontinuity
        // counter — turning the previously-silent no-op into a loud, actionable diagnostic.
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
    std::set<std::string> present_isb; // all NON-reference ISBs observed this epoch
    bool reference_stepped = false;    // a REFERENCE-constellation (GPS) clock re-referenced
    for (const auto& obs : modeled_obs) {
        if (isb_std > 0.0) {
            const std::string sn = StateInitializer::systemToIsbName(obs.system);
            if (!sn.empty()) present_isb.insert(sn);
        }
        if (auto c = ssr.getClockDiscontinuity(obs.satellite)) {
            if (state_manager_->noteSsrDiscontinuity("SC|" + obs.satellite, *c)) {
                // Card CST: the clock declaration landed for THIS satellite.  The
                // reaction below is necessarily constellation-scoped (one receiver
                // clock, one ISB per constellation — see the T3g finding), so the
                // row says "declared, and the consumer's only possible response
                // fired", which is the distinction the trace has to preserve.
                noteConsumerSatEvent_(obs.satellite + "|CLK", "reinflate_isb");
                log->info("{}SSR clock discontinuity: {} (datum re-reference)",
                          epoch_prefix, obs.satellite);
                // A receiver clock estimated fresh each epoch (white noise)
                // re-absorbs the common-mode part with no action; the ISB
                // re-inflation below handles the differential part.
                // T3g FINDING: the clock discontinuity is DETECTED per-satellite
                // (getClockDiscontinuity(obs.satellite)), but the receiver-side
                // REACTION cannot be per-satellite — there is ONE ISB per
                // constellation and ONE shared receiver clock, no per-sat receiver
                // clock/ISB state.  So "per-satellite scoping" of this reaction is
                // structurally impossible; the constellation scope below is already
                // the finest correct granularity.
                if (isb_std > 0.0) {
                    const std::string sys_name =
                        StateInitializer::systemToIsbName(obs.system);
                    if (!sys_name.empty()) {
                        // Non-reference constellation stepped: the differential
                        // lands on its OWN ISB — scoped re-inflation of that one.
                        reref_isb.insert(sys_name);
                    } else {
                        // REFERENCE constellation (GPS) re-referenced: the receiver
                        // clock absorbs the common mode, but the step lands
                        // DIFFERENTIALLY on EVERY non-reference ISB (each is defined
                        // relative to the GPS datum).  systemToIsbName(GPS) is "", so
                        // scoping to it would re-inflate nothing and leave every
                        // Galileo/GLONASS/BeiDou ISB dragging for many epochs — the
                        // exact GPS-vs-non-ref seam this handler exists to cure.  Mark
                        // the reference step and re-inflate all present ISBs below.
                        reference_stepped = true;
                    }
                }
            }
        }
    }
    if (reference_stepped) {
        reref_isb.insert(present_isb.begin(), present_isb.end());
        if (!present_isb.empty())
            log->info("{}Reference (GPS) clock re-reference — re-inflating {} "
                      "non-reference ISB variance(s) for the differential step.",
                      epoch_prefix, present_isb.size());
    }
    for (const std::string& sys : reref_isb)
        state_manager_->reinflateSystemTimeOffsetVariance(sys, isb_std * isb_std);
}

} // namespace GnssProcessor
