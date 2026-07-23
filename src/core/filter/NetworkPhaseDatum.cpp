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
#include <map>
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
        std::map<std::string, std::string>& anchor_incumbent,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys) {
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
    }
    this_epoch_anchor.clear();
    shared_phase_keys.clear();
    std::map<std::string, std::string> new_incumbent;
    for (const auto& [amb, pbkey] : union_keys) {
        const char c = amb.empty() ? char(0) : amb.front();
        size_t anchor = static_cast<size_t>(-1);
        if (sticky) {
            auto it = anchor_incumbent.find(amb);
            if (it != anchor_incumbent.end())
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i] && stations[i].name == it->second
                        && usable_keys[i].count(amb)) { anchor = i; break; }
        }
        if (anchor == static_cast<size_t>(-1)) {
            auto cit = cand.find(c);
            if (cit != cand.end())
                for (size_t i : cit->second)
                    if (usable_keys[i].count(amb)) { anchor = i; break; }
        }
        if (anchor == static_cast<size_t>(-1)) continue;   // no tracker (unreachable)
        stations[anchor].pinned_amb_targets[amb] = 0.0;   // prioritized: pin to 0
        this_epoch_anchor[amb] = stations[anchor].name;
        new_incumbent[amb]     = stations[anchor].name;
        shared_phase_keys.insert(pbkey);
    }
    anchor_incumbent.swap(new_incumbent);
}

} // namespace GnssProcessor
