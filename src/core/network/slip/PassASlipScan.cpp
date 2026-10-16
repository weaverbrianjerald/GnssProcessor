/**
 * @file PassASlipScan.cpp
 * @brief The PASS A slip scan stages declared in PassASlipScan.hpp.
 *
 * The bodies were cut verbatim from `Application::networkPassASlipScan_`; only the
 * bindings at the head of each stage and the calls between stages are new.
 */

#include "network/slip/PassASlipScan.hpp"

#include "network/slip/SlipQc.hpp"            // PassABuffers, the MW admission and the drain seams
#include "network/slip/RawObservables.hpp"    // the raw slip observables and the CS-GF recursion
#include "network/slip/CommonBandScope.hpp"   // CommonBandEpoch, CommonBandProcessed
#include "network/assembly/SignalRoster.hpp"  // the roster's modelled role reads
#include "network/assembly/ExtraBandScope.hpp"   // the pin bands and the primary-pair predicate
#include "network/assembly/SecondCodeScope.hpp"  // secondCodeCellEdges
#include "network/assembly/SecondPhaseScope.hpp" // secondPhaseCellEdges, secondPhaseWitness
#include "network/assembly/Responses.hpp"     // prmRespond
#include "network/state/ArcLedger.hpp"        // arcReasonExpand, the re-entry population
#include "network/state/Entities.hpp"         // net::putLos and the entity tables
#include "network/state/RunContext.hpp"       // the run context and its counters
#include "network/ar/ArSuspect.hpp"           // arSuspectNote
#include "network/diag/RunTraces.hpp"         // NetworkRunTraces::emitEvent
#include "filter/StateKeySchema.hpp"          // StateKeys
#include "slip/MwSlipDetector.hpp"            // the station MW detector
#include "slip/TurboEditResolver.hpp"         // TurboEdit integer repair
#include "slip/IfPhaseSlipDetector.hpp"       // IF-phase triple-difference detection
#include "config/GnssConfig.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"            // systemOfSat, letterOfSat, ObservableId, obsTypeOf
#include "rinex/MultiRinexSync.hpp"           // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void passAScanStations(const PassAView& v) {
    const SyncedEpoch& se = v.se;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    const std::set<std::string>& shared_phase_keys = v.shared_phase_keys;
    const int n_sta_total = v.n_sta_total;
    const bool parallel_on = v.parallel_on;
    const int n_threads = v.n_threads;
    const bool phase_datum_prioritized = v.phase_datum_prioritized;
    const std::set<std::string>* cbadm_admit = v.cbadm_admit;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;
    const bool sid_screens = v.sid_screens;
    std::vector<std::map<std::string, DropReason>>& pa_drops = v.pa_drops;
    auto& a_ok = pa.a_ok;
    auto& a_err = pa.a_err;
    auto& sta_if_resets = pa.sta_if_resets;

    // PASS A tail (stations[i_lo..]: from 1 on the legacy datum, from 0 when
    // prioritized), parallel over stations when enabled.  computeModels and
    // ensureNetworkSharedStates touch only station i's own sm/model/mm/pstate;
    // shared_phase_keys is read-only here and modeled[i] is a distinct slot per i,
    // so there is no shared write.  Each iteration catches its own exceptions,
    // because an exception must not escape an OpenMP structured block (UB).
    // present[] is a std::vector<bool> (packed bits, so concurrent writes to
    // adjacent indices race), so threads record into a per-index char buffer that
    // is folded into present[] serially afterwards.
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
            passAScanMw(v, i, st);

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
}

void passAScanMw(const PassAView& v, int i, NetStation& st) {
    const GnssConfig& config_ = v.config_;
    double dt = v.dt;
    double pl_now_s = v.pl_now_s;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    PassABuffers& pa = v.pa;
    auto& sta_repairs = pa.sta_repairs;

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
                passAMwDetectOnly(v, i, st, mwr);
            }
        } catch (...) { /* best-effort: no MW action this epoch */ }
    }
}

