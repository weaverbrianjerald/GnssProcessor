/**
 * @file NetworkPhaseDatum.cpp
 * @brief Prioritized phase-bias datum: the near-null phase-bias heal, the candidate
 *        census, the ensemble datum, the anchor election, anchor-slip handover
 *        staging and the re-reference carry erase (see the header).
 *
 * The bodies were moved unchanged out of ApplicationNetwork.cpp (INC-8, INC-8b);
 * former lambda captures are explicit parameters (the capture-to-parameter map is in
 * the INC-8 cut spec).
 */

#include "filter/NetworkInject.hpp"
#include "filter/NetworkPhaseDatum.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::parseSatPhaseBiasKey, parseAmbiguityKey
#include "common/GnssIdentity.hpp"      // letterOfSat
#include "config/GnssConfig.hpp"        // the census reads two knobs (INC-8b)
#include "common/PppObservation.hpp"    // elevation source for modeled[] (INC-8b)

#include <algorithm>
#include <cmath>                        // std::isfinite / std::abs: handover ledger
#include <map>
#include <tuple>                        // local-carry key (station, sat, signal), item 1(d)
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
    // The cutoff uses the resolved rank-gate tolerance, not the kNullSpaceTol
    // default, so the heal anchors only directions the gate itself counts as null.
    // With a compile-time constant here, an armed (looser-counting) gate would
    // leave this scan soft-anchoring weak but real families, which puts prior
    // information on real parameters.  The gate, the dump and the heal read the
    // same value (card DG2).
    const double cutoff = est.rankGateTol() * std::max(smax, 1.0);
    std::map<int, std::pair<std::string, std::string>> pbcol;
    for (const auto& pk : shared_phase_keys) {
        const auto p = StateKeys::parseSatPhaseBiasKey(pk);
        if (p) pbcol[static_cast<int>(
            est.phaseBiasColumn(p->satellite, p->signal))] = {p->satellite, p->signal};
    }
    // A phase-bias column with |component| > 0.3 in a near-null direction is
    // weakly observable or unobservable and is soft-anchored.  A clock-gauge direction
    // has almost no phase-bias weight, so its minor PB participants stay below
    // 0.3 and are left alone.
    constexpr double kSpanFrac = 0.3;
    std::set<int> heal_cols;
    // Attribution per healed column: the largest |component| over the near-null
    // directions it spans, and that direction's eigenvalue.  The pair separates
    // a deep orphan gauge (ev ~1e-10, span ~0.99) from a weak but real family in
    // the trace.  Observational only: it does not change which columns are
    // soft-anchored (card PA2).
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

    // (a) Per present station: satellite elevation (from the pruned obs) and the
    //     anchorable keys, which are the per-(sat,signal) phase-bias states this
    //     station allocated in PASS A.  The ambiguity states are allocated later,
    //     in PASS B's computeMeasurementUpdate, so getAmbiguityIndex cannot be
    //     tested here (it returns -1 for every key at this point).
    //     ensureNetworkSharedStates allocates a phase-bias state through the same
    //     usability gate as the row build, so a live phase-bias state implies a
    //     phase row and therefore a pinnable ambiguity in PASS B.  The pin fold's
    //     own getAmbiguityIndex + h2local check is the final guard: a miss orphans
    //     the key, which the null-space gate catches (withhold, then heal; rare).
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
            usable_keys[i].emplace(StateKeys::makeAmbiguityKey(p->satellite, p->signal), kv.first);
        }
    }

    // (b) Per-constellation candidate receivers and their coverage (number of
    //     satellites tracked at >= pref_el).  The election ranks candidates by
    //     descending coverage, then by lexical station name for determinism.  A
    //     station is a candidate for a constellation iff it can anchor at least
    //     one of its keys; a station that tracks only at low elevation stays a
    //     candidate (ranked last) so it can still anchor a setting satellite no
    //     other station sees.
    std::map<char, std::map<size_t, int>> cover;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& [sat, el] : sat_el[i])
            if (!sat.empty() && el >= pref_el) ++cover[letterOfSat(sat)][i];
    }
    std::map<char, std::set<size_t>> cand_set;
    std::map<std::string, std::string> union_keys;   // "sat_signal" -> "sat|signal|PB"
    for (size_t i = 0; i < stations.size(); ++i)
        for (const auto& [amb, pb] : usable_keys[i]) {
            if (const auto parts = StateKeys::parseAmbiguityKey(amb))
                cand_set[letterOfSat(parts->satellite)].insert(i);
            union_keys.emplace(amb, pb);
        }

    // Coverage-threshold gate on the anchored union
    // (network.phase_datum_min_stations; 1 = off, byte-identical): every
    // (sat,signal) tracked by fewer than k present stations is dropped before the
    // election.  A column with one or two trackers (a) can get a marginal
    // tracking station ejected as non-PD, because its local block barely
    // separates the column from that station's own ambiguity, (b) re-levels by a
    // whole ambiguity when its one-station anchor pool thins, and (c) leaks the
    // anchor's ambiguity into the clock/phase-bias split.  A gated key's phase
    // rows still serve the satellite clock: each station's float ambiguity
    // absorbs the bias, as for non-shared signals and GLONASS network phase.  The
    // tracking stations' allocated phase-bias states are pruned so the
    // per-station column sets stay consistent with the gated union (the same
    // pattern as the kMinStationsPerSat prune in ApplicationNetworkAssembly.cpp);
    // the emitter sees a per-key absence (staleness and first-sighting semantics
    // on return).
    //
    // `gated_amb` is declared outside the `pdp_min_sta > 1` branch so it can be
    // handed to the caller in every posture (empty when the gate is off).  The
    // drop-edge rule needs the set, and a second census would double-count the
    // two gate counters this function owns (card EV7 §6).
    std::set<std::string> gated_amb;   // "sat_signal" excluded this epoch
    if (pdp_min_sta > 1) {
        std::map<std::string, int> n_trk;
        for (size_t i = 0; i < stations.size(); ++i)
            for (const auto& kv : usable_keys[i]) ++n_trk[kv.first];
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
                    if (p && gated_amb.count(StateKeys::makeAmbiguityKey(p->satellite, p->signal))) {
                        drop_any = true;
                        // The parameter inventory's absorbed instance (card NOM N1b).
                        if (stations[i].mm)
                            stations[i].mm->noteAbsorbedSharedState(
                                StateContainer::SatPhaseBiasStates, kv.first,
                                AbsorbedReason::AnchoredUnionCoverage);
                        continue;
                    }
                    keep_pb.insert(kv.first);
                }
                if (drop_any) sm.pruneSatPhaseBiasStatesExcept(keep_pb);
            }
        }
    }

    // The five census containers are declared above as they were in the run TU
    // (sizing constructor arguments intact) and handed to the caller here.
    // Move-assignment overwrites each member, so the census is fresh on every
    // call however the caller stores `cnd`.
    cnd.sat_el      = std::move(sat_el);
    cnd.usable_keys = std::move(usable_keys);
    cnd.cover       = std::move(cover);
    cnd.cand_set    = std::move(cand_set);
    cnd.union_keys  = std::move(union_keys);
    // The gate's exclusions, read by the drop-edge predicate.  Moved like the
    // other members so the census stays fresh on every call (card EV7 §6, F1).
    cnd.gated_keys  = std::move(gated_amb);
}

