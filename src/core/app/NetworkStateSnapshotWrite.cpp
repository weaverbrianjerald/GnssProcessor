/**
 * @file NetworkStateSnapshotWrite.cpp
 * @brief @internal networkWriteStateSnapshot: the periodic atomic state snapshot
 *        (RTB-7(a)), written one record layer at a time as the mirror of the warm resume.
 *
 * One function, called by the driver (app/ApplicationNetwork.cpp) at the end of every
 * kept epoch: an epoch the driver skips returns before the call, and an epoch the SH
 * health gate withholds from emission still reaches it.  With a snapshot path it writes on
 * its first call and then once per network.state_snapshot_interval_s of product time;
 * without one it does nothing.  The restore of each layer is in NetworkWarmResume.cpp,
 * and the fingerprint the snapshot carries is formed in NetworkSnapshotFingerprint.cpp.
 *
 * The body opens with an alias block (decomposition INC-4) that binds the driver's former
 * local names to members of the context, so the relocated code keeps those names.
 *
 * One of the seven files that define the network run context's functions: card NOM3 step
 * HV0 split them out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/app/NetworkRunContext.hpp.
 */

#include "app/NetworkRunContext.hpp"

#include "config/GnssConfig.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"   // branch-faithful carry (de)serialization (RTB-7(c))
#include "products/NetworkClockEmitRebase.hpp"       // CLK-R held frame, written with the snapshot (§18.10)
#include "products/NetworkEmitRebase.hpp"            // eq34 emit re-base frame, written with the snapshot (RS-5, §21.10)
#include "products/NetworkPbEmitFilter.hpp"          // PB emit filter state, written with the snapshot (RS-5, §21.10)
#include "math/TimeUtils.hpp"                // GnssTime
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "realtime/RealTimeEngine.hpp"       // consumer snapshot written beside the network history copy (card CC, part B)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <sstream>

namespace GnssProcessor {

void networkWriteStateSnapshot(NetworkRunContext& ctx, const GnssConfig& config_,
                               const GnssTime& t, double pl_now_s,
                               NetworkProductEmitter& emitter,
                               const NetworkInvariantCarryDriver& carry_driver,
                               const std::vector<NetStation>& stations,
                               const std::shared_ptr<spdlog::logger>& log,
                               const NetworkClockEmitRebase* clk_rebase,
                               const NetworkEmitRebase* emit_rebase,
                               const NetworkPbEmitFilter* pb_filter,
                               RealTimeEngine* consumer) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const auto& nc = config_.getNetworkConfig();
    auto& snap_path = ctx.carry.snap_path;
    auto& last_snapshot_write_s = ctx.carry.last_snapshot_write_s;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_pivot = ctx.carry.prev_pivot;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& c1_zoh_active = ctx.carry.c1_zoh_active;
    auto& prev_clk = ctx.carry.prev_clk;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& clock_rate_est = ctx.carry.clock_rate_est;
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& retro_active = ctx.carry.retro_active;
    auto& relevel_iode = ctx.carry.relevel_iode;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& ura_ewma_updates = ctx.carry.ura_ewma_updates;
    auto& ramp_lifted = ctx.carry.ramp_lifted;
    auto& first_emit_content_s = ctx.carry.first_emit_content_s;
    auto& generation_id = ctx.counters.generation_id;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_sigma0_withheld = ctx.counters.n_sigma0_withheld;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
    auto& iod = ctx.counters.iod;
    auto& prev_week = ctx.counters.prev_week;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& last_emit_content_s = ctx.counters.last_emit_content_s;
    auto& last_health_write_s = ctx.counters.last_health_write_s;

