/**
 * @file WarmResume.cpp
 * @brief @internal The warm resume (RTB-7): read a state snapshot, validate it against
 *        the run, and restore the carried state one record layer at a time.
 *
 * Three functions, called by the driver (network/driver/ApplicationNetwork.cpp) in this order before
 * the first epoch:
 *   networkTryWarmResume            reads and validates the snapshot, restores the layers
 *                                   held by the run context, the product emitter, the
 *                                   carry driver and the emit engines, and stages the two
 *                                   layers that need the station roster (the CN and RL
 *                                   rows);
 *   networkRestoreCanonicalSignals  hands the staged canonical-signal rows to the station
 *                                   models (RS-8);
 *   networkApplyRxCbLinRestore      installs the staged code-bias linearization points on
 *                                   the station initializers (RS-9).
 * The write of each layer is in StateSnapshotWrite.cpp, and the fingerprint the
 * resume checks is formed in SnapshotFingerprint.cpp.
 *
 * networkTryWarmResume opens with an alias block (decomposition INC-4) that binds the
 * driver's former local names to members of the context, so the relocated code keeps those
 * names.
 *
 * One of the seven files that define the network run context's functions, split
 * out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/network/state/RunContext.hpp.
 */

#include "network/state/RunContext.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // letterOfSat
#include "network/carry/InvariantCarryDriver.hpp"   // branch-faithful carry (de)serialization (RTB-7(c))
#include "network/emit/ClockEmitRebase.hpp"       // CLK-R held frame, restored on warm resume (§18.10)
#include "network/emit/EmitRebase.hpp"            // eq34 emit re-base frame, restored on warm resume (RS-5, §21.10)
#include "math/TimeUtils.hpp"                // TimeUtils::continuousGpsSeconds
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include "network/carry/LocalCarryHarvest.hpp"   // HarvestBorder, StationHarvestBlock
#include "network/driver/Run.hpp"
#include "network/driver/Epoch.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "network/diag/AmbStateTrace.hpp"   // AmbStateRow / ambStateNoteErase (RF-1 §7.4)
#include "network/ar/AmbFloat.hpp"       // ambFloatBirthFloor (RF-1 step 4)
#include "network/carry/CarryVce.hpp"
#include "network/assembly/ParamInventory.hpp" // rxIfbFamilyReseeded
#include "network/qc/PriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "network/solve/NormalEquations.hpp"
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan

namespace GnssProcessor {

void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                          const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                          NetworkInvariantCarryDriver& carry_driver,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkClockEmitRebase* clk_rebase,
                          NetworkEmitRebase* emit_rebase) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const auto& nc = config_.getNetworkConfig();
    const auto& synced = sync.getSyncedEpochs();
    auto& snap_path = ctx.carry.snap_path;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
    auto& prev_carry = ctx.carry.prev_carry;
    net::StationTable& sta_rows = ctx.entities.stations;   // the station carries
    net::NetworkEntities& entities = ctx.entities;   // written: constellations sat_signals sats
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& code_bias_datum_resumed_ = ctx.carry.code_bias_datum_resumed_;
    auto& ura_ewma_updates = ctx.carry.ura_ewma_updates;
    auto& ramp_lifted = ctx.carry.ramp_lifted;
    auto& first_emit_content_s = ctx.carry.first_emit_content_s;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    auto& generation_id = ctx.counters.generation_id;
    auto& iod = ctx.counters.iod;
    auto& prev_week = ctx.counters.prev_week;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_sigma0_withheld = ctx.counters.n_sigma0_withheld;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
    auto& last_emit_content_s = ctx.counters.last_emit_content_s;
    auto& last_health_write_s = ctx.counters.last_health_write_s;
    const double start_offset_s = ctx.knobs.start_offset_s;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   fullcov_on = ctx.knobs.fullcov_on;
    const bool   ura_on = ctx.knobs.ura_on;

    // With state_snapshot_require armed, any failure to complete a warm resume is a run
    // error instead of a warning and a cold start (RTB-7(d), HB-1).  This guards
    // same-snapshot A/B runs: a resume overwrites the snapshot it read, so a relaunch that
    // was not re-seeded would otherwise be graded as warm while running cold.
    //
    // NetworkStateSnapshotIO::warmResumeRefusalMessage builds the message, so every
    // branch below reports the refusal in the same words, and the non-positive-age branch
    // names its own remedy (SPEC_event_harness §6b).  The throw is caught at the
    // runNetwork() call site, which logs e.what() as a single error line and returns 1: a
    // controlled abort with the reason in the log, not a 0xC0000409 fast-fail.
    auto hb1 = [&](WarmResumeRefusal why, const std::string& detail) {
        const std::string msg = NetworkStateSnapshotIO::warmResumeRefusalMessage(
                nc.state_snapshot_require, why, detail);
        if (!msg.empty()) throw std::runtime_error(msg);
    };
    // The cold-start warnings below are suppressed under `require`, because the run is
    // about to refuse the cold start they would announce (§6b).
    const bool cold_ok = !nc.state_snapshot_require;
    if (nc.state_snapshot_require && !snap_path.empty()
        && !std::filesystem::exists(snap_path))
        hb1(WarmResumeRefusal::MissingFile,
            "snapshot file '" + snap_path.string() + "' does not exist");
    // Carry VCE apply (M2 v2) has one field-blocking deferral, enforced here: the variance
    // factor α² is cross-epoch, affects the solve, and is not in the snapshot.  A resumed
    // leg would restart every group at α² = 1 and climb back at the slew rate, so the
    // carry priors on either side of the seam would encode different variances for the
    // same carried values.  The fingerprint cannot detect this (an apply-on seed and an
    // apply-on leg fingerprint identically), so the refusal is here.  v1 supports fresh
    // convergence only; lifting that needs an α² field in the snapshot (there is none)
    // plus a resume seed of the monitor through NetworkCarryVce::seedRobustFloor.
    if (ctx.knobs.vce_apply_on && !snap_path.empty()
        && std::filesystem::exists(snap_path))
        throw std::runtime_error(
            "network.carry_vce_apply (M2 v2) is armed and a state snapshot exists at '"
            + snap_path.string() + "' — a WARM RESUME is refused.  The carry variance factor "
            "alpha2 is cross-epoch, solve-affecting and deliberately NOT snapshotted "
            "(docs/SPEC_carry_vce_apply.md section 8), so resuming would fold the restored "
            "carries at alpha2 = 1 and re-converge across the seam.  v1 is "
            "fresh-convergence-only: run this arm from a cold start (move or delete the "
            "snapshot, or clear network.state_snapshot_path).");
    if (!snap_path.empty() && std::filesystem::exists(snap_path)) {
        NetworkStateSnapshot snap;
        std::string err;
        if (!NetworkStateSnapshotIO::read(snap_path, snap, &err)) {
            if (cold_ok)
                log->warn("Network (RTB-7): state snapshot '{}' failed to parse ({}) — COLD START "
                          "(a new generation id is minted at first emit as usual).",
                          snap_path.string(), err);
            hb1(WarmResumeRefusal::ParseFailure, "parse failure: " + err);
        } else {
            // First epoch this run processes (after the start_offset_hours fast-forward);
            // it is the age reference of the gate.
            double first_live_s = -1.0;
            if (!synced.empty()) {
                const double anchor_s = TimeUtils::continuousGpsSeconds(synced.front().time);
                for (const auto& sf : synced) {
                    const double t_s = TimeUtils::continuousGpsSeconds(sf.time);
                    if (start_offset_s <= 0.0
                        || TimeUtils::offsetReached(t_s, anchor_s, start_offset_s)) {
                        first_live_s = t_s;
                        break;
                    }
                }
            }
            // Carry-validity horizon = max of the per-key gap bounds.  Past it every
            // restored key would be dropped or reseeded downstream anyway.  Within it,
            // per-key staleness after the restore is handled by the existing logic
            // (local-carry max-gap drop, D2 q·dt inflation over the true gap through the
            // restored prev_week/prev_tow, clock-model per-satellite t_last gap logic), so
            // everything is restored and each key is judged by its own rule.
            const double horizon_s = std::max(pl_max_gap_s, clk_model_max_gap);
            std::string gate;
            WarmResumeRefusal gate_why = WarmResumeRefusal::None;
            if (first_live_s < 0.0) {
                if (cold_ok)
                    log->warn("Network (RTB-7): state snapshot '{}' found but there are no live "
                              "epochs to resume against — COLD START.", snap_path.string());
                hb1(WarmResumeRefusal::NoLiveEpochs,
                    "no live epochs to resume against (snapshot '" + snap_path.string() + "')");
            } else if (!NetworkStateSnapshotIO::warmResumeAllowed(
                           snap,
                           // CTU G5: a snapshot written without the CTU knobs is
                           // admitted under the stripped fingerprint (switch-on only).
                           (ctx.carry.live_fingerprint_ctu_off != 0
                            && snap.config_fingerprint == ctx.carry.live_fingerprint_ctu_off
                            && snap.config_fingerprint != live_fingerprint)
                               ? (ctx.carry.ctu_switch_on_resume = true,
                                  ctx.carry.live_fingerprint_ctu_off)
                               : live_fingerprint,
                           first_live_s, horizon_s, &gate, &gate_why)) {
                if (cold_ok)
                    log->warn("Network (RTB-7): state snapshot '{}' NOT warm-resumable — {} — "
                              "COLD START (a new generation id is minted at first emit as usual).",
                              snap_path.string(), gate);
                hb1(gate_why, "snapshot '" + snap_path.string() + "': " + gate);
            } else {
                // Emitter layer first (all or nothing): a failed emitter restore discards
                // the whole warm resume.  Restoring the driver carries without the
                // emitter's baselines and pending flags would emit wrong discontinuity
                // counters, a consumer datum break with no signal.
                std::istringstream es(snap.emitter_state);
                if (!emitter.restoreState(es)) {
                    if (cold_ok)
                        log->warn("Network (RTB-7): state snapshot '{}' passed the gate but its "
                                  "EMITTER section failed to restore (corrupt or parameter "
                                  "mismatch) — COLD START; driver state deliberately NOT "
                                  "restored (the two layers only make sense together).",
                                  snap_path.string());
                    hb1(WarmResumeRefusal::EmitterRestore,
                        "snapshot '" + snap_path.string() + "' passed the gate but its EMITTER "
                        "section failed to restore (corrupt, or written under different "
                        "emitter parameters)");
                } else {
                    // --- Driver layer -----------------------------------------
                    prev_carry.clear();
                    prev_carry.reserve(snap.shared_carry.size());
                    for (const auto& sc : snap.shared_carry) {
                        CarryPrior cp;
                        cp.kind      = static_cast<SharedParamKind>(sc.kind);
                        cp.sat       = sc.sat;
                        cp.signal    = sc.signal;
                        cp.sh_n      = sc.sh_n;
                        cp.sh_m      = sc.sh_m;
                        cp.sh_trig   = static_cast<char>(sc.sh_trig);
                        cp.value     = sc.value;
                        cp.var       = sc.var;
                        cp.inflate_q = sc.inflate_q;
                        // Dual-layer ionosphere (Iono-2): the second shell's coefficients
                        // use the sat field (otherwise empty for VtecSh) as the layer tag
                        // "L1"; an older reader fails the key match and reseeds from
                        // climatology, which is safe.
                        if (cp.kind == SharedParamKind::VtecSh && cp.sat == "L1") {
                            cp.sh_layer = 1;
                            cp.sat.clear();
                        }
                        prev_carry.push_back(std::move(cp));
                    }
                    // Dense carry (C1): the D records serialize per-key (value, var) only,
                    // so the carried block's cross-covariance comes from the QK/QP side-car
                    // or not at all.  When it is restored (RS-7, §18.12 (2a)) the first warm
                    // fold runs the dense path; otherwise it runs the legacy per-key
                    // diagonal and the dense block re-seeds at the first emit.
                    //
                    // This line is printed before the dense-border block below, so it does
                    // not claim "diagonal" on a leg that is about to restore the border
                    // (invariant #4 applies in both directions, and P9a, §21.14, reads this
                    // clause).  The predicate is the block's own arming test without the
                    // post-filter nk>0 / finite screen, which only the block can evaluate;
                    // if that screen fails, the block warns that it degraded to the
                    // diagonal, so the two lines stay consistent either way.
                    const bool border_restore_armed =
                        (ctx.knobs.resume_coupling_restore || ctx.knobs.resume_border_restore)
                        && fullcov_on;
                    const bool border_rows_present =
                        !snap.carry_cov_keys.empty()
                        && snap.carry_cov_rows.size() == snap.carry_cov_keys.size();
                    if (fullcov_on && !prev_carry.empty()) {
                        if (border_restore_armed && border_rows_present)
                            log->info("Network (C1): warm resume restored {} carries; the "
                                      "dense shared border is restored below from the "
                                      "snapshot's {} QK/QP record(s) (RS-7), so the first "
                                      "warm fold runs the full-covariance path.",
                                      prev_carry.size(), snap.carry_cov_keys.size());
                        else
                            log->info("Network (C1): warm resume restored {} diagonal carries — "
                                      "the full-covariance carry re-seeds at the first emitted "
                                      "epoch.", prev_carry.size());
                    }
                    net::recordFromSnapshot("V", snap, ctx);   // the constellation pivots (N4b)
                    net::recordFromSnapshot("C", snap, ctx);   // the satellites (N4e1)
                    net::recordFromSnapshot("J", snap, ctx);   // the satellites (N4e1)
                    net::recordFromSnapshot("K", snap, ctx);   // the satellites (N4e1)
                    // The arcs' leveling carries, with the restored-arc
                    // age convention (SPEC_event_harness §15.4, makeRestoredLocalCarry).
                    net::recordFromSnapshot("L", snap, ctx);
                    // The Z rows (the station table's zwd).  Same
                    // restored-entry convention as the L rows above.  The ZWD carry has no
                    // age reader and its harvest replaces the whole entry (so the sentinel
                    // does not survive the first stash), but a restored Z row is a restored
                    // entry and is marked as one, so an age gate added here later inherits
                    // §15.4.
                    net::recordFromSnapshot("Z", snap, ctx);
                    // Seam-scoped ZWD re-seed (SPEC_synthetic_minimum_network §17 finding
                    // 3): arm the post-resume inject window.  This is the re-coupling edge
                    // (invariant #7): the restored h-vintage ZWD posteriors reconnect to
                    // the solve for a bounded horizon while the station filters (not
                    // snapshotted, by design) rebuild their own memory; the retire at the
                    // end of the horizon is the decoupling edge, logged at the harvest.
                    // The window uses the ordinary B9 fold, so per-entry staleness
                    // (pl_max_gap_s) and the carry_warm gate still apply per epoch.
                    if (ctx.knobs.zwd_seam_on) {
                        if (net::present(sta_rows, &net::StationState::zwd) != 0) {
                            ctx.carry.zwd_seam_ep_left = ctx.knobs.zwd_seam_epochs;
                            // The window's effective q is its own (network.zwd_seam_reseed_q)
                            // when armed, else network.zwd_carry_q (RS-10b, §21.21).  The log
                            // line names it because it is the on-path telemetry for the
                            // window: the run-end counters show that the window folded, and
                            // this line shows how stiffly.
                            log->info("Network (ZWD seam re-seed): ARMED at the resume seam — {} "
                                      "restored Z row(s) will fold as B9-style priors for the next "
                                      "{} emitted epoch(s) (RW q {:.3e} m²/s over the true gap, "
                                      "{}), then retire.",
                                      net::present(sta_rows,
                                                   &net::StationState::zwd), ctx.knobs.zwd_seam_epochs,
                                      ctx.knobs.zwdQEff(/*seam_live=*/true),
                                      ctx.knobs.zwd_seam_q > 0.0
                                          ? "network.zwd_seam_reseed_q — the seam's own"
                                          : "inherited from network.zwd_carry_q");
                        } else {
                            log->warn("Network (ZWD seam re-seed): warm resume completed but the "
                                      "snapshot carried ZERO Z rows (written by a pre-capture "
                                      "build?) — the seam window is INERT this run; the ZWD "
                                      "restarts exactly as an unarmed resume.");
                        }
                    }
                    // Resume frame restore (network.resume_frame_restore, SPEC_synthetic_
                    // minimum_network §18.10), the re-coupling edge (invariant #7).  The
                    // realization-frame bookkeeping that the v1 snapshot omitted
                    // (per-(sat, signal) phase-anchor incumbents and R2 sticky pin targets,
                    // the Q3 clock-anchor per-satellite state with its t_last, so the stale
                    // test runs on the true gap, and the CLK-R held frame) reconnects to
                    // the solve before the first election, clock fold and emit.  Without it
                    // the election runs without incumbents for every family, marks them
                    // all, erases every restored leveling carry, marginalizes their D2 PB
                    // prior and re-pins the frame at fresh code seeds; every clock folds
                    // loose for the re-couple horizon; and the held frame restarts at 0.
                    // Identities are counted (invariant #4: 0 under an armed knob means
                    // inert).
                    if (ctx.knobs.resume_frame_restore) {
                        if (!snap.hasFrameState()) {
                            log->warn("Network (resume frame restore): ARMED but the snapshot "
                                      "carries NO frame rows (written by a pre-v2 build?) — "
                                      "INERT this run; the first election re-pins cold exactly "
                                      "as an unarmed resume.");
                        } else {
                            net::recordFromSnapshot("PA", snap, ctx);   // the satellite signals (N4d)
                            net::recordFromSnapshot("PT", snap, ctx);   // the satellite signals (N4d)
                            ctx.counters.n_resume_frame_anchors =
                                static_cast<long long>(snap.anchor_incumbent.size());
                            ctx.counters.n_resume_frame_targets =
                                static_cast<long long>(snap.anchor_pin_target.size());
                            ctx.carry.clk_anchor.sat.clear();
                            for (const auto& [sat, ca] : snap.clock_anchor) {
                                ClockAnchorSat st;
                                st.t_last       = ca.t_last;
                                st.last_value   = ca.last_value;
                                st.last_var_eff = ca.last_var_eff;
                                st.in_band      = ca.in_band;
                                st.anchored     = ca.anchored;
                                st.ever_anchored = ca.anchored;   // measure-only
                                ctx.carry.clk_anchor.sat[sat] = st;
                                ++ctx.counters.n_resume_frame_cta;
                                if (ca.anchored) ++ctx.counters.n_resume_frame_cta_anchored;
                            }
                            // Prime the two-edge handover ledger from the restored
                            // leveling rows (the judge graft), so a family whose
                            // restored incumbent does not track the satellite at the first
                            // live epoch takes the continuous handover path (the
                            // PhaseDatum.cpp re-validate gate on the restored
                            // conditional variance) instead of the cold pin, 1(d) mark and
                            // erase.  The first post-resume harvest rebuilds the ledger
                            // wholesale, so this is a one-epoch seed, the same lifetime as
                            // the ordinary ledger.  Done only when the continuous handover
                            // is armed (the map is not read otherwise).
                            std::size_t n_ledger_primed = 0;
                            if (ctx.knobs.pd_handover_continuous) {
                                net::forEach(ctx.entities.arcs, &net::ArcState::amb,
                                             [&](const ArcId& id, const LocalCarry& lc) {
                                    if (!(lc.var > 0.0) || !std::isfinite(lc.value_abs)) return;
                                    AnchorHandoverLedger::Entry e;
                                    e.value = lc.value_abs;
                                    e.var   = lc.var;
                                    e.var_first = lc.var_first;
                                    ctx.carry.handover.value_abs[net::arcPair(id)] = e;
                                    ++n_ledger_primed;
                                });
                            }
                            ctx.counters.n_resume_frame_ledger =
                                static_cast<long long>(n_ledger_primed);
                            if (clk_rebase && (!snap.clk_frame_acc.empty()
                                               || !snap.clk_frame_prev.empty()
                                               || snap.clk_frame_epochs > 0)) {
                                NetworkClockEmitRebase::FrameState fs;
                                fs.acc    = snap.clk_frame_acc;
                                fs.prev   = snap.clk_frame_prev;
                                fs.epochs = snap.clk_frame_epochs;
                                clk_rebase->restoreFrameState(fs);
                                ctx.counters.n_resume_frame_clk_keys =
                                    static_cast<long long>(snap.clk_frame_prev.size());
                            }
                            log->info("Network (resume frame restore): ARMED at the resume seam — "
                                      "{} phase-anchor incumbent(s), {} sticky pin target(s), {} "
                                      "clock-anchor sat state(s) ({} anchored), {} CLK-R held-frame "
                                      "key(s) (A_c over {} constellation(s), {} engine epoch(s)), "
                                      "handover ledger primed from {} restored leveling row(s), "
                                      "restored BEFORE the first election / clock fold / emit "
                                      "(re-coupling edge; the election keeps each incumbent and "
                                      "pins at its restored target, so no 1(d) re-reference mark "
                                      "and no carry erase fire at the seam).",
                                      ctx.counters.n_resume_frame_anchors,
                                      ctx.counters.n_resume_frame_targets,
                                      ctx.counters.n_resume_frame_cta,
                                      ctx.counters.n_resume_frame_cta_anchored,
                                      ctx.counters.n_resume_frame_clk_keys,
                                      snap.clk_frame_acc.size(), snap.clk_frame_epochs,
                                      n_ledger_primed);
                        }
                    }
                    // Datum restore (RS-3; network.resume_datum_restore, SPEC_event_harness
                    // §21.7).  The frame restore above brought back the satellite side of
                    // the eq34 tree (anchor_incumbent); this brings back the receiver side
                    // and the second, unrelated datum memory before anything that could
                    // run an election.  networkTryWarmResume runs as a whole before the
                    // first epoch, so the ordering holds by construction, and the loader
                    // guarantees resume_frame_restore is armed with it.
                    //
                    // Without it, the sticky-tree seed sees an empty eq34_rx_parent /
                    // eq34_sat_root, rebuilds the tree from the cold arc registry, and every
                    // phase-bias family re-realizes by the integer between the old and the
                    // new pinned arc (PHASE_DATUM/eq34_reref events at the first
                    // post-resume epoch and large per-satellite PB offsets).  The DD branch
                    // cannot prevent it, since a DD is invariant to the datum by
                    // construction; that is why the branch-faithful resume (RS-2) leaves
                    // the wire's realization untouched.
                    //
                    // The two halves are gated and reported separately: a file may carry
                    // one and not the other (a run with phase_datum_eq34 but no
                    // code_datum_time_linked writes no CB* rows, and vice versa), and an
                    // absent half reads as inert for that half alone, without borrowing the
                    // other's verdict (as hasFrameState/hasCouplingState do).
                    bool datum_cb_restored = false;
                    if (ctx.knobs.resume_datum_restore) {
                        const bool have_eq34 =
                            !snap.eq34_datum_rx.empty() || !snap.eq34_rx_parent.empty()
                            || !snap.eq34_sat_root.empty() || !snap.eq34_prev_target.empty()
                            || !snap.eq34_prev_el.empty()
                            || !snap.eq34_retained_edges.empty();
                        const bool have_cb =
                            !snap.code_bias_prev.empty() || !snap.code_bias_sat_seen.empty()
                            || !snap.code_bias_ifpin_pair.empty();
                        if (have_eq34) {
                            net::recordFromSnapshot("EQD", snap, ctx);   // the constellation channels (N4b)
                            net::recordFromSnapshot("EQP", snap, ctx);   // the station signals (N4a)
                            net::recordFromSnapshot("EQS", snap, ctx);   // the satellite signals (N4e2)
                            net::recordFromSnapshot("EQT", snap, ctx);   // the arcs (N4f1)
                            net::recordFromSnapshot("EQE", snap, ctx);   // the lines of sight (N4c)
                            net::recordFromSnapshot("EQR", snap, ctx);   // the arcs (N4f1)
                            ctx.counters.n_resume_datum_eq34_rx =
                                static_cast<long long>(snap.eq34_datum_rx.size());
                            ctx.counters.n_resume_datum_eq34_edges =
                                static_cast<long long>(snap.eq34_rx_parent.size());
                            ctx.counters.n_resume_datum_sat_roots =
                                static_cast<long long>(snap.eq34_sat_root.size());
                            ctx.counters.n_resume_datum_prev_targets =
                                static_cast<long long>(snap.eq34_prev_target.size());
                            ctx.counters.n_resume_datum_prev_el =
                                static_cast<long long>(snap.eq34_prev_el.size());
                            ctx.counters.n_resume_datum_retained =
                                static_cast<long long>(snap.eq34_retained_edges.size());
                        } else {
                            log->warn("Network (RS-3 datum restore): ARMED but the snapshot "
                                      "carries NO eq34 tree rows (a writer without "
                                      "network.state_snapshot_datum_capture, or a run with "
                                      "network.phase_datum_eq34 off) — that half is INERT; "
                                      "the first election rebuilds the spanning tree from "
                                      "the cold arc registry exactly as an unarmed resume "
                                      "does, and every phase-bias family re-references.");
                        }
                        if (have_cb) {
                            net::recordFromSnapshot("CBP", snap, ctx);   // the constellation channels (N4b)
                            net::recordFromSnapshot("CBS", snap, ctx);   // the satellites (N4e2)
                            // Restore also the IF-pin pair those values were solved under.
                            // Restoring the values without the pair is worse than restoring
                            // neither: `pair_same` would read false everywhere, so the IF-pin
                            // RHS would re-seed to 0, and the restored seen-set would make
                            // every one of those re-seeds bump.  A pair that did change
                            // across the seam still trips the test and bumps, because the
                            // resume's previous kept epoch is the writer's.
                            net::recordFromSnapshot("CBI", snap, ctx);   // the satellites (N4e2)
                            datum_cb_restored = true;
                            long long n_cbp = 0;
                            for (const auto& [fam, per_sat] : snap.code_bias_prev)
                                n_cbp += static_cast<long long>(per_sat.size());
                            ctx.counters.n_resume_datum_cb_prev  = n_cbp;
                            ctx.counters.n_resume_datum_cb_seen  =
                                static_cast<long long>(snap.code_bias_sat_seen.size());
                            ctx.counters.n_resume_datum_cb_pair  =
                                static_cast<long long>(snap.code_bias_ifpin_pair.size());
                        } else {
                            log->warn("Network (RS-3 datum restore): ARMED but the snapshot "
                                      "carries NO CBP/CBS/CBI rows (a writer without "
                                      "network.state_snapshot_datum_capture, or a run with "
                                      "network.code_datum_time_linked off) — that half is "
                                      "INERT; the run is marked 'resumed' below and every "
                                      "first post-resume code-bias re-seed disc-bumps.");
                        }
                        if (have_eq34 || have_cb)
                            log->info("Network (RS-3 datum restore): ARMED at the resume seam "
                                      "— eq34 tree: {} sticky datum receiver(s), {} receiver "
                                      "parent edge(s), {} satellite root(s), {} pin target(s), "
                                      "{} longevity elevation(s), {} gap-retained edge(s); "
                                      "code-bias datum: {} previous value(s) over {} family "
                                      "(families), {} ever-seen satellite(s), {} IF-pin "
                                      "pair(s).  Restored BEFORE "
                                      "the first election, so its sticky seed CONTINUES the "
                                      "writer's tree; the code-bias 'resumed' latch stays {} "
                                      "(a re-seed bumps only where it genuinely re-seeds).",
                                      ctx.counters.n_resume_datum_eq34_rx,
                                      ctx.counters.n_resume_datum_eq34_edges,
                                      ctx.counters.n_resume_datum_sat_roots,
                                      ctx.counters.n_resume_datum_prev_targets,
                                      ctx.counters.n_resume_datum_prev_el,
                                      ctx.counters.n_resume_datum_retained,
                                      ctx.counters.n_resume_datum_cb_prev,
                                      snap.code_bias_prev.size(),
                                      ctx.counters.n_resume_datum_cb_seen,
                                      ctx.counters.n_resume_datum_cb_pair,
                                      datum_cb_restored ? "FALSE" : "true (CB half inert)");
                    }
                    // Emit-layer restore (RS-5; network.resume_emit_restore,
                    // SPEC_event_harness §21.10).  It runs after the frame and datum
                    // restores and before anything that could emit; networkTryWarmResume
                    // runs as a whole before the first epoch, so the ordering holds by
                    // construction.  The loader guarantees resume_frame_restore is armed
                    // with it: an emit layer restored on top of a re-elected realization
                    // frame would re-base this run's biases onto a level the writer never
                    // emitted.
                    //
                    // Without it, the leg's emit layer starts at 0 on every key while the
                    // writer's carries its accumulated re-base, so the emitted phase biases
                    // step by the difference of the two emit layers at the seam, not by
                    // anything the estimator did (§21.10 has the measurement).
                    if (ctx.knobs.resume_emit_restore) {
                        const bool have_er = !snap.emit_rebase_last.empty();
                        if (have_er && emit_rebase) {
                            emit_rebase->restoreState(snap.emit_rebase_last);
                            ctx.counters.n_resume_emit_rebase_keys =
                                static_cast<long long>(snap.emit_rebase_last.size());
                        } else {
                            log->warn("Network (RS-5 emit restore): ARMED but {} — the "
                                      "eq34 emit RE-BASE half is INERT; the first "
                                      "post-resume epoch first-sights every key and emits "
                                      "the RAW solve-frame phase bias, exactly as an "
                                      "unarmed resume does, so the emitted level steps by "
                                      "the writer's whole accumulated re-base.",
                                      have_er
                                          ? "this run never constructs the re-base engine "
                                            "(network.carry_invariant / "
                                            "carry_invariant_emit_rebase off)"
                                          : "the snapshot carries NO ER rows (a writer "
                                            "without network.state_snapshot_emit_capture, "
                                            "or one with the re-base off)");
                        }
                        if (have_er && emit_rebase)
                            log->info("Network (RS-5 emit restore): ARMED at the resume "
                                      "seam — {} emission-frame key(s) restored into the "
                                      "eq34 emit re-base.  Restored BEFORE the first emit, "
                                      "so the first post-resume epoch re-bases onto the "
                                      "writer's last EMITTED values instead of passing the "
                                      "raw solve-frame value through; outputs-only, no "
                                      "estimate moves.",
                                      ctx.counters.n_resume_emit_rebase_keys);
                    }
                    // Station-layer restore (RS-6; network.resume_station_restore,
                    // SPEC_event_harness §21.13).  It runs after the emit restore and
                    // before the first epoch (networkTryWarmResume runs as a whole before
                    // it), and the loader guarantees resume_frame_restore is armed with it,
                    // because both memories are expressed in the realization frame under
                    // them.
                    //
                    // Without it, the whole station layer re-seeds from one epoch of code
                    // (the rx-CB inject gate then withholds more of its priors), and the
                    // deviation entries cold-seed at the first epoch, which produces the SH
                    // a₀₀ transient (§21.13).
                    //
                    // The two halves are gated and reported separately, as hasFrameState /
                    // hasDatumState / hasEmitState are: a file may carry RC rows and no SD
                    // (a writer with the rx-CB carry and no T5 deviation carry), and an
                    // absent half reads as inert for that half alone.  The two sub-switches
                    // are diagnostic: they let one §21.13 attribution leg restore a single
                    // half, and a withheld half is logged as withheld, not as absent.
                    // CTU G5: the level information of the writer's monitor, and
                    // the raise it arms for the first leg epoch.
                    if (ctx.knobs.clm_on && !snap.level_info.empty()) {
                        net::recordFromSnapshot("LB", snap, ctx);   // the satellites (N4e1)
                        ctx.carry.resume.clm_raise = ctx.knobs.clm_restore_raise;
                        log->info("Network (clock level restore): {} satellite(s) of contrast-frame level "
                                  "information restored from the snapshot's LB rows{}.",
                                  snap.level_info.size(),
                                  ctx.knobs.clm_restore_raise
                                      ? "; the level raise on restore is armed for the first "
                                        "leg epoch" : "");
                    } else if (ctx.knobs.clm_restore_raise) {
                        log->warn("Network (clock level restore): network.clock_level_restore_raise is "
                                  "armed but the snapshot carries no LB rows (the writer did not "
                                  "run network.clock_level_monitor): nothing to raise to, INERT.");
                    }
                    if (ctx.knobs.resume_station_restore) {
                        const bool have_rc = !snap.rx_cb_carry.empty();
                        const bool have_sd = !snap.sh_dev_carry.empty();
                        if (have_rc && ctx.knobs.resume_station_rxcb) {
                            // Restored-arc age (§15.4.1), as for the L / Z rows: the entry
                            // goes back through makeRestoredLocalCarry rather than a bare
                            // aggregate, so a restored rx-CB entry is distinguishable from a
                            // never-stashed one at any reader that adds an age test.  The
                            // rx-CB harvest stashes a bare LocalCarry and its inject reads
                            // only value_abs / var / last_seen_s, so the sentinel has no
                            // effect on any current reader; it keeps the convention for a
                            // later age gate.
                            net::recordFromSnapshot("RC", snap, ctx);   // the station signals (N4a)
                            ctx.counters.n_resume_station_rxcb =
                                static_cast<long long>(net::present(
                                    ctx.entities.stations, &net::StationState::signals,
                                    &net::StationSignalState::rx_cb));
                        } else {
                            log->warn("Network (RS-6 station restore): ARMED but {} — the "
                                      "rx-CB half is INERT; every receiver code-bias prior "
                                      "first-sights at the seam and the harvest re-seeds "
                                      "it from ONE epoch of code, exactly as an unarmed "
                                      "resume does, so the station layer re-splits code "
                                      "bias / iono / clock across the whole network.",
                                      have_rc
                                          ? "network.resume_station_restore_rxcb is false "
                                            "(a §21.13 attribution leg)"
                                          : "the snapshot carries NO RC rows (a writer "
                                            "without network.state_snapshot_station_"
                                            "capture, or one without the rx-CB carry)");
                        }
                        if (have_sd) {
                            // Restored as written, without a sentinel: ShDevCarry's own cold
                            // test is `t_s <= 0`, so keeping the writer's harvest stamp makes
                            // the entry read warm, and the epoch's aging pre-pass then ages it
                            // over the true gap (and drops it at 5·tau, the staleness rule).
                            net::recordFromSnapshot("SD", snap, ctx);   // the lines of sight (N4c, O3)
                            ctx.counters.n_resume_station_shdev =
                                static_cast<long long>(net::present(ctx.entities.los,
                                                                    &net::LosState::sh_dev));
                        } else {
                            log->warn("Network (RS-6 station restore): ARMED but {} — the "
                                      "T5 deviation half is INERT; every per-LOS entry "
                                      "cold-seeds at the steady state (dev 0, var "
                                      "representation_sigma²) at the seam, which is the "
                                      "SH a₀₀ transient.",
                                      "the snapshot carries NO SD rows (a writer "
                                      "without network.state_snapshot_station_"
                                      "capture, or one without the T5 deviation "
                                      "carry)");
                        }
                        if (ctx.counters.n_resume_station_rxcb > 0
                            || ctx.counters.n_resume_station_shdev > 0)
                            log->info("Network (RS-6 station restore): ARMED at the resume "
                                      "seam — {} rx-CB carry entr{} and {} T5 per-LOS "
                                      "deviation entr{} restored BEFORE the first epoch, so "
                                      "the receiver code-bias priors and the SH "
                                      "representation-error levels continue the writer's "
                                      "instead of re-seeding from one epoch of code.",
                                      ctx.counters.n_resume_station_rxcb,
                                      ctx.counters.n_resume_station_rxcb == 1 ? "y" : "ies",
                                      ctx.counters.n_resume_station_shdev,
                                      ctx.counters.n_resume_station_shdev == 1 ? "y" : "ies");
                    }
                    // Canonical-signal restore (RS-8; network.resume_signal_canon_restore,
                    // SPEC_event_harness §21.16).  The rows are staged here and applied by
                    // networkRestoreCanonicalSignals, which the driver calls immediately
                    // after this function returns.  The latch lives in each station's
                    // PppModel and this seam has no roster; adding one to reach the models
                    // would make the resume seam depend on the whole station bundle for one
                    // map, so the rows are parked in ctx.carry and the next call drains them
                    // (an empty stash there means this block did not run or the snapshot
                    // carried nothing).
                    //
                    // Without it, the resume epoch's satellite counts re-decide the latch,
                    // which can switch a station between L2X and L2W; the L2X tracker set
                    // then changes, the eq34 L2X component loses its datum receiver, and
                    // the L2X phase-bias families step at the seam (§21.16).
                    if (ctx.knobs.resume_signal_canon_restore) {
                        net::dropAll(sta_rows, &net::StationState::resume_canon_rows);
                        long long staged = 0;
                        for (const auto& [idx, rows] : snap.canonical_signals) {
                            auto& dst = net::put(sta_rows, StationId{idx},
                                                 &net::StationState::resume_canon_rows);
                            for (const auto& c : rows) {
                                dst.push_back(PppModel::CanonicalSignal{
                                    c.constellation, c.band, c.obs_type, c.signal});
                                ++staged;
                            }
                        }
                        if (staged == 0)
                            log->warn("Network (RS-8 canonical restore): ARMED but the "
                                      "snapshot carries NO CN rows (a writer without "
                                      "network.state_snapshot_signal_capture) — INERT; "
                                      "every station re-DECIDES its canonical per-band "
                                      "signal from THIS epoch's satellite counts, exactly "
                                      "as an unarmed resume does, and a band whose two "
                                      "channels are within a satellite or two of each "
                                      "other can flip silently.");
                        else
                            log->info("Network (RS-8 canonical restore): staged {} "
                                      "canonical entr{} across {} station(s) from the "
                                      "snapshot; applied to the station models before the "
                                      "first epoch.",
                                      staged, staged == 1 ? "y" : "ies",
                                      net::present(ctx.entities.stations,
                                                   &net::StationState::resume_canon_rows));
                    }
                    // Code-bias linearization-point restore (RS-9;
                    // network.resume_rxcb_lin_restore, SPEC_event_harness §21.18): the
                    // other edge of the seam that RS-6 above opens.  RS-6 restores the
                    // rx-CB carry's numerator; the gate that decides whether it is ever
                    // folded tests it against x_lin(cb), a one-shot seed that this leg
                    // would re-derive from the resume epoch's code.  Without this restore,
                    // a station whose seed lands outside the gate's fixed bar is refused on
                    // every epoch of the leg and keeps no receiver-DCB memory, while the
                    // run-end line still counts it as carried (§21.18(a)/(b)).  This is
                    // invariant #7: RS-6 re-validates the value it reconnects, not the
                    // frame it is judged against.
                    //
                    // The rows are only stashed here.  x_lin lives in each station's
                    // parameter set, and the one-shot latch that freezes it does not close
                    // until that station's first networkBirthLocalStates, so a separate
                    // seam (networkApplyRxCbLinRestore), called from runNetwork right after
                    // this function where the station roster is in scope, consumes them.
                    // The loader requires resume_station_restore and its rxcb half, so an
                    // armed knob here always sits under a restored carry.
                    if (ctx.knobs.resume_rxcb_lin_restore) {
                        if (!snap.rx_cb_lin.empty()) {
                            net::dropAll(sta_rows, &net::StationState::rxcb_lin_restore);
                            for (const auto& [key, xl] : snap.rx_cb_lin)
                                net::put(sta_rows, StationId{key.first},
                                         &net::StationState::rxcb_lin_restore)[key.second] = xl;
                            ctx.counters.n_resume_rxcb_lin_stations =
                                static_cast<long long>(
                                    net::present(ctx.entities.stations,
                                                 &net::StationState::rxcb_lin_restore));
                            ctx.counters.n_resume_rxcb_lin_entries =
                                static_cast<long long>(snap.rx_cb_lin.size());
                            log->info("Network (RS-9 rx-CB frame restore): ARMED at the "
                                      "resume seam — {} code-bias linearization point(s) "
                                      "across {} station(s) stashed from the snapshot's RL "
                                      "rows; each station's first-epoch code-bias seed will "
                                      "take the writer's x_lin instead of a median over ONE "
                                      "epoch of code, so the rx-CB inject gate differences "
                                      "the restored carry against the writer's frame.",
                                      ctx.counters.n_resume_rxcb_lin_entries,
                                      ctx.counters.n_resume_rxcb_lin_stations);
                        } else {
                            log->warn("Network (RS-9 rx-CB frame restore): ARMED but the "
                                      "snapshot carries NO RL rows (a writer without "
                                      "network.state_snapshot_rxcb_lin_capture) — INERT; "
                                      "every station re-derives its code-bias "
                                      "linearization point from the RESUME epoch's code, "
                                      "so the rx-CB gate judges the RESTORED carry against "
                                      "a frame the writer never held and can refuse a "
                                      "station for the whole leg (§21.18).");
                        }
                    }
                    // Coupling regressors (network.resume_coupling_restore): the second half
                    // of the frame restore, and the re-coupling edge for the leveling carry
                    // itself.
                    //
                    // Without it a restored LocalCarry has an empty `coupling`, so
                    // buildCplRow's per-entry loop does not run: row.shared is empty,
                    // rhs = value_abs − x_lin, σ² = var_eff.  That is a plain diagonal prior
                    // at the pure conditional variance, which no continuous epoch produces:
                    // the diagonal carry is stiff at the marginal variance, while the
                    // coupling row is stiff at the conditional one but shared-neutral, its
                    // A_s = M_r cancelling the reduced-normals contribution.  In the Schur
                    // reduction (A_r = e_c, A_s = M, weight w, P = N_rr⁻¹, n_c = e_cᵀPe_c,
                    // M_d = N_sr P e_c, α = w/(1+w·n_c)) the shared normals gain
                    // α(M − M_d)(M − M_d)ᵀ.  An empty coupling is the M = 0 case, which
                    // drives the border by the full α‖M_d‖ at α ≈ 1e6 m⁻² per row, four
                    // orders of magnitude above the anchored clock prior's ≈ 1.1e4 m⁻²
                    // (the source of the first-fold trips in §18.11).
                    //
                    // The restore changes the row's direction only; `var` is never
                    // modified.  Changing the row's weight instead lets the level re-float
                    // on the cold station layer and walks the consumer (invariant #1, the
                    // slip-relevel-walk family).
                    //
                    // Each row is all or nothing, guaranteed upstream: the parser refuses
                    // any file with incomplete, out-of-range, non-finite or orphaned rows,
                    // so a row reaching here is bit for bit the write epoch's harvest, in
                    // harvest order.  A row with no QL/QZ record keeps its empty coupling
                    // and degrades to the diagonal.  Coefficients are not re-sparsified and
                    // ŝ_prev / P_prev are not re-derived: P_prev = 0 is the meaningful
                    // "datum-pinned, exactly known" value, and re-deriving it would activate
                    // the κ shrink on a column the datum re-applies exactly.
                    //
                    // This block sits outside the resume_frame_restore block on purpose: it
                    // has its own knob, so frame on / coupling off stays byte-identical to
                    // the old path in the new binary, the same-build control that
                    // invariant #6 requires.  hasFrameState() is also the wrong inert
                    // predicate here, and the frame block's ledger sub-block carries a third
                    // gate that this block must not inherit.
                    if (ctx.knobs.resume_coupling_restore) {
                        // Gate on the population the restore rebuilds, not on
                        // hasCouplingState() (which is also true for a QR- or QS-only file):
                        // otherwise a file with no QL/QZ takes the else branch and logs
                        // "ARMED ... 0 L row(s) + 0 Z row(s)" while seeding tu_reinit_hist,
                        // an inert path reported as armed (invariant #4).
                        if (snap.coupling_rows.empty() && snap.coupling_zrows.empty()) {
                            log->warn("Network (resume coupling restore): ARMED but the "
                                      "snapshot carries NO QS/QL records (a writer without "
                                      "network.state_snapshot_coupling_capture, or a pre-v3 "
                                      "file) — INERT this run; every restored leveling row "
                                      "injects as a plain DIAGONAL prior, which IS the "
                                      "§18.11 defect (33 of 52 anchored clocks refused).");
                        } else {
                            const auto rebuild =
                                [&](const std::vector<std::pair<int, double>>& ents,
                                    LocalCarry& lc) {
                                    std::vector<CoupledShared> cpl;
                                    cpl.reserve(ents.size());
                                    for (const auto& [id, mj] : ents) {
                                        const auto& q = snap.coupling_dict[
                                            static_cast<std::size_t>(id)];
                                        CoupledShared cs;
                                        cs.kind     = static_cast<SharedParamKind>(q.kind);
                                        cs.sat      = q.sat;
                                        cs.signal   = q.signal;
                                        cs.n        = q.sh_n;
                                        cs.m        = q.sh_m;
                                        cs.trig     = static_cast<char>(q.sh_trig);
                                        cs.sh_layer = q.sh_layer;   // a layer-1 SH coeff
                                        cs.coeff    = mj;           // must not resolve to
                                        cs.s_prev   = q.s_prev;     // the layer-0 column
                                        cs.p_prev   = q.p_prev;
                                        cpl.push_back(std::move(cs));
                                    }
                                    lc.coupling = std::move(cpl);
                                };
                            long long n_rows = 0, n_zrows = 0, n_terms = 0, n_miss = 0;
                            for (const auto& [key, ents] : snap.coupling_rows) {
                                const auto it = net::getArc(ctx.entities.arcs, key, &net::ArcState::amb);
                                if (it == nullptr) { ++n_miss; continue; }
                                rebuild(ents, (*it));
                                ++n_rows;
                                n_terms += static_cast<long long>(ents.size());
                            }
                            for (const auto& [idx, ents] : snap.coupling_zrows) {
                                LocalCarry* zc = net::get(sta_rows, StationId{idx},
                                                          &net::StationState::zwd);
                                if (zc == nullptr) { ++n_miss; continue; }
                                rebuild(ents, *zc);
                                ++n_zrows;
                                n_terms += static_cast<long long>(ents.size());
                            }
                            ctx.counters.n_resume_cpl_dict =
                                static_cast<long long>(snap.coupling_dict.size());
                            ctx.counters.n_resume_cpl_rows    = n_rows;
                            ctx.counters.n_resume_cpl_zrows   = n_zrows;
                            ctx.counters.n_resume_cpl_terms   = n_terms;
                            ctx.counters.n_resume_cpl_orphan  = n_miss;
                            ctx.carry.resume.cpl_census = (n_rows + n_zrows) > 0;

                            // ---- The decoupling edge that the snapshot cannot otherwise
                            // reach (standing invariant #7).  ctx.carry.tu_reinit_hist is
                            // per-epoch state and never serialized, so on the first warm
                            // epoch no restored term would be diverted for a column that was
                            // being re-drawn at the write epoch.  It is seeded from the three
                            // sources the snapshot does carry, at a stamp strictly above
                            // every restored last_seen_s, so tuReinitSince fires for exactly
                            // the restored population.  The term then takes the departed
                            // route (κ = 0, M²·P_prev marginalized into σ², coefficient handed
                            // to the gauge restore) instead of folding a re-drawn value at
                            // full weight.
                            //
                            // Not seeded, on purpose: a pivot change, a sticky re-pin, a
                            // Σz=0 re-realization, or a per-epoch set-mean code-bias datum
                            // step.  Those are pure gauge motions, and the identity
                            // Mᵀg_s = −g_r(c) makes a complete row exactly invariant under
                            // them: the solution moves as (r + αg_r, s + αg_s) and the row
                            // residual is unchanged.  Diverting them wholesale would inflate
                            // σ² on about 40 % of every row's terms with no gain in
                            // correctness.  carry_coupling_gauge_project, which the loader
                            // requires here, repairs the family sum for whatever does
                            // truncate.
                            //
                            // The fourth source, prev_carry.inflate_q, needs no seed: it is
                            // restored on the D rows, and the first warm epoch's serial
                            // pre-pass writes it at pl_now_s, above every restored stamp.
                            double t_seed = 0.0;
                            net::forEach(ctx.entities.arcs, &net::ArcState::amb,
                                         [&](const ArcId&, const LocalCarry& lc) {
                                             t_seed = std::max(t_seed, lc.last_seen_s);
                                         });
                            net::forEach(sta_rows, &net::StationState::zwd,
                                         [&](const StationId&, const LocalCarry& lc) {
                                             t_seed = std::max(t_seed, lc.last_seen_s);
                                         });
                            t_seed = std::nextafter(t_seed,
                                         std::numeric_limits<double>::infinity());
                            long long n_seed = 0, n_loose = 0;
                            const auto seedClock = [&](const std::string& sat) {
                                net::putSig(entities, sat, "", &net::SatSignalState::tu_reinit)
                                    [SharedParamKind::Clock] = t_seed;   // the satellite-wide wildcard
                                ++n_seed;
                            };
                            for (const auto& [sat, ca] : snap.clock_anchor)
                                if (!ca.anchored) { seedClock(sat); ++n_loose; }
                            for (const std::string& sat : snap.eph_cutover_disc_pending)
                                seedClock(sat);
                            // QR: a hard-refloated satellite is skip-stashed, so restoring the
                            // set is the only way the first warm epoch's pre-pass (the
                            // prev_clock_refloat_sats loop in ApplicationNetworkAssembly.cpp)
                            // behaves as a continuous epoch's would.
                            net::recordFromSnapshot("QR", snap, ctx);   // the satellites (N4e2)
                            ctx.counters.n_resume_cpl_seed_reinit = n_seed;

                            log->info("Network (resume coupling restore): ARMED at the resume "
                                      "seam — {} L row(s) + {} Z row(s) rebuilt their coupling "
                                      "regression from {} term(s) over a {}-column dictionary "
                                      "({} row(s) had no matching carry); {} clock reinit "
                                      "seed(s) planted ({} LOOSE anchor(s), {} pending eph "
                                      "cutover(s)) and {} write-epoch refloat(s) restored, so a "
                                      "column that was being RE-DRAWN at the seam takes the "
                                      "departed route.  The first PASS-B inject now builds a "
                                      "CONDITIONAL row (A_s = M_r, y = (value_abs − x_lin) + "
                                      "Σ M_j·ŝ_prev,j at the pure conditional variance) instead "
                                      "of a plain diagonal.",
                                      n_rows, n_zrows, n_terms, snap.coupling_dict.size(),
                                      n_miss, n_seed, n_loose,
                                      snap.eph_cutover_disc_pending.size(),
                                      snap.clock_refloat_sats.size());
                        }
                    }
                    // Dense D2 covariance side-car (§18.12 (2a)).  Two read knobs reach it
                    // (RS-7, §21.14).  Under resume_coupling_restore it is the other half of
                    // the coupling seam: the coupling rows are conditional from the first
                    // warm fold, so the border they condition on has to fold dense from the
                    // first fold too, or the diagonal asymmetry measured in §18.12 remains.
                    // Under resume_border_restore it stands alone: a covariance block carries
                    // no regression and no gauge identity, so it needs neither
                    // carry_conditional_coupling nor the gauge projection, which makes it usable on a
                    // carry_invariant field lineage where the loader refuses
                    // resume_coupling_restore.  Keys with no restored D row are dropped (row
                    // and column) and counted; the fold still degrades per key as before.
                    //
                    // Invariant #4: armed with no usable QK/QP records (a writer without
                    // network.state_snapshot_coupling_capture, or a truncated block), the
                    // knob is inert and logs that it is, because a border that restarts
                    // diagonal without a message is the defect the restore closes.
                    if (ctx.knobs.resume_border_restore && fullcov_on && !border_rows_present)
                        log->warn("Network (RS-7 border restore): ARMED but the snapshot "
                                  "carries NO usable QK/QP records ({} key(s), {} row(s)) — "
                                  "a writer without network.state_snapshot_coupling_capture, "
                                  "or a truncated block.  INERT this run: the dense shared "
                                  "border re-seeds from the first emitted epoch exactly as "
                                  "before, which IS the §21.14 defect.",
                                  snap.carry_cov_keys.size(), snap.carry_cov_rows.size());
                    if (border_restore_armed && border_rows_present) {
                        // Name the knob that armed the restore: on a field leg the loader
                        // refuses resume_coupling_restore, so a line reading "resume
                        // coupling restore" would point at a knob the config does not carry.
                        const char* const border_knob =
                            ctx.knobs.resume_border_restore ? "RS-7 border restore"
                                                            : "resume coupling restore";
                        std::set<std::string> live_keys;
                        for (const CarryPrior& cp : prev_carry)
                            if (cp.kind != SharedParamKind::VtecSh)
                                live_keys.insert(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                        std::vector<std::size_t> keep;   // snapshot row index of each kept key
                        keep.reserve(snap.carry_cov_keys.size());
                        long long n_drop_cov = 0;
                        for (std::size_t i = 0; i < snap.carry_cov_keys.size(); ++i) {
                            if (live_keys.count(snap.carry_cov_keys[i])) keep.push_back(i);
                            else ++n_drop_cov;
                        }
                        const Eigen::Index nk = static_cast<Eigen::Index>(keep.size());
                        SharedCarryCov cov;
                        cov.P = Eigen::MatrixXd::Zero(nk, nk);
                        bool finite = true;
                        for (Eigen::Index a = 0; a < nk; ++a) {
                            const std::size_t ia = keep[static_cast<std::size_t>(a)];
                            cov.row.emplace(snap.carry_cov_keys[ia], static_cast<std::size_t>(a));
                            for (Eigen::Index b = 0; b <= a; ++b) {
                                const std::size_t ib = keep[static_cast<std::size_t>(b)];
                                const double v = snap.carry_cov_rows[ia][ib];   // ib <= ia: lower triangle
                                if (!std::isfinite(v)) finite = false;
                                cov.P(a, b) = v;
                                cov.P(b, a) = v;
                            }
                        }
                        if (nk > 0 && finite) {
                            ctx.carry.prev_carry_cov = std::move(cov);
                            // CTU G5: the exact update's stash stamp is the snapshot's
                            // own epoch (the coupling rows restored with it were harvested
                            // there), and the clock pivot is the snapshot's (pinned at 0 in
                            // its datum).  Without this the first leg epoch has no valid
                            // stash and every row takes the time update's kappa.
                            if (ctx.knobs.carry_cpl_exact) {
                                ctx.carry.cxu_stash_s =
                                    static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
                                ctx.carry.cxu_stash_valid = ctx.carry.prev_carry_cov.P.rows() > 0;
                                net::dropAll(entities.constellations,
                                             &net::ConstellationState::cxu_pivot);
                                for (const auto& [c, sat] : snap.prev_pivot)
                                    if (!sat.empty())
                                        net::put(entities.constellations, systemOfSat(sat),
                                                 &net::ConstellationState::cxu_pivot) = {sat, 0.0};
                                ++ctx.counters.cxu_resume_stash;
                            }
                            // The restored block's frame is the snapshot's pivots.
                            if (ctx.knobs.pfq_on) {
                                net::dropAll(entities.constellations,
                                             &net::ConstellationState::pfq_pivot);
                                for (const auto& [c, sat] : snap.prev_pivot)
                                    if (!sat.empty())
                                        net::put(entities.constellations, systemOfSat(sat),
                                                 &net::ConstellationState::pfq_pivot) = sat;
                            }
                            ctx.counters.n_resume_cpl_cov_keys    = static_cast<long long>(nk);
                            ctx.counters.n_resume_cpl_cov_dropped = n_drop_cov;
                            log->info("Network ({}, dense border): {} dense-carry key(s) "
                                      "restored from the snapshot's QK/QP records ({} "
                                      "dropped: no restored D row) — the first warm fold "
                                      "runs the C1 DENSE path instead of the diagonal "
                                      "asymmetry.",
                                      border_knob, nk, n_drop_cov);
                        } else {
                            log->warn("Network ({}, dense border): QK/QP records present but "
                                      "unusable ({} usable key(s), finite={}) — the first "
                                      "warm fold degrades to the diagonal path.",
                                      border_knob, nk, finite);
                        }
                    }
                    // First-fold census (§21.14, invariant #4): n_resume_cpl_diag_border,
                    // the count of carried keys that missed prev_carry_cov.row and folded
                    // through the legacy per-key diagonal path, is the one instrument that
                    // shows whether the border came back dense.  It is latched for every
                    // warm resume with a dense carry, not only inside the coupling restore,
                    // so that the control legs of a border A/B read the full diagonal count
                    // and the border legs read about 0 (plus whatever the drop screen
                    // rejected).  Trace-class: everything the latch gates is a counter or a
                    // measure-only census vector, so products stay byte-identical.
                    if (fullcov_on && !prev_carry.empty()) {
                        ctx.carry.resume.cpl_census = true;
                        ctx.counters.n_resume_dense_carry_keys =
                            static_cast<long long>(prev_carry.size());
                    }
                    // Resume slip warm-up (network.resume_slip_warmup_ep, §18.10): the
                    // MW/LLI drain is diagnostic-only for K post-resume epochs (its cold
                    // reference fires en masse at the second warm epoch); T1 / wind-up
                    // drains stay live.  Countdown decremented at the PASS-A dispatch.
                    if (ctx.knobs.resume_slip_warmup_ep > 0) {
                        ctx.carry.resume_slip_warmup_left = ctx.knobs.resume_slip_warmup_ep;
                        log->info("Network (resume slip warm-up): ARMED — the MW/LLI (T0) drain "
                                  "acts diagnostic-only for the next {} processed epoch(s); "
                                  "IF-TD (T1) and wind-up drains stay live.",
                                  ctx.knobs.resume_slip_warmup_ep);
                    }
                    // The same window one layer up, on the estimation-side fine-slip gate
                    // (RS-2b, §21.6).  It is armed here and nowhere else: a cold start never
                    // sets it, since the gate would otherwise be diagnostic-only on a run
                    // with no restored branch to protect.  The countdown is decremented at
                    // the install seam.
                    if (ctx.knobs.resume_d5_warmup_ep > 0) {
                        ctx.carry.resume_d5_warmup_left = ctx.knobs.resume_d5_warmup_ep;
                        log->info("Network (RS-2b resume D5 warm-up): ARMED — the invariant-carry D5 "
                                  "innovation gate acts diagnostic-only for the next {} "
                                  "processed epoch(s): trips are counted, never re-tapped and "
                                  "never marked slipped.  The σ̂₀ feed, the v3 corroboration "
                                  "gate and the MW/LLI/IF-TD drains are untouched.",
                                  ctx.knobs.resume_d5_warmup_ep);
                    }
                    ura_sigma0_ewma     = snap.ura_sigma0_ewma;
                    net::recordFromSnapshot("Y", snap, ctx);   // the satellites (N4e1)
                    net::recordFromSnapshot("R", snap, ctx);   // the satellites (N4e1)
                    net::recordFromSnapshot("A", snap, ctx);   // the satellites (N4e1)
                    // Restore the eph-cutover re-level baseline and any pending forced
                    // disc, so a warm resume does not re-level against a stale baseline (or
                    // lose a pending forced bump).  Both are empty in an sp3 snapshot or one
                    // written before the ephemeris-cutover re-level, and the first brdc_rts epoch then re-seeds.
                    net::recordFromSnapshot("I", snap, ctx);   // the satellites (N4e2)
                    net::recordFromSnapshot("X", snap, ctx);   // the satellites (N4e2)
                    // Without the datum restore, the time-linked code-bias previous-value
                    // map re-seeds empty here.  A satellite emitted before the snapshot
                    // whose split re-seeds is a re-reference the consumer has to see, so the
                    // run is marked "resumed" and every first post-resume re-seed disc-bumps
                    // (conservative: a new satellite's first emit absorbs a harmless bump).
                    // Inert unless code_datum_time_linked.
                    //
                    // When the datum restore above did bring the map back (RS-3, §21.7), the
                    // datum continues across the seam and a satellite whose split is
                    // unchanged has no step to signal, so the blanket latch would create
                    // consumer-visible discontinuities that the wire does not have.  The
                    // per-satellite seen-set restored with the map keeps the distinction the
                    // latch approximated: a satellite that does re-seed is still a return
                    // and still bumps.
                    code_bias_datum_resumed_ = !datum_cb_restored;
                    // Telemetry latch (RS-3): measure the first post-resume epoch's datum
                    // churn (eq34 rerefs and reattaches, code-bias re-seed discs) on every
                    // warm resume, armed or not, so an A/B's control leg is read by the same
                    // instrument as its treatment.  It only feeds counters and nothing
                    // downstream branches on it, so the off path stays byte-identical (like
                    // anchor_age_resume_seed and resume.adm).
                    ctx.carry.resume_datum_first_ep = true;
                    // The pin pair hold baseline is not snapshotted, so the first
                    // kept epoch's holds are declared as found at a resume, not as begun.
                    // Telemetry only (empty hold maps unless code_datum_pair_continuity).
                    ctx.carry.resume.if_pin_holds = true;
                    // One-shot latch of the declaration half (RS-4, §21.9), set beside the
                    // RS-3 telemetry latch and for the same reason: every warm resume arms
                    // it, so an A/B's control leg is read by the same instrument.
                    // NetworkEmitStore consumes it at the first emit, the only point at
                    // which the emitter's restored per-key baselines still hold the previous
                    // run's emitted values.  Read only under the knob.
                    ctx.carry.resume.pb_seam = true;
                    // eq34 reattach hygiene (spec §2, INC-1 warm-resume rule): the arc-age
                    // map is not serialized, so without this latch every arc would look one
                    // epoch old on the first resumed epoch, and the eligibility gate would
                    // block every reattach for min_arc_ep epochs.  Arcs behind a snapshot
                    // are mature by definition, so the first hygiene pass seeds them at the
                    // threshold instead of 1.  Inert unless ctx.knobs.anchor_hygiene_on.
                    ctx.carry.anchor_age_resume_seed = true;
                    // Flicker/gap signal integrity (spec addendum): a warm resume is the
                    // network-wide form of the flicker case.  The snapshot restores tight
                    // carries but no detector state (MW Welford counts and IF-TD arc history
                    // are not deserialized and re-converge from scratch), so for the first
                    // revet_ep epochs every arc in the network carries fully warm state with
                    // no time-difference coverage, the same hazard as a flicker.  Latched as
                    // a countdown, so the classifier can decrement it without knowing about
                    // resumes.  Inert unless ctx.knobs.sig_resume_warmup.
                    ctx.carry.si_resume_warmup_ep = ctx.knobs.sig_resume_warmup
                                                        ? ctx.knobs.sig_revet_ep : 0;
                    // Product-referenced measurement admission (spec §3, INC-1 second
                    // clause): the admission-record map is not serialized, so on the first
                    // post-resume screen pass every channel looks new, while its estimator
                    // state was restored and may carry the corrupt seed the screen exists
                    // to catch.  A restored corrupt arc is not a birth: it is evented as a
                    // restore and refloated (a birth has nothing to refloat; a restore has a
                    // stiff carried chord that would otherwise span the hole the withhold
                    // opens, standing invariant #1).  The census's INC-3 cannot see
                    // this case by construction, which is why the spec names it.  Written
                    // unconditionally (inert unless ctx.knobs.adm_screen), so the off path
                    // stays byte-identical, as for anchor_age_resume_seed.
                    ctx.carry.resume.adm = true;
                    // Preserve the snapshot's generation id (RTA-3 seam).  The first-emit
                    // minting site mints only when generation_id == 0, so a restored
                    // non-zero id suppresses the mint, and consumers keep the same product
                    // datum without a hard reset.  emit_gen_id is in the fingerprint, so an
                    // id-less snapshot warm-resumes only an id-less run, where 0 is correct.
                    generation_id = snap.generation_id;
                    iod           = snap.iod;
                    prev_week     = snap.prev_week;
                    prev_tow      = snap.prev_tow;
                    n_emitted           = static_cast<size_t>(snap.n_emitted);
                    n_skipped           = static_cast<size_t>(snap.n_skipped);
                    n_gate_withheld     = static_cast<size_t>(snap.n_gate_withheld);
                    n_sigma0_withheld   = static_cast<size_t>(snap.n_sigma0_withheld);
                    n_runaway_epochs    = static_cast<size_t>(snap.n_runaway_epochs);
                    n_perconst_partial_epochs    = static_cast<size_t>(snap.n_perconst_partial);
                    n_perconst_diffuse_published = static_cast<size_t>(snap.n_perconst_diffuse);
                    last_emit_content_s = snap.last_emit_content_s;
                    last_health_write_s = snap.last_health_write_s;
                    // The ramp state is restored, so a warm-resumed producer is not degraded
                    // (RTB-7(b)).  Already lifted ⇒ grade 0 ("warm"); mid-ramp ⇒ the ramp
                    // continues from the restored counts (it does not restart), with the
                    // grade anchored at the original first emit, so the reported time to
                    // grade covers the whole product lifetime.
                    ura_ewma_updates     = static_cast<size_t>(snap.ura_ewma_updates);
                    ramp_lifted          = snap.ramp_lifted;
                    first_emit_content_s = snap.first_emit_content_s;
                    if (nc.warmup_ura_floor_m > 0.0 && ura_on && ramp_lifted) {
                        grade_metric_s = 0.0;
                        log->info("Network (RTB-7): cold-start-to-product-grade: 0 s (warm "
                                  "resume — ramp already lifted before the bounce).");
                    }
                    // Branch-faithful resume (RTB-7(c)): restore the invariant DD carry (the
                    // z-store branch) and the ArcKey vertex numbering it references, only
                    // when armed and the blob is present.  A failure cold-starts the carry
                    // (shared and emitter state stay warm), so the resume degrades to
                    // RTB-7(a).
                    if (nc.state_snapshot_carry && !snap.carry_state.empty()) {
                        net::recordFromSnapshot("SV", snap, ctx);   // the satellites (N4e2)
                        net::recordFromSnapshot("GG", snap, ctx);   // the constellation channels (N4b)
                        std::istringstream cs(snap.carry_state);
                        std::string cerr;
                        bool carry_ok = carry_driver.restoreCarry(cs, &cerr);
                        if (carry_ok) {
                            // The blob is numbered in the writing run's id space, so it is
                            // rejected unless every chord and arc binds to these live maps,
                            // instead of re-keying chords onto the wrong satellites (RE-4).
                            std::set<int> satv, gids;
                            net::forEach(ctx.entities.sats, &net::SatelliteState::carry_vertex,
                                         [&](const SatId&, int v) { satv.insert(v); });
                            net::forEach(ctx.entities.constellations,
                                         &net::ConstellationState::channels,
                                         &net::ChannelState::group_id,
                                         [&](System, const std::string&, int gid) { gids.insert(gid); });
                            carry_ok = carry_driver.validateCarryBinding(
                                static_cast<int>(nc.stations.size()), satv, gids, &cerr);
                        }
                        if (!carry_ok) {
                            log->warn("Network (RTB-7c): carry blob present but NOT usable ({}) "
                                      "— the invariant carry COLD-STARTS (branch not "
                                      "reproduced; shared/emitter state stay warm).", cerr);
                            carry_driver.reset();
                            net::dropAll(entities.sats, &net::SatelliteState::carry_vertex);
                            net::dropAll(entities.constellations,
                                         &net::ConstellationState::channels,
                                         &net::ChannelState::group_id);
                            hb1(WarmResumeRefusal::Other,
                                "the invariant-carry blob could not be restored/bound: " + cerr);
                        } else {
                            log->info("Network (RTB-7c): BRANCH-FAITHFUL — restored invariant "
                                      "DD carry ({} carried DD), {} sat-vertex + {} group-id map "
                                      "entries; sigma0 {}.  Pᶻ shape: {}.  "
                                      "Linearization frame: {}.",
                                      carry_driver.resumeCarryChords(),
                                      snap.carry_sat_vertex.size(), snap.carry_group_id.size(),
                                      carry_driver.resumeSigma0Seeded()
                                          ? fmt::format("SEEDED {:.4f}",
                                                        carry_driver.resumeSigma0Value())
                                          : std::string("not seeded (v1 blob / calib off)"),
                                      // A diagonal restore looks identical to a dense one
                                      // everywhere else (RTB-7(e), invariant #4).
                                      carry_driver.resumeCarryDiagonal()
                                          ? fmt::format("DIAGONAL-ONLY (v3 blob, {} block(s)) — "
                                                        "means exact, chord cross-covariances "
                                                        "start at ZERO and re-earn from data",
                                                        carry_driver.resumeCarryDiagonalBlocks())
                                          : std::string("dense (v1/v2 blob) — the branch "
                                                        "geometry resumes exactly"),
                                      // Whether the blob carries ZL rows; nothing else in
                                      // this line shows it (RS-2, invariant #4).  Either way
                                      // the branch folds in the WRITER's frame.
                                      carry_driver.resumeCarryLinBlocks() > 0
                                          ? fmt::format("ZL rows present on {} block(s) — the "
                                                        "branch folds in the WRITER's frame",
                                                        carry_driver.resumeCarryLinBlocks())
                                          : std::string("NO ZL rows — the means are in the "
                                                        "WRITER's linearization frame"));
                            // Reporting half of the PD-safe restore guard (INC-2c).  A blob
                            // written without pd_safe can carry a corrupt joint lane block
                            // (indefinite Pᶻ, negative diagonals); importing it verbatim would
                            // seed the fixed build with the frozen carry the fix releases.  A
                            // non-zero count on the first pd_safe resume is expected; a
                            // non-zero count when resuming from a pd_safe-armed run is a new
                            // defect, because the writing run's blocks were PD by construction.
                            if (carry_driver.resumeCarryRebirthBlocks() > 0)
                                log->warn("Network (PD-safe fold, INC-2c): {} restored "
                                          "carry block(s) FAILED the equilibrated PD screen "
                                          "and were REBIRTHED — means kept, Pᶻ re-seeded to "
                                          "the diffuse fresh-chord prior.  Those blocks take "
                                          "their data-only tap-off over the next epoch.  "
                                          "Expected on a first resume from a pre-PD-safe "
                                          "snapshot; investigate if the blob came from a "
                                          "pd_safe-armed run.",
                                          carry_driver.resumeCarryRebirthBlocks());
                        }
                    }
                    const double snap_s =
                        static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
                    log->info("Network (RTB-7): WARM RESUME from '{}' (written {}) — snapshot "
                              "age {:.1f} s vs first live epoch (horizon {:.1f} s); generation "
                              "id {} PRESERVED (first-emit minting suppressed).  Restored: {} "
                              "D2 shared-carry, {} ambiguity-carry, {} ZWD-carry, {} clock-"
                              "oscillator, {} clock-monitor baseline, {}/{} pivot baseline "
                              "(prev/last-ever), {} pending retro-bump, {} refloat-horizon "
                              "entries; emitter stream state (counters, baselines, first-"
                              "sighting sets, pending bumps, last-seen stamps) restored; "
                              "n_emitted {} (carry stays warm).",
                              snap_path.string(), snap.wall_utc, first_live_s - snap_s,
                              horizon_s, generation_id, prev_carry.size(), net::present(ctx.entities.arcs, &net::ArcState::amb),
                              net::present(sta_rows,
                                           &net::StationState::zwd),
                              net::present(ctx.entities.sats, &net::SatelliteState::clock_osc),
                              net::present(ctx.entities.sats, &net::SatelliteState::prev_clk),
                              net::present(ctx.entities.constellations,
                                           &net::ConstellationState::prev_pivot),
                              net::present(ctx.entities.constellations,
                                           &net::ConstellationState::last_pivot),
                              net::present(ctx.entities.sats, net::Disc::UraRetro),
                              net::present(ctx.entities.sats, &net::SatelliteState::ura_refloat_horizon),
                              n_emitted);
                }
            }
        }
    }
}