void networkElectEnsembleDatum(
        std::vector<NetStation>& stations,
        const PhaseDatumCandidates& cnd,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys) {
    const auto& union_keys = cnd.union_keys;

    // Anchor-free ensemble datum (Fix A): the shared phase-bias set is the
    // coverage-gated union, with no anchor election and no pins.  The gauge is
    // realized by the kernel sum-to-zero fold, built from every tracking
    // station's ambiguity column after the reduce, so no station gets a
    // pinned_amb_targets entry (do_fold stays false, isGaugePinned is false,
    // every station carries all its arcs) and there is no anchor to hand over.
    // The candidate ranking and the anchor election do not run on this path.
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
        double max_var_ratio,
        const NetworkInjectState* inject,
        const AnchorCandidacyUsable* cand_usable,
        PhaseHeldBasis* held,
        PhaseGateDrop* gate_drop,
        AnchorReentry* reentry) {
    const auto& usable_keys = cnd.usable_keys;
    // Anchor re-entry (card SID phase 4).  `reentry` is null (the default) on every
    // unarmed run, so each branch below that tests it is unreachable and the
    // election is byte-identical.  With only its observe half armed (S2) it reads
    // and counts and changes nothing.
    const bool re_on      = reentry != nullptr && reentry->observe
                            && reentry->led != nullptr && reentry->state != nullptr;
    const bool re_restore = re_on && reentry->restore;
    // Station name -> index, built once per call and only when the read is armed.
    // The drop memo names its incumbent by station name (the incumbent map's value
    // type) while the ledger keys on the station index, and a linear search over
    // `stations` per lookup would make the read O(families x stations).
    std::map<std::string, int> re_sta_index;
    if (re_on)
        for (std::size_t i = 0; i < stations.size(); ++i)
            re_sta_index[stations[i].name] = static_cast<int>(i);
    // Held S-basis (RF-2).  `held` is null (the default) on every unarmed run, so
    // each branch below that tests it is unreachable and the election is
    // byte-identical.
    const bool held_on = held && held->on && held->basis;
    /// This candidate's LOS elevation (deg) for @p amb, or −1 when the elevation map
    /// is absent or has no entry.  −1 sorts last, so a station with unknown
    /// elevation never outranks one with a known elevation (RF-2 item 3).
    const auto elevOf = [&](std::size_t i, const std::string& amb) -> double {
        if (!held_on || !held->elev || i >= held->elev->size()) return -1.0;
        const auto p = StateKeys::parseAmbiguityKey(amb);
        if (!p) return -1.0;
        const auto it = (*held->elev)[i].find(p->satellite);
        return it == (*held->elev)[i].end() ? -1.0 : it->second;
    };
    /// Whether station @p i may hold the S-basis for @p amb (RF-2 items 3 and 4).  Two
    /// gates: the arc must be at least min_age old, because a leg born at a
    /// re-admission carries its code-level birth error and the pin would fix that
    /// error in at the gauge weight; and no screen may have flagged the arc within
    /// flag_quiet_ep, so the datum is the quietest arc available and not merely the
    /// oldest.  Each refusal is counted, so a family with no qualifying candidate
    /// shows in the counters instead of falling back unrecorded.
    const auto basisEligible = [&](std::size_t i, const std::string& amb) -> bool {
        if (!held_on) return true;
        if (held->min_age_ep > 0 && held->carry) {
            const auto p = StateKeys::parseAmbiguityKey(amb);
            if (!p) return false;
            const auto it = held->carry->find(
                std::make_tuple(static_cast<int>(i), p->satellite, p->signal));
            // An arc with no carry is newborn by definition and is refused.  An
            // entry with a non-positive birth stamp reads as mature (the
            // restored-arc convention).
            if (it == held->carry->end()
                || localCarryYoung(it->second, held->now_ep, held->min_age_ep)) {
                ++held->n_refused_young;
                return false;
            }
        }
        if (held->flag_quiet_ep > 0 && held->flag_ep) {
            const auto ft = held->flag_ep->find({static_cast<int>(i), amb});
            if (ft != held->flag_ep->end()
                && held->now_ep - ft->second < held->flag_quiet_ep) {
                ++held->n_refused_flagged;
                return false;
            }
        }
        return true;
    };
    const auto& cand_set    = cnd.cand_set;
    const auto& union_keys  = cnd.union_keys;
    const auto& cover       = cnd.cover;
    auto cov = [&](char c, size_t i) -> int {
        auto it = cover.find(c); if (it == cover.end()) return 0;
        auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
    };

    // ---- Ledger usability predicate: one definition, two readers --------------
    // Cold anchor fallback (docs/SPEC_event_harness.md §15).  The continuous branch
    // further down decides whether a successor's carried value may serve as the
    // datum; the candidacy pass asks the same question of every candidate before
    // one is elected.  Both must read one predicate: a screen that diverged from
    // the decision it previews would elect a station the branch then refuses, and
    // the handover would go cold anyway.  The forced-cold directive is part of the
    // predicate: a harness request for a cold handover makes every candidate
    // unusable, so the pass cannot re-pick the station a cold leg lands on.
    struct LedgerVerdict {
        bool ok = false;          ///< usable as a datum
        bool has_entry = false;   ///< a ledger entry exists at all
        bool var_over = false;    ///< entry present and var above the absolute ceiling
        bool ratio_on = false;    ///< the variance-ratio ceiling is armed
        bool ratio_ok = true;     ///< ...and this entry passes it
    };
    auto ledgerVerdict = [&](const AnchorHandoverLedger::Entry* e,
                             bool forced_cold) -> LedgerVerdict {
        LedgerVerdict v;
        v.ratio_on  = max_var_ratio > 0.0;
        v.has_entry = e != nullptr;
        v.var_over  = e != nullptr && e->var > max_var;
        v.ratio_ok  = !v.ratio_on
                || (e != nullptr && e->var_first > 0.0 && std::isfinite(e->var_first)
                    && e->var <= max_var_ratio * e->var_first);
        v.ok = !forced_cold && e != nullptr && std::isfinite(e->value)
               && e->var > 0.0 && std::isfinite(e->var) && e->var <= max_var
               && v.ratio_ok;
        return v;
    };
    auto ledgerEntry = [&](size_t i, const std::string& amb)
            -> const AnchorHandoverLedger::Entry* {
        if (!handover) return nullptr;
        const auto it = handover->value_abs.find({static_cast<int>(i), amb});
        return it == handover->value_abs.end() ? nullptr : &it->second;
    };
    // Cold anchor fallback: whether arc (i, amb) was freed this epoch.  A slip
    // response that freed an arc re-draws its level from one epoch of code, so its
    // carried value is not a datum however small last epoch's variance is (the
    // re-coupling edge of invariant #1, tested at candidacy time).  Both vehicles
    // are read: the leveling free-epoch latch and the DD invariant-carry funnel.
    auto candFreed = [&](size_t i, const std::string& amb) {
        if (!cand_usable) return false;
        const std::pair<int, std::string> k{static_cast<int>(i), amb};
        return (cand_usable->free_ambs && cand_usable->free_ambs->count(k) != 0)
            || (cand_usable->slipped_ambs && cand_usable->slipped_ambs->count(k) != 0);
    };
    // Cold anchor fallback: whether arc (i, amb) is mature.  Age is
    // n_loop_epochs − birth_ep of the station's own phase-leveling carry, tested
    // through localCarryYoung, the definition shared with the F3 block admission
    // gate.  An unknown age (no carry entry, an unstamped entry, or a
    // warm-resume-restored entry carrying LocalCarry::kBirthEpRestored,
    // SPEC_event_harness §15.4) counts as mature, the restored-entry convention
    // that the F3 block gate and the young-arc inflation also use.  That leaves
    // the decision to the variance ceiling instead of refusing a candidate the
    // ledger reports as converged.
    auto candMature = [&](size_t i, const std::string& amb) {
        if (!cand_usable || cand_usable->min_age_ep <= 0 || !cand_usable->local_carry)
            return true;
        const auto parts = StateKeys::parseAmbiguityKey(amb);
        if (!parts) return true;
        const auto it = cand_usable->local_carry->find(
                {static_cast<int>(i), parts->satellite, parts->signal});
        if (it == cand_usable->local_carry->end()) return true;   // never carried ⇒ unknown
        return !localCarryYoung(it->second, cand_usable->now_ep,
                                cand_usable->min_age_ep);
    };

    // Candidacy acceptance test, one definition with three readers: the per-family
    // pass, the satellite clause's joint scoring, and the co-located cold decision.
    // It returns the reason for a refusal so the four skip splits stay a partition
    // of `candidacy_skipped_unusable` (invariant #4: a total without the partition
    // cannot say whether the ledger is thin or the maturity bar too tight).
    enum class CandSkip { kOk, kNotCandidate, kNoEntry, kVarRefused, kFreed, kYoung };
    auto candScreen = [&](size_t i, const std::string& amb,
                          const InjectAnchorDirective* dir_,
                          const std::string& banned_name) -> CandSkip {
        if (!usable_keys[i].count(amb) || !hyg.eligibleHost(i, amb)
            || (!banned_name.empty() && stations[i].name == banned_name))
            return CandSkip::kNotCandidate;   // not a candidate at all; never counted
        const LedgerVerdict v = ledgerVerdict(ledgerEntry(i, amb), dir_ && dir_->cold);
        if (!v.ok)              return v.has_entry ? CandSkip::kVarRefused
                                                   : CandSkip::kNoEntry;
        if (candFreed(i, amb))  return CandSkip::kFreed;
        if (!candMature(i, amb))return CandSkip::kYoung;
        return CandSkip::kOk;
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

    // ---- Satellite clause (iteration 2) ----------------------------------------
    // docs/SPEC_event_harness.md §15.8.  A per-family screen (iteration 1) lets each
    // band of a satellite elect the host whose carry is best for that band.  Each
    // band is then correct on its own, but the bands are referenced to different
    // stations, an inter-frequency inconsistency the per-band producer instruments
    // cannot see and the consumer pays for (§15.6).
    //
    // With the clause on, the handover is resolved per satellite, ahead of the
    // per-family loop:
    //   (a) every family of satellite s that moves this epoch takes one successor:
    //       the candidate usable for the largest number of them (ties go to
    //       coverage order).  A family for which that host is not usable is still
    //       pinned there, through the cold path: a cold pin on the same host is a
    //       frame step, not a split.
    //   (b) when only some of s's families move, the station already anchoring the
    //       others is tried first.
    // A satellite with no usable candidate for any moving family is left to the
    // per-family sweeps.
    struct SatJoint {
        std::vector<std::string> moving;                  ///< amb keys handing over
        std::size_t host = static_cast<std::size_t>(-1);  ///< the joint successor
        bool colocated = false;                           ///< rule (b) decided it
    };
    std::map<std::string, SatJoint> sat_joint;
    if (cand_usable && cand_usable->on && cand_usable->satellite_clause) {
        // The moving families are decided here with the predicate the loop applies
        // below (sticky ∧ present ∧ tracks ∧ not banned keeps the pin), so the
        // pre-pass and the loop agree on the population.
        std::map<std::string, std::set<std::string>> held;   // sat -> stations keeping a pin
        for (const auto& [amb, pbkey] : union_keys) {
            if (hyg.isBridged(amb)) continue;
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            if (!parts) continue;
            const auto inc = anchor_incumbent.find(amb);
            if (inc == anchor_incumbent.end()) continue;   // first sighting: no handover
            const InjectAnchorDirective* d = inject ? inject->anchorFor(amb) : nullptr;
            const std::string bn = d ? inc->second : std::string{};
            bool keeps = false;
            if (sticky)
                for (std::size_t i = 0; i < stations.size(); ++i)
                    if (present[i] && stations[i].name == inc->second
                        && usable_keys[i].count(amb)
                        && !(!bn.empty() && stations[i].name == bn)) { keeps = true; break; }
            if (keeps) held[parts->satellite].insert(inc->second);
            else       sat_joint[parts->satellite].moving.push_back(amb);
        }
        for (auto& [sat, j] : sat_joint) {
            const char c2 = sat.empty() ? char(0) : letterOfSat(sat);
            const auto cit = cand.find(c2);
            if (cit == cand.end()) { ++tally.sat_clause_no_usable; continue; }
            auto nUsable = [&](std::size_t i) {
                std::size_t n = 0;
                for (const auto& amb : j.moving) {
                    const InjectAnchorDirective* d = inject ? inject->anchorFor(amb) : nullptr;
                    std::string bn;
                    if (d) {
                        const auto it = anchor_incumbent.find(amb);
                        if (it != anchor_incumbent.end()) bn = it->second;
                    }
                    if (candScreen(i, amb, d, bn) == CandSkip::kOk) ++n;
                }
                return n;
            };
            // Rule (b) first.  The held host is taken whenever it is usable for at
            // least one moving family, because that is the only choice under which
            // the satellite cannot split: a different host with a higher score
            // would split the satellite's references again.  A family the held
            // host cannot serve takes a co-located cold pin instead.
            const auto h = held.find(sat);
            if (h != held.end())
                for (std::size_t i : cit->second)
                    if (h->second.count(stations[i].name) && nUsable(i) > 0) {
                        j.host = i;
                        j.colocated = true;
                        break;
                    }
            // Rule (a) otherwise: the joint maximum, first in coverage order.
            if (j.host == static_cast<std::size_t>(-1)) {
                std::size_t best = 0;
                for (std::size_t i : cit->second) {
                    const std::size_t n = nUsable(i);
                    if (n > best) { best = n; j.host = i; }
                }
            }
            if (j.host == static_cast<std::size_t>(-1)) ++tally.sat_clause_no_usable;
            else                                        ++tally.sat_clause_satellites;
        }
    }

    // (c) Elect one anchor per (sat,signal): keep the sticky incumbent while it
    //     still tracks the key (fewest handovers), else take the highest-ranked
    //     candidate that tracks it.  Most of a constellation then sits on one
    //     receiver (frame-consistent), with per-satellite fallback.  The pin is
    //     recorded in the anchor's own pinned_amb_targets (its local block, which
    //     the datum depends on), in the shared column set, and in
    //     this_epoch_anchor (read at the emit site for handover signalling).
    for (auto& st : stations) {
        st.pinned_amb_targets.clear();
        st.pinned_rpb_keys.clear();   // eq34-only; always empty here (no-op)
        st.pinned_amb_sigma_scale.clear();   // eq34 hygiene INC-3 (empty ⇒ no scaling)
    }
    this_epoch_anchor.clear();
    shared_phase_keys.clear();
    std::map<std::string, std::string> new_incumbent;
    // The sticky continuous targets that survive this election (R2, §5.09).
    // Rebuilt from scratch like new_incumbent, so a key that leaves the union, or
    // whose handover goes cold, drops its target and is pinned in the 0 frame.
    // Never populated on the handover == nullptr path, which keeps the knob-off
    // election byte-identical.
    std::map<std::string, double> new_target;
    for (const auto& [amb, pbkey] : union_keys) {
        const auto amb_parts = StateKeys::parseAmbiguityKey(amb);   // "G01_L1C" -> (G01, L1C)
        const char c = amb_parts ? letterOfSat(amb_parts->satellite) : char(0);
        // A bridged key's gauge is the soft shared pseudo-obs row
        // (networkInjectPinBridgePrior), not a receiver pin (eq34 reattach hygiene,
        // INC-2).  It stays in the shared set, so the column is registered and
        // solved, but no station anchors it and no handover is signalled; the
        // emitted bias keeps the level the bridge holds instead of stepping onto a
        // cold host's ambiguity.
        if (hyg.isBridged(amb)) {
            shared_phase_keys.insert(pbkey);
            continue;
        }
        // Event harness: a staged anchor_handover retires this family's incumbent
        // for this epoch only.  It is expressed as a candidacy ban rather than a
        // direct assignment so the successor is chosen by the production rules
        // (coverage rank, hygiene eligibility, cold fallback); the harness forces
        // the event, not the outcome.
        const InjectAnchorDirective* dir = inject ? inject->anchorFor(amb) : nullptr;
        std::string banned;
        if (dir) {
            const auto bit = anchor_incumbent.find(amb);
            if (bit != anchor_incumbent.end()) banned = bit->second;
        }
        auto banned_sta = [&](size_t i) {
            return !banned.empty() && stations[i].name == banned;
        };
        size_t anchor = static_cast<size_t>(-1);
        if (sticky) {
            auto it = anchor_incumbent.find(amb);
            if (it != anchor_incumbent.end())
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i] && stations[i].name == it->second
                        && usable_keys[i].count(amb) && !banned_sta(i)) {
                        // With the held basis armed, an incumbent is kept only while it
                        // still qualifies (RF-2 items 3 and 4), so stickiness also asks
                        // the basis question.  A T4, T3, slip or gross-LOS flag on the
                        // basis ambiguity deposes the holder here, at the election,
                        // which runs before PASS B and before any response acts on that
                        // column: the handover happens first, and the response then
                        // applies to the former anchor as an ordinary float state
                        // (item 4).  Otherwise the response would re-realize the datum
                        // in place.
                        // The incumbent is deposed only by an event on its own basis
                        // column, never by a flag elsewhere at the same station: the
                        // flag test is keyed on (station, this ambiguity key).  RF-2b
                        // counts each deposition cause separately so a run shows why
                        // its datum moved, not only how often.
                        if (held_on) {
                            // Of the three causes the card names (a flag on the basis
                            // column, arc death, the incumbent's own ledger verdict),
                            // only the flag is tested here.
                            //
                            // The ledger verdict cannot be evaluated for an incumbent: a
                            // gauge-pinned column is excluded from the carry at both the
                            // inject and the harvest (the S-basis contract), so the
                            // handover ledger has no live entry for the station holding
                            // the basis.  Testing it deposes the incumbent on almost
                            // every epoch.  A held value is constant by construction;
                            // n_depose_unusable is kept so a build can show it reads 0.
                            //
                            // The age gate is a candidate gate, not an incumbent gate:
                            // an incumbent passed it at its own election.  Testing it
                            // here would also fail for the same structural reason: a
                            // pinned column is not harvested, so its LocalCarry goes
                            // stale and is gap-pruned after phase_leveling_max_gap_s,
                            // after which the age test reads "no carry, so newborn" and
                            // deposes the holder.  The arc-death test for an incumbent is
                            // the sticky loop's own `usable_keys[i].count(amb)` above
                            // (whether this station still tracks the family this epoch).
                            //
                            // The one cause tested here is therefore a screen flag on the
                            // incumbent's own basis column (item 4), and keying the test
                            // on that column keeps a flag elsewhere at the station from
                            // deposing it.
                            bool flagged = false;
                            if (held->flag_quiet_ep > 0 && held->flag_ep) {
                                const auto ft =
                                    held->flag_ep->find({static_cast<int>(i), amb});
                                flagged = ft != held->flag_ep->end()
                                       && held->now_ep - ft->second < held->flag_quiet_ep;
                            }
                            if (flagged) {
                                ++held->n_depose_flag;
                                ++held->n_rank_changed;
                                break;      // fall through to a fresh election
                            }
                        }
                        anchor = i; break;
                    }
        }
        // On a reattach, prefer (in the coverage rank order) a candidate whose arc
        // has converged; only when none has does the second sweep accept a fresh
        // host.  That case is counted, because an unavoidable cold pin is a
        // different event from a clean handover, and INC-3 can ramp its σ (INC-1).
        // The sticky incumbent above is exempt: keeping it is not a reattach.
        // The two sweeps are factored into todayPick() so the candidacy pass below
        // can compute the pick they would make without incrementing their counter
        // twice: first the hygiene-eligible sweep, then the fresh-host fallback
        // (its `cold_fallback_pins` increment is at the single call site).
        auto todayPick = [&]() -> std::pair<size_t, bool> {
            auto cit = cand.find(c);
            if (cit == cand.end()) return {static_cast<size_t>(-1), false};
            for (size_t i : cit->second)
                if (usable_keys[i].count(amb) && hyg.eligibleHost(i, amb)
                    && !banned_sta(i))
                    return {i, false};
            for (size_t i : cit->second)
                if (usable_keys[i].count(amb) && !banned_sta(i))
                    return {i, true};
            return {static_cast<size_t>(-1), false};
        };
        // Cold anchor fallback, first pass (docs/SPEC_event_harness.md §15).
        // Entered only where a handover is about to happen: an incumbent exists for
        // this key and the sticky branch did not keep it (absent, lost the key,
        // banned, or deposed by a basis flag).  A key without an incumbent has no
        // outgoing frame to preserve: the continuous branch cannot run for it and
        // every pin is a 0-pin whichever station takes it, so screening there would
        // move elections without improving them.  The screen includes the hygiene
        // eligibility test, so the per-family pick is always a station the first
        // todayPick() sweep would also accept; the pass can only move the choice
        // further down the same coverage order.  The satellite clause's joint host
        // (below) overrides that pick and only has to track the key.
        const auto inc_it = anchor_incumbent.find(amb);
        int  n_cand_skipped = 0;
        std::string first_unusable;
        bool from_candidacy = false;
        if (anchor == static_cast<size_t>(-1) && cand_usable && cand_usable->on
            && inc_it != anchor_incumbent.end()) {
            // The per-family pass runs whether or not the satellite clause is armed:
            // it owns the four skip counters and the row's cand_skipped /
            // cand_first_unusable, and under the clause its pick is the family-only
            // reference the split census compares against (as `rank_changed` does).
            size_t fam_pick = static_cast<size_t>(-1);
            auto cit = cand.find(c);
            if (cit != cand.end()) {
                for (size_t i : cit->second) {
                    const CandSkip v = candScreen(i, amb, dir, banned);
                    if (v == CandSkip::kNotCandidate) continue;
                    if (v == CandSkip::kOk) { fam_pick = i; break; }
                    // Exactly one reason per skipped candidate, so the four splits
                    // partition `candidacy_skipped_unusable`.
                    ++tally.candidacy_skipped_unusable;
                    switch (v) {
                        case CandSkip::kNoEntry:    ++tally.candidacy_skip_no_entry;    break;
                        case CandSkip::kVarRefused: ++tally.candidacy_skip_var_refused; break;
                        case CandSkip::kFreed:      ++tally.candidacy_skip_freed;       break;
                        default:                    ++tally.candidacy_skip_young;       break;
                    }
                    ++n_cand_skipped;
                    if (first_unusable.empty()) first_unusable = stations[i].name;
                }
            }
            size_t pick = fam_pick;
            // Satellite clause (iteration 2): the satellite's joint successor
            // overrides the family-only pick, so every family of the satellite that
            // moves this epoch lands on one host.  A family the host does not track
            // cannot be pinned there (no ambiguity state) and falls back to the
            // per-family pick; that is the only remaining way the clause can split a
            // satellite, and it is counted (sat_clause_host_untracked).
            const auto sj = sat_joint.find(amb_parts ? amb_parts->satellite : amb);
            if (sj != sat_joint.end()
                && sj->second.host != static_cast<std::size_t>(-1)) {
                const size_t host = sj->second.host;
                if (usable_keys[host].count(amb)) {
                    pick = host;
                    ++tally.sat_clause_families;
                    if (sj->second.colocated) ++tally.sat_clause_colocated;
                    if (fam_pick != static_cast<size_t>(-1) && fam_pick != host)
                        ++tally.sat_clause_split_avoided;
                    if (candScreen(host, amb, dir, banned) != CandSkip::kOk)
                        ++tally.sat_clause_cold_colocated;
                } else {
                    ++tally.sat_clause_host_untracked;
                }
            }
            if (pick != static_cast<size_t>(-1)) {
                anchor = pick;
                from_candidacy = true;
                if (candScreen(pick, amb, dir, banned) == CandSkip::kOk)
                    ++tally.candidacy_usable_elected;
                // candidacy_rank_changed shows whether the pass changed anything:
                // todayPick() is cheap to evaluate alongside, and an arm whose
                // elected station always matches it changed nothing (invariant #4).
                if (todayPick().first != anchor) ++tally.candidacy_rank_changed;
            } else {
                ++tally.candidacy_no_usable_fallback;
                n_cand_skipped = 0;      // the fall-through is the plain election, not a screen
                first_unusable.clear();
            }
        }
        // ---- Anchor re-entry: a return is not a birth (card SID phase 4, S2 read, B1)
        // Entered only where the election is about to treat this family as a first
        // sighting (no incumbent at the previous epoch), the predicate the audit
        // named (E.2: "a returning key is a first sighting").  The read runs before
        // the held-basis rank and the coverage sweeps, so a restore composes with
        // them instead of overriding a decision they made: when the restore
        // declines, every branch below runs unchanged.
        AnchorFirstSighting re_row;
        bool re_row_live = false;
        if (re_on && anchor == static_cast<size_t>(-1)
            && anchor_incumbent.find(amb) == anchor_incumbent.end()) {
            re_row_live = true;
            re_row.amb = amb;
            const std::string r_sat = amb_parts ? amb_parts->satellite : amb;
            const std::string r_sig = amb_parts ? amb_parts->signal : std::string{};
            int n_dead_ = 0;
            anchorReentryFamilyCensus(*reentry->led, r_sat, r_sig,
                                      re_row.n_tracking, re_row.n_returning, n_dead_);
            const auto lie = reentry->state->last_incumbent_ep.find(amb);
            re_row.since_incumbent_ep =
                lie == reentry->state->last_incumbent_ep.end()
                    ? -1 : reentry->now_ep - lie->second;
            const auto memo = reentry->state->drop.find(amb);
            int restore_i = -1;
            if (memo != reentry->state->drop.end()) {
                re_row.prev_anchor = memo->second.prev_anchor;
                re_row.cls = anchorReentryClassify(*reentry->led, memo->second,
                                                   r_sat, r_sig, re_sta_index,
                                                   &restore_i);
                memo->second.returned = true;
            }
            ++reentry->n_first_sighting;
            if (re_row.n_returning > 0) ++reentry->n_first_sighting_return;
            // Restore (B1).  The restored station is subject to the election's own
            // rules, not a second set: it must still track the key (which in this
            // census implies present), must not be the epoch's banned incumbent,
            // must pass the hygiene host test, and under a held S-basis must pass
            // the minimum-age and quiet rules.  A same-generation return whose
            // station fails any of them is counted as refused (with the reason in
            // re_row.refused) and falls through to the normal election.
            if (re_restore && re_row.cls == AnchorReentryClass::SameGeneration) {
                const std::size_t ri = static_cast<std::size_t>(restore_i);
                if (restore_i < 0 || ri >= stations.size()) {
                    re_row.refused = "no_station";
                } else if (!usable_keys[ri].count(amb)) {
                    re_row.refused = "untracked";
                } else if (banned_sta(ri)) {
                    re_row.refused = "banned";
                } else if (!hyg.eligibleHost(ri, amb)) {
                    re_row.refused = "hygiene";
                } else if (!basisEligible(ri, amb)) {
                    re_row.refused = "basis";
                } else {
                    anchor = ri;
                    re_row.restored = true;
                }
                if (re_row.restored) ++reentry->n_restore;
                else                 ++reentry->n_restore_refused;
            } else if (re_restore && re_row.cls == AnchorReentryClass::NewGeneration) {
                ++reentry->n_reentry_new;
            }
        }
        // Held-basis election (RF-2 item 3): the successor is the tracking station
        // with the highest elevation among arcs at least min_age old and not flagged
        // by any screen within flag_quiet_ep.  Runs only when no earlier branch chose
        // an anchor, that is on the elections that set a new basis.  The
        // coverage-ranked sweeps below remain the fallback for a family with no
        // qualifying candidate, so a held-basis run never withholds a family it
        // would otherwise have pinned.
        if (held_on && anchor == static_cast<size_t>(-1)) {
            const auto cit = cand.find(c);
            if (cit != cand.end()) {
                // Arc death (RF-2b): the incumbent exists in the map, but no earlier
                // branch kept it or chose a successor, so the family has lost the
                // station that held its basis.  Counted here, where the sticky loop's
                // result is known, instead of being inferred from a carry map that a
                // pinned column does not populate.  The count also includes an
                // incumbent deposed by a basis flag (already counted in
                // n_depose_flag) or banned by a directive, and it is reached only when
                // the constellation has candidates.
        if (held_on && anchor == static_cast<size_t>(-1)
            && anchor_incumbent.find(amb) != anchor_incumbent.end())
            ++held->n_depose_arc_death;
                // Rank by elevation among ledger-usable candidates first, and fall
                // back to elevation-only when none qualifies (RF-2b).  Usable means
                // the continuous branch's own `ledgerVerdict` accepts the candidate's
                // carried level as a datum; age and quiet alone do not guarantee
                // that, and where the ledger ceiling refuses most mature arcs an
                // elevation-only rank sends most elections cold, each re-realizing
                // the datum at 0.  A family with no usable candidate must still be
                // pinned, so the fallback is kept and counted (n_elev_fallback): a
                // cold pin is a different event from a clean handover.
                size_t best = static_cast<size_t>(-1), best_any = static_cast<size_t>(-1);
                double best_el = -2.0, best_any_el = -2.0;
                for (size_t i : cit->second) {
                    if (!usable_keys[i].count(amb) || banned_sta(i)) continue;
                    if (!basisEligible(i, amb)) continue;
                    const double el = elevOf(i, amb);
                    if (el > best_any_el) { best_any_el = el; best_any = i; }
                    // The verdict the continuous branch below applies to the elected
                    // station, asked of every candidate before one is elected.  As in
                    // the candidacy pass, one predicate serves both: a rank that
                    // elected a station the branch then refuses would produce a cold
                    // handover anyway.
                    if (!ledgerVerdict(ledgerEntry(i, amb), dir && dir->cold).ok) {
                        ++held->n_refused_unusable;
                        continue;
                    }
                    if (el > best_el) { best_el = el; best = i; }
                }
                if (best == static_cast<size_t>(-1) && best_any != static_cast<size_t>(-1)) {
                    best = best_any;                 // elevation-only fallback
                    ++held->n_elev_fallback;
                }
                if (best != static_cast<size_t>(-1)) {
                    const auto tp = todayPick();
                    if (tp.first != best) ++held->n_rank_changed;
                    anchor = best;
                }
            }
        }
        if (anchor == static_cast<size_t>(-1)) {
            const auto pick = todayPick();
            anchor = pick.first;
            if (anchor != static_cast<size_t>(-1) && pick.second)
                ++tally.cold_fallback_pins;
        }
        if (anchor == static_cast<size_t>(-1)) {
            // No tracker at all, or, under a directive, the banned incumbent was the
            // only one.  The family gets no pin this epoch.  The directive counts it
            // so a handover that could not happen is not read as one that did
            // (invariant #7: a decoupling edge with no re-coupling edge).
            if (dir) ++dir->n_no_successor;
            continue;
        }
        // Two-edge continuous handover (docs/SPEC_pivot_visibility_gauge_defect.md).
        //
        // The gauge is "this anchor's ambiguity for `amb` = target".  Pinning a new
        // anchor at 0 realizes a different frame: the whole (sat,signal) family
        // steps by the inter-station ambiguity difference and the satellite clock
        // absorbs it, a gauge change with no S-transform.
        //
        // On a handover, and only then, the target is the incoming anchor's own
        // absolute ambiguity estimate in the outgoing frame, taken from the
        // previous epoch's ledger.  Pinning it there reproduces the frame the
        // outgoing anchor realized, so nothing steps.  A key whose anchor did not
        // move keeps its 0.0 pin, which keeps a settled network byte-identical.
        const auto  inc = anchor_incumbent.find(amb);
        const bool  is_handover = inc != anchor_incumbent.end()
                                  && inc->second != stations[anchor].name;
        double target = 0.0;
        bool   continuous = false;
        double ev_var = -1.0, ev_var_first = 0.0;   // the successor's ledger entry, for the event row
        // A retained incumbent keeps realizing the frame it inherited (R2, §5.09).
        // Otherwise the ledger pin only defers the step by one epoch: the next
        // election re-pins the same anchor at 0 and the family steps at T+1 as it
        // would have at T (§5.07.3).  The map is populated only by continuous
        // handovers below, so the knob-off path never finds an entry and stays
        // byte-identical.
        if (!is_handover && inc != anchor_incumbent.end()) {
            const auto tgt = anchor_pin_target.find(amb);
            if (tgt != anchor_pin_target.end()) {
                target = tgt->second;
                new_target[amb] = target;
            }
        }
        // ---- Held S-basis (RF-2 item 1) ----------------------------------------------
        // A retained incumbent re-pins at the value frozen at its election, read from
        // the basis ledger instead of being re-derived.  On the float_kf vehicle every
        // other ambiguity is a live state; the one that realizes the satellite
        // phase-bias datum is held constant, because a datum that converges, drops and
        // is re-flagged re-realizes, and each re-realization is a phase-bias step the
        // consumer has to re-converge through.
        //
        // This supersedes R2's `anchor_pin_target` above for two reasons.  That map is
        // populated only by continuous handovers, so a cold tenure re-derives 0.0 every
        // epoch and keeps no record of what it holds; and it is rewritten each epoch,
        // so a mid-tenure value change cannot be detected.  The ledger is written once
        // per tenure, and `n_value_drift` counts any attempt to move it; it must read 0
        // on every armed run.
        bool held_this_epoch = false;
        if (held_on && !is_handover && inc != anchor_incumbent.end()) {
            const auto be = held->basis->find(amb);
            if (be != held->basis->end() && be->second.station == stations[anchor].name) {
                if (target != be->second.value && new_target.count(amb))
                    ++held->n_value_drift;   // the property's own alarm; expected 0
                target = be->second.value;
                new_target[amb] = target;
                held_this_epoch = true;
                ++held->n_held;
            }
        }
        if (is_handover && handover) {
            const auto hit = handover->value_abs.find(
                    {static_cast<int>(anchor), amb});
            // Re-validate before re-coupling (invariant #7).  The pin is a hard
            // constraint of weight ~1e10: pinning at an ill-determined estimate is
            // worse than re-zeroing, because it injects that estimate's own error as
            // a datum (an unvalidated target can produce a residual spike on the
            // next epoch larger than the step it replaced).  A value is therefore
            // usable only when its conditional variance is finite, positive and
            // below the caller's ceiling; everything else takes the cold path.
            // network.phase_datum_handover_max_var_ratio adds a ceiling on the σ
            // scale of the arc itself.  The absolute ceiling was sized to the
            // over-counted σ of the diagonal leveling carry
            // (SPEC_amb_iono_split_overcount §6.8.3: 1e-4 m² refuses 93/93 successors
            // under F1/F3's σ; 1e-2 is a hand-set value).  The ratio asks a
            // posture-invariant question instead: has this arc converged since its
            // birth, var <= ratio × its first-stash conditional variance.  An unknown
            // var_first (a fresh arc that was never carried) is refused.  The
            // absolute ceiling stays as a rail; both refusals are counted separately.
            // The predicate has two readers: the candidacy pass above previews it
            // for every candidate, and this branch decides it for the elected one.
            // Both call `ledgerVerdict`, so the screen and the decision agree.  The
            // refusal splits below read the same verdict's fields, so
            // `handover_refused_var` and `handover_refused_ratio` keep their meaning.
            const LedgerVerdict lv = ledgerVerdict(
                    hit != handover->value_abs.end() ? &hit->second : nullptr,
                    dir && dir->cold);            // event harness: forced cold handover
            const bool ratio_on = lv.ratio_on;
            const bool ratio_ok = lv.ratio_ok;
            const bool usable   = lv.ok;
            if (hit != handover->value_abs.end()) {
                ev_var       = hit->second.var;
                ev_var_first = hit->second.var_first;
            }
            if (usable) {
                target     = hit->second.value;
                continuous = true;
                // Persist the target (R2): the successor stays pinned at this value
                // for its whole tenure, so the inherited frame survives past the
                // handover epoch.  Chains compose: the ledger is harvested in the
                // frame the previous pins realized, so each continuous handover
                // extends the same original frame.
                new_target[amb] = target;
                ++tally.handover_continuous;
                tally.handover_max_abs_m =
                        std::max(tally.handover_max_abs_m, std::abs(target));
            } else {
                // Either no estimate for this successor (a fresh arc, or the run's
                // first epoch), or one too poorly determined to serve as a datum.
                // Pin at 0 and count it: the frame steps here, and a re-realization
                // declared to the consumer is a different event from an undeclared one.
                ++tally.handover_cold;
                if (hit != handover->value_abs.end() && hit->second.var > max_var)
                    ++tally.handover_refused_var;
                else if (hit != handover->value_abs.end() && ratio_on && !ratio_ok)
                    ++tally.handover_refused_ratio;
            }
        }
        if (dir) {
            ++dir->n_families;
            if (dir->cold) ++dir->n_forced_cold;
            // A handover that fell back to the cold pin (no ledger entry for the
            // successor, or one refused by the variance gates) re-realizes the
            // gauge, the event anchor-slip handover staging is meant to avoid.  It
            // is counted separately from `n_forced_cold`, which counts harness
            // requests for cold, so a cold outcome is never folded into a
            // continuous total.
            if (is_handover && !continuous) ++dir->n_cold_fallback;
            if (!banned.empty()) dir->retired.push_back(banned);
            dir->elected.push_back(stations[anchor].name);
            // Cold anchor fallback readback: the candidates the candidacy pass
            // skipped to reach this successor (0 and empty with the knob off).
            dir->n_cand_skipped += n_cand_skipped;
            if (dir->cand_first_unusable.empty())
                dir->cand_first_unusable = first_unusable;
        }
        // ---- Write the basis ledger (RF-2 items 1 and 2) ------------------------------
        // Every election that did not re-pin a retained incumbent at its frozen value
        // sets a new basis (a first sighting or a handover).  The entry freezes `target`
        // for the new anchor's whole tenure and records the value it replaced, which is
        // the Δ item 2 declares to the consumer.  For a continuous handover Δ is ~0 by
        // construction and the family does not step; for a cold one the new basis is 0
        // against an outgoing basis of `prev_value`, and the emitted phase bias steps by
        // that difference.  Both kinds are recorded so every handover is declared: only
        // the continuous kind is expressible as an S-transform, and a cold handover
        // otherwise steps the wire with only a carry erase to mark it.
        if (held_on) {
            auto& be = (*held->basis)[amb];
            if (!held_this_epoch) {
                PhaseBasisEntry ne;
                ne.station     = stations[anchor].name;
                ne.value       = target;
                ne.elected_ep  = held->now_ep;
                ne.cold        = is_handover && !continuous;
                ne.prev_value  = be.station.empty() ? 0.0 : be.value;
                ne.prev_station= be.station;
                be = ne;
                ++held->n_elected;
                if (ne.cold) ++held->n_elected_cold;
            }
        }
        stations[anchor].pinned_amb_targets[amb] = target;
        if (is_handover && handovers_out) {
            AnchorHandoverEvent ev{amb, inc->second, stations[anchor].name, target, continuous};
            ev.var       = ev_var;
            ev.var_first = ev_var_first;
            // Cold anchor fallback: the number of better-ranked candidates this
            // handover skipped and the first refused one, so the census of §15 can be
            // re-run from the PBGAUGE rows alone.
            ev.cand_skipped         = n_cand_skipped;
            ev.cand_first_unusable  = first_unusable;
            handovers_out->push_back(std::move(ev));
        }
        // Re-reference marks, item 1(d) (SPEC_pivot_visibility_gauge_defect §5.07.5
        // R1): mark every pin realized in a frame not continuously inherited from
        // an incumbent.  That is a cold handover (the datum re-realizes on a new
        // host) and an incumbent-less pin (inc == end()).  The second class is
        // required: a bridged key skips the election before any incumbent
        // bookkeeping, so its incumbent record is gone after one bridged epoch, and
        // the bridge-exit re-pin (a full frame step against a still-stashed carry)
        // would otherwise read as a first sighting and go unmarked (the "return
        // read as a first sighting" case of invariant #7).  Genuine first sightings
        // are marked too, without harm: for a family with no surviving carry every
        // downstream consumer is a no-op, and a family with a surviving carry
        // (a long-gap return inside the prune horizon) is the stale-frame case the
        // mark exists for.  The key is parsed from the registered pbkey, never
        // split from the amb string.
        // A continuous handover is not marked (R2, §5.09): under the sticky target
        // it realizes the same frame at T and every following epoch, up to the
        // ledger value's own ε ≤ √max_var (the reason the armed configs use a
        // tight ceiling), so the family's carried state stays in frame and
        // marginalizing or erasing it would only truncate arcs.  A first sighting
        // or bridge exit realizes a fresh frame and stays marked.
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
        // The re-entry row is completed here, where the elected station is known, so
        // an S2 case names the station that took the family rather than the one the
        // read suggested (card SID phase 4).
        if (re_row_live) {
            re_row.station = stations[anchor].name;
            reentry->first_sightings.push_back(re_row);
        }
        this_epoch_anchor[amb] = stations[anchor].name;
        new_incumbent[amb]     = stations[anchor].name;
        shared_phase_keys.insert(pbkey);
    }
    // ---- Coverage-gate drop edge (card EV7 §6) -----------------------------------
    // Detected here because this is the last statement at which `anchor_incumbent`
    // still describes the previous epoch, which the second clause of the edge rule
    // requires.  The loop above never visited these keys (the gate erased them from
    // union_keys in networkBuildPhaseDatumCandidates), so nothing below can collide
    // with a handover or first-sighting mark made above.
    //
    // When the gate drops a key, each station's float ambiguity absorbs the bias
    // (see the gate's comment in networkBuildPhaseDatumCandidates).  On the level
    // vehicle that ambiguity is refloated; on the float vehicle it is a carried
    // state at a centimetre prior, which absorbs the removed bias whole in one
    // epoch, and what the prior will not yield lands in the satellite clock and the
    // station (EV7 §5.3).  The return edge already erases the family's carries
    // through `reref_out`; this gives the drop edge the same response, the
    // decoupling half of invariant #7.
    if (gate_drop) {
        for (const std::string& amb
                 : networkPhaseGateDropEdges(cnd.gated_keys, anchor_incumbent)) {
            const auto p = StateKeys::parseAmbiguityKey(amb);
            if (!p) continue;   // an unparsable key cannot address a carry either
            PhaseGateDropEvent ev;
            ev.sat    = p->satellite;
            ev.signal = p->signal;
            const auto inc = anchor_incumbent.find(amb);
            if (inc != anchor_incumbent.end()) ev.prev_anchor = inc->second;
            // The stations still tracking it.  usable_keys is the pre-gate census
            // (the gate erases from union_keys only), so it still names every
            // present station that allocated the column, by definition fewer than
            // min_stations.
            for (std::size_t i = 0; i < usable_keys.size(); ++i) {
                if (!usable_keys[i].count(amb)) continue;
                ++ev.n_trackers;
                if (i < stations.size()) {
                    if (!ev.survivors.empty()) ev.survivors += "|";
                    ev.survivors += stations[i].name;
                }
            }
            // Armed: the family joins the epoch's re-reference set, so the existing
            // erase path drops every station's carried ambiguity of it before PASS B
            // injects the carry.  Off: detected and reported, solve untouched (F2).
            if (gate_drop->on && reref_out) {
                reref_out->insert({p->satellite, p->signal});
                ev.handled = true;
            }
            // Record the drop edge in the re-entry memo (card SID phase 4).  Only
            // here are the family's outgoing anchor and its arcs' generations both
            // known: after the swap below the incumbent map describes this epoch and
            // does not contain the family, which is why a later return would
            // otherwise read as a first sighting.
            if (re_on)
                anchorReentryNoteDrop(*reentry->state, *reentry->led, amb,
                                      p->satellite, p->signal, ev.prev_anchor,
                                      ev.n_trackers, reentry->now_ep);
            gate_drop->events.push_back(std::move(ev));
        }
    }
    // The family's datum history, kept where the election's own memory is swapped
    // (card SID phase 4).  Each family that holds an incumbent at this epoch stamps
    // the epoch; the stamp lets a later first sighting report how long the family
    // has been without one, which separates a gate-drop return from a new rise.
    if (re_on)
        for (const auto& [k, sta] : new_incumbent) {
            (void)sta;
            reentry->state->last_incumbent_ep[k] = reentry->now_ep;
        }
    anchor_incumbent.swap(new_incumbent);
    anchor_pin_target.swap(new_target);
}

