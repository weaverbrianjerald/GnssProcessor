/**
 * @file NetworkPhaseDatum.cpp
 * @brief Prioritized-datum spectrum-based drop-and-heal (see the header).
 *
 * Decomposition INC-8 (2026-07-17) — body relocated VERBATIM from
 * ApplicationNetwork.cpp; former lambda captures are explicit parameters
 * (capture→param map in the INC-8 cut spec).
 */

#include "filter/NetworkPhaseDatum.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::parseSatPhaseBiasKey
#include "config/GnssConfig.hpp"        // INC-8b: the census reads 2 knobs
#include "common/PppObservation.hpp"    // INC-8b: modeled[] elevation source

#include <algorithm>
#include <cmath>                        // std::isfinite / std::abs — handover ledger
#include <map>
#include <tuple>                        // 1(d) local-carry key (station, sat, signal)
#include <utility>

namespace GnssProcessor {

int healNearNullPhaseBias(NetworkEstimator& est,
                          const std::set<std::string>& shared_phase_keys,
                          bool phase_datum_prioritized,
                          bool within_epoch_armed,
                          std::vector<NetworkEstimator::SharedPrior>& cap_priors,
                          std::vector<PhaseHealEvent>* healed_out) {
    if (!phase_datum_prioritized || est.numShared() == 0 || shared_phase_keys.empty())
        return 0;
    const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) return 0;
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    const double smax = ev.size() ? ev(ev.size() - 1) : 0.0;
    // Card DG2: the RESOLVED gate tolerance, NOT the kNullSpaceTol default.  The
    // heal must anchor only directions the gate itself counts as null — under a
    // shared compile constant an ARMED (looser-counting) gate would leave this
    // scan soft-anchoring weak-but-REAL families, i.e. putting prior information
    // on real parameters.  Same value, three consumers (gate / dump / heal).
    const double cutoff = est.rankGateTol() * std::max(smax, 1.0);
    std::map<int, std::pair<std::string, std::string>> pbcol;
    for (const auto& pk : shared_phase_keys) {
        const auto p = StateKeys::parseSatPhaseBiasKey(pk);
        if (p) pbcol[static_cast<int>(
            est.phaseBiasColumn(p->satellite, p->signal))] = {p->satellite, p->signal};
    }
    // Every phase-bias column meaningfully spanning a near-null direction
    // (|component| > 0.3) is weakly/un-observable → heal it.  A clock-gauge
    // direction has ~zero phase-bias weight, so its minor PB participants
    // stay below 0.3 and are left alone.
    constexpr double kSpanFrac = 0.3;
    std::set<int> heal_cols;
    // PA2 attribution: per healed column, the near-null direction it spans MOST
    // heavily (max |component|) and that direction's eigenvalue — enough to tell
    // a deep orphan gauge (ev ~1e-10, span ~0.99) from a weak-but-real family
    // from the trace alone.  Observational: it never changes which columns heal.
    std::map<int, std::pair<double, double>> heal_attr;   // col -> {span, ev}
    for (Eigen::Index j = 0; j < ev.size(); ++j) {
        if (ev(j) >= cutoff) continue;                 // observable direction
        const Eigen::VectorXd v = es.eigenvectors().col(j).cwiseAbs();
        for (const auto& [c, ss] : pbcol)
            if (c < v.size() && v(c) > kSpanFrac) {
                heal_cols.insert(c);
                auto& a = heal_attr[c];
                if (v(c) > a.first) a = {v(c), ev(j)};
            }
    }
    if (heal_cols.empty()) return 0;
    std::vector<NetworkEstimator::SharedPrior> hp;
    hp.reserve(heal_cols.size());
    for (int c : heal_cols) {
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::PhaseBias;
        p.sat    = pbcol[c].first;
        p.signal = pbcol[c].second;
        p.value  = 0.0;
        p.info   = 1.0;   // σ = 1 m
        hp.push_back(p);
        if (healed_out) {
            const auto& a = heal_attr[c];
            healed_out->push_back(PhaseHealEvent{p.sat, p.signal, a.second, a.first});
        }
    }
    est.attribTag("pb_heal");
    est.addSharedPrior(hp);
    if (within_epoch_armed)
        cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
    return static_cast<int>(hp.size());
}

