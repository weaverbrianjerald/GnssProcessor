/**
 * @file ApplicationNetworkAssembly.cpp
 * @brief Assembly-phase member functions of Application::runNetwork: the
 *        pre-loop column-split and iono-row helpers, PASS-A geometry, the
 *        observability prefilter, the shared-column registration, and PASS-B
 *        produce/consume.
 *
 * The bodies originate in ApplicationNetwork.cpp and were moved here by the
 * network decomposition (docs/network_decomposition_plan.md, INC-7).  Former
 * run-TU [&] lambdas are private Application member functions; their ambient
 * captures are explicit parameters or member reads (the capture-to-parameter map
 * is recorded in the INC-7 cut spec).  The include block mirrors
 * ApplicationNetwork.cpp (INC-1 precedent); unused-include cleanup is deferred to
 * INC-12.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // emit-layer frame re-base (Card 4, M1 phase D)
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic (M4)
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume (RTB-7)
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts (Card 5, RTF-1)
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (Card 5, RTF-1)
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkExtraBandScope.hpp"  // the extra-band / primary-pair predicates (card L5P)
#include "filter/NetworkSecondCodeScope.hpp" // secondCodeCellEdges, the two cell edges (card SCC)
#include "filter/NetworkSecondPhaseScope.hpp" // the cell edges + the witness (card SID phase 6a)
#include "filter/NetworkPivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "filter/NetworkCarryVce.hpp"        // LS-VCE carry-row variance-component monitor (M2)
#include "filter/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (Card 4, M1 C3b)
#include "filter/NetworkSdEwlAr.hpp"               // the shared EWL band table (sdEwlBands) + increment-2a decision (SD EWL)
#include "filter/NetworkCoupling.hpp"       // conditional coupling-row builder (Card 1/1d, INC-7b)
#include "filter/NetworkParamInventory.hpp" // the parameter inventory and the receiver IFB rule (card NOM N1)
#include "filter/NetworkContributions.hpp" // row tags and the contribution tallies (card NOM N2a)
#include "filter/NetworkPrefitCensus.hpp"   // structural fuse (SPEC_prefit_census_observability, INC-2)
#include "filter/NetworkShDevVtec.hpp"     // networkShDevCarryAge (card NOM3 step PB1a)
#include "filter/NetworkStationBuild.hpp"  // networkAssemblyTally (card NOM3 step PB1a)
#include "filter/NetworkSolveStations.hpp" // the consume and the in-solve track (PB1a)
#include "filter/NetworkChainDatum.hpp"    // networkEq34PinAudit (card NOM3 step PB1a)
#include "filter/NetworkArSuspect.hpp"      // arSuspectNote: the AR search's suspect-corner stamps (card ARL)
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // run-loop record types: NetStation / StationAssembly / carry PODs (INC-2)
#include "filter/NetworkRobustStats.hpp"   // median/MAD robust attribution helpers (INC-2)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "logging/NetworkRunTraces.hpp" // trace-stream ownership + open logic + roster + FlagRate (INC-3)
#include "app/NetworkRunContext.hpp"    // ctx: knobs + Tier-1 carry state + service counters (INC-4)
#include "app/NetworkSetupResult.hpp"   // setup-phase handoff (setup) + navAvailabilityGpsSeconds (INC-5)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, Increment 2)
#include "slip/TurboEditResolver.hpp"   // per-signal MW+GF integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // slip-QC pure helpers: ifElevMap / parseIfPairResetKey (INC-2)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // identity reads (systemOfSat, ObservableId, ...), carriers
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic (B5)
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to 1 thread inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)

namespace GnssProcessor {

EpochAction Application::networkPassAHead_(NetworkRun& run, const NetworkEpoch& ep,
                                           EpochPassA& pass_a) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    std::vector<bool>& present = pass_a.present;  // written
    std::set<std::string>& shared_phase_keys = pass_a.shared_phase_keys;  // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic): the body reads the former runNetwork()
    // locals under their original names, and the former epoch-loop `continue`s are
    // the EpochAction::SkipEpoch returns (their site-specific bookkeeping is kept
    // as it was).
    auto& stations = setup.stations;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };
    // CB-ADM phase co-gate feed: the run-latched admitted code-key set (null unless
    // the flag is armed and the census ran).  It is passed into every
    // ensureNetworkSharedStates call, so a signal whose code was quarantined also
    // allocates no phase-bias state, and shared_phase_keys (built from the states)
    // excludes it consistently.  Null: byte-identical.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    // Legacy single-pivot datum: the pivot is modelled first, so its usable phase
    // signals restrict the shared phase-bias columns to those with a pivot-receiver
    // ambiguity pin, and an absent pivot skips the epoch (no datum).  Prioritized
    // mode treats the pivot as an ordinary station (modelled in the tail loop from
    // i_lo = 0, with no special skip) and elects the per-(sat,signal) anchors after
    // the cross-station prune, so this block is skipped and a missing pivot does not
    // cost the whole epoch (the pivot is not a single point of failure).
    if (!phase_datum_prioritized) {
        NetStation& piv = stations.front();
        auto it = se.by_station.find(piv.name);
        if (it == se.by_station.end() || it->second.empty()) {
            // No datum station this epoch, so no clean phase-bias gauge: skip.
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.pstate.epoch_time = se.time;
        // The pivot's own elevation-mask / no-model writer (card SID phase 4a, S1).
        // Reached only on the legacy (non-prioritized) datum, where station 0 is
        // modelled here instead of in the parallel tail; both working bases run the
        // prioritized datum, so this block is inert there and the tail below covers
        // the whole population.
        std::map<std::string, DropReason> piv_drops;
        if (!piv.model->computeModels(it->second, piv.pstate, modeled.front(),
                                      ctx.reasons.armed ? &piv_drops : nullptr)
            || modeled.front().empty()) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.mm->ensureNetworkSharedStates(modeled.front(), {}, cbadm_admit);  // allow all pivot phase signals (minus CB-ADM quarantined)
        present.front() = true;
        for (const auto& [sat, why] : piv_drops) {
            if (why == DropReason::None) continue;
            arcReasonExpand(ctx.reasons, ctx.arc_ledger, 0, sat, "",
                            why == DropReason::BelowMask ? kArcReasonMask
                                                         : kArcReasonNoModel);
        }
    }

    if (!phase_datum_prioritized)
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    return EpochAction::Proceed;
}

void Application::networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                        NetworkSlipFunnel& funnel) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    double dt = ep.clock.dt;
    double pl_now_s = ep.clock.pl_now_s;
    bool carry_warm = ep.clock.carry_warm;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    std::vector<bool>& present = pass_a.present;  // written
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = funnel.carry_slipped_raw;  // written
    SlipQcChannel& qc = run.engines.slip_qc;
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    auto log = log_manager_.getLogger("main_log");
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    // Card ARL: the producer AR search's suspect-corner mask is armed (the MW admission
    // records its unknown-integer identities only then).
    const bool ar_suspect_armed = ctx.knobs.ar_suspect_epochs > 0;
    auto& local_carry = ctx.carry.local_carry;
    auto& fr_if_slip = ctx.counters.fr_if_slip;
    // CB-ADM phase co-gate feed (see networkPassAHead_): null unless the flag is
    // armed and the census ran.  Withholds a quarantined signal's phase-bias state.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    const int i_lo = phase_datum_prioritized ? 0 : 1;

    // PASS A tail (stations[i_lo..]: from 1 on the legacy datum, from 0 when
    // prioritized), parallel over stations when enabled.  computeModels and
    // ensureNetworkSharedStates touch only station i's own sm/model/mm/pstate;
    // shared_phase_keys is read-only here and modeled[i] is a distinct slot per i,
    // so there is no shared write.  Each iteration catches its own exceptions,
    // because an exception must not escape an OpenMP structured block (UB).
    // present[] is a std::vector<bool> (packed bits, so concurrent writes to
    // adjacent indices race), so threads record into a per-index char buffer that
    // is folded into present[] serially afterwards.
    // PASS-A slip-scan seam (INC-6): the per-station buffers that the parallel loop
    // below fills and the serial slip-QC drains (slip/NetworkSlipQc.cpp) consume.
    PassABuffers pa(stations.size());
    auto& a_ok           = pa.a_ok;
    auto& a_err          = pa.a_err;
    auto& sta_repairs    = pa.sta_repairs;
    auto& sta_if_resets  = pa.sta_if_resets;
    auto& sta_mw_refloat = pa.sta_mw_refloat;
    // The elevation-mask and no-model writers (card SID phase 4a, S1).  The refusal
    // is taken inside computeSatelliteGeometry, which carries no station identity,
    // so it is captured through the model's own per-satellite drop-reason map and
    // folded here, where the station index is known.  One map per station index:
    // the loop below is OpenMP-parallel, so a shared buffer would race (the same
    // reason present[] is folded through a per-index char buffer).  Allocated only
    // while the writers are armed, so an unarmed run pays one bool test.
    const bool sid_screens = ctx.reasons.armed;
    std::vector<std::map<std::string, DropReason>> pa_drops(
        sid_screens ? stations.size() : 0);

    #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
    for (int i = i_lo; i < n_sta_total; ++i) {
        try {
            NetStation& st = stations[i];
            auto it = se.by_station.find(st.name);
            if (it == se.by_station.end() || it->second.empty()) continue;
            st.pstate.epoch_time = se.time;
            if (!st.model->computeModels(
                    it->second, st.pstate, modeled[i],
                    sid_screens ? &pa_drops[static_cast<std::size_t>(i)] : nullptr)
                || modeled[i].empty())
                continue;
            // Prioritized: every station (including the former pivot, i=0)
            // allocates all its own trackable phase signals; the anchored shared
            // set is the post-election union, not a pivot restriction.
            if (phase_datum_prioritized)
                st.mm->ensureNetworkSharedStates(modeled[i], {}, cbadm_admit);
            else
                st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys, cbadm_admit);
            a_ok[i] = 1;

            // Slip detection (T0): roll this station's MW + GF arcs forward and
            // collect per-(sat,signal) integer repairs.  Station-local detector
            // state, no shared write, safe in the parallel region.  Runs every
            // epoch (the resolver has to advance its GF arcs even with no slips)
            // and regardless of the carry warmup, so the adaptive noise floors are
            // warm when the carry activates.  A detection failure costs only this
            // station's repairs for this epoch, not the station itself.
            if (st.mw_slip) {
                try {
                    const auto mwr = st.mw_slip->detectLargeMwResets(modeled[i]);
                    if (st.turbo_edit) {
                        // turbo_edit (1 s data): MW+GF → integer repair of the carry.
                        auto er = st.turbo_edit->resolve(
                            mwr, st.mw_slip->getEpochCleanPairs(), modeled[i], dt,
                            st.mw_slip->getAcrossGapWlEvidence(pl_now_s));
                        sta_repairs[i] = std::move(er.repairs);
                    } else if (config_.networkCycleSlipMwDetect()) {
                        // mw_detect (any rate): detect only, and refloat confident
                        // wide-lane integer jumps (no GF, no integer applied).  frac
                        // gates rounding cleanliness: a BIA-less float re-init rounds
                        // to a large spurious integer but with a large frac (left
                        // alone); a known non-zero integer with a small frac is a real
                        // WL slip.  LLI loss-of-lock is handled directly in the serial
                        // drain.  The branch is gated on mw_detect explicitly; the
                        // detector is built only under turbo_edit or mw_detect, so after
                        // the TurboEdit branch the test always holds.
                        //
                        // MW evidence refloat (network.cycle_slip.mw_evidence_refloat):
                        // the frac gate above is the T0 repair gate, and this path never
                        // applies an integer, so a strong, mature, bias-free-lane flag is
                        // also admissible on the strength of the detection.  The census
                        // below runs regardless of the knob; with the knob off the rule is
                        // evaluated and counted but never admits (byte-identical).
                        const double frac_max = config_.networkCycleSlipTurboEditFracMax();
                        const bool   ev_on    = config_.networkCycleSlipMwEvidenceRefloat();
                        const double ev_ratio = config_.networkCycleSlipMwEvidenceRatioMin();
                        const int    ev_narc  = config_.networkCycleSlipMwEvidenceMinArcEpochs();
                        // Pair scope (card L5P).  Under 'primary' the network forms
                        // only the pair whose two signals both sit on the
                        // constellation's ionosphere-free pin bands; on a lineage
                        // that latches one signal per band, that is the station's
                        // latched band-1 / band-2 pair.  A pair carrying any other
                        // band is not formed at all: no candidate, no refloat, and no
                        // entry in the common-band scope's evidence below, so the
                        // detector's view of the epoch is the two-band one.  The
                        // band-5 phase keeps LLI, gap re-entry, IF-TD and the band
                        // scope, which is what gate G2 grades (invariant #1).
                        // 'all' (the default) evaluates the lambda to true for
                        // every pair, so an unarmed run is byte-identical.
                        const bool mw_pairs_primary =
                            config_.networkCycleSlipMwDetectPairs()
                                == NetworkMwDetectPairs::Primary;
                        std::map<char, NetworkPinBands> mw_pin_by_sys;
                        const auto mw_pair_ok =
                            [&](const std::string& sat, const std::string& s1,
                                const std::string& s2) {
                                if (!mw_pairs_primary) return true;
                                if (sat.empty()) return true;
                                const char cons = letterOfSat(sat);
                                auto pit = mw_pin_by_sys.find(cons);
                                if (pit == mw_pin_by_sys.end())
                                    pit = mw_pin_by_sys.emplace(
                                              cons, networkPinBandsFor(cons)).first;
                                return networkMwPairInPrimaryScope(s1, s2, pit->second);
                            };
                        for (const auto& r : mwr) {
                            if (r.sig1.empty() || r.sig2.empty()) continue;
                            if (!mw_pair_ok(r.satellite, r.sig1, r.sig2)) {
                                ++pa.sta_mw_admit[i].scope_skipped;
                                continue;
                            }
                            const auto d = networkMwAdmitCandidate(
                                r.slip_wl_known, r.slip_wl_cycles, r.slip_frac, frac_max,
                                r.evidence_ratio, r.arc_epochs, r.bia_corrected,
                                ev_on, ev_ratio, ev_narc);
                            auto& ac = pa.sta_mw_admit[i];
                            if (d.admit_legacy)    ++ac.legacy_admitted;
                            if (d.via_evidence)    ++ac.evidence_admitted;
                            if (d.would_admit)     ++ac.evidence_would_admit;
                            if (d.drop_loose)      ++ac.drop_loose;
                            if (d.drop_unknown) {
                                ++ac.drop_unknown;
                                if (ar_suspect_armed) ac.drop_unknown_sats.push_back(r.satellite);
                            }
                            if (d.admitted)
                                sta_mw_refloat[i].push_back(PassABuffers::MwRefloatCand{
                                    r.satellite, r.sig1, r.sig2,
                                    r.slip_wl_cycles, r.slip_frac, r.lambda_wl,
                                    r.slip_wl_known, r.evidence_ratio, r.arc_epochs,
                                    d.via_evidence});
                        }

                        // Common-band scope evidence (SPEC_event_harness §14),
                        // collected regardless of the knob (invariant #4) from what
                        // this station's detector already produced: every arc it
                        // processed a sample for, with that sample's maturity and
                        // threshold; every flag, admitted or dropped (a dropped flag
                        // is still evidence); and the receiver's own LLI bits.  It
                        // reads the processed map, not getEpochCleanPairs: that set
                        // answers TurboEdit's cleanliness question, whose σ-awareness
                        // bar a narrow lane on a noisy bed never meets (see
                        // MwSlipDetector::getEpochPairStatus).  Per-station slot, no
                        // shared write.  The IF-TD half is a network-pooled decision
                        // and is previewed serially in the verdict seam.
                        // The pair scope (card L5P) applies here as well: a pair the
                        // network did not form must not testify about which bands an
                        // epoch implicated.  Leaving it in the evidence would mean the
                        // detector still "formed" it, just without a candidate, which
                        // is not what the knob claims.  Under 'all' the predicate is
                        // true for every pair, so an unarmed run's evidence is
                        // byte-identical.
                        auto& cbev = pa.sta_cb_ev[i];
                        for (const auto& [pkey, pst] : st.mw_slip->getEpochPairStatus()) {
                            std::string csat, cs1, cs2;
                            if (!parseIfPairResetKey(pkey, csat, cs1, cs2)) continue;
                            if (!mw_pair_ok(csat, cs1, cs2)) continue;
                            CommonBandProcessed cp;
                            cp.arc_n     = pst.arc_n_at_sample;
                            cp.flagged   = pst.flagged;
                            cp.thresh_wl = (pst.lambda_wl > 0.0)
                                               ? pst.thresh_m / pst.lambda_wl
                                               : std::numeric_limits<double>::quiet_NaN();
                            cp.lli_shadow = pst.lli_shadow;   // rule F2 (§14.11)
                            cbev[csat].processed[{cs1, cs2}] = cp;
                        }
                        for (const auto& r : mwr) {
                            if (r.sig1.empty() || r.sig2.empty()) continue;
                            if (!mw_pair_ok(r.satellite, r.sig1, r.sig2)) continue;
                            cbev[r.satellite].flagged_pairs.insert({r.sig1, r.sig2});
                        }
                        for (const auto& obs : modeled[i])
                            for (const auto& [code, lli] : obs.lli_flags)
                                if (obsTypeOf(code) == ObsType::Phase && (lli & 1))
                                    cbev[obs.satellite].lli_signals.insert(code);
                    }
                } catch (...) { /* best-effort: no MW action this epoch */ }
            }

            // IF-phase triple-difference slip detection: between-satellite,
            // between-epoch IF-phase OMC.  Station positions are fixed near truth,
            // so position_sigma_3d = 0 disables the pre-convergence gate (the gate
            // that keeps this detector staged on other paths), and st.ecef makes
            // the inter-epoch geometry correction a no-op (Δpos ≡ 0).  Detector
            // state is station-local, no shared write, safe here.  Best-effort: a
            // failure costs only this station's flags for this epoch.
            if (st.if_slip) {
                try {
                    sta_if_resets[i] = st.if_slip->detect(modeled[i], 0.0, st.ecef);
                } catch (...) { /* best-effort: no IF-phase flags this epoch */ }
            }
        } catch (const std::exception& ex) {
            a_err[i] = ex.what();
        } catch (...) {
            a_err[i] = "unknown exception";
        }
    }

    // Serial drain: commit present[] and log diagnostics in station order.  Under
    // the prioritized datum i_lo = 0 includes station 0 (an ordinary station there);
    // on the legacy path i_lo = 1, and station 0's present was already set by the
    // pivot-first block in networkPassAHead_.
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (a_ok[i]) present[i] = true;
        else if (!a_err[i].empty())
            log->warn("Network: station '{}' PASS A geometry threw ({}); "
                      "skipping it this epoch", stations[i].name, a_err[i]);
    }

    // Fold the drop reasons serially, in station order (card SID phase 4a, S1).  The
    // refusal is per satellite, so it expands to that station's arcs of that
    // satellite that the admitted lifecycle still calls alive (the brief's rule for a
    // screen whose identity is coarser than an arc).  A station that threw is left
    // out: its whole epoch is missing, which is a station outage and not a
    // per-satellite screen, and the ledger freezes an absent station's arcs.
    if (sid_screens)
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!a_ok[i]) continue;
            for (const auto& [sat, why] : pa_drops[static_cast<std::size_t>(i)]) {
                unsigned bit = kArcReasonNoModel;
                if (why == DropReason::BelowMask) bit = kArcReasonMask;
                else if (why == DropReason::None) continue;
                arcReasonExpand(ctx.reasons, ctx.arc_ledger, i, sat, "", bit);
            }
        }

    // --- Second code channel, per-epoch bookkeeping, R7 (card SCC) --------------
    // (docs/refound/SCC_second_code_channel_2026-09-21.md.)  Serial and in station
    // order, so the counters and the event rows are deterministic and the edges are
    // diffed against a set only this thread touches.  Placed after present[] is
    // final (an absent station's cells close, which is the decoupling edge) and
    // before any later reader of `modeled`; this block only reads it.
    //
    // Two tasks.  (1) Republish each station's companion cell set for PASS B: the
    // row-weight seam has the signal and the satellite of every row but no way to
    // tell a companion from the fallback case (a band-1 code that arrived without
    // its canonical code beside it), and both route through the same receiver
    // state.  (2) Count the cells, both cell edges, the receiver inter-signal states
    // alive and their reseeds, and the (satellite, signal) columns that reached the
    // two-station registration bar only through companions (the card's thesis as a
    // number).
    //
    // Off: the whole block is skipped, every set stays empty and nothing is emitted,
    // so the epoch is byte-identical.
    if (config_.networkSecondCodeRowsActive()) {
        auto& C = ctx.counters.second_code;
        // Per-(satellite, signal): how many present stations support the column at
        // all, and how many of those support it through the canonical code (not a
        // companion row).  The registration bar is two stations (see the code-bias
        // column registration below), so a column with >= 2 supporters of which < 2
        // are canonical is one that the companion rows put on the wire.
        std::map<std::string, std::pair<int, int>> col_support;   // "sat|signal" -> (total, canonical)
        long long isc_alive = 0;
        for (int i = 0; i < n_sta_total; ++i) {
            NetStation& st = stations[i];
            std::set<std::string> cells;
            if (present[i] && i < static_cast<int>(modeled.size())) {
                for (const auto& ob : modeled[static_cast<std::size_t>(i)]) {
                    for (const auto& [sig, val] : ob.observed) {
                        (void)val;
                        if (obsTypeOf(sig) != ObsType::Code) continue;
                        const std::string key = StateKeys::makeSatBiasKey(ob.satellite, sig);
                        auto& sup = col_support[key];
                        ++sup.first;
                        if (ob.companion_codes.count(sig)) cells.insert(key);
                        else                               ++sup.second;
                    }
                }
            }
            // The two edges (standing invariant #7).  A cell opens when a station
            // starts contributing a companion row for a satellite and signal, and
            // closes when it stops, including when the whole station drops out (the
            // case the invariant names: a station with zero rows drops out of the map
            // and its return reads as a first sighting).
            std::vector<std::string> opened, closed;
            secondCodeCellEdges(st.scc_cells_prev, cells, &opened, &closed);
            for (const auto& cell : opened) {
                const auto parts = StateKeys::parseSatBiasKey(cell);
                traces.emitEvent(se.time, "SCC", "companion_open",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "open",
                                 parts ? parts->signal : cell);
            }
            for (const auto& cell : closed) {
                const auto parts = StateKeys::parseSatBiasKey(cell);
                traces.emitEvent(se.time, "SCC", "companion_close",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "close",
                                 parts ? parts->signal : cell);
            }
            C.cell_open  += static_cast<long long>(opened.size());
            C.cell_close += static_cast<long long>(closed.size());
            C.cells      += static_cast<long long>(cells.size());
            st.scc_cells_prev = cells;
            st.scc_cells      = std::move(cells);
            // The receiver inter-signal state census, and its reseed test: a state
            // this station has held before, did not hold last epoch and holds now was
            // cold-started instead of restored by the receiver code-bias carry.  That
            // is the one way card SCC can determine a receiver bias from a single
            // epoch of code (standing invariant #1's re-coupling edge).
            std::set<std::string> isc_now;
            if (st.sm) {
                for (const auto& [key, idx] : st.sm->getCodeBiasStates()) {
                    (void)idx;
                    if (!StateKeys::isReceiverIscKey(key)) continue;
                    isc_now.insert(key);
                    if (!st.scc_isc_prev.count(key) && st.scc_isc_seen.count(key))
                        ++C.isc_reseeds;
                }
            }
            isc_alive += static_cast<long long>(isc_now.size());
            st.scc_isc_seen.insert(isc_now.begin(), isc_now.end());
            st.scc_isc_prev = std::move(isc_now);
        }
        C.isc_states_max = std::max(C.isc_states_max, isc_alive);
        for (const auto& [key, sup] : col_support) {
            (void)key;
            if (sup.first >= 2 && sup.second < 2) ++C.cols_via_companion;
        }
    }

    // --- Second phase channel, per-epoch bookkeeping (card SID phase 6a) --------
    // (docs/refound/SID_signal_identity_2026-09-22.md section 4, R7 + R8.)  Same
    // placement and the same reasons as the block above: serial, in station order,
    // after present[] is final (an absent station's cells close, which is the
    // decoupling edge) and before any later reader of `modeled`; this block only
    // reads it.
    //
    // Four tasks.  (1) Republish each station's second-phase cell set for PASS B: the
    // row-weight seam has the signal and the satellite of every row and no other way
    // to tell a second phase from the latched one, because card SID builds no
    // receiver state to key on.  (2) Count the cells, both cell edges, the arcs born,
    // the live peak, the contributing stations, and the (satellite, signal) columns
    // that reached the two-station registration bar only through second phases (the
    // card's thesis as a number).  (3) Count the fallback occurrences: an
    // allow-listed band-1 phase reported for a satellite whose band has no latched
    // phase this epoch, which R1 does not add, by design.  (4) Advance the
    // between-channel witness (R8), observatory only.
    //
    // Off: the whole block is skipped, every set stays empty and nothing is emitted,
    // so the epoch is byte-identical.
    if (config_.networkSecondPhaseRowsActive()) {
        auto& C = ctx.counters.second_phase;
        const auto& allow = config_.networkSecondPhaseSignals();
        const long long now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        // Per-(satellite, signal): how many present stations support the column at
        // all, and how many of those support it as their latched phase.  The
        // registration bar is two stations, so a column with >= 2 supporters of which
        // < 2 are latched is one that the second-phase rows put on the wire.
        std::map<std::string, std::pair<int, int>> col_support;
        long long live = 0, contributing = 0;
        for (int i = 0; i < n_sta_total; ++i) {
            NetStation& st = stations[i];
            std::set<std::string> cells;
            if (present[i] && i < static_cast<int>(modeled.size())) {
                for (const auto& ob : modeled[static_cast<std::size_t>(i)]) {
                    for (const auto& [sig, val] : ob.observed) {
                        (void)val;
                        if (obsTypeOf(sig) != ObsType::Phase) continue;
                        const std::string key =
                            StateKeys::makeSatPhaseBiasKey(ob.satellite, sig);
                        auto& sup = col_support[key];
                        ++sup.first;
                        if (ob.second_phases.count(sig)) cells.insert(key);
                        else                             ++sup.second;
                    }
                    // (3) Fallback occurrences.  The selection adds nothing when the
                    // band's latched phase is absent this epoch, so the case is visible
                    // only here, against the station's own latch.  `raw_observed` is
                    // the untouched raw key set, which is what the selection screened.
                    const System sys = systemOfSat(ob.satellite);
                    const std::string cons = systemName(sys);
                    // Band scope (card SID phase 6b).  Under the phase-6a knob the
                    // card's own allow-list is band 1 by construction.  Under the
                    // all-channel widening there is no allow-list, and every selected
                    // band has a latched phase and possibly several others, so both the
                    // fallback count and the witness walk every observed band digit (a
                    // band with no latched phase contributes nothing).  The per-band
                    // candidate set is then "every raw phase of this band besides the
                    // latched one", which is what the widening admits, and the two
                    // branches coincide on a band-1-only posture.
                    const bool chan_all = config_.networkChannelModeAll();
                    std::vector<char> bands6b;
                    if (chan_all) {
                        for (const auto& [sig, val] : ob.observed) {
                            (void)val;
                            const auto phase = ObservableId::parse(sys, sig);
                            if (!phase || phase->type != ObsType::Phase) continue;
                            const char digit = phase->signal.digit;
                            if (std::find(bands6b.begin(), bands6b.end(), digit)
                                == bands6b.end())
                                bands6b.push_back(digit);
                        }
                        std::sort(bands6b.begin(), bands6b.end());
                    } else {
                        bands6b.push_back('1');
                    }
                    for (const char bd : bands6b) {
                    // The band's phase as the roster's Modelled role names it, the
                    // latch where the roster has none yet (card NOM step N3b; every
                    // read compared with the latch: census site latch_read_second_phase).
                    const std::string latched =
                        st.model ? signalRosterModelledOr(
                                       ctx.roster,
                                       st.model->latchedCanonicalPhaseOnBandDigit(cons, bd),
                                       i, cons, bd, 'L', &ctx.counters.sid.n3b,
                                       RosterN3bSite::LatchReadSecondPhase)
                                 : std::string{};
                    // The per-band candidate list: the configured allow-list under the
                    // phase-6a knob, and every other raw phase of this band under the
                    // widening (the set the widening itself admits).
                    std::vector<std::string> cand;
                    if (chan_all) {
                        for (const auto& [sig, val] : ob.raw_observed) {
                            (void)val;
                            const auto phase = ObservableId::parse(sys, sig);
                            if (!phase || phase->type != ObsType::Phase
                                || phase->signal.digit != bd) continue;
                            cand.push_back(sig);
                        }
                    } else {
                        cand = allow;
                    }
                    if (!latched.empty() && !ob.raw_observed.count(latched))
                        for (const std::string& sig : cand)
                            if (sig != latched && ob.raw_observed.count(sig))
                                ++C.fallback_rows;
                    // (4) Witness (R8), observatory only: at a station carrying two
                    // phases of one band for a satellite, the epoch-to-epoch change of
                    // (latched minus second) in cycles.  `raw_observed` holds the raw
                    // RINEX values, so a phase is already in cycles and the two share a
                    // frequency; a slip on one channel is an integer here, and a common
                    // slip cancels.  Measured on the raw pair, so it does not depend on
                    // the row having been admitted.
                    if (latched.empty()) continue;
                    const auto lit = ob.raw_observed.find(latched);
                    if (lit == ob.raw_observed.end()) continue;
                    for (const std::string& sig : cand) {
                        if (sig == latched) continue;
                        const auto sit = ob.raw_observed.find(sig);
                        if (sit == ob.raw_observed.end()) continue;
                        const double d = secondPhaseWitness(lit->second, sit->second);
                        if (!std::isfinite(d)) continue;
                        const std::string wkey = ob.satellite + "|" + sig;
                        auto wit = st.spc_witness.find(wkey);
                        if (wit != st.spc_witness.end()
                            && wit->second.second == now_ep - 1) {
                            const double jump = std::abs(d - wit->second.first);
                            ++C.witness_pairs;
                            if (jump > 0.5) ++C.witness_jumps;
                            if (jump > C.witness_max) {
                                C.witness_max    = jump;
                                C.witness_max_id = fmt::format("{}|{}|{}@{:.0f}",
                                                               st.name, ob.satellite,
                                                               sig, se.time.tow);
                            }
                        }
                        st.spc_witness[wkey] = { d, now_ep };
                    }
                    }   // band digit (card SID phase 6b)
                }
            }
            // The two edges (standing invariant #7).  A cell opens when a station
            // starts contributing a second phase for a satellite and signal, and
            // closes when it stops, including when the whole station drops out.
            std::vector<std::string> opened, closed;
            secondPhaseCellEdges(st.spc_cells_prev, cells, &opened, &closed);
            for (const auto& cell : opened) {
                const auto parts = StateKeys::parseSatPhaseBiasKey(cell);
                if (st.spc_cells_seen.insert(cell).second) ++C.arcs_born;
                traces.emitEvent(se.time, "SPC", "second_phase_open",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "open",
                                 parts ? parts->signal : cell);
            }
            for (const auto& cell : closed) {
                const auto parts = StateKeys::parseSatPhaseBiasKey(cell);
                traces.emitEvent(se.time, "SPC", "second_phase_close",
                                 parts ? parts->satellite : std::string{}, st.name,
                                 std::nullopt, std::nullopt, "close",
                                 parts ? parts->signal : cell);
            }
            C.cell_open  += static_cast<long long>(opened.size());
            C.cell_close += static_cast<long long>(closed.size());
            C.cells      += static_cast<long long>(cells.size());
            live         += static_cast<long long>(cells.size());
            if (!cells.empty()) ++contributing;
            st.spc_cells_prev = cells;
            st.spc_cells      = std::move(cells);
        }
        C.arcs_live_max = std::max(C.arcs_live_max, live);
        C.stations_max  = std::max(C.stations_max, contributing);
        for (const auto& [key, sup] : col_support) {
            (void)key;
            if (sup.first >= 2 && sup.second < 2) ++C.cols_via_second;
        }
    }

    // Shared raw slip observables (docs/SPEC_slip_qc_hardening.md §1), computed once
    // per epoch, here: after present[] is final and before both the drains (Card L
    // needs the evidence) and the observability pre-filter (a satellite about to be
    // pruned must still be measured, which is the trace writer's contract).  Nothing
    // between this point and the trace writer mutates `modeled`, so the relocated
    // rows are byte-identical.  Skipped entirely when no consumer asked for them
    // (qc.utils null: every card inert).
    qc.epoch.reset(stations.size());
    if (qc.armed()) {
        // The per-station canonical phase latch (card GRK), bound over the station
        // vector rather than copied into a table, because the latch is locked for
        // the run and lives on each station's own PppModel.  A station whose model
        // is absent returns empty, which the tracker reads as "change nothing".
        //
        // Card NOM step N3b: the lookup reads the roster's Modelled role, which is the
        // latch the roster copies, and the latch answers only where the roster has no
        // role yet (a constellation's first epoch; the fallback the roster read below
        // states).  Every read is compared with the latch (census site
        // latch_read_tracker, 0 on every census arm), so the tracker names each band as
        // before.
        const TrackerLatchLookup latch =
            [&stations, &ctx](int sta, const std::string& system, char band) -> std::string {
                if (sta < 0 || sta >= static_cast<int>(stations.size())) return {};
                const auto& m = stations[static_cast<std::size_t>(sta)].model;
                if (!m) return {};
                return signalRosterModelledOr(ctx.roster,
                                              m->latchedCanonicalPhaseOnBandDigit(system, band),
                                              sta, system, band, 'L', &ctx.counters.sid.n3b,
                                              RosterN3bSite::LatchReadTracker);
            };
        // The tracker's read of the phase-1 roster (card SID phase 2,
        // network.roster_tracker).  Same lookup shape, bound to the roster's
        // modelled_phase instead of the station's latch, so the selection rule below
        // is untouched and only the source of the answer changes.  When armed, this
        // lookup is bound and the latch lookup above is not.
        //
        // One fallback, which is not a bypass.  The roster is built at the top of the
        // epoch body, upstream of computeModels, and the canonical latch is decided
        // inside computeModels, so on the first epoch of a constellation the roster's
        // cells carry no modelled role (card SID section 6 states this as a property
        // of the build site).  Answering "nothing" there would put that epoch's
        // tracker back on the lexical rule while card GRK's lookup names the latch,
        // and the two would then differ at epoch 0 at every station whose priority
        // latch is not the lexically lowest channel.  That epoch matters: it seeds
        // `RawSlipHistory::prev`, so epoch 1's MW/GF step would be formed across a
        // pair change and could fire a detection caused only by the reader.  The
        // lookup therefore falls back to that station's latch exactly where the
        // roster has no answer (the undecided first epoch and nothing else), and both
        // outcomes are counted, so a run reports how much of the work the roster did
        // (standing invariant 4).
        const bool roster_tracker = config_.networkRosterTracker() && ctx.roster.active;
        const TrackerLatchLookup roster_lu =
            [&ctx, &stations](int sta, const std::string& system, char band) -> std::string {
                auto& c = ctx.counters.sid;
                const std::string m = signalRosterModelledPhase(ctx.roster, sta, system, band);
                if (!m.empty()) { ++c.trk_roster_hits; return m; }
                if (sta < 0 || sta >= static_cast<int>(stations.size())) {
                    ++c.trk_roster_none;
                    return {};
                }
                const auto& mdl = stations[static_cast<std::size_t>(sta)].model;
                if (!mdl) { ++c.trk_roster_none; return {}; }
                const std::string l = mdl->latchedCanonicalPhaseOnBandDigit(system, band);
                if (l.empty()) ++c.trk_roster_none;
                else           ++c.trk_roster_fallback;
                return l;
            };
        networkComputeRawObservables(modeled, present, n_sta_total, *qc.utils, qc.epoch,
                                     qc.params.gap_reentry_per_signal,
                                     // The tracker's band signal from the latch (card
                                     // GRK), with the mismatch cell counted in both
                                     // knob states (which is why the counters sink is
                                     // passed unconditionally).  The roster read (card
                                     // SID phase 2) arms the same selection, from the
                                     // roster's own answer.
                                     qc.params.tracker_modeled_first || roster_tracker,
                                     &qc.counters,
                                     roster_tracker ? &roster_lu : &latch);
        // The qualification pass (empirical step gates, LLI credibility, arc
        // resumptions) runs only for a decision card; with only the trace armed the
        // observables are pure output and nothing is adjudicated.
        if (qc.cardsArmed())
            networkQualifyRawObservables(modeled, present, n_sta_total, qc.epoch_idx,
                                         qc.params, qc.hist, qc.epoch, qc.counters);
        // CS-GF authority advance (network.level_authority_csgf, the LV-6 census):
        // one recursion step per dual-band LOS from this epoch's
        // qualified verdicts.  Serial and station-ordered, so deterministic.  The
        // qualify pass's empirical gates (mw_step/gf_step) are the slip reset; lost
        // adjacency (gap / first sighting / pair change through the gate) also
        // resets.  Runs before the drains: the authority is detector-independent
        // evidence, and the harvest reads it post-solve.
        if (qc.params.iono_cs) {
            for (int i = 0; i < n_sta_total; ++i) {
                if (!present[i]
                    || i >= static_cast<int>(qc.epoch.rows.size())) continue;
                const auto& vmap = qc.epoch.verdict[static_cast<std::size_t>(i)];
                for (const auto& row : qc.epoch.rows[static_cast<std::size_t>(i)]) {
                    if (!row.dual || !(row.gf_denom > 0.0)) continue;
                    const auto vit = vmap.find(row.satellite);
                    const bool adjacent =
                        vit != vmap.end() && vit->second.measured;
                    const bool stepped =
                        adjacent && (vit->second.mw_step || vit->second.gf_step);
                    // I = gf_code/(1−γ) = −gf_code/(γ−1); ΔI = d_gf/(γ−1).
                    const double i_code = -row.gf_code_m / row.gf_denom;
                    const double di =
                        adjacent ? vit->second.d_gf / row.gf_denom : 0.0;
                    const double srad = std::sin(row.elev_deg * 0.017453292519943295);
                    const double sigma =
                        kIonoCsCodeSigmaZenith / std::max(srad, 0.15);
                    auto& cs = net::putLos(ctx.entities, i, row.satellite, &net::LosState::iono_cs);
                    networkIonoCsUpdate(cs, i_code, di, adjacent, stepped, sigma);
                    cs.lam1_m = row.lam1_m;   // exact per-pair TECU conversion (LV-6)
                }
            }
        }
    }

    // Datum-station slip detection (network.cycle_slip.datum_station_detect,
    // §9.7.10): under the legacy pivot datum the parallel scan above starts at
    // i_lo == 1, so the datum station's MW detector (built by setup under the same
    // toggle) is rolled here, serially (one station, so determinism is preserved).
    // Only detector-mode candidates are collected, never T0 repairs: the pivot's
    // ambiguities are gauge-pinned, so an integer repair of them is meaningless.
    // The MW drain below routes station-0 candidates, and station-0 LLI assertions,
    // to the network re-reference path instead of a local refloat.
    const bool datum_detect_on = !phase_datum_prioritized
                                 && config_.networkCycleSlipDatumStationDetect()
                                 && config_.networkCycleSlipMwDetect();
    if (datum_detect_on && present.front() && stations.front().mw_slip) {
        try {
            const auto mwr =
                stations.front().mw_slip->detectLargeMwResets(modeled.front());
            // Same admission rule as the parallel scan above (one helper, so the
            // datum station can never drift to a different gate).
            const double frac_max = config_.networkCycleSlipTurboEditFracMax();
            const bool   ev_on    = config_.networkCycleSlipMwEvidenceRefloat();
            const double ev_ratio = config_.networkCycleSlipMwEvidenceRatioMin();
            const int    ev_narc  = config_.networkCycleSlipMwEvidenceMinArcEpochs();
            // The same pair scope as the parallel scan above (card L5P): the datum
            // station must not drift to a different pair set any more than to a
            // different admission gate.
            const bool dm_pairs_primary =
                config_.networkCycleSlipMwDetectPairs() == NetworkMwDetectPairs::Primary;
            std::map<char, NetworkPinBands> dm_pin_by_sys;
            for (const auto& r : mwr) {
                if (r.sig1.empty() || r.sig2.empty()) continue;
                if (dm_pairs_primary && !r.satellite.empty()) {
                    const char cons = letterOfSat(r.satellite);
                    auto pit = dm_pin_by_sys.find(cons);
                    if (pit == dm_pin_by_sys.end())
                        pit = dm_pin_by_sys.emplace(cons, networkPinBandsFor(cons)).first;
                    if (!networkMwPairInPrimaryScope(r.sig1, r.sig2, pit->second)) {
                        ++pa.sta_mw_admit[0].scope_skipped;
                        continue;
                    }
                }
                const auto d = networkMwAdmitCandidate(
                    r.slip_wl_known, r.slip_wl_cycles, r.slip_frac, frac_max,
                    r.evidence_ratio, r.arc_epochs, r.bia_corrected,
                    ev_on, ev_ratio, ev_narc);
                auto& ac = pa.sta_mw_admit[0];
                if (d.admit_legacy) ++ac.legacy_admitted;
                if (d.via_evidence) ++ac.evidence_admitted;
                if (d.would_admit)  ++ac.evidence_would_admit;
                if (d.drop_loose)   ++ac.drop_loose;
                if (d.drop_unknown) {
                    ++ac.drop_unknown;
                    if (ar_suspect_armed) ac.drop_unknown_sats.push_back(r.satellite);
                }
                if (d.admitted)
                    sta_mw_refloat[0].push_back(PassABuffers::MwRefloatCand{
                        r.satellite, r.sig1, r.sig2,
                        r.slip_wl_cycles, r.slip_frac, r.lambda_wl,
                        r.slip_wl_known, r.evidence_ratio, r.arc_epochs,
                        d.via_evidence});
            }
        } catch (...) { /* best-effort: no datum-station MW detection this epoch */ }
    }

    // MW admission census reduce: the per-station slots are filled index-addressed
    // in the parallel region (and, for station 0, by the serial datum scan just
    // above), so the fold is serial and station-ordered, hence deterministic.  Runs
    // unconditionally: these counters are the card's knob-independent telemetry.
    for (int i = 0; i < n_sta_total; ++i) {
        const auto& ac = pa.sta_mw_admit[static_cast<std::size_t>(i)];
        ctx.counters.n_mw_admit_legacy         += ac.legacy_admitted;
        ctx.counters.n_mw_evidence_admitted    += ac.evidence_admitted;
        ctx.counters.n_mw_evidence_would_admit += ac.evidence_would_admit;
        ctx.counters.n_mw_drop_loose           += ac.drop_loose;
        ctx.counters.n_mw_drop_unknown         += ac.drop_unknown;
        ctx.counters.n_mw_pair_scope_skipped   += ac.scope_skipped;   // pair scope (card L5P)
        // The producer AR search's suspect-corner mask (card ARL,
        // filter/NetworkArSuspect.hpp): an MW verdict refused for want of an integer still
        // says the arc's wide lane moved, so its corner is stamped (PASS A runs before the
        // search: this epoch's stamp).
        if (ar_suspect_armed)
            for (const std::string& sat : ac.drop_unknown_sats)
                arSuspectNote(ctx.carry.ar_suspect, i, sat,
                              static_cast<long long>(ctx.counters.n_loop_epochs),
                              kArSuspectMwUnknown, &ctx.counters.netar_suspect);
    }

    // Serial slip-QC drains (INC-6), implemented in slip/NetworkSlipQc.cpp: the
    // phase-leveling T0 repair drain, the MW-detect / LLI refloat and the IF-phase
    // TD driver, in that order.
    networkSlipDrainT0Repairs(pa, config_, stations, present, i_lo, n_sta_total,
                              local_carry, se, log);

    // Per-epoch re-level response state (SPEC_slip_relevel_walk §6): cleared here
    // (the harvest and the PASS-B inject consumed last epoch's sets after this point
    // in the previous epoch), filled by the two drains below, and read by the inject
    // site, the harvest guard and the IF-TD bridge.  nullptr when the knob is off,
    // so the drains' inserts are never reached (byte-identical; the
    // refsat_mark_sats pattern).
    ctx.carry.relevel.clear();
    RelevelEpochState* relevel_ptr =
        config_.networkCycleSlipRelevelFreeEpoch() ? &ctx.carry.relevel : nullptr;

    // Resume slip warm-up (network.resume_slip_warmup_ep,
    // SPEC_synthetic_minimum_network §18.10): for the first K processed epochs after
    // a warm resume the MW/LLI (T0) drain runs diagnostic-only, because its cold
    // reference (two samples at the second warm epoch, no Welford history) declares
    // false integer WL slips and re-levels restored arcs (measured on the §18.9 bed).
    // The T1 IF-TD and wind-up drains below keep their live carry_warm, so a real
    // in-gap slip still gets a response.  Decremented once per processed epoch here
    // (the single dispatch site); the retirement is the decoupling edge of the
    // warm-up window (invariant #7) and is logged.  0 on every continuous epoch, so
    // mw_carry_warm == carry_warm and the output is byte-identical.
    bool mw_carry_warm = carry_warm;
    if (ctx.carry.resume_slip_warmup_left > 0) {
        mw_carry_warm = false;
        ++ctx.counters.n_resume_slip_warmup_epochs;
        if (--ctx.carry.resume_slip_warmup_left == 0)
            log->info("Network (resume slip warm-up): RETIRED at TOW {:.1f} — the MW/LLI "
                      "drain acted diagnostic-only for {} post-resume epoch(s); it is live "
                      "from the next epoch.",
                      se.time.tow, ctx.counters.n_resume_slip_warmup_epochs);
    }
    // Common-band scope (SPEC_event_harness §14): one verdict per (station,
    // satellite) for this epoch, formed here, before either drain, so the MW
    // refloat, the LLI free and the IF-TD pair refloat all follow the same decision
    // and their per-epoch union is exactly `implicated` (Study 3's per-epoch
    // invariant, §Z.2).  Runs armed or not: the census sizes the card before it is
    // armed (invariant #4).  Only the armed run passes a non-null pointer into the
    // drains, so off is byte-identical.
    CommonBandEpoch common_band;
    networkCommonBandVerdictEpoch(pa, config_, stations, present, i_lo,
                                  n_sta_total, se, traces, log,
                                  &ctx.carry.anchor_incumbent, common_band);
    {
        auto& C = ctx.counters;
        C.n_cb_verdicts          += common_band.verdicts;
        C.n_cb_narrowed          += common_band.narrowed_n;
        C.n_cb_all_no_exoneration += common_band.all_no_exoneration;
        C.n_cb_all_inconsistent  += common_band.all_inconsistent;
        C.n_cb_bands_kept        += common_band.bands_kept;
        C.n_cb_iftd_veto         += common_band.iftd_veto;
        for (const auto& [pr, n] : common_band.iftd_veto_by_pair)   // per pair kind, §14.10
            C.n_cb_iftd_veto_by_pair[pr] += n;
        C.n_cb_iftd_ref_arc      += common_band.iftd_ref_arc;    // see §22
        for (const auto& [pr, n] : common_band.iftd_ref_arc_by_pair)
            C.n_cb_iftd_ref_arc_by_pair[pr] += n;
        C.n_cb_immature_pairs    += common_band.immature_pairs;
        C.n_cb_lli_shadow        += common_band.lli_shadow;      // rule F2, §14.11
        C.n_cb_lli_verdicts      += common_band.lli_verdicts;    // rule F3, §14.12
        C.n_cb_lli_narrowed      += common_band.lli_narrowed;
        C.n_cb_lli_anchor_colocate += common_band.lli_anchor_colocate;   // see §14.12.5
        C.n_cb_weak_silence      += common_band.weak_silence;
        C.n_cb_anchor_colocate   += common_band.anchor_colocate;
        C.n_cb_would_narrow      += common_band.would_narrow;
    }
    const CommonBandEpoch* common_band_ptr = common_band.armed ? &common_band : nullptr;

    // Card PRM phase 3a: the datum-station slip re-reference's carry erases, read off the
    // drain's own counter around the call (serial: between the two parallel regions).
    const long long dse0 = qc.counters.datum_slip_carry_erases;
    networkSlipDrainMwRefloat(pa, config_, stations, present, modeled, i_lo,
                              n_sta_total, carry_invariant, mw_carry_warm,
                              carry_slipped_ambs, qc.epoch, qc.params, qc.hist,
                              qc.counters, se, traces, log,
                              datum_detect_on ? &ctx.carry.datum_reref_pb_sats : nullptr,
                              datum_detect_on ? &ctx.carry.datum_reref_amb_sats : nullptr,
                              datum_detect_on ? &ctx.carry.refsat_mark_sats : nullptr,
                              datum_detect_on ? &local_carry : nullptr,
                              relevel_ptr,
                              relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                              relevel_ptr ? &local_carry : nullptr,
                              // Simulation-only oracle slip response: null unless
                              // network.cycle_slip.oracle_relevel armed the run,
                              // so the off path never reaches a hook call
                              // (structurally byte-identical, invariant #6).
                              ctx.carry.slip_oracle.get(),
                              common_band_ptr);
    prmRespond(ctx.counters.prm_resp, ResponseSite::AmbDatumSlipErase,
               qc.counters.datum_slip_carry_erases - dse0);

    // The mark sink is passed only when armed (Edge-1 piece 2), so the off path
    // passes nullptr and the driver's insert is never reached (byte-identical).
    networkSlipIfTdDriver(pa, config_, stations, present, i_lo, n_sta_total,
                          carry_invariant, carry_warm, carry_slipped_ambs,
                          trace_flag_rates, fr_if_slip, se, traces, log,
                          config_.networkMarkRefsatResetDisc()
                              ? &ctx.carry.refsat_mark_sats : nullptr,
                          relevel_ptr,
                          relevel_ptr ? &local_carry : nullptr,
                          relevel_ptr ? &qc.counters : nullptr,
                          relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
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

    // ---- Arc-scoped GF jump trigger (card ARL) ---------------------------------
    // (include/slip/NetworkGfArcJump.hpp.)  The pre-solve witness of the slips the MW
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
                              relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                              common_band_ptr);
    // The producer AR search's suspect-corner mask (card ARL,
    // filter/NetworkArSuspect.hpp): every GF flag stamps its corner, whatever the drain
    // did with it (a cold, pinned or common-band-kept flag still names a doubtful arc).
    if (ar_suspect_armed)
        for (const LosId& los : qc.epoch.gf_jump_flagged)
            arSuspectNote(ctx.carry.ar_suspect, los,
                          static_cast<long long>(ctx.counters.n_loop_epochs),
                          kArSuspectGfJump, &ctx.counters.netar_suspect);

    // ---- Wind-up accumulator-restart response (§10.4) ------------------------
    // (SPEC_phase_windup_sign §10.4.)  Drains the per-station restart ledgers that
    // the model evaluations above just filled and refloats the stepped arcs: the
    // wired invariant-#1 response that any accumulate_phase_windup arm requires.
    // A restart-freed pair enters this epoch's freed set like every other.  No-op unless
    // network.cycle_slip.windup_restart_refloat (disarmed by the loader outside
    // accumulated mode).  It receives the leveling carry for the reason the gap
    // re-entry drain below does: a restart is a return after a gap, so the arc
    // usually has no ambiguity column yet at this point and its carry entry is the
    // target (card WUP).
    networkSlipDrainWindupRestart(config_, stations, present, i_lo, n_sta_total,
                                  carry_invariant, carry_warm,
                                  carry_slipped_ambs, qc.counters, se, traces,
                                  log, relevel_ptr, &ctx.carry.relevel_pend,
                                  ctx.carry.slip_oracle.get(),    // simulation-only oracle
                                  &local_carry, pl_now_s, ctx.knobs.pl_max_gap_s,
                                  // The arc identity from the arc ledger (card NOM step N3b).
                                  ctx.roster.active ? &ctx.arc_ledger : nullptr,
                                  &ctx.counters.sid.n3b);

    // ---- Gap re-entry refloat (card G response) ------------------------------
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
    // Ledger re-entry population (card SID phase 3, network.ledger_reentry): the
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
    // on every run since card NOM step N3a), so the four-way split is comparable
    // across the flip.  A record only: nothing in the solve reads it.
    ArcReentryOutcomeMap* sid_out =
        ctx.roster.active ? &ctx.carry.sid_reentry_outcome : nullptr;
    networkSlipDrainGapReentry(stations, present, i_lo, n_sta_total,
                               carry_invariant, carry_warm, carry_slipped_ambs,
                               qc.epoch, qc.params,
                               local_carry, pl_now_s, ctx.knobs.pl_max_gap_s,
                               qc.counters, se, traces, log,
                               relevel_ptr,
                               relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                               sid.armed ? &sid : nullptr, sid_out,
                               ctx.roster.active ? &ctx.arc_ledger : nullptr);

    // ---- The same response, keyed on the signal (card GRS) ------------------
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
                                        local_carry, pl_now_s, ctx.knobs.pl_max_gap_s,
                                        qc.counters, se, traces, log,
                                        relevel_ptr,
                                        relevel_ptr ? &ctx.carry.relevel_pend : nullptr,
                                        sid.armed ? &sid : nullptr, sid_out);

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
                                     relevel_ptr ? &ctx.carry.relevel_pend : nullptr);
}