int networkStageAnchorSlipHandovers(
        const std::vector<NetStation>& stations,
        const std::map<std::string, std::string>& anchor_incumbent,
        const std::set<std::pair<int, std::string>>& free_ambs,
        const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
        NetworkInjectState& inject) {
    if (anchor_incumbent.empty()
        || (free_ambs.empty() && carry_slipped_ambs.empty()))
        return 0;   // nothing freed, or no incumbent map yet (the run's first epoch)

    // One directive per (sat, signal) family, not per freed arc.  Two stations can
    // slip the same satellite in one epoch and only one of them can be the anchor,
    // but the de-duplication also covers a family named by both freed sets and
    // keeps `anchorFor`'s first-match contract unambiguous.
    std::set<std::string> staged;
    int n = 0;

    const auto consider = [&](const std::set<std::pair<int, std::string>>& freed) {
        for (const auto& [i, amb] : freed) {
            if (i < 0 || static_cast<std::size_t>(i) >= stations.size()) continue;
            const auto inc = anchor_incumbent.find(amb);
            if (inc == anchor_incumbent.end()) continue;       // family has no anchor
            if (inc->second != stations[i].name) continue;     // a non-anchor arc: no-op
            if (!staged.insert(amb).second) continue;          // already staged
            const auto parts = StateKeys::parseAmbiguityKey(amb);
            if (!parts) continue;
            InjectAnchorDirective d;
            d.sat    = parts->satellite;
            d.signal = parts->signal;
            d.cold   = false;   // continuous: the successor is pinned at its own
                                // carried value_abs, so the family does not step.
                                // A cold outcome is still possible (no usable
                                // ledger entry) and is counted.
            inject.anchor_now_slip.push_back(std::move(d));
            ++n;
        }
    };
    // Both vehicles.  `free_ambs` is the phase-leveling free-epoch latch; on a
    // carry_invariant lineage that latch is structurally empty (the loader
    // disarms relevel_free_epoch there) and the DD funnel carries the same pairs.
    consider(free_ambs);
    consider(carry_slipped_ambs);
    return n;
}