void passAMwDetectOnly(const PassAView& v, int i, NetStation& st,
                       const std::vector<MwSlipResult>& mwr) {
    const GnssConfig& config_ = v.config_;

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
    // Pair scope.  Under 'primary' the network forms
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
    passAMwAdmit(v, i, mwr, mw_pair_ok, frac_max, ev_on, ev_ratio, ev_narc);
    passAMwEvidence(v, i, st, mwr, mw_pair_ok);
}

void passAMwAdmit(const PassAView& v, int i, const std::vector<MwSlipResult>& mwr,
                  const PassAMwPairOk& mw_pair_ok, const double frac_max, const bool ev_on,
                  const double ev_ratio, const int ev_narc) {
    const bool ar_suspect_armed = v.ar_suspect_armed;
    PassABuffers& pa = v.pa;
    auto& sta_mw_refloat = pa.sta_mw_refloat;

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
}

void passAMwEvidence(const PassAView& v, int i, NetStation& st,
                     const std::vector<MwSlipResult>& mwr, const PassAMwPairOk& mw_pair_ok) {
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    PassABuffers& pa = v.pa;

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
    // The pair scope applies here as well: a pair the
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

void passAFoldPresent(const PassAView& v) {
    auto log = v.log;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    const int n_sta_total = v.n_sta_total;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;
    auto& a_ok = pa.a_ok;
    auto& a_err = pa.a_err;

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
}

void passAFoldDropReasons(const PassAView& v) {
    NetworkRunContext& ctx = v.ctx;
    const int n_sta_total = v.n_sta_total;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;
    const bool sid_screens = v.sid_screens;
    std::vector<std::map<std::string, DropReason>>& pa_drops = v.pa_drops;
    auto& a_ok = pa.a_ok;

    // Fold the drop reasons serially, in station order.  The
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
}

void passASecondCode(const PassAView& v) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    const int n_sta_total = v.n_sta_total;

    // --- Second code channel, per-epoch bookkeeping, R7 --------------
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
    // two-station registration bar only through companions (the mechanism's thesis as a
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
            // is the one way the second code channel can determine a receiver bias from a single
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
}

void passASecondPhase(const PassAView& v) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    const int n_sta_total = v.n_sta_total;

    // --- Second phase channel, per-epoch bookkeeping --------
    // (docs/refound/SID_signal_identity_2026-09-22.md section 4, R7 + R8.)  Same
    // placement and the same reasons as the block above: serial, in station order,
    // after present[] is final (an absent station's cells close, which is the
    // decoupling edge) and before any later reader of `modeled`; this block only
    // reads it.
    //
    // Four tasks.  (1) Republish each station's second-phase cell set for PASS B: the
    // row-weight seam has the signal and the satellite of every row and no other way
    // to tell a second phase from the latched one, because the signal-identity layer builds no
    // receiver state to key on.  (2) Count the cells, both cell edges, the arcs born,
    // the live peak, the contributing stations, and the (satellite, signal) columns
    // that reached the two-station registration bar only through second phases (the
    // mechanism's thesis as a number).  (3) Count the fallback occurrences: an
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
                    passASecondPhaseWitness(v, i, st, ob, C, allow, now_ep);
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
}

void passASecondPhaseWitness(const PassAView& v, int i, NetStation& st, const PppObservation& ob,
                             NetworkServiceCounters::SecondPhase& C,
                             const std::vector<std::string>& allow, const long long now_ep) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config_;

    // (3) Fallback occurrences.  The selection adds nothing when the
    // band's latched phase is absent this epoch, so the case is visible
    // only here, against the station's own latch.  `raw_observed` is
    // the untouched raw key set, which is what the selection screened.
    const System sys = systemOfSat(ob.satellite);
    const std::string cons = systemName(sys);
    // Band scope.  Under the phase-6a knob the
    // allow-list is band 1 by construction.  Under the
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
    // latch where the roster has none yet (every
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
    }   // band digit
}