void networkRestoreCanonicalSignals(NetworkRunContext& ctx, const GnssConfig& config_,
                                    std::vector<NetStation>& stations,
                                    const std::shared_ptr<spdlog::logger>& log) {
    // Apply half of the canonical-signal restore (RS-8, SPEC_event_harness §21.16).
    // networkTryWarmResume parsed the snapshot and staged its CN rows; the driver calls
    // this seam next, with the station roster in scope, and it is the only place that
    // writes the latch.  It has to run before the first processed epoch:
    // PppModel::decideConstellationCanonicals locks the latch on that epoch, and a
    // restore after it would already be overwritten.
    //
    // Unarmed, or on a run that did not resume, the stash is empty and this is a no-op
    // (byte-identical).  Armed with an empty stash after a resume, networkTryWarmResume
    // has already warned once about the snapshot without CN rows; invariant #4 is served
    // there, where the file is in hand, instead of warning twice.
    if (!ctx.knobs.resume_signal_canon_restore) return;
    net::StationTable& staged = ctx.entities.stations;   // the staged CN rows (resume_canon_rows)
    if (net::present(staged, &net::StationState::resume_canon_rows) == 0) return;

    long long n_sta = 0, n_ent = 0, n_orphan = 0;
    net::forEach(staged, &net::StationState::resume_canon_rows,
                 [&](const StationId& sid, const std::vector<PppModel::CanonicalSignal>& rows) {
        const int idx = sid.index;
        if (idx < 0 || idx >= static_cast<int>(stations.size())
            || !stations[static_cast<std::size_t>(idx)].model) {
            // Unreachable when the fingerprint passes (the roster is one of its terms),
            // so this is a tripwire rather than a handled case, and the drop is counted
            // and logged below rather than skipped without a trace.
            ++n_orphan;
            return;
        }
        const std::size_t applied =
            stations[static_cast<std::size_t>(idx)].model->restoreCanonicalSignals(rows);
        if (applied == 0) return;
        ++n_sta;
        n_ent += static_cast<long long>(applied);
    });
    ctx.counters.n_resume_canon_stations = n_sta;
    ctx.counters.n_resume_canon_entries  = n_ent;
    net::dropAll(staged,
                 &net::StationState::resume_canon_rows);   // one-shot: the latch is written, the buffer owns nothing further

    if (n_orphan > 0)
        log->warn("Network (RS-8 canonical restore): {} snapshot station index(es) name "
                  "no station on this roster and were DROPPED — the config fingerprint "
                  "should have refused this file; treat the run as suspect.", n_orphan);
    log->info("Network (RS-8 canonical restore): applied {} canonical entr{} to {} "
              "station model(s) BEFORE the first epoch — each of those stations keeps "
              "the writer's per-band channel instead of re-deciding it from this "
              "epoch's satellite counts.  The first epoch AUDITS the latch (it runs the "
              "tally diagnostically and reports how many entries it would have picked "
              "differently); the run-end RS-8 line carries that count.",
              n_ent, n_ent == 1 ? "y" : "ies", n_sta);
}

