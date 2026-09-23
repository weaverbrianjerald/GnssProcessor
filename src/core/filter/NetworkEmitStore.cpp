/**
 * @file NetworkEmitStore.cpp
 * @brief Implementation of @ref networkEmitStore — the SB_EMITSTORE emit/store/
 *        product-write + closed-loop block relocated VERBATIM (dedent-only, zero
 *        substitution) out of Application::runNetwork at decomposition INC-11
 *        (2026-07-22).
 *
 * Body = byte-copy of the former SB_EMITSTORE block (ApplicationNetwork.cpp lines
 * 5045-5522), dedented 4 spaces.  Alias preamble = mechanically-derived run-TU
 * aliases + 5 reconstructions (cl / shc / cl_truth / cl_truth_llh / kClRecentWindow).
 * Carries the dormant Iono-2 dual-layer edits verbatim.  See NetworkEmitStore.hpp
 * for the ordering contract.
 */

#include "filter/NetworkEmitStore.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "filter/NetworkClockLevel.hpp"        // card CLV: networkClockLevelDeclare
#include "filter/NetworkLedgerEmit.hpp"
#include "filter/NetworkPbHold.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "math/Coordinates.hpp"
#include "products/NetworkProductIO.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkEmitStore(
    NetworkRunContext& ctx,
                     const GnssConfig& config_,
                     const std::shared_ptr<spdlog::logger>& log,
                     NetworkRunTraces& traces,
                     NetworkSetupResult& setup,
                     const SyncedEpoch& se,
                     const NetworkEstimator& est,
                     const Eigen::VectorXd& x_s,
                     double pl_now_s,
                     bool datum_changed,
                     double sigma0_adapt,
                     NetworkSolutionEpoch& sol,
                     const std::set<char>& epoch_withheld_consts,
                     bool global_bump,
                     const std::set<char>& serialized_consts,
                     const std::set<std::string>& retro_emit_sats,
                     bool emit_rebase_on,
                     bool emit_pb_filter_on,
                     bool pb_filter_emit_sigma,
                     NetworkEmitRebase& emit_rebase_engine,
                     NetworkPbEmitFilter& pb_filter_engine,
                     NetworkClockEmitRebase& clk_rebase_engine,
                     const std::vector<bool>& present,
                     const std::vector<int>& est_index_of,
                     const std::vector<std::vector<PppObservation>>& modeled,
                     const std::set<std::string>& shared_phase_keys,
                     const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                     bool we_t2_opp,
                     bool we_t2_flag,
                     std::size_t& cl_epochs,
                     std::size_t& cl_ar_ran,
                     std::size_t& cl_ar_fixed,
                     long long& cl_sum_fixed,
                     double& cl_first_fix_tow,
                     int& cl_first_fix_week,
                     std::deque<double>& cl_recent_err3d) {
    // --- Alias preamble: mechanically-derived byte-copies of the former
    //     Application::runNetwork alias block + 5 reconstructions, so the relocated
    //     SB_EMITSTORE body reads the original names. ---------------------------
    const auto& nc = config_.getNetworkConfig();
    const bool closed_loop_on = setup.closed_loop_on;
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    SsrCorrectionStore& network_store = setup.network_store;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& product_writer = setup.product_writer;
    auto& pending_contract = setup.pending_contract;
    auto& consumer = setup.consumer;
    auto& cl_trace             = traces.cl_trace;
    auto& soln                 = traces.soln;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
    auto& eq34_emitted_pdr = ctx.carry.eq34_emitted_pdr;
    auto& eq34_ppp_utils = ctx.carry.eq34_ppp_utils;
    auto& first_emit_content_s = ctx.carry.first_emit_content_s;
    const bool   ser_disc_ctrs   = ctx.knobs.ser_disc_ctrs;    // RTA-4: serialize post-emit disc-counter VALUES on C/B/P records
    const double contract_rebroadcast_s = ctx.knobs.contract_rebroadcast_s; // RTA-5: S-contract re-broadcast period (product time, s; 0 = off)
    const bool   emit_epoch_status = ctx.knobs.emit_epoch_status;     // RTA-10: per-epoch H heartbeat in the product file (incl. withheld epochs)
    auto& last_contract_emit_s = ctx.counters.last_contract_emit_s;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& last_emit_content_s = ctx.counters.last_emit_content_s;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& fr_we_trigger = ctx.counters.fr_we_trigger;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    const ClosedLoopConfig& cl = nc.closed_loop;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    const Eigen::Vector3d cl_truth = setup.cl_truth_arp;   // ARP-referenced (setup owns the reduction)
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    constexpr std::size_t kClRecentWindow = 30;

    // SPEC_day129_frame_guards Phase 2.2: an ENGAGED SH health gate withholds
    // this epoch's EMISSION — no store push, no product write, no closed-loop
    // consume/iono supply, no n_emitted advance (NetworkSolPrep already
    // early-returned on the same flag, so `sol` is empty).  The withheld H
    // heartbeat + per-epoch event keep the era visible on the wire and in the
    // traces; NetworkStashStatus stamps the epoch-status disposition.  The
    // carry stash and every QC tier still run — emission-only by design.
    if (nc.sh_health_gate && ctx.carry.sh_gate_engaged) {
        ++ctx.counters.n_sh_gate_withheld;
        traces.emitEvent(se.time, "SH_GATE", "withhold", "", "",
                         ctx.carry.sh_gate_last_a00, std::nullopt, "skip_emission",
                         fmt::format("rms_all={:.2f} oob_run={}",
                                     ctx.carry.sh_gate_last_rms,
                                     ctx.carry.sh_gate_oob_run));
        // RTA-10: product-level heartbeat for the WITHHELD epoch (H line, no E
        // line) — a live tail can tell "SH gate withheld" from "producer died".
        // Before the deferred writer opens (the first emit) the heartbeat is
        // BUFFERED in setup.pending_h_pre_open and flushed at the open.
        if (emit_epoch_status) {
            NetworkProducerStatus hst;
            hst.time.week = se.time.week;
            hst.time.tow  = se.time.tow;
            hst.state     = NetworkProducerStatus::State::WithheldShGate;
            // H-rider: the census σ̂₀ exists on a withheld epoch too (pre-solve).
            if (ctx.knobs.h_sigma0_census) hst.sigma0 = ctx.carry.ep_h_sigma0_census;
            hst.withheld  = "*";
            if (product_writer) {
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            } else if (setup.contract_pending) {
                // Buffer ONLY while a v2 writer open is still coming — with no
                // product path (or v1 immediate open) there is nothing to flush into.
                setup.pending_h_pre_open.push_back(hst);
            }
        }
        return;
    }

    // Card 4 (M1) phase D: emit-layer frame re-base (docs/SPEC_card4_invariant_carry.md §10
    // §1).  Under carry_invariant the eq34 datum is re-realized freely, so the raw
    // emitted per-(sat,σ) phase bias churns every epoch by a gauge element (a common
    // per-component fractional + a per-sat integer×λ) that leaves user DDs invariant
    // but would trip the emitter's 0.05 m jump test and storm the consumer with
    // spurious resets.  Re-reference the emitted biases onto the PREVIOUS EMISSION
    // frame BEFORE the store push + on-disk write, so both the in-process consumer
    // and a NetworkProductIO replay carry the re-based values (the transform is baked
    // into the product — replay reproduces the disc counters with no re-base rerun).
    // A genuine reference-path (tree-edge) slip's integer step is left INTACT so the
    // jump test still fires (never laundered — standing invariant #1).  Gated on
    // carry_invariant && emit_rebase ⇒ OFF path (carry_invariant=false) byte-identical.
    if (emit_rebase_on && !sol.sats.empty()) {
        // (a) Reference-path slip set: (sat,σ) whose emission reference — a spanning-
        //     tree/chain (pinned_amb_targets) edge — slipped this epoch.  Only a
        //     tree-edge slip moves the emitted b_s; a chord slip refloats its DD
        //     without touching the reference, so it is NOT a re-base survivor.
        std::set<std::pair<std::string, std::string>> rb_slipped_ref;   // (sat, signal)
        for (const auto& [sta_i, amb_key] : carry_slipped_ambs) {
            if (sta_i < 0 || sta_i >= static_cast<int>(stations.size())) continue;
            if (!stations[sta_i].pinned_amb_targets.count(amb_key)) continue;   // tree edge only
            const auto parts = StateKeys::parseAmbiguityKey(amb_key);
            if (parts && !parts->satellite.empty())
                rb_slipped_ref.insert({parts->satellite, parts->signal});
        }

        // (b) Connected-component partition of the (receiver,satellite) bipartite
        //     graph per (const,signal): the common fractional δ is shared only within
        //     a component (each elects its OWN datum receiver r1).  Union-find over
        //     this epoch's shared arcs — the same walk buildCarryDdPriors does.
        std::map<std::pair<char, std::string>, std::map<std::string, std::string>> dsu;
        auto findRoot = [](const std::map<std::string, std::string>& p,
                           std::string v) -> std::string {
            auto it = p.find(v);
            while (it != p.end() && it->second != v) { v = it->second; it = p.find(v); }
            return v;
        };
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || est_index_of[i] < 0) continue;
            for (const auto& [amb, sidx] : stations[i].sm->getAmbiguities()) {
                (void)sidx;
                const auto parts = StateKeys::parseAmbiguityKey(amb);
                if (!parts || parts->satellite.empty() || parts->satellite.front() == 'R')
                    continue;   // GLONASS FDMA excluded (no common-λ integer DD)
                if (!shared_phase_keys.count(
                        StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal)))
                    continue;   // not an eq34 graph arc (gated/legacy lump)
                auto& p = dsu[{parts->satellite.front(), parts->signal}];
                const std::string rv = "R" + std::to_string(i);
                const std::string sv = "S" + parts->satellite;
                if (!p.count(rv)) p[rv] = rv;
                if (!p.count(sv)) p[sv] = sv;
                p[findRoot(p, rv)] = findRoot(p, sv);   // union
            }
        }
        // (sat,signal) -> global component id (1-based; 0 = unassigned sentinel).
        std::map<std::string, int> comp_of;
        int next_comp = 0;
        for (auto& [g, p] : dsu) {
            std::map<std::string, int> root2id;
            for (const auto& [v, par] : p) {
                (void)par;
                if (v.empty() || v[0] != 'S') continue;   // satellite vertices only
                const std::string root = findRoot(p, v);
                int& id = root2id[root];
                if (id == 0) id = ++next_comp;
                comp_of[v.substr(1) + "|" + g.second] = id;
            }
        }

        // (c) Build re-base entries from the emitted phase biases (GLONASS + non-eq34
        //     keys excluded ⇒ their raw emitted frame is untouched).  λ per (const,σ).
        std::vector<NetworkEmitRebase::Entry> rb_entries;
        std::map<std::pair<char, std::string>, double> lambda_of;
        auto sysOfChar = [](char c) -> System {
            switch (c) { case 'G': return System::GPS;    case 'E': return System::Galileo;
                         case 'C': return System::BeiDou; case 'J': return System::QZSS;
                         default:  return System::Unknown; }
        };
        for (auto& s : sol.sats) {
            if (s.sat.empty() || s.sat.front() == 'R') continue;
            const char cons = s.sat.front();
            for (auto& [signal, npb] : s.phase_bias) {
                const auto cit = comp_of.find(s.sat + "|" + signal);
                if (cit == comp_of.end()) continue;   // legacy/gated key: keep raw frame
                const std::pair<char, std::string> g{cons, signal};
                double& lam = lambda_of[g];
                if (lam == 0.0 && eq34_ppp_utils)
                    lam = eq34_ppp_utils->getWavelength(sysOfChar(cons), signal, s.sat);
                NetworkEmitRebase::Entry e;
                e.sat = s.sat; e.signal = signal; e.component = cit->second;
                e.lambda_m = lam; e.bias_m = npb.bias_m;
                e.slipped = rb_slipped_ref.count({s.sat, signal}) != 0;
                rb_entries.push_back(std::move(e));
            }
        }
        emit_rebase_engine.rebaseEpoch(rb_entries);
        // Write the re-based biases back into the emitted epoch (match by key; the
        // engine mutates in place but a key match is robust to any future reorder).
        std::map<std::string, double> rb_out;
        for (const auto& e : rb_entries) rb_out[e.sat + "|" + e.signal] = e.bias_m;
        for (auto& s : sol.sats)
            for (auto& [signal, npb] : s.phase_bias) {
                const auto it = rb_out.find(s.sat + "|" + signal);
                if (it != rb_out.end()) npb.bias_m = it->second;
            }

        // M5 prerequisite: PB emission-continuity filter on the RE-BASED values.
        // A per-key scalar KF + self-tuning k·σ̂_step jump gate smooths the per-
        // epoch estimation noise the re-base leaves, so the emitter's (unchanged)
        // jump test fires only at genuine events — clearing the threshold-in-noise
        // discontinuity floor.  Filters EXACTLY the keys the re-base put into the
        // emission frame (rb_out); legacy/gated/GLONASS keys keep their raw value
        // + the emitter's normal jump test.  A reference-path slip (rb_slipped_ref,
        // the same set the re-base used) forces a reset that PASSES THE RAW VALUE
        // THROUGH, so the genuine step survives to the emitter — never laundered
        // (invariant #1).  Baked into sol BEFORE emitToStore + the on-disk write,
        // so a NetworkProductIO replay reproduces the emitter's disc counters with
        // no filter rerun (the transform is in the product, like the re-base).
        if (emit_pb_filter_on) {
            const double pbf_epoch_s =
                sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;
            std::vector<NetworkPbEmitFilter::Entry> pf_entries;
            pf_entries.reserve(rb_out.size());
            for (auto& s : sol.sats) {
                if (s.sat.empty() || s.sat.front() == 'R') continue;   // GLONASS excluded (FDMA)
                for (auto& [signal, npb] : s.phase_bias) {
                    if (!rb_out.count(s.sat + "|" + signal)) continue;  // only re-based keys
                    NetworkPbEmitFilter::Entry e;
                    e.sat = s.sat; e.signal = signal;
                    e.value_m = npb.bias_m;
                    e.slipped = rb_slipped_ref.count({s.sat, signal}) != 0;
                    pf_entries.push_back(std::move(e));
                }
            }
            pb_filter_engine.filterEpoch(pf_entries, pbf_epoch_s);
            std::map<std::string, std::pair<double, double>> pf_out;  // key -> (filtered value, posterior σ)
            for (const auto& e : pf_entries)
                pf_out[e.sat + "|" + e.signal] = {e.value_m, e.post_sigma_m};
            for (auto& s : sol.sats)
                for (auto& [signal, npb] : s.phase_bias) {
                    const auto it = pf_out.find(s.sat + "|" + signal);
                    if (it == pf_out.end()) continue;
                    npb.bias_m = it->second.first;
                    if (pb_filter_emit_sigma) npb.sigma_m = it->second.second;
                }
        }
    }
    // Card CLK-R (docs/SPEC_clock_emit_rebase.md): clock emission re-base — the
    // per-constellation common-mode FRAME HOLD (stage 1) + the census-aligned
    // per-sat STEP MARKS (stage 2).  Baked into sol BEFORE emitToStore + the
    // on-disk write, so both the in-process consumer and a NetworkProductIO
    // replay carry the held-frame values, and the marks reproduce from the
    // serialized E/R records with no re-run.  Outputs-only: the solve is
    // byte-identical, and between-sat differences are preserved exactly (one
    // shared subtrahend per constellation), so user SD/DD quantities are
    // untouched by construction.  Both knobs OFF ⇒ this block never runs.
    // Card CLV: stamp each satellite's level step since its previous emission (the
    // code-only re-base moved its clock and phase biases together) before anything
    // reads the clock values, so the clock re-base below, the emitter's phase-bias
    // jump test and the C record's lvl= token all see the same declaration.  No-op
    // unless network.clock_level_carry.
    networkClockLevelDeclare(ctx, sol);
    bool clk_reanchor_marked = false;
    const std::set<char>*        ser_consts_use = &serialized_consts;
    const std::set<std::string>* retro_use      = &retro_emit_sats;
    std::set<char>        clk_ser_consts;    // alive until the writer call below
    std::set<std::string> clk_retro_merged;  //   "
    if ((ctx.knobs.clk_frame_hold || ctx.knobs.clk_jump_test) && !sol.sats.empty()) {
        const double clk_epoch_s = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;
        // Forced keys: a per-sat retro bump or a datum-changed constellation this
        // epoch — marked upstream already, so they must neither drag the μ vote
        // nor double-mark.
        auto clkForced = [&](const std::string& sat) {
            if (retro_emit_sats.count(sat)) return true;
            if (sat.empty()) return false;
            return global_bump || serialized_consts.count(sat[0]) != 0;
        };
        std::vector<NetworkClockEmitRebase::Entry> ce;
        ce.reserve(sol.sats.size());
        for (const auto& s : sol.sats) {
            if (!s.has_clock || s.sat.empty()) continue;
            NetworkClockEmitRebase::Entry e;
            e.sat    = s.sat;
            e.c0_m   = s.clock_c0_m;
            e.forced = clkForced(s.sat);
            e.level_step_m = s.level_step_m;   // card CLV: 0 unless armed
            ce.push_back(std::move(e));
        }
        if (!ce.empty()) {
            // F1 seam rule: a warm-resumed lineage's first ARMED epoch is a datum
            // event — the engine cold-starts in the solve frame while a consumer
            // may still hold the previous run's held-frame products.  Mark it.
            const bool resume_seam = ctx.knobs.clk_frame_hold
                                     && clk_rebase_engine.counters().epochs == 0
                                     && n_emitted > 0;
            std::set<char> reanchored;
            clk_rebase_engine.rebaseEpoch(ce, clk_epoch_s, reanchored);
            std::map<std::string, double> clk_held;
            for (const auto& e : ce) {
                clk_held[e.sat] = e.c0_m;
                if (e.marked) {
                    traces.emitEvent(se.time, "CLK_STEP", "mark", e.sat, "",
                                     e.step_m, config_.networkClockDiscJumpM(),
                                     "retro_bump",
                                     "per-sat emitted-clock re-realization (common-removed step)");
                }
            }
            for (auto& s : sol.sats) {
                const auto it = clk_held.find(s.sat);
                if (it != clk_held.end()) s.clock_c0_m = it->second;
            }
            if (resume_seam)
                for (const auto& e : ce)
                    if (!e.sat.empty()) reanchored.insert(e.sat.front());
            if (!reanchored.empty()) {
                // Invariant #7: the re-anchor (or resume seam) is a re-coupling
                // edge — mark it so no consumer integrates across it silently.
                emitter.markDatumChange(reanchored);
                clk_ser_consts = serialized_consts;
                clk_ser_consts.insert(reanchored.begin(), reanchored.end());
                ser_consts_use     = &clk_ser_consts;
                clk_reanchor_marked = true;
                for (char cst : reanchored)
                    traces.emitEvent(se.time, "CLK_FRAME",
                                     resume_seam ? "resume_seam" : "reanchor",
                                     std::string(1, cst), "",
                                     clk_rebase_engine.counters().max_abs_acc,
                                     config_.networkClockFrameLeashM(), "datum_mark",
                                     "held frame re-anchored to the solve frame");
            }
            std::set<std::string> clk_marks;
            for (const auto& e : ce)
                if (e.marked) clk_marks.insert(e.sat);
            if (!clk_marks.empty()) {
                // Card MS: CLK-R stage 2 is a CLOCK step test — the emitted clock
                // value moved and nothing says the biases did, so under the
                // per-channel scope this mark reports the clock channel only.
                emitter.markSatDiscontinuity(clk_marks,   // R record + per-sat counters (B1 retro path)
                                             NetworkProductEmitter::kDiscChanClock);
                clk_retro_merged = retro_emit_sats;
                clk_retro_merged.insert(clk_marks.begin(), clk_marks.end());
                retro_use = &clk_retro_merged;
            }
        }
    }
    // RS-4 (SPEC_event_harness §21.9): the warm-resume PHASE-BIAS seam declaration.
    // The CLK-R rule above marks a resume only when the held-frame clock engine comes
    // back COLD (`epochs == 0`), so a lineage that RESTORES the frame is never marked —
    // exactly the case where the clock is continuous across the seam and the phase-bias
    // LEVEL is not (measured on the field golden: ~17 m p50 per (sat,σ) against the
    // restored baselines, with `datum_changed 0` and no R record on the wire).  The
    // emitter's per-key baselines are the previous run's emitted values only until
    // emitToStore overwrites them, so the census MUST be taken here, before that call.
    // One shot (the latch is cleared whether or not anything is marked), warm resumes
    // only (nothing sets the latch on a cold start), and outputs-only: `sol` is not
    // touched, so every estimate and every between-satellite difference is unchanged.
    std::set<char>        pb_ser_consts;    // alive until the writer call below
    if (ctx.knobs.resume_pb_seam_mark && ctx.carry.resume_pb_seam_pending
        && n_emitted > 0 && !sol.sats.empty()) {
        ctx.carry.resume_pb_seam_pending = false;
        const auto census = emitter.pbSeamCensus(sol);
        std::set<char> pb_marked;
        for (const auto& [cst, t] : census) {
            if (t.keys_tested <= 0) continue;
            ctx.counters.n_pb_seam_keys_tested += t.keys_tested;
            ctx.counters.n_pb_seam_keys_over   += t.keys_over;
            const double frac =
                static_cast<double>(t.keys_over) / static_cast<double>(t.keys_tested);
            if (frac <= GnssConfig::kResumePbSeamMarkFraction) continue;
            pb_marked.insert(cst);
            ++ctx.counters.n_pb_seam_marks;
            traces.emitEvent(se.time, "PB_FRAME", "resume_seam", std::string(1, cst), "",
                             frac, GnssConfig::kResumePbSeamMarkFraction, "datum_mark",
                             fmt::format("keys_over={}/{} ({:.1f} %) max|dPB|={:.3f} m "
                                         "vs restored baselines",
                                         t.keys_over, t.keys_tested, frac * 100.0,
                                         t.max_abs_delta_m));
        }
        if (!pb_marked.empty()) {
            // Same wiring as the CLK-R mark: the emitter consumes the forced bump inside
            // emitToStore, so the on-disk E record must carry the SAME constellation set
            // or a replay diverges from live.  Merge onto whatever the CLK-R block left
            // in *ser_consts_use (which is `serialized_consts` unless it re-anchored).
            emitter.markDatumChange(pb_marked);
            pb_ser_consts = *ser_consts_use;
            pb_ser_consts.insert(pb_marked.begin(), pb_marked.end());
            ser_consts_use = &pb_ser_consts;
        }
    }
    // Card PBH (docs/refound/PBH_phase_bias_hold_2026-09-19.md §5): the emit-side
    // phase-bias HOLD.  Deliberately the LAST writer of the emitted phase bias and
    // the last statement before emitToStore, which buys three things at once: it
    // freezes what the card-4 emission re-base and the PB emission filter produced
    // rather than being overwritten by them; it sits AHEAD of the emitter's
    // phase-bias value-jump test, so the declared population screens the value the
    // consumer actually receives; and every marking site of the epoch has already
    // run, so the seam's pendingMarkChannels read sees the complete set of channels
    // about to be declared (which is what the coupled clock+phase-bias cost census
    // needs).  The on-disk write below consumes the same `sol`, so the file and the
    // in-process store carry the same held values.  OFF ⇒ immediate return ⇒
    // byte-identical.
    networkPbHold(ctx, config_, log, traces, emitter, se, sol);
    // Card SID phase 5a (E1): the CAPTURE half, between the hold (which stamps the
    // generations into `sol`) and emitToStore (which overwrites the emitter's
    // per-family baselines, after which every key answers "not a first sighting").
    // Reads only; the fold that compares it with the ledger runs after the
    // roster's audit pass, where this epoch's admitted machine has been advanced.
    // OFF ⇒ immediate return ⇒ byte-identical.
    networkLedgerEmitCapture(ctx, emitter, sol);
    // Card SID phase 5a (E2): the declaration the producer is about to state on
    // the wire, built from the roster as it stands AFTER the datum-pin writer, so
    // the dph= / dco= designations describe this epoch's realized S-basis pin.
    // Built here rather than at the writer so the in-process store and the file
    // carry the same statement even when the writer is absent.
    if (ctx.ledger_emit.armed) {
        ++ctx.counters.le.decl_builds;
        NetworkSignalDeclaration decl = networkLedgerEmitBuildDeclaration(ctx, stations);
        if (decl.present && decl != ctx.ledger_emit.decl) {
            ctx.ledger_emit.decl         = std::move(decl);
            ctx.ledger_emit.decl_pending = true;
            ++ctx.counters.le.decl_changes;
            long long rows = 0;
            for (const auto& [cons, per_band] : ctx.ledger_emit.decl.bands) {
                (void)cons;
                rows += static_cast<long long>(per_band.size());
            }
            ctx.counters.le.decl_rows = rows;
        }
    }
    emitter.emitToStore(sol, network_store);
    // Card SID phase 5a (E2): the declaration reaches the IN-PROCESS consumer here,
    // immediately after this epoch's records and before the closed-loop consumer
    // reads them below, which is the same position the file replay gives it.
    if (ctx.ledger_emit.armed && ctx.ledger_emit.decl.present)
        network_store.setDeclaredRoster(ctx.ledger_emit.decl);
    // --- Card JT: drain the de-commoned jump test's classified cells ----------
    // Invariant #7 at BOTH edges with identities: the DECOUPLING edge is the
    // declaration this test raises (decl_decommon_only / decl_both) and the
    // RE-COUPLING edge is the declaration it withholds (raw_only_suppressed — the
    // frame moved, this satellite did not, and the consumer's receiver phase clock
    // absorbs it by construction).  Both are events, both are counted, and a
    // fallback_raw row names every cell whose signal was too thin for a frame
    // estimate.  The vector is EMPTY on an OFF run, so this loop never runs and the
    // OFF path stays byte-identical.
    for (const auto& e : emitter.pbJumpDecommonEvents()) {
        const char* kind = "decl_both";
        switch (e.cls) {
            case NetworkProductEmitter::PbJumpDecommonClass::DecommonOnly:
                kind = "decl_decommon_only"; ++ctx.counters.n_jt_decommon_only; break;
            case NetworkProductEmitter::PbJumpDecommonClass::Both:
                kind = "decl_both";          ++ctx.counters.n_jt_both;          break;
            case NetworkProductEmitter::PbJumpDecommonClass::RawOnlySuppressed:
                kind = "raw_only_suppressed"; ++ctx.counters.n_jt_raw_only;     break;
            case NetworkProductEmitter::PbJumpDecommonClass::FallbackRaw:
                kind = "fallback_raw";       ++ctx.counters.n_jt_fallback_raw;  break;
        }
        traces.emitEvent(se.time, "PB_JUMP", kind, e.sat, "",
                         e.decommon_m, e.threshold_m,
                         e.declared ? "declare" : "hold",
                         fmt::format("sig={} raw_dpb={:.6f} c_s={:.6f} decommon_dpb={:.6f} "
                                     "bar={:.6f} n_med={}",
                                     e.signal, e.raw_delta_m, e.common_m, e.decommon_m,
                                     e.threshold_m, e.population));
    }
    // --- Card MS: drain the mark-scope census of this emit --------------------
    // Invariant #7 at BOTH edges with identities.  The DECOUPLING edge is what a
    // per-satellite mark actually stepped (n_mark_clk / n_mark_cb / n_mark_pb) and
    // what it was WITHHELD from (n_mark_suppressed_by_scope, one MARK_SCOPE row per
    // suppressed (sat, channel) with the mask the marking sites asked for).  The
    // RE-COUPLING edge is not this card's: a channel the mark did not name is still
    // declared by the emitter's own per-(sat, signal) jump test if its value
    // genuinely re-realized — that is what the TOW 276810 population rides.  Both
    // the tally and the vector are zero/empty under the default `sat` scope, so the
    // OFF path stays byte-identical and this block is a no-op.
    {
        const auto& mt = emitter.markChannelTally();
        ctx.counters.n_mark_clk += mt.clk;
        ctx.counters.n_mark_cb  += mt.cb;
        ctx.counters.n_mark_pb  += mt.pb;
        for (const auto& e : emitter.markScopeEvents()) {
            ++ctx.counters.n_mark_suppressed_by_scope;
            switch (e.suppressed) {
                case NetworkProductEmitter::kDiscChanClock:
                    ++ctx.counters.n_mark_suppressed_by_scope_clk; break;
                case NetworkProductEmitter::kDiscChanCodeBias:
                    ++ctx.counters.n_mark_suppressed_by_scope_cb;  break;
                case NetworkProductEmitter::kDiscChanPhaseBias:
                    ++ctx.counters.n_mark_suppressed_by_scope_pb;  break;
                default: break;
            }
            traces.emitEvent(se.time, "MARK_SCOPE", "suppressed", e.sat, "",
                             std::nullopt, std::nullopt,
                             NetworkProductEmitter::discChannelName(e.suppressed),
                             fmt::format("marked_channels={}{}{}",
                                         (e.marked & NetworkProductEmitter::kDiscChanClock) ? "clk " : "",
                                         (e.marked & NetworkProductEmitter::kDiscChanCodeBias) ? "cb " : "",
                                         (e.marked & NetworkProductEmitter::kDiscChanPhaseBias) ? "pb" : ""));
        }
    }
    // RTA-5: periodic S-contract re-broadcast — a CURRENT S line (refreshed
    // realized pivots) every N seconds of PRODUCT time, and additionally on any
    // epoch whose emit carried a datum change (markDatumChange fired above), so a
    // late joiner sees the contract and a post-handover reader sees the pivots the
    // product is NOW realized in.  Pivots refresh from last_pivot — the merged
    // emit-baseline map that advanced just above, paired with this epoch's bump:
    // it holds this epoch's realized pivot for every present constellation and the
    // last-EMITTED pivot for an absent one, i.e. exactly the datum of the product
    // as of this emit.  pending_contract is refreshed in place so the end-of-run
    // round-trip self-check compares against the LAST written contract.  Written
    // BEFORE the epoch block so the S line positionally precedes the epochs it
    // describes; the elapsed>0 guard keeps the first-emit epoch single-S (the
    // header line just written).  OFF (0) ⇒ no extra S lines (byte-identical).  Card OWD:
    // the contract is on every wire, so contract_rebroadcast_s alone governs.
    if (contract_rebroadcast_s > 0.0 && product_writer
        && last_contract_emit_s >= 0.0 && pl_now_s > last_contract_emit_s) {
        const bool due = (pl_now_s - last_contract_emit_s) >= contract_rebroadcast_s;
        // Card 3 (D4): a datum-receiver (root) handover re-references the whole
        // component's biases by a common fractional + per-sat integers — a
        // materially softer event than a clock-pivot handover, but still a
        // datum break the consumer should re-sync the contract on.
        const std::string cur_pdr = (phase_datum_eq34 && !eq34_datum_rx.empty())
                                      ? eq34_datum_rx.begin()->second : std::string{};
        const bool eq34_root_changed = phase_datum_eq34 && cur_pdr != eq34_emitted_pdr;
        if (due || datum_changed || eq34_root_changed || clk_reanchor_marked) {
            for (const auto& [cst, sat] : last_pivot)
                pending_contract.decl.clock_pivot_sat[cst] = sat;   // refresh realized pivots
            if (phase_datum_eq34) {
                pending_contract.decl.eq34_datum_receiver = cur_pdr;   // refresh pdr=
                eq34_emitted_pdr = cur_pdr;
            }
            product_writer->writeContract(pending_contract);
            last_contract_emit_s = pl_now_s;
        }
    }
    if (product_writer) {
        // Card SID phase 5a (E2): the DECLARED SIGNAL ROSTER, written as a
        // contiguous N block immediately before this epoch's heartbeat and epoch
        // block: the S-line positional precedent, so a positional reader holds
        // the declaration for the epochs that follow it.  Written only when the
        // declaration CHANGED (and therefore always at the first emitted epoch),
        // which is what keeps the wire's growth proportional to the roster's
        // motion rather than to the run's length.  ⚠ A consumer that joins
        // mid-file sees none until the next change; the wire carries no periodic
        // re-broadcast in this revision.
        if (ctx.ledger_emit.decl_pending) {
            ctx.ledger_emit.decl_pending = false;
            const std::size_t n = product_writer->writeSignalDeclaration(ctx.ledger_emit.decl);
            ctx.counters.le.decl_records += static_cast<long long>(n);
            for (const std::string& l : signalDeclarationToWire(ctx.ledger_emit.decl))
                ctx.counters.le.decl_bytes += static_cast<double>(l.size() + 1);
            log->info("Network (card SID phase 5a): declared signal roster written to the wire "
                      "({} N record(s), TOW {:.1f}): {}",
                      n, se.time.tow, signalDeclarationSummary(ctx.ledger_emit.decl));
        }
        // RTA-10: the emitted epoch's heartbeat, written immediately BEFORE its epoch
        // block (the S-line positional precedent — an H describes what follows).
        // status=partial iff any constellation was withheld from this emission (the
        // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out);
        // sigma0 = the global within-epoch σ̂0 (0 when the within-epoch RAIM did not
        // run this epoch — "unavailable" per the H grammar); n_sats counts the
        // POST-prune emission.  Never feeds replay; flushed with the epoch below.
        if (emit_epoch_status) {
            NetworkProducerStatus hst;
            hst.time.week        = se.time.week;
            hst.time.tow         = se.time.tow;
            hst.state            = epoch_withheld_consts.empty()
                                       ? NetworkProducerStatus::State::Ok
                                       : NetworkProducerStatus::State::Partial;
            // H-rider (network.h_sigma0_prefit_census): the census-fed σ̂₀,
            // UNIFORM across every H site when armed (the S contract carries
            // hsig=pc so a consumer knows which population field 5 is).  The
            // legacy feed (sigma0_adapt) is structurally 0.0000 on every
            // no-leveling lineage and a degenerate 1.0000 on quiet leveled
            // ones — the wire shipped with no usable residual statistic.
            hst.sigma0           = ctx.knobs.h_sigma0_census
                                       ? ctx.carry.ep_h_sigma0_census
                                       : sigma0_adapt;
            hst.n_sats_published = static_cast<int>(sol.sats.size());
            if (epoch_withheld_consts.empty()) {
                hst.withheld = "-";
            } else {
                hst.withheld.clear();
                for (char cst : epoch_withheld_consts) {
                    if (!hst.withheld.empty()) hst.withheld += ';';
                    hst.withheld += cst;
                }
            }
            product_writer->writeEpochStatus(hst);
        }
        // Card CLK-R: *_use point at the caller's sets unless this epoch added a
        // frame re-anchor datum mark / step marks (then they point at the merged
        // locals above) — the on-disk E/R records must describe every bump the
        // emitToStore call above consumed, or replay diverges from live.
        const bool any_bump = global_bump || !ser_consts_use->empty();
        product_writer->write(sol, any_bump, global_bump ? std::set<char>{} : *ser_consts_use,
                              *retro_use,
                              // RTA-4: post-emit counter source — emitToStore ran
                              // above, so the emitter's counters ARE this epoch's
                              // store-record values.
                              ser_disc_ctrs ? &emitter : nullptr);
        // RTA-6: per-epoch flush so a live tail consumer (NetworkProductTail)
        // sees this epoch block now, not at an ofstream-buffer boundary.
        // Covers the RTA-5 S re-broadcast written above too (same stream).
        // Changes no bytes; default-off toggle discipline.
        if (nc.product_flush_per_epoch) product_writer->flush();
    }
    if (n_emitted == 0) first_emit_content_s = pl_now_s;   // RTB-7(c): grade-metric anchor
    ++n_emitted;
    last_emit_content_s = pl_now_s;   // RTB-6: content time of this emit (health last-emit age)
    // R12: commit the B8/T2 within-epoch-trigger flag-rate now that the epoch has EMITTED
    // (withheld/skipped epochs never reach here, so T2's denominator = emitted epochs, the
    // same population T3–T6 count over).  Guarded by trace_flag_rates ⇒ OFF byte-identical.
    if (trace_flag_rates && we_t2_opp) {
        ++fr_we_trigger.opp;
        if (we_t2_flag) ++fr_we_trigger.flag;
    }

    // --- Closed-loop consume: held-out station vs the fresh network store -
    // Run the consumer ONLY now that this epoch's products are in the store,
    // so its SSR clock + phase bias reflect the just-solved network solution.
    if (closed_loop_on && consumer) {
        auto cit = se.by_station.find(cl.station.name);
        if (cit != se.by_station.end() && !cit->second.empty()) {
            // --- Supply this epoch's SH-VTEC slant ionosphere to the consumer -
            // Evaluate the just-solved global VTEC field at the consumer's own
            // pierce points and push an UNDIFFERENCED per-satellite slant-iono
            // value (metres on L1, Odijk-2016 Table 11) into the store BEFORE
            // the consumer reads it, so it applies the tight ionosphere-weighted
            // prior in place of floating its slant iono.  Reuses the producer's
            // own SH→slant machinery (appendShIonoRows) at the consumer geometry.
            // OFF (default) ⇒ nothing pushed, consumer applies nothing (byte-
            // identical).  In-process only — not part of the on-disk product.
            if (cl.consume_network_products && cl.consume_network_iono && sh_model) {
                const auto& iw = config_.getIonosphereWeightingConfig();
                const double Re = shc.earth_radius_km * 1000.0;
                const double H  = shc.layer_height_km * 1000.0;
                const double tecu_to_L1m =
                    40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
                const double lon_sun = shc.sun_fixed ? meanSunLongitude(se.time) : 0.0;
                // SH coefficient vector from the shared solution (guard the
                // column lookups, as elsewhere in this file).
                const auto& coeffs = sh_model->coeffs();
                Eigen::VectorXd coeff(static_cast<Eigen::Index>(coeffs.size()));
                bool coeff_ok = true;
                for (std::size_t k = 0; k < coeffs.size() && coeff_ok; ++k) {
                    try {
                        coeff(static_cast<Eigen::Index>(k)) = x_s(
                            est.vtecCoeffColumn(coeffs[k].n, coeffs[k].m, coeffs[k].trig));
                    } catch (...) { coeff_ok = false; }
                }
                // Iono-2 dual-layer: second-shell coefficients for the layer-sum
                // supply (the consumer receives the SUMMED slant — it is
                // layer-agnostic by design).
                Eigen::VectorXd coeff2;
                if (coeff_ok && sh_model2) {
                    const auto& c2 = sh_model2->coeffs();
                    coeff2.resize(static_cast<Eigen::Index>(c2.size()));
                    for (std::size_t k = 0; k < c2.size() && coeff_ok; ++k) {
                        try {
                            coeff2(static_cast<Eigen::Index>(k)) = x_s(
                                est.vtecCoeffColumn(c2[k].n, c2[k].m, c2[k].trig, 1));
                        } catch (...) { coeff_ok = false; }
                    }
                }
                if (coeff_ok) {
                    // The consumer's raw RINEX carries no geometry; harvest each
                    // satellite's ECEF (receiver-independent at IPP accuracy) from
                    // the network's already-modeled obs this epoch.
                    std::map<std::string, Eigen::Vector3d> sat_pos;
                    for (const auto& mv : modeled)
                        for (const auto& o : mv)
                            if (!o.satellite.empty() && o.sat_pos.allFinite())
                                sat_pos.emplace(o.satellite, o.sat_pos);
                    // T3f: SH-coefficient covariance + column map + fresh-OSB set (built ONLY
                    // when the formal-σ supply is armed; unused/empty otherwise).
                    std::vector<Eigen::Index> sh_cols;
                    std::set<std::string>     fresh_osb;
                    const Eigen::MatrixXd*    shcov_p = nullptr;
                    if (nc.iono_supply_formal_sigma) {
                        sh_cols.assign(coeffs.size(), -1);
                        for (std::size_t k = 0; k < coeffs.size(); ++k)
                            try { sh_cols[k] = est.vtecCoeffColumn(
                                      coeffs[k].n, coeffs[k].m, coeffs[k].trig); }
                            catch (...) {}
                        shcov_p = &est.sharedCovariance();
                        if (config_.estimateNetworkSatCodeBias())
                            for (const auto& s : sol.sats)
                                if (!s.code_bias_m.empty()) fresh_osb.insert(s.sat);
                    }
                    int n_iono = 0;
                    for (const auto& o : cit->second) {
                        if (o.satellite.empty() || o.satellite.front() == 'R')
                            continue;   // GPS/Galileo only — the SH field excludes GLONASS
                        const auto pit = sat_pos.find(o.satellite);
                        if (pit == sat_pos.end()) continue;
                        // T3f: skip the supply for a sat without FRESH OSB coverage this epoch
                        // — the SH-implied slant leans on the code-bias observability, so an
                        // OSB-less sat's field value is not trustworthy enough to inject.
                        if (nc.iono_supply_formal_sigma
                            && config_.estimateNetworkSatCodeBias()
                            && !fresh_osb.count(o.satellite)) continue;
                        const IppGeometry g = computeIppGeometry(cl_truth, pit->second, Re, H);
                        if (!g.valid) continue;
                        if (g.elevation_rad * 180.0 / Constants::PI < iw.elevation_cutoff_deg)
                            continue;
                        const double lon_sf = g.ipp_lon_rad - lon_sun;
                        const double vtec   = sh_model->vtec(coeff, g.ipp_lat_rad, lon_sf);
                        SsrSlantIono si;
                        si.sat           = o.satellite;
                        si.iono_m        = vtec * g.mapping * tecu_to_L1m;   // slant on L1 (m)
                        const double map = g.mapping * tecu_to_L1m;          // VTEC(TECU)→slant(m)
                        // Dual-layer: sum the second shell at its own pierce
                        // geometry (invalid ⇒ zero contribution, as in the rows).
                        IppGeometry g2; g2.valid = false;
                        if (sh_model2) {
                            g2 = computeIppGeometry(cl_truth, pit->second, Re,
                                                    shc.layer2_height_km * 1000.0);
                            if (g2.valid)
                                si.iono_m += sh_model2->vtec(coeff2, g2.ipp_lat_rad,
                                                             g2.ipp_lon_rad - lon_sun)
                                             * g2.mapping * tecu_to_L1m;
                        }
                        double sigma = std::max(iw.floor_m,
                                                shc.representation_sigma_tecu * map);
                        if (nc.iono_supply_formal_sigma && shcov_p) {
                            // T3f: RSS the SH-coefficient FORMAL σ onto the representativeness
                            // floor.  aᵀ Q_sh a (a = ∂VTEC/∂coeff basis row, Q_sh = SH coeff
                            // covariance, TECU²) → VTEC variance; ×map → slant metres.
                            std::vector<double> a;
                            sh_model->basis(g.ipp_lat_rad, lon_sf, a);
                            if (!sh_model2) {
                                double aQa = 0.0;
                                for (std::size_t j = 0; j < sh_cols.size(); ++j)
                                    for (std::size_t k = 0; k < sh_cols.size(); ++k)
                                        if (sh_cols[j] >= 0 && sh_cols[k] >= 0
                                            && sh_cols[j] < shcov_p->rows()
                                            && sh_cols[k] < shcov_p->cols())
                                            aQa += a[j] * (*shcov_p)(sh_cols[j], sh_cols[k]) * a[k];
                                aQa = std::max(0.0, aQa);   // guard tiny negatives
                                const double repr   = shc.representation_sigma_tecu * map;
                                const double formal = map * std::sqrt(aQa);
                                sigma = std::max(iw.floor_m, std::sqrt(repr * repr + formal * formal));
                            } else {
                                // Dual-layer: exact slant formal σ over the
                                // CONCATENATED per-layer weighted basis
                                // w = [map₁·a₁ ; map₂·a₂] (cross-layer
                                // covariance included) — wᵀ Q w is already m².
                                std::vector<std::pair<Eigen::Index, double>> wv;
                                wv.reserve(sh_cols.size() + 64);
                                for (std::size_t j = 0; j < sh_cols.size(); ++j)
                                    if (sh_cols[j] >= 0 && sh_cols[j] < shcov_p->rows())
                                        wv.emplace_back(sh_cols[j], map * a[j]);
                                if (g2.valid) {
                                    std::vector<double> a2;
                                    sh_model2->basis(g2.ipp_lat_rad,
                                                     g2.ipp_lon_rad - lon_sun, a2);
                                    const double map2 = g2.mapping * tecu_to_L1m;
                                    const auto& c2 = sh_model2->coeffs();
                                    for (std::size_t j = 0; j < c2.size(); ++j) {
                                        try {
                                            const Eigen::Index cj = est.vtecCoeffColumn(
                                                c2[j].n, c2[j].m, c2[j].trig, 1);
                                            if (cj >= 0 && cj < shcov_p->rows())
                                                wv.emplace_back(cj, map2 * a2[j]);
                                        } catch (...) {}
                                    }
                                }
                                double wQw = 0.0;
                                for (const auto& [cj, wj] : wv)
                                    for (const auto& [ck, wk] : wv)
                                        wQw += wj * (*shcov_p)(cj, ck) * wk;
                                wQw = std::max(0.0, wQw);
                                const double repr = shc.representation_sigma_tecu * map;
                                sigma = std::max(iw.floor_m,
                                                 std::sqrt(repr * repr + wQw));
                            }
                        }
                        si.sigma_m       = sigma;
                        si.received_time = se.time;
                        network_store.pushSlantIono(si);
                        ++n_iono;
                    }
                    if (n_emitted == 1 || n_emitted % 60 == 0)
                        log->info("Network closed-loop: supplied SH-VTEC slant iono for {} "
                                  "consumer satellite(s) (TOW {:.1f}).", n_iono, se.time.tow);
                }
            }
            consumer->processEpoch(cit->second);
            ++cl_epochs;

            const Eigen::Vector3d pos = consumer->getPosition();
            const Eigen::Vector3d d   = pos - cl_truth;
            const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, cl_truth_llh);
            const double err3d = d.norm();

            const bool ar_ran = consumer->lastArRan();
            const AmbiguityResolution& ar = consumer->lastArResult();
            const bool fixed = ar_ran && ar.success;
            if (ar_ran) ++cl_ar_ran;
            if (fixed) {
                ++cl_ar_fixed;
                cl_sum_fixed += ar.num_fixed;
                if (cl_first_fix_tow < 0.0) {
                    cl_first_fix_tow = se.time.tow; cl_first_fix_week = se.time.week;
                }
            }
            // Converged-segment position RMS: rolling window over the most
            // recent epochs (the float transient + any early wrong-fix
            // excursions are not a fair measure of steady-state quality).
            cl_recent_err3d.push_back(err3d);
            if (cl_recent_err3d.size() > kClRecentWindow) cl_recent_err3d.pop_front();

            if (cl_trace) {
                // T2c: attribute WHICH gate zeroed AR this epoch.  gate_reason is set at
                // the pre-MLAMBDA early gates; reason 0 means MLAMBDA was reached, so the
                // outcome is read from success/ratio (fixed / ratio-battery reject / a
                // degenerate MLAMBDA that produced no ratio).
                std::string ar_gate;
                if (!ar_ran) {
                    ar_gate = "not_run";
                } else switch (ar.gate_reason) {
                    case 1:  ar_gate = "disabled";      break;
                    case 2:  ar_gate = "min_epochs";    break;
                    case 3:  ar_gate = fmt::format("sigma_gate({:.3f}m)", ar.gate_sigma_3d); break;
                    case 4:  ar_gate = "no_candidates"; break;
                    case 5:  ar_gate = "wl_guard";      break;
                    default: ar_gate = ar.success ? (ar.wl_only_fix ? "wl_only" : "fixed")
                                     : (ar.ratio > 0.0 ? "ratio_battery" : "mlambda_fail");
                             break;
                }
                cl_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << cit->second.size() << ',' << (ar_ran ? 1 : 0) << ','
                         << (fixed ? 1 : 0) << ',' << (ar_ran ? ar.num_fixed : 0) << ','
                         << (ar_ran ? ar.num_candidates : 0) << ','
                         << fmt::format("{:.2f}", ar_ran ? ar.ratio : 0.0) << ','
                         << fmt::format("{:.4f}", err3d) << ','
                         << fmt::format("{:.4f}", enu.x()) << ','
                         << fmt::format("{:.4f}", enu.y()) << ','
                         << fmt::format("{:.4f}", enu.z()) << ','
                         << ar_gate << ','
                         << fmt::format("{:.6f}", ar_ran ? ar.p_succ : 0.0) << '\n';
            }
            // Card GAL: the consumer's receiver inter-system bias, same stamp as the
            // row above.  The stream is open only with a second constellation active.
            if (traces.cl_isb_trace.is_open()) {
                const double clk = consumer->getClockBias();
                for (const auto& [sys, vs] : consumer->getSystemTimeOffsets())
                    traces.cl_isb_trace << se.time.week << ','
                                        << fmt::format("{:.3f}", se.time.tow) << ',' << sys
                                        << ',' << fmt::format("{:.4f}", vs.first) << ','
                                        << fmt::format("{:.4f}", vs.second) << ','
                                        << fmt::format("{:.4f}", clk) << '\n';
            }
            // Card CST: the per-SATELLITE twin of the row above.  The engine
            // harvested its own epoch at the processEpoch tail (traceConsumerSat_);
            // this seam only moves the buffer to the stream NetworkRunTraces owns,
            // stamped with the SAME (week, tow) closed_loop_trace.csv just used, so
            // the two files join with no tolerance.  Both counters are invariant
            // #4's ON-path telemetry: rows == 0 on an armed run means the consumer
            // deferred every epoch, which no absence-of-crash reading would show.
            if (nc.trace_consumer_sat) {
                const std::size_t n_cst =
                    traces.writeConsumerSatRows(se.time, consumer->consumerSatRows());
                if (n_cst > 0) ++ctx.counters.n_cst_epochs;
                ctx.counters.n_cst_rows += static_cast<long long>(n_cst);
            }
            // Card ALC: drain the consumer's AMB_LEVEL events into network_events.csv
            // and the run counters -- invariant #7's BOTH edges with identities.  The
            // DECOUPLING edge is the `trip` row (which test, |state-level|, sigma_level,
            // n, and both statistics in the detail); the RE-COUPLING edge is the
            // `refloat` row naming the two ambiguity keys re-floated and the std they
            // were re-floated at.  In `observe` mode the trip rows appear and the
            // refloat rows do not, which is the mode read back off the artefact.  The
            // engine's buffer is cleared at the top of every processEpoch, so an epoch
            // the consumer deferred drains nothing rather than repeating the last one.
            if (consumer->ambLevelCheckArmed()) {
                ctx.counters.n_amb_level_checks +=
                    static_cast<long long>(consumer->ambLevelEpochChecks());
                for (const auto& e : consumer->ambLevelEvents()) {
                    const bool is_trip = (std::string(e.kind) == "trip");
                    if (is_trip) {
                        if (std::string(e.test) == "wl") ++ctx.counters.n_amb_level_trips_wl;
                        else                             ++ctx.counters.n_amb_level_trips_if;
                    } else {
                        ++ctx.counters.n_amb_level_refloats;
                    }
                    traces.emitEvent(se.time, "AMB_LEVEL", e.kind, e.sat, "",
                                     e.diff, e.sigma, e.test,
                                     fmt::format("n={} {}", e.n, e.detail));
                }
            }
        }
    }

    // Per-satellite product trace (after emit, so discontinuity counters are current).
    if (soln) {
        for (const auto& s : sol.sats) {
            if (s.has_clock)
                soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                     << s.sat << ",CLK,," << fmt::format("{:.4f}", s.clock_c0_m)
                     << ',' << emitter.clockDiscontinuityCounter(s.sat) << '\n';  // A2: 0 when off (byte-identical)
            for (const auto& [signal, pb] : s.phase_bias)
                soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                     << s.sat << ",PB," << signal << ',' << fmt::format("{:.4f}", pb.bias_m)
                     << ',' << emitter.discontinuityCounter(s.sat, signal) << '\n';
            for (const auto& [signal, cb] : s.code_bias_m)   // W6 satellite code bias (OSB)
                soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                     << s.sat << ",CB," << signal << ',' << fmt::format("{:.4f}", cb)
                     << ',' << emitter.codeBiasDiscontinuityCounter(s.sat, signal) << '\n';
        }
    }
}

} // namespace GnssProcessor
