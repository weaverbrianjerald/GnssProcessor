/**
 * @file StateSnapshotWrite.cpp
 * @brief @internal networkWriteStateSnapshot: the periodic atomic state snapshot,
 *        written one record group at a time as the mirror of the warm resume.
 *
 * One function, called by the driver (network/driver/ApplicationNetworkEpoch.cpp) at the end of every
 * kept epoch: an epoch the driver skips returns before the call.  With a snapshot path it
 * writes on its first call and then once per network.state_snapshot_interval_s of product time;
 * without one it does nothing.  The restore of each group is in WarmResume.cpp,
 * and the fingerprint the snapshot carries is formed in SnapshotFingerprint.cpp.
 *
 * The function builds the snapshot by one file-local capture function per record group (capture...),
 * called in the order below, each opening with its arming gate as its first statement (a group armed by
 * no knob has none) and then binding the names it uses; the groups are defined here in the order they run.
 * Every group copies run state into the snapshot and writes nothing the run reads, except the counters of
 * what it wrote, so the products and every trace are unchanged by arming any of them.
 *
 * Order of the groups (group, then the knob that arms it):
 *   captureHeader              always: epoch, counters, URA estimate, warm-up ramp
 *   captureSharedCarry         always: D rows
 *   captureEntityRecords       always: V C J K L Z Y R A I X
 *   captureFrame               network.state_snapshot_frame_capture
 *   captureCoupling            network.state_snapshot_coupling_capture with the conditional coupling:
 *                              the dictionary and rows (QS QL QZ, QR), then the dense side-car (QK QP)
 *   captureDatum               network.state_snapshot_datum_capture
 *   captureEmitLayer           network.state_snapshot_emit_capture
 *   captureHold                network.state_snapshot_hold_capture
 *   captureChainMemory         network.state_snapshot_chain_capture
 *   captureStationMemory       network.state_snapshot_station_memory_capture (STMEM .. SMEND)
 *   captureStationLayer        network.state_snapshot_station_capture (RC SD)
 *   captureClockLevel          the clock level monitor (LB)
 *   captureCanonicalSignals    network.state_snapshot_signal_capture
 *   captureRxCbLinearization   network.state_snapshot_rxcb_lin_capture
 *   captureCarryBlob           network.state_snapshot_carry
 *   captureEmitterState        always
 *   writeSnapshotFile          always; writeHistoryCopy under network.state_snapshot_keep_history
 *
 * Order dependencies:
 *   - The capture runs after the epoch's emit and flush: the snapshot copies the state the next epoch starts from.
 *   - The record order is the order of the writer's recordToSnapshot calls, which the parser reads back in
 *     kRestoreOrder (network/state/Entities.cpp); the restore of each group is expressed in the frame of the
 *     groups before it, so the frame group precedes the datum, emit and hold groups.
 *   - The station layer comes before the receiver code-bias linearization group, which is driven by the RC rows
 *     the station group wrote (one RL row beside each RC row).
 *   - The emitter state is serialized after every group that reads run state, and the file is written last;
 *     the history copy and the consumer's snapshot beside it follow the main write.
 *   - The snapshot time stamp of the last write (last_snapshot_write_s) is set after the whole write.
 *
 * The declaration is in include/network/state/RunContext.hpp.
 */

#include "network/state/RunContext.hpp"

#include "config/GnssConfig.hpp"
#include "network/carry/InvariantCarryDriver.hpp"   // branch-faithful carry (de)serialization
#include "network/emit/ClockEmitRebase.hpp"       // clock emit re-base held frame, written with the snapshot
#include "network/emit/EmitRebase.hpp"            // eq34 emit re-base frame, written with the snapshot
#include "network/state/ChainMemory.hpp"     // the chain memory section (network.state_snapshot_chain_capture)
#include "network/state/StationMemory.hpp"   // the station memory section (network.state_snapshot_station_memory_capture)
#include "network/state/StationParams.hpp"   // the stations' parameter sets: the code-bias linearization points
#include "math/TimeUtils.hpp"                // GnssTime
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "realtime/RealTimeEngine.hpp"       // consumer snapshot written beside the network history copy

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

