/**
 * @file NetworkPhaseDatum.cpp
 * @brief Prioritized-datum spectrum-based drop-and-heal (see the header).
 *
 * Decomposition INC-8 (2026-07-17) — body relocated VERBATIM from
 * ApplicationNetwork.cpp; former lambda captures are explicit parameters
 * (capture→param map in the INC-8 cut spec).
 */

#include "filter/NetworkInject.hpp"
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
    //
    // Card EV7 §6: `gated_amb` is HOISTED out of the `pdp_min_sta > 1` branch so it
    // can be handed to the caller below in every posture (empty when the gate is
    // off).  Nothing inside the branch changed — the DROP EDGE needs the set, and a
    // second census would double-count the two gate counters this function owns.
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
    // Card EV7 §6 (F1): the gate's exclusions, for the DROP-EDGE predicate.  Moved
    // like every other member so the census stays structurally fresh per call.
    cnd.gated_keys  = std::move(gated_amb);
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
        double max_var_ratio,
        const NetworkInjectState* inject,
        const AnchorCandidacyUsable* cand_usable,
        PhaseHeldBasis* held,
        PhaseGateDrop* gate_drop,
        AnchorReentry* reentry) {
    const auto& usable_keys = cnd.usable_keys;
    // Card SID phase 4.  Null (the default) on every unarmed run, so every branch
    // below that tests it is unreachable and the election is byte-identical; armed
    // in its OBSERVE half only (S2) it reads and counts and still changes nothing.
    const bool re_on      = reentry != nullptr && reentry->observe
                            && reentry->led != nullptr && reentry->state != nullptr;
    const bool re_restore = re_on && reentry->restore;
    // Station name -> index, built once per call and only where the read is armed:
    // the drop memo names its incumbent by NAME (the incumbent map's own key type)
    // while the ledger keys on the INDEX, and resolving one lookup at a time over
    // `stations` would make the read O(families x stations).
    std::map<std::string, int> re_sta_index;
    if (re_on)
        for (std::size_t i = 0; i < stations.size(); ++i)
            re_sta_index[stations[i].name] = static_cast<int>(i);
    // RF-2 — the HELD S-BASIS.  Null (the default) on every unarmed run, so every
    // branch below that tests it is unreachable and the election is byte-identical.
    const bool held_on = held && held->on && held->basis;
    /// RF-2 item 3: this candidate's LOS elevation (deg) for @p amb, or −1 when the
    /// elevation map is absent / has no entry — which sorts LAST, so a station whose
    /// elevation we do not know never outranks one we do.
    const auto elevOf = [&](std::size_t i, const std::string& amb) -> double {
        if (!held_on || !held->elev || i >= held->elev->size()) return -1.0;
        const auto p = StateKeys::parseAmbiguityKey(amb);
        if (!p) return -1.0;
        const auto it = (*held->elev)[i].find(p->satellite);
        return it == (*held->elev)[i].end() ? -1.0 : it->second;
    };
    /// RF-2 item 3 / item 4: may station @p i hold the S-BASIS for @p amb?  Two gates,
    /// both from the card: the arc must be OLDER than min_age (never a newborn low arc —
    /// a leg born at a re-admission carries its code-level birth error, and the pin
    /// stamps that in at the gauge weight), and it must not have been flagged by ANY
    /// screen within flag_quiet_ep (the datum must be the quietest arc available, not
    /// merely the oldest).  Counted, so a family with no qualifying candidate is visible
    /// rather than silently falling back.
    const auto basisEligible = [&](std::size_t i, const std::string& amb) -> bool {
        if (!held_on) return true;
        if (held->min_age_ep > 0 && held->carry) {
            const auto p = StateKeys::parseAmbiguityKey(amb);
            if (!p) return false;
            const auto it = held->carry->find(
                std::make_tuple(static_cast<int>(i), p->satellite, p->signal));
            // No carry at all is a NEWBORN by definition — refused.  An entry with a
            // non-positive birth stamp reads MATURE (the restored-arc convention).
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

    // ---- THE LEDGER USABILITY PREDICATE — one definition, two readers ----------
    // COLD ANCHOR-FALLBACK (docs/SPEC_event_harness.md §15).  The continuous branch
    // far below decides whether a successor's carried value may serve as the datum;
    // the candidacy pass asks the SAME question of every candidate BEFORE one is
    // elected.  Sharing the predicate is load-bearing, not tidiness: a screen that
    // drifted from the decision it previews would elect a station the branch then
    // refuses — the defect with extra steps.  The forced-cold directive is part of
    // the predicate (the harness ASKING for cold must make every candidate unusable,
    // so the pass cannot quietly re-pick the station a cold leg lands on).
    struct LedgerVerdict {
        bool ok = false;          ///< usable as a datum
        bool has_entry = false;   ///< a ledger entry exists at all
        bool var_over = false;    ///< entry present and var above the ABSOLUTE ceiling
        bool ratio_on = false;    ///< the honest-σ-scale ceiling is armed
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
    // COLD ANCHOR-FALLBACK: was arc (i, amb) FREED this epoch?  A slip response that
    // freed an arc is about to re-draw its level from one epoch of code, so its
    // carried value is not a datum however good last epoch's variance looks —
    // standing invariant #1's re-coupling edge, asked at candidacy time.  Both
    // vehicles (the leveling free-epoch latch and the DD invariant-carry funnel).
    auto candFreed = [&](size_t i, const std::string& amb) {
        if (!cand_usable) return false;
        const std::pair<int, std::string> k{static_cast<int>(i), amb};
        return (cand_usable->free_ambs && cand_usable->free_ambs->count(k) != 0)
            || (cand_usable->slipped_ambs && cand_usable->slipped_ambs->count(k) != 0);
    };
    // COLD ANCHOR-FALLBACK: is arc (i, amb) MATURE?  Age is n_loop_epochs − birth_ep
    // of the station's own phase-leveling carry, asked through localCarryYoung —
    // the ONE definition, shared with the F3 block admission gate.  An UNKNOWN age —
    // no carry entry, an unstamped one, or a warm-resume-restored entry carrying
    // LocalCarry::kBirthEpRestored (SPEC_event_harness §15.4) — counts as MATURE,
    // the same restored-entry convention the F3 block gate and the young-arc
    // inflation use: the conservative direction is to let the variance ceiling
    // decide rather than refuse a candidate the ledger says has converged.
    auto candMature = [&](size_t i, const std::string& amb) {
        if (!cand_usable || cand_usable->min_age_ep <= 0 || !cand_usable->local_carry)
            return true;
        const std::size_t us = amb.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= amb.size()) return true;
        const auto it = cand_usable->local_carry->find(
                {static_cast<int>(i), amb.substr(0, us), amb.substr(us + 1)});
        if (it == cand_usable->local_carry->end()) return true;   // never carried ⇒ unknown
        return !localCarryYoung(it->second, cand_usable->now_ep,
                                cand_usable->min_age_ep);
    };

    // THE CANDIDACY ACCEPTANCE TEST — one definition, three readers: the per-family
    // pass, the satellite clause's joint scoring, and the co-located cold decision.
    // It returns WHY a candidate was refused so the four skip splits stay a partition
    // of `candidacy_skipped_unusable` (invariant #4: a total with no partition cannot
    // say whether the ledger is thin or the maturity bar too tight).
    enum class CandSkip { kOk, kNotCandidate, kNoEntry, kVarRefused, kFreed, kYoung };
    auto candScreen = [&](size_t i, const std::string& amb,
                          const InjectAnchorDirective* dir_,
                          const std::string& banned_name) -> CandSkip {
        if (!usable_keys[i].count(amb) || !hyg.eligibleHost(i, amb)
            || (!banned_name.empty() && stations[i].name == banned_name))
            return CandSkip::kNotCandidate;   // not a candidate at all — never counted
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

    // ---- ITERATION 2: the SATELLITE clause -----------------------------------
    // docs/SPEC_event_harness.md §15.8.  Iteration 1 screened per FAMILY, so a
    // satellite's three bands could each elect the host whose carry was best for
    // THEIR band — correct one band at a time and, jointly, an inter-frequency
    // inconsistency the per-band producer instruments cannot see (measured: the
    // storm arm's producer channels all improved while the consumer median rose
    // 49 % and split-referenced satellites went 0.0 → 0.9 mean / 3 max, §15.6).
    //
    // So the handover is resolved per SATELLITE, ahead of the per-family loop:
    //   (a) every family of s moving THIS epoch takes ONE successor — the first
    //       coverage-ranked candidate usable for the largest number of them (ties
    //       → coverage order).  A family that host is not usable for is still
    //       pinned THERE, through the honest cold path: a cold pin on the same
    //       host is a frame step, not a split.
    //   (b) when only SOME of s's families move, the station already anchoring the
    //       others is tried FIRST.
    // A satellite with no usable candidate for any mover is left to today's sweeps.
    struct SatJoint {
        std::vector<std::string> moving;                  ///< amb keys handing over
        std::size_t host = static_cast<std::size_t>(-1);  ///< the joint successor
        bool colocated = false;                           ///< rule (b) decided it
    };
    std::map<std::string, SatJoint> sat_joint;
    if (cand_usable && cand_usable->on && cand_usable->satellite_clause) {
        // Which families move is decided HERE with the same predicate the loop
        // applies below (sticky ∧ present ∧ tracks ∧ not banned keeps the pin), so
        // the pre-pass and the loop cannot disagree about the population.
        std::map<std::string, std::set<std::string>> held;   // sat -> stations keeping a pin
        for (const auto& [amb, pbkey] : union_keys) {
            if (hyg.isBridged(amb)) continue;
            const std::size_t us = amb.find('_');
            if (us == std::string::npos || us == 0) continue;
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
            if (keeps) held[amb.substr(0, us)].insert(inc->second);
            else       sat_joint[amb.substr(0, us)].moving.push_back(amb);
        }
        for (auto& [sat, j] : sat_joint) {
            const char c2 = sat.empty() ? char(0) : sat.front();
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
            // (b) first.  Taking the held host whenever it is usable for AT LEAST
            // ONE mover is the only reading under which the satellite cannot split:
            // letting a higher-scoring stranger win would re-create exactly the
            // defect this iteration exists to remove, and a family the held host
            // cannot serve takes a CO-LOCATED cold pin instead.
            const auto h = held.find(sat);
            if (h != held.end())
                for (std::size_t i : cit->second)
                    if (h->second.count(stations[i].name) && nUsable(i) > 0) {
                        j.host = i;
                        j.colocated = true;
                        break;
                    }
            // (a) otherwise the joint maximum, in coverage order.
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
        // EVENT HARNESS: a staged anchor_handover retires this family's incumbent
        // for THIS epoch only.  It is expressed as a candidacy ban rather than a
        // direct assignment so the successor is chosen by the production rules
        // (coverage rank, hygiene eligibility, cold fallback) — the harness forces
        // the EVENT, never the outcome.
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
                        // RF-2 items 3 + 4: "an incumbent is not deposed WHILE IT
                        // QUALIFIES" — so stickiness now asks the basis question too.
                        // A T4 / slip / gross flag on the BASIS ambiguity therefore
                        // deposes the holder HERE, at the election, which runs BEFORE
                        // PASS B and before any response acts on that column: the
                        // handover happens first and the response then applies to the
                        // ex-anchor as an ordinary float state (item 4).  Without this
                        // the datum is re-realized in place by whatever the response
                        // does to it, which is the churn the card names.
                        // ⚠ RF-2b: the incumbent is deposed ONLY by something about
                        // ITS OWN BASIS COLUMN — never by a flag elsewhere at the same
                        // station.  `basisEligible` is keyed on (station, THIS ambiguity
                        // key), so the flag test is already column-scoped; what RF-2b
                        // adds is the third cause and the per-cause split, so a run can
                        // say WHY its datum moved instead of only how often.
                        //   * flag on the basis column   — a T4 / T3 / slip response on
                        //     this exact arc.  Item 4: the handover happens HERE, before
                        //     PASS B and before the response acts on that column;
                        //   * arc death                  — the basis arc has no carry any
                        //     more (pruned / never stashed), so there is no level to
                        //     hold and no age to qualify on;
                        //   * ledger verdict             — the incumbent's OWN carried
                        //     level has stopped being usable as a datum (var over the
                        //     ceiling).  Holding a value whose arc has drifted out of
                        //     tolerance is exactly the "+17.08 m target, 378-σ spike"
                        //     failure the continuous branch's gate exists to stop.
                        if (held_on) {
                            // ⚠ MEASURED, and the reason the third cause is NOT tested
                            // here.  The card names three: a flag on the basis column,
                            // the arc's death, and "its own ledger verdict".  The third
                            // is STRUCTURALLY UNAVAILABLE for an incumbent: a
                            // gauge-pinned column is excluded from the carry at BOTH the
                            // inject and the harvest (the S-basis contract), so the
                            // handover ledger has no live entry for the station that is
                            // currently holding the basis.  Testing it deposed the
                            // incumbent EVERY epoch — measured on the first RF-2b sim
                            // arm: 44 008 "ledger verdict" deposals against 1 293 held
                            // family-epochs and 43 879 re-elections, i.e. the basis was
                            // re-elected almost every epoch and "held" in name only.
                            //
                            // That is not a tuning miss, it is a category error: a HELD
                            // value is a constant BY CONSTRUCTION, and re-validating it
                            // against a ledger that structurally does not track it is
                            // incoherent.  What an incumbent still has is its column's
                            // flag history and whether it has an arc at all — so those
                            // are the two live causes, and the ledger counter is kept so
                            // a future build can show it reads 0.
                            // ⚠ And the AGE gate is a CANDIDATE gate, not an incumbent
                            // one.  "Never a newborn low arc" is a question about a
                            // station being ELECTED; an incumbent already answered it at
                            // its own election and has been holding since.  Worse, asking
                            // it of an incumbent is self-defeating for the same
                            // structural reason as the ledger: a pinned column is not
                            // harvested, so its LocalCarry goes stale and is gap-pruned
                            // after phase_leveling_max_gap_s — and the age test then
                            // reads "no carry ⇒ newborn" and deposes the holder for the
                            // crime of having held.  The REAL arc-death test is the
                            // sticky loop's own `usable_keys[i].count(amb)` above: does
                            // this station still TRACK the family this epoch.
                            //
                            // So the incumbent is deposed by exactly one live cause here
                            // — a screen flag on ITS OWN basis column (item 4) — and the
                            // column scoping is what makes "not by a flag elsewhere at
                            // its station" true by construction.
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
        // INC-1: on a REATTACH prefer, in the existing coverage rank order, a
        // candidate whose arc has actually converged; only if none has does the
        // second sweep accept a fresh host (counted — an unavoidable cold pin is not
        // the same event as a clean handover, and INC-3 can ramp its σ).  The sticky
        // incumbent above is deliberately exempt: keeping it is not a reattach.
        // Today's two sweeps, factored so the candidacy pass below can compute the
        // pick they WOULD make without bumping their counter twice.  Behaviour is
        // unchanged: first the hygiene-eligible sweep, then the fresh-host fallback
        // (whose `cold_fallback_pins` bump moves to the single call site).
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
        // COLD ANCHOR-FALLBACK (docs/SPEC_event_harness.md §15) — the FIRST pass.
        // Entered only where a HANDOVER is about to happen: an incumbent exists for
        // this key and the sticky branch did not keep it (absent, lost the key, or
        // banned).  An incumbent-LESS key has no outgoing frame to preserve — the
        // continuous branch cannot run for it and every pin is a 0-pin whichever
        // station takes it — so screening there would move elections this card
        // cannot improve.  The hygiene eligibility test rides along, which makes the
        // elected station always one today's FIRST sweep would also have accepted:
        // the pass can only move the choice LATER down the same coverage order.
        const auto inc_it = anchor_incumbent.find(amb);
        int  n_cand_skipped = 0;
        std::string first_unusable;
        bool from_candidacy = false;
        if (anchor == static_cast<size_t>(-1) && cand_usable && cand_usable->on
            && inc_it != anchor_incumbent.end()) {
            // The per-FAMILY pass runs whether or not the satellite clause is armed:
            // it owns the four skip counters and the row's cand_skipped /
            // cand_first_unusable, and under the clause it is also the FAMILY-ONLY
            // pick the split census is measured against (the `rank_changed` idiom).
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
            // ITERATION 2: the satellite's JOINT successor overrides the family-only
            // pick, so every family of the satellite that moves this epoch lands on
            // ONE host.  A family the host does not TRACK cannot be pinned there at
            // all (no ambiguity state) and falls back to the per-family path — the
            // only residual way this clause can still split, and counted as such.
            const auto sj = sat_joint.find(amb.substr(0, amb.find('_')));
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
                // The number that proves the pass DID something: today's sweep is
                // cheap to evaluate alongside, and an arm whose elected station
                // always matches it changed nothing however clean its log looks.
                if (todayPick().first != anchor) ++tally.candidacy_rank_changed;
            } else {
                ++tally.candidacy_no_usable_fallback;
                n_cand_skipped = 0;      // the fall-through is today's election, not a screen
                first_unusable.clear();
            }
        }
        // ---- Card SID phase 4 (S2 read, B1 action): A RETURN IS NOT A BIRTH ------
        // Entered only where the election is about to treat this family as a FIRST
        // SIGHTING — no incumbent at the previous epoch — which is the exact
        // predicate the audit named (E.2: "a returning key is a first sighting").
        // The read runs BEFORE the held-basis rank and the coverage sweeps, so a
        // restore composes with them rather than overriding a decision they made:
        // if the restore declines, every branch below runs exactly as it does today.
        AnchorFirstSighting re_row;
        bool re_row_live = false;
        if (re_on && anchor == static_cast<size_t>(-1)
            && anchor_incumbent.find(amb) == anchor_incumbent.end()) {
            re_row_live = true;
            re_row.amb = amb;
            const std::size_t us2 = amb.find('_');
            const std::string r_sat = us2 == std::string::npos ? amb : amb.substr(0, us2);
            const std::string r_sig = us2 == std::string::npos ? std::string{}
                                                               : amb.substr(us2 + 1);
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
            // B1.  The restored station is subject to the election's OWN rules, not
            // to a second set: it must still track the key (which in this census
            // implies present), must not be the epoch's banned incumbent, must pass
            // the hygiene host test, and under a held S-basis must pass the
            // minimum-age and quiet rules.  A same-generation return whose station
            // fails any of them is COUNTED as refused and falls through to today's
            // election — never silently pinned anyway.
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
        // RF-2 item 3: THE ELECTION.  "the successor is the tracking station with the
        // HIGHEST ELEVATION among arcs older than min_age and not flagged by any screen
        // within flag_quiet_ep."  Runs only when stickiness did not keep an incumbent,
        // i.e. exactly on the elections that set a new basis; the coverage-ranked sweeps
        // below stay in place as the fallback for a family with no qualifying candidate,
        // so a held-basis run never withholds a family it would otherwise have pinned.
        if (held_on && anchor == static_cast<size_t>(-1)) {
            const auto cit = cand.find(c);
            if (cit != cand.end()) {
                // RF-2b: ARC DEATH is the sticky loop's own miss — the incumbent exists in the
        // map but was not found present-and-tracking above, so the family has lost the
        // station that held its basis.  Counted here, where the loop's verdict is known,
        // rather than inferred from a carry map that a pinned column does not populate.
        if (held_on && anchor == static_cast<size_t>(-1)
            && anchor_incumbent.find(amb) != anchor_incumbent.end())
            ++held->n_depose_arc_death;
        // ⚠ RF-2b, THE ONE-PREDICATE FIX.  RF-2's rank REPLACED the ledger
                // screen instead of COMPOSING with it: it asked for age and quiet but
                // never whether the candidate's carried level is usable AS A DATUM by
                // the continuous branch's own `ledgerVerdict`.  On the field most
                // mature high-elevation arcs also pass the 0.01 m² ceiling, so 89 % of
                // handovers came out continuous; on the sim, whose ambiguity scale is
                // ~5× larger, they mostly do NOT — 67 % of elections went cold, each
                // re-realized the datum at 0, and σ̂0 blew past the withhold ceiling
                // (362 of 720 epochs withheld, WL integers 99.0 % → 35.2 %).
                //
                // So: rank by elevation among LEDGER-USABLE candidates first, and fall
                // back to elevation-only when none qualifies — a family with no usable
                // candidate must still be pinned, and the fallback is COUNTED, because
                // a cold pin is a different event from a clean handover.
                size_t best = static_cast<size_t>(-1), best_any = static_cast<size_t>(-1);
                double best_el = -2.0, best_any_el = -2.0;
                for (size_t i : cit->second) {
                    if (!usable_keys[i].count(amb) || banned_sta(i)) continue;
                    if (!basisEligible(i, amb)) continue;
                    const double el = elevOf(i, amb);
                    if (el > best_any_el) { best_any_el = el; best_any = i; }
                    // THE COMPOSED PREDICATE: the SAME verdict the continuous branch
                    // below applies to the elected station, asked of every candidate
                    // before one is elected.  Sharing it is load-bearing, not tidiness
                    // (the pre-existing candidacy pass makes the same argument): a rank
                    // that elects a station the branch then refuses is the defect with
                    // extra steps.
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
            // No tracker at all — or, under a directive, the banned incumbent was
            // the ONLY one.  The family gets no pin this epoch; count it so a
            // handover that could not happen is never read as one that did
            // (invariant #7: the decoupling edge with no re-coupling edge).
            if (dir) ++dir->n_no_successor;
            continue;
        }
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
        // ---- RF-2 item 1: THE HELD S-BASIS ------------------------------------------
        // A RETAINED incumbent re-pins at the value FROZEN AT ITS ELECTION, read from the
        // basis ledger rather than re-derived.  That is the whole card in one branch: on
        // the float_kf vehicle every OTHER ambiguity is a live state, and the one that
        // realizes the satellite phase-bias datum must not be — a datum that converges,
        // drops and is re-flagged is a datum that re-realizes, and every re-realization
        // is a phase-bias step the consumer re-converges through (the field measured T4
        // hitting the anchor 454 times an hour).
        //
        // It supersedes R2's `anchor_pin_target` above for two reasons the counters keep
        // honest: that map is populated ONLY by continuous handovers, so a COLD tenure
        // silently re-derives 0.0 every epoch and has no record of what it holds; and it
        // is re-written each epoch, so nothing can detect a mid-tenure value change.  The
        // ledger is written ONCE per tenure, and `n_value_drift` counts any attempt to
        // move it — which must read 0 on every armed run.
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
            // COLD ANCHOR-FALLBACK: this predicate now has TWO readers — the
            // candidacy pass above previews it for every candidate, this branch
            // decides it for the elected one.  ONE lambda (`ledgerVerdict`), so the
            // screen can never drift from the decision.  The refusal splits below
            // read the same verdict's fields, which keeps `handover_refused_var` /
            // `handover_refused_ratio` exactly what they were.
            const LedgerVerdict lv = ledgerVerdict(
                    hit != handover->value_abs.end() ? &hit->second : nullptr,
                    dir && dir->cold);            // EVENT HARNESS: forced COLD handover
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
        if (dir) {
            ++dir->n_families;
            if (dir->cold) ++dir->n_forced_cold;
            // A handover that FELL BACK to the cold pin (no ledger entry for the
            // successor, or one refused by the variance gates) re-realizes the
            // gauge — the exact event the anchor-slip card exists to avoid.  It is
            // counted separately from `n_forced_cold`, which is the harness ASKING
            // for cold: a cold outcome must never hide inside a continuous total.
            if (is_handover && !continuous) ++dir->n_cold_fallback;
            if (!banned.empty()) dir->retired.push_back(banned);
            dir->elected.push_back(stations[anchor].name);
            // COLD ANCHOR-FALLBACK readback: what the candidacy pass had to walk
            // past to reach this successor (0 / empty with the knob off).
            dir->n_cand_skipped += n_cand_skipped;
            if (dir->cand_first_unusable.empty())
                dir->cand_first_unusable = first_unusable;
        }
        // ---- RF-2 items 1 + 2: WRITE THE BASIS LEDGER ------------------------------
        // Every election that did NOT re-pin a retained incumbent at its frozen value is
        // a NEW BASIS: a first sighting, or a handover.  The entry freezes `target` for
        // the whole of the new anchor's tenure and records what it REPLACED, which is the
        // Δ item 2 declares to the consumer — for a CONTINUOUS handover Δ is (by
        // construction of the continuous branch) ~0 and the family does not step, while
        // for a COLD one the new basis is 0 against an outgoing basis of `prev_value`, so
        // the emitted phase bias steps by exactly that difference.  Recording it for BOTH
        // is the card's "every handover declared": today only the continuous half is
        // expressible as an S-transform, and the cold half — 55 of 81 field handovers —
        // steps the wire with nothing but an erase to show for it.
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
            // COLD ANCHOR-FALLBACK: the census of §15 can then be re-run from the
            // PBGAUGE rows alone — how many better-ranked candidates this handover
            // walked past, and who the first refused one was.
            ev.cand_skipped         = n_cand_skipped;
            ev.cand_first_unusable  = first_unusable;
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
        // Card SID phase 4: the row is completed HERE, where the elected station is
        // finally known, so an S2 case states who actually took the family rather
        // than who the read suggested.
        if (re_row_live) {
            re_row.station = stations[anchor].name;
            reentry->first_sightings.push_back(re_row);
        }
        this_epoch_anchor[amb] = stations[anchor].name;
        new_incumbent[amb]     = stations[anchor].name;
        shared_phase_keys.insert(pbkey);
    }
    // ---- Card EV7 §6: THE COVERAGE-GATE DROP EDGE ------------------------------
    // HERE and nowhere else: this is the last statement at which `anchor_incumbent`
    // still describes the PREVIOUS epoch, which is the whole second clause of the
    // edge rule.  The loop above never visited these keys — the gate erased them
    // from union_keys inside networkBuildPhaseDatumCandidates — so nothing below
    // can collide with a handover or a first-sighting mark made above.
    //
    // The defect (EV7 §5.3): the gate's comment promises that "each station's float
    // ambiguity absorbs the bias", which on the level vehicle was a refloat and on
    // the FLOAT vehicle is a CARRIED state at a centimetre prior.  It absorbs the
    // removed bias whole, in one epoch, silently (VOIM00MDG G30 L1W +15.829 m at a
    // variance of 2.8e-4 m^2), and what the prior will not yield lands in the
    // satellite clock and the station.  The RETURN edge already erases the family's
    // carries through `reref_out`; this gives the DROP edge the same response, which
    // is standing invariant #7's decoupling half.
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
            // Who is LEFT holding it.  usable_keys is the PRE-gate census (the gate
            // erases from union_keys only), so it still names every present station
            // that allocated the column — by definition fewer than min_stations.
            for (std::size_t i = 0; i < usable_keys.size(); ++i) {
                if (!usable_keys[i].count(amb)) continue;
                ++ev.n_trackers;
                if (i < stations.size()) {
                    if (!ev.survivors.empty()) ev.survivors += "|";
                    ev.survivors += stations[i].name;
                }
            }
            // ARMED: the family joins the epoch's re-reference set, so the existing
            // erase path drops every station's carried ambiguity of it before PASS B
            // injects the carry.  OFF: detected and reported, solve untouched (F2).
            if (gate_drop->on && reref_out) {
                reref_out->insert({p->satellite, p->signal});
                ev.handled = true;
            }
            // Card SID phase 4: REMEMBER the edge.  This is the only statement at
            // which the family's outgoing anchor and its arcs' generations are both
            // knowable — one line later the incumbent map describes this epoch and
            // the family is not in it at all, which is precisely why the return
            // reads as a first sighting today.
            if (re_on)
                anchorReentryNoteDrop(*reentry->state, *reentry->led, amb,
                                      p->satellite, p->signal, ev.prev_anchor,
                                      ev.n_trackers, reentry->now_ep);
            gate_drop->events.push_back(std::move(ev));
        }
    }
    // Card SID phase 4: the family's datum history, kept where the election's own
    // memory is swapped.  A family that holds an incumbent at this epoch stamps
    // the epoch; the stamp is what lets a later first sighting say HOW LONG the
    // family has been without one, which is the quantity that separates a
    // gate-drop return from a genuine new rise.
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

    // One directive per (sat, signal) FAMILY, not per freed arc: two stations can
    // slip the same satellite in one epoch and only one of them can be the anchor,
    // but a defensive de-duplication costs nothing and keeps `anchorFor`'s
    // first-match contract unambiguous.
    std::set<std::string> staged;
    int n = 0;

    const auto consider = [&](const std::set<std::pair<int, std::string>>& freed) {
        for (const auto& [i, amb] : freed) {
            if (i < 0 || static_cast<std::size_t>(i) >= stations.size()) continue;
            const auto inc = anchor_incumbent.find(amb);
            if (inc == anchor_incumbent.end()) continue;       // family has no anchor
            if (inc->second != stations[i].name) continue;     // a NON-anchor arc: no-op
            if (!staged.insert(amb).second) continue;          // already staged
            const std::size_t us = amb.find('_');
            if (us == std::string::npos || us == 0 || us + 1 >= amb.size()) continue;
            InjectAnchorDirective d;
            d.sat    = amb.substr(0, us);
            d.signal = amb.substr(us + 1);
            d.cold   = false;   // CONTINUOUS: the successor is pinned at its own
                                // carried value_abs, which is the whole cure.  A
                                // cold OUTCOME is still possible (no usable ledger
                                // entry) and is counted, never hidden.
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
    // Verbatim the eq34 D4 erase (NetworkChainDatum.cpp): drop the family's
    // per-station ambiguity carries at EVERY station — the frame stepped, so the
    // carried floats are stale; refloat next epoch.
    int n_erased = 0;
    for (auto it = local_carry.begin(); it != local_carry.end();) {
        const auto& key = it->first;   // (station idx, sat, signal)
        if (reref.count({std::get<1>(key), std::get<2>(key)})) {
            // RF-1 §7.4 D3: capture BEFORE the erase.  This is the whole-FAMILY,
            // every-station refloat design §3.6 replaces with a declared, exact
            // S-transformation (step 5) — its inventory is the before/after that
            // arm is graded on.
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
