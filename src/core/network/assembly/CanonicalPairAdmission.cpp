/**
 * @file CanonicalPairAdmission.cpp
 * @brief Canonical-pair admission (design in CanonicalPairAdmission.hpp).
 */
#include "network/assembly/CanonicalPairAdmission.hpp"

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"      // systemName, systemFromName, SignalId
#include "config/GnssConfig.hpp"
#include "network/driver/RunTypes.hpp"      // NetStation (roster + its PppModel)
#include "network/diag/RunTraces.hpp"
#include "model/PppModel.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>

namespace GnssProcessor {

    namespace {

        /// @brief The station's canonical pairs, keyed by constellation display name.
        ///
        /// Read directly from the model's latch: @c PppModel::exportCanonicalSignals
        /// returns one row per decided (constellation, band, obs-type), so a
        /// constellation absent from the result has not been decided yet.  The four
        /// slots are filled by the RINEX band digit of each latched signal code,
        /// which matches what the emulation meant by "band 1" and "band 2" and needs
        /// no second band-name table.  A constellation that cannot fill all four
        /// (structurally Galileo, whose band digits are 1/5/7/6) ends with
        /// @c CanonicalPair::decided() false and is never screened.
        std::map<std::string, CanonicalPair> latchedPairs(const PppModel& model) {
            std::map<std::string, CanonicalPair> out;
            for (const auto& e : model.exportCanonicalSignals()) {
                const auto signal = SignalId::fromObsCode(systemFromName(e.constellation), e.signal);
                if (!signal) continue;
                // The pair's bands are the digits 1 and 2 (a band role).
                const bool band1 = signal->digit == '1';
                if (!band1 && signal->digit != '2') continue;
                CanonicalPair& p = out[e.constellation];
                if (e.obs_type == 'L') (band1 ? p.l1 : p.l2) = e.signal;
                else if (e.obs_type == 'C') (band1 ? p.c1 : p.c2) = e.signal;
            }
            return out;
        }

        /// @brief True when all four canonical observables are present in the record.
        ///
        /// "Present" means a key in @c measurements: the parser inserts nothing for a
        /// blank RINEX field, nor, under @c processing.rinex_skip_zero_values, for a
        /// written 0.0 sentinel, so key presence is the tracking state.
        bool pairHeld(const RinexObservation& o, const CanonicalPair& p) {
            return o.measurements.count(p.l1) && o.measurements.count(p.c1)
                && o.measurements.count(p.l2) && o.measurements.count(p.c2);
        }

    } // namespace

    // =====================================================================
    // The pure state machine
    // =====================================================================

    CanonicalPairStep canonicalPairStep(CanonicalPairTrack& tr, bool good,
                                        long long epoch_idx, int keep) {
        CanonicalPairStep s;
        if (good) {
            // The re-coupling edge is read before the state is updated: the gap it
            // reports is the run of withheld epochs that ends here, which sizes what
            // was reconnected.
            s.readmit_edge = tr.withheld_ep > 0;
            s.gap_ep       = tr.withheld_ep;
            s.action       = CanonicalPairAction::AdmitGood;
            tr.last_good   = epoch_idx;
            tr.withheld_ep = 0;
            return s;
        }
        // Not good.  A pair that has never been good is before_first regardless of
        // the tolerance: the tolerance carries admission forward from a good epoch,
        // and there is none to carry from.
        const bool never = tr.last_good < 0;
        const long long since = never ? 0 : epoch_idx - tr.last_good;
        if (!never && since <= static_cast<long long>(keep)) {
            s.action = CanonicalPairAction::AdmitWithinKeep;
            s.gap_ep = since;
            // An epoch admitted within the tolerance crosses neither edge: the pair
            // was not withheld, so nothing opens or closes here.
            return s;
        }
        s.action = never ? CanonicalPairAction::WithholdBeforeFirst
                         : CanonicalPairAction::WithholdBeyondKeep;
        s.withhold_edge = tr.withheld_ep == 0;   // the decoupling edge, once per run of withholds
        s.gap_ep        = since;
        ++tr.withheld_ep;
        return s;
    }

    const char* canonicalPairReason(CanonicalPairAction a) {
        switch (a) {
        case CanonicalPairAction::WithholdBeforeFirst: return "before_first";
        case CanonicalPairAction::WithholdBeyondKeep:  return "beyond_keep";
        default:                                       return "";
        }
    }

    // =====================================================================
    // Arming
    // =====================================================================