namespace {

/// What every capture group reads: the run, the epoch time and the snapshot being built.
struct SnapshotScope {
    NetworkRun& run;
    const GnssTime& t;                 ///< The time of the epoch the snapshot is written at.
    NetworkStateSnapshot& snap;        ///< The snapshot being built; each group writes its own rows.
};

/**
 * @brief Stamps the snapshot with its epoch, wall time, fingerprint, producer counters and warm-up ramp state.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureHeader(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const GnssTime& t = scope.t;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
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
}

/**
 * @brief Copies the shared (border) carry priors into the D records; parked entries are not written.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureSharedCarry(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    auto& prev_carry = ctx.carry.prev_carry;

    snap.shared_carry.reserve(prev_carry.size());
    snap.parked_carry.clear();
    for (const CarryPrior& cp : prev_carry) {
        // A parked entry (a departed satellite's pre-gap realization, held for
        // re-admission) is not serialized: the D record has no park stamp, so a restored copy would fold as
        // a fresh live carry with epoch-dt inflation.  Under the forward-only response
        // policy, a seam inside a satellite's absence loses its seed, and the
        // satellite returns cold.
        if (cp.t_park_s > 0.0) {
            ++ctx.counters.n_snap_parked_excluded;
            // Under network.state_snapshot_park_capture the entry is written apart, with its park stamp (DP lines), so
            // a resume armed with network.resume_park_restore re-admits the satellite as the uninterrupted run does.
            if (run.config.networkStateSnapshotParkCapture())
                snap.parked_carry.push_back({ static_cast<int>(cp.kind), cp.sat, cp.signal, cp.value, cp.var,
                                              cp.t_park_s });
            continue;
        }
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
        // Dual-layer ionosphere: tag the second shell in the sat field
        // (otherwise empty for VtecSh); the record arity is unchanged, and older
        // readers reseed safely.
        if (cp.kind == SharedParamKind::VtecSh && cp.sh_layer > 0)
            sc.sat = "L1";
        snap.shared_carry.push_back(std::move(sc));
    }
    ctx.counters.snap_parked_written_last = static_cast<long long>(snap.parked_carry.size());
}

/**
 * @brief Writes the records every snapshot carries: pivots, satellite clocks and biases, leveling and wet-delay carries, URA and the ephemeris-cutover rows.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureEntityRecords(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    net::recordToSnapshot("V", ctx, snap);   // the constellation pivots
    net::recordToSnapshot("C", ctx, snap);   // the satellites
    net::recordToSnapshot("J", ctx, snap);   // the satellites
    net::recordToSnapshot("K", ctx, snap);   // the satellites
    net::recordToSnapshot("L", ctx, snap);   // the arcs' leveling carries
    net::recordToSnapshot("Z", ctx, snap);   // the station table's zwd
    net::recordToSnapshot("Y", ctx, snap);   // the satellites
    net::recordToSnapshot("R", ctx, snap);   // the satellites
    net::recordToSnapshot("A", ctx, snap);   // the satellites
    net::recordToSnapshot("I", ctx, snap);   // eph cutover: the satellites
    net::recordToSnapshot("X", ctx, snap);   // eph cutover: the satellites
}

/**
 * @brief Writes the realization-frame rows: phase-anchor incumbents and pin targets, the clock-anchor state and the clock re-base held frame.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureFrame(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_frame_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const NetworkClockEmitRebase* clk_rebase = &run.emit.clk_rebase;

    // Resume frame state (snapshot format v2), captured when
    // network.state_snapshot_frame_capture is armed: the per-(sat,signal)
    // phase-anchor incumbents and sticky pin targets as of this epoch's election, the
    // clock-anchor per-satellite state (folded satellites only), and the clock emit
    // re-base held frame.  It has its own knob, unlike the Z rows, because a binary that
    // predates it cannot read a v2 file, so the default snapshot stays v1 byte for
    // byte.  Whether a resume uses the rows is the resume-side knob
    // network.resume_frame_restore.
    net::recordToSnapshot("PA", ctx, snap);   // the satellite signals
    net::recordToSnapshot("PT", ctx, snap);   // the satellite signals
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
}

/**
 * @brief Writes the coupling regression dictionary and one packed line per carried row, the refloat set and the row counters.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureCouplingRows(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const net::StationTable& sta_rows = ctx.entities.stations;

    // Coupling regressors (snapshot format v3), captured under
    // network.state_snapshot_coupling_capture.  This is a separate knob from the frame
    // knob because the rows make the file about 37 times larger (and the keep-history
    // library with it), and a frame-only writer stays available for cheap legs.  Layout: a dictionary of shared columns keyed on
    // (identity, ŝ_prev, P_prev), plus one packed line per carried row.
    //
    // The value pair is part of the dictionary key, not only the payload, because
    // networkHarvestColumn assigns cs.s_prev = x_s(j) and cs.p_prev = cov_df(j,j) from the
    // shared column alone (network/carry/LocalCarryHarvest.cpp): rows of
    // one harvest epoch agree, but a station whose harvest failed hits `continue`
    // before the stash and keeps an older coupling, so a file can hold L rows with an
    // older last_seen_s.  An identity-only hoist would restore those rows against the
    // wrong frame.
    //
    // Row atomicity: a row is written whole or not at all.  A truncated
    // coefficient vector loses the exact gauge identity Mᵀg_s = −g_r(c); every
    // coupling row then observes the clock gauge, and the null-space rank gate withholds
    // each epoch that carries rows, which is worse than the diagonal degrade an empty
    // vector already gives.  fmt writes "nan"/"inf", and the parser's strtod would
    // accept and consume them, so the finite screen here is required, not a second
    // guard.
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
        // the format description rules out.  Row atomicity covers the
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
    net::forEach(ctx.entities.arcs, &net::ArcState::amb,
                 [&](const ArcId& id, const LocalCarry& lc) {
                     if (lc.coupling.empty()) return;          // legacy-diagonal entry
                     if (!pack(lc.coupling)) { ++n_drop; return; }
                     n_terms += static_cast<long long>(ents.size());
                     snap.coupling_rows[net::arcTuple(id)] = ents;
                 });
    net::forEach(sta_rows, &net::StationState::zwd,
                 [&](const StationId& id, const LocalCarry& lc) {
                     if (lc.coupling.empty()) return;
                     if (!pack(lc.coupling)) { ++n_drop; return; }
                     n_terms += static_cast<long long>(ents.size());
                     snap.coupling_zrows[id.index] = ents;
                 });
    // The refloat set has no D-row carrier (a hard-refloated satellite is
    // skip-stashed), so without this the resume cannot reproduce the pre-pass mark.
    net::recordToSnapshot("QR", ctx, snap);   // the satellites
    ctx.counters.snap_cpl_dict_last    = static_cast<long long>(snap.coupling_dict.size());
    ctx.counters.snap_cpl_rows_last    = static_cast<long long>(snap.coupling_rows.size());
    ctx.counters.snap_cpl_zrows_last   = static_cast<long long>(snap.coupling_zrows.size());
    ctx.counters.snap_cpl_terms_last   = n_terms;
    ctx.counters.snap_cpl_rows_dropped += n_drop;
}

/**
 * @brief Writes the dense shared-carry covariance side-car (the QK keys and QP lower triangle); the whole block is omitted when any entry is unusable.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureCarryCovariance(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // The dense shared-carry covariance (SharedCarryCov) side-car, the other half
    // of the seam.  Keys in row order (QK), lower triangle per row (QP); the whole block
    // is omitted (never truncated) when any entry is non-finite or a row has no
    // key.  Parked entries never have a row, so nothing here references them.
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

/**
 * @brief Writes the coupling regressors (snapshot format v3): the dictionary and one packed line per carried row with the refloat
 *        set, then the dense shared-carry covariance side-car.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureCoupling(const SnapshotScope& scope) {
    const auto& nc = scope.run.config.getNetworkConfig();
    if (!(nc.state_snapshot_coupling_capture && scope.run.config.networkCarryConditionalCoupling())) return;
    captureCouplingRows(scope);
    captureCarryCovariance(scope);
}

/**
 * @brief Writes the datum memories: the eq34 spanning-tree rows and the time-linked code-bias previous values, seen set and pin pair.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureDatum(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_datum_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // Datum capture (network.state_snapshot_datum_capture): the write half of the two
    // datum memories that a warm resume would otherwise re-realize.  The frame block
    // above carries the satellite side of the
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
    net::recordToSnapshot("EQD", ctx, snap);   // the constellation channels
    net::recordToSnapshot("EQP", ctx, snap);   // the station signals
    net::recordToSnapshot("EQS", ctx, snap);   // the satellite signals
    net::recordToSnapshot("EQT", ctx, snap);   // the arcs
    net::recordToSnapshot("EQE", ctx, snap);   // the lines of sight
    net::recordToSnapshot("EQR", ctx, snap);   // the arcs
    // Non-finite screen on the code-bias values only: the eq34 targets and
    // elevations are rounded or geometric quantities the election recomputes,
    // while code_bias_prev_ is the quantity the veteran-referenced sum-to-zero RHS
    // differences against.  A nan there would survive the parse (strtod accepts
    // it) and corrupt the restored datum, so it is dropped at the source.
    net::recordToSnapshot("CBP", ctx, snap);   // the constellation channels
    net::recordToSnapshot("CBS", ctx, snap);   // the satellites
    // The IF-pin pair that produced those values, the third container of the same
    // kept-epoch lifecycle.  Without it the restore is inert on the IF-pin half:
    // `pair_same` reads false for every satellite, the split re-seeds to 0 and
    // every satellite disc-bumps.
    net::recordToSnapshot("CBI", ctx, snap);   // the satellites
    ctx.counters.snap_datum_rows_last =
        static_cast<long long>(snap.eq34_datum_rx.size()
                               + snap.eq34_rx_parent.size()
                               + snap.eq34_sat_root.size()
                               + snap.eq34_prev_target.size()
                               + snap.eq34_prev_el.size()
                               + snap.eq34_retained_edges.size()
                               + snap.code_bias_sat_seen.size()
                               + snap.code_bias_ifpin_pair.size());
    for (const auto& [fam, per_sat] : snap.code_bias_prev)
        ctx.counters.snap_datum_rows_last += static_cast<long long>(per_sat.size());
}

/**
 * @brief Exports the eq34 emit re-base frame (the last emitted phase-bias levels).
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureEmitLayer(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_emit_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const NetworkEmitRebase* emit_rebase = &run.emit.rebase;

    // Emit-layer capture (network.state_snapshot_emit_capture): the write half of the
    // emit-side cross-epoch state.
    // The frame and datum blocks above carry the estimator's realization; the emit
    // re-base sits downstream of it on the outputs only (EmitStore.cpp) and is run
    // state outside ctx.carry (run.emit.rebase), so without the capture a warm resume
    // cold-starts it, and the first post-resume epoch emits the raw
    // solve-frame phase bias while the continuous wire carries the accumulated re-base.
    //
    // These are plain exports after the emit, into `snap` only, with the same
    // write-side discipline as the datum block, so the knob can be added to an
    // existing recipe.  A run that does not arm the re-base (carry_invariant off)
    // exports an empty map and writes no rows, so arming the capture there costs
    // nothing.  Nothing here enters the warm-resume fingerprint: the rows
    // change no estimate, only the level in which the emitted biases are expressed.
    if (emit_rebase) snap.emit_rebase_last = emit_rebase->exportState();
    ctx.counters.snap_emit_rows_last =
        static_cast<long long>(snap.emit_rebase_last.size());
}

/**
 * @brief Writes the phase-bias hold rows: held keys with their generations, the common clock step and the previous-clock baselines.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureHold(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_hold_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // Hold capture (network.state_snapshot_hold_capture, card CRS2): the write half of
    // the emit-side phase-bias hold.  The emit block above carries the re-base frame
    // under the hold; this carries the hold itself: every held key's generation, held
    // value and transfer references, the common emitted-clock step per constellation
    // and the two previous-clock baselines per satellite.  Plain copies of the entity
    // tables after the emit, into `snap` only, with the same write-side discipline as
    // the blocks above: products and traces stay byte-identical, so the knob can be
    // added to an existing recipe.  A run with the hold off holds no key and writes no
    // row.  The smoothed hold's clock clone is not written (the restore queues its
    // birth again).
    net::recordToSnapshot("PH", ctx, snap);    // the satellite signals
    net::recordToSnapshot("PHM", ctx, snap);   // the constellations
    net::recordToSnapshot("PHC", ctx, snap);   // the satellites
    net::recordToSnapshot("PHF", ctx, snap);   // the satellites
    ctx.counters.snap_hold_rows_last =
        static_cast<long long>(snap.pb_hold.size() + snap.pb_hold_frz_M.size()
                               + snap.pb_hold_prev_clock_m.size()
                               + snap.pb_hold_frz_prev_clk.size());
}

/**
 * @brief Writes the chain memory section (tree memory, ring fix links, emitter held values, group-delay latch).
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureChainMemory(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_chain_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // Chain memory capture (network.state_snapshot_chain_capture): the chain's tree memory, the ring fix's links
    // and maps, the emitter's held-value state and the group-delay latch, as one verbatim section
    // (network/state/ChainMemory).  Copies taken after the emit; the products and every trace are unchanged.
    snap.chain_memory = networkChainMemoryWrite(run);
    ctx.counters.snap_chain_lines_last =
        static_cast<long long>(std::count(snap.chain_memory.begin(), snap.chain_memory.end(), '\n'));
}

/**
 * @brief Writes the station memory section (every station's wind-up accumulator).
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureStationMemory(const SnapshotScope& scope) {
    if (!scope.run.config.networkStateSnapshotStationMemoryCapture()) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // Station memory capture (network.state_snapshot_station_memory_capture): every station's wind-up accumulator,
    // exactly as kept, as one verbatim section (network/state/StationMemory).  Copies taken after the epoch; the
    // products and every trace are unchanged.
    long long n_records = 0;
    snap.station_memory = networkStationMemoryWrite(run, n_records);
    ctx.counters.snap_station_memory_records_last = n_records;
}

/**
 * @brief Writes the station layer: the receiver code-bias carries and the per-line-of-sight deviation entries.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureStationLayer(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_station_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // Station-layer capture (network.state_snapshot_station_capture): the write half of
    // the two per-station memories that otherwise restart cold at every warm resume.  The frame and datum blocks carry
    // the network's realization and the emit block the level on top of it; these two
    // are the station layer underneath: the receiver code-bias posterior per
    // (station, CB family) and the per-LOS SH representation-error deviation.
    //
    // These are plain copies of ctx.carry after the emit, into `snap` only, with the
    // same write-side discipline as the datum and emit blocks, so the knob can be
    // added to an existing recipe.  A run that arms neither carry
    // (network.carry_rx_code_bias / the SH deviation carry) exports empty maps and writes no
    // rows, so arming the capture there costs nothing.
    //
    // The non-finite screen is at the source, as for code_bias_prev: a nan that strtod
    // would accept on the way back in becomes the prior the next fold's innovation
    // differences against (rx-CB) or the GM level the next harvest ages (SH deviation).  An
    // entry dropped here is only cold on the resume (the behaviour without the
    // capture, for that one key), whereas a corrupt entry cannot be recovered.
    net::recordToSnapshot("RC", ctx, snap);   // the station signals
    net::recordToSnapshot("SD", ctx, snap);   // the lines of sight
    ctx.counters.snap_station_rows_last =
        static_cast<long long>(snap.rx_cb_carry.size() + snap.sh_dev_carry.size());
}

/**
 * @brief Writes the clock level monitor's contrast-frame level information (LB rows).
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureClockLevel(const SnapshotScope& scope) {
    if (!scope.run.ctx.knobs.clm_on) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;

    // The monitor's contrast-frame level information (LB rows).
    net::recordToSnapshot("LB", ctx, snap);   // the satellites
}

/**
 * @brief Writes the per-station canonical-signal latch (CN rows) from the station models.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureCanonicalSignals(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_signal_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const std::vector<NetStation>& stations = run.setup.stations;

    // Canonical-signal capture (network.state_snapshot_signal_capture): the write half of
    // the per-station latch that no other snapshot group holds, PppModel's
    // per-(constellation, band, obs-type)
    // canonical signal, decided from the satellite counts of that station's first
    // processed epoch and then locked for the run.
    //
    // It is one of the two snapshot groups that cannot be read out of ctx.carry (the
    // latch is inside each station's model), which is why this function reads
    // `stations`; the RL block below is the other, and it reads the station's own
    // state vector.
    //
    // Read-only with respect to the models and written into `snap` alone, with the
    // same write-side discipline as the datum, emit and station blocks, so the knob
    // can be added to an existing recipe: products and traces stay byte-identical.
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
    ctx.counters.snap_canon_rows_last = n;
}

/**
 * @brief Writes the receiver code-bias linearization points (RL rows), one beside each receiver code-bias carry row.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureRxCbLinearization(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_rxcb_lin_capture) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const std::vector<NetStation>& stations = run.setup.stations;

    // Code-bias linearization-point capture (network.state_snapshot_rxcb_lin_capture):
    // the reference of the RC row.  The RC row above carries the rx-CB carry's numerator;
    // the gate that decides whether that prior is folded tests `value_abs − x_lin(cb)`
    // against a fixed 5.13 m bar, and x_lin is the one-shot seed that
    // the first-epoch code-bias seed wrote on this station's first processed
    // epoch and never revises.  It is not in ctx.carry but in the station's own parameter
    // set, which is why this block reads `stations`.
    //
    // Read-only and after the emit, like the blocks above, so arming it moves no
    // estimate and no trace: an x_lin shift is an exact solve no-op for a state that
    // enters linearly, and nothing here writes one.
    // Keyed exactly as the RC rows, (roster index, CB state key), so the restore can
    // pair them without a second identity, and screened for finiteness at the source
    // for a stronger form of the RC reason: a non-finite linearization point would
    // corrupt the first epoch's iono and ambiguity seeds through the raw residual,
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
    for (const auto& [key, l] : snap.rx_cb_carry) {
        (void)l;
        if (key.first < 0 || key.first >= static_cast<int>(stations.size())) continue;
        const StationParams& params = stations[static_cast<std::size_t>(key.first)].params;
        const int cb_idx = findReceiverCodeBias(params, key.second);
        if (cb_idx < 0) continue;
        const double x_lin = slotAt(params, cb_idx).x_lin;
        if (!std::isfinite(x_lin)) continue;
        snap.rx_cb_lin[{key.first, key.second}] = x_lin;
    }
    ctx.counters.snap_rxcb_lin_rows_last =
        static_cast<long long>(snap.rx_cb_lin.size());
}

/**
 * @brief Serializes the invariant DD carry blob and the vertex and group numbering it references.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureCarryBlob(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_carry) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    NetworkStateSnapshot& snap = scope.snap;
    const NetworkInvariantCarryDriver& carry_driver = run.engines.carry_driver;
    const auto& nc = config_.getNetworkConfig();

    // Branch-faithful snapshot: serialize the invariant DD carry (z-store
    // branch) and its ArcKey vertex numbering (sat to vertex, (const,signal) to group), so a
    // warm resume reproduces the phase-ambiguity branch instead of re-converging one.
    net::recordToSnapshot("SV", ctx, snap);   // the satellites
    net::recordToSnapshot("GG", ctx, snap);   // the constellation channels
    std::ostringstream ccs;
    // Write-side shape knob.  The blob is about 99 % of the snapshot at
    // scale (at 305 stations, 13.1M dense Pᶻ numbers against 7.9k means), so the
    // diagonal shape makes the difference between a 258 MB and a ~160 KB file.  The
    // read side is unaffected.
    carry_driver.serializeCarry(ccs, nc.state_snapshot_carry_diagonal);
    snap.carry_state = ccs.str();
    ctx.counters.snap_carry_bytes_last =
        static_cast<long long>(snap.carry_state.size());
    ctx.counters.snap_carry_diagonal = nc.state_snapshot_carry_diagonal;
}

/**
 * @brief Serializes the product emitter state (counters, baselines, first-sighting sets, pending bumps).
 * @param scope The run, the epoch time and the snapshot being built.
 */
void captureEmitterState(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkStateSnapshot& snap = scope.snap;
    NetworkProductEmitter& emitter = *run.setup.emitter;

    std::ostringstream es;
    emitter.serializeState(es);
    snap.emitter_state = es.str();
}

/**
 * @brief Writes the snapshot atomically; a failed write leaves the previous good snapshot intact.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void writeSnapshotFile(const SnapshotScope& scope) {
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    NetworkStateSnapshot& snap = scope.snap;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    auto& snap_path = ctx.carry.snap_path;

    std::string werr;
    if (!NetworkStateSnapshotIO::write(snap_path, snap, &werr))
        log->warn("Network (RTB-7): state-snapshot write to '{}' failed ({}) — the "
                  "previous good snapshot (if any) is left intact.",
                  snap_path.string(), werr);
    else
        ++ctx.counters.snap_writes;   // run-end blob-size readout
}

/**
 * @brief Keeps a per-epoch copy of the snapshot, and the closed-loop consumer's own snapshot beside it, when history is armed.
 * @param scope The run, the epoch time and the snapshot being built.
 */
void writeHistoryCopy(const SnapshotScope& scope) {
    if (!scope.run.config.getNetworkConfig().state_snapshot_keep_history) return;
    NetworkRun& run = scope.run;
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    NetworkStateSnapshot& snap = scope.snap;
    const GnssTime& t = scope.t;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    RealTimeEngine* consumer = run.setup.consumer.get();
    const auto& nc = config_.getNetworkConfig();
    auto& snap_path = ctx.carry.snap_path;

    // Keep a per-epoch copy so the branch can be graded over time
    // (scripts/diff_carry_blobs.py).  Write-side only: the same atomic
    // temp file and rename, and the estimation cannot see it (this runs after the
    // emit, over const references).
    std::filesystem::path hp = snap_path;
    hp += fmt::format(".ep{:04d}", static_cast<unsigned long long>(snap.n_emitted));
    std::string herr;
    if (!NetworkStateSnapshotIO::write(hp, snap, &herr))
        log->warn("Network (RTB-7d/BR-1): history snapshot write to '{}' failed ({}).",
                  hp.string(), herr);
    // The closed-loop consumer's own snapshot beside this history copy, with the
    // same suffix and tow.  The consumer processed this epoch
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
            log->info("Network closed-loop: consumer snapshot written beside "
                      "'{}' -> '{}' (network tow {:.6f}, consumer tow {:.6f}).",
                      hp.filename().string(), cp.filename().string(), t.tow,
                      ct ? ct->tow : -1.0);
        } else {
            ++ctx.counters.cl_snap_skipped;
            log->warn("Network closed-loop: consumer snapshot NOT written "
                      "beside '{}' — {}.", hp.filename().string(), cerr);
        }
    }
}

} // namespace