void Application::networkAssemblyPrefilter_(NetworkRun& run, const NetworkEpoch& ep,
                                            EpochPassA& pass_a) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    const SlipQcChannel& qc = run.engines.slip_qc;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    const std::vector<bool>& present = ep.pass_a.present;
    int& n_dropped_sats = pass_a.n_dropped_sats;  // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    auto& slip_raw_trace       = traces.slip_raw_trace;

    // Raw cycle-slip observables (network.trace_slip_raw), written whether or not any
    // slip detector is armed.  Written before the observability pre-filter, so a
    // satellite about to be dropped is still captured.  For each present
    // station+satellite the primary dual-frequency pair (the two lowest distinct bands
    // whose phase and matching code "C"+sig[1:] are present) yields MW, GF (=L1−L2) and
    // first-signal phase-minus-code (all in m), plus the per-band RINEX LLI.  Absolute
    // values: the slip signature comes from differencing adjacent epochs offline.
    //
    // The arithmetic lives in slip/NetworkRawObservables.cpp (slip-QC hardening, spec
    // §1): Cards L and G need the same numbers as evidence, and a single definition of
    // "the raw step" keeps the three consumers from diverging.  This site is a pure
    // consumer walking qc.epoch.rows in the same order, so its rows are byte-unchanged
    // when the trace is the only consumer.
    if (slip_raw_trace.is_open() && qc.epoch.computed) {
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& r : qc.epoch.rows[static_cast<std::size_t>(i)]) {
                slip_raw_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << stations[i].name << ',' << r.satellite << ','
                    << r.sig1 << ',' << r.sig2 << ','
                    << fmt::format("{:.2f}", r.elev_deg) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.mw_m) : std::string{}) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.gf_m) : std::string{}) << ','
                    << fmt::format("{:.4f}", r.l1_c1_m) << ',' << r.lli1 << ',' << r.lli2 << '\n';
            }
        }
    }

    // --- Cross-station observability pre-filter -------------------------
    // A satellite clock is well determined for a network product only when at
    // least kMinStationsPerSat stations observe it: a single-station satellite
    // clock is tied to that one receiver clock and, at marginal geometry, becomes
    // the extra near-zero direction that trips the rank gate.  Such satellites are
    // dropped from the network entirely for this epoch, both the shared
    // clock/phase-bias states and the underlying observations.  The observations
    // must go too: a satellite kept in the design with no shared clock column would
    // pin its station's receiver clock absolutely and so remove the clock gauge the
    // datum is meant to pin (declared > observed, and the gate would then reject
    // every epoch).  Dropping the whole satellite keeps one weak satellite from
    // forcing the whole epoch to be withheld; the gate remains the final safety net
    // for a real residual deficiency.
    constexpr int kMinStationsPerSat = 2;
    n_dropped_sats = 0;
    {
        std::map<std::string, int> sat_station_count;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates())
                ++sat_station_count[StateKeys::satelliteFromSatClockKey(kv.first)];
        }
        std::set<std::string> dropped_sats;
        for (const auto& kv : sat_station_count)
            if (kv.second < kMinStationsPerSat) { dropped_sats.insert(kv.first); ++n_dropped_sats; }

        if (!dropped_sats.empty()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                // The cross-station observability gate is a coverage gate too (card
                // SID phase 4a, S1): the same rule as the phase-bias gate one level
                // up, applied to the satellite clock.  It is the one that removes
                // rows, so it opens admitted gaps where the phase-bias gate opens
                // none.  Both share the `gate` bit; the drop-edge counters keep them
                // separable at the readout.
                for (const std::string& ds : dropped_sats)
                    arcReasonExpand(ctx.reasons, ctx.arc_ledger,
                                    static_cast<int>(i), ds, "", kArcReasonGate);
                // Remove the dropped satellites' observations so their rows are
                // not built (and so cannot pin the receiver clock).
                auto& obs = modeled[i];
                obs.erase(std::remove_if(obs.begin(), obs.end(),
                              [&](const PppObservation& o) { return dropped_sats.count(o.satellite) != 0; }),
                          obs.end());
                // Prune the now-orphaned shared states.
                StateManager& sm = *stations[i].sm;
                std::set<std::string> keep_clk, keep_pb;
                for (const auto& kv : sm.getSatClockStates())
                    if (!dropped_sats.count(StateKeys::satelliteFromSatClockKey(kv.first)))
                        keep_clk.insert(kv.first);
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (p && !dropped_sats.count(p->satellite)) keep_pb.insert(kv.first);
                }
                sm.pruneSatClockStatesExcept(keep_clk);
                sm.pruneSatPhaseBiasStatesExcept(keep_pb);
            }
        }
    }
}

