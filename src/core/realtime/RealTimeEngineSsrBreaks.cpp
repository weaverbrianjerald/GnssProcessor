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
#include <set>
#include <vector>

namespace GnssProcessor {

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
            if (auto pb = ssr.getPhaseBias(obs.satellite, code)) {
                if (state_manager_->noteSsrDiscontinuity(
                        key, pb->discontinuity_counter)) {
                    state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                    noteConsumerSatEvent_(key, "reset_disc");   // card CST (no-op unarmed)
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
                    state_manager_->resetAmbiguity(obs.satellite + "_" + code, reset_std);
                    noteConsumerSatEvent_(key, "reset_flip");   // card CST (no-op unarmed)
                    log->info("{}SSR phase-bias freshness flip ({} -> {}): reset {} {}",
                              epoch_prefix, it->second ? "fresh" : "stale",
                              applied_now ? "fresh" : "stale", obs.satellite, code);
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
                        reref_systems.insert(PppUtils::getSystemString(obs.system));
                        noteConsumerSatEvent_(obs.satellite + "|" + code,
                                              "reinflate_flip");   // card CST (no-op unarmed)
                        log->info("{}SSR code-bias freshness flip ({} -> {}): {} {} "
                                  "(re-inflating rx code-bias states)",
                                  epoch_prefix, it->second ? "applied" : "absent",
                                  applied_now ? "applied" : "absent",
                                  obs.satellite, code);
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