// ---------------------------------------------------------------------------
// Install the resume's stashed code-bias linearization points on the station
// parameter sets.
//
// This is a separate seam rather than a block inside networkTryWarmResume because
// the two need different inputs: the resume function owns the snapshot and stashes
// the RL map into ctx.carry, while this one needs the mutable station roster.  The
// install matters only between the resume and the first networkBirthLocalStates,
// since the first-epoch code-bias seed is a one-shot latch, so the driver
// calls it immediately after the resume, in the same way as the RS-8
// canonical-signal restore.
// ---------------------------------------------------------------------------
void networkApplyRxCbLinRestore(NetworkRunContext& ctx, const GnssConfig& config_,
                                std::vector<NetStation>& stations,
                                const std::shared_ptr<spdlog::logger>& log) {
    // Nothing stashed ⇒ nothing to do: the knob is off, no warm resume happened, or
    // the snapshot carried no RL rows (warned at the resume).  No allocation and no
    // branch taken inside any station, so the run is byte-identical.
    net::StationTable& sta_rows = ctx.entities.stations;   // the staged RL rows (rxcb_lin_restore)
    if (net::present(sta_rows, &net::StationState::rxcb_lin_restore) == 0) return;
    (void)config_;   // the knob is already resolved into ctx.knobs / the stash
    // One out-slot pair per station, sized once here and never resized: each
    // station parameter set keeps a raw pointer to its own element and increments it from
    // inside the OpenMP parallel produce phase.  Distinct objects ⇒ race-free without
    // atomics, the same discipline as the per-station StationAssembly tallies.  The
    // pair is {applied, carried-but-unseeded}; the second is scoped to the keys the
    // map carries (the RL rows pair 1:1 with the RC rows), so a code-bias family with
    // no carry, whose x_lin no gate reads, is not counted as a miss.
    ctx.carry.rxcb_lin_sta_counts.assign(stations.size(), {0LL, 0LL});
    long long installed_sta = 0, installed_entries = 0, orphan = 0;
    net::forEach(sta_rows, &net::StationState::rxcb_lin_restore,
                 [&](const StationId& sid, const std::map<std::string, double>& keymap) {
        const int sidx = sid.index;
        if (sidx < 0 || sidx >= static_cast<int>(stations.size())) { ++orphan; return; }
        auto& st = stations[static_cast<std::size_t>(sidx)];
        if (!st.model) { ++orphan; return; }
        auto& slot = ctx.carry.rxcb_lin_sta_counts[static_cast<std::size_t>(sidx)];
        st.params.rx_cb_restore = CodeBiasRestore{keymap, &slot.first, &slot.second};
        ++installed_sta;
        installed_entries += static_cast<long long>(keymap.size());
    });
    // The roster is inside the warm-resume fingerprint, so an index that does not
    // resolve should be unreachable; if one occurs, the restore for that station is
    // lost, and the warning makes it visible (invariant #4).
    if (orphan > 0)
        log->warn("Network (RS-9 rx-CB frame restore): {} restored station index(es) did "
                  "not resolve to a live station model and were DROPPED — those stations "
                  "re-derive their code-bias linearization point from the resume epoch's "
                  "code, which is the pre-restore behaviour for them.", orphan);
    log->info("Network (RS-9 rx-CB frame restore): installed on {} station(s), {} "
              "linearization point(s) — consumed at each station's first-epoch code-bias "
              "initialization (the one-shot seed latch), so the rx-CB "
              "inject gate's reference is the writer's rather than this leg's.",
              installed_sta, installed_entries);
    // Consumed: the map has been handed to the parameter sets, and re-installing it
    // later would be inert (the latch has closed).  Clearing it here enforces that.
    net::dropAll(sta_rows, &net::StationState::rxcb_lin_restore);
}