void networkBuildPhaseDatumCandidates(
        const GnssConfig& config_,
        std::vector<NetStation>& stations,
        const std::vector<bool>& present,
        const std::vector<std::vector<PppObservation>>& modeled,
        PhaseDatumCandidates& cnd,
        std::size_t& n_gated_epochs,
        std::size_t& n_gated_key_epochs) {
    const double pref_el = config_.networkPhaseDatumPrefElevDeg();
    const int    pdp_min_sta = config_.networkPhaseDatumMinStations();

    // (a) Per present station: satellite elevation (from the pruned obs) and
    //     the ANCHORABLE keys — the per-(sat,signal) phase-bias STATES this
    //     station allocated in PASS A.  NOTE the timing: the ambiguity states
    //     are allocated later, in PASS B's computeMeasurementUpdate, so we
    //     canNOT test getAmbiguityIndex here (it is -1 for everything at this
    //     point).  ensureNetworkSharedStates allocates a phase-bias state via
    //     the SAME usability gate as the row build, so a live phase-bias state
    //     ⟹ a phase row ⟹ a pinnable ambiguity in PASS B; the pin fold's own
    //     getAmbiguityIndex + h2local check is the final guard (a miss orphans
    //     the key, caught by the null-space gate → withhold + heal, rare).
    //     usable_keys[i]: "sat_signal" -> "sat|signal|PB".
    std::vector<std::map<std::string, double>>      sat_el(stations.size());
    std::vector<std::map<std::string, std::string>> usable_keys(stations.size());
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& o : modeled[i]) sat_el[i][o.satellite] = o.elevation;
        StateManager& sm = *stations[i].sm;
        for (const auto& kv : sm.getSatPhaseBiasStates()) {
            const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
            if (!p) continue;
            usable_keys[i].emplace(p->satellite + "_" + p->signal, kv.first);
        }
    }

    // (b) Per-constellation candidate receivers, ranked by DESCENDING
    //     coverage (# satellites tracked at >= pref_el) then lexical station
    //     name (determinism).  A station is a candidate for a constellation
    //     iff it can anchor >=1 of its keys; a low-elevation-only tracker
    //     stays a (last-ranked) candidate so it can still anchor a setting
    //     satellite nobody else sees.
    std::map<char, std::map<size_t, int>> cover;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& [sat, el] : sat_el[i])
            if (!sat.empty() && el >= pref_el) ++cover[sat.front()][i];
    }
    std::map<char, std::set<size_t>> cand_set;
    std::map<std::string, std::string> union_keys;   // "sat_signal" -> "sat|signal|PB"
    for (size_t i = 0; i < stations.size(); ++i)
        for (const auto& [amb, pb] : usable_keys[i]) {
            if (!amb.empty()) cand_set[amb.front()].insert(i);
            union_keys.emplace(amb, pb);
        }

    // Coverage-threshold gating of the anchored union
    // (network.phase_datum_min_stations; 1 = off, byte-identical): drop
    // every (sat,signal) tracked by fewer than k present stations BEFORE
    // the election.  A one/two-tracker column is exactly the pathology
    // class the pdp validation surfaced: it (a) ejects a marginal
    // tracking station as non-PD (its local block barely separates the
    // column from that station's own ambiguity — the TWTF/VOIM symptom),
    // (b) re-levels by a full ambiguity lump when its one-station anchor
    // pool thins, and (c) leaks its anchor lump into the clock↔phase-bias
    // split.  A gated key's PHASE rows still serve the satellite clock:
    // each station's float ambiguity absorbs the bias, exactly as legacy
    // non-shared signals (and GLONASS network phase) always have.  The
    // tracking stations' allocated phase-bias states are pruned so the
    // per-station column sets stay consistent with the gated union (the
    // kMinStationsPerSat prune pattern above); the emitter sees a clean
    // per-key absence (staleness / first-sighting semantics on return).
    if (pdp_min_sta > 1) {
        std::map<std::string, int> n_trk;
        for (size_t i = 0; i < stations.size(); ++i)
            for (const auto& kv : usable_keys[i]) ++n_trk[kv.first];
        std::set<std::string> gated_amb;   // "sat_signal" excluded this epoch
        for (auto it = union_keys.begin(); it != union_keys.end();) {
            if (n_trk[it->first] < pdp_min_sta) {
                gated_amb.insert(it->first);
                it = union_keys.erase(it);
            } else {
                ++it;
            }
        }
        if (!gated_amb.empty()) {
            ++n_gated_epochs;
            n_gated_key_epochs += gated_amb.size();
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> keep_pb;
                bool drop_any = false;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (p && gated_amb.count(p->satellite + "_" + p->signal)) {
                        drop_any = true;
                        continue;
                    }
                    keep_pb.insert(kv.first);
                }
                if (drop_any) sm.pruneSatPhaseBiasStatesExcept(keep_pb);
            }
        }
    }

    // The five census containers are declared above EXACTLY as they were in the
    // run TU (sizing constructor arguments intact), so the body is a pure
    // dedent; they are handed to the caller here.  Move-assignment fully
    // overwrites each member, which is what makes the census structurally fresh
    // per call regardless of how the caller stores `cnd`.
    cnd.sat_el      = std::move(sat_el);
    cnd.usable_keys = std::move(usable_keys);
    cnd.cover       = std::move(cover);
    cnd.cand_set    = std::move(cand_set);
    cnd.union_keys  = std::move(union_keys);
}