EpochAction Application::networkAssemblyRegisterColumns_(NetworkRun& run, const NetworkEpoch& ep,
                                                         EpochColumns& columns) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const std::vector<bool>& present = ep.pass_a.present;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    std::vector<std::string>& registered_clock_sats = columns.registered_clock_sats;  // written
    std::vector<std::pair<std::string, std::string>>& registered_code_keys = columns.registered_code_keys;  // written
    std::vector<std::string>& col_desc = columns.col_desc;  // written
    int& n_clock = columns.n_clock;  // written
    int& n_pb = columns.n_pb;  // written
    int& n_cb = columns.n_cb;  // written
    int& n_present = columns.n_present;  // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    const auto& nc = config_.getNetworkConfig();
    auto& stations = setup.stations;
    auto& sh_model = setup.sh_model;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& last_n_present = ctx.counters.last_n_present;
    auto& last_n_clock_sats = ctx.counters.last_n_clock_sats;
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };

    // --- Register the shared column layout (reset the hoisted estimator) ---
    est.reset();
    std::set<std::string> clk_seen;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& kv : stations[i].sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            est.registerClock(sat);
            if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
        }
    }
    // CB-ADM per-epoch registration decision (on only): an admitted code pair is
    // registered only when >= 2 present stations hold its state this epoch (the
    // per-epoch guard).  A single-station code column is confounded with that
    // receiver's clock and goes near-null; registering on window-latched admission
    // alone (at >= 1 station) brings those columns back (22 -> 43 near-null
    // directions on the 305-station arm).  The anti-flap property is kept because
    // the admitted set stays latched (census): only the per-epoch registration
    // follows coverage, and the SM state is never pruned, so a withheld column
    // re-registers as soon as >= 2 stations return.  cbadm_reg_code_keys holds the
    // (sat,signal) code columns registered this epoch.
    //
    // The phase co-gate is not applied here (Card H1).  It sits upstream, at
    // phase-bias state allocation in MeasurementModel::ensureNetworkSharedStates,
    // and it keys off the run-latched census-admitted set, not off
    // cbadm_reg_code_keys, the narrower per-epoch registered subset.  Two separate
    // reasons, both load-bearing:
    //  - Level: withholding a registerPhaseBias for a key still present in
    //    shared_phase_keys breaks the "every shared_phase_key is a registered
    //    column" invariant that eq34 / carry / snapshot rely on (a registration-site
    //    co-gate crashes the on path).  Allocating no phase-bias state at all keeps
    //    shared_phase_keys consistent everywhere by construction.
    //  - Granularity (Card H2, a deliberate ruling): the co-gate stays at census
    //    granularity.  Following per-epoch code coverage would churn phase-state
    //    allocation every epoch, which the anti-flap design exists to prevent, and
    //    thrash eq34/carry/snapshot.  The accepted residue is a transient orphan
    //    window, counted by ctx.counters.n_cbadm_phase_orphan_key_epochs below.
    //
    // Why a sparse signal must leave the shared system on both bias families: that
    // phase column has no same-signal code co-anchor and, with two stations, no
    // reachable ambiguity pin, so it collapses to a pure-PB near-null gauge (89 such
    // directions on the 305-station arm, the phase counterparts L2X/L7X/L1I of the
    // quarantined code pairs C2X/C7X/C1I).  Withholding it folds the bias into each
    // station's float ambiguity, symmetric with the code sponge.  A per-satellite
    // co-gate is too coarse (a satellite admitted on its dense signals would keep
    // its sparse signals' phase columns); the granularity must match the code
    // admission unit, which is per-(sat,signal).
    std::set<std::string> cbadm_reg_code_keys;   // raw "sat|sig" code keys registered this epoch (admitted and >=2 present stations)
    const bool cbadm_on =
        config_.estimateNetworkSatCodeBias() && ctx.admission.active;
    if (cbadm_on) {
        std::map<std::string, int> code_epoch_count;   // admitted "sat|sig" -> #present stations this epoch
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatBiasStates())
                if (ctx.admission.isAdmitted(kv.first)) ++code_epoch_count[kv.first];
        }
        for (const auto& [key, cnt] : code_epoch_count)
            if (cnt >= 2) cbadm_reg_code_keys.insert(key);
    }
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Register every shared_phase_key unconditionally: the CB-ADM phase
        // co-gate sits upstream at phase-bias state allocation
        // (MeasurementModel::ensureNetworkSharedStates, cbadm_admitted_code_keys),
        // so shared_phase_keys already excludes a quarantined signal's phase.
        // Gating here instead would break the "every shared_phase_key is a
        // registered column" invariant that eq34/carry/snapshot rely on (crash).
        est.registerPhaseBias(parts->satellite, parts->signal);
        // Measure the granularity seam's orphan window (Card H2).  This loop is the
        // only place that sees all three sets at once: the census-admitted set
        // (ctx.admission), this epoch's registered code keys (cbadm_reg_code_keys)
        // and the shared phase set (shared_phase_keys).  An admitted code key that
        // fell below 2 present stations this epoch has its code column withheld
        // while its same-signal phase column stays shared: one (key, epoch) pair of
        // orphan window.  Expected 0 at 305-station density (every admitted
        // satellite has >= 2 present stations there); a sparse-tier run showing it
        // nonzero and coincident with withholds is the data that would reopen the
        // ruling.
        if (cbadm_on) {
            // Key-format asymmetry: phase-bias keys carry the phase observable
            // ("C26|L2X|PB"), while the admission census and cbadm_reg_code_keys
            // carry the code one ("C26|C2X").  The companion mapping is
            // L<band><attr> -> C<band><attr>, the same transform the state-alloc
            // co-gate applies (MeasurementModel::ensureNetworkSharedStates).
            // Reusing the phase signal unchanged would match nothing and leave this
            // counter permanently 0, inert wiring that reads as "no orphans"
            // (standing invariant #4).
            const auto phase =
                SignalId::fromObsCode(systemOfSat(parts->satellite), parts->signal);
            if (phase) {
                const std::string code_key =
                    StateKeys::makeSatBiasKey(parts->satellite, phase->obsCode(ObsType::Code));
                if (ctx.admission.isAdmitted(code_key) && !cbadm_reg_code_keys.count(code_key))
                    ++ctx.counters.n_cbadm_phase_orphan_key_epochs;
            }
        }
    }
    // Register the satellite code-bias (OSB) columns (W6, full multi-band): every
    // (sat,signal), i.e. every frequency band and every tracking channel
    // (attribute), for which at least two present stations carry an OSB state.
    // Collected (deduplicated, first-seen order) for the code-bias datum
    // constraints, the D2 carry-forward and the solution trace.  Empty unless
    // estimate_sat_code_bias.
    //
    // The default admission gate (CB-ADM off) is per-epoch >=2-station coverage: a
    // single-station code bias is fully confounded with that station's receiver
    // code bias and cannot be separated, so it stays a local nuisance (its sat-bias
    // state is pruned below, and its raw code row folds into the receiver clock /
    // slant iono, as without W6).  Everything with >=2 stations is a first-class
    // shared OSB column and is datumed below: the GIM between-satellite iono
    // structure makes the inter-band DCBs observable, while same-band channels
    // (C1C vs C1W, C2W vs C2L, ...) need no iono at all (at equal frequency the
    // receiver and ionosphere terms cancel in the raw between-channel difference,
    // leaving the satellite inter-channel bias).  The datum pins every registered
    // column, including those on a third band.
    //
    // Under CB-ADM (ctx.admission.active, network.cb_admission on), registration
    // follows the run-latched census verdict and the per-epoch coverage: an
    // admitted pair registers when >= 2 present stations hold its state this epoch
    // (cbadm_reg_code_keys above), and its state is kept through coverage dips, so
    // it re-registers when coverage returns (the anti-flap property).  A
    // quarantined pair is never pruned (station_keep always holds it), so it
    // survives as a station-local sponge (Schur-eliminated station-side, never
    // registered, never datumed).  See filter/NetworkBiasAdmission.hpp.
    if (config_.estimateNetworkSatCodeBias()) {
        std::set<std::string> cb_seen;
        NetworkBiasAdmission& adm = ctx.admission;
        if (!adm.active) {
            // --- CB-ADM off: legacy per-epoch >=2-station gate (byte-identical) ---
            std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatBiasStates())
                    ++cb_station_count[kv.first];
            }
            std::set<std::string> keep_keys;
            for (const auto& [key, cnt] : cb_station_count)
                if (cnt >= 2) keep_keys.insert(key);

            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    if (!keep_keys.count(kv.first)) continue;
                    station_keep.insert(kv.first);
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;
                    est.registerCodeBias(parts->satellite, parts->signal);
                    if (cb_seen.insert(kv.first).second)
                        registered_code_keys.emplace_back(parts->satellite, parts->signal);
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        } else {
            // --- CB-ADM on: run-latched admission, per-epoch >=2 registration and
            // station-local quarantine.  cbadm_reg_code_keys (computed above the
            // phase loop) is the admitted set restricted to keys with >= 2 present
            // stations this epoch; only those register a shared column.  Every SM
            // state is kept (station_keep holds it unconditionally: quarantined and
            // per-epoch-withheld admitted states are never pruned), so a withheld
            // column takes the scatter's codeBiasColumn-throw local fallback this
            // epoch and re-registers when coverage returns.
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    station_keep.insert(kv.first);   // never pruned while the flag is on
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;            // malformed key: kept (harmless), never registered
                    if (cbadm_reg_code_keys.count(kv.first)) {
                        est.registerCodeBias(parts->satellite, parts->signal);
                        if (cb_seen.insert(kv.first).second)
                            registered_code_keys.emplace_back(parts->satellite, parts->signal);
                    } else if (!adm.isAdmitted(kv.first)
                               && !adm.seen.count(kv.first)
                               && adm.late_tracked.insert(kv.first).second) {
                        // A key the census never classified was first seen after the
                        // window and is implicitly quarantined (invariant #4
                        // telemetry).  An admitted key that is only below the
                        // per-epoch coverage this epoch is not late: it is kept and
                        // re-registers later.
                        ++ctx.counters.n_cb_late_quarantined;
                    }
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        }
    }

    // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
    // layout is fixed (all (N+1)² coefficients of every shell, every epoch,
    // independent of the satellite set), so the field is continuous across epochs
    // and the Kaula prior can keep every coefficient soft-pinned (the rank gate then
    // stays at #constellations regardless of coverage; design §6.1).  Dual layer
    // (Iono-2): the second shell registers under its own "SH2" keys after the
    // legacy layer, preserving the layer-0 column order.
    if (config_.estimateNetworkIonoSh() && sh_model) {
        for (const ShLayer& lay :
             shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                      setup.sh_model2.get()))
            for (const auto& c : lay.model->coeffs())
                est.registerVtecCoeff(c.n, c.m, c.trig, lay.layer);
    }

    // Card SCL (network.carry_coupling_clock_clone): the one-epoch clock clones, after every
    // other registration so that no existing column index moves (the clones are the last
    // columns), and before the label map and beginEpoch.  No-op off.
    networkClockCloneRegister(ctx, est, registered_clock_sats);
    // Card SCL2 X2 (network.carry_coupling_shared_history): the shared-history columns, after
    // the clones (filter/NetworkSharedHistory).  No-op off.
    networkSharedHistoryRegister(ctx, est);

    n_clock = static_cast<int>(registered_clock_sats.size());
    n_pb    = static_cast<int>(shared_phase_keys.size());
    n_cb    = static_cast<int>(registered_code_keys.size());
    n_present = static_cast<int>(std::count(present.begin(), present.end(), true));
    last_n_present    = n_present;   // most-recent station-presence count for the health heartbeat (RTB-6)
    last_n_clock_sats = n_clock;     // most-recent registered-clock-sat count for the health heartbeat (RTB-6, P4b)

    // Column -> human descriptor map (W6 estimability diagnostic): lets the one-shot
    // spectrum dump name the shared parameters that span the near-zero
    // eigen-directions (unpinned gauges).  Populated only when the estimability
    // trace (off by default) is armed; it feeds nothing but that dump, so a
    // production run skips the string-building entirely.
    col_desc.resize(static_cast<size_t>(est.numShared()));
    if (spectrum_diag_budget > 0 && nc.trace_estimability) {
        for (const std::string& sat : registered_clock_sats)
            col_desc[static_cast<size_t>(est.clockColumn(sat))] = "CLK|" + sat;
        for (const auto& pk : shared_phase_keys) {
            const auto p = StateKeys::parseSatPhaseBiasKey(pk);
            if (p) col_desc[static_cast<size_t>(est.phaseBiasColumn(p->satellite, p->signal))]
                       = "PB|" + p->satellite + "|" + p->signal;
        }
        for (const auto& kv : registered_code_keys)
            col_desc[static_cast<size_t>(est.codeBiasColumn(kv.first, kv.second))]
                = "CB|" + kv.first + "|" + kv.second;
        // Name the SH-VTEC columns (required, design §6.4): otherwise a missing SH
        // datum prints as an anonymous colNNN in the spectrum dump,
        // indistinguishable from a clock-datum fault.
        if (config_.estimateNetworkIonoSh() && sh_model) {
            for (const ShLayer& lay :
                 shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                          setup.sh_model2.get()))
                for (const auto& c : lay.model->coeffs())
                    col_desc[static_cast<size_t>(
                        est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer))] =
                        fmt::format("{}|{}|{}|{}", lay.layer > 0 ? "SH2" : "SH",
                                    c.n, c.m, c.trig);
        }
        for (const auto& [sat, col] : ctx.carry.ccl.col)   // card SCL clones; empty off
            col_desc[static_cast<size_t>(col)] = "CLKC|" + sat;
        for (std::size_t j = 0; j < ctx.carry.shx.col.size(); ++j)   // card SCL2 X2 history; empty off
            if (ctx.carry.shx.col[j] >= 0)
                col_desc[static_cast<size_t>(ctx.carry.shx.col[j])] = "HIST|" + std::to_string(j);
    }

    if (est.numShared() == 0) {
        ++n_skipped;
        if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                         << ',' << n_present << ",0,0,0,0,0,0,0,0\n";
        { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
          emitEpochStatus("sync_skip", es); }
        prev_week = se.time.week; prev_tow = se.time.tow;
        return EpochAction::SkipEpoch;
    }

    est.beginEpoch();
    return EpochAction::Proceed;
}

