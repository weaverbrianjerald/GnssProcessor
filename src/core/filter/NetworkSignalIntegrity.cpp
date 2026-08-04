/**
 * @file NetworkSignalIntegrity.cpp
 * @brief Flicker/gap signal integrity — the band-state classifier, the unvetted
 *        phase demotion and the regain refloat.  See the header for the
 *        epistemics, the measured case and the ordering contract.
 */

#include "filter/NetworkSignalIntegrity.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"   // GnssTime (event stamps)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace GnssProcessor {

namespace {

/// Drop a LOS record that has not been seen for this many PROCESSED epochs.  Purely
/// a memory bound (the key set is stations x satellites, ~12 k at 305 stations, so
/// this never binds in practice); one full day at 30 s.  A record dropped here makes
/// the arc's eventual return read as a first sighting instead of a re-entry, which
/// is the CONSERVATIVE direction — both are unvetted, only the census label differs,
/// and the drop is counted so the label shift is never silent.
constexpr long long kArcForgetEp = 2880;

/// Is this a PHASE observable key?  RINEX-3 codes on the uncombined producer path
/// are always the raw 3-character forms ("L1C", "C2W", …), so the type character is
/// the whole test.
inline bool isPhaseKey(const std::string& k) { return !k.empty() && k[0] == 'L'; }

} // namespace

const char* signalIntegrityStateName(SignalIntegrityState s) {
    switch (s) {
        case SignalIntegrityState::Stable:        return "stable";
        case SignalIntegrityState::New:           return "new";
        case SignalIntegrityState::Reentry:       return "reentry";
        case SignalIntegrityState::FlickerRegain: return "flicker_regain";
        case SignalIntegrityState::Degraded:      return "degraded";
    }
    return "?";
}

std::set<std::string> signalIntegrityPhaseBands(const PppObservation& obs) {
    std::set<std::string> bands;
    for (const auto& [key, val] : obs.observed) {
        (void)val;
        if (isPhaseKey(key)) bands.insert(key);
    }
    return bands;
}