// ---- Harvest half (generated by scripts/nom_hv1_cut.py) ----

void networkResumeCouplingCensusRow(NetworkRun& run, const LocalCarry& restored,
                                    const std::vector<CoupledShared>& coupling) {
    // ---- From the run, the epoch and the harvest (generated by
    //      scripts/nom_hv1_cut.py, each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    // ---- End of the bindings ----
    const auto idOf = [](const CoupledShared& c) {
        return std::make_tuple(static_cast<int>(c.kind), c.sat,
                               c.signal, c.n, c.m,
                               static_cast<int>(c.trig), c.sh_layer);
    };
    std::map<decltype(idOf(coupling.front())), double> now;
    double maxabs = 0.0, rhs_new = 0.0, rhs_old = 0.0, dmax = 0.0;
    for (const CoupledShared& cs : coupling) {
        now[idOf(cs)] = cs.coeff;
        maxabs  = std::max(maxabs, std::abs(cs.coeff));
        rhs_new += cs.coeff * cs.s_prev;
    }
    long long matched = 0, gone = 0;
    for (const CoupledShared& cs : restored.coupling) {
        rhs_old += cs.coeff * cs.s_prev;
        const auto h = now.find(idOf(cs));
        if (h == now.end()) { ++gone; continue; }
        ++matched;
        dmax = std::max(dmax, std::abs(cs.coeff - h->second));
    }
    ++ctx.counters.n_resume_cpl_census_rows;
    ctx.counters.n_resume_cpl_census_terms     += matched;
    ctx.counters.n_resume_cpl_census_unmatched += gone;
    if (maxabs > 0.0)
        ctx.carry.resume_cpl_census_rel.push_back(dmax / maxabs);
    ctx.carry.resume_cpl_census_rhs.push_back(
        std::abs(rhs_new - rhs_old));
}