    // --- Periodic atomic state snapshot (RTB-7(a)) ---------------------------
    // Written at the end of emitted epochs only (skipped epochs `continue` in the
    // driver and change no carry state), every state_snapshot_interval_s of product
    // time.  It is written after this epoch's product emit and flush, through the
    // snapshot's own temp file and rename; this block never flushes or reorders the
    // product writer, so emission is unaffected.  Empty path (the default) ⇒
    // byte-identical.
    if (!snap_path.empty()
        && (last_snapshot_write_s < 0.0
            || pl_now_s - last_snapshot_write_s >= nc.state_snapshot_interval_s)) {
        // Cold-state exclusions (RTB-7).  The snapshot carries the producer's own state:
        // the cross-epoch carries (phase level, iono, ZWD), clock oscillators, pivots,
        // generation id, URA/ramp metrics and the emitter's disc-counter, baseline and
        // first-sighting bookkeeping, plus the optional capture groups below.  The
        // per-station filter state is not snapshotted, by design: per-station cycle-slip
        // arcs and slip-detector history, per-station StateManager ambiguity / iono /
        // receiver-bias marks, and per-station covariance.  On a warm resume those layers
        // cold-start and re-warm within a few epochs, and the post-fit RAIM absorbs the
        // transient; serializing the large per-station covariance blocks would add nothing
        // the re-warm does not already provide.  That is the recovery-budget trade RTB-7
        // accepts.
        NetworkStateSnapshot snap;
        snap.snap_week          = t.week;
        snap.snap_tow           = t.tow;
        snap.wall_utc           = NetworkStateSnapshotIO::utcNowIso();
        snap.generation_id      = generation_id;
        snap.config_fingerprint = live_fingerprint;
        snap.n_emitted          = n_emitted;
        snap.n_skipped          = n_skipped;
        snap.n_gate_withheld    = n_gate_withheld;
        snap.n_sigma0_withheld  = n_sigma0_withheld;
        snap.n_runaway_epochs   = n_runaway_epochs;
        snap.n_perconst_partial = n_perconst_partial_epochs;
        snap.n_perconst_diffuse = n_perconst_diffuse_published;
        snap.iod                = iod;
        snap.prev_week          = prev_week;
        snap.prev_tow           = prev_tow;
        snap.last_emit_content_s = last_emit_content_s;
        snap.last_health_write_s = last_health_write_s;
        snap.ura_sigma0_ewma      = ura_sigma0_ewma;
        snap.ura_ewma_updates     = ura_ewma_updates;
        snap.ramp_lifted          = ramp_lifted;
        snap.first_emit_content_s = first_emit_content_s;
        snap.shared_carry.reserve(prev_carry.size());
        for (const CarryPrior& cp : prev_carry) {
            // A parked entry (a departed satellite's pre-gap realization, READMIT) is not
            // serialized: the D record has no park stamp, so a restored copy would fold as
            // a fresh live carry with epoch-dt inflation.  Under the forward-only response
            // policy, a seam inside a satellite's absence loses its seed, and the
            // satellite returns cold.
            if (cp.t_park_s > 0.0) { ++ctx.counters.n_snap_parked_excluded; continue; }
            NetworkStateSnapshot::SharedCarry sc;
            sc.kind      = static_cast<int>(cp.kind);
            sc.sat       = cp.sat;
            sc.signal    = cp.signal;
            sc.sh_n      = cp.sh_n;
            sc.sh_m      = cp.sh_m;
            sc.sh_trig   = static_cast<int>(cp.sh_trig);
            sc.value     = cp.value;
            sc.var       = cp.var;
            sc.inflate_q = cp.inflate_q;
            // Dual-layer ionosphere (Iono-2): tag the second shell in the sat field
            // (otherwise empty for VtecSh); the record arity is unchanged, and older
            // readers reseed safely.
            if (cp.kind == SharedParamKind::VtecSh && cp.sh_layer > 0)
                sc.sat = "L1";
            snap.shared_carry.push_back(std::move(sc));
        }
        snap.prev_pivot    = prev_pivot;
        snap.last_pivot    = last_pivot;
        snap.c1_zoh_active = c1_zoh_active;
        snap.prev_clk      = prev_clk;
        for (const auto& [sat, o] : clock_osc) {
            NetworkStateSnapshot::Osc so;
            so.x0 = o.x0; so.x1 = o.x1;
            so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
            so.t_last = o.t_last; so.init = o.init;
            snap.clock_osc[sat] = so;
        }
        for (const auto& [sat, o] : clock_rate_est) {   // network.emit_clock_rate decoupled C1 state
            NetworkStateSnapshot::Osc so;
            so.x0 = o.x0; so.x1 = o.x1;
            so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
            so.t_last = o.t_last; so.init = o.init;
            snap.clock_rate_est[sat] = so;
        }
        // network.phase_datum_handover_max_var_ratio: the arc's first-stash variance is an
        // optional trailing L field, filled only when the ratio ceiling is armed, so a
        // default snapshot's L rows stay byte-identical.
        const bool l_var_first = ctx.knobs.pd_handover_max_var_ratio > 0.0;
        // Card SCL2 X2: the history is not snapshotted, so a row is written with its marginal
        // variance (var plus its history coefficients' squared norm); off, var as before.
        const bool shx_marg = ctx.knobs.carry_cpl_shx;
        for (const auto& [key, lc] : local_carry) {
            NetworkStateSnapshot::Local L{lc.value_abs,
                                          shx_marg ? sharedHistoryMarginalVar(lc) : lc.var,
                                          lc.last_seen_s};
            if (l_var_first) L.var_first = lc.var_first;
            snap.local_carry[key] = L;
        }
        for (const auto& [idx, lc] : zwd_carry)
            snap.zwd_carry[idx] =
                NetworkStateSnapshot::Local{lc.value_abs,
                                            shx_marg ? sharedHistoryMarginalVar(lc) : lc.var,
                                            lc.last_seen_s};
        snap.ura_refloat_horizon = ura_refloat_horizon;
        snap.ura_retro_sats      = ura_retro_sats;
        snap.retro_active        = retro_active;
        snap.relevel_iode              = relevel_iode;              // eph cutover (card 6)
        snap.eph_cutover_disc_pending  = eph_cutover_disc_pending;  // eph cutover (card 6)
        // Resume frame state (§18.10, snapshot format v2), captured when
        // network.state_snapshot_frame_capture is armed: the per-(sat,signal)
        // phase-anchor incumbents and R2 sticky targets as of this epoch's election, the
        // Q3 clock-anchor per-satellite state (folded satellites only), and the CLK-R
        // held frame.  It has its own knob, unlike the Z rows, because a binary that
        // predates it cannot read a v2 file, so the default snapshot stays v1 byte for
        // byte.  Whether a resume uses the rows is the resume-side knob
        // network.resume_frame_restore.
        if (nc.state_snapshot_frame_capture) {
        snap.anchor_incumbent  = ctx.carry.anchor_incumbent;
        snap.anchor_pin_target = ctx.carry.anchor_pin_target;
        for (const auto& [sat, st] : ctx.carry.clk_anchor.sat) {
            if (!(st.t_last > 0.0)) continue;   // never folded: nothing to carry
            NetworkStateSnapshot::ClockAnchorSat cs;
            cs.t_last       = st.t_last;
            cs.last_value   = st.last_value;
            cs.last_var_eff = st.last_var_eff;
            cs.in_band      = st.in_band;
            cs.anchored     = st.anchored;
            snap.clock_anchor[sat] = cs;
        }
        if (clk_rebase) {
            const auto fs = clk_rebase->exportFrameState();
            snap.clk_frame_acc    = fs.acc;
            snap.clk_frame_prev   = fs.prev;
            snap.clk_frame_epochs = fs.epochs;
        }
        }   // state_snapshot_frame_capture
        // Card-1 coupling regressors (§18.12, snapshot format v3), captured under
        // network.state_snapshot_coupling_capture.  This is a separate knob from the frame
        // knob because the rows make the file about 37 times larger (and the keep-history
        // library with it), and a frame-only writer stays available for cheap legs and for
        // the arm-B control.  Layout: a dictionary of shared columns keyed on
        // (identity, ŝ_prev, P_prev), plus one packed line per carried row.
        //
        // The value pair is part of the dictionary key, not only the payload, because
        // harvestCoupling assigns cs.s_prev = x_s(j) and cs.p_prev = cov_df(j,j) from the
        // shared column alone (the harvest lambda in networkHarvestLocalCarries): rows of
        // one harvest epoch agree, but a station whose harvest failed hits `continue`
        // before the stash and keeps an older coupling, so a file can hold L rows with an
        // older last_seen_s.  An identity-only hoist would restore those rows against the
        // wrong frame (the frozen-frame defect class, the reason the lane seed-site snap
        // was deleted).
        //
        // Row atomicity (card 1e): a row is written whole or not at all.  A truncated
        // coefficient vector loses the exact gauge identity Mᵀg_s = −g_r(c); every
        // coupling row then observes the clock gauge, and the W5 null-space gate withholds
        // each epoch that carries rows, which is worse than the diagonal degrade an empty
        // vector already gives.  fmt writes "nan"/"inf", and the parser's strtod would
        // accept and consume them, so the finite screen here is required, not a second
        // guard.
        if (nc.state_snapshot_coupling_capture && config_.networkCarryConditionalCoupling()) {
            using CplKey = std::tuple<int, std::string, std::string, int, int, int, int,
                                      double, double>;
            const auto keyOf = [](const CoupledShared& cs) {
                return CplKey{static_cast<int>(cs.kind), cs.sat, cs.signal, cs.n, cs.m,
                              static_cast<int>(cs.trig), cs.sh_layer, cs.s_prev, cs.p_prev};
            };
            std::map<CplKey, int> dict_id;          // scratch; ordered => deterministic
            std::vector<std::pair<int, double>> ents;
            long long n_terms = 0, n_drop = 0;
            const auto pack = [&](const std::vector<CoupledShared>& cpl) -> bool {
                ents.clear();
                // Screen the whole row before hoisting anything.  Returning from inside the
                // build loop below would leave the columns already pushed for terms 0..k-1
                // in snap.coupling_dict as orphan QS records that no QL/QZ line references:
                // they inflate snap_cpl_dict_last (the '{}-column dictionary' the run-end
                // line prints), and if every row drops, hasCouplingState() stays true and a
                // v3 header is written for a file with a dictionary and zero rows, a shape
                // the format description rules out.  Row atomicity (card 1e) covers the
                // dictionary half as well.
                for (const CoupledShared& cs : cpl)
                    if (!std::isfinite(cs.coeff) || !std::isfinite(cs.s_prev)
                        || !std::isfinite(cs.p_prev) || cs.p_prev < 0.0)
                        return false;               // omit the whole row, never part of it
                ents.reserve(cpl.size());
                for (const CoupledShared& cs : cpl) {
                    const CplKey k = keyOf(cs);
                    auto it = dict_id.find(k);
                    if (it == dict_id.end()) {
                        NetworkStateSnapshot::CouplingCol q;
                        q.kind     = static_cast<int>(cs.kind);
                        q.sat      = cs.sat;
                        q.signal   = cs.signal;
                        q.sh_n     = cs.n;
                        q.sh_m     = cs.m;
                        q.sh_trig  = static_cast<int>(cs.trig);
                        q.sh_layer = cs.sh_layer;   // own field, not the D-record "L1" tag
                        q.s_prev   = cs.s_prev;
                        q.p_prev   = cs.p_prev;
                        it = dict_id.emplace(
                                 k, static_cast<int>(snap.coupling_dict.size())).first;
                        snap.coupling_dict.push_back(std::move(q));
                    }
                    ents.emplace_back(it->second, cs.coeff);   // harvest order preserved
                }
                return !ents.empty();
            };
            for (const auto& [key, lc] : local_carry) {
                if (lc.coupling.empty()) continue;             // legacy-diagonal entry
                if (!pack(lc.coupling)) { ++n_drop; continue; }
                n_terms += static_cast<long long>(ents.size());
                snap.coupling_rows[key] = ents;
            }
            for (const auto& [idx, lc] : zwd_carry) {
                if (lc.coupling.empty()) continue;
                if (!pack(lc.coupling)) { ++n_drop; continue; }
                n_terms += static_cast<long long>(ents.size());
                snap.coupling_zrows[idx] = ents;
            }
            // The refloat set has no D-row carrier (a hard-refloated satellite is
            // skip-stashed), so without this the resume cannot reproduce the pre-pass mark
            // (cards 1d/1g).
            snap.clock_refloat_sats = ctx.carry.prev_clock_refloat_sats;
            ctx.carry.snap_cpl_dict_last    = static_cast<long long>(snap.coupling_dict.size());
            ctx.carry.snap_cpl_rows_last    = static_cast<long long>(snap.coupling_rows.size());
            ctx.carry.snap_cpl_zrows_last   = static_cast<long long>(snap.coupling_zrows.size());
            ctx.carry.snap_cpl_terms_last   = n_terms;
            ctx.carry.snap_cpl_rows_dropped += n_drop;
            // The C1 dense D2 covariance side-car, the other half of the seam (§18.12
            // (2a)).  Keys in row order (QK), lower triangle per row (QP); the whole block
            // is omitted (never truncated) when any entry is non-finite or a row has no
            // key.  Parked entries never have a row, so nothing here references them.
            {
                const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
                const Eigen::Index n = pcc.P.rows();
                if (ctx.knobs.fullcov_on && n > 0 && pcc.P.cols() == n && pcc.P.allFinite()
                    && static_cast<Eigen::Index>(pcc.row.size()) == n) {
                    std::vector<std::string> keys(static_cast<std::size_t>(n));
                    bool ok = true;
                    for (const auto& [k, r] : pcc.row) {
                        if (r >= keys.size() || !keys[r].empty()) { ok = false; break; }
                        keys[r] = k;
                    }
                    for (const auto& k : keys) if (k.empty()) ok = false;
                    if (ok) {
                        snap.carry_cov_keys = std::move(keys);
                        snap.carry_cov_rows.assign(static_cast<std::size_t>(n), {});
                        for (Eigen::Index i = 0; i < n; ++i) {
                            auto& row = snap.carry_cov_rows[static_cast<std::size_t>(i)];
                            row.reserve(static_cast<std::size_t>(i) + 1);
                            for (Eigen::Index j = 0; j <= i; ++j) row.push_back(pcc.P(i, j));
                        }
                    }
                }
            }
        }   // state_snapshot_coupling_capture
        // Datum capture (RS-3; network.state_snapshot_datum_capture, SPEC_event_harness
        // §21.7): the write half of the two datum memories that a warm resume would
        // otherwise re-realize.  The frame block above carries the satellite side of the
        // eq34 tree (anchor_incumbent); this carries the receiver side plus the
        // per-component root vote, the pin targets, the longevity elevations and the
        // extend-only retained set (together, what networkElectChainDatum's sticky seed
        // reads), and the time-linked code-bias previous-value map and ever-seen set.
        //
        // These are plain copies of ctx.carry after the emit, into `snap` only: the writer
        // run's products and every trace are byte-identical, so the knob can be added to
        // an existing recipe.  Every container is empty on a run that does not arm the
        // datum it belongs to (phase_datum_eq34 / code_datum_time_linked), so arming the
        // capture on such a run costs nothing and writes no rows.
        if (nc.state_snapshot_datum_capture) {
            snap.eq34_datum_rx  = ctx.carry.eq34_datum_rx;
            snap.eq34_rx_parent = ctx.carry.eq34_rx_parent;
            snap.eq34_sat_root  = ctx.carry.eq34_sat_root;
            for (const auto& [key, tgt] : ctx.carry.eq34_prev_target)
                snap.eq34_prev_target[{key.first, key.second}] = tgt;
            for (const auto& [key, el] : ctx.carry.eq34_prev_el)
                snap.eq34_prev_el[{static_cast<int>(key.first), key.second}] = el;
            for (const auto& [idx, amb] : ctx.carry.eq34_retained_edges)
                snap.eq34_retained_edges.emplace(idx, amb);
            // Non-finite screen on the code-bias values only: the eq34 targets and
            // elevations are rounded or geometric quantities the election recomputes,
            // while code_bias_prev_ is the quantity the veteran-referenced sum-to-zero RHS
            // differences against.  A nan there would survive the parse (strtod accepts
            // it) and corrupt the restored datum, so it is dropped at the source.
            for (const auto& [fam, per_sat] : ctx.carry.code_bias_prev_)
                for (const auto& [sat, v] : per_sat)
                    if (std::isfinite(v)) snap.code_bias_prev[fam][sat] = v;
            snap.code_bias_sat_seen = ctx.carry.code_bias_sat_seen_;
            // The IF-pin pair that produced those values, the third container of the same
            // kept-epoch lifecycle.  Without it the restore is inert on the IF-pin half:
            // `pair_same` reads false for every satellite, the split re-seeds to 0 and
            // every satellite disc-bumps.
            snap.code_bias_ifpin_pair = ctx.carry.code_bias_ifpin_pair_;
            ctx.carry.snap_datum_rows_last =
                static_cast<long long>(snap.eq34_datum_rx.size()
                                       + snap.eq34_rx_parent.size()
                                       + snap.eq34_sat_root.size()
                                       + snap.eq34_prev_target.size()
                                       + snap.eq34_prev_el.size()
                                       + snap.eq34_retained_edges.size()
                                       + snap.code_bias_sat_seen.size()
                                       + snap.code_bias_ifpin_pair.size());
            for (const auto& [fam, per_sat] : snap.code_bias_prev)
                ctx.carry.snap_datum_rows_last += static_cast<long long>(per_sat.size());
        }   // state_snapshot_datum_capture
        // Emit-layer capture (RS-5; network.state_snapshot_emit_capture,
        // SPEC_event_harness §21.10): the write half of the emit-side cross-epoch state.
        // The frame and datum blocks above carry the estimator's realization; these two
        // engines sit downstream of it on the outputs only (the re-base and the filter in
        // NetworkEmitStore.cpp) and are runNetwork locals, so without the capture a warm
        // resume cold-starts both, and the first post-resume epoch emits the raw
        // solve-frame phase bias while the continuous wire carries the accumulated re-base
        // (§21.10 has the closure table).
        //
        // These are plain exports after the emit, into `snap` only, with the same
        // write-side discipline as the datum block, so the knob can be added to an
        // existing recipe.  A run that arms neither engine (carry_invariant off /
        // emit_pb_filter off) exports empty maps and writes no rows, so arming the capture
        // there costs nothing.  Nothing here enters the warm-resume fingerprint: the rows
        // change no estimate, only the level in which the emitted biases are expressed.
        if (nc.state_snapshot_emit_capture) {
            if (emit_rebase) snap.emit_rebase_last = emit_rebase->exportState();
            if (pb_filter) {
                const auto fs = pb_filter->exportState();
                for (const auto& [key, ks] : fs.state)
                    snap.pb_filter_state[key] =
                        NetworkStateSnapshot::PbFilterKey{ks.mean, ks.var, ks.last_seen_s};
                for (const auto& [cst, ac] : fs.step)
                    snap.pb_filter_step[cst] =
                        NetworkStateSnapshot::PbFilterStep{ac.sum_log_e2, ac.n_eff};
            }
            ctx.carry.snap_emit_rows_last =
                static_cast<long long>(snap.emit_rebase_last.size()
                                       + snap.pb_filter_state.size()
                                       + snap.pb_filter_step.size());
        }   // state_snapshot_emit_capture
        // Station-layer capture (RS-6; network.state_snapshot_station_capture,
        // SPEC_event_harness §21.13): the write half of the two per-station memories that
        // otherwise restart cold at every warm resume.  The frame and datum blocks carry
        // the network's realization and the emit block the level on top of it; these two
        // are the station layer underneath: the receiver code-bias posterior per
        // (station, CB family) and the per-LOS SH representation-error deviation.
        //
        // These are plain copies of ctx.carry after the emit, into `snap` only, with the
        // same write-side discipline as the datum and emit blocks, so the knob can be
        // added to an existing recipe.  A run that arms neither carry
        // (network.carry_rx_code_bias / the T5 deviation) exports empty maps and writes no
        // rows, so arming the capture there costs nothing.
        //
        // The non-finite screen is at the source, as for code_bias_prev: a nan that strtod
        // would accept on the way back in becomes the prior the next fold's innovation
        // differences against (rx-CB) or the GM level the next harvest ages (T5).  An
        // entry dropped here is only cold on the resume (the behaviour without the
        // capture, for that one key), whereas a corrupt entry cannot be recovered.
        if (nc.state_snapshot_station_capture) {
            for (const auto& [key, lc] : ctx.carry.rx_cb_carry)
                if (std::isfinite(lc.value_abs) && std::isfinite(lc.var) && lc.var > 0.0
                    && std::isfinite(lc.last_seen_s))
                    snap.rx_cb_carry[{key.first, key.second}] =
                        NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            for (const auto& [key, dc] : ctx.carry.sh_dev_carry)
                if (std::isfinite(dc.dev) && std::isfinite(dc.var) && dc.var >= 0.0
                    && std::isfinite(dc.var_ss) && dc.var_ss >= 0.0
                    && std::isfinite(dc.t_s))
                    snap.sh_dev_carry[key] =
                        NetworkStateSnapshot::ShDev{dc.dev, dc.var, dc.var_ss, dc.t_s};
            ctx.carry.snap_station_rows_last =
                static_cast<long long>(snap.rx_cb_carry.size() + snap.sh_dev_carry.size());
        }
        // Card CTU G5: the monitor's contrast-frame level information (LB rows).
        if (ctx.knobs.clm_on) {
            for (const auto& [sat, info] : ctx.carry.clm_info_c)
                if (std::isfinite(info) && info > 0.0) {
                    const auto nit = ctx.carry.clm_n.find(sat);
                    snap.level_info[sat] = {info, nit != ctx.carry.clm_n.end() ? nit->second : 0};
                }
        }   // state_snapshot_station_capture
        // Canonical-signal capture (RS-8; network.state_snapshot_signal_capture,
        // SPEC_event_harness §21.16): the write half of the per-station latch that no
        // other snapshot group holds, PppModel's per-(constellation, band, obs-type)
        // canonical signal, decided from the satellite counts of that station's first
        // processed epoch and then locked for the run.
        //
        // It is one of the two snapshot groups that cannot be read out of ctx.carry (the
        // latch is inside each station's model), which is why this function takes the
        // roster; the RS-9 RL block below is the other, and it reads the station's own
        // state vector.
        //
        // Read-only with respect to the models and written into `snap` alone, with the
        // same write-side discipline as the datum, emit and station blocks, so the knob
        // can be added to an existing recipe: products and traces stay byte-identical.
        if (nc.state_snapshot_signal_capture) {
            for (std::size_t i = 0; i < stations.size(); ++i) {
                const auto& st = stations[i];
                if (!st.model) continue;
                auto rows = st.model->exportCanonicalSignals();
                if (rows.empty()) continue;   // never processed an epoch, or nothing admitted
                auto& dst = snap.canonical_signals[static_cast<int>(i)];
                for (const auto& c : rows)
                    dst.push_back(NetworkStateSnapshot::CanonSignal{
                        c.constellation, c.band, c.obs_type, c.signal});
            }
            long long n = 0;
            for (const auto& [idx, rows] : snap.canonical_signals)
                n += static_cast<long long>(rows.size());
            ctx.carry.snap_canon_rows_last = n;
        }   // state_snapshot_signal_capture
        // Code-bias linearization-point capture (RS-9;
        // network.state_snapshot_rxcb_lin_capture, SPEC_event_harness §21.18): the
        // reference of the RC row.  The RC row above carries the rx-CB carry's numerator;
        // the gate that decides whether that prior is folded tests `value_abs − x_lin(cb)`
        // against a fixed 5.13 m bar, and x_lin is the one-shot seed that
        // StateInitializer::initializeCodeBiases wrote on this station's first processed
        // epoch and never revises.  It is not in ctx.carry but in the station's own state
        // vector, which is why this block reads `stations`.
        //
        // Read-only and after the emit, like the blocks above, so arming it moves no
        // estimate and no trace: an x_lin shift is an exact solve no-op for a state that
        // enters linearly (project_network_xlin_invariance), and nothing here writes one.
        // Keyed exactly as the RC rows, (roster index, CB state key), so the restore can
        // pair them without a second identity, and screened for finiteness at the source
        // for a stronger form of the RC reason: a non-finite linearization point would
        // corrupt the first epoch's iono and ambiguity seeds through computeRawResidual,
        // not only one fold.
        //
        // Driven by the RC rows themselves, not by the station's code-bias state set: one
        // RL row beside each RC row, and no orphans.  A station carries more code-bias
        // states than the rx-CB carry holds (a GPS L1/L2/L5 lineage has GPS_DCB and
        // GPS_IFB5 per station while only GPS_DCB is carried), and a reference with no
        // carry to judge has no consumer; restoring it would be an unmeasured state
        // change.  Iterating what was written also makes the pairing structural: the RL
        // count equals the RC count, so a restore cannot bind a frame to a carry the file
        // does not hold.  The loader's requirement on state_snapshot_station_capture is
        // what makes this non-empty.
        if (nc.state_snapshot_rxcb_lin_capture) {
            for (const auto& [key, l] : snap.rx_cb_carry) {
                (void)l;
                if (key.first < 0 || key.first >= static_cast<int>(stations.size())) continue;
                const auto& st = stations[static_cast<std::size_t>(key.first)];
                if (!st.sm) continue;
                const auto& cbs = st.sm->getCodeBiasStates();
                const auto cit = cbs.find(key.second);
                if (cit == cbs.end()) continue;
                const Eigen::VectorXd xs = st.sm->getStateVector();
                const Eigen::Index cb_idx = static_cast<Eigen::Index>(cit->second);
                if (cb_idx < 0 || cb_idx >= xs.size()) continue;
                if (!std::isfinite(xs(cb_idx))) continue;
                snap.rx_cb_lin[{key.first, key.second}] = xs(cb_idx);
            }
            ctx.carry.snap_rxcb_lin_rows_last =
                static_cast<long long>(snap.rx_cb_lin.size());
        }   // state_snapshot_rxcb_lin_capture
        // Branch-faithful snapshot (RTB-7(c)): serialize the invariant DD carry (z-store
        // branch) and its ArcKey vertex numbering (sat→vertex, (const,signal)→group), so a
        // warm resume reproduces the phase-ambiguity branch instead of re-converging one.
        if (nc.state_snapshot_carry) {
            snap.carry_sat_vertex = ctx.carry.carry_sat_vertex;
            snap.carry_group_id.clear();
            snap.carry_group_id.reserve(ctx.carry.carry_group_id.size());
            for (const auto& [key, gid] : ctx.carry.carry_group_id)
                snap.carry_group_id.emplace_back(key.first, key.second, gid);
            std::ostringstream ccs;
            // Write-side shape knob (RTB-7(e)).  The blob is about 99 % of the snapshot at
            // scale (at 305 stations, 13.1M dense Pᶻ numbers against 7.9k means), so the
            // diagonal shape makes the difference between a 258 MB and a ~160 KB file.  The
            // read side is unaffected.
            carry_driver.serializeCarry(ccs, nc.state_snapshot_carry_diagonal);
            snap.carry_state = ccs.str();
            ctx.carry.snap_carry_bytes_last =
                static_cast<long long>(snap.carry_state.size());
            ctx.carry.snap_carry_diagonal = nc.state_snapshot_carry_diagonal;
        }
        {
            std::ostringstream es;
            emitter.serializeState(es);
            snap.emitter_state = es.str();
        }
        std::string werr;
        if (!NetworkStateSnapshotIO::write(snap_path, snap, &werr))
            log->warn("Network (RTB-7): state-snapshot write to '{}' failed ({}) — the "
                      "previous good snapshot (if any) is left intact.",
                      snap_path.string(), werr);
        else
            ++ctx.carry.snap_writes;   // run-end blob-size readout (RTB-7(e), invariant #4)
        // Keep a per-epoch copy so the branch can be graded over time
        // (scripts/diff_carry_blobs.py; RTB-7(d), BR-1).  Write-side only: the same atomic
        // temp file and rename, and the estimation cannot see it (this runs after the
        // emit, over const references).
        if (nc.state_snapshot_keep_history) {
            std::filesystem::path hp = snap_path;
            hp += fmt::format(".ep{:04d}", static_cast<unsigned long long>(snap.n_emitted));
            std::string herr;
            if (!NetworkStateSnapshotIO::write(hp, snap, &herr))
                log->warn("Network (RTB-7d/BR-1): history snapshot write to '{}' failed ({}).",
                          hp.string(), herr);
            // The closed-loop consumer's own snapshot beside this history copy, with the
            // same suffix and tow (card CC, part B).  The consumer processed this epoch
            // inside networkEmitStore (earlier in the same loop iteration);
            // writeStateSnapshotTo refuses (writes nothing, counts and logs it) if its
            // last processed epoch is not `t`, so the file on disk cannot carry a
            // different tow than the copy beside it.  Consumer memory only: the
            // producer's snapshot, solve and products are unaffected.
            if (nc.closed_loop.state_snapshot && consumer) {
                const std::filesystem::path cp = RealTimeEngine::consumerSnapshotHistoryPath(
                    snap_path, static_cast<unsigned long long>(snap.n_emitted));
                std::string cerr;
                if (consumer->writeStateSnapshotTo(cp, &t, &cerr)) {
                    ++ctx.counters.cl_snap_written;
                    const auto ct = consumer->lastProcessedEpochTime();
                    log->info("Network closed-loop (card CC): consumer snapshot written beside "
                              "'{}' -> '{}' (network tow {:.6f}, consumer tow {:.6f}).",
                              hp.filename().string(), cp.filename().string(), t.tow,
                              ct ? ct->tow : -1.0);
                } else {
                    ++ctx.counters.cl_snap_skipped;
                    log->warn("Network closed-loop (card CC): consumer snapshot NOT written "
                              "beside '{}' — {}.", hp.filename().string(), cerr);
                }
            }
        }
        last_snapshot_write_s = pl_now_s;
    }
}

} // namespace GnssProcessor