void networkSignalIntegrity(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces,
                            const GnssTime& t,
                            long long epoch_idx,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            std::vector<std::vector<PppObservation>>& modeled,
                            std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::pair<int, std::string>>& carry_slipped_raw,
                            bool carry_invariant, bool carry_warm) {
    (void)config_;   // every mode predicate is already resolved into ctx.knobs
    if (!ctx.knobs.sig_integrity_on) return;   // OFF: no map, no row, no observable

    const double scale        = ctx.knobs.sig_unvetted_scale;    // 1.0 = no deweight
    const bool   withhold     = ctx.knobs.sig_withhold;
    const int    revet_ep     = ctx.knobs.sig_revet_ep;
    const int    flicker_max  = ctx.knobs.sig_flicker_max_ep;
    const bool   refloat_on   = ctx.knobs.sig_refloat_regain;
    const bool   deweight     = scale > 1.0;

    auto& arcs = ctx.carry.si_arc;
    auto& ctr  = ctx.counters.sig;
    ++ctr.epochs;

    // RESUME WARMUP (spec ADDENDUM).  A warm resume restores TIGHT carries but no
    // detector state, so for the first revet_ep epochs the whole network is warm and
    // unmonitored at once.  That case is invisible to the per-arc rule below — a
    // freshly-empty band map makes every arc look like a first sighting, and a first
    // sighting is deliberately exempt — so it is carried as its own countdown and
    // OVERRIDES the per-arc verdict while it lasts.  Zero on every cold start, which
    // is what keeps cold starts exempt by principle rather than by exception.
    const bool resume_warmup = ctx.carry.si_resume_warmup_ep > 0;
    if (resume_warmup) --ctx.carry.si_resume_warmup_ep;

    auto emit = [&](const char* event, const std::string& sat,
                    const std::string& station, std::optional<double> statistic,
                    std::optional<double> threshold, const char* action,
                    const std::string& detail) {
        traces.emitEvent(t, "SIGNAL", event, sat, station, statistic, threshold,
                         action, detail);
    };

    // Every LOS touched this epoch, so the disappearance sweep below can tell a LOS
    // that VANISHED from one that is simply at an absent station.
    std::set<std::pair<int, std::string>> seen;

    for (std::size_t i = 0; i < stations.size(); ++i) {
        NetStation& st = stations[i];
        // Republish the per-epoch demotion map unconditionally for every station,
        // present or not: this seam is its SOLE owner (the three existing
        // pinned_amb_sigma_scale clears all live inside election seams that do not
        // run at all on the legacy single-pivot datum, so none of them could be
        // reused), and a stale scale surviving into an epoch that did not
        // re-derive it would demote an arc nobody classified.
        std::map<std::string, double> new_scale;
        if (!present[i] || !st.sm) { st.phase_sigma_scale.swap(new_scale); continue; }
        const int si = static_cast<int>(i);
        StateManager& sm = *st.sm;
        std::set<std::string> withheld_pb;   // "sat|signal|PB" state keys to prune
        bool prune_pb = false;

        for (PppObservation& obs : modeled[i]) {
            const std::string& sat = obs.satellite;
            if (sat.empty()) continue;
            seen.insert({si, sat});
            ++ctr.los_epochs;

            const std::set<std::string> cur = signalIntegrityPhaseBands(obs);
            const auto key = std::make_pair(si, sat);
            auto it = arcs.find(key);
            const bool first = (it == arcs.end());
            if (first) it = arcs.emplace(key, SignalIntegrityArc{}).first;
            SignalIntegrityArc& a = it->second;

            const long long gap = first ? -1 : (epoch_idx - a.last_seen_ep - 1);
            if (cur.size() >= 2) a.ever_dual = true;
            if (cur.empty()) ++ctr.no_phase_epochs;

            // ---- (1) classify ------------------------------------------------
            // Order is load-bearing: a gap DOMINATES a band event (an arc that was
            // absent and returns with fewer bands is a RE-ENTRY, whose whole band
            // set re-establishes, not a DEGRADED continuation of a set nobody
            // observed across the gap), and a band event dominates the steady state.
            SignalIntegrityState state = SignalIntegrityState::Stable;
            bool transition = false;
            std::set<std::string> regained;   // FLICKER-REGAIN bands (INC-3a trigger)

            if (first) {
                state = SignalIntegrityState::New;
                transition = true;
                a.ref_bands = cur;
                a.missing_since.clear();
                ++ctr.new_arcs;
                // Deliberately NO event row: at 305 stations the first processed
                // epoch would write ~12 000 of them, which is a flood, not a signal.
                // The counter carries the volume and the census reads it against
                // `los_epochs`.
            } else if (gap >= 1) {
                state = SignalIntegrityState::Reentry;
                transition = true;
                // The reference set re-establishes on what actually came back.  An
                // arc that returns single-frequency therefore re-vets on ONE band
                // and becomes STABLE again rather than being demoted forever — the
                // standing-single-frequency policy is explicitly deferred (spec §7),
                // and ever_dual keeps it visible in the census meanwhile.
                a.ref_bands = cur;
                a.missing_since.clear();
                ++ctr.reentries;
                if (gap == 1)      ++ctr.reentry_gap_1;
                else if (gap <= 5) ++ctr.reentry_gap_2_5;
                else               ++ctr.reentry_gap_gt5;
                emit("reentry", sat, st.name, static_cast<double>(gap), std::nullopt,
                     "reentry", fmt::format("gap_ep={};bands={}", gap, cur.size()));
            } else {
                // Continuous: the LOS was present at the immediately preceding
                // processed epoch, so band arrivals and departures are observable
                // events rather than inferences across an outage.
                for (const std::string& b : cur) {
                    auto mit = a.missing_since.find(b);
                    if (mit != a.missing_since.end()) {
                        // A reference band is back and the LOS never went away, so
                        // NO producer-registered gap exists for it: this is the
                        // FLICKER the card is named for, and the exact shape of the
                        // measured G21 case.
                        regained.insert(b);
                        a.missing_since.erase(mit);
                        ++ctr.band_regain;
                    } else if (!a.ref_bands.count(b)) {
                        a.ref_bands.insert(b);
                        transition = true;
                        ++ctr.band_gained;
                    }
                }
                if (!regained.empty()) {
                    state = SignalIntegrityState::FlickerRegain;
                    transition = true;
                    emit("band_regain", sat, st.name,
                         static_cast<double>(regained.size()),
                         static_cast<double>(flicker_max), "refloat",
                         fmt::format("{};bands={}", *regained.begin(), cur.size()));
                }
                // Reference bands that are NOT present this epoch.
                for (const std::string& b : a.ref_bands) {
                    if (cur.count(b)) continue;
                    if (a.missing_since.emplace(b, epoch_idx).second) {
                        transition = true;
                        ++ctr.band_lost;
                        if (!a.degraded_open) {
                            a.degraded_open = true;
                            emit("band_lost", sat, st.name,
                                 static_cast<double>(cur.size()),
                                 static_cast<double>(a.ref_bands.size()), "degrade",
                                 fmt::format("{};missing={}", b,
                                             a.missing_since.size()));
                        }
                    }
                }
                // A band gone longer than the flicker window is treated as GENUINELY
                // gone: it leaves the reference set, the arc re-vets on the reduced
                // set, and its eventual return is a band GAIN (a fresh ambiguity the
                // estimator births normally) rather than a flicker regain.  Without
                // this the arc would sit DEGRADED — and hence demoted — for the rest
                // of its life, which is the policy question spec §7 defers, not one
                // this card is entitled to answer by omission.
                for (auto mit = a.missing_since.begin(); mit != a.missing_since.end();) {
                    if (epoch_idx - mit->second > flicker_max) {
                        a.ref_bands.erase(mit->first);
                        transition = true;
                        ++ctr.band_dropped_perm;
                        mit = a.missing_since.erase(mit);
                    } else {
                        ++mit;
                    }
                }
                if (!a.missing_since.empty()
                    && state != SignalIntegrityState::FlickerRegain) {
                    state = SignalIntegrityState::Degraded;
                }
            }
            if (a.missing_since.empty()) a.degraded_open = false;

            const bool full_band = a.missing_since.empty();
            if (!full_band) ++ctr.degraded_epochs;
            if (a.ever_dual && cur.size() < 2) ++ctr.sf_continuation_epochs;
            if (!a.ever_dual) {
                ++ctr.standing_sf_epochs;
                if (!a.sf_counted) { a.sf_counted = true; ++ctr.standing_sf_arcs; }
            }

            // ---- (2) the vet verdict, and INC-2's disposition ----------------
            // Tested BEFORE this epoch's increment, which is what makes the demoted
            // window run from the transition epoch through the epoch that COMPLETES
            // the vet: transition -> vet 0 (demoted), then revet_ep full-band epochs
            // each demoted while counting up, and full weight resumes the epoch
            // after.  That is the spec §6D arithmetic ("regain 19:25:30 ... demoted
            // phase through 19:27:00" at K=3), and it is the reason the vet ends
            // exactly when monitoring begins rather than one epoch before it.
            if (transition || !full_band) a.vet = 0;
            const bool unvetted = a.vet < revet_ep;

            // The demotion's precondition (spec ADDENDUM): there must be WARM STATE
            // for a slip to poison.  A phase row's damage potential is its weight
            // times the TIGHTNESS of what it updates, and an arc that has never
            // completed a re-vet in this run has none — a diffuse ambiguity (σ 1000 m)
            // and no accumulated carry chord — so its own priors already ARE the
            // demotion and vetting it would only slow its convergence.  A COLD START
            // is therefore exempt by principle, not by a special case: every arc is
            // un-converged there.  The RESUME WARMUP overrides this, and must: after a
            // resume the carries are tight even though every arc looks brand new here.
            const bool has_warm_state = a.ever_stable || resume_warmup;

            if (unvetted) {
                ++ctr.unvetted_los_epochs;
                if (!has_warm_state) ++ctr.birth_exempt_los_epochs;
                else if (resume_warmup && !a.ever_stable) ++ctr.resume_warmup_los_epochs;
            }
            if (unvetted && has_warm_state) {
                if (withhold) {
                    // WITHHOLD: remove this LOS's PHASE observables for the epoch.
                    // Done HERE, on the observation, rather than by zeroing the row
                    // weight later: a zero-weight row still contributes its (live)
                    // H column to the local block, so its ambiguity column would
                    // enter A_r carrying no information and trip the per-station PD
                    // guard — turning a per-arc demotion into a whole-station
                    // exclusion, the exact hard drop the fallback doctrine forbids.
                    // Erasing the observable makes the column all-zero, which the
                    // existing dead-column scan already drops cleanly.
                    // raw_observed is deliberately untouched: it belongs to the slip
                    // detectors, which have already run this epoch and must keep
                    // seeing the true tracking state.
                    std::size_t n_removed = 0;
                    for (auto oit = obs.observed.begin(); oit != obs.observed.end();) {
                        if (isPhaseKey(oit->first)) { oit = obs.observed.erase(oit); ++n_removed; }
                        else ++oit;
                    }
                    if (n_removed > 0) {
                        ++ctr.withheld_los_epochs;
                        ctr.withheld_rows += static_cast<long long>(n_removed);
                        // Prune this station's satellite phase-bias states for the
                        // withheld signals, exactly as the coverage gate prunes a
                        // thin key's.  Load-bearing: the anchor election runs AFTER
                        // this seam and derives its candidates from the phase-bias
                        // STATES (not from the observables), so without the prune it
                        // could elect a datum pin here whose supporting row this
                        // seam has just removed — the pin would silently fail to
                        // fold and the gauge would lose a constraint.
                        prune_pb = true;
                        for (const std::string& b : cur)
                            withheld_pb.insert(sat + "|" + b);
                    }
                } else if (deweight) {
                    new_scale[sat] = scale;
                    ++ctr.demoted_los_epochs;
                }
            }
            if (!transition && full_band && a.vet < revet_ep) {
                if (++a.vet >= revet_ep) { ++ctr.vet_completions; a.ever_stable = true; }
            }

            // ---- (3) INC-3: the reset response --------------------------------
            // Reset, never repair: across a loss of lock nothing bounds the slip, so
            // there is no value to correct toward — only a state to discard.  The
            // insertion rides the EXACT funnel the MW / LLI / IF-TD detections use
            // (a parallel reset path would be a second definition of "this arc
            // re-taps fresh"), and it covers ALL of the LOS's bands because a
            // flicker window slips all of them at once.
            if (state == SignalIntegrityState::FlickerRegain
                || state == SignalIntegrityState::Reentry) {
                // The keys are built from THIS epoch's bands and are deliberately
                // NOT gated on a live StateManager ambiguity.  `ensureRequiredStates`
                // prunes every ambiguity whose phase observable is absent, with no
                // grace epoch, so both a re-entering LOS and a regained band have
                // NO state at this point — gating on one would make every counter
                // below read zero and every insertion a no-op, which is exactly the
                // "clean log over dead wiring" failure.  The funnel is keyed by
                // STRING and its consumers resolve the key against THIS epoch's
                // arcs, so an insertion for a key PASS B is about to allocate lands
                // correctly; `markAmbiguityPendingReseed` is a plain set insert and
                // is likewise safe ahead of the allocation.
                std::vector<std::string> amb_keys;
                amb_keys.reserve(cur.size());
                bool any_marked = false, any_state = false;
                for (const std::string& b : cur) {
                    const std::string amb = StateKeys::makeAmbiguityKey(sat, b);
                    amb_keys.push_back(amb);
                    if (carry_slipped_ambs.count({si, amb})
                        || sm.isAmbiguityPendingReseed(amb))
                        any_marked = true;
                    if (sm.getAmbiguityIndex(amb) >= 0) any_state = true;
                }

                if (state == SignalIntegrityState::Reentry && !amb_keys.empty()) {
                    // ASSERT the spec's premise; do NOT act on it.  "True REENTRY
                    // already refloats via the funnel — no change, but assert it with
                    // a counter" (spec §2 INC-3a).  Acting here would push ~1.7
                    // extra funnel entries per epoch into a channel the Iono-2 v3k
                    // erase policy is tuned on, so the honest move is to MEASURE the
                    // premise and let the number decide the next card.
                    // Guarded on a non-empty band set: a CODE-ONLY re-entry (measured
                    // — G21 @ BSHM returns with bands=0) has no ambiguity to refloat,
                    // so counting it as "not already refloated" would inflate the very
                    // number the premise is judged on with cases the premise does not
                    // cover.
                    if (any_marked) ++ctr.reentry_already_refloated;
                    else            ++ctr.reentry_unrefloated;
                    if (any_state)  ++ctr.reentry_amb_state_live;

                    // INC-3c audit: does an LLI raised ON a re-entry epoch actually
                    // reach the refloat drain when the arc is too immature for
                    // MW/IF-TD?  The PASS-A drains ran before this seam, so what is
                    // read here is exactly what they left.
                    bool lli = false;
                    for (const auto& [okey, flag] : obs.lli_flags)
                        if (isPhaseKey(okey) && (flag & 0x01)) { lli = true; break; }
                    if (lli) {
                        ++ctr.lli_at_reentry;
                        if (any_marked) ++ctr.lli_at_reentry_funnelled;
                    }
                } else if (state == SignalIntegrityState::FlickerRegain && refloat_on) {
                    // FLICKER-REGAIN: reset, unconditionally.  No detector can see a
                    // flicker — MW and GF both need the SAME dual pair on both sides
                    // of the step, and the missing band is precisely what breaks that
                    // — so `any_marked` is expected false and is not consulted.
                    long long n_ins = 0, n_seed = 0;
                    for (const std::string& amb : amb_keys) {
                        // Mirror the MW/LLI drain's exact gating (NetworkSlipQc).
                        // The RAW set too: a band that physically vanished and came
                        // back IS a raw observable event, so the Iono-2 v3k
                        // erase-policy split must ERASE this line of sight's GM
                        // deviation rather than D5-KEEP a level carried across a
                        // loss of lock.
                        if (carry_invariant) {
                            carry_slipped_raw.insert({si, amb});
                            if (carry_slipped_ambs.insert({si, amb}).second) ++n_ins;
                        }
                        if (carry_warm) {
                            sm.markAmbiguityPendingReseed(amb);
                            ++n_seed;
                        }
                    }
                    ctr.refloat_regain_ambs   += n_ins;
                    ctr.refloat_regain_reseeds += n_seed;
                    ++ctr.refloat_regain;
                }
            }

            a.bands = cur;
            a.last_seen_ep = epoch_idx;
        }

        st.phase_sigma_scale.swap(new_scale);
        if (prune_pb) {
            std::set<std::string> keep_pb;
            for (const auto& kv : sm.getSatPhaseBiasStates()) {
                const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                if (p && withheld_pb.count(p->satellite + "|" + p->signal)) {
                    ++ctr.withheld_pb_pruned;
                    continue;
                }
                keep_pb.insert(kv.first);
            }
            sm.pruneSatPhaseBiasStatesExcept(keep_pb);
        }
    }

    // --- Disappearance sweep + record ageing -------------------------------
    // A LOS that was present at the PREVIOUS processed epoch and is absent now, at
    // a station that IS present, is the "(a) LOS dropped on band loss" candidate the
    // census exists to size.  It is deliberately measured against
    // sf_continuation_epochs rather than replacing it: if the producer DROPS a
    // degrading arc, this count is large and the continuation count near zero; if it
    // CONTINUES it single-frequency, the reverse.  Neither number alone answers the
    // audit, and neither assumes the answer.
    for (auto it = arcs.begin(); it != arcs.end();) {
        const int si = it->first.first;
        SignalIntegrityArc& a = it->second;
        if (seen.count(it->first)) { ++it; continue; }
        const bool sta_present = si >= 0 && si < static_cast<int>(present.size())
                                 && present[static_cast<std::size_t>(si)];
        if (sta_present && a.last_seen_ep == epoch_idx - 1) {
            ++ctr.los_vanished;
            if (a.bands.size() >= 2) ++ctr.los_vanished_dual;
        }
        if (epoch_idx - a.last_seen_ep > kArcForgetEp) {
            ++ctr.arcs_forgotten;
            it = arcs.erase(it);
            continue;
        }
        ++it;
    }

    if (ctr.epochs == 1)
        log->debug("Network (signal integrity): first classifier pass — {} line(s) of "
                   "sight classified, {} first sighting(s), demotion {}.",
                   ctr.los_epochs, ctr.new_arcs,
                   withhold ? "WITHHOLD" : (deweight ? "deweight" : "OFF (census only)"));
}

} // namespace GnssProcessor