    void networkCanonicalPairArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                 int n_sta) {
        auto& st = ctx.carry.cpa;
        st.armed = config_.networkCanonicalPairAdmission();
        st.keep_ep = std::max(0, config_.networkCanonicalPairKeepEpochs());
        if (!st.armed) return;
        st.pairs.assign(static_cast<std::size_t>(std::max(0, n_sta)), {});
    }

    // =====================================================================
    // The per-epoch rule
    // =====================================================================

    const SyncedEpoch& networkCanonicalPairAdmission(
            NetworkRunContext& ctx,
            const std::shared_ptr<spdlog::logger>& log,
            NetworkRunTraces& traces,
            const std::vector<NetStation>& stations,
            const SyncedEpoch& se_in,
            SyncedEpoch& scratch) {
        auto& st = ctx.carry.cpa;
        if (!st.armed) return se_in;           // byte-identical path: no copy, no walk

        auto& ctr = ctx.counters.cpa;
        // The processed-epoch clock of this card.  It is a separate counter, not
        // the slip-QC index: that index is advanced inside PASS A, downstream of
        // this seam, and an epoch PASS A skips does not advance it, so a gap
        // measured against it would stop counting where a station dropped out.
        // One increment per seam invocation counts every epoch that reached the
        // epoch body, which is the clock the emulation used.
        const long long epoch_idx = st.epochs++;
        ++ctr.epochs;

        // Pass 1 decides on the caller's epoch and records what must be removed.
        // The copy is taken only if pass 1 found something, so an armed epoch that
        // withholds nothing costs one walk and no allocation.
        std::vector<std::pair<int, std::string>> kill;   // (station index, satellite)

        for (std::size_t i = 0; i < stations.size(); ++i) {
            const auto sit = se_in.by_station.find(stations[i].name);
            if (sit == se_in.by_station.end() || sit->second.empty()) continue;
            auto& cache = st.pairs[i];

            for (const auto& o : sit->second) {
                if (o.system == System::Unknown) continue;
                const std::string cons = systemName(o.system);

                auto pit = cache.find(cons);
                if (pit == cache.end()) {
                    // First sighting of this constellation at this station, or the
                    // latch was still undecided at the previous lookup.  The latch
                    // is written once and never revised, so the refresh stops once
                    // it yields an entry.
                    if (!stations[i].model) continue;
                    auto fresh = latchedPairs(*stations[i].model);
                    auto fit = fresh.find(cons);
                    if (fit == fresh.end()) {
                        // Undecided: the station has not yet run computeModels for
                        // this constellation (the latch for its first epoch is
                        // decided inside PASS A, downstream of here).  Left
                        // untouched and retried next epoch.
                        ++ctr.unscreened;
                        continue;
                    }
                    pit = cache.emplace(cons, fit->second).first;
                }
                if (!pit->second.decided()) {
                    // A constellation the latch decided but that has no canonical
                    // pair on band digits 1 and 2 (Galileo's digits are 1/5/7/6) is
                    // outside this rule and is left untouched.
                    ++ctr.unscreened;
                    continue;
                }

                ++ctr.screened;
                const bool good = pairHeld(o, pit->second);
                const auto key  = std::make_pair(static_cast<int>(i), o.satellite);
                const auto step = canonicalPairStep(st.track[key], good, epoch_idx,
                                                    st.keep_ep);

                if (step.action == CanonicalPairAction::AdmitWithinKeep)
                    ++ctr.admit_within_keep;

                if (step.readmit_edge) {
                    ++ctr.readmit_edges;
                    ctr.max_gap_ep = std::max(ctr.max_gap_ep, step.gap_ep);
                    traces.emitEvent(se_in.time, "ADMISSION", "canonical_pair",
                                     o.satellite, stations[i].name,
                                     static_cast<double>(step.gap_ep),
                                     static_cast<double>(st.keep_ep), "readmit",
                                     fmt::format("pair_returned;withheld_ep={};pair={}+{}+{}+{}",
                                                 step.gap_ep, pit->second.l1,
                                                 pit->second.c1, pit->second.l2,
                                                 pit->second.c2));
                }
                if (!canonicalPairWithholds(step.action)) continue;

                const char* reason = canonicalPairReason(step.action);
                if (step.action == CanonicalPairAction::WithholdBeforeFirst)
                    ++ctr.withheld_before_first;
                else
                    ++ctr.withheld_beyond_keep;
                ++ctr.withheld_total;
                st.affected.insert(key);
                if (step.withhold_edge) {
                    ++ctr.withhold_edges;
                    traces.emitEvent(se_in.time, "ADMISSION", "canonical_pair",
                                     o.satellite, stations[i].name,
                                     static_cast<double>(step.gap_ep),
                                     static_cast<double>(st.keep_ep), "withhold",
                                     fmt::format("{};since_good_ep={};pair={}+{}+{}+{}",
                                                 reason, step.gap_ep, pit->second.l1,
                                                 pit->second.c1, pit->second.l2,
                                                 pit->second.c2));
                    if (ctr.withhold_edges == 1)
                        log->debug("Network: first canonical-pair WITHHOLD at "
                                   "TOW {:.1f}: station '{}' satellite {} ({}; canonical "
                                   "{}+{}+{}+{}).",
                                   se_in.time.tow, stations[i].name, o.satellite, reason,
                                   pit->second.l1, pit->second.c1, pit->second.l2,
                                   pit->second.c2);
                }
                kill.emplace_back(key);
            }
        }

        if (kill.empty()) return se_in;         // nothing withheld: no copy this epoch

        // Pass 2: the single copy, then the erase.  The whole record leaves the
        // epoch (never a per-signal edit), so every downstream consumer
        // (computeModels, the T0 detectors, networkComputeRawObservables, assembly,
        // the RAIM pools, the traces) sees what it would see had the RINEX record
        // been missing, and the row-level gap re-entry tier reads the return as a
        // re-entry rather than as a continuing arc.
        scratch = se_in;
        ++ctr.epochs_copied;
        std::size_t k = 0;
        while (k < kill.size()) {
            const int sta = kill[k].first;
            std::size_t k_end = k;
            while (k_end < kill.size() && kill[k_end].first == sta) ++k_end;
            auto sit = scratch.by_station.find(stations[static_cast<std::size_t>(sta)].name);
            if (sit != scratch.by_station.end()) {
                std::set<std::string> drop;
                for (std::size_t m = k; m < k_end; ++m) drop.insert(kill[m].second);
                auto& v = sit->second;
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [&](const RinexObservation& o) {
                                           return drop.count(o.satellite) != 0;
                                       }),
                        v.end());
            }
            k = k_end;
        }
        return scratch;
    }

    // =====================================================================
    // Run-end readout
    // =====================================================================

    void networkCanonicalPairRunEnd(const NetworkRunContext& ctx,
                                    const std::shared_ptr<spdlog::logger>& log) {
        const auto& st = ctx.carry.cpa;
        if (!st.armed) return;
        const auto& c = ctx.counters.cpa;
        log->info("Network (card CPA / CANONICAL-PAIR ADMISSION, keep={} epoch(s)): {} "
                  "epoch(s) screened {} (station,satellite) record(s) against a decided "
                  "canonical pair ({} left alone: their constellation has no canonical "
                  "code+phase on band digits 1 and 2); WITHHELD {} record(s) = {} "
                  "before_first + {} beyond_keep, over {} distinct (station,satellite) "
                  "pair(s), on {} epoch(s); {} record(s) were admitted by the tolerance "
                  "alone.  EDGES (invariant 7): {} withhold, {} readmit, longest withheld "
                  "run that came back {} epoch(s).{}",
                  st.keep_ep, c.epochs, c.screened, c.unscreened, c.withheld_total,
                  c.withheld_before_first, c.withheld_beyond_keep,
                  static_cast<long long>(st.affected.size()), c.epochs_copied,
                  c.admit_within_keep, c.withhold_edges, c.readmit_edges, c.max_gap_ep,
                  c.withheld_total == 0
                      ? (c.screened == 0
                             ? "  ⚠ ARMED AND INERT: ZERO records were screened at all: "
                               "no station latched a canonical code+phase pair on band "
                               "digits 1 and 2, so the rule cannot act.  This is dead "
                               "wiring, not a quiet run."
                             : "  ⚠ ARMED AND INERT: ZERO records were withheld although "
                               "the screen ran.  Every screened record held its canonical "
                               "pair; read this as a property of the data, and check it "
                               "against the screened denominator before crediting the rule.")
                      : "");
    }

    // ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    // Run-end report: the canonical-pair admission (CPA).
    int networkCanonicalPairRunEnd(const NetworkRunEndView& v) {
        const NetworkRunContext& ctx = v.ctx;
        const std::shared_ptr<spdlog::logger>& log = v.log;

        // Canonical-pair admission run-end (
        // network/assembly/CanonicalPairAdmission.cpp): the withheld census and both edges, in
        // one line.  Silent when the rule is disarmed; the ARMED AND INERT clause covers an
        // armed run whose counters never moved (invariant #4).  Read before the zero-emit
        // guard for the same reason as the injector's clause: a rule that withheld the whole
        // network has to be visible on a run that also produced nothing.
        networkCanonicalPairRunEnd(ctx, log);
        return 0;
    }

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