void networkResumeFirstEpochRetire(NetworkRun& run) {
    // ---- From the run, the epoch and the harvest (generated by
    //      scripts/nom_hv1_cut.py, each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    // ---- End of the bindings ----
    // Retire the first-warm-epoch latch at the end of this epoch's serial harvest
    // (§18.12).  Everything the latch gates (the C1 diagonal-border count in
    // networkInjectD2CarryPrior and the present/departed fold split, both earlier in the
    // epoch, and the restored-against-fresh regressor census above) therefore covers
    // exactly the first
    // post-resume epoch.  Unconditional: a run that did not resume leaves it false, and
    // clearing a false bool is a no-op on every off path.
    if (ctx.carry.resume.cpl_census)
        // Second instrument of P9a (RS-7, §21.14).  The warm resume runs before any
        // epoch, so every Q3 decouple counted by now belongs to the first post-resume
        // epoch, with no baseline subtraction and no per-epoch plumbing.  When the
        // seam's carried rows meet a border with no cross-covariance, many anchored
        // clock priors are out-voted at the first fold (§18.11); with the border
        // restored, this count is expected to match the continuous run's level.
        ctx.counters.n_resume_first_fold_cta_trips =
            ctx.counters.cta_trip_nis + ctx.counters.cta_trip_abs
          + ctx.counters.cta_trip_gap + ctx.counters.cta_trip_datum
          + ctx.counters.cta_trip_ext;
    ctx.carry.resume.cpl_census = false;
}

// ---- End of the harvest half ----

} // namespace GnssProcessor