void passARawObservables(const PassAView& v) {
    NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config_;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;

    // Shared raw slip observables (docs/SPEC_slip_qc_hardening.md §1), computed once
    // per epoch, here: after present[] is final and before both the drains (the LLI corroboration
    // needs the evidence) and the observability pre-filter (a satellite about to be
    // pruned must still be measured, which is the trace writer's contract).  Nothing
    // between this point and the trace writer mutates `modeled`, so the relocated
    // rows are byte-identical.  Skipped entirely when no consumer asked for them
    // (qc.utils null: every consumer inert).
    qc.epoch.reset(stations.size());
    if (qc.armed()) {
        // The per-station canonical phase latch, bound over the station
        // vector rather than copied into a table, because the latch is locked for
        // the run and lives on each station's own PppModel.  A station whose model
        // is absent returns empty, which the tracker reads as "change nothing".
        //
        // The lookup reads the roster's Modelled role, which is the
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
        // The tracker's read of the phase-1 roster (key
        // network.roster_tracker).  Same lookup shape, bound to the roster's
        // modelled_phase instead of the station's latch, so the selection rule below
        // is untouched and only the source of the answer changes.  When armed, this
        // lookup is bound and the latch lookup above is not.
        //
        // One fallback, which is not a bypass.  The roster is built at the top of the
        // epoch body, upstream of computeModels, and the canonical latch is decided
        // inside computeModels, so on the first epoch of a constellation the roster's
        // cells carry no modelled role (the signal-identity design states this as a property
        // of the build site).  Answering "nothing" there would put that epoch's
        // tracker back on the lexical rule while the modeled-first lookup names the latch,
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
                                     // The tracker's band signal from the latch,
                                     // with the mismatch cell counted in both
                                     // knob states (which is why the counters sink is
                                     // passed unconditionally).  The roster read
                                     // arms the same selection, from the
                                     // roster's own answer.
                                     qc.params.tracker_modeled_first || roster_tracker,
                                     &qc.counters,
                                     roster_tracker ? &roster_lu : &latch);
        // The qualification pass (empirical step gates, LLI credibility, arc
        // resumptions) runs only for a decision mechanism; with only the trace armed the
        // observables are pure output and nothing is adjudicated.
        if (qc.cardsArmed())
            networkQualifyRawObservables(modeled, present, n_sta_total, qc.epoch_idx,
                                         qc.params, qc.hist, qc.epoch, qc.counters);
        passAIonoCsAdvance(v);
    }
}