void networkWriteStateSnapshot(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch.  Reads bind through `ep`; writes are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const GnssTime& t = ep.se.time;
    double pl_now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    const auto& nc = config_.getNetworkConfig();
    auto& snap_path = ctx.carry.snap_path;
    auto& last_snapshot_write_s = ctx.carry.last_snapshot_write_s;


    // --- Periodic atomic state snapshot ---------------------------
    // Written at the end of emitted epochs only (skipped epochs return earlier in the
    // driver and change no carry state), every state_snapshot_interval_s of product
    // time.  It is written after this epoch's product emit and flush, through the
    // snapshot's own temp file and rename; this block never flushes or reorders the
    // product writer, so emission is unaffected.  An empty path (the default) leaves
    // the output byte-identical.
    if (!snap_path.empty()
        && (last_snapshot_write_s < 0.0
            || pl_now_s - last_snapshot_write_s >= nc.state_snapshot_interval_s)) {
        // Cold-state exclusions.  The snapshot carries the producer's own state:
        // the cross-epoch carries (phase level, iono, ZWD), clock oscillators, pivots,
        // generation id, URA/ramp metrics and the emitter's disc-counter, baseline and
        // first-sighting bookkeeping, plus the optional capture groups below.  The
        // per-station filter state is not snapshotted, by design: per-station cycle-slip
        // arcs and slip-detector history, per-station ambiguity / iono /
        // receiver-bias marks, and per-station covariance.  On a warm resume those layers
        // cold-start and re-warm within a few epochs, and the post-fit RAIM absorbs the
        // transient; serializing the large per-station covariance blocks would add nothing
        // the re-warm does not already provide.  That is the accepted recovery-budget trade.
        NetworkStateSnapshot snap;
        const SnapshotScope scope{run, t, snap};
        captureHeader(scope);
        captureSharedCarry(scope);
        captureEntityRecords(scope);
        captureFrame(scope);
        captureCoupling(scope);
        captureDatum(scope);
        captureEmitLayer(scope);
        captureHold(scope);
        captureChainMemory(scope);
        captureStationMemory(scope);
        captureStationLayer(scope);
        captureClockLevel(scope);
        captureCanonicalSignals(scope);
        captureRxCbLinearization(scope);
        captureCarryBlob(scope);
        captureEmitterState(scope);
        writeSnapshotFile(scope);
        writeHistoryCopy(scope);
        last_snapshot_write_s = pl_now_s;
    }
}

} // namespace GnssProcessor