void networkElectEnsembleDatum(
        std::vector<NetStation>& stations,
        const PhaseDatumCandidates& cnd,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys) {
    const auto& union_keys = cnd.union_keys;

    // Fix A: anchor-free ENSEMBLE datum — the shared phase-bias set is the
    // (coverage-gated) UNION, with NO anchor election and NO pins.  The
    // gauge is realized by the kernel sum-to-zero fold (built from every
    // tracking station's ambiguity column AFTER the reduce), so no station
    // gets a pinned_amb_key (⇒ do_fold stays false, isGaugePinned false,
    // every station carries all its arcs) and there is no anchor to hand
    // over.  The candidate ranking + election below are skipped.
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (never set here)
    }
    this_epoch_anchor.clear();
    shared_phase_keys.clear();
    for (const auto& [amb, pbkey] : union_keys) shared_phase_keys.insert(pbkey);
}

void networkElectPhaseAnchors(
        bool sticky,
        std::vector<NetStation>& stations,
        const std::vector<bool>& present,
        const PhaseDatumCandidates& cnd,
        const AnchorHygiene& hyg,
        NetworkAnchorTallies& tally,
        std::map<std::string, std::string>& anchor_incumbent,
        std::map<std::string, double>& anchor_pin_target,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys,
        const AnchorHandoverLedger* handover,
        std::vector<AnchorHandoverEvent>* handovers_out,
        double max_var,
        std::set<std::pair<std::string, std::string>>* reref_out,
        double max_var_ratio) {
    const auto& usable_keys = cnd.usable_keys;
    const auto& cand_set    = cnd.cand_set;
    const auto& union_keys  = cnd.union_keys;
    const auto& cover       = cnd.cover;
    auto cov = [&](char c, size_t i) -> int {
        auto it = cover.find(c); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };

    std::map<char, std::vector<size_t>> cand;
    for (auto& [c, s] : cand_set) {
        std::vector<size_t> v(s.begin(), s.end());
        std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
            const int ca = cov(c, a), cb = cov(c, b);
            if (ca != cb) return ca > cb;
            return stations[a].name < stations[b].name;
        });
        cand[c] = std::move(v);
    }

    // (c) Elect one anchor per (sat,signal): keep the STICKY incumbent while
    //     it still tracks the key (fewest handovers), else the highest-ranked
    //     candidate that tracks it — concentrating most of a constellation on
    //     one receiver (frame-consistent) with graceful per-satellite
    //     fallback.  Record the pin in the anchor's OWN pinned_amb_keys (its
    //     local block — the load-bearing invariant), the shared column, and
    //     this_epoch_anchor (read at the emit site for handover signalling).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();   // eq34-only; always empty here (no-op)
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (empty ⇒ no scaling)
    }
    this_epoch_anchor.clear();
    shared_phase_keys.clear();
    std::map<std::string, std::string> new_incumbent;
    // R2 (§5.09): the sticky continuous targets surviving this election.  Rebuilt
    // wholesale like new_incumbent, so a key that leaves the union (or whose
    // handover goes COLD) drops its target — the 0-frame is then realized
    // honestly.  Never populated on the handover == nullptr path, which keeps the
    // knob-OFF election byte-identical.
    std::map<std::string, double> new_target;
    for (const auto& [amb, pbkey] : union_keys) {
        const char c = amb.empty() ? char(0) : amb.front();
        // eq34 REATTACH HYGIENE INC-2: a BRIDGED key's gauge is the soft shared
        // pseudo-obs row (networkInjectPinBridgePrior), not a receiver pin.  It stays
        // in the shared set — the column is registered and solved — but NO station
        // anchors it and no handover is signalled, so the emitted bias keeps the
        // level the bridge holds instead of stepping onto a cold host's ambiguity.
        if (hyg.isBridged(amb)) {
            shared_phase_keys.insert(pbkey);
            continue;
        }
        size_t anchor = static_cast<size_t>(-1);
        if (sticky) {
            auto it = anchor_incumbent.find(amb);
            if (it != anchor_incumbent.end())
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i] && stations[i].name == it->second
                        && usable_keys[i].count(amb)) { anchor = i; break; }
        }
        // INC-1: on a REATTACH prefer, in the existing coverage rank order, a
        // candidate whose arc has actually converged; only if none has does the
        // second sweep accept a fresh host (counted — an unavoidable cold pin is not
        // the same event as a clean handover, and INC-3 can ramp its σ).  The sticky
        // incumbent above is deliberately exempt: keeping it is not a reattach.
        if (anchor == static_cast<size_t>(-1)) {
            auto cit = cand.find(c);
            if (cit != cand.end()) {
                for (size_t i : cit->second)
                    if (usable_keys[i].count(amb) && hyg.eligibleHost(i, amb)) {
                        anchor = i;
                        break;
                    }
                if (anchor == static_cast<size_t>(-1))
                    for (size_t i : cit->second)
                        if (usable_keys[i].count(amb)) {
                            anchor = i;
                            ++tally.cold_fallback_pins;
                            break;
                        }
            }
        }
        if (anchor == static_cast<size_t>(-1)) continue;   // no tracker (unreachable)
        // TWO-EDGE CONTINUOUS HANDOVER (docs/SPEC_pivot_visibility_gauge_defect.md).
        //
        // The gauge is "this anchor's ambiguity for `amb` = target".  Re-pinning a
        // NEW anchor at 0 realizes a DIFFERENT frame: the whole (sat,signal) family
        // steps by the inter-station ambiguity difference and the satellite clock
        // absorbs it.  That is a gauge change with no S-transform — the defect this
        // block exists to close.
        //
        // So on a HANDOVER (and only then — a key whose anchor did not move keeps
        // its 0.0 pin, which is what keeps a settled network byte-identical) the
        // target becomes the incoming anchor's OWN absolute ambiguity estimate in
        // the OUTGOING frame, taken from the previous epoch's ledger.  Pinning it
        // there reproduces the frame the outgoing anchor realized, so nothing steps.
        const auto  inc = anchor_incumbent.find(amb);
        const bool  is_handover = inc != anchor_incumbent.end()
                                  && inc->second != stations[anchor].name;
        double target = 0.0;
        bool   continuous = false;
        double ev_var = -1.0, ev_var_first = 0.0;   // the successor's ledger entry, for the event row
        // R2 (§5.09): a RETAINED incumbent keeps realizing the frame it inherited.
        // Without this the ledger pin is a one-epoch deferral — the very next
        // election re-pins the same anchor at 0 and the family steps at T+1
        // exactly as it would have at T (the §5.07.3 revert that made the
        // unsticky handover measure null on its own target channel).  The map is
        // populated only by continuous handovers below, so the knob-OFF path
        // never finds an entry and stays byte-identical.
        if (!is_handover && inc != anchor_incumbent.end()) {
            const auto tgt = anchor_pin_target.find(amb);
            if (tgt != anchor_pin_target.end()) {
                target = tgt->second;
                new_target[amb] = target;
            }
        }
        if (is_handover && handover) {
            const auto hit = handover->value_abs.find(
                    {static_cast<int>(anchor), amb});
            // RE-VALIDATE BEFORE RE-COUPLING (invariant #7).  The pin is a hard
            // ~1e10-weight constraint: stamping in an ILL-DETERMINED estimate is
            // strictly worse than re-zeroing, because it injects that estimate's
            // own error as a datum.  Measured before this gate existed: a +17.08 m
            // target taken from an unvalidated arc produced a 378-sigma residual
            // spike on the following epoch, worse than the step it replaced.  So a
            // value is usable only when its conditional variance is finite,
            // positive and below the caller's ceiling; everything else falls
            // through to the honest COLD path.
            // network.phase_datum_handover_max_var_ratio — the HONEST-σ-SCALE ceiling.
            // The absolute ceiling was sized to the over-counted σ of the diagonal
            // leveling carry (SPEC_amb_iono_split_overcount §6.8.3: 1e-4 m² refuses
            // 93/93 successors under F1/F3's honest σ; 1e-2 is a hand value).  The ratio
            // asks the posture-invariant question instead: has THIS arc converged since
            // its birth — var <= ratio × its first-stash conditional variance.  An
            // unknown var_first (a fresh arc that was never carried) is refused.  The
            // absolute ceiling stays as a rail; both are counted separately.
            const bool ratio_on = max_var_ratio > 0.0;
            const bool ratio_ok = !ratio_on
                    || (hit != handover->value_abs.end()
                        && hit->second.var_first > 0.0
                        && std::isfinite(hit->second.var_first)
                        && hit->second.var <= max_var_ratio * hit->second.var_first);
            const bool usable =
                    hit != handover->value_abs.end()
                    && std::isfinite(hit->second.value)
                    && hit->second.var > 0.0
                    && std::isfinite(hit->second.var)
                    && hit->second.var <= max_var
                    && ratio_ok;
            if (hit != handover->value_abs.end()) {
                ev_var       = hit->second.var;
                ev_var_first = hit->second.var_first;
            }
            if (usable) {
                target     = hit->second.value;
                continuous = true;
                // R2: persist — the successor stays pinned HERE for its whole
                // tenure, so the inherited frame survives past the handover
                // epoch.  Chains compose: the ledger is harvested in whatever
                // frame the previous pins realized, so each continuous handover
                // extends the same original frame.
                new_target[amb] = target;
                ++tally.handover_continuous;
                tally.handover_max_abs_m =
                        std::max(tally.handover_max_abs_m, std::abs(target));
            } else {
                // Either no estimate for this successor (a genuinely fresh arc, or
                // the run's first epoch) or one too poorly determined to be trusted
                // as a datum.  Pin at 0 and COUNT it — the frame does step here, and
                // an unavoidable re-realization the consumer is TOLD about is a
                // different object from a silent one.
                ++tally.handover_cold;
                if (hit != handover->value_abs.end() && hit->second.var > max_var)
                    ++tally.handover_refused_var;
                else if (hit != handover->value_abs.end() && ratio_on && !ratio_ok)
                    ++tally.handover_refused_ratio;
            }
        }
        stations[anchor].pinned_amb_targets[amb] = target;
        if (is_handover && handovers_out) {
            AnchorHandoverEvent ev{amb, inc->second, stations[anchor].name, target, continuous};
            ev.var       = ev_var;
            ev.var_first = ev_var_first;
            handovers_out->push_back(std::move(ev));
        }
        // QUEUE item 1(d) (SPEC_pivot_visibility_gauge_defect §5.07.5 R1): mark
        // every pin realized in a frame NOT continuously inherited from an
        // incumbent — a HANDOVER (the datum re-realizes on a new host), and an
        // INCUMBENT-LESS pin (inc == end()).  The second class is load-bearing
        // (review finding, 2026-08-14): a BRIDGED key skips the election before
        // any incumbent bookkeeping, so its record evaporates after one bridged
        // epoch and the eventual bridge-EXIT re-pin — a full frame step against
        // a still-stashed carry — would otherwise read as a first sighting and
        // go unmarked (standing invariant #7's "return read as a first sighting"
        // failure, verbatim).  Genuine first sightings are marked too, and
        // harmlessly: a family with no surviving carry makes every downstream
        // consumer a no-op, while one WITH a surviving carry (long-gap return
        // inside the prune horizon) is exactly the stale-frame case the mark
        // exists for.  The key is parsed from the registered pbkey, never split
        // from the amb string.
        // R2 (§5.09) narrows the mark to COLD re-realizations: a CONTINUOUS
        // handover under the sticky target provably realizes the SAME frame at T
        // and every following epoch (up to the ledger value's own ε ≤ √max_var —
        // which is why the armed configs run a tight ceiling), so the family's
        // carried state stays in-frame and marginalizing/erasing it would be pure
        // arc truncation.  The incumbent-less clause is unchanged: a first
        // sighting / bridge exit realizes a fresh frame and must stay marked.
        if (reref_out && ((is_handover && !continuous)
                          || inc == anchor_incumbent.end())) {
            const auto p = StateKeys::parseSatPhaseBiasKey(pbkey);
            if (p) reref_out->insert({p->satellite, p->signal});
        }
        {
            if (is_handover)
                ++tally.reattaches;
            const double sc = hyg.pinSigmaScale(anchor, amb);
            if (sc > 1.0) {
                stations[anchor].pinned_amb_sigma_scale[amb] = sc;
                ++tally.fresh_pin_ramps;
            }
        }
        this_epoch_anchor[amb] = stations[anchor].name;
        new_incumbent[amb]     = stations[anchor].name;
        shared_phase_keys.insert(pbkey);
    }
    anchor_incumbent.swap(new_incumbent);
    anchor_pin_target.swap(new_target);
}

int networkApplyPhaseAnchorCarryReref(
        const std::set<std::pair<std::string, std::string>>& reref,
        std::map<std::tuple<int, std::string, std::string>, LocalCarry>& local_carry) {
    if (reref.empty() || local_carry.empty()) return 0;
    // Verbatim the eq34 D4 erase (NetworkChainDatum.cpp): drop the family's
    // per-station ambiguity carries at EVERY station — the frame stepped, so the
    // carried floats are stale; refloat next epoch.
    int n_erased = 0;
    for (auto it = local_carry.begin(); it != local_carry.end();) {
        const auto& key = it->first;   // (station idx, sat, signal)
        if (reref.count({std::get<1>(key), std::get<2>(key)})) {
            it = local_carry.erase(it);
            ++n_erased;   // returned; the caller folds it into the retirement census
        } else {
            ++it;
        }
    }
    return n_erased;
}

} // namespace GnssProcessor