void passAIonoCsAdvance(const PassAView& v) {
    NetworkRunContext& ctx = v.ctx;
    std::vector<bool>& present = v.present;
    SlipQcChannel& qc = v.qc;
    const int n_sta_total = v.n_sta_total;

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

void passADatumStationMwDetect(const PassAView& v, const bool datum_detect_on) {
    const GnssConfig& config_ = v.config_;
    auto& stations = v.stations;
    std::vector<std::vector<PppObservation>>& modeled = v.modeled;
    std::vector<bool>& present = v.present;
    const bool ar_suspect_armed = v.ar_suspect_armed;
    PassABuffers& pa = v.pa;
    auto& sta_mw_refloat = pa.sta_mw_refloat;

    // Datum-station slip detection (network.cycle_slip.datum_station_detect,
    // §9.7.10): under the legacy pivot datum the parallel scan above starts at
    // i_lo == 1, so the datum station's MW detector (built by setup under the same
    // toggle) is rolled here, serially (one station, so determinism is preserved).
    // Only detector-mode candidates are collected, never T0 repairs: the pivot's
    // ambiguities are gauge-pinned, so an integer repair of them is meaningless.
    // The MW drain below routes station-0 candidates, and station-0 LLI assertions,
    // to the network re-reference path instead of a local refloat.
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
            // The same pair scope as the parallel scan above: the datum
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
}

void passAFoldMwAdmission(const PassAView& v) {
    NetworkRunContext& ctx = v.ctx;
    const int n_sta_total = v.n_sta_total;
    const bool ar_suspect_armed = v.ar_suspect_armed;
    PassABuffers& pa = v.pa;

    // MW admission census reduce: the per-station slots are filled index-addressed
    // in the parallel region (and, for station 0, by the serial datum scan just
    // above), so the fold is serial and station-ordered, hence deterministic.  Runs
    // unconditionally: these counters are the mechanism's knob-independent telemetry.
    for (int i = 0; i < n_sta_total; ++i) {
        const auto& ac = pa.sta_mw_admit[static_cast<std::size_t>(i)];
        ctx.counters.n_mw_admit_legacy         += ac.legacy_admitted;
        ctx.counters.n_mw_evidence_admitted    += ac.evidence_admitted;
        ctx.counters.n_mw_evidence_would_admit += ac.evidence_would_admit;
        ctx.counters.n_mw_drop_loose           += ac.drop_loose;
        ctx.counters.n_mw_drop_unknown         += ac.drop_unknown;
        ctx.counters.n_mw_pair_scope_skipped   += ac.scope_skipped;   // pair scope
        // The producer AR search's suspect-corner mask (
        // network/ar/ArSuspect.hpp): an MW verdict refused for want of an integer still
        // says the arc's wide lane moved, so its corner is stamped (PASS A runs before the
        // search: this epoch's stamp).
        if (ar_suspect_armed)
            for (const std::string& sat : ac.drop_unknown_sats)
                arSuspectNote(ctx.carry.ar_suspect, i, sat,
                              static_cast<long long>(ctx.counters.n_loop_epochs),
                              kArSuspectMwUnknown, &ctx.counters.netar_suspect);
    }
}

void passAResumeSlipWarmup(const PassAView& v, bool& mw_carry_warm) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    auto log = v.log;

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
    if (ctx.carry.resume_slip_warmup_left > 0) {
        mw_carry_warm = false;
        ++ctx.counters.n_resume_slip_warmup_epochs;
        if (--ctx.carry.resume_slip_warmup_left == 0)
            log->info("Network (resume slip warm-up): RETIRED at TOW {:.1f} — the MW/LLI "
                      "drain acted diagnostic-only for {} post-resume epoch(s); it is live "
                      "from the next epoch.",
                      se.time.tow, ctx.counters.n_resume_slip_warmup_epochs);
    }
}

void passACommonBandVerdict(const PassAView& v, CommonBandEpoch& common_band) {
    const SyncedEpoch& se = v.se;
    NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const GnssConfig& config_ = v.config_;
    auto log = v.log;
    auto& stations = v.stations;
    std::vector<bool>& present = v.present;
    const int n_sta_total = v.n_sta_total;
    const int i_lo = v.i_lo;
    PassABuffers& pa = v.pa;

    // Common-band scope (SPEC_event_harness §14): one verdict per (station,
    // satellite) for this epoch, formed here, before either drain, so the MW
    // refloat, the LLI free and the IF-TD pair refloat all follow the same decision
    // and their per-epoch union is exactly `implicated` (Study 3's per-epoch
    // invariant, §Z.2).  Runs armed or not: the census sizes the mechanism before it is
    // armed (invariant #4).  Only the armed run passes a non-null pointer into the
    // drains, so off is byte-identical.
    // The incumbent anchors as the "sat_signal"-keyed map the scope's prefix scan reads
    // (key order is id order, O1).
    const std::map<std::string, std::string> anchor_view =
        net::sigMap(ctx.entities.sat_signals, &net::SatSignalState::anchor_incumbent, '_');
    networkCommonBandVerdictEpoch(pa, config_, stations, present, i_lo,
                                  n_sta_total, se, traces, log,
                                  &anchor_view, common_band);
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
}

} // namespace GnssProcessor