int networkApplyPhaseAnchorCarryReref(
        const std::set<std::pair<std::string, std::string>>& reref,
        std::map<std::tuple<int, std::string, std::string>, LocalCarry>& local_carry,
        std::vector<std::pair<std::tuple<int, std::string, std::string>, LocalCarry>>*
            erased_out) {
    if (reref.empty() || local_carry.empty()) return 0;
    // Same erase as eq34 D4 (NetworkChainDatum.cpp): drop the family's per-station
    // ambiguity carries at every station.  The frame stepped, so the carried floats
    // are stale; they refloat next epoch.
    int n_erased = 0;
    for (auto it = local_carry.begin(); it != local_carry.end();) {
        const auto& key = it->first;   // (station idx, sat, signal)
        if (reref.count({std::get<1>(key), std::get<2>(key)})) {
            // Capture the entry before the erase (RF-1 §7.4, D3).  This whole-family,
            // every-station refloat is what design §3.6 replaces with a declared,
            // exact S-transformation (step 5); the captured inventory is the
            // before/after that arm is graded on.
            if (erased_out) erased_out->emplace_back(key, it->second);
            it = local_carry.erase(it);
            ++n_erased;   // returned; the caller folds it into the retirement census
        } else {
            ++it;
        }
    }
    return n_erased;
}

} // namespace GnssProcessor