EpochAction Application::networkPassB_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                       EpochAssembly& assembly) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    int n_clock = ep.columns.n_clock;
    int n_pb = ep.columns.n_pb;
    int n_present = ep.columns.n_present;
    std::vector<StationAssembly>& asm_res = assembly.asm_res;  // written
    std::vector<int>& est_index_of = assembly.est_index_of;  // written
    int& total_pins = assembly.total_pins;  // written
    double& sec_asm_ms_epoch = assembly.sec_asm_ms_epoch;  // written
    double& sec_asm_wall_ms = run.timing.asm_wall_ms;
    // ---- End of the bindings ----
    const bool sec_timers_on = ctx.knobs.sec_timers_on;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };

    // --- PASS B: per-station assembly, then serial scatter -------------
    // Split into a parallel produce phase (pure per-station compute into
    // pre-sized slots, no shared write) and a serial consume phase that scatters
    // each block into the shared arrowhead in deterministic station order.
    // computeMeasurementUpdate, splitStation and the pivot pin fold touch only
    // station i's own state and const queries on `est` (its column layout is
    // frozen after registration; addStation is the only mutation and runs
    // serially below), so the produced blocks and their add order are identical
    // for any thread count.
    //
    // The assembly wall-clock timer (D0) brackets the parallel produce
    // (per-station row build) and the serial scatter (est.addStation): the
    // "assembly" section.
    std::chrono::steady_clock::time_point sec_asm_t0;
    if (sec_timers_on) sec_asm_t0 = std::chrono::steady_clock::now();

    // The serial stages before the parallel produce (card NOM3 step PB1a).
    networkShDevCarryAge(run, ep);
    networkCouplingReinitHistory(run, ep);

    // The parallel produce: one station's block per OpenMP iteration, the station steps of
    // kStationBuildSteps (filter/NetworkStationBuild.hpp, card NOM3 step PB1b).
    networkAssemblyProduce(run, ep, gim_, assembly);

    // The serial stages after the parallel produce (card NOM3 step PB1a).
    networkAssemblyTally(run, ep, assembly);
    networkPrefitFuseEpoch(run, ep, assembly);
    const bool epoch_ok = networkAssemblyConsume(run, ep, pass_a, assembly);
    networkSolveStationsTrack(run, ep, assembly, epoch_ok);
    networkEq34PinAudit(run, ep, assembly, epoch_ok);

    // Close the assembly wall-clock timer (D0; produce + scatter).  Accumulated even
    // when the epoch skips below, because the assembly work was still spent.
    if (sec_timers_on) {
        sec_asm_ms_epoch = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sec_asm_t0).count();
        sec_asm_wall_ms += sec_asm_ms_epoch;
    }

    // Card NOM N2a: the station side of "contributions as armed" (the rows and diagonal
    // entries of the stations the estimator took, per kind), and every assembled station's
    // tags checked against its rows.  Serial, trace class; the epoch tail adds the shared
    // folds and writes the table.
    networkContributionsStationDrain(ctx.contributions, asm_res, est_index_of,
                                     se.time.week, se.time.tow);

    if (!epoch_ok || est.numShared() == 0) {
        ++n_skipped;
        if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                         << ',' << n_present << ',' << n_clock << ',' << n_pb
                         << ',' << 0 << ",0,0,0,0," << total_pins << '\n';
        { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
          emitEpochStatus("sync_skip", es); }
        prev_week = se.time.week; prev_tow = se.time.tow;
        return EpochAction::SkipEpoch;
    }

    return EpochAction::Proceed;
}

} // namespace GnssProcessor
